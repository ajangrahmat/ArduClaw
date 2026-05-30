#ifndef ARDUCLAW_DASHBOARD_H
#define ARDUCLAW_DASHBOARD_H

#include <Arduino.h>

#define _CSS \
"*{margin:0;padding:0;box-sizing:border-box}" \
"body{font-family:'Inter','system-ui',-apple-system,sans-serif;background:#f0f0f3;color:#111;-webkit-font-smoothing:antialiased;font-size:15px;line-height:1.5}" \
".w{width:100%;max-width:1100px;margin:0 auto;height:100dvh;display:flex;flex-direction:column;background:#fff;box-shadow:0 0 0 1px rgba(0,0,0,.06)}" \
".app-body{flex:1;display:flex;overflow:hidden}" \
".main-col{flex:1;display:flex;flex-direction:column;min-width:0;overflow-y:auto}" \
".h{display:flex;align-items:center;justify-content:space-between;padding:0 24px;height:56px;border-bottom:1px solid rgba(0,0,0,.07);background:#fff;position:sticky;top:0;z-index:20;flex-shrink:0}" \
".hl{display:flex;align-items:baseline;gap:6px}" \
".h h1{font-size:20px;font-weight:700;letter-spacing:-.02em;line-height:1}.h h1 span{color:#D92B2B}" \
".sb{font-size:11px;font-weight:500;color:#999;letter-spacing:.01em}" \
".hm{display:none;width:34px;height:34px;align-items:center;justify-content:center;border:1px solid rgba(0,0,0,.08);background:transparent;border-radius:8px;font-size:18px;cursor:pointer;color:#444;transition:background .12s;flex-shrink:0}" \
".hm:hover{background:#f5f5f5}" \
".n{width:200px;flex-shrink:0;display:flex;flex-direction:column;padding:16px 0;border-right:1px solid rgba(0,0,0,.07);background:#fafafa;overflow-y:auto;position:sticky;top:0;height:100%;align-self:flex-start}" \
".n::-webkit-scrollbar{width:3px}.n::-webkit-scrollbar-thumb{background:#d0d0d0;border-radius:99px}" \
".n a{display:block;padding:9px 20px;font-weight:600;color:#666;font-size:13px;text-decoration:none;transition:all .15s;border-left:2px solid transparent;letter-spacing:.01em}" \
".n a.on{color:#D92B2B;background:rgba(217,43,43,.06);border-left-color:#D92B2B}" \
".n a:not(.on):hover{color:#111;background:rgba(0,0,0,.04);border-left-color:rgba(0,0,0,.12)}" \
".b{padding:24px;flex:1}" \
".f{padding:14px 24px;text-align:center;font-size:11px;color:#bbb;border-top:1px solid rgba(0,0,0,.06);font-weight:500;letter-spacing:.02em;flex-shrink:0}" \
".cd{background:#fff;border:1px solid rgba(0,0,0,.08);border-radius:14px;padding:24px;}" \
".fm{display:flex;flex-direction:column;gap:16px}" \
".fd{display:flex;flex-direction:column;gap:5px}" \
".fd label{font-size:11px;font-weight:600;color:#888;text-transform:uppercase;letter-spacing:.07em}" \
".fd input,.fd select{padding:9px 12px;background:#f8f8f9;border:1px solid rgba(0,0,0,.1);border-radius:9px;font-size:14px;font-family:'Inter','system-ui',sans-serif;color:#111;outline:none;transition:border-color .15s,box-shadow .15s}" \
".fd input:focus,.fd select:focus{border-color:#D92B2B;box-shadow:0 0 0 3px rgba(217,43,43,.1)}" \
".bt{display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:10px 18px;border:none;border-radius:10px;cursor:pointer;font-weight:600;font-size:14px;font-family:'Inter','system-ui',sans-serif;transition:all .15s;width:100%;text-align:center;letter-spacing:.01em}" \
".bt:hover{opacity:.88;transform:translateY(-1px)}.bt:active{transform:translateY(0)}.bt:disabled{opacity:.3;cursor:not-allowed;transform:none}" \
".bt.pr{background:#D92B2B;color:#fff}" \
".bt.sc{background:#f4f4f6;color:#333;border:1px solid rgba(0,0,0,.08)}" \
".row{display:flex;align-items:center;justify-content:space-between;padding:11px 14px;background:#f8f8f9;border-radius:10px;margin-bottom:7px;border:1px solid rgba(0,0,0,.05)}" \
".row .l{color:#888;font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.06em}" \
".row .v{font-weight:600;color:#111;font-size:13px}" \
".bdg{display:inline-block;padding:3px 10px;border-radius:99px;font-size:11px;font-weight:600;letter-spacing:.02em}" \
".bdg.g{background:#e8f7e8;color:#257a25;border:1px solid #b8dcb8}" \
".bdg.y{background:#fef8e7;color:#7a6000;border:1px solid #f5d57a}" \
".bdg.r{background:#fdeaea;color:#a02222;border:1px solid #f0a0a0}" \
".sk{display:inline-block;padding:4px 11px;margin:3px;background:#f3f3f6;border:1px solid rgba(0,0,0,.07);border-radius:7px;font-size:12px;font-weight:600;color:#444}" \
".b{flex:1;display:flex;flex-direction:column;min-height:0}.ch{flex:1;display:flex;flex-direction:column;min-height:0;padding:0 0 8px 0}" \
".ms{flex:1;overflow-y:auto;padding:20px 16px;display:flex;flex-direction:column;gap:2px;min-height:0}" \
".ms::-webkit-scrollbar{width:3px}.ms::-webkit-scrollbar-thumb{background:#d0d0d0;border-radius:99px}" \
".mu{margin-bottom:4px;padding:10px 14px;border-radius:14px;max-width:86%;word-wrap:break-word;font-size:14px;line-height:1.5}" \
".mu.me{align-self:flex-end;background:#D92B2B;color:#fff;border-bottom-right-radius:4px}" \
".mu.bo{align-self:flex-start;background:#f2f2f4;color:#111;border-bottom-left-radius:4px}" \
".mu.er{align-self:flex-start;background:#fdeaea;color:#a02222;font-size:13px;border-bottom-left-radius:4px;border:1px solid rgba(217,43,43,.15)}" \
".mu.sk{align-self:flex-start;background:#edf4ff;color:#1a4a90;font-size:12px;border-bottom-left-radius:4px;border:1px solid rgba(26,74,144,.15)}" \
".in{display:flex;gap:8px;padding:12px 16px;background:#fafafa;flex-shrink:0;border-radius:14px;margin:0 8px 0}" \
".in input{flex:1;padding:10px 14px;border:1px solid rgba(0,0,0,.1);border-radius:10px;font-size:14px;font-family:'Inter','system-ui',sans-serif;outline:none;background:#fff;transition:border-color .15s,box-shadow .15s}" \
".in input:focus{border-color:#D92B2B;box-shadow:0 0 0 3px rgba(217,43,43,.1)}" \
".in button{padding:10px 18px;background:#D92B2B;color:#fff;border:none;border-radius:10px;cursor:pointer;font-weight:600;font-size:14px;font-family:'Inter','system-ui',sans-serif;transition:all .15s;letter-spacing:.01em}" \
".in button:disabled{opacity:.3}.in button:hover:not(:disabled){background:#b82424;transform:translateY(-1px)}" \
"@media(max-width:768px){.hm{display:flex}.n{position:fixed;top:0;left:-270px;width:248px;height:100dvh;z-index:100;background:#fff;box-shadow:4px 0 20px rgba(0,0,0,.08);transition:left .25s cubic-bezier(.4,0,.2,1);padding:56px 0 16px;display:flex;flex-direction:column;overflow-y:auto;border-right:none}.n.s{left:0}.n a{padding:11px 20px;font-size:14px}}" \
".tb{width:34px;height:34px;display:flex;align-items:center;justify-content:center;border:1px solid rgba(0,0,0,.08);background:transparent;border-radius:8px;cursor:pointer;font-size:17px;transition:background .12s;flex-shrink:0}" \
".tb:hover{background:#f5f5f5}" \
"html.d body{background:#151515!important;color:#e2e2e2!important}" \
"html.d .w{background:#1c1c1c!important;box-shadow:0 0 0 1px rgba(255,255,255,.06)!important}" \
"html.d .h{background:#1c1c1c!important;border-color:rgba(255,255,255,.07)!important}" \
"html.d .hm,html.d .tb{background:transparent!important;border-color:rgba(255,255,255,.1)!important;color:#ccc!important}" \
"html.d .hm:hover,html.d .tb:hover{background:rgba(255,255,255,.06)!important}" \
"html.d .h h1 span{color:#e04040!important}" \
"html.d .n{background:#181818!important;border-color:rgba(255,255,255,.07)!important}" \
"html.d .n a{color:#888!important}" \
"html.d .n a.on{color:#e04040!important;background:rgba(224,64,64,.08)!important;border-left-color:#e04040!important}" \
"html.d .n a:not(.on):hover{color:#e2e2e2!important;background:rgba(255,255,255,.05)!important;border-left-color:rgba(255,255,255,.1)!important}" \
"html.d .bt.pr{background:#c93030!important}" \
"html.d .bt.pr:hover:not(:disabled){background:#b82828!important}" \
"html.d .mu.me{background:#c93030!important}" \
"html.d .in button{background:#c93030!important}" \
"html.d .in button:hover:not(:disabled){background:#b02828!important}" \
"html.d .cd{background:#222!important;border-color:rgba(255,255,255,.08)!important}" \
"html.d .fd input,html.d .fd select{background:#191919!important;border-color:rgba(255,255,255,.1)!important;color:#e2e2e2!important}" \
"html.d .fd input:focus,html.d .fd select:focus{border-color:#e04040!important;box-shadow:0 0 0 3px rgba(224,64,64,.15)!important}" \
"html.d .bt.sc{background:#2a2a2a!important;color:#ccc!important;border-color:rgba(255,255,255,.1)!important}" \
"html.d .row{background:#191919!important;border-color:rgba(255,255,255,.06)!important}" \
"html.d .row .v{color:#e2e2e2!important}" \
"html.d .in{background:#181818!important;border-color:rgba(255,255,255,.07)!important}" \
"html.d .in input{background:#222!important;border-color:rgba(255,255,255,.1)!important;color:#e2e2e2!important}" \
"html.d .in input:focus{border-color:#e04040!important;box-shadow:0 0 0 3px rgba(224,64,64,.15)!important}" \
"html.d .mu.bo{background:#2a2a2a!important;color:#e2e2e2!important}" \
"html.d .mu.er{background:#2e1a1a!important;color:#f08080!important;border-color:rgba(240,128,128,.15)!important}" \
"html.d .mu.sk{background:#182030!important;color:#7ab4f8!important;border-color:rgba(122,180,248,.15)!important}" \
"html.d .sk{background:#252525!important;border-color:rgba(255,255,255,.08)!important;color:#aaa!important}" \
"html.d .fafafa,.fafafa html.d{background:#181818!important}" \
"html.d .f{border-color:rgba(255,255,255,.06)!important;color:#555!important}"

