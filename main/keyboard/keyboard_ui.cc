// main/keyboard/keyboard_ui.cc
// 见 keyboard_ui.h。暗色主题;字体复用小智内置 noto basic 20px(本板配置)。
#include "keyboard_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

LV_FONT_DECLARE(kb_ui_font_20);
LV_FONT_DECLARE(kb_text_font_16);

#define FONT (&kb_ui_font_20)

#define COLOR_BG     lv_color_hex(0x000000)
#define COLOR_CARD   lv_color_hex(0x182136)
#define COLOR_ACCENT lv_color_hex(0x38BDF8)
#define COLOR_OK     lv_color_hex(0x4ADE80)
#define COLOR_WARN   lv_color_hex(0xF59E0B)
#define COLOR_DANGER lv_color_hex(0xF87171)
#define COLOR_TEXT   lv_color_hex(0xE5E7EB)
#define COLOR_DIM    lv_color_hex(0x8CA0B8)

#define UI_W 240
#define UI_H 320

#define KB_UI_PERM_MAX 8

static const char *TAG = "kb_ui";

static lv_obj_t *s_scr;
static lv_obj_t *s_status_label;
static lv_obj_t *s_bat_label;

static lv_obj_t *s_bars[5];
static lv_timer_t *s_bar_timer;
static lv_obj_t *s_rec_time_label;
static int s_rec_secs;
static lv_timer_t *s_rec_timer;

static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;

static lv_obj_t *s_perm_rows[KB_UI_PERM_MAX];
static int s_perm_count;
static bool s_link_up;
static int s_bat = -1;

#define UI_LOCK() do { lvgl_port_lock(-1); } while (0)
#define UI_UNLOCK() do { lvgl_port_unlock(); } while (0)

static lv_obj_t *label_create(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_obj_set_style_text_font(lb, FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lb, color, LV_PART_MAIN);
    lv_label_set_text(lb, text);
    return lb;
}

static void status_bar_add(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, UI_W, 28);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 2);

    s_status_label = label_create(bar, "未连接", COLOR_DIM);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 8, 0);

    s_bat_label = label_create(bar, "--%", COLOR_DIM);
    lv_obj_align(s_bat_label, LV_ALIGN_RIGHT_MID, -8, 0);
}

