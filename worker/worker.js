export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const assets = env.ASSETS;
    if (url.pathname === '/api/health' && request.method === 'GET') return Response.json({ok:true,service:'vyanhq-draw',version:'1.14'}, {headers:{'cache-control':'no-store'}});
    if (url.pathname === '/api/room/new' && request.method === 'POST') {
      const body = await request.json().catch(()=>({}));
      const room = 'room-' + crypto.randomUUID().slice(0, 8);
      const id = env.ROOMS.idFromName(room); const stub = env.ROOMS.get(id);
      const r = await stub.fetch(new Request(url.origin + '/init', {method:'POST', body:JSON.stringify({kind:'member', name:body?.name || 'VyanHQ Live Draw'}), headers:{'content-type':'application/json'}}));
      return Response.json({room, ...(await r.json())},{headers:{'cache-control':'no-store'}});
    }
    if (url.pathname === '/api/canvas/new' && request.method === 'POST') {
      const body = await request.json().catch(()=>({}));
      const canvas = 'canvas-' + crypto.randomUUID().slice(0, 8);
      const id = env.ROOMS.idFromName(canvas); const stub = env.ROOMS.get(id);
      const r = await stub.fetch(new Request(url.origin + '/init', {method:'POST', body:JSON.stringify({kind:'canvas', name:body?.name || 'VyanHQ Draw'}), headers:{'content-type':'application/json'}}));
      const data=await r.json(); return Response.json({canvas, name:body?.name || 'VyanHQ Draw', hostToken:data.hostToken},{headers:{'cache-control':'no-store'}});
    }
    if (url.pathname === '/api/control' && request.method === 'POST') {
      const body = await request.json().catch(()=>null);
      if (!body?.room || !body?.hostToken || !['clear-members','clear','lock','unlock'].includes(body.action)) return Response.json({error:'bad request'},{status:400});
      const id=env.ROOMS.idFromName(body.room); return env.ROOMS.get(id).fetch(new Request(url.origin+'/control',{method:'POST',body:JSON.stringify(body),headers:{'content-type':'application/json'}}));
    }
    if (url.pathname === '/ws') {
      const room=url.searchParams.get('room')||'', token=url.searchParams.get('token')||'', role=url.searchParams.get('role')||'member';
      if(!room||token.length<6) return new Response('Unauthorized',{status:401});
      const id=env.ROOMS.idFromName(room); return env.ROOMS.get(id).fetch(request);
    }
    if (url.pathname === '/member' || url.pathname === '/host' || url.pathname === '/hostdraw' || url.pathname === '/overlay') {
      const target=url.pathname+'.html'; return assets.fetch(new Request(new URL(target,request.url),request));
    }
    return assets.fetch(request);
  }
};

