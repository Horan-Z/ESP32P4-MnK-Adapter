#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "usbh_core.h"
#include "usbh_hid.h"
#include "input_bridge.h"

static char *TAG = "HOST";

typedef struct {
    bool is_active;
    esp_timer_handle_t timer;
    uint8_t *buffer;
} hid_int_in_t;

typedef struct {
    uint8_t protocol;
    uint16_t len;
    uint8_t data[64];
} hid_msg_t;

struct custom_mouse_report {
    uint16_t buttons;
    int16_t x;
    int16_t y;
    int8_t wheel;
};

#define QUEUE_LEN 1024
static QueueHandle_t s_msg_queue = NULL;
static TaskHandle_t s_msg_task_handle = NULL;

void ld_include_hid(void) {}

static void usbh_hid_keyboard_report_callback(void *arg, int nbytes)
{
    struct usb_hid_kbd_report *kb_report = (struct usb_hid_kbd_report *)arg;
    bridge_push_keyboard(kb_report->key, kb_report->modifier);
}

static void usbh_hid_mouse_report_callback(void *arg, int nbytes)
{
    struct custom_mouse_report *mouse_report = (struct custom_mouse_report *)arg;
    bridge_push_mouse(mouse_report->buttons, mouse_report->x, mouse_report->y, mouse_report->wheel);
}

