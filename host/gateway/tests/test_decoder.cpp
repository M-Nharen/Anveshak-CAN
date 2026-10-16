// Encoder -> decoder round trips and the generated-code mapping for every message family.
#include "rover.h"
#include "test_util.hpp"

using namespace anv::gw;

namespace {

Decoded must_decode(const CanFrame& f) {
  Decoded d;
  const bool ok = decode_frame(f, d);
  CHECK(ok);
  return d;
}

// Returns a copy of the decoded message (default-constructed if the type is wrong, which also
// fails the check), so callers can pass a temporary.
template <class M>
M as(const Decoded& d) {
  const M* m = std::get_if<M>(&d.msg);
  CHECK(m != nullptr);
  return m ? *m : M{};
}

}  // namespace

TEST(host_hb_round_trip_for_each_host) {
  for (uint8_t host : {1, 2, 3}) {
    HostHbMsg hb;
    hb.host_node = host; hb.role = 2; hb.tie_break = host; hb.term = 0x1234; hb.believed_master = host;
    hb.flags = kHbFlagArmed | kHbFlagEstopLatched; hb.uptime_s = 777;
    const CanFrame f = make_host_hb(hb);
    CHECK_EQ(f.id, host == 1 ? 0x008 : host == 2 ? 0x010 : 0x018);
    CHECK_EQ(f.dlc, 8);
    const Decoded d = must_decode(f);
    const HostHbMsg m = as<HostHbMsg>(d);
    CHECK_EQ(m.host_node, host);
    CHECK_EQ(m.role, 2);
    CHECK_EQ(m.term, 0x1234);
    CHECK_EQ(m.flags, kHbFlagArmed | kHbFlagEstopLatched);
    CHECK_EQ(m.uptime_s, 777);
  }
}

TEST(lead_request_round_trip) {
  for (uint8_t sender : {1, 2, 3}) {
    const CanFrame f = make_lead_request({sender, 2, 9});
    CHECK_EQ(f.id, sender == 1 ? 0x009 : sender == 2 ? 0x011 : 0x019);
    CHECK_EQ(f.dlc, 2);
    const Decoded d = must_decode(f);
    const LeadRequestMsg m = as<LeadRequestMsg>(d);
    CHECK_EQ(m.sender_node, sender);
    CHECK_EQ(m.requested_leader, 2);
    CHECK_EQ(m.request_counter, 9);
  }
}

TEST(network_frames_round_trip) {
  const CanFrame estop = make_estop();
  CHECK_EQ(estop.id, 0x000);
  CHECK_EQ(estop.dlc, 1);
  CHECK_EQ(estop.data[0], 0xE5);  // identical payload from every sender, so simultaneous E-stops merge
  CHECK_EQ(as<EstopMsg>(must_decode(estop)).magic, 0xE5);

  const CanFrame rel = make_estop_release(2, 7);
  CHECK_EQ(rel.id, 0x001);
  CHECK_EQ(as<EstopReleaseMsg>(must_decode(rel)).master_term, 7);

  const CanFrame sync = make_sync(255);
  CHECK_EQ(sync.id, 0x002);
  CHECK_EQ(as<SyncMsg>(must_decode(sync)).cycle_counter, 255);

  const CanFrame nmt = make_nmt(0, 1, 1, 3);
  CHECK_EQ(nmt.id, 0x003);
  CHECK_EQ(nmt.dlc, 4);
  const NmtMsg n = as<NmtMsg>(must_decode(nmt));
  {
    CHECK_EQ(n.target_node, 0);
    CHECK_EQ(n.command, 1);
    CHECK_EQ(n.master_node_id, 1);
    CHECK_EQ(n.master_term, 3);
  }
}

TEST(drive_cmd_round_trip_for_all_six_drives) {
  for (uint8_t drive = 1; drive <= 6; ++drive) {
    DriveCmdMsg c;
    c.drive = drive; c.seq = 5; c.sender_host = 2; c.term_low = 3; c.mode = 2;
    c.target_a = -1234; c.target_b = 4321; c.accel_limit = 999;
    const CanFrame f = make_drive_cmd(c);
    CHECK_EQ(f.id, 0x120 + 8 * (drive - 1));  // ID = class 1, node 3 + n, sub 0
    const Decoded d = must_decode(f);
    const DriveCmdMsg m = as<DriveCmdMsg>(d);
    CHECK_EQ(m.drive, drive);
    CHECK_EQ(m.seq, 5);
    CHECK_EQ(m.sender_host, 2);
    CHECK_EQ(m.term_low, 3);
    CHECK_EQ(m.target_a, -1234);
    CHECK_EQ(m.target_b, 4321);
    CHECK_EQ(m.accel_limit, 999);
  }
  CHECK_EQ(make_drive_cmd({7, 0, 0, 0, 0, 0, 0, 0}).id, kInvalidFrameId);
}

