# Development and implementation notes

These notes retain the implementation history and original local validation results. Use the root README and WINDOWS.md for the current installation flow; earlier experiments and commands below may have been superseded.

# Rocket League → Skyrim bridge

This is the installed first-pass bridge for this machine. Rocket League owns controller input, vehicle and ball physics, and camera settings. Two local plugins transfer their poses to Skyrim, which renders the Fennec and ball inside its world.

**The real Fennec, ball and boost are visibly verified in live paired play.** Loaded Skyrim Havok geometry now runs inside RL's native Bullet collision world, including suspension ray tests. Live driving passed slopes, airtime and landings with four native wheel contacts, well beyond the old arena bounds and below its floor. A native physics height offset keeps RL's camera above the old floor while wire/Skyrim coordinates remain unchanged. Physical controller driving was confirmed by the user.

## Installed here

| Component | Installed version or result |
|---|---|
| Rocket League | Steam build 25535926, offline executable |
| BakkesMod | Official injector 2.0.76 and runtime 228; runtime initialization confirmed under Proton |
| Skyrim SE | 1.7.104.0 |
| SKSE64 | 2.3.1, matching runtime DLL and scripts |
| Address Library | All in One v13, `versionlib-1-7-104-0.bin` |
| Bridge plugins | Built as x64 MSVC-compatible DLLs and installed in both games |
| Fennec / ball | Real installed RL meshes exported, converted to SSE NIFs, installed with textures |
| Protocol and replay | C++/Python compatibility, live UDP capture/replay, interpolation, malformed packet checks pass |
| Isolated viewer | Two private X displays, authenticated local browser viewer, startup and cleanup checks pass |
| Runtime validation | Both plugins load; Fennec and ball visibly render during live paired play. Native Skyrim triangle collision, wheel contacts and controller driving verified during live play. |

The Fennec body is exported from `body_grain_SF.upk`, verified using the equipped car's live product metadata (4284, Fennec, `body_grain.body_grain`). The earlier Norton export was the wrong body and has been replaced. The current visuals use rigid OEM+ wheels and base materials. They do not reproduce the exact equipped wheels, animated steering/suspension, equipped paint/decal or other cosmetics. Boost uses an original procedural orange flame texture with twin additive exhaust quads, triggered by native boost activity; it does not reproduce the equipped boost cosmetic. The standard ball mesh and its diffuse/normal textures come from the installed game. Exported game assets stay in ignored local folders and are not part of the source distribution.

## Launch on a separate display

From this folder:

```bash
./launch-bridge.sh
```

To play in a direct Skyrim desktop window without browser video latency, run:

```bash
./launch-bridge.sh --retry-skyrim --manual-bridge --desktop-skyrim
```

Run this from your desktop terminal. RL stays on a private display; use the printed panel for its offline Free Play setup, then play in the normal Skyrim window. Load a scene and press F8; F10 brings the ball ahead. This mode preserves the caller's desktop display connection and avoids the Xvfb presentation override for Skyrim. Do not combine it with `--gamescope-skyrim`. Physical controller input into background RL still needs verification.

Open Steam normally and sign in first. The script refuses to start a private Steam client. It starts each game on its own authenticated Xvfb display, using Steam's existing per-game CachyOS Proton and its matching Steam Linux Runtime. It prints a `build/sessions/.../panel.html` path. Open that file in a browser when ready. Both games are reachable through tabs in the same panel. The script does not open or focus a desktop window automatically. It stops before launching Skyrim when Steam's latest recorded attempt failed with `FamilySharing`, avoiding another native Steam license dialog. After borrowing works, run `./launch-bridge.sh --retry-skyrim` to retry.

In the RL tab, complete the normal startup screen and enter **offline Free Play**. In the Skyrim tab, load a save in a flat open area. The launcher enables streaming and automatic bridge entry for that session. The controller remains attached to RL; Skyrim's gamepad setting is temporarily disabled. This routing is implemented but has not been tested with live controller input. The browser viewer adds display latency; it does not alter RL's bindings or physics. Both renderers are temporarily capped to 60 FPS; RL physics packets still run at 120 Hz. Use Champions Field: Beckwith Park Stormy produced a DX11 device reset in the isolated renderer even with terrain feedback disabled. The optional `--gamescope-skyrim` mode runs a nested GPU compositor inside its private display.

Both games now start on the account following the user’s family setup. The doctor separates old sharing refusals from the latest launch result. No account switching or sharing settings were changed by the installer.

