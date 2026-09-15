# VyanHQ Draw Cloudflare relay

This Worker exposes an HTTPS member canvas and a WebSocket room endpoint. Each room is a Durable Object; operations are stored only while the room is active and are cleared when the last client disconnects.

## Deploy

```powershell
cd cloudflare
npm install
npx wrangler login
npm run deploy
```

After deployment, use the `https://<worker>.workers.dev` URL in VyanHQ Draw's **CONNECT** dialog. The plugin converts it to `wss://` for the native OBS client. The **Copy member link** button produces `https://<worker>.workers.dev/room/<room>` for viewers.

No home IP, port forwarding, or inbound connection to OBS is required.
