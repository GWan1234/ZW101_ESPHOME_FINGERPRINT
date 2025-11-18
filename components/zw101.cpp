#include "zw101.h"
#include "esphome/core/log.h"

namespace esphome {
namespace zw101 {

static const char *const TAG = "zw101";

void ZW101Component::setup() {
  ESP_LOGI(TAG, "Initializing ZW101 Fingerprint Module");

  // 初始化搜索状态
  search_last_action_ = millis();

  // 延迟读取模组信息,在 loop 中第一次运行时读取
  read_fp_info();  // 暂时注释,避免启动阻塞
}

void ZW101Component::loop() {
  uint32_t now = millis();

  // 启动后0.5秒立即关闭LED(避免模组默认灯光)
  static bool led_off_sent = false;
  if (!led_off_sent && now > 500) {
    led_off_sent = true;
    set_rgb_led(4, 0, 0);
    ESP_LOGI(TAG, "LED turned off");
  }

  // 检查自动模式超时
  if (auto_mode_active_ && auto_mode_timeout_ > 0 && now >= auto_mode_timeout_) {
    ESP_LOGW(TAG, "Auto mode timeout, cancelling");
    cancel_auto_mode();
  }

  // 处理匹配成功后的清除状态
  if (match_found_ && now >= match_clear_time_) {
    match_found_ = false;
    if (fingerprint_sensor_)
      fingerprint_sensor_->publish_state(false);
  }

  // 处理注册流程
  if (enroll_state_ != ENROLL_IDLE) {
    process_enrollment();
    return;  // 注册过程中不进行自动搜索
  }

  // 如果在自动模式或休眠模式,不进行主动搜索
  if (auto_mode_active_ || sleep_mode_) {
    return;
  }

  // 处理搜索流程
  process_search();
}

// 非阻塞式搜索流程处理
void ZW101Component::process_search() {
  uint32_t now = millis();

  switch (search_state_) {
    case SEARCH_IDLE:
      // 每1秒启动一次新搜索
      if (now - search_last_action_ > 1000) {
        search_state_ = SEARCH_GET_IMAGE;
        search_retry_count_ = 0;
        search_last_action_ = now;
      }
      break;

    case SEARCH_GET_IMAGE:
      // 获取图像
      send_cmd(CMD_GET_IMAGE);
      if (receive_response()) {
        // 成功读取图像,进入生成特征
        search_state_ = SEARCH_GEN_CHAR;
      } else {
        // 没有检测到指纹,进入等待重试
        search_state_ = SEARCH_WAIT_RETRY;
        search_last_action_ = now;
      }
      break;

    case SEARCH_GEN_CHAR:
      // 生成特征
      send_cmd2(CMD_GEN_CHAR, 1);
      if (receive_response()) {
        // 特征生成成功,进行搜索
        search_state_ = SEARCH_DO_SEARCH;
      } else {
        // 特征生成失败
        search_retry_count_++;
        if (search_retry_count_ >= 5) {
          // 达到最大重试次数,返回空闲
          if (status_sensor_)
            status_sensor_->publish_state("No Valid Fingerprint");
          search_state_ = SEARCH_IDLE;
          search_last_action_ = now;
        } else {
          // 重试
          search_state_ = SEARCH_WAIT_RETRY;
          search_last_action_ = now;
        }
      }
      break;

    case SEARCH_WAIT_RETRY:
      // 等待500ms后重试
      if (now - search_last_action_ > 500) {
        search_state_ = SEARCH_GET_IMAGE;
      }
      break;

    case SEARCH_DO_SEARCH:
      // 搜索指纹库 - 从Page 0开始,搜索整个库
      send_search_cmd(1, 0, library_capacity_);

      uint8_t response[50];
      uint8_t length = wait_for_response(response, 50, 500);

      // 调试: 打印完整响应包
      if (length > 0) {
        ESP_LOGI(TAG, "Search response length: %d", length);
        ESP_LOG_BUFFER_HEX(TAG, response, length);
      }

      if (length >= 12 && response[9] == 0x00) {
        // 搜索命令执行成功,检查是否真的找到匹配
        uint16_t match_page = (response[10] << 8) | response[11];
        uint16_t match_score = (response[12] << 8) | response[13];

        ESP_LOGI(TAG, "Search response - Page: %d (0x%04X), Score: %d", match_page, match_page, match_score);

        // 判断是否真的找到匹配:
        // - 0xFFFF 表示未找到匹配
        // - 有效的页码应该在 0 到 library_capacity_ 范围内
        if (match_page != 0xFFFF && match_page < library_capacity_) {
          // 真正的匹配成功
          ESP_LOGI(TAG, "Match found! Page: %d, Score: %d", match_page, match_score);

          if (fingerprint_sensor_)
            fingerprint_sensor_->publish_state(true);
          if (match_id_sensor_)
            match_id_sensor_->publish_state(match_page);  // Page号就是显示的ID
          if (match_score_sensor_)
            match_score_sensor_->publish_state(match_score);
          if (status_sensor_)
            status_sensor_->publish_state("Match Found");

          // 设置匹配标志,3秒后自动清除
          match_found_ = true;
          match_clear_time_ = now + 3000;
        } else {
          // 未匹配
          ESP_LOGD(TAG, "No match found (Page=0x%04X)", match_page);
          if (status_sensor_)
            status_sensor_->publish_state("No Match");
        }
      } else if (length >= 12 && response[9] == 0x09) {
        // 0x09 = PS_NOT_SEARCHED: 没有搜索到匹配
        ESP_LOGD(TAG, "Search returned: No match (0x09)");
        if (status_sensor_)
          status_sensor_->publish_state("No Match");
      }

      // 搜索完成,返回空闲状态
      search_state_ = SEARCH_IDLE;
      search_last_action_ = now;
      break;
  }

  yield();  // 让出CPU时间
}

// 非阻塞式注册流程处理
void ZW101Component::process_enrollment() {
  uint32_t now = millis();

  switch (enroll_state_) {
    case ENROLL_WAIT_FINGER:
      // 等待手指放置
      if (now - enroll_last_action_ > 200) {  // 每200ms检查一次
        enroll_last_action_ = now;
        send_cmd(CMD_GET_IMAGE_ENROLL);  // 使用注册模式采图命令 0x29

        if (receive_response()) {
          // 检测到手指,开始生成特征
          enroll_state_ = ENROLL_CAPTURING;
          ESP_LOGI(TAG, "Finger detected, capturing sample %d/5", enroll_sample_count_ + 1);
        } else if (now - enroll_last_action_ > 30000) {
          // 超时30秒
          if (status_sensor_)
            status_sensor_->publish_state("Enroll Timeout");
          enroll_state_ = ENROLL_IDLE;
        }
      }
      break;

    case ENROLL_CAPTURING:
      // 生成特征
      send_cmd2(CMD_GEN_CHAR, enroll_sample_count_ + 1);
      if (receive_response()) {
        enroll_sample_count_++;
        ESP_LOGI(TAG, "Sample %d captured", enroll_sample_count_);

        if (enroll_sample_count_ >= 5) {
          // 收集完成,开始合并
          enroll_state_ = ENROLL_MERGING;
        } else {
          // 等待手指移开
          enroll_state_ = ENROLL_WAIT_REMOVE;
          enroll_last_action_ = now;
        }
      } else {
        // 失败,返回等待
        enroll_state_ = ENROLL_WAIT_FINGER;
      }
      break;

    case ENROLL_WAIT_REMOVE:
      // 等待手指移开
      if (now - enroll_last_action_ > 1000) {
        ESP_LOGI(TAG, "Remove finger and place again (%d/5)", enroll_sample_count_);
        enroll_state_ = ENROLL_WAIT_FINGER;
        enroll_last_action_ = now;
      }
      break;

    case ENROLL_MERGING:
      // 合并特征
      send_cmd(CMD_REG_MODEL);
      if (receive_response()) {
        enroll_state_ = ENROLL_STORING;
      } else {
        if (status_sensor_)
          status_sensor_->publish_state("Enroll Failed - Merge");
        enroll_state_ = ENROLL_IDLE;
      }
      break;

    case ENROLL_STORING:
      // 存储模板
      send_store_cmd(1, next_fingerprint_id_);  // 使用下一个可用ID
      if (receive_response()) {
        ESP_LOGI(TAG, "Fingerprint enrolled successfully as ID %d", next_fingerprint_id_);

        if (status_sensor_) {
          char buf[64];
          snprintf(buf, sizeof(buf), "Enroll Success (ID: %d)", next_fingerprint_id_);
          status_sensor_->publish_state(buf);
        }

        // 存储成功后递增ID
        next_fingerprint_id_++;
        if (next_fingerprint_id_ >= library_capacity_) {
          next_fingerprint_id_ = 0;  // 超过容量则从0开始循环
        }
        ESP_LOGI(TAG, "Next fingerprint will use ID: %d", next_fingerprint_id_);
      } else {
        if (status_sensor_)
          status_sensor_->publish_state("Enroll Failed - Store");
      }
      enroll_state_ = ENROLL_IDLE;
      break;

    default:
      enroll_state_ = ENROLL_IDLE;
      break;
  }

  yield();  // 让出CPU时间,避免看门狗超时
}

// 注册指纹 - 启动非阻塞流程
bool ZW101Component::register_fingerprint() {
  if (enroll_state_ != ENROLL_IDLE) {
    ESP_LOGW(TAG, "Enrollment already in progress");
    return false;
  }

  if (status_sensor_)
    status_sensor_->publish_state("Enrolling...");
  ESP_LOGI(TAG, "Starting fingerprint enrollment");

  enroll_state_ = ENROLL_WAIT_FINGER;
  enroll_sample_count_ = 0;
  enroll_last_action_ = millis();

  ESP_LOGI(TAG, "Place finger (sample 1/5)");
  return true;
}

// 清空指纹库
bool ZW101Component::clear_fingerprint_library() {
  ESP_LOGI(TAG, "Clearing fingerprint library");
  if (status_sensor_)
    status_sensor_->publish_state("Clearing Library...");

  send_cmd(CMD_CLEAR_LIB);
  if (receive_response()) {
    if (status_sensor_)
      status_sensor_->publish_state("Library Cleared");
    ESP_LOGI(TAG, "Library cleared successfully");
    next_fingerprint_id_ = 0;  // 重置ID从0开始
    return true;
  }

  if (status_sensor_)
    status_sensor_->publish_state("Clear Failed");
  return false;
}

// 读取模组信息
void ZW101Component::read_fp_info() {
  uint8_t response[32];

  // 首先读取系统参数获取指纹库容量
  send_cmd(CMD_READ_SYSPARA);
  uint8_t length = wait_for_response(response, 32, 2000);

  if (length >= 28 && response[9] == 0x00) {
    uint16_t fp_lib_size = (response[14] << 8) | response[15];
    library_capacity_ = fp_lib_size;
    ESP_LOGI(TAG, "Library capacity: %d", fp_lib_size);
  }

  // 然后读取实际已注册数量 (更准确)
  uint8_t packet[12];
  uint16_t pkt_length = 3;
  uint16_t checksum = 1 + pkt_length + CMD_READ_VALID_NUMS;

  build_packet_header(packet, pkt_length);
  packet[9] = CMD_READ_VALID_NUMS;
  packet[10] = (checksum >> 8) & 0xFF;
  packet[11] = checksum & 0xFF;

  write_array(packet, 12);
  flush();

  uint8_t resp_len = wait_for_response(response, 32, 1000);

  if (resp_len >= 14 && response[9] == 0x00) {
    uint16_t register_cnt = (response[10] << 8) | response[11];
    next_fingerprint_id_ = register_cnt;  // 下一个ID = 已注册数量 (因为ID从0开始)

    ESP_LOGI(TAG, "Module Info - Registered: %d, Library Size: %d", register_cnt, library_capacity_);
    ESP_LOGI(TAG, "Next available ID: %d", next_fingerprint_id_);

    if (status_sensor_) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Ready (Enrolled: %d/%d)", register_cnt, library_capacity_);
      status_sensor_->publish_state(buf);
    }
  } else {
    ESP_LOGW(TAG, "Failed to read template count, using ID=0");
    next_fingerprint_id_ = 0;
  }
}

// 读取有效模板个数
bool ZW101Component::read_valid_template_count() {
  uint8_t packet[12];
  uint16_t length = 3;
  uint16_t checksum = 1 + length + CMD_READ_VALID_NUMS;

  build_packet_header(packet, length);
  packet[9] = CMD_READ_VALID_NUMS;
  packet[10] = (checksum >> 8) & 0xFF;
  packet[11] = checksum & 0xFF;

  write_array(packet, 12);
  flush();

  uint8_t response[32];
  uint8_t resp_len = wait_for_response(response, 32, 1000);

  if (resp_len >= 14 && response[9] == 0x00) {
    uint16_t template_count = (response[10] << 8) | response[11];
    ESP_LOGI(TAG, "Valid template count: %d", template_count);

    if (status_sensor_) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Templates: %d", template_count);
      status_sensor_->publish_state(buf);
    }

    return true;
  }

