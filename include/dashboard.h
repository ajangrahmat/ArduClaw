#ifndef ARDUCLAW_DASHBOARD_H
#define ARDUCLAW_DASHBOARD_H

#include <Arduino.h>

#define _CSS \
"*{margin:0;padding:0;box-sizing:border-box}" \
"body{font-family:'Nunito','system-ui',-apple-system,sans-serif;background:#f5f5f7;color:#171717;-webkit-font-smoothing:antialiased}" \
".w{max-width:600px;margin:0 auto;min-height:100dvh;display:flex;flex-direction:column;background:#fff;box-shadow:0 2px 12px rgba(0,0,0,.05)}" \
".h{display:flex;align-items:center;justify-content:space-between;padding:12px 20px;border-bottom:1px solid rgba(207,207,207,.3);background:rgba(255,255,255,.7);-webkit-backdrop-filter:blur(12px);backdrop-filter:blur(12px);position:sticky;top:0;z-index:20}" \
".hl{display:flex;align-items:baseline;gap:4px 10px;flex-wrap:wrap}" \
".h h1{font-size:22px;font-weight:800;line-height:1.1}.h h1 span{color:#F31D1C}" \
".sb{font-size:10px;font-weight:500;color:#AEAEB2;letter-spacing:.03em}" \
".hm{display:none;width:36px;height:36px;align-items:center;justify-content:center;border:none;background:rgba(0,0,0,.04);border-radius:10px;font-size:20px;cursor:pointer;color:#171717;transition:background .15s;flex-shrink:0}" \
".hm:hover{background:rgba(0,0,0,.08)}" \
".n{display:flex;overflow-x:auto;gap:4px;-webkit-overflow-scrolling:touch;padding:10px 16px 14px;scrollbar-width:none;border-bottom:1px solid rgba(207,207,207,.3);background:rgba(255,255,255,.5)}" \
".n::-webkit-scrollbar{display:none}" \
".n a{flex-shrink:0;padding:8px 18px;font-weight:700;color:#6E6E73;font-size:13px;text-decoration:none;border-radius:20px;transition:all .2s;white-space:nowrap}" \
".n a.on{color:#fff;background:#F31D1C}" \
".n a:not(.on):hover{background:rgba(0,0,0,.04)}" \
".b{padding:20px;flex:1;display:flex;flex-direction:column}" \
".f{padding:16px;text-align:center;font-size:11px;color:#AEAEB2;border-top:1px solid rgba(207,207,207,.2);font-weight:500}" \
".cd{background:#fff;border:1px solid rgba(207,207,207,.4);border-radius:1.375rem;padding:24px;box-shadow:0 2px 6px rgba(0,0,0,.04)}" \
".fm{display:flex;flex-direction:column;gap:14px}" \
".fd{display:flex;flex-direction:column;gap:4px}" \
".fd label{font-size:10px;font-weight:700;color:#6E6E73;text-transform:uppercase;letter-spacing:.06em}" \
".fd input,.fd select{padding:9px 13px;background:#f8f8fb;border:1px solid rgba(0,0,0,.09);border-radius:11px;font-size:14px;font-family:'Nunito','system-ui',sans-serif;color:#171717;outline:none;transition:border-color .15s}" \
".fd input:focus,.fd select:focus{border-color:#F31D1C}" \
".bt{display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:10px 20px;border:none;border-radius:12px;cursor:pointer;font-weight:700;font-size:14px;font-family:'Nunito','system-ui',sans-serif;transition:all .15s;width:100%;text-align:center}" \
".bt:hover{opacity:.9}.bt:disabled{opacity:.35;cursor:not-allowed}" \
".bt.pr{background:#F31D1C;color:#fff}" \
".bt.sc{background:#f0f0f4;color:#171717;border:1px solid rgba(0,0,0,.09)}" \
".row{display:flex;align-items:center;justify-content:space-between;padding:12px 16px;background:#f8f8fb;border-radius:12px;margin-bottom:8px}" \
".row .l{color:#6E6E73;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.04em}" \
".row .v{font-weight:700;color:#171717;font-size:14px}" \
".bdg{display:inline-block;padding:3px 10px;border-radius:99px;font-size:11px;font-weight:700}" \
".bdg.g{background:#e6f7e6;color:#2d8a2d;border:1px solid #b7dfb8}" \
".bdg.y{background:#fff3cd;color:#856404;border:1px solid #fac775}" \
".bdg.r{background:#fcebeb;color:#a32d2d;border:1px solid #f09595}" \
".sk{display:inline-block;padding:4px 10px;margin:3px;background:#f0f0f5;border:1px solid rgba(0,0,0,.06);border-radius:8px;font-size:11px;font-weight:600}" \
".ch{flex:1;display:flex;flex-direction:column;min-height:0}" \
".ms{flex:1;overflow-y:auto;padding:16px;display:flex;flex-direction:column}" \
".ms::-webkit-scrollbar{width:4px}.ms::-webkit-scrollbar-thumb{background:#d4d4d4;border-radius:99px}" \
".mu{margin-bottom:10px;padding:10px 14px;border-radius:16px;max-width:88%;word-wrap:break-word;font-size:14px;line-height:1.45}" \
".mu.me{align-self:flex-end;background:#F31D1C;color:#fff;border-bottom-right-radius:4px}" \
".mu.bo{align-self:flex-start;background:#f0f0f5;color:#171717;border-bottom-left-radius:4px}" \
".mu.er{align-self:flex-start;background:#fcebeb;color:#a32d2d;font-size:13px;border-bottom-left-radius:4px}" \
".mu.sk{align-self:flex-start;background:#f0f7ff;color:#1a4d8f;font-size:12px;border-bottom-left-radius:4px}" \
".in{display:flex;gap:8px;padding:12px 16px;border-top:1px solid rgba(207,207,207,.3);background:#fafafa}" \
".in input{flex:1;padding:10px 14px;border:1px solid rgba(0,0,0,.09);border-radius:12px;font-size:15px;font-family:'Nunito','system-ui',sans-serif;outline:none;background:#fff;transition:border-color .15s}" \
".in input:focus{border-color:#F31D1C}" \
".in button{padding:10px 18px;background:#F31D1C;color:#fff;border:none;border-radius:12px;cursor:pointer;font-weight:700;font-size:14px;font-family:'Nunito','system-ui',sans-serif;transition:all .15s}" \
".in button:disabled{opacity:.35}.in button:hover:not(:disabled){background:#d0100f}" \
"@media(max-width:600px){.hm{display:flex}.n{display:none;flex-direction:column;padding:8px 16px 12px;gap:2px;overflow-x:visible}.n.s{display:flex}.n a{padding:10px 16px;border-radius:12px;font-size:14px}}"

