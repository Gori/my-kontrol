# XS1 USB device over external ULPI PHY (clean-room)

Clean-room USB device driver for the XMOS XS1-L (external ULPI PHY) on the
Komplete Kontrol S-Series MK1. Written only from public specifications: the
**USB 2.0** spec, the **ULPI 1.1** spec, the **XS1 architecture manual**
(ports / clock blocks / channels), the **TinyUSB `dcd.h`** interface, and our own
RE notes `docs/hardware/s-mk1.md` + `fw/kks/notes/xmos.md §5`. No XMOS `lib_xud`
/ XUD source and no NI firmware were read, decompiled or copied.

## Files

| file | layer | tested on host |
|---|---|---|
| `usb_crc.c/.h` | CRC5 / CRC16 (USB 2.0 §8.3.5) | yes |
| `usb_packet.c/.h` | PID + token/data framing; ULPI register-cycle sequencer (abstract bus) | yes |
| `usb_ep.c/.h` | device endpoint / control-transfer state machine | yes |
| `usb_ulpi.c` | XS1 ports, clock block, PHY bring-up, packet TX/RX, SIE thread | no (no ULPI model / hardware) |
| `dcd_xs1_ulpi.c` | TinyUSB `dcd_*` over the SIE queues | in the emulator, with a host model in place of the SIE (`xtest.py usb`) |

