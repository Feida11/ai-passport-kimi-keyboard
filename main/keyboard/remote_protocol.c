// main/remote_protocol.c
// 见 remote_protocol.h。构建侧只用 snprintf(输出为内部固定字符串,无用户文本注入);
// 解析侧用 cJSON。
#include "remote_protocol.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

static bool copy_text_field(const cJSON *root, const char *key, const char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    *out = item->valuestring;
    return true;
}

static bool copy_int_field(const cJSON *root, const char *key, int32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = (int32_t)item->valuedouble;
    return true;
}

bool rp_parse_line(const char *line, size_t len, rp_message_t *out, void **root_out)
{
    cJSON *root;
    const cJSON *t;

    memset(out, 0, sizeof(*out));
    out->type = RP_MSG_UNKNOWN;
    out->id = -1;
    out->choice = 0;
    out->timeout_s = 0;
    out->battery = -1;
    *root_out = NULL;

    root = cJSON_ParseWithLength(line, len);
    if (root == NULL || !cJSON_IsObject(root)) {
        goto fail;
    }
    t = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (!cJSON_IsString(t) || t->valuestring == NULL) {
        goto fail;
    }

    if (strcmp(t->valuestring, "hello") == 0) {
        out->type = RP_MSG_HELLO;
    } else if (strcmp(t->valuestring, "ping") == 0) {
        out->type = RP_MSG_PING;
    } else if (strcmp(t->valuestring, "asr") == 0) {
        out->type = RP_MSG_ASR;
        if (!copy_int_field(root, "id", &out->id)) goto fail;
        copy_text_field(root, "text", &out->text);   // text 与 err 二选一,允许都缺省
        copy_text_field(root, "err", &out->err);
    } else if (strcmp(t->valuestring, "perm_req") == 0) {
        const cJSON *opts;
        out->type = RP_MSG_PERM_REQ;
        if (!copy_int_field(root, "id", &out->id)) goto fail;
        if (!copy_text_field(root, "text", &out->text)) goto fail;
        if (!copy_int_field(root, "timeout", &out->timeout_s)) goto fail;
        opts = cJSON_GetObjectItemCaseSensitive(root, "opts");
        if (!cJSON_IsArray(opts)) goto fail;
        out->option_count = 0;
        for (const cJSON *it = opts->child; it != NULL && out->option_count < RP_PERM_OPTIONS_MAX;
             it = it->next) {
            if (!cJSON_IsString(it) || it->valuestring == NULL) goto fail;
            out->options[out->option_count++] = it->valuestring;
        }
        if (out->option_count == 0) goto fail;
    } else if (strcmp(t->valuestring, "perm_cancel") == 0) {
        out->type = RP_MSG_PERM_CANCEL;
        if (!copy_int_field(root, "id", &out->id)) goto fail;
    } else if (strcmp(t->valuestring, "hello_ack") == 0) {
        out->type = RP_MSG_HELLO_ACK;
        copy_int_field(root, "bat", &out->battery);
    } else if (strcmp(t->valuestring, "pong") == 0) {
        out->type = RP_MSG_PONG;
        copy_int_field(root, "bat", &out->battery);
    } else if (strcmp(t->valuestring, "rec_start") == 0) {
        out->type = RP_MSG_REC_START;
        if (!copy_int_field(root, "id", &out->id)) goto fail;
    } else if (strcmp(t->valuestring, "rec_stop") == 0) {
        out->type = RP_MSG_REC_STOP;
        if (!copy_int_field(root, "id", &out->id)) goto fail;
        copy_text_field(root, "reason", &out->reason);
    } else if (strcmp(t->valuestring, "perm_resp") == 0) {
        out->type = RP_MSG_PERM_RESP;
        if (!copy_int_field(root, "id", &out->id)) goto fail;
        if (!copy_int_field(root, "choice", &out->choice)) goto fail;
    } else if (strcmp(t->valuestring, "key") == 0) {
        out->type = RP_MSG_KEY;
        if (!copy_text_field(root, "btn", &out->btn)) goto fail;
        if (!copy_text_field(root, "ev", &out->ev)) goto fail;
    } else if (strcmp(t->valuestring, "mode") == 0) {
        out->type = RP_MSG_MODE;
        if (!copy_text_field(root, "m", &out->mode)) goto fail;
    } else {
        goto fail;
    }

    *root_out = root;
    return true;

fail:
    cJSON_Delete(root);
    return false;
}

void rp_message_dispose(void *root)
{
    cJSON_Delete((cJSON *)root);
}

// ---------------------------------------------------------------------------
// 构建(设备 -> PC)
// ---------------------------------------------------------------------------

