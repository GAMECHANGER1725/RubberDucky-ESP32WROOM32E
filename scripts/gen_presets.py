#!/usr/bin/env python3
"""Generate the PRESETS_JSON block for src/main.cpp.

All scripts are harmless and carry no 'rubber ducky' signature (no quack/duck/
'you have been ducked'). Every prank has a Windows and a macOS version.
Inspired by the harmless subset of the hak5 usbrubberducky prank library;
destructive payloads (MEMZ, fork bombs, BSOD, lockers, process killers) and
copyrighted lyric dumps are intentionally excluded.
"""
import json, re, pathlib

RICK = "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
LOFI = "https://www.youtube.com/watch?v=jfKfPfyJRdk"

# ---- Windows helpers -------------------------------------------------------
def w_run(cmd):
    return ["DELAY 700", "GUI r", "DELAY 400", "STRING " + cmd, "ENTER"]

def w_cmd(cmds):
    out = ["DELAY 700", "GUI r", "DELAY 400", "STRING cmd", "ENTER", "DELAY 800"]
    for c in cmds:
        out += ["STRING " + c, "ENTER"]
    return out

def w_ps(oneliner):
    return w_run('powershell -c "' + oneliner + '"')

# ---- macOS helpers ---------------------------------------------------------
def m_spot(app):
    return ["DELAY 700", "GUI SPACE", "DELAY 400", "STRING " + app, "ENTER"]

def m_term(cmds):
    out = ["DELAY 700", "GUI SPACE", "DELAY 400", "STRING Terminal", "ENTER", "DELAY 900"]
    for c in cmds:
        out += ["STRING " + c, "ENTER"]
    return out

def m_osa(script):
    return m_term(["osascript -e '" + script + "'"])

