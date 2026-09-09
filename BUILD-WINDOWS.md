# Windows build options

## Recommended: GitHub Actions

Use the included `.github/workflows/build-windows.yml`. The workflow clones the official OBS plugin template, overlays this plugin, downloads OBS 32.2.2 sources, calculates the source archive SHA256 on the Windows runner, configures CMake, builds, packages, and uploads the resulting Windows x64 plugin ZIP.

This is preferable to manually reconstructing the OBS SDK because the official template supplies the CMake helper modules and packaging scripts required by OBS plugins.

## Local build

On Windows 11, install Visual Studio 2022 with Desktop development with C++, CMake 3.30+, and Git. Clone the official OBS plugin template, then copy this project's `src/plugin-main.cpp`, `CMakeLists.txt`, `buildspec.json`, and locale file into the template before running the template's Windows build script/presets.
