/**
 * CompositeHID.ino — keyboard, mouse, and media keys over one BLE connection
 *
 * The host sees a single paired device that is all three at once. One HID
 * service, one report map, one pairing.
 *
 * Pair with "Air HID" from your host's Bluetooth settings, open a text editor,
 * then press the BOOT button (GPIO 0):
 *
 *   - types a line of text
 *   - moves the pointer in a square
 *   - double-clicks
 *   - nudges the volume up twice and back down twice
 *
 * The Caps Lock LED on the host is reported back through onLEDChange().
 */

#include <AirHID.h>
#include <AirKeyboard.h>
#include <AirMouse.h>
#include <AirConsumer.h>

AirHID      hid("Air HID", "AirHID");
AirKeyboard keyboard;
AirMouse    mouse;                 // 5 buttons, no horizontal scroll
AirConsumer consumer;              // media, volume, brightness, browser keys

const int BUTTON_PIN = 0;          // BOOT button on most ESP32 dev boards

// Runs on the NimBLE task — keep it short.
void onLeds(uint8_t leds) {
    Serial.printf("Host LEDs: caps=%d num=%d scroll=%d\n",
                  (leds & AIRHID_LED_CAPS_LOCK)   ? 1 : 0,
                  (leds & AIRHID_LED_NUM_LOCK)    ? 1 : 0,
                  (leds & AIRHID_LED_SCROLL_LOCK) ? 1 : 0);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("AirHID — composite keyboard + mouse");

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    hid.setLogLevel(HIDLogLevel::Normal);

    // Registration order fixes the report IDs: keyboard 1, mouse 2, consumer 3.
    // Register new report types at the END so existing IDs do not shift.
    // Any change here changes the descriptor and invalidates every existing
    // pairing, so keep it stable once you ship.
    hid.addReport(keyboard);
    hid.addReport(mouse);
    hid.addReport(consumer);

    keyboard.onLEDChange(onLeds);

    if (!hid.begin()) {
        Serial.println("begin() failed — halted");
        while (true) delay(1000);
    }

    Serial.println("Advertising. Pair, then press BOOT.");
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
        delay(50);                                    // debounce
        if (digitalRead(BUTTON_PIN) != LOW) return;

        Serial.println("Typing...");
        keyboard.println("Hello from AirHID - one device, both roles.");

        // Ctrl+A then Ctrl+C, using held modifiers
        keyboard.press(KEY_LCTRL);
        keyboard.tap(KEY_A);
        keyboard.tap(KEY_C);
        keyboard.release(KEY_LCTRL);

        Serial.println("Moving...");
        mouse.moveTo( 120,    0, 300);
        mouse.moveTo(   0,  120, 300);
        mouse.moveTo(-120,    0, 300);
        mouse.moveTo(   0, -120, 300);

        mouse.doubleClick(MouseButton::Left);

        Serial.println("Volume...");
        consumer.tap(MEDIA_VOLUME_UP);
        consumer.tap(MEDIA_VOLUME_UP);
        consumer.tap(MEDIA_VOLUME_DOWN);
        consumer.tap(MEDIA_VOLUME_DOWN);

        Serial.printf("Idle for %lu ms\n", (unsigned long)hid.getIdleTime());

        while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    }
    delay(10);
}
