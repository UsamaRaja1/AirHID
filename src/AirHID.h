#pragma once
/**
 * AirHID.h
 *
 * Composite BLE HID device for ESP32 — one Bluetooth connection, one HID
 * service, many report types.
 *
 * AirHID itself owns nothing HID-specific. It owns the BLE stack, the GATT
 * server, the HID service, advertising, pairing, connection state, power
 * management, and the single send path. Everything a host actually sees as
 * "a mouse" or "a keyboard" is supplied by an AirHIDReport subclass that is
 * registered before begin().
 *
 * Built on NimBLE-Arduino >= 2.4.0. Deprecated NimBLE APIs are not used —
 * in particular NimBLEHIDDevice::startServices() is gone: since 2.4 the GATT
 * server starts its services itself when advertising starts.
 *
 * Copyright (c) 2026. Licensed under the Apache License, Version 2.0.
 * Derived from HijelHID_BLEMouse and HijelHID_BLEKeyboard (c) 2026 Hijel,
 * also Apache-2.0. See NOTICE.
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLECharacteristic.h>
#include <NimBLEHIDDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string>

// ---------------------------------------------------------------------------
// Build-time limits. Override before including this header if you need more.
// ---------------------------------------------------------------------------

// Maximum number of AirHIDReport objects that can be registered.
#ifndef AIRHID_MAX_REPORTS
#define AIRHID_MAX_REPORTS 6
#endif

// Maximum number of report channels (input + output) across all reports.
// A keyboard uses two (input + LED output); most reports use one.
#ifndef AIRHID_MAX_CHANNELS
#define AIRHID_MAX_CHANNELS 10
#endif

// Scratch buffer for the assembled composite report map, in bytes.
// Keyboard + consumer + mouse is ~161 bytes; 512 leaves room for a gamepad.
#ifndef AIRHID_REPORT_MAP_SIZE
#define AIRHID_REPORT_MAP_SIZE 512
#endif

// Largest single HID report payload, in bytes. Bounds the zero-report buffer
// used for the Windows resume prime.
#ifndef AIRHID_MAX_REPORT_LEN
#define AIRHID_MAX_REPORT_LEN 16
#endif

// ---------------------------------------------------------------------------
// Timing and connection constants
// ---------------------------------------------------------------------------

// Inactivity before the radio requests slave latency (power saving).
#define AIRHID_IDLE_THRESHOLD_MS   5000
// Connection events the peripheral may skip while idle.
#define AIRHID_IDLE_LATENCY          80
// Connection interval used while idle, in 1.25 ms units (7.5 ms).
#define AIRHID_IDLE_INTERVAL          6
// Relaxed interval used during pairing, in 1.25 ms units (40 ms).
#define AIRHID_CONNECT_INTERVAL      32
// Supervision timeout, in 10 ms units (3000 ms).
#define AIRHID_CONN_TIMEOUT         300

// Windows selectively suspends idle BLE HID devices and drops the first
// notification of the resume handshake. After this much silence, notify()
// sends a zero report first to absorb the drop.
#define AIRHID_WINDOWS_PRIME_MS     800
#define AIRHID_PRIME_SETTLE_MS       50

// Settle time after re-authentication before the first report is sent.
#define AIRHID_AFTER_WAKE_SETTLE_MS 250
// Total time budget for afterWake().
#define AIRHID_AFTER_WAKE_TIMEOUT_MS 15000

// notify() retries while the controller's buffers are full.
#define AIRHID_NOTIFY_RETRIES        20

// The BLE advertising packet leaves 29 bytes for the device name.
#define AIRHID_MAX_DEVICE_NAME_LEN   29
// Bluetooth Core Spec maximum GATT attribute length.
#define AIRHID_MAX_MANUFACTURER_LEN 512

// Default PnP identity: source = Bluetooth SIG (0x01), VID = Espressif (0x02E5).
#define AIRHID_PNP_SOURCE          0x01
#define AIRHID_PNP_VID           0x02E5
#define AIRHID_PNP_PID           0x0001
#define AIRHID_PNP_VERSION       0x0100

// GAP appearance values (Bluetooth SIG assigned numbers).
#define AIRHID_APPEARANCE_GENERIC  0x03C0
#define AIRHID_APPEARANCE_KEYBOARD 0x03C1
#define AIRHID_APPEARANCE_MOUSE    0x03C2
#define AIRHID_APPEARANCE_GAMEPAD  0x03C4

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/**
 * Serial debug verbosity.
 *
 * `Off`     — silent. [Default]
 * `Normal`  — lifecycle, connection, and pairing events.
 * `Verbose` — all of the above plus every report sent.
 */
enum class HIDLogLevel : uint8_t {
    Off     = 0,
    Normal  = 1,
    Verbose = 2,
};

/**
 * BLE pairing security mode.
 *
 * `JustWorks` — encrypted, no passcode. [Default]
 * `Passkey`   — Numeric Comparison. Register `setPasskeyCallback()` to display
 *               the 6-digit code, which the user confirms on the host.
 */
enum class HIDSecurity : uint8_t {
    JustWorks = 0,
    Passkey   = 1,
};

/**
 * Report rate / BLE connection interval, in 1.25 ms units.
 *
 * `Hz25` — 40 ms, `Hz50` — 20 ms, `Hz100` — 10 ms, `Hz125` — 7.5 ms [Default].
 * 7.5 ms is the BLE minimum connection interval.
 */
enum class HIDRate : uint16_t {
    Hz25  = 32,
    Hz50  = 16,
    Hz100 = 8,
    Hz125 = 6,
};

class AirHID;

// ---------------------------------------------------------------------------
// AirHIDReport
// ---------------------------------------------------------------------------

/**
 * Base class for anything that contributes a HID report to the composite
 * device — a mouse, a keyboard, consumer control, system control, a gamepad.
 *
 * A report supplies descriptor bytes and produces payloads. It never touches
 * NimBLE: everything goes out through `_core->notify()`. This is what lets one
 * BLE connection carry any mix of report types.
 *
 * Register with `AirHID::addReport()` before `begin()`. Report IDs are handed
 * out by the core in registration order, so register in a stable order and the
 * IDs stay stable across builds.
 *
 * Lifecycle, in order:
 *   1. `onAttach()`        — claim report IDs and channels
 *   2. `buildDescriptor()` — emit descriptor bytes for the composite map
 *   3. `onConnect()`       — a host authenticated
 *   4. `onTick()`          — once per report interval while paired
 *   5. `onOutput()`        — the host wrote one of our output channels
 *   6. `onDisconnect()`    — connection gone; clear held state
 */
class AirHIDReport {
    friend class AirHID;

public:
    virtual ~AirHIDReport() = default;

    /** Short name used in log output. */
    virtual const char* reportName() const { return "report"; }

protected:
    /**
     * Write this report's HID descriptor bytes into `out`, at most `maxLen`.
     * Return the number of bytes written, or 0 to abort `begin()`.
     *
     * Every collection MUST carry a Report ID — a composite map with an
     * un-IDed collection is malformed and the host will silently ignore it.
     * Use the ID claimed in `onAttach()`.
     */
    virtual uint16_t buildDescriptor(uint8_t* out, uint16_t maxLen) = 0;

    /**
     * Claim report IDs and register channels. `_core` is valid here.
     * Called during `begin()`, before the descriptor is assembled.
     *
     *   _reportId = _core->claimReportId();
     *   _core->registerInput(_reportId, this);
     */
    virtual void onAttach() {}

    /**
     * A host has connected and authenticated. Reset state here.
     *
     * Runs on the NimBLE task — do NOT call `notify()` from here: it takes the
     * send mutex and may block on the Windows prime. The core has already
     * armed priming, so the first real report primes itself.
     */
    virtual void onConnect() {}

    /** The connection is gone. Clear held keys, buttons, and accumulators. */
    virtual void onDisconnect() {}

    /**
     * The host wrote one of the output channels registered in `onAttach()`.
     * Runs on the NimBLE task — keep it short and do not block.
     */
    virtual void onOutput(uint8_t reportId, const uint8_t* data, size_t len) {
        (void)reportId; (void)data; (void)len;
    }

    /**
     * Called once per report interval from the core's send task, only while
     * paired. Reports that stream state (a mouse) send from here; reports that
     * are purely event-driven (a keyboard) can ignore it.
     */
    virtual void onTick() {}

    /** The owning core. Valid from `onAttach()` onwards. */
    AirHID* _core = nullptr;
};

// ---------------------------------------------------------------------------
// AirHID
// ---------------------------------------------------------------------------

