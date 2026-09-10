# Build VyanHQ Draw v1.15

Use the same GitHub Actions workflow proven by v1.11. Replace `src/plugin-main.cpp` with this version and rebuild the Windows x64 artifact.


## v1.21 changes
Native private canvas rendering uses `obs_source_draw()` and DRAW detects the real OBS source so a stale QSettings flag cannot suppress the Create Canvas dialog.
