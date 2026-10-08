/*
 * hmi.c - Task nhận / task hỏi màn hình HMI (xem hmi.h)
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "cau_hinh.h"
#include "hmi.h"
#include "nhat_ky.h"

static SemaphoreHandle_t khoa;          // mutex bảo vệ 'dl'
static hmi_du_lieu_t dl = { .trang = -1 };

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

// Khung 10 byte: 5A A5 07 | lệnh | addr H L | data H L | CRC L H
// uart_write_bytes của ESP-IDF có khóa riêng nên nhiều task cùng gửi không bị xen byte.
static void gui_khung(uint8_t lenh, uint16_t dia_chi, uint16_t gia_tri)
{
    uint8_t f[10] = {0x5A, 0xA5, 0x07, lenh,
                     (uint8_t)(dia_chi >> 8), (uint8_t)dia_chi,
                     (uint8_t)(gia_tri >> 8), (uint8_t)gia_tri, 0, 0};
    uint16_t c = crc16_modbus(&f[3], 5);
    f[8] = c & 0xFF;
    f[9] = c >> 8;
    uart_write_bytes(HMI_UART, (const char *)f, sizeof(f));
}

void hmi_ghi(uint16_t dia_chi, uint16_t gia_tri) { gui_khung(0x10, dia_chi, gia_tri); }

hmi_du_lieu_t hmi_lay(void)
{
    hmi_du_lieu_t d;
    xSemaphoreTake(khoa, portMAX_DELAY);
    d = dl;
    xSemaphoreGive(khoa);
    return d;
}

bool hmi_con_lien_lac(const hmi_du_lieu_t *d)
{
    return d->co_bien && (ms_now() - d->lan_nhan_cuoi_ms < HMI_MAT_KET_NOI_MS);
}

// ---------------- Giải mã khung nhận được ----------------
static void xu_ly_khung(const uint8_t *p, uint16_t n)
{
    if (n < 3) return;
    uint16_t crc_nhan = p[n - 2] | (p[n - 1] << 8);
    if (crc_nhan != crc16_modbus(p, n - 2)) return;

    // Kết quả đọc: 03 | addr(2) | số word(2) | data | CRC
    if (p[0] == 0x03 && n >= 7) {
        uint16_t dia_chi = (p[1] << 8) | p[2];
        uint16_t so_word = (p[3] << 8) | p[4];
        if (n != 5 + 2 * so_word + 2) return;
        xSemaphoreTake(khoa, portMAX_DELAY);
        for (int i = 0; i < so_word; i++) {
            uint16_t v = (p[5 + 2 * i] << 8) | p[6 + 2 * i];
            uint16_t a = dia_chi + i;
            if (a == 0x7000) dl.trang = v;
            else if (a >= 0x1000 && a < 0x1000 + HMI_SO_BIEN) { dl.bien[a - 0x1000] = v; dl.co_bien = true; }
        }
        dl.lan_nhan_cuoi_ms = ms_now();
        xSemaphoreGive(khoa);
    }
    // Touch Returned Message: 41 | addr(2) | giá trị(2)
    else if (p[0] == 0x41 && n == 7) {
        uint16_t dia_chi = (p[1] << 8) | p[2];
        uint16_t v = (p[3] << 8) | p[4];
        if (dia_chi >= 0x1000 && dia_chi < 0x1000 + HMI_SO_BIEN) {
            xSemaphoreTake(khoa, portMAX_DELAY);
            dl.bien[dia_chi - 0x1000] = v;
            xSemaphoreGive(khoa);
        }
    }
}

typedef enum { CHO_5A, CHO_A5, CHO_LEN, DOC } buoc_t;
static buoc_t   st = CHO_5A;
static uint8_t  buf[260];
static uint16_t len_khung = 0, idx = 0;

static void nhan_byte(uint8_t c)
{
    switch (st) {
    case CHO_5A:  if (c == 0x5A) st = CHO_A5; break;
    case CHO_A5:  st = (c == 0xA5) ? CHO_LEN : (c == 0x5A ? CHO_A5 : CHO_5A); break;
    case CHO_LEN: len_khung = c; idx = 0; st = (len_khung > 0) ? DOC : CHO_5A; break;
    case DOC:
        buf[idx++] = c;
        if (idx >= len_khung) { xu_ly_khung(buf, len_khung); st = CHO_5A; }
        break;
    }
}

// ---------------- Task nhận: ngủ cho tới khi có byte về ----------------
static void task_hmi_nhan(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    uint8_t rx[128];
    while (1) {
        int n = uart_read_bytes(HMI_UART, rx, sizeof(rx), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) nhan_byte(rx[i]);
        esp_task_wdt_reset();               // báo watchdog "task này còn sống"
    }
}

// ---------------- Task hỏi: đúng chu kỳ 200 ms ----------------
static void task_hmi_hoi(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    TickType_t moc = xTaskGetTickCount();
    bool luot_trang = true;
    while (1) {
        vTaskDelayUntil(&moc, pdMS_TO_TICKS(HMI_CHU_KY_HOI_MS));   // chu kỳ đều, không trôi
        if (luot_trang) gui_khung(0x03, 0x7000, 1);
        else            gui_khung(0x03, 0x1000, HMI_SO_BIEN);
        luot_trang = !luot_trang;
        esp_task_wdt_reset();

#if THU_TREO_TASK
        if (ms_now() > 20000) {
            nhat_ky_gui("!! THU: task hmi_hoi bi treo (watchdog se khoi dong lai sau %d s)", WDT_TIMEOUT_MS / 1000);
            while (1) vTaskDelay(pdMS_TO_TICKS(1000));   // treo: không còn báo watchdog
        }
#endif
    }
}

void hmi_khoi_dong(void)
{
    khoa = xSemaphoreCreateMutex();

    const uart_config_t cfg = {
        .baud_rate  = HMI_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(HMI_UART, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HMI_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(HMI_UART, HMI_TX_PIN, HMI_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreatePinnedToCore(task_hmi_nhan, "hmi_nhan", STACK_HMI, NULL, UU_TIEN_HMI_NHAN, NULL, CORE_UNG_DUNG);
    xTaskCreatePinnedToCore(task_hmi_hoi,  "hmi_hoi",  STACK_HMI, NULL, UU_TIEN_HMI_HOI,  NULL, CORE_UNG_DUNG);
}
