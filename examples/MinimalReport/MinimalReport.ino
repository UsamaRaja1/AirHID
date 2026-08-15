/**
 * MinimalReport.ino — AirHID Phase 1 smoke test
 *
 * AirHID's core owns the BLE stack but knows nothing about mice or keyboards.
 * Everything a host sees comes from an AirHIDReport subclass registered before
 * begin(). This sketch defines the smallest useful one — a Consumer Control
 * report that sends Volume Up — to prove the whole core path works:
 *
 *   descriptor assembly -> report ID claim -> characteristic creation ->
 *   advertising -> pairing -> notify
 *
 * It lives in the sketch on purpose. Mouse, keyboard, and consumer classes
 * arrive in later phases as proper library types; this stays as the reference
 * for writing your own report.
 *
 * Pair with "AirHID Core" from your host's Bluetooth settings, then press the
 * BOOT button (GPIO 0) to raise the volume.
 */

#include <AirHID.h>

// ---------------------------------------------------------------------------
// A minimal report: Consumer Control, one 16-bit usage ID per report.
// ---------------------------------------------------------------------------
class ConsumerReport : public AirHIDReport {
public:
    const char* reportName() const override { return "consumer"; }

    /** Send a Consumer Page usage. 0x0000 releases. */
    void send(uint16_t usageId) {
        if (_core == nullptr) return;
        uint8_t payload[2] = { (uint8_t)(usageId & 0xFF), (uint8_t)(usageId >> 8) };
        _core->markInput();
        _core->notify(_reportId, payload, sizeof(payload));
    }

    /** Press then release, so the host sees a discrete key event. */
    void tap(uint16_t usageId, uint16_t holdMs = 25) {
        send(usageId);
        delay(holdMs);
        send(0x0000);
    }

protected:
    // The core calls this first, before the report map is assembled.
    void onAttach() override {
        _reportId = _core->claimReportId();
        _core->registerInput(_reportId, this);
    }

    // Emit this report's slice of the composite descriptor.
    uint16_t buildDescriptor(uint8_t* out, uint16_t maxLen) override {
        const uint8_t desc[] = {
            0x05, 0x0C,        // Usage Page (Consumer)
            0x09, 0x01,        // Usage (Consumer Control)
            0xA1, 0x01,        // Collection (Application)
            0x85, _reportId,   //   Report ID (claimed in onAttach)
            0x15, 0x00,        //   Logical Minimum (0)
            0x26, 0xFF, 0x03,  //   Logical Maximum (0x3FF)
            0x19, 0x00,        //   Usage Minimum (0)
            0x2A, 0xFF, 0x03,  //   Usage Maximum (0x3FF)
            0x75, 0x10,        //   Report Size (16 bits)
            0x95, 0x01,        //   Report Count (1)
            0x81, 0x00,        //   Input (Data, Array, Absolute)
            0xC0,              // End Collection
        };
        if (sizeof(desc) > maxLen) return 0;
        memcpy(out, desc, sizeof(desc));
        return sizeof(desc);
    }

    // Connection gone — nothing held, so nothing to clear.
    void onDisconnect() override {}

private:
    uint8_t _reportId = 0;
};

// ---------------------------------------------------------------------------

AirHID         hid("AirHID Core", "AirHID");
ConsumerReport consumer;

const int BUTTON_PIN = 0;   // BOOT button on most ESP32 dev boards

#define MEDIA_VOLUME_UP 0x00E9

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("AirHID — Phase 1 core smoke test");

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    hid.setLogLevel(HIDLogLevel::Normal);

    // Registration order fixes report ID assignment: this one gets ID 1.
    hid.addReport(consumer);

    if (!hid.begin()) {
        Serial.println("begin() failed — halted");
        while (true) delay(1000);
    }

    Serial.println("Advertising. Pair, then press BOOT for volume up.");
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
            Serial.println("Volume up");
            consumer.tap(MEDIA_VOLUME_UP);
            while (digitalRead(BUTTON_PIN) == LOW) delay(10);
        }
    }
    delay(10);
}
