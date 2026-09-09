# Windows build

The GitHub Actions workflow runs on `windows-2022`, clones the official OBS
plugin template, injects the VyanHQ plugin sources, patches the template for
OBS Studio 32.2.2 and then calls the template's official
`.github/scripts/Build-Windows.ps1` script.

The resulting Windows x64 package is uploaded as a GitHub Actions artifact.
