# My Kontrol Installer

Static WebUSB flasher (Chrome, Edge, Opera). No build step, no runtime dependencies (fflate is vendored in `vendor/`).

- Run locally: `cd web && python3 -m http.server`, then open http://localhost:8000 (WebUSB needs localhost or https).
- Tests: `cd web && node --test test/*.test.js` (NI files in `../fw/` and `python3` are used if present, skipped otherwise).
- Publish: CI builds `firmware/build/m32.bin`, copies it to `web/firmware/m32/my-kontrol-m32-<version>.bin`, writes its
  `version`, `file`, `size` and `sha256` as the first release in `web/firmware/manifest.json`, and deploys `web/` to GitHub Pages.
- NI firmware is never hosted here: "Restore NI firmware" extracts it in the browser from the user's own NI updater zip.
