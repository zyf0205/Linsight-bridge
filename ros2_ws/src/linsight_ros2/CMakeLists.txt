cmake_minimum_required(VERSION 3.8)
project(linsight_ros2)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/CanFrame.msg"
  "msg/BusStats.msg"
  DEPENDENCIES std_msgs)

add_executable(linsight_bridge_node src/linsight_bridge_node.cpp)
ament_target_dependencies(linsight_bridge_node rclcpp std_msgs)
rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} "rosidl_typesupport_cpp")
target_link_libraries(linsight_bridge_node "${cpp_typesupport_target}")

install(TARGETS linsight_bridge_node DESTINATION lib/${PROJECT_NAME})
install(FILES README_ROS2.md DESTINATION share/${PROJECT_NAME})

ament_export_dependencies(rosidl_default_runtime)
ament_package()
