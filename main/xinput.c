// xinput.c
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "XInputPad.h"
#include "input_bridge.h"
#include "usbd_core.h"
#include "freertos/FreeRTOS.h"
#include <math.h>

// ============================================================
// 端点配置
// ============================================================
#define XINPUT_EP_IN  0x81
#define XINPUT_EP_OUT 0x02
#define XINPUT_INTF_NUM 0
#define XINPUT_BUSID 1

// ============================================================
// Globals
// ============================================================
__attribute__((aligned(64))) ReportDataXinput XboxButtonData;
static volatile bool tx_in_flight = false;

// ============================================================
// Tunables & Xbox 映射宏
// ============================================================
#define XINPUT_MOUSE_TO_STICK_SCALE 48
#define XINPUT_MOUSE_TO_STICK_SCALE_X5 180

#define XINPUT_STICK_MAX  32767
#define XINPUT_STICK_MIN -32767

#define XINPUT_STICK_DIAG  23170
#define XINPUT_STICK_DIAG_N -23170

#define XBOX_BUTTON_UP     (1 << 0)
#define XBOX_BUTTON_DOWN   (1 << 1)
#define XBOX_BUTTON_LEFT   (1 << 2)
#define XBOX_BUTTON_RIGHT  (1 << 3)
#define XBOX_BUTTON_START  (1 << 4)
#define XBOX_BUTTON_BACK   (1 << 5)
#define XBOX_BUTTON_L3     (1 << 6)
#define XBOX_BUTTON_R3     (1 << 7)
#define XBOX_BUTTON_LB     (1 << 8)
#define XBOX_BUTTON_RB     (1 << 9)
#define XBOX_BUTTON_GUIDE  (1 << 10)
#define XBOX_BUTTON_A      (1 << 12)
#define XBOX_BUTTON_B      (1 << 13)
#define XBOX_BUTTON_X      (1 << 14)
#define XBOX_BUTTON_Y      (1 << 15)

/* keycode -> xbox buttons */
static const uint32_t s_key_lut_btn[256] = {
    [0x14] = XBOX_BUTTON_LB,                    // Q
    [0x08] = XBOX_BUTTON_X,                     // E
    [0x15] = XBOX_BUTTON_X,                     // R
    [0x09] = XBOX_BUTTON_R3,                    // F
    [0x0A] = XBOX_BUTTON_RIGHT,                 // G
    [0x17] = XBOX_BUTTON_RIGHT,                 // T
    [0x10] = XBOX_BUTTON_BACK,                  // M
    [0x1D] = XBOX_BUTTON_LB | XBOX_BUTTON_RB,   // Z

    [0x1E] = XBOX_BUTTON_Y,                     // 1
    [0x1F] = XBOX_BUTTON_Y,                     // 2
    [0x20] = XBOX_BUTTON_Y,                     // 3

    [0x2C] = XBOX_BUTTON_A,                     // Space
    [0x2B] = XBOX_BUTTON_START,                 // Tab
    [0x29] = XBOX_BUTTON_START,                 // Esc

    [0x52] = XBOX_BUTTON_UP,                    // Up
    [0x51] = XBOX_BUTTON_DOWN,                  // Down
    [0x50] = XBOX_BUTTON_LEFT,                  // Left
    [0x4F] = XBOX_BUTTON_RIGHT,                 // Right
};

/* keycode -> WASD bitmask */
static const uint8_t s_key_lut_wasd[256] = {
    [0x1A] = 1u << 0,   // W
    [0x04] = 1u << 1,   // A
    [0x16] = 1u << 2,   // S
    [0x07] = 1u << 3,   // D
};

/* WASD -> LX/LY (处理为标准圆形) */
static const int16_t s_lx_lut[16] = {
    0,                  // 0000
    0,                  // 0001: W
    XINPUT_STICK_MIN,   // 0010: A
    XINPUT_STICK_DIAG_N,// 0011: W+A (对角线)
    0,                  // 0100: S
    0,                  // 0101: W+S
    XINPUT_STICK_DIAG_N,// 0110: A+S (对角线)
    XINPUT_STICK_MIN,   // 0111: W+A+S (W和S抵消，纯A)
    XINPUT_STICK_MAX,   // 1000: D
    XINPUT_STICK_DIAG,  // 1001: W+D (对角线)
    0,                  // 1010: A+D
    0,                  // 1011: W+A+D (A和D抵消，X轴为0)
    XINPUT_STICK_DIAG,  // 1100: D+S (对角线)
    XINPUT_STICK_MAX,   // 1101: W+D+S (W和S抵消，纯D)
    0,                  // 1110: A+D+S (A和D抵消，X轴为0)
    0                   // 1111
};

