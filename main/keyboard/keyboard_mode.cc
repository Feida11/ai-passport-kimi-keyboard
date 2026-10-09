// main/keyboard/keyboard_mode.cc
// 见 keyboard_mode.h。从 ai-passport 的 remote_app.c 移植:
// 事件队列 + 纯 C 状态机 + 定时器 + BLE 协议,音频改走 AudioService::ReadAudioData。
#include "keyboard_mode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "application.h"
#include "audio/audio_service.h"
#include "audio_codec.h"
#include "boards/common/board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "keyboard_ble.h"
#include "keyboard_dispatch.h"
#include "keyboard_ui.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "remote_adpcm.h"
#include "remote_protocol.h"
#include "remote_state.h"

static const char *TAG = "kb_mode";

#define KB_QUEUE_DEPTH 8
#define CTRL_LINE_CAP 480
#define REC_MAX_MS 30000
#define RESULT_HOLD_MS 6000
#define PERM_DEFAULT_TIMEOUT_S 60
#define MIC_TASK_STACK 4096

typedef enum {
    KBAPP_EV_BLE_LINK_UP = 0,
    KBAPP_EV_BLE_LINK_DOWN,
    KBAPP_EV_CTRL_LINE,
    KBAPP_EV_REC_TIMEOUT,
    KBAPP_EV_ASR_TIMEOUT,
    KBAPP_EV_PERM_TIMEOUT,
    KBAPP_EV_RESULT_TIMEOUT,
    KBAPP_EV_BATTERY_POLL,
} kbapp_ev_type_t;

typedef struct {
    kbapp_ev_type_t type;
    char line[CTRL_LINE_CAP];
} kbapp_event_t;

static QueueHandle_t s_queue;
static rs_sm_t s_sm;
static TimerHandle_t s_rec_timer;
static TimerHandle_t s_asr_timer;
static TimerHandle_t s_perm_timer;
static TimerHandle_t s_result_timer;
static TimerHandle_t s_bat_timer;
static volatile bool s_mic_run;
static TaskHandle_t s_mic_task;
static volatile bool s_exit;

static char s_asr_text[320];
static char s_asr_err[160];
// 键盘工作模式:0=Kimi(定位 Kimi Code 窗口+权限监视),1=通用(粘贴到焦点窗口)
static int s_uni;

static void send_ctrl(const char *line); // 定义在下方,send_mode 需用

static void load_mode(void)
{
    nvs_handle_t h;
    if (nvs_open("kb_cfg", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "uni", &v) == ESP_OK) s_uni = v ? 1 : 0;
        nvs_close(h);
    }
}

static void save_mode(void)
{
    nvs_handle_t h;
    if (nvs_open("kb_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "uni", s_uni ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void send_mode(void)
{
    char buf[48];
    int n = rp_build_mode(buf, sizeof(buf), s_uni);
    if (n > 0) {
        for (int i = 0; i < 3; i++) {
            send_ctrl(buf);
            if (i < 2) vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void post_event(const kbapp_event_t *ev)
{
    if (s_queue && xQueueSend(s_queue, ev, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, drop %d", ev->type);
    }
}

static void rec_timer_cb(TimerHandle_t t) { (void)t; kbapp_event_t e = {.type = KBAPP_EV_REC_TIMEOUT}; post_event(&e); }
static void asr_timer_cb(TimerHandle_t t) { (void)t; kbapp_event_t e = {.type = KBAPP_EV_ASR_TIMEOUT}; post_event(&e); }
static void perm_timer_cb(TimerHandle_t t) { (void)t; kbapp_event_t e = {.type = KBAPP_EV_PERM_TIMEOUT}; post_event(&e); }
static void result_timer_cb(TimerHandle_t t) { (void)t; kbapp_event_t e = {.type = KBAPP_EV_RESULT_TIMEOUT}; post_event(&e); }
static void bat_timer_cb(TimerHandle_t t) { (void)t; kbapp_event_t e = {.type = KBAPP_EV_BATTERY_POLL}; post_event(&e); }

// ---------------------------------------------------------------------------
// BLE 回调(host 任务,仅复制并入队)
// ---------------------------------------------------------------------------
static void ble_evt_cb(const kb_ble_evt_t *evt, void *ctx)
{
    (void)ctx;
    kbapp_event_t e = {};
    switch (evt->type) {
    case KB_BLE_EVT_LINK_UP:
        e.type = KBAPP_EV_BLE_LINK_UP;
        break;
    case KB_BLE_EVT_LINK_DOWN:
        e.type = KBAPP_EV_BLE_LINK_DOWN;
        break;
    case KB_BLE_EVT_CTRL_LINE: {
        e.type = KBAPP_EV_CTRL_LINE;
        size_t len = evt->data.ctrl_line.len;
        if (len >= sizeof(e.line)) len = sizeof(e.line) - 1;
        memcpy(e.line, evt->data.ctrl_line.data, len);
        e.line[len] = '\0';
        break;
    }
    default:
        return;
    }
    post_event(&e);
}

// ---------------------------------------------------------------------------
// 动作执行
// ---------------------------------------------------------------------------
static int read_battery(void)
{
    int level = -1;
    bool charging = false, discharging = false;
    Board::GetInstance().GetBatteryLevel(level, charging, discharging);
    return level;
}

static void send_ctrl(const char *line)
{
    esp_err_t err = kb_ble_send_ctrl(line, strlen(line));
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "send ctrl failed: %s", esp_err_to_name(err));
    }
}

static void send_rec_stop(int32_t id, const char *reason)
{
    char buf[96];
    int n = rp_build_rec_stop(buf, sizeof(buf), id, reason);
    if (n <= 0) return;
    // notify 可能丢包:连发 3 遍,电脑端按 id 去重
    for (int i = 0; i < 3; i++) {
        send_ctrl(buf);
        if (i < 2) vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void send_perm_resp(int32_t id, int32_t choice)
{
    char buf[96];
    int n = rp_build_perm_resp(buf, sizeof(buf), id, choice);
    if (n > 0) send_ctrl(buf);
}

static void arm_timer(TimerHandle_t timer, uint32_t ms)
{
    if (timer) {
        xTimerChangePeriod(timer, pdMS_TO_TICKS(ms), pdMS_TO_TICKS(100));
        xTimerStart(timer, pdMS_TO_TICKS(100));
    }
}

static void disarm_timer(TimerHandle_t timer)
{
    if (timer) xTimerStop(timer, pdMS_TO_TICKS(100));
}

static void refresh_ui(void)
{
    int bat = read_battery();
    kb_ui_set_status(rs_link_up(&s_sm), bat);
    switch (rs_state_get(&s_sm)) {
    case RS_ST_IDLE:
        kb_ui_show_home(rs_link_up(&s_sm), bat, s_uni);
        break;
    case RS_ST_RECORDING:
        kb_ui_show_recording();
        break;
    case RS_ST_SENDING:
        kb_ui_show_sending();
        break;
    case RS_ST_RESULT:
        // s_asr_err 是 static 数组、永不 NULL;空串必须传 NULL,
        // 否则 UI 恒走错误分支把识别文本顶掉(屏幕空白/残留旧错误)
        kb_ui_show_result(s_asr_text, s_asr_err[0] ? s_asr_err : NULL);
        break;
    case RS_ST_PERM:
        break;
    default:
        break;
    }
}

static void apply_result(const rs_result_t *r)
{
    if (r->actions & RS_ACT_START_RECORD) {
        // 先通知电脑录音开始(携带录音编号),再启动麦克风采集
        char buf[96];
        // 与 mic 任务一致的采样率判定:让电脑知道实际采集率,避免按帧长猜错
        // 用当前连接实时 MTU(s_att_mtu 全局缓存会被幽灵连接污染回 23)
        int rec_rate = (kb_ble_att_mtu() >= 96) ? 16000 : 8000;
        int n = rp_build_rec_start(buf, sizeof(buf), r->record_id, rec_rate);
        if (n > 0) {
            ESP_LOGI(TAG, "send rec_start id=%ld len=%d sr=%d", (long)r->record_id, n, rec_rate);
            // notify 可能丢包:连发 3 遍,电脑端按 id 去重
            for (int i = 0; i < 3; i++) {
                send_ctrl(buf);
                if (i < 2) vTaskDelay(pdMS_TO_TICKS(100));
            }
        } else {
            ESP_LOGE(TAG, "build rec_start failed");
        }
        // 音频服务空闲超时会关闭 codec 输入;录音前显式唤醒
        Board::GetInstance().GetAudioCodec()->EnableInput(true);
        s_mic_run = true; // 录音任务立即开始
        arm_timer(s_rec_timer, REC_MAX_MS);
    }
    if (r->actions & RS_ACT_STOP_RECORD_SEND) {
        disarm_timer(s_rec_timer);
        s_mic_run = false;
        send_rec_stop(r->record_id, "ok");
        // ASR 超时只在 SENDING 阶段有意义:发送后武装 30s,收到结果/断开时解除
        arm_timer(s_asr_timer, 30000);
    }
    if (r->actions & RS_ACT_CANCEL_RECORD) {
        disarm_timer(s_rec_timer);
        s_mic_run = false;
        send_rec_stop(r->record_id, "cancel");
    }
    if (r->actions & RS_ACT_SEND_PERM_RESP) {
        disarm_timer(s_perm_timer);
        send_perm_resp(r->perm_id, r->perm_choice);
    }
    if (r->actions & RS_ACT_UI_REDRAW) {
        refresh_ui();
    }
    // RS_ACT_BEEP / RS_ACT_BLE_RESTART 在本模式无对应动作(小智工程无 beep;
    // 重连由 BLE 层断连自动重播完成)
}

// ---------------------------------------------------------------------------
// 麦克风任务:16kHz PCM -> ADPCM -> BLE
// ---------------------------------------------------------------------------
static void mic_task(void *arg)
{
    (void)arg;
    std::vector<int16_t> pcm(RP_ADPCM_SAMPLES_PER_FRAME);
    uint8_t adpcm[RP_ADPCM_STATE_LEN + RP_ADPCM_SAMPLES_PER_FRAME / 2];
    uint8_t frame[RP_AUDIO_HDR_LEN + sizeof(adpcm)];
    int16_t prev = 0;
    int16_t index = 0;
    uint16_t seq = 0;

    while (true) {
        if (!s_mic_run) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        prev = 0;
        index = 0;
        seq = 0;

        auto &asr = Application::GetInstance().GetAudioService();
        // MTU 充足(≥96,notify 载荷 20+ ADPCM 字节)用 16kHz;否则降 8kHz 减半吞吐,
        // 保证 MTU 23 + 15ms 连接间隔下可传。WAV 头携带实际采样率,电脑端重采样到 16k。
        int mtu = (int)kb_ble_att_mtu();
        if (mtu < 96) {
            // 本连接未协商大 MTU(如 Windows 重连未交换):主动发起交换,
            // 本次先降级跑通,下次录音即可 16kHz
            kb_ble_kick_mtu_exchange();
        }
        int rate = (mtu >= 96) ? 16000 : 8000;
        int samples = (mtu - 13) * 2;
        int max_samples = rate / 100; // 10ms
        if (samples < 20) samples = 20;
        if (samples > max_samples) samples = max_samples;
        ESP_LOGI(TAG, "mic start: mtu=%d rate=%d", mtu, rate);
        uint32_t frame_count = 0;
        uint32_t send_failed = 0;
        while (s_mic_run) {
            if (!asr.ReadAudioData(pcm, rate, samples)) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            frame_count++;
            rp_adpcm_encode(pcm.data(), samples, &prev, &index, adpcm);
            size_t adpcm_len = RP_ADPCM_STATE_LEN + ((size_t)samples + 1) / 2;
            size_t len = rp_audio_pack(frame, seq, 0, adpcm, adpcm_len);
            seq++;
            if (kb_ble_is_connected() &&
                kb_ble_send_audio(frame, len) != ESP_OK) {
                send_failed++;
            }
        }
        ESP_LOGI(TAG, "mic stop: %lu frames, %lu send failed",
                 (unsigned long)frame_count, (unsigned long)send_failed);
    }
}

// ---------------------------------------------------------------------------
// 事件处理
// ---------------------------------------------------------------------------
static void handle_key(const kb_key_event_t *key)
{
    if (key->ev == KB_EV_LONG && key->key == KB_KEY_OK) {
        s_exit = true; // 请求退出键盘模式
        return;
    }
    if (key->ev != KB_EV_CLICK) {
        return;
    }
    // IDLE 短按 OK:切换 Kimi/通用 模式(录音/权限流程不劫持此键,
    // 它们走下方状态机事件)
    if (key->key == KB_KEY_OK &&
        rs_state_get(&s_sm) == RS_ST_IDLE) {
        s_uni = !s_uni;
        save_mode();
        send_mode();
        refresh_ui();
        return;
    }
    rs_event_t ev = {};
    ev.type = RS_EV_NONE;
    if (key->key == KB_KEY_UP) ev.type = RS_EV_KEY_UP;
    else if (key->key == KB_KEY_DOWN) ev.type = RS_EV_KEY_DOWN;
    else if (key->key == KB_KEY_OK) ev.type = RS_EV_KEY_OK;
    if (ev.type == RS_EV_NONE) return;

    int prev_sel = rs_perm_selection(&s_sm);
    rs_result_t r = rs_step(&s_sm, &ev);
    if (rs_state_get(&s_sm) == RS_ST_PERM && rs_perm_selection(&s_sm) != prev_sel) {
        kb_ui_set_perm_selection(rs_perm_selection(&s_sm));
    }
    apply_result(&r);
}

static void handle_ctrl_line(const char *line)
{
    rp_message_t msg;
    void *root = NULL;
    if (!rp_parse_line(line, strlen(line), &msg, &root)) {
        ESP_LOGW(TAG, "bad ctrl line: %.60s", line);
        return;
    }
    ESP_LOGI(TAG, "ctrl line: type=%d id=%ld state=%d rec_id=%ld text=%.40s",
             (int)msg.type, (long)msg.id, (int)rs_state_get(&s_sm),
             (long)rs_record_id(&s_sm), msg.text ? msg.text : "-");
    switch (msg.type) {
    case RP_MSG_HELLO: {
        char buf[120];
        int n = rp_build_hello_ack(buf, sizeof(buf), read_battery());
        if (n > 0) send_ctrl(buf);
        send_mode(); // hello 时桥接必已订阅,确保模式送达
        break;
    }
    case RP_MSG_PING: {
        char buf[80];
        int n = rp_build_pong(buf, sizeof(buf), read_battery());
        if (n > 0) send_ctrl(buf);
        break;
    }
    case RP_MSG_ASR: {
        rs_event_t ev = {};
        ev.type = RS_EV_ASR_RESULT;
        ev.id = msg.id;
        ev.text = msg.text;
        ev.err = msg.err;
        if (msg.text) {
            snprintf(s_asr_text, sizeof(s_asr_text), "%s", msg.text);
            ev.text = s_asr_text;
        }
        if (msg.err) {
            snprintf(s_asr_err, sizeof(s_asr_err), "%s", msg.err);
            ev.err = s_asr_err;
        }
        disarm_timer(s_asr_timer);
        rs_result_t r = rs_step(&s_sm, &ev);
        apply_result(&r);
        if (rs_state_get(&s_sm) == RS_ST_RESULT) {
            arm_timer(s_result_timer, RESULT_HOLD_MS);
        }
        break;
    }
    case RP_MSG_PERM_REQ: {
        rs_event_t ev = {};
        ev.type = RS_EV_PERM_REQ;
        ev.id = msg.id;
        ev.text = msg.text;
        ev.option_count = (int32_t)msg.option_count;
        ev.timeout_s = msg.timeout_s > 0 ? msg.timeout_s : PERM_DEFAULT_TIMEOUT_S;
        rs_result_t r = rs_step(&s_sm, &ev);
        if (rs_state_get(&s_sm) == RS_ST_PERM && rs_perm_id(&s_sm) == msg.id) {
            kb_ui_show_perm(msg.text, msg.options, (int)msg.option_count,
                            rs_perm_selection(&s_sm));
            if (r.actions & RS_ACT_SEND_PERM_RESP) {
                send_perm_resp(r.perm_id, r.perm_choice);
            }
            arm_timer(s_perm_timer, (uint32_t)ev.timeout_s * 1000);
        } else {
            apply_result(&r);
        }
        break;
    }
    case RP_MSG_PERM_CANCEL: {
        rs_event_t ev = {};
        ev.type = RS_EV_PERM_CANCEL;
        ev.id = msg.id;
        rs_result_t r = rs_step(&s_sm, &ev);
        disarm_timer(s_perm_timer);
        apply_result(&r);
        break;
    }
    default:
        break;
    }
    rp_message_dispose(root);
}

void keyboard_mode_run(void)
{
    ESP_LOGI(TAG, "keyboard mode start");
    s_exit = false;
    s_mic_run = false;
    load_mode();
    rs_init(&s_sm);
    s_asr_text[0] = '\0';
    s_asr_err[0] = '\0';

    if (!s_queue) {
        s_queue = xQueueCreate(KB_QUEUE_DEPTH, sizeof(kbapp_event_t));
    }

    if (!s_rec_timer) {
        s_rec_timer = xTimerCreate("kb_rec", pdMS_TO_TICKS(REC_MAX_MS), pdFALSE, NULL, rec_timer_cb);
        s_asr_timer = xTimerCreate("kb_asr", pdMS_TO_TICKS(30000), pdFALSE, NULL, asr_timer_cb);
        s_perm_timer = xTimerCreate("kb_perm", pdMS_TO_TICKS(PERM_DEFAULT_TIMEOUT_S * 1000), pdFALSE, NULL, perm_timer_cb);
        s_result_timer = xTimerCreate("kb_result", pdMS_TO_TICKS(RESULT_HOLD_MS), pdFALSE, NULL, result_timer_cb);
        s_bat_timer = xTimerCreate("kb_bat", pdMS_TO_TICKS(30000), pdFALSE, NULL, bat_timer_cb);
    }
    xTimerStart(s_bat_timer, 0);

    if (!s_mic_task) {
        xTaskCreate(mic_task, "kb_mic", MIC_TASK_STACK, NULL, 6, &s_mic_task);
    }

    kb_ui_init();

    esp_err_t err = kb_ble_start(ble_evt_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ble start: %s", esp_err_to_name(err));
    }

    while (!s_exit) {
        kb_key_event_t key;
        if (keyboard_get_key(&key, 50)) {
            handle_key(&key);
            continue;
        }
        kbapp_event_t ev;
        if (xQueueReceive(s_queue, &ev, 0) != pdTRUE) {
            continue;
        }
        switch (ev.type) {
        case KBAPP_EV_BLE_LINK_UP: {
            rs_event_t se = {.type = RS_EV_LINK_UP};
            rs_result_t r = rs_step(&s_sm, &se);
            apply_result(&r);
            char buf[120];
            int n = rp_build_hello_ack(buf, sizeof(buf), read_battery());
            if (n > 0) send_ctrl(buf);
            send_mode(); // 让桥接按设备端模式工作
            break;
        }
        case KBAPP_EV_BLE_LINK_DOWN: {
            rs_event_t se = {.type = RS_EV_LINK_DOWN};
            rs_result_t r = rs_step(&s_sm, &se);
            s_mic_run = false;
            disarm_timer(s_rec_timer);
            disarm_timer(s_asr_timer);
            disarm_timer(s_perm_timer);
            apply_result(&r);
            break;
        }
        case KBAPP_EV_CTRL_LINE:
            handle_ctrl_line(ev.line);
            break;
        case KBAPP_EV_REC_TIMEOUT: {
            rs_event_t se = {.type = RS_EV_REC_TIMEOUT};
            rs_result_t r = rs_step(&s_sm, &se);
            apply_result(&r);
            break;
        }
        case KBAPP_EV_PERM_TIMEOUT: {
            rs_event_t se = {.type = RS_EV_PERM_TIMEOUT};
            rs_result_t r = rs_step(&s_sm, &se);
            apply_result(&r);
            break;
        }
        case KBAPP_EV_ASR_TIMEOUT: {
            // 30 秒没等到识别结果:按识别失败处理,避免永久卡在识别中
            if (rs_state_get(&s_sm) == RS_ST_SENDING) {
                rs_event_t se = {};
                se.type = RS_EV_ASR_RESULT;
                se.id = rs_record_id(&s_sm);
                snprintf(s_asr_err, sizeof(s_asr_err), "识别超时");
                se.err = s_asr_err;
                rs_result_t r = rs_step(&s_sm, &se);
                apply_result(&r);
                if (rs_state_get(&s_sm) == RS_ST_RESULT) {
                    arm_timer(s_result_timer, RESULT_HOLD_MS);
                }
            }
            break;
        }
        case KBAPP_EV_RESULT_TIMEOUT: {
            rs_event_t se = {.type = RS_EV_RESULT_DONE};
            rs_result_t r = rs_step(&s_sm, &se);
            apply_result(&r);
            break;
        }
        case KBAPP_EV_BATTERY_POLL:
            kb_ui_set_status(rs_link_up(&s_sm), read_battery());
            arm_timer(s_bat_timer, 30000);
            break;
        default:
            break;
        }
    }

    // 清理:停录音、停定时器、释放 BLE、回到菜单前重置状态机
    ESP_LOGI(TAG, "keyboard mode exit");
    s_mic_run = false;
    s_exit = false;
    disarm_timer(s_rec_timer);
    disarm_timer(s_asr_timer);
    disarm_timer(s_perm_timer);
    disarm_timer(s_result_timer);
    disarm_timer(s_bat_timer);
    rs_init(&s_sm);
    kb_ble_stop();
}
