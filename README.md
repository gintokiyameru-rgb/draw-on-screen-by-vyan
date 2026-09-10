VyanHQ Draw v1.16

DRAW is a completely local/private OBS drawing canvas. It does not create or require a Cloudflare member room.
CONNECT is only for creating the collaborative member room and its cloud connection.
The profile-outline button copies the member link. OBS hotkeys control member clear, member lock/unlock, private canvas clear, and private canvas visibility.


## v1.17 build fix
Fixed OBS 32.2.2 `obs_source_info::get_name` callback signature for the native private canvas source. Also retains en-US and en-GB locale files.


This source revision includes cursor mapping/rendering fixes and explicit en-US/en-GB packaging.


## v1.21
- Private OBS source renders through `obs_source_draw()` so the 1920×1080 transparent texture is composited correctly.
- DRAW checks the actual OBS source rather than a stale saved flag; it only shows Create Canvas when the source is absent.
- Existing private source is automatically ensured visible in the active scene.
- OBS overlay is fixed at 1920×1080.
- Both en-US and en-GB locale files are included in the build package.
