# Changelog

## Unreleased

- **Host protocol version 2** ([docs/protocol.md](docs/protocol.md)), so any DAW can build in native support:
  - The DAW sends each knob's name, group, kind and default once per page, then small value updates with its own value text.
  - A surface description in JSON tells a DAW what the keyboard has, so new models need no DAW changes.
  - DAW-defined knob modes, and the DAW can switch the mode when you click a plug-in (System > DAW Sets View).
  - Track and channel meters.
  - Smart Play follows the DAW's key (System > DAW Scale).
  - Drum lane names on screen as you play.
  - List and message screens for browsers, takes and assistants.
  - A DAW that stalls is marked lost instead of disappearing, and reconnects by itself.
  - A DAW can take the 4D encoder for itself; System > 4D as Keys is now Auto, Always or Off.
  - Version 1 hosts keep working.
- **Simulator** ([firmware/sim](firmware/sim/README.md)): the real firmware on macOS with the keyboard's MIDI ports, for testing DAW support without a keyboard.
- **Ableton Live script** speaks version 2.

## 0.9.0-beta.1

First public beta, for the Komplete Kontrol M32.

- **Standalone play**
  - Scales (quantize and easy), chords and chord memory.
  - Strum.
  - Arpeggiator with swing, hold and MIDI clock sync.
  - Note repeat.
  - Velocity curves, keyboard split, octave and transpose.
  - 4 knob pages, 8 presets.
- **DAWs**
  - Mackie Control for Logic and others.
  - Ableton Live script with device, mixer and clip views.
- **Lights and screen**
  - Button lights show what is available and what is on.
  - The Tempo light follows the clock, the DAW or your taps.
  - Screen saver.
- **Recovery and installing**
  - Crash recovery, and the Shift + Browser boot combo for update mode.
  - Web installer, including restoring NI's firmware from NI's updater.