#define _H \
"</style><script>var t=localStorage.getItem('t');if(t==='l')document.documentElement.className='';else if(t==='d'||matchMedia('(prefers-color-scheme:dark)').matches)document.documentElement.className='d';else document.documentElement.className='';function tt(){var h=document.documentElement,b=document.querySelector('.tb');if(h.className==='d'){h.className='';localStorage.setItem('t','l');b.textContent='🌙'}else{h.className='d';localStorage.setItem('t','d');b.textContent='☀️'}}if(document.documentElement.className==='d')document.querySelector('.tb').textContent='☀️'</script>" \
"</head><body><div class=\"w\">" \
"<div class=\"h\"><div class=\"hl\"><h1>Ardu<span>Claw</span></h1><span class=\"sb\">ArduMeka Claw AI Bot</span></div><div style=\"display:flex;align-items:center;gap:6px;flex-shrink:0\"><button class=\"tb\" onclick=\"tt()\">🌙</button><span class=\"hm\" onclick=\"document.getElementById('nv').classList.toggle('s')\">☰</span></div></div>" \
"<div class=\"app-body\"><div class=\"n\" id=\"nv\">"

#define _LKS \
"<a href=\"/\">Chat</a>" \
"<a href=\"/dashboard\">Dashboard</a>" \
"<a href=\"/wifi\">WiFi</a>" \
"<a href=\"/llm\">LLM</a>" \
"<a href=\"/mqtt\">MQTT</a>" \
"<a href=\"/skills\">Skills</a>"

