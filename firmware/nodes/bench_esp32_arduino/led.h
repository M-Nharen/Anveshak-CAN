/* Status LED: a short flash for every frame the bus ACKed, fast blink when another board has the
 * same node ID (check the DIP switches), solid while bus-off, slow even blink while identifying
 * (NMT identify or the `blink` action).
 * On the S3's RGB LED: green = sent, red blink = duplicate ID, red = bus-off, white = identify. */
#ifndef BENCH_LED_H
#define BENCH_LED_H

#include <stdint.h>

enum class LedMode : uint8_t { IdConflict, Running, BusOff, Identify };

void led_init();
// Call every loop iteration. tx_ok_count is the driver's running count of ACKed frames.
void led_update(uint32_t now_ms, LedMode mode, uint32_t tx_ok_count);

#endif /* BENCH_LED_H */
