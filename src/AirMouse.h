#pragma once
/**
 * AirMouse.h
 *
 * Mouse report for AirHID. Buttons, relative X/Y, vertical wheel, and
 * optional horizontal scroll (AC Pan).
 *
 * Two layers:
 *   - Base layer (move, addScroll, setButton, setButtons) is ISR-safe. It only
 *     mutates state under a spinlock and never touches BLE.
 *   - Macro layer (moveTo, scroll, click, doubleClick) blocks the calling task
 *     and must run from loop() or a FreeRTOS task.
 *
 * The core's send task drains state and sends at the report interval.
 *
 * Copyright (c) 2026. Licensed under the Apache License, Version 2.0.
 * Derived from HijelHID_BLEMouse (c) 2026 Hijel.
 */

#include "AirHID.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * Mouse buttons. `Back` and `Forward` are only live in 5-button mode.
 */
enum class MouseButton : uint8_t {
    Left    = 0x01,
    Right   = 0x02,
    Middle  = 0x04,
    Back    = 0x08,   // 5-button mode only
    Forward = 0x10,   // 5-button mode only
};

/**
 * moveTo() FIFO capacity in entries. Each entry is 2 bytes, so the default
 * costs 256 bytes of RAM. Override before including this header.
 */
#ifndef AIRHID_MOVE_BUFFER_SIZE
#define AIRHID_MOVE_BUFFER_SIZE 128
#endif
#if AIRHID_MOVE_BUFFER_SIZE < 1
#error "AIRHID_MOVE_BUFFER_SIZE must be at least 1"
#endif

class AirMouse : public AirHIDReport {
public:

    /**
     * `buttonCount`      — 3 or 5. Back/Forward are only live at 5. [Default: 5]
     * `horizontalScroll` — adds AC Pan as a fifth report axis. [Default: false]
     *
     * Both shape the HID descriptor, so both are latched at `AirHID::begin()`.
     */
    explicit AirMouse(uint8_t buttonCount = 5, bool horizontalScroll = false);

    const char* reportName() const override { return "mouse"; }

    // -----------------------------------------------------------------------
    // Configuration — before AirHID::begin()
    // -----------------------------------------------------------------------

    /** 3 or 5 buttons. Any other value is treated as 3. */
    void setButtonCount(uint8_t count);

    /** Enable horizontal scroll (AC Pan). Adds a fifth byte to every report. */
    void setHorizontalScroll(bool enable);

    // -----------------------------------------------------------------------
    // Base layer — ISR-safe, non-blocking
    // -----------------------------------------------------------------------

    /**
     * Set the relative movement for the next report. ISR-safe.
     *
     * Clamped to ±127; excess is discarded. Calling twice within one report
     * interval overwrites — use `moveTo()` when every delta matters.
     */
    void move(int16_t dx, int16_t dy);

    /** Accumulate vertical scroll. Negative = down, positive = up. ISR-safe. */
    void addScroll(int8_t dz);

    /** Accumulate horizontal scroll. No effect unless AC Pan is enabled. ISR-safe. */
    void addScrollH(int8_t dz);

    /** Set or clear one button. ISR-safe. */
    void setButton(MouseButton button, bool pressed);

    /** Replace the whole button bitmask at once. ISR-safe. */
    void setButtons(uint8_t mask);

    // -----------------------------------------------------------------------
    // Macro layer — blocking, loop task only
    // -----------------------------------------------------------------------

    /**
     * Move by dx, dy spread across multiple reports. Blocks until complete.
     *
     * `durationMs = 0` moves as fast as the report rate allows. Otherwise the
     * movement is spread over the requested duration; if that works out to
     * less than one pixel per report, ±1 is injected every N reports instead.
     *
     * A new call discards any move still in progress.
     */
    void moveTo(int16_t dx, int16_t dy, uint32_t durationMs = 0);

    /** Queue a large vertical scroll, drained across several reports. */
    void scroll(int16_t dz);

    /** Queue a large horizontal scroll (AC Pan). */
    void scrollH(int16_t dz);

    /** Hold a button down until `release()` or `releaseAll()`. */
    void press(MouseButton button);

    /** Release a held button. */
    void release(MouseButton button);

    /** Release every button at once. */
    void releaseAll();

    /**
     * Press then release. `releaseDelay_ms = 0` uses two report intervals
     * (~15 ms at 125 Hz), enough for the host to see two distinct
     * notifications. Raise it to 20+ ms if clicks are missed.
     */
    void click(MouseButton button, uint16_t releaseDelay_ms = 0);

    /** Two clicks. Zero delays mean two report intervals each. */
    void doubleClick(MouseButton button, uint16_t releaseDelay_ms = 0,
                                         uint16_t betweenDelay_ms = 0);

    /** HID report ID assigned by the core, or 0 before `begin()`. */
    uint8_t reportId() const { return _reportId; }

protected:
    uint16_t buildDescriptor(uint8_t* out, uint16_t maxLen) override;
    void     onAttach() override;
    void     onTick() override;
    void     onDisconnect() override;

private:
    uint8_t _reportId        = 0;
    uint8_t _buttonCount     = 5;
    bool    _horizontalScroll = false;

    // Spinlock guarding every field below. Must be a named instance —
    // portENTER_CRITICAL_ISR takes a pointer.
    portMUX_TYPE _spinlock = portMUX_INITIALIZER_UNLOCKED;

    struct InputState {
        uint8_t buttons = 0;
        int8_t  x       = 0;
        int8_t  y       = 0;
        int8_t  scrollV = 0;
        int8_t  scrollH = 0;
    };
    InputState _state;

    // Scroll remainders, filled by scroll()/scrollH() and drained one int8
    // chunk per report.
    int32_t _remainingScroll  = 0;
    int32_t _remainingScrollH = 0;

    // moveTo() FIFO — one entry per report's worth of movement. Written by
    // moveTo() on the caller's task, drained by the core's send task.
    struct MoveEntry { int8_t x; int8_t y; };
    MoveEntry         _moveBuffer[AIRHID_MOVE_BUFFER_SIZE];
    volatile uint16_t _moveBufHead  = 0;
    volatile uint16_t _moveBufTail  = 0;
    volatile uint16_t _moveBufCount = 0;

    // True if the previous report carried anything. Lets onTick() stay silent
    // while nothing is happening but still transmit the final zero report that
    // tells the host a button came up.
    bool _lastReportActive = false;

    void     _writePointerBlock(uint8_t*& p) const;
    uint16_t _reportIntervalMs() const;
    bool     _moveBufPush(int8_t x, int8_t y);   // caller holds _spinlock
    void     _moveBufPop();                      // caller holds _spinlock

    static int8_t _clampInt8(int16_t value);
    static int8_t _clampDrain(int32_t& acc);
};
