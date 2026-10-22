#include "host.h"
#include "hal.h"
#include "version.h"
#include "mcu.h"

host_state_t host;

static host_send_fn send;
static host_notify_fn notify;
static uint32_t silence_ms, hi_ms;

#define HI_EVERY_MS 2000
#define TIMEOUT_MS 3000

static void sysex(uint8_t cmd, const uint8_t *d, uint8_t n)
{
    uint8_t m[24] = {0xF0, 0x7D, 0x4D, 0x4B, cmd};
    uint8_t k = 5;
    for (int i = 0; i < n && k < sizeof m - 1; i++)
        m[k++] = d[i] & 0x7F;
    m[k++] = 0xF7;
    send(m, k);
}

static void clear(void)
{
    uint8_t view = host.view;
    uint8_t *p = (uint8_t *)&host;
    for (unsigned i = 0; i < sizeof host; i++)
        p[i] = 0;
    host.view = view;
    host.beat = 127;
    host.gen++;
}

void host_init(host_send_fn s, host_notify_fn n)
{
    send = s;
    notify = n;
    clear();
    hi_ms = HI_EVERY_MS - 300;    /* first HI shortly after boot */
}

void host_usb_reset(void)
{
    clear();
    hi_ms = HI_EVERY_MS - 300;
}

void host_tick_ms(uint32_t ms)
{
    if (host.connected) {
        silence_ms += ms;
        if (silence_ms >= TIMEOUT_MS)
            clear();
        return;
    }
    hi_ms += ms;
    if (hi_ms >= HI_EVERY_MS && !mcu_connected) {   /* a Mackie host owns the port */
        hi_ms = 0;
        uint8_t d[1] = {1};
        sysex(0x02, d, 1);
    }
}

static void copy_text(char *dst, int size, const uint8_t *src, int n)
{
    int i = 0;
    for (; i < n && i < size - 1; i++)
        dst[i] = src[i] >= 0x20 && src[i] < 0x7F ? src[i] : ' ';
    dst[i] = 0;
}

static void text(uint8_t field, const uint8_t *s, int n)
{
    char *dst = 0;
    int size = 0;
    if (field <= HT_SUBTITLE) {
        char *f[4] = {host.track, host.device, host.bank, host.subtitle};
        dst = f[field];
        size = HOST_TEXT_LEN;
    } else if (field >= HT_KNOB_NAME && field < HT_KNOB_NAME + 8) {
        dst = host.knob_name[field - HT_KNOB_NAME];
        size = sizeof host.knob_name[0];
    } else if (field >= HT_KNOB_VALUE && field < HT_KNOB_VALUE + 8) {
        dst = host.knob_value[field - HT_KNOB_VALUE];
        size = sizeof host.knob_value[0];
    } else if (field >= HT_COL && field < HT_COL + GRID_COLS) {
        dst = host.col[field - HT_COL];
        size = sizeof host.col[0];
    } else if (field >= HT_ROW && field < HT_ROW + GRID_ROWS) {
        dst = host.row[field - HT_ROW];
        size = sizeof host.row[0];
    }
    if (dst)
        copy_text(dst, size, s, n);
}

int host_sysex(const uint8_t *m, uint16_t n)
{
    if (n < 6 || m[1] != 0x7D || m[2] != 0x4D || m[3] != 0x4B)
        return 0;
    const uint8_t *d = m + 5;
    int len = n - 6;                    /* data bytes between cmd and F7 */
    silence_ms = 0;
    switch (m[4]) {
    case 0x00: {                        /* HELLO */
        int was = host.connected;
        if (!was)
            clear();
        host.connected = 1;
        uint8_t id[9] = {1, board_info.id, MK_VERSION_MAJOR, MK_VERSION_MINOR, MK_VERSION_PATCH,
                         board_info.keys, board_info.knobs, board_info.caps, host.view};
        sysex(0x01, id, sizeof id);
        if (!was && notify)
            notify("Ableton Live", "Connected");
        break;
    }
    case 0x03:                          /* PING */
        break;
    case 0x04:                          /* BYE */
        clear();
        break;
    default:
        if (!host.connected)
            return 1;                   /* ignore state until HELLO */
        switch (m[4]) {
        case 0x10:
            if (len >= 1)
                text(d[0], d + 1, len - 1);
            break;
        case 0x11:
            if (len >= 3 && d[0] < 8) {
                host.knob_pos[d[0]] = d[1];
                host.knob_flags[d[0]] = d[2];
            }
            break;
        case 0x12:
            if (len >= 2 && d[0] < sizeof host.light)
                host.light[d[0]] = d[1];
            break;
        case 0x13:
            if (len >= 5) {
                uint8_t beat = d[3];
                host.transport = d[0];
                host.tempo_x10 = d[1] << 7 | d[2];
                if (beat != 127 && (host.transport & HOST_TF_PLAYING))
                    host.beat_gen++;
                host.beat = beat;
                host.beats_per_bar = d[4];
            }
            break;
        case 0x14:
            if (len >= 4) {
                host.grid_cols = d[0] > GRID_COLS ? GRID_COLS : d[0];
                host.grid_rows = d[1] > GRID_ROWS ? GRID_ROWS : d[1];
                host.cursor_col = d[2];
                host.cursor_row = d[3];
                for (int r = 0; r < host.grid_rows; r++)
                    for (int c = 0; c < host.grid_cols; c++) {
                        int i = 4 + r * d[0] + c;
                        host.grid[r][c] = i < len ? d[i] : GRID_NONE;
                    }
            }
            break;
        case 0x15:
            if (notify && len > 0) {
                char title[22], value[22];
                int i = 0;
                while (i < len && d[i])
                    i++;
                copy_text(title, sizeof title, d, i);
                copy_text(value, sizeof value, d + i + 1, i + 1 < len ? len - i - 1 : 0);
                notify(title, value);
            }
            break;
        }
    }
    host.gen++;
    return 1;
}

/* signed-bit relative: 1..63 up, 65..127 down */
static uint8_t rel(int d)
{
    if (d > 63) d = 63;
    if (d < -63) d = -63;
    return d >= 0 ? d : 0x40 | -d;
}

static void send3(uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t m[3] = {a, b, c};
    send(m, 3);
}

void host_button(uint8_t ctl, int down) { send3(0x9F, ctl & 0x7F, down ? 127 : 0); }
void host_knob(uint8_t k, int d) { if (d) send3(0xBF, 0x10 + (k & 7), rel(d)); }
void host_knob_touch(uint8_t k, int t) { send3(0x9F, 0x40 + (k & 7), t ? 127 : 0); }
void host_encoder(int d) { if (d) send3(0xBF, 0x30, rel(d)); }

void host_set_view(uint8_t view)
{
    if (view >= VIEW_COUNT)
        return;
    host.view = view;
    host.gen++;
    if (host.connected)
        sysex(0x05, &view, 1);
}
