#pragma once
/* DFU 1.1 with XMOS's vendor requests, as a TinyUSB class driver (usbd_app_driver_get_cb).
   One driver for both modes:
   - runtime (in the normal device): DETACH and XMOS RESETINTODFU (0x21 F2) reboot into
     DFU mode; RESETDEVICE (F0) reboots;
   - DFU mode (its own device, PID +2, this interface only): DNLOAD writes an image, UPLOAD
     reads the installed one back, REVERTFACTORY (F1) removes it so the factory image boots,
     RESETFROMDFU (F3) reboots into the normal device; SELECTIMAGE, SAVESTATE, RESTORESTATE
     (F4..F6) are accepted and do nothing.
   Requests and order as NI's updater uses them (docs/hardware/s-mk1.md, Update mode):
   64-byte DNLOAD blocks, GETSTATUS after each, a zero-length DNLOAD, GETSTATUS until idle.
   The flash side is the backend's (platform/xs1/upgrade.c on the S MK1). Reboots happen
   after the request's status stage. Host-tested (test/test_usbdev.c). */
#include <stdint.h>
#include "tusb.h"
#include "device/usbd_pvt.h"

#define DFU_XMOS_RESETDEVICE    0xF0
#define DFU_XMOS_REVERTFACTORY  0xF1
#define DFU_XMOS_RESETINTODFU   0xF2
#define DFU_XMOS_RESETFROMDFU   0xF3
#define DFU_XMOS_SELECTIMAGE    0xF4
#define DFU_XMOS_SAVESTATE      0xF5
#define DFU_XMOS_RESTORESTATE   0xF6
#define DFU_XMOS_BLOCK          64          /* wTransferSize */

typedef struct {
    int (*begin)(void);                          /* the first DNLOAD block; 0 = ok, else a DFU status */
    int (*write)(const uint8_t *p, uint16_t n);
    int (*end)(void);                            /* after the zero-length DNLOAD: check and commit */
    uint16_t (*read)(uint32_t off, uint8_t *p, uint16_t n);   /* UPLOAD; fewer than n = the end */
    int (*revert)(void);                         /* REVERTFACTORY */
    void (*reboot)(int into_dfu);                /* does not return on the device */
} dfu_backend_t;

void dfu_xmos_setup(const dfu_backend_t *backend, int dfu_mode);
uint8_t dfu_xmos_state(void);                    /* DFU state (dfu.h), for tests */
extern const usbd_class_driver_t dfu_xmos_driver;
