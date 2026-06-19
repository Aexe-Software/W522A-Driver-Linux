#!/usr/bin/env python3
# Self-contained local speedtest mini-site for the W522A AP box.
# Pure Python3 stdlib, no external deps. Measures the WiFi link phone<->box
# (run it on the AP, hit http://192.168.50.1:8080 from the phone).
#
# Endpoints:
#   GET  /                 -> speedtest UI
#   GET  /reports          -> history table + chart of how speed changed
#   GET  /garbage?n=BYTES  -> streams BYTES of incompressible data (download test)
#   POST /empty            -> sinks the body, returns bytes received (upload test)
#   GET  /ping             -> tiny reply (latency/jitter)
#   POST /api/save         -> append a result {down,up,ping,jitter,note}
#   GET  /api/results      -> JSON of all saved results
import os, json, time, threading, http.server, socketserver, urllib.parse

PORT = 8080
RESULTS = "/root/speedtest/results.json"
CHUNK = b"\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff" * 4096  # 64 KiB
_lock = threading.Lock()

def load_results():
    try:
        with open(RESULTS) as f:
            return json.load(f)
    except Exception:
        return []

def save_result(rec):
    with _lock:
        data = load_results()
        data.append(rec)
        os.makedirs(os.path.dirname(RESULTS), exist_ok=True)
        tmp = RESULTS + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f)
        os.replace(tmp, RESULTS)
    return len(data)

