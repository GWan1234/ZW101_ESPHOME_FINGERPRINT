#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/switch/switch.h"

namespace esphome {
namespace zw101 {

// 前向声明
class EnrollSwitch;
class ClearSwitch;

class ZW101Component : public Component, public uart::UARTDevice {
 public:
  // 定义指令包格式
  static const uint8_t HEADER_HIGH = 0xEF;
  static const uint8_t HEADER_LOW = 0x01;
  static const uint32_t DEVICE_ADDRESS = 0xFFFFFFFF;

  // 定义指令码
  static const uint8_t CMD_GET_IMAGE = 0x01;     // 获取图像(匹配模式)
  static const uint8_t CMD_GET_IMAGE_ENROLL = 0x29; // 获取图像(注册模式)
  static const uint8_t CMD_GEN_CHAR = 0x02;      // 生成特征
  static const uint8_t CMD_MATCH = 0x03;         // 精确比对指纹
  static const uint8_t CMD_SEARCH = 0x04;        // 搜索指纹
  static const uint8_t CMD_REG_MODEL = 0x05;     // 合并特征
  static const uint8_t CMD_STORE_CHAR = 0x06;    // 存储模板
  static const uint8_t CMD_DEL_CHAR = 0x0C;      // 删除模板
  static const uint8_t CMD_CLEAR_LIB = 0x0D;     // 清空指纹库
  static const uint8_t CMD_WRITE_SYSPARA = 0x0E; // 写系统参数
  static const uint8_t CMD_READ_SYSPARA = 0x0F;  // 读模组基本参数
  static const uint8_t CMD_READ_VALID_NUMS = 0x1D; // 读有效模板个数
  static const uint8_t CMD_READ_INDEX_TABLE = 0x1F; // 读索引表
  static const uint8_t CMD_AUTO_ENROLL = 0x31;   // 自动注册
  static const uint8_t CMD_AUTO_MATCH = 0x32;    // 自动匹配
  static const uint8_t CMD_INTO_SLEEP = 0x33;    // 进入休眠
  static const uint8_t CMD_HANDSHAKE = 0x35;     // 握手
  static const uint8_t CMD_RGB_CTRL = 0x3C;      // RGB灯控制

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Setters for sensors and switches
  void set_fingerprint_sensor(binary_sensor::BinarySensor *sensor) { fingerprint_sensor_ = sensor; }
  void set_match_score_sensor(sensor::Sensor *sensor) { match_score_sensor_ = sensor; }
  void set_match_id_sensor(sensor::Sensor *sensor) { match_id_sensor_ = sensor; }
  void set_status_sensor(text_sensor::TextSensor *sensor) { status_sensor_ = sensor; }
  void set_enroll_switch(EnrollSwitch *sw) { enroll_switch_ = sw; }
  void set_clear_switch(ClearSwitch *sw) { clear_switch_ = sw; }

  // 公共方法
  bool register_fingerprint();
  bool clear_fingerprint_library();
  void read_fp_info();
  bool read_valid_template_count();        // 读有效模板个数
  bool handshake();                        // 握手测试
  bool delete_fingerprint(uint16_t id);    // 删除指定指纹
  void set_rgb_led(uint8_t mode, uint8_t color, uint8_t brightness = 100); // RGB灯控制
  bool enter_sleep_mode();                 // 进入休眠模式
  bool auto_enroll_mode(uint16_t timeout_sec = 60); // 自动注册模式
  bool auto_match_mode();                  // 自动匹配模式
  void cancel_auto_mode();                 // 取消自动模式

  // 控制自动搜索（简化方案 - 不依赖休眠命令）
  void disable_auto_search() {
    sleep_mode_ = true;
    ESP_LOGI("zw101", "Auto search disabled (LED mode)");
  }
  void enable_auto_search() {
    sleep_mode_ = false;
    ESP_LOGI("zw101", "Auto search enabled");
  }

 protected:
  // Sensors
  binary_sensor::BinarySensor *fingerprint_sensor_{nullptr};
  sensor::Sensor *match_score_sensor_{nullptr};
  sensor::Sensor *match_id_sensor_{nullptr};
  text_sensor::TextSensor *status_sensor_{nullptr};

  // Switches
  EnrollSwitch *enroll_switch_{nullptr};
  ClearSwitch *clear_switch_{nullptr};

  // 注册流程状态
  enum EnrollState {
    ENROLL_IDLE,
    ENROLL_WAIT_FINGER,
    ENROLL_CAPTURING,
    ENROLL_WAIT_REMOVE,
    ENROLL_MERGING,
    ENROLL_STORING
  };
  EnrollState enroll_state_{ENROLL_IDLE};
  uint8_t enroll_sample_count_{0};
  uint32_t enroll_last_action_{0};
  uint16_t next_fingerprint_id_{0};  // 下一个可用ID (从0开始)
  uint16_t library_capacity_{50};

  // 搜索流程状态
  enum SearchState {
    SEARCH_IDLE,
    SEARCH_GET_IMAGE,
    SEARCH_GEN_CHAR,
    SEARCH_WAIT_RETRY,
    SEARCH_DO_SEARCH
  };
  SearchState search_state_{SEARCH_IDLE};
  uint8_t search_retry_count_{0};
  uint32_t search_last_action_{0};

  // 匹配成功状态
  bool match_found_{false};
  uint32_t match_clear_time_{0};

  // 初始化标志
  bool info_read_{false};

  // 休眠和自动模式状态
  bool sleep_mode_{false};
  bool auto_mode_active_{false};
  uint32_t auto_mode_timeout_{0};

  // 内部方法
  void process_enrollment();
  void process_search();  // 新增非阻塞搜索处理
  void send_cmd(uint8_t cmd);
  void send_cmd2(uint8_t cmd, uint8_t param1);
  void send_store_cmd(uint8_t buffer_id, uint16_t template_id);
  void send_search_cmd(uint8_t buffer_id, uint16_t start_page, uint16_t page_num);
  void build_packet_header(uint8_t *packet, uint16_t length);
  bool receive_response();
  uint8_t wait_for_response(uint8_t *buffer, uint8_t max_length, uint32_t timeout_ms);
};

// 注册指纹开关
class EnrollSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(ZW101Component *parent) { parent_ = parent; }

 protected:
  void write_state(bool state) override {
    if (state && parent_) {
      parent_->register_fingerprint();
      publish_state(false);  // 自动关闭
    }
  }

  ZW101Component *parent_{nullptr};
};

// 清空指纹库开关
class ClearSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(ZW101Component *parent) { parent_ = parent; }

 protected:
  void write_state(bool state) override {
    if (state && parent_) {
      parent_->clear_fingerprint_library();
      publish_state(false);  // 自动关闭
    }
  }

  ZW101Component *parent_{nullptr};
};

}  // namespace zw101
}  // namespace esphome
