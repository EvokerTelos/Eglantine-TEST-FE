# Portrait edge quality and diagnostics

The opening dialogue portraits can show jagged edges with Nearest Neighbor output
scaling and anti-aliasing disabled. For Fortune's Weave, use the game's Properties
to select **Bilinear** window adapting/scaling filter and **SMAA** anti-aliasing.
These are existing presentation settings; this change does not enable them by default.

On Windows, the same opening dialogue was compared at 2560x1440 game output with
1920x1080 emulator frame captures. Bilinear plus SMAA visibly smoothed portrait
outlines and internal lines. The comparison presented at approximately 30 fps
over a ten-second steady window, with no black captured frames during the observed
dialogue interval or logged guest panics. Saves were unchanged. Both runs ended at
the harness timeout; extended gameplay and clean shutdown were not verified.

The two settings were tested together. This establishes a presentation workaround,
not a sampler decoding or shader translation fix. No forced guest filtering was
used in that comparison. Existing Linux packages were not rebuilt for this test.

## Optional developer diagnostics

- `EDEN_NX2_SAMPLER_DIAGNOSTICS=1` logs filtering, LOD clamps and bias for the first
  64 constructed samplers.
- `EDEN_NX2_FORCE_FILTER=linear` forces linear filtering and removes the LOD clamp.
  `point` or `nearest` forces nearest filtering and disables anisotropy. These
  overrides are diagnostic experiments, not the validated portrait workaround;
  leave them unset for normal play.
- `EDEN_NATIVE_TEXTURE_REQUESTS=1` now also permits bounded texture and sampler
  snapshots armed by `native-textures.request` in the emulator's working directory.
  The capture covers the next 64 graphics draws and retains its existing limits.
  Request polling uses the shared throttled scan, avoiding a filesystem probe on
  every draw. Full `EDEN_NATIVE_DRAW_DIAGNOSTICS` is no longer required.

The Windows GUI and CLI were rebuilt. The narrow capture path successfully emitted
atlas data, texture descriptors and sampler metadata during the dialogue test.
No unit-test suite was rerun for these diagnostic changes. Captures, game assets,
saves, keys and firmware are private local test inputs and are not included here.

AI assistance: Codex helped investigate, validate and document these changes.