  ESP_LOGW(TAG, "Failed to read valid template count");
  return false;
}

// 握手测试
bool ZW101Component::handshake() {
  uint8_t packet[12];
  uint16_t length = 3;
  uint16_t checksum = 1 + length + CMD_HANDSHAKE;

  build_packet_header(packet, length);
  packet[9] = CMD_HANDSHAKE;
  packet[10] = (checksum >> 8) & 0xFF;
  packet[11] = checksum & 0xFF;

  write_array(packet, 12);
  flush();

  uint8_t response[32];
  uint8_t resp_len = wait_for_response(response, 32, 500);

  if (resp_len >= 12 && response[9] == 0x00) {
    ESP_LOGI(TAG, "Handshake successful");
    if (status_sensor_) {
      status_sensor_->publish_state("Module Online");
    }
    return true;
  }

  ESP_LOGW(TAG, "Handshake failed");
  if (status_sensor_) {
    status_sensor_->publish_state("Module Offline");
  }
  return false;
}

// 删除指定指纹
bool ZW101Component::delete_fingerprint(uint16_t id) {
  uint8_t packet[16];
  uint16_t length = 7;
  uint16_t delete_count = 1;  // 删除1个指纹
  uint16_t checksum = 1 + length + CMD_DEL_CHAR +
                      (id >> 8) + (id & 0xFF) +
                      (delete_count >> 8) + (delete_count & 0xFF);

  build_packet_header(packet, length);
  packet[9] = CMD_DEL_CHAR;
  packet[10] = (id >> 8) & 0xFF;
  packet[11] = id & 0xFF;
  packet[12] = (delete_count >> 8) & 0xFF;
  packet[13] = delete_count & 0xFF;
  packet[14] = (checksum >> 8) & 0xFF;
  packet[15] = checksum & 0xFF;

  write_array(packet, 16);
  flush();

  uint8_t response[32];
  uint8_t resp_len = wait_for_response(response, 32, 1000);

  if (resp_len >= 12 && response[9] == 0x00) {
    ESP_LOGI(TAG, "Fingerprint ID %d deleted successfully", id);
    if (status_sensor_) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Deleted ID: %d", id);
      status_sensor_->publish_state(buf);
    }
    return true;
  }

  ESP_LOGW(TAG, "Failed to delete fingerprint ID %d", id);
  return false;
}

