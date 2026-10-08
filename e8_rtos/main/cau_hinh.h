/*
 * cau_hinh.h - Cấu hình chung: chân, thông số, ưu tiên và stack của các task
 */
#pragma once

// Màn hình HMI
#define HMI_UART            UART_NUM_2
#define HMI_RX_PIN          17          // nối TXA của màn hình
#define HMI_TX_PIN          16          // nối RXB của màn hình
#define HMI_BAUD            115200
#define HMI_CHU_KY_HOI_MS   200         // cứ 200 ms hỏi màn hình 1 lần (luân phiên trang / biến)
#define HMI_MAT_KET_NOI_MS  2000        // quá 2 s không nhận được dữ liệu -> coi như mất màn hình

// CM8 (chân đặt trong cm8.h) 
#define MODE_CHAY           3           // Mode khi động cơ chạy (CHƯA có giá trị chính thức -> tạm 3)
#define RATIO_CHAY          9           // Ratio (code CM8 lấy 9 làm chuẩn)
#define GIOI_HAN_DONG       370         // 3,70 A
#define TOC_DO_MAX          32          // tốc độ CM8 0..32 (CM8 chỉ dừng đúng ở số CHẴN)
#define CHO_CHUAN_BI_MS     3000        // chờ CM8 về trạng thái 0 trước lần chạy đầu (tối đa)
#define DUNG_MAN_HINH_KHI_LOI 1         // 1: CM8 lỗi / mất kết nối khi đang tập -> ghi 0x1001 = 0

// Xung 
#define XUNG_BIEN_DO        2           // dao động ± 2 mức
#define XUNG_BUOC_MS        300         // mỗi 300 ms đổi 1 mức

#define DIA_CHI_HIEN_THI    0x100B      // biến cường độ hiển thị trên đồng hồ
#define LAM_MOI_HIEN_THI_MS 1000

// Task 
// Ưu tiên: số càng lớn càng được chạy trước. Task an toàn (điều khiển) cao nhất,
// in log thấp nhất để không bao giờ làm chậm phần điều khiển.
#define UU_TIEN_DIEU_KHIEN  6
#define UU_TIEN_CM8         5
#define UU_TIEN_HMI_NHAN    5
#define UU_TIEN_HMI_HOI     4
#define UU_TIEN_NHAT_KY     2

// Stack: trong ESP-IDF tính bằng BYTE (FreeRTOS gốc tính bằng word)
#define STACK_DIEU_KHIEN    4096
#define STACK_CM8           4096
#define STACK_HMI           4096
#define STACK_NHAT_KY       4096

#define CORE_UNG_DUNG       1           // chạy các task trên core 1 (core 0 để hệ thống)

#define CHU_KY_DIEU_KHIEN_MS 50
#define WDT_TIMEOUT_MS      3000        // task nào không báo "còn sống" quá 3 s -> khởi động lại

// Gỡ lỗi 
#define IN_THONG_KE_TASK    1           // 1: mỗi 30 s in stack còn trống của từng task
#define THU_TREO_TASK       0           // 1: sau 20 s cố tình làm task "hmi_hoi" bị treo
                                        //    để thử watchdog (NHỚ ĐỂ 0 khi dùng thật)
