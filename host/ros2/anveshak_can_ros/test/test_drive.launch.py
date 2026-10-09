"""Start the node on the in-process simulated rover, take the lead, arm, send /cmd_vel and see the
wheels turn; then check the other topics and the stub services."""
import time
import unittest

import launch
import launch_ros.actions
import launch_testing.actions
import pytest
import rclpy
from anveshak_can_msgs.msg import BusStatus, DriveFeedback, NodeRegistry
from anveshak_can_msgs.srv import Arm, Disarm, Estop, ParamGet, RequestLead, SetDebug, SetRate, TakeControl
from diagnostic_msgs.msg import DiagnosticArray
from geometry_msgs.msg import Twist


@pytest.mark.launch_test
def generate_test_description():
    node = launch_ros.actions.Node(
        package="anveshak_can_ros",
        executable="can_gateway_node",
        name="can_gateway",
        output="screen",
        parameters=[{"bus": "fake", "host_id": 1, "wheel_radius": 0.2, "track_width": 0.8}],
    )
    return launch.LaunchDescription([node, launch_testing.actions.ReadyToTest()]), {"gateway": node}


class TestDrive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()
        cls.node = rclpy.create_node("test_drive")
        cls.fb = None
        cls.bus = None
        cls.nodes = None
        cls.diag = None
        cls.node.create_subscription(DriveFeedback, "/can/drive/feedback", lambda m: setattr(cls, "fb", m), 10)
        cls.node.create_subscription(BusStatus, "/can/bus", lambda m: setattr(cls, "bus", m), 10)
        cls.node.create_subscription(NodeRegistry, "/can/nodes", lambda m: setattr(cls, "nodes", m), 10)
        cls.node.create_subscription(DiagnosticArray, "/diagnostics", lambda m: setattr(cls, "diag", m), 10)
        cls.cmd_vel = cls.node.create_publisher(Twist, "/cmd_vel", 10)

    @classmethod
    def tearDownClass(cls):
        cls.node.destroy_node()
        rclpy.shutdown()

    def spin_until(self, pred, timeout=10.0, tick=None):
        end = time.time() + timeout
        while time.time() < end:
            if tick:
                tick()
            rclpy.spin_once(self.node, timeout_sec=0.02)
            if pred():
                return True
        return pred()

    def call(self, srv_type, name, request, timeout=10.0):
        client = self.node.create_client(srv_type, name)
        self.assertTrue(client.wait_for_service(timeout_sec=timeout), f"{name} not available")
        future = client.call_async(request)
        rclpy.spin_until_future_complete(self.node, future, timeout_sec=timeout)
        self.assertTrue(future.done(), f"{name} did not answer")
        return future.result()

    def test_drive_sequence(self):
        # All seven simulated nodes (six drives and the FPGA) show up.
        self.assertTrue(
            self.spin_until(
                lambda: self.nodes is not None
                and sum(n.alive for n in self.nodes.nodes) >= 7
                and self.bus is not None
            ),
            "simulated nodes never became alive",
        )

        # Arming before leading is refused.
        res = self.call(Arm, "/can/arm", Arm.Request())
        self.assertFalse(res.success)

        res = self.call(RequestLead, "/can/request_lead", RequestLead.Request(host_id=1))
        self.assertTrue(res.success, res.message)
        self.assertTrue(
            self.spin_until(lambda: self.bus is not None and self.bus.role == BusStatus.ROLE_MASTER),
            "host never became master",
        )
        res = self.call(Arm, "/can/arm", Arm.Request())
        self.assertTrue(res.success, res.message)

        # Drive forward: every wheel reports ARMED and a non-zero speed.
        twist = Twist()
        twist.linear.x = 0.5

        def moving():
            if self.fb is None:
                return False
            return all(w.state == 2 and abs(w.speed_a) > 0.0 for w in self.fb.wheels)

        self.assertTrue(
            self.spin_until(moving, timeout=15.0, tick=lambda: self.cmd_vel.publish(twist)),
            "wheels never reported non-zero speed",
        )
        # 0.5 m/s / 0.2 m radius = 2.5 rad/s on every wheel when driving straight.
        for w in self.fb.wheels:
            self.assertAlmostEqual(w.speed_a, 2.5, delta=0.05)

        # Commands stop: the wheels stop on their own.
        self.assertTrue(
            self.spin_until(lambda: all(abs(w.speed_a) == 0.0 for w in self.fb.wheels), timeout=5.0),
            "wheels did not stop after commands ended",
        )

        self.assertIsNotNone(self.diag)
        self.assertTrue(any(s.name == "can_gateway" for s in self.diag.status))

        # Parameter reads against the simulated rover (no SERVICE responder) time out, not crash.
        res = self.call(ParamGet, "/can/param_get", ParamGet.Request(node_id=4, index=0))
        self.assertFalse(res.success)

        # /can/set_rate: host rates are gateway settings, applied directly.
        res = self.call(SetRate, "/can/set_rate", SetRate.Request(target=SetRate.Request.TARGET_HOST_HB, hz=25.0))
        self.assertTrue(res.success, res.message)
        res = self.call(SetRate, "/can/set_rate", SetRate.Request(target=SetRate.Request.TARGET_CYCLE, hz=10.0))
        self.assertFalse(res.success)  # below the 25 Hz floor

        # /can/take_control: one holder at a time, released by the same holder id.
        res = self.call(TakeControl, "/can/take_control", TakeControl.Request(holder="teleop", release=False))
        self.assertTrue(res.success and res.holding, res.message)
        res = self.call(TakeControl, "/can/take_control", TakeControl.Request(holder="dashboard", release=False))
        self.assertFalse(res.success)  # teleop already holds it
        res = self.call(TakeControl, "/can/take_control", TakeControl.Request(holder="teleop", release=True))
        self.assertTrue(res.success, res.message)

        # Stub: debug levels are separate work (B4).
        res = self.call(SetDebug, "/can/set_debug", SetDebug.Request(node_id=0, level=1, category_mask=0))
        self.assertFalse(res.success)

        # E-stop and release.
        res = self.call(Estop, "/can/estop", Estop.Request(release=False))
        self.assertTrue(res.success, res.message)
        self.assertTrue(self.spin_until(lambda: self.bus.estop_latched))
        res = self.call(Estop, "/can/estop", Estop.Request(release=True))
        self.assertTrue(res.success, res.message)
        res = self.call(Disarm, "/can/disarm", Disarm.Request())
        self.assertTrue(res.success, res.message)


@launch_testing.post_shutdown_test()
class TestExit(unittest.TestCase):
    def test_exit_code(self, proc_info, gateway):
        launch_testing.asserts.assertExitCodes(proc_info, allowable_exit_codes=[0, -2, -15], process=gateway)