Additional modes:

```bash
./launch-bridge.sh --manual-bridge   # enter Skyrim driving with F8
./launch-bridge.sh --no-skyrim       # RL setup/recording only
./launch-bridge.sh --demo            # Skyrim with synthetic test poses
./launch-bridge.sh --replay build/native-freeplay-20260930.rlsb --gamescope-skyrim
./launch-bridge.sh --vanilla --gamescope-skyrim # normal Skyrim first-run setup
./launch-bridge.sh --dry-run         # show launch commands without running them
./launch-bridge.sh --official-injector # alternative official BakkesMod GUI
./launch-bridge.sh --retry-skyrim    # retry once Steam borrowing is working
./launch-bridge.sh --viewer-only --seconds 5  # display/viewer check, no games
python3 tools/doctor.py             # installed dependencies and actual probe evidence
```

Stop the launcher with Ctrl+C. It terminates only processes belonging to its private displays, closes its viewer servers, and restores temporary settings if they have not subsequently changed. Original setting bytes and logs are saved under `build/sessions/`. Existing game sessions cause the launcher to refuse a second copy.

The official injector can misidentify the platform when enumerating the game's modules under Proton. The default `BridgeInjector.exe` loads only the installed official BakkesMod DLL into `RocketLeague.exe`, after verifying the target's actual command line contains `-NoEAC`. It rejects multiple game copies and EAC launcher parents. It records the result in the BakkesMod folder's `bridge-loader.log`; a successful library load alone does not prove the bridge plugin has started. The launcher also checks the known-compatible RL build IDs and BakkesMod 228 before using it. The official GUI remains available as an alternative and is configured to start minimized.

Both VNC and browser proxy servers bind to loopback, use a generated VNC password, and stop with the session. Ports are 5901/5902 and 6081/6082. The launcher preserves the RL control bindings, Steam launch options, save files, and the user's desktop session.

## Controls and configuration

| Action | Control |
|---|---|
| Enter/exit bridge in Skyrim | F8 |
| Re-anchor RL's current location to the Skyrim position | F9 |
| Reset the real RL ball ahead of the car | F10 |
| Start/stop RL stream from BakkesMod console | `sb_start` / `sb_stop` |
| Record RL packets | `sb_record session` |
| Stop recording | `sb_record stop` |

Skyrim configuration is `Data/SKSE/Plugins/SkyrimRocketBridge.ini`. The installer has set `CarModel=rocketbridge\fennec.nif`, `BallModel=rocketbridge\ball.nif`, and both model scales to 1. Native unit conversion is `1/1.43`; `AnchorCarClearance=17` aligns the car's resting center above the anchor point. F8/F9/F10 use DirectInput scan codes 66/67/68.

The listener interpolates by 30 ms and stops after 3000 ms without a valid stream. The plugin restores player collision, player visibility, position, camera mode, FOV and vanity settings on exit or disconnection. World changes, death and saves also stop the driving state. The transient model nodes are not saved game references. The original player's position is restored before saving.

`SuppressSurvivalPrompt=1` is installed at the user’s request. It marks the Survival startup question as already answered when the plugin loads or a save finishes loading. It does not enable Survival Mode. Set it to 0 to restore normal prompting on a fresh character.

`NpcBumps=1` enables moving-world contacts with native terrain enabled. The plugins exchange live Havok body poses, shapes, velocities, masses and materials. A persistent Bullet coupling world solves complete contact manifolds at each RL physics step, including friction and angular response. Only contact-induced velocity/position changes return to the real RL bodies; opposite contact-point impulses go to the real Havok bodies. Standing NPCs use capsule hulls with character-controller velocity feedback and switch to their actual bone bodies when ragdolling. Damage uses closing speed, including flips, with `NpcDamageScale=0.03`; the cooldown limits damage rather than physical contact. Skyrim retains damage attribution and essential/protected behavior.

Moving shapes use convex hull approximations. Compound concavity and Skyrim's joint constraints are not reproduced in the coupling world; Havok retains those constraints on its side. Carts, clutter, weapons and ragdolls are supported when they expose loaded, movable Havok bodies. This path is staged for a full restart of both games and has not yet been verified in live gameplay. The earlier swept NPC/recoil path remains only as a fallback when native terrain is disabled.

