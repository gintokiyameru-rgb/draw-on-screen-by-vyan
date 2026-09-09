# VyanHQ Draw v1.8 — Full Auto + Windows Build Fix

This package combines the working OBS 32.2.2 build configuration from v1.5 with the v1.6 full-automatic OBS UI/source.

Key fixes:
- Restores the complete `buildspec.json` required by the current OBS plugin template (`dependencies`, `email`, `platformConfig`).
- Keeps OBS 32.2.2 / Windows x64 dependency versions.
- Enables OBS Frontend API and Qt during CI build.
- Uses the official OBS Windows build script.
- Uses `actions/upload-artifact@v6`.

The `web/` and `worker/` directories may stay in the same GitHub repository; they are not compiled into the OBS DLL unless explicitly referenced by the build.
