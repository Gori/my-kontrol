#include "host.h"
#include "hal.h"
#include "version.h"
#include "mcu.h"
#include "midi.h"

host_state_t host;

static host_send_fn send;
static host_notify_fn notify;
static uint32_t silence_ms, hi_ms;
static uint16_t describe_part;          /* next DESCRIPTION part, 0xFFFF = none */

#define HI_EVERY_MS 2000
#define LOST_MS 3000
#define GONE_MS 33000                   /* 30 s after being marked lost */
#define DESCRIBE_CHUNK 96
#define SYSEX_OUT 112

static void sysex(uint8_t cmd, const uint8_t *d, uint8_t n)
{
    uint8_t m[SYSEX_OUT] = {0xF0, 0x7D, 0x4D, 0x4B, cmd};
    uint8_t k = 5;
    for (int i = 0; i < n && k < sizeof m - 1; i++)
        m[k++] = d[i] & 0x7F;
    m[k++] = 0xF7;
    send(m, k);
}

static void clear(void)
{
    uint8_t view = host.view;
    uint16_t gen = host.gen;
    uint8_t *p = (uint8_t *)&host;
    for (unsigned i = 0; i < sizeof host; i++)
        p[i] = 0;
    host.view = view < VIEW_COUNT ? view : VIEW_INSTRUMENT;   /* host-defined modes go with the host */
    host.beat = 127;
    for (int i = 0; i < HOST_LIST_CACHE; i++)
        host.rows[i].index = HOST_ROW_NONE;
    host.gen = gen + 1;
    describe_part = 0xFFFF;
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

static void describe_tick(void)
{
    if (describe_part == 0xFFFF)
        return;
    int len = 0;
    while (board_surface_json[len])
        len++;
    int parts = (len + DESCRIBE_CHUNK - 1) / DESCRIBE_CHUNK;
    if (describe_part >= parts) {
        describe_part = 0xFFFF;
        return;
    }
    if (midi_tx_free() < 64)            /* leave room for everything else */
        return;
    uint8_t d[4 + DESCRIBE_CHUNK];
    int off = describe_part * DESCRIBE_CHUNK, n = len - off < DESCRIBE_CHUNK ? len - off : DESCRIBE_CHUNK;
    d[0] = describe_part >> 7;
    d[1] = describe_part & 0x7F;
    d[2] = parts >> 7;
    d[3] = parts & 0x7F;
    for (int i = 0; i < n; i++)
        d[4 + i] = board_surface_json[off + i];
    sysex(0x08, d, 4 + n);
    describe_part++;
}

void host_tick_ms(uint32_t ms)
{
    describe_tick();
    if (host.connected) {
        silence_ms += ms;
        if (silence_ms >= GONE_MS) {
            clear();
        } else if (silence_ms >= LOST_MS && !host.lost) {
            host.lost = 1;
            host.gen++;
            hi_ms = HI_EVERY_MS;        /* ask for the handshake right away */
        }
        if (!host.lost)
            return;
    }
    hi_ms += ms;
    if (hi_ms >= HI_EVERY_MS && !mcu_connected) {   /* a Mackie host owns the port */
        hi_ms = 0;
        uint8_t d[1] = {1};
        sysex(0x02, d, 1);
    }
}

/* ---------- strings ---------- */

/* Latin-1 letters U+00C0..U+00FF without their accents */
static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

static char fold(uint32_t c)
{
    if (c >= 0x20 && c < 0x7F) return c;
    if (c >= 0xC0 && c <= 0xFF) return latin1[c - 0xC0];
    switch (c) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2018: case 0x2019: case 0x00B4: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x266D: return 'b';            /* flat */
    case 0x266F: return '#';            /* sharp */
    case 0x00B0: return 'o';            /* degree */
    case 0x00B5: case 0x03BC: return 'u';   /* micro */
    case 0x00A0: case 0x2009: case 0x202F: return ' ';
    case 0x2022: case 0x00B7: return '.';
    }
    return '?';
}

int host_string(const uint8_t *d, int len, int *pos, char *dst, int size, int keep_newline)
{
    int p = *pos, o = 0;
    if (p >= len) {
        if (size) dst[0] = 0;
        return 0;
    }
    int n = d[p++], end = p + n;
    if (end > len)
        end = len;
    while (p < end) {
        uint32_t c = d[p++];
        if (c == 0x7F) {
            if (p + 3 > end) break;
            c = (uint32_t)d[p] << 14 | d[p + 1] << 7 | d[p + 2];
            p += 3;
            if (c == 0x2026) {          /* ellipsis */
                for (int i = 0; i < 3 && o < size - 1; i++) dst[o++] = '.';
                continue;
            }
        }
        char ch = c == 0x0A ? (keep_newline ? '\n' : ' ') : fold(c);
        if (o < size - 1)
            dst[o++] = ch;
    }
    if (size)
        dst[o] = 0;
    *pos = end;
    return 1;
}

