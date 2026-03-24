#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "usbh_core.h"
#include "usbh_hid.h"
#include "usbd_core.h"

extern void xinput_descriptor_init(uint8_t busid);
extern void xinput_init(void);

void usbd_event_handler(uint8_t busid, uint8_t event) {
    (void)busid;
    (void)event;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    xinput_descriptor_init(1);
    vTaskDelay(pdMS_TO_TICKS(300));
    xinput_init();
    vTaskDelay(pdMS_TO_TICKS(300));
    usbd_initialize(1, ESP_USB_FS0_BASE, usbd_event_handler);
    ESP_LOGI("DEVICE", "Init gamepad");

    vTaskDelay(pdMS_TO_TICKS(1000));
    usbh_initialize(0, ESP_USB_HS0_BASE);
    ESP_LOGI("HOST", "Init usb");
}
