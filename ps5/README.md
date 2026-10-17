# Building the PS5 app

The PS5 app (title ID PPSA92046) is built on Linux. Nothing from the game is in this repository: the recompiled code is generated from your own `default.xbe`.

## What you need

- [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle), after `make deps` (the PS5 payload SDK and RADV from PS5_Mesa)
- `ps5/patches/radv-videoout-frame-rate.patch` applied to RADV before it's built (the 60/120 Hz switch used by frame generation)
- SDL3 3.4.18, glslang and the Vulkan headers built for the PS5 into one prefix (`DEFJAM_PS5_PREFIX`)
- Dear ImGui v1.92.9b
- The recompiled code: `scripts/pipeline.py analyze` and `recomp`, run on your own USA Xbox `default.xbe`

## Build

```
cmake -S . -B build/ps5 -G Ninja -DCMAKE_TOOLCHAIN_FILE=ps5/cmake/ps5-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release -DPS5_PAYLOAD_SDK=<sdk> -DDEFJAM_PS5_PREFIX=<prefix> \
  -DFETCHCONTENT_SOURCE_DIR_IMGUI=<imgui>
ninja -C build/ps5 defjam_recomp
ps5/tools/ps5_package.sh build/ps5 <ps5-plume-triangle> PPSA92046
```

The app folder ends up in `build/ps5/ps5-app/PPSA92046`. The art that isn't in this repository (`sce_sys` pictures, borders, `mods/custom`) is copied from `ps5/assets` when it's there.

## Where things are on the console

- Game files: `PPSA92046/GameData`
- Texture packs: `PPSA92046/mods/<pack>`
- Saves and `settings.ini`: `/data/DefJamFFNY` when it can be written, otherwise the app folder
- The log: `PPSA92046/logs/defjam.log.err`