TEST(node_hb_round_trip_and_node_ids) {
  for (uint8_t node : {4, 5, 6, 7, 8, 9, 20}) {
    NodeHbMsg hb;
    hb.node_id = node; hb.state = 2; hb.node_type = 1; hb.protocol_version = 1; hb.debug_level = 2;
    hb.fw_build_id = 0xBEEF; hb.uptime_s = 4242;
    const Decoded d = must_decode(make_node_hb(hb));
    const NodeHbMsg m = as<NodeHbMsg>(d);
    CHECK_EQ(m.node_id, node);
    CHECK_EQ(m.fw_build_id, 0xBEEF);
    CHECK_EQ(m.uptime_s, 4242);
    CHECK_EQ(m.debug_level, 2);
  }
}

TEST(feedback_and_telemetry_use_generated_layouts) {
  CanFrame f;
  f.dlc = 8;

  struct rover_drive3_fb_a_t a;
  rover_drive3_fb_a_init(&a);
  a.state = 2; a.fault_flags = 0x41; a.speed_a = -250; a.speed_b = 300; a.current_a = 10; a.current_b = 20;
  f.id = ROVER_DRIVE3_FB_A_FRAME_ID;
  CHECK(rover_drive3_fb_a_pack(f.data.data(), &a, 8) == 8);
  const Decoded da = must_decode(f);
  const DriveFbAMsg ma = as<DriveFbAMsg>(da);
  {
    CHECK_EQ(ma.drive, 3);
    CHECK_EQ(ma.speed_a, -250);
    CHECK_EQ(ma.fault_flags, 0x41);
    CHECK(speed_rad_s(ma.speed_b) > 2.99 && speed_rad_s(ma.speed_b) < 3.01);
    CHECK(current_a(ma.current_b) > 3.99 && current_a(ma.current_b) < 4.01);
  }

  struct rover_drive6_fb_b_t b;
  rover_drive6_fb_b_init(&b);
  b.encoder_a = -100000; b.encoder_b = 2000000000;
  f.id = ROVER_DRIVE6_FB_B_FRAME_ID;
  CHECK(rover_drive6_fb_b_pack(f.data.data(), &b, 8) == 8);
  const DriveFbBMsg mb = as<DriveFbBMsg>(must_decode(f));
  {
    CHECK_EQ(mb.drive, 6);
    CHECK_EQ(mb.encoder_a, -100000);
    CHECK_EQ(mb.encoder_b, 2000000000);
  }

  struct rover_drive2_telem_0_t t0;
  rover_drive2_telem_0_init(&t0);
  t0.supply_voltage = 24100; t0.temp_driver_a = -5; t0.temp_mcu = 41; t0.tec = 17; t0.rec = 3; t0.bus_off_recovery_count = 2;
  f.id = ROVER_DRIVE2_TELEM_0_FRAME_ID;
  CHECK(rover_drive2_telem_0_pack(f.data.data(), &t0, 8) == 8);
  const DriveTelem0Msg m0 = as<DriveTelem0Msg>(must_decode(f));
  {
    CHECK_EQ(m0.drive, 2);
    CHECK_EQ(m0.supply_voltage_mv, 24100);
    CHECK_EQ(m0.temp_driver_a, -5);
    CHECK_EQ(m0.tec, 17);
  }

  struct rover_drive4_telem_1_t t1;
  rover_drive4_telem_1_init(&t1);
  t1.duty_a = -1500; t1.watchdog_reset_count = 4; t1.last_reset_cause = 2; t1.commands_missed = 600;
  f.id = ROVER_DRIVE4_TELEM_1_FRAME_ID;
  CHECK(rover_drive4_telem_1_pack(f.data.data(), &t1, 8) == 8);
  const DriveTelem1Msg m1 = as<DriveTelem1Msg>(must_decode(f));
  {
    CHECK_EQ(m1.drive, 4);
    CHECK_EQ(m1.duty_a, -1500);
    CHECK_EQ(m1.commands_missed, 600);
  }
}

