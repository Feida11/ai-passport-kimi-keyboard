// main/remote_protocol.h
// Kimi 语音遥控器 BLE 控制通道协议:换行分隔 JSON 行 + 二进制音频帧 + 通知分片。
// 纯 C、无 ESP-IDF 依赖;解析端借 cJSON,可在主机测试中与 cJSON.c 一起编译。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 控制通道消息(JSON 行)
// ---------------------------------------------------------------------------

typedef enum {
    RP_MSG_UNKNOWN = 0,
    RP_MSG_HELLO,        // PC -> 设备: {"t":"hello","v":1}
    RP_MSG_PING,         // PC -> 设备: {"t":"ping"}
    RP_MSG_ASR,          // PC -> 设备: {"t":"asr","id":n,"text":"..."} 或 {"t":"asr","id":n,"err":"..."}
    RP_MSG_PERM_REQ,     // PC -> 设备: {"t":"perm_req","id":n,"text":"...","opts":[...],"timeout":60}
    RP_MSG_PERM_CANCEL,  // PC -> 设备: {"t":"perm_cancel","id":n}
    RP_MSG_HELLO_ACK,    // 设备 -> PC: {"t":"hello_ack","v":1,"bat":87}
    RP_MSG_PONG,         // 设备 -> PC: {"t":"pong","bat":87}
    RP_MSG_REC_START,    // 设备 -> PC: {"t":"rec_start","id":n}
    RP_MSG_REC_STOP,     // 设备 -> PC: {"t":"rec_stop","id":n,"reason":"ok"|"cancel"|"timeout"}
    RP_MSG_PERM_RESP,    // 设备 -> PC: {"t":"perm_resp","id":n,"choice":i} (choice=-1 取消/超时)
    RP_MSG_KEY,          // 设备 -> PC: {"t":"key","btn":"up"|"down"|"ok","ev":"click"|"long"}
    RP_MSG_MODE,         // 设备 -> PC: {"t":"mode","m":"kimi"|"uni"} 键盘工作模式
} rp_msg_type_t;

#define RP_PERM_OPTIONS_MAX 8

typedef struct {
    rp_msg_type_t type;
    int32_t id;        // 录音编号 / 权限请求编号
    int32_t choice;    // RP_MSG_PERM_RESP 选中项;-1 表示取消或超时
    int32_t timeout_s; // RP_MSG_PERM_REQ 超时秒数
    int32_t battery;   // RP_MSG_HELLO_ACK / RP_MSG_PONG 电量百分比;-1 表示未知
    const char *text;  // ASR 结果文本 / 权限请求文本;借用 cJSON 树内存
    const char *err;   // ASR 错误信息;借用 cJSON 树内存
    const char *reason; // rec_stop 原因字符串;借用 cJSON 树内存
    const char *options[RP_PERM_OPTIONS_MAX]; // 权限选项文本;借用 cJSON 树内存
    size_t option_count;
    const char *btn;   // RP_MSG_KEY 按键名;借用 cJSON 树内存
    const char *ev;    // RP_MSG_KEY 事件名;借用 cJSON 树内存
    const char *mode;  // RP_MSG_MODE "kimi"|"uni";借用 cJSON 树内存
} rp_message_t;

// 解析一行 JSON 文本(无需以 NUL 结尾)。成功返回 true 并填充 out;
// *root_out 持有分配的 cJSON 树,out 内所有字符串指针指向该树内存,
// 调用方用 rp_message_dispose() 释放。无法识别或解析失败返回 false。
bool rp_parse_line(const char *line, size_t len, rp_message_t *out, void **root_out);

// 释放 rp_parse_line 分配的解析树(root_out 内容)。
void rp_message_dispose(void *root);

// ---- 设备 -> PC 消息构造(输出换行分隔 JSON,不含换行符;返回写入长度,失败 -1) ----
int rp_build_hello_ack(char *buf, size_t cap, int battery);
int rp_build_mode(char *buf, size_t cap, int uni);
int rp_build_pong(char *buf, size_t cap, int battery);
int rp_build_rec_start(char *buf, size_t cap, int32_t id, int32_t sample_rate);
int rp_build_rec_stop(char *buf, size_t cap, int32_t id, const char *reason);
int rp_build_perm_resp(char *buf, size_t cap, int32_t id, int32_t choice);
int rp_build_key(char *buf, size_t cap, const char *btn, const char *ev);

// ---------------------------------------------------------------------------
// 音频帧(AUDIO_TX 通知,二进制)
// 布局: [0]=0xA5 [1]=seq_lo [2]=seq_hi [3]=flags(bit0=录音最后一帧) [4]=len_lo [5]=len_hi [6..]=载荷
// ---------------------------------------------------------------------------

#define RP_AUDIO_MAGIC 0xA5u
#define RP_AUDIO_HDR_LEN 6u
#define RP_AUDIO_FLAG_LAST 0x01u

typedef struct {
    uint16_t seq;       // 帧序号(每条录音内从 0 递增,16 位回绕)
    uint8_t flags;      // RP_AUDIO_FLAG_LAST 等
    uint16_t payload_len;
    const uint8_t *payload; // 指向帧缓冲区,不复制
} rp_audio_frame_t;

// 打包一帧;out 容量至少 RP_AUDIO_HDR_LEN + payload_len。
size_t rp_audio_pack(uint8_t *out, uint16_t seq, uint8_t flags,
                     const uint8_t *payload, size_t payload_len);

// 解析一帧;校验魔数、长度与缓冲区一致。frame_len 为完整帧字节数。
bool rp_audio_parse(const uint8_t *frame, size_t frame_len, rp_audio_frame_t *out);

// ---------------------------------------------------------------------------
// 控制通道通知分片(长 JSON 行超过单条 notify 载荷时拆分)
// 每片前缀 1 字节: 0x01=后面还有片,0x00=最后一片(或唯一一片)
// ---------------------------------------------------------------------------

// 计算 message_len 字节的消息需要的分片数,frag_payload 为每片最大载荷(不含前缀)。
size_t rp_frag_count(size_t message_len, size_t frag_payload);

// 取第 index 个分片,写入 out(cap 至少 frag_payload+1)。返回该片总长度。
size_t rp_frag_get(const uint8_t *message, size_t message_len, size_t frag_payload,
                   size_t index, uint8_t *out);

// 重组器(调用方持有结构体内存)。
typedef struct {
    uint8_t *buf;       // 调用方提供的组装缓冲
    size_t cap;         // 缓冲容量
    size_t len;         // 当前已收长度
    bool in_progress;   // 是否处于多片组装中
} rp_reasm_t;

void rp_reasm_init(rp_reasm_t *r, uint8_t *buf, size_t cap);

// 喂入一个分片(frame 含 1 字节前缀)。完整一条消息时返回 true 且 *out_len/*out 有效
// (out 指向 r->buf)。出错(容量不足/状态混乱)返回 false 并复位重组器。
bool rp_reasm_feed(rp_reasm_t *r, const uint8_t *frame, size_t frame_len,
                   const uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif
