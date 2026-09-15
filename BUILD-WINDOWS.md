# VyanHQ Draw v2 — Windows x64 build

This project is designed to be built using the same GitHub Actions workflow that already succeeded for VyanHQ Draw v1.11/v1.18.

1. Replace the source/workflow/buildspec files in the existing GitHub repository with this project's files.
2. Run `Actions -> Build VyanHQ Draw for Windows x64 -> Run workflow` on `main`.
3. Download the `VyanHQ-Draw-OBS-Windows-x64` artifact.
4. Install the release package to OBS Studio 32.2.2 x64.

The first V2 milestone is intentionally local/private: native canvas + native OBS source. Cloudflare member collaboration remains isolated so the native renderer can be validated first.
