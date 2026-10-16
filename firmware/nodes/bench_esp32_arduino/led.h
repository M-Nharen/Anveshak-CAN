/* Status LED: a short flash for every frame the bus ACKed, fast blink while waiting for a node ID,
 * solid while bus-off. On the S3's RGB LED: green = sent, blue = waiting for an ID, red = bus-off. */
#ifndef BENCH_LED_H
#define BENCH_LED_H

#include <stdint.h>

enum class LedMode : uint8_t { WaitingForId, Running, BusOff };

void led_init();
// Call every loop iteration. tx_ok_count is the driver's running count of ACKed frames.
void led_update(uint32_t now_ms, LedMode mode, uint32_t tx_ok_count);

#endif /* BENCH_LED_H */