INDEX_HTML = r"""<!DOCTYPE html>
<html lang="uk"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>W522A Local SpeedTest</title>
<style>
*{box-sizing:border-box;font-family:system-ui,Segoe UI,Roboto,sans-serif}
body{margin:0;background:#0d1117;color:#e6edf3;text-align:center}
.wrap{max-width:680px;margin:0 auto;padding:18px}
h1{font-size:20px;margin:10px 0 2px}.sub{color:#8b949e;font-size:13px;margin-bottom:14px}
.g{display:flex;gap:12px;justify-content:center;flex-wrap:wrap;margin:18px 0}
.card{background:#161b22;border:1px solid #30363d;border-radius:14px;padding:16px 18px;min-width:140px}
.card .lbl{color:#8b949e;font-size:12px;text-transform:uppercase;letter-spacing:.5px}
.card .val{font-size:34px;font-weight:700;margin-top:4px}
.card .unit{font-size:13px;color:#8b949e}
#dl .val{color:#58a6ff}#ul .val{color:#3fb950}#pg .val{color:#d29922}#jt .val{color:#bc8cff}
button{background:#238636;color:#fff;border:0;border-radius:10px;padding:14px 34px;font-size:17px;font-weight:600;cursor:pointer}
button:disabled{background:#30363d;cursor:not-allowed}
.note{margin:14px 0}.note input{background:#0d1117;border:1px solid #30363d;color:#e6edf3;border-radius:8px;padding:10px;width:80%;font-size:14px}
.bar{height:6px;background:#21262d;border-radius:3px;overflow:hidden;margin:14px 0}
.bar>i{display:block;height:100%;width:0;background:#58a6ff;transition:width .2s}
.status{color:#8b949e;font-size:13px;height:18px;margin:6px 0}
a{color:#58a6ff;text-decoration:none}.foot{margin-top:22px;font-size:13px}
</style></head><body><div class="wrap">
<h1>W522A &nbsp;Local&nbsp;SpeedTest</h1>
<div class="sub">міряє WiFi-лінк телефон &harr; приставка (без інтернету)</div>
<div class="g">
 <div class="card" id="dl"><div class="lbl">Download</div><div class="val">0.0</div><div class="unit">Mbit/s</div></div>
 <div class="card" id="ul"><div class="lbl">Upload</div><div class="val">0.0</div><div class="unit">Mbit/s</div></div>
 <div class="card" id="pg"><div class="lbl">Ping</div><div class="val">0</div><div class="unit">ms</div></div>
 <div class="card" id="jt"><div class="lbl">Jitter</div><div class="val">0</div><div class="unit">ms</div></div>
</div>
<div class="bar"><i id="prog"></i></div>
<div class="status" id="status">готовий</div>
<div class="note"><input id="noteIn" placeholder="мітка тесту (напр. throttle off, wide_lim=8)" maxlength="80"></div>
<button id="go">Старт</button>
<div class="foot"><a href="/reports">&#128202; Звіти — як змінюється швидкість &rarr;</a></div>
</div>
<script>
const $=s=>document.querySelector(s);
const setv=(id,v)=>{$('#'+id+' .val').textContent=v;};
const prog=p=>{$('#prog').style.width=Math.min(100,p*100)+'%';};
const stat=t=>{$('#status').textContent=t;};
const TEST_S=10, STREAMS=4;

async function pingTest(){
  let rtts=[];
  for(let i=0;i<14;i++){
    let t=performance.now();
    try{await fetch('/ping?x='+Math.random(),{cache:'no-store'});}catch(e){}
    rtts.push(performance.now()-t);
  }
  rtts.sort((a,b)=>a-b); rtts=rtts.slice(1,-2); // drop warmup+outliers
  let avg=rtts.reduce((a,b)=>a+b,0)/rtts.length;
  let jit=0; for(let i=1;i<rtts.length;i++)jit+=Math.abs(rtts[i]-rtts[i-1]);
  jit/=Math.max(1,rtts.length-1);
  return {ping:Math.round(avg*10)/10, jitter:Math.round(jit*10)/10};
}

async function dlTest(){
  let total=0, stop=false, t0=performance.now();
  function one(){
    return new Promise(res=>{
      const xhr=new XMLHttpRequest();
      xhr.open('GET','/garbage?n=104857600&x='+Math.random());
      let last=0;
      xhr.onprogress=e=>{total+=e.loaded-last;last=e.loaded; if(stop)xhr.abort();};
      xhr.onloadend=()=>res();
      xhr.onerror=()=>res();
      xhr.send();
    });
  }
  let ps=[]; for(let i=0;i<STREAMS;i++)ps.push(one());
  let iv=setInterval(()=>{
    let dt=(performance.now()-t0)/1000;
    let mbps=total*8/dt/1e6;
    setv('dl',mbps.toFixed(1)); prog(dt/TEST_S);
    if(dt>=TEST_S){stop=true;}
  },200);
  let guard=setTimeout(()=>{stop=true;},TEST_S*1000+500);
  await new Promise(r=>{let c=setInterval(()=>{if(stop){clearInterval(c);r();}},100);});
  clearInterval(iv); clearTimeout(guard);
  await Promise.all(ps).catch(()=>{});
  let dt=(performance.now()-t0)/1000;
  return total*8/dt/1e6;
}

async function ulTest(){
  let total=0, stop=false, t0=performance.now();
  const blob=new Uint8Array(2*1024*1024); // 2 MiB chunk
  for(let i=0;i<blob.length;i+=4096)blob[i]=i&255;
  function loop(){
    if(stop)return Promise.resolve();
    return fetch('/empty?x='+Math.random(),{method:'POST',body:blob,cache:'no-store'})
      .then(()=>{total+=blob.length; if(!stop)return loop();})
      .catch(()=>{});
  }
  let ps=[]; for(let i=0;i<STREAMS;i++)ps.push(loop());
  let iv=setInterval(()=>{
    let dt=(performance.now()-t0)/1000;
    setv('ul',(total*8/dt/1e6).toFixed(1)); prog(dt/TEST_S);
    if(dt>=TEST_S)stop=true;
  },200);
  await new Promise(r=>{let c=setInterval(()=>{if(stop){clearInterval(c);r();}},100);});
  clearInterval(iv);
  await Promise.all(ps).catch(()=>{});
  let dt=(performance.now()-t0)/1000;
  return total*8/dt/1e6;
}

$('#go').onclick=async()=>{
  $('#go').disabled=true; setv('dl','0.0');setv('ul','0.0');setv('pg','0');setv('jt','0');
  stat('пінг...'); let p=await pingTest(); setv('pg',p.ping);setv('jt',p.jitter);
  stat('завантаження (download)...'); let d=await dlTest(); setv('dl',d.toFixed(1)); prog(0);
  stat('віддача (upload)...'); let u=await ulTest(); setv('ul',u.toFixed(1)); prog(0);
  stat('збереження...');
  let note=$('#noteIn').value||'';
  try{await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({down:+d.toFixed(2),up:+u.toFixed(2),ping:p.ping,jitter:p.jitter,note})});}catch(e){}
  stat('готово &#10003; — результат збережено у Звіти');
  $('#status').innerHTML='готово &#10003; — <a href="/reports">дивитись звіти</a>';
  $('#go').disabled=false;
};
</script></body></html>"""

