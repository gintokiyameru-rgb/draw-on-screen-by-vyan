# VyanHQ Draw v1.1 — OBS Windows Build Package

Target: OBS Studio 32.2.2 64-bit on Windows x64.

This package is designed to be uploaded to GitHub and built by GitHub Actions on a Windows runner. It uses the official OBS plugin template at build time, avoiding the incomplete CMake skeleton that was in the earlier prototype.

## What the native plugin does

- Adds a `VyanHQ Draw` dock inside OBS.
- Stores cloud Server URL, Room, Host Token and Member Token.
- Provides `Clear All`, `Lock`, `Unlock` controls.
- Registers native OBS hotkeys for Clear All / Lock / Unlock.
- Copies Member URL and transparent OBS Overlay URL.
- Opens the Host dashboard.

The actual transparent drawing layer remains an OBS Browser Source; this keeps the rendering side web-based while the native plugin is the OBS control center.

## Build

1. Create a GitHub repository.
2. Upload the contents of this folder to the repository.
3. Open Actions → `Build VyanHQ Draw for Windows x64`.
4. Click `Run workflow`.
5. Download artifact `VyanHQ-Draw-OBS-Windows-x64`.

The workflow pins the OBS source target to 32.2.2 and uses the current OBS dependency/Qt6 release hashes published by the OBS project.

## Install

The artifact is a plugin package. Close OBS before installing. Extract/copy its `obs-plugins/64bit` and `data/obs-plugins/vyanhq-draw` directories into your OBS Studio installation directory, normally:

`C:\Program Files\obs-studio\`

Then restart OBS.

The dock appears under `View → Docks → VyanHQ Draw`.

## Cloud worker

The worker/web folders from the earlier VyanHQ Draw project are intentionally not merged into the native plugin build. Deploy them separately to Cloudflare Workers + Durable Objects so members never connect to the streamer's IP.

