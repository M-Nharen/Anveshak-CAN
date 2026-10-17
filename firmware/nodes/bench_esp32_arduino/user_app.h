/* Your own code, running inside the bench node next to the CAN side.
 *
 * Put experiments here (blink an LED, read a sensor) and flash them over CAN: the sketch keeps its
 * CAN link and update receiver, so the board stays on the screen and can always be reflashed. A
 * sketch without them would drop off the bus and need USB again.
 *
 * Rule: never block. No delay(), no long loops: use the millis() value passed in. A blocked loop
 * stops heartbeats and CAN replies. */
#ifndef BENCH_USER_APP_H
#define BENCH_USER_APP_H

#include <stdint.h>

void user_setup();
void user_loop(uint32_t now_ms);
// true = the status LED is yours (the CAN status flashes stop); false = leave it to the node.
bool user_owns_led();

#endif /* BENCH_USER_APP_H */