#define _H \
"</style></head><body><div class=\"w\">" \
"<div class=\"h\"><div class=\"hl\"><h1>Ardu<span>Claw</span></h1><span class=\"sb\">ArduMeka Claw AI Bot</span></div><span class=\"hm\" onclick=\"document.getElementById('nv').classList.toggle('s')\">☰</span></div>" \
"<div class=\"n\" id=\"nv\">"

#define _LKS \
"<a href=\"/\">Chat</a>" \
"<a href=\"/dashboard\">Dashboard</a>" \
"<a href=\"/wifi\">WiFi</a>" \
"<a href=\"/llm\">LLM</a>" \
"<a href=\"/mqtt\">MQTT</a>" \
"<a href=\"/skills\">Skills</a>"

#define _F "</div><div class=\"b\">"

// ── Chat page (/)
// Send POST to /api/chat with JSON {"prompt":"..."}
// Expect {"ok":true,"response":"...","skill_results":[...]}
// ─────────────────────────────────────────────────────────────────────────

static const char CHAT_HTML[] PROGMEM =
"<!DOCTYPE html><html lang=\"id\"><head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\">"
"<title>Chat - ArduClaw</title>"
"<style>" _CSS _H
"<a href=\"/\" class=\"on\">Chat</a>" _LKS _F
"<div class=\"ch\">"
"<div class=\"ms\" id=\"chatMsgs\"></div>"
"<div class=\"in\">"
"<input id=\"chatInput\" type=\"text\" placeholder=\"Ketik perintah...\" autocomplete=\"off\">"
"<button id=\"chatBtn\" onclick=\"chatSend()\">Kirim</button>"
"</div>"
"</div>"
"</div><div class=\"f\">ArduClaw v0.7</div></div>"
"<script>"
"var ci=document.getElementById('chatInput'),"
"    cb=document.getElementById('chatBtn'),"
"    cm=document.getElementById('chatMsgs'),"
"    sk='clawChat';"
"function chatSave(){"
  "var a=[];"
  "for(var i=0;i<cm.children.length;i++){"
    "var ch=cm.children[i];"
    "a.push({c:ch.className.replace('mu ',''),t:ch.textContent})"
  "}"
  "try{localStorage.setItem(sk,JSON.stringify(a))}catch(e){}"
