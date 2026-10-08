/*
 * e8_cm8 (ESP-IDF) - Màn hình E8 + mạch động cơ CM8
 *
 *   ESP32 <-> Màn hình HMI : UART2, GPIO16 (TX) -> RXB, GPIO17 (RX) <- TXA, 115200
 *   ESP32 <-> CM8          : UART1, GPIO32 (TX) -> SP3232 RXD, GPIO33 (RX) <- SP3232 TXD, 9600
 *
 * Hoạt động:
 *   - Vừa cấp điện: CM8 lệnh 1, Mode/Ratio = 0  -> QUẠT QUAY, động cơ đứng yên.
 *   - Bấm ▶ trên màn hình buổi tập: ghi Mode/Ratio, CM8 khởi động mềm rồi chạy theo cường độ
 *     (kể cả dao động khi bật Xung).
 *   - ⏸ / ■ / hết giờ / mất liên lạc màn hình: lệnh 2 -> CM8 giảm tốc về 0 rồi tắt quạt.
 *   - CM8 báo lỗi hoặc mất kết nối khi đang tập: ESP32 ghi 0x1001 = 0 để màn hình tạm dừng.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "cm8.h"

// Cấu hình
#define HMI_UART        UART_NUM_2
#define HMI_RX_PIN      17          // nối TXA của màn hình
#define HMI_TX_PIN      16          // nối RXB của màn hình
#define HMI_BAUD        115200
#define CHU_KY_DOC_MS   400         // chu kỳ đọc (luân phiên 2 lệnh, mỗi lệnh 200 ms)
#define IN_KHUNG_HEX    0           // 1: in khung dữ liệu thô để gỡ lỗi

// Cấu hình CM8
#define MODE_CHAY           3       // Mode khi động cơ chạy (CHƯA có giá trị chính thức -> tạm 3)
#define RATIO_CHAY          9       // Ratio (code CM8 lấy 9 làm chuẩn)
#define GIOI_HAN_DONG       370     // 3,70 A
#define TOC_DO_MAX          32      // tốc độ CM8 0..32 (CM8 chỉ dừng đúng ở số CHẴN)
#define MAT_KET_NOI_MS      2000    // không nhận dữ liệu từ màn hình quá 2 s -> dừng
#define CHO_CHUAN_BI_MS     3000    // chờ CM8 về trạng thái 0 trước lần chạy đầu tiên (tối đa)
#define DUNG_MAN_HINH_KHI_LOI 1     // 1: CM8 lỗi/mất kết nối khi đang tập -> ghi 0x1001 = 0

// Chế độ XUNG: cường độ dao động quanh mức đã chọn, ví dụ chọn 15 -> chạy 13..17
#define XUNG_BIEN_DO        2       // dao động ± 2 mức
#define XUNG_BUOC_MS        300     // mỗi 300 ms đổi 1 mức (1 chu kỳ 13->17->13 = 2,4 s)

#define DIA_CHI_HIEN_THI    0x100B  // biến cường độ hiển thị (Icon đồng hồ đọc biến này)
#define LAM_MOI_HIEN_THI_MS 1000    // ghi lại định kỳ dù không đổi (phòng màn hình khởi động lại)

// Tên hiển thị
static const char *TIEU_DE[22] = {
    "Tonification - Bras", "Tonification - Poitrine Bras", "Tonification - Sangle abdominale",
    "Tonification - Cuisses Fessiers", "Tonification - Complet",
    "Remodelage - Poitrine Bras", "Remodelage - Sangle abdominale", "Remodelage - Cuisses Fessiers 1",
    "Remodelage - Cuisses Fessiers 2", "Remodelage - Complet",
    "Anti-cellulite - Cuisses Fessiers", "Gainage - Complet 1", "Gainage - Complet 2",
    "Relaxation Drainage - Drainage", "Relaxation Drainage - Relax 1", "Relaxation Drainage - Relax 2",
    "Proprioception - 1", "Proprioception - 2", "Proprioception - 3",
    "Proprioception - 4", "Proprioception - 5", "Proprioception - 6"
};
static const char *CAP_DO[4] = {"Débutant", "Initié", "Confirmé", ""};

static int phut_cua_trang_duree(int t)
{
    if (t == 2) return 1;
    if (t >= 12 && t <= 20) return t - 10;
    return 0;
}

static void ten_trang(int t, char *out, size_t n)
{
    int phut = phut_cua_trang_duree(t);
    if (phut) { snprintf(out, n, "Durée Exercices - chọn %d phút", phut); return; }
    const char *s;
    switch (t) {
    case 0:  s = "Khởi động"; break;
    case 1:  s = "Menu chính"; break;
    case 3:  s = "Programmes - chọn mục tiêu"; break;
    case 4:  s = "Tonification - chọn nhóm cơ"; break;
    case 21: s = "Remodelage - chọn nhóm cơ"; break;
    case 22: s = "Anti-cellulite - chọn nhóm cơ"; break;
    case 23: s = "Gainage - chọn nhóm cơ"; break;
    case 24: s = "Relaxation Drainage - chọn nhóm cơ"; break;
    case 25: s = "Proprioception - chọn 1..6"; break;
    case 5:  s = "Chọn cấp độ"; break;
    case 6:  s = "Buổi tập Programmes"; break;
    case 7:  s = "Perso - chọn 1..6"; break;
    case 8:  s = "Buổi tập Perso"; break;
    case 9:  s = "Mot de passe"; break;
    case 10: s = "Buổi tập Manuel"; break;
    case 11: s = "Buổi tập Durée"; break;
    default: snprintf(out, n, "Trang %d", t); return;
    }
    snprintf(out, n, "%s", s);
}

static bool la_trang_buoi_tap(int t) { return t == 6 || t == 8 || t == 10 || t == 11; }

// Dữ liệu đọc được
static int      trang = -1;
static uint16_t bien[10];      // bien[i] = giá trị tại 0x1000 + i
static bool     co_bien = false;
static int64_t  lan_nhan_cuoi = 0;   // lần cuối nhận được dữ liệu từ màn hình (ms)

// CRC-16 Modbus
static uint16_t crc16_modbus(const uint8_t *d, size_t n)
{
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0xA001 : (c >> 1);
    }
    return c;
}

// 5A A5 07 03 [addr H L] [số word H L] [CRC L H]
static void gui_lenh_doc(uint16_t dia_chi, uint16_t so_word)
{
    uint8_t f[10] = {0x5A, 0xA5, 0x07, 0x03,
                     (uint8_t)(dia_chi >> 8), (uint8_t)dia_chi,
                     (uint8_t)(so_word >> 8), (uint8_t)so_word, 0, 0};
    uint16_t c = crc16_modbus(&f[3], 5);
    f[8] = c & 0xFF;
    f[9] = c >> 8;
    uart_write_bytes(HMI_UART, (const char *)f, sizeof(f));
}

// Lệnh ghi 1 biến: 5A A5 07 10 [addr H L] [data H L] [CRC L H]
static void gui_lenh_ghi(uint16_t dia_chi, uint16_t gia_tri)
{
    uint8_t f[10] = {0x5A, 0xA5, 0x07, 0x10,
                     (uint8_t)(dia_chi >> 8), (uint8_t)dia_chi,
                     (uint8_t)(gia_tri >> 8), (uint8_t)gia_tri, 0, 0};
    uint16_t c = crc16_modbus(&f[3], 5);
    f[8] = c & 0xFF;
    f[9] = c >> 8;
    uart_write_bytes(HMI_UART, (const char *)f, sizeof(f));
}

// Nhận khung
typedef enum { CHO_5A, CHO_A5, CHO_LEN, DOC } trang_thai_t;
static trang_thai_t st = CHO_5A;
static uint8_t  buf[260];
static uint16_t len_khung = 0, idx = 0;

static void xu_ly_khung(const uint8_t *p, uint16_t n)
{
    if (n < 3) return;
    uint16_t crc_nhan = p[n - 2] | (p[n - 1] << 8);
    if (crc_nhan != crc16_modbus(p, n - 2)) return;

#if IN_KHUNG_HEX
    printf("RX: 5A A5 %02X ", n);
    for (int i = 0; i < n; i++) printf("%02X ", p[i]);
    printf("\n");
#endif

    // Kết quả đọc: 03 | addr(2) | số word(2) | data | CRC
    if (p[0] == 0x03 && n >= 7) {
        uint16_t dia_chi = (p[1] << 8) | p[2];
        uint16_t so_word = (p[3] << 8) | p[4];
        if (n != 5 + 2 * so_word + 2) return;
        for (int i = 0; i < so_word; i++) {
            uint16_t v = (p[5 + 2 * i] << 8) | p[6 + 2 * i];
            uint16_t a = dia_chi + i;
            if (a == 0x7000) trang = v;
            else if (a >= 0x1000 && a < 0x100A) { bien[a - 0x1000] = v; co_bien = true; }
        }
        lan_nhan_cuoi = esp_timer_get_time() / 1000;
    }
    // Touch Returned Message của Variable Button: 41 | addr(2) | giá trị(2)
    else if (p[0] == 0x41 && n == 7) {
        uint16_t dia_chi = (p[1] << 8) | p[2];
        uint16_t v = (p[3] << 8) | p[4];
        if (dia_chi >= 0x1000 && dia_chi < 0x100A) { bien[dia_chi - 0x1000] = v; }
    }
}

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

// In ra monitor
static void mmss(uint16_t giay, char *out, size_t n)
{
    snprintf(out, n, "%02u:%02u", giay / 60, giay % 60);
}

static void ten_chuong_trinh(char *out, size_t n)
{
    switch (trang) {
    case 6: {
        uint16_t id = bien[7], cd = bien[9];
        const char *g = (id < 22) ? TIEU_DE[id] : "Programmes";
        if (cd < 3) snprintf(out, n, "%s - %s", g, CAP_DO[cd]);
        else        snprintf(out, n, "%s", g);
        return;
    }
    case 8:  snprintf(out, n, "Perso - %u", bien[6]); return;
    case 10: snprintf(out, n, "Manuel"); return;
    case 11: snprintf(out, n, "Durée Exercices"); return;
    default: out[0] = '\0';
    }
}

static void mo_ta_cm8(char *out, size_t n);   // định nghĩa ở phần CM8

static int  trang_truoc = -1, chay_truoc = -1, giay_truoc = -1;
static bool da_bao_ket_thuc = false;

static void cap_nhat_man_hinh(void)
{
    if (!co_bien) return;
    bool biet = (trang >= 0);
    char t1[64], t2[16];

    // 1) Đổi trang
    if (biet && trang != trang_truoc) {
        ten_trang(trang, t1, sizeof(t1));
        printf("\n[Trang] %s\n", t1);
        if (la_trang_buoi_tap(trang)) {
            ten_chuong_trinh(t1, sizeof(t1));
            mmss(bien[3], t2, sizeof(t2));
            printf("  Chương trình: %s\n", t1);
            printf("  Thời gian: %s  -> bấm ▶ trên màn hình để bắt đầu\n", t2);
        }
        trang_truoc = trang; chay_truoc = bien[1]; giay_truoc = bien[3]; da_bao_ket_thuc = false;
        return;
    }
    if (biet && !la_trang_buoi_tap(trang)) return;

    int chay = bien[1];
    int giay = bien[3];

    // 2) Bắt đầu / tạm dừng / kết thúc
    if (chay != chay_truoc) {
        mmss(giay, t2, sizeof(t2));
        if (chay == 1) {
            ten_chuong_trinh(t1, sizeof(t1));
            printf(">> BẮT ĐẦU - %s - còn %s\n", t1, t2);
            da_bao_ket_thuc = false;
        } else if (giay == 0) {
            if (!da_bao_ket_thuc) { printf(">> KẾT THÚC buổi tập\n"); da_bao_ket_thuc = true; }
        } else if (chay_truoc == 1) {
            printf(">> TẠM DỪNG tại %s\n", t2);
        }
        chay_truoc = chay;
    }

    // 3) Đang chạy: in mỗi khi số giây đổi
    if (chay == 1 && giay != giay_truoc) {
        mmss(giay, t2, sizeof(t2));
        if (bien[5]) {
            int lo = bien[0] + 1 - XUNG_BIEN_DO, hi = bien[0] + 1 + XUNG_BIEN_DO;
            if (lo < 1) lo = 1;
            if (hi > 32) hi = 32;
            char t3[96]; mo_ta_cm8(t3, sizeof(t3));
            printf("   Còn lại %s | Cường độ %2u | Xung bật (%d..%d) | %s\n",
                   t2, bien[0] + 1, lo, hi, t3);
        } else {
            char t3[96]; mo_ta_cm8(t3, sizeof(t3));
            printf("   Còn lại %s | Cường độ %2u | Xung tắt | %s\n",
                   t2, bien[0] + 1, t3);
        }
    }
    if (chay == 1 && giay == 0 && !da_bao_ket_thuc) {
        printf(">> KẾT THÚC buổi tập\n");
        da_bao_ket_thuc = true;
    }
    giay_truoc = giay;
}

// Chương trình chính
static int64_t ms_now(void) { return esp_timer_get_time() / 1000; }

// Điều khiển CM8
typedef enum {
    CM8_CHI_QUAT,   // vừa bật máy: lệnh 1, Mode 0 -> quạt quay, động cơ đứng
    CM8_CHUAN_BI,   // lần chạy đầu: ghi Mode/Ratio + lệnh 2, chờ CM8 về trạng thái 0
    CM8_CHAY,       // lệnh 1 + tốc độ theo cường độ
    CM8_DUNG        // lệnh 2: CM8 giảm tốc về 0, tắt quạt
} che_do_cm8_t;
static const char *TEN_CHE_DO[] = {"CHI QUAT", "CHUAN BI", "CHAY", "DUNG"};

static che_do_cm8_t che_do = CM8_CHI_QUAT;
static int64_t  lan_doi_che_do = 0;
static uint16_t toc_do_gui = 0;
static int      muc_dich_gan_nhat = 0;   // mức đích 1..32 (đã tính xung), 0 = không chạy
static bool     cho_chay_gan_nhat = false;

// Cường độ 1..32 -> tốc độ CM8 0..32, làm tròn xuống số CHẴN
// (CM8 tăng/giảm 2 đơn vị mỗi bước nên số lẻ làm motor nhảy qua lại)
static uint16_t muc_sang_toc_do(int muc)
{
    if (muc <= 1) return 0;
    int v = (muc - 1) * TOC_DO_MAX / 31;
    if (v > TOC_DO_MAX) v = TOC_DO_MAX;
    return (uint16_t)(v & ~1);
}

static void doi_che_do(che_do_cm8_t moi)
{
    if (moi == che_do) return;
    che_do = moi;
    lan_doi_che_do = ms_now();
    printf("   [CM8] -> %s\n", TEN_CHE_DO[moi]);
}

static void mo_ta_cm8(char *out, size_t n)
{
    cm8_trang_thai_t t = cm8_lay_trang_thai();
    if (!t.ket_noi) { snprintf(out, n, "CM8 KHONG PHAN HOI"); return; }
    snprintf(out, n, "CM8 toc do %2u | DAC %4u mV | I %.2f A | Loi %u (%s)",
             toc_do_gui, t.dac_mv, t.dong_x100 / 100.0, t.loi, cm8_ten_loi(t.loi));
}

// Gọi liên tục trong vòng lặp chính
static void cm8_cap_nhat(void)
{
    bool lien_lac = co_bien && (ms_now() - lan_nhan_cuoi < MAT_KET_NOI_MS);
    bool cho_chay = lien_lac && (trang < 0 || la_trang_buoi_tap(trang)) && bien[1] == 1;
    int  muc_dich = cho_chay ? (int)bien[0] + 1 : 0;      // cường độ 0..31 -> mức 1..32
    if (muc_dich > 32) muc_dich = 32;

    // Xung: cộng thêm độ lệch dạng tam giác 0,+1,+2,+1,0,-1,-2,-1 (với biên độ 2)
    if (muc_dich > 0 && bien[5] == 1) {
        const int so_buoc = 4 * XUNG_BIEN_DO;
        int k = (int)((ms_now() / XUNG_BUOC_MS) % so_buoc);
        int lech;
        if      (k <= XUNG_BIEN_DO)     lech = k;
        else if (k <= 3 * XUNG_BIEN_DO) lech = 2 * XUNG_BIEN_DO - k;
        else                            lech = k - 4 * XUNG_BIEN_DO;
        muc_dich += lech;
        if (muc_dich < 1)  muc_dich = 1;
        if (muc_dich > 32) muc_dich = 32;
    }
    muc_dich_gan_nhat = muc_dich;
    cho_chay_gan_nhat = cho_chay;

    cm8_trang_thai_t t = cm8_lay_trang_thai();
    bool cm8_ok = t.ket_noi && t.loi == 0;

    // Chuyển chế độ
    switch (che_do) {
    case CM8_CHI_QUAT:
        if (cho_chay && cm8_ok) doi_che_do(CM8_CHUAN_BI);
        break;
    case CM8_CHUAN_BI:
        // Đã ghi Mode/Ratio thì không về CHI_QUAT được nữa (CM8 nhớ Mode)
        if (!cho_chay) doi_che_do(CM8_DUNG);
        else if ((t.lenh == CM8_LENH_DUNG && t.mode == MODE_CHAY && t.trang_thai == 0) ||
                 ms_now() - lan_doi_che_do > CHO_CHUAN_BI_MS)
            doi_che_do(CM8_CHAY);                         // từ trạng thái 0 -> CM8 khởi động mềm
        break;
    case CM8_CHAY:
        if (!cho_chay) doi_che_do(CM8_DUNG);
        break;
    case CM8_DUNG:
        if (cho_chay && cm8_ok) doi_che_do(CM8_CHAY);
        break;
    }

    // Giá trị gửi xuống CM8
    switch (che_do) {
    case CM8_CHI_QUAT:
        toc_do_gui = 0;
        cm8_dat(CM8_LENH_CHAY, 0, GIOI_HAN_DONG, 0, 0);
        break;
    case CM8_CHUAN_BI:
    case CM8_DUNG:
        toc_do_gui = 0;
        cm8_dat(CM8_LENH_DUNG, 0, GIOI_HAN_DONG, MODE_CHAY, RATIO_CHAY);
        break;
    case CM8_CHAY:
        toc_do_gui = muc_sang_toc_do(muc_dich);
        cm8_dat(CM8_LENH_CHAY, toc_do_gui, GIOI_HAN_DONG, MODE_CHAY, RATIO_CHAY);
        break;
    }

    // CM8 lỗi / mất kết nối khi đang tập -> cho màn hình tạm dừng
    static int64_t lan_bao_loi = 0;
    if (cho_chay && !cm8_ok && ms_now() - lan_bao_loi >= 1000) {
        lan_bao_loi = ms_now();
        if (!t.ket_noi) printf("   [CM8] KHONG PHAN HOI - kiem tra nguon 220V, day, module SP3232\n");
        else            printf("   [CM8] LOI %u (%s) - tat/bat lai nguon CM8\n", t.loi, cm8_ten_loi(t.loi));
#if DUNG_MAN_HINH_KHI_LOI
        gui_lenh_ghi(0x1001, 0);
        printf("   [CM8] -> tam dung buoi tap tren man hinh\n");
#endif
    }

    // In khi trạng thái kết nối / lỗi của CM8 thay đổi
    static int ket_noi_truoc = -1, loi_truoc = -1;
    if ((int)t.ket_noi != ket_noi_truoc || (t.ket_noi && (int)t.loi != loi_truoc)) {
        if (t.ket_noi) printf("   [CM8] Ket noi OK | Loi %u (%s)\n", t.loi, cm8_ten_loi(t.loi));
        else if (ket_noi_truoc != -1) printf("   [CM8] MAT KET NOI\n");
        ket_noi_truoc = t.ket_noi;
        loi_truoc = t.loi;
    }
}

// Đồng hồ cường độ trên màn hình
static void cap_nhat_hien_thi(void)
{
    static int     da_ghi = -1;
    static int64_t lan_ghi = 0;
    if (!co_bien) return;

    int muon = (cho_chay_gan_nhat && bien[5] == 1 && muc_dich_gan_nhat > 0)
               ? muc_dich_gan_nhat - 1          // đang dao động: mức 1..32 -> 0..31
               : (int)bien[0];                  // bình thường: đúng cường độ đã chọn
    if (muon < 0) muon = 0;
    if (muon > 31) muon = 31;

    if (muon != da_ghi || ms_now() - lan_ghi >= LAM_MOI_HIEN_THI_MS) {
        gui_lenh_ghi(DIA_CHI_HIEN_THI, (uint16_t)muon);
        da_ghi = muon;
        lan_ghi = ms_now();
    }
}

void app_main(void)
{
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

    cm8_dat(CM8_LENH_CHAY, 0, GIOI_HAN_DONG, 0, 0);   // vừa bật lên là quạt quay
    cm8_begin();
    printf("\n=== E8 + CM8: man hinh UART2 (GPIO16/17), CM8 UART1 (GPIO%d/%d) ===\n",
           CM8_TX_PIN, CM8_RX_PIN);

    uint8_t rx[128];
    int64_t lan_doc = 0;
    bool luot_trang = true, da_nhac = false;

    while (1) {
        // Đọc dữ liệu từ màn hình (chờ tối đa 10 ms)
        int n = uart_read_bytes(HMI_UART, rx, sizeof(rx), pdMS_TO_TICKS(10));
        for (int i = 0; i < n; i++) nhan_byte(rx[i]);

        // Luân phiên gửi lệnh đọc trang (0x7000) và 10 biến 0x1000..0x1009
        if (ms_now() - lan_doc >= CHU_KY_DOC_MS / 2) {
            lan_doc = ms_now();
            if (luot_trang) gui_lenh_doc(0x7000, 1);
            else            gui_lenh_doc(0x1000, 10);
            luot_trang = !luot_trang;
        }

        cap_nhat_man_hinh();
        cm8_cap_nhat();
        cap_nhat_hien_thi();

        if (!da_nhac && ms_now() > 5000 && trang < 0 && !co_bien) {
            printf("(Chưa nhận được dữ liệu từ màn hình - kiểm tra dây GPIO16/GPIO17/GND)\n");
            da_nhac = true;
        }
    }
}