#define _F "</div><script>var a=document.querySelectorAll('.n a'),p=location.pathname;for(var i=0;i<a.length;i++){if(a[i].getAttribute('href')===p){a[i].className='on';break}}</script><div class=\"main-col\"><div class=\"b\">"

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
_LKS _F
"<div class=\"ch\">"
"<div class=\"ms\" id=\"chatMsgs\"></div>"
"<div class=\"in\">"
"<input id=\"chatInput\" type=\"text\" placeholder=\"Ketik perintah...\" autocomplete=\"off\">"
"<button class=\"tb\" onclick=\"chatClear()\" title=\"Hapus chat\">✕</button>"
"<button id=\"chatBtn\" onclick=\"chatSend()\">Kirim</button>"
"</div>"
"</div>"
"</div></div></div><div class=\"f\">ArduClaw v0.7</div></div>"
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
  "w.style.cssText='font-size:13px;color:#888;background:#f8f8f9;align-self:flex-start';"
  "w.innerHTML='<b style=\"color:#D92B2B\">ArduClaw</b> AI Agent siap. Kirim prompt untuk memulai.';"
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
"function chatClear(){"
  "if(!confirm('Hapus semua chat?'))return;"
  "cm.innerHTML='';"
  "try{localStorage.removeItem(sk)}catch(e){}"
  "chatLoad()"
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
_LKS _F
"<div class=\"cd\" id=\"d\"><p style=\"color:#999;font-size:14px\">Memuat...</p></div>"
"</div></div></div><div class=\"f\">ArduClaw v0.7</div></div>"
"<script>"
"var r=function(l,v){return'<div class=\"row\"><div class=\"l\">'+l+'</div><div class=\"v\">'+v+'</div></div>'};"
"var b=function(c,t){return'<span class=\"bdg '+c+'\">'+t+'</span>'};"
"var e=document.getElementById('d');"
"e.innerHTML='';"
"loadStatus();"
"function loadStatus(){"
  "var x=new XMLHttpRequest();"
  "x.open('GET','/api/status',true);"
  "x.onload=function(){"
    "try{"
      "var d=JSON.parse(x.responseText);"
      "if(!d.ok){e.innerHTML='<p style=\"color:#a02222;font-weight:600\">Gagal memuat status</p>';loadLlm();return}"
      "var h='<p style=\"font-size:11px;font-weight:600;color:#999;text-transform:uppercase;letter-spacing:.07em;margin-bottom:12px\">WiFi &amp; Sistem</p>';"
      "h+=r('WiFi',d.connected?b('g','Connected'):b('r','Disconnected'));"
      "h+=r('SSID',d.ssid||'-');"
      "h+=r('IP',d.ip||'-');"
      "h+=r('Uptime',(d.uptime||0)+' s');"
      "h+=r('Heap',(d.heap||0)+' B');"
      "h+=r('Min Heap',(d.min_heap||0)+' B');"
      "h+=r('Total Skills',d.skill_count||0);"
      "e.innerHTML=h"
    "}catch(e){}"
    "loadLlm()"
  "};"
  "x.send()"
