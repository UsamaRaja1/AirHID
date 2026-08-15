/**
 * AirMouse.cpp
 *
 * Copyright (c) 2026. Licensed under the Apache License, Version 2.0.
 * Derived from HijelHID_BLEMouse (c) 2026 Hijel.
 */

#include "AirMouse.h"

AirMouse::AirMouse(uint8_t buttonCount, bool horizontalScroll)
    : _buttonCount(buttonCount == 5 ? 5 : 3),
      _horizontalScroll(horizontalScroll)
{
}

void AirMouse::setButtonCount(uint8_t count)     { _buttonCount = (count == 5) ? 5 : 3; }
void AirMouse::setHorizontalScroll(bool enable)  { _horizontalScroll = enable; }

uint16_t AirMouse::_reportIntervalMs() const {
    return (_core != nullptr) ? _core->reportIntervalMs() : 8;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void AirMouse::onAttach() {
    _reportId = _core->claimReportId();
    _core->registerInput(_reportId, this);
}

// ---------------------------------------------------------------------------
// HID descriptor
//
// One Application collection wrapping a Physical collection: buttons, then
// relative X / Y / Wheel. With AC Pan enabled a fifth relative axis is added
// inside the same physical collection by switching to the Consumer usage page
// for that one field — exactly how real tilt-wheel mice declare it. Declaring
// it as a separate Consumer collection instead is NOT recognised as horizontal
// scroll by mouhid.sys, IOHIDFamily, or Linux HID.
//
// Unlike the standalone mouse library this descriptor always carries a Report
// ID: a composite report map with an un-IDed collection is malformed.
// ---------------------------------------------------------------------------

void AirMouse::_writePointerBlock(uint8_t*& p) const {
    *p++ = 0x09; *p++ = 0x01;  // Usage (Pointer)
    *p++ = 0xA1; *p++ = 0x00;  // Collection (Physical)

    // Buttons
    *p++ = 0x05; *p++ = 0x09;  // Usage Page (Button)
    *p++ = 0x19; *p++ = 0x01;  // Usage Minimum (Button 1)
    if (_buttonCount >= 5) {
        *p++ = 0x29; *p++ = 0x05;  // Usage Maximum (Button 5)
        *p++ = 0x95; *p++ = 0x05;  // Report Count (5)
    } else {
        *p++ = 0x29; *p++ = 0x03;  // Usage Maximum (Button 3)
        *p++ = 0x95; *p++ = 0x03;  // Report Count (3)
    }
    *p++ = 0x15; *p++ = 0x00;  // Logical Minimum (0)
    *p++ = 0x25; *p++ = 0x01;  // Logical Maximum (1)
    *p++ = 0x75; *p++ = 0x01;  // Report Size (1 bit)
    *p++ = 0x81; *p++ = 0x02;  // Input (Data, Var, Abs)

    // Pad the button bitfield out to a byte boundary
    *p++ = 0x95; *p++ = (uint8_t)((_buttonCount >= 5) ? 3 : 5);
    *p++ = 0x75; *p++ = 0x01;
    *p++ = 0x81; *p++ = 0x03;  // Input (Const, Var, Abs)

    // X, Y, Wheel — relative, -127..127
    *p++ = 0x05; *p++ = 0x01;  // Usage Page (Generic Desktop)
    *p++ = 0x09; *p++ = 0x30;  // Usage (X)
    *p++ = 0x09; *p++ = 0x31;  // Usage (Y)
    *p++ = 0x09; *p++ = 0x38;  // Usage (Wheel)
    *p++ = 0x15; *p++ = 0x81;  // Logical Minimum (-127)
    *p++ = 0x25; *p++ = 0x7F;  // Logical Maximum (127)
    *p++ = 0x75; *p++ = 0x08;  // Report Size (8 bits)
    *p++ = 0x95; *p++ = 0x03;  // Report Count (3)
    *p++ = 0x81; *p++ = 0x06;  // Input (Data, Var, Rel)
}

uint16_t AirMouse::buildDescriptor(uint8_t* out, uint16_t maxLen) {
    // Worst case is 5 buttons with AC Pan: 71 bytes including the Report ID.
    uint8_t  scratch[96];
    uint8_t* p = scratch;

    *p++ = 0x05; *p++ = 0x01;       // Usage Page (Generic Desktop)
    *p++ = 0x09; *p++ = 0x02;       // Usage (Mouse)
    *p++ = 0xA1; *p++ = 0x01;       // Collection (Application)
    *p++ = 0x85; *p++ = _reportId;  //   Report ID

    _writePointerBlock(p);

    if (_horizontalScroll) {
        // AC Pan, inside the same physical collection
        *p++ = 0x05; *p++ = 0x0C;               // Usage Page (Consumer)
        *p++ = 0x0A; *p++ = 0x38; *p++ = 0x02;  // Usage (AC Pan, 0x0238)
        *p++ = 0x15; *p++ = 0x81;               // Logical Minimum (-127)
        *p++ = 0x25; *p++ = 0x7F;               // Logical Maximum (127)
        *p++ = 0x75; *p++ = 0x08;               // Report Size (8 bits)
        *p++ = 0x95; *p++ = 0x01;               // Report Count (1)
        *p++ = 0x81; *p++ = 0x06;               // Input (Data, Var, Rel)
    }

    *p++ = 0xC0;   // End Collection (Physical)
    *p++ = 0xC0;   // End Collection (Application)

    uint16_t len = (uint16_t)(p - scratch);
    if (_reportId == 0 || len > maxLen) return 0;
    memcpy(out, scratch, len);
    return len;
}

// ---------------------------------------------------------------------------
// Base layer — ISR-safe
// ---------------------------------------------------------------------------

void AirMouse::move(int16_t dx, int16_t dy) {
    int8_t cx = _clampInt8(dx);
    int8_t cy = _clampInt8(dy);
    portENTER_CRITICAL_ISR(&_spinlock);
    _state.x = cx;
    _state.y = cy;
    portEXIT_CRITICAL_ISR(&_spinlock);
    if (_core) _core->markInput();
}

void AirMouse::addScroll(int8_t dz) {
    portENTER_CRITICAL_ISR(&_spinlock);
    _remainingScroll += dz;
    portEXIT_CRITICAL_ISR(&_spinlock);
    if (_core) _core->markInput();
}

void AirMouse::addScrollH(int8_t dz) {
    if (!_horizontalScroll) return;
    portENTER_CRITICAL_ISR(&_spinlock);
    _remainingScrollH += dz;
    portEXIT_CRITICAL_ISR(&_spinlock);
    if (_core) _core->markInput();
}

void AirMouse::setButton(MouseButton button, bool pressed) {
    portENTER_CRITICAL_ISR(&_spinlock);
    if (pressed) _state.buttons |=  (uint8_t)button;
    else         _state.buttons &= ~(uint8_t)button;
    portEXIT_CRITICAL_ISR(&_spinlock);
    if (_core) _core->markInput();
}

void AirMouse::setButtons(uint8_t mask) {
    portENTER_CRITICAL_ISR(&_spinlock);
    _state.buttons = mask;
    portEXIT_CRITICAL_ISR(&_spinlock);
    if (_core) _core->markInput();
}

// ---------------------------------------------------------------------------
// Send — called at the report interval from the core's send task
// ---------------------------------------------------------------------------

void AirMouse::onTick() {
    if (_reportId == 0 || _core == nullptr) return;

    InputState snap;
    portENTER_CRITICAL(&_spinlock);

    // One queued moveTo() step, if any, becomes this report's movement.
    if (_moveBufCount > 0) _moveBufPop();

    if (_remainingScroll != 0) {
        _state.scrollV += _clampDrain(_remainingScroll);
    }
    if (_horizontalScroll && _remainingScrollH != 0) {
        _state.scrollH += _clampDrain(_remainingScrollH);
    }

    snap = _state;
    // Deltas are consumed by this report; buttons persist until changed.
    _state.x       = 0;
    _state.y       = 0;
    _state.scrollV = 0;
    _state.scrollH = 0;
    portEXIT_CRITICAL(&_spinlock);

    bool active = snap.buttons || snap.x || snap.y || snap.scrollV || snap.scrollH;

    // Stay quiet while nothing is happening. The one exception is the first
    // idle report after activity: the host needs it to see a button release
    // and to stop applying the last movement.
    if (!active && !_lastReportActive) return;
    _lastReportActive = active;

    if (_horizontalScroll) {
        uint8_t data[5] = {
            snap.buttons, (uint8_t)snap.x, (uint8_t)snap.y,
            (uint8_t)snap.scrollV, (uint8_t)snap.scrollH
        };
        _core->notify(_reportId, data, sizeof(data));
    } else {
        uint8_t data[4] = {
            snap.buttons, (uint8_t)snap.x, (uint8_t)snap.y, (uint8_t)snap.scrollV
        };
        _core->notify(_reportId, data, sizeof(data));
    }
}

void AirMouse::onDisconnect() {
    portENTER_CRITICAL(&_spinlock);
    _state             = InputState();
    _remainingScroll   = 0;
    _remainingScrollH  = 0;
    _moveBufHead       = 0;
    _moveBufTail       = 0;
    _moveBufCount      = 0;
    portEXIT_CRITICAL(&_spinlock);
    _lastReportActive = false;
}

// ---------------------------------------------------------------------------
// Macro layer
// ---------------------------------------------------------------------------

void AirMouse::moveTo(int16_t dx, int16_t dy, uint32_t durationMs) {
    if (dx == 0 && dy == 0) return;

    uint16_t intervalMs = _reportIntervalMs();
    int32_t  absX = (dx > 0) ? dx : -dx;
    int32_t  absY = (dy > 0) ? dy : -dy;

    // durationMs == 0: as fast as possible, so one step per 127-unit chunk of
    // the longer axis. Otherwise: one step per report interval in the duration.
    uint32_t totalSteps;
    if (durationMs == 0) {
        uint32_t stepsX = (absX + 126) / 127;
        uint32_t stepsY = (absY + 126) / 127;
        totalSteps = (stepsX > stepsY) ? stepsX : stepsY;
    } else {
        totalSteps = durationMs / intervalMs;
    }
    if (totalSteps == 0) totalSteps = 1;

    // Discard any move still in flight
    portENTER_CRITICAL(&_spinlock);
    _moveBufHead  = 0;
    _moveBufTail  = 0;
    _moveBufCount = 0;
    portEXIT_CRITICAL(&_spinlock);

    // Per axis: either a whole number of units per step (normal mode), or a
    // single unit every N steps when the move is too small for the duration
    // (spacing mode).
    int8_t   stepX = 0, stepY = 0;
    int8_t   remX  = 0, remY  = 0;
    uint32_t spacingX = 1, spacingY = 1;

    if (dx != 0) {
        if (absX >= (int32_t)totalSteps) {
            int32_t chunk = absX / totalSteps;
            if (chunk > 127) chunk = 127;
            stepX = (dx > 0) ? (int8_t)chunk : -(int8_t)chunk;
            int32_t left = absX - chunk * (int32_t)(totalSteps - 1);
            if (left > 127) left = 127;
            remX = (dx > 0) ? (int8_t)left : -(int8_t)left;
        } else {
            stepX    = (dx > 0) ? 1 : -1;
            remX     = stepX;
            spacingX = totalSteps / absX;
            if (spacingX == 0) spacingX = 1;
        }
    }

    if (dy != 0) {
        if (absY >= (int32_t)totalSteps) {
            int32_t chunk = absY / totalSteps;
            if (chunk > 127) chunk = 127;
            stepY = (dy > 0) ? (int8_t)chunk : -(int8_t)chunk;
            int32_t left = absY - chunk * (int32_t)(totalSteps - 1);
            if (left > 127) left = 127;
            remY = (dy > 0) ? (int8_t)left : -(int8_t)left;
        } else {
            stepY    = (dy > 0) ? 1 : -1;
            remY     = stepY;
            spacingY = totalSteps / absY;
            if (spacingY == 0) spacingY = 1;
        }
    }

    // Fill the FIFO, waiting for the send task to drain it as needed.
    uint32_t stepsQueued = 0;
    while (stepsQueued < totalSteps) {
        portENTER_CRITICAL(&_spinlock);
        while (stepsQueued < totalSteps && _moveBufCount < AIRHID_MOVE_BUFFER_SIZE) {
            uint32_t step    = stepsQueued + 1;   // 1-based for the spacing modulo
            bool     isFinal = (stepsQueued == totalSteps - 1);

            int8_t ex, ey;
            if (isFinal) {
                ex = remX;
                ey = remY;
            } else {
                ex = ((spacingX == 1) || (step % spacingX == 0)) ? stepX : 0;
                ey = ((spacingY == 1) || (step % spacingY == 0)) ? stepY : 0;
            }
            _moveBufPush(ex, ey);
            stepsQueued++;
        }
        portEXIT_CRITICAL(&_spinlock);

        if (stepsQueued < totalSteps) vTaskDelay(pdMS_TO_TICKS(intervalMs));
    }

    // Block until the send task has drained everything queued.
    bool empty = false;
    while (!empty) {
        portENTER_CRITICAL(&_spinlock);
        empty = (_moveBufCount == 0);
        portEXIT_CRITICAL(&_spinlock);
        if (!empty) vTaskDelay(pdMS_TO_TICKS(intervalMs));
    }
}

void AirMouse::scroll(int16_t dz) {
    portENTER_CRITICAL(&_spinlock);
    _remainingScroll += dz;
    portEXIT_CRITICAL(&_spinlock);
    if (_core) _core->markInput();
}

void AirMouse::scrollH(int16_t dz) {
    if (!_horizontalScroll) return;
    portENTER_CRITICAL(&_spinlock);
    _remainingScrollH += dz;
    portEXIT_CRITICAL(&_spinlock);
    if (_core) _core->markInput();
}

void AirMouse::press(MouseButton button)   { setButton(button, true); }
void AirMouse::release(MouseButton button) { setButton(button, false); }
void AirMouse::releaseAll()                { setButtons(0x00); }

void AirMouse::click(MouseButton button, uint16_t releaseDelay_ms) {
    uint16_t d = (releaseDelay_ms == 0) ? 2 * _reportIntervalMs() : releaseDelay_ms;
    press(button);
    vTaskDelay(pdMS_TO_TICKS(d));
    release(button);
}

void AirMouse::doubleClick(MouseButton button, uint16_t releaseDelay_ms,
                                               uint16_t betweenDelay_ms) {
    uint16_t rel  = (releaseDelay_ms == 0) ? 2 * _reportIntervalMs() : releaseDelay_ms;
    uint16_t btwn = (betweenDelay_ms  == 0) ? 2 * _reportIntervalMs() : betweenDelay_ms;
    press(button);
    vTaskDelay(pdMS_TO_TICKS(rel));
    release(button);
    vTaskDelay(pdMS_TO_TICKS(btwn));
    press(button);
    vTaskDelay(pdMS_TO_TICKS(rel));
    release(button);
}

// ---------------------------------------------------------------------------
// moveTo() FIFO — callers hold _spinlock
// ---------------------------------------------------------------------------

bool AirMouse::_moveBufPush(int8_t x, int8_t y) {
    if (_moveBufCount >= AIRHID_MOVE_BUFFER_SIZE) return false;
    _moveBuffer[_moveBufTail].x = x;
    _moveBuffer[_moveBufTail].y = y;
    _moveBufTail = (_moveBufTail + 1) % AIRHID_MOVE_BUFFER_SIZE;
    _moveBufCount++;
    return true;
}

void AirMouse::_moveBufPop() {
    if (_moveBufCount == 0) return;
    _state.x = _moveBuffer[_moveBufHead].x;
    _state.y = _moveBuffer[_moveBufHead].y;
    _moveBufHead = (_moveBufHead + 1) % AIRHID_MOVE_BUFFER_SIZE;
    _moveBufCount--;
}

// ---------------------------------------------------------------------------
// Clamping. The descriptor declares -127..127, not -128.
// ---------------------------------------------------------------------------

int8_t AirMouse::_clampInt8(int16_t value) {
    if (value >  127) return  127;
    if (value < -127) return -127;
    return (int8_t)value;
}

int8_t AirMouse::_clampDrain(int32_t& acc) {
    int8_t chunk;
    if      (acc >  127) chunk =  127;
    else if (acc < -127) chunk = -127;
    else                 chunk = (int8_t)acc;
    acc -= chunk;
    return chunk;
}
