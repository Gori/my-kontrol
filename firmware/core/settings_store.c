/* Settings persistence.

   Settings: an append-only log of CRC-checked records in two flash pages.
   The newest valid record wins; when a page fills, the other one is erased
   and becomes current. A power cut loses at most the record being written.

   Presets: 8 slots stored as one block, alternating between two pages. A
   block's header (with a CRC over all slots) is written last, so a power
   cut during a save leaves the previous block intact.

   Flash writes stall the CPU for up to ~40 ms, so the application only
   saves while nothing is being played (settings_tick_ms idle flag). */
#include "settings_store.h"
#include "hal.h"
#include "settings.h"

#define MAGIC 0x4B4D5331u        /* "1SMK" */
#define PRESET_MAGIC 0x4B4D5031u /* "1PMK" */
#define SETTINGS_VERSION 4       /* bump when settings_t changes: older records are ignored */
#define SAVE_DELAY_MS 2000

typedef struct {
    uint32_t magic;
    uint16_t version, seq;
    settings_t data;
    uint32_t crc;
} record_t;

#define REC_SIZE ((sizeof(record_t) + 3) & ~3u)

typedef struct {
    uint32_t magic;
    uint16_t version, seq;
    uint32_t crc;                /* over PRESETS * REC_SIZE slot bytes */
    uint32_t pad;
} preset_hdr_t;

#define PRESETS 8
#define SLOT(page, i) ((page) + sizeof(preset_hdr_t) + (i) * REC_SIZE)

_Static_assert(REC_SIZE <= FLASH_PAGE, "settings record larger than a page");
_Static_assert(sizeof(preset_hdr_t) + PRESETS * REC_SIZE <= FLASH_PAGE, "presets do not fit a page");

static uintptr_t cur_page;
static uint32_t next_off;
static uint16_t seq;
static uint32_t save_delay_ms;

static uint32_t crc32_update(uint32_t c, const uint8_t *p, uint32_t n)
{
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? (c >> 1) ^ 0xEDB88320 : c >> 1;
    }
    return c;
}

static uint32_t crc32(const void *p, uint32_t n)
{
    return ~crc32_update(0xFFFFFFFF, p, n);
}

static int valid(const record_t *r)
{
    return r->magic == MAGIC && r->version == SETTINGS_VERSION &&
           r->crc == crc32(r, sizeof *r - sizeof r->crc);
}

static void make_record(record_t *rec, uint16_t s)
{
    uint8_t *p = (uint8_t *)rec;
    for (unsigned i = 0; i < sizeof *rec; i++)
        p[i] = 0;
    rec->magic = MAGIC;
    rec->version = SETTINGS_VERSION;
    rec->seq = s;
    rec->data = settings;
    rec->crc = crc32(rec, sizeof *rec - sizeof rec->crc);
}

/* Write a record padded with 0xFF to REC_SIZE. */
static int write_record(uintptr_t addr, const record_t *rec)
{
    uint8_t buf[REC_SIZE];
    const uint8_t *src = (const uint8_t *)rec;
    for (unsigned i = 0; i < REC_SIZE; i++)
        buf[i] = i < sizeof *rec ? src[i] : 0xFF;
    return hal_flash_write(addr, buf, REC_SIZE);
}

/* ---------- settings log ---------- */

/* Scan one page: newest valid record (or 0) and the first free offset. */
static const record_t *scan(uintptr_t page, uint32_t *free_off)
{
    const record_t *best = 0;
    uint32_t off = 0;
    for (; off + REC_SIZE <= FLASH_PAGE; off += REC_SIZE) {
        const record_t *r = (const record_t *)(page + off);
        if (r->magic == 0xFFFFFFFF)
            break;
        if (valid(r) && (!best || (int16_t)(r->seq - best->seq) > 0))
            best = r;
    }
    /* leftovers from other firmware: if the tail is not erased, call the
       page full so the next save erases before writing */
    for (uint32_t i = off; i < FLASH_PAGE; i += 4)
        if (*(const volatile uint32_t *)(page + i) != 0xFFFFFFFF) {
            off = FLASH_PAGE;
            break;
        }
    *free_off = off;
    return best;
}

void settings_load(void)
{
    uint32_t fa, fb;
    const record_t *a = scan(FLASH_SETTINGS_A, &fa), *b = scan(FLASH_SETTINGS_B, &fb);
    const record_t *r = a;
    cur_page = FLASH_SETTINGS_A;
    next_off = fa;
    if (b && (!a || (int16_t)(b->seq - a->seq) > 0)) {
        r = b;
        cur_page = FLASH_SETTINGS_B;
        next_off = fb;
    }
    if (r) {
        settings = r->data;
        seq = r->seq;
    } else {
        settings_defaults(&settings);
        seq = 0;
    }
    settings_sanitize(&settings);
}