"}"
"function loadLlm(){"
  "var x=new XMLHttpRequest();"
  "x.open('GET','/api/llm/get',true);"
  "x.onload=function(){"
    "try{"
      "var d=JSON.parse(x.responseText);"
      "if(d.ok){"
        "var h=e.innerHTML;"
        "h+='<p style=\"font-size:11px;font-weight:600;color:#999;text-transform:uppercase;letter-spacing:.07em;margin:18px 0 12px\">LLM</p>';"
        "h+=r('Status',d.url?b('g','Terkonfigurasi'):b('y','Belum diatur'));"
        "if(d.url){"
          "h+=r('URL','<span style=\"word-break:break-all;font-size:12px\">'+d.url+'</span>');"
          "h+=r('Model',d.model||'-');"
          "h+=r('API Key',d.has_key?'<span style=\"color:#257a25\">Tersimpan</span>':'<span style=\"color:#a02222\">Belum</span>')"
        "}"
        "e.innerHTML=h"
      "}"
    "}catch(e){}"
    "loadMqtt()"
  "};"
  "x.send()"
"}"
"function loadMqtt(){"
  "var x=new XMLHttpRequest();"
  "x.open('GET','/api/mqtt/get',true);"
  "x.onload=function(){"
    "try{"
      "var d=JSON.parse(x.responseText);"
      "if(d.ok){"
        "var h=e.innerHTML;"
        "h+='<p style=\"font-size:11px;font-weight:600;color:#999;text-transform:uppercase;letter-spacing:.07em;margin:18px 0 12px\">MQTT</p>';"
        "h+=r('Status',d.host?b('g','Terkonfigurasi'):b('y','Belum diatur'));"
        "if(d.host){"
          "h+=r('Broker',d.host);"
          "h+=r('Port',d.port||1883);"
          "h+=r('Client ID',d.client_id||'-')"
        "}"
        "e.innerHTML=h"
      "}"
    "}catch(e){}"
    "loadSubs()"
  "};"
  "x.send()"
