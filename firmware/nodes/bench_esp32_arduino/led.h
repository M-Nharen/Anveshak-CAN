/* Status LED: a short flash for every frame the bus ACKed, fast blink when another board has the
 * same node ID (check the DIP switches), solid while bus-off, slow even blink while identifying
 * (NMT identify or the `blink` action).
 * On the S3's RGB LED: green = sent, red blink = duplicate ID, red = bus-off, white = identify. */
#ifndef BENCH_LED_H
#define BENCH_LED_H

#include <stdint.h>

// IdConflict: red blink. WaitingForId: blue blink (asking the Pi's ID service). Running: green
// flash per sent frame. BusOff (or CAN didn't start): solid red. Identify: white blink.
enum class LedMode : uint8_t { IdConflict, WaitingForId, Running, BusOff, Identify };

// pin and rgb come from the board's pins (anv_board_esp32: per chip).
void led_init(int pin, bool rgb);
// Set the LED by hand (white on an RGB LED).
void led_set(bool on);
// Call every loop iteration. tx_ok_count is the driver's running count of ACKed frames.
void led_update(uint32_t now_ms, LedMode mode, uint32_t tx_ok_count);

#endif /* BENCH_LED_H */