"}"
"function chatLoad(){"
  "try{"
    "var r=JSON.parse(localStorage.getItem(sk));"
    "if(r&&r.length){"
      "for(var i=0;i<r.length;i++){"
        "var d=document.createElement('div');"
        "d.className='mu '+r[i].c;"
        "d.appendChild(document.createTextNode(r[i].t));"
        "cm.appendChild(d)"
      "}"
      "cm.scrollTop=cm.scrollHeight;"
      "return"
    "}"
  "}catch(e){}"
  "var w=document.createElement('div');"
  "w.className='mu bo';"
  "w.style.cssText='font-size:13px;color:#6E6E73;background:#f5f5f7;align-self:flex-start';"
  "w.innerHTML='<b style=\"color:#F31D1C\">ArduClaw</b> AI Agent siap. Kirim prompt untuk memulai.';"
  "cm.appendChild(w)"
"}"
"function chatAdd(c,t){"
  "var d=document.createElement('div');"
  "d.className='mu '+c;"
  "d.appendChild(document.createTextNode(t));"
  "cm.appendChild(d);"
  "cm.scrollTop=cm.scrollHeight;"
  "chatSave()"
"}"
"chatLoad();"
"function chatSend(){"
  "var t=ci.value.trim();"
  "if(!t)return;"
  "ci.value='';"
  "cb.disabled=true;"
  "chatAdd('me',t);"
  "var x=new XMLHttpRequest();"
  "x.open('POST','/api/chat',true);"
  "x.setRequestHeader('Content-Type','application/json');"
  "x.onload=function(){"
    "cb.disabled=false;"
    "ci.focus();"
    "try{"
      "var d=JSON.parse(x.responseText);"
      "if(d.ok){"
        "chatAdd('bo',d.response);"
        "if(d.skill_results&&d.skill_results.length){"
          "for(var i=0;i<d.skill_results.length;i++){"
            "var sr=d.skill_results[i];"
            "var txt=(sr.success?'OK: ':'GAGAL: ')+sr.tool+' - '+(sr.message||'');"
            "chatAdd('sk',txt)"
          "}"
        "}"
      "}else{"
        "chatAdd('er','Error: '+(d.error||d.msg||'unknown'))"
      "}"
    "}catch(e){"
      "chatAdd('er','Parse error: '+e.message)"
    "}"
  "};"
  "x.onerror=function(){"
    "cb.disabled=false;"
    "ci.focus();"
    "chatAdd('er','Network error - ESP tidak merespons')"
  "};"
  "x.send(JSON.stringify({prompt:t}))"