# ---- Category / item table -------------------------------------------------
# each item: (name, desc, windows_lines, mac_lines)
CATS = [
 ("Rickrolls & Music", [
   ("Classic rickroll", "Opens the famous video in the browser",
     w_run(RICK), m_term(['open "%s"' % RICK])),
   ("Fullscreen rickroll", "Opens the video and jumps to fullscreen",
     w_run(RICK) + ["DELAY 5000", "STRING f"],
     m_term(['open "%s"' % RICK]) + ["DELAY 5000", "STRING f"]),
   ("Max-volume surprise", "Cranks the volume, then opens the video",
     w_ps("$w=New-Object -ComObject WScript.Shell;1..50|%{$w.SendKeys([char]175)}") + ["DELAY 400"] + w_run(RICK),
     m_term(['osascript -e "set volume output volume 100"', 'open "%s"' % RICK])),
   ("Lofi radio", "Opens a 24/7 music live stream",
     w_run(LOFI), m_term(['open "%s"' % LOFI])),
 ]),
 ("Jump Scares & Sounds", [
   ("Loud alert tones", "Maxes volume and plays sharp beeps",
     w_ps("$w=New-Object -ComObject WScript.Shell;1..40|%{$w.SendKeys([char]175)};1..8|%{[console]::beep(1200,180)}"),
     m_term(['osascript -e "set volume output volume 100"', 'for i in 1 2 3 4 5 6; do afplay /System/Library/Sounds/Sosumi.aiff; done'])),
   ("Creepy whisper", "Speaks a spooky line out loud",
     w_ps("Add-Type -AssemblyName System.Speech;(New-Object System.Speech.Synthesis.SpeechSynthesizer).Speak('I can see you')"),
     m_term(['say -v Whisper "I can see you"'])),
   ("Robot voice", "Speaks a warning in a robotic voice",
     w_ps("Add-Type -AssemblyName System.Speech;(New-Object System.Speech.Synthesis.SpeechSynthesizer).Speak('You should lock your computer')"),
     m_term(['say -v Zarvox "you should lock your computer"'])),
   ("Beep melody", "Plays a short tune",
     w_ps("[console]::beep(523,250);[console]::beep(659,250);[console]::beep(784,250);[console]::beep(1046,400)"),
     m_term(['afplay /System/Library/Sounds/Glass.aiff', 'afplay /System/Library/Sounds/Ping.aiff'])),
   ("Single alert sound", "Plays one system alert",
     w_ps("[console]::beep(880,400)"),
     m_term(['afplay /System/Library/Sounds/Glass.aiff'])),
 ]),
 ("Popups & Fake Errors", [
   ("Single popup", "Shows one harmless message box",
     w_run("mshta \"javascript:alert('Gotcha');close()\""),
     m_osa('display dialog "Gotcha" buttons {"OK"}')),
   ("Popup barrage", "Pops five message boxes in a row",
     w_ps("Add-Type -AssemblyName System.Windows.Forms;1..5|%{[System.Windows.Forms.MessageBox]::Show('Are you sure?')}"),
     m_term(['for i in 1 2 3 4 5; do osascript -e \'display dialog "Are you sure?" buttons {"OK"}\'; done'])),
   ("Fake critical error", "Shows a scary-looking error dialog",
     w_ps("Add-Type -AssemblyName System.Windows.Forms;[System.Windows.Forms.MessageBox]::Show('A critical error occurred. Please contact your administrator.','System Error',0,16)"),
     m_osa('display dialog "A critical error occurred." buttons {"OK"} with icon stop')),
   ("Fake update banner", "Shows a system-style notification",
     w_ps("Add-Type -AssemblyName System.Windows.Forms;[System.Windows.Forms.MessageBox]::Show('An update is available.','Software Update')"),
     m_osa('display notification "An update is available." with title "Software Update"')),
 ]),
 ("Screen & Desktop Trolls", [
   ("Minimize everything", "Clears the screen to the desktop",
     ["DELAY 500", "GUI d"],
     ["DELAY 500", "GUI h"]),
   ("Hide desktop icons", "Makes the desktop icons disappear (reversible)",
     w_ps("$p='HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced';Set-ItemProperty $p HideIcons 1;Stop-Process -Name explorer -Force;Start-Process explorer"),
     m_term(['defaults write com.apple.finder CreateDesktop false', 'killall Finder'])),
   ("Restore desktop icons", "Undoes the hide-icons prank",
     w_ps("$p='HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced';Set-ItemProperty $p HideIcons 0;Stop-Process -Name explorer -Force;Start-Process explorer"),
     m_term(['defaults write com.apple.finder CreateDesktop true', 'killall Finder'])),
   ("Open the screenshot tool", "Launches the region screenshot capture",
     ["DELAY 500", "GUI SHIFT s"],
     ["DELAY 500", "GUI SHIFT 3"]),
 ]),
 ("Fake Hacker & Terminal", [
   ("Matrix rain", "Green scrolling text in a console",
     w_cmd(["color 0a", "tree"]),
     m_term(["printf '\\033[32m'", "ls -laR /System 2>/dev/null | head -n 400"])),
   ("Hacker typer", "Opens a fake 'hacking' website",
     w_run("https://hackertyper.net"),
     m_term(['open "https://hackertyper.net"'])),
   ("Fake breach terminal", "Red console flashing an access warning",
     w_cmd(["color 0c", "echo ACCESS GRANTED", "echo Downloading files...", "timeout 3", "echo Done."]),
     m_term(["printf '\\033[31m'", "echo ACCESS GRANTED", "echo Downloading files...", "sleep 2", "echo Done."])),
   ("Fake download progress", "Prints a fake progress readout",
     w_cmd(["echo Installing updates...", "echo 25%", "timeout 1", "echo 60%", "timeout 1", "echo 100% complete"]),
     m_term(["echo Installing updates...", "echo 25%", "sleep 1", "echo 60%", "sleep 1", "echo 100% complete"])),
 ]),
 ("Typing Trolls", [
   ("Creepy note", "Types an unsettling (then reassuring) note",
     w_run("notepad") + ["DEFAULT_DELAY 40", "DELAY 900", "STRING I know what you did last summer.", "ENTER", "STRING ...just kidding. Lock your screen next time!"],
     m_spot("TextEdit") + ["DEFAULT_DELAY 40", "DELAY 1400", "STRING I know what you did last summer.", "ENTER", "STRING ...just kidding. Lock your screen next time!"]),
   ("Slow ghost typing", "Types a message very slowly",
     w_run("notepad") + ["DEFAULT_DELAY 180", "DELAY 900", "STRING is anyone there?"],
     m_spot("TextEdit") + ["DEFAULT_DELAY 180", "DELAY 1400", "STRING is anyone there?"]),
   ("Caps Lock troll", "Toggles Caps Lock repeatedly",
     ["CAPSLOCK", "DELAY 250", "CAPSLOCK", "DELAY 250", "CAPSLOCK", "DELAY 250", "CAPSLOCK", "DELAY 250", "CAPSLOCK"],
     ["CAPSLOCK", "DELAY 250", "CAPSLOCK", "DELAY 250", "CAPSLOCK", "DELAY 250", "CAPSLOCK", "DELAY 250", "CAPSLOCK"]),
   ("Repeat note", "Fills a text editor with a repeated line",
     w_run("notepad") + ["DEFAULT_DELAY 30", "DELAY 900", "STRING look behind you... ", "REPEAT 20"],
     m_spot("TextEdit") + ["DEFAULT_DELAY 30", "DELAY 1400", "STRING look behind you... ", "REPEAT 20"]),
 ]),
 ("Apps & Websites", [
   ("Open Calculator", "Launches the calculator",
     w_run("calc"), m_spot("Calculator")),
   ("Open the camera app", "Opens the webcam app (harmless surprise)",
     w_run("microsoft.windows.camera:"), m_spot("Photo Booth")),
   ("Pointer Pointer", "Opens the silly pointerpointer.com site",
     w_run("http://pointerpointer.com"), m_term(['open "http://pointerpointer.com"'])),
   ("Open a website", "Opens a URL in the default browser (edit it)",
     w_run("https://example.com"), m_term(['open "https://example.com"'])),
   ("Open a text editor", "Opens a blank note",
     w_run("notepad"), m_spot("TextEdit")),
 ]),
 ("System", [
   ("Task list", "Shows running processes",
     ["DELAY 500", "CTRL SHIFT ESC"], m_spot("Activity Monitor")),
   ("Lock the screen", "Locks the computer",
     ["DELAY 500", "GUI l"], ["DELAY 500", "CTRL GUI q"]),
   ("System info", "Prints hardware / OS details",
     w_cmd(["systeminfo"]), m_term(["system_profiler SPHardwareDataType"])),
   ("Network info", "Shows the network configuration",
     w_cmd(["ipconfig /all"]), m_term(["ifconfig"])),
 ]),
]

def build(idx):
    groups = []
    for cat, items in CATS:
        groups.append({
            "cat": cat,
            "items": [{"name": n, "desc": d, "lines": lines[idx]} for (n, d, *lines) in items]
        })
    return groups

data = {"windows": build(0), "mac": build(1)}
body = json.dumps(data, indent=1, ensure_ascii=True)

block = 'static const char PRESETS_JSON[] PROGMEM = R"PRESETS(\n' + body + '\n)PRESETS";'

path = pathlib.Path(__file__).resolve().parent.parent / "src" / "main.cpp"
src = path.read_text()
new, n = re.subn(r'static const char PRESETS_JSON\[\] PROGMEM = R"PRESETS\(.*?\)PRESETS";',
                 lambda m: block, src, count=1, flags=re.S)
assert n == 1, "PRESETS_JSON block not found"
path.write_text(new)

# validate + report
parsed = json.loads(body)
for k in ("windows", "mac"):
    total = sum(len(g["items"]) for g in parsed[k])
    print(f"{k}: {total} presets, {len(parsed[k])} categories")
print("OK")
