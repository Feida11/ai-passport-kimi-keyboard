// main/keyboard/keyboard_dispatch.c
#include "keyboard_dispatch.h"

#include <stdbool.h>

static QueueHandle_t s_queue;
static volatile op_mode_t s_mode = OP_MODE_MENU;

QueueHandle_t keyboard_dispatch_init(void)
{
    if (s_queue == NULL) {
        s_queue = xQueueCreate(8, sizeof(kb_key_event_t));
    }
    return s_queue;
}

bool keyboard_get_key(kb_key_event_t *out, int timeout_ms)
{
    if (s_queue == NULL) return false;
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_queue, out, ticks) == pdTRUE;
}

void keyboard_post_key(kb_key_t key, kb_ev_t ev)
{
    keyboard_dispatch_init();
    kb_key_event_t e = {.key = key, .ev = ev};
    xQueueSend(s_queue, &e, pdMS_TO_TICKS(20));
}

op_mode_t keyboard_op_mode(void)
{
    return s_mode;
}

void keyboard_set_op_mode(op_mode_t mode)
{
    s_mode = mode;
}
