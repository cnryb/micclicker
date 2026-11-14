#pragma once

// FoloToy AI Passport board facts, from the MIT-licensed upstream BSP:
// https://github.com/FoloToy/ai-passport (components/bsp/include/bsp_pins.h),
// pinned to commit 55c7519 (2026-11-14). Pin, bus and voltage-window values below
// match that revision one-for-one; re-check them against the upstream file before
// changing any of them, since older mirrors and search snapshots carry stale values.
// Keep board-specific pins and thresholds here rather than in protocol or audio code.

#include "esp_adc/adc_oneshot.h"

#define MIC_BOARD_LCD_SPI_HOST       1  // SPI2_HOST
#define MIC_BOARD_LCD_MOSI_GPIO      9
#define MIC_BOARD_LCD_SCLK_GPIO      8
#define MIC_BOARD_LCD_CS_GPIO        1
#define MIC_BOARD_LCD_DC_GPIO        20
#define MIC_BOARD_LCD_RESET_GPIO     (-1)  // 屏复位硬接 3.3V，只能走 SWRESET 软复位
#define MIC_BOARD_LCD_PCLK_HZ        (80 * 1000 * 1000)
#define MIC_BOARD_LCD_SPI_MODE       0     // SCK 空闲低、上升沿采样
#define MIC_BOARD_LCD_INVERT_COLOR   1     // 本屏出厂需反色；换屏后若呈负片改为 0
#define MIC_BOARD_BACKLIGHT_GPIO     21

#define MIC_BOARD_I2C_SDA_GPIO       10
#define MIC_BOARD_I2C_SCL_GPIO       7
#define MIC_BOARD_ES8311_ADDR        0x18
#define MIC_BOARD_CW2017_ADDR        0x63

#define MIC_BOARD_I2S_MCLK_GPIO      6
#define MIC_BOARD_I2S_BCLK_GPIO      5
#define MIC_BOARD_I2S_WS_GPIO        3
#define MIC_BOARD_I2S_DOUT_GPIO      2
#define MIC_BOARD_I2S_DIN_GPIO       4
#define MIC_BOARD_I2S_PA_CTRL_GPIO   (-1)  // 功放使能未接 MCU（常通）；接了的板子填对应 GPIO

#define MIC_BOARD_BUTTON_GPIO        0
#define MIC_BOARD_BUTTON_ADC_UNIT    ADC_UNIT_1
#define MIC_BOARD_BUTTON_ADC_CHANNEL ADC_CHANNEL_0
// 三键共用 GPIO0，靠外部 10k 上拉 + 分压电阻分档。不能用内部上拉（约 45k 且离散大），
// 否则三档全挤进 0~154 mV 并随温漂重叠。窗口边界取相邻档中点。
#define MIC_BOARD_BUTTON_MV_UP_MAX   150
#define MIC_BOARD_BUTTON_MV_DOWN_MAX 447
#define MIC_BOARD_BUTTON_MV_OK_MAX   1900
#define MIC_BOARD_BUTTON_MV_RELEASED_MIN 1900  // 高于此值视为全键松开；入睡前据此拒绝带键休眠
#define MIC_BOARD_BUTTON_DEBOUNCE_MS 20
// 电压解码层的按键锁：空闲时新窗口须连续 CONFIRM 才认作按下，锁定后改判须连续 HOLD。
// 用于吸收按住过程中触点短暂断开导致的窗口扫过（组件自身去抖只有约 10 ms）。
#define MIC_BOARD_BUTTON_CONFIRM_MS  40
#define MIC_BOARD_BUTTON_HOLD_MS     120
#define MIC_BOARD_BUTTON_SHORT_PRESS_MS 180
#define MIC_BOARD_BUTTON_LONG_PRESS_MS  500  // 上游把组件默认 1500 ms 收敛到 500 ms

#define MIC_BOARD_USB_DM_GPIO        18
#define MIC_BOARD_USB_DP_GPIO        19