static const int16_t s_ly_lut[16] = {
    0,                  // 0000
    XINPUT_STICK_MAX,   // 0001: W
    0,                  // 0010: A
    XINPUT_STICK_DIAG,  // 0011: W+A (对角线)
    XINPUT_STICK_MIN,   // 0100: S
    0,                  // 0101: W+S
    XINPUT_STICK_DIAG_N,// 0110: A+S (对角线)
    0,                  // 0111: W+A+S (W和S抵消，Y轴为0)
    0,                  // 1000: D
    XINPUT_STICK_DIAG,  // 1001: W+D (对角线)
    0,                  // 1010: A+D
    XINPUT_STICK_MAX,   // 1011: W+A+D (A和D抵消，纯W)
    XINPUT_STICK_DIAG_N,// 1100: D+S (对角线)
    0,                  // 1101: W+D+S (W和S抵消，Y轴为0)
    XINPUT_STICK_MIN,   // 1110: A+D+S (A和D抵消，纯S)
    0                   // 1111
};

// ============================================================
// Helpers
// ============================================================
static inline int16_t clamp_s16(int32_t v)
{
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

// ============================================================
// CherryUSB 底层回调机制
// ============================================================

// IN 端点发送完成回调
static void xinput_in_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    // 数据成功发给主机后，清除标志位，允许发送下一帧
    tx_in_flight = false;
}

// OUT 端点接收回调 (处理主机发来的震动反馈等)
static void xinput_out_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    // 如果有震动数据，可以在这里解析并触发硬件 PWM
    
    // 准备接收下一次 OUT 数据包
    // usbd_ep_start_read(XINPUT_BUSID, XINPUT_EP_OUT, out_buffer, buffer_len); 
}

// 控制传输回调 (处理 EP0 Setup 阶段特有的类请求)
static int xinput_class_interface_request(uint8_t busid, struct usb_setup_packet *setup, uint8_t **data, uint32_t *len)
{
    (void)busid;
    // 大部分常规 XInput 不需要特殊处理，直接返回 0 (通过)
    return 0; 
}

// 静态定义 CherryUSB 结构体
static struct usbd_endpoint xinput_in_ep = {
    .ep_addr = XINPUT_EP_IN,
    .ep_cb = xinput_in_cb
};

static struct usbd_endpoint xinput_out_ep = {
    .ep_addr = XINPUT_EP_OUT,
    .ep_cb = xinput_out_cb
};

static struct usbd_interface xinput_intf;

// 强制内联，减少函数调用开销
static inline void square_to_circle(int16_t x, int16_t y, int16_t *out_x, int16_t *out_y) {
    if (x == 0 && y == 0) {
        *out_x = 0; *out_y = 0;
        return;
    }

    // 1. 映射到浮点空间
    float fx = (float)x;
    float fy = (float)y;

    // 2. 计算当前点到原点的欧几里得距离 (D)
    // ESP32-P4 的 FPU 计算 sqrtf 非常快
    float mag = sqrtf(fx * fx + fy * fy);

    // 3. 计算正方形边缘到原点的距离 (L)
    // 在当前方向上，正方形的边缘跨度由 max(|x|, |y|) 决定
    float ax = fabsf(fx);
    float ay = fabsf(fy);
    float max_side = (ax > ay) ? ax : ay;

    // 4. 核心变换：将长度从 D 压缩/拉伸到 L
    // 这样 45 度角的 (32767, 32767) 会缩放到圆周上
    float rescale = max_side / mag;

    *out_x = (int16_t)(fx * rescale);
    *out_y = (int16_t)(fy * rescale);
}

// ============================================================
// Mapping: Keyboard+Mouse -> XInput report
// ============================================================
static void build_xinput_report(raw_input_state_t const *in, ReportDataXinput *out)
{
  uint8_t wasd = 0;

  // Modifier
  if (in->kbd_modifier & ((1u << 0) | (1u << 4))) out->buttons |= XBOX_BUTTON_B;   // Ctrl
  if (in->kbd_modifier & ((1u << 2) | (1u << 3))) out->buttons |= XBOX_BUTTON_DOWN;// Alt
  if (in->kbd_modifier & ((1u << 1) | (1u << 5))) out->buttons |= XBOX_BUTTON_L3;  // Shift

  int16_t temp_x = 0;
  int16_t temp_y = 0;
  int16_t recoil_offset = 0;

  // keys
  for (uint8_t i = 0; i < 6; i++) {
    uint8_t kc = in->kbd_keycode[i];
    out->buttons  |= s_key_lut_btn[kc];
    wasd          |= s_key_lut_wasd[kc];
  }

  if(in->mouse_buttons & (1u << 4)) {
    out->r_x = s_lx_lut[wasd & 0x0F];
    out->r_y = s_ly_lut[wasd & 0x0F];
    if(in->mouse_wheel < 0) out->r_y = -32767;
    if(in->mouse_wheel > 0) out->r_y = 32767;
    square_to_circle(clamp_s16(in->mouse_dx * XINPUT_MOUSE_TO_STICK_SCALE_X5),
                     clamp_s16(-in->mouse_dy * XINPUT_MOUSE_TO_STICK_SCALE_X5),
                     &temp_x,
                     &temp_y);
    out->l_x = temp_x;
    out->l_y = temp_y;
    if (in->mouse_buttons & (1u << 0)) out->buttons |= XBOX_BUTTON_A;
    if (in->mouse_buttons & (1u << 1)) out->buttons |= XBOX_BUTTON_X;
  } else {
    out->l_x = s_lx_lut[wasd & 0x0F];
    out->l_y = s_ly_lut[wasd & 0x0F];
    if (in->mouse_buttons & (1u << 0)) { // Left
        out->rt = 255;
        recoil_offset = -200;
    } 
    if (in->mouse_buttons & (1u << 1)) { // Right
        out->lt = 255;
        recoil_offset *= 3;
    } 
    if (in->mouse_buttons & (1u << 2)) out->buttons |= XBOX_BUTTON_RB;   // Middle
    if (in->mouse_buttons & (1u << 3)) out->buttons |= XBOX_BUTTON_UP;   // X1
    if (in->mouse_wheel != 0)          out->buttons |= XBOX_BUTTON_Y;
    square_to_circle(clamp_s16(in->mouse_dx * XINPUT_MOUSE_TO_STICK_SCALE),
                    clamp_s16(-in->mouse_dy * XINPUT_MOUSE_TO_STICK_SCALE + recoil_offset),
                    &temp_x,
                    &temp_y);
    out->r_x = temp_x;
    out->r_y = temp_y;
}
}

