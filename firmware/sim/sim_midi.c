/* CoreMIDI virtual ports with the real keyboard's names. A DAW sees two
   sources (keyboard -> DAW) and two destinations (DAW -> keyboard):
   "My Kontrol Keys" is cable 0, "My Kontrol DAW" is cable 1.

   In: CoreMIDI calls read_proc on its own thread with a byte stream; it is
   cut into USB-MIDI packets and queued. The main thread feeds the queue to
   midi_rx_packet(), so the core only ever runs on one thread.
   Out: the core's USB-MIDI queue is turned back into bytes and sent with
   MIDIReceived(), a whole sysex per MIDIPacket.
   With --stdio there are no ports: sim.c injects input bytes and the output
   is printed as "out <cable> <hex>" lines. */
#include <pthread.h>
#include <string.h>
#include <CoreMIDI/CoreMIDI.h>
#include "sim.h"
#include "midi.h"

FILE *sim_log;
uint32_t sim_midi_in, sim_midi_out;
int sim_midi_stdio;

static const char *const port_name[PORT_COUNT] = {"My Kontrol Keys", "My Kontrol DAW"};

static MIDIClientRef client;
static MIDIEndpointRef src[PORT_COUNT], dst[PORT_COUNT];

/* ---------- bytes -> USB-MIDI packets (CoreMIDI thread) ---------- */

typedef struct {
    uint8_t status, need, n, data[2];
    uint8_t in_sx, sx[3], sx_n;
} packer_t;

static packer_t packers[PORT_COUNT];

#define RX_LEN 4096                 /* packets, power of two */
static uint8_t rx_q[RX_LEN][4];
static uint32_t rx_head, rx_tail;
static pthread_mutex_t rx_lock = PTHREAD_MUTEX_INITIALIZER;

static void rx_put(uint8_t cable, uint8_t cin, uint8_t a, uint8_t b, uint8_t c)
{
    if (rx_head - rx_tail >= RX_LEN)
        return;                     /* full: drop, like a USB endpoint would stall */
    uint8_t *p = rx_q[rx_head++ & (RX_LEN - 1)];
    p[0] = cable << 4 | cin;
    p[1] = a;
    p[2] = b;
    p[3] = c;
}

static void pack_byte(uint8_t cable, uint8_t b)
{
    packer_t *p = &packers[cable];
    if (b >= 0xF8) {                         /* realtime: anywhere, even inside a sysex */
        rx_put(cable, 0xF, b, 0, 0);
        return;
    }
    if (b == 0xF0) {
        p->in_sx = 1;
        p->sx[0] = b;
        p->sx_n = 1;
        p->status = 0;
        return;
    }
    if (p->in_sx) {
        if (b == 0xF7) {
            p->sx[p->sx_n++] = b;
            rx_put(cable, 0x4 + p->sx_n, p->sx[0], p->sx_n > 1 ? p->sx[1] : 0, p->sx_n > 2 ? p->sx[2] : 0);
            p->in_sx = 0;
            return;
        }
        if (!(b & 0x80)) {
            p->sx[p->sx_n++] = b;
            if (p->sx_n == 3) {
                rx_put(cable, 0x4, p->sx[0], p->sx[1], p->sx[2]);
                p->sx_n = 0;
            }
            return;
        }
        p->in_sx = 0;                        /* any other status aborts the sysex */
    }
    if (b & 0x80) {
        p->status = b;
        p->n = 0;
        uint8_t hi = b & 0xF0;
        p->need = hi == 0xC0 || hi == 0xD0 || b == 0xF1 || b == 0xF3 ? 1 : b < 0xF0 || b == 0xF2 ? 2 : 0;
        if (!p->need) {
            if (b == 0xF6)
                rx_put(cable, 0x5, b, 0, 0);
            p->status = 0;                   /* F4, F5, stray F7 */
        }
        return;
    }
    if (!p->status)
        return;
    p->data[p->n++] = b;
    if (p->n < p->need)
        return;
    uint8_t s = p->status;
    rx_put(cable, s < 0xF0 ? s >> 4 : p->need == 1 ? 0x2 : 0x3, s, p->data[0], p->need > 1 ? p->data[1] : 0);
    p->n = 0;
    if (s >= 0xF0)
        p->status = 0;                       /* running status is for channel messages only */
}

static void read_proc(const MIDIPacketList *list, void *ref, void *src_ref)
{
    (void)src_ref;
    uint8_t cable = (uint8_t)(uintptr_t)ref;
    pthread_mutex_lock(&rx_lock);
    const MIDIPacket *pk = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; i++) {
        for (UInt16 k = 0; k < pk->length; k++)
            pack_byte(cable, pk->data[k]);
        pk = MIDIPacketNext(pk);
    }
    pthread_mutex_unlock(&rx_lock);
}

void sim_midi_inject(uint8_t cable, const uint8_t *b, int n)
{
    pthread_mutex_lock(&rx_lock);
    for (int i = 0; i < n; i++)
        pack_byte(cable & 1, b[i]);
    pthread_mutex_unlock(&rx_lock);
}

/* ---------- USB-MIDI packets -> messages (main thread) ---------- */

typedef struct {
    uint8_t buf[1024];
    uint16_t n;
    uint8_t in_sx;
} unpacker_t;

/* One packet in. Returns n > 0 when a whole sysex is in u->buf, -n for a
   short message of n bytes at p[1], 0 while a sysex is still coming. */
static int unpack(unpacker_t *u, const uint8_t p[4])
{
    static const uint8_t len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    uint8_t cin = p[0] & 0xF, n = len[cin];
    int sysex = cin == 0x4 || cin == 0x6 || cin == 0x7 || (cin == 0x5 && (u->in_sx || p[1] == 0xF7));
    if (!sysex)
        return -n;                  /* realtime may sit inside a sysex: keep it */
    if (p[1] == 0xF0) {
        u->n = 0;
        u->in_sx = 1;
    }
    for (int i = 0; i < n; i++)
        if (u->n < sizeof u->buf)
            u->buf[u->n++] = p[1 + i];
    if (cin == 0x4)
        return 0;
    int m = u->n;
    u->n = 0;
    u->in_sx = 0;
    return m;
}

static void log_sysex(const char *dir, int port, const uint8_t *m, int n)
{
    if (!sim_log || !n || m[0] != 0xF0)
        return;
    fprintf(sim_log, "%8u.%03u %-3s %-4s", sim_now_ms / 1000, sim_now_ms % 1000, dir, port ? "DAW" : "Keys");
    for (int i = 0; i < n; i++)
        fprintf(sim_log, " %02X", m[i]);
    fputc('\n', sim_log);
    fflush(sim_log);
}

void sim_midi_rx(void)
{
    static unpacker_t logs[PORT_COUNT];
    uint8_t batch[256][4];
    for (;;) {
        int n = 0;
        pthread_mutex_lock(&rx_lock);
        while (n < 256 && rx_tail != rx_head)
            memcpy(batch[n++], rx_q[rx_tail++ & (RX_LEN - 1)], 4);
        pthread_mutex_unlock(&rx_lock);
        if (!n)
            return;
        for (int i = 0; i < n; i++) {
            int port = batch[i][0] >> 4 & 1;
            sim_midi_in++;
            if (sim_log) {
                int m = unpack(&logs[port], batch[i]);
                if (m > 0)
                    log_sysex("in", port, logs[port].buf, m);
            }
            midi_rx_packet(batch[i]);
        }
    }
}

void sim_midi_tx(void)
{
    static unpacker_t out[PORT_COUNT];
    static Byte lists[PORT_COUNT][8192];
    MIDIPacketList *list[PORT_COUNT];
    MIDIPacket *cur[PORT_COUNT];
    for (int i = 0; i < PORT_COUNT; i++) {
        list[i] = (MIDIPacketList *)lists[i];
        cur[i] = MIDIPacketListInit(list[i]);
    }
    uint8_t p[4];
    while (midi_tx_peek(p)) {
        midi_tx_pop();
        sim_midi_out++;
        int port = p[0] >> 4 & 1;
        int m = unpack(&out[port], p);
        const uint8_t *msg = m > 0 ? out[port].buf : p + 1;
        int n = m > 0 ? m : -m;
        if (!n)
            continue;
        if (m > 0)
            log_sysex("out", port, msg, n);
        if (sim_midi_stdio) {
            printf("out %d", port);
            for (int i = 0; i < n; i++)
                printf(" %02X", msg[i]);
            printf("\n");
            fflush(stdout);
            continue;
        }
        MIDIPacket *next = MIDIPacketListAdd(list[port], sizeof lists[port], cur[port], 0, n, msg);
        if (!next) {                         /* list full: send it and start another */
            MIDIReceived(src[port], list[port]);
            cur[port] = MIDIPacketListInit(list[port]);
            next = MIDIPacketListAdd(list[port], sizeof lists[port], cur[port], 0, n, msg);
        }
        if (next)
            cur[port] = next;
    }
    for (int i = 0; i < PORT_COUNT; i++)
        if (list[i]->numPackets)
            MIDIReceived(src[i], list[i]);
}

/* ---------- setup ---------- */

static MIDIEndpointRef endpoint(int port, int is_src)
{
    CFStringRef name = CFStringCreateWithCString(NULL, port_name[port], kCFStringEncodingUTF8);
    MIDIEndpointRef e = 0;
    OSStatus err = is_src ? MIDISourceCreate(client, name, &e)
                          : MIDIDestinationCreate(client, name, read_proc, (void *)(uintptr_t)port, &e);
    CFRelease(name);
    if (err) {
        fprintf(stderr, "mk-sim: cannot create %s \"%s\" (CoreMIDI error %d)\n",
                is_src ? "source" : "destination", port_name[port], (int)err);
        return 0;
    }
    /* a fixed unique ID lets a DAW remember the port across runs */
    MIDIObjectSetIntegerProperty(e, kMIDIPropertyUniqueID, 0x4D4B5300 | port << 1 | is_src);
    MIDIObjectSetStringProperty(e, kMIDIPropertyManufacturer, CFSTR("My Kontrol"));
    MIDIObjectSetStringProperty(e, kMIDIPropertyModel, CFSTR("M32 Simulator"));
    return e;
}

int sim_midi_open(void)
{
    OSStatus err = MIDIClientCreate(CFSTR("My Kontrol Simulator"), NULL, NULL, &client);
    if (err) {
        fprintf(stderr, "mk-sim: cannot create a CoreMIDI client (error %d)\n", (int)err);
        return -1;
    }
    for (int i = 0; i < PORT_COUNT; i++) {
        src[i] = endpoint(i, 1);
        dst[i] = endpoint(i, 0);
        if (!src[i] || !dst[i])
            return -1;
    }
    return 0;
}

void sim_midi_close(void)
{
    for (int i = 0; i < PORT_COUNT; i++) {
        if (src[i]) MIDIEndpointDispose(src[i]);
        if (dst[i]) MIDIEndpointDispose(dst[i]);
        src[i] = dst[i] = 0;
    }
    if (client)
        MIDIClientDispose(client);
    client = 0;
}
