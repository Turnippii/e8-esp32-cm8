/*
 * cm8.c - Task Modbus RTU master cho mạch CM8 (xem cm8.h)
 *   - Chưa có lệnh (cm8_dat chưa được gọi) thì chỉ ĐỌC, để task điều khiển biết
 *     CM8 vừa bật hay đã chạy từ trước. Đọc cũng tính là tin nhắn nên CM8 không báo lỗi 2.
 *   - Mỗi vòng báo watchdog "còn sống".
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "cau_hinh.h"
#include "cm8.h"

#define TIMEOUT_MS        300    // chờ CM8 trả lời tối đa
#define NGHI_GIUA_MS      50     // nghỉ giữa 2 giao dịch (giống control_motor: ~100 ms/chu kỳ)
#define SO_LAN_MAT_KN     5      // 5 lần liên tiếp không trả lời => báo mất kết nối

#define SO_REG_GHI        15     // ghi reg 0..14 (giống telegram[0] của control_motor)
#define SO_REG_DOC        29     // đọc reg 0..28

// ---- Giá trị muốn gửi (main đặt) ----
static portMUX_TYPE khoa = portMUX_INITIALIZER_UNLOCKED;
static uint16_t dat_lenh = CM8_LENH_DUNG, dat_toc_do = 0, dat_dong = 0, dat_mode = 0, dat_ratio = 0;
static bool     co_lenh = false;      // đã có lệnh từ task điều khiển chưa

// ---- Trạng thái đọc về ----
static cm8_trang_thai_t tt;

static int64_t ms_now(void) { return esp_timer_get_time() / 1000; }

static uint16_t crc16_modbus(const uint8_t *d, size_t n)
{
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0xA001 : (c >> 1);
    }
    return c;
}

static bool crc_dung(const uint8_t *d, int n)
{
    return n >= 4 && (uint16_t)(d[n - 2] | (d[n - 1] << 8)) == crc16_modbus(d, n - 2);
}

// Gửi khung (tự thêm CRC), chờ đủ 'can_nhan' byte. Trả về số byte nhận được.
static int giao_dich(uint8_t *khung, int n, uint8_t *tra_loi, int can_nhan)
{
    uint16_t c = crc16_modbus(khung, n);
    khung[n++] = c & 0xFF;
    khung[n++] = c >> 8;

    uart_flush_input(CM8_UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(5));                       // khoảng lặng > 3,5 ký tự
    uart_write_bytes(CM8_UART_NUM, (const char *)khung, n);
    uart_wait_tx_done(CM8_UART_NUM, pdMS_TO_TICKS(100));

    int nhan = 0;
    int64_t t0 = ms_now();
    while (nhan < can_nhan && ms_now() - t0 < TIMEOUT_MS) {
        int k = uart_read_bytes(CM8_UART_NUM, tra_loi + nhan, can_nhan - nhan, pdMS_TO_TICKS(20));
        if (k > 0) nhan += k;
    }

    // Ghi lại để chẩn đoán: nhận được bao nhiêu byte, có phải đang nghe lại chính mình không
    if (nhan > 0) {
        portENTER_CRITICAL(&khoa);
        tt.tong_byte_nhan += nhan;
        tt.so_mau = nhan < 8 ? nhan : 8;
        memcpy(tt.mau, tra_loi, tt.so_mau);
        if (khung[1] == 0x03)   // chỉ xét với lệnh đọc (trả lời thật bắt đầu 01 03 3A..., khác khung gửi)
            tt.nghe_lai = (nhan >= 6 && memcmp(tra_loi, khung, 6) == 0);
        portEXIT_CRITICAL(&khoa);
    }
    return nhan;
}

// FC16: ghi reg 0..14 một lần (lệnh, tốc độ, giới hạn dòng, mode, ratio; còn lại = 0)
static bool ghi_khoi_lenh(void)
{
    uint16_t v[SO_REG_GHI] = {0};
    portENTER_CRITICAL(&khoa);
    v[0] = dat_lenh;
    v[3] = dat_dong;
    v[6] = dat_toc_do;
    v[8] = dat_mode;
    v[9] = dat_ratio;
    portEXIT_CRITICAL(&khoa);

    uint8_t f[7 + 2 * SO_REG_GHI + 2] = {CM8_ID, 0x10, 0x00, 0x00, 0x00, SO_REG_GHI, 2 * SO_REG_GHI};
    int n = 7;
    for (int i = 0; i < SO_REG_GHI; i++) { f[n++] = v[i] >> 8; f[n++] = v[i] & 0xFF; }

    uint8_t r[8];
    int k = giao_dich(f, n, r, 8);
    return k == 8 && crc_dung(r, 8) && r[0] == CM8_ID && r[1] == 0x10;
}

// FC03: đọc reg 0..28
static bool doc_trang_thai(void)
{
    uint8_t f[8] = {CM8_ID, 0x03, 0x00, 0x00, 0x00, SO_REG_DOC};
    uint8_t r[5 + 2 * SO_REG_DOC];
    int can = sizeof(r);
    int k = giao_dich(f, 6, r, can);
    if (k != can || !crc_dung(r, k) || r[1] != 0x03 || r[2] != 2 * SO_REG_DOC) return false;

    uint16_t g[SO_REG_DOC];
    for (int i = 0; i < SO_REG_DOC; i++) g[i] = (r[3 + 2 * i] << 8) | r[4 + 2 * i];

    portENTER_CRITICAL(&khoa);
    tt.lenh = g[0];        tt.toc_do = g[6];
    tt.mode = g[8];        tt.ratio = g[9];
    tt.nguong_cao = g[4];  tt.nguong_thap = g[5];
    tt.loi = g[17];        tt.the = g[18];
    tt.dac_mv = g[19];     tt.dong_x100 = g[22];
    tt.dem_qua_dong = g[23];
    tt.trang_thai = g[28];
    portEXIT_CRITICAL(&khoa);
    return true;
}

static void cap_nhat_dem(bool ok, int *lien_tiep_loi)
{
    portENTER_CRITICAL(&khoa);
    if (ok) { tt.so_lan_ok++; *lien_tiep_loi = 0; tt.ket_noi = true; }
    else {
        tt.so_lan_loi++;
        if (++(*lien_tiep_loi) >= SO_LAN_MAT_KN) tt.ket_noi = false;
    }
    portEXIT_CRITICAL(&khoa);
}

static void task_cm8(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    int lien_tiep_loi = 0;
    while (1) {
        // Luân phiên GHI rồi ĐỌC (sửa lỗi u8query của control_motor: chỉ có 2 telegram 0 và 1)
        portENTER_CRITICAL(&khoa);
        bool ghi = co_lenh;
        portEXIT_CRITICAL(&khoa);
        if (ghi) {
            cap_nhat_dem(ghi_khoi_lenh(), &lien_tiep_loi);
            vTaskDelay(pdMS_TO_TICKS(NGHI_GIUA_MS));
        }
        cap_nhat_dem(doc_trang_thai(), &lien_tiep_loi);
        vTaskDelay(pdMS_TO_TICKS(NGHI_GIUA_MS));
        esp_task_wdt_reset();               // 1 vòng tối đa ~0,8 s (2 lần chờ 300 ms) < 3 s
    }
}

void cm8_begin(void)
{
    const uart_config_t cfg = {
        .baud_rate  = CM8_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CM8_UART_NUM, 512, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CM8_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CM8_UART_NUM, CM8_TX_PIN, CM8_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreatePinnedToCore(task_cm8, "cm8", STACK_CM8, NULL, UU_TIEN_CM8, NULL, CORE_UNG_DUNG);
}

void cm8_dat(uint16_t lenh, uint16_t toc_do, uint16_t gioi_han_dong_x100,
             uint16_t mode, uint16_t ratio)
{
    if (toc_do > 32) toc_do = 32;
    portENTER_CRITICAL(&khoa);
    dat_lenh = lenh;
    dat_toc_do = toc_do;
    dat_dong = gioi_han_dong_x100;
    dat_mode = mode;
    dat_ratio = ratio;
    co_lenh = true;
    portEXIT_CRITICAL(&khoa);
}

cm8_trang_thai_t cm8_lay_trang_thai(void)
{
    portENTER_CRITICAL(&khoa);
    cm8_trang_thai_t t = tt;
    portEXIT_CRITICAL(&khoa);
    return t;
}

const char *cm8_ten_loi(uint16_t loi)
{
    switch (loi) {
    case 0:  return "OK";
    case 2:  return "MAT KET NOI";
    case 3:  return "QUA DONG";
    case 4:  return "QUA AP";
    default: return "?";
    }
}
