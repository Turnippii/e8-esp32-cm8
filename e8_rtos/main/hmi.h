/*
 * hmi.h - Giao tiếp màn hình HMI (LT7689, UI_Editor-II) bằng 2 task:
 *   hmi_nhan : chỉ nhận và giải mã khung từ màn hình
 *   hmi_hoi  : định kỳ gửi lệnh đọc trang (0x7000) và biến 0x1000..0x1009
 * Dữ liệu dùng chung được bảo vệ bằng mutex; task khác lấy bản sao qua hmi_lay().
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HMI_SO_BIEN 10                  // 0x1000 .. 0x1009

typedef struct {
    int      trang;                     // trang hiện tại, -1 = chưa biết
    uint16_t bien[HMI_SO_BIEN];         // bien[i] = giá trị tại 0x1000 + i
    bool     co_bien;                   // đã đọc được biến ít nhất 1 lần
    int64_t  lan_nhan_cuoi_ms;          // lần cuối nhận được dữ liệu (ms)
} hmi_du_lieu_t;

void hmi_khoi_dong(void);
hmi_du_lieu_t hmi_lay(void);            // bản sao dữ liệu mới nhất (an toàn giữa các task)
bool hmi_con_lien_lac(const hmi_du_lieu_t *d);
void hmi_ghi(uint16_t dia_chi, uint16_t gia_tri);   // ghi 1 biến lên màn hình (gọi từ task nào cũng được)