"}"
"ci.addEventListener('keypress',function(e){if(e.key==='Enter')chatSend()});"
"ci.focus();"
"</script></body></html>";

// ── Dashboard page ──────────────────────────────────────────────────────

static const char DASHBOARD_HTML[] PROGMEM =
"<!DOCTYPE html><html lang=\"id\"><head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Dashboard - ArduClaw</title>"
"<style>" _CSS _H
"<a href=\"/dashboard\" class=\"on\">Dashboard</a>" _LKS _F
"<div class=\"cd\" id=\"d\"><p style=\"color:#6E6E73;font-size:14px\">Memuat...</p></div>"
"</div><div class=\"f\">ArduClaw v0.7</div></div>"
"<script>"
"var r=function(l,v){return'<div class=\"row\"><div class=\"l\">'+l+'</div><div class=\"v\">'+v+'</div></div>'};"
"var x=new XMLHttpRequest();"
"x.open('GET','/api/status',true);"
"x.onload=function(){"
  "try{"
    "var d=JSON.parse(x.responseText);"
    "if(!d.ok){document.getElementById('d').innerHTML='<p style=\"color:#a32d2d;font-weight:700\">Gagal</p>';return}"
    "document.getElementById('d').innerHTML="
      "r('WiFi',d.connected?'<span class=\"bdg g\">Connected</span>':'<span class=\"bdg r\">Disconnected</span>')+"
      "r('SSID',d.ssid||'-')+"
      "r('IP',d.ip||'-')+"
      "r('Uptime',(d.uptime||0)+' s')+"
      "r('Heap',(d.heap||0)+' B')+"
      "r('Min Heap',(d.min_heap||0)+' B')+"
      "r('Skills',d.skill_count||0)"
  "}catch(e){document.getElementById('d').innerHTML='<p style=\"color:#a32d2d;font-weight:700\">Parse error</p>'}"
"};"
"x.send()"
"</script></body></html>";

// ── WiFi page ───────────────────────────────────────────────────────────

static const char WIFI_HTML[] PROGMEM =
"<!DOCTYPE html><html lang=\"id\"><head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>WiFi - ArduClaw</title>"
"<style>" _CSS _H
"<a href=\"/wifi\" class=\"on\">WiFi</a>" _LKS _F
"<div class=\"cd\">"
"<div class=\"fm\">"
"<div class=\"fd\"><label>SSID</label><input id=\"ws\" type=\"text\" placeholder=\"Nama WiFi\"></div>"
"<div class=\"fd\"><label>Password</label><input id=\"wp\" type=\"password\" placeholder=\"Password\"></div>"
"<button class=\"bt pr\" onclick=\"wifiSave()\" style=\"margin-top:4px\">Simpan &amp; Hubungkan</button>"
"<button class=\"bt sc\" onclick=\"wifiClear()\">Hapus &amp; Putuskan</button>"
"<p id=\"wr\" style=\"font-size:13px;color:#6E6E73;text-align:center;margin:0\"></p>"
"</div>"
"</div>"
"</div><div class=\"f\">ArduClaw v0.7</div></div>"
"<script>"
"var xw=new XMLHttpRequest();"
"xw.open('GET','/api/wifi/get',true);"
"xw.onload=function(){try{var d=JSON.parse(xw.responseText);if(d.ok&&d.ssid)document.getElementById('ws').value=d.ssid}catch(e){}};"
"xw.send();"
"function wifiSave(){"
  "var s=document.getElementById('ws').value.trim(),p=document.getElementById('wp').value;"
  "if(!s){alert('SSID wajib diisi');return}"
  "document.getElementById('wr').textContent='Menyimpan...';"
  "var x=new XMLHttpRequest();"
  "x.open('POST','/api/wifi/set',true);"
  "x.setRequestHeader('Content-Type','application/json');"
  "x.onload=function(){try{var d=JSON.parse(x.responseText);document.getElementById('wr').textContent=d.ok?'Tersimpan. Menghubungkan...':d.error||'Gagal'}catch(e){document.getElementById('wr').textContent='Error'}};"
  "x.send(JSON.stringify({ssid:s,pass:p}))"