Xbox B / PlayStation Circle sends `sb_interact`; E works in Skyrim. Drive close and face an NPC, door, container or activator. Native activation opens normal dialogue/interaction menus. Driving freezes while a Skyrim menu is open and resumes afterwards. Skyrim gamepad menu input is enabled only while a UI is open; dialogue holds the camera toward the activated NPC. World transitions re-anchor after the new cell loads. These new interaction/impact paths require live validation beyond the unit-tested collision/momentum calculation.

## Replay and inspection

The Python tools require only the standard library:

```bash
python3 tools/bridge.py demo --seconds 15 --output build/demo.rlsb
python3 tools/bridge.py inspect build/demo.rlsb
python3 tools/bridge.py replay build/demo.rlsb --loop
python3 tools/bridge.py capture --seconds 15 --output build/live.rlsb
```

`sb_record session` writes `session.rlsb` in the BakkesMod data folder. Only basename characters are accepted. Reusing the basename replaces that recording. The Python capture/demo tools refuse to overwrite an existing output.

The Skyrim plugin and a sniffer cannot bind the same listener port. To inspect a stream while Skyrim runs, set the BakkesMod cvar `sb_state_port 29740`, then use:

```bash
python3 tools/bridge.py capture --port 29740 --forward-port 29741
```

State forwarding works; feedback events require the sender's configured port to match, so use direct streaming for ball reset/NPC feedback. Set `sb_state_port 29741` again afterwards.

`RLSBREC1` recordings store elapsed microseconds, packet length, and the exact packet. Replay creates a fresh sender session and timestamps on each loop so the plugin can reject delayed packets from an earlier run.

## Build

Native core tests need CMake, Ninja, a C++20 compiler and Python:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

The game plugins require the **x64 MSVC ABI**. MinGW cannot link these SDKs correctly. The Linux build uses native `clang-cl` and `lld-link` with Microsoft's headers/libraries; it does not run a Windows compiler or open Wine windows. Fetch the pinned public dependencies first:

```bash
python3 tools/fetch_deps.py
python3 tools/build_linux.py --msvc-root /path/to/msvc-install --sdk-root /path/to/windows-sdk-install
```

The MSVC installation contains `VC/Tools/MSVC/<version>/include` and `lib/x64`. The SDK installation contains `Windows Kits/10/Include/<version>` and `Lib/<version>`. This machine was built with MSVC 14.51.36231 and Windows SDK 10.0.26100.0. The CommonLib v7.1.0 prebuilt bundle requires its modern MSVC runtime; matching app-local CRT DLLs are installed without changing Wine's system32.

`build-win/bakkes-plugin/RocketSkyrim.dll` and `build-win/skse-plugin/SkyrimRocketBridge.dll` are the outputs. The case-insensitive BakkesMod header overlay is generated for Clang on Linux. SDK/library sources and the verified CommonLib prebuilt are pinned by `tools/fetch_deps.py`.

On Windows, use a recent Visual Studio x64 toolchain. Either provide the verified CommonLib bundle plus a matching installed spdlog/fmt CMake prefix, or build CommonLib from source with its dependencies. For the prebuilt route:

```powershell
python tools/fetch_deps.py
cmake -S . -B build-win -A x64 -DBRIDGE_BUILD_PLUGINS=ON -DBUILD_TESTING=OFF -DBRIDGE_COMMONLIB_PREBUILT=".deps/commonlib-prebuilt/commonlibsse-ng-prebuilt-v7.1.0-all-msvc-cmake" -DCMAKE_PREFIX_PATH="C:/path/to/spdlog-fmt-prefix"
cmake --build build-win --config Release
```

Only the Linux cross-build has been run here.

## Install / restore

`tools/install.py` verifies the installed game versions, the downloaded Address Library's runtime header and the official BakkesMod runtime before writing files. It keeps existing BakkesMod config/bind files and adds the bridge's plugin load command. It expects matching SKSE to be installed; this machine already has the matching loader, runtime DLL and scripts.

```bash
python3 tools/install.py --address-library /path/to/Address-Library-v13.zip \
  --crt /path/to/VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT
```