/* v1 strings: the rest of the message, ASCII */
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

/* ---------- v2 state ---------- */

static uint16_t u14(const uint8_t *d) { return d[0] << 7 | d[1]; }

static void ident(void)
{
    uint8_t id[24] = {host.proto >= 2 ? 2 : 1, board_info.id, MK_VERSION_MAJOR, MK_VERSION_MINOR,
                      MK_VERSION_PATCH, board_info.keys, board_info.knobs, board_info.caps, host.view};
    int n = 9;
    if (host.proto >= 2) {
        id[n++] = MIDI_SYSEX_MAX >> 7;
        id[n++] = MIDI_SYSEX_MAX & 0x7F;
        id[n++] = HOST_LIST_ROWS;
        id[n++] = HOST_LIST_CACHE;
        id[n++] = HOST_MSG_CHARS >> 7;
        id[n++] = HOST_MSG_CHARS & 0x7F;
        id[n++] = HOST_KEYS;
        id[n++] = HOST_KEY_CHARS;
        id[n++] = 1;                    /* screen: monochrome */
        id[n++] = 1;                    /* lights: levels */
        id[n++] = 0;                    /* light guide keys */
    }
    sysex(0x01, id, n);
}

static void hello(const uint8_t *d, int len)
{
    int fresh = !host.connected;
    if (fresh)
        clear();
    host.proto = len >= 1 && d[0] >= 2 ? 2 : 1;
    if (host.proto >= 2 && len >= 3) {
        int p = 3;
        host.features = u14(d + 1);
        host_string(d, len, &p, host.name, sizeof host.name, 0);
    }
    host.connected = 1;
    host.lost = 0;
    ident();
    if (fresh && notify)
        notify(host.name[0] ? host.name : "DAW", "Connected");
}

static void clear_slots(void)
{
    for (int k = 0; k < 8; k++) {
        host.knob_name[k][0] = host.knob_value[k][0] = host.group[k][0] = 0;
        host.knob_pos[k] = host.knob_flags[k] = host.slot_kind[k] = host.slot_steps[k] = 0;
        host.meter[k] = 0;
    }
}

static void modes(const uint8_t *d, int len)
{
    int p = 1, n = 0;
    for (int i = 0; i < (len ? d[0] : 0) && p + 2 <= len; i++) {
        uint8_t id = d[p], flags = d[p + 1];
        p += 2;
        char name[HOST_MODE_NAME];
        host_string(d, len, &p, name, sizeof name, 0);
        int ok = id < VIEW_COUNT || (id >= VIEW_HOST && id < VIEW_HOST_END);
        if (!ok || n >= HOST_MODES)
            continue;
        host.mode_id[n] = id;
        host.mode_flags[n] = flags;
        for (int c = 0; c < HOST_MODE_NAME; c++)
            host.mode_name[n][c] = name[c];
        n++;
    }
    host.n_modes = n;
    host.modes_gen++;
}

static void page(const uint8_t *d, int len)
{
    if (len < 4 || !d[0] || d[1] != host.view)
        return;
    host.page_gen = d[0];
    host.page_bank = d[2];
    host.page_banks = d[3];
    int p = 4;
    host_string(d, len, &p, host.device, sizeof host.device, 0);
    host_string(d, len, &p, host.subtitle, sizeof host.subtitle, 0);
    host.bank[0] = 0;
    clear_slots();
}

static void slot(const uint8_t *d, int len)
{
    if (len < 10 || d[0] != host.page_gen || !host.page_gen || d[1] >= 8)
        return;
    int k = d[1], kind = d[2] <= SLOT_ENUM ? d[2] : SLOT_CONTINUOUS;
    host.slot_kind[k] = kind;
    host.slot_steps[k] = d[4];
    host.knob_flags[k] = (kind ? 1 : 0) | (d[3] & 1 ? 2 : 0) | (kind >= SLOT_TOGGLE ? 4 : 0);
    int p = 10;                         /* after default (2) and colour (3) */
    host_string(d, len, &p, host.knob_name[k], sizeof host.knob_name[k], 0);
    host_string(d, len, &p, host.group[k], sizeof host.group[k], 0);
}

