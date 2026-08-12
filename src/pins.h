// Pin map for the LilyGO T-Dongle-S3 family.
// Values taken from the vendor factory example:
// https://github.com/Xinyuan-LilyGO/T-Dongle-S3 examples/factory_screen
#pragma once

// ---- Button -------------------------------------------------------------
#define BOOT_PIN 0  // active low

// ---- APA102 status LED --------------------------------------------------
#define LED_DI_PIN 40
#define LED_CI_PIN 39

// ---- ST7735 160x80 LCD (SPI2 / HSPI) ------------------------------------
#define LCD_MOSI_PIN 3
#define LCD_SCLK_PIN 5
#define LCD_CS_PIN   4
#define LCD_DC_PIN   2
#define LCD_RST_PIN  1
#define LCD_BL_PIN   38

#define LCD_WIDTH  160
#define LCD_HEIGHT 80

// The panel sits in the controller's 132x162 RAM with these offsets. In the
// landscape orientation used here (MADCTL MV set) the long axis takes the
// small gap and the short axis the large one.
#define LCD_X_GAP 1
#define LCD_Y_GAP 26

// Backlight is active low on this board.
#define LCD_BL_ACTIVE_LEVEL 0

// ---- microSD (SDMMC, 4-bit) ---------------------------------------------
#define SD_CLK_PIN 12
#define SD_CMD_PIN 16
#define SD_D0_PIN  14
#define SD_D1_PIN  17
#define SD_D2_PIN  21
#define SD_D3_PIN  18

// ---- T-Dongle-S3-Plus only ----------------------------------------------
#ifdef ARDUINO_DONGLES3_PLUS
#define IR_SEND_PIN 7
#define MIC_DATA_PIN 8
#define MIC_SCK_PIN 9
#define I2C_SDA_PIN 11
#define I2C_SCL_PIN 10
#endif
