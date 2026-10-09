// main/keyboard/root_menu.h
// 开机根菜单:在小智/语音键盘之间选择。阻塞至用户确认。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOOT_MODE_XIAOZHI = 0,
    BOOT_MODE_KEYBOARD,
} boot_mode_t;

// 显示根菜单并阻塞等待选择(UP/DOWN 移动,OK 确认)。返回所选模式。
// 调用前要求:显示与按键已初始化,当前模式为 OP_MODE_MENU。
boot_mode_t root_menu_show(void);

#ifdef __cplusplus
}
#endif
