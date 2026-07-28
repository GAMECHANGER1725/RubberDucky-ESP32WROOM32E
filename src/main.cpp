/*
 * ESP32-WROOM-32E BLE Rubber Ducky
 * -----------------------------------------------------------------------------
 * The classic ESP32 has no native USB device peripheral, so this firmware turns
 * the board into a *Bluetooth LE* HID keyboard. Pair the board with a target
 * computer/phone, then push DuckyScript payloads to it from a web UI that the
 * board itself serves over its own WiFi access point.
 *
 * Web UI (http://192.168.4.1) has three tabs:
 *   - Scripts     : paste a DuckyScript and press Go, browse preset payloads,
 *                   and manage your own saved payloads.
 *   - Connection  : live BLE + WiFi status.
 *   - Settings    : change the WiFi SSID/password, the Bluetooth name, and the
 *                   UI theme (persisted to flash; network changes auto-reboot).
 *
 * For AUTHORIZED testing/education on hardware you own or have permission to test.
 * -----------------------------------------------------------------------------
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <BleKeyboard.h>

// ---------------------------------------------------------------------------
// Persistent settings (stored in NVS via Preferences)
// ---------------------------------------------------------------------------
Preferences prefs;

String cfgSsid;      // WiFi AP name the board hosts
String cfgPass;      // WiFi AP password (>= 8 chars)
String cfgBleName;   // Bluetooth pairing name
String cfgTheme;     // "dark" | "light"
String cfgAccent;    // CSS accent color, e.g. "#2563eb"

static void loadSettings() {
  prefs.begin("ducky", false);
  cfgSsid    = prefs.getString("ssid",    "DuckyESP32");
  cfgPass    = prefs.getString("pass",    "quack1234");
  cfgBleName = prefs.getString("blename", "ESP32 Keyboard");
  cfgTheme   = prefs.getString("theme",   "dark");
  cfgAccent  = prefs.getString("accent",  "#2563eb");
  prefs.end();
}

// LittleFS layout: saved payloads live at /pl_<name>.txt
static const char *PL_PREFIX = "/pl_";
static const char *PL_SUFFIX = ".txt";

BleKeyboard *bleKeyboard = nullptr;   // constructed after settings load
WebServer    server(80);

uint32_t defaultDelay = 0;   // DEFAULT_DELAY between commands (ms)
String   lastLine     = "";  // last executed line, for REPEAT

// ---------------------------------------------------------------------------
// Key name mapping
// ---------------------------------------------------------------------------
static uint8_t modifierKey(const String &tok) {
  String u = tok; u.toUpperCase();
  if (u == "CTRL" || u == "CONTROL")                     return KEY_LEFT_CTRL;
  if (u == "SHIFT")                                      return KEY_LEFT_SHIFT;
  if (u == "ALT")                                        return KEY_LEFT_ALT;
  if (u == "GUI" || u == "WINDOWS" || u == "WIN" ||
      u == "COMMAND" || u == "META")                     return KEY_LEFT_GUI;
  return 0;
}

static uint8_t namedKey(const String &tok) {
  String u = tok; u.toUpperCase();
  if (u == "ENTER" || u == "RETURN")   return KEY_RETURN;
  if (u == "ESC"   || u == "ESCAPE")   return KEY_ESC;
  if (u == "BACKSPACE" || u == "BKSP") return KEY_BACKSPACE;
  if (u == "TAB")                      return KEY_TAB;
  if (u == "SPACE")                    return ' ';
  if (u == "CAPSLOCK")                 return KEY_CAPS_LOCK;
  if (u == "DELETE" || u == "DEL")     return KEY_DELETE;
  if (u == "INSERT" || u == "INS")     return KEY_INSERT;
  if (u == "HOME")                     return KEY_HOME;
  if (u == "END")                      return KEY_END;
  if (u == "PAGEUP"   || u == "PGUP")  return KEY_PAGE_UP;
  if (u == "PAGEDOWN" || u == "PGDN")  return KEY_PAGE_DOWN;
  if (u == "UP"    || u == "UPARROW")    return KEY_UP_ARROW;
  if (u == "DOWN"  || u == "DOWNARROW")  return KEY_DOWN_ARROW;
  if (u == "LEFT"  || u == "LEFTARROW")  return KEY_LEFT_ARROW;
  if (u == "RIGHT" || u == "RIGHTARROW") return KEY_RIGHT_ARROW;
#ifdef KEY_MENU
  if (u == "MENU"  || u == "APP")      return KEY_MENU;
#endif
#ifdef KEY_PRTSC
  if (u == "PRINTSCREEN" || u == "PRTSCN") return KEY_PRTSC;
#endif
#ifdef KEY_PAUSE
  if (u == "PAUSE" || u == "BREAK")    return KEY_PAUSE;
#endif
#ifdef KEY_NUM_LOCK
  if (u == "NUMLOCK")                  return KEY_NUM_LOCK;
#endif
#ifdef KEY_SCROLL_LOCK
  if (u == "SCROLLLOCK")               return KEY_SCROLL_LOCK;
#endif
  if (u.length() >= 2 && u[0] == 'F') {           // F1..F12
    int n = u.substring(1).toInt();
    if (n >= 1 && n <= 12) return KEY_F1 + (n - 1);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// DuckyScript execution
// ---------------------------------------------------------------------------

// Handle a key/combo line, e.g. "GUI r", "CTRL ALT DELETE", "ENTER".
static void pressCombo(const String &line) {
  const int MAXT = 8;
  String tok[MAXT];
  int nt = 0;

  int start = 0;
  int len = line.length();
  while (start < len && nt < MAXT) {
    int sp = line.indexOf(' ', start);
    if (sp < 0) { tok[nt++] = line.substring(start); break; }
    if (sp > start) tok[nt++] = line.substring(start, sp);
    start = sp + 1;
  }
  if (nt == 0) return;

  for (int i = 0; i < nt - 1; i++) {
    uint8_t m = modifierKey(tok[i]);
    if (m) bleKeyboard->press(m);
  }

  String last = tok[nt - 1];
  uint8_t nk = namedKey(last);
  if (nk) {
    bleKeyboard->press(nk);
  } else if (last.length() == 1) {
    bleKeyboard->press((uint8_t)last[0]);
  } else {
    uint8_t m = modifierKey(last);
    if (m) bleKeyboard->press(m);
  }

  delay(8);
  bleKeyboard->releaseAll();
}

// Execute a single DuckyScript line.
static void execLine(const String &raw) {
  String line = raw;
  line.replace("\r", "");
  line.trim();
  if (line.length() == 0) return;

  int sp = line.indexOf(' ');
  String cmd  = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? ""   : line.substring(sp + 1);
  String CMD = cmd; CMD.toUpperCase();

  if (CMD == "REM") return;
  if (CMD == "DEFAULT_DELAY" || CMD == "DEFAULTDELAY") { defaultDelay = rest.toInt(); return; }
  if (CMD == "DELAY")     { delay(rest.toInt()); return; }
  if (CMD == "STRING")    { bleKeyboard->print(rest); return; }
  if (CMD == "STRINGLN")  { bleKeyboard->print(rest); bleKeyboard->write(KEY_RETURN); return; }
  if (CMD == "REPEAT") {
    int n = rest.toInt();
    for (int i = 0; i < n; i++) {
      execLine(lastLine);
      if (defaultDelay > 0) delay(defaultDelay);
    }
    return;
  }

  pressCombo(line);
}

// Run a full multi-line script.
static void runScript(const String &script) {
  if (!bleKeyboard->isConnected()) return;

  int i = 0;
  int n = script.length();
  while (i < n) {
    int nl = script.indexOf('\n', i);
    String line = (nl < 0) ? script.substring(i) : script.substring(i, nl);
    i = (nl < 0) ? n : nl + 1;

    execLine(line);

    String u = line; u.trim(); u.toUpperCase();
    if (!u.startsWith("REPEAT") && u.length() > 0 && !u.startsWith("REM"))
      lastLine = line;

    if (defaultDelay > 0) delay(defaultDelay);
  }
}

// ---------------------------------------------------------------------------
// LittleFS payload storage
// ---------------------------------------------------------------------------
static String sanitize(const String &name) {
  String out;
  for (size_t i = 0; i < name.length() && out.length() < 32; i++) {
    char c = name[i];
    if (isalnum(c) || c == '_' || c == '-') out += c;
    else out += '_';
  }
  if (out.length() == 0) out = "payload";
  return out;
}

static String pathFor(const String &name) {
  return String(PL_PREFIX) + sanitize(name) + PL_SUFFIX;
}

// ---------------------------------------------------------------------------
// Preset payload library (baked into firmware, served at /presets).
// Split by OS ("windows" / "mac"); the UI's OS toggle picks which set to show.
// Each script is an array of lines, joined with '\n' in the browser.
//
// Windows presets use the Run dialog (GUI r) + cmd/powershell.
// macOS presets use Spotlight (GUI SPACE) + Terminal commands (open/say/afplay),
// since macOS has no Run dialog and no PowerShell.
// ---------------------------------------------------------------------------
static const char PRESETS_JSON[] PROGMEM = R"PRESETS(
{
 "windows":[
  {"cat":"Demos","items":[
    {"name":"Hello Notepad","desc":"Opens Notepad and types a message",
     "lines":["DEFAULT_DELAY 40","DELAY 600","GUI r","DELAY 400","STRING notepad","ENTER","DELAY 900","STRING Hello from an ESP32 over BLE!","ENTER","STRING Keystroke injection demo."]}
  ]},
  {"cat":"YouTube & Media","items":[
    {"name":"Open a YouTube video","desc":"Launches a video in the default browser (edit the URL)",
     "lines":["DELAY 700","GUI r","DELAY 400","STRING https://www.youtube.com/watch?v=dQw4w9WgXcQ","ENTER"]},
    {"name":"Lofi hip hop radio","desc":"Opens the 24/7 lofi live stream",
     "lines":["DELAY 700","GUI r","DELAY 400","STRING https://www.youtube.com/watch?v=jfKfPfyJRdk","ENTER"]},
    {"name":"YouTube fullscreen","desc":"Opens a video, then presses 'f' to go fullscreen",
     "lines":["DELAY 700","GUI r","DELAY 400","STRING https://www.youtube.com/watch?v=dQw4w9WgXcQ","ENTER","DELAY 5000","STRING f"]}
  ]},
  {"cat":"Sounds","items":[
    {"name":"Beep melody","desc":"Plays notes through PowerShell console beeps",
     "lines":["DELAY 700","GUI r","DELAY 400","STRING powershell -c \"[console]::beep(523,250);[console]::beep(659,250);[console]::beep(784,250);[console]::beep(1046,400)\"","ENTER"]},
    {"name":"Text-to-speech","desc":"Makes the target speak a phrase",
     "lines":["DELAY 700","GUI r","DELAY 400","STRING powershell -c \"Add-Type -AssemblyName System.Speech;(New-Object System.Speech.Synthesis.SpeechSynthesizer).Speak('You have been ducked')\"","ENTER"]}
  ]},
  {"cat":"Pranks","items":[
    {"name":"Fake Windows Update","desc":"Opens fakeupdate.net fullscreen (harmless prank page)",
     "lines":["DELAY 800","GUI r","DELAY 400","STRING https://fakeupdate.net/win10ug/","ENTER","DELAY 3500","STRING f"]},
    {"name":"Rickroll","desc":"Opens the classic video in the browser",
     "lines":["DELAY 700","GUI r","DELAY 400","STRING https://www.youtube.com/watch?v=dQw4w9WgXcQ","ENTER"]},
    {"name":"Endless Notepad note","desc":"Opens Notepad and repeats a line 20 times",
     "lines":["DEFAULT_DELAY 30","DELAY 700","GUI r","DELAY 400","STRING notepad","ENTER","DELAY 900","STRING You have been ducked! ","REPEAT 20"]},
    {"name":"Caps Lock chaos","desc":"Toggles Caps Lock several times",
     "lines":["CAPSLOCK","DELAY 250","CAPSLOCK","DELAY 250","CAPSLOCK","DELAY 250","CAPSLOCK","DELAY 250","CAPSLOCK"]}
  ]},
  {"cat":"Utilities","items":[
    {"name":"Open Calculator","desc":"Launches calc.exe",
     "lines":["DELAY 700","GUI r","DELAY 400","STRING calc","ENTER"]},
    {"name":"Lock the workstation","desc":"Win+L locks the screen",
     "lines":["DELAY 500","GUI l"]},
    {"name":"Open a website","desc":"Opens a URL in the default browser (edit it)",
     "lines":["DELAY 700","GUI r","DELAY 400","STRING https://example.com","ENTER"]}
  ]},
  {"cat":"System","items":[
    {"name":"Open Task Manager","desc":"Ctrl+Shift+Esc",
     "lines":["DELAY 500","CTRL SHIFT ESC"]},
    {"name":"systeminfo","desc":"Prints system info in a command prompt",
     "lines":["DELAY 700","GUI r","DELAY 400","STRING cmd","ENTER","DELAY 700","STRING systeminfo","ENTER"]},
    {"name":"ipconfig","desc":"Shows the network configuration",
     "lines":["DELAY 700","GUI r","DELAY 400","STRING cmd","ENTER","DELAY 700","STRING ipconfig /all","ENTER"]}
  ]}
 ],
 "mac":[
  {"cat":"Demos","items":[
    {"name":"Hello TextEdit","desc":"Opens TextEdit via Spotlight and types a message",
     "lines":["DEFAULT_DELAY 40","DELAY 600","GUI SPACE","DELAY 400","STRING TextEdit","ENTER","DELAY 1400","STRING Hello from an ESP32 BLE keyboard!","ENTER","STRING Keystroke injection demo."]}
  ]},
  {"cat":"YouTube & Media","items":[
    {"name":"Open a YouTube video","desc":"Uses Terminal 'open' to launch the default browser (edit the URL)",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Terminal","ENTER","DELAY 900","STRING open \"https://www.youtube.com/watch?v=dQw4w9WgXcQ\"","ENTER"]},
    {"name":"Lofi hip hop radio","desc":"Opens the 24/7 lofi live stream",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Terminal","ENTER","DELAY 900","STRING open \"https://www.youtube.com/watch?v=jfKfPfyJRdk\"","ENTER"]},
    {"name":"YouTube fullscreen","desc":"Opens a video, then presses 'f' to go fullscreen",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Terminal","ENTER","DELAY 900","STRING open \"https://www.youtube.com/watch?v=dQw4w9WgXcQ\"","ENTER","DELAY 5000","STRING f"]}
  ]},
  {"cat":"Sounds","items":[
    {"name":"Text-to-speech","desc":"Uses the built-in 'say' command",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Terminal","ENTER","DELAY 900","STRING say \"you have been ducked\"","ENTER"]},
    {"name":"System beeps","desc":"Plays three system alert beeps via osascript",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Terminal","ENTER","DELAY 900","STRING osascript -e 'beep 3'","ENTER"]},
    {"name":"Play a sound","desc":"Plays a built-in macOS sound with afplay",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Terminal","ENTER","DELAY 900","STRING afplay /System/Library/Sounds/Glass.aiff","ENTER"]}
  ]},
  {"cat":"Pranks","items":[
    {"name":"Fake Update screen","desc":"Opens fakeupdate.net fullscreen (harmless prank page)",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Terminal","ENTER","DELAY 900","STRING open \"https://fakeupdate.net/mac/\"","ENTER","DELAY 3500","CTRL GUI f"]},
    {"name":"Rickroll","desc":"Opens the classic video in the browser",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Terminal","ENTER","DELAY 900","STRING open \"https://www.youtube.com/watch?v=dQw4w9WgXcQ\"","ENTER"]},
    {"name":"Endless TextEdit note","desc":"Opens TextEdit and repeats a line 20 times",
     "lines":["DEFAULT_DELAY 30","DELAY 700","GUI SPACE","DELAY 400","STRING TextEdit","ENTER","DELAY 1400","STRING You have been ducked! ","REPEAT 20"]},
    {"name":"Caps Lock chaos","desc":"Toggles Caps Lock several times",
     "lines":["CAPSLOCK","DELAY 250","CAPSLOCK","DELAY 250","CAPSLOCK","DELAY 250","CAPSLOCK","DELAY 250","CAPSLOCK"]}
  ]},
  {"cat":"Utilities","items":[
    {"name":"Open Calculator","desc":"Launches Calculator via Spotlight",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Calculator","ENTER"]},
    {"name":"Lock the screen","desc":"Cmd+Ctrl+Q locks macOS",
     "lines":["DELAY 500","CTRL GUI q"]},
    {"name":"Open a website","desc":"Opens a URL via Terminal (edit it)",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Terminal","ENTER","DELAY 900","STRING open \"https://example.com\"","ENTER"]}
  ]},
  {"cat":"System","items":[
    {"name":"Open Activity Monitor","desc":"macOS equivalent of Task Manager",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Activity Monitor","ENTER"]},
    {"name":"System info","desc":"Prints hardware info in Terminal",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Terminal","ENTER","DELAY 900","STRING system_profiler SPHardwareDataType","ENTER"]},
    {"name":"Network info","desc":"Shows network configuration with ifconfig",
     "lines":["DELAY 700","GUI SPACE","DELAY 400","STRING Terminal","ENTER","DELAY 900","STRING ifconfig","ENTER"]}
  ]}
 ]
}
)PRESETS";

// ---------------------------------------------------------------------------
// Web UI (single page, navbar with Scripts / Connection / Settings tabs)
// ---------------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 BLE Ducky</title>
<style>
 :root{--accent:#2563eb;--bg:#0f1216;--card:#161b22;--line:#242a32;--text:#e6e6e6;--muted:#8b98a5}
 [data-theme=light]{--bg:#f5f6f8;--card:#fff;--line:#e2e5e9;--text:#1b2129;--muted:#5b6470}
 *{box-sizing:border-box}
 body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--text);margin:0}
 header{position:sticky;top:0;background:var(--card);border-bottom:1px solid var(--line);padding:10px 16px}
 header h1{font-size:1rem;margin:0 0 8px}
 nav{display:flex;gap:6px}
 nav button{flex:1;background:transparent;color:var(--muted);border:1px solid var(--line);
   border-radius:8px;padding:8px;font-size:.85rem;cursor:pointer}
 nav button.active{background:var(--accent);color:#fff;border-color:var(--accent)}
 main{padding:16px;max-width:760px;margin:0 auto}
 .tab{display:none}.tab.show{display:block}
 textarea{width:100%;height:200px;background:var(--card);color:var(--text);border:1px solid var(--line);
   border-radius:8px;padding:10px;font-family:ui-monospace,Menlo,monospace;font-size:.85rem}
 input,select{background:var(--card);color:var(--text);border:1px solid var(--line);border-radius:8px;
   padding:9px;font-size:.85rem;width:100%}
 label{display:block;font-size:.78rem;color:var(--muted);margin:12px 0 4px}
 button.act{background:var(--accent);color:#fff;border:0;border-radius:8px;padding:10px 16px;
   font-size:.9rem;cursor:pointer;margin-top:10px}
 button.sec{background:var(--line);color:var(--text)}
 button.small{padding:5px 10px;font-size:.75rem;border:0;border-radius:6px;cursor:pointer;margin-left:6px}
 button.small.run{background:var(--accent);color:#fff}button.small.sec{background:var(--line);color:var(--text)}
 button.small.dan{background:#7f1d1d;color:#fff}
 .badge{display:inline-block;padding:2px 10px;border-radius:12px;font-size:.75rem;font-weight:600}
 .on{background:#12351f;color:#4ade80}.off{background:#3a1417;color:#f87171}
 .card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px;margin:10px 0}
 .catlabel{font-size:.72rem;letter-spacing:.06em;text-transform:uppercase;color:var(--muted);margin:18px 0 6px}
 .preset{display:flex;align-items:center;gap:8px;padding:8px 0;border-bottom:1px solid var(--line)}
 .preset:last-child{border-bottom:0}
 .preset .n{font-weight:600;font-size:.88rem}.preset .d{font-size:.75rem;color:var(--muted)}
 .preset .meta{flex:1;min-width:0}
 .row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
 .kv{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--line);font-size:.88rem}
 .kv:last-child{border-bottom:0}.kv .k{color:var(--muted)}
 .note{font-size:.75rem;color:var(--muted);margin-top:8px}
</style></head><body>
<header>
 <h1>ESP32 BLE Rubber Ducky</h1>
 <nav>
  <button data-tab="scripts" class="active">Scripts</button>
  <button data-tab="connection">Connection</button>
  <button data-tab="settings">Settings</button>
 </nav>
</header>
<main>

 <section id="scripts" class="tab show">
  <div class="card">
   <div class="row"><b style="font-size:.9rem">Run a script</b>
     <span id="badge" class="badge off" style="margin-left:auto">checking…</span></div>
   <textarea id="script" placeholder="REM Paste DuckyScript here&#10;DELAY 500&#10;GUI r&#10;STRING notepad&#10;ENTER"></textarea>
   <div class="row">
     <button class="act" onclick="go()">&#9654; Go</button>
     <input id="pname" placeholder="save as…" style="max-width:160px">
     <button class="act sec" onclick="save()">Save</button>
   </div>
   <div class="note">Keystrokes go to the paired Bluetooth target. Pair with the board first (see Connection).</div>
  </div>

  <div id="saved"></div>
  <div class="row" style="margin:14px 0 0">
    <span class="catlabel" style="margin:0 4px 0 0">Presets for</span>
    <button id="os_win" class="small run" onclick="setOS('windows')">Windows</button>
    <button id="os_mac" class="small sec" onclick="setOS('mac')">macOS</button>
  </div>
  <div id="presets"></div>
 </section>

 <section id="connection" class="tab">
  <div class="card">
   <div class="kv"><span class="k">BLE status</span><span id="c_ble" class="badge off">…</span></div>
   <div class="kv"><span class="k">Bluetooth name</span><span id="c_bt">…</span></div>
   <div class="kv"><span class="k">WiFi SSID</span><span id="c_ssid">…</span></div>
   <div class="kv"><span class="k">Web UI address</span><span id="c_ip">…</span></div>
   <div class="kv"><span class="k">WiFi clients</span><span id="c_cli">…</span></div>
  </div>
  <div class="note">To use the ducky: on the target device, open Bluetooth settings and pair with the
   Bluetooth name shown above. The badge turns green once it connects.</div>
 </section>

 <section id="settings" class="tab">
  <div class="card">
   <b style="font-size:.9rem">Network</b>
   <label>WiFi SSID</label><input id="s_ssid">
   <label>WiFi password (min 8 chars)</label><input id="s_pass">
   <label>Bluetooth name</label><input id="s_ble">
   <div class="note">Saving network changes reboots the board; you'll need to rejoin the WiFi and re-pair.</div>
   <button class="act" onclick="saveNet()">Save &amp; reboot</button>
  </div>
  <div class="card">
   <b style="font-size:.9rem">Appearance</b>
   <label>Theme</label>
   <select id="s_theme"><option value="dark">Dark</option><option value="light">Light</option></select>
   <label>Accent color</label><input id="s_accent" type="color" style="height:42px">
   <button class="act" onclick="saveUi()">Save appearance</button>
  </div>
 </section>

</main>
<script>
 let CFG={};
 function $(id){return document.getElementById(id);}
 // tabs
 document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{
   document.querySelectorAll('nav button').forEach(x=>x.classList.remove('active'));
   document.querySelectorAll('.tab').forEach(x=>x.classList.remove('show'));
   b.classList.add('active');$(b.dataset.tab).classList.add('show');
 });
 function applyTheme(){document.documentElement.dataset.theme=CFG.theme||'dark';
   document.documentElement.style.setProperty('--accent',CFG.accent||'#2563eb');}
 async function go(){await fetch('/run',{method:'POST',body:$('script').value});}
 async function save(){let n=$('pname').value||'payload';
   await fetch('/save?name='+encodeURIComponent(n),{method:'POST',body:$('script').value});loadSaved();}
 async function runScriptText(t){await fetch('/run',{method:'POST',body:t});}
 let PRESETS={};let OS=localStorage.getItem('os')||'windows';
 async function loadPresets(){PRESETS=await (await fetch('/presets')).json();renderPresets();}
 function setOS(os){OS=os;localStorage.setItem('os',os);renderPresets();}
 function renderPresets(){
   $('os_win').className='small '+(OS=='windows'?'run':'sec');
   $('os_mac').className='small '+(OS=='mac'?'run':'sec');
   let groups=PRESETS[OS]||[];let h='';
   groups.forEach(g=>{h+='<div class="catlabel">'+g.cat+'</div><div class="card">';
     g.items.forEach((p,i)=>{let s=p.lines.join('\n').replace(/"/g,'&quot;');
       h+='<div class="preset"><div class="meta"><div class="n">'+p.name+'</div><div class="d">'+p.desc+'</div></div>'+
          '<button class="small run" onclick="runScriptText(this.dataset.s)" data-s="'+s+'">Run</button>'+
          '<button class="small sec" onclick="$(\'script\').value=this.dataset.s" data-s="'+s+'">Load</button></div>';});
     h+='</div>';});
   $('presets').innerHTML=h;}
 async function loadSaved(){
   let arr=await (await fetch('/list')).json();
   if(!arr.length){$('saved').innerHTML='';return;}
   let h='<div class="catlabel">Your saved payloads</div><div class="card">';
   arr.forEach(n=>{h+='<div class="preset"><div class="meta"><div class="n">'+n+'</div></div>'+
     '<button class="small run" onclick="runFile(\''+n+'\')">Run</button>'+
     '<button class="small sec" onclick="editFile(\''+n+'\')">Edit</button>'+
     '<button class="small dan" onclick="delFile(\''+n+'\')">Del</button></div>';});
   h+='</div>';$('saved').innerHTML=h;}
 async function runFile(n){await fetch('/runfile?name='+encodeURIComponent(n),{method:'POST'});}
 async function editFile(n){$('pname').value=n;$('script').value=await (await fetch('/load?name='+encodeURIComponent(n))).text();
   document.querySelector('nav button[data-tab=scripts]').click();window.scrollTo(0,0);}
 async function delFile(n){await fetch('/delete?name='+encodeURIComponent(n),{method:'POST'});loadSaved();}
 async function poll(){try{let j=await (await fetch('/status')).json();
   let up=j.connected;
   let b=$('badge');b.textContent=up?'connected':'not connected';b.className='badge '+(up?'on':'off');
   let c=$('c_ble');c.textContent=up?'connected':'not connected';c.className='badge '+(up?'on':'off');
   $('c_bt').textContent=j.blename;$('c_ssid').textContent=j.ssid;$('c_ip').textContent='http://'+j.ip;
   $('c_cli').textContent=j.clients;}catch(e){}}
 async function loadCfg(){CFG=await (await fetch('/settings')).json();applyTheme();
   $('s_ssid').value=CFG.ssid;$('s_pass').value=CFG.pass;$('s_ble').value=CFG.blename;
   $('s_theme').value=CFG.theme;$('s_accent').value=CFG.accent;}
 async function saveNet(){let p=$('s_pass').value;
   if(p.length<8){alert('WiFi password must be at least 8 characters.');return;}
   let body='ssid='+encodeURIComponent($('s_ssid').value)+'&pass='+encodeURIComponent(p)+
            '&blename='+encodeURIComponent($('s_ble').value);
   await fetch('/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
   alert('Saved. The board is rebooting — rejoin the WiFi and re-pair Bluetooth.');}
 async function saveUi(){
   let body='theme='+encodeURIComponent($('s_theme').value)+'&accent='+encodeURIComponent($('s_accent').value);
   await fetch('/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
   CFG.theme=$('s_theme').value;CFG.accent=$('s_accent').value;applyTheme();}
 loadCfg();loadPresets();loadSaved();poll();setInterval(poll,1500);
</script></body></html>
)HTML";

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static void handleRoot()    { server.send_P(200, "text/html", INDEX_HTML); }
static void handlePresets() { server.send_P(200, "application/json", PRESETS_JSON); }

static void handleStatus() {
  String j = "{";
  j += "\"connected\":" + String(bleKeyboard->isConnected() ? "true" : "false");
  j += ",\"ssid\":\"" + cfgSsid + "\"";
  j += ",\"blename\":\"" + cfgBleName + "\"";
  j += ",\"ip\":\"" + WiFi.softAPIP().toString() + "\"";
  j += ",\"clients\":" + String(WiFi.softAPgetStationNum());
  j += "}";
  server.send(200, "application/json", j);
}

static void handleGetSettings() {
  String j = "{";
  j += "\"ssid\":\""    + cfgSsid    + "\",";
  j += "\"pass\":\""    + cfgPass    + "\",";
  j += "\"blename\":\"" + cfgBleName + "\",";
  j += "\"theme\":\""   + cfgTheme   + "\",";
  j += "\"accent\":\""  + cfgAccent  + "\"}";
  server.send(200, "application/json", j);
}

static void handlePostSettings() {
  bool rebootNeeded = false;
  prefs.begin("ducky", false);

  if (server.hasArg("ssid")) {
    String v = server.arg("ssid");
    if (v.length() && v != cfgSsid) { prefs.putString("ssid", v); rebootNeeded = true; }
  }
  if (server.hasArg("pass")) {
    String v = server.arg("pass");
    if (v.length() >= 8 && v != cfgPass) { prefs.putString("pass", v); rebootNeeded = true; }
  }
  if (server.hasArg("blename")) {
    String v = server.arg("blename");
    if (v.length() && v != cfgBleName) { prefs.putString("blename", v); rebootNeeded = true; }
  }
  if (server.hasArg("theme"))  { prefs.putString("theme",  server.arg("theme"));  cfgTheme  = server.arg("theme"); }
  if (server.hasArg("accent")) { prefs.putString("accent", server.arg("accent")); cfgAccent = server.arg("accent"); }
  prefs.end();

  server.send(200, "application/json",
              String("{\"ok\":true,\"reboot\":") + (rebootNeeded ? "true" : "false") + "}");

  if (rebootNeeded) { delay(400); ESP.restart(); }
}

static void handleRun() {
  String body = server.arg("plain");
  server.send(200, "text/plain", bleKeyboard->isConnected() ? "running" : "not connected");
  runScript(body);
}
static void handleSave() {
  File f = LittleFS.open(pathFor(server.arg("name")), "w");
  if (!f) { server.send(500, "text/plain", "write failed"); return; }
  f.print(server.arg("plain"));
  f.close();
  server.send(200, "text/plain", "saved");
}
static void handleLoad() {
  File f = LittleFS.open(pathFor(server.arg("name")), "r");
  if (!f) { server.send(404, "text/plain", ""); return; }
  server.streamFile(f, "text/plain");
  f.close();
}
static void handleDelete() {
  LittleFS.remove(pathFor(server.arg("name")));
  server.send(200, "text/plain", "deleted");
}
static void handleRunFile() {
  File f = LittleFS.open(pathFor(server.arg("name")), "r");
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  String body = f.readString();
  f.close();
  server.send(200, "text/plain", bleKeyboard->isConnected() ? "running" : "not connected");
  runScript(body);
}
static void handleList() {
  String json = "[";
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  bool first = true;
  String prefix = String(PL_PREFIX).substring(1); // "pl_"
  while (file) {
    String fn = String(file.name());
    int slash = fn.lastIndexOf('/');
    if (slash >= 0) fn = fn.substring(slash + 1);
    if (fn.startsWith(prefix) && fn.endsWith(PL_SUFFIX)) {
      String nm = fn.substring(prefix.length(), fn.length() - strlen(PL_SUFFIX));
      if (!first) json += ",";
      json += "\"" + nm + "\"";
      first = false;
    }
    file = root.openNextFile();
  }
  json += "]";
  server.send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  loadSettings();

  if (!LittleFS.begin(true)) Serial.println("[FS] LittleFS mount failed");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(cfgSsid.c_str(), cfgPass.c_str());
  Serial.printf("[WiFi] AP '%s'  http://%s\n", cfgSsid.c_str(), WiFi.softAPIP().toString().c_str());

  bleKeyboard = new BleKeyboard(cfgBleName.c_str(), "Espressif", 100);
  bleKeyboard->begin();
  Serial.printf("[BLE] advertising as '%s'\n", cfgBleName.c_str());

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/presets", HTTP_GET,  handlePresets);
  server.on("/settings",HTTP_GET,  handleGetSettings);
  server.on("/settings",HTTP_POST, handlePostSettings);
  server.on("/list",    HTTP_GET,  handleList);
  server.on("/load",    HTTP_GET,  handleLoad);
  server.on("/run",     HTTP_POST, handleRun);
  server.on("/save",    HTTP_POST, handleSave);
  server.on("/delete",  HTTP_POST, handleDelete);
  server.on("/runfile", HTTP_POST, handleRunFile);
  server.begin();
  Serial.println("[HTTP] server started");
}

void loop() {
  server.handleClient();
  delay(2);
}
