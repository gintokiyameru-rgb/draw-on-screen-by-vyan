export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/api/room/new' && request.method === 'POST') {
      const room = 'room-' + crypto.randomUUID().slice(0, 8);
      const id = env.ROOMS.idFromName(room);
      const stub = env.ROOMS.get(id);
      const r = await stub.fetch(new Request(url.origin + '/init', {method:'POST'}));
      const data = await r.json();
      return Response.json({room, ...data}, {headers:{'cache-control':'no-store'}});
    }
    if (url.pathname === '/api/control' && request.method === 'POST') {
      const body = await request.json().catch(()=>null);
      if (!body?.room || !body?.hostToken || !['clear','lock','unlock'].includes(body.action)) return Response.json({error:'bad request'}, {status:400});
      const id = env.ROOMS.idFromName(body.room);
      return env.ROOMS.get(id).fetch(new Request(url.origin + '/control', {method:'POST', body:JSON.stringify(body), headers:{'content-type':'application/json'}}));
    }
    if (url.pathname === '/ws') {
      const room = url.searchParams.get('room') || '';
      const token = url.searchParams.get('token') || '';
      const role = url.searchParams.get('role') || 'member';
      if (!room || token.length < 6) return new Response('Unauthorized', {status:401});
      const id = env.ROOMS.idFromName(room);
      return env.ROOMS.get(id).fetch(request);
    }
    if (url.pathname === '/member' || url.pathname === '/host' || url.pathname === '/overlay') {
      const target = url.pathname + '.html';
      return env.ASSETS.fetch(new Request(new URL(target, request.url), request));
    }
    return env.ASSETS.fetch(request);
  }
};

export class Room {
  constructor(state) { this.state = state; this.sockets = new Map(); this.objects = new Map(); this.locked = false; this.loaded = false; }
  async load() {
    if (this.loaded) return;
    this.loaded = true;
    await this.state.storage.sql.exec('CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT);');
    await this.state.storage.sql.exec('CREATE TABLE IF NOT EXISTS objects (id TEXT PRIMARY KEY, json TEXT NOT NULL);');
    const meta = await this.state.storage.sql.exec('SELECT k,v FROM meta').toArray();
    for (const row of meta) { if(row.k==='member_token') this.memberToken=row.v; if(row.k==='host_token') this.hostToken=row.v; if(row.k==='locked') this.locked=row.v==='1'; }
    const rows = await this.state.storage.sql.exec('SELECT id,json FROM objects').toArray();
    for (const row of rows) this.objects.set(row.id, JSON.parse(row.json));
  }
  async fetch(request) {
    await this.load();
    const url = new URL(request.url);
    if (url.pathname === '/init') {
      if (!this.memberToken) {
        this.memberToken = crypto.randomUUID().replaceAll('-','');
        this.hostToken = crypto.randomUUID().replaceAll('-','');
        await this.state.storage.sql.exec('INSERT OR REPLACE INTO meta(k,v) VALUES(?,?),(?,?),(?,?)','member_token',this.memberToken,'host_token',this.hostToken,'locked','0');
      }
      return Response.json({memberToken:this.memberToken, hostToken:this.hostToken});
    }
    if (url.pathname === '/control' && request.method === 'POST') {
      const body = await request.json();
      if (body.hostToken !== this.hostToken) return Response.json({error:'forbidden'},{status:403});
      if (body.action === 'clear') { this.objects.clear(); await this.state.storage.sql.exec('DELETE FROM objects'); await this.broadcast({type:'clear'}); }
      if (body.action === 'lock') { this.locked=true; await this.persistLock(); await this.broadcast({type:'lock',locked:true}); }
      if (body.action === 'unlock') { this.locked=false; await this.persistLock(); await this.broadcast({type:'lock',locked:false}); }
      return Response.json({ok:true,locked:this.locked});
    }
    if (request.headers.get('Upgrade') !== 'websocket') return new Response('WebSocket required',{status:426});
    const token = url.searchParams.get('token') || '';
    const role = url.searchParams.get('role') || 'member';
    if ((role==='host' || role==='overlay') && token !== this.hostToken) return new Response('Forbidden',{status:403});
    if (role==='member' && token !== this.memberToken) return new Response('Forbidden',{status:403});
    const pair = new WebSocketPair(); const client=pair[0], server=pair[1]; const id=crypto.randomUUID(); this.sockets.set(id,{ws:server,role}); server.accept();
    server.send(JSON.stringify({type:'snapshot',objects:[...this.objects.values()],locked:this.locked}));
    server.addEventListener('message',async e=>{try{const m=JSON.parse(e.data);if(m.type==='object'&&role==='member'&&!this.locked){this.objects.set(m.object.id,m.object);await this.state.storage.sql.exec('INSERT OR REPLACE INTO objects(id,json) VALUES(?,?)',m.object.id,JSON.stringify(m.object));await this.broadcast(m)}else if(m.type==='delete'&&role==='member'){this.objects.delete(m.id);await this.state.storage.sql.exec('DELETE FROM objects WHERE id=?',m.id);await this.broadcast(m)}else if(m.type==='clear-user'&&role==='member'){for(const[id,o]of this.objects){if(o.userId===m.userId){this.objects.delete(id);await this.state.storage.sql.exec('DELETE FROM objects WHERE id=?',id)}}await this.broadcast(m)}else if(m.type==='clear'&&role==='host'){this.objects.clear();await this.state.storage.sql.exec('DELETE FROM objects');await this.broadcast({type:'clear'})}else if(m.type==='lock'&&role==='host'){this.locked=!!m.locked;await this.persistLock();await this.broadcast(m)}}catch{}});
    const cleanup=()=>this.sockets.delete(id);server.addEventListener('close',cleanup);server.addEventListener('error',cleanup);return new Response(null,{status:101,webSocket:client});
  }
  async persistLock(){await this.state.storage.sql.exec('INSERT OR REPLACE INTO meta(k,v) VALUES(?,?)','locked',this.locked?'1':'0')}
  async broadcast(m){const data=JSON.stringify(m);for(const[id,item]of this.sockets){try{item.ws.send(data)}catch{this.sockets.delete(id)}}}
}