"}"
"function wifiClear(){"
  "var x=new XMLHttpRequest();"
  "x.open('POST','/api/wifi/set',true);"
  "x.setRequestHeader('Content-Type','application/json');"
  "x.onload=function(){document.getElementById('ws').value='';document.getElementById('wp').value='';document.getElementById('wr').textContent='WiFi dihapus'};"
  "x.send(JSON.stringify({ssid:'',pass:''}))"
"}"
"</script></body></html>";

// ── LLM page ────────────────────────────────────────────────────────────

static const char LLM_HTML[] PROGMEM =
"<!DOCTYPE html><html lang=\"id\"><head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>LLM - ArduClaw</title>"
"<style>" _CSS _H
"<a href=\"/llm\" class=\"on\">LLM</a>" _LKS _F
"<div class=\"cd\">"
"<div class=\"fm\">"
"<div class=\"fd\"><label>API URL</label><input id=\"lu\" type=\"url\" placeholder=\"https://.../v1/chat/completions\"></div>"
"<div class=\"fd\"><label>API Key</label><input id=\"lk\" type=\"password\" placeholder=\"sk-...\"></div>"
"<div class=\"fd\"><label>Model</label><input id=\"lm\" type=\"text\" placeholder=\"gpt-4o-mini\"></div>"
"<button class=\"bt pr\" onclick=\"llmSave()\" style=\"margin-top:4px\">Simpan ke NVS</button>"
"<p id=\"lr\" style=\"font-size:13px;color:#6E6E73;text-align:center;margin:0\"></p>"
"</div>"
"</div>"
"</div><div class=\"f\">ArduClaw v0.7</div></div>"
"<script>"
"var xl=new XMLHttpRequest();"
"xl.open('GET','/api/llm/get',true);"
"xl.onload=function(){try{var d=JSON.parse(xl.responseText);if(d.ok){document.getElementById('lu').value=d.url||'';document.getElementById('lm').value=d.model||''}}catch(e){}};"
"xl.send();"
"function llmSave(){"
  "var u=document.getElementById('lu').value.trim(),k=document.getElementById('lk').value,m=document.getElementById('lm').value.trim();"
  "if(!u){alert('URL wajib diisi');return}"
  "document.getElementById('lr').textContent='Menyimpan...';"
  "var x=new XMLHttpRequest();"
  "x.open('POST','/api/llm/set',true);"
  "x.setRequestHeader('Content-Type','application/json');"
  "x.onload=function(){try{var d=JSON.parse(x.responseText);document.getElementById('lr').textContent=d.ok?'Tersimpan ke NVS':'Gagal';if(d.ok)document.getElementById('lk').value=''}catch(e){document.getElementById('lr').textContent='Error'}};"
  "x.send(JSON.stringify({url:u,key:k,model:m||undefined}))"
"}"
"</script></body></html>";

// ── MQTT page ───────────────────────────────────────────────────────────

