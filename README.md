# VyanHQ Draw v1.7

Windows build workflow for OBS Studio 32.2.2 x64.

This revision fixes the CI invocation to use the official OBS template's
`.github/scripts/Build-Windows.ps1` instead of a root `build.ps1` that does not
exist in the cloned template. Artifact upload uses `actions/upload-artifact@v6`.
