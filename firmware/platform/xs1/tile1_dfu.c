/* Tile 1's DFU backend (tile1_dfu.h). */
#include "tile1_dfu.h"
#include "upgrade.h"

static const spi_bus_t *bus;
static int chip;
static void (*reboot_fn)(int);
static upgrade_writer_t w;
static upgrade_slot_t slot;

static int begin(void) { return upgrade_begin(&w, bus, chip); }
static int write(const uint8_t *p, uint16_t n) { return upgrade_write(&w, p, n); }
static int end(void) { return upgrade_end(&w); }
static int revert(void) { return upgrade_revert(bus, chip); }
static void reboot(int into_dfu) { reboot_fn(into_dfu); }

static uint16_t read(uint32_t off, uint8_t *p, uint16_t n)
{
    if (off == 0 && upgrade_scan(bus, &slot))
        slot.len = 0;
    return (uint16_t)upgrade_read(bus, &slot, off, p, n);
}

static const dfu_backend_t backend = { begin, write, end, read, revert, reboot };

const dfu_backend_t *tile1_dfu_backend(const spi_bus_t *b, int c, void (*r)(int))
{
    bus = b, chip = c, reboot_fn = r;
    return &backend;
}
