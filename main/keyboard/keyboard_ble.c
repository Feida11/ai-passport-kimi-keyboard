// main/remote_ble.c
// 见 remote_ble.h。单连接外设;控制通道与音频通道分特征;PC->设备写也走分片前缀协议。
#include "keyboard_ble.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "remote_protocol.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "remote_ble";
#define DEVICE_NAME "KimiRemote"

// 128-bit UUID(自行生成,仅本应用使用)
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x4d, 0x49, 0x4b, 0x4c, 0x4f, 0x54, 0x43, 0x9e,
                     0x4a, 0x4f, 0x52, 0x50, 0x01, 0x00, 0x6b, 0x6b);
static const ble_uuid128_t s_chr_ctrl_rx_uuid =
    BLE_UUID128_INIT(0x4d, 0x49, 0x4b, 0x4c, 0x4f, 0x54, 0x43, 0x9e,
                     0x4a, 0x4f, 0x52, 0x50, 0x02, 0x00, 0x6b, 0x6b);
static const ble_uuid128_t s_chr_ctrl_tx_uuid =
    BLE_UUID128_INIT(0x4d, 0x49, 0x4b, 0x4c, 0x4f, 0x54, 0x43, 0x9e,
                     0x4a, 0x4f, 0x52, 0x50, 0x03, 0x00, 0x6b, 0x6b);
static const ble_uuid128_t s_chr_audio_tx_uuid =
    BLE_UUID128_INIT(0x4d, 0x49, 0x4b, 0x4c, 0x4f, 0x54, 0x43, 0x9e,
                     0x4a, 0x4f, 0x52, 0x50, 0x04, 0x00, 0x6b, 0x6b);

#define CTRL_REASM_CAP 512
#define CTRL_LINE_CAP 480

static kb_ble_evt_cb_t s_cb;
static void *s_cb_ctx;
static uint16_t s_conn_handle;
static uint16_t s_att_mtu;
static uint8_t s_addr_type;
static bool s_initialized;
static TaskHandle_t s_host_task;
static SemaphoreHandle_t s_host_stopped;

// 特征值句柄(注册时由 NimBLE 填充)
static uint16_t s_chr_ctrl_rx_handle;
static uint16_t s_chr_ctrl_tx_handle;
static uint16_t s_chr_audio_tx_handle;

// 控制通道写重组(PC -> 设备)
static uint8_t s_reasm_buf[CTRL_REASM_CAP];
static rp_reasm_t s_reasm;
static char s_line_buf[CTRL_LINE_CAP];

// CCC 订阅状态
static bool s_ctrl_subscribed;
static bool s_audio_subscribed;

static int gap_event(struct ble_gap_event *event, void *arg);
static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg);

static struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_chr_ctrl_rx_uuid.u,
                .access_cb = gatt_access,
                .val_handle = &s_chr_ctrl_rx_handle,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_chr_ctrl_tx_uuid.u,
                .access_cb = gatt_access,
                .val_handle = &s_chr_ctrl_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_chr_audio_tx_uuid.u,
                .access_cb = gatt_access,
                .val_handle = &s_chr_audio_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static void notify_link_up(void)
{
    if (s_cb) {
        kb_ble_evt_t evt = {.type = KB_BLE_EVT_LINK_UP};
        s_cb(&evt, s_cb_ctx);
    }
}

static void notify_link_down(int reason)
{
    if (s_cb) {
        kb_ble_evt_t evt = {.type = KB_BLE_EVT_LINK_DOWN};
        evt.data.link_down.reason = reason;
        s_cb(&evt, s_cb_ctx);
    }
}

static void notify_ctrl_line(const char *line, size_t len)
{
    if (s_cb) {
        kb_ble_evt_t evt = {.type = KB_BLE_EVT_CTRL_LINE};
        evt.data.ctrl_line.data = line;
        evt.data.ctrl_line.len = len;
        s_cb(&evt, s_cb_ctx);
    }
}

