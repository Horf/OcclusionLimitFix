# Occlusion Limit Fix

An SKSE plugin that fixes the crash Skyrim runs into when too many occlusion planes and boxes are loaded at once, plus two optional performance settings for areas with very many occluders.

**AI disclaimer:** This plugin was developed with AI assistance (Claude by Anthropic). The AI helped analyze the game code, write the C++ code and draft the documentation. The reverse engineering in Ghidra, all decisions and all in-game testing were done by me.

## The problem

Occlusion mods (for example eFPS or the occluders shipped with large city mods) add occlusion planes and boxes so the engine can skip drawing objects hidden behind them. Skyrim can only handle a limited number of them at the same time. Above that, the game crashes:

- first when **opening the map**, with even more occluders already **when a cell loads**,
- often **without a crash log**, the Windows event log shows `0xc0000005` with an unknown module and an offset like `0x0000003d0000003d`,
- crash log analyzers point to `BSGeometryListCullingProcess` ("too many occluders").

### Cause

Each occlusion plane adds one set of frustum planes to the engine's `BSCompoundFrustum`, each occlusion box adds two. While culling, four engine functions save the active plane state of all these sets with `BSCompoundFrustum::GetActivePlaneState` into a **fixed stack buffer of 256 entries** (one for the camera, 255 for occluders) and restore it afterwards with `SetActivePlaneState`. Neither function checks the buffer size. With more than 255 plane sets they write past the buffer, overwrite saved registers and the return address, and the game jumps to an address built from plane bitmasks (`0x3D`, `0x3F`).

## What the plugin does

### Crash fix (always on)

Replaces `GetActivePlaneState` and `SetActivePlaneState`. The first 255 plane sets still go into the engine's buffer, everything above is kept in a separate per-thread storage and restored from there. There is no limit anymore, and every occluder keeps working as before.

### Experimental options (off by default)

Only worth enabling if you get noticeably lower FPS in areas with very many occluders. Both only take effect while the crash fix is active.

| Setting | What it does |
| --- | --- |
| `bSkipOcclusionOnMap` | Skips all occluder tests while the map (world and local) is open. Seen from above, occluders hide almost nothing, but the engine still tests every object against every occluder. |
| `iMaxActiveOccluders` | Keeps at most this many plane sets active per frame (planes count 1, boxes count 2). The occluders that appear largest on screen are kept. The engine itself has no size test, so every occluder in view would otherwise be active. |

## Performance

The crash fix itself costs practically nothing (well below 1% of frame time in profiling). The cost of many occluders comes from the engine: every active occluder is tested against every visible object, on the main thread.

Results from stress tests on 1.5.97 with a test plugin that spreads occlusion planes over 5 × 5 cells. All numbers are rough FPS losses compared to the same spot without the test occluders. **They come from a single test system and can differ a lot on other hardware and load orders.** A slower CPU will see bigger losses earlier, a faster one smaller losses.

| Situation | Without plugin | Crash fix | + `bSkipOcclusionOnMap` | + `iMaxActiveOccluders = 255` |
| --- | --- | --- | --- | --- |
| 300 planes | crash | no measurable loss | | |
| 2000 planes, world map | crash | about 55% fewer FPS | no measurable loss | |
| 2000 planes, looking at the area | crash | about 25 to 50% fewer FPS | | about 10% or less |
| 2000 planes + 1000 boxes, looking at the area | crash | about 20% fewer FPS | | about 15% |

The cost grows with the number of occluders active at the same time:

| Active plane sets | Effect on the test system |
| --- | --- |
| up to about 250 | no measurable loss (vanilla range) |
| about 500 | about 25% fewer FPS |
| more than 1000 | about 50% fewer FPS |

With either option, no difference in occlusion was visible in these tests, and draw calls stayed about the same. The tests cannot cover every area and every mod, which is why both options are experimental and off by default.

### Real-world numbers

Measured with a load order of 516 plugins that includes an eFPS-style occlusion mod, with and without an additional dense occlusion patch. "Loaded" is the number of plane sets in the 5 × 5 loaded cells, "active" is the highest number the plugin logged.

| Area | Loaded | Active in game | Active on the map |
| --- | --- | --- | --- |
| Tamriel (-9,-20) | 190 | 115 | 205 |
| Whiterun | 174 | 101 | 172 |
| Windhelm | 130 | 76 | 130 |
| Tamriel (-8,-20), dense patch | 214 | 104 | 217 |
| Blackreach, dense patch | 396 | 169 | 400 |
| Soul Cairn, dense patch | 332 | 182 | 234 (339 when flying over the area) |

