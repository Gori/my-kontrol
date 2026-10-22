# Contributing

Very welcome, especially support for more keyboards ([porting guide](docs/porting.md)).

- **Bugs:** open an issue with your keyboard, firmware version (System > About), OS and DAW, and what you pressed.
- **New keyboard:** open an issue first with what you know about it. Someone may already be on it.
- **Code:**
  - Keep `firmware/core` free of hardware.
  - Add a host test when you fix something in the core: `firmware/test`, `make test`.
  - Match the style of the file you are in: C11, 4-space indent, short comments that say why.
- **Screens:** `make -C firmware/test preview` renders every screen to PNG. Attach before/after images to UI changes.
- **Never** add Native Instruments firmware, updaters or decompiled code to the repository.

By contributing you agree that your work is released under the GPL-3.0.