TEST(debug_event_service_boot_and_fpga_decode) {
  CanFrame f;
  f.dlc = 8;

  struct rover_drive5_debug_b_t dbg;
  rover_drive5_debug_b_init(&dbg);
  dbg.setpoint = 10; dbg.measured = -20; dbg.integrator = 30; dbg.output = -40;
  f.id = ROVER_DRIVE5_DEBUG_B_FRAME_ID;
  CHECK(rover_drive5_debug_b_pack(f.data.data(), &dbg, 8) == 8);
  const DriveDebugMsg md = as<DriveDebugMsg>(must_decode(f));
  {
    CHECK_EQ(md.drive, 5);
    CHECK(md.channel_b);
    CHECK_EQ(md.output, -40);
  }

  struct rover_drive1_node_event_t ev;
  rover_drive1_node_event_init(&ev);
  ev.severity = 2; ev.error_code = 0x0301; ev.event_seq = 9; ev.detail = 123; ev.uptime_0_1_s = 55;
  f.id = ROVER_DRIVE1_NODE_EVENT_FRAME_ID;
  CHECK(rover_drive1_node_event_pack(f.data.data(), &ev, 8) == 8);
  const NodeEventMsg me = as<NodeEventMsg>(must_decode(f));
  {
    CHECK_EQ(me.node_id, 4);
    CHECK_EQ(me.error_code, 0x0301);
    CHECK_EQ(me.event_seq, 9);
  }

  struct rover_drive2_service_resp_rpi_t sr;
  rover_drive2_service_resp_rpi_init(&sr);
  sr.opcode = 1; sr.status = 3; sr.transaction_id = 77; sr.param_index = 12; sr.value_raw = 0xCAFEF00D;
  f.id = ROVER_DRIVE2_SERVICE_RESP_RPI_FRAME_ID;
  CHECK(rover_drive2_service_resp_rpi_pack(f.data.data(), &sr, 8) == 8);
  const ServiceMsg ms = as<ServiceMsg>(must_decode(f));
  {
    CHECK_EQ(ms.node_id, 5);
    CHECK_EQ(ms.host, 2);
    CHECK(ms.is_response);
    CHECK_EQ(ms.status, 3);
    CHECK_EQ(ms.value_raw, 0xCAFEF00Du);
  }

  struct rover_drive6_service_req_tool_t rq;
  rover_drive6_service_req_tool_init(&rq);
  rq.opcode = 2; rq.transaction_id = 5; rq.param_index = 3; rq.value_raw = 42;
  f.id = ROVER_DRIVE6_SERVICE_REQ_TOOL_FRAME_ID;
  CHECK(rover_drive6_service_req_tool_pack(f.data.data(), &rq, 8) == 8);
  const ServiceMsg mq = as<ServiceMsg>(must_decode(f));
  {
    CHECK_EQ(mq.node_id, 9);
    CHECK_EQ(mq.host, 3);
    CHECK(!mq.is_response);
    CHECK_EQ(mq.value_raw, 42u);
  }

  f.id = ROVER_DRIVE3_BOOT_NODE_TO_HOST_FRAME_ID;
  for (int i = 0; i < 8; ++i) f.data[i] = static_cast<uint8_t>(0x10 + i);
  const BootMsg mbt = as<BootMsg>(must_decode(f));
  {
    CHECK_EQ(mbt.node_id, 6);
    CHECK(mbt.node_to_host);
    CHECK_EQ(mbt.bytes[7], 0x17);
  }

  struct rover_fpga_sensors_b_t sb;
  rover_fpga_sensors_b_init(&sb);
  sb.sensor_5 = 1; sb.sensor_6 = 2; sb.sensor_7 = 3; sb.sensor_8 = 65535;
  f.id = ROVER_FPGA_SENSORS_B_FRAME_ID;
  CHECK(rover_fpga_sensors_b_pack(f.data.data(), &sb, 8) == 8);
  const FpgaSensorsMsg mf = as<FpgaSensorsMsg>(must_decode(f));
  {
    CHECK(mf.second_half);
    CHECK_EQ(mf.values[3], 65535);
  }
}

TEST(every_dbc_message_decodes_to_a_named_type) {
  // Walks every ID the layout rule allows and checks that exactly the IDs the DBC defines decode.
  // The DBC has: 5 network singles, 3 host HB, 3 lead requests, 6 drives x 17, FPGA x 4, and the
  // 4 ID-allocation messages. On top, auxiliary nodes 16-30 except the FPGA (14 nodes) decode with
  // the shared layouts: heartbeat + 2 sensor frames each.
  size_t decoded = 0;
  for (uint16_t id = 0; id < 0x800; ++id) {
    CanFrame f;
    f.id = id;
    f.dlc = 8;
    Decoded d;
    if (decode_frame(f, d)) {
      ++decoded;
      CHECK(message_name(d.msg) != "UNKNOWN");
    }
  }
  CHECK_EQ(decoded, 5u + 3u + 3u + 6u * 17u + 4u + 4u + 14u * 3u);
}

