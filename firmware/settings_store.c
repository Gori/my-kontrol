/* Settings persistence: append-only log of CRC-checked records in two flash
   pages. The newest valid record wins; when a page fills, the other page is
   erased and becomes current. Both pages are 2 KB aligned and below the
   bootloader flag page, so this holds for 1 KB and 2 KB erase sizes. */
#include "settings_store.h"
#include "hw.h"
#include "settings.h"

#define PAGE_A 0x0801E800u
#define PAGE_B 0x0801F000u
#define PAGE_SIZE 1024u
#define MAGIC 0x4D333253u  /* "S23M" */
#define VERSION 2  /* bump when settings_t changes: old records are ignored */

typedef struct {
    uint32_t magic;
    uint16_t version, seq;
    settings_t data;
    uint32_t crc;
} record_t;

#define REC_SIZE ((sizeof(record_t) + 3) & ~3u)

static uint32_t cur_page, next_off;
static uint16_t seq;
static uint32_t save_delay_ms;

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFF;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? (c >> 1) ^ 0xEDB88320 : c >> 1;
    }
    return ~c;
}

static int valid(const record_t *r)
{
    return r->magic == MAGIC && r->version == VERSION &&
           r->crc == crc32((const uint8_t *)r, sizeof *r - sizeof r->crc);
}

/* Scan one page: returns newest valid record (or 0) and the first free offset. */
static const record_t *scan(uint32_t page, uint32_t *free_off)
{
    const record_t *best = 0;
    uint32_t off = 0;
    for (; off + REC_SIZE <= PAGE_SIZE; off += REC_SIZE) {
        const record_t *r = (const record_t *)(page + off);
        if (r->magic == 0xFFFFFFFF)
            break;
        if (valid(r) && (!best || (int16_t)(r->seq - best->seq) > 0))
            best = r;
    }
    /* leftovers from other firmware: if the tail isn't erased, call it full so
       the next save erases a page before writing */
    for (uint32_t i = off; i < PAGE_SIZE; i += 4)
        if (*(const volatile uint32_t *)(page + i) != 0xFFFFFFFF) {
            off = PAGE_SIZE;
            break;
        }
    *free_off = off;
    return best;
}

void settings_load(void)
{
    uint32_t fa, fb;
    const record_t *a = scan(PAGE_A, &fa), *b = scan(PAGE_B, &fb);
    const record_t *r = a;
    cur_page = PAGE_A;
    next_off = fa;
    if (b && (!a || (int16_t)(b->seq - a->seq) > 0)) {
        r = b;
        cur_page = PAGE_B;
        next_off = fb;
    }
    if (r) {
        settings = r->data;
        seq = r->seq;
    } else {
        settings_defaults(&settings);
        seq = 0;
    }
}

void settings_save_now(void)
{
    record_t rec = {MAGIC, VERSION, (uint16_t)(seq + 1), settings, 0};
    rec.crc = crc32((const uint8_t *)&rec, sizeof rec - sizeof rec.crc);

    /* nothing changed since the last save */
    uint32_t dummy;
    const record_t *last = scan(cur_page, &dummy);
    if (last && last->seq == seq) {
        const uint8_t *x = (const uint8_t *)&last->data, *y = (const uint8_t *)&settings;
        unsigned i = 0;
        while (i < sizeof settings && x[i] == y[i])
            i++;
        if (i == sizeof settings)
            return;
    }

    if (next_off + REC_SIZE > PAGE_SIZE) {
        cur_page = cur_page == PAGE_A ? PAGE_B : PAGE_A;
        flash_erase_page(cur_page);
        next_off = 0;
    }
    uint8_t buf[REC_SIZE];
    const uint8_t *src = (const uint8_t *)&rec;
    for (unsigned i = 0; i < REC_SIZE; i++)
        buf[i] = i < sizeof rec ? src[i] : 0xFF;
    flash_program(cur_page + next_off, buf, REC_SIZE);
    next_off += REC_SIZE;
    seq++;
}

/* ---------- user presets: 8 records in their own page ---------- */

#define PRESET_PAGE 0x0801E000u
#define PRESETS 8

static uint8_t page_buf[PAGE_SIZE];

int presets_load(int slot)
{
    if (slot < 0 || slot >= PRESETS)
        return -1;
    const record_t *r = (const record_t *)(PRESET_PAGE + slot * REC_SIZE);
    if (!valid(r))
        return -1;
    settings = r->data;
    return 0;
}

int presets_save(int slot)
{
    if (slot < 0 || slot >= PRESETS || PRESETS * REC_SIZE > PAGE_SIZE)
        return -1;
    const uint8_t *src = (const uint8_t *)PRESET_PAGE;
    for (unsigned i = 0; i < PAGE_SIZE; i++)
        page_buf[i] = src[i];
    record_t rec = {MAGIC, VERSION, 0, settings, 0};
    rec.crc = crc32((const uint8_t *)&rec, sizeof rec - sizeof rec.crc);
    uint8_t *dst = page_buf + slot * REC_SIZE;
    const uint8_t *s = (const uint8_t *)&rec;
    for (unsigned i = 0; i < REC_SIZE; i++)
        dst[i] = i < sizeof rec ? s[i] : 0xFF;
    flash_erase_page(PRESET_PAGE);
    flash_program(PRESET_PAGE, page_buf, PRESETS * REC_SIZE);
    return presets_load(slot);   /* read back to verify */
}

int presets_used(int slot)
{
    return slot >= 0 && slot < PRESETS && valid((const record_t *)(PRESET_PAGE + slot * REC_SIZE));
}

void settings_touch(void)
{
    save_delay_ms = 3000;
}

void settings_tick_ms(uint32_t ms)
{
    if (!save_delay_ms)
        return;
    if (ms >= save_delay_ms) {
        save_delay_ms = 0;
        settings_save_now();
    } else {
        save_delay_ms -= ms;
    }
}
