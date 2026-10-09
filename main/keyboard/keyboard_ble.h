// main/remote_ble.h
// Kimi 语音遥控器 BLE 外设:NimBLE GATT 服务,连接管理,通知发送。
// 协议见 remote_protocol.h。回调均在 NimBLE host 任务上下文执行,只能入队轻量事件。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KB_BLE_EVT_LINK_UP = 0,     // 电脑已连接(可开始发送)
    KB_BLE_EVT_LINK_DOWN,       // 连接断开(原因见 reason)
    KB_BLE_EVT_CTRL_LINE,       // 收到一行控制通道 JSON(借用缓冲,回调返回后失效)
} kb_ble_evt_type_t;

typedef struct {
    kb_ble_evt_type_t type;
    union {
        struct {
            int reason;             // 断开原因(ble_gap 的 status/reason)
        } link_down;
        struct {
            const char *data;       // 行内容(无换行)
            size_t len;
        } ctrl_line;
    } data;
} kb_ble_evt_t;

typedef void (*kb_ble_evt_cb_t)(const kb_ble_evt_t *evt, void *ctx);

// 初始化并启动广播。cb 在 host 任务上下文调用。
esp_err_t kb_ble_start(kb_ble_evt_cb_t cb, void *ctx);

// 停止并释放 BLE(幂等)。
esp_err_t kb_ble_stop(void);

bool kb_ble_is_connected(void);

// 经控制通道发送一条完整消息(自动分片)。仅连接状态有效;返回 ESP_OK 表示已排入发送队列。
esp_err_t kb_ble_send_ctrl(const char *line, size_t len);

// 经音频通道发送一帧(二进制,见 remote_protocol.h 音频帧格式)。
esp_err_t kb_ble_send_audio(const uint8_t *frame, size_t len);

// 诊断辅助
uint16_t s_att_mtu_log(void);
int s_audio_subscribed_log(void);

// 当前连接实时 ATT MTU(按连接句柄现查,不受全局缓存污染)
uint16_t kb_ble_att_mtu(void);
// 本连接未做过 MTU 交换时主动发起一次(受益下次录音)
void kb_ble_kick_mtu_exchange(void);

#ifdef __cplusplus
}
#endif
