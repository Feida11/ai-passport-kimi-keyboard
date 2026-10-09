// main/remote_state.c
// 见 remote_state.h。
#include "remote_state.h"

#include <string.h>

#define RS_PERM_NONE (-1)

void rs_init(rs_sm_t *sm)
{
    memset(sm, 0, sizeof(*sm));
    sm->state = RS_ST_IDLE;
    sm->link_up = false;
    sm->next_record_id = 1;
    sm->current_record_id = -1;
    sm->perm_id = RS_PERM_NONE;
    sm->perm_sel = 0;
    sm->perm_option_count = 0;
    sm->perm_timeout_s = 0;
    sm->perm_return = RS_ST_IDLE;
}

static rs_result_t act(uint32_t actions, int32_t record_id, int32_t perm_id, int32_t perm_choice)
{
    rs_result_t r;
    r.actions = actions;
    r.record_id = record_id;
    r.perm_id = perm_id;
    r.perm_choice = perm_choice;
    return r;
}

// 进入权限模态:记录下层状态、存借用文本、默认选中第一项
static void perm_enter(rs_sm_t *sm, const rs_event_t *ev)
{
    if (sm->state != RS_ST_PERM) {
        sm->perm_return = sm->state;
    }
    sm->state = RS_ST_PERM;
    sm->perm_id = ev->id;
    sm->perm_text = ev->text;
    sm->perm_option_count = ev->option_count;
    sm->perm_sel = 0;
    sm->perm_timeout_s = ev->timeout_s;
}

// 退出权限模态并回到下层状态(下层若是录音/发送且链路已断,由调用路径另行处理)
static void perm_leave(rs_sm_t *sm)
{
    sm->state = sm->perm_return;
    sm->perm_id = RS_PERM_NONE;
    sm->perm_text = NULL;
    sm->perm_option_count = 0;
}

