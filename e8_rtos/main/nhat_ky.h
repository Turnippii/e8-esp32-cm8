/*
 * nhat_ky.h - Task in log ra Monitor (ưu tiên thấp nhất)
 *
 * Các task khác KHÔNG printf trực tiếp mà gửi chuỗi vào hàng đợi (queue) bằng nhat_ky_gui().
 * Hàm này không bao giờ chờ: hàng đợi đầy thì bỏ dòng log, task điều khiển không bị chậm.
 * Task nhật ký cũng tự in tiến trình buổi tập (trang, bắt đầu/tạm dừng, đếm giờ...).
 */
#pragma once

void nhat_ky_khoi_dong(void);
void nhat_ky_gui(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
