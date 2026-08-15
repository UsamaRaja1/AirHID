/**
 * AirConsumer.cpp
 *
 * Copyright (c) 2026. Licensed under the Apache License, Version 2.0.
 * Derived from HijelHID_BLEKeyboard (c) 2026 Hijel.
 */

#include "AirConsumer.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void AirConsumer::onAttach() {
    _reportId = _core->claimReportId();
    _core->registerInput(_reportId, this);
}

// ---------------------------------------------------------------------------
// HID descriptor
//
// A single 16-bit array field. Logical and usage maximum 0x3FF cover every
// MEDIA_* constant in AirHIDMediaKeys.h — and any other Consumer Page usage in
// that range — without touching this descriptor.
//
// Array rather than variable: the host reads one active usage per report,
// which is how consumer controls are normally declared and what makes 0x0000
// mean "nothing pressed".
// ---------------------------------------------------------------------------

uint16_t AirConsumer::buildDescriptor(uint8_t* out, uint16_t maxLen) {
    const uint8_t desc[] = {
        0x05, 0x0C,        // Usage Page (Consumer)
        0x09, 0x01,        // Usage (Consumer Control)
        0xA1, 0x01,        // Collection (Application)
        0x85, _reportId,   //   Report ID
        0x15, 0x00,        //   Logical Minimum (0)
        0x26, 0xFF, 0x03,  //   Logical Maximum (0x3FF)
        0x19, 0x00,        //   Usage Minimum (0)
        0x2A, 0xFF, 0x03,  //   Usage Maximum (0x3FF)
        0x75, 0x10,        //   Report Size (16 bits)
        0x95, 0x01,        //   Report Count (1)
        0x81, 0x00,        //   Input (Data, Array, Abs)
        0xC0,              // End Collection
    };

    if (_reportId == 0 || sizeof(desc) > maxLen) return 0;
    memcpy(out, desc, sizeof(desc));
    return sizeof(desc);
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

bool AirConsumer::_ready() const {
    return _core != nullptr && _reportId != 0 && _core->isPaired();
}

void AirConsumer::_send(uint16_t usageId) {
    if (!_ready()) return;
    uint8_t report[AIRHID_CONSUMER_REPORT_SIZE] = {
        (uint8_t)(usageId & 0xFF),
        (uint8_t)(usageId >> 8)
    };
    _core->markInput();
    _core->notify(_reportId, report, sizeof(report));
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

void AirConsumer::press(uint16_t usageId) {
    if (!_ready()) return;
    _activeUsage = usageId;
    _send(usageId);
}

void AirConsumer::release() {
    if (!_ready()) return;
    // Nothing held — skip the report. An unnecessary zero notify can interfere
    // with the next real one on some hosts.
    if (_activeUsage == 0) return;
    _activeUsage = 0;
    _send(0x0000);
}

void AirConsumer::tap(uint16_t usageId, uint16_t delayMs, uint16_t keyGap) {
    if (!_ready()) return;
    if (delayMs == 0) delayMs = _tapDelay;
    if (keyGap  == 0) keyGap  = _keyGap;

    press(usageId);
    delay(delayMs);
    release();
    delay(keyGap);
}

void AirConsumer::onDisconnect() {
    _activeUsage = 0;
}
