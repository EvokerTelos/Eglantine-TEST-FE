Eden test build: Fire Emblem: Fortune's Weave (Switch 2), 2026-10-31
=======================================================================

Unofficial test build of the Eden emulator with experimental Switch 2 support.
Windows x64 only. Expect bugs.

You must provide your own game dump, keys and firmware from your own console.
None are included, so please don't ask for them.


WHAT'S NEW IN THIS BUILD
------------------------
- Sharper picture: the game now renders at 2560x1440 when Console Mode is
  Docked and 1920x1080 in Handheld. Before, it was stuck at 1280x720.
- Movies (such as the opening movie) play at full speed. They were slow before.
- Fewer slowdowns while shaders build. Eden no longer rebuilds the same shaders
  over and over, and existing shader caches load much faster.
- Native shader capture and translation are now reused across pipelines, and
  compute shaders request a 32-wide subgroup only when they need warp operations.
  The user confirmed faster shader builds with less gameplay slowdown.
- Existing shader caches now retain native constant-buffer folding on replay.
- Shader stages support 32 storage buffers, driver pipeline-cache writes use a
  temporary file, and invalid macro-bind indices are rejected safely.
- Black flicker and black screens are fixed (cutscenes, battles, title screen).
- Crash shortly before character creation is fixed (with asynchronous
  presentation on).


SETUP
-----
1. Extract into a new folder and run eden.exe. The Visual C++ runtime is included.
   Keep the included user folder beside eden.exe for portable mode. The settings
   below are already configured, including Bilinear scaling and SMAA.
2. Install your keys and firmware, and add your game folder, as usual.
3. Settings (Emulation > Configure):
     System > System:      Memory Layout = 12GB DRAM (Unsafe)
                           Console Mode = Docked (1440p) or Handheld (1080p,
                           lighter on the GPU)
     Graphics:             Window Adapting Filter = Bilinear
                           Anti-Aliasing = SMAA (smoother portrait edges)
     Graphics > Advanced:  GPU Mode = Fast
                           Force maximum clocks = on
                           Sync Memory Operations = on
                           ASTC Recompression Method = BC3 (Medium quality)
     Graphics > Extras:    Enable asynchronous presentation = on
                           Enable asynchronous shader compilation = on
     General > Debug:      Enable Auto-Stub = on (required, or the game stops at boot)

This portable build stores its configuration and data in user beside eden.exe.
It does not automatically load your existing %APPDATA%\eden data. To retain your
own keys, firmware and saves, close both builds and copy the keys and nand folders
from the old data folder into the new user folder. Keep the supplied user/config
to use the new preset. See user/README.txt for details.


KNOWN ISSUES
------------
- A few seconds of black screen at boot, before the title, is normal.
- The first time a new effect or area appears, expect a short stutter while its
  shaders build. Later runs reuse the shader cache.
- GPU Mode "Accurate" works but is slow (single-digit fps in battles).
  Use Fast.
- If the game runs slowly on your GPU, try Console Mode = Handheld (1080p).


WHAT TO TEST AND REPORT
-----------------------
Please play normally, including cutscenes and several battles, and report:
- any black frames, flicker or black screens
- any stretch where the game crawls (note where you were and roughly how long)
- movies that stutter, run slowly or show wrong colours
- crashes or freezes, with what you were doing
- picture differences between the first launch and later launches with a warm
  shader cache, or changes in sharpness and temporal smearing

With each report, send the log file:
    user\log\eden_log.txt (inside the extracted folder)
Copy it right after the problem: it is overwritten on the next launch (the
previous one is kept as eden_log.txt.old.txt). Also mention your GPU and driver
version.


SOURCE
------
Eden is licensed under the GPL v3 (see LICENSE.txt). This build is based on
Eden commit a3debcf plus local Switch 2 changes; the source is available on
request.
