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
  cfgAccent  = prefs.getString("accent",  "#00e676");
  // Migrate the old default blue accent to the new terminal green default.
  if (cfgAccent == "#2563eb") { cfgAccent = "#00e676"; prefs.putString("accent", cfgAccent); }
  prefs.end();
}

// LittleFS layout: saved scripts live under /s (see SROOT), as <name>.txt,
// optionally inside a folder subdirectory: /s/<folder>/<name>.txt
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

// Scripts live under /s . Folders are subdirectories: /s/<folder>/<name>.txt .
// Scripts with no folder live directly at /s/<name>.txt .
static const char *SROOT = "/s";

static String folderPath(const String &folder) {
  return String(SROOT) + "/" + sanitize(folder);
}
static String pathFor(const String &folder, const String &name) {
  if (folder.length()) return folderPath(folder) + "/" + sanitize(name) + PL_SUFFIX;
  return String(SROOT) + "/" + sanitize(name) + PL_SUFFIX;
}
static String baseName(const String &path) {
  int sl = path.lastIndexOf('/');
  return sl >= 0 ? path.substring(sl + 1) : path;
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
 "windows": [
  {
   "cat": "Rickrolls & Music",
   "items": [
    {
     "name": "Classic rickroll",
     "desc": "Opens the famous video in the browser",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING https://www.youtube.com/watch?v=dQw4w9WgXcQ",
      "ENTER"
     ]
    },
    {
     "name": "Fullscreen rickroll",
     "desc": "Opens the video and jumps to fullscreen",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING https://www.youtube.com/watch?v=dQw4w9WgXcQ",
      "ENTER",
      "DELAY 5000",
      "STRING f"
     ]
    },
    {
     "name": "Max-volume surprise",
     "desc": "Cranks the volume, then opens the video",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING powershell -c \"$w=New-Object -ComObject WScript.Shell;1..50|%{$w.SendKeys([char]175)}\"",
      "ENTER",
      "DELAY 400",
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING https://www.youtube.com/watch?v=dQw4w9WgXcQ",
      "ENTER"
     ]
    },
    {
     "name": "Lofi radio",
     "desc": "Opens a 24/7 music live stream",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING https://www.youtube.com/watch?v=jfKfPfyJRdk",
      "ENTER"
     ]
    }
   ]
  },
  {
   "cat": "Jump Scares & Sounds",
   "items": [
    {
     "name": "Loud alert tones",
     "desc": "Maxes volume and plays sharp beeps",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING powershell -c \"$w=New-Object -ComObject WScript.Shell;1..40|%{$w.SendKeys([char]175)};1..8|%{[console]::beep(1200,180)}\"",
      "ENTER"
     ]
    },
    {
     "name": "Creepy whisper",
     "desc": "Speaks a spooky line out loud",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING powershell -c \"Add-Type -AssemblyName System.Speech;(New-Object System.Speech.Synthesis.SpeechSynthesizer).Speak('I can see you')\"",
      "ENTER"
     ]
    },
    {
     "name": "Robot voice",
     "desc": "Speaks a warning in a robotic voice",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING powershell -c \"Add-Type -AssemblyName System.Speech;(New-Object System.Speech.Synthesis.SpeechSynthesizer).Speak('You should lock your computer')\"",
      "ENTER"
     ]
    },
    {
     "name": "Beep melody",
     "desc": "Plays a short tune",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING powershell -c \"[console]::beep(523,250);[console]::beep(659,250);[console]::beep(784,250);[console]::beep(1046,400)\"",
      "ENTER"
     ]
    },
    {
     "name": "Single alert sound",
     "desc": "Plays one system alert",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING powershell -c \"[console]::beep(880,400)\"",
      "ENTER"
     ]
    }
   ]
  },
  {
   "cat": "Popups & Fake Errors",
   "items": [
    {
     "name": "Single popup",
     "desc": "Shows one harmless message box",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING mshta \"javascript:alert('Gotcha');close()\"",
      "ENTER"
     ]
    },
    {
     "name": "Popup barrage",
     "desc": "Pops five message boxes in a row",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING powershell -c \"Add-Type -AssemblyName System.Windows.Forms;1..5|%{[System.Windows.Forms.MessageBox]::Show('Are you sure?')}\"",
      "ENTER"
     ]
    },
    {
     "name": "Fake critical error",
     "desc": "Shows a scary-looking error dialog",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING powershell -c \"Add-Type -AssemblyName System.Windows.Forms;[System.Windows.Forms.MessageBox]::Show('A critical error occurred. Please contact your administrator.','System Error',0,16)\"",
      "ENTER"
     ]
    },
    {
     "name": "Fake update banner",
     "desc": "Shows a system-style notification",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING powershell -c \"Add-Type -AssemblyName System.Windows.Forms;[System.Windows.Forms.MessageBox]::Show('An update is available.','Software Update')\"",
      "ENTER"
     ]
    }
   ]
  },
  {
   "cat": "Screen & Desktop Trolls",
   "items": [
    {
     "name": "Minimize everything",
     "desc": "Clears the screen to the desktop",
     "lines": [
      "DELAY 500",
      "GUI d"
     ]
    },
    {
     "name": "Hide desktop icons",
     "desc": "Makes the desktop icons disappear (reversible)",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING powershell -c \"$p='HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced';Set-ItemProperty $p HideIcons 1;Stop-Process -Name explorer -Force;Start-Process explorer\"",
      "ENTER"
     ]
    },
    {
     "name": "Restore desktop icons",
     "desc": "Undoes the hide-icons prank",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING powershell -c \"$p='HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced';Set-ItemProperty $p HideIcons 0;Stop-Process -Name explorer -Force;Start-Process explorer\"",
      "ENTER"
     ]
    },
    {
     "name": "Open the screenshot tool",
     "desc": "Launches the region screenshot capture",
     "lines": [
      "DELAY 500",
      "GUI SHIFT s"
     ]
    }
   ]
  },
  {
   "cat": "Fake Hacker & Terminal",
   "items": [
    {
     "name": "Matrix rain",
     "desc": "Green scrolling text in a console",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING cmd",
      "ENTER",
      "DELAY 800",
      "STRING color 0a",
      "ENTER",
      "STRING tree",
      "ENTER"
     ]
    },
    {
     "name": "Hacker typer",
     "desc": "Opens a fake 'hacking' website",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING https://hackertyper.net",
      "ENTER"
     ]
    },
    {
     "name": "Fake breach terminal",
     "desc": "Red console flashing an access warning",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING cmd",
      "ENTER",
      "DELAY 800",
      "STRING color 0c",
      "ENTER",
      "STRING echo ACCESS GRANTED",
      "ENTER",
      "STRING echo Downloading files...",
      "ENTER",
      "STRING timeout 3",
      "ENTER",
      "STRING echo Done.",
      "ENTER"
     ]
    },
    {
     "name": "Fake download progress",
     "desc": "Prints a fake progress readout",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING cmd",
      "ENTER",
      "DELAY 800",
      "STRING echo Installing updates...",
      "ENTER",
      "STRING echo 25%",
      "ENTER",
      "STRING timeout 1",
      "ENTER",
      "STRING echo 60%",
      "ENTER",
      "STRING timeout 1",
      "ENTER",
      "STRING echo 100% complete",
      "ENTER"
     ]
    }
   ]
  },
  {
   "cat": "Typing Trolls",
   "items": [
    {
     "name": "Creepy note",
     "desc": "Types an unsettling (then reassuring) note",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING notepad",
      "ENTER",
      "DEFAULT_DELAY 40",
      "DELAY 900",
      "STRING I know what you did last summer.",
      "ENTER",
      "STRING ...just kidding. Lock your screen next time!"
     ]
    },
    {
     "name": "Slow ghost typing",
     "desc": "Types a message very slowly",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING notepad",
      "ENTER",
      "DEFAULT_DELAY 180",
      "DELAY 900",
      "STRING is anyone there?"
     ]
    },
    {
     "name": "Caps Lock troll",
     "desc": "Toggles Caps Lock repeatedly",
     "lines": [
      "CAPSLOCK",
      "DELAY 250",
      "CAPSLOCK",
      "DELAY 250",
      "CAPSLOCK",
      "DELAY 250",
      "CAPSLOCK",
      "DELAY 250",
      "CAPSLOCK"
     ]
    },
    {
     "name": "Repeat note",
     "desc": "Fills a text editor with a repeated line",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING notepad",
      "ENTER",
      "DEFAULT_DELAY 30",
      "DELAY 900",
      "STRING look behind you... ",
      "REPEAT 20"
     ]
    }
   ]
  },
  {
   "cat": "Apps & Websites",
   "items": [
    {
     "name": "Open Calculator",
     "desc": "Launches the calculator",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING calc",
      "ENTER"
     ]
    },
    {
     "name": "Open the camera app",
     "desc": "Opens the webcam app (harmless surprise)",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING microsoft.windows.camera:",
      "ENTER"
     ]
    },
    {
     "name": "Pointer Pointer",
     "desc": "Opens the silly pointerpointer.com site",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING http://pointerpointer.com",
      "ENTER"
     ]
    },
    {
     "name": "Open a website",
     "desc": "Opens a URL in the default browser (edit it)",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING https://example.com",
      "ENTER"
     ]
    },
    {
     "name": "Open a text editor",
     "desc": "Opens a blank note",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING notepad",
      "ENTER"
     ]
    }
   ]
  },
  {
   "cat": "System",
   "items": [
    {
     "name": "Task list",
     "desc": "Shows running processes",
     "lines": [
      "DELAY 500",
      "CTRL SHIFT ESC"
     ]
    },
    {
     "name": "Lock the screen",
     "desc": "Locks the computer",
     "lines": [
      "DELAY 500",
      "GUI l"
     ]
    },
    {
     "name": "System info",
     "desc": "Prints hardware / OS details",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING cmd",
      "ENTER",
      "DELAY 800",
      "STRING systeminfo",
      "ENTER"
     ]
    },
    {
     "name": "Network info",
     "desc": "Shows the network configuration",
     "lines": [
      "DELAY 700",
      "GUI r",
      "DELAY 400",
      "STRING cmd",
      "ENTER",
      "DELAY 800",
      "STRING ipconfig /all",
      "ENTER"
     ]
    }
   ]
  }
 ],
 "mac": [
  {
   "cat": "Rickrolls & Music",
   "items": [
    {
     "name": "Classic rickroll",
     "desc": "Opens the famous video in the browser",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING open \"https://www.youtube.com/watch?v=dQw4w9WgXcQ\"",
      "ENTER"
     ]
    },
    {
     "name": "Fullscreen rickroll",
     "desc": "Opens the video and jumps to fullscreen",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING open \"https://www.youtube.com/watch?v=dQw4w9WgXcQ\"",
      "ENTER",
      "DELAY 5000",
      "STRING f"
     ]
    },
    {
     "name": "Max-volume surprise",
     "desc": "Cranks the volume, then opens the video",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING osascript -e \"set volume output volume 100\"",
      "ENTER",
      "STRING open \"https://www.youtube.com/watch?v=dQw4w9WgXcQ\"",
      "ENTER"
     ]
    },
    {
     "name": "Lofi radio",
     "desc": "Opens a 24/7 music live stream",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING open \"https://www.youtube.com/watch?v=jfKfPfyJRdk\"",
      "ENTER"
     ]
    }
   ]
  },
  {
   "cat": "Jump Scares & Sounds",
   "items": [
    {
     "name": "Loud alert tones",
     "desc": "Maxes volume and plays sharp beeps",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING osascript -e \"set volume output volume 100\"",
      "ENTER",
      "STRING for i in 1 2 3 4 5 6; do afplay /System/Library/Sounds/Sosumi.aiff; done",
      "ENTER"
     ]
    },
    {
     "name": "Creepy whisper",
     "desc": "Speaks a spooky line out loud",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING say -v Whisper \"I can see you\"",
      "ENTER"
     ]
    },
    {
     "name": "Robot voice",
     "desc": "Speaks a warning in a robotic voice",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING say -v Zarvox \"you should lock your computer\"",
      "ENTER"
     ]
    },
    {
     "name": "Beep melody",
     "desc": "Plays a short tune",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING afplay /System/Library/Sounds/Glass.aiff",
      "ENTER",
      "STRING afplay /System/Library/Sounds/Ping.aiff",
      "ENTER"
     ]
    },
    {
     "name": "Single alert sound",
     "desc": "Plays one system alert",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING afplay /System/Library/Sounds/Glass.aiff",
      "ENTER"
     ]
    }
   ]
  },
  {
   "cat": "Popups & Fake Errors",
   "items": [
    {
     "name": "Single popup",
     "desc": "Shows one harmless message box",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING osascript -e 'display dialog \"Gotcha\" buttons {\"OK\"}'",
      "ENTER"
     ]
    },
    {
     "name": "Popup barrage",
     "desc": "Pops five message boxes in a row",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING for i in 1 2 3 4 5; do osascript -e 'display dialog \"Are you sure?\" buttons {\"OK\"}'; done",
      "ENTER"
     ]
    },
    {
     "name": "Fake critical error",
     "desc": "Shows a scary-looking error dialog",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING osascript -e 'display dialog \"A critical error occurred.\" buttons {\"OK\"} with icon stop'",
      "ENTER"
     ]
    },
    {
     "name": "Fake update banner",
     "desc": "Shows a system-style notification",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING osascript -e 'display notification \"An update is available.\" with title \"Software Update\"'",
      "ENTER"
     ]
    }
   ]
  },
  {
   "cat": "Screen & Desktop Trolls",
   "items": [
    {
     "name": "Minimize everything",
     "desc": "Clears the screen to the desktop",
     "lines": [
      "DELAY 500",
      "GUI h"
     ]
    },
    {
     "name": "Hide desktop icons",
     "desc": "Makes the desktop icons disappear (reversible)",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING defaults write com.apple.finder CreateDesktop false",
      "ENTER",
      "STRING killall Finder",
      "ENTER"
     ]
    },
    {
     "name": "Restore desktop icons",
     "desc": "Undoes the hide-icons prank",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING defaults write com.apple.finder CreateDesktop true",
      "ENTER",
      "STRING killall Finder",
      "ENTER"
     ]
    },
    {
     "name": "Open the screenshot tool",
     "desc": "Launches the region screenshot capture",
     "lines": [
      "DELAY 500",
      "GUI SHIFT 3"
     ]
    }
   ]
  },
  {
   "cat": "Fake Hacker & Terminal",
   "items": [
    {
     "name": "Matrix rain",
     "desc": "Green scrolling text in a console",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING printf '\\033[32m'",
      "ENTER",
      "STRING ls -laR /System 2>/dev/null | head -n 400",
      "ENTER"
     ]
    },
    {
     "name": "Hacker typer",
     "desc": "Opens a fake 'hacking' website",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING open \"https://hackertyper.net\"",
      "ENTER"
     ]
    },
    {
     "name": "Fake breach terminal",
     "desc": "Red console flashing an access warning",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING printf '\\033[31m'",
      "ENTER",
      "STRING echo ACCESS GRANTED",
      "ENTER",
      "STRING echo Downloading files...",
      "ENTER",
      "STRING sleep 2",
      "ENTER",
      "STRING echo Done.",
      "ENTER"
     ]
    },
    {
     "name": "Fake download progress",
     "desc": "Prints a fake progress readout",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING echo Installing updates...",
      "ENTER",
      "STRING echo 25%",
      "ENTER",
      "STRING sleep 1",
      "ENTER",
      "STRING echo 60%",
      "ENTER",
      "STRING sleep 1",
      "ENTER",
      "STRING echo 100% complete",
      "ENTER"
     ]
    }
   ]
  },
  {
   "cat": "Typing Trolls",
   "items": [
    {
     "name": "Creepy note",
     "desc": "Types an unsettling (then reassuring) note",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING TextEdit",
      "ENTER",
      "DEFAULT_DELAY 40",
      "DELAY 1400",
      "STRING I know what you did last summer.",
      "ENTER",
      "STRING ...just kidding. Lock your screen next time!"
     ]
    },
    {
     "name": "Slow ghost typing",
     "desc": "Types a message very slowly",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING TextEdit",
      "ENTER",
      "DEFAULT_DELAY 180",
      "DELAY 1400",
      "STRING is anyone there?"
     ]
    },
    {
     "name": "Caps Lock troll",
     "desc": "Toggles Caps Lock repeatedly",
     "lines": [
      "CAPSLOCK",
      "DELAY 250",
      "CAPSLOCK",
      "DELAY 250",
      "CAPSLOCK",
      "DELAY 250",
      "CAPSLOCK",
      "DELAY 250",
      "CAPSLOCK"
     ]
    },
    {
     "name": "Repeat note",
     "desc": "Fills a text editor with a repeated line",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING TextEdit",
      "ENTER",
      "DEFAULT_DELAY 30",
      "DELAY 1400",
      "STRING look behind you... ",
      "REPEAT 20"
     ]
    }
   ]
  },
  {
   "cat": "Apps & Websites",
   "items": [
    {
     "name": "Open Calculator",
     "desc": "Launches the calculator",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Calculator",
      "ENTER"
     ]
    },
    {
     "name": "Open the camera app",
     "desc": "Opens the webcam app (harmless surprise)",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Photo Booth",
      "ENTER"
     ]
    },
    {
     "name": "Pointer Pointer",
     "desc": "Opens the silly pointerpointer.com site",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING open \"http://pointerpointer.com\"",
      "ENTER"
     ]
    },
    {
     "name": "Open a website",
     "desc": "Opens a URL in the default browser (edit it)",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING open \"https://example.com\"",
      "ENTER"
     ]
    },
    {
     "name": "Open a text editor",
     "desc": "Opens a blank note",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING TextEdit",
      "ENTER"
     ]
    }
   ]
  },
  {
   "cat": "System",
   "items": [
    {
     "name": "Task list",
     "desc": "Shows running processes",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Activity Monitor",
      "ENTER"
     ]
    },
    {
     "name": "Lock the screen",
     "desc": "Locks the computer",
     "lines": [
      "DELAY 500",
      "CTRL GUI q"
     ]
    },
    {
     "name": "System info",
     "desc": "Prints hardware / OS details",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING system_profiler SPHardwareDataType",
      "ENTER"
     ]
    },
    {
     "name": "Network info",
     "desc": "Shows the network configuration",
     "lines": [
      "DELAY 700",
      "GUI SPACE",
      "DELAY 400",
      "STRING Terminal",
      "ENTER",
      "DELAY 900",
      "STRING ifconfig",
      "ENTER"
     ]
    }
   ]
  }
 ]
}
)PRESETS";