static int start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        return rc;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(200);

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "advertising as %s", DEVICE_NAME);
    } else {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    }
    return rc;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_att_mtu = 23; // 默认,MTU 交换后更新
            ESP_LOGI(TAG, "connected");
            // 请求更短连接间隔(提升音频吞吐)
            struct ble_gap_upd_params upd = {
                .itvl_min = BLE_GAP_CONN_ITVL_MS(15),
                .itvl_max = BLE_GAP_CONN_ITVL_MS(30),
                .latency = 0,
                .supervision_timeout = 500,
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            ble_gap_update_params(event->connect.conn_handle, &upd);
            notify_link_up();
        } else {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected: reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_ctrl_subscribed = false;
        s_audio_subscribed = false;
        notify_link_down(event->disconnect.reason);
        start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        s_att_mtu = event->mtu.value;
        if (s_att_mtu > 247) s_att_mtu = 247;
        ESP_LOGI(TAG, "mtu=%d", s_att_mtu);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe: attr=%d cur_notify=%d",
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == s_chr_ctrl_tx_handle) {
            s_ctrl_subscribed = event->subscribe.cur_notify != 0;
        } else if (event->subscribe.attr_handle == s_chr_audio_tx_handle) {
            s_audio_subscribed = event->subscribe.cur_notify != 0;
        }
        break;

    default:
        break;
    }
    return 0;
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // 写重组:每片带 1 字节前缀(0x00 结束 / 0x01 继续)
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len == 0 || om_len > CTRL_REASM_CAP) {
            rp_reasm_init(&s_reasm, s_reasm_buf, sizeof(s_reasm_buf));
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint8_t frag[CTRL_REASM_CAP + 1];
        int rc = ble_hs_mbuf_to_flat(ctxt->om, frag, sizeof(frag), NULL);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        const uint8_t *msg = NULL;
        size_t msg_len = 0;
        if (rp_reasm_feed(&s_reasm, frag, om_len, &msg, &msg_len)) {
            if (msg_len >= CTRL_LINE_CAP) {
                msg_len = CTRL_LINE_CAP - 1;
            }
            memcpy(s_line_buf, msg, msg_len);
            s_line_buf[msg_len] = '\0';
            notify_ctrl_line(s_line_buf, msg_len);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "nimble reset: %d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0) rc = start_advertising();
    if (rc != 0) {
        ESP_LOGE(TAG, "sync setup failed: %d", rc);
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    xSemaphoreGive(s_host_stopped);
    for (;;) vTaskSuspend(NULL);
}

esp_err_t kb_ble_start(kb_ble_evt_cb_t cb, void *ctx)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cb = cb;
    s_cb_ctx = ctx;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_att_mtu = 23;
    s_ctrl_subscribed = false;
    s_audio_subscribed = false;
    rp_reasm_init(&s_reasm, s_reasm_buf, sizeof(s_reasm_buf));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;

    // 顺序遵循 NimBLE bleprph 范例:先初始化 GAP/GATT 服务,再注册应用服务;
    // 服务表必须是可写静态存储(NimBLE 注册时会回写句柄与标志位)。
    ble_svc_gap_init();
    ble_svc_gatt_init();
    // 必须先统计属性数量(ble_gatts_count_cfg)再注册,否则 ATT 属性池按 0 分配,
    // 注册服务时 ble_att_svr_entry_alloc 失败返回 ENOMEM(rc=6)。
    ble_gatts_count_cfg(s_gatt_svcs);
    int rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        nimble_port_deinit();
        return ESP_FAIL;
    }

    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) {
        nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }

    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        nimble_port_deinit();
        vSemaphoreDelete(s_host_stopped);
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    if (xTaskCreatePinnedToCore(host_task, "nimble_host", NIMBLE_HS_STACK_SIZE,
                                NULL, configMAX_PRIORITIES - 4, &s_host_task,
                                NIMBLE_CORE) != pdPASS) {
        nimble_port_deinit();
        vSemaphoreDelete(s_host_stopped);
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t kb_ble_stop(void)
{
    if (!s_initialized) return ESP_OK;

    (void)ble_gap_adv_stop();
    int rc = nimble_port_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_host_stopped, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_host_task) {
        vTaskDelete(s_host_task);
        s_host_task = NULL;
    }
    vSemaphoreDelete(s_host_stopped);
    s_host_stopped = NULL;
    nimble_port_deinit();
    s_initialized = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_ctrl_subscribed = false;
    s_audio_subscribed = false;
    return ESP_OK;
}

