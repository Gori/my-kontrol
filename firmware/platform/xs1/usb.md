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
| `usb_ulpi.c` | XS1 ports, clock block, PHY bring-up, packet TX/RX, SIE thread | no (no USB model / hardware) |
| `dcd_xs1_ulpi.c` | TinyUSB `dcd_*` over the SIE channel | interface check only |

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

## Thread / channel design

Two threads on the USB tile (segment 1 in our map), one XS1 channel between them,
no interrupts:

```
   wire (ULPI)            SIE thread (usb_ulpi.c)              TinyUSB thread
  ───────────►  owns 8A/1N/1O/1P + clkblk + usb_ep state   (usbd + class drivers)
                        │   ▲                                   │   ▲
          tokens/data   │   │ usb_ep callbacks                  │   │ dcd_* calls
                        ▼   │                                   ▼   │
                   service_wire()                         dcd_edpt_xfer/open/...
                        │   │                                   │   │
                        └── channel ◄── ULPI_CMD_* (open/xfer/stall/addr) ──┘
                            channel ──► ULPI_EVT_* (setup/complete/reset/sof) ──► usb_dcd_pump() → dcd_event_*
```

- The **SIE thread** is the only owner of the ULPI ports and of the `usb_ep`
  state machine, so no shared-memory races. It blocks in `wait_event()` on either
  the channel (a DCD command) or wire activity (DIR edge), then dispatches.
- Host-initiated transactions on the wire drive `usb_ep_on_setup/in/out`; the
  responses (DATA/ACK/NAK/STALL) go straight back on the wire.
- Endpoint completions and SETUP packets are posted to the TinyUSB thread as
  fixed 4-word channel messages ended by one control token. `usb_dcd_pump()`
  (the event path, in place of an ISR — `dcd_int_handler` forwards to it) turns
  them into `dcd_event_*` calls.
- DCD calls from the stack (`dcd_edpt_open/xfer/stall`, `dcd_set_address`) become
  `ULPI_CMD_*` messages applied to `usb_ep` inside the SIE thread.
- Decoded MIDI/HID payloads cross the inter-tile link to tile 0 (the app); that
  link is outside this driver.

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
- xcore build: `usb_crc/usb_packet/usb_ep/usb_ulpi` compile with
  `clang --target=xcore`, assemble with `tools/xcore/xas.py` and link into a tile
  image with `tools/xcore/xld.py` (≈2.5 KB). `dcd_xs1_ulpi.c` compiles for xcore
  against the real TinyUSB `dcd.h` (with `CFG_TUSB_MCU = OPT_MCU_NONE` and the
  xcore libc headers in `tools/xcore/test/include`).

## Risks for hardware bring-up (unverifiable here)

1. **Data-bus turnaround.** Turning one 8-bit XS1 port between drive and sample
   at 60 MHz without losing a ULPI cycle on DIR changes is the single biggest
   risk. NI's use of extra 8-bit ports (8B–8D) suggests a two-port scheme; this
   driver uses one port and must be checked against the real timing.
2. **`setc` control-token values** in `usb_ulpi.c` and the **port resource ids**
   in `usb_ulpi.h` are written from the documented encodings but not verified
   against the XS1-L header for this die.
3. **60 MHz packet TX/RX timing** (`txpkt`/`rxpkt`) and the NXT ready-strobe wiring
   are untested; the per-byte NXT handshake and the RX end-of-packet (DIR low)
   detection need a logic-analyser pass.
4. **Event loop** (`wait_event`) uses raw XS1 event vectoring; the arming order
   and `edu` placement need confirming on silicon or in an XC `select`.
5. **PHY reset timing**: the FUNC_CTRL.RESET pulse uses a spin delay, not a timed
   wait against the ULPI reset spec.
6. No SOF handling, suspend/resume, or remote wakeup yet (not needed for the
   first beta; `dcd_sof_enable`/`dcd_remote_wakeup` are stubs).
