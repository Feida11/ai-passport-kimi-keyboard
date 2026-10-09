// main/keyboard/root_menu.cc
// 见 root_menu.h。暗色主题,与小智 UI 无关的自包含实现。
#include "root_menu.h"

#include <stdio.h>

#include "boards/common/board.h"
#include "esp_lvgl_port.h"
#include "keyboard_dispatch.h"
#include "lvgl.h"

LV_FONT_DECLARE(kb_ui_font_20);

#define COLOR_BG     lv_color_hex(0x000000)
#define COLOR_CARD   lv_color_hex(0x182136)
#define COLOR_ACCENT lv_color_hex(0x38BDF8)
#define COLOR_TEXT   lv_color_hex(0xE5E7EB)
#define COLOR_DIM    lv_color_hex(0x8CA0B8)

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_xiaozhi_scr;   // 小智 SetupUI 的屏幕(首次进入菜单时捕获)
static lv_obj_t *s_rows[2];
static int s_sel;

static void set_row_style(int idx, bool on)
{
    lv_obj_set_style_bg_color(s_rows[idx], on ? COLOR_ACCENT : COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_rows[idx], on ? LV_OPA_40 : LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_rows[idx], on ? COLOR_ACCENT : COLOR_DIM, LV_PART_MAIN);
    lv_obj_t *txt = lv_obj_get_child(s_rows[idx], 0);
    if (txt) {
        lv_obj_set_style_text_color(txt, on ? COLOR_TEXT : COLOR_DIM, LV_PART_MAIN);
    }
}

static void refresh_sel(void)
{
    set_row_style(0, s_sel == 0);
    set_row_style(1, s_sel == 1);
}

static void add_battery(lv_obj_t *scr)
{
    int level = -1;
    bool charging = false, discharging = false;
    Board::GetInstance().GetBatteryLevel(level, charging, discharging);
    lv_obj_t *bat = lv_label_create(scr);
    lv_obj_set_style_text_font(bat, &kb_ui_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(bat, COLOR_DIM, LV_PART_MAIN);
    if (level < 0) {
        lv_label_set_text(bat, "--%");
    } else {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d%%", level);
        lv_label_set_text(bat, buf);
    }
    lv_obj_align(bat, LV_ALIGN_TOP_RIGHT, -8, 6);
}

boot_mode_t root_menu_show(void)
{
    lvgl_port_lock(-1);

    // 首次进入菜单时,活动屏幕就是小智 SetupUI 的屏幕,捕获备用
    if (s_xiaozhi_scr == NULL) {
        s_xiaozhi_scr = lv_screen_active();
    }

    // 上次菜单留下的屏幕(已被后续界面顶替为非活动)在此清理;
    // 结束时保留当前菜单屏幕由调用方接替加载,避免删除活动屏幕。
    if (s_menu_scr != NULL) {
        lv_obj_delete(s_menu_scr);
        s_menu_scr = NULL;
    }

    s_menu_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_menu_scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_remove_flag(s_menu_scr, LV_OBJ_FLAG_SCROLLABLE);

    add_battery(s_menu_scr);

    lv_obj_t *title = lv_label_create(s_menu_scr);
    lv_obj_set_style_text_font(title, &kb_ui_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_label_set_text(title, "AI Passport");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

    lv_obj_t *sub = lv_label_create(s_menu_scr);
    lv_obj_set_style_text_font(sub, &kb_ui_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, COLOR_DIM, LV_PART_MAIN);
    lv_label_set_text(sub, "选择功能");
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 12, 44);

    static const char *kNames[2] = {"小智 AI", "语音键盘"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = lv_obj_create(s_menu_scr);
        lv_obj_set_size(row, 216, 56);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 92 + i * 68);
        lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_rows[i] = row;

        lv_obj_t *txt = lv_label_create(row);
        lv_obj_set_style_text_font(txt, &kb_ui_font_20, LV_PART_MAIN);
        lv_label_set_text(txt, kNames[i]);
        lv_obj_center(txt);
    }
    s_sel = 0;
    refresh_sel();

    lv_obj_t *hint = lv_label_create(s_menu_scr);
    lv_obj_set_style_text_font(hint, &kb_ui_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, COLOR_DIM, LV_PART_MAIN);
    lv_label_set_text(hint, "上/下 选择    OK 确认");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_screen_load(s_menu_scr);
    lvgl_port_unlock();

    boot_mode_t choice = BOOT_MODE_XIAOZHI;
    bool done = false;
    TickType_t menu_start = xTaskGetTickCount();
    // 重启/模式切换后的按键弹跳保护:菜单刚显示时的按键忽略
    while (!done) {
        kb_key_event_t ev;
        if (!keyboard_get_key(&ev, -1)) {
            continue;
        }
        if ((xTaskGetTickCount() - menu_start) < pdMS_TO_TICKS(600)) {
            continue;
        }
        if (ev.ev != KB_EV_CLICK) {
            continue;
        }
        lvgl_port_lock(-1);
        switch (ev.key) {
        case KB_KEY_UP:
            s_sel = (s_sel + 1) % 2; // 上键向上滚动(两项,等价于翻转)
            refresh_sel();
            break;
        case KB_KEY_DOWN:
            s_sel = (s_sel + 1) % 2;
            refresh_sel();
            break;
        case KB_KEY_OK:
            choice = (s_sel == 0) ? BOOT_MODE_XIAOZHI : BOOT_MODE_KEYBOARD;
            done = true;
            break;
        default:
            break;
        }
        lvgl_port_unlock();
    }

    if (choice == BOOT_MODE_XIAOZHI) {
        // 切回小智界面:加载其屏幕并删除菜单屏幕
        if (s_xiaozhi_scr != NULL) {
            lv_screen_load(s_xiaozhi_scr);
        }
        if (s_menu_scr != NULL) {
            lv_obj_delete(s_menu_scr);
            s_menu_scr = NULL;
        }
    }
    // 键盘模式:菜单屏幕保留(仍活动),键盘 UI 会接替加载;下次进菜单时清理。
    return choice;
}
