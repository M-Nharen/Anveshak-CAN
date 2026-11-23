/* One call that starts a node on an ESP32 (WROOM) or ESP32-S3 board: CAN on the right pins, NVS,
 * the board's identity, CAN firmware updates, restart, and the DIP switches (read only if the
 * board is set to id_source = dip). The pins for each chip live here, so a sketch says nothing
 * about wiring unless a board is wired differently.
 *
 *   #include "anv_board_esp32.h"
 *   static anv_board_t board;
 *
 *   void setup() {
 *     anv_node_config_t cfg;
 *     anv_node_config_default(&cfg);
 *     cfg.type = &anv_blank_type;
 *     cfg.outputs_off = my_outputs_off;     // plus any other hooks
 *     if (!anv_board_begin(&board, &cfg, NULL)) Serial.println(anv_board_error(&board));
 *   }
 *   void loop() { anv_board_poll(&board); delay(1); }
 *
 * The node ID: leave cfg.node_id at 0. A new board asks the Pi's ID service for one (16 and up)
 * and keeps it. A board with switches: set it once to id_source = dip
 * (`anv-host set can1 <id> id_source=dip --save`, then reset) and its switches are the ID from
 * then on; or set cfg.dip_by_default = true to make that this build's default.
 */
#ifndef ANV_BOARD_ESP32_H
#define ANV_BOARD_ESP32_H

#include "anv_link.h"
#include "anv_link_twai.h"
#include "anv_node.h"
#include "anv_node_esp32.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_BOARD_MAX_DIP 8u

/* The wiring. anv_board_pins_default() fills in this chip's standard bench wiring; change a
 * field for a board that's wired differently. -1 = not connected. Each default can also be
 * changed for a whole build with a -D flag of the same name (ANV_BOARD_CAN_TX=..., etc.). */
typedef struct {
    int can_tx;      /* to the transceiver's TX/CTX/D pin */
    int can_rx;      /* to its RX/CRX/R pin */
    int tcan_stb;    /* transceiver standby pin, driven low at start; -1 if tied to GND */
    int led;         /* status LED, for the app to drive (the library leaves it alone) */
    bool led_rgb;    /* an addressable RGB LED (S3 DevKits) rather than a plain one */
    int dip[ANV_BOARD_MAX_DIP]; /* switches to GND, bit 0 first */
    uint8_t n_dip;   /* 0 = this board has no switches */
    uint32_t bitrate;
} anv_board_pins_t;

typedef struct {
    anv_board_pins_t pins;
    anv_twai_t twai;
    anv_link_t link;
    anv_node_nvs_t nvs;
    anv_node_t node;
    const char *error; /* why anv_board_begin failed, for people */
    bool ok;
} anv_board_t;

/* This chip's defaults:
 *   ESP32-S3: CAN TX 16, RX 15; RGB LED (the board variant's, else 48); DIP 4, 5, 6, 7
 *   ESP32 (WROOM): CAN TX 4, RX 5; LED 2; DIP 13, 14, 25, 26 */
void anv_board_pins_default(anv_board_pins_t *pins);

/* Start CAN, storage and identity, fill the board parts of cfg (link, storage, MAC, OTA, restart,
 * DIP reader), then anv_node_init + anv_node_begin. cfg needs the type, outputs_off and your
 * hooks. pins NULL = this chip's defaults. False (see anv_board_error) if CAN or the node can't
 * start; the node then stays off the bus and anv_board_poll does nothing. */
bool anv_board_begin(anv_board_t *board, anv_node_config_t *cfg, const anv_board_pins_t *pins);
/* Call from loop(): runs the node with the current time. */
void anv_board_poll(anv_board_t *board);
anv_node_t *anv_board_node(anv_board_t *board);
const char *anv_board_error(const anv_board_t *board);

/* The DIP switches as a cfg.read_dip hook (dip_ctx = the anv_board_pins_t). anv_board_begin sets
 * this up; it's here for sketches that start the node by hand. */
uint16_t anv_board_read_dip(void *pins);

#ifdef __cplusplus
}
#endif

#endif /* ANV_BOARD_ESP32_H */
