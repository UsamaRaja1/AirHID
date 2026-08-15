/**
 * AirHIDCore.cpp
 *
 * BLE infrastructure for the composite HID device: stack lifecycle, GATT
 * server, HID service, report map assembly, advertising, pairing, connection
 * state, idle power management, and the single serialised send path.
 *
 * Nothing in this file knows what a mouse or a keyboard is.
 *
 * Copyright (c) 2026. Licensed under the Apache License, Version 2.0.
 * Derived from HijelHID_BLEMouse and HijelHID_BLEKeyboard (c) 2026 Hijel.
 */

#include "AirHID.h"
#include <stdarg.h>

// TX power scale 1–8 → dBm. NimBLE 2.4+ takes dBm directly via setPower();
// the older setPowerLevel(esp_power_level_t) path is not used.
static const int8_t kTxPowerTable[8] = { -12, -9, -6, -3, 0, 3, 6, 9 };

static bool allZero(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void AirHID::_log(HIDLogLevel level, const char* fmt, ...) const {
    if (_logLevel < level) return;
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print("[AirHID] ");
    Serial.println(buf);
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

AirHID::AirHID(const char* deviceName, const char* manufacturer, uint8_t batteryLevel)
    : _serverCb(this),
      _outputCb(this),
      _batteryLevel(100)
{
    _deviceName = (deviceName != nullptr && deviceName[0] != '\0') ? deviceName : "AirHID";
    if (_deviceName.length() > AIRHID_MAX_DEVICE_NAME_LEN) {
        _deviceName.resize(AIRHID_MAX_DEVICE_NAME_LEN);
        _nameTruncated = true;
    }

    _manufacturer = (manufacturer != nullptr && manufacturer[0] != '\0') ? manufacturer : "AirHID";
    if (_manufacturer.length() > AIRHID_MAX_MANUFACTURER_LEN) {
        _manufacturer.resize(AIRHID_MAX_MANUFACTURER_LEN);
        _mfrTruncated = true;
    }

    if (batteryLevel == 0) {
        _batteryLevel = 1;
        _batClamped   = true;
    } else if (batteryLevel > 100) {
        _batteryLevel = 100;
        _batClamped   = true;
    } else {
        _batteryLevel = batteryLevel;
    }
}

// ---------------------------------------------------------------------------
// Report registration
// ---------------------------------------------------------------------------

bool AirHID::addReport(AirHIDReport& report) {
    if (_state == State::Running) {
        _log(HIDLogLevel::Normal, "addReport() after begin() — ignored");
        return false;
    }
    if (_reportCount >= AIRHID_MAX_REPORTS) {
        _log(HIDLogLevel::Normal,
             "addReport() — limit of %d reached, raise AIRHID_MAX_REPORTS", AIRHID_MAX_REPORTS);
        return false;
    }
    for (uint8_t i = 0; i < _reportCount; i++) {
        if (_reports[i] == &report) return false;   // already registered
    }
    _reports[_reportCount++] = &report;
    return true;
}

uint8_t AirHID::claimReportId() {
    // Report ID 0 means "no report ID" in the HID spec, so IDs start at 1.
    if (_nextReportId == 0 || _nextReportId > 0xFF - 1) return 0;
    return _nextReportId++;
}

bool AirHID::registerInput(uint8_t reportId, AirHIDReport* owner) {
    if (reportId == 0 || _channelCount >= AIRHID_MAX_CHANNELS) return false;
    if (_findChannel(reportId, false) != nullptr) return false;
    Channel& ch = _channels[_channelCount++];
    ch.id     = reportId;
    ch.output = false;
    ch.owner  = owner;
    ch.chr    = nullptr;
    return true;
}

bool AirHID::registerOutput(uint8_t reportId, AirHIDReport* owner) {
    if (reportId == 0 || _channelCount >= AIRHID_MAX_CHANNELS) return false;
    if (_findChannel(reportId, true) != nullptr) return false;
    Channel& ch = _channels[_channelCount++];
    ch.id     = reportId;
    ch.output = true;
    ch.owner  = owner;
    ch.chr    = nullptr;
    return true;
}

AirHID::Channel* AirHID::_findChannel(uint8_t reportId, bool output) {
    for (uint8_t i = 0; i < _channelCount; i++) {
        if (_channels[i].id == reportId && _channels[i].output == output) {
            return &_channels[i];
        }
    }
    return nullptr;
}

AirHID::Channel* AirHID::_findChannelByChr(NimBLECharacteristic* chr) {
    for (uint8_t i = 0; i < _channelCount; i++) {
        if (_channels[i].chr == chr) return &_channels[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// begin()
// ---------------------------------------------------------------------------

bool AirHID::begin() {
    if (_state == State::Running) {
        _log(HIDLogLevel::Normal, "begin() while already running — ignored");
        return true;
    }
    if (_state == State::Killed) {
        _log(HIDLogLevel::Normal, "begin() after kill() — refused");
        return false;
    }

    // Constructor warnings are deferred to here so they land after Serial.begin().
    if (_nameTruncated) {
        Serial.printf("[AirHID] WARNING: device name truncated to %d chars: \"%s\"\n",
                      AIRHID_MAX_DEVICE_NAME_LEN, _deviceName.c_str());
    }
    if (_mfrTruncated) {
        Serial.printf("[AirHID] WARNING: manufacturer string truncated to %d chars\n",
                      AIRHID_MAX_MANUFACTURER_LEN);
    }
    if (_batClamped) {
        Serial.printf("[AirHID] WARNING: battery level out of range, clamped to %d%%\n",
                      _batteryLevel);
    }

    if (_reportCount == 0) {
        Serial.println("[AirHID] ERROR: begin() with no reports registered. "
                       "Call addReport() first — a HID device with an empty "
                       "report map is invalid.");
        return false;
    }

    // Restart path — the stack is already up from a previous begin()/end() cycle.
    // GATT objects are reused wholesale; only advertising needs restarting.
    if (NimBLEDevice::isInitialized()) {
        _log(HIDLogLevel::Normal, "Restarting advertising (stack already initialised)");
        // Neither the address type nor TX power survives an end()/begin() cycle.
        if (_randomAddress) NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
        _applyTxPower();
        NimBLEDevice::startAdvertising();
        _state = State::Running;
        _startSendTask();
        _log(HIDLogLevel::Normal, "Advertising as \"%s\"", _deviceName.c_str());
        return true;
    }

    if (!_initStack())       return false;
    _configureSecurity();

    _pServer = NimBLEDevice::createServer();
    _pServer->setCallbacks(&_serverCb, false);   // false: we own the callbacks object
    _pServer->advertiseOnDisconnect(false);      // advertising is restarted manually

    // NimBLEHIDDevice creates the HID service (0x1812), the Device Information
    // Service (0x180A), and the Battery Service (0x180F).
    _pHID = new NimBLEHIDDevice(_pServer);
    _pHID->setManufacturer(_manufacturer);
    _pHID->setPnp(_pnpSource, _pnpVid, _pnpPid, _pnpVersion);
    _pHID->setHidInfo(0x00, 0x01);               // country = 0, normally connectable

    // Let every report claim its IDs and channels before the map is assembled.
    for (uint8_t i = 0; i < _reportCount; i++) {
        _reports[i]->_core = this;
        _reports[i]->onAttach();
    }

    if (!_buildReportMap())  return false;
    if (!_createChannels())  return false;

    _pHID->setBatteryLevel(_batteryLevel);

    _configureAdvertising();

    // NimBLE >= 2.4 starts the GATT server and its services when advertising
    // starts, so there is no startServices() call here. All characteristics
    // must therefore exist before this point.
    if (!NimBLEDevice::startAdvertising()) {
        Serial.println("[AirHID] ERROR: startAdvertising() failed");
        return false;
    }

    _state = State::Running;
    _startSendTask();
    _log(HIDLogLevel::Normal, "Advertising as \"%s\" with %d report(s)",
         _deviceName.c_str(), _reportCount);
    return true;
}

bool AirHID::_initStack() {
    _log(HIDLogLevel::Normal, "Initialising NimBLE");
    if (!NimBLEDevice::init(_deviceName)) {
        Serial.println("[AirHID] ERROR: NimBLEDevice::init() failed");
        return false;
    }

    // init() returns before the host task has finished syncing on some
    // core/NimBLE combinations. Wait for it rather than racing the first
    // GATT call.
    uint32_t start = millis();
    while (!NimBLEDevice::isInitialized()) {
        delay(10);
        if (millis() - start > 5000) {
            Serial.println("[AirHID] ERROR: NimBLE host did not sync within 5s");
            return false;
        }
    }

    // Must be set after init() and before advertising.
    if (_randomAddress) {
        NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
        _log(HIDLogLevel::Normal, "Using random static address");
    }
    _applyTxPower();
    return true;
}

void AirHID::_configureSecurity() {
    if (_securityMode == HIDSecurity::Passkey) {
        _log(HIDLogLevel::Normal, "Security: Passkey (numeric comparison)");
        NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND |
                                      BLE_SM_PAIR_AUTHREQ_MITM |
                                      BLE_SM_PAIR_AUTHREQ_SC);
        // DisplayYesNo selects Numeric Comparison under LE Secure Connections
        // (Core Spec Vol 3 Part H Table 2.8). macOS negotiates this path
        // regardless of what we advertise, so declaring it keeps both sides
        // in agreement.
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);
    } else {
        _log(HIDLogLevel::Normal, "Security: Just Works");
        NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND |
                                      BLE_SM_PAIR_AUTHREQ_SC);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    }
    // Distribute LTK + IRK in pairing phase 3 — required by macOS, harmless
    // and correct everywhere else.
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
}

// ---------------------------------------------------------------------------
// Composite report map
//
// Each registered report appends its own collection, in registration order.
// The result is one report map describing every report the device offers.
// ---------------------------------------------------------------------------

bool AirHID::_buildReportMap() {
    uint8_t  map[AIRHID_REPORT_MAP_SIZE];
    uint16_t used = 0;

    for (uint8_t i = 0; i < _reportCount; i++) {
        uint16_t remaining = AIRHID_REPORT_MAP_SIZE - used;
        uint16_t written   = _reports[i]->buildDescriptor(map + used, remaining);
        if (written == 0 || written > remaining) {
            Serial.printf("[AirHID] ERROR: report \"%s\" produced %u descriptor "
                          "bytes with %u available — raise AIRHID_REPORT_MAP_SIZE\n",
                          _reports[i]->reportName(), written, remaining);
            return false;
        }
        used += written;
        _log(HIDLogLevel::Verbose, "descriptor: %s contributed %u bytes",
             _reports[i]->reportName(), written);
    }

    _pHID->setReportMap(map, used);
    _log(HIDLogLevel::Normal, "Report map assembled: %u bytes from %d report(s)",
         used, _reportCount);
    return true;
}

// ---------------------------------------------------------------------------
// Report characteristics
//
// One characteristic per registered channel. NimBLE distinguishes them by the
// Report Reference descriptor (report ID + type), not by UUID — they all share
// UUID 0x2A4D. getInputReport()/getOutputReport() create on demand and return
// the existing characteristic if the (id, type) pair already exists.
// ---------------------------------------------------------------------------

bool AirHID::_createChannels() {
    if (_channelCount == 0) {
        Serial.println("[AirHID] ERROR: no report channels registered. "
                       "Reports must call registerInput() in onAttach().");
        return false;
    }

    for (uint8_t i = 0; i < _channelCount; i++) {
        Channel& ch = _channels[i];
        if (ch.output) {
            ch.chr = _pHID->getOutputReport(ch.id);
            if (ch.chr) ch.chr->setCallbacks(&_outputCb);
        } else {
            ch.chr = _pHID->getInputReport(ch.id);
        }
        if (ch.chr == nullptr) {
            Serial.printf("[AirHID] ERROR: could not create %s characteristic "
                          "for report ID %u\n", ch.output ? "output" : "input", ch.id);
            return false;
        }
        _log(HIDLogLevel::Verbose, "channel: report ID %u %s", ch.id,
             ch.output ? "output" : "input");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Advertising
// ---------------------------------------------------------------------------

void AirHID::_configureAdvertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(_appearance);
    adv->addServiceUUID(_pHID->getHidService()->getUUID());
    adv->addServiceUUID(_pHID->getBatteryService()->getUUID());

    // 20–40 ms advertising interval; Android 11+ rejects faster.
    adv->setPreferredParams(0x10, 0x20);
    adv->setMinInterval(0x20);
    adv->setMaxInterval(0x40);

    // The name goes in the scan response so it renders correctly in Bluetooth
    // settings on every host — Android in particular ignores a name that only
    // appears in the advertising packet.
    NimBLEAdvertisementData scanResponse;
    scanResponse.setName(_deviceName);
    adv->setScanResponseData(scanResponse);
    adv->enableScanResponse(true);
}

// ---------------------------------------------------------------------------
// end() / kill()
// ---------------------------------------------------------------------------

void AirHID::end() {
    if (_state != State::Running) return;
    _log(HIDLogLevel::Normal, "Stopping");

    _stopSendTaskAndWait();

    // Set the state before disconnecting so the disconnect callback does not
    // restart advertising underneath us.
    _state = State::Stopped;

    if (_connected && _pServer != nullptr) {
        for (auto handle : _pServer->getPeerDevices()) {
            _pServer->disconnect(handle);
        }
        delay(150);   // let NimBLE complete the disconnect and fire callbacks
    }

    NimBLEDevice::stopAdvertising();
    _resetConnectionState();
    _log(HIDLogLevel::Normal, "Stopped — call begin() to restart");
}

void AirHID::kill() {
    if (_state == State::Killed) return;
    if (_state == State::Running) end();

    _log(HIDLogLevel::Normal, "Killing BLE (permanent)");
    NimBLEDevice::deinit(true);

    // deinit(true) reclaims memory that overlaps our own allocations, so
    // deleting _pHID here would be a double free. Null the pointers and accept
    // a small bounded one-time leak — begin() is refused after kill(), so it
    // cannot compound.
    _pHID    = nullptr;
    _pServer = nullptr;
    for (uint8_t i = 0; i < _channelCount; i++) _channels[i].chr = nullptr;

    if (_notifyMutex) {
        vSemaphoreDelete(_notifyMutex);
        _notifyMutex = nullptr;
    }

    _state = State::Killed;
    _log(HIDLogLevel::Normal, "BLE killed — begin() will be refused");
}

// ---------------------------------------------------------------------------
// Sleep hooks
// ---------------------------------------------------------------------------

void AirHID::beforeSleep() {
    _log(HIDLogLevel::Normal, "beforeSleep");

    _stopSendTaskAndWait();

    // Drop the radio to idle parameters so the last moments before sleep are
    // cheap, and give the request a moment to go out.
    if (_connected && _connHandle != BLE_HS_CONN_HANDLE_NONE) {
        _updateConnParams(AIRHID_IDLE_INTERVAL, AIRHID_IDLE_INTERVAL,
                          AIRHID_IDLE_LATENCY, AIRHID_CONN_TIMEOUT);
        delay(50);
    }

    for (uint8_t i = 0; i < _reportCount; i++) _reports[i]->onDisconnect();

    NimBLEDevice::deinit(false);   // false = keep bonding data
    _resetConnectionState();

    // The stack is gone; the GATT objects it owned are invalid.
    _pServer = nullptr;
    _pHID    = nullptr;
    for (uint8_t i = 0; i < _channelCount; i++) _channels[i].chr = nullptr;

    // Report IDs and channels are re-claimed from scratch on the next begin().
    _channelCount = 0;
    _nextReportId = 1;

    _state = State::Stopped;
    _log(HIDLogLevel::Normal, "beforeSleep: NimBLE stopped");
}

void AirHID::afterWake() {
    _log(HIDLogLevel::Normal, "afterWake: reinitialising");
    if (!begin()) return;

    // Wait for a bonded host to reconnect and complete LTK re-encryption.
    // _authenticated is the reliable ready signal — _connected goes true at the
    // GAP layer before the host's HID stack has finished negotiating.
    uint32_t deadline = millis() + AIRHID_AFTER_WAKE_TIMEOUT_MS;
    while (!_authenticated && millis() < deadline) {
        delay(50);
    }
    if (!_authenticated) {
        _log(HIDLogLevel::Normal, "afterWake: timed out waiting for the host");
        return;
    }

    // Let the host finish HID descriptor negotiation before any report goes out.
    delay(AIRHID_AFTER_WAKE_SETTLE_MS);
    markInput();
    _log(HIDLogLevel::Normal, "afterWake: ready");
}

// ---------------------------------------------------------------------------
// Configuration setters
// ---------------------------------------------------------------------------

void AirHID::setLogLevel(HIDLogLevel level)     { _logLevel = level; }
void AirHID::setSecurityMode(HIDSecurity mode)  { _securityMode = mode; }
void AirHID::setUpdateRate(HIDRate rate)        { _updateRate = rate; }
void AirHID::setAppearance(uint16_t appearance) { _appearance = appearance; }
void AirHID::setRandomAddress(bool enable)      { _randomAddress = enable; }

void AirHID::setPasskeyCallback(void (*cb)(uint32_t)) { _cbPasskey = cb; }
void AirHID::onPairingComplete(void (*cb)(bool))      { _cbPairing = cb; }

void AirHID::setPnp(uint8_t sig, uint16_t vid, uint16_t pid, uint16_t version) {
    _pnpSource  = sig;
    _pnpVid     = vid;
    _pnpPid     = pid;
    _pnpVersion = version;
}

void AirHID::setTxPower(uint8_t level) {
    if (level < 1) level = 1;
    if (level > 8) level = 8;
    _txPowerLevel = level;
    if (NimBLEDevice::isInitialized()) _applyTxPower();
}

void AirHID::_applyTxPower() {
    NimBLEDevice::setPower(kTxPowerTable[_txPowerLevel - 1]);
    _log(HIDLogLevel::Verbose, "TX power level %d (%d dBm)",
         _txPowerLevel, kTxPowerTable[_txPowerLevel - 1]);
}

void AirHID::setBatteryLevel(uint8_t percent) {
    if (percent == 0)   percent = 1;
    if (percent > 100)  percent = 100;
    _batteryLevel = percent;
    if (_pHID != nullptr) {
        _pHID->setBatteryLevel(percent, true);   // true = notify
        // Give the host a connection interval to acknowledge the battery
        // notification; a report fired immediately after can otherwise lose
        // the ACL buffer race.
        if (_connected) delay(30);
        _log(HIDLogLevel::Normal, "Battery level %d%%", percent);
    }
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

bool AirHID::isBonded() const {
    if (_state == State::Killed) return false;
    return NimBLEDevice::getNumBonds() > 0;
}

void AirHID::clearBonds() {
    if (_state == State::Killed) return;

    // Delete each peer individually rather than calling deleteAllBonds().
    // deleteBond() routes through ble_gap_unpair(), which flushes the security
    // manager's in-memory state for the peer; deleteAllBonds() calls
    // ble_store_clear(), which wipes the NVS records but leaves stale SM
    // context behind — re-pairing then fails within the same boot cycle on iOS
    // and other hosts. Iterating in reverse avoids index shift while deleting.
    int numBonds = NimBLEDevice::getNumBonds();
    _log(HIDLogLevel::Normal, "Clearing %d bond(s)", numBonds);
    for (int i = numBonds - 1; i >= 0; i--) {
        NimBLEDevice::deleteBond(NimBLEDevice::getBondedAddress(i));
    }
}

uint16_t AirHID::reportIntervalMs() const {
    // Connection interval units are 1.25 ms: ms = units * 5 / 4.
    return (uint16_t)(((uint32_t)_updateRate * 5) / 4);
}

// ---------------------------------------------------------------------------
// Send path
// ---------------------------------------------------------------------------

bool AirHID::notify(uint8_t reportId, const uint8_t* data, size_t len) {
    if (!_authenticated || data == nullptr || len == 0) return false;
    if (len > AIRHID_MAX_REPORT_LEN) {
        _log(HIDLogLevel::Normal, "notify(): report ID %u is %u bytes, max is %d",
             reportId, (unsigned)len, AIRHID_MAX_REPORT_LEN);
        return false;
    }

    Channel* ch = _findChannel(reportId, false);
    if (ch == nullptr || ch->chr == nullptr) return false;

    if (_notifyMutex) xSemaphoreTake(_notifyMutex, portMAX_DELAY);

    // Windows selectively suspends idle BLE HID devices and drops the first
    // notification of the resume handshake. Send a zero report first to absorb
    // that drop. Pointless if the real report is itself all zeros.
    bool needsPrime = _primingNeeded ||
                      ((millis() - _lastReportMs) >= AIRHID_WINDOWS_PRIME_MS);
    if (needsPrime && !allZero(data, len)) {
        uint8_t empty[AIRHID_MAX_REPORT_LEN] = {};
        ch->chr->notify(empty, len);
        vTaskDelay(pdMS_TO_TICKS(AIRHID_PRIME_SETTLE_MS));
    }
    _primingNeeded = false;

    bool sent = false;
    for (int i = 0; i < AIRHID_NOTIFY_RETRIES && _connected; i++) {
        if (ch->chr->notify(data, len)) { sent = true; break; }
        vTaskDelay(1);   // controller buffers full — yield and retry
    }
    _lastReportMs = millis();

    if (_notifyMutex) xSemaphoreGive(_notifyMutex);

    _log(HIDLogLevel::Verbose, "notify id=%u len=%u %s",
         reportId, (unsigned)len, sent ? "ok" : "FAILED");
    return sent;
}

// ---------------------------------------------------------------------------
// Send task
//
// Ticks at the report interval. Gives every report a chance to send, then runs
// the idle power state machine. This is the only periodic context in the
// library — reports that stream state send from onTick(), event-driven reports
// call notify() directly from the caller's task. The mutex in notify() makes
// the two safe together.
// ---------------------------------------------------------------------------

void AirHID::_startSendTask() {
    if (_notifyMutex == nullptr) {
        _notifyMutex = xSemaphoreCreateMutex();
    }
    if (_sendTaskHandle != nullptr) return;

    _stopSendTask = false;
    // Not pinned to a core: correct on single-core (C3/C6/H2) and dual-core.
    xTaskCreate(_sendTaskEntry, "AirHIDSend", 4096, this, 5, &_sendTaskHandle);
}

void AirHID::_stopSendTaskAndWait() {
    if (_sendTaskHandle == nullptr) return;
    // Ask the task to exit at its next iteration rather than deleting it —
    // deleting mid-notify would leave the mutex permanently held.
    _stopSendTask = true;
    while (_sendTaskHandle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    _stopSendTask = false;
}

void AirHID::_sendTaskEntry(void* arg) {
    static_cast<AirHID*>(arg)->_sendTask();
}

void AirHID::_sendTask() {
    TickType_t lastWake = xTaskGetTickCount();

    while (!_stopSendTask) {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(reportIntervalMs()));
        if (_stopSendTask) break;
        if (!_authenticated) continue;

        for (uint8_t i = 0; i < _reportCount; i++) {
            _reports[i]->onTick();
        }
        _serviceIdle();
    }

    _sendTaskHandle = nullptr;
    vTaskDelete(nullptr);   // self-delete, outside any critical section
}

// ---------------------------------------------------------------------------
// Idle power management
//
// After AIRHID_IDLE_THRESHOLD_MS with no input, ask the host for slave latency
// so the radio can skip connection events — roughly 133/sec down to 1.6/sec.
// The first input restores full rate. Hosts are not obliged to accept the
// request, so this is best-effort.
//
// Runs only here, on the send task, so NimBLE is never called from an ISR or
// from a timer daemon. markInput() is a bare timestamp write for that reason.
// ---------------------------------------------------------------------------

void AirHID::_serviceIdle() {
    uint32_t idle = getIdleTime();

    if (_connState == ConnState::Active && idle >= AIRHID_IDLE_THRESHOLD_MS) {
        _connState = ConnState::Idle;
        _updateConnParams(AIRHID_IDLE_INTERVAL, AIRHID_IDLE_INTERVAL,
                          AIRHID_IDLE_LATENCY, AIRHID_CONN_TIMEOUT);
        // Windows may suspend the device while it is idle; make sure the next
        // real report is preceded by a prime.
        _primingNeeded = true;
        _log(HIDLogLevel::Verbose, "Radio idle (latency %d)", AIRHID_IDLE_LATENCY);

    } else if (_connState == ConnState::Idle && idle < AIRHID_IDLE_THRESHOLD_MS) {
        _connState = ConnState::Active;
        _updateConnParams((uint16_t)_updateRate, (uint16_t)_updateRate,
                          0, AIRHID_CONN_TIMEOUT);
        _log(HIDLogLevel::Verbose, "Radio active (interval %d)", (uint16_t)_updateRate);
    }
}

void AirHID::_updateConnParams(uint16_t minInterval, uint16_t maxInterval,
                               uint16_t latency, uint16_t timeout) {
    if (!_connected || _connHandle == BLE_HS_CONN_HANDLE_NONE) return;
    if (_pServer == nullptr) return;
    _pServer->updateConnParams(_connHandle, minInterval, maxInterval, latency, timeout);
}

void AirHID::_resetConnectionState() {
    _connected     = false;
    _authenticated = false;
    _connHandle    = BLE_HS_CONN_HANDLE_NONE;
    _connState     = ConnState::Disconnected;
    _primingNeeded = true;
    _lastReportMs  = 0;
}

// ---------------------------------------------------------------------------
// Connection callbacks
// ---------------------------------------------------------------------------

void AirHID::_onConnect(uint16_t connHandle) {
    _connected  = true;
    _connHandle = connHandle;
    _connState  = ConnState::Connecting;
    _log(HIDLogLevel::Normal, "Connected handle=%d", connHandle);

    // Relaxed 40 ms interval during pairing — the fast rate is requested once
    // authentication completes, so the handshake is not competing with it.
    _updateConnParams(AIRHID_CONNECT_INTERVAL, AIRHID_CONNECT_INTERVAL,
                      0, AIRHID_CONN_TIMEOUT);
}

void AirHID::_onDisconnect(int reason) {
    _resetConnectionState();
    for (uint8_t i = 0; i < _reportCount; i++) {
        _reports[i]->onDisconnect();
    }

    if (_state == State::Running) {
        _log(HIDLogLevel::Normal, "Disconnected reason=%d — restarting advertising", reason);
        NimBLEDevice::startAdvertising();
    } else {
        _log(HIDLogLevel::Normal, "Disconnected reason=%d", reason);
    }
}

void AirHID::_onAuthComplete(NimBLEConnInfo& connInfo) {
    if (!connInfo.isEncrypted()) {
        _log(HIDLogLevel::Normal, "Authentication failed — disconnecting");
        _authenticated = false;
        _connState     = ConnState::Connecting;
        if (_pServer) _pServer->disconnect(connInfo.getConnHandle());
        if (_cbPairing) _cbPairing(false);
        return;
    }

    _authenticated = true;
    _connState     = ConnState::Active;
    markInput();

    // Escalate to the configured report rate now that pairing is done.
    _updateConnParams((uint16_t)_updateRate, (uint16_t)_updateRate,
                      0, AIRHID_CONN_TIMEOUT);

    // Windows drops the first notification after a reconnect as part of the
    // CCCD handshake. Let each report prime itself.
    _primingNeeded = true;
    for (uint8_t i = 0; i < _reportCount; i++) {
        _reports[i]->onConnect();
    }

    _log(HIDLogLevel::Normal, "Paired and authenticated handle=%d", connInfo.getConnHandle());
    if (_cbPairing) _cbPairing(true);
}

void AirHID::_onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t passkey) {
    if (_cbPasskey) {
        _cbPasskey(passkey);
    } else {
        // Print unconditionally when no callback is registered — otherwise the
        // code is invisible at HIDLogLevel::Off and the user cannot verify it.
        Serial.printf("[AirHID] Passkey: %06lu — confirm on host\n", (unsigned long)passkey);
    }
    // We always confirm; the user verifies the matching code on the host side.
    // Rejecting here silently drops the connection on macOS.
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
}

void AirHID::_onCharWrite(NimBLECharacteristic* chr) {
    Channel* ch = _findChannelByChr(chr);
    if (ch == nullptr || ch->owner == nullptr) return;
    NimBLEAttValue value = chr->getValue();
    ch->owner->onOutput(ch->id, value.data(), value.length());
}

// ---------------------------------------------------------------------------
// NimBLE callback adapters
// ---------------------------------------------------------------------------

void AirHID::ServerCallbacks::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
    (void)pServer;
    _parent->_onConnect(connInfo.getConnHandle());
}

void AirHID::ServerCallbacks::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
    (void)pServer; (void)connInfo;
    _parent->_onDisconnect(reason);
}

void AirHID::ServerCallbacks::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    _parent->_onAuthComplete(connInfo);
}

void AirHID::ServerCallbacks::onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t passkey) {
    _parent->_onConfirmPassKey(connInfo, passkey);
}

void AirHID::OutputCallbacks::onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) {
    (void)connInfo;
    _parent->_onCharWrite(chr);
}
