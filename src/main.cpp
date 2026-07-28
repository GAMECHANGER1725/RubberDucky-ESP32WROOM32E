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
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Keyboard Control Panel</title>
<style>
 :root{--accent:#2563eb;--bg:#0f1216;--card:#161b22;--card2:#1c222b;--line:#252c36;--text:#e9edf1;--muted:#8b98a5;--radius:12px}
 [data-theme=light]{--bg:#f3f5f8;--card:#ffffff;--card2:#f7f9fc;--line:#e3e7ec;--text:#1b2129;--muted:#5b6470}
 *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
 body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--text);margin:0;
   font-size:15px;line-height:1.4;padding-bottom:40px}
 header{position:sticky;top:0;z-index:20;background:var(--card);border-bottom:1px solid var(--line);padding:12px 16px;
   backdrop-filter:saturate(140%) blur(6px)}
 .wrap{max-width:760px;margin:0 auto}
 .brand{display:flex;align-items:center;gap:9px;margin-bottom:11px}
 .brand h1{font-size:1.02rem;margin:0;font-weight:700;letter-spacing:.2px}
 .dot{width:9px;height:9px;border-radius:50%;background:#f87171;box-shadow:0 0 0 3px rgba(248,113,113,.18);flex:none}
 .dot.on{background:#4ade80;box-shadow:0 0 0 3px rgba(74,222,128,.18)}
 nav{display:flex;gap:6px}
 nav button{flex:1;background:transparent;color:var(--muted);border:1px solid var(--line);
   border-radius:10px;padding:9px;font-size:.86rem;font-weight:600;cursor:pointer;transition:.15s}
 nav button.active{background:var(--accent);color:#fff;border-color:var(--accent)}
 main{padding:16px;max-width:760px;margin:0 auto}
 .tab{display:none}.tab.show{display:block;animation:fade .18s ease}
 @keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
 textarea{width:100%;height:190px;background:var(--card);color:var(--text);border:1px solid var(--line);
   border-radius:var(--radius);padding:12px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.85rem;resize:vertical}
 input,select{background:var(--card);color:var(--text);border:1px solid var(--line);border-radius:10px;
   padding:11px;font-size:.9rem;width:100%}
 input:focus,textarea:focus,select:focus{outline:none;border-color:var(--accent)}
 label{display:block;font-size:.78rem;color:var(--muted);margin:14px 0 5px;font-weight:600}
 button{font-family:inherit}
 .btn{background:var(--accent);color:#fff;border:0;border-radius:10px;padding:12px 18px;font-size:.9rem;
   font-weight:600;cursor:pointer;transition:.12s;min-height:44px}
 .btn:active{transform:scale(.97)}
 .btn.sec{background:var(--line);color:var(--text)}
 .btn.block{width:100%;margin-top:14px}
 .mini{padding:8px 14px;font-size:.78rem;border:0;border-radius:8px;cursor:pointer;font-weight:600;min-height:36px}
 .mini.run{background:var(--accent);color:#fff}.mini.sec{background:var(--line);color:var(--text)}
 .mini.dan{background:#7f1d1d;color:#fff}
 .badge{display:inline-block;padding:3px 11px;border-radius:12px;font-size:.74rem;font-weight:700}
 .on{background:#12351f;color:#4ade80}.off{background:#3a1417;color:#f87171}
 .card{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);padding:14px;margin:12px 0}
 .card h2{font-size:.92rem;margin:0 0 4px}
 .row{display:flex;gap:9px;align-items:center;flex-wrap:wrap}
 .kv{display:flex;justify-content:space-between;align-items:center;padding:11px 0;border-bottom:1px solid var(--line);font-size:.9rem}
 .kv:last-child{border-bottom:0}.kv .k{color:var(--muted)}
 .note{font-size:.78rem;color:var(--muted);margin-top:10px;line-height:1.5}
 /* segmented OS toggle */
 .seg{display:inline-flex;background:var(--line);border-radius:11px;padding:3px;gap:3px;width:100%;max-width:280px}
 .seg button{flex:1;border:0;background:transparent;color:var(--muted);padding:9px;border-radius:9px;
   font-size:.85rem;cursor:pointer;font-weight:700;transition:.12s}
 .seg button.on{background:var(--accent);color:#fff}
 /* search */
 .search{position:relative;margin:14px 0 4px}
 .search input{padding-left:36px}
 .search svg{position:absolute;left:11px;top:50%;transform:translateY(-50%);opacity:.5}
 /* accordion */
 .acc{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);margin:11px 0;overflow:hidden}
 .acc-h{width:100%;display:flex;align-items:center;gap:11px;background:transparent;border:0;color:var(--text);
   padding:15px 14px;font-size:.93rem;font-weight:700;cursor:pointer;text-align:left}
 .acc-h .cnt{margin-left:auto;font-size:.72rem;color:var(--muted);font-weight:700;background:var(--card2);
   border:1px solid var(--line);padding:2px 9px;border-radius:11px}
 .acc-h .chev{transition:transform .2s;color:var(--muted);font-size:.8rem}
 .acc.open .chev{transform:rotate(90deg)}
 .acc-b{display:none;padding:0 14px 8px}
 .acc.open .acc-b{display:block}
 .preset{display:flex;align-items:center;gap:11px;padding:12px 0;border-top:1px solid var(--line)}
 .preset .meta{flex:1;min-width:0}
 .preset .n{font-weight:600;font-size:.9rem}
 .preset .d{font-size:.78rem;color:var(--muted);margin-top:2px}
 .empty{color:var(--muted);font-size:.85rem;text-align:center;padding:22px}
 /* toast */
 #toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,20px);opacity:0;background:#0b0e12;color:#fff;
   padding:11px 20px;border-radius:11px;font-size:.86rem;font-weight:600;pointer-events:none;transition:.25s;
   z-index:60;box-shadow:0 8px 26px rgba(0,0,0,.45);max-width:88%;border:1px solid #2a323c}
 #toast.show{opacity:1;transform:translate(-50%,0)}
</style></head><body>
<header><div class="wrap">
 <div class="brand"><span id="dot" class="dot"></span><h1>Control Panel</h1></div>
 <nav>
  <button data-tab="scripts" class="active">Scripts</button>
  <button data-tab="connection">Connection</button>
  <button data-tab="settings">Settings</button>
 </nav>
</div></header>
<main>

 <section id="scripts" class="tab show">
  <div class="card">
   <div class="row"><h2 style="margin:0">Run a script</h2>
     <span id="badge" class="badge off" style="margin-left:auto">checking…</span></div>
   <textarea id="script" placeholder="Paste a script here, then press Go&#10;&#10;DELAY 500&#10;GUI r&#10;STRING notepad&#10;ENTER" style="margin-top:10px"></textarea>
   <div class="row" style="margin-top:10px">
     <button class="btn" onclick="go()">&#9654;&nbsp; Go</button>
     <input id="pname" placeholder="save as…" style="flex:1;min-width:120px">
     <button class="btn sec" onclick="save()">Save</button>
   </div>
   <div class="note">Keystrokes are sent to the paired Bluetooth target. Pair the board first (see the Connection tab).</div>
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
   document.documentElement.style.setProperty('--accent',CFG.accent||'#2563eb');}
 function previewTheme(){document.documentElement.dataset.theme=$('s_theme').value;
   document.documentElement.style.setProperty('--accent',$('s_accent').value);}

 // running scripts (with feedback)
 async function runText(t){try{let r=await fetch('/run',{method:'POST',body:t});let m=await r.text();
   toast(m=='running'?'Sent to target ✓':'Not connected — pair Bluetooth first');}
   catch(e){toast('Error sending');}}
 function go(){let s=$('script').value.trim();if(!s){toast('Nothing to run');return;}runText(s);}
 function loadInto(t){$('script').value=t;window.scrollTo({top:0,behavior:'smooth'});toast('Loaded into editor');}
 async function save(){let n=$('pname').value||'payload';
   await fetch('/save?name='+encodeURIComponent(n),{method:'POST',body:$('script').value});
   toast('Saved');loadSaved();}

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
   let arr=await (await fetch('/list')).json();
   if(!arr.length){$('saved').innerHTML='';return;}
   let h='<div class="acc open"><button class="acc-h" onclick="this.parentNode.classList.toggle(\'open\')">'+
     '<span class="chev">&#9654;</span><span>Your saved payloads</span><span class="cnt">'+arr.length+'</span></button><div class="acc-b">';
   arr.forEach(n=>{h+='<div class="preset"><div class="meta"><div class="n">'+n+'</div></div>'+
     '<button class="mini run" onclick="runFile(\''+n+'\')">Run</button>'+
     '<button class="mini sec" onclick="editFile(\''+n+'\')">Edit</button>'+
     '<button class="mini dan" onclick="delFile(\''+n+'\')">Del</button></div>';});
   h+='</div></div>';$('saved').innerHTML=h;}
 async function runFile(n){let r=await fetch('/runfile?name='+encodeURIComponent(n),{method:'POST'});
   let m=await r.text();toast(m=='running'?'Sent to target ✓':'Not connected — pair Bluetooth first');}
 async function editFile(n){$('pname').value=n;$('script').value=await (await fetch('/load?name='+encodeURIComponent(n))).text();
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
