#include "user_app.h"

#include <Arduino.h>

#include "board_pins.h"

// Example: the LED 5 s on, 5 s off. Set to 1 and flash; set back to 0 to get the CAN status LED back.
#define USER_BLINK_DEMO 0

void user_setup() {
#if USER_BLINK_DEMO && !BENCH_LED_IS_RGB
  pinMode(BENCH_LED_GPIO, OUTPUT);
#endif
}

void user_loop(uint32_t now_ms) {
#if USER_BLINK_DEMO
  static int shown = -1;
  const int on = (now_ms / 5000) % 2 == 0;
  if (on == shown) return;  // only touch the LED when it changes
  shown = on;
#if BENCH_LED_IS_RGB
  rgbLedWrite(BENCH_LED_GPIO, on ? 24 : 0, on ? 24 : 0, on ? 24 : 0);  // white
#else
  digitalWrite(BENCH_LED_GPIO, on ? HIGH : LOW);
#endif
#else
  (void)now_ms;
#endif
}

bool user_owns_led() { return USER_BLINK_DEMO; }
