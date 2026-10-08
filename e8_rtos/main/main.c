/*
 * e8_rtos (ESP-IDF) - Màn hình E8 + mạch động cơ CM8, chia thành nhiều task FreeRTOS
 *
 *   Task          Ưu tiên  Việc
 *   dieu_khien      6      50 ms/lần: đọc dữ liệu màn hình + CM8 -> quyết định lệnh, an toàn
 *   cm8             5      Modbus RTU với CM8 (UART1, GPIO32/33, 9600)
 *   hmi_nhan        5      nhận + giải mã khung từ màn hình (UART2, GPIO16/17, 115200)
 *   hmi_hoi         4      200 ms/lần: hỏi màn hình trang hiện tại và các biến
 *   nhat_ky         2      in log ra Monitor (các task khác gửi chuỗi qua hàng đợi)
 *
 *   Dữ liệu dùng chung: mutex (màn hình), critical section (CM8, điều khiển), queue (log).
 *   Task watchdog: task nào không báo "còn sống" quá 3 s -> ESP32 khởi động lại,
 *   sau đó màn hình bị cho tạm dừng, người dùng phải bấm ▶ lại.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "cau_hinh.h"
#include "cm8.h"
#include "hmi.h"
#include "dieu_khien.h"
#include "nhat_ky.h"

static const char *ten_ly_do_reset(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "cap dien";
    case ESP_RST_SW:       return "khoi dong lai bang phan mem";
    case ESP_RST_PANIC:    return "LOI CHUONG TRINH (panic)";
    case ESP_RST_INT_WDT:  return "WATCHDOG ngat";
    case ESP_RST_TASK_WDT: return "WATCHDOG task (co task bi treo)";
    case ESP_RST_WDT:      return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "SUT AP nguon";
    case ESP_RST_EXT:      return "nut EN / chan reset";
    default:               return "khac";
    }
}

void app_main(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    printf("\n=== E8 + CM8 (FreeRTOS) | khoi dong vi: %s ===\n", ten_ly_do_reset(r));

    // Task watchdog: cấu hình lại ngay trong code (không phụ thuộc file sdkconfig)
    esp_task_wdt_config_t wdt = {
        .timeout_ms     = WDT_TIMEOUT_MS,
        .idle_core_mask = (1 << 0) | (1 << 1),   // giám sát cả task Idle của 2 core
        .trigger_panic  = true,                  // quá hạn -> khởi động lại
    };
    if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);

    // Thứ tự: log trước (để task khác gửi log được), rồi CM8, màn hình, điều khiển
    nhat_ky_khoi_dong();
    cm8_begin();
    hmi_khoi_dong();
    dieu_khien_khoi_dong();

    printf("Da tao 5 task: dieu_khien, cm8, hmi_nhan, hmi_hoi, nhat_ky\n");
    // app_main kết thúc ở đây; các task tiếp tục chạy độc lập
}
