// One drive wheel on anv_node. Same sketch on all four wheels.
// Wheel IDs: with DIP switches set 1-4 and id_source = dip (front-left 1, front-right 2,
// rear-left 3, rear-right 4), or IDs from the Pi moved there with `anv-host set-id`.
#include <Arduino.h>

#include "anv_board_esp32.h"
#include "src/schema/anv_wheel.h"

constexpr int PWM_PIN = 18, DIR_PIN = 19, ENABLE_PIN = 21;   // your motor driver (WROOM)

static anv_board_t s_board;
static float s_target = 0;     // rad/s, from the leader
static float s_measured = 0;   // rad/s, from the encoder

// ---- your hardware: replace these four ----
static void motor_enable(bool on) { digitalWrite(ENABLE_PIN, on ? HIGH : LOW); }
static void motor_drive(float duty) {   // -1..1
  digitalWrite(DIR_PIN, duty >= 0 ? HIGH : LOW);
  analogWrite(PWM_PIN, (int)(fabsf(duty) * 255));
}
static float encoder_speed() { return s_measured; }   // read your encoder here
static float motor_current() { return 0.0f; }         // read your current sense here

// REQUIRED: make the motor safe now. Boot, and every way out of ARMED (timeout, E-stop, ...).
static void hook_outputs_off(anv_node_t*, void*) {
  s_target = 0;
  motor_drive(0);
  motor_enable(false);
}

// A setpoint from the leader: only while armed, all wheels together at the SYNC.
static void hook_command(anv_node_t* node, const anv_msg_desc_t* msg, const uint8_t* data, uint8_t len, void*) {
  if (msg->port != ANV_WHEEL_SETPOINT_PORT) return;
  anv_wheel_setpoint_t sp;
  if (!anv_wheel_setpoint_unpack(&sp, data, len)) return;
  anv_value_t max_speed, invert;
  anv_node_get(node, ANV_WHEEL_PARAM_MAX_SPEED, &max_speed);
  anv_node_get(node, ANV_WHEEL_PARAM_INVERT, &invert);
  float v = sp.mode == ANV_WHEEL_MODE_VELOCITY ? anv_wheel_setpoint_speed_to_phys(sp.speed) : 0.0f;
  if (invert.u) v = -v;
  s_target = constrain(v, -max_speed.f, max_speed.f);
}

static bool hook_fill_stream(anv_node_t*, const anv_msg_desc_t* msg, uint8_t out[8], void*) {
  if (msg->port != ANV_WHEEL_FEEDBACK_PORT) return false;
  anv_wheel_feedback_t fb = {};
  fb.speed = anv_wheel_feedback_speed_from_phys(encoder_speed());
  fb.current = anv_wheel_feedback_current_from_phys(motor_current());
  anv_wheel_feedback_pack(out, &fb);
  return true;
}

static uint8_t hook_action(anv_node_t*, const anv_action_desc_t* a, anv_value_t, void*) {
  if (a->index == ANV_WHEEL_ACTION_ZERO_COUNTS) { /* encoder_zero(); */ return ANV_SERVICE_STATUS_OK; }
  return ANV_SERVICE_STATUS_BAD_INDEX;
}

// The control loop, 1 kHz. Outputs only while armed.
static void hook_loop(anv_node_t* node, uint32_t now_ms, void*) {
  static uint32_t last_ms = 0;
  if (now_ms == last_ms) return;
  last_ms = now_ms;
  if (!anv_node_armed(node)) return;

  anv_value_t kp;
  anv_node_get(node, ANV_WHEEL_PARAM_KP, &kp);
  motor_enable(true);
  motor_drive(constrain(kp.f * (s_target - encoder_speed()) / 100.0f, -1.0f, 1.0f));

  const float amps = motor_current();
  if (amps > 20.0f) {                                   // trip: the runtime disarms -> outputs_off
    anv_node_event(node, ANV_WHEEL_EVENT_OVERCURRENT, (uint16_t)(amps * 10));
    anv_node_set_fault(node, true);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ENABLE_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);

  anv_node_config_t cfg;
  anv_node_config_default(&cfg);
  cfg.type = &anv_wheel_type;
  cfg.outputs_off = hook_outputs_off;
  cfg.on_command = hook_command;
  cfg.fill_stream = hook_fill_stream;
  cfg.on_action = hook_action;
  cfg.loop = hook_loop;

  if (!anv_board_begin(&s_board, &cfg, NULL)) {
    Serial.printf("wheel node: %s\n", anv_board_error(&s_board));
  }
}

void loop() {
  anv_board_poll(&s_board);
  delay(1);
}