static void value(const uint8_t *d, int len)
{
    if (len < 4 || d[0] != host.page_gen || !host.page_gen || d[1] >= 8)
        return;
    int k = d[1];
    host.knob_pos[k] = u14(d + 2) >> 7;
    char t[sizeof host.knob_value[0]];
    int p = 4;
    if (host_string(d, len, &p, t, sizeof t, 0) && d[4])   /* empty text: unchanged */
        for (unsigned i = 0; i < sizeof t; i++)
            host.knob_value[k][i] = t[i];
}

static void meter(const uint8_t *d, int len)
{
    if (len < 4)
        return;
    int target = d[0], kind = d[1], n = d[3];
    if (n > len - 4)
        n = len - 4;
    if (target == 0 && d[2] == host.page_gen) {
        host.meter_kind = kind;
        for (int k = 0; k < 8; k++)
            host.meter[k] = k < n ? d[4 + k] : 0;
    } else if (target == 1) {
        host.channel_meter_kind = kind;
        host.channel_meter[0] = n > 0 ? d[4] : 0;
        host.channel_meter[1] = n > 1 ? d[5] : host.channel_meter[0];
    }
}

static void scale(const uint8_t *d, int len)
{
    if (len < 4)
        return;
    host.scale_state = d[0];
    host.scale_root = d[1] % 12;
    host.scale_mask = (u14(d + 2) | 1) & 0xFFF;
    int p = 4;
    host_string(d, len, &p, host.scale_name, sizeof host.scale_name, 0);
    host.scale_gen++;
}

static void key(const uint8_t *d, int len)
{
    if (len < 5)
        return;
    uint8_t note = d[0] & 0x7F, role = d[1];
    char label[HOST_KEY_CHARS + 1];
    int p = 5;
    host_string(d, len, &p, label, sizeof label, 0);
    int i = 0;
    while (i < host.keys_n && host.key_note[i] != note)
        i++;
    if (!role && !label[0]) {           /* clear this note */
        if (i < host.keys_n) {
            for (int j = i; j + 1 < host.keys_n; j++) {
                host.key_note[j] = host.key_note[j + 1];
                for (int c = 0; c <= HOST_KEY_CHARS; c++)
                    host.key_label[j][c] = host.key_label[j + 1][c];
            }
            host.keys_n--;
        }
        return;
    }
    if (i == host.keys_n) {
        if (host.keys_n >= HOST_KEYS)
            return;                     /* over the limit: the first ones stay */
        host.keys_n++;
    }
    host.key_note[i] = note;
    for (int c = 0; c <= HOST_KEY_CHARS; c++)
        host.key_label[i][c] = label[c];
}

static void drop_rows(void)
{
    for (int i = 0; i < HOST_LIST_CACHE; i++)
        host.rows[i].index = HOST_ROW_NONE;
}

static void list(const uint8_t *d, int len)
{
    if (len < 1)
        return;
    if (!d[0]) {
        host.list_id = 0;
        drop_rows();
        return;
    }
    if (len < 7)
        return;
    if (d[0] != host.list_id)
        drop_rows();
    host.list_id = d[0];
    host.list_total = u14(d + 1);
    host.list_cursor = u14(d + 3);
    host.list_first = u14(d + 5);
    int p = 7;
    host_string(d, len, &p, host.list_title, sizeof host.list_title, 0);
}

/* eviction order: farther from the cursor first, and of two equally far
   rows the one with the higher index */
static uint32_t evict_key(uint16_t index)
{
    uint16_t c = host.list_cursor;
    return (uint32_t)(index > c ? index - c : c - index) << 16 | index;
}

static void row(const uint8_t *d, int len)
{
    if (len < 4 || !host.list_id || d[0] != host.list_id)
        return;
    uint16_t index = u14(d + 1);
    int at = -1;
    for (int i = 0; i < HOST_LIST_CACHE && at < 0; i++)
        if (host.rows[i].index == index)
            at = i;
    for (int i = 0; i < HOST_LIST_CACHE && at < 0; i++)
        if (host.rows[i].index == HOST_ROW_NONE)
            at = i;
    if (at < 0) {                       /* full: drop whichever comes first in eviction order */
        uint32_t far = evict_key(index);
        for (int i = 0; i < HOST_LIST_CACHE; i++) {
            uint32_t k = evict_key(host.rows[i].index);
            if (k > far) {
                far = k;
                at = i;
            }
        }
        if (at < 0)
            return;                     /* the new row would be evicted first */
    }
    host_row_t *r = &host.rows[at];
    r->index = index;
    r->flags = d[3];
    int p = 4;
    host_string(d, len, &p, r->text, sizeof r->text, 0);
}

