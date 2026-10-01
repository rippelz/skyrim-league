# Skyrim League on Windows

Native Windows support is experimental. The DLLs use the Windows x64 MSVC ABI; the launcher, installer and controller focus routing also have native Windows paths. Python/process/path tests and Windows physics/timing executables pass under headless Wine. Real Windows graphics, game startup, injection and physical controller behavior still need gameplay validation.

## Quick setup

1. Extract `Skyrim-League-Windows.zip` into a writable folder, such as `C:\Games\Skyrim League`. Install 64-bit Python 3.11 or newer with the `py` launcher, and Microsoft's [latest x64 Visual C++ Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).
2. Install Steam Skyrim Special Edition **1.7.104.0**, matching **SKSE 2.3.1** from [the official SKSE site](https://skse.silverlock.org/), and **Address Library v13 for 1.7.104.0**. Install SKSE's DLL/EXE files beside SkyrimSE.exe and its Data/Scripts files in the game's Data/Scripts folder. The launcher uses skse64_loader.exe, following [SKSE's instructions](https://github.com/ianpatt/skse64/blob/master/skse64_readme.txt).
3. Install the Steam edition of Rocket League and the compatible official [BakkesMod injector](https://bakkesmod.com/download.php). This bridge currently accepts RL Steam builds **25400034 / 25535926** with BakkesMod runtime **228**. Newer versions need bridge compatibility work; setup and launch check versions rather than loading unknown offsets. Epic/GOG game launch paths are not implemented in this launcher.
4. Transfer your private extracted visuals as described below. Close both games, then open a terminal in this folder and run:

```bat
setup-windows.cmd --assets "D:\Skyrim-League-Assets" --address-library "C:\Users\You\Downloads\AddressLibrary.zip" --injector "C:\Program Files\BakkesMod\BakkesMod.exe"
```

Steam and its separate game libraries are discovered automatically. BakkesMod data defaults to `%APPDATA%\bakkesmod\bakkesmod`. Override discovery with `--steam`, `--skyrim`, `--rl`, or `--bakkesmod`. The `--injector` path is optional if you start the official injector yourself. `--address-library` is optional when the matching library is already installed. Use `--dry-run` to check setup without copying anything.

5. Keep Steam signed in, start the official injector (unless its path was saved during setup), and double-click **bridge-ui.cmd**. Enable each game independently and choose Desktop or Minimized, plus a connected monitor. Enter offline Free Play in RL, load a Skyrim save, and press **F8** to anchor/activate. **B / Circle** adds interaction while preserving the RL binding; F9 re-anchors and F10 resets the ball. Controller menu routing stays gated by Skyrim menus.

The controller must be visible through XInput. Xbox-compatible pads work through that API; PlayStation pads need an XInput mapping that both games can see. Physical controller validation on Windows is still pending.

Windows monitor placement does not activate windows, but game startup itself can activate its window. Choose windowed/borderless modes for reliable placement. Windows virtual desktop placement and Linux-style isolated/private display streaming are not implemented. Use Windows' own desktop controls to move windows. Minimized RL can throttle its own updates; Desktop on a second monitor is the better choice if that causes gaps.

## Bring your car and ball visuals

The public bundle contains bridge code and DLLs. It does not contain Rocket League's models/textures, SKSE, Address Library or BakkesMod.

On your existing working Linux installation, export your installed visuals:

```bash
python3 tools/export_local_assets.py /path/to/Skyrim-League-Assets
```

Copy that folder privately to your Windows PC and pass it as `--assets`. It contains only the bridge's car/ball render meshes, NIFs and texture folder. Do not copy Linux `install-state.json`, terrain/contact files or the Linux INI: setup writes native Windows paths for the shared terrain/contact data. Already-installed matching visuals can be used by omitting `--assets`.

Exported asset folders and ZIPs are private local files and are excluded from GitHub releases. If you received a private asset ZIP separately, extract it into a folder and pass that folder to `--assets`. Keep locally extracted game assets private.

## Manage instances and settings

The browser UI edits the same brightness, shadows, damage, smoothing and camera settings as Linux. Settings save only after sessions stop and retain a backup. Stop closes only games started by that session; it leaves Steam and the official injector running. Save Skyrim before stopping.

`launch-bridge.cmd --attach` watches selected already-running games without owning/stopping them. `--no-rl` / `--no-skyrim` select one game, and `--dry-run` prints commands without launching. Normal starts reject pre-existing selected game processes. Stop verifies executable path and process start time to protect against PID reuse.

Setup keeps a backup manifest under `build\install\`. Restore matching files using:

```bat
py -3 tools\restore.py "build\install\YOUR-BACKUP\manifest.json"
```

The launcher scopes optional bridge runtime replacements to its owned offline RL session and restores originals afterward. Normal Steam/EAC startup keeps the stock game runtimes.

Close games before restoring. Later edits are preserved when their hashes differ from the installed version.

## Build natively

Install Visual Studio's C++ desktop workload, Git, Python, CMake and Ninja. Open an **x64 Native Tools Command Prompt** in the source folder:

```bat
py -3 -m pip install cmake ninja -r requirements-windows.txt
py -3 tools\build_windows.py
```

This fetches pinned SDKs and the verified CommonLib bundle, builds one uniformly patched RL-compatible Bullet tree, builds the DLLs, and runs core/timing/contact regressions. It needs no Linux compiler, Proton, X server or VFS header overlay. Output uses `build-win\`, matching the installer. Use a clean build directory when moving a source checkout between Linux cross builds and native Windows builds.

Package a fresh Windows build with `py -3 tools\package_windows.py`. Full sources and dependency references are included alongside binary checksums. Install the Visual C++ runtime before running the bundled regression executables directly.
