/*
 * cm8.h - Thư viện ESP32 (ESP-IDF) làm Modbus MASTER điều khiển mạch CM8
 *
 * Viết lại theo ý tưởng file control_motor.cpp (Arduino) anh hướng dẫn gửi:
 *   - Liên tục GHI khối thanh ghi 0..14 (FC16): lệnh, tốc độ, giới hạn dòng, mode, ratio
 *   - Liên tục ĐỌC trạng thái (FC03): lỗi, thẻ, DAC, dòng điện...
 *   => CM8 luôn nhận được tin nhắn nên KHÔNG báo lỗi 2 (mất kết nối).
 *
 * Việc gửi/nhận chạy trong 1 task FreeRTOS riêng, nên vòng lặp chính
 * (sau này là code màn hình E8) không bị chặn.
 *
 * Cách dùng:
 *   cm8_begin();                                  // 1 lần lúc khởi động
 *   cm8_dat(lenh, toc_do, gioi_han_dong, mode, ratio);   // đổi giá trị bất cứ lúc nào
 *   cm8_trang_thai_t t = cm8_lay_trang_thai();    // đọc trạng thái mới nhất
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Chân / thông số (đổi ở đây nếu cần) 
#define CM8_UART_NUM     UART_NUM_1
#define CM8_TX_PIN       32      // -> module SP3232 RXD
#define CM8_RX_PIN       33      // <- module SP3232 TXD
#define CM8_BAUD         9600
#define CM8_ID           1

// Giá trị lệnh (thanh ghi 0)
#define CM8_LENH_CHAY    1       // bật quạt + bật nguồn công suất + chạy theo tốc độ đặt
#define CM8_LENH_DUNG    2       // giảm tốc về 0, tắt quạt

typedef struct {
    bool     ket_noi;        // true = CM8 đang trả lời
    uint16_t lenh;           // reg 0  (CM8 đang giữ)
    uint16_t toc_do;         // reg 6
    uint16_t mode, ratio;    // reg 8, 9
    uint16_t nguong_cao;     // reg 4  upper_setting (mV)
    uint16_t nguong_thap;    // reg 5  lower_setting (mV)
    uint16_t loi;            // reg 17 0 OK, 2 mất kết nối, 3 quá dòng, 4 quá áp
    uint16_t the;            // reg 18 trạng thái thẻ / IN1
    uint16_t dac_mv;         // reg 19 điện áp DAC (mV) - càng THẤP motor càng NHANH
    uint16_t dong_x100;      // reg 22 dòng điện x100 (A)
    uint16_t dem_qua_dong;   // reg 23
    uint16_t trang_thai;     // reg 28 state_run: 0 dừng, 1 khởi động mềm, 2 đang chạy
    uint32_t so_lan_ok;      // số giao dịch thành công
    uint32_t so_lan_loi;     // số giao dịch không có phản hồi / sai CRC
    uint32_t tong_byte_nhan; // tổng số byte nhận được trên chân RX (kể cả rác)
    uint8_t  mau[8];         // vài byte nhận được gần nhất (để chẩn đoán)
    uint8_t  so_mau;
    bool     nghe_lai;       // true = nhận lại đúng khung mình gửi (TX nối vòng về RX)
} cm8_trang_thai_t;

void cm8_begin(void);
void cm8_dat(uint16_t lenh, uint16_t toc_do, uint16_t gioi_han_dong_x100,
             uint16_t mode, uint16_t ratio);
cm8_trang_thai_t cm8_lay_trang_thai(void);
const char *cm8_ten_loi(uint16_t loi);