bool kb_ble_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

// 发送自包含通知(NimBLE 会消费 mbuff,无需手动释放)
static int notify_custom(uint16_t att_handle, const void *data, uint16_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }
    return ble_gatts_notify_custom(s_conn_handle, att_handle, om);
}

esp_err_t kb_ble_send_ctrl(const char *line, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_ctrl_subscribed) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t frag_payload = s_att_mtu - 3 - 1; // notify 载荷 = MTU-3,再减 1 字节分片前缀
    if (frag_payload < 8) frag_payload = 8;
    size_t count = rp_frag_count(len, frag_payload);
    uint8_t frag[256];
    for (size_t i = 0; i < count; i++) {
        size_t n = rp_frag_get((const uint8_t *)line, len, frag_payload, i, frag);
        int rc = notify_custom(s_chr_ctrl_tx_handle, frag, (uint16_t)n);
        for (int retry = 0; rc == BLE_HS_EAGAIN && retry < 5; retry++) {
            vTaskDelay(pdMS_TO_TICKS(3));
            rc = notify_custom(s_chr_ctrl_tx_handle, frag, (uint16_t)n);
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "ctrl notify failed: %d", rc);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t kb_ble_send_audio(const uint8_t *frame, size_t len)
{
    // 不做 s_audio_subscribed 闸门:Windows 的 CCCD 写可能因陈旧缓存落到错误
    // 描述符(表现为桥接读回 0100 但设备未订阅)。NimBLE 的 notify_custom
    // 不校验 CCCD 直接发,Windows 按客户端订阅状态投递,直接发即可流通。
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > kb_ble_att_mtu() - 3) {
        return ESP_ERR_INVALID_SIZE;
    }
    int rc = notify_custom(s_chr_audio_tx_handle, frame, (uint16_t)len);
    for (int retry = 0; rc == BLE_HS_EAGAIN && retry < 5; retry++) {
        vTaskDelay(pdMS_TO_TICKS(2));
        rc = notify_custom(s_chr_audio_tx_handle, frame, (uint16_t)len);
    }
    if (rc != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// 诊断辅助:供 keyboard_mode 日志打印内部状态
uint16_t s_att_mtu_log(void) { return s_att_mtu; }
int s_audio_subscribed_log(void) { return s_audio_subscribed ? 1 : 0; }

// 当前连接实时协商的 ATT MTU。不能用 s_att_mtu:它由" whichever 连接
// 发起交换"的事件更新,Windows 重连/幽灵连接会把全局值污染回 23,
// 导致录音按 8kHz/碎帧跑、链路拥塞丢帧。这里按连接句柄现查。
uint16_t kb_ble_att_mtu(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return 23;
    uint16_t mtu = ble_att_mtu(s_conn_handle);
    if (mtu < 23) mtu = 23;
    if (mtu > 247) mtu = 247;
    return mtu;
}

// 若本连接还没做过 MTU 交换(ATT 仍为默认 23),由外设侧主动发起一次。
// 本次录音赶不及受益,下一次录音即可拿到大 MTU。
void kb_ble_kick_mtu_exchange(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        ble_att_mtu(s_conn_handle) <= 23) {
        ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
    }
}
