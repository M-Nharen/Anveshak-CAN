// The smallest node: type `blank` (schema/types/blank.yaml), no messages of its own. Use it to
// test a board: CAN wiring, the node ID, heartbeat, settings and flashing over CAN.
//
// Pins come from the library for this chip (anv_board_esp32.h). The node ID: a new board asks the
// Pi's ID service; a board with DIP switches uses them once set to id_source = dip.
#include <Arduino.h>

#include "anv_board_esp32.h"
#include "src/schema/anv_blank.h"

static anv_board_t s_board;

static void outputs_off(anv_node_t*, void*) {}  // required; a blank board drives nothing

static void state_changed(anv_node_t* node, uint8_t from, uint8_t to, void*) {
  Serial.printf("%s -> %s, node ID %u\n", anv_node_state_name(from), anv_node_state_name(to), anv_node_id(node));
}

void setup() {
  Serial.begin(115200);
  anv_node_config_t cfg;
  anv_node_config_default(&cfg);
  cfg.type = &anv_blank_type;
  cfg.outputs_off = outputs_off;
  cfg.on_state_changed = state_changed;
  if (!anv_board_begin(&s_board, &cfg, NULL)) {  // NULL = this chip's default pins
    Serial.printf("blank node: %s\n", anv_board_error(&s_board));
  }
}

void loop() {
  anv_board_poll(&s_board);
  delay(1);
}
