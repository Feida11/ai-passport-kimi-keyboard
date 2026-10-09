// main/remote_state.h
// Kimi 语音遥控器应用状态机(纯 C,无 ESP-IDF/LVGL 依赖,主机可测)。
//
// 状态:IDLE(主页) / RECORDING(录音) / SENDING(已发送待识别) / RESULT(识别结果)
//       / PERM(权限确认,模态,记住下层状态)。
// 事件:按键单击、OK 长按、录音超时、BLE 连接/断开、ASR 结果、权限请求/取消/界面超时。
// 输出:动作位图(开始/停止/取消录音、发送权限响应、重启 BLE、UI 刷新、蜂鸣)。
// 文本指针均为借用(事件方持有内存),直到下一次同类事件,调用方不得释放。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RS_ST_IDLE = 0,
    RS_ST_RECORDING,
    RS_ST_SENDING,
    RS_ST_RESULT,
    RS_ST_PERM,
} rs_state_t;

typedef enum {
    RS_EV_NONE = 0,
    RS_EV_KEY_UP,     // 上键单击
    RS_EV_KEY_DOWN,   // 下键单击
    RS_EV_KEY_OK,     // 确认键单击
    RS_EV_KEY_OK_LONG,// 确认键长按
    RS_EV_REC_TIMEOUT,// 录音达到最长时长
    RS_EV_LINK_UP,    // BLE 已连接
    RS_EV_LINK_DOWN,  // BLE 断开
    RS_EV_ASR_RESULT, // ASR 结果(id+text 或 id+err)
    RS_EV_PERM_REQ,   // 新的权限请求(id+text+option_count+timeout_s)
    RS_EV_PERM_CANCEL,// 电脑撤销权限请求(id)
    RS_EV_PERM_TIMEOUT,// 权限界面超时
    RS_EV_RESULT_DONE,// 结果页停留结束(应用层定时器触发)
} rs_event_type_t;

typedef enum {
    RS_ACT_NONE = 0,
    RS_ACT_START_RECORD = 1u << 0,     // 开始录音(record_id 有效)
    RS_ACT_STOP_RECORD_SEND = 1u << 1, // 停止录音并发送(record_id 有效)
    RS_ACT_CANCEL_RECORD = 1u << 2,    // 丢弃录音
    RS_ACT_SEND_PERM_RESP = 1u << 3,   // 发送权限响应(perm_id/perm_choice,choice=-1 表取消)
    RS_ACT_BLE_RESTART = 1u << 4,      // 重启 BLE 广播
    RS_ACT_UI_REDRAW = 1u << 5,        // 界面需要刷新
    RS_ACT_BEEP = 1u << 6,             // 提示音
} rs_action_t;

typedef struct {
    rs_event_type_t type;
    int32_t id;           // ASR/权限请求编号
    const char *text;     // ASR 文本 / 权限请求文本(借用)
    const char *err;      // ASR 错误(借用)
    int32_t option_count; // 权限请求选项数
    int32_t timeout_s;    // 权限请求超时秒数
} rs_event_t;

typedef struct {
    uint32_t actions;
    int32_t record_id;
    int32_t perm_id;
    int32_t perm_choice;
} rs_result_t;

typedef struct {
    rs_state_t state;
    bool link_up;
    int32_t next_record_id;
    int32_t current_record_id;   // RECORDING/SENDING 时的录音编号;-1 无
    const char *last_asr_text;   // 借用
    const char *last_asr_err;    // 借用
    const char *perm_text;       // 借用
    int32_t perm_id;
    int32_t perm_sel;            // 当前选中选项下标
    int32_t perm_option_count;
    int32_t perm_timeout_s;
    rs_state_t perm_return;      // PERM 退出后回到的状态
} rs_sm_t;

void rs_init(rs_sm_t *sm);
rs_result_t rs_step(rs_sm_t *sm, const rs_event_t *ev);

// 只读访问器(UI 层取显示内容)
rs_state_t rs_state_get(const rs_sm_t *sm);
bool rs_link_up(const rs_sm_t *sm);
int32_t rs_record_id(const rs_sm_t *sm);        // 当前/最近一条录音编号
const char *rs_last_asr_text(const rs_sm_t *sm);
const char *rs_last_asr_err(const rs_sm_t *sm);
int32_t rs_perm_id(const rs_sm_t *sm);
const char *rs_perm_text(const rs_sm_t *sm);
int32_t rs_perm_selection(const rs_sm_t *sm);
int32_t rs_perm_option_count(const rs_sm_t *sm);

#ifdef __cplusplus
}
#endif
