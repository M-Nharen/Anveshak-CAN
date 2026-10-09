"""Start the CAN gateway node with the parameters of one host.

  ros2 launch anveshak_can_ros can_gateway.launch.py host:=orin
  ros2 launch anveshak_can_ros can_gateway.launch.py host:=rpi bus:=fake
"""
import os

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def _make_node(context):
    host = LaunchConfiguration("host").perform(context)
    params = LaunchConfiguration("params_file").perform(context)
    if not params:
        params = os.path.join(get_package_share_directory("anveshak_can_ros"), "config", f"{host}.yaml")
    overrides = {}
    bus = LaunchConfiguration("bus").perform(context)
    if bus:
        overrides["bus"] = bus
    return [
        Node(
            package="anveshak_can_ros",
            executable="can_gateway_node",
            name="can_gateway",
            output="screen",
            parameters=[params, overrides],
        )
    ]


def generate_launch_description():
    return LaunchDescription(
        [
            DeclareLaunchArgument("host", default_value="orin", description="orin or rpi"),
            DeclareLaunchArgument("params_file", default_value="", description="override the per-host YAML"),
            DeclareLaunchArgument("bus", default_value="", description="socketcan or fake; empty = from the YAML"),
            OpaqueFunction(function=_make_node),
        ]
    )
