// main/keyboard/keyboard_dispatch.h
// 根菜单/语音键盘模式与板级按键之间的轻量分发。
// 板级按键回调按当前模式路由:小智模式走原行为,菜单/键盘模式把按键事件
// 投递到这里,由根菜单或语音键盘模式消费。跨模块共享,避免 core 依赖具体板类。
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KB_KEY_UP = 0,
    KB_KEY_DOWN,
    KB_KEY_OK,
} kb_key_t;

typedef enum {
    KB_EV_CLICK = 0,
    KB_EV_LONG,
} kb_ev_t;

typedef enum {
    OP_MODE_MENU = 0,   // 根菜单(开机默认)
    OP_MODE_KEYBOARD,   // 语音键盘
    OP_MODE_XIAOZHI,    // 小智 AI
} op_mode_t;

typedef struct {
    kb_key_t key;
    kb_ev_t ev;
} kb_key_event_t;

// 初始化分发队列(幂等)。返回队列句柄。
QueueHandle_t keyboard_dispatch_init(void);

// 取按键事件;无事件时阻塞 timeout_ms(-1 表示永远等待)。收到返回 true。
bool keyboard_get_key(kb_key_event_t *out, int timeout_ms);

// 投递按键事件(按键回调上下文,仅入队)。
void keyboard_post_key(kb_key_t key, kb_ev_t ev);

// 当前运行模式(全局单例,由 Application 设置)。
op_mode_t keyboard_op_mode(void);
void keyboard_set_op_mode(op_mode_t mode);

#ifdef __cplusplus
}
#endif
