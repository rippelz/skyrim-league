# Dependencies and references

Project code is GPL-3.0-or-later. Dependencies are fetched or built into ignored local folders; their licenses remain applicable. Exported Rocket League assets, Skyrim files, Microsoft runtime binaries, SKSE and BakkesMod releases are not relicensed as project source and are not included in a source-only package.

| Dependency | Role | Source / license |
|---|---|---|
| CommonLibSSE-NG 7.1.0 | Skyrim engine bindings | https://github.com/alandtse/CommonLibSSE-NG — GPL-3.0-or-later |
| BakkesMod SDK | RL engine bindings | https://github.com/bakkesmodorg/BakkesModSDK — upstream SDK terms |
| spdlog / fmt | Plugin logging | https://github.com/gabime/spdlog / https://github.com/fmtlib/fmt — MIT |
| rapidcsv | CommonLib dependency | https://github.com/d99kris/rapidcsv — BSD-3-Clause |
| Nifly | SSE NIF creation | https://github.com/ousnius/nifly — GPL-3.0-or-later |
| nlohmann JSON 3.12.0 | Local asset scene parsing | https://github.com/nlohmann/json — MIT |
| UModel / UEViewer | Private mesh/texture export | https://github.com/gildor2/UEViewer — MIT |
| RLUPKTools | Private package decoding helper | https://github.com/CrunchyRL/RLUPKTools — retain upstream terms; not redistributed here |
| noVNC 1.7.0 | Local browser display | https://github.com/novnc/noVNC — MPL-2.0 and dependency notices |
| websockify 0.13.0 | Local display websocket proxy | https://github.com/novnc/websockify — LGPL-3.0 |
| x11vnc / LibVNCServer | Isolated display server | https://github.com/LibVNC/x11vnc / https://github.com/LibVNC/libvncserver — GPL-2.0-or-later |

The direct scene renderer’s target integration and pipeline-state preservation are adapted from SkyCraft’s `WorldRender.cpp`. The camera update call-site hook and leaf-geometry player hiding are adapted from [SkyCraft’s Game.cpp](https://github.com/chasmlol/SkyCraft/blob/master/skse/src/Game.cpp), Copyright (c) 2026 chasmlol, under the MIT license. Its license notice is preserved in `licenses/SkyCraft-MIT.txt`. SkyCraft also informed the engine integration architecture. SDK/public-source commits and the CommonLib bundle checksum are recorded by the dependency fetcher. The asset builder separately pins UModel, RLUPKTools and Nifly, and verifies the JSON header checksum. Viewer packages are verified against Pacman's repository checksums and Arch's package-signing keyring, then extracted locally without installing system packages.

The passthrough collision-query workflow follows the user-supplied [universal-modder mashup skill](https://github.com/rehan-remade/universal-modder/blob/main/skills/mashup-mods/SKILL.md). Its source was inspected locally; no game-specific collision implementation was copied.

The native terrain backend uses Bullet 2.82 (zlib license) and MinHook (BSD-2-Clause); notices are preserved in `licenses/`. Native world discovery and layout were informed by ZealanL’s RLArenaCollisionDumper (MIT, Copyright 2023 ZealanL), with its notice preserved. The Skyrim shape traversal is adapted from SkyCraft’s `Collision.cpp` (MIT notice above). The cross-build creates explicitly altered Bullet header overlays: SIMD broadcasts use lane zero for clang-cl, and `btCollisionShape` adds RL’s `getAabbSlow` virtual slot to match the installed game’s fork. Upstream sources remain untouched.