rs_result_t rs_step(rs_sm_t *sm, const rs_event_t *ev)
{
    if (ev == NULL) {
        return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);
    }

    switch (ev->type) {
    case RS_EV_LINK_UP:
        sm->link_up = true;
        return act(RS_ACT_UI_REDRAW, sm->current_record_id, RS_PERM_NONE, 0);

    case RS_EV_LINK_DOWN:
        sm->link_up = false;
        if (sm->state == RS_ST_RECORDING) {
            // 录音中的链路断开:丢弃录音;若在权限模态内,下层也一并取消
            sm->current_record_id = -1;
            sm->state = RS_ST_IDLE;
            sm->perm_id = RS_PERM_NONE;
            sm->perm_text = NULL;
            return act(RS_ACT_CANCEL_RECORD | RS_ACT_UI_REDRAW, -1, RS_PERM_NONE, 0);
        }
        if (sm->state == RS_ST_SENDING) {
            sm->current_record_id = -1;
            sm->state = RS_ST_IDLE;
            return act(RS_ACT_UI_REDRAW, -1, RS_PERM_NONE, 0);
        }
        if (sm->state == RS_ST_PERM) {
            bool recording_under = (sm->perm_return == RS_ST_RECORDING);
            sm->state = RS_ST_IDLE;
            sm->perm_id = RS_PERM_NONE;
            sm->perm_text = NULL;
            if (recording_under) {
                sm->current_record_id = -1;
                return act(RS_ACT_CANCEL_RECORD | RS_ACT_UI_REDRAW, -1, RS_PERM_NONE, 0);
            }
            return act(RS_ACT_UI_REDRAW, -1, RS_PERM_NONE, 0);
        }
        return act(RS_ACT_UI_REDRAW, sm->current_record_id, RS_PERM_NONE, 0);

    case RS_EV_KEY_UP:
        if (sm->state == RS_ST_IDLE) {
            if (!sm->link_up) {
                return act(RS_ACT_UI_REDRAW | RS_ACT_BEEP, -1, RS_PERM_NONE, 0);
            }
            sm->current_record_id = sm->next_record_id++;
            sm->state = RS_ST_RECORDING;
            return act(RS_ACT_START_RECORD | RS_ACT_UI_REDRAW | RS_ACT_BEEP,
                       sm->current_record_id, RS_PERM_NONE, 0);
        }
        if (sm->state == RS_ST_PERM && sm->perm_option_count > 0) {
            sm->perm_sel = (sm->perm_sel + 1) % sm->perm_option_count;
            return act(RS_ACT_UI_REDRAW, -1, sm->perm_id, 0);
        }
        if (sm->state == RS_ST_RESULT) {
            sm->state = RS_ST_IDLE;
            return act(RS_ACT_UI_REDRAW, -1, RS_PERM_NONE, 0);
        }
        return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);

    case RS_EV_KEY_DOWN:
        if (sm->state == RS_ST_RECORDING) {
            int32_t rid = sm->current_record_id;
            sm->current_record_id = -1;
            sm->state = RS_ST_IDLE;
            return act(RS_ACT_CANCEL_RECORD | RS_ACT_UI_REDRAW | RS_ACT_BEEP,
                       rid, RS_PERM_NONE, 0);
        }
        if (sm->state == RS_ST_PERM) {
            int32_t pid = sm->perm_id;
            perm_leave(sm);
            return act(RS_ACT_SEND_PERM_RESP | RS_ACT_UI_REDRAW | RS_ACT_BEEP,
                       -1, pid, -1);
        }
        if (sm->state == RS_ST_RESULT) {
            sm->state = RS_ST_IDLE;
            return act(RS_ACT_UI_REDRAW, -1, RS_PERM_NONE, 0);
        }
        return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);

    case RS_EV_KEY_OK:
        if (sm->state == RS_ST_RECORDING) {
            int32_t rid = sm->current_record_id;
            sm->state = RS_ST_SENDING;
            return act(RS_ACT_STOP_RECORD_SEND | RS_ACT_UI_REDRAW,
                       rid, RS_PERM_NONE, 0);
        }
        if (sm->state == RS_ST_SENDING) {
            // 识别中按确认:不打断,仅忽略
            return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);
        }
        if (sm->state == RS_ST_RESULT) {
            sm->state = RS_ST_IDLE;
            return act(RS_ACT_UI_REDRAW, -1, RS_PERM_NONE, 0);
        }
        if (sm->state == RS_ST_PERM) {
            int32_t pid = sm->perm_id;
            int32_t sel = sm->perm_sel;
            perm_leave(sm);
            return act(RS_ACT_SEND_PERM_RESP | RS_ACT_UI_REDRAW | RS_ACT_BEEP,
                       -1, pid, sel);
        }
        return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);

    case RS_EV_KEY_OK_LONG:
        // 任意界面长按 OK 重启 BLE(权限模态除外)
        if (sm->state != RS_ST_PERM) {
            return act(RS_ACT_BLE_RESTART | RS_ACT_UI_REDRAW | RS_ACT_BEEP, -1, RS_PERM_NONE, 0);
        }
        return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);

    case RS_EV_REC_TIMEOUT:
        if (sm->state == RS_ST_RECORDING) {
            int32_t rid = sm->current_record_id;
            sm->state = RS_ST_SENDING;
            return act(RS_ACT_STOP_RECORD_SEND | RS_ACT_UI_REDRAW, rid, RS_PERM_NONE, 0);
        }
        return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);

    case RS_EV_ASR_RESULT:
        if (sm->state == RS_ST_SENDING && ev->id == sm->current_record_id) {
            sm->state = RS_ST_RESULT;
            sm->last_asr_text = ev->text;
            sm->last_asr_err = ev->err;
            sm->current_record_id = -1;
            return act(RS_ACT_UI_REDRAW | RS_ACT_BEEP, -1, RS_PERM_NONE, 0);
        }
        return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);

    case RS_EV_RESULT_DONE:
        if (sm->state == RS_ST_RESULT) {
            sm->state = RS_ST_IDLE;
            return act(RS_ACT_UI_REDRAW, -1, RS_PERM_NONE, 0);
        }
        return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);

    case RS_EV_PERM_REQ:
        if (ev->text == NULL || ev->option_count <= 0) {
            return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);
        }
        if (sm->state == RS_ST_PERM) {
            // 顶替旧请求:先按取消应答旧请求,再进入新请求
            int32_t old = sm->perm_id;
            rs_state_t ret = sm->perm_return;
            perm_enter(sm, ev);
            sm->perm_return = ret;
            return act(RS_ACT_SEND_PERM_RESP | RS_ACT_UI_REDRAW | RS_ACT_BEEP,
                       -1, old, -1);
        }
        perm_enter(sm, ev);
        return act(RS_ACT_UI_REDRAW | RS_ACT_BEEP, -1, sm->perm_id, 0);

    case RS_EV_PERM_CANCEL:
        if (sm->state == RS_ST_PERM && ev->id == sm->perm_id) {
            perm_leave(sm);
            return act(RS_ACT_UI_REDRAW | RS_ACT_BEEP, -1, RS_PERM_NONE, 0);
        }
        return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);

    case RS_EV_PERM_TIMEOUT:
        if (sm->state == RS_ST_PERM) {
            int32_t pid = sm->perm_id;
            perm_leave(sm);
            return act(RS_ACT_SEND_PERM_RESP | RS_ACT_UI_REDRAW, -1, pid, -1);
        }
        return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);

    default:
        return act(RS_ACT_NONE, -1, RS_PERM_NONE, 0);
    }
}

rs_state_t rs_state_get(const rs_sm_t *sm) { return sm->state; }
bool rs_link_up(const rs_sm_t *sm) { return sm->link_up; }
int32_t rs_record_id(const rs_sm_t *sm) { return sm->current_record_id; }
const char *rs_last_asr_text(const rs_sm_t *sm) { return sm->last_asr_text; }
const char *rs_last_asr_err(const rs_sm_t *sm) { return sm->last_asr_err; }
int32_t rs_perm_id(const rs_sm_t *sm) { return sm->perm_id; }
const char *rs_perm_text(const rs_sm_t *sm) { return sm->perm_text; }
int32_t rs_perm_selection(const rs_sm_t *sm) { return sm->perm_sel; }
int32_t rs_perm_option_count(const rs_sm_t *sm) { return sm->perm_option_count; }
