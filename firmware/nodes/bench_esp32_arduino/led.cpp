#include "led.h"

#include <Arduino.h>

#include "board_pins.h"

namespace {

constexpr uint32_t kFlashMs = 15;    // on-time per sent frame
constexpr uint32_t kMinGapMs = 35;   // off-time between flashes, so 10-20 frames/s still flicker
constexpr uint32_t kBlinkHalfMs = 125;

enum class Color : uint8_t { Off, Green, Blue, Red };
Color s_shown = Color::Red;  // force the first write
uint32_t s_last_tx = 0;
uint32_t s_flash_until = 0;

void show(Color c) {
  if (c == s_shown) return;  // the RGB LED is driven over RMT; only write on change
  s_shown = c;
#if BENCH_LED_IS_RGB
  const uint8_t level = 24;  // the bare RGB LED is blinding at full brightness
  rgbLedWrite(BENCH_LED_GPIO, c == Color::Red ? level : 0, c == Color::Green ? level : 0, c == Color::Blue ? level : 0);
#else
  digitalWrite(BENCH_LED_GPIO, c == Color::Off ? LOW : HIGH);
#endif
}

}  // namespace

void led_init() {
#if !BENCH_LED_IS_RGB
  pinMode(BENCH_LED_GPIO, OUTPUT);
#endif
  show(Color::Off);
}

void led_update(uint32_t now_ms, LedMode mode, uint32_t tx_ok_count) {
  switch (mode) {
    case LedMode::BusOff:
      show(Color::Red);
      return;
    case LedMode::WaitingForId:
      show((now_ms / kBlinkHalfMs) % 2 ? Color::Blue : Color::Off);
      return;
    case LedMode::Running:
      if (tx_ok_count != s_last_tx && (int32_t)(now_ms - s_flash_until) >= (int32_t)kMinGapMs) {
        s_last_tx = tx_ok_count;
        s_flash_until = now_ms + kFlashMs;
      }
      show((int32_t)(now_ms - s_flash_until) < 0 ? Color::Green : Color::Off);
      return;
  }
}