// RGB LED 控制
void ZW101Component::set_rgb_led(uint8_t mode, uint8_t color, uint8_t brightness) {
  uint8_t packet[18];  // 总共18字节: 头(9) + 命令参数(6) + 校验和(2)
  uint16_t length = 8;  // 包长度字段: 8字节 (命令码到保留字节,不含校验和)

  // RGB 控制参数 (与原始C代码一致)
  uint8_t func_code = mode;      // 功能码: 1=呼吸 2=闪烁 3=常亮 4=关闭 5=渐变开 6=渐变关 7=跑马灯
  uint8_t start_color = color;   // 起始颜色: 1=蓝 2=绿 3=青 4=红 5=紫 6=黄 7=白
  uint8_t end_color_duty = brightness;  // 结束颜色/占空比: 0-255
  uint8_t loop_times = 0;        // 循环次数(0=无限)
  uint8_t cycle = 0x0f;          // 周期 (0x0f = 15, 单位:100ms)

  // 计算校验和: 从包标识开始到保留字节
  uint16_t checksum = 1 + length + CMD_RGB_CTRL +
                      func_code + start_color + end_color_duty + loop_times + cycle + 0x00;

  build_packet_header(packet, length);
  packet[9] = CMD_RGB_CTRL;
  packet[10] = func_code;
  packet[11] = start_color;
  packet[12] = end_color_duty;
  packet[13] = loop_times;
  packet[14] = cycle;
  packet[15] = 0x00;  // 保留字节
  packet[16] = (checksum >> 8) & 0xFF;
  packet[17] = checksum & 0xFF;

  write_array(packet, 18);  // 发送18字节
  flush();

  ESP_LOGI(TAG, "RGB LED set - Mode: %d, Color: %d, Brightness: %d", mode, color, brightness);
}

