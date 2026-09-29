# Contributing to NO SIGNAL

[中文](CONTRIBUTING_CN.md) · **English**

Thanks for wanting to help. This is a small hardware toy with a deliberately narrow design space,
so a five-minute read here saves a long review later.

## The five house rules

1. **Small patches to proven code.** This firmware is one 3,800-line `main.cpp` plus a handful of
   headers, and it is *verified on hardware*, not on a CI machine. Restructuring for taste costs
   more than it buys. Additive, local changes are what get merged.
2. **Evidence, not impressions.** "Should work" is not a result. If your change is visual, attach a
   framebuffer capture or a numeric measurement (see BUILDING.md §7). If your change is audible,
   describe the listening test you did on a device.
3. **Be honest about what you could not test.** Power-save and audio cannot be verified over the
   network harness (Light Sleep detaches USB CDC, backlight never touches the framebuffer). Saying
   "I could not verify this on battery" is worth more than a confident claim.
4. **Version discipline.** This project releases *small* numbers: v0.0.13 → v0.0.14 → … → v0.1.0.
   A visually huge change is still a patch bump. Do not introduce a new major/minor without asking.
5. **One concern per PR.** A new world subject, a fixed glyph and a refactor are three PRs.

## Code conventions

- **Comments are in Chinese.** That is the author's language and the codebase is written in it.
  English comments are fine in new files; please do not "translate" existing ones — it is churn.
- Static functions and file-local state, no C++ containers, no exceptions, no dynamic allocation
  inside the per-frame path. Big buffers (a whole frame) go to PSRAM explicitly.
- Everything is drawn straight into the RGB565 framebuffer. Do **not** use `M5.Display.drawString`
  or any canvas-relative drawing for screen elements: the canvas applies a coordinate transform at
  `pushSprite` time, and mixing the two is exactly the "sign is off-centre" bug of v0.2.
- Integer maths in the hot path. If you must use `sinf`/`sqrtf`, compute once per row or use a LUT.
- `Serial.printf` (ESP-IDF flavour) does not support `%lld`/`%f`; formatting those shifts every
  following argument.
- Keep the frame budget in mind: 15 fps = 66 ms. `pio run -e sticks3-prod` on a 240 MHz ESP32-S3
  has room, but post-processing 32,400 pixels per frame is where it goes.

## Adding a world channel (a new program subject)

The most welcome kind of contribution, and the most mechanical:

1. `src/main.cpp` — extend the `ProgSubj[]` table (id, display name, **category tag**: 0 nature /
   1 human / 2 mythic / 3 sci-fi). The category tag decides which sound profile the subject gets.
2. `progDrawBase()` — add a `case` for the new subject id, drawing the silhouette with the
   primitives from `src/prog_shapes.h` (`psTri` `psRect` `psCircle` `psEllipse` `psDome` `psSpire`
   `psObelisk` `psPillar` `psArch` `psBeam` `psVgrad` …). Silhouettes must read at a glance on a
   240×135 amber screen: abstract is fine, mush is not.
3. Nothing else is required — the recency memory, the station IDs, the celestial bodies, the
   textures and the action layers pick up the new subject automatically.
4. Verify: pin the subject through the harness (dev build), capture 4–6 frames across the 6 s
   program, and check the silhouette is visible in every phase (enter / hold / exit).

## Adding a warp

1. `g_g.warp` enum + `case` in the warp switch inside `sceneApply()`.
2. Constraints: operate in place on the framebuffer, allocate nothing, and if you shift rows
   *normalise the index* (`((x + shift) % SCR_W + SCR_W) % SCR_W`) — a negative index reads
   whatever sits before the scratch buffer in memory.
3. Verify with the `marker` injection (BUILDING.md §6): deterministic dots before the warp make a
   row shift measurable instead of eyeballed.

## Adding a sound profile

`src/prog_sound.h` holds the sound axis: `psPick()` chooses a profile from the subject category,
`psSetSnd()` latches it when a program starts, and `psSoundTick()` produces samples. The profiles
are built from a Geiger-style impulse generator plus tone steps, layered on the white-noise engine.
Add a profile by extending the enum, the picker and the tick switch — and remember the volume has
to stay under the noise bed, otherwise the "picture and sound tell one story" trick falls apart.

## Pull requests

Use the PR template. It asks for the two things that matter here: **which envs you built** and
**what evidence you have**. If your change is visual, attach the capture; if it is timing-related,
say which part you could not verify. A PR that says "tested on a real StickS3, capture attached,
could not test power save on battery" is easy to merge.
