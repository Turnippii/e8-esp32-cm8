/*
 * nhat_ky.c - Task in log (xem nhat_ky.h)
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "cau_hinh.h"
#include "hmi.h"
#include "cm8.h"
#include "dieu_khien.h"
#include "nhat_ky.h"

#define DO_DAI_DONG   160
#define SO_DONG_CHO   16

static QueueHandle_t hang_doi;
static hmi_du_lieu_t d;              // bản sao dữ liệu màn hình, chỉ task nhật ký dùng

static int64_t ms_now(void) { return esp_timer_get_time() / 1000; }

void nhat_ky_gui(const char *fmt, ...)
{
    if (!hang_doi) return;
    char dong[DO_DAI_DONG];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dong, sizeof(dong), fmt, ap);
    va_end(ap);
    xQueueSend(hang_doi, dong, 0);   // 0 = không chờ; đầy thì bỏ dòng này
}

// ---------------- Tên hiển thị ----------------
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

static void mmss(uint16_t giay, char *out, size_t n)
{
    snprintf(out, n, "%02u:%02u", giay / 60, giay % 60);
}

static void ten_chuong_trinh(char *out, size_t n)
{
    switch (d.trang) {
    case 6: {
        uint16_t id = d.bien[7], cd = d.bien[9];
        const char *g = (id < 22) ? TIEU_DE[id] : "Programmes";
        if (cd < 3) snprintf(out, n, "%s - %s", g, CAP_DO[cd]);
        else        snprintf(out, n, "%s", g);
        return;
    }
    case 8:  snprintf(out, n, "Perso - %u", d.bien[6]); return;
    case 10: snprintf(out, n, "Manuel"); return;
    case 11: snprintf(out, n, "Durée Exercices"); return;
    default: out[0] = '\0';
    }
}


static void mo_ta_cm8(char *out, size_t n, uint16_t toc_do)
{
    cm8_trang_thai_t t = cm8_lay_trang_thai();
    if (!t.ket_noi) { snprintf(out, n, "CM8 KHONG PHAN HOI"); return; }
    snprintf(out, n, "CM8 toc do %2u | DAC %4u mV | I %.2f A | Loi %u (%s)",
             toc_do, t.dac_mv, t.dong_x100 / 100.0, t.loi, cm8_ten_loi(t.loi));
}

// ---------------- Tiến trình buổi tập (trước đây là cap_nhat_man_hinh) ----------------
static int  trang_truoc = -1, chay_truoc = -1, giay_truoc = -1;
static bool da_bao_ket_thuc = false;

static void in_tien_trinh(void)
{
    if (!d.co_bien) return;
    bool biet = (d.trang >= 0);
    char t1[64], t2[16], t3[96];

    if (biet && d.trang != trang_truoc) {
        ten_trang(d.trang, t1, sizeof(t1));
        printf("\n[Trang] %s\n", t1);
        if (la_trang_buoi_tap(d.trang)) {
            ten_chuong_trinh(t1, sizeof(t1));
            mmss(d.bien[3], t2, sizeof(t2));
            printf("  Chương trình: %s\n", t1);
            printf("  Thời gian: %s  -> bấm ▶ trên màn hình để bắt đầu\n", t2);
        }
        trang_truoc = d.trang; chay_truoc = d.bien[1]; giay_truoc = d.bien[3]; da_bao_ket_thuc = false;
        return;
    }
    if (biet && !la_trang_buoi_tap(d.trang)) return;

    int chay = d.bien[1];
    int giay = d.bien[3];

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

    if (chay == 1 && giay != giay_truoc) {
        dieu_khien_trang_thai_t k = dieu_khien_lay();
        mmss(giay, t2, sizeof(t2));
        mo_ta_cm8(t3, sizeof(t3), k.toc_do_gui);
        if (d.bien[5]) {
            int lo = d.bien[0] + 1 - XUNG_BIEN_DO, hi = d.bien[0] + 1 + XUNG_BIEN_DO;
            if (lo < 1) lo = 1;
            if (hi > 32) hi = 32;
            printf("   Còn lại %s | Cường độ %2u | Xung bật (%d..%d) | %s\n", t2, d.bien[0] + 1, lo, hi, t3);
        } else {
            printf("   Còn lại %s | Cường độ %2u | Xung tắt | %s\n", t2, d.bien[0] + 1, t3);
        }
    }
    if (chay == 1 && giay == 0 && !da_bao_ket_thuc) {
        printf(">> KẾT THÚC buổi tập\n");
        da_bao_ket_thuc = true;
    }
    giay_truoc = giay;
}

#if IN_THONG_KE_TASK
// Stack còn trống ít nhất từ lúc chạy (ESP-IDF: tính bằng byte). Còn < ~500 byte là nên tăng stack.
static void in_thong_ke(void)
{
    static const char *TEN[] = {"dieu_khien", "cm8", "hmi_nhan", "hmi_hoi", "nhat_ky"};
    printf("---- Stack con trong (byte):");
    for (int i = 0; i < 5; i++) {
        TaskHandle_t h = xTaskGetHandle(TEN[i]);
        if (h) printf(" %s=%u", TEN[i], (unsigned)uxTaskGetStackHighWaterMark(h));
    }
    printf(" | heap trong=%u ----\n", (unsigned)xPortGetFreeHeapSize());
}
#endif

static void task_nhat_ky(void *arg)
{
    (void)arg;
    char dong[DO_DAI_DONG];
    bool da_nhac = false;
    int64_t lan_thong_ke = ms_now();
    while (1) {
        // Chờ dòng log tối đa 100 ms, rồi in hết các dòng đang chờ
        if (xQueueReceive(hang_doi, dong, pdMS_TO_TICKS(100)) == pdTRUE) {
            printf("%s\n", dong);
            while (xQueueReceive(hang_doi, dong, 0) == pdTRUE) printf("%s\n", dong);
        }

        d = hmi_lay();
        in_tien_trinh();

        if (!da_nhac && ms_now() > 5000 && !d.co_bien) {
            printf("(Chưa nhận được dữ liệu từ màn hình - kiểm tra dây GPIO16/GPIO17/GND)\n");
            da_nhac = true;
        }
#if IN_THONG_KE_TASK
        if (ms_now() - lan_thong_ke >= 30000) { lan_thong_ke = ms_now(); in_thong_ke(); }
#else
        (void)lan_thong_ke;
#endif
    }
}

void nhat_ky_khoi_dong(void)
{
    hang_doi = xQueueCreate(SO_DONG_CHO, DO_DAI_DONG);
    xTaskCreatePinnedToCore(task_nhat_ky, "nhat_ky", STACK_NHAT_KY, NULL, UU_TIEN_NHAT_KY, NULL, CORE_UNG_DUNG);
}
