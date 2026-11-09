#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rclcpp/rclcpp.hpp"
#include "linsight_ros2/msg/bus_stats.hpp"
#include "linsight_ros2/msg/can_frame.hpp"

class LinsightBridgeNode : public rclcpp::Node {
public:
  LinsightBridgeNode() : Node("linsight_bridge_node") {
    iface_ = declare_parameter<std::string>("interface", "vcan0");
    open_can();
    frames_ = create_publisher<linsight_ros2::msg::CanFrame>("/can_rx", rclcpp::QoS(100));
    stats_ = create_publisher<linsight_ros2::msg::BusStats>("/linsight/stats", 10);
    timer_ = create_wall_timer(std::chrono::milliseconds(2),
                               std::bind(&LinsightBridgeNode::drain, this));
    stats_timer_ = create_wall_timer(std::chrono::seconds(1),
                                     std::bind(&LinsightBridgeNode::publish_stats, this));
    RCLCPP_INFO(get_logger(), "reading SocketCAN interface %s", iface_.c_str());
  }

  ~LinsightBridgeNode() override {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

private:
  void open_can() {
    fd_ = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd_ < 0) {
      throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }

    struct ifreq ifr {};
    std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface_.c_str());
    if (ioctl(fd_, SIOCGIFINDEX, &ifr) < 0) {
      const std::string err = std::strerror(errno);
      close(fd_);
      fd_ = -1;
      throw std::runtime_error("ioctl " + iface_ + ": " + err);
    }

    struct sockaddr_can addr {};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
      const std::string err = std::strerror(errno);
      close(fd_);
      fd_ = -1;
      throw std::runtime_error("bind " + iface_ + ": " + err);
    }

    const int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
  }

  void drain() {
    for (int i = 0; i < 64; ++i) {
      struct can_frame cf {};
      const ssize_t n = read(fd_, &cf, sizeof(cf));
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        ++counters_.errors;
        return;
      }
      if (static_cast<size_t>(n) != sizeof(cf)) {
        ++counters_.errors;
        continue;
      }

      ++counters_.received;
      linsight_ros2::msg::CanFrame msg;
      msg.is_extended = (cf.can_id & CAN_EFF_FLAG) != 0;
      msg.is_rtr = (cf.can_id & CAN_RTR_FLAG) != 0;
      msg.is_error = (cf.can_id & CAN_ERR_FLAG) != 0;
      msg.id = cf.can_id & (msg.is_extended ? CAN_EFF_MASK : CAN_SFF_MASK);
      msg.dlc = cf.can_dlc > 8 ? 8 : cf.can_dlc;
      std::memcpy(msg.data.data(), cf.data, 8);
      frames_->publish(msg);
      ++counters_.published;
    }
  }

  void publish_stats() {
    linsight_ros2::msg::BusStats msg;
    msg.header.stamp = now();
    msg.header.frame_id = iface_;
    msg.received = counters_.received;
    msg.published = counters_.published;
    msg.errors = counters_.errors;
    msg.dropped = counters_.dropped;
    stats_->publish(msg);
  }

  struct Counters {
    uint64_t received = 0;
    uint64_t published = 0;
    uint64_t errors = 0;
    uint64_t dropped = 0;
  };

  std::string iface_;
  int fd_ = -1;
  Counters counters_;
  rclcpp::Publisher<linsight_ros2::msg::CanFrame>::SharedPtr frames_;
  rclcpp::Publisher<linsight_ros2::msg::BusStats>::SharedPtr stats_;
  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp::TimerBase::SharedPtr stats_timer_;
};

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  try {
    rclcpp::spin(std::make_shared<LinsightBridgeNode>());
  } catch (const std::exception &e) {
    RCLCPP_FATAL(rclcpp::get_logger("linsight_bridge_node"), "%s", e.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::shutdown();
  return 0;
}
