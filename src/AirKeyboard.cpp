/**
 * AirKeyboard.cpp
 *
 * Copyright (c) 2026. Licensed under the Apache License, Version 2.0.
 * Derived from HijelHID_BLEKeyboard (c) 2026 Hijel.
 */

#include "AirKeyboard.h"

// ---------------------------------------------------------------------------
// ASCII -> HID lookup, indexed by (c - 0x20) for printable ASCII 0x20..0x7E.
// Control characters are handled explicitly in write() before these are used.
// Assumes a US layout on the host.
// ---------------------------------------------------------------------------

static const uint8_t kKeycodeTable[95] = {
    // 0x20..0x2F  (space ! " # $ % & ' ( ) * + , - . /)
    KEY_SPACE, KEY_1, KEY_APOSTROPHE, KEY_3, KEY_4, KEY_5, KEY_7,
    KEY_APOSTROPHE, KEY_9, KEY_0, KEY_8, KEY_EQUAL, KEY_COMMA,
    KEY_MINUS, KEY_DOT, KEY_SLASH,
    // 0x30..0x39  (0-9)
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    // 0x3A..0x3F  (: ; < = > ?)
    KEY_SEMICOLON, KEY_SEMICOLON, KEY_COMMA, KEY_EQUAL, KEY_DOT, KEY_SLASH,
    // 0x40  (@)
    KEY_2,
    // 0x41..0x5A  (A-Z)
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
    KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
    KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    // 0x5B..0x60  ([ \ ] ^ _ `)
    KEY_LEFTBRACE, KEY_BACKSLASH, KEY_RIGHTBRACE, KEY_6, KEY_MINUS, KEY_GRAVE,
    // 0x61..0x7A  (a-z)
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
    KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
    KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    // 0x7B..0x7E  ({ | } ~)
    KEY_LEFTBRACE, KEY_BACKSLASH, KEY_RIGHTBRACE, KEY_GRAVE,
};

static const uint8_t kModifierTable[95] = {
    // 0x20..0x2F
    0,               // space
    KEY_MOD_LSHIFT,  // !
    KEY_MOD_LSHIFT,  // "
    KEY_MOD_LSHIFT,  // #
    KEY_MOD_LSHIFT,  // $
    KEY_MOD_LSHIFT,  // %
    KEY_MOD_LSHIFT,  // &
    0,               // '
    KEY_MOD_LSHIFT,  // (
    KEY_MOD_LSHIFT,  // )
    KEY_MOD_LSHIFT,  // *
    KEY_MOD_LSHIFT,  // +
    0,               // ,
    0,               // -
    0,               // .
    0,               // /
    // 0x30..0x39  (0-9, unshifted)
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x3A..0x3F
    KEY_MOD_LSHIFT,  // :
    0,               // ;
    KEY_MOD_LSHIFT,  // <
    0,               // =
    KEY_MOD_LSHIFT,  // >
    KEY_MOD_LSHIFT,  // ?
    // 0x40  (@)
    KEY_MOD_LSHIFT,
    // 0x41..0x5A  (A-Z, all shifted)
    KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT,
    KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT,
    KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT,
    KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT,
    KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT,
    KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT, KEY_MOD_LSHIFT,
    KEY_MOD_LSHIFT, KEY_MOD_LSHIFT,
    // 0x5B..0x60
    0,               // [
    0,               // backslash
    0,               // ]
    KEY_MOD_LSHIFT,  // ^
    KEY_MOD_LSHIFT,  // _
    0,               // `
    // 0x61..0x7A  (a-z, unshifted)
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x7B..0x7E
    KEY_MOD_LSHIFT,  // {
    KEY_MOD_LSHIFT,  // |
    KEY_MOD_LSHIFT,  // }
    KEY_MOD_LSHIFT,  // ~
};