int rp_build_hello_ack(char *buf, size_t cap, int battery)
{
    int n = snprintf(buf, cap, "{\"t\":\"hello_ack\",\"v\":1,\"bat\":%d}", battery);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int rp_build_pong(char *buf, size_t cap, int battery)
{
    int n = snprintf(buf, cap, "{\"t\":\"pong\",\"bat\":%d}", battery);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int rp_build_mode(char *buf, size_t cap, int uni)
{
    int n = snprintf(buf, cap, "{\"t\":\"mode\",\"m\":\"%s\"}",
                     uni ? "uni" : "kimi");
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int rp_build_rec_start(char *buf, size_t cap, int32_t id, int32_t sample_rate)
{
    int n;
    if (sample_rate > 0 && sample_rate != 16000) {
        n = snprintf(buf, cap, "{\"t\":\"rec_start\",\"id\":%ld,\"sr\":%ld}",
                     (long)id, (long)sample_rate);
    } else {
        n = snprintf(buf, cap, "{\"t\":\"rec_start\",\"id\":%ld}", (long)id);
    }
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int rp_build_rec_stop(char *buf, size_t cap, int32_t id, const char *reason)
{
    if (reason == NULL || strchr(reason, '"') != NULL || strchr(reason, '\\') != NULL) {
        return -1;
    }
    int n = snprintf(buf, cap, "{\"t\":\"rec_stop\",\"id\":%ld,\"reason\":\"%s\"}",
                     (long)id, reason);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int rp_build_perm_resp(char *buf, size_t cap, int32_t id, int32_t choice)
{
    int n = snprintf(buf, cap, "{\"t\":\"perm_resp\",\"id\":%ld,\"choice\":%ld}",
                     (long)id, (long)choice);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int rp_build_key(char *buf, size_t cap, const char *btn, const char *ev)
{
    if (btn == NULL || ev == NULL || strchr(btn, '"') != NULL || strchr(ev, '"') != NULL) {
        return -1;
    }
    int n = snprintf(buf, cap, "{\"t\":\"key\",\"btn\":\"%s\",\"ev\":\"%s\"}", btn, ev);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

// ---------------------------------------------------------------------------
// 音频帧
// ---------------------------------------------------------------------------

size_t rp_audio_pack(uint8_t *out, uint16_t seq, uint8_t flags,
                     const uint8_t *payload, size_t payload_len)
{
    out[0] = RP_AUDIO_MAGIC;
    out[1] = (uint8_t)(seq & 0xFF);
    out[2] = (uint8_t)((seq >> 8) & 0xFF);
    out[3] = flags;
    out[4] = (uint8_t)(payload_len & 0xFF);
    out[5] = (uint8_t)((payload_len >> 8) & 0xFF);
    if (payload_len > 0) {
        memcpy(out + RP_AUDIO_HDR_LEN, payload, payload_len);
    }
    return RP_AUDIO_HDR_LEN + payload_len;
}

bool rp_audio_parse(const uint8_t *frame, size_t frame_len, rp_audio_frame_t *out)
{
    if (frame == NULL || frame_len < RP_AUDIO_HDR_LEN) {
        return false;
    }
    if (frame[0] != RP_AUDIO_MAGIC) {
        return false;
    }
    out->seq = (uint16_t)(frame[1] | ((uint16_t)frame[2] << 8));
    out->flags = frame[3];
    out->payload_len = (uint16_t)(frame[4] | ((uint16_t)frame[5] << 8));
    if (frame_len != RP_AUDIO_HDR_LEN + out->payload_len) {
        return false;
    }
    out->payload = frame + RP_AUDIO_HDR_LEN;
    return true;
}

// ---------------------------------------------------------------------------
// 通知分片
// ---------------------------------------------------------------------------

size_t rp_frag_count(size_t message_len, size_t frag_payload)
{
    if (frag_payload == 0) {
        return 0;
    }
    return (message_len + frag_payload - 1) / frag_payload;
}

size_t rp_frag_get(const uint8_t *message, size_t message_len, size_t frag_payload,
                   size_t index, uint8_t *out)
{
    size_t offset = index * frag_payload;
    if (offset >= message_len && message_len > 0) {
        return 0;
    }
    size_t chunk = message_len - offset;
    if (chunk > frag_payload) {
        chunk = frag_payload;
    }
    bool last = (offset + chunk) >= message_len;
    out[0] = last ? 0x00 : 0x01;
    if (chunk > 0) {
        memcpy(out + 1, message + offset, chunk);
    }
    return 1 + chunk;
}

void rp_reasm_init(rp_reasm_t *r, uint8_t *buf, size_t cap)
{
    r->buf = buf;
    r->cap = cap;
    r->len = 0;
    r->in_progress = false;
}

bool rp_reasm_feed(rp_reasm_t *r, const uint8_t *frame, size_t frame_len,
                   const uint8_t **out, size_t *out_len)
{
    bool more;
    size_t chunk;

    if (frame == NULL || frame_len < 1) {
        rp_reasm_init(r, r->buf, r->cap);
        return false;
    }
    more = frame[0] != 0x00;
    chunk = frame_len - 1;

    if (r->len + chunk > r->cap) {
        rp_reasm_init(r, r->buf, r->cap);
        return false;
    }
    if (chunk > 0) {
        memcpy(r->buf + r->len, frame + 1, chunk);
    }
    r->len += chunk;
    r->in_progress = more;

    if (more) {
        return false;
    }
    *out = r->buf;
    *out_len = r->len;
    r->len = 0;
    r->in_progress = false;
    return true;
}
