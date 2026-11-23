#include "user_app.h"

#include <Arduino.h>

#include "led.h"

// Example: the LED 5 s on, 5 s off. Set to 1 and flash; set back to 0 to get the CAN status LED back.
#define USER_BLINK_DEMO 0

void user_setup() {}

void user_loop(uint32_t now_ms) {
#if USER_BLINK_DEMO
  static int shown = -1;
  const int on = (now_ms / 5000) % 2 == 0;
  if (on == shown) return;  // only touch the LED when it changes
  shown = on;
  led_set(on);
#else
  (void)now_ms;
#endif
}

bool user_owns_led() { return USER_BLINK_DEMO; }
