#!/usr/bin/env bash
# [ps5 port] Link the PS5 build of Def Jam: Fight for NY and package it as a
# PS5 homebrew app folder (eboot.bin, sce_module/libc.prx, sce_sys/param.json).
#
#   ps5/tools/ps5_package.sh <cmake build dir> <ps5-plume-triangle checkout> [title id]
#
# The link and packaging reuse the PS5 tooling of Nazky's ps5-plume-triangle
# (mihawk-99's RADV link recipe, app CRT, libc.prx and FSELF signer), as the
# FIFA Street port does. Run its `make deps` once; the app CRT and the AGC
# stubs are built here if its build/ does not have them yet.
# No game files are packaged: the player copies their own extracted disc into
# the app's GameData folder.
set -euo pipefail

build=$(realpath "${1:?cmake build dir}")
tri=$(realpath "${2:?ps5-plume-triangle checkout}")
title_id=${3:-PPSA92046}
port=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)

sdk_root=${PS5_PAYLOAD_SDK:-$tri/.deps/native/ps5-payload-sdk}
radv_archive=${RADV_ARCHIVE:-$tri/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
tool=$tri/build/host/ps5-native-tool
if [[ ! -f $tri/build/obj/app_crt.o || ! -f $tri/build/stubs/libSceAgc.so ]]; then
    echo "==> [runtime] app CRT and AGC stubs from ps5-plume-triangle"
    mkdir -p "$tri/build/obj" "$tri/build/stubs" "$tri/build/host"
    cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$tri/tooling/prospero-clang18" "$@"; }
    cc -std=c++20 -O2 -Wall -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
        -c "$tri/tooling/native/app_crt.cpp" -o "$tri/build/obj/app_crt.o"
    cc -std=c++20 -O2 -Wall -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
        -c "$tri/tooling/native/app_cpp_runtime.cpp" -o "$tri/build/obj/app_cpp_runtime.o"
    for pair in libSceAgc:agc_canary_link_stub.c libSceAgcDriver:agc_driver_canary_link_stub.c; do
        lib=${pair%%:*}; src=${pair#*:}
        cc -std=c11 -O2 -fPIC -c "$tri/vendor/ps5/sdk/stubs/$src" -o "$tri/build/obj/${lib}_stub.o"
        "$sdk_root/bin/prospero-lld" --shared -soname "${lib}.prx" \
            -o "$tri/build/stubs/${lib}.so" "$tri/build/obj/${lib}_stub.o"
    done
    [[ -x $tool ]] || { cp -f "$tri/tooling/native/ps5-native-tool" "$tool"; chmod +x "$tool"; }
fi
for f in "$sdk_root/bin/prospero-lld" "$radv_archive" "$tool" "$tri/build/obj/app_crt.o" \
         "$tri/build/obj/app_cpp_runtime.o" "$tri/build/stubs/libSceAgc.so" "$tri/runtime/libc.prx"; do
    [[ -e $f ]] || { echo "missing $f (build ps5-plume-triangle first)" >&2; exit 2; }
done

# Staging root for radv_link_recipe: it reads tooling/ and vendor/ and writes build/.
stage=$build/ps5-link
mkdir -p "$stage/build"
ln -sfn "$tri/tooling" "$stage/tooling"
ln -sfn "$tri/vendor" "$stage/vendor"

# The objects and static libraries CMake links into defjam_recomp, in its order.
link_cmd=$(ninja -C "$build" -t commands defjam_recomp | tail -1)
mapfile -t inputs < <(cd "$build" && for w in $link_cmd; do
    case $w in
        *.o|*.a) [[ $w = /* ]] && echo "$w" || echo "$build/$w" ;;
    esac
done)
echo "==> [link] ${#inputs[@]} objects and libraries from CMake"

export PS5_CLANG=${PS5_CLANG:-$(command -v clang-19 || command -v clang)}
source "$tri/tools/radv-link.sh"
radv_link_recipe "$stage" "$sdk_root" "$radv_archive"

# The system modules an app may import. libkernel_sys and libScePosixForWebKit
# are left out: a normal app cannot load them, and one import from either
# closes it before it starts (ps5/src/ps5_libc_compat.c covers their few
# functions the desktop code mentions).
sys_libs=()
for so in "$sdk_root"/target/lib/*.so; do
    case $(basename "$so") in
        libkernel_sys.so|libScePosixForWebKit.so) ;;
        *) sys_libs+=("$so") ;;
    esac
done

echo "==> [link] Def Jam + RADV into a PS5 PIE ELF"
"$sdk_root/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$tri/tooling/native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$stage/build/llvm-pie.elf" \
    "$tri/build/obj/app_crt.o" "$tri/build/obj/app_cpp_runtime.o" \
    --start-group "${inputs[@]}" --end-group \
    "$tri/build/stubs/libSceAgc.so" "$tri/build/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "${sys_libs[@]}"

echo "==> [package] eboot.bin for $title_id"
"$tool" link --in "$stage/build/llvm-pie.elf" --out "$stage/build/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$tri/build/stubs/libSceAgc.so" \
    --stub "$tri/build/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
app=$build/ps5-app/$title_id
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
# The code as data, for the MMIO trap (code is execute-only on the PS5).
python3 "$port/ps5/tools/make_code_bin.py" "$stage/build/llvm-pie.elf" "$app/code.bin"
"$tool" self --sign --in "$stage/build/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$tri/runtime/libc.prx" "$app/sce_module/libc.prx"
cat > "$app/sce_sys/param.json" <<EOF
{
  "ageLevel": { "default": 0 },
  "applicationCategoryType": 0,
  "applicationDrmType": "free",
  "attribute": 0,
  "attribute2": 0,
  "attribute3": 0,
  "conceptId": "${title_id:4}",
  "contentBadgeType": 1,
  "contentId": "UP9000-${title_id}_00-DEFJAMFFNYPS5000",
  "contentVersion": "01.000.000",
  "downloadDataSize": 256,
  "gameIntent": { "permittedIntents": [ { "intentType": "launchActivity" } ] },
  "localizedParameters": {
    "defaultLanguage": "en-US",
    "en-US": { "titleName": "Def Jam: Fight for NY" }
  },
  "masterVersion": "01.00",
  "pubtools": { "creationDate": "2026-10-16 00:00:00", "loudnessSnd0": "-28.00", "toolVersion": "2.00" },
  "requiredSystemSoftwareVersion": "0x0000000000000000",
  "sdkVersion": "0x0000000000000000",
  "titleId": "${title_id}",
  "versionFileUri": ""
}
EOF
# Home screen art (ps5/assets/sce_sys): icon0.png 512x512; pic0.dds /
# pic1.dds the 3840x2160 BC7 background and launch splash.
for asset in icon0.png pic0.dds pic1.dds snd0.at9; do
    [[ -f $port/ps5/assets/sce_sys/$asset ]] && cp "$port/ps5/assets/sce_sys/$asset" "$app/sce_sys/$asset"
done
# The in-game menu's font (Dear ImGui ships Roboto; Apache License 2.0).
imgui=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_IMGUI:PATH=//p' "$build/CMakeCache.txt")
if [[ -n $imgui && -f $imgui/misc/fonts/Roboto-Medium.ttf ]]; then
    mkdir -p "$app/fonts"
    cp "$imgui/misc/fonts/Roboto-Medium.ttf" "$app/fonts/"
    cat > "$app/fonts/README.txt" <<'TXT'
Roboto-Medium.ttf: Roboto by Christian Robertson, https://fonts.google.com/specimen/Roboto
Licensed under the Apache License, Version 2.0 (LICENSE-Apache-2.0.txt).
Used for the in-game menu.
TXT
    if [[ -f /usr/share/common-licenses/Apache-2.0 ]]; then
        cp /usr/share/common-licenses/Apache-2.0 "$app/fonts/LICENSE-Apache-2.0.txt"
    fi
else
    echo "warning: no Roboto font found; the in-game menu uses Dear ImGui's built-in one" >&2
fi
# The border beside 4:3 menus (display.widescreen_side_fill = border); replaceable.
# ps5/assets is not tracked; the default picture is made by ps5/tools/make_border.py.
if [[ ! -f $port/ps5/assets/borders/border.png ]]; then
    mkdir -p "$port/ps5/assets/borders"
    python3 "$port/ps5/tools/make_border.py" "$port/ps5/assets/borders/border.png" \
        || echo "warning: could not make the default border (needs numpy and Pillow)" >&2
fi
if [[ -f $port/ps5/assets/borders/border.png ]]; then
    mkdir -p "$app/borders"
    cp "$port/ps5/assets/borders/border.png" "$app/borders/"
    cat > "$app/borders/README.txt" <<'TXT'
border.png fills the black bars beside the game's 4:3 picture in menus, movies
and loading screens. It goes away on its own once a fight or cutscene starts.

To use your own: save a 3840x2160 PNG (or JPEG) here as border.png, replacing
this one. The game covers the middle 2880 pixels, so only the left 480 and
right 480 pixels show. Leave the middle empty or fill it, it doesn't matter.
Other sizes work too; the picture is stretched to the screen.

Turn it off in the in-game menu (L3+R3): Display, "Beside 4:3 pictures",
set to black.
TXT
fi
mkdir -p "$app/GameData"
cat > "$app/GameData/PUT GAME FILES HERE.txt" <<'TXT'
Copy everything from your extracted Def Jam: Fight for NY (USA) Xbox disc into
this folder: default.xbe, fighter.xml, gamedata.xml, gconfig.xml, venue.xml
and the assets, audio, fonts, motion, movies and screens folders.
TXT
"$tool" self --inspect --file "$app/eboot.bin" | tail -3
echo "==> $app ($(stat -c %s "$app/eboot.bin") bytes)"