/**
 * The composite BLE HID device.
 *
 *   AirHID hid("Air HID", "Hijel");
 *
 *   void setup() {
 *       hid.addReport(keyboard);   // claims report ID 1
 *       hid.addReport(mouse);      // claims report ID 2
 *       hid.begin();
 *   }
 */
class AirHID {
public:

    // -----------------------------------------------------------------------
    // Constructor
    // -----------------------------------------------------------------------

    /**
     * `deviceName`   — name shown when pairing. Truncated to 29 chars.
     * `manufacturer` — Device Information Service string. Truncated to 512.
     * `batteryLevel` — initial battery percent, clamped to 1–100.
     */
    AirHID(const char* deviceName   = "AirHID",
           const char* manufacturer = "AirHID",
           uint8_t     batteryLevel = 100);

    // -----------------------------------------------------------------------
    // Report registration — before begin()
    // -----------------------------------------------------------------------

    /**
     * Register a report with the device. Call before `begin()`.
     *
     * Registration order determines report ID assignment, so keep it stable:
     * changing the order changes the descriptor, which invalidates every
     * existing host pairing.
     *
     * Returns false if the report is already registered, `AIRHID_MAX_REPORTS`
     * is exhausted, or `begin()` has already run.
     */
    bool addReport(AirHIDReport& report);

    /** Number of registered reports. */
    uint8_t reportCount() const { return _reportCount; }

    // -----------------------------------------------------------------------
    // Called by AirHIDReport::onAttach() — not intended for sketches
    // -----------------------------------------------------------------------

    /** Allocate the next free HID report ID (1, 2, 3, …). Returns 0 if exhausted. */
    uint8_t claimReportId();

    /** Register an input channel (device → host) for a claimed report ID. */
    bool registerInput(uint8_t reportId, AirHIDReport* owner);

    /** Register an output channel (host → device) for a claimed report ID. */
    bool registerOutput(uint8_t reportId, AirHIDReport* owner);

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * Initialise the BLE stack, assemble the composite report map, create the
     * GATT services, and start advertising. Call once in `setup()` after all
     * reports are registered.
     *
     * Returns false if no reports are registered, the descriptor did not fit,
     * NimBLE failed to start, or `kill()` was called earlier.
     */
    bool begin();

    /**
     * Stop advertising and disconnect. The BLE stack and all GATT objects stay
     * in memory, so `begin()` restarts quickly. Use for light sleep.
     */
    void end();

    /**
     * Permanently shut down and free the BLE stack. `begin()` is refused after
     * this. A small one-time leak in the ESP-IDF NimBLE port is unavoidable.
     */
    void kill();

    /**
     * Prepare for deep sleep: stop the send task, drop the radio to idle
     * parameters, and deinitialise NimBLE. Bonds are preserved.
     * Call immediately before `esp_deep_sleep_start()`.
     */
    void beforeSleep();

    /**
     * Reinitialise after waking, then wait (bounded by
     * `AIRHID_AFTER_WAKE_TIMEOUT_MS`) for a bonded host to reconnect and
     * re-encrypt. Check `isPaired()` afterwards to confirm success.
     */
    void afterWake();

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    /** Serial debug verbosity. Safe to call at any time. */
    void setLogLevel(HIDLogLevel level);

    /** Pairing security mode. Takes effect on the next `begin()`. */
    void setSecurityMode(HIDSecurity mode);

    /** Called with the 6-digit Numeric Comparison code in Passkey mode. */
    void setPasskeyCallback(void (*cb)(uint32_t passkey));

    /** Called with `true` on successful pairing, `false` on failure. */
    void onPairingComplete(void (*cb)(bool success));

    /**
     * Advertise from a random static address instead of the fixed public MAC.
     * Bypasses stale bond caches on hosts (notably Android) that refuse to
     * re-pair after a failed attempt. The address changes on every power
     * cycle, so bonded hosts must re-pair after a reboot.
     * Takes effect on the next `begin()`.
     */
    void setRandomAddress(bool enable);

    /**
     * GAP appearance advertised to the host. Default is
     * `AIRHID_APPEARANCE_KEYBOARD` (0x03C1) — hosts use it only to pick an
     * icon and a pairing flow, not to decide which reports they accept.
     * Takes effect on the next `begin()`.
     */
    void setAppearance(uint16_t appearance);