// ---------------------------------------------------------------------------
// Registration
//
// Input and output share one report ID. NimBLE tells the two characteristics
// apart by the type byte in their Report Reference descriptors, not by UUID.
// ---------------------------------------------------------------------------

void AirKeyboard::onAttach() {
    _reportId = _core->claimReportId();
    _core->registerInput(_reportId, this);
    _core->registerOutput(_reportId, this);
}

// ---------------------------------------------------------------------------
// HID descriptor
//
// Byte 0    modifier bitmask (left/right ctrl, shift, alt, gui)
// Byte 1    reserved, always 0
// Bytes 2-7 up to six simultaneous keycodes (6KRO)
//
// Plus a 1-byte LED output report: five lock LEDs and three padding bits.
// Logical maximum 0xE7 covers every standard key up to KEY_RGUI, including
// international and language keys.
// ---------------------------------------------------------------------------

uint16_t AirKeyboard::buildDescriptor(uint8_t* out, uint16_t maxLen) {
    const uint8_t desc[] = {
        0x05, 0x01,        // Usage Page (Generic Desktop)
        0x09, 0x06,        // Usage (Keyboard)
        0xA1, 0x01,        // Collection (Application)
        0x85, _reportId,   //   Report ID

        // Modifier bits
        0x05, 0x07,        //   Usage Page (Key Codes)
        0x19, 0xE0,        //   Usage Minimum (Left Control)
        0x29, 0xE7,        //   Usage Maximum (Right GUI)
        0x15, 0x00,        //   Logical Minimum (0)
        0x25, 0x01,        //   Logical Maximum (1)
        0x75, 0x01,        //   Report Size (1 bit)
        0x95, 0x08,        //   Report Count (8)
        0x81, 0x02,        //   Input (Data, Var, Abs)

        // Reserved byte
        0x95, 0x01,        //   Report Count (1)
        0x75, 0x08,        //   Report Size (8 bits)
        0x81, 0x01,        //   Input (Const)

        // LED output report
        0x95, 0x05,        //   Report Count (5)
        0x75, 0x01,        //   Report Size (1 bit)
        0x05, 0x08,        //   Usage Page (LEDs)
        0x19, 0x01,        //   Usage Minimum (Num Lock)
        0x29, 0x05,        //   Usage Maximum (Kana)
        0x91, 0x02,        //   Output (Data, Var, Abs)
        0x95, 0x01,        //   Report Count (1)
        0x75, 0x03,        //   Report Size (3 bits)
        0x91, 0x01,        //   Output (Const) — pad to a whole byte

        // 6-key rollover array
        0x95, 0x06,        //   Report Count (6)
        0x75, 0x08,        //   Report Size (8 bits)
        0x15, 0x00,        //   Logical Minimum (0)
        0x26, 0xE7, 0x00,  //   Logical Maximum (0xE7)
        0x05, 0x07,        //   Usage Page (Key Codes)
        0x19, 0x00,        //   Usage Minimum (0)
        0x2A, 0xE7, 0x00,  //   Usage Maximum (0xE7)
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

bool AirKeyboard::_ready() const {
    return _core != nullptr && _reportId != 0 && _core->isPaired();
}

void AirKeyboard::_sendKeyReport() {
    if (!_ready()) return;
    _core->markInput();
    _core->notify(_reportId, _keyReport, AIRHID_KEYBOARD_REPORT_SIZE);
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

void AirKeyboard::press(uint8_t keycode, uint8_t modifiers) {
    if (!_ready()) return;

    if (_isModifier(keycode)) {
        _keyReport[0] |= _keycodeToModBit(keycode);
    } else {
        _addKeycode(keycode);
    }
    _keyReport[0] |= modifiers;
    _sendKeyReport();
}

void AirKeyboard::release(uint8_t keycode) {
    if (!_ready()) return;
    if (keycode == KEY_NONE) { releaseAll(); return; }

    if (_isModifier(keycode)) {
        _keyReport[0] &= ~_keycodeToModBit(keycode);
    } else {
        _removeKeycode(keycode);
    }
    _sendKeyReport();
}

void AirKeyboard::releaseAll() {
    if (!_ready()) return;
    memset(_keyReport, 0, sizeof(_keyReport));
    _sendKeyReport();
}

void AirKeyboard::tap(uint8_t keycode, uint8_t modifiers,
                      uint16_t delayMs, uint16_t keyGap) {
    if (!_ready()) return;
    if (delayMs == 0) delayMs = _tapDelay;
    if (keyGap  == 0) keyGap  = _keyGap;

    // Snapshot the modifier byte before press() merges this tap's modifiers in,
    // so modifiers the caller is holding via press() survive while the ones
    // passed here do not.
    uint8_t savedMods = _keyReport[0];

    press(keycode, modifiers);
    delay(delayMs);

    if (_isModifier(keycode)) {
        _keyReport[0] &= ~_keycodeToModBit(keycode);
    } else {
        _removeKeycode(keycode);
    }
    _keyReport[0] = savedMods;
    _sendKeyReport();

    delay(keyGap);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

size_t AirKeyboard::write(uint8_t c) {
    if (!_ready()) return 0;

    if (c == '\n' || c == '\r') { tap(KEY_RETURN);    return 1; }
    if (c == '\t')              { tap(KEY_TAB);       return 1; }
    if (c == 0x08)              { tap(KEY_BACKSPACE); return 1; }
    if (c == 0x1B)              { tap(KEY_ESCAPE);    return 1; }

    if (c >= 0x20 && c <= 0x7E) {
        uint8_t idx     = c - 0x20;
        uint8_t keycode = kKeycodeTable[idx];
        if (keycode != 0) {
            tap(keycode, kModifierTable[idx]);
            return 1;
        }
    }
    return 0;
}

size_t AirKeyboard::write(const uint8_t* buffer, size_t size) {
    if (!_ready() || size == 0) return 0;
    size_t written = 0;
    for (size_t i = 0; i < size; i++) {
        written += write(buffer[i]);
    }
    return written;
}

// ---------------------------------------------------------------------------
// Host callbacks
// ---------------------------------------------------------------------------

void AirKeyboard::onOutput(uint8_t reportId, const uint8_t* data, size_t len) {
    (void)reportId;
    if (len < AIRHID_LED_REPORT_SIZE) return;
    _ledState = data[0];
    if (_cbLED) _cbLED(data[0]);
}

void AirKeyboard::onDisconnect() {
    memset(_keyReport, 0, sizeof(_keyReport));
    _ledState = 0;
}

// ---------------------------------------------------------------------------
// Key state helpers
// ---------------------------------------------------------------------------

bool AirKeyboard::_addKeycode(uint8_t keycode) {
    // Already held — do nothing, so the array cannot pick up duplicates.
    for (int i = 2; i < AIRHID_KEYBOARD_REPORT_SIZE; i++) {
        if (_keyReport[i] == keycode) return true;
    }
    for (int i = 2; i < AIRHID_KEYBOARD_REPORT_SIZE; i++) {
        if (_keyReport[i] == 0) {
            _keyReport[i] = keycode;
            return true;
        }
    }
    return false;   // 6KRO limit reached, keycode dropped
}

bool AirKeyboard::_removeKeycode(uint8_t keycode) {
    for (int i = 2; i < AIRHID_KEYBOARD_REPORT_SIZE; i++) {
        if (_keyReport[i] == keycode) {
            _keyReport[i] = 0;
            return true;
        }
    }
    return false;
}

bool AirKeyboard::_isModifier(uint8_t keycode) {
    return keycode >= KEY_LCTRL && keycode <= KEY_RGUI;
}

uint8_t AirKeyboard::_keycodeToModBit(uint8_t keycode) {
    return (uint8_t)(1 << (keycode - KEY_LCTRL));
}