In game, roughly half of the loaded occluders are active at once. Seen from above, on the map or from a high camera, almost all of them are. That is why the crash usually shows up when opening the map. No FPS loss was noticeable in any of these areas, in game or on the map.

## Requirements

- [SKSE64](https://www.nexusmods.com/skyrimspecialedition/mods/30379)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

## Supported versions

| Version | Crash fix | Map option | Occluder budget |
| --- | --- | --- | --- |
| 1.5.97 | tested | tested | tested |
| 1.6.640 | verified in disassembly | verified in disassembly | verified in disassembly |
| 1.6.1170 | tested by users | verified in disassembly | verified in disassembly |
| 1.7.104 | verified in disassembly | verified in disassembly | verified in disassembly |
| VR | not supported | not supported | not supported |

Other 1.6.x and 1.7.x builds may work. Before patching, the plugin checks every patched location. If the code does not look as expected (unknown game version, or another mod already patched it), that part stays disabled and the log says why. The game then behaves as without the plugin.

Feedback from AE users is welcome.

## Installation

Install with a mod manager, or copy into `Data`:

```
Data/SKSE/Plugins/OcclusionLimitFix.dll
Data/SKSE/Plugins/OcclusionLimitFix.ini
```

## Settings

`Data/SKSE/Plugins/OcclusionLimitFix.ini`:

```ini
[General]
; Log detail: error, warn, info, debug
sLogLevel = info

[Experimental]
; Only change these if FPS drop noticeably in areas with very many occluders.

; Skip occluder tests while the map (world and local) is open.
; Fixes low FPS on the map with many occlusion planes.
bSkipOcclusionOnMap = false

; Maximum number of occluders active per frame, the largest on screen are kept.
; Planes count 1, boxes count 2. Fewer active occluders cost less CPU. 0 = no limit, 255 is a good start.
; Fixes low FPS when looking at areas with very many occluders.
iMaxActiveOccluders = 0
```

The log is written to `Documents/My Games/Skyrim Special Edition/SKSE/OcclusionLimitFix.log`. With `sLogLevel = debug` it also records the highest number of active plane sets seen, which is useful for bug reports.

## Technical details

All addresses are resolved through the Address Library. Hooks are 5-byte branches or calls through the SKSE trampoline.

| Part | SE ID | AE ID | Hook |
| --- | --- | --- | --- |
| `BSCompoundFrustum::GetActivePlaneState` | 75075 | 76843 | replaced |
| `BSCompoundFrustum::SetActivePlaneState` | 75076 | 76844 | replaced |
| Occluder test per object (loop 1) | call in 99738 + 0xBF | call in 106382 + 0xBF | map option |
| Occluders against bound (loop 2) | call in 31398 + 0x15D | call in 32189 + 0x15D | map option |
| Wrapper of loop 2 | calls in 31398 + 0x1E9, 39262 + 0x10A | calls in 32189 + 0x20B, 40335 + 0x10A | map option |
| Per-frame occlusion plane states | call in 35571 + 0x7C | calls in 36574 + 0x7C, 36564 + 0x960 (1.7: 0x972) | occluder budget |

`visibilityState` of `BSOcclusionShape`: 0 untested, 1 active, 2 skipped, 3 skipped by bound test. The engine sets the state of occlusion planes once per frame (SE 99736 / AE 106380) from a view frustum test and a facing test. Boxes are tested later when their state is 0. The occluder budget runs right after that per-frame pass, ranks planes and boxes by area divided by squared distance and sets everything beyond the budget to 2. Boxes it demoted are reset to 0 in the next frame, so the engine's own states are never changed permanently.

## Building

Requirements:

- [XMake](https://xmake.io) 3.0.0 or newer
- C++23 compiler (MSVC or Clang-CL)

```
git clone --recurse-submodules https://github.com/Horf/OcclusionLimitFix
cd OcclusionLimitFix
xmake build
```

To copy the build output straight into a mod manager or the game, set `XSE_TES5_MODS_PATH` or `XSE_TES5_GAME_PATH`. For Visual Studio: `xmake project -k vsxmake`.

## Credits

- [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) by meh321
- [BethesdaGhidraScripts](https://github.com/1001Bits/BethesdaGhidraScripts) for the Ghidra setup used for reverse engineering

## License

[GPL-3.0](LICENSE)
