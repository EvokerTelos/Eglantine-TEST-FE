Portable settings for Eden FE: Fortune's Weave
=============================================

The Windows ZIP puts qt-config.ini at user/config/qt-config.ini beside eden.exe.
The user folder makes Eden store its settings and data in the extracted folder.
The preset is written from scratch and contains no personal paths or device IDs.

Preconfigured settings:
- 12 GB memory layout, Auto-Stub enabled
- Vulkan, Fast GPU mode, 1x internal resolution, Docked output (1440p)
- Bilinear scaling and SMAA
- Maximum GPU clocks and synchronized memory operations
- GPU ASTC decoding with BC3 recompression
- Asynchronous GPU emulation, presentation and shader compilation
- Disk shader cache enabled, Info logging

You still need to install your own keys and firmware and select your game dump.
Bind your controller as needed. If performance is low, switch Console Mode to
Handheld for 1080p output.

For an existing Eden installation, close both builds and copy your own keys and
nand folders from its data folder into this build's user folder to retain keys,
firmware and saves. Keep the supplied user/config for the new preset. The new
portable build does not automatically load settings or saves from %APPDATA%/eden.

For later portable updates, preserve your populated user folder and back it up
before replacing configuration files.