static void message(const uint8_t *d, int len)
{
    if (len < 1)
        return;
    if (!d[0]) {
        host.msg_id = 0;
        host.msg_len = 0;
        host.msg[0] = 0;
        return;
    }
    if (len < 3)
        return;
    if (d[1] == 0 || d[0] != host.msg_id) {
        host.msg_len = 0;
        host.msg[0] = 0;
    }
    host.msg_id = d[0];
    host.msg_flags = d[2];
    char part[65];
    int p = 3;
    host_string(d, len, &p, part, sizeof part, 1);
    for (int i = 0; part[i] && host.msg_len < HOST_MSG_CHARS; i++)
        host.msg[host.msg_len++] = part[i];
    host.msg[host.msg_len] = 0;
}

int host_sysex(const uint8_t *m, uint16_t n)
{
    if (n < 6 || m[1] != 0x7D || m[2] != 0x4D || m[3] != 0x4B)
        return 0;
    const uint8_t *d = m + 5;
    int len = n - 6;                    /* data bytes between cmd and F7 */
    silence_ms = 0;
    if (host.lost && m[4] != 0x00) {    /* any message: the host is back */
        host.lost = 0;
        host.gen++;
    }
    switch (m[4]) {
    case 0x00:                          /* HELLO */
        hello(d, len);
        break;
    case 0x03:                          /* PING */
        break;
    case 0x04:                          /* BYE */
        clear();
        break;
    default:
        if (!host.connected)
            return 1;                   /* ignore state until HELLO */
        switch (m[4]) {
        case 0x07: describe_part = 0; break;
        case 0x09: modes(d, len); break;
        case 0x0A: if (len >= 1) host.set_mode = d[0] + 1; break;
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
        case 0x29:                      /* LIGHT_RGB: no colour on this keyboard */
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
        case 0x20: page(d, len); break;
        case 0x21: slot(d, len); break;
        case 0x22: value(d, len); break;
        case 0x23: meter(d, len); break;
        case 0x24: scale(d, len); break;
        case 0x25: key(d, len); break;
        case 0x26: list(d, len); break;
        case 0x27: row(d, len); break;
        case 0x28: message(d, len); break;
        case 0x2A: host.keys_n = 0; break;
        }
    }
    host.gen++;
    return 1;
}

/* ---------- queries ---------- */

int host_mode_index(uint8_t view)
{
    if (!host.n_modes)
        return view < VIEW_COUNT ? view : -1;
    for (int i = 0; i < host.n_modes; i++)
        if (host.mode_id[i] == view)
            return i;
    return -1;
}

int host_mode_enabled(uint8_t view)
{
    int i = host_mode_index(view);
    return i >= 0 && (!host.n_modes || (host.mode_flags[i] & 1));
}

const char *host_mode_name(uint8_t view)
{
    int i = host.n_modes ? host_mode_index(view) : -1;
    return i >= 0 && host.mode_name[i][0] ? host.mode_name[i] : 0;
}

int host_modal(void) { return host.connected && (host.list_id || host.msg_id); }

const char *host_key_label(uint8_t note)
{
    for (int i = 0; i < host.keys_n; i++)
        if (host.key_note[i] == note)
            return host.key_label[i];
    return 0;
}

/* ---------- controls ---------- */

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
void host_note(uint8_t note, int on) { send3(0x9F, note & 0x7F, on ? 127 : 0); }
void host_knob(uint8_t k, int d) { if (d) send3(0xBF, 0x10 + (k & 7), rel(d)); }
void host_knob_touch(uint8_t k, int t) { send3(0x9F, 0x40 + (k & 7), t ? 127 : 0); }
void host_encoder(int d) { if (d) send3(0xBF, 0x30, rel(d)); }

void host_set_view(uint8_t view)
{
    if (view >= VIEW_COUNT && (view < VIEW_HOST || view >= VIEW_HOST_END))
        return;
    if (view != host.view) {
        host.view = view;
        host.page_gen = 0;              /* the old page's messages no longer apply */
        clear_slots();
    }
    host.gen++;
    if (host.connected)
        sysex(0x05, &view, 1);
}
