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
| 2 | Mouse report | planned |
| 3 | Keyboard report (+ LED output) | planned |
| 4 | Consumer control, system control, gamepad | planned |

Phase 1 ships the core and the extension point. There are no built-in report
types yet — `examples/MinimalReport` defines one in the sketch to exercise the
whole path end to end.

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

```cpp
#include <AirHID.h>

AirHID hid("Air HID", "Acme");

void setup() {
    hid.setLogLevel(HIDLogLevel::Normal);

    hid.addReport(keyboard);   // claims report ID 1
    hid.addReport(mouse);      // claims report ID 2

    hid.begin();
}
```

Registration order determines report ID assignment. Keep it stable — changing
the order changes the descriptor, which invalidates every existing pairing.

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
