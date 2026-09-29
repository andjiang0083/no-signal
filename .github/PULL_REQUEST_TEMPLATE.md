## What this changes / 这个 PR 做了什么

<!-- One or two sentences. 一两句说清。 -->

## How it was verified / 怎么验证的

- [ ] `pio run -e sticks3` (dev) builds with no new warnings
- [ ] `pio run -e sticks3-prod` (release) builds with no new warnings
- [ ] Flashed to a real M5StickS3 and exercised on the device
- [ ] Evidence attached (framebuffer capture / numeric measurement / boot log) — screenshots of the serial console alone are not evidence, see CONTRIBUTING.md
- [ ] If this touches the sign text or the bitmap font: every affected string was visually checked glyph by glyph (missing glyphs render as silently dropped letters)
- [ ] If this touches the power-save path: tested **on battery** (USB power deliberately bypasses Light Sleep)
- [ ] `README.md` / `README_CN.md` / `CHANGELOG.md` updated if behaviour or version changed
- [ ] Documentation is bilingual where it is user-facing (README/BUILDING/CONTRIBUTING/ROADMAP)

## Realm of the change / 影响范围

- [ ] Snow engine / warp pipeline
- [ ] Program subjects (38 worlds)
- [ ] Sound axis
- [ ] Sign text / font
- [ ] Power save / clock
- [ ] Time sync
- [ ] Tooling / docs only (no firmware behaviour change)
