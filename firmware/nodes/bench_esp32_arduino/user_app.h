/* Your own code, running inside the bench node next to the CAN side.
 *
 * Put experiments here (blink an LED, read a sensor). anv_node calls user_setup() once it has
 * loaded its settings and user_loop() on every pass, so the board stays a live node on the bus
 * while your code runs. (Flashing over CAN comes back with the v2 update service, L5.)
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
