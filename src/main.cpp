/*
 * ESP32-WROOM-32E BLE Rubber Ducky
 * -----------------------------------------------------------------------------
 * The classic ESP32 has no native USB device peripheral, so this firmware turns
 * the board into a *Bluetooth LE* HID keyboard. Pair the board with a target
 * computer/phone, then push DuckyScript payloads to it from a web UI that the
 * board itself serves over its own WiFi access point.
 *
 * Flow:
 *   1. Board boots, starts a WiFi AP (DuckyESP32) and advertises as a BLE
 *      keyboard ("ESP32 Keyboard").
 *   2. On the target, pair with "ESP32 Keyboard" via Bluetooth settings.
 *   3. On any device, join the WiFi AP and open http://192.168.4.1 to write,
 *      save, and run DuckyScript payloads.
 *
 * For AUTHORIZED testing/education on hardware you own or have permission to test.
 * -----------------------------------------------------------------------------
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <BleKeyboard.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static const char *AP_SSID   = "DuckyESP32";     // WiFi network the board hosts
static const char *AP_PASS   = "quack1234";      // must be >= 8 chars
static const char *BLE_NAME  = "ESP32 Keyboard"; // name shown when pairing

// LittleFS layout: saved payloads live at /pl_<name>.txt
static const char *PL_PREFIX = "/pl_";
static const char *PL_SUFFIX = ".txt";

BleKeyboard bleKeyboard(BLE_NAME, "Espressif", 100);
WebServer   server(80);

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

  // Every token except the last is treated as a modifier.
  for (int i = 0; i < nt - 1; i++) {
    uint8_t m = modifierKey(tok[i]);
    if (m) bleKeyboard.press(m);
  }

  // Final token: named key, single character, or a bare modifier (e.g. "GUI").
  String last = tok[nt - 1];
  uint8_t nk = namedKey(last);
  if (nk) {
    bleKeyboard.press(nk);
  } else if (last.length() == 1) {
    bleKeyboard.press((uint8_t)last[0]);
  } else {
    uint8_t m = modifierKey(last);
    if (m) bleKeyboard.press(m);
  }

  delay(8);
  bleKeyboard.releaseAll();
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

  if (CMD == "REM") return;                                   // comment
  if (CMD == "DEFAULT_DELAY" || CMD == "DEFAULTDELAY") { defaultDelay = rest.toInt(); return; }
  if (CMD == "DELAY")     { delay(rest.toInt()); return; }
  if (CMD == "STRING")    { bleKeyboard.print(rest); return; }
  if (CMD == "STRINGLN")  { bleKeyboard.print(rest); bleKeyboard.write(KEY_RETURN); return; }
  if (CMD == "REPEAT") {
    int n = rest.toInt();
    for (int i = 0; i < n; i++) {
      execLine(lastLine);
      if (defaultDelay > 0) delay(defaultDelay);
    }
    return;
  }

  // Anything else is a key or modifier combo.
  pressCombo(line);
}

// Run a full multi-line script.
static void runScript(const String &script) {
  if (!bleKeyboard.isConnected()) return;

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
// Web UI
// ---------------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 BLE Ducky</title>
<style>
 body{font-family:system-ui,sans-serif;background:#0f1216;color:#e6e6e6;margin:0;padding:16px}
 h1{font-size:1.2rem;margin:0 0 4px}
 .sub{color:#8b98a5;font-size:.8rem;margin-bottom:12px}
 #status{display:inline-block;padding:2px 10px;border-radius:12px;font-size:.75rem;font-weight:600}
 .on{background:#12351f;color:#4ade80}.off{background:#3a1417;color:#f87171}
 textarea{width:100%;height:220px;background:#161b22;color:#e6e6e6;border:1px solid #2a2f37;
   border-radius:8px;padding:10px;font-family:ui-monospace,Menlo,monospace;font-size:.85rem;box-sizing:border-box}
 button{background:#2563eb;color:#fff;border:0;border-radius:8px;padding:8px 14px;font-size:.85rem;
   cursor:pointer;margin:6px 6px 0 0}
 button.sec{background:#2a2f37}button.dan{background:#7f1d1d}
 input{background:#161b22;color:#e6e6e6;border:1px solid #2a2f37;border-radius:8px;padding:8px;font-size:.85rem}
 ul{list-style:none;padding:0}li{display:flex;align-items:center;gap:8px;padding:6px 0;border-bottom:1px solid #1f242b}
 li span{flex:1;font-family:ui-monospace,monospace;font-size:.85rem}
 .small{padding:4px 10px;font-size:.75rem}
</style></head><body>
<h1>ESP32 BLE Rubber Ducky</h1>
<div class="sub">BLE status: <span id="status" class="off">checking...</span> &middot; pair with "ESP32 Keyboard" first</div>
<textarea id="script" placeholder="REM Example&#10;DELAY 500&#10;GUI r&#10;DELAY 300&#10;STRING notepad&#10;ENTER"></textarea>
<div>
 <button onclick="run()">&#9654; Run</button>
 <input id="name" placeholder="payload name" size="14">
 <button class="sec" onclick="save()">Save</button>
 <button class="sec" onclick="load()">Load into editor</button>
</div>
<h1 style="margin-top:20px;font-size:1rem">Saved payloads</h1>
<ul id="list"></ul>
<script>
 async function poll(){
   try{let r=await fetch('/status');let j=await r.json();
     let s=document.getElementById('status');
     s.textContent=j.connected?'connected':'not connected';
     s.className=j.connected?'on':'off';}catch(e){}
 }
 setInterval(poll,1500);poll();
 async function run(){await fetch('/run',{method:'POST',body:document.getElementById('script').value});}
 async function save(){let n=document.getElementById('name').value||'payload';
   await fetch('/save?name='+encodeURIComponent(n),{method:'POST',body:document.getElementById('script').value});refresh();}
 async function load(){let n=document.getElementById('name').value;if(!n)return;
   let r=await fetch('/load?name='+encodeURIComponent(n));document.getElementById('script').value=await r.text();}
 async function refresh(){let r=await fetch('/list');let arr=await r.json();
   let ul=document.getElementById('list');ul.innerHTML='';
   arr.forEach(n=>{let li=document.createElement('li');
     li.innerHTML='<span>'+n+'</span>'+
       '<button class="small" onclick="runFile(\''+n+'\')">Run</button>'+
       '<button class="small sec" onclick="pick(\''+n+'\')">Edit</button>'+
       '<button class="small dan" onclick="del(\''+n+'\')">Del</button>';
     ul.appendChild(li);});}
 async function runFile(n){await fetch('/runfile?name='+encodeURIComponent(n),{method:'POST'});}
 async function pick(n){document.getElementById('name').value=n;load();}
 async function del(n){await fetch('/delete?name='+encodeURIComponent(n),{method:'POST'});refresh();}
 refresh();
</script></body></html>
)HTML";

static void handleRoot()   { server.send_P(200, "text/html", INDEX_HTML); }
static void handleStatus() {
  server.send(200, "application/json",
              String("{\"connected\":") + (bleKeyboard.isConnected() ? "true" : "false") + "}");
}
static void handleRun() {
  String body = server.arg("plain");
  server.send(200, "text/plain", bleKeyboard.isConnected() ? "running" : "not connected");
  runScript(body);
}
static void handleSave() {
  String name = server.arg("name");
  String body = server.arg("plain");
  File f = LittleFS.open(pathFor(name), "w");
  if (!f) { server.send(500, "text/plain", "write failed"); return; }
  f.print(body);
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
  server.send(200, "text/plain", bleKeyboard.isConnected() ? "running" : "not connected");
  runScript(body);
}
static void handleList() {
  String json = "[";
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  bool first = true;
  String prefix = String(PL_PREFIX).substring(1); // "pl_" without leading slash
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

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] AP '%s'  http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  bleKeyboard.begin();
  Serial.printf("[BLE] advertising as '%s'\n", BLE_NAME);

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/status",  HTTP_GET,  handleStatus);
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
