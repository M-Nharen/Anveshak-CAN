#include "led.h"

#include <Arduino.h>

namespace {

constexpr uint32_t kFlashMs = 15;    // on-time per sent frame
constexpr uint32_t kMinGapMs = 35;   // off-time between flashes, so 10-20 frames/s still flicker
constexpr uint32_t kBlinkHalfMs = 125;
constexpr uint32_t kIdentifyHalfMs = 250;

enum class Color : uint8_t { Off, Green, Blue, Red, White };
Color s_shown = Color::Red;  // force the first write
int s_pin = -1;
bool s_rgb = false;
uint32_t s_last_tx = 0;
uint32_t s_flash_until = 0;

void show(Color c) {
  if (c == s_shown) return;  // the RGB LED is driven over RMT; only write on change
  s_shown = c;
  if (s_pin < 0) return;
  if (s_rgb) {
    const uint8_t level = 24;  // the bare RGB LED is blinding at full brightness
    const bool w = c == Color::White;
    rgbLedWrite(s_pin, c == Color::Red || w ? level : 0, c == Color::Green || w ? level : 0,
                c == Color::Blue || w ? level : 0);
  } else {
    digitalWrite(s_pin, c == Color::Off ? LOW : HIGH);
  }
}

}  // namespace

void led_init(int pin, bool rgb) {
  s_pin = pin;
  s_rgb = rgb;
  if (s_pin >= 0 && !s_rgb) pinMode(s_pin, OUTPUT);
  show(Color::Off);
}

void led_set(bool on) { show(on ? Color::White : Color::Off); }

void led_update(uint32_t now_ms, LedMode mode, uint32_t tx_ok_count) {
  switch (mode) {
    case LedMode::BusOff:
      show(Color::Red);
      return;
    case LedMode::IdConflict:
      show((now_ms / kBlinkHalfMs) % 2 ? Color::Red : Color::Off);
      return;
    case LedMode::WaitingForId:
      show((now_ms / kIdentifyHalfMs) % 2 ? Color::Blue : Color::Off);
      return;
    case LedMode::Identify:
      show((now_ms / kIdentifyHalfMs) % 2 ? Color::White : Color::Off);
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
