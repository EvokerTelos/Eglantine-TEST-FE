# Validated NS2 optimizations (2026-10-31)

The user tested the Windows GUI build on Fortune's Weave and confirmed that
shaders build faster and slow the game down less while they build.

The following changes were adapted from the `nx2` branch of
[Djihads80/Eglantine](https://github.com/Djihads80/Eglantine):

| Source | Change |
| --- | --- |
| PR #78 | Request subgroup size 32 only for compute shaders that use warp operations. |
| PR #83 | Capture and lower each native program once per `ShaderInfo`, then reuse it for pipeline environments. Read instructions in 4 KiB pages with `ReadBlockUnsafe`. |
| PR #83 | Recognize the native address map during disk-cache replay so cached programs use the same constant-buffer folding as live programs. |
| PR #113 | Increase storage-buffer bindings per stage from 16 to 32, with a bitmask-size assertion. |
| PR #29 | Write the Vulkan driver pipeline cache to a temporary file before replacing the previous file; recreate caches shorter than their header. |
| PR #109 (partial) | Reject macro-bind indices outside the table and bound the error log volume. |
| PR #19 (partial) | Demote repeated native-lowering and storage-buffer tracking logs to Debug. |

The reused native program also retains the Ampere frontend state, indirect
tables, trap prefix and source-header size. Compute trap guards still receive
their per-dispatch checks. The alternative-frontend compute twin does not reuse
the default frontend's lowering.

## Validation and limits

- The Windows MSVC Vulkan GUI configuration built and linked successfully.
- The user confirmed the performance improvement in the packaged GUI build.
- OpenGL was disabled in this configuration, so its matching compute-environment
  change has not been compiled here.
- The Windows build has no unit-test target; the unit suite was not rerun for
  this optimization set.
- The subgroup change also affects the game's internal upscaler. A controlled
  frame comparison for changes in sharpness or temporal smearing remains useful.
- A controlled cold-cache/warm-cache rendering comparison remains useful for the
  disk-cache replay correction.

## Deferred candidates

PR #84 (Switch 2 VRAM budgeting), PR #52 (GPU-written command lists in Fast mode)
and PR #51 (deferred instance draws) require separate ports and test cycles.
PR #114 depends on Eglantine's `MappedBacking` implementation and is not portable
as a patch to this tree.