"}"
"function loadSubs(){"
  "var x=new XMLHttpRequest();"
  "x.open('GET','/api/mqtt/subs',true);"
  "x.onload=function(){"
    "try{"
      "var d=JSON.parse(x.responseText);"
      "if(d.ok&&d.count){"
        "var h=e.innerHTML;"
        "h+=r('Subscriptions',d.count);"
        "e.innerHTML=h"
      "}"
    "}catch(e){}"
    "loadSkills()"
  "};"
  "x.send()"
"}"
"function loadSkills(){"
  "var x=new XMLHttpRequest();"
  "x.open('GET','/api/skills',true);"
  "x.onload=function(){"
    "try{"
      "var d=JSON.parse(x.responseText);"
      "if(d.ok&&d.skills){"
        "var a=d.skills.split(',').map(function(x){return x.trim()}).filter(function(x){return x});"
        "if(a.length){"
          "var h=e.innerHTML;"
          "h+='<p style=\"font-size:11px;font-weight:600;color:#999;text-transform:uppercase;letter-spacing:.07em;margin:18px 0 12px\">Skill ('+a.length+')</p>';"
          "for(var i=0;i<a.length;i++){h+='<span class=\"sk\">'+a[i]+'</span>'}"
          "e.innerHTML=h"
        "}"
      "}"
    "}catch(e){}"
    "loadPersist()"
  "};"
  "x.send()"
"}"
"function fmtArgs(a){"
  "if(!a)return'';"
  "if(a.action=='gpio.write'){"
    "var v=a.args&&a.args.value?parseInt(a.args.value):(a.args&&a.args.hasOwnProperty('value')?parseInt(a.args.value):1);"
    "return'→ tulis '+(v?'HIGH':'LOW')+' ke P'+(a.args&&a.args.pin?a.args.pin:'?')"
  "}"
  "if(a.action=='gpio.pulse'){"
    "return'→ pulse P'+(a.args&&a.args.pin?a.args.pin:'?')+' '+(a.args&&a.args.ms?a.args.ms:'200')+'ms'"
  "}"
  "if(a.action=='gpio.toggle'){"
    "return'→ toggle P'+(a.args&&a.args.pin?a.args.pin:'?')"
  "}"
  "return'→ '+a.action+(a.args?' '+JSON.stringify(a.args):'')"