REPORTS_HTML = r"""<!DOCTYPE html>
<html lang="uk"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SpeedTest — Звіти</title>
<style>
*{box-sizing:border-box;font-family:system-ui,Segoe UI,Roboto,sans-serif}
body{margin:0;background:#0d1117;color:#e6edf3}.wrap{max-width:960px;margin:0 auto;padding:18px}
h1{font-size:20px}a{color:#58a6ff;text-decoration:none}
.card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:14px;margin:14px 0}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{padding:8px 10px;border-bottom:1px solid #21262d;text-align:right;white-space:nowrap}
th:first-child,td:first-child,th.l,td.l{text-align:left}
td.dn{color:#58a6ff;font-weight:600}td.up{color:#3fb950;font-weight:600}
.note{color:#8b949e}.muted{color:#8b949e;font-size:13px}
.btns{margin:8px 0}button{background:#21262d;color:#e6edf3;border:1px solid #30363d;border-radius:8px;padding:8px 14px;cursor:pointer}
</style></head><body><div class="wrap">
<h1>&#128202; Звіти швидкості &nbsp;<span class="muted"><a href="/">&larr; назад до тесту</a></span></h1>
<div class="card"><canvas id="chart" height="260"></canvas></div>
<div class="btns"><button onclick="dl()">Експорт CSV</button> <button onclick="clr()">Очистити історію</button></div>
<div class="card"><table id="tbl"><thead><tr>
<th class="l">#</th><th class="l">Час</th><th>Download</th><th>Upload</th><th>Ping</th><th>Jitter</th><th class="l">Мітка</th>
</tr></thead><tbody></tbody></table><div class="muted" id="empty" style="display:none;padding:10px">Поки немає результатів — запустіть тест.</div></div>
</div>
<script>
let R=[];
function fmtT(ts){let d=new Date(ts*1000);return d.toLocaleString();}
function draw(){
  const c=document.getElementById('chart'),x=c.getContext('2d');
  c.width=c.clientWidth; const W=c.width,H=c.height; x.clearRect(0,0,W,H);
  if(!R.length)return;
  const pad=38, n=R.length;
  let mx=Math.max(10,...R.map(r=>Math.max(r.down,r.up)))*1.15;
  x.strokeStyle='#30363d';x.fillStyle='#8b949e';x.font='11px sans-serif';x.lineWidth=1;
  for(let i=0;i<=4;i++){let y=pad+(H-2*pad)*i/4;let v=(mx*(4-i)/4).toFixed(0);
    x.beginPath();x.moveTo(pad,y);x.lineTo(W-8,y);x.stroke();x.fillText(v,4,y+3);}
  const X=i=>pad+(W-pad-8)*(n<=1?0.5:i/(n-1));
  const Y=v=>pad+(H-2*pad)*(1-v/mx);
  function line(key,col){x.strokeStyle=col;x.lineWidth=2;x.beginPath();
    R.forEach((r,i)=>{let px=X(i),py=Y(r[key]);i?x.lineTo(px,py):x.moveTo(px,py);});x.stroke();
    x.fillStyle=col;R.forEach((r,i)=>{x.beginPath();x.arc(X(i),Y(r[key]),3,0,7);x.fill();});}
  line('down','#58a6ff');line('up','#3fb950');
  x.fillStyle='#58a6ff';x.fillText('Download',W-150,16);
  x.fillStyle='#3fb950';x.fillText('Upload',W-70,16);
}
function rows(){
  const tb=document.querySelector('#tbl tbody');tb.innerHTML='';
  document.getElementById('empty').style.display=R.length?'none':'block';
  R.slice().reverse().forEach((r,i)=>{
    const tr=document.createElement('tr');
    tr.innerHTML=`<td class="l">${R.length-i}</td><td class="l">${fmtT(r.ts)}</td>`+
      `<td class="dn">${r.down.toFixed(1)}</td><td class="up">${r.up.toFixed(1)}</td>`+
      `<td>${r.ping}</td><td>${r.jitter}</td><td class="l note">${(r.note||'').replace(/[<>]/g,'')}</td>`;
    tb.appendChild(tr);
  });
}
async function load(){R=await (await fetch('/api/results',{cache:'no-store'})).json();draw();rows();}
function dl(){let s='time,download_mbit,upload_mbit,ping_ms,jitter_ms,note\n'+
  R.map(r=>`${fmtT(r.ts)},${r.down},${r.up},${r.ping},${r.jitter},"${(r.note||'').replace(/"/g,'')}"`).join('\n');
  let a=document.createElement('a');a.href=URL.createObjectURL(new Blob([s],{type:'text/csv'}));
  a.download='speedtest.csv';a.click();}
async function clr(){if(!confirm('Очистити всю історію?'))return;
  await fetch('/api/results',{method:'DELETE'});load();}
window.onresize=draw;load();
</script></body></html>"""

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def _h(self, code=200, ctype="text/html; charset=utf-8", extra=None, length=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        if length is not None: self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        if extra:
            for k, v in extra.items(): self.send_header(k, v)
        self.end_headers()
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = urllib.parse.parse_qs(u.query)
        if u.path == "/":
            b = INDEX_HTML.encode(); self._h(length=len(b)); self.wfile.write(b)
        elif u.path == "/reports":
            b = REPORTS_HTML.encode(); self._h(length=len(b)); self.wfile.write(b)
        elif u.path == "/ping":
            self._h(ctype="text/plain", length=2); self.wfile.write(b"ok")
        elif u.path == "/garbage":
            n = min(int(q.get("n", ["104857600"])[0]), 2*1024*1024*1024)
            self._h(ctype="application/octet-stream", length=n)
            try:
                sent = 0
                while sent < n:
                    w = min(len(CHUNK), n - sent)
                    self.wfile.write(CHUNK[:w]); sent += w
            except (BrokenPipeError, ConnectionResetError):
                pass
        elif u.path == "/api/results":
            b = json.dumps(load_results()).encode()
            self._h(ctype="application/json", length=len(b)); self.wfile.write(b)
        else:
            self._h(404, length=0)
    def do_DELETE(self):
        if urllib.parse.urlparse(self.path).path == "/api/results":
            with _lock:
                try: os.replace(RESULTS, RESULTS + ".bak")
                except Exception: pass
                with open(RESULTS, "w") as f: f.write("[]")
            self._h(ctype="text/plain", length=2); self.wfile.write(b"ok")
        else:
            self._h(404, length=0)
    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        clen = int(self.headers.get("Content-Length", 0))
        if u.path == "/empty":
            got = 0
            while got < clen:
                d = self.rfile.read(min(262144, clen - got))
                if not d: break
                got += len(d)
            self._h(ctype="text/plain", length=len(str(got))); self.wfile.write(str(got).encode())
        elif u.path == "/api/save":
            body = self.rfile.read(clen) if clen else b"{}"
            try: rec = json.loads(body or b"{}")
            except Exception: rec = {}
            rec = {"ts": int(time.time()),
                   "down": float(rec.get("down", 0)), "up": float(rec.get("up", 0)),
                   "ping": float(rec.get("ping", 0)), "jitter": float(rec.get("jitter", 0)),
                   "note": str(rec.get("note", ""))[:80]}
            cnt = save_result(rec)
            b = json.dumps({"ok": True, "count": cnt}).encode()
            self._h(ctype="application/json", length=len(b)); self.wfile.write(b)
        else:
            self._h(404, length=0)

class Srv(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

if __name__ == "__main__":
    os.makedirs(os.path.dirname(RESULTS), exist_ok=True)
    if not os.path.exists(RESULTS):
        with open(RESULTS, "w") as f: f.write("[]")
    print("W522A speedtest on :%d" % PORT, flush=True)
    Srv(("0.0.0.0", PORT), H).serve_forever()
