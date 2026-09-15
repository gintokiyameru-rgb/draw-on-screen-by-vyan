import { DurableObject } from "cloudflare:workers";

interface Env {
  DRAW_ROOM: DurableObjectNamespace;
}

const HTML = String.raw`<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>VyanHQ Draw</title>
<style>html,body{margin:0;height:100%;background:#17171b;color:#eee;font-family:system-ui,sans-serif}body{display:flex;flex-direction:column}#bar{display:flex;gap:6px;align-items:center;padding:8px;background:#202026;flex-wrap:wrap}button{border:1px solid #484852;background:#2b2b33;color:#eee;border-radius:6px;padding:7px 10px;cursor:pointer}button.active{background:#6d4aff;border-color:#9a84ff}#status{margin-left:auto;color:#aaa}#wrap{flex:1;display:flex;justify-content:center;align-items:center;padding:10px}canvas{background:#000;max-width:100%;max-height:100%;box-shadow:0 0 0 1px #4b4b54}input{width:80px;background:#2b2b33;color:#eee;border:1px solid #484852;border-radius:4px;padding:5px}</style></head>
<body><div id="bar"><button data-tool="pen" class="active">Pen</button><button data-tool="eraser">Eraser</button><button data-tool="line">Line</button><button data-tool="rect">Rect</button><button data-tool="circle">Circle</button><button data-tool="arrow">Arrow</button><button id="clear">Clear</button><label>Size <input id="size" type="range" min="1" max="64" value="8"></label><span id="status">Connecting…</span></div><div id="wrap"><canvas id="c" width="1920" height="1080"></canvas></div>
<script>
const canvas=document.getElementById('c'),ctx=canvas.getContext('2d');ctx.clearRect(0,0,canvas.width,canvas.height);ctx.lineCap='round';ctx.lineJoin='round';
const room=decodeURIComponent(location.pathname.split('/').filter(Boolean).pop()||'');
const statusEl=document.getElementById('status'); let tool='pen',size=8,drawing=false,start=null,last=null;
const color='#ffffff';
function setTool(t){tool=t;document.querySelectorAll('[data-tool]').forEach(b=>b.classList.toggle('active',b.dataset.tool===t));}
document.querySelectorAll('[data-tool]').forEach(b=>b.onclick=()=>setTool(b.dataset.tool));document.getElementById('size').oninput=e=>size=+e.target.value;
function point(e){const r=canvas.getBoundingClientRect();return {x:Math.max(0,Math.min(1919,Math.round((e.clientX-r.left)/r.width*1919))),y:Math.max(0,Math.min(1079,Math.round((e.clientY-r.top)/r.height*1079)))};}
function pen(op){ctx.strokeStyle=op.color||color;ctx.lineWidth=op.size||size;ctx.globalCompositeOperation=op.tool==='eraser'?'destination-out':'source-over';ctx.beginPath();ctx.moveTo(op.x1,op.y1);ctx.lineTo(op.x2,op.y2);ctx.stroke();ctx.globalCompositeOperation='source-over';}
function shape(op){ctx.strokeStyle=op.color||color;ctx.lineWidth=op.size||size;ctx.globalCompositeOperation='source-over';const a={x:op.x1,y:op.y1},b={x:op.x2,y:op.y2};if(op.tool==='line')ctx.beginPath(),ctx.moveTo(a.x,a.y),ctx.lineTo(b.x,b.y),ctx.stroke();else if(op.tool==='rect')ctx.strokeRect(Math.min(a.x,b.x),Math.min(a.y,b.y),Math.abs(b.x-a.x),Math.abs(b.y-a.y));else if(op.tool==='circle'){const cx=(a.x+b.x)/2,cy=(a.y+b.y)/2,rx=Math.abs(b.x-a.x)/2,ry=Math.abs(b.y-a.y)/2;ctx.beginPath();ctx.ellipse(cx,cy,rx,ry,0,0,Math.PI*2);ctx.stroke();}else if(op.tool==='arrow'){ctx.beginPath();ctx.moveTo(a.x,a.y);ctx.lineTo(b.x,b.y);ctx.stroke();const ang=Math.atan2(b.y-a.y,b.x-a.x),s=Math.max(12,(op.size||size)*2.5);ctx.beginPath();ctx.moveTo(b.x,b.y);ctx.lineTo(b.x-Math.cos(ang-Math.PI/6)*s,b.y-Math.sin(ang-Math.PI/6)*s);ctx.moveTo(b.x,b.y);ctx.lineTo(b.x-Math.cos(ang+Math.PI/6)*s,b.y-Math.sin(ang+Math.PI/6)*s);ctx.stroke();}}
function apply(op){if(op.type==='clear'){ctx.clearRect(0,0,1920,1080);return}if(op.tool==='pen'||op.tool==='eraser')pen(op);else if(op.tool==='line'||op.tool==='rect'||op.tool==='circle'||op.tool==='arrow')shape(op);}
function send(op){if(ws&&ws.readyState===1)ws.send(JSON.stringify(op));}
canvas.addEventListener('pointerdown',e=>{e.preventDefault();canvas.setPointerCapture(e.pointerId);start=last=point(e);drawing=true;if(tool==='pen'||tool==='eraser'){const op={type:'draw',tool,x1:last.x,y1:last.y,x2:last.x,y2:last.y,color,size};apply(op);send(op);}});
canvas.addEventListener('pointermove',e=>{if(!drawing)return;const p=point(e);if(tool==='pen'||tool==='eraser'){const op={type:'draw',tool,x1:last.x,y1:last.y,x2:p.x,y2:p.y,color,size};apply(op);send(op);last=p;}});
canvas.addEventListener('pointerup',e=>{if(!drawing)return;const p=point(e);if(['line','rect','circle','arrow'].includes(tool)){const op={type:'draw',tool,x1:start.x,y1:start.y,x2:p.x,y2:p.y,color,size};apply(op);send(op);}drawing=false;});
document.getElementById('clear').onclick=()=>{ctx.clearRect(0,0,1920,1080);send({type:'clear'});};
const ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/room/'+encodeURIComponent(room));
ws.onopen=()=>{statusEl.textContent='Connected · Room '+room;ws.send(JSON.stringify({type:'join',room,client:'web-'+crypto.randomUUID()}));};
ws.onclose=()=>statusEl.textContent='Disconnected';ws.onerror=()=>statusEl.textContent='Connection error';
ws.onmessage=e=>{try{const m=JSON.parse(e.data);if(m.type==='ops'){for(const op of (m.ops||[]))apply(op);}else apply(m);}catch{}};
</script></body></html>`;

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const match = url.pathname.match(/^\/room\/([^/]+)$/);
    if (match && request.headers.get("Upgrade")?.toLowerCase() === "websocket") {
      const room = decodeURIComponent(match[1]);
      const id = env.DRAW_ROOM.idFromName(room);
      const stub = env.DRAW_ROOM.get(id);
      return stub.fetch(request);
    }
    if (match) return new Response(HTML, { headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store" } });
    return new Response("VyanHQ Draw relay is online. Open /room/<room> for a member canvas.", { headers: { "content-type": "text/plain; charset=utf-8" } });
  }
};