"}"
"function loadPersist(){"
  "var x=new XMLHttpRequest();"
  "x.open('GET','/api/persist',true);"
  "x.onload=function(){"
    "try{"
      "var d=JSON.parse(x.responseText);"
      "if(!d.ok)return;"
      "var h=e.innerHTML;"
      "h+='<p style=\"font-size:11px;font-weight:600;color:#999;text-transform:uppercase;letter-spacing:.07em;margin:18px 0 12px\">Logika Aktif</p>';"
      "h+=r('NVS Config',d.has_config?'<span style=\"color:#257a25\">Tersimpan</span>':'<span style=\"color:#999\">Kosong</span>');"
      "if(d.gpio_modes){var gm=d.gpio_modes;var ks=Object.keys(gm);if(ks.length){for(var i=0;i<ks.length;i++){h+=r('GPIO P'+ks[i],'mode='+gm[ks[i]])}}}"
      "if(d.gpio_outputs){var go=d.gpio_outputs;var ks=Object.keys(go);if(ks.length){for(var i=0;i<ks.length;i++){h+=r('GPIO P'+ks[i],go[ks[i]]==1?'HIGH':'LOW')}}}"
      "if(d.blinks&&d.blinks.length){for(var i=0;i<d.blinks.length;i++){var b=d.blinks[i];h+=r('LED P'+b.pin,'toggle '+b.on_ms+'/'+b.off_ms+'ms')}}"
      "if(d.monitors&&d.monitors.length){for(var i=0;i<d.monitors.length;i++){var m=d.monitors[i];h+=r('Monitor P'+m.pin,'Jika '+m.trigger+' '+fmtArgs(m)+(m.once?' (1×)':''))}}"
      "if(d.subs&&d.subs.length){for(var i=0;i<d.subs.length;i++){var s=d.subs[i];var p=s.args&&s.args.pin?'P'+s.args.pin:'?';h+=r('MQTT '+p,s.topic+' → tulis {msg} ke '+p)}}"
      "if(d.pwms&&d.pwms.length){for(var i=0;i<d.pwms.length;i++){var p=d.pwms[i];h+=r('PWM P'+p.pin,p.duty+'/'+255+' ('+Math.round(p.duty/255*100)+'%) '+p.freq+'Hz')}}"
      "if(d.servos&&d.servos.length){for(var i=0;i<d.servos.length;i++){var s=d.servos[i];h+=r('Servo P'+s.pin,'terlampir')}}"
      "if(d.debounces&&d.debounces.length){for(var i=0;i<d.debounces.length;i++){var b=d.debounces[i];h+=r('Debounce P'+b.pin,b.ms+'ms → '+b.action)}}"
      "if(d.publish_on_change&&d.publish_on_change.length){for(var i=0;i<d.publish_on_change.length;i++){var p=d.publish_on_change[i];h+=r('AutoPub P'+p.pin,p.topic+' (HIGH='+p.high+' LOW='+p.low+')')}}"
      "if(d.files){var fk=Object.keys(d.files);if(fk.length){for(var i=0;i<fk.length;i++){h+=r('File',fk[i])}}}"
      "e.innerHTML=h"
    "}catch(e){}"
  "};"
  "x.send()"
"}"
"</script></body></html>";

// ── WiFi page ───────────────────────────────────────────────────────────

