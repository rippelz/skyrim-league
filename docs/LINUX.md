# Linux / Proton setup

The tested environment is CachyOS/Arch with Steam, Hyprland and AMD Vulkan graphics. The launcher uses Steam's configured Proton and Linux Runtime. Hyprland placement and Arch's viewer-package fetcher are platform-specific; other Linux desktops/distributions need adaptation.

Install both Steam games, run Skyrim's normal first-time setup once, and install SKSE 2.3.1 for Skyrim 1.7.104.0 plus Address Library v13. Keep Steam signed in. The tested Rocket League builds and BakkesMod runtime are listed in the root README. This bridge runs offline Free Play.

## Build and install

Fetch the pinned SDKs, then cross-build with clang-cl/lld-link, CMake, Ninja and an x64 MSVC header/library installation:

```bash
python3 tools/fetch_deps.py
python3 tools/build_linux.py --msvc-root /path/to/msvc-install --sdk-root /path/to/windows-sdk-install
```

The compiler runs on Linux using the MSVC ABI. Outputs are `build-win/bakkes-plugin/RocketSkyrim.dll` and `build-win/skse-plugin/SkyrimRocketBridge.dll`.

Download the official compatible BakkesMod runtime/injector from [BakkesMod](https://bakkesmod.com/download.php) and place `BakkesMod.exe` plus the extracted `files/` directory under `.deps/bakkesmod-runtime`. The Linux installer preserves existing BakkesMod configuration/binds, verifies versions, installs the bridge DLLs and writes the shared cross-prefix terrain path:

```bash
python3 tools/install.py --address-library /path/to/Address-Library-v13.zip \
  --crt /path/to/Microsoft.VC145.CRT
```

Matching SKSE must already be installed in Skyrim's game folder. `--crt` supplies optional matching app-local Visual C++ runtime DLLs for Skyrim and the injector. RL’s bundled DLLs stay intact; the launcher borrows the newer injector runtimes only during a bridge session, restoring originals on stop. Backups and detected paths are written under ignored `build/`.

## Local visual assets

Game models/textures are not in the repository. The existing extractor reads your installed RL packages and works on private copies. To prepare the converter and exporter:

```bash
python3 tools/build_assets.py --exporter
python3 -m venv .deps/asset-env
.deps/asset-env/bin/python -m pip install cryptography
```

The extraction helper imports Tkinter, so its Python installation also needs Tk support. Export the Fennec body/lenses, wheels and standard ball; the ball texture objects need explicit exports:

```bash
.deps/asset-env/bin/python tools/extract_assets.py body_grain_SF.upk
.deps/asset-env/bin/python tools/extract_assets.py wheel_oemplus_SF.upk
.deps/asset-env/bin/python tools/extract_assets.py GameInfo_Soccar_SF.upk
.deps/asset-env/bin/python tools/extract_assets.py GameInfo_Soccar_SF.upk Ball_Default00_D
.deps/asset-env/bin/python tools/extract_assets.py GameInfo_Soccar_SF.upk Ball_Default00_N
python3 tools/install_assets.py
```

This builds actual NIF/direct-render meshes, copies textures and configures model paths. See the [development notes](DEVELOPMENT-NOTES.md) for material and mesh details. Exported packages, meshes and textures stay in ignored local folders.

## Launcher and displays

On Arch/CachyOS, prepare the local browser-viewer dependencies:

```bash
python3 tools/fetch_viewer.py
./bridge-ui.sh
```

The fetcher uses pacman metadata and the Arch package keyring, and extracts dependencies locally. The launcher also needs Xvfb and xauth; it uses Openbox, x11vnc, noVNC and websockify for private displays.

Choose each game's enabled state, private/desktop display, monitor and workspace independently. For direct Skyrim play while RL remains private:

```bash
./launch-bridge.sh --retry-skyrim --manual-bridge --desktop-skyrim
```

Open the printed local panel for RL's Free Play setup, load Skyrim and press F8. The session does not start another Steam client. It refuses duplicate game sessions and recorded sharing failures unless retried explicitly. Ctrl+C stops only owned session processes and restores settings that have not changed since launch. Closing the browser alone leaves the games running.

Use `python3 tools/doctor.py` for local diagnostics. Save Skyrim before stopping. Keep `build/install-state.json`, session logs, saves and private browser tokens out of GitHub.
