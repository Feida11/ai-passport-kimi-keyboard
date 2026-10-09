// main/keyboard/keyboard_mode.h
// 语音键盘模式:阻塞运行,直到用户长按 OK 请求退回根菜单。
// 使用前先 keyboard_set_op_mode(OP_MODE_KEYBOARD) 并确保音频服务已 Initialize。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void keyboard_mode_run(void);

#ifdef __cplusplus
}
#endif
