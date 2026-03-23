#include "usbd_core.h"

// ==============================================================================
// 1. 设备描述符 
// ==============================================================================
static const uint8_t xinput_device_descriptor[] = {
    0x12,                           // bLength (18 bytes)
    0x01,                           // bDescriptorType (Device)
    0x00, 0x02,                     // bcdUSB (USB 2.0)
    0xFF,                           // bDeviceClass (Vendor Specific)
    0xFF,                           // bDeviceSubClass
    0xFF,                           // bDeviceProtocol
    0x40,                           // bMaxPacketSize0 (CherryUSB 默认控制端点大小 64)
    0x5E, 0x04,                     // idVendor (0x045E - Microsoft)
    0x8E, 0x02,                     // idProduct (0x028E - Xbox 360 Controller)
    0x72, 0x05,                     // bcdDevice (0x0572)
    0x01,                           // iManufacturer (String Index 1)
    0x02,                           // iProduct (String Index 2)
    0x03,                           // iSerialNumber (String Index 3)
    0x01                            // bNumConfigurations (1)
};

// ==============================================================================
// 2. 配置描述符 
// ==============================================================================
static const uint8_t xinput_config_descriptor[] = {
    // Configuration Descriptor:
    0x09,   // bLength
    0x02,   // bDescriptorType
    0x30, 0x00, // wTotalLength   (48 bytes)
    0x01,   // bNumInterfaces
    0x01,   // bConfigurationValue
    0x00,   // iConfiguration
    0x80,   // bmAttributes   (Bus-powered Device)
    0xFA,   // bMaxPower      (500 mA)

    // Interface Descriptor:
    0x09,   // bLength
    0x04,   // bDescriptorType
    0x00,   // bInterfaceNumber
    0x00,   // bAlternateSetting
    0x02,   // bNumEndPoints
    0xFF,   // bInterfaceClass      (Vendor specific)
    0x5D,   // bInterfaceSubClass   
    0x01,   // bInterfaceProtocol   
    0x00,   // iInterface

    // Unknown Descriptor: (XInput 专属安全/特性描述符)
    0x10, 0x21, 0x10, 0x01, 0x01, 0x24, 0x81, 0x14, 
    0x03, 0x00, 0x03, 0x13, 0x02, 0x00, 0x03, 0x00, 

    // Endpoint Descriptor (IN):
    0x07,   // bLength
    0x05,   // bDescriptorType
    0x81,   // bEndpointAddress  (IN endpoint 1)
    0x03,   // bmAttributes      (Transfer: Interrupt)
    0x20, 0x00, // wMaxPacketSize    (1 x 32 bytes)
    0x01,   // bInterval         (1 frames)

    // Endpoint Descriptor (OUT):
    0x07,   // bLength
    0x05,   // bDescriptorType
    0x02,   // bEndpointAddress  (OUT endpoint 2)
    0x03,   // bmAttributes      (Transfer: Interrupt)
    0x20, 0x00, // wMaxPacketSize    (1 x 32 bytes)
    0x08,   // bInterval         (8 frames)
};

// ==============================================================================
// 3. 字符串描述符数组
// ==============================================================================
static const char *xinput_string_descriptors[] = {
    (const char[]){0x09, 0x04},        // 0: 支持的语言 ID (0x0409 English)
    "Microsoft",                       // 1: Manufacturer
    "Xbox 360 Controller",             // 2: Product
    "187382748279"                     // 3: Serials
};

// ==============================================================================
// 4. CherryUSB 描述符回调函数
// ==============================================================================
static const uint8_t *xinput_device_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return xinput_device_descriptor;
}

static const uint8_t *xinput_config_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return xinput_config_descriptor;
}

static const char *xinput_string_descriptor_cb(uint8_t speed, uint8_t index)
{
    (void)speed;
    if (index >= (sizeof(xinput_string_descriptors) / sizeof(xinput_string_descriptors[0]))) {
        return NULL;
    }
    return xinput_string_descriptors[index];
}

// ⬇️ 新增：为高速设备提供限定描述符回调，内部返回 NULL 防崩
static const uint8_t *xinput_device_quality_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return NULL; 
}

// ⬇️ 新增：为高速设备提供其他速度描述符回调，内部返回 NULL 防崩
static const uint8_t *xinput_other_speed_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return NULL;
}

// ==============================================================================
// 5. 组装为 CherryUSB 的统一描述符结构
// ==============================================================================
static const struct usb_descriptor xinput_desc = {
    .device_descriptor_callback = xinput_device_descriptor_cb,
    .config_descriptor_callback = xinput_config_descriptor_cb,
    .device_quality_descriptor_callback = xinput_device_quality_descriptor_cb,
    .other_speed_descriptor_callback = xinput_other_speed_descriptor_cb,
    .string_descriptor_callback = xinput_string_descriptor_cb,
    .bos_descriptor = NULL,
    .msosv1_descriptor = NULL,
    .msosv2_descriptor = NULL,
    .webusb_url_descriptor = NULL
};

// ==============================================================================
// 提供给 main.c 调用的注册 API
// ==============================================================================
void xinput_descriptor_init(uint8_t busid)
{
    usbd_desc_register(busid, &xinput_desc);
}