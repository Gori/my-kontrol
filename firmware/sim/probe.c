/* mk-probe: a minimal My Kontrol host over CoreMIDI, and a test for the
   simulator or a real keyboard. It finds the "My Kontrol DAW" ports, then

     1. waits for HI           F0 7D 4D 4B 02 01 F7
     2. sends HELLO v2         and expects IDENT with proto 2
     3. sends DESCRIBE         joins the DESCRIPTION parts and checks the JSON

   and says BYE. With --listen it stays connected (PING every 500 ms) and
   prints everything the keyboard sends until Ctrl-C. Exit status 0 = all ok.

   usage: mk-probe [--port NAME] [--json] [--listen] */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <CoreMIDI/CoreMIDI.h>

static MIDIClientRef client;
static MIDIPortRef in_port, out_port;
static MIDIEndpointRef src, dst;
static volatile sig_atomic_t stop;

/* ---------- received messages ---------- */

typedef struct {
    uint16_t n;
    uint8_t d[1024];
} msg_t;

#define QLEN 256
static msg_t q[QLEN];
static unsigned q_head, q_tail;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;

static void q_put(const uint8_t *d, int n)
{
    pthread_mutex_lock(&q_lock);
    if (q_head - q_tail < QLEN) {
        msg_t *m = &q[q_head++ % QLEN];
        m->n = n < (int)sizeof m->d ? n : (int)sizeof m->d;
        memcpy(m->d, d, m->n);
    }
    pthread_mutex_unlock(&q_lock);
}

static int q_get(msg_t *out)
{
    pthread_mutex_lock(&q_lock);
    int got = q_tail != q_head;
    if (got)
        *out = q[q_tail++ % QLEN];
    pthread_mutex_unlock(&q_lock);
    return got;
}

/* CoreMIDI thread: a sysex may span packets and callbacks, realtime bytes
   may sit inside it; everything else arrives as whole messages */
static void read_proc(const MIDIPacketList *list, void *ref, void *src_ref)
{
    (void)ref; (void)src_ref;
    static uint8_t sx[1024];
    static int sx_n = -1;                  /* -1 = not in a sysex */
    const MIDIPacket *p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; i++, p = MIDIPacketNext(p)) {
        int k = 0;
        while (k < p->length) {
            uint8_t b = p->data[k];
            if (b >= 0xF8) {
                q_put(&b, 1);
                k++;
            } else if (b == 0xF0 || (sx_n >= 0 && !(b & 0x80))) {
                if (b == 0xF0)
                    sx_n = 0;
                if (sx_n < (int)sizeof sx)
                    sx[sx_n++] = b;
                k++;
            } else if (b == 0xF7 && sx_n >= 0) {
                if (sx_n < (int)sizeof sx)
                    sx[sx_n++] = b;
                q_put(sx, sx_n);
                sx_n = -1;
                k++;
            } else {
                sx_n = -1;                 /* a short message: up to the next status byte */
                int e = k + 1;
                while (e < p->length && !(p->data[e] & 0x80))
                    e++;
                q_put(p->data + k, e - k);
                k = e;
            }
        }
    }
}

/* ---------- sending ---------- */

static void send_bytes(const uint8_t *d, int n)
{
    Byte buf[1024];
    MIDIPacketList *list = (MIDIPacketList *)buf;
    MIDIPacket *p = MIDIPacketListInit(list);
    if (MIDIPacketListAdd(list, sizeof buf, p, 0, n, d))
        MIDISend(out_port, dst, list);
}

/* F0 7D 4D 4B cmd data... F7 */
static void send_cmd(uint8_t cmd, const uint8_t *data, int n)
{
    uint8_t m[256] = {0xF0, 0x7D, 0x4D, 0x4B, cmd};
    memcpy(m + 5, data, n);
    m[5 + n] = 0xF7;
    send_bytes(m, n + 6);
}

/* ---------- helpers ---------- */

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void pump(double ms)
{
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, ms / 1000, false);
}

static void hex(const uint8_t *d, int n, int max)
{
    for (int i = 0; i < n && i < max; i++)
        printf(" %02X", d[i]);
    if (n > max)
        printf(" ... (%d bytes)", n);
}

static int ours(const msg_t *m, uint8_t cmd)
{
    return m->n >= 6 && m->d[0] == 0xF0 && m->d[1] == 0x7D && m->d[2] == 0x4D && m->d[3] == 0x4B &&
           m->d[4] == cmd && m->d[m->n - 1] == 0xF7;
}

/* next message with this command within ms; others are dropped */
static int wait_cmd(uint8_t cmd, double ms, msg_t *out)
{
    double end = now_ms() + ms;
    while (now_ms() < end && !stop) {
        while (q_get(out))
            if (ours(out, cmd))
                return 1;
        pump(5);
    }
    return 0;
}

/* a virtual endpoint (the simulator) wins over a device with the same name */
static MIDIEndpointRef find(const char *name, int is_src)
{
    MIDIEndpointRef best = 0;
    ItemCount n = is_src ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; i++) {
        MIDIEndpointRef e = is_src ? MIDIGetSource(i) : MIDIGetDestination(i);
        CFStringRef s = 0;
        char b[128] = "";
        if (MIDIObjectGetStringProperty(e, kMIDIPropertyName, &s) == noErr && s) {
            CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8);
            CFRelease(s);
        }
        if (strcmp(b, name))
            continue;
        MIDIEntityRef ent = 0;
        if (MIDIEndpointGetEntity(e, &ent) != noErr || !ent)
            return e;
        if (!best)
            best = e;
    }
    return best;
}

/* ---------- JSON check: a small validating parser ---------- */

static const char *js;
static long board_id = -1;

static void ws(void) { while (*js == ' ' || *js == '\t' || *js == '\n' || *js == '\r') js++; }
static int value(int depth, const char *key);

static int string(char *out, int max)
{
    if (*js++ != '"')
        return 0;
    int n = 0;
    while (*js && *js != '"') {
        char c = *js++;
        if (c == '\\') {
            if (!*js)
                return 0;
            c = *js++;
            if (c == 'u') {
                for (int i = 0; i < 4; i++)
                    if (!strchr("0123456789abcdefABCDEF", *js++))
                        return 0;
                c = '?';
            } else if (!strchr("\"\\/bfnrt", c)) {
                return 0;
            }
        } else if ((unsigned char)c < 0x20) {
            return 0;
        }
        if (out && n < max - 1)
            out[n++] = c;
    }
    if (out)
        out[n] = 0;
    return *js++ == '"';
}

static int number(const char *key, int depth)
{
    char *end;
    double v = strtod(js, &end);
    if (end == js)
        return 0;
    if (depth == 1 && key && !strcmp(key, "board"))
        board_id = (long)v;
    js = end;
    return 1;
}

static int value(int depth, const char *key)
{
    ws();
    if (*js == '{') {
        js++;
        ws();
        if (*js == '}')
            return js++, 1;
        for (;;) {
            char k[64];
            ws();
            if (!string(k, sizeof k))
                return 0;
            ws();
            if (*js++ != ':' || !value(depth + 1, k))
                return 0;
            ws();
            if (*js == '}')
                return js++, 1;
            if (*js++ != ',')
                return 0;
        }
    }
    if (*js == '[') {
        js++;
        ws();
        if (*js == ']')
            return js++, 1;
        for (;;) {
            if (!value(depth + 1, 0))
                return 0;
            ws();
            if (*js == ']')
                return js++, 1;
            if (*js++ != ',')
                return 0;
        }
    }
    if (*js == '"')
        return string(0, 0);
    for (const char *const *w = (const char *const[]){"true", "false", "null", 0}; *w; w++)
        if (!strncmp(js, *w, strlen(*w)))
            return js += strlen(*w), 1;
    return number(key, depth);
}

static int json_ok(const char *s)
{
    js = s;
    if (!value(0, 0))
        return 0;
    ws();
    return *js == 0;
}

/* ---------- main ---------- */

static void on_signal(int sig) { (void)sig; stop = 1; }

static int fail(const char *what)
{
    printf("FAIL %s\n", what);
    send_cmd(0x04, 0, 0);                  /* BYE */
    pump(50);
    return 1;
}

int main(int argc, char **argv)
{
    const char *name = "My Kontrol DAW";
    int print_json = 0, listen = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            name = argv[++i];
        else if (!strcmp(argv[i], "--json"))
            print_json = 1;
        else if (!strcmp(argv[i], "--listen"))
            listen = 1;
        else {
            fprintf(stderr, "usage: mk-probe [--port NAME] [--json] [--listen]\n");
            return 2;
        }
    }
    signal(SIGINT, on_signal);

    if (MIDIClientCreate(CFSTR("mk-probe"), NULL, NULL, &client) ||
        MIDIInputPortCreate(client, CFSTR("in"), read_proc, NULL, &in_port) ||
        MIDIOutputPortCreate(client, CFSTR("out"), &out_port)) {
        fprintf(stderr, "mk-probe: CoreMIDI setup failed\n");
        return 1;
    }
    double t = now_ms();
    while (!(src = find(name, 1)) || !(dst = find(name, 0))) {
        if (now_ms() - t > 5000 || stop) {
            printf("FAIL no source and destination named \"%s\" (is mk-sim running?)\n", name);
            return 1;
        }
        pump(100);
    }
    MIDIPortConnectSource(in_port, src, NULL);
    printf("ports        \"%s\" source and destination\n", name);

    /* 1. HI: the keyboard announces itself every 2 s while no host is connected */
    msg_t m;
    t = now_ms();
    if (!wait_cmd(0x02, 2500, &m))
        return fail("no HI within 2.5 s");
    printf("HI           after %.0f ms:", now_ms() - t);
    hex(m.d, m.n, 16);
    printf("\n");
    if (m.n != 7 || m.d[5] != 0x01)
        return fail("HI is not F0 7D 4D 4B 02 01 F7");

    /* 2. HELLO v2: features 0x7F, name "Sim", version "1.0" */
    static const uint8_t hello[] = {0x02, 0x00, 0x7F, 3, 'S', 'i', 'm', 3, '1', '.', '0'};
    send_cmd(0x00, hello, sizeof hello);
    if (!wait_cmd(0x01, 500, &m))
        return fail("no IDENT within 500 ms of HELLO");
    const uint8_t *d = m.d + 5;
    int n = m.n - 6;
    printf("IDENT       ");
    hex(m.d, m.n, 32);
    printf("\n");
    if (n < 9)
        return fail("IDENT too short");
    printf("             proto %d, board %d, firmware %d.%d.%d, %d keys, %d knobs, caps 0x%02X, view %d\n",
           d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8]);
    if (n >= 20)
        printf("             max_rx %d, list rows %d cache %d, msg chars %d, key labels %d x %d, "
               "screen %d, lights %d, light guide %d\n",
               d[9] << 7 | d[10], d[11], d[12], d[13] << 7 | d[14], d[15], d[16], d[17], d[18], d[19]);
    if (d[0] != 2)
        return fail("IDENT proto is not 2");

    /* 3. DESCRIBE: DESCRIPTION part(u14) parts(u14) json... */
    send_cmd(0x07, 0, 0);
    static char json[16384];
    int len = 0, parts = -1, next = 0;
    while (parts < 0 || next < parts) {
        if (!wait_cmd(0x08, 500, &m))
            return fail(parts < 0 ? "no DESCRIPTION within 500 ms of DESCRIBE" : "DESCRIPTION parts missing");
        int part = m.d[5] << 7 | m.d[6], total = m.d[7] << 7 | m.d[8], k = m.n - 10;
        if (part != next)
            return fail("DESCRIPTION part out of order");
        parts = total;
        next++;
        if (len + k >= (int)sizeof json)
            return fail("DESCRIPTION too long");
        memcpy(json + len, m.d + 9, k);
        len += k;
    }
    json[len] = 0;
    int ok = json_ok(json);
    printf("DESCRIPTION  %d parts, %d bytes of JSON, %s, \"board\":%ld\n", parts, len,
           ok ? "parses" : "does NOT parse", board_id);
    if (print_json)
        printf("%s\n", json);
    if (!ok)
        return fail("DESCRIPTION is not valid JSON");
    if (board_id != 1)
        return fail("DESCRIPTION \"board\" is not 1");

    if (listen) {
        printf("listening; Ctrl-C to stop\n");
        double ping = now_ms();
        while (!stop) {
            while (q_get(&m)) {
                printf("%10.0f ", now_ms() - t);
                hex(m.d, m.n, 64);
                printf("\n");
            }
            fflush(stdout);
            if (now_ms() - ping >= 500) {
                ping = now_ms();
                send_cmd(0x03, 0, 0);      /* PING */
            }
            pump(5);
        }
    }
    send_cmd(0x04, 0, 0);                  /* BYE */
    pump(50);
    printf("ok\n");
    return 0;
}