Above the DCD (tile 1's USB device, milestone 4b): `usb/usb_link.c` (descriptors, MIDI and
keyboard queues to the tile link), `usb/dfu_xmos.c` (DFU 1.1 + XMOS requests),
`tile1_dfu.c` + `upgrade.c` (the upgrade image slot), `tusb/tusb_config.h`. Host-tested
through TinyUSB with a fake DCD (`test/test_usbdev.c`, `test/test_upgrade.c`) and run as
built for tile 1 in the emulator (`tools/xcore/xtest.py usb`: `s49emu.UsbHost` takes the
SIE thread's place at the queue level).

The pure layers (`usb_crc`, `usb_packet`, `usb_ep`) carry all the logic that can
be verified without hardware. The port, clock and channel code in `usb_ulpi.c`
and the channel code in `dcd_xs1_ulpi.c` are compiled and linked but cannot be
run here.

## ULPI signal → XS1 port map

Derived from ULPI 1.1 §3.1 (an 8-bit 60 MHz link needs an 8-bit bidirectional
data bus plus DIR, NXT from the PHY and STP to the PHY, all synchronous to the
PHY's 60 MHz CLK) and the pins our RE notes record NI using
(`fw/kks/notes/xmos.md §5`: CLK on 1H; data/control/flags on 8A–8D and 1N/1O/1P).

| ULPI signal | dir | XS1 port | width | why |
|---|---|---|---|---|
| CLK  | PHY→link | **1H** | 1 | ULPI clock source (fact, notes §5); drives clock block 1 |
| DATA[7:0] | bidir | **8A** | 8 | the only 8-bit bus needed; buffered, transfer width 8, clocked by 1H |
| DIR  | PHY→link | **1N** | 1 | bus-direction / turnaround (ULPI §3.4) |
| NXT  | PHY→link | **1O** | 1 | throttle; wired as the data port's ready-in (`setrdy`) |
| STP  | link→PHY | **1P** | 1 | end-of-transmit and register-write strobe |

NXT maps cleanly onto an XS1 buffered-port **ready-in** strobe, so during a
packet the data port's `in`/`out` self-throttle on NXT; DIR is sampled on its own
1-bit port to decide turnaround; STP is a plain output. This is the minimal
assignment the ULPI spec requires.

**8B, 8C, 8D** appear in NI's port setup in our RE notes but a single 8-bit ULPI
data bus needs only one 8-bit port. They are left unassigned here. The most
likely reason NI used a second 8-bit port is to avoid a dead ULPI cycle when the
bidirectional data bus turns around on an XS1 port (see Risks); the bring-up team
should confirm against the board which of 8A–8D the 8 data lines actually land on
and whether a second port is wired for turnaround.

Resource ids in `usb_ulpi.h` use the documented formula `width<<16 | index<<8`
with letter-order indices (A=0..P=15). The numeric ids and the `setc` control-
token values must be checked against the XS1-L port header for the exact die
before flashing.

## Thread / queue design

Two threads on the USB tile, no interrupts, sharing memory (same tile):

```
   wire (ULPI)            SIE thread (usb_ulpi.c)              TinyUSB thread (tile1.c usb_loop)
  ───────────►  owns 8A/1N/1O/1P + clkblk + usb_ep state   (usbd + class drivers, usb_link.c)
                        │   ▲                                   │   ▲
                        │   └── ulpi_cmdq ◄── ULPI_CMD_* (open/xfer/stall/addr) ──┘   │
                        └────── ulpi_evq ───► ULPI_EVT_* (setup/complete/reset) ──────┘
                 + a one-token "bell" on a chanend pair each way when a queue goes non-empty
```

- The **SIE thread** is the only owner of the ULPI ports and of the `usb_ep` state machine.
  It blocks in `wait_event()` on its chanend (a bell: commands queued) or wire activity
  (DIR), then dispatches.
- Messages are 4 words in two 16-entry queues (`ulpi_cmdq`, `ulpi_evq`, usb_ulpi.h). A
  producer that finds the consumer's `bell` flag clear sets it and sends one token; the
  consumer takes the token, clears the flag, then empties the queue. At most one token is
  in flight each way, so neither thread ever blocks on the other. (The first version sent
  every message as a channel packet both ways: a SIE posting an event while the DCD posts a
  command fills both 8-token buffers and the two threads wait for each other forever.)
- The TinyUSB thread waits on its chanend with a 1 ms timeout, pumps queued events into
  `dcd_event_*` (`usb_dcd_pump()`), then runs `tud_task()` and the link queues.
- Decoded MIDI/HID payloads cross the tile link to tile 0 (the app) through tile 1's main
  thread (tile1.c).

## Full speed vs high speed

**Target full speed (12 Mbit/s) for the first beta.** The stock device is high
speed (`docs/hardware/s-mk1.md` "Stock USB"). Reasons to start at FS:

- FS removes the HS chirp/handshake and the tight HS turnaround budget, which is
  the hardest part to get right on an XS1 port without hardware to measure.
- MIDI, HID and DFU are all low-bandwidth; FS bulk/interrupt is ample.
- The PHY still runs its ULPI link at 60 MHz in FS; only `FUNC_CTRL.XcvrSelect`
  / `TermSelect` change (ULPI 1.1 §4.1), so moving to HS later is a register
  change plus the HS detection sequence, not a rewrite.

`ulpi_phy_init_fs()` writes `FUNC_CTRL = SUSPENDM | TERMSELECT | XcvrSelect(FS)`
(0x45) and `OTG_CTRL = 0` (device, no host pulldowns).

## Spec sections used (provenance)

- **USB 2.0**: §8.3.1 PID encoding; §8.3.5.1 CRC5 (token); §8.3.5.2 CRC16 (data);
  §8.5.3 control-transfer stages and the DATA1-first data/status toggle; §9.4.6
  SET_ADDRESS applied after the status stage.
- **ULPI 1.1**: §3.1 8-bit 60 MHz interface signals; §3.4 DIR/turnaround; §3.8
  TX CMD / register read-write cycles (§3.8.1 transmit = TX CMD with PID then
  packet bytes, ended by STP; §3.8.2 immediate register write/read); §4.1
  FUNC_CTRL; §4.2 OTG_CTRL.
- **XS1 architecture manual**: buffered ports, `settw`/`setrdy`/`setpsc`, clock
  blocks and `setclk`, channel `out`/`in`/`outct`/`chkct`, events (`setv`/`eeu`/
  `waiteu`). The specific `setc` control-token *values* still need confirming
  against the manual's table.
- **TinyUSB**: `src/device/dcd.h` — the exact `dcd_*` functions implemented.

## Build / test

- Pure logic host test: `firmware/test/test_usb.c`, wired into
  `firmware/test/Makefile` (`make` runs it). Covers CRC5/CRC16, PID/token/data
  framing, the ULPI register-cycle sequencer against a fake bus, and a simulated
  enumeration: GET_DESCRIPTOR, SET_ADDRESS (address latched only after the status
  IN), SET_CONFIGURATION, plus multi-packet bulk IN toggling and stall. All pass.
- xcore build: all of it is in tile 1's image (`make BOARD=s49`, platform.mk TILE1_SRCS):
  the SIE, the DCD, TinyUSB (device core, MIDI, HID), our descriptors and DFU; about 16 KB
  of tile 1's 64 KB.
- `firmware/test/test_usbdev.c`: TinyUSB as configured for tile 1 with a fake DCD playing
  the host: both descriptor sets parsed, MIDI and keyboard both ways, the DFU state machine,
  downloads into the slot of a fake flash, the reboots after their status stages.
- `tools/xcore/xtest.py usb`: the tile 1 image in the emulator with `s49emu.UsbHost` in
  place of the SIE thread (same queues and bells): enumeration, MIDI through the tile link
  to the core and the 5-pin port, RESETINTODFU over a chip reset into the DFU device, a
  67 KB download into the upgrade slot and the upload, RESETFROMDFU.

## Risks for hardware bring-up (unverifiable here)

Fixed while wiring it in (4b): the clock block id (`ULPI_CLKBLK` was 1, a timer), starting
the clock block (setc START, not syncr, which is for ports), the setc values (now as xs1.h:
BUFFERS 0x200F, RDY_HANDSHAKE 0x3017, CLOCKPORT 0x500F), the SIE/DCD deadlock above.

Assessment: the wire layer as written is **not expected to work on hardware**; it is the
part to redesign at bring-up with a logic analyser. Everything above it is tested.

1. **60 MHz timing.** `rxpkt`/`txpkt` poll DIR, NXT and DATA with separate `in`s per byte.
   A thread gets at most 125 MIPS (500 MHz / 4; 100 with tile 1's 5 threads running),
   about 2 instructions per 16.7 ns ULPI cycle: far too few for three port reads, a test and a
   store. A working design has to let the ports do the per-cycle work: the data port
   clocked by 1H with transfer width 32 (one `in` per 4 bytes), NXT as its ready-in, DIR on
   a port event, and the turnaround cycle accounted for. NI's firmware sets up 8A, 8B, 8C,
   8D and 1N/1O/1P for its USB (fw notes xmos.md section 5): the XS1-L appears to have a
   ULPI-specific port arrangement (separate TX and RX data ports, register ports, decoded
   flags) that a clean-room driver would have to rediscover from the XS1-L datasheet.
2. **Data-bus turnaround** on one 8-bit port (`data_drive()` is a no-op today).
3. **No bus reset detection.** RX CMD line state (SE0 for > 2.5 us) is never parsed, so
   `ULPI_EVT_RESET` is never posted and the address is not cleared on a host reset;
   enumeration after a re-plug or a host-side reset would fail.
4. **Event loop** (`wait_event`) uses raw XS1 event vectoring; the arming order and `edu`
   placement need confirming on silicon.
5. **PHY reset timing**: the FUNC_CTRL.RESET pulse uses a spin delay.
6. No SOF handling, suspend/resume, or remote wakeup (not needed for MIDI/HID/DFU).