    /**
     * PnP identity in the Device Information Service. Defaults to Bluetooth
     * SIG source with Espressif's vendor ID (0x02E5).
     * Takes effect on the next `begin()`.
     *
     * Changing this after hosts have bonded can leave them holding a stale
     * cached identity — pick one and keep it.
     */
    void setPnp(uint8_t sig, uint16_t vid, uint16_t pid, uint16_t version);

    /** Report rate and active connection interval. Safe to call at any time. */
    void setUpdateRate(HIDRate rate);

    /**
     * BLE transmit power, 1–8, mapping to -12 dBm through +9 dBm in 3 dBm
     * steps. Values outside 1–8 are clamped. Safe to call at any time; the
     * value is re-applied on every `begin()`.
     */
    void setTxPower(uint8_t level);

    /** Battery percentage, 1–100. Notifies the host if connected. */
    void setBatteryLevel(uint8_t percent);

    // -----------------------------------------------------------------------
    // Status
    // -----------------------------------------------------------------------

    /** True while a host is connected at the GAP layer. */
    bool isConnected() const { return _connected; }

    /**
     * True once the host is connected AND authenticated. This is the
     * ready-to-send signal — `isConnected()` goes true briefly before the
     * encryption handshake completes.
     */
    bool isPaired() const { return _authenticated; }

    /** True if at least one bond is stored in NVS. */
    bool isBonded() const;

    /**
     * Erase all stored bonds, forcing a re-pair on the next connection.
     *
     * Note: after a descriptor change, this alone is not enough — hosts cache
     * the report map against the bond, so the device must also be removed on
     * the host side.
     */
    void clearBonds();

    /** Milliseconds since the last input event from any report. */
    uint32_t getIdleTime() const { return millis() - _lastInputTime; }

    /** Current report interval in milliseconds, derived from `setUpdateRate()`. */
    uint16_t reportIntervalMs() const;

    // -----------------------------------------------------------------------
    // Send path — the only route from a report to the host
    // -----------------------------------------------------------------------

    /**
     * Send one HID report. Serialised against every other report with a mutex,
     * so it is safe to call from the send task and the Arduino loop task
     * concurrently.
     *
     * Blocks briefly, and may block for `AIRHID_PRIME_SETTLE_MS` when the
     * Windows resume prime fires. NOT safe to call from an ISR.
     *
     * Returns false if not paired, the report ID has no input channel, `len`
     * exceeds `AIRHID_MAX_REPORT_LEN`, or the host never accepted the
     * notification.
     */
    bool notify(uint8_t reportId, const uint8_t* data, size_t len);

    /**
     * Stamp an input event, restarting the idle timer. ISR-safe — it touches
     * no BLE state; the send task performs the actual connection parameter
     * change on the next tick.
     */
    void markInput() { _lastInputTime = millis(); }

    // -----------------------------------------------------------------------
    // Internal — called from NimBLE callback objects. Do not call directly.
    // -----------------------------------------------------------------------
    void _onConnect(uint16_t connHandle);
    void _onDisconnect(int reason);
    void _onAuthComplete(NimBLEConnInfo& connInfo);
    void _onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t passkey);
    void _onCharWrite(NimBLECharacteristic* chr);
    void _log(HIDLogLevel level, const char* fmt, ...) const;