// 进入休眠模式
bool ZW101Component::enter_sleep_mode() {
  uint8_t packet[12];
  uint16_t length = 3;
  uint16_t checksum = 1 + length + CMD_INTO_SLEEP;

  build_packet_header(packet, length);
  packet[9] = CMD_INTO_SLEEP;
  packet[10] = (checksum >> 8) & 0xFF;
  packet[11] = checksum & 0xFF;

  ESP_LOGI(TAG, "Sending sleep command...");
  ESP_LOG_BUFFER_HEX(TAG, packet, 12);

  write_array(packet, 12);
  flush();

  uint8_t response[32];
  uint8_t resp_len = wait_for_response(response, 32, 400);

  ESP_LOGI(TAG, "Sleep response length: %d", resp_len);
  if (resp_len > 0) {
    ESP_LOG_BUFFER_HEX(TAG, response, resp_len);
  }

  if (resp_len >= 12 && response[9] == 0x00) {
    sleep_mode_ = true;
    ESP_LOGI(TAG, "Module entered sleep mode");
    if (status_sensor_) {
      status_sensor_->publish_state("Sleep Mode");
    }
    return true;
  }

  if (resp_len > 0 && resp_len >= 10) {
    ESP_LOGW(TAG, "Failed to enter sleep mode - Error code: 0x%02X", response[9]);
  } else {
    ESP_LOGW(TAG, "Failed to enter sleep mode - No response or timeout");
  }
  return false;
}

