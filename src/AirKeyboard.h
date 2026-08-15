#pragma once
/**
 * AirKeyboard.h
 *
 * Keyboard report for AirHID. Full 104/105-key coverage including numpad,
 * international and language keys, 6-key rollover, and host LED state.
 *
 * Every method blocks and calls into BLE — this class is loop-task only and is
 * NOT ISR-safe, unlike AirMouse's base layer.
 *
 * Media keys are not here: they belong to a Consumer Control report, which
 * arrives as its own AirHIDReport in a later phase.
 *
 * Copyright (c) 2026. Licensed under the Apache License, Version 2.0.
 * Derived from HijelHID_BLEKeyboard (c) 2026 Hijel.
 */

#include "AirHID.h"
#include "AirHIDKeys.h"

// Keyboard input report: [modifiers][reserved][key0..key5]
#define AIRHID_KEYBOARD_REPORT_SIZE 8
// LED output report: one bitmask byte, host -> device
#define AIRHID_LED_REPORT_SIZE      1

// LED bitmask values, as received in the output report.
#define AIRHID_LED_NUM_LOCK    0x01
#define AIRHID_LED_CAPS_LOCK   0x02
#define AIRHID_LED_SCROLL_LOCK 0x04
#define AIRHID_LED_COMPOSE     0x08
#define AIRHID_LED_KANA        0x10

// How long a key is held by tap(), and the gap before the next one.
// iOS and iPadOS need roughly 15 ms of each to register every keypress,
// including repeats of the same key.
#define AIRHID_DEFAULT_TAP_DELAY_MS 25
#define AIRHID_DEFAULT_KEY_GAP_MS   25

class AirKeyboard : public AirHIDReport, public Print {
public:
    AirKeyboard() = default;

    const char* reportName() const override { return "keyboard"; }

    // -----------------------------------------------------------------------
    // Keys
    // -----------------------------------------------------------------------

    /**
     * Hold a key down. Up to 6 non-modifier keys at once (6KRO).
     *
     * `keycode`   — a `KEY_*` constant from `AirHIDKeys.h`.
     * `modifiers` — optional `KEY_MOD_*` bitmask, OR'd together.
     *
     * Modifiers set here stay held until cleared by `releaseAll()` or by
     * releasing the matching modifier keycode.
     */
    void press(uint8_t keycode, uint8_t modifiers = 0);

    /** Release a held key. `KEY_NONE` releases everything. */
    void release(uint8_t keycode);

    /** Release all keys and modifiers. Safe at any time to clear stuck keys. */
    void releaseAll();

    /**
     * Press and release in one call.
     *
     * `delayMs` overrides the hold time for this tap only, `keyGap` the gap
     * after it. Zero means use the global values.
     *
     * Modifiers held via `press()` survive; modifiers passed here do not.
     */
    void tap(uint8_t keycode, uint8_t modifiers = 0,
             uint16_t delayMs = 0, uint16_t keyGap = 0);

    // -----------------------------------------------------------------------
    // Text — Print interface, so print() and println() work
    // -----------------------------------------------------------------------

    /**
     * Type one ASCII character, picking the keycode and shift state
     * automatically. Handles printable ASCII (0x20–0x7E) plus `\n`, `\r`,
     * `\t`, backspace, and escape. Assumes a US layout on the host.
     */
    size_t write(uint8_t c) override;

    /** Type a buffer, one character at a time. */
    size_t write(const uint8_t* buffer, size_t size) override;

    // -----------------------------------------------------------------------
    // Timing
    // -----------------------------------------------------------------------

    /** Global key hold time for `tap()`. Raise it if the host misses keys. */
    void setTapDelay(uint16_t ms) { _tapDelay = ms; }

    /** Global gap after each `tap()`. Raise it if repeated keys are dropped. */
    void setKeyGap(uint16_t ms)   { _keyGap = ms; }

    // -----------------------------------------------------------------------
    // LED state, reported by the host
    // -----------------------------------------------------------------------

    bool isNumLockOn()    const { return (_ledState & AIRHID_LED_NUM_LOCK)    != 0; }
    bool isCapsLockOn()   const { return (_ledState & AIRHID_LED_CAPS_LOCK)   != 0; }
    bool isScrollLockOn() const { return (_ledState & AIRHID_LED_SCROLL_LOCK) != 0; }

    /** Raw LED bitmask most recently sent by the host. */
    uint8_t ledState() const { return _ledState; }

    /**
     * Called when the host changes LED state, with the raw bitmask.
     * Runs on the NimBLE task — keep it short and do not block.
     */
    void onLEDChange(void (*cb)(uint8_t leds)) { _cbLED = cb; }

    /** HID report ID assigned by the core, or 0 before `begin()`. */
    uint8_t reportId() const { return _reportId; }

protected:
    uint16_t buildDescriptor(uint8_t* out, uint16_t maxLen) override;
    void     onAttach() override;
    void     onDisconnect() override;
    void     onOutput(uint8_t reportId, const uint8_t* data, size_t len) override;

private:
    uint8_t  _reportId = 0;
    uint8_t  _keyReport[AIRHID_KEYBOARD_REPORT_SIZE] = {};
    volatile uint8_t _ledState = 0;
    uint16_t _tapDelay = AIRHID_DEFAULT_TAP_DELAY_MS;
    uint16_t _keyGap   = AIRHID_DEFAULT_KEY_GAP_MS;

    void (*_cbLED)(uint8_t) = nullptr;

    bool    _ready() const;
    void    _sendKeyReport();
    bool    _addKeycode(uint8_t keycode);
    bool    _removeKeycode(uint8_t keycode);
    static bool    _isModifier(uint8_t keycode);
    static uint8_t _keycodeToModBit(uint8_t keycode);
};
