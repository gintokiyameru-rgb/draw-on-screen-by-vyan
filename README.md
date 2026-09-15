# VyanHQ Draw v2 — Native Canvas Test

This build is the first V2 native-canvas prototype. It focuses on the private DRAW workflow:

- Native OBS source at 1920x1080 with transparent background
- DRAW -> Create Canvas dialog -> native drawing window
- Pen, eraser, line, rectangle, ellipse, arrow, text
- Brush and eraser sliders
- Color picker
- Undo/redo
- Zoom
- Basic layers panel
- OBS Hotkeys: Clear My Canvas and Toggle Canvas
- CONNECT remains reserved for the future Cloudflare member room

The v2 architecture intentionally keeps member/cloud networking separate from the native private canvas so the renderer can be validated first.