private:

    // -----------------------------------------------------------------------
    // Internal state
    // -----------------------------------------------------------------------

    // BLE stack lifecycle, independent of whether a host is connected.
    enum class State : uint8_t { Stopped, Running, Killed };

    // Activity state of the current connection. Drives idle power saving.
    enum class ConnState : uint8_t { Disconnected, Connecting, Active, Idle };

    // One report characteristic: an input or output channel owned by a report.
    struct Channel {
        uint8_t               id     = 0;
        bool                  output = false;
        AirHIDReport*         owner  = nullptr;
        NimBLECharacteristic* chr    = nullptr;
    };

    // -----------------------------------------------------------------------
    // NimBLE callback adapters
    //
    // Held by value so NimBLE never owns them. The server's setCallbacks() is
    // called with deleteCallbacks = false for the same reason — its default is
    // true, which would hand a member's address to delete.
    // -----------------------------------------------------------------------
    class ServerCallbacks : public NimBLEServerCallbacks {
    public:
        explicit ServerCallbacks(AirHID* parent) : _parent(parent) {}
        void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override;
        void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override;
        void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;
        void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t passkey) override;
    private:
        AirHID* _parent;
    };

    class OutputCallbacks : public NimBLECharacteristicCallbacks {
    public:
        explicit OutputCallbacks(AirHID* parent) : _parent(parent) {}
        void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override;
    private:
        AirHID* _parent;
    };

    ServerCallbacks _serverCb;
    OutputCallbacks _outputCb;

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------
    std::string _deviceName;
    std::string _manufacturer;
    uint8_t     _batteryLevel;
    HIDSecurity _securityMode  = HIDSecurity::JustWorks;
    HIDRate     _updateRate    = HIDRate::Hz125;
    HIDLogLevel _logLevel      = HIDLogLevel::Off;
    uint8_t     _txPowerLevel  = 8;
    uint16_t    _appearance    = AIRHID_APPEARANCE_KEYBOARD;
    bool        _randomAddress = false;

    uint8_t  _pnpSource  = AIRHID_PNP_SOURCE;
    uint16_t _pnpVid     = AIRHID_PNP_VID;
    uint16_t _pnpPid     = AIRHID_PNP_PID;
    uint16_t _pnpVersion = AIRHID_PNP_VERSION;

    void (*_cbPasskey)(uint32_t) = nullptr;
    void (*_cbPairing)(bool)     = nullptr;

    // Deferred constructor warnings — printed in begin(), after Serial.begin().
    bool _nameTruncated = false;
    bool _mfrTruncated  = false;
    bool _batClamped    = false;

    // -----------------------------------------------------------------------
    // BLE objects
    //
    // _pServer is owned by the NimBLE singleton. _pHID is allocated here but
    // never deleted: deinit(true) reclaims memory overlapping it, so deleting
    // afterwards is a double free. Null the pointer instead.
    // -----------------------------------------------------------------------
    NimBLEServer*    _pServer = nullptr;
    NimBLEHIDDevice* _pHID    = nullptr;

    // -----------------------------------------------------------------------
    // Reports and channels
    // -----------------------------------------------------------------------
    AirHIDReport* _reports[AIRHID_MAX_REPORTS] = {};
    uint8_t       _reportCount   = 0;
    Channel       _channels[AIRHID_MAX_CHANNELS];
    uint8_t       _channelCount  = 0;
    uint8_t       _nextReportId  = 1;

    // -----------------------------------------------------------------------
    // Runtime state
    // -----------------------------------------------------------------------
    State             _state         = State::Stopped;
    volatile bool     _connected     = false;
    volatile bool     _authenticated = false;
    volatile uint16_t _connHandle    = BLE_HS_CONN_HANDLE_NONE;
    volatile ConnState _connState    = ConnState::Disconnected;
    volatile uint32_t _lastInputTime = 0;
    volatile uint32_t _lastReportMs  = 0;
    volatile bool     _primingNeeded = true;

    // -----------------------------------------------------------------------
    // Send task
    // -----------------------------------------------------------------------
    TaskHandle_t      _sendTaskHandle = nullptr;
    volatile bool     _stopSendTask   = false;
    SemaphoreHandle_t _notifyMutex    = nullptr;
    static void       _sendTaskEntry(void* arg);
    void              _sendTask();
    void              _startSendTask();
    void              _stopSendTaskAndWait();

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------
    bool     _initStack();
    void     _configureSecurity();
    bool     _buildReportMap();
    bool     _createChannels();
    void     _configureAdvertising();
    void     _applyTxPower();
    void     _serviceIdle();
    void     _updateConnParams(uint16_t minInterval, uint16_t maxInterval,
                               uint16_t latency, uint16_t timeout);
    void     _resetConnectionState();
    Channel* _findChannel(uint8_t reportId, bool output);
    Channel* _findChannelByChr(NimBLECharacteristic* chr);
};

// ---------------------------------------------------------------------------
// Report types live in their own headers — include the ones you need:
//
//   #include <AirHID.h>
//   #include <AirMouse.h>
//   #include <AirKeyboard.h>
//
// This header deliberately does not pull them in. The core knows nothing about
// any particular report type, and a sketch that only wants a mouse should not
// compile a keyboard.
// ---------------------------------------------------------------------------
