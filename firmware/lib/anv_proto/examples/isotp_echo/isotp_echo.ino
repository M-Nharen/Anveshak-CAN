// isotp_echo: anv_link (TWAI, 29-bit IDs) + anv_proto (ISO-TP) on an ESP32 or ESP32-S3.
//
// Flash it on two boards on one bus. Each picks a node ID from its MAC (this is only a link
// demo: real nodes get IDs from the host, see L3) and echoes every transfer it receives back to
// the sender. In the Serial Monitor (115200, newline), `send <id> <bytes>` sends a test
// transfer to another board and checks the echo; `stats` prints the link counters.
#include <Arduino.h>

#include "anv_isotp.h"
#include "anv_link.h"
#include "anv_link_twai.h"

// Pins as wired on the bench boards (firmware/nodes/bench_esp32_arduino/board_pins.h).
#if defined(CONFIG_IDF_TARGET_ESP32S3)
constexpr int kCanTx = 16, kCanRx = 15;
#else
constexpr int kCanTx = 4, kCanRx = 5;
#endif

static anv_twai_t twai;
static anv_link_t can_link;  // not `link`: that is a POSIX function
static anv_isotp_t tp;
static anv_isotp_config_t tp_cfg;
static anv_node_id_t self_id;

static uint8_t rx_buf[ANV_ISOTP_MAX_LEN];
static uint8_t tx_buf[ANV_ISOTP_MAX_LEN];
static uint16_t test_len;      // > 0 while our own test transfer waits for its echo
static uint32_t test_start_us;

static bool send_frame(void*, const anv_link_frame_t* f) { return anv_link_send(&can_link, f) == ANV_LINK_OK; }

static void on_rx(void*, const uint8_t* data, uint16_t len) {
  if (test_len != 0) {  // the echo of our test
    const bool same = len == test_len && memcmp(data, tx_buf, len) == 0;
    Serial.printf("echo from %u: %u bytes, %s, %lu ms\n", tp.cfg.peer, len, same ? "matches" : "DIFFERENT",
                  (unsigned long)((micros() - test_start_us) / 1000));
    test_len = 0;
    return;
  }
  memcpy(tx_buf, data, len);  // anv_isotp_send() keeps a pointer: send from our own buffer
  const anv_isotp_result_t r = anv_isotp_send(&tp, tx_buf, len, micros());
  Serial.printf("got %u bytes from %u, echoing: %s\n", len, tp.cfg.peer, anv_isotp_result_name(r));
}

static void on_tx_done(void*, anv_isotp_result_t r) {
  if (r != ANV_ISOTP_OK) Serial.printf("send failed: %s\n", anv_isotp_result_name(r));
}

static void on_rx_error(void*, anv_isotp_result_t r) { Serial.printf("receive failed: %s\n", anv_isotp_result_name(r)); }

// One channel, re-pointed at whichever peer we talk to while it is idle.
static bool use_peer(anv_node_id_t peer) {
  if (tp.cfg.peer == peer) return true;
  if (anv_isotp_tx_busy(&tp) || anv_isotp_rx_busy(&tp)) return false;
  tp_cfg.peer = peer;
  return anv_isotp_init(&tp, &tp_cfg) == ANV_ISOTP_OK;
}

static void print_stats() {
  const anv_link_stats_t* s = anv_link_stats(&can_link);
  Serial.printf("node %u: state %d tec %u rec %u | tx %lu rejected %lu failed %lu | rx %lu filtered %lu "
                "overflow %lu | bus-off %lu\n",
                self_id, s->state, s->tec, s->rec, (unsigned long)s->tx_frames, (unsigned long)s->tx_rejected,
                (unsigned long)s->tx_failed, (unsigned long)s->rx_frames, (unsigned long)s->rx_filtered,
                (unsigned long)s->rx_overflow, (unsigned long)s->bus_off_count);
}

static void handle_command(const String& line) {
  unsigned peer = 0, len = 0;
  if (sscanf(line.c_str(), "send %u %u", &peer, &len) == 2) {
    if (peer < ANV_NODE_FIRST || peer > ANV_NODE_LAST || peer == self_id || len < 1 || len > ANV_ISOTP_MAX_LEN) {
      Serial.println("usage: send <node id> <1..4095>");
    } else if (test_len != 0 || !use_peer(peer)) {
      Serial.println("busy");
    } else {
      for (unsigned i = 0; i < len; i++) tx_buf[i] = uint8_t(i * 7 + (i >> 8));
      test_len = len;
      test_start_us = micros();
      anv_isotp_send(&tp, tx_buf, len, test_start_us);
    }
  } else if (line == "stats") {
    print_stats();
  }
}

void setup() {
  Serial.begin(115200);
  self_id = anv_node_id_t(ANV_NODE_FIRST + (ESP.getEfuseMac() >> 40) % (ANV_NODE_LAST - ANV_NODE_FIRST + 1));

  anv_twai_setup(&twai, kCanTx, kCanRx);
  anv_link_init(&can_link, anv_twai_ops(), &twai);
  anv_link_config_t cfg;
  anv_link_config_default(&cfg);
  cfg.n_filters = anv_proto_node_filters(self_id, cfg.filters);  // done by the TWAI filter in hardware
  const int rc = anv_link_start(&can_link, &cfg);

  anv_isotp_config_default(&tp_cfg);
  tp_cfg.self = self_id;
  tp_cfg.peer = ANV_NODE_FIRST;
  tp_cfg.rx_buf = rx_buf;
  tp_cfg.rx_buf_size = sizeof rx_buf;
  tp_cfg.send = send_frame;
  tp_cfg.on_rx = on_rx;
  tp_cfg.on_tx_done = on_tx_done;
  tp_cfg.on_rx_error = on_rx_error;
  anv_isotp_init(&tp, &tp_cfg);

  Serial.printf("isotp_echo: node %u, CAN %s at %lu bit/s\n", self_id, rc == ANV_LINK_OK ? "up" : "FAILED",
                (unsigned long)cfg.bitrate);
}

void loop() {
  const uint32_t now_us = micros();
  anv_link_frame_t f;
  while (anv_link_receive(&can_link, &f)) {
    // A transfer from a new peer takes the channel if it is free.
    if (anv_id_port(f.id) == ANV_TRANSFER_PORT && anv_id_dst(f.id) == self_id) use_peer(anv_id_src(f.id));
    anv_isotp_on_frame(&tp, &f, now_us);
  }
  anv_isotp_poll(&tp, now_us);

  const uint32_t ev = anv_link_poll(&can_link, millis());
  if (ev & ANV_LINK_EV_BUS_OFF) Serial.println("CAN bus-off, recovering");
  if (ev & ANV_LINK_EV_RECOVERED) Serial.println("CAN recovered");
  if (ev & ANV_LINK_EV_PASSIVE) Serial.println("CAN error-passive");

  static String line;
  while (Serial.available()) {
    const char c = char(Serial.read());
    if (c == '\n' || c == '\r') {
      if (line.length()) handle_command(line);
      line = "";
    } else {
      line += c;
    }
  }
}