// ---------------------------------------------------------------------------
// Web UI (single page, navbar with Scripts / Connection / Settings tabs)
// ---------------------------------------------------------------------------
// Shared terminal/"hacker" stylesheet, served at /style.css for both pages.
static const char STYLE_CSS[] PROGMEM = R"CSS(
:root{--accent:#00e676;--bg:#04070a;--card:#0a1310;--card2:#0d1a14;--line:#16351f;
  --text:#b8f5c9;--muted:#5f8a6e;--danger:#ff5370;--radius:8px;--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,"Liberation Mono",monospace}
[data-theme=light]{--bg:#e9f2ec;--card:#ffffff;--card2:#f2f8f4;--line:#c9e3d2;--text:#0d2417;--muted:#4e7a5e}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{-webkit-text-size-adjust:100%}
body{font-family:var(--mono);background:var(--bg);color:var(--text);margin:0;font-size:14.5px;line-height:1.45;
  padding-bottom:48px;letter-spacing:.2px}
body::before{content:"";position:fixed;inset:0;pointer-events:none;z-index:1;
  background:repeating-linear-gradient(0deg,rgba(0,255,140,.035) 0 1px,transparent 1px 3px)}
a{color:var(--accent);text-decoration:none}
::selection{background:var(--accent);color:#04070a}
header{position:sticky;top:0;z-index:20;background:linear-gradient(180deg,var(--card),rgba(10,19,16,.92));
  border-bottom:1px solid var(--line);padding:12px 16px;backdrop-filter:blur(6px)}
.wrap{max-width:760px;margin:0 auto}
.brand{display:flex;align-items:center;gap:9px;margin-bottom:11px}
.brand h1{font-size:.98rem;margin:0;font-weight:700;letter-spacing:.5px;color:var(--accent);
  text-shadow:0 0 8px rgba(0,230,118,.55)}
.brand .cur{display:inline-block;width:8px;height:15px;background:var(--accent);margin-left:2px;
  box-shadow:0 0 8px var(--accent);animation:blink 1.1s steps(1) infinite}
@keyframes blink{50%{opacity:0}}
.dot{width:9px;height:9px;border-radius:50%;background:var(--danger);box-shadow:0 0 8px var(--danger);flex:none}
.dot.on{background:var(--accent);box-shadow:0 0 8px var(--accent)}
nav{display:flex;gap:6px}
nav button{flex:1;background:transparent;color:var(--muted);border:1px solid var(--line);border-radius:var(--radius);
  padding:9px;font-family:var(--mono);font-size:.82rem;font-weight:700;letter-spacing:.5px;text-transform:lowercase;cursor:pointer;transition:.15s}
nav button::before{content:"> "}
nav button.active{background:transparent;color:var(--accent);border-color:var(--accent);
  box-shadow:0 0 10px rgba(0,230,118,.25) inset}
main{padding:16px;max-width:760px;margin:0 auto;position:relative;z-index:2}
.tab{display:none}.tab.show{display:block;animation:fade .18s ease}
@keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
textarea{width:100%;height:190px;background:#050a08;color:var(--text);border:1px solid var(--line);
  border-radius:var(--radius);padding:12px;font-family:var(--mono);font-size:.84rem;resize:vertical}
input,select{background:#050a08;color:var(--text);border:1px solid var(--line);border-radius:var(--radius);
  padding:11px;font-family:var(--mono);font-size:.86rem;width:100%}
input:focus,textarea:focus,select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 1px var(--accent)}
label{display:block;font-size:.74rem;color:var(--muted);margin:14px 0 5px;font-weight:700;text-transform:uppercase;letter-spacing:.6px}
button{font-family:var(--mono)}
.btn{background:transparent;color:var(--accent);border:1px solid var(--accent);border-radius:var(--radius);padding:11px 16px;
  font-size:.84rem;font-weight:700;letter-spacing:.5px;text-transform:uppercase;cursor:pointer;transition:.12s;min-height:44px}
.btn:hover{background:var(--accent);color:#04070a;box-shadow:0 0 14px rgba(0,230,118,.4)}
.btn:active{transform:scale(.97)}
.btn.sec{color:var(--text);border-color:var(--line)}
.btn.sec:hover{background:var(--line);color:var(--text);box-shadow:none}
.btn.block{width:100%;margin-top:14px}
.mini{padding:8px 13px;font-size:.72rem;font-weight:700;letter-spacing:.4px;text-transform:uppercase;
  background:transparent;color:var(--accent);border:1px solid var(--accent);border-radius:6px;cursor:pointer;min-height:34px}
.mini:hover{background:var(--accent);color:#04070a}
.mini.sec{color:var(--muted);border-color:var(--line)}.mini.sec:hover{background:var(--line);color:var(--text)}
.mini.dan{color:var(--danger);border-color:#4a1622}.mini.dan:hover{background:var(--danger);color:#04070a}
.badge{display:inline-block;padding:3px 10px;border-radius:6px;font-size:.7rem;font-weight:700;letter-spacing:.5px;text-transform:uppercase;border:1px solid}
.on{color:var(--accent);border-color:var(--accent);box-shadow:0 0 8px rgba(0,230,118,.3)}
.off{color:var(--danger);border-color:#4a1622}
.card{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);padding:14px;margin:12px 0}
.card h2{font-size:.82rem;margin:0 0 4px;text-transform:uppercase;letter-spacing:.6px;color:var(--accent)}
.card h2::before{content:"# "}
.row{display:flex;gap:9px;align-items:center;flex-wrap:wrap}
.kv{display:flex;justify-content:space-between;align-items:center;padding:11px 0;border-bottom:1px solid var(--line);font-size:.86rem}
.kv:last-child{border-bottom:0}.kv .k{color:var(--muted);text-transform:uppercase;font-size:.72rem;letter-spacing:.5px}
.note{font-size:.76rem;color:var(--muted);margin-top:10px;line-height:1.55}
.note::before{content:"// "}
.seg{display:inline-flex;background:#050a08;border:1px solid var(--line);border-radius:var(--radius);padding:3px;gap:3px;width:100%;max-width:300px}
.seg button{flex:1;border:0;background:transparent;color:var(--muted);padding:9px;border-radius:6px;font-family:var(--mono);
  font-size:.8rem;cursor:pointer;font-weight:700;letter-spacing:.4px;transition:.12s}
.seg button.on{background:var(--accent);color:#04070a;box-shadow:0 0 10px rgba(0,230,118,.4)}
.search{position:relative;margin:14px 0 4px}
.search input{padding-left:34px}
.search svg{position:absolute;left:10px;top:50%;transform:translateY(-50%);opacity:.6;color:var(--accent)}
.acc{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);margin:11px 0;overflow:hidden}
.acc-h{width:100%;display:flex;align-items:center;gap:10px;background:transparent;border:0;color:var(--text);
  padding:14px;font-family:var(--mono);font-size:.86rem;font-weight:700;letter-spacing:.4px;cursor:pointer;text-align:left}
.acc-h .cnt{margin-left:auto;font-size:.68rem;color:var(--accent);font-weight:700;background:#050a08;
  border:1px solid var(--line);padding:2px 8px;border-radius:6px}
.acc-h .chev{transition:transform .2s;color:var(--accent);font-size:.75rem}
.acc.open .chev{transform:rotate(90deg)}
.acc-b{display:none;padding:0 14px 8px}
.acc.open .acc-b{display:block}
.preset{display:flex;align-items:center;gap:11px;padding:12px 0;border-top:1px solid var(--line)}
.preset .meta{flex:1;min-width:0}
.preset .n{font-weight:700;font-size:.86rem}
.preset .d{font-size:.74rem;color:var(--muted);margin-top:2px}
.empty{color:var(--muted);font-size:.82rem;text-align:center;padding:22px}
#toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,20px);opacity:0;background:#04070a;color:var(--accent);
  padding:11px 18px;border-radius:var(--radius);font-family:var(--mono);font-size:.82rem;font-weight:700;pointer-events:none;
  transition:.25s;z-index:60;box-shadow:0 0 20px rgba(0,230,118,.25);max-width:88%;border:1px solid var(--accent)}
#toast.show{opacity:1;transform:translate(-50%,0)}
)CSS";

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>root@ducky:~#</title>
<link rel="stylesheet" href="/style.css">
</head><body>
<header><div class="wrap">
 <div class="brand"><span id="dot" class="dot"></span><h1>root@ducky:~#</h1><span class="cur"></span></div>
 <nav>
  <button data-tab="scripts" class="active">scripts</button>
  <button data-tab="connection">connection</button>
  <button data-tab="settings">settings</button>
 </nav>
</div></header>
<main>

 <section id="scripts" class="tab show">
  <div class="card">
   <div class="row"><h2 style="margin:0">Run a script</h2>
     <span id="badge" class="badge off" style="margin-left:auto">checking…</span></div>
   <textarea id="script" placeholder="Paste a script here, then press Go&#10;&#10;DELAY 500&#10;GUI r&#10;STRING notepad&#10;ENTER" style="margin-top:10px"></textarea>
   <div class="row" style="margin-top:10px">
     <button class="btn" onclick="go()">&#9654;&nbsp; run</button>
   </div>
   <label>Save this script</label>
   <div class="row">
     <input id="pname" placeholder="script name…" style="flex:2;min-width:130px">
     <select id="pfolder" style="flex:1;min-width:120px"></select>
     <button class="btn sec" onclick="save()">Save</button>
   </div>
   <div class="note">Keystrokes are sent to the paired Bluetooth target. Pair the board first (see the connection tab).</div>
  </div>

  <div class="card">
   <div class="row"><h2 style="margin:0">Folders</h2></div>
   <div class="row" style="margin-top:8px">
     <input id="newfolder" placeholder="new folder name…" style="flex:1;min-width:130px" onkeydown="if(event.key=='Enter')mkFolder()">
     <button class="btn sec" onclick="mkFolder()">+ folder</button>
   </div>
   <div class="note">Folders open in a new tab where you can run, edit, and save scripts inside them.</div>
  </div>

  <div id="saved"></div>

  <div class="row" style="margin:16px 2px 0;justify-content:center">
    <div class="seg">
      <button id="os_win" onclick="setOS('windows')">Windows</button>
      <button id="os_mac" onclick="setOS('mac')">macOS</button>
    </div>
  </div>
  <div class="search">
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="7"/><path d="M21 21l-4.3-4.3"/></svg>
    <input id="q" placeholder="Search presets…" oninput="filterPresets()">
  </div>
  <div id="presets"></div>
 </section>

 <section id="connection" class="tab">
  <div class="card">
   <div class="kv"><span class="k">Bluetooth status</span><span id="c_ble" class="badge off">…</span></div>
   <div class="kv"><span class="k">Bluetooth name</span><span id="c_bt">…</span></div>
   <div class="kv"><span class="k">WiFi network</span><span id="c_ssid">…</span></div>
   <div class="kv"><span class="k">Web address</span><span id="c_ip">…</span></div>
   <div class="kv"><span class="k">WiFi clients</span><span id="c_cli">…</span></div>
  </div>
  <div class="note">On the target device, open Bluetooth settings and pair with the name shown above.
   The status turns green once it connects, and scripts will then type on that device.</div>
 </section>

 <section id="settings" class="tab">
  <div class="card">
   <h2>Network</h2>
   <label>WiFi network name (SSID)</label><input id="s_ssid">
   <label>WiFi password (min 8 characters)</label><input id="s_pass">
   <label>Bluetooth name</label><input id="s_ble">
   <div class="note">Saving network changes reboots the board — you'll need to rejoin the WiFi and re-pair Bluetooth.</div>
   <button class="btn block" onclick="saveNet()">Save &amp; reboot</button>
  </div>
  <div class="card">
   <h2>Appearance</h2>
   <label>Theme</label>
   <select id="s_theme" onchange="previewTheme()"><option value="dark">Dark</option><option value="light">Light</option></select>
   <label>Accent color</label><input id="s_accent" type="color" style="height:46px;padding:4px" oninput="previewTheme()">
   <button class="btn block" onclick="saveUi()">Save appearance</button>
  </div>
 </section>

</main>
<div id="toast"></div>
<script>
 let CFG={},PRESETS={},OS=localStorage.getItem('os')||'windows';
 function $(id){return document.getElementById(id);}
 function enc(t){return t.replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;');}
 let tT;function toast(m){let t=$('toast');t.textContent=m;t.classList.add('show');
   clearTimeout(tT);tT=setTimeout(()=>t.classList.remove('show'),1700);}

 // tab switching
 document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{
   document.querySelectorAll('nav button').forEach(x=>x.classList.remove('active'));
   document.querySelectorAll('.tab').forEach(x=>x.classList.remove('show'));
   b.classList.add('active');$(b.dataset.tab).classList.add('show');window.scrollTo(0,0);});

 function applyTheme(){document.documentElement.dataset.theme=CFG.theme||'dark';
   document.documentElement.style.setProperty('--accent',CFG.accent||'#00e676');}
 function previewTheme(){document.documentElement.dataset.theme=$('s_theme').value;
   document.documentElement.style.setProperty('--accent',$('s_accent').value);}

 // running scripts (with feedback)
 async function runText(t){try{let r=await fetch('/run',{method:'POST',body:t});let m=await r.text();
   toast(m=='running'?'Sent to target ✓':'Not connected — pair Bluetooth first');}
   catch(e){toast('Error sending');}}
 function go(){let s=$('script').value.trim();if(!s){toast('Nothing to run');return;}runText(s);}
 function loadInto(t){$('script').value=t;window.scrollTo({top:0,behavior:'smooth'});toast('Loaded into editor');}
 async function save(){let n=$('pname').value||'payload';let f=$('pfolder').value;
   await fetch('/save?folder='+encodeURIComponent(f)+'&name='+encodeURIComponent(n),{method:'POST',body:$('script').value});
   toast(f?('Saved to '+f):'Saved');loadSaved();}
 async function mkFolder(){let n=$('newfolder').value.trim();if(!n){toast('Enter a folder name');return;}
   await fetch('/mkfolder?name='+encodeURIComponent(n),{method:'POST'});$('newfolder').value='';
   toast('Folder created');loadSaved();}
 async function rmFolder(n){if(!confirm('Delete folder "'+n+'" and all scripts inside?'))return;
   await fetch('/rmfolder?name='+encodeURIComponent(n),{method:'POST'});toast('Folder deleted');loadSaved();}

 async function loadPresets(){PRESETS=await (await fetch('/presets')).json();renderPresets();}
 function setOS(os){OS=os;localStorage.setItem('os',os);$('q').value='';renderPresets();}
 function renderPresets(){
   $('os_win').className=OS=='windows'?'on':'';
   $('os_mac').className=OS=='mac'?'on':'';
   let groups=PRESETS[OS]||[];let h='';
   groups.forEach(g=>{
     h+='<div class="acc" data-cat="'+enc(g.cat)+'">'+
        '<button class="acc-h" onclick="this.parentNode.classList.toggle(\'open\')">'+
        '<span class="chev">&#9654;</span><span>'+g.cat+'</span><span class="cnt">'+g.items.length+'</span></button>'+
        '<div class="acc-b">';
     g.items.forEach(p=>{let s=enc(p.lines.join('\n'));
       h+='<div class="preset" data-txt="'+enc((p.name+' '+p.desc).toLowerCase())+'">'+
          '<div class="meta"><div class="n">'+p.name+'</div><div class="d">'+p.desc+'</div></div>'+
          '<button class="mini run" data-s="'+s+'" onclick="runText(this.dataset.s)">Run</button>'+
          '<button class="mini sec" data-s="'+s+'" onclick="loadInto(this.dataset.s)">Load</button></div>';});
     h+='</div></div>';});
   $('presets').innerHTML=h;filterPresets();}
 function filterPresets(){
   let q=$('q').value.trim().toLowerCase();
   document.querySelectorAll('#presets .acc').forEach(acc=>{
     let any=0;
     acc.querySelectorAll('.preset').forEach(p=>{
       let hit=!q||p.dataset.txt.indexOf(q)>=0;p.style.display=hit?'':'none';if(hit)any++;});
     acc.style.display=any?'':'none';
     if(q)acc.classList.toggle('open',!!any);});}

 async function loadSaved(){
   let folders=await (await fetch('/folders')).json();
   let root=await (await fetch('/list')).json();
   // populate the "save to folder" dropdown
   let sel=$('pfolder');let cur=sel.value;
   sel.innerHTML='<option value="">(no folder)</option>'+folders.map(f=>'<option>'+enc(f.name)+'</option>').join('');
   sel.value=cur;
   let h='';
   if(folders.length){
     h+='<div class="acc open"><button class="acc-h" onclick="this.parentNode.classList.toggle(\'open\')">'+
        '<span class="chev">&#9654;</span><span>Folders</span><span class="cnt">'+folders.length+'</span></button><div class="acc-b">';
     folders.forEach(f=>{let u='/folder?name='+encodeURIComponent(f.name);
       h+='<div class="preset"><div class="meta"><div class="n">&#128193; <a href="'+u+'" target="_blank" rel="noopener">'+enc(f.name)+'</a></div>'+
          '<div class="d">'+f.count+' script(s)</div></div>'+
          '<a class="mini" href="'+u+'" target="_blank" rel="noopener">open &#8599;</a>'+
          '<button class="mini dan" onclick="rmFolder(this.dataset.n)" data-n="'+enc(f.name)+'">del</button></div>';});
     h+='</div></div>';
   }
   if(root.length){
     h+='<div class="acc open"><button class="acc-h" onclick="this.parentNode.classList.toggle(\'open\')">'+
        '<span class="chev">&#9654;</span><span>Ungrouped scripts</span><span class="cnt">'+root.length+'</span></button><div class="acc-b">';
     root.forEach(n=>{h+='<div class="preset"><div class="meta"><div class="n">'+enc(n)+'</div></div>'+
       '<button class="mini" onclick="runFile(this.dataset.n)" data-n="'+enc(n)+'">run</button>'+
       '<button class="mini sec" onclick="editFile(this.dataset.n)" data-n="'+enc(n)+'">edit</button>'+
       '<button class="mini dan" onclick="delFile(this.dataset.n)" data-n="'+enc(n)+'">del</button></div>';});
     h+='</div></div>';
   }
   $('saved').innerHTML=h;}
 async function runFile(n){let r=await fetch('/runfile?name='+encodeURIComponent(n),{method:'POST'});
   let m=await r.text();toast(m=='running'?'Sent to target ✓':'Not connected — pair Bluetooth first');}
 async function editFile(n){$('pname').value=n;$('pfolder').value='';
   $('script').value=await (await fetch('/load?name='+encodeURIComponent(n))).text();
   document.querySelector('nav button[data-tab=scripts]').click();window.scrollTo(0,0);}
 async function delFile(n){await fetch('/delete?name='+encodeURIComponent(n),{method:'POST'});toast('Deleted');loadSaved();}

 async function poll(){try{let j=await (await fetch('/status')).json();let up=j.connected;
   $('dot').className='dot'+(up?' on':'');
   let b=$('badge');b.textContent=up?'connected':'not connected';b.className='badge '+(up?'on':'off');
   let c=$('c_ble');c.textContent=up?'connected':'not connected';c.className='badge '+(up?'on':'off');
   $('c_bt').textContent=j.blename;$('c_ssid').textContent=j.ssid;$('c_ip').textContent='http://'+j.ip;
   $('c_cli').textContent=j.clients;}catch(e){}}
 async function loadCfg(){CFG=await (await fetch('/settings')).json();applyTheme();
   $('s_ssid').value=CFG.ssid;$('s_pass').value=CFG.pass;$('s_ble').value=CFG.blename;
   $('s_theme').value=CFG.theme;$('s_accent').value=CFG.accent;}
 async function saveNet(){let p=$('s_pass').value;
   if(p.length<8){toast('WiFi password needs 8+ characters');return;}
   let body='ssid='+encodeURIComponent($('s_ssid').value)+'&pass='+encodeURIComponent(p)+
            '&blename='+encodeURIComponent($('s_ble').value);
   await fetch('/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
   toast('Saved — rebooting. Rejoin WiFi and re-pair.');}
 async function saveUi(){
   let body='theme='+encodeURIComponent($('s_theme').value)+'&accent='+encodeURIComponent($('s_accent').value);
   await fetch('/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
   CFG.theme=$('s_theme').value;CFG.accent=$('s_accent').value;applyTheme();toast('Appearance saved');}

 loadCfg();loadPresets();loadSaved();poll();setInterval(poll,1500);
</script></body></html>
)HTML";

// Per-folder page, opened in a new tab from the main page (GET /folder?name=X).
static const char FOLDER_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>root@ducky:~/</title>
<link rel="stylesheet" href="/style.css">
</head><body>
<header><div class="wrap">
 <div class="brand"><span id="dot" class="dot"></span><h1 id="ftitle">root@ducky:~/</h1><span class="cur"></span></div>
 <nav><button onclick="location.href='/'">&lt; back to control panel</button></nav>
</div></header>
<main>
 <div class="card">
  <div class="row"><h2 style="margin:0">New script in this folder</h2>
    <span id="badge" class="badge off" style="margin-left:auto">checking…</span></div>
  <textarea id="script" placeholder="Type or paste a script, then Save or Run&#10;&#10;DELAY 500&#10;GUI r&#10;STRING notepad&#10;ENTER" style="margin-top:10px"></textarea>
  <div class="row" style="margin-top:10px">
    <button class="btn" onclick="go()">&#9654;&nbsp; run</button>
    <input id="pname" placeholder="script name…" style="flex:1;min-width:120px">
    <button class="btn sec" onclick="save()">save here</button>
  </div>
 </div>
 <div id="list"></div>
</main>
<div id="toast"></div>
<script>
 let FOLDER=new URLSearchParams(location.search).get('name')||'';
 function $(id){return document.getElementById(id);}
 function enc(t){return t.replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;');}
 let tT;function toast(m){let t=$('toast');t.textContent=m;t.classList.add('show');
   clearTimeout(tT);tT=setTimeout(()=>t.classList.remove('show'),1700);}
 document.title='root@ducky:~/'+FOLDER;
 $('ftitle').textContent='~/'+FOLDER;
 let qs='folder='+encodeURIComponent(FOLDER);
 async function runText(t){try{let r=await fetch('/run',{method:'POST',body:t});let m=await r.text();
   toast(m=='running'?'Sent to target ✓':'Not connected — pair Bluetooth first');}catch(e){toast('Error');}}
 function go(){let s=$('script').value.trim();if(!s){toast('Nothing to run');return;}runText(s);}
 async function save(){let n=$('pname').value||'payload';
   await fetch('/save?'+qs+'&name='+encodeURIComponent(n),{method:'POST',body:$('script').value});
   toast('Saved');load();}
 async function load(){
   let arr=await (await fetch('/list?'+qs)).json();
   if(!arr.length){$('list').innerHTML='<div class="empty">No scripts in this folder yet.</div>';return;}
   let h='<div class="acc open"><button class="acc-h" onclick="this.parentNode.classList.toggle(\'open\')">'+
     '<span class="chev">&#9654;</span><span>Scripts</span><span class="cnt">'+arr.length+'</span></button><div class="acc-b">';
   arr.forEach(n=>{h+='<div class="preset"><div class="meta"><div class="n">'+enc(n)+'</div></div>'+
     '<button class="mini" onclick="runFile(this.dataset.n)" data-n="'+enc(n)+'">run</button>'+
     '<button class="mini sec" onclick="editFile(this.dataset.n)" data-n="'+enc(n)+'">edit</button>'+
     '<button class="mini dan" onclick="delFile(this.dataset.n)" data-n="'+enc(n)+'">del</button></div>';});
   h+='</div></div>';$('list').innerHTML=h;}
 async function runFile(n){let r=await fetch('/runfile?'+qs+'&name='+encodeURIComponent(n),{method:'POST'});
   let m=await r.text();toast(m=='running'?'Sent to target ✓':'Not connected — pair Bluetooth first');}
 async function editFile(n){$('pname').value=n;
   $('script').value=await (await fetch('/load?'+qs+'&name='+encodeURIComponent(n))).text();
   window.scrollTo({top:0,behavior:'smooth'});toast('Loaded into editor');}
 async function delFile(n){await fetch('/delete?'+qs+'&name='+encodeURIComponent(n),{method:'POST'});toast('Deleted');load();}
 async function poll(){try{let j=await (await fetch('/status')).json();let up=j.connected;
   $('dot').className='dot'+(up?' on':'');
   let b=$('badge');b.textContent=up?'connected':'not connected';b.className='badge '+(up?'on':'off');}catch(e){}}
 load();poll();setInterval(poll,1500);
</script></body></html>
)HTML";

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static void handleRoot()       { server.send_P(200, "text/html", INDEX_HTML); }
static void handleFolderPage() { server.send_P(200, "text/html", FOLDER_HTML); }
static void handleStyle()      { server.send_P(200, "text/css", STYLE_CSS); }
static void handlePresets()    { server.send_P(200, "application/json", PRESETS_JSON); }

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
  String folder = server.arg("folder");
  if (folder.length()) {
    String fp = folderPath(folder);
    if (!LittleFS.exists(fp)) LittleFS.mkdir(fp);
  }
  File f = LittleFS.open(pathFor(folder, server.arg("name")), "w");
  if (!f) { server.send(500, "text/plain", "write failed"); return; }
  f.print(server.arg("plain"));
  f.close();
  server.send(200, "text/plain", "saved");
}
static void handleLoad() {
  File f = LittleFS.open(pathFor(server.arg("folder"), server.arg("name")), "r");
  if (!f) { server.send(404, "text/plain", ""); return; }
  server.streamFile(f, "text/plain");
  f.close();
}
static void handleDelete() {
  LittleFS.remove(pathFor(server.arg("folder"), server.arg("name")));
  server.send(200, "text/plain", "deleted");
}
static void handleRunFile() {
  File f = LittleFS.open(pathFor(server.arg("folder"), server.arg("name")), "r");
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  String body = f.readString();
  f.close();
  server.send(200, "text/plain", bleKeyboard->isConnected() ? "running" : "not connected");
  runScript(body);
}
// List script names inside a folder (or the /s root when folder is empty).
static void handleList() {
  String folder = server.arg("folder");
  String base = folder.length() ? folderPath(folder) : String(SROOT);
  String json = "[";
  File dir = LittleFS.open(base);
  File e = dir.openNextFile();
  bool first = true;
  while (e) {
    if (!e.isDirectory()) {
      String fn = baseName(String(e.name()));
      if (fn.endsWith(PL_SUFFIX)) {
        String nm = fn.substring(0, fn.length() - strlen(PL_SUFFIX));
        if (!first) json += ",";
        json += "\"" + nm + "\"";
        first = false;
      }
    }
    e = dir.openNextFile();
  }
  json += "]";
  server.send(200, "application/json", json);
}
// List folders (subdirectories of /s) with a script count each.
static void handleFolders() {
  String json = "[";
  File root = LittleFS.open(SROOT);
  File e = root.openNextFile();
  bool first = true;
  while (e) {
    if (e.isDirectory()) {
      String fn = baseName(String(e.name()));
      int cnt = 0;
      File d = LittleFS.open(String(SROOT) + "/" + fn);
      File c = d.openNextFile();
      while (c) { if (!c.isDirectory()) cnt++; c = d.openNextFile(); }
      if (!first) json += ",";
      json += "{\"name\":\"" + fn + "\",\"count\":" + String(cnt) + "}";
      first = false;
    }
    e = root.openNextFile();
  }
  json += "]";
  server.send(200, "application/json", json);
}
static void handleMkFolder() {
  String name = server.arg("name");
  if (!name.length()) { server.send(400, "text/plain", "name required"); return; }
  String fp = folderPath(name);
  if (!LittleFS.exists(fp)) LittleFS.mkdir(fp);
  server.send(200, "text/plain", "ok");
}
static void handleRmFolder() {
  String fp = folderPath(server.arg("name"));
  File d = LittleFS.open(fp);
  String paths = "";
  File c = d.openNextFile();
  while (c) { if (!c.isDirectory()) paths += String(c.name()) + "\n"; c = d.openNextFile(); }
  int i = 0;
  while (i < (int)paths.length()) {
    int nl = paths.indexOf('\n', i);
    LittleFS.remove(paths.substring(i, nl));
    i = nl + 1;
  }
  LittleFS.rmdir(fp);
  server.send(200, "text/plain", "ok");
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  loadSettings();

  if (!LittleFS.begin(true)) Serial.println("[FS] LittleFS mount failed");

  // Ensure the scripts root exists, then migrate any legacy /pl_*.txt saves.
  if (!LittleFS.exists(SROOT)) LittleFS.mkdir(SROOT);
  {
    File r = LittleFS.open("/");
    File e = r.openNextFile();
    String moves = "";
    while (e) {
      String b = baseName(String(e.name()));
      if (!e.isDirectory() && b.startsWith("pl_") && b.endsWith(PL_SUFFIX))
        moves += String(e.name()) + "\n";
      e = r.openNextFile();
    }
    int i = 0;
    while (i < (int)moves.length()) {
      int nl = moves.indexOf('\n', i);
      String src = moves.substring(i, nl); i = nl + 1;
      String nm = baseName(src).substring(3); // strip "pl_"
      File in = LittleFS.open(src, "r");
      if (in) {
        String data = in.readString(); in.close();
        File out = LittleFS.open(String(SROOT) + "/" + nm, "w");
        if (out) { out.print(data); out.close(); }
        LittleFS.remove(src);
      }
    }
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(cfgSsid.c_str(), cfgPass.c_str());
  Serial.printf("[WiFi] AP '%s'  http://%s\n", cfgSsid.c_str(), WiFi.softAPIP().toString().c_str());

  bleKeyboard = new BleKeyboard(cfgBleName.c_str(), "Espressif", 100);
  bleKeyboard->begin();
  Serial.printf("[BLE] advertising as '%s'\n", cfgBleName.c_str());

  server.on("/",         HTTP_GET,  handleRoot);
  server.on("/style.css",HTTP_GET,  handleStyle);
  server.on("/folder",   HTTP_GET,  handleFolderPage);
  server.on("/status",   HTTP_GET,  handleStatus);
  server.on("/presets",  HTTP_GET,  handlePresets);
  server.on("/settings", HTTP_GET,  handleGetSettings);
  server.on("/settings", HTTP_POST, handlePostSettings);
  server.on("/list",     HTTP_GET,  handleList);
  server.on("/load",     HTTP_GET,  handleLoad);
  server.on("/folders",  HTTP_GET,  handleFolders);
  server.on("/mkfolder", HTTP_POST, handleMkFolder);
  server.on("/rmfolder", HTTP_POST, handleRmFolder);
  server.on("/run",      HTTP_POST, handleRun);
  server.on("/save",     HTTP_POST, handleSave);
  server.on("/delete",   HTTP_POST, handleDelete);
  server.on("/runfile",  HTTP_POST, handleRunFile);
  server.begin();
  Serial.println("[HTTP] server started");
}

void loop() {
  server.handleClient();
  delay(2);
}
