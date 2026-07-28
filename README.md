# ESP32-WROOM-32E BLE Rubber Ducky

Turn an **ESP32-WROOM-32E** into a wireless keystroke-injection tool ("rubber
ducky") that runs [DuckyScript](docs/DUCKYSCRIPT.md) payloads. Payloads are
written, saved, and launched from a web UI that the board itself hosts.

> ⚠️ **For authorized use only.** Build and use this only against computers you
> own or have explicit written permission to test. Unauthorized keystroke
> injection is illegal in most jurisdictions.

---

## Read this first: the WROOM-32E can't do USB HID

Most "rubber ducky" guides assume a device with **native USB** (a USB Rubber
Ducky, a Digispark/ATmega32u4, or an ESP32-**S2/S3**). The classic
**ESP32-WROOM-32E has no native USB peripheral** — its micro-USB port is only a
USB-to-serial bridge (CP2102/CH340) used for flashing and the serial monitor.

So this board **cannot** be plugged into a target and enumerate as a USB
keyboard. What it *can* do is act as a **Bluetooth LE HID keyboard**: you pair
it with the target once, and then it injects keystrokes wirelessly. That is what
this firmware does, and it's what the popular "ESP32 rubber ducky" projects
actually are.

| Board | USB HID (plug & type) | BLE HID (this project) |
|---|---|---|
| ESP32-WROOM-32E (yours) | ❌ no native USB | ✅ yes |
| ESP32-S2 / S3 | ✅ | ✅ |
| Digispark / USB Rubber Ducky | ✅ | ❌ |

If you specifically need the "plug the USB cable in and it types instantly with
no pairing" behavior, you need an S2/S3 or a dedicated HID board — no firmware
can add a USB device controller the WROOM-32E doesn't physically have.

---

## How it works

```
                WiFi AP "DuckyESP32"              Bluetooth LE
  [ phone/laptop ] ───── web UI ─────► [ ESP32 ] ═══ HID keyboard ═══► [ target PC ]
     write/run DuckyScript          192.168.4.1        (paired once)
```

1. On boot the board starts a WiFi access point **`DuckyESP32`** and advertises
   over Bluetooth as **`ESP32 Keyboard`**.
2. You pair the **target computer** with `ESP32 Keyboard` (once).
3. From any device, join the `DuckyESP32` WiFi and open **http://192.168.4.1**
   to write, save, and run payloads.

## Hardware

- An ESP32-WROOM-32E dev board (e.g. ESP32 DevKitC / NodeMCU-32S).
- A USB cable (data-capable) for flashing and power.
- A target that supports **Bluetooth LE keyboards** (Windows 10+, macOS, Linux
  with BlueZ, Android, iOS).

No wiring is required — it's all in software.

## Build & flash

You need the [Arduino ESP32 core](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)
and two libraries:

- **NimBLE-Arduino** (lightweight BLE stack)
- **ESP32-BLE-Keyboard** (<https://github.com/T-vK/ESP32-BLE-Keyboard>)

### Option A — PlatformIO (recommended)

Dependencies are declared in [`platformio.ini`](platformio.ini), so this is one
command:

```bash
pio run -t upload        # compile + flash
pio device monitor       # 115200 baud — shows the AP IP and BLE status
```

### Option B — Arduino IDE

1. Install the **esp32 by Espressif Systems** board package (Boards Manager).
2. Install libraries: **NimBLE-Arduino** (Library Manager) and
   **ESP32-BLE-Keyboard** (download the ZIP from the repo above →
   *Sketch ▸ Include Library ▸ Add .ZIP Library*).
3. Enable NimBLE: open `BleKeyboard.h` in the library and uncomment
   `#define USE_NIMBLE` (this project relies on it so BLE + WiFi fit in RAM).
4. Create a sketch folder `RubberDuckyESP32/` and copy
   [`src/main.cpp`](src/main.cpp) into it as `RubberDuckyESP32.ino`.
5. Select board **ESP32 Dev Module**, pick the serial port, and click Upload.

## Usage

1. Flash the board and open the serial monitor to confirm it printed the AP IP.
2. **Pair the target:** on the target PC, go to Bluetooth settings, add a
   device, and select **`ESP32 Keyboard`** (or whatever name you set).
3. **Open the control panel:** on your phone/laptop, join WiFi `DuckyESP32`
   (password `quack1234`) and browse to **http://192.168.4.1**.

## The web UI

The control panel is a single page with a navbar of three tabs:

- **Scripts** — paste a DuckyScript into the box and press **Go** to run it
  immediately. Below that is a **preset library** with a **Windows / macOS
  toggle** — the presets are OS-specific (Windows uses the Run dialog +
  `cmd`/PowerShell; macOS uses Spotlight + Terminal `open`/`say`/`afplay`), so
  pick the toggle that matches your target. Presets are grouped by category
  (Demos, YouTube & Media, Sounds, Pranks, Utilities, System); each has **Run**
  (execute now) and **Load** (drop it into the editor to tweak). Your own
  **saved payloads** also appear here with Run / Edit / Del.
- **Connection** — live status: BLE connected badge, the Bluetooth pairing
  name, the WiFi SSID, the web address, and how many WiFi clients are joined.
- **Settings** — change the **WiFi SSID/password** and the **Bluetooth name**
  (saved to flash; the board reboots to apply them), plus **theme** (dark/light)
  and **accent color** for the UI.

The green **connected** badge appears once the target is paired over Bluetooth.

## Configuration

Everything is configurable at runtime from the **Settings** tab and stored in
flash (NVS), so it survives reflashing the same firmware. The values below are
just the first-boot defaults, defined near the top of
[`src/main.cpp`](src/main.cpp) in `loadSettings()`:

```cpp
cfgSsid    = prefs.getString("ssid",    "DuckyESP32");
cfgPass    = prefs.getString("pass",    "quack1234");     // must be >= 8 chars
cfgBleName = prefs.getString("blename", "ESP32 Keyboard");
```

> Changing the SSID or Bluetooth name reboots the board. Rejoin the new WiFi,
> and if you renamed the Bluetooth device, "forget" the old one on the target
> and pair the new name.

## Writing payloads

See the [DuckyScript reference](docs/DUCKYSCRIPT.md) for the supported commands,
modifiers, and named keys.

The **preset library** in the Scripts tab is baked into the firmware (defined in
`PRESETS_JSON` in [`src/main.cpp`](src/main.cpp)), split into `"windows"` and
`"mac"` sets — add or edit entries in either to grow it. Both ship with harmless
demos and pranks such as a **fake update** page, rickroll, text-to-speech, sound
effects, and system-info commands, each written with that OS's native commands.

Standalone example payloads also live in [`payloads/`](payloads/):

- `01-hello-notepad-windows.txt` — opens Notepad and types a message
- `02-spotlight-note-macos.txt` — opens TextEdit via Spotlight
- `03-key-combos-demo.txt` — modifier combos, arrows, and `REPEAT`

## Troubleshooting

| Symptom | Fix |
|---|---|
| Web page loads but status stays **not connected** | The target isn't paired over Bluetooth. Pair with `ESP32 Keyboard` first. |
| Compile fails / out of memory | Make sure NimBLE is enabled (`USE_NIMBLE`); the default BLE stack is too big alongside WiFi. |
| Wrong symbols typed | HID keymap is US layout; set the target to US, or adjust the script. |
| Won't pair again after re-flash | Remove/"forget" the old `ESP32 Keyboard` on the target, then pair again. |
| WiFi + BLE unstable | Coexistence is memory-tight on the classic ESP32; keep payloads modest and avoid huge web pages. |

## Legal & ethical use

This is a security-education and authorized-testing tool. Using it against
devices you do not own or lack written permission to test may be a crime.
You are responsible for how you use it.