// 自动注册模式
bool ZW101Component::auto_enroll_mode(uint16_t timeout_sec) {
  if (auto_mode_active_) {
    ESP_LOGW(TAG, "Auto mode already active");
    return false;
  }

  uint8_t packet[15];
  uint16_t length = 6;
  uint16_t timeout_ms = timeout_sec * 1000;
  uint16_t checksum = 1 + length + CMD_AUTO_ENROLL +
                      (timeout_ms >> 8) + (timeout_ms & 0xFF);

  build_packet_header(packet, length);
  packet[9] = CMD_AUTO_ENROLL;
  packet[10] = (timeout_ms >> 8) & 0xFF;
  packet[11] = timeout_ms & 0xFF;
  packet[12] = 0x00;  // 保留字节
  packet[13] = (checksum >> 8) & 0xFF;
  packet[14] = checksum & 0xFF;

  write_array(packet, 15);
  flush();

  auto_mode_active_ = true;
  auto_mode_timeout_ = millis() + (timeout_sec * 1000);

  ESP_LOGI(TAG, "Auto enroll mode activated, timeout: %d seconds", timeout_sec);
  if (status_sensor_) {
    status_sensor_->publish_state("Auto Enroll Mode");
  }

  return true;
}

// 自动匹配模式
bool ZW101Component::auto_match_mode() {
  if (auto_mode_active_) {
    ESP_LOGW(TAG, "Auto mode already active");
    return false;
  }

  uint8_t packet[17];
  uint16_t length = 8;
  uint8_t buffer_id = 2;
  uint16_t start_page = 0;
  uint16_t page_num = library_capacity_;
  uint8_t security_level = 2;  // 安全等级

  uint16_t checksum = 1 + length + CMD_AUTO_MATCH + buffer_id +
                      (start_page >> 8) + (start_page & 0xFF) +
                      (page_num >> 8) + (page_num & 0xFF) +
                      security_level;

  build_packet_header(packet, length);
  packet[9] = CMD_AUTO_MATCH;
  packet[10] = buffer_id;
  packet[11] = (start_page >> 8) & 0xFF;
  packet[12] = start_page & 0xFF;
  packet[13] = (page_num >> 8) & 0xFF;
  packet[14] = page_num & 0xFF;
  packet[15] = security_level;
  packet[16] = (checksum >> 8) & 0xFF;
  packet[17] = checksum & 0xFF;

  write_array(packet, 17);
  flush();

  auto_mode_active_ = true;
  auto_mode_timeout_ = 0;  // 无超时

  ESP_LOGI(TAG, "Auto match mode activated");
  if (status_sensor_) {
    status_sensor_->publish_state("Auto Match Mode");
  }

  return true;
}

// 取消自动模式
void ZW101Component::cancel_auto_mode() {
  if (!auto_mode_active_) {
    return;
  }

  uint8_t packet[12];
  uint16_t length = 3;
  uint16_t checksum = 1 + length + 0x30;  // CMD_AUTO_CANCEL

  build_packet_header(packet, length);
  packet[9] = 0x30;  // CMD_AUTO_CANCEL
  packet[10] = (checksum >> 8) & 0xFF;
  packet[11] = checksum & 0xFF;

  write_array(packet, 12);
  flush();

  auto_mode_active_ = false;
  auto_mode_timeout_ = 0;

  ESP_LOGI(TAG, "Auto mode cancelled");
  if (status_sensor_) {
    status_sensor_->publish_state("Auto Mode Cancelled");
  }
}

