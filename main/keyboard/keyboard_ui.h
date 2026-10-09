// main/keyboard/keyboard_ui.h
// 语音键盘模式的 LVGL 界面。内部自行获取 LVGL 锁。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t kb_ui_init(void);

void kb_ui_show_home(bool link_up, int bat, int uni);
void kb_ui_show_recording(void);
void kb_ui_set_rec_seconds(int secs);
void kb_ui_show_sending(void);
void kb_ui_show_result(const char *text, const char *err);
void kb_ui_show_perm(const char *text, const char *const *options, int option_count, int sel);
void kb_ui_set_perm_selection(int sel);
void kb_ui_set_status(bool link_up, int bat);
void kb_ui_toast(const char *msg);

#ifdef __cplusplus
}
#endif