export class DrawRoom extends DurableObject<Env> {
  private initialized = false;
  private ensureSchema() {
    if (this.initialized) return;
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS ops (id INTEGER PRIMARY KEY AUTOINCREMENT, data TEXT NOT NULL)");
    this.initialized = true;
  }

  async fetch(request: Request): Promise<Response> {
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") return new Response("Expected WebSocket", { status: 426 });
    this.ensureSchema();
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server, ["draw"]);
    server.serializeAttachment({ joined: false, client: crypto.randomUUID() });
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws: WebSocket, message: ArrayBuffer | string) {
    this.ensureSchema();
    let msg: any;
    try { msg = JSON.parse(typeof message === "string" ? message : new TextDecoder().decode(message)); } catch { return; }
    if (msg.type === "join") {
      ws.serializeAttachment({ joined: true, client: msg.client || crypto.randomUUID() });
      const rows = this.ctx.storage.sql.exec("SELECT data FROM ops ORDER BY id").toArray();
      const ops = rows.map((r: any) => JSON.parse(r.data));
      ws.send(JSON.stringify({ type: "ops", ops }));
      this.broadcast(JSON.stringify({ type: "peer", event: "join" }), ws);
      return;
    }
    if (msg.type === "clear") {
      this.ctx.storage.sql.exec("DELETE FROM ops");
      this.broadcast(JSON.stringify({ type: "clear" }));
      return;
    }
    if (msg.type === "draw") {
      const safe = {
        type: "draw", tool: String(msg.tool || "pen"), x1: Number(msg.x1)||0, y1: Number(msg.y1)||0,
        x2: Number(msg.x2)||0, y2: Number(msg.y2)||0, color: String(msg.color || "#ffffffff"), size: Math.max(1, Math.min(128, Number(msg.size)||8)),
        ...(msg.text ? { text: String(msg.text).slice(0, 2000) } : {})
      };
      this.ctx.storage.sql.exec("INSERT INTO ops (data) VALUES (?)", JSON.stringify(safe));
      this.broadcast(JSON.stringify(safe), ws);
    }
  }

  webSocketClose() { this.cleanupWhenEmpty(); }
  webSocketError() { this.cleanupWhenEmpty(); }

  private broadcast(data: string, except?: WebSocket) {
    for (const ws of this.ctx.getWebSockets()) {
      if (ws !== except && ws.readyState === WebSocket.OPEN) {
        try { ws.send(data); } catch {}
      }
    }
  }

  private cleanupWhenEmpty() {
    if (this.ctx.getWebSockets().length === 0) {
      this.ensureSchema();
      this.ctx.storage.sql.exec("DELETE FROM ops");
    }
  }
}
`;
