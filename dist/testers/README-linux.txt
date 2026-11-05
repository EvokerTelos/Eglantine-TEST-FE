Eden test build: Fire Emblem: Fortune's Weave (Switch 2), Linux
================================================================

Unofficial test build of the Eden emulator with experimental Switch 2 support.
Linux x86_64 AppImage. Expect bugs.

You must provide your own game dump, keys and firmware from your own console.
None are included, so please don't ask for them.


WHAT'S NEW IN THIS BUILD
------------------------
- Sharper picture: the game now renders at 2560x1440 when Console Mode is
  Docked and 1920x1080 in Handheld. Before, it was stuck at 1280x720.
- Movies (such as the opening movie) play at full speed. They were slow before.
- Fewer slowdowns while shaders build. Eden no longer rebuilds the same shaders
  over and over, and existing shader caches load much faster.
- Black flicker and black screens are fixed (cutscenes, battles, title screen).
- Crash shortly before character creation is fixed (with asynchronous
  presentation on).


SETUP
-----
1. Make the AppImage executable and run it:
       chmod +x eden-ns2-fe-linux-*.AppImage
       ./eden-ns2-fe-linux-*.AppImage
   If it does not start, your distribution may need FUSE 2 (often a package
   called fuse2 or libfuse2). The uruntime-based local AppImage also supports:
       ./eden-ns2-fe-linux-*.AppImage --appimage-extract-and-run
2. Install your keys and firmware, and add your game folder, as usual.
   For an extracted game, choose File > Load Folder and select the directory
   containing main and main.npdm (usually base/exefs). This avoids file-picker
   filters that hide the extensionless main executable.
3. Settings (Emulation > Configure):
     System > System:      Memory Layout = 12GB DRAM (Unsafe)
                           Console Mode = Docked (1440p) or Handheld (1080p,
                           lighter on the GPU)
     Graphics:             Window Adapting Filter = Bilinear
                           Anti-Aliasing = SMAA (portrait workaround tested on Windows)
     Graphics > Advanced:  GPU Mode = Fast
                           Force maximum clocks = on
                           Sync Memory Operations = on
                           ASTC Recompression Method = BC3 (Medium quality)
     Graphics > Extras:    Enable asynchronous presentation = on
                           Enable asynchronous shader compilation = on
     General > Debug:      Enable Auto-Stub = on (required, or the game stops at boot)

This build uses the same folders as any other Eden install (~/.config/eden and
~/.local/share/eden). Back up ~/.config/eden first if you also use another Eden
build.


KNOWN ISSUES
------------
- A few seconds of black screen at boot, before the title, is normal.
- The first time a new effect or area appears, expect a short stutter while its
  shaders build. Later runs reuse the shader cache.
- GPU Mode "Accurate" works but is slow (single-digit fps in battles).
  Use Fast.
- If the game runs slowly on your GPU, try Console Mode = Handheld (1080p).
- This Linux build is new and less tested than the Windows one.


WHAT TO TEST AND REPORT
-----------------------
Please play normally, including cutscenes and several battles, and report:
- any black frames, flicker or black screens
- any stretch where the game crawls (note where you were and roughly how long)
- movies that stutter, run slowly or show wrong colours
- crashes or freezes, with what you were doing

With each report, send the log file:
    ~/.local/share/eden/log/eden_log.txt
Copy it right after the problem: it is overwritten on the next launch (the
previous one is kept as eden_log.txt.old.txt). Also mention your distribution,
GPU, and driver (for example Mesa 25.x or NVIDIA 580.x).


SOURCE
------
Eden is licensed under the GPL v3 (see LICENSE.txt). This build is based on
Eden commit a3debcf plus local Switch 2 changes; the source is available on
request.

Linux source build and AppImage packaging instructions:
    docs/build/Linux-NS2.md in this repository