// ============================================================
// 发送逻辑
// ============================================================
static bool xinput_try_send(void)
{
  // 1. 检查 USB 是否已经枚举配置成功
  if (!usb_device_is_configured(XINPUT_BUSID)) return false;
  
  // 2. 检查底层是否正在发送中
  if (tx_in_flight) return false;

  tx_in_flight = true;
  
  // 3. 将数据压入 CherryUSB 发送队列
  // 注意：XInput 报文长度通常是 20 字节
  int ret = usbd_ep_start_write(XINPUT_BUSID, XINPUT_EP_IN, (uint8_t*)&XboxButtonData, 20);
  if (ret < 0) {
      // 压入失败，恢复空闲状态
      tx_in_flight = false;
      return false;
  }
  
  return true;
}

// 处理 XInput 特有的厂商验证请求 (Vendor Request)
static int xinput_vendor_handler(uint8_t busid, struct usb_setup_packet *setup, uint8_t **data, uint32_t *len)
{
    (void)busid;

    // 1. 处理 wValue 0x0000 (通常是 XInput 初始查询)
    if (setup->bmRequestType == 0xC1 && setup->bRequest == 0x01 && setup->wValue == 0x0000) {
        // 这是标准的 XInput 响应数据 (长度 8)
        static __attribute__((aligned(64))) uint8_t xinput_init_rsp[8] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        *data = xinput_init_rsp;
        *len = setup->wLength; // 对应请求的 wLength (0x0008)
        return 0; 
    }

    // 2. 处理 wValue 0x0100 (XInput 特性/信息查询)
    if (setup->bmRequestType == 0xC1 && setup->bRequest == 0x01 && setup->wValue == 0x0100) {
        // 这是通过抓包提取的标准 XInput 欺骗/特性响应数据 (长度 20)
        static __attribute__((aligned(64))) uint8_t xinput_info[20] = {
            0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
        };
        *data = xinput_info;
        *len = setup->wLength; // 对应请求的 wLength (0x0014 = 20)
        return 0; 
    }

    // 如果是其他不认识的厂商请求，返回 -1 让底层回复 STALL
    return -1; 
}

void xinput_task(void *pvParameters)
{
    (void)pvParameters;

    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
        raw_input_state_t in;
        
        // 从你的桥接层获取最新的键鼠状态
        bridge_get_snapshot(&in);
        
        // 清空并重新构建手柄报文
        memset(&XboxButtonData, 0, sizeof(XboxButtonData));
        build_xinput_report(&in, &XboxButtonData);
        
        // 尝试发送
        (void)xinput_try_send();
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1)); 
    }
}

// ============================================================
// Public API called by main.c
// ============================================================

void xinput_init(void)
{
  memset(&XboxButtonData, 0, sizeof(XboxButtonData));
  tx_in_flight = false;

  // 1. 最好先清空整个结构体，防止内存里的脏数据导致指针异常 (如 vendor_handler 变成野指针)
  memset(&xinput_intf, 0, sizeof(struct usbd_interface));

  // 2. 注册接口回调和【接口编号】
  xinput_intf.class_interface_handler = xinput_class_interface_request;
  xinput_intf.vendor_handler = xinput_vendor_handler;
  xinput_intf.intf_num = XINPUT_INTF_NUM;

  // (可选) 如果你想监听 host 的 Set_Configuration 或者 Reset 事件，可以挂载 notify_handler
  // xinput_intf.notify_handler = xinput_notify_handler;

  // 3. 将接口注册到总线
  usbd_add_interface(XINPUT_BUSID, &xinput_intf);

  // 4. 注册端点
  usbd_add_endpoint(XINPUT_BUSID, &xinput_in_ep);
  usbd_add_endpoint(XINPUT_BUSID, &xinput_out_ep);

  xTaskCreatePinnedToCore(xinput_task, "xinput_task", 8192, NULL, 5, NULL, 1);
}