# AirHID

Composite Bluetooth Low Energy HID library for ESP32 — mouse, keyboard, and
more over **one** connection.

Most ESP32 BLE HID libraries each stand up a complete, exclusive HID device:
their own BLE stack init, their own HID service, their own report map, their
own advertising. Two of them cannot coexist in one sketch. AirHID inverts that.
The core owns all the BLE infrastructure exactly once, and report types plug
into it.

**Requires** NimBLE-Arduino 2.4.0+ and ESP32 Arduino Core 3.x. Deprecated
NimBLE APIs are not used.

---

## Status

| Phase | Scope | State |
|---|---|---|
| **1** | Library skeleton, `AirHID` core, generic report registry | **complete** |
| **2** | `AirKeyboard` (+ LED output), `AirMouse`, composite reports | **complete** |
| **3** | `AirConsumer` — media, volume, brightness, browser keys | **complete** |
| 4 | System control, gamepad | planned |

Media keys are in `AirConsumer`, not `AirKeyboard` — they are a separate HID
collection, so they get their own report type. `examples/MinimalReport` writes
a System Control report by hand and doubles as the reference for building your
own report type.

---

## What it can do

Three report types over one pairing. The host sees a keyboard, a mouse, and a
media controller at the same time.

### Keyboard — `AirKeyboard`

144 keycodes: everything on a full 104/105-key board.

| Group | Coverage |
|---|---|
| Typing | A–Z, 0–9, all punctuation, Space, Enter, Tab, Backspace, Esc |
| Function | F1–**F24** |
| Navigation | Arrows, Home, End, Page Up/Down, Insert, Delete |
| Locks & system | Caps Lock, Num Lock, Scroll Lock, Print Screen, Pause, Power, Application (menu) |
| Numpad | 0–9, `.` `,` `/` `*` `-` `+` `=`, Enter |
| Modifiers | Left **and** right Ctrl, Shift, Alt, GUI (Win/Cmd) |
| International | Non-US backslash, INTL1–6, LANG1–5 (JIS, Hangul, …) |

**Any keyboard shortcut works.** Modifiers can be held as keys or passed as a
bitmask, so `Ctrl+C`, `Alt+Tab`, `Win+D`, `Cmd+Space`, and `Ctrl+Shift+Esc` are
all one call:

```cpp
keyboard.tap(KEY_C, KEY_MOD_LCTRL);                    // Ctrl+C
keyboard.tap(KEY_ESCAPE, KEY_MOD_LCTRL | KEY_MOD_LSHIFT);  // Ctrl+Shift+Esc

keyboard.press(KEY_LALT);                              // hold Alt across taps
keyboard.tap(KEY_TAB);
keyboard.tap(KEY_TAB);
keyboard.release(KEY_LALT);
```

Six non-modifier keys at once (6KRO). `print()` / `println()` type ASCII
strings directly. Host LED state (Caps/Num/Scroll) arrives via `onLEDChange()`.

### Mouse — `AirMouse`

Five buttons (left, right, middle, **back**, **forward**), relative movement,
scroll wheel. `moveTo()` spreads movement smoothly over a duration;
`click()`, `doubleClick()`, and `press()`/`release()` for dragging.

Horizontal scroll (AC Pan) is implemented but **off by default** — call
`mouse.setHorizontalScroll(true)` before `begin()`.

### Media and consumer keys — `AirConsumer`

41 named usages, plus any raw Consumer Page usage up to `0x3FF`.

| Group | Keys |
|---|---|
| Transport | play, pause, play/pause, stop, next, previous, fast-forward, rewind, record, eject, shuffle |
| Volume | up, down, mute, bass boost, bass ±, treble ± |
| Display | brightness up/down, backlight toggle |
| Keyboard backlight | up, down, toggle |
| Launch | media player, mail, calculator, file explorer, screensaver, task manager |
| Browser | search, home, back, forward, stop, refresh, bookmarks |
| Power | sleep, lock screen |

### From the core

Bonding that survives power cycles, Just Works or passkey pairing, battery
level reporting, TX power control (−12 to +9 dBm), automatic idle power saving,
and deep-sleep hooks.

### Host support caveats

- **Brightness and backlight keys are the least reliable.** Host support for
  consumer-page brightness over BLE HID varies widely — macOS commonly ignores
  it, Windows and Linux are inconsistent. Volume and transport work
  essentially everywhere.
- **`MEDIA_LOCK_SCREEN` is not a real lock.** The Consumer Page has no
  dedicated lock usage, so it sends AL Screen Saver and the behaviour is
  OS-dependent. For a genuine lock, `Win+L` from the keyboard is more reliable.
- **ASCII typing assumes a US layout** on the host. `print("@")` sends Shift+2,
  which produces `"` on a UK layout. Use explicit keycodes for other layouts.
- **One media key at a time** — a Consumer Page array-field limitation, not
  an AirHID one.

### Not built yet

System Control (real power off / sleep / wake) exists only as the hand-written
example in `examples/MinimalReport`, not as a library class. Gamepad, absolute
or touchpad pointing, and multi-host switching are unbuilt.

---

## How it works

`AirHID` owns the BLE stack, the GATT server, the HID service, the Device
Information and Battery services, advertising, pairing and bonding, connection
state, idle power management, and the single serialised send path. It contains
no knowledge of mice or keyboards.

Anything the host actually sees is an `AirHIDReport`. A report supplies
descriptor bytes and produces payloads; it never touches NimBLE. The core
concatenates every registered report's descriptor into one composite report
map, assigns report IDs, creates the characteristics, and routes traffic both
ways.

That is what makes report types additive: consumer control, system control, or
a gamepad drop in beside mouse and keyboard without touching the core.

```
AirHID (core)
 ├── BLE stack, server, HID service, advertising, pairing
 ├── composite report map assembly + report ID allocation
 ├── send task (idle power management, periodic reports)
 └── notify() — the only path to the host
      ↑
   AirHIDReport  ← mouse, keyboard, consumer, system, gamepad …
```

## Usage

Include the core plus whatever report types you need. `AirHID.h` deliberately
does not pull them in — a sketch that only wants a mouse should not compile a
keyboard.

```cpp
#include <AirHID.h>
#include <AirKeyboard.h>
#include <AirMouse.h>
#include <AirConsumer.h>

AirHID      hid("Air HID", "Acme");
AirKeyboard keyboard;
AirMouse    mouse;              // 5 buttons, no horizontal scroll
AirConsumer consumer;

void setup() {
    hid.setLogLevel(HIDLogLevel::Normal);

    hid.addReport(keyboard);    // claims report ID 1
    hid.addReport(mouse);       // claims report ID 2
    hid.addReport(consumer);    // claims report ID 3

    hid.begin();
}

void loop() {
    if (!hid.isPaired()) return;

    keyboard.println("Hello");
    keyboard.tap(KEY_TAB);

    mouse.moveTo(120, 0, 300);
    mouse.click(MouseButton::Left);

    consumer.tap(MEDIA_VOLUME_UP);
}
```

Registration order determines report ID assignment. Register new report types
at the **end** so existing IDs do not shift. Any change to the set or order
changes the descriptor, which invalidates every existing pairing.

### Report types

| Class | Header | Constants |
|---|---|---|
| `AirKeyboard` | `<AirKeyboard.h>` | `KEY_*`, `KEY_MOD_*` in `AirHIDKeys.h` |
| `AirMouse` | `<AirMouse.h>` | `MouseButton::` enum |
| `AirConsumer` | `<AirConsumer.h>` | `MEDIA_*` in `AirHIDMediaKeys.h` |

See [What it can do](#what-it-can-do) for the full key coverage.

`AirConsumer` holds one usage at a time — that is what the Consumer Page array
field allows. `press()` replaces whatever was held; `release()` sends `0x0000`.

### Threading, per class

`AirMouse`'s base layer (`move`, `addScroll`, `setButton`, `setButtons`) is
ISR-safe. Its macro layer (`moveTo`, `click`, `doubleClick`, `scroll`) blocks
and is loop-task only.

`AirKeyboard` and `AirConsumer` are **entirely** loop-task only — every method
blocks and calls into BLE. Do not call them from an interrupt.

## Writing a report

```cpp
class MyReport : public AirHIDReport {
public:
    const char* reportName() const override { return "my-report"; }

protected:
    void onAttach() override {
        _id = _core->claimReportId();
        _core->registerInput(_id, this);
        // _core->registerOutput(_id, this);  // for host -> device data
    }

    uint16_t buildDescriptor(uint8_t* out, uint16_t maxLen) override {
        // Emit one collection. It MUST carry Report ID `_id` — a composite
        // report map with an un-IDed collection is malformed, and the host
        // will silently ignore every report.
    }

    void onTick() override { /* called at the report interval while paired */ }
    void onDisconnect() override { /* clear held state */ }

private:
    uint8_t _id = 0;
};
```

Send with `_core->notify(_id, payload, len)`, and call `_core->markInput()` on
real input so the radio leaves its idle state.

### Hooks

| Hook | When | Context |
|---|---|---|
| `onAttach()` | during `begin()`, before the map is built | loop task |
| `buildDescriptor()` | during `begin()` | loop task |
| `onConnect()` | host authenticated | **NimBLE task** — do not `notify()` |
| `onTick()` | every report interval while paired | send task |
| `onOutput()` | host wrote an output channel | **NimBLE task** — keep it short |
| `onDisconnect()` | connection gone | NimBLE task |

## Threading

`notify()` is serialised by a mutex, so the send task and the Arduino loop task
can both send safely. It blocks, and it is **not** ISR-safe.

`markInput()` **is** ISR-safe — it writes a timestamp and nothing else. The
send task performs the actual connection parameter changes on its next tick, so
NimBLE is never called from an interrupt or a timer daemon.

## Power

After 5 seconds with no input, the core asks the host for slave latency,
dropping the radio from roughly 133 connection events per second to about 1.6.
The first input restores full rate. Hosts are not required to honour the
request, so this is best-effort.

`beforeSleep()` / `afterWake()` bracket deep sleep. `end()` / `begin()` is the
cheaper pause/resume for light sleep — it keeps the stack and GATT objects in
memory.

## Notes

- **Changing report layout invalidates pairings.** Hosts cache the report map
  against the bond. `clearBonds()` on the device is not enough — the device
  must also be removed on the host side. This bites hardest on Windows.
- Windows selectively suspends idle BLE HID devices and drops the first
  notification of the resume handshake. `notify()` sends a zero report first to
  absorb it after 800 ms of silence.
- PnP identity defaults to Bluetooth SIG source with Espressif's vendor ID
  (0x02E5). Override with `setPnp()` before `begin()`, and then leave it alone —
  changing it after hosts have bonded leaves them holding a stale identity.

## Licence

Apache 2.0. Derived from `HijelHID_BLEMouse` and `HijelHID_BLEKeyboard`
(c) 2026 Hijel, also Apache 2.0. See `NOTICE`.