TEST(auxiliary_nodes_decode_with_the_shared_heartbeat_and_sensor_layouts) {
  NodeHbMsg hb;
  hb.node_id = 17;
  hb.state = 1;
  hb.node_type = 3;
  hb.protocol_version = 1;
  hb.fw_build_id = 0xBEEF;
  hb.uptime_s = 42;
  const CanFrame f = make_node_hb(hb);
  CHECK_EQ(f.id, 0x688);
  const NodeHbMsg m = as<NodeHbMsg>(must_decode(f));
  CHECK_EQ(m.node_id, 17);
  CHECK_EQ(m.node_type, 3);
  CHECK_EQ(m.fw_build_id, 0xBEEF);
  CHECK_EQ(m.uptime_s, 42);

  const CanFrame sf = make_aux_sensors(18, true, {1, 2, 3, 4});
  CHECK_EQ(sf.id, 0x491);
  const FpgaSensorsMsg sm = as<FpgaSensorsMsg>(must_decode(sf));
  CHECK(sm.second_half);
  CHECK_EQ(sm.values[3], 4);

  // Unconfigured boards (31) send no heartbeat, and host slots never decode through the generic path.
  NodeHbMsg bad = hb;
  bad.node_id = kNodeUnconfigured;
  CHECK_EQ(make_node_hb(bad).id, kInvalidFrameId);
  CanFrame host_slot = f;
  host_slot.id = 0x610;  // node 2 (RPi) heartbeat slot: not a node
  Decoded d;
  CHECK(!decode_frame(host_slot, d));
}

TEST(id_request_and_assign_round_trip_with_a_48_bit_mac) {
  const uint64_t mac = 0xA4CF12345678ull;
  const CanFrame rq = make_node_id_request({mac, 3, kHwTargetEsp32S3});
  CHECK_EQ(rq.id, 0x5FB);
  const NodeIdRequestMsg r = as<NodeIdRequestMsg>(must_decode(rq));
  CHECK(r.mac == mac);
  CHECK_EQ(r.hw_target, kHwTargetEsp32S3);
  CHECK_EQ(rq.data[0], 0x78);  // least-significant byte first

  NodeIdAssignMsg a;
  a.host = 2;
  a.mac = mac;
  a.assigned_id = 16;
  const CanFrame af = make_node_id_assign(a);
  CHECK_EQ(af.id, 0x5F9);
  const NodeIdAssignMsg am = as<NodeIdAssignMsg>(must_decode(af));
  CHECK_EQ(am.host, 2);
  CHECK(am.mac == mac);
  CHECK_EQ(am.assigned_id, 16);
  a.host = 7;
  CHECK_EQ(make_node_id_assign(a).id, kInvalidFrameId);
}

TEST(short_unknown_and_error_frames_are_rejected) {
  CanFrame f = make_host_hb({1, 1, 1, 0, 0, 0, 0});
  f.dlc = 4;  // HOST_HB is 8 bytes
  Decoded d;
  CHECK(!decode_frame(f, d));

  CanFrame unknown;
  unknown.id = 0x7FF;
  unknown.dlc = 8;
  CHECK(!decode_frame(unknown, d));

  CanFrame err = make_estop();
  err.is_error = true;
  CHECK(!decode_frame(err, d));

  CanFrame empty_estop = make_estop();
  empty_estop.dlc = 0;
  CHECK(!decode_frame(empty_estop, d));
}

TEST(fake_bus_delivers_to_other_ports_only) {
  FakeBusHub hub;
  auto a = hub.connect();
  auto b = hub.connect();
  auto c = hub.connect();
  CHECK(a->send(make_sync(3)));
  CanFrame f;
  CHECK(b->recv(f, 50) == RecvStatus::Ok);
  CHECK_EQ(f.id, 0x002);
  CHECK(f.rx_ns > 0);
  CHECK(c->recv(f, 50) == RecvStatus::Ok);
  CHECK(a->recv(f, 20) == RecvStatus::Timeout);  // no echo to the sender, like SocketCAN's default
  b->close();
  CHECK(b->recv(f, 20) == RecvStatus::Closed);
  CHECK(!b->send(make_sync(4)));
  CHECK_EQ(hub.frames_sent(), 1u);
}

int main() { return t::run_all(); }