//Note: This callback is in the interrupt context
static void usbh_hid_callback(void *arg, int nbytes)
{
    BaseType_t xTaskWoken = pdFALSE;
    struct usbh_hid *hid_class = (struct usbh_hid *)arg;
    hid_int_in_t *hid_intin = (hid_int_in_t *)hid_class->user_data;

    if (nbytes <= 0) {
        hid_intin->is_active = false;
        return;
    }
    uint8_t sub_class = hid_class->hport->config.intf[hid_class->intf].altsetting[0].intf_desc.bInterfaceSubClass;
    uint8_t protocol = hid_class->hport->config.intf[hid_class->intf].altsetting[0].intf_desc.bInterfaceProtocol;

    if (s_msg_queue) {
        hid_msg_t msg;
        if (nbytes <= sizeof(msg.data)) {
            msg.protocol = HID_PROTOCOL_NONE;
            if (sub_class == HID_SUBCLASS_BOOTIF) {
                if (protocol == HID_PROTOCOL_KEYBOARD) {
                    msg.protocol = HID_PROTOCOL_KEYBOARD;
                } else if (protocol == HID_PROTOCOL_MOUSE) {
                    msg.protocol = HID_PROTOCOL_MOUSE;
                }
            }
            msg.len = nbytes;
            memcpy(msg.data, hid_intin->buffer, nbytes);
            if (xQueueSendFromISR(s_msg_queue, &msg, &xTaskWoken) != pdTRUE) {
                ESP_EARLY_LOGD(TAG, "msg queue full");
            }
        } else {
            ESP_EARLY_LOGD(TAG, "nbytes(%d) > sizeof(msg.data)", nbytes);
        }

    }
    hid_intin->is_active = false;
    if (xTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

//Note: If the dispatch_method of esp_timer is ESP_TIMER_ISR, the callback is in the interrupt context.
static void intin_timer_cb(void *arg)
{
    int ret;
    struct usbh_hid *hid_class = (struct usbh_hid *)arg;
    hid_int_in_t *hid_intin = (hid_int_in_t *)hid_class->user_data;
    if (hid_intin->is_active) {
        return;
    }
    usbh_int_urb_fill(&hid_class->intin_urb, hid_class->hport, hid_class->intin, hid_intin->buffer, hid_class->intin->wMaxPacketSize, 0,
                      usbh_hid_callback, hid_class);

    hid_intin->is_active = true;
    ret = usbh_submit_urb(&hid_class->intin_urb);
    if (ret != 0) {
        if (ret == -USB_ERR_NOTCONN) {
            esp_timer_stop(hid_intin->timer);
            return;
        }
        hid_intin->is_active = false;
        ESP_EARLY_LOGE(TAG, "usbh_submit_urb failed");
    }
}

static void usbh_hid_msg_task(void *arg)
{
    hid_msg_t msg;
    while (1) {
        BaseType_t err = xQueueReceive(s_msg_queue, &msg, portMAX_DELAY);
        if (err != pdTRUE) {
            continue;
        }
        if (msg.protocol == HID_PROTOCOL_MOUSE) {
            usbh_hid_mouse_report_callback(msg.data, msg.len);
        } else {
            usbh_hid_keyboard_report_callback(msg.data, msg.len);
        }
    }
    vTaskDelete(NULL);
}

static void creat_msg_task(void)
{
    if (s_msg_queue == NULL) {
        s_msg_queue = xQueueCreate(QUEUE_LEN, sizeof(hid_msg_t));
        if (s_msg_queue == NULL) {
            ESP_LOGE(TAG, "ringbuf create failed");
            return;
        }
    }
    if (s_msg_task_handle == NULL) {
        xTaskCreatePinnedToCore(usbh_hid_msg_task, "usbh_hid_msg_task", 8192, NULL, 5, &s_msg_task_handle, 0);
    }
}

void usbh_hid_run(struct usbh_hid *hid_class)
{
    int ret;
    esp_err_t err;
    uint8_t sub_class = hid_class->hport->config.intf[hid_class->intf].altsetting[0].intf_desc.bInterfaceSubClass;
    uint8_t protocol = hid_class->hport->config.intf[hid_class->intf].altsetting[0].intf_desc.bInterfaceProtocol;
    ESP_LOGI(TAG, "intf %u, SubClass %u, Protocol %u", hid_class->intf, sub_class, protocol);

    if (sub_class == HID_SUBCLASS_BOOTIF) {
        ret = usbh_hid_set_protocol(hid_class, HID_PROTOCOL_REPORT);
        if (ret < 0) {
            return;
        }
    }

    creat_msg_task();

    if (hid_class->intin == NULL) {
        ESP_LOGW(TAG, "no intin ep desc");
        return;
    }

    hid_int_in_t *hid_intin = heap_caps_calloc(1, sizeof(hid_int_in_t), MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
    if (hid_intin == NULL) {
        ESP_LOGW(TAG, "Malloc failed");
        return;
    }
    hid_intin->buffer = heap_caps_aligned_alloc(CONFIG_USB_ALIGN_SIZE, hid_class->intin->wMaxPacketSize, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (hid_intin->buffer == NULL) {
        ESP_LOGW(TAG, "Malloc failed");
        goto error;
    }
    hid_intin->is_active = false;
    esp_timer_create_args_t timer_cfg = {
        .callback = intin_timer_cb,
        .arg = hid_class,
#if CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
        .dispatch_method = ESP_TIMER_ISR,
#else
        .dispatch_method = ESP_TIMER_TASK,
#endif
        .name = "intin timer",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&timer_cfg, &hid_intin->timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer create failed");
        goto error;
    }

    hid_class->user_data = hid_intin;

    esp_timer_start_periodic(hid_intin->timer, USBH_GET_URB_INTERVAL(hid_class->intin->bInterval, hid_class->hport->speed));

    return;
error:
    if (hid_intin->buffer) {
        heap_caps_free(hid_intin->buffer);
    }
    heap_caps_free(hid_intin);
}

void usbh_hid_stop(struct usbh_hid *hid_class)
{
    hid_int_in_t *hid_intin = (hid_int_in_t *)hid_class->user_data;
    if (hid_intin) {
        esp_timer_stop(hid_intin->timer);
        esp_timer_delete(hid_intin->timer);
        heap_caps_free(hid_intin->buffer);
        heap_caps_free(hid_intin);
    }
    ESP_LOGW(TAG, "hid stop");
}
