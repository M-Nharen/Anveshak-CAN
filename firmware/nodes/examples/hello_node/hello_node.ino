// My first node: blinks the LED and tells the CAN bus how many times it has blinked.
#include <Arduino.h>

#include "anv_board_esp32.h"        // CAN, storage and the node on this chip's pins
#include "src/schema/anv_hello.h"   // generated from schema/types/examples/hello.yaml

static anv_board_t s_board;
static anv_board_pins_t s_pins;     // this chip's wiring (for the LED pin)
static uint32_t s_blinks = 0;
static bool s_led_on = false;

static void led_write(bool on) {
  if (s_pins.led_rgb) {
    rgbLedWrite(s_pins.led, 0, on ? 24 : 0, 0);   // dim green on the S3's RGB LED
  } else {
    digitalWrite(s_pins.led, on ? HIGH : LOW);    // the WROOM's blue LED
  }
}

// Required by the library: switch every output off. A blinking LED is harmless, so nothing to do.
static void hook_outputs_off(anv_node_t*, void*) {}

// Called whenever the `status` stream is due (2 times a second unless the Pi changes it).
static bool hook_fill_stream(anv_node_t*, const anv_msg_desc_t* msg, uint8_t out[8], void*) {
  if (msg->port != ANV_HELLO_STATUS_PORT) return false;
  anv_hello_status_t st = {};
  st.blinks = s_blinks;
  st.led_on = s_led_on;
  anv_hello_status_pack(out, &st);   // generated: puts the fields into the 8 bytes
  return true;
}

// Called on every poll. Never use delay() here: check the time and come back later.
static void hook_loop(anv_node_t*, uint32_t now_ms, void*) {
  static uint32_t next_ms = 0;
  if ((int32_t)(now_ms - next_ms) < 0) return;
  next_ms = now_ms + 500;
  s_led_on = !s_led_on;
  if (s_led_on) s_blinks++;
  led_write(s_led_on);
}

// The node's state changed: print it (the first change shows the ID it got).
static void hook_state(anv_node_t* node, uint8_t from, uint8_t to, void*) {
  Serial.printf("%s -> %s, node ID %u\n", anv_node_state_name(from), anv_node_state_name(to), anv_node_id(node));
}

void setup() {
  Serial.begin(115200);
  anv_board_pins_default(&s_pins);
  if (!s_pins.led_rgb) pinMode(s_pins.led, OUTPUT);

  anv_node_config_t cfg;
  anv_node_config_default(&cfg);
  cfg.type = &anv_hello_type;           // which schema type this board is
  cfg.outputs_off = hook_outputs_off;
  cfg.fill_stream = hook_fill_stream;
  cfg.loop = hook_loop;
  cfg.on_state_changed = hook_state;
  // No pins and no node ID here: the library knows the pins, and the ID comes from the Pi
  // (or the DIP switches, on a board set to id_source = dip).

  if (!anv_board_begin(&s_board, &cfg, &s_pins)) {
    Serial.printf("hello node: %s\n", anv_board_error(&s_board));
  }
}

void loop() {
  anv_board_poll(&s_board);   // the library does its work here
  delay(1);
}