static const char WIFI_HTML[] PROGMEM =
"<!DOCTYPE html><html lang=\"id\"><head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>WiFi - ArduClaw</title>"
"<style>" _CSS _H
_LKS _F
"<div class=\"cd\">"
"<div class=\"fm\">"
"<div class=\"fd\"><label>SSID</label><input id=\"ws\" type=\"text\" placeholder=\"Nama WiFi\"></div>"
"<div class=\"fd\"><label>Password</label><input id=\"wp\" type=\"password\" placeholder=\"Password\"></div>"
"<button class=\"bt pr\" onclick=\"wifiSave()\" style=\"margin-top:4px\">Simpan &amp; Hubungkan</button>"
"<button class=\"bt sc\" onclick=\"wifiClear()\">Hapus &amp; Putuskan</button>"
"<p id=\"wr\" style=\"font-size:13px;color:#999;text-align:center;margin:0\"></p>"
"</div>"
"</div>"
"</div></div></div><div class=\"f\">ArduClaw v0.7</div></div>"
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
_LKS _F
"<div class=\"cd\">"
"<div class=\"fm\">"
"<div class=\"fd\"><label>API URL</label><input id=\"lu\" type=\"url\" placeholder=\"https://.../v1/chat/completions\"></div>"
"<div class=\"fd\"><label>API Key</label><input id=\"lk\" type=\"password\" placeholder=\"sk-...\"></div>"
"<div class=\"fd\"><label>Model</label><input id=\"lm\" type=\"text\" placeholder=\"gpt-4o-mini\"></div>"
"<button class=\"bt pr\" onclick=\"llmSave()\" style=\"margin-top:4px\">Simpan ke NVS</button>"
"<p id=\"lr\" style=\"font-size:13px;color:#999;text-align:center;margin:0\"></p>"
"</div>"
"</div>"
"</div></div></div><div class=\"f\">ArduClaw v0.7</div></div>"
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
_LKS _F
"<div class=\"cd\">"
"<div class=\"fm\">"
"<div class=\"fd\"><label>Broker Host</label><input id=\"mh\" type=\"text\" placeholder=\"broker.example.com\"></div>"
"<div class=\"fd\"><label>Port</label><input id=\"mo\" type=\"number\" placeholder=\"1883\" value=\"1883\"></div>"
"<div class=\"fd\"><label>Username</label><input id=\"mu\" type=\"text\" placeholder=\"(opsional)\"></div>"
"<div class=\"fd\"><label>Password</label><input id=\"mp\" type=\"password\" placeholder=\"(opsional)\"></div>"
"<div class=\"fd\"><label>Client ID</label><input id=\"mc\" type=\"text\" placeholder=\"arduclaw-xxxxxx\"></div>"
"<button class=\"bt pr\" onclick=\"mqttSave()\" style=\"margin-top:4px\">Simpan ke NVS</button>"
"<p id=\"mr\" style=\"font-size:13px;color:#999;text-align:center;margin:0\"></p>"
"</div>"
"</div>"
"</div></div></div><div class=\"f\">ArduClaw v0.7</div></div>"
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
_LKS _F
"<div class=\"cd\" id=\"sl\"><p style=\"color:#999;font-size:14px\">Memuat...</p></div>"
"</div></div></div><div class=\"f\">ArduClaw v0.7</div></div>"
"<script>"
"var xs=new XMLHttpRequest();"
"xs.open('GET','/api/skills',true);"
"xs.onload=function(){"
  "try{"
    "var d=JSON.parse(xs.responseText);"
    "var e=document.getElementById('sl');"
    "if(!d.ok||!d.skills){e.innerHTML='<p style=\"color:#a02222;font-weight:600\">Gagal memuat</p>';return}"
    "var a=d.skills.split(',').map(function(x){return x.trim()}).filter(function(x){return x});"
    "if(!a.length){e.innerHTML='<p style=\"color:#999\">Tidak ada skill</p>';return}"
    "var h='<p style=\"font-size:11px;font-weight:600;color:#999;text-transform:uppercase;letter-spacing:.07em;margin-bottom:12px\">'+a.length+' skill terdaftar</p>';"
    "for(var i=0;i<a.length;i++){h+='<span class=\"sk\">'+a[i]+'</span>'}"
    "e.innerHTML=h"
  "}catch(e){document.getElementById('sl').innerHTML='<p style=\"color:#a02222;font-weight:600\">Parse error</p>'}"
"};"
"xs.send()"
"</script></body></html>";

#endif