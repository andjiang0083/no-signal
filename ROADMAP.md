# Roadmap

[中文](ROADMAP_CN.md) · **English**

Everything here is a concrete, claimable item. Nothing on this list is a promise with a date; the
project releases when something is verified on hardware. If you want one, open a short issue saying
which item and what you plan to check, so two people do not do the same work.

## 1. Performance — make the glitch scenes frame-stable

The glitch scenes are the only heavy part of the firmware: every frame they run a full-frame
post-process over 32,400 pixels. On a 240 MHz ESP32-S3 there is headroom, but the v0.0.16 audit
found three cheap wins that were never implemented:

| Item | Where | The idea |
|---|---|---|
| Per-row precompute in `pincApply` | `main.cpp` | `srcY` only depends on `y`; compute it 135 times per frame instead of once per pixel |
| Incremental maths in `zoomApply` | `main.cpp` | `(x - cx) * k` can accumulate with `+k` per step instead of a multiply per pixel |
| Reuse the vignette LUT in `vignApply` | `main.cpp` / `breath.h` | `breath.h` already builds `s_vigC[256]`; the warp currently recomputes `dx*dx + dy*dy` per pixel |

**Verification**: add a temporary `#ifdef` timing probe, measure the scene-frame cost before and
after, and report both numbers. (The probe must not stay in the release build.)

## 2. Structure — split `progDrawBase()`

It is ~1,500 lines, of which ~1,150 are the subject switch. The original plan was
`progSubjNature/Civil/Myth/Scifi` (optionally in a new `prog_subj.h`). Adding subject #39 currently
means scrolling through a 1,150-line switch; after the split it is a one-line dispatch.
**Constraint**: a pure move — no behaviour change, no renaming, one commit, verified by a
before/after capture of the same pinned subject.

## 3. Robustness — stop hand-pairing the RNG state

`progDrawBase()` saves `rngState`, swaps in `progSeed`, and restores it at every exit. Both exits
are correct today, but one missed restore means "elements drift frame to frame" — a nasty bug class.
**Idea**: pass the state as a parameter (`fastRand(uint32_t&)`), or use a tiny RAII guard.

## 4. Content — more worlds

Candidates that fit the current palettes: a lighthouse at dusk, a desert observatory dome, a
cable-stayed bridge, a salt flat mirror, a storm-chasing radar dish, a submerged wreck, a Mars
rover convoy, a space elevator, a drowned city. Each is one `ProgSubj[]` row + one draw case
(see CONTRIBUTING.md). Please attach a capture of 4–6 frames.

## 5. Content — more warps and sign texts

Untried warps: chroma split with a shifted duplicate row, per-region brightness pumping, a rolling
bar with a soft edge, a tape-drop (vertical overshoot with a settle), a sync loss that re-locks
gradually. Untried sign texts: a rolling caption, a price-tag style ticker, an emergency-broadcast
strip, a station clock with seconds.

## 6. Sound — a tuning pass

The five event profiles are synthesised and chosen by subject family. What is missing is an
objective seat-of-the-pants pass: play each profile 10 times each, note which ones read as "TV
static artefact" versus "computer beeping", and adjust the impulse rate / decay / tone ratio.
Needs a device and ears, not a PR full of code.

## 7. Tooling

- `tools/capture.py` — a script that reproduces the README images deterministically (pin a scene
  through the harness, capture, upscale 4×, save JPEG). Today those images were made by hand.
- `make-release.sh` — version bump in all the places listed in BUILDING.md §10, build both envs,
  produce the merged image, verify the layout, write the `.sha256` files.

## 8. Documentation

- **A real photograph of the device** for the top of the README. The current images are framebuffer
  captures, which are honest but flat; a photo of the thing sitting on a desk would do more for the
  project than any paragraph here.
- A `PORTS.md` note on what it would take to run this on a StickC or a Cardputer (different
  display, different buttons, no PSRAM on the StickC — the scene pool would have to shrink).

## Done in v0.1.0 (for reference)

The whole v0.0.16 audit — 1 P0, 8 P1, 9 P2 — plus the on-device verification of every fix:
see [`docs/CODE-REVIEW-v0.0.16.md`](docs/CODE-REVIEW-v0.0.16.md) and
[`docs/VERIFY-v0.1.0.md`](docs/VERIFY-v0.1.0.md). Highlights: the sound axis was never wired
(4 of 5 profiles were dead code), 7 glyphs were missing so `CH 5` rendered as `C 5`, the power-save
wake pins pointed at the PSRAM bus, and 28.8 KB of static RAM was reserved for a 60-row buffer that
already existed.
