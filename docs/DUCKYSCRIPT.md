# DuckyScript reference (this firmware's dialect)

This firmware implements a practical subset of DuckyScript. One command per line.

## Commands

| Command | Description |
|---|---|
| `REM <text>` | Comment. Ignored. |
| `DELAY <ms>` | Pause for `<ms>` milliseconds. |
| `DEFAULT_DELAY <ms>` | Pause automatically inserted after *every* command. `DEFAULTDELAY` also accepted. |
| `STRING <text>` | Type the literal text. |
| `STRINGLN <text>` | Type the text, then press Enter. |
| `REPEAT <n>` | Repeat the **previous** line `<n>` more times. |
| `<KEY>` / `<MOD> <KEY>` | Press a key or a modifier+key combo (see below). |

## Modifiers

`CTRL` (`CONTROL`), `SHIFT`, `ALT`, `GUI` (`WINDOWS`, `WIN`, `COMMAND`, `META`).

Chain them before the final key, separated by spaces:

```
GUI r
CTRL ALT DELETE
CTRL SHIFT ESC
GUI SPACE
```

A bare modifier on its own line presses just that key (e.g. `GUI` = tap the Windows/Command key).

## Named keys

`ENTER`/`RETURN`, `ESC`/`ESCAPE`, `BACKSPACE`, `TAB`, `SPACE`, `CAPSLOCK`,
`DELETE`/`DEL`, `INSERT`/`INS`, `HOME`, `END`, `PAGEUP`/`PGUP`, `PAGEDOWN`/`PGDN`,
`UP`, `DOWN`, `LEFT`, `RIGHT`, `MENU`/`APP`, `PRINTSCREEN`, `PAUSE`, `NUMLOCK`,
`SCROLLLOCK`, and `F1`–`F12`.

## Notes / limitations

- Keystrokes are sent over **Bluetooth LE**, so the target must be paired with
  the board (advertised as `ESP32 Keyboard`) before running a payload.
- The HID keymap is **US layout**. On a target set to another keyboard layout,
  some symbols will differ.
- Use spaces (not dashes) to separate modifiers: `CTRL ALT DEL`, not `CTRL-ALT-DEL`.
