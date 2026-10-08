/*
 * dieu_khien.h - Task điều khiển (ưu tiên cao nhất, chu kỳ 50 ms)
 *
 * Lấy bản sao dữ liệu màn hình + trạng thái CM8 -> quyết định lệnh gửi CM8,
 * cập nhật đồng hồ cường độ trên màn hình, xử lý lỗi / mất kết nối.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    CM8_CHI_QUAT,   // vừa bật máy: lệnh 1, Mode 0 -> quạt quay, động cơ đứng
    CM8_CHUAN_BI,   // lần chạy đầu: ghi Mode/Ratio + lệnh 2, chờ CM8 về trạng thái 0
    CM8_CHAY,       // lệnh 1 + tốc độ theo cường độ
    CM8_DUNG        // lệnh 2: CM8 giảm tốc về 0, tắt quạt
} che_do_cm8_t;

typedef struct {
    che_do_cm8_t che_do;
    uint16_t     toc_do_gui;     // tốc độ đang gửi CM8 (0..32)
    int          muc_dich;       // mức 1..32 (đã tính xung), 0 = không chạy
    bool         cho_chay;
} dieu_khien_trang_thai_t;

void dieu_khien_khoi_dong(void);
dieu_khien_trang_thai_t dieu_khien_lay(void);
const char *dieu_khien_ten_che_do(che_do_cm8_t c);