static void mic_graphic_add(lv_obj_t *parent, bool animated)
{
    lv_obj_t *disc = lv_obj_create(parent);
    lv_obj_set_size(disc, 110, 110);
    lv_obj_align(disc, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(disc, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_color(disc, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(disc, 3, LV_PART_MAIN);
    lv_obj_set_style_border_opa(disc, animated ? LV_OPA_100 : LV_OPA_40, LV_PART_MAIN);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = lv_obj_create(disc);
    lv_obj_set_size(body, 26, 44);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_radius(body, 13, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 0, LV_PART_MAIN);

    lv_obj_t *stem = lv_obj_create(disc);
    lv_obj_set_size(stem, 5, 16);
    lv_obj_align(stem, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_style_radius(stem, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(stem, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(stem, 0, LV_PART_MAIN);

    lv_obj_t *foot = lv_obj_create(disc);
    lv_obj_set_size(foot, 22, 5);
    lv_obj_align(foot, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_radius(foot, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(foot, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(foot, 0, LV_PART_MAIN);
}

static void bar_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    for (int i = 0; i < 5; i++) {
        if (s_bars[i]) {
            lv_obj_set_height(s_bars[i], 12 + (rand() % 44));
        }
    }
}

static void rec_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_rec_secs++;
    if (s_rec_time_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d:%02d", s_rec_secs / 60, s_rec_secs % 60);
        lv_label_set_text(s_rec_time_label, buf);
    }
}

static void stop_rec_anim(void)
{
    if (s_bar_timer) {
        lv_timer_delete(s_bar_timer);
        s_bar_timer = NULL;
    }
    if (s_rec_timer) {
        lv_timer_delete(s_rec_timer);
        s_rec_timer = NULL;
    }
    s_rec_time_label = NULL;
    for (int i = 0; i < 5; i++) s_bars[i] = NULL;
}

static void toast_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
}

static void show_screen(lv_obj_t *scr)
{
    if (s_scr != scr) {
        lv_obj_t *old = s_scr;
        lv_screen_load(scr);
        s_scr = scr;
        if (old != NULL) {
            if (s_toast != NULL && lv_obj_get_screen(s_toast) == old) {
                s_toast = NULL;
                if (s_toast_timer != NULL) {
                    lv_timer_delete(s_toast_timer);
                    s_toast_timer = NULL;
                }
            }
            lv_obj_delete(old);
        }
    }
}

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

esp_err_t kb_ui_init(void)
{
    UI_LOCK();
    kb_ui_show_home(false, -1, 0);
    UI_UNLOCK();
    return ESP_OK;
}

void kb_ui_show_home(bool link_up, int bat, int uni)
{
    UI_LOCK();
    stop_rec_anim();

    lv_obj_t *scr = new_screen();
    status_bar_add(scr);

    lv_obj_t *title = label_create(scr, "语音键盘", COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    // 工作模式指示(Kimi=定位 Kimi Code 窗口并监视权限弹窗;
    // 通用=粘贴到当前焦点窗口)。IDLE 短按 OK 切换。
    lv_obj_t *mode = label_create(scr, uni ? "模式:通用" : "模式:Kimi",
                                  uni ? COLOR_ACCENT : COLOR_DIM);
    lv_obj_align(mode, LV_ALIGN_TOP_MID, 0, 64);

    mic_graphic_add(scr, false);

    lv_obj_t *hint = label_create(scr, "按上键开始语音指令", COLOR_ACCENT);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_t *hint2 = label_create(scr, "长按 OK 返回菜单", COLOR_DIM);
    lv_obj_align(hint2, LV_ALIGN_BOTTOM_MID, 0, -12);

    show_screen(scr);
    kb_ui_set_status(s_link_up, s_bat);
    kb_ui_set_status(link_up, bat);
    UI_UNLOCK();
}

void kb_ui_show_recording(void)
{
    UI_LOCK();
    stop_rec_anim();

    lv_obj_t *scr = new_screen();
    status_bar_add(scr);

    lv_obj_t *title = label_create(scr, "录音中", COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *bars = lv_obj_create(scr);
    lv_obj_remove_style_all(bars);
    lv_obj_set_size(bars, 180, 64);
    lv_obj_align(bars, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bars, 10, LV_PART_MAIN);
    for (int i = 0; i < 5; i++) {
        s_bars[i] = lv_obj_create(bars);
        lv_obj_set_size(s_bars[i], 14, 20);
        lv_obj_set_style_radius(s_bars[i], 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bars[i], COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_bars[i], 0, LV_PART_MAIN);
    }
    s_bar_timer = lv_timer_create(bar_timer_cb, 120, NULL);

    s_rec_secs = 0;
    s_rec_time_label = label_create(scr, "0:00", COLOR_DIM);
    lv_obj_align(s_rec_time_label, LV_ALIGN_CENTER, 0, 40);
    s_rec_timer = lv_timer_create(rec_timer_cb, 1000, NULL);

    lv_obj_t *hint = label_create(scr, "OK 发送      下键 取消", COLOR_TEXT);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    show_screen(scr);
    kb_ui_set_status(s_link_up, s_bat);
    UI_UNLOCK();
}

void kb_ui_set_rec_seconds(int secs)
{
    UI_LOCK();
    s_rec_secs = secs;
    if (s_rec_time_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
        lv_label_set_text(s_rec_time_label, buf);
    }
    UI_UNLOCK();
}

void kb_ui_show_sending(void)
{
    UI_LOCK();
    stop_rec_anim();

    lv_obj_t *scr = new_screen();
    status_bar_add(scr);

    lv_obj_t *title = label_create(scr, "识别中", COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *arc = lv_arc_create(scr);
    lv_obj_set_size(arc, 90, 90);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 10);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 30);
    lv_obj_set_style_arc_color(arc, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);

    lv_obj_t *hint = label_create(scr, "已发送,等待电脑识别", COLOR_DIM);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    show_screen(scr);
    kb_ui_set_status(s_link_up, s_bat);
    UI_UNLOCK();
}

void kb_ui_show_result(const char *text, const char *err)
{
    UI_LOCK();
    stop_rec_anim();

    lv_obj_t *scr = new_screen();
    status_bar_add(scr);

    lv_obj_t *title = label_create(scr, "识别结果", COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, UI_W - 28, 150);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_style_bg_color(card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 10, LV_PART_MAIN);

    lv_obj_t *body = label_create(card, "", err ? COLOR_DANGER : COLOR_TEXT);
    lv_obj_set_style_text_font(body, &kb_text_font_16, LV_PART_MAIN);
    lv_obj_set_width(body, UI_W - 28 - 20);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    if (err) {
        lv_label_set_text(body, err);
    } else if (text && text[0] != '\0') {
        lv_label_set_text(body, text);
    } else {
        lv_obj_set_style_text_color(body, COLOR_DIM, LV_PART_MAIN);
        lv_label_set_text(body, "未识别到文字");
    }

    lv_obj_t *hint;
    if (!err) {
        hint = label_create(scr, "已发送给电脑", COLOR_OK);
    } else {
        hint = label_create(scr, "请重试", COLOR_WARN);
    }
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_t *hint2 = label_create(scr, "OK 返回", COLOR_DIM);
    lv_obj_align(hint2, LV_ALIGN_BOTTOM_MID, 0, -12);

    show_screen(scr);
    kb_ui_set_status(s_link_up, s_bat);
    UI_UNLOCK();
}

void kb_ui_show_perm(const char *text, const char *const *options, int option_count, int sel)
{
    UI_LOCK();
    stop_rec_anim();
    if (option_count > KB_UI_PERM_MAX) option_count = KB_UI_PERM_MAX;

    lv_obj_t *scr = new_screen();
    status_bar_add(scr);

    lv_obj_t *title = label_create(scr, "权限请求", COLOR_WARN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, UI_W - 24, 130);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_color(card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);

    lv_obj_t *body = label_create(card, "", COLOR_TEXT);
    lv_obj_set_style_text_font(body, &kb_text_font_16, LV_PART_MAIN);
    lv_obj_set_width(body, UI_W - 24 - 16);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(body, text ? text : "");

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_W - 24, 34 * option_count + 8);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);

    s_perm_count = option_count;
    for (int i = 0; i < option_count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, UI_W - 24, 30);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_left(row, 10, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_perm_rows[i] = row;

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);

        lv_obj_t *txt = label_create(row, "", COLOR_DIM);
        lv_obj_set_style_text_font(txt, &kb_text_font_16, LV_PART_MAIN);
        lv_label_set_text(txt, options[i] ? options[i] : "");
        lv_obj_align(txt, LV_ALIGN_LEFT_MID, 20, 0);
    }

    lv_obj_t *hint = label_create(scr, "上键 切换    OK 确认    下键 取消", COLOR_TEXT);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    show_screen(scr);
    kb_ui_set_status(s_link_up, s_bat);
    UI_UNLOCK();
    kb_ui_set_perm_selection(sel);
}

void kb_ui_set_perm_selection(int sel)
{
    UI_LOCK();
    for (int i = 0; i < s_perm_count; i++) {
        lv_obj_t *row = s_perm_rows[i];
        if (!row) continue;
        bool on = (i == sel);
        lv_obj_set_style_bg_color(row, on ? COLOR_ACCENT : COLOR_CARD, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, on ? LV_OPA_40 : LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, on ? COLOR_ACCENT : COLOR_DIM, LV_PART_MAIN);

        lv_obj_t *dot = lv_obj_get_child(row, 0);
        if (dot) {
            lv_obj_set_style_bg_color(dot, on ? COLOR_ACCENT : COLOR_DIM, LV_PART_MAIN);
        }
        lv_obj_t *txt = lv_obj_get_child(row, 1);
        if (txt) {
            lv_obj_set_style_text_color(txt, on ? COLOR_TEXT : COLOR_DIM, LV_PART_MAIN);
        }
    }
    UI_UNLOCK();
}

void kb_ui_set_status(bool link_up, int bat)
{
    UI_LOCK();
    s_link_up = link_up;
    s_bat = bat;
    if (s_status_label) {
        lv_label_set_text(s_status_label, link_up ? "已连接" : "未连接");
        lv_obj_set_style_text_color(s_status_label, link_up ? COLOR_OK : COLOR_DIM, LV_PART_MAIN);
    }
    if (s_bat_label) {
        if (bat < 0) {
            lv_label_set_text(s_bat_label, "--%");
        } else {
            char buf[12];
            snprintf(buf, sizeof(buf), "%d%%", bat);
            lv_label_set_text(s_bat_label, buf);
        }
    }
    UI_UNLOCK();
}

void kb_ui_toast(const char *msg)
{
    UI_LOCK();
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (s_toast) {
        lv_obj_delete(s_toast);
    }
    if (s_scr == NULL) {
        UI_UNLOCK();
        return;
    }
    s_toast = lv_obj_create(s_scr);
    lv_obj_set_size(s_toast, UI_W - 40, 36);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_style_radius(s_toast, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x2A3850), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_toast, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lb = label_create(s_toast, msg, COLOR_TEXT);
    lv_obj_center(lb);

    s_toast_timer = lv_timer_create(toast_timer_cb, 2000, NULL);
    UI_UNLOCK();
}
