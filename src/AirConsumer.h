#pragma once
/**
 * AirConsumer.h
 *
 * Consumer Control report for AirHID — media transport, volume, brightness,
 * browser navigation, and application launch keys.
 *
 * One 16-bit usage ID per report. Only one usage can be active at a time,
 * which is what the Consumer Page array field allows; `0x0000` releases.
 *
 * Loop-task only, like AirKeyboard: every method calls into BLE and `tap()`
 * blocks. Not ISR-safe.
 *
 * Copyright (c) 2026. Licensed under the Apache License, Version 2.0.
 * Derived from HijelHID_BLEKeyboard (c) 2026 Hijel.
 */

#include "AirHID.h"
#include "AirHIDMediaKeys.h"

// A consumer report is a single little-endian 16-bit usage ID.
#define AIRHID_CONSUMER_REPORT_SIZE 2

// Default hold and gap for tap(), in milliseconds. Consumer keys are edge
// triggered on most hosts, so these only need to be long enough for the press
// and release to arrive as two distinct notifications.
#define AIRHID_CONSUMER_TAP_DELAY_MS 25
#define AIRHID_CONSUMER_KEY_GAP_MS   25

class AirConsumer : public AirHIDReport {
public:
    AirConsumer() = default;

    const char* reportName() const override { return "consumer"; }

    // -----------------------------------------------------------------------
    // Keys
    // -----------------------------------------------------------------------

    /**
     * Hold a consumer key down.
     *
     * `usageId` is a `MEDIA_*` constant from `AirHIDMediaKeys.h`, or any raw
     * Consumer Page usage up to 0x3FF.
     *
     * Only one usage is active at a time — pressing a second replaces the
     * first. Follow with `release()`.
     */
    void press(uint16_t usageId);

    /** Release the held key by sending usage 0x0000. */
    void release();

    /**
     * Press and release in one call.
     *
     * `delayMs` overrides the hold time for this tap only, `keyGap` the gap
     * after it. Zero means use the global values.
     */
    void tap(uint16_t usageId, uint16_t delayMs = 0, uint16_t keyGap = 0);

    // -----------------------------------------------------------------------
    // Timing
    // -----------------------------------------------------------------------

    /** Global hold time for `tap()`. Raise it if the host misses keys. */
    void setTapDelay(uint16_t ms) { _tapDelay = ms; }

    /** Global gap after each `tap()`. */
    void setKeyGap(uint16_t ms)   { _keyGap = ms; }

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    /** Usage currently held, or 0 if none. */
    uint16_t activeUsage() const { return _activeUsage; }

    /** HID report ID assigned by the core, or 0 before `begin()`. */
    uint8_t reportId() const { return _reportId; }

protected:
    uint16_t buildDescriptor(uint8_t* out, uint16_t maxLen) override;
    void     onAttach() override;
    void     onDisconnect() override;

private:
    uint8_t  _reportId    = 0;
    uint16_t _activeUsage = 0;
    uint16_t _tapDelay    = AIRHID_CONSUMER_TAP_DELAY_MS;
    uint16_t _keyGap      = AIRHID_CONSUMER_KEY_GAP_MS;

    bool _ready() const;
    void _send(uint16_t usageId);
};
