/**
 * MinimalReport.ino — writing your own AirHIDReport
 *
 * AirHID ships keyboard, mouse, and consumer reports, but the core has no
 * built-in knowledge of any of them: every report type is just an
 * AirHIDReport registered before begin(). This sketch writes one by hand — a
 * System Control report (power down / sleep / wake up) — to show the whole
 * contract in one file:
 *
 *   onAttach()        claim a report ID and register a channel
 *   buildDescriptor() emit this report's slice of the composite map
 *   notify()          send a payload through the core
 *
 * Only Wake Up is wired to the button. Power Down and Sleep are declared in
 * the descriptor and left unsent on purpose — this is an example, and it
 * should not be able to suspend your machine by accident.
 *
 * Pair with "AirHID Custom", then press BOOT (GPIO 0) to send a wake request.
 */

#include <AirHID.h>

// System Control bits, matching the descriptor below.
#define SYSCTL_POWER_DOWN 0x01
#define SYSCTL_SLEEP      0x02
#define SYSCTL_WAKE_UP    0x04

// ---------------------------------------------------------------------------
// A hand-written report: System Control, one byte of bit flags.
// ---------------------------------------------------------------------------
class SystemControlReport : public AirHIDReport {
public:
    const char* reportName() const override { return "system-control"; }

    /** Set the active bits. 0x00 releases. */
    void send(uint8_t bits) {
        if (_core == nullptr) return;
        _core->markInput();
        _core->notify(_reportId, &bits, 1);
    }

    /** Pulse one bit, so the host sees a discrete press and release. */
    void tap(uint8_t bits, uint16_t holdMs = 25) {
        send(bits);
        delay(holdMs);
        send(0x00);
    }

protected:
    // Called first, before the report map is assembled.
    void onAttach() override {
        _reportId = _core->claimReportId();
        _core->registerInput(_reportId, this);
    }

    uint16_t buildDescriptor(uint8_t* out, uint16_t maxLen) override {
        const uint8_t desc[] = {
            0x05, 0x01,        // Usage Page (Generic Desktop)
            0x09, 0x80,        // Usage (System Control)
            0xA1, 0x01,        // Collection (Application)
            0x85, _reportId,   //   Report ID (claimed in onAttach)
            0x15, 0x00,        //   Logical Minimum (0)
            0x25, 0x01,        //   Logical Maximum (1)
            0x75, 0x01,        //   Report Size (1 bit)
            0x95, 0x03,        //   Report Count (3)
            0x09, 0x81,        //   Usage (System Power Down)  -> bit 0
            0x09, 0x82,        //   Usage (System Sleep)       -> bit 1
            0x09, 0x83,        //   Usage (System Wake Up)     -> bit 2
            0x81, 0x02,        //   Input (Data, Var, Abs)
            0x95, 0x05,        //   Report Count (5)
            0x81, 0x01,        //   Input (Const) — pad to a whole byte
            0xC0,              // End Collection
        };
        if (sizeof(desc) > maxLen) return 0;
        memcpy(out, desc, sizeof(desc));
        return sizeof(desc);
    }

    // Connection gone — nothing is held, so nothing to clear.
    void onDisconnect() override {}

private:
    uint8_t _reportId = 0;
};

// ---------------------------------------------------------------------------

AirHID              hid("AirHID Custom", "AirHID");
SystemControlReport sysCtl;

const int BUTTON_PIN = 0;   // BOOT button on most ESP32 dev boards

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("AirHID — custom report example");

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    hid.setLogLevel(HIDLogLevel::Normal);
    hid.addReport(sysCtl);      // claims report ID 1

    if (!hid.begin()) {
        Serial.println("begin() failed — halted");
        while (true) delay(1000);
    }

    Serial.println("Advertising. Pair, then press BOOT to send Wake Up.");
}

void loop() {
    if (!hid.isPaired()) {
        static uint32_t lastPrint = 0;
        if (millis() - lastPrint > 3000) {
            Serial.println("Waiting for a host...");
            lastPrint = millis();
        }
        delay(50);
        return;
    }

    if (digitalRead(BUTTON_PIN) == LOW) {
        delay(50);                                  // debounce
        if (digitalRead(BUTTON_PIN) == LOW) {
            Serial.println("System Wake Up");
            sysCtl.tap(SYSCTL_WAKE_UP);
            while (digitalRead(BUTTON_PIN) == LOW) delay(10);
        }
    }
    delay(10);
}