// ==================== 私有方法 ====================

// 发送简单命令
void ZW101Component::send_cmd(uint8_t cmd) {
  uint8_t packet[12];
  uint16_t length = 3;
  uint16_t checksum = 1 + length + cmd;

  build_packet_header(packet, length);
  packet[9] = cmd;
  packet[10] = (checksum >> 8) & 0xFF;
  packet[11] = checksum & 0xFF;

  write_array(packet, 12);
  flush();
}

// 发送带1个参数的命令
void ZW101Component::send_cmd2(uint8_t cmd, uint8_t param1) {
  uint8_t packet[13];
  uint16_t length = 4;
  uint16_t checksum = 1 + length + cmd + param1;

  build_packet_header(packet, length);
  packet[9] = cmd;
  packet[10] = param1;
  packet[11] = (checksum >> 8) & 0xFF;
  packet[12] = checksum & 0xFF;

  write_array(packet, 13);
  flush();
}

// 发送存储命令
void ZW101Component::send_store_cmd(uint8_t buffer_id, uint16_t template_id) {
  uint8_t packet[15];
  uint16_t length = 6;
  uint16_t checksum = 1 + length + CMD_STORE_CHAR + buffer_id + (template_id >> 8) + (template_id & 0xFF);

  build_packet_header(packet, length);
  packet[9] = CMD_STORE_CHAR;
  packet[10] = buffer_id;
  packet[11] = (template_id >> 8) & 0xFF;
  packet[12] = template_id & 0xFF;
  packet[13] = (checksum >> 8) & 0xFF;
  packet[14] = checksum & 0xFF;

  write_array(packet, 15);
  flush();
}

// 发送搜索命令
void ZW101Component::send_search_cmd(uint8_t buffer_id, uint16_t start_page, uint16_t page_num) {
  uint8_t packet[17];
  uint16_t length = 8;

  build_packet_header(packet, length);
  packet[9] = CMD_SEARCH;
  packet[10] = buffer_id;
  packet[11] = (start_page >> 8) & 0xFF;
  packet[12] = start_page & 0xFF;
  packet[13] = (page_num >> 8) & 0xFF;
  packet[14] = page_num & 0xFF;
  
  // 计算校验和: 从packet[6]开始到packet[14] (不含校验和本身)
  uint16_t checksum = 0;
  for (int i = 6; i < 15; i++) {
    checksum += packet[i];
  }
  
  packet[15] = (checksum >> 8) & 0xFF;
  packet[16] = checksum & 0xFF;

  // 调试: 打印发送的搜索命令
  ESP_LOGI(TAG, "Search CMD - buffer_id:%d, start:%d, num:%d", buffer_id, start_page, page_num);
  ESP_LOG_BUFFER_HEX(TAG, packet, 17);

  write_array(packet, 17);
  flush();
}

// 构建数据包头部
void ZW101Component::build_packet_header(uint8_t *packet, uint16_t length) {
  packet[0] = HEADER_HIGH;
  packet[1] = HEADER_LOW;
  packet[2] = (DEVICE_ADDRESS >> 24) & 0xFF;
  packet[3] = (DEVICE_ADDRESS >> 16) & 0xFF;
  packet[4] = (DEVICE_ADDRESS >> 8) & 0xFF;
  packet[5] = DEVICE_ADDRESS & 0xFF;
  packet[6] = 0x01;  // 包标识: 命令包
  packet[7] = (length >> 8) & 0xFF;
  packet[8] = length & 0xFF;
}

// 接收响应 - 简单版本
bool ZW101Component::receive_response() {
  uint8_t response[50];
  uint8_t length = wait_for_response(response, 50, 500);

  // 检查确认码
  return (length >= 12 && response[9] == 0x00);
}

// 等待并读取响应数据
uint8_t ZW101Component::wait_for_response(uint8_t *buffer, uint8_t max_length, uint32_t timeout_ms) {
  uint8_t index = 0;
  uint32_t start_time = millis();

  while (millis() - start_time < timeout_ms && index < max_length) {
    if (available()) {
      buffer[index++] = read();
    } else {
      // 没有数据可读时让出CPU
      yield();
    }
  }

  return index;
}

}  // namespace zw101
}  // namespace esphome