export class Room {
  constructor(state){this.state=state;this.sockets=new Map();this.objects=new Map();this.locked=false;this.loaded=false;this.kind='member';this.name='VyanHQ Draw';}
  async load(){
    if(this.loaded)return; this.loaded=true;
    await this.state.storage.sql.exec('CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT);');
    await this.state.storage.sql.exec('CREATE TABLE IF NOT EXISTS objects (id TEXT PRIMARY KEY, json TEXT NOT NULL);');
    const meta=await this.state.storage.sql.exec('SELECT k,v FROM meta').toArray();
    for(const row of meta){if(row.k==='member_token')this.memberToken=row.v;if(row.k==='host_token')this.hostToken=row.v;if(row.k==='locked')this.locked=row.v==='1';if(row.k==='kind')this.kind=row.v;if(row.k==='name')this.name=row.v;}
    const rows=await this.state.storage.sql.exec('SELECT id,json FROM objects').toArray(); for(const row of rows)this.objects.set(row.id,JSON.parse(row.json));
  }
  async persist(k,v){await this.state.storage.sql.exec('INSERT OR REPLACE INTO meta(k,v) VALUES(?,?)',k,v);}
  async fetch(request){
    await this.load(); const url=new URL(request.url);
    if(url.pathname==='/init'){
      const body=await request.json().catch(()=>({}));
      if(!this.hostToken){this.hostToken=crypto.randomUUID().replaceAll('-',''); await this.persist('host_token',this.hostToken);}
      this.kind=body.kind||this.kind; this.name=body.name||this.name; await this.persist('kind',this.kind); await this.persist('name',this.name);
      if(this.kind==='member'&&!this.memberToken){this.memberToken=crypto.randomUUID().replaceAll('-',''); await this.persist('member_token',this.memberToken);}
      return Response.json({name:this.name,hostToken:this.hostToken,...(this.kind==='member'?{memberToken:this.memberToken}:{} )});
    }
    if(url.pathname==='/control'&&request.method==='POST'){
      const body=await request.json(); if(body.hostToken!==this.hostToken)return Response.json({error:'forbidden'},{status:403});
      if(body.action==='clear-members'||body.action==='clear'){for(const[id,o]of this.objects){if(body.action==='clear'||o.userId!=='host'){this.objects.delete(id);await this.state.storage.sql.exec('DELETE FROM objects WHERE id=?',id);}}await this.broadcast({type:body.action});}
      if(body.action==='lock'){this.locked=true;await this.persist('locked','1');await this.broadcast({type:'lock',locked:true});}
      if(body.action==='unlock'){this.locked=false;await this.persist('locked','0');await this.broadcast({type:'lock',locked:false});}
      return Response.json({ok:true,locked:this.locked});
    }
    if(request.headers.get('Upgrade')!=='websocket')return new Response('WebSocket required',{status:426});
    const token=url.searchParams.get('token')||'',role=url.searchParams.get('role')||'member';
    const validHost=(role==='host'||role==='overlay')&&token===this.hostToken;
    const validMember=role==='member'&&this.kind==='member'&&token===this.memberToken;
    if(!validHost&&!validMember)return new Response('Forbidden',{status:403});
    const pair=new WebSocketPair(),client=pair[0],server=pair[1],id=crypto.randomUUID(); this.sockets.set(id,{ws:server,role,userId:role==='host'?'host':id}); server.accept();
    server.send(JSON.stringify({type:'snapshot',objects:[...this.objects.values()],locked:this.locked,name:this.name,kind:this.kind}));
    server.addEventListener('message',async e=>{try{const m=JSON.parse(e.data); if(m.type==='object'&&role==='member'&&!this.locked){m.object.userId=m.userId||id;this.objects.set(m.object.id,m.object);await this.state.storage.sql.exec('INSERT OR REPLACE INTO objects(id,json) VALUES(?,?)',m.object.id,JSON.stringify(m.object));await this.broadcast(m);} else if(m.type==='object'&&role==='host'&&!this.locked){m.object.userId='host';this.objects.set(m.object.id,m.object);await this.state.storage.sql.exec('INSERT OR REPLACE INTO objects(id,json) VALUES(?,?)',m.object.id,JSON.stringify(m.object));await this.broadcast(m);} else if(m.type==='delete'&&(role==='member'||role==='host')){this.objects.delete(m.id);await this.state.storage.sql.exec('DELETE FROM objects WHERE id=?',m.id);await this.broadcast(m);} else if(m.type==='clear-user'&&role==='member'){for(const[id,o]of this.objects){if(o.userId===m.userId){this.objects.delete(id);await this.state.storage.sql.exec('DELETE FROM objects WHERE id=?',id);}}await this.broadcast(m);} else if(m.type==='clear-members'&&role==='host'){for(const[id,o]of this.objects){if(o.userId!=='host'){this.objects.delete(id);await this.state.storage.sql.exec('DELETE FROM objects WHERE id=?',id);}}await this.broadcast({type:'clear-members'});} else if(m.type==='clear-host'&&role==='host'){for(const[id,o]of this.objects){if(o.userId==='host'){this.objects.delete(id);await this.state.storage.sql.exec('DELETE FROM objects WHERE id=?',id);}}await this.broadcast({type:'clear-host'});} else if(m.type==='lock'&&role==='host'){this.locked=!!m.locked;await this.persist('locked',this.locked?'1':'0');await this.broadcast(m);}}catch{}});
    const cleanup=()=>this.sockets.delete(id); server.addEventListener('close',cleanup);server.addEventListener('error',cleanup);return new Response(null,{status:101,webSocket:client});
  }
  async broadcast(m){const data=JSON.stringify(m);for(const[id,item]of this.sockets){try{item.ws.send(data);}catch{this.sockets.delete(id);}}}
}
