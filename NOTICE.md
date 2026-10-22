# Third-party components

My Kontrol is licensed under the GNU General Public License v3.0 (see LICENSE).
It includes or links the following, under their own licenses:

| Component | Where | License |
|---|---|---|
| TinyUSB | `firmware/lib/tinyusb` (submodule) | MIT, (c) hathach (tinyusb.org) |
| CMSIS 6 core headers | `firmware/lib/CMSIS_6` (submodule) | Apache-2.0, (c) Arm Limited |
| STM32F1 device headers | `firmware/lib/cmsis_device_f1` (submodule) | Apache-2.0, (c) STMicroelectronics |
| Spleen bitmap fonts 5x8, 8x16, 12x24 | `fonts/spleen`, compiled into `firmware/core/fonts.h` | BSD-2-Clause, (c) 2018-2026 Frederic Cambus |
| fflate | `web/vendor/fflate.js` | MIT, (c) 2023 Arjun Barrett |
| Geist | `web/fonts` | SIL Open Font License 1.1, (c) 2023 Vercel |

## Spleen license (required in binary distributions)

```
Copyright (c) 2018-2026, Frederic Cambus
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## Native Instruments

No Native Instruments code, firmware or other copyrighted material is part of
this repository. The web installer can restore NI's firmware by extracting it,
in the user's browser, from NI's official updater that the user downloads
from NI. The hardware notes in `docs/hardware/` describe how the keyboard
works; they were written by observing the hardware and NI's firmware.
