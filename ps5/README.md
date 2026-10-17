# Def Jam: Fight for NY for PS5

A native PS5 homebrew build of Def Jam: Fight for NY, from vyanhursky's static recompilation of the original Xbox game. Title ID PPSA92004.

## Install

1. Copy the `PPSA92004` folder to `/data/homebrew/` on your PS5.
2. Copy everything from your own extracted **USA Xbox** disc into `PPSA92004/GameData`: `default.xbe`, the `.xml` files and the `assets`, `audio`, `fonts`, `motion`, `movies` and `screens` folders.
3. Launch it from your homebrew launcher.

Saves, `settings.ini` and the log (`logs/defjam.log.err`) are kept in the app folder.

## Build

On Linux, with:

- ps5-plume-triangle, after `make deps` (the PS5 payload SDK and RADV)
- SDL3 3.4.18, glslang and the Vulkan headers built for the PS5 into one prefix (`DEFJAM_PS5_PREFIX`)
- the lifted code from `scripts/pipeline.py analyze` and `recomp`, run on your own `default.xbe`

```
cmake -S . -B build/ps5 -G Ninja -DCMAKE_TOOLCHAIN_FILE=ps5/cmake/ps5-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release -DPS5_PAYLOAD_SDK=<sdk> -DDEFJAM_PS5_PREFIX=<prefix> \
  -DFETCHCONTENT_SOURCE_DIR_IMGUI=<imgui v1.92.9b>
ninja -C build/ps5 defjam_recomp
ps5/tools/ps5_package.sh build/ps5 <ps5-plume-triangle> PPSA92004
```

## What's different on PS5

- The picture goes straight to the TV through VK_KHR_display on RADV.
- The DualSense is read with scePad and handed to SDL as a gamepad.
- Sound goes out through sceAudioOut.
- Shaders are compiled on the console with glslang (`ps5/shaderc`).
- The PS5's 16 KB pages are handled in the device-register traps.

## Credits

- **vyanhursky**: [DefJamFFNY-recomp](https://github.com/vyanhursky/DefJamFFNY-recomp), the recompilation this is built on
- **sp00nz**: xboxrecomp
- **elripalda**: the PS5 port
- ps5-payload-dev, mihawk-99 (PS5_Mesa, the payload SDK fork) and Nazky (ps5-plume-triangle) for the PS5 toolchain

Unofficial fan project, not affiliated with EA, AKI, Def Jam, Microsoft or Sony. No game files are included.