static int same_as_saved(void)
{
    uint32_t dummy;
    const record_t *last = scan(cur_page, &dummy);
    if (!last || last->seq != seq)
        return 0;
    const uint8_t *x = (const uint8_t *)&last->data, *y = (const uint8_t *)&settings;
    for (unsigned i = 0; i < sizeof settings; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

int settings_save_now(void)
{
    save_delay_ms = 0;
    if (same_as_saved())
        return 0;
    if (next_off + REC_SIZE > FLASH_PAGE) {
        cur_page = cur_page == FLASH_SETTINGS_A ? FLASH_SETTINGS_B : FLASH_SETTINGS_A;
        if (hal_flash_erase(cur_page))
            return -1;
        next_off = 0;
    }
    record_t rec;
    make_record(&rec, seq + 1);
    int r = write_record(cur_page + next_off, &rec);
    next_off += REC_SIZE;
    if (r || !valid((const record_t *)(cur_page + next_off - REC_SIZE)))
        return -1;
    seq++;
    return 0;
}

void settings_touch(void)
{
    save_delay_ms = SAVE_DELAY_MS;
}

int settings_pending(void)
{
    return save_delay_ms != 0;
}

void settings_tick_ms(uint32_t ms, int idle)
{
    if (!save_delay_ms)
        return;
    if (ms < save_delay_ms) {
        save_delay_ms -= ms;
        return;
    }
    if (!idle) {
        save_delay_ms = 1;               /* due: write as soon as playing stops */
        return;
    }
    settings_save_now();
}

/* ---------- presets ---------- */

static int block_valid(uintptr_t page)
{
    const preset_hdr_t *h = (const preset_hdr_t *)page;
    return h->magic == PRESET_MAGIC && h->version == SETTINGS_VERSION &&
           h->crc == crc32((const void *)SLOT(page, 0), PRESETS * REC_SIZE);
}

static uintptr_t preset_page(void)
{
    int a = block_valid(FLASH_PRESETS_A), b = block_valid(FLASH_PRESETS_B);
    if (a && b) {
        const preset_hdr_t *ha = (const preset_hdr_t *)FLASH_PRESETS_A;
        const preset_hdr_t *hb = (const preset_hdr_t *)FLASH_PRESETS_B;
        return (int16_t)(hb->seq - ha->seq) > 0 ? FLASH_PRESETS_B : FLASH_PRESETS_A;
    }
    return a ? FLASH_PRESETS_A : b ? FLASH_PRESETS_B : 0;
}

static const record_t *preset(int slot)
{
    uintptr_t page = preset_page();
    if (!page || slot < 0 || slot >= PRESETS)
        return 0;
    const record_t *r = (const record_t *)SLOT(page, slot);
    return valid(r) ? r : 0;
}

int presets_used(int slot)
{
    return preset(slot) != 0;
}

int presets_load(int slot)
{
    const record_t *r = preset(slot);
    if (!r)
        return -1;
    settings = r->data;
    settings_sanitize(&settings);
    return 0;
}

int presets_save(int slot)
{
    if (slot < 0 || slot >= PRESETS)
        return -1;
    uintptr_t old = preset_page();
    uintptr_t page = old == FLASH_PRESETS_A ? FLASH_PRESETS_B : FLASH_PRESETS_A;
    uint16_t s = old ? ((const preset_hdr_t *)old)->seq + 1 : 1;
    if (hal_flash_erase(page))
        return -1;

    /* slots first, copying the others from the previous block */
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < PRESETS; i++) {
        record_t rec;
        const record_t *prev = old ? (const record_t *)SLOT(old, i) : 0;
        if (i == slot) {
            make_record(&rec, 0);
        } else if (prev && valid(prev)) {
            rec = *prev;
        } else {
            uint8_t *p = (uint8_t *)&rec;
            for (unsigned k = 0; k < sizeof rec; k++)
                p[k] = 0xFF;
        }
        if (write_record(SLOT(page, i), &rec))
            return -1;
        crc = crc32_update(crc, (const uint8_t *)SLOT(page, i), REC_SIZE);
    }
    preset_hdr_t h = {PRESET_MAGIC, SETTINGS_VERSION, s, ~crc, 0xFFFFFFFF};
    if (hal_flash_write(page, &h, sizeof h))
        return -1;
    return presets_used(slot) ? 0 : -1;
}