The official BakkesMod runtime input is `.deps/bakkesmod-runtime/BakkesMod.exe` and its extracted `files/` folder. The supported runtime is 228. Official sources: [injector 2.0.76 release](https://github.com/bakkesmodorg/BakkesModInjectorCpp/releases/tag/2.0.76), [SKSE](https://skse.silverlock.org/), [Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files). A newer game build needs a corresponding verified runtime update.

Every installer write records its previous contents and the installed checksum in `build/install/<timestamp>/manifest.json`. To inspect or restore a particular batch:

```bash
python3 tools/restore.py build/install/<timestamp>/manifest.json --dry-run
python3 tools/restore.py build/install/<timestamp>/manifest.json
```

Stop both games first. Restore batches from newest to oldest to undo the entire installation. Files edited after the installer wrote them are preserved. The same restore command accepts a session's `temporary-files.json` after an interrupted launch. Keep the backup folders until you no longer need recovery.

## Private asset pipeline

`tools/build_assets.py` builds the Nifly converter using pinned sources. It can also build the native UModel exporter. A Python virtual environment with `cryptography` is needed for package decoding:

```bash
python3 tools/build_assets.py --exporter
python3 -m venv .deps/asset-env
.deps/asset-env/bin/pip install cryptography
.deps/asset-env/bin/python tools/extract_assets.py body_grain_SF.upk
.deps/asset-env/bin/python tools/extract_assets.py wheel_oemplus_SF.upk
.deps/asset-env/bin/python tools/extract_assets.py GameInfo_Soccar_SF.upk Ball_DefaultBall00
```

The extractor works on private copies and validates the updated licensee-34 chunk layout before exporting. It leaves installed `.upk` files and texture caches untouched. UModel's DDS exports also need the ball's `Ball_Default00_D` and `Ball_Default00_N` texture objects exported; mesh-only export may omit them. Export those objects individually from the same package when necessary.

```bash
.deps/asset-env/bin/python tools/extract_assets.py GameInfo_Soccar_SF.upk Ball_Default00_D
.deps/asset-env/bin/python tools/extract_assets.py GameInfo_Soccar_SF.upk Ball_Default00_N
python3 tools/install_assets.py
```

PSK/PSKX geometry becomes material-separated JSON, SSE `BSTriShape` NIFs and private `.rmesh` triangle buffers. The direct D3D11 renderer follows SkyCraft’s scene-target integration because the NIF body/ball surfaces were invisible in the tested runtime. It uses actual geometry, linear normal maps, material-specific roughness, GGX sun highlights, Fresnel environment lighting and up to eight nearby Skyrim lights. It retains HDR rendering, fog and depth occlusion. Original RL shaders and true environment cubemaps are not reproduced. Native outdoor directional shadow cascades now receive the actual car and ball meshes. The converter reloads each NIF to validate the shape count. Fennec wheel hubs come from its actual bind skeleton. All source/exported assets remain under ignored `.deps/` and `build/`; only this user's game installation receives them.

## Native terrain collision

`NativeTerrain=1` exports actual triangles from the loaded Skyrim Havok world to `TerrainMeshPath`. The installer sets its cross-prefix path automatically. The exporter follows the car as Skyrim streams cells, covering a 5000-unit neighborhood. Native landscape and compressed/extended triangle meshes are preserved; primitive boxes and rounded shapes are tessellated. Only fixed Havok bodies enter the static BVH. Moving bodies and character controllers use the live contact stream beside it.

The offline RL backend validates the native physics ABI, replaces one stock arena BVH mesh, and disables the other arena bodies' contact/filter participation. RL's own collision response, wheel suspension, grip, grounded state, car/ball contacts and controller inputs remain responsible for driving. Mesh updates run before the native physics step and rebuild using RL's allocator. Cached contact algorithms are cleared before triangle indices change; original arena geometry is restored on exit.

The current build lifts native physics by 100,000 RL units and subtracts that height from transmitted car, ball and camera positions. The original 10,000-unit buffer still allowed long Skyrim descents to reach RL's low-height respawn zone. Terrain and moving-body proxies use the same larger offset. Skyrim's visible calibration is unchanged. Offline arena respawns return to the latest grounded terrain position. Regression tests exercise contacts 75,000 RL units below the calibration point, but long exploration still needs live validation; this is a larger finite buffer, not an unlimited floating origin.

Live validation survived repeated growing/shrinking mesh updates, driving slopes and landing with `grounded=1` and four native wheel contacts. The car reached X greater than 110,000 and Z below -1900 in bridge coordinates without the original arena wall/floor blocking it. Geometry covers loaded cells rather than the entire world at once; very fast travel can outrun cell loading. Update cost, unsupported shapes, large-coordinate precision and native ABI changes remain practical limits.

`TerrainCollisions=0` and `sb_wheel_contacts=0` keep the older discrete-plane/contact-injection experiments disabled. `sb_noclip=1` is a fallback before native terrain is ready. `MaxRange=0` removes the calibration distance cutoff. All runtime modification is restricted to offline play.

Open Cities 3.2.4 and USSEP 4.3.9c are installed locally, with verified masters and Open Cities after USSEP in Plugins.txt. Their archives and game assets are excluded from the source distribution.

## Protocol and source layout

* `shared/`: packed version-1 state/event protocol, checked decode, unit/axis/quaternion transform, timeline and loopback UDP.
* `bakkes-plugin/`: local-car physics-tick exporter, real ball/camera state, offline-only event feedback and recording.
* `skse-plugin/`: main-thread receiver, temporary visual nodes, player/camera hooks, restore/toggle handling and optional NPC knockback.
* `tools/`: sniffer/replay/demo, build/install/export, isolated launcher/viewer and diagnosis/recovery.
* `tests/`: cross-language wire data, malformed recordings, actual UDP replay, transforms/interpolation, settings recovery and optional viewer lifecycle checks.

State packets are 176 bytes, event packets 80 bytes. Both carry a 32-byte header with magic, version, kind, length, sequence, sender session and microsecond timestamp. State carries body/camera validity flags, car and ball rigid-body poses/velocities, camera pose/FOV and boost. Receiver freshness uses local arrival time rather than assuming equal clocks between Wine prefixes. Sequence/session handling rejects stale or reordered data. Teleports snap instead of interpolating through walls.

The user-provided [universal-modder](https://github.com/rehan-remade/universal-modder) was reviewed for its mashup workflow and collision requirements; it does not supply a ready Rocket League UE3 terrain backend. The approach follows [SkyCraft's architecture](https://github.com/chasmlol/SkyCraft): both engines keep their own responsibilities. This bridge now also transfers loaded Skyrim collision into RL’s native physics world. [Psyonix's offline mod support](https://www.rocketleague.com/news/easy-anti-cheat-comes-to-rocket-league-on-pc-today/) permits community mods with EAC disabled. This bridge uses the non-EAC executable, refuses online match state and does not modify anti-cheat installations.

Project source is GPL-3.0-or-later; see `LICENSE` and `THIRD_PARTY.md`. Game assets and proprietary runtimes retain their original licenses.

The October 1 feedback revision reduces diffuse lighting energy, removes doubled local-light brightness, and uses Havok rays to shade car/ball sunlight and overhead ambient light. These per-object shelter samples approximate shadows; they are not native per-pixel shadow-map rendering. Revised lighting, menu controls, contact recoil and damage require a restart and live gameplay validation.

## Skyrim League launcher

Run `./bridge-ui.sh` to open the local control room. Choose each game independently: enabled or off, private or desktop display, connected monitor, and desktop workspace. Choices are remembered in the browser. The UI shows running/recent bridge sessions, provides session-specific stop controls and private game views, and edits damage, smoothing, model size and camera/Survival settings. Save Skyrim before stopping a session. Settings are backed up under `build/ui-settings-backups` and apply on the next launch. The server binds only to loopback and uses a private per-run access link. Closing the browser leaves the games running. Stop the UI server with Ctrl+C in its terminal.

The local Rofi application entry is **Skyrim League**. Press Super+R and search for that name.

The next lighting/input revision adds `RenderBrightness=0.2` (UI range 0.02–3), sampled every 200 ms by the renderer, and receives the existing Skyrim screen-space sunlight mask when its dimensions match. This is an approximation based on the underlying world depth, and does not make the streamed meshes cast shadows. Both Proton processes request physical SDL controller input (`PROTON_USE_SDL=1`) with background joystick events enabled (`SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`). These environment changes require restarting both games and have not yet been verified with the physical controller while Skyrim is focused.

`CastShadows=1` appends the real Fennec and ball meshes to Skyrim’s native directional shadow maps after the engine renders its sun cascades. Existing world depth is retained, all touched D3D state is restored, and the original world shaders shade the receiving surfaces. The pass omits boost, skips hidden/demolished meshes, and supports standard/reversed shadow depth. Point-light/interior shadow casting is not included. The projection math is unit tested; live visual validation is recorded separately.

The October 1 controller correction polls physical XInput only while the bridge is active and Skyrim has focus, preserving gameplay bindings and sensitivity settings. B / Circle adds interaction without replacing the RL or BakkesMod binding. Open Skyrim menus pause driving and retain their normal controller input. D-pad freeplay actions use the configured RL bindings. The Fennec body uses its extracted paint mask with the material default orange coat; equipped paint and decals are not yet streamed. Both meshes retain their original diffuse/normal maps. RenderBrightness is now 1.0 after the daylight screenshot confirmed 0.2 was still too dark. The earlier 0.04 experiment was reverted. Material appearance remains visually unverified.

The daylight texture investigation verified the original diffuse maps and material roles. The ball RGB texture is a mask, not a colour map. The next Skyrim build removes screen-space ground-depth shadow-mask sampling from the direct meshes (which can shade them with their own ground shadows) and the extra diffuse/albedo dimming. Native mesh shadow casting remains enabled. Material map names are logged at renderer initialization. This correction requires restarting Skyrim and has not been visually verified.

The material contrast revision corrects reversed PSK winding: the previously exported ball had all 2522 normals pointing inward. It also fixes tangent handedness and bakes short-range occlusion into direct-mesh vertex alpha using `tools/bake_mesh_ao.cpp`. Occlusion only affects ambient illumination/reflection, never direct sunlight. Paint, rubber, glass and ball use distinct shading parameters, with less generic environment reflection. These geometry and renderer changes activate after restarting Skyrim; appearance is not yet verified in game.


The moving-body contact revision adds `.rldyn` snapshots and acknowledged `.rlcontacts` batches beside `TerrainMeshPath`. Body tokens are resolved locally under the exact Havok world; response files never provide native pointers. Snapshot expiry, session validation, finite-value bounds, native car/ball mass guards and menu pause gating prevent stale contacts from carrying into another session. Logs report `Dynamic collision car_abi=... ball_abi=... proxies=...` in RL and `Dynamic collision snapshot` in Skyrim. A car/ball ABI value of 0 means that native body was not validated and did not receive coupling corrections.

Run the physics regressions with `cmake -S . -B build-contact-tests -DBRIDGE_TEST_DYNAMIC_CONTACTS=ON`, build `bridge_contact_tests`, then run it. They cover momentum conservation, off-center car torque, gentle pushing, heavier-body resistance, anchored blocking, moving kinematic contacts and retired-session rejection. These tests validate the coupling solver, not the in-game Havok/native-ABI integration.


The ball-reset crash fix removes inconsistent Bullet vtables from the Windows build. `tools/build_linux.py` now creates one private RL-compatible Bullet source tree and compiles every collision/dynamics/test translation unit against it. The Windows coupling regression executable is also run under headless Wine; Linux tests alone do not exercise that ABI. Driver proxies own copies of the native box/sphere/compound shapes, rather than retaining engine-owned car or ball shapes through a reset. Body replacement and large teleports recreate the proxy and remove its cached manifolds. The reset test requires successful contact before each of 100 reset/teleport/body-destruction cycles. The same change initializes zero-mass inertia explicitly; Bullet vector default constructors do not zero their components. This revision is staged for the next RL launch, without reloading live game processes.


The stream-timing revision receives UDP packets on a dedicated thread and records arrival times independently of Skyrim updates. The game thread drains an ordered, bounded queue; no Skyrim APIs run on the receiver thread. A minimum-delay clock estimate with gradual drift correction keeps interpolation independent of packet-arrival jitter. Buffer underruns predict visual car/ball positions and body rotations for at most 20 ms; camera translation follows exactly the same car delta. Authoritative collision feedback still reads the latest received state. Resets/teleports bypass prediction, including ball-only resets. Normal logging reports receive gaps, queue drops and buffer underruns once per second; verbose geometry diagnostics are debug-only. `bridge_timing_tests` covers delayed packets, playback continuity, bounded prediction, camera coherence, reset handling, stale streams and background loopback reception while the consumer stalls. Linux and Windows MSVC-ABI tests under headless Wine pass. The update is staged for the next Skyrim restart; live jitter reduction is not yet verified.


## Windows

Native Windows setup, the browser launcher, Steam library discovery, session ownership and controller focus routing are implemented. See [WINDOWS.md](WINDOWS.md) for prerequisites, private visual-asset transfer, setup commands and native Visual Studio builds. `tools/package_windows.py` creates `dist/Skyrim-League-Windows.zip` with built plugins and sources. Windows executables and Python tests pass under headless Wine; real Windows gameplay validation is pending. The Linux running games are unchanged.
