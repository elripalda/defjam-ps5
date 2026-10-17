# Def Jam: Fight for NY REMASTERED for PS5

**Def Jam: Fight for NY (Xbox, 2004) running natively on a jailbroken PS5**, at 60 fps with up to 4x resolution, widescreen fights, PlayStation buttons and optional HD textures.

> ### Built on DefJamFFNY-recomp by Vlad Yanhursky
> This port exists because of **[DefJamFFNY-recomp](https://github.com/vyanhursky/DefJamFFNY-recomp)**, Vlad Yanhursky's native recompilation of Def Jam: Fight for NY for Windows, macOS and Linux. The recompiled game and its fixes, the Vulkan renderer, widescreen, the HD texture system and the settings all come from his work, and **[xboxrecomp](https://github.com/sp00nznet/xboxrecomp)** by sp00nz is the toolkit underneath it. This release takes that work to the PS5.
>
> All the credit and all the love goes to him. Go star the original project and support it.

## Get it on Recomp Store
The easiest way to play is **[Recomp Store](https://recompstore.com)**. It installs the app, helps you set up your own game files and keeps it up to date.

## What's in it
- The original Xbox game recompiled to run natively on the PS5, at 60 fps
- Up to 4x the original resolution, sent to the TV in 4K
- 16:9 widescreen fights, with menus and movies kept in 4:3 between a choice of borders
- PlayStation button prompts throughout the game
- Optional HD texture packs for the fighters, venues, menus and HUD
- Sharper menu text
- Frame generation (beta): 120 fps on a 120 Hz TV
- Up to four players on DualSense controllers, with button remapping
- An in-game settings menu, and saves that carry over when you update

## What you need
- A jailbroken PS5 that runs homebrew apps from `/data/homebrew/`
- Your **own** copy of Def Jam: Fight for NY for the **USA Xbox**. PS2 and GameCube versions won't work. No game files are included.

## Install by hand
1. Download `PPSA92046.zip` from [Releases](../../releases) and copy the `PPSA92046` folder to `/data/homebrew/` on your PS5.
2. Copy everything from your extracted disc into its `GameData` folder:

```
/data/homebrew/PPSA92046/GameData/default.xbe
/data/homebrew/PPSA92046/GameData/*.xml
/data/homebrew/PPSA92046/GameData/assets/ audio/ fonts/ motion/ movies/ screens/
```

3. Launch it from your homebrew launcher.

To update, copy the new `PPSA92046` folder over the old one. Your saves, settings and `GameData` stay where they are.

## HD textures (optional)
Two packs, each a folder that goes in `/data/homebrew/PPSA92046/mods/`:

| Pack | What it changes | Download |
|---|---|---|
| Faithful HD | Fighters, venues and crowds, upscaled 4x from the originals | *Coming soon* |
| HD Menus | The menus and HUD, with upscaling | *Coming soon* |

Turn them on under **Video › HD textures** and **Video › HD menus**. Both switch instantly, no restart needed.

## The in-game menu
Hold **L3 + R3** (press both sticks in) at any time to open it.

| Button | Does |
|---|---|
| L1 / R1 | Switch pages: Video, Controls, Buttons, Game |
| Up / Down | Move between settings |
| Left / Right | Change a setting |
| Cross | Select |
| Circle | Close the menu |
| Triangle | Restart the game, when a change needs it |

The bottom of the menu explains whichever setting is highlighted. Most settings apply right away. The few that need a restart are listed at the bottom, and Triangle restarts the game for them.

## Frame generation (beta)
Frame generation adds a generated frame between every two real ones, so the game shows 120 fps on a TV that runs at 120 Hz. The TV switches to 120 Hz when you turn it on and back to 60 Hz when you turn it off. It adds a little input lag, so it's off by default.

## Building from source
The full source is in this repository. Build instructions are in [ps5/README.md](ps5/README.md). You need your own copy of the game to build it, and nothing from the game is stored here.

## Credits
- **[Vlad Yanhursky](https://github.com/vyanhursky)**: [DefJamFFNY-recomp](https://github.com/vyanhursky/DefJamFFNY-recomp), the recompilation and everything it does, which this port is built on
- **[sp00nz](https://github.com/sp00nznet)**: [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), the Xbox static recompilation toolkit, with hardware work from the [xemu](https://github.com/xemu-project/xemu) project
- **[elripalda](https://github.com/elripalda)**: the PS5 port, the in-game menu, the PlayStation buttons and the art
- **[mihawk-99](https://github.com/mihawk-99)**: [MihawkGen](https://github.com/mihawk-99/PS5_MihawkGen), the frame generation, and [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa), the PS5 Vulkan driver
- **[Nazky](https://github.com/Nazky)**: [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle), the PS5 Vulkan app setup this builds on
- **[ps5-payload-dev](https://github.com/ps5-payload-dev)**: the PS5 payload SDK
- [SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), [glslang](https://github.com/KhronosGroup/glslang), [stb](https://github.com/nothings/stb), and the [Bebas Neue](https://fonts.google.com/specimen/Bebas+Neue) and [Oswald](https://fonts.google.com/specimen/Oswald) fonts
- Thanks to everyone who played the test builds

## License
The code from DefJamFFNY-recomp and this port is under the [MIT License](LICENSE). xboxrecomp keeps its own licenses (see [tools/xboxrecomp](tools/xboxrecomp)). MihawkGen is GPL-3.0, so the PS5 app, which includes it, is shared under GPL-3.0 with this source. None of these licenses cover the game or its artwork.

*An unofficial fan project, not affiliated with or endorsed by Electronic Arts, AKI, Def Jam Recordings, Microsoft or Sony. No game files are included.*