static const char MQTT_HTML[] PROGMEM =
"<!DOCTYPE html><html lang=\"id\"><head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>MQTT - ArduClaw</title>"
"<style>" _CSS _H
"<a href=\"/mqtt\" class=\"on\">MQTT</a>" _LKS _F
"<div class=\"cd\">"
"<div class=\"fm\">"
"<div class=\"fd\"><label>Broker Host</label><input id=\"mh\" type=\"text\" placeholder=\"broker.example.com\"></div>"
"<div class=\"fd\"><label>Port</label><input id=\"mo\" type=\"number\" placeholder=\"1883\" value=\"1883\"></div>"
"<div class=\"fd\"><label>Username</label><input id=\"mu\" type=\"text\" placeholder=\"(opsional)\"></div>"
"<div class=\"fd\"><label>Password</label><input id=\"mp\" type=\"password\" placeholder=\"(opsional)\"></div>"
"<div class=\"fd\"><label>Client ID</label><input id=\"mc\" type=\"text\" placeholder=\"arduclaw-xxxxxx\"></div>"
"<button class=\"bt pr\" onclick=\"mqttSave()\" style=\"margin-top:4px\">Simpan ke NVS</button>"
"<p id=\"mr\" style=\"font-size:13px;color:#6E6E73;text-align:center;margin:0\"></p>"
"</div>"
"</div>"
"</div><div class=\"f\">ArduClaw v0.7</div></div>"
"<script>"
"var xm=new XMLHttpRequest();"
"xm.open('GET','/api/mqtt/get',true);"
"xm.onload=function(){try{var d=JSON.parse(xm.responseText);if(d.ok){document.getElementById('mh').value=d.host||'';document.getElementById('mo').value=d.port||1883;document.getElementById('mu').value=d.user||'';document.getElementById('mc').value=d.client_id||'arduclaw-xxxxxx'}}catch(e){}};"
"xm.send();"
"function mqttSave(){"
  "var h=document.getElementById('mh').value.trim(),o=parseInt(document.getElementById('mo').value)||1883,u=document.getElementById('mu').value,p=document.getElementById('mp').value,c=document.getElementById('mc').value||'arduclaw-xxxxxx';"
  "document.getElementById('mr').textContent='Menyimpan...';"
  "var x=new XMLHttpRequest();"
  "x.open('POST','/api/mqtt/set',true);"
  "x.setRequestHeader('Content-Type','application/json');"
  "x.onload=function(){try{var d=JSON.parse(x.responseText);document.getElementById('mr').textContent=d.ok?'Tersimpan':'Gagal';if(d.ok)document.getElementById('mp').value=''}catch(e){document.getElementById('mr').textContent='Error'}};"
  "x.send(JSON.stringify({host:h,port:o,user:u,pass:p,client_id:c}))"
"}"
"</script></body></html>";

// ── Skills page ─────────────────────────────────────────────────────────

static const char SKILLS_HTML[] PROGMEM =
"<!DOCTYPE html><html lang=\"id\"><head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Skills - ArduClaw</title>"
"<style>" _CSS _H
"<a href=\"/skills\" class=\"on\">Skills</a>" _LKS _F
"<div class=\"cd\" id=\"sl\"><p style=\"color:#6E6E73;font-size:14px\">Memuat...</p></div>"
"</div><div class=\"f\">ArduClaw v0.7</div></div>"
"<script>"
"var xs=new XMLHttpRequest();"
"xs.open('GET','/api/skills',true);"
"xs.onload=function(){"
  "try{"
    "var d=JSON.parse(xs.responseText);"
    "var e=document.getElementById('sl');"
    "if(!d.ok||!d.skills){e.innerHTML='<p style=\"color:#a32d2d;font-weight:700\">Gagal memuat</p>';return}"
    "var a=d.skills.split(',').map(function(x){return x.trim()}).filter(function(x){return x});"
    "if(!a.length){e.innerHTML='<p style=\"color:#6E6E73\">Tidak ada skill</p>';return}"
    "var h='<p style=\"font-size:11px;font-weight:700;color:#6E6E73;text-transform:uppercase;letter-spacing:.06em;margin-bottom:10px\">'+a.length+' skill terdaftar</p>';"
    "for(var i=0;i<a.length;i++){h+='<span class=\"sk\">'+a[i]+'</span>'}"
    "e.innerHTML=h"
  "}catch(e){document.getElementById('sl').innerHTML='<p style=\"color:#a32d2d;font-weight:700\">Parse error</p>'}"
"};"
"xs.send()"
"</script></body></html>";

#endif
