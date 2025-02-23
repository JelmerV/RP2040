/*

  driver.c - driver code for RP2040 ARM processors

  Part of grblHAL

  Copyright (c) 2021 Volksolive
  Copyright (c) 2021-2024 Terje Io

  grblHAL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  grblHAL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with grblHAL. If not, see <http://www.gnu.org/licenses/>.

*/

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <malloc.h>

#include "pico/time.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"
#include "hardware/rtc.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/iobank0.h"
#include "hardware/structs/sio.h"

#include "driver.h"
#include "serial.h"
#include "driverPIO.pio.h"
#include "ws2812.pio.h"

#include "grbl/crossbar.h"
#include "grbl/machine_limits.h"
#include "grbl/state_machine.h"
#include "grbl/motor_pins.h"
#include "grbl/pin_bits_masks.h"
#include "grbl/protocol.h"

#ifdef I2C_PORT
#include "i2c.h"
#endif

#if SDCARD_ENABLE
#include "sdcard/sdcard.h"
#include "ff.h"
#include "diskio.h"
#endif

#if LITTLEFS_ENABLE
#include "littlefs_hal.h"
#include "sdcard/fs_littlefs.h"
#endif

#if USB_SERIAL_CDC
#include "usb_serial.h"
#endif

#if EEPROM_ENABLE
#include "eeprom/eeprom.h"
#endif

#if KEYPAD_ENABLE == 2
#include "keypad/keypad.h"
#endif

#if ODOMETER_ENABLE
#include "odometer/odometer.h"
#endif

#if PPI_ENABLE
#include "laser/ppi.h"
#endif

#if FLASH_ENABLE
#include "flash.h"
#endif

#if IOEXPAND_ENABLE
#include "ioexpand.h"
#endif

#if WIFI_ENABLE
#include "wifi.h"
#include "pico/cyw43_arch.h"
#endif

#if ETHERNET_ENABLE
#include "networking/wiznet/enet.h"
#endif

#if BLUETOOTH_ENABLE == 1
#include "bluetooth.h"
#endif

#ifdef GPIO_PIO_1
static uint x_step_sm;
static uint y_step_sm;
static uint z_step_sm;
static PIO z_step_pio;
#ifdef X2_STEP_PIN
static uint x2_step_sm;
#endif
#ifdef Y2_STEP_PIN
static uint y2_step_sm;
#endif
#ifdef Z2_STEP_PIN
static uint z2_step_sm;
#endif
#ifdef A_STEP_PIN
static uint a_step_sm;
#endif
#ifdef B_STEP_PIN
static uint b_step_sm;
#endif
#ifdef C_STEP_PIN
static uint c_step_sm;
#endif
#endif

#ifdef NEOPIXELS_PIN

#ifndef NEOPIXELS_NUM
#define NEOPIXELS_NUM 1
#endif

static PIO neop_pio;
static uint neop_sm;

#endif

typedef union
{
    uint32_t value;
    struct
    {
        uint32_t delay  :8,
                 length :8,
                 set    :6,
                 reset  :6;
    };
} pio_steps_t;

#if ETHERNET_ENABLE || WIFI_ENABLE || BLUETOOTH_ENABLE == 1

typedef union
{
    uint8_t value;
    struct
    {
        uint8_t wifi      :1,
                bluetooth :1,
                ethernet  :1,
                unused    :5;
    };
} net_types_t;

static net_types_t net = {0};

#endif

#if DRIVER_SPINDLE_ENABLE
static spindle_id_t spindle_id = -1;
#endif
#if DRIVER_SPINDLE_PWM_ENABLE
static bool pwmEnabled = false;
static spindle_pwm_t spindle_pwm;
#define pwm(s) ((spindle_pwm_t *)s->context)
#endif

static pio_steps_t pio_steps = {.delay = 20, .length = 100};
static uint stepper_timer_sm, stepper_timer_sm_offset;
static uint16_t pulse_length, pulse_delay;
static bool IOInitDone = false;
static status_code_t (*on_unknown_sys_command)(uint_fast16_t state, char *line, char *lcline);
static volatile uint32_t elapsed_ticks = 0;
static probe_state_t probe = { .connected = On };
static pin_group_pins_t limit_inputs;
#ifdef SAFETY_DOOR_BIT
static input_signal_t *safety_door;
#endif

#if IOEXPAND_ENABLE
static ioexpand_t io_expander = {0};
#endif

#ifdef NEOPIXELS_PIN
neopixel_cfg_t neopixel = { .intensity = 255 };
#endif

#include "grbl/stepdir_map.h"

static periph_signal_t *periph_pins = NULL;

static input_signal_t inputpin[] = {
#ifdef RESET_PIN
#if ESTOP_ENABLE
    { .id = Input_EStop, .port = GPIO_INPUT, .pin = RESET_PIN, .group = PinGroup_Control },
#else
    { .id = Input_Reset, .port = GPIO_INPUT, .pin = RESET_PIN, .group = PinGroup_Control },
#endif
#endif
#ifdef FEED_HOLD_PIN
    { .id = Input_FeedHold, .port = GPIO_INPUT, .pin = FEED_HOLD_PIN, .group = PinGroup_Control },
#endif
#ifdef CYCLE_START_PIN
    { .id = Input_CycleStart, .port = GPIO_INPUT, .pin = CYCLE_START_PIN, .group = PinGroup_Control },
#endif
#if SAFETY_DOOR_BIT
    { .id = Input_SafetyDoor, .port = GPIO_INPUT, .pin = SAFETY_DOOR_PIN, .group = PinGroup_Control },
#endif
#ifdef LIMITS_OVERRIDE_PIN
    { .id = Input_LimitsOverride, .port = GPIO_INPUT, .pin = LIMITS_OVERRIDE_PIN, .group = PinGroup_Control },
#endif
#ifdef PROBE_PIN
    { .id = Input_Probe, .port = GPIO_INPUT, .pin = PROBE_PIN, .group = PinGroup_Probe },
#endif
    { .id = Input_LimitX, .port = GPIO_INPUT, .pin = X_LIMIT_PIN, .group = PinGroup_Limit },
#ifdef X2_LIMIT_PIN
    { .id = Input_LimitX_2, .port = GPIO_INPUT, .pin = X2_LIMIT_PIN, .group = PinGroup_Limit },
#endif
    { .id = Input_LimitY, .port = GPIO_INPUT, .pin = Y_LIMIT_PIN, .group = PinGroup_Limit },
#ifdef Y2_LIMIT_PIN
    { .id = Input_LimitY_Max, .port = GPIO_INPUT, .pin = Y2_LIMIT_PIN, .group = PinGroup_Limit },
#endif
    { .id = Input_LimitZ, .port = GPIO_INPUT, .pin = Z_LIMIT_PIN, .group = PinGroup_Limit },
#ifdef Z2_LIMIT_PIN
    { .id = Input_LimitZ_Max, .port = GPIO_INPUT, .pin = Z2_LIMIT_PIN, .group = PinGroup_Limit },
#endif
#ifdef A_LIMIT_PIN
    { .id = Input_LimitA, .port = GPIO_INPUT, .pin = A_LIMIT_PIN, .group = PinGroup_Limit },
#endif
#ifdef B_LIMIT_PIN
    { .id = Input_LimitB, .port = GPIO_INPUT, .pin = B_LIMIT_PIN, .group = PinGroup_Limit },
#endif
#ifdef C_LIMIT_PIN
    { .id = Input_LimitC, .port = GPIO_INPUT, .pin = C_LIMIT_PIN, .group = PinGroup_Limit },
#endif
#if MPG_MODE_PIN
    { .id = Input_MPGSelect, .port = GPIO_INPUT, .pin = MPG_MODE_PIN, .group = PinGroup_MPG },
#endif
#if I2C_STROBE_ENABLE && defined(I2C_STROBE_PIN)
    { .id = Input_I2CStrobe, .port = GPIO_INPUT, .pin = I2C_STROBE_PIN, .group = PinGroup_I2C },
#endif
#ifdef SPI_IRQ_PIN
    { .id = Input_SPIIRQ,    .port = GPIO_INPUT, .pin = SPI_IRQ_PIN,    .group = PinGroup_SPI },
#endif
#ifdef AUXINPUT0_PIN
    { .id = Input_Aux0, .port = GPIO_INPUT, .pin = AUXINPUT0_PIN, .group = PinGroup_AuxInput },
#endif
#ifdef AUXINPUT1_PIN
    { .id = Input_Aux1, .port = GPIO_INPUT, .pin = AUXINPUT1_PIN, .group = PinGroup_AuxInput },
#endif
#ifdef AUXINPUT2_PIN
    { .id = Input_Aux2, .port = GPIO_INPUT, .pin = AUXINPUT2_PIN, .group = PinGroup_AuxInput },
#endif
#ifdef AUXINPUT3_PIN
    { .id = Input_Aux3, .port = GPIO_INPUT, .pin = AUXINPUT3_PIN, .group = PinGroup_AuxInput },
#endif
#ifdef AUXINPUT4_PIN
    { .id = Input_Aux4, .port = GPIO_INPUT, .pin = AUXINPUT4_PIN, .group = PinGroup_AuxInput },
#endif
#ifdef AUXINPUT5_PIN
    { .id = Input_Aux5, .port = GPIO_INPUT, .pin = AUXINPUT5_PIN, .group = PinGroup_AuxInput },
#endif
#ifdef AUXINPUT6_PIN
    { .id = Input_Aux6, .port = GPIO_INPUT, .pin = AUXINPUT6_PIN, .group = PinGroup_AuxInput },
#endif
#ifdef AUXINPUT7_PIN
    { .id = Input_Aux7, .port = GPIO_INPUT, .pin = AUXINPUT7_PIN, .group = PinGroup_AuxInput }
#endif

#ifdef AUXINPUT_ANALOG0_PIN
    { .id = Input_Analog_Aux0, .port = AUXINPUT_ANALOG0_PORT, .pin = AUXINPUT_ANALOG0_PIN, .group = PinGroup_AuxInputAnalog }
#endif
#ifdef AUXINPUT_ANALOG1_PIN
    { .id = Input_Analog_Aux1, .port = AUXINPUT_ANALOG1_PORT, .pin = AUXINPUT_ANALOG1_PIN, .group = PinGroup_AuxInputAnalog }
#endif
#ifdef AUXINPUT_ANALOG2_PIN
    { .id = Input_Analog_Aux2, .port = AUXINPUT_ANALOG2_PORT, .pin = AUXINPUT_ANALOG2_PIN, .group = PinGroup_AuxInputAnalog }
#endif
#ifdef AUXINPUT_ANALOG3_PIN
    { .id = Input_Analog_Aux3, .port = AUXINPUT_ANALOG3_PORT, .pin = AUXINPUT_ANALOG3_PIN, .group = PinGroup_AuxInputAnalog }
#endif
};


#if STEP_PORT == GPIO_SR8
#if N_ABC_MOTORS > 1
#error "Axis configuration is not supported!"
#endif
#define X_STEP_PIN 0
#define Y_STEP_PIN 1
#define Z_STEP_PIN 2
#ifdef X2_STEP_PORT
#undef X2_STEP_PIN
#define X2_STEP_PIN 3
#endif
#ifdef Y2_STEP_PORT
#undef Y2_STEP_PIN
#define Y2_STEP_PIN 3
#endif
#ifdef Z2_STEP_PORT
#undef Z2_STEP_PIN
#define Z2_STEP_PIN 3
#endif
#ifdef A_STEP_PORT
#undef A_STEP_PIN
#define A_STEP_PIN 3
#endif
#endif

#if DIRECTION_PORT == GPIO_SR8
#if N_ABC_MOTORS > 1
#error "Axis configuration is not supported!"
#endif
#define X_DIRECTION_PIN 4
#define Y_DIRECTION_PIN 5
#define Z_DIRECTION_PIN 6
#ifdef X2_DIRECTION_PORT
#undef X2_DIRECTION_PIN
#define X2_DIRECTION_PIN 7
#endif
#ifdef Y2_DIRECTION_PORT
#undef Y2_DIRECTION_PIN
#define Y2_DIRECTION_PIN 7
#endif
#ifdef Z2_DIRECTION_PORT
#undef Z2_DIRECTION_PIN
#define Z2_DIRECTION_PIN 7
#endif
#ifdef A_DIRECTION_PORT
#undef A_DIRECTION_PIN
#define A_DIRECTION_PIN 7
#endif
#endif

#if ENABLE_PORT == GPIO_SR16
#if N_ABC_MOTORS > 1
#error "Axis configuration is not supported!"
#endif
#define X_ENABLE_PIN 0
#define Y_ENABLE_PIN 1
#define Z_ENABLE_PIN 2
#ifdef X2_ENABLE_PORT
#undef X2_ENABLE_PIN
#define X2_ENABLE_PIN 3
#endif
#ifdef Y2_ENABLE_PORT
#undef Y2_ENABLE_PIN
#define Y2_ENABLE_PIN 3
#endif
#ifdef Z2_ENABLE_PORT
#undef Z2_ENABLE_PIN
#define Z2_ENABLE_PIN 3
#endif
#ifdef A_ENABLE_PORT
#undef A_ENABLE_PIN
#define A_ENABLE_PIN 3
#endif
#endif

#ifndef SPI_RST_PORT
#define SPI_RST_PORT GPIO_OUTPUT
#endif

static output_signal_t outputpin[] = {
    { .id = Output_StepX,   .port = STEP_PORT,      .pin = X_STEP_PIN,       .group = PinGroup_StepperStep,    .mode = { STEP_PINMODE } },
    { .id = Output_StepY,   .port = STEP_PORT,      .pin = Y_STEP_PIN,       .group = PinGroup_StepperStep,    .mode = { STEP_PINMODE} },
    { .id = Output_StepZ,   .port = STEP_PORT,      .pin = Z_STEP_PIN,       .group = PinGroup_StepperStep,    .mode = { STEP_PINMODE } },
#ifdef A_STEP_PIN
    { .id = Output_StepA,   .port = STEP_PORT,      .pin = A_STEP_PIN,       .group = PinGroup_StepperStep,    .mode = { STEP_PINMODE } },
#endif
#ifdef B_STEP_PIN
    { .id = Output_StepB,   .port = STEP_PORT,      .pin = B_STEP_PIN,       .group = PinGroup_StepperStep,    .mode = { STEP_PINMODE } },
#endif
#ifdef C_STEP_PIN
    { .id = Output_StepC,   .port = STEP_PORT,      .pin = C_STEP_PIN,       .group = PinGroup_StepperStep,    .mode =  {STEP_PINMODE } },
#endif
#ifdef X2_STEP_PIN
    { .id = Output_StepX_2, .port = STEP_PORT,      .pin = X2_STEP_PIN,      .group = PinGroup_StepperStep,    .mode = { STEP_PINMODE } },
#endif
#ifdef Y2_STEP_PIN
    { .id = Output_StepY_2, .port = STEP_PORT,      .pin = Y2_STEP_PIN,      .group = PinGroup_StepperStep,    .mode = { STEP_PINMODE } },
#endif
#ifdef Z2_STEP_PIN
    { .id = Output_StepZ_2, .port = STEP_PORT,      .pin = Z2_STEP_PIN,      .group = PinGroup_StepperStep,    .mode = { STEP_PINMODE } },
#endif
    { .id = Output_DirX,    .port = DIRECTION_PORT, .pin = X_DIRECTION_PIN,  .group = PinGroup_StepperDir,     .mode = { DIRECTION_PINMODE } },
    { .id = Output_DirY,    .port = DIRECTION_PORT, .pin = Y_DIRECTION_PIN,  .group = PinGroup_StepperDir,     .mode = { DIRECTION_PINMODE } },
    { .id = Output_DirZ,    .port = DIRECTION_PORT, .pin = Z_DIRECTION_PIN,  .group = PinGroup_StepperDir,     .mode = { DIRECTION_PINMODE } },
#ifdef A_DIRECTION_PIN
    { .id = Output_DirA,    .port = DIRECTION_PORT, .pin = A_DIRECTION_PIN,  .group = PinGroup_StepperDir,     .mode = { DIRECTION_PINMODE } },
#endif
#ifdef B_DIRECTION_PIN
    { .id = Output_DirB,    .port = DIRECTION_PORT, .pin = B_DIRECTION_PIN,  .group = PinGroup_StepperDir,     .mode = { DIRECTION_PINMODE } },
#endif
#ifdef C_DIRECTION_PIN
    { .id = Output_DirC,    .port = DIRECTION_PORT, .pin = C_DIRECTION_PIN,  .group = PinGroup_StepperDir,     .mode = { DIRECTION_PINMODE } },
#endif
#ifdef X2_DIRECTION_PIN
    { .id = Output_DirX_2,  .port = DIRECTION_PORT, .pin = X2_DIRECTION_PIN, .group = PinGroup_StepperDir,     .mode = {DIRECTION_PINMODE } },
#endif
#ifdef Y2_DIRECTION_PIN
    { .id = Output_DirY_2,  .port = DIRECTION_PORT, .pin = Y2_DIRECTION_PIN, .group = PinGroup_StepperDir,     .mode = { DIRECTION_PINMODE } },
#endif
#ifdef Z2_DIRECTION_PIN
    { .id = Output_DirZ_2,  .port = DIRECTION_PORT, .pin = Z2_DIRECTION_PIN, .group = PinGroup_StepperDir,     .mode = { DIRECTION_PINMODE } },
#endif
#if !(TRINAMIC_ENABLE && TRINAMIC_I2C)
#ifndef STEPPERS_ENABLE_PIN
#ifdef X_ENABLE_PIN
    { .id = Output_StepperEnableX, .port = ENABLE_PORT, .pin = X_ENABLE_PIN, .group = PinGroup_StepperEnable,  .mode = { STEPPERS_ENABLE_PINMODE } },
#endif
#ifdef Y_ENABLE_PIN
    { .id = Output_StepperEnableY, .port = ENABLE_PORT, .pin = Y_ENABLE_PIN, .group = PinGroup_StepperEnable,  .mode = { STEPPERS_ENABLE_PINMODE } },
#endif
#ifdef Z_ENABLE_PIN
    { .id = Output_StepperEnableZ, .port = ENABLE_PORT, .pin = Z_ENABLE_PIN, .group = PinGroup_StepperEnable,  .mode = { STEPPERS_ENABLE_PINMODE } },
#endif
#ifdef X2_ENABLE_PIN
    { .id = Output_StepperEnableX, .port = ENABLE_PORT, .pin = X2_ENABLE_PIN, .group = PinGroup_StepperEnable, .mode = { STEPPERS_ENABLE_PINMODE } },
#endif
#ifdef Y2_ENABLE_PIN
    { .id = Output_StepperEnableY, .port = ENABLE_PORT, .pin = Y2_ENABLE_PIN, .group = PinGroup_StepperEnable, .mode = { STEPPERS_ENABLE_PINMODE } },
#endif
#ifdef Z2_ENABLE_PIN
    { .id = Output_StepperEnableZ, .port = ENABLE_PORT, .pin = Z2_ENABLE_PIN, .group = PinGroup_StepperEnable, .mode = { STEPPERS_ENABLE_PINMODE } },
#endif
#ifdef A_ENABLE_PIN
    { .id = Output_StepperEnableA, .port = ENABLE_PORT, .pin = A_ENABLE_PIN,  .group = PinGroup_StepperEnable, .mode = { STEPPERS_ENABLE_PINMODE } },
#endif
#ifdef B_ENABLE_PIN
    { .id = Output_StepperEnableB, .port = ENABLE_PORT, .pin = B_ENABLE_PIN,  .group = PinGroup_StepperEnable, .mode = { STEPPERS_ENABLE_PINMODE } },
#endif
#ifdef C_ENABLE_PIN
    { .id = Output_StepperEnableC, .port = ENABLE_PORT, .pin = C_ENABLE_PIN,  .group = PinGroup_StepperEnable, .mode = { STEPPERS_ENABLE_PINMODE } },
#endif
#else // STEPPERS_ENABLE_PIN
    { .id = Output_StepperEnable,  .port = ENABLE_PORT, .pin = STEPPERS_ENABLE_PIN, .group = PinGroup_StepperEnable, .mode = { STEPPERS_ENABLE_PINMODE } },
#endif
#endif // !(TRINAMIC_ENABLE && TRINAMIC_I2C)
#ifdef SPINDLE_PWM_PIN
    { .id = Output_SpindlePWM,   .port = SPINDLE_PWM_PORT, .pin = SPINDLE_PWM_PIN,       .group = PinGroup_SpindlePWM },
#endif
#ifdef RTS_PIN
    { .id = Output_RTS,          .port = GPIO_OUTPUT,      .pin = RTS_PIN,               .group = PinGroup_UART },
#endif
#ifdef SD_CS_PIN
    { .id = Output_SdCardCS,     .port = GPIO_OUTPUT,      .pin = SD_CS_PIN,             .group = PinGroup_SdCard },
#endif
#ifdef SPI_CS_PIN
    { .id = Output_SPICS,        .port = GPIO_OUTPUT,      .pin = SPI_CS_PIN,            .group = PinGroup_SPI },
#endif
#ifdef SPI_RST_PIN
    { .id = Output_SPIRST,       .port = SPI_RST_PORT,     .pin = SPI_RST_PIN,           .group = PinGroup_SPI },
#endif
#ifndef SD_SHIFT_REGISTER
#ifdef SPINDLE_ENABLE_PIN
    { .id = Output_SpindleOn,    .port = SPINDLE_PORT,     .pin = SPINDLE_ENABLE_PIN,    .group = PinGroup_SpindleControl},
#endif
#ifdef SPINDLE_DIRECTION_PIN
    { .id = Output_SpindleDir,   .port = SPINDLE_PORT,     .pin = SPINDLE_DIRECTION_PIN, .group = PinGroup_SpindleControl},
#endif
#ifdef COOLANT_FLOOD_PIN
    { .id = Output_CoolantFlood, .port = COOLANT_PORT,     .pin = COOLANT_FLOOD_PIN,     .group = PinGroup_Coolant},
#endif
#ifdef COOLANT_MIST_PIN
    { .id = Output_CoolantMist,  .port = COOLANT_PORT,     .pin = COOLANT_MIST_PIN,      .group = PinGroup_Coolant},
#endif
#ifdef LED_PIN
    { .id = Output_LED,          .port = GPIO_OUTPUT,      .pin = LED_PIN,               .group = PinGroup_LED },
#endif
#ifdef LED_R_PIN
    { .id = Output_LED_R,        .port = GPIO_OUTPUT,      .pin = LED_R_PIN,             .group = PinGroup_LED },
#endif
#ifdef LED_G_PIN
    { .id = Output_LED_G,        .port = GPIO_OUTPUT,      .pin = LED_G_PIN,             .group = PinGroup_LED },
#endif
#ifdef LED_B_PIN
    { .id = Output_LED_B,        .port = GPIO_OUTPUT,      .pin = LED_B_PIN,             .group = PinGroup_LED },
#endif
#ifdef LED_W_PIN
    { .id = Output_LED_W,        .port = GPIO_OUTPUT,      .pin = LED_W_PIN,             .group = PinGroup_LED },
#endif
#ifdef AUXOUTPUT0_PORT
    { .id = Output_Aux0,         .port = AUXOUTPUT0_PORT,  .pin = AUXOUTPUT0_PIN,        .group = PinGroup_AuxOutput},
#endif
#ifdef AUXOUTPUT1_PORT
    { .id = Output_Aux1,         .port = AUXOUTPUT1_PORT,  .pin = AUXOUTPUT1_PIN,        .group = PinGroup_AuxOutput},
#endif
#ifdef AUXOUTPUT2_PORT
    { .id = Output_Aux2,         .port = AUXOUTPUT2_PORT,  .pin = AUXOUTPUT2_PIN,        .group = PinGroup_AuxOutput},
#endif
#ifdef AUXOUTPUT3_PORT
    { .id = Output_Aux3,         .port = AUXOUTPUT3_PORT,  .pin = AUXOUTPUT3_PIN,        .group = PinGroup_AuxOutput},
#endif
#ifdef AUXOUTPUT4_PORT
    { .id = Output_Aux4,         .port = AUXOUTPUT4_PORT,  .pin = AUXOUTPUT4_PIN,        .group = PinGroup_AuxOutput},
#endif
#ifdef AUXOUTPUT5_PORT
    { .id = Output_Aux5,         .port = AUXOUTPUT5_PORT,  .pin = AUXOUTPUT5_PIN,        .group = PinGroup_AuxOutput},
#endif
#ifdef AUXOUTPUT6_PORT
    { .id = Output_Aux6,         .port = AUXOUTPUT6_PORT,  .pin = AUXOUTPUT6_PIN,        .group = PinGroup_AuxOutput},
#endif
#ifdef AUXOUTPUT7_PORT
    { .id = Output_Aux7,         .port = AUXOUTPUT7_PORT,  .pin = AUXOUTPUT7_PIN,        .group = PinGroup_AuxOutput},
#endif
#else // SD_SHIFT_REGISTER pin definitions - for $pins command only
    { .id = Output_SpindleOn,    .port = GPIO_SR16, .pin = 4,  .group = PinGroup_SpindleControl },
    { .id = Output_SpindleDir,   .port = GPIO_SR16, .pin = 5,  .group = PinGroup_SpindleControl },
    { .id = Output_CoolantFlood, .port = GPIO_SR16, .pin = 6,  .group = PinGroup_Coolant },
    { .id = Output_CoolantMist,  .port = GPIO_SR16, .pin = 7,  .group = PinGroup_Coolant },
    { .id = Output_Aux0,         .port = GPIO_SR16, .pin = 8,  .group = PinGroup_AuxOutput },
    { .id = Output_Aux1,         .port = GPIO_SR16, .pin = 9,  .group = PinGroup_AuxOutput },
    { .id = Output_Aux2,         .port = GPIO_SR16, .pin = 10, .group = PinGroup_AuxOutput },
    { .id = Output_Aux3,         .port = GPIO_SR16, .pin = 11, .group = PinGroup_AuxOutput },
    { .id = Output_Aux4,         .port = GPIO_SR16, .pin = 12, .group = PinGroup_AuxOutput },
    { .id = Output_Aux5,         .port = GPIO_SR16, .pin = 13, .group = PinGroup_AuxOutput },
    { .id = Output_Aux6,         .port = GPIO_SR16, .pin = 14, .group = PinGroup_AuxOutput },
#ifdef AUXOUTPUT7_PORT
    { .id = Output_Aux7,         .port = AUXOUTPUT7_PORT, .pin = AUXOUTPUT7_PIN, .group = PinGroup_AuxOutput},
#endif
    { .id = Output_SPIRST,       .port = GPIO_SR16, .pin = 15, .group = PinGroup_SPI },
#endif // SD_SHIFT_REGISTER
#ifdef AUXOUTPUT0_PWM_PIN
    { .id = Output_Analog_Aux0, .port = GPIO_OUTPUT, .pin = AUXOUTPUT0_PWM_PIN, .group = PinGroup_AuxOutputAnalog, .mode = { PINMODE_PWM } },
#endif
#ifdef AUXOUTPUT1_PWM_PIN
    { .id = Output_Analog_Aux1, .port = GPIO_OUTPUT, .pin = AUXOUTPUT1_PWM_PIN, .group = PinGroup_AuxOutputAnalog, .mode = { PINMODE_PWM } },
#endif
#ifdef AUXOUTPUT2_PWM_PIN
    { .id = Output_Analog_Aux2, .port = GPIO_OUTPUT, .pin = AUXOUTPUT2_PWM_PIN, .group = PinGroup_AuxOutputAnalog, .mode = { PINMODE_PWM } },
#endif
#ifdef AUXOUTPUT3_PWM_PIN
    { .id = Output_Analog_Aux3, .port = GPIO_OUTPUT, .pin = AUXOUTPUT3_PWM_PIN, .group = PinGroup_AuxOutputAnalog, .mode = { PINMODE_PWM } },
#endif
};

#ifndef I2C_STROBE_BIT
#define I2C_STROBE_BIT 0
#endif

#ifndef SPI_IRQ_BIT
#define SPI_IRQ_BIT 0
#endif

// This should be a sdk function but it doesn't exist yet
#define gpio_set_irqover(gpio, value) hw_write_masked(&iobank0_hw->io[gpio].ctrl, value << IO_BANK0_GPIO0_CTRL_IRQOVER_LSB, IO_BANK0_GPIO0_CTRL_IRQOVER_BITS);

#define NVIC_HIGH_LEVEL_PRIORITY 0xC0
#define NVIC_MEDIUM_LEVEL_PRIORITY 0x80
#define NVIC_LOW_LEVEL_PRIORITY 0x40

#define DRIVER_IRQMASK (LIMIT_MASK | CONTROL_MASK | I2C_STROBE_BIT | SPI_IRQ_BIT | SPINDLE_INDEX_BIT)

#ifndef DEBOUNCE_DELAY
#define DEBOUNCE_DELAY 40 // ms
#endif

/*
#define DEBOUNCE_ALARM_HW_TIMER 0   // Hardware alarm timer 0 used for the debounce alarm pool
#define DEBOUNCE_ALARM_MAX_TIMER 16 // Maximum number of alarm timer in the debounce alarm pool (based on SDK 'PICO_TIME_DEFAULT_ALARM_POOL_MAX_TIMERS 16' for default pool used for driver_delay in driver.c)

typedef struct {
    alarm_id_t id;
    uint8_t pin;
    uint8_t level;
} debounce_pool_t;
static alarm_pool_t *debounceAlarmPool;
static volatile debounce_pool_t debounceAlarmPoolArray[DEBOUNCE_ALARM_MAX_TIMER];

*/

#if SD_SHIFT_REGISTER
static step_dir_sr_t sd_sr;
#endif

#if OUT_SHIFT_REGISTER
static uint32_t out_sr_sm;
static output_sr_t out_sr;
#endif

static void systick_handler(void);
static void stepper_int_handler(void);
static void gpio_int_handler(uint gpio, uint32_t events);

#if I2C_STROBE_BIT || SPI_IRQ_BIT

#if I2C_STROBE_BIT
static driver_irq_handler_t i2c_strobe = { .type = IRQ_I2C_Strobe };
#endif

#if SPI_IRQ_BIT
static driver_irq_handler_t spi_irq = { .type = IRQ_SPI };
#endif

static bool irq_claim (irq_type_t irq, uint_fast8_t id, irq_callback_ptr handler)
{
    bool ok = false;

    switch(irq) {

#if I2C_STROBE_BIT
        case IRQ_I2C_Strobe:
            if((ok = i2c_strobe.callback == NULL))
                i2c_strobe.callback = handler;
            break;
#endif

#if SPI_IRQ_BIT
        case IRQ_SPI:
            if((ok = spi_irq.callback == NULL))
                spi_irq.callback = handler;
            break;
#endif

        default:
            break;
    }

    return ok;
}

#endif // I2C_STROBE_BIT || SPI_IRQ_BIT

static int64_t delay_callback (alarm_id_t id, void *callback)
{
    ((delay_callback_ptr)callback)();

    return 0;
}

static void driver_delay (uint32_t ms, delay_callback_ptr callback)
{
    if (ms > 0) {
        if (callback)
            add_alarm_in_ms(ms, delay_callback, callback, false);
        else {
            uint32_t delay = ms * 1000, start = timer_hw->timerawl;
            while (timer_hw->timerawl - start < delay)
                grbl.on_execute_delay(state_get());
        }
    }
    else if (callback)
        callback();
    tight_loop_contents();
}

//*************************  STEPPER  *************************//

// Enable/disable stepper motors
static void stepperEnable (axes_signals_t enable)
{
    enable.mask ^= settings.steppers.enable_invert.mask;
#if TRINAMIC_ENABLE && TRINAMIC_I2C
    axes_signals_t tmc_enable = trinamic_stepper_enable(enable);
#elif ENABLE_PORT == GPIO_OUTPUT
#ifndef STEPPERS_ENABLE_PIN
    gpio_put(X_ENABLE_PIN, enable.x);
#ifdef Y_ENABLE_PIN
    gpio_put(Y_ENABLE_PIN, enable.y);
#endif
    gpio_put(Z_ENABLE_PIN, enable.z);
#ifdef X2_ENABLE_PIN
    gpio_put(X2_ENABLE_PIN, enable.x);
#endif
#ifdef Y2_ENABLE_PIN
    gpio_put(Y2_ENABLE_PIN, enable.y);
#endif
#ifdef Z2_ENABLE_PIN
    gpio_put(Z2_ENABLE_PIN, enable.z);
#endif
#ifdef A_ENABLE_PIN
    gpio_put(A_ENABLE_PIN, enable.a);
#endif
#ifdef B_ENABLE_PIN
    gpio_put(B_ENABLE_PIN, enable.b);
#endif
#ifdef C_ENABLE_PIN
    gpio_put(C_ENABLE_PIN, enable.c);
#endif
#else // STEPPERS_ENABLE_PIN
    gpio_put(STEPPERS_ENABLE_PIN, enable.x);
#endif
#elif ENABLE_PORT == GPIO_SR16
    out_sr.x_ena = enable.x;
#ifdef X2_ENABLE_PIN
    out_sr.m3_ena = enable.x;
#endif
    out_sr.y_ena = enable.y;
#ifdef Y2_ENABLE_PIN
    out_sr.m3_ena = enable.y;
#endif
    out_sr.z_ena = enable.z;
#ifdef Z2_ENABLE_PIN
    out_sr.m3_ena = enable.z;
#endif
#ifdef A_ENABLE_PIN
    out_sr.m3_ena = enable.a;
#endif
    out_sr16_write(pio1, out_sr_sm, out_sr.value);
#elif ENABLE_PORT == GPIO_IOEXPAND
#ifdef STEPPERS_DISABLEX_PIN
    ioex_out(STEPPERS_DISABLEX_PIN) = enable.x;
#endif
#ifdef STEPPERS_DISABLEZ_PIN
    ioex_out(STEPPERS_DISABLEZ_PIN) = enable.z;
#endif
    ioexpand_out(io_expander);
#endif
}

// Starts stepper driver ISR timer and forces a stepper driver interrupt callback
static void stepperWakeUp (void)
{
    stepperEnable((axes_signals_t){AXES_BITMASK});
    stepper_timer_set_period(pio1, stepper_timer_sm, stepper_timer_sm_offset, hal.f_step_timer / 500); // ~2ms delay to allow drivers time to wake up.
    irq_set_enabled(PIO1_IRQ_0, true);
}

// Disables stepper driver interrupts
static void stepperGoIdle (bool clear_signals)
{
    irq_set_enabled(PIO1_IRQ_0, false);
    stepper_timer_stop(pio1, stepper_timer_sm);
}

// Sets up stepper driver interrupt timeout, "Normal" version
static void __not_in_flash_func(stepperCyclesPerTick)(uint32_t cycles_per_tick)
{
    stepper_timer_set_period(pio1, stepper_timer_sm, stepper_timer_sm_offset, cycles_per_tick < 1000000 ? cycles_per_tick : 1000000);
}

#ifdef SQUARING_ENABLED

static axes_signals_t motors_1 = {AXES_BITMASK}, motors_2 = {AXES_BITMASK};

// Set stepper pulse output pins
// NOTE: step_outbits are: bit0 -> X, bit1 -> Y, bit2 -> Z...
inline static __attribute__((always_inline)) void stepperSetStepOutputs (axes_signals_t step_outbits_1)
{
    axes_signals_t step_outbits_2;

    step_outbits_2.mask = (step_outbits_1.mask & motors_2.mask) ^ settings.steppers.step_invert.mask;
    step_outbits_1.mask = (step_outbits_1.mask & motors_1.mask) ^ settings.steppers.step_invert.mask;

#if STEP_PORT == GPIO_PIO_1

    pio_steps.set = step_outbits_1.x;
    pio_steps.reset = settings.steppers.step_invert.x;
    step_pulse_generate(pio1, x_step_sm, pio_steps.value);
#ifdef X2_STEP_PIN
    pio_steps.set = step_outbits_2.x;
    step_pulse_generate(pio0, x2_step_sm, pio_steps.value);
#endif
    pio_steps.set = step_outbits_1.y;
    pio_steps.reset = settings.steppers.step_invert.y;
    step_pulse_generate(pio1, y_step_sm, pio_steps.value);
#ifdef Y2_STEP_PIN
    pio_steps.set = step_outbits_2.y;
    step_pulse_generate(pio0, y2_step_sm, pio_steps.value);
#endif
    pio_steps.set = step_outbits_1.z;
    pio_steps.reset = settings.steppers.step_invert.z;
    step_pulse_generate(z_step_pio, z_step_sm, pio_steps.value);
#ifdef Z2_STEP_PIN
    pio_steps.set = step_outbits_2.z;
    step_pulse_generate(pio0, z2_step_sm, pio_steps.value);
#endif
#ifdef A_STEP_PIN
    pio_steps.set = step_outbits_1.a;
    pio_steps.reset = settings.steppers.step_invert.a;
    step_pulse_generate(pio0, a_step_sm, pio_steps.value);
#endif
#ifdef B_STEP_PIN
    pio_steps.set = step_outbits_1.b;
    pio_steps.reset = settings.steppers.step_invert.b;
    step_pulse_generate(pio0, b_step_sm, pio_steps.value);
#endif
#ifdef C_STEP_PIN
    pio_steps.set = step_outbits_1.c;
    pio_steps.reset = settings.steppers.step_invert.c;
    step_pulse_generate(pio0, c_step_sm, pio_steps.value);
#endif

#elif STEP_PORT == GPIO_PIO

    pio_steps.set = step_outbits_1.mask & 0x07;

#ifdef X2_STEP_PIN
    if (step_outbits_2.x)
        pio_steps.set |= (1 << (X2_STEP_PIN - STEP_PINS_BASE));
#endif
#ifdef Y2_STEP_PIN
    if (step_outbits_2.y)
        pio_steps.set |= (1 << (Y2_STEP_PIN - STEP_PINS_BASE));
#endif
#ifdef Z2_STEP_PIN
    if (step_outbits_2.z)
        pio_steps.set |= (1 << (Z2_STEP_PIN - STEP_PINS_BASE));
#endif
#ifdef A_STEP_PIN
    if (step_outbits_1.a)
        pio_steps.set |= (1 << (A_STEP_PIN - STEP_PINS_BASE));
#endif
#ifdef B_STEP_PIN
    if (step_outbits_1.b)
        pio_steps.set |= (1 << (B_STEP_PIN - STEP_PINS_BASE));
#endif
#ifdef C_STEP_PIN
    if (step_outbits_1.c)
        pio_steps.set |= (1 << (C_STEP_PIN - STEP_PINS_BASE));
#endif

    step_pulse_generate(pio0, 0, pio_steps.value);

#elif STEP_PORT == GPIO_SR8

    sd_sr.set.x_step = step_outbits_1.x;
#ifdef X2_STEP_PIN
    sd_sr.set.m3_step = step_outbits_2.x;
#endif
    sd_sr.set.y_step = step_outbits_1.y;
#ifdef Y2_STEP_PIN
    sd_sr.set.m3_step = step_outbits_2.y;
#endif
    sd_sr.set.z_step = step_outbits_1.z;
#ifdef Z2_STEP_PIN
    sd_sr.set.m3_step = step_outbits_2.z;
#endif
#ifdef A_STEP_PIN
    sd_sr.set.m3_step = step_outbits_1.a;
#endif
    step_dir_sr4_write(pio0, 0, sd_sr.value);

#endif
}

// Enable/disable motors for auto squaring of ganged axes
static void StepperDisableMotors (axes_signals_t axes, squaring_mode_t mode)
{
    motors_1.mask = (mode == SquaringMode_A || mode == SquaringMode_Both ? axes.mask : 0) ^ AXES_BITMASK;
    motors_2.mask = (mode == SquaringMode_B || mode == SquaringMode_Both ? axes.mask : 0) ^ AXES_BITMASK;
}

#else // SQUARING_ENABLED

// Set stepper pulse output pins
// NOTE: step_outbits are: bit0 -> X, bit1 -> Y, bit2 -> Z...
inline static __attribute__((always_inline)) void stepperSetStepOutputs (axes_signals_t step_outbits)
{
    step_outbits.mask ^= settings.steppers.step_invert.mask;

#if STEP_PORT == GPIO_PIO_1

    pio_steps.set = step_outbits.x;
    pio_steps.reset = settings.steppers.step_invert.x;
    step_pulse_generate(pio1, x_step_sm, pio_steps.value);
#ifdef X2_STEP_PIN
    step_pulse_generate(pio0, x2_step_sm, pio_steps.value);
#endif
    pio_steps.set = step_outbits.y;
    pio_steps.reset = settings.steppers.step_invert.y;
    step_pulse_generate(pio1, y_step_sm, pio_steps.value);
#ifdef Y2_STEP_PIN
    step_pulse_generate(pio0, y2_step_sm, pio_steps.value);
#endif
    pio_steps.set = step_outbits.z;
    pio_steps.reset = settings.steppers.step_invert.z;
    step_pulse_generate(z_step_pio, z_step_sm, pio_steps.value);
#ifdef Z2_STEP_PIN
    step_pulse_generate(pio0, z2_step_sm, pio_steps.value);
#endif
#ifdef A_STEP_PIN
    pio_steps.set = step_outbits.a;
    pio_steps.reset = settings.steppers.step_invert.a;
    step_pulse_generate(pio0, a_step_sm, pio_steps.value);
#endif
#ifdef B_STEP_PIN
    pio_steps.set = step_outbits.b;
    pio_steps.reset = settings.steppers.step_invert.b;
    step_pulse_generate(pio0, b_step_sm, pio_steps.value);
#endif
#ifdef C_STEP_PIN
    pio_steps.set = step_outbits.c;
    pio_steps.reset = settings.steppers.step_invert.c;
    step_pulse_generate(pio0, c_step_sm, pio_steps.value);
#endif

#elif STEP_PORT == GPIO_PIO

    pio_steps.set = step_outbits.mask & 0x07;
#ifdef X2_STEP_PIN
    if (step_outbits.x)
        pio_steps.set |= (1 << (X2_STEP_PIN - STEP_PINS_BASE));
#endif
#ifdef Y2_STEP_PIN
    if (step_outbits.y)
        pio_steps.set |= (1 << (Y2_STEP_PIN - STEP_PINS_BASE));
#endif
#ifdef Z2_STEP_PIN
    if (step_outbits.z)
        pio_steps.set |= (1 << (Z2_STEP_PIN - STEP_PINS_BASE));
#endif
#ifdef A_STEP_PIN
    if (step_outbits.a)
        pio_steps.set |= (1 << (A_STEP_PIN - STEP_PINS_BASE));
#endif
#ifdef B_STEP_PIN
    if (step_outbits.b)
        pio_steps.set |= (1 << (B_STEP_PIN - STEP_PINS_BASE));
#endif
#ifdef C_STEP_PIN
    if (step_outbits.c)
        pio_steps.set |= (1 << (C_STEP_PIN - STEP_PINS_BASE));
#endif

    step_pulse_generate(pio0, 0, pio_steps.value);

#elif STEP_PORT == GPIO_SR8

    sd_sr.set.x_step = step_outbits.x;
#ifdef X2_STEP_PIN
    sd_sr.set.m3_step = step_outbits.x;
#endif
    sd_sr.set.y_step = step_outbits.y;
#ifdef Y2_STEP_PIN
    sd_sr.set.m3_step = step_outbits.y;
#endif
    sd_sr.set.z_step = step_outbits.z;
#ifdef Z2_STEP_PIN
    sd_sr.set.m3_step = step_outbits.z;
#endif
#ifdef A_STEP_PIN
    sd_sr.set.m3_step = step_outbits.a;
#endif
    step_dir_sr4_write(pio0, 0, sd_sr.value);

#endif
}

#endif // SQUARING_ENABLED

#ifdef GANGING_ENABLED

static axes_signals_t getGangedAxes (bool auto_squared)
{
    axes_signals_t ganged = {0};

    if (auto_squared) {
#if X_AUTO_SQUARE
        ganged.x = On;
#endif
#if Y_AUTO_SQUARE
        ganged.y = On;
#endif
#if Z_AUTO_SQUARE
        ganged.z = On;
#endif
    } else {
#if X_GANGED
        ganged.x = On;
#endif

#if Y_GANGED
        ganged.y = On;
#endif

#if Z_GANGED
        ganged.z = On;
#endif
    }

    return ganged;
}

#endif

// Set stepper direction output pins
// NOTE: see note for stepperSetStepOutputs()
//inline static __attribute__((always_inline)) void stepperSetDirOutputs (axes_signals_t dir_outbits)
static void stepperSetDirOutputs (axes_signals_t dir_outbits)
{
#if DIRECTION_PORT == GPIO_OUTPUT

#if DIRECTION_OUTMODE == GPIO_MAP
    gpio_put_masked(DIRECTION_MASK, dir_outmap[dir_outbits.mask]);
#ifdef X2_DIRECTION_PIN
    DIGITAL_OUT(X2_DIRECTION_BIT, (dir_outbits.x ^ settings.steppers.dir_invert.x) ^ settings.steppers.ganged_dir_invert.x);
#endif
#ifdef Y2_DIRECTION_PIN
    DIGITAL_OUT(Y2_DIRECTION_BIT, (dir_outbits.y ^ settings.steppers.dir_invert.y) ^ settings.steppers.ganged_dir_invert.y);
#endif
#ifdef Z2_DIRECTION_PIN
    DIGITAL_OUT(Z2_DIRECTION_BIT, (dir_outbits.z ^ settings.steppers.dir_invert.z) ^ settings.steppers.ganged_dir_invert.z);
#endif
#else
    dir_outbits.mask ^= settings.steppers.dir_invert.mask;
    gpio_put_masked(DIRECTION_MASK, dir_outbits.mask << DIRECTION_OUTMODE);
#ifdef GANGING_ENABLED
    dir_outbits.mask ^= settings.steppers.ganged_dir_invert.mask;
#ifdef X2_DIRECTION_PIN
    DIGITAL_OUT(X2_DIRECTION_BIT, dir_outbits.x);
#endif
#ifdef Y2_DIRECTION_PIN
    DIGITAL_OUT(Y2_DIRECTION_BIT, dir_outbits.y);
#endif
#ifdef Z2_DIRECTION_PIN
    DIGITAL_OUT(Z2_DIRECTION_BIT, dir_outbits.z);
#endif
#endif
#endif

#elif DIRECTION_PORT == GPIO_SR8

    dir_outbits.mask ^= settings.steppers.dir_invert.mask;
    sd_sr.set.x_dir = sd_sr.reset.x_dir = dir_outbits.x;
    sd_sr.set.y_dir = sd_sr.reset.y_dir = dir_outbits.y;
    sd_sr.set.z_dir = sd_sr.reset.z_dir = dir_outbits.z;
#ifdef GANGING_ENABLED
    dir_outbits.mask ^= settings.steppers.ganged_dir_invert.mask;
#ifdef X2_DIRECTION_PIN
    sd_sr.set.m3_dir = sd_sr.reset.m3_dir = dir_outbits.x;
#endif
#ifdef Y2_DIRECTION_PIN
    sd_sr.set.m3_dir = sd_sr.reset.m3_dir = dir_outbits.y;
#endif
#ifdef Z2_DIRECTION_PIN
    sd_sr.set.m3_dir = sd_sr.reset.m3_dir = dir_outbits.z;
#endif
#endif
#ifdef A_DIRECTION_PIN
    sd_sr.set.m3_dir = sd_sr.reset.m3_dir = dir_outbits.a;
#endif
    // dir signals are set on the next step pulse output
#endif
}

// Sets stepper direction and pulse pins and starts a step pulse.
static void __not_in_flash_func(stepperPulseStart)(stepper_t *stepper)
{
    if (stepper->dir_change)
        stepperSetDirOutputs(stepper->dir_outbits);

    if (stepper->step_outbits.value)
        stepperSetStepOutputs(stepper->step_outbits);
}

#if STEP_INJECT_ENABLE

void stepperOutputStep (axes_signals_t step_outbits, axes_signals_t dir_outbits)
{
    if(step_outbits.value) {

        dir_outbits.mask ^= settings.steppers.dir_invert.mask;
#ifdef GANGING_ENABLED
        axes_signals_t dir_outbits2;
        dir_outbits2.mask = dir_outbits.mask ^ settings.steppers.ganged_dir_invert.mask;
#endif

#if DIRECTION_PORT == GPIO_SR8

        if(step_outbits.x) {
            sd_sr.set.x_dir = sd_sr.reset.x_dir = dir_outbits.x;
  #ifdef X2_DIRECTION_PIN
            sd_sr.set.m3_dir = sd_sr.reset.m3_dir = dir_outbits2.x;
  #endif
        }
        if(step_outbits.y) {
            sd_sr.set.y_dir = sd_sr.reset.y_dir = dir_outbits.y;
  #ifdef Y2_DIRECTION_PIN
            sd_sr.set.m3_dir = sd_sr.reset.m3_dir = dir_outbits2.y;
  #endif
        }
        if(step_outbits.z) {
            sd_sr.set.z_dir = sd_sr.reset.z_dir = dir_outbits.z;
  #ifdef Z2_DIRECTION_PIN
            sd_sr.set.m3_dir = sd_sr.reset.m3_dir = dir_outbits2.z;
  #endif
        }
  #ifdef A_DIRECTION_PIN
        if(step_outbits.a)
            sd_sr.set.m3_dir = sd_sr.reset.m3_dir = dir_outbits.a;
  #endif
    // dir signals are set on the next step pulse output
#else // DIRECTION_OUTMODE <= GPIO_MAP
        if(step_outbits.x) {
            DIGITAL_OUT(X_DIRECTION_BIT, dir_outbits.x);
  #ifdef X2_DIRECTION_PIN
            DIGITAL_OUT(X2_DIRECTION_BIT, dir_outbits2.x);
  #endif
        }
        if(step_outbits.y) {
            DIGITAL_OUT(Y_DIRECTION_BIT, dir_outbits.y);
  #ifdef Y2_DIRECTION_PIN
            DIGITAL_OUT(Y2_DIRECTION_BIT, dir_outbits2.y);
  #endif
        }
        if(step_outbits.z) {
            DIGITAL_OUT(Z_DIRECTION_BIT, dir_outbits.z);
  #ifdef Z2_DIRECTION_PIN
            DIGITAL_OUT(Z2_DIRECTION_BIT, dir_outbits2.z);
  #endif
        }
  #ifdef A_AXIS
        if(step_outbits.a)
            DIGITAL_OUT(A_DIRECTION_BIT, dir_outbits.a);
  #endif
  #ifdef B_AXIS
        if(step_outbits.b)
            DIGITAL_OUT(C_DIRECTION_BIT, dir_outbits.b);
  #endif
  #ifdef C_AXIS
        if(step_outbits.c)
            DIGITAL_OUT(A_DIRECTION_BIT, dir_outbits.c);
  #endif
#endif
    }

    stepperSetStepOutputs(step_outbits);
}

#endif // STEP_INJECT_ENABLE

//*************************  LIMIT  *************************//

// Enable/disable limit pins interrupt
static void limitsEnable (bool on, axes_signals_t homing_cycle)
{
    bool disable = !on;
    uint32_t i = limit_inputs.n_pins;
    axes_signals_t pin;
    limit_signals_t homing_source = xbar_get_homing_source_from_cycle(homing_cycle);

    on = on && settings.limits.flags.hard_enabled;

    do {
        i--;
        if(on && homing_cycle.mask) {
            pin = xbar_fn_to_axismask(limit_inputs.pins.inputs[i].id);
            disable = limit_inputs.pins.inputs[i].group == PinGroup_Limit ? (pin.mask & homing_source.min.mask) : (pin.mask & homing_source.max.mask);
        }
        pinEnableIRQ(&limit_inputs.pins.inputs[i], disable ? IRQ_Mode_None : limit_inputs.pins.inputs[i].mode.irq_mode);
    } while (i);

#if TRINAMIC_ENABLE
//    trinamic_homing(homing_cycle.mask != 0);
#endif
}

// Returns limit state as an limit_signals_t variable.
// Each bitfield bit indicates an axis limit, where triggered is 1 and not triggered is 0.
inline static limit_signals_t limitsGetState (void)
{
    limit_signals_t signals = {0};

    signals.min.x = DIGITAL_IN(X_LIMIT_BIT);
#ifdef X2_LIMIT_PIN
    signals.min2.x = DIGITAL_IN(X2_LIMIT_BIT);
#endif
    signals.min.y = DIGITAL_IN(Y_LIMIT_BIT);
#ifdef Y2_LIMIT_PIN
    signals.min2.y = DIGITAL_IN(Y2_LIMIT_BIT);
#endif
    signals.min.z = DIGITAL_IN(Z_LIMIT_BIT);
#ifdef Z2_LIMIT_PIN
    signals.min2.z = DIGITAL_IN(Z2_LIMIT_BIT);
#endif
#ifdef A_LIMIT_PIN
    signals.min.a = DIGITAL_IN(A_LIMIT_BIT);
#endif
#ifdef B_LIMIT_PIN
    signals.min.b = DIGITAL_IN(B_LIMIT_BIT);
#endif
#ifdef C_LIMIT_PIN
    signals.min.c = DIGITAL_IN(C_LIMIT_BIT);
#endif

    return signals;
}

// Returns system state as a control_signals_t variable.
// Each bitfield bit indicates a control signal, where triggered is 1 and not triggered is 0.
static control_signals_t __not_in_flash_func(systemGetState)(void)
{
    control_signals_t signals = {0};

#ifdef RESET_PIN
#ifdef ESTOP_ENABLE
    signals.e_stop = DIGITAL_IN(RESET_BIT);
#else
    signals.reset = DIGITAL_IN(RESET_BIT);
#endif
#endif
#ifdef FEED_HOLD_PIN
    signals.feed_hold = DIGITAL_IN(FEED_HOLD_BIT);
#endif
#ifdef CYCLE_START_PIN
    signals.cycle_start = DIGITAL_IN(CYCLE_START_BIT);
#endif
#if SAFETY_DOOR_BIT
    signals.safety_door_ajar = safety_door->active;
#endif

#if AUX_CONTROLS_ENABLED

  #ifdef SAFETY_DOOR_PIN
    if(aux_ctrl[AuxCtrl_SafetyDoor].debouncing)
        signals.safety_door_ajar = !settings.control_invert.safety_door_ajar;
    else
        signals.safety_door_ajar = DIGITAL_IN(1 << SAFETY_DOOR_PIN) ^ settings.control_invert.safety_door_ajar;
  #endif
  #ifdef MOTOR_FAULT_PIN
    signals.motor_fault = DIGITAL_IN(1 << MOTOR_FAULT_PIN) ^ settings.control_invert.motor_fault;
  #endif
  #ifdef MOTOR_WARNING_PIN
    signals.motor_warning = DIGITAL_IN(1 << MOTOR_WARNING_PIN) ^ settings.control_invert.motor_warning;
  #endif

  #if AUX_CONTROLS_SCAN
    uint_fast8_t i;
    for(i = AUX_CONTROLS_SCAN; i < AuxCtrl_NumEntries; i++) {
        if(aux_ctrl[i].enabled) {
            signals.mask &= ~aux_ctrl[i].cap.mask;
            if(hal.port.wait_on_input(Port_Digital, aux_ctrl[i].port, WaitMode_Immediate, 0.0f) == 1)
                signals.mask |= aux_ctrl[i].cap.mask;
        }
    }
  #endif

#endif // AUX_CONTROLS_ENABLED

    return signals;
}

#if AUX_CONTROLS_ENABLED

static int64_t __not_in_flash_func(aux_irq_latch)(alarm_id_t id, void *input)
{
    control_signals_t signals;

    aux_ctrl[AuxCtrl_SafetyDoor].debouncing = false;

    if((signals = systemGetState()).safety_door_ajar)
        hal.control.interrupt_callback(signals);

    return 0;
}

static void __not_in_flash_func(aux_irq_handler) (uint8_t port, bool state)
{
    uint_fast8_t i;
    control_signals_t signals = {0};

    for(i = 0; i < AuxCtrl_NumEntries; i++) {
        if(aux_ctrl[i].port == port) {
            if(!aux_ctrl[i].debouncing) {
                if(i == AuxCtrl_SafetyDoor && (aux_ctrl[i].debouncing = add_alarm_in_ms(DEBOUNCE_DELAY, aux_irq_latch, NULL, false)))
                    break;
                signals.mask |= aux_ctrl[i].cap.mask;
                if(aux_ctrl[i].irq_mode == IRQ_Mode_Change)
                    signals.deasserted = hal.port.wait_on_input(Port_Digital, aux_ctrl[i].port, WaitMode_Immediate, 0.0f) == 0;
            }
            break;
        }
    }

    if(signals.mask) {
        if(!signals.deasserted)
            signals.mask |= systemGetState().mask;
        hal.control.interrupt_callback(signals);
    }
}

static bool aux_attach (xbar_t *properties, aux_ctrl_t *aux_ctrl)
{
    bool ok;
    uint_fast8_t i = sizeof(inputpin) / sizeof(input_signal_t);

    do {
        i--;
        if((ok = inputpin[i].pin == properties->pin)) {
            inputpin[i].aux_ctrl = aux_ctrl;
            break;
        }
    } while(i);

    return ok;
}


static bool aux_claim (xbar_t *properties, uint8_t port, void *data)
{
    bool ok;

    ((aux_ctrl_t *)data)->port = port;

    if((ok = ioport_claim(Port_Digital, Port_Input, &((aux_ctrl_t *)data)->port, xbar_fn_to_pinname(((aux_ctrl_t *)data)->function))))
        aux_attach(properties, (aux_ctrl_t *)data);

    return ok;
}

#if AUX_CONTROLS_XMAP

static bool aux_claim_explicit (aux_ctrl_t *aux_ctrl)
{
    if((aux_ctrl->enabled = aux_ctrl->port != 0xFF && ioport_claim(Port_Digital, Port_Input, &aux_ctrl->port, xbar_fn_to_pinname(aux_ctrl->function)))) {
        hal.signals_cap.mask |= aux_ctrl->cap.mask;
        aux_attach(hal.port.get_pin_info(Port_Digital, Port_Input, aux_ctrl->port), aux_ctrl);
    } else
        aux_ctrl->port = 0xFF;

    return aux_ctrl->enabled;
}

#endif

#endif // AUX_CONTROLS_ENABLED

//*************************  PROBE  *************************//

#ifdef PROBE_PIN

// Sets up the probe pin invert mask to
// appropriately set the pin logic according to setting for normal-high/normal-low operation
// and the probing cycle modes for toward-workpiece/away-from-workpiece.
static void probeConfigure (bool is_probe_away, bool probing)
{
    probe.triggered = false;
    probe.inverted = is_probe_away ? !settings.probe.invert_probe_pin : settings.probe.invert_probe_pin;

    gpio_set_inover(PROBE_PIN, probe.inverted ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);

    if ((probe.is_probing = probing))
        gpio_set_irq_enabled(PROBE_PIN, probe.inverted ? GPIO_IRQ_LEVEL_LOW : GPIO_IRQ_LEVEL_HIGH, true);
    else
        gpio_set_irq_enabled(PROBE_PIN, GPIO_IRQ_ALL, false);
}

// Returns the probe connected and triggered pin states.
static probe_state_t probeGetState (void)
{
    probe_state_t state = {0};

    state.connected = probe.connected;
    state.triggered = probe.is_probing ? probe.triggered : DIGITAL_IN(PROBE_BIT);

    return state;
}

#endif

//*************************  SPINDLE  *************************//

#if DRIVER_SPINDLE_ENABLE

// Static spindle (off, on cw & on ccw)
inline static void spindle_off (void)
{
#if SPINDLE_PORT == GPIO_OUTPUT

#ifdef SPINDLE_ENABLE_PIN
    DIGITAL_OUT(SPINDLE_ENABLE_BIT, Off);
#endif

#elif SPINDLE_PORT == GPIO_IOEXPAND

    ioex_out(SPINDLE_ENABLE_PIN) = settings.spindle.invert.on;
    ioexpand_out(io_expander);

#elif SPINDLE_PORT == GPIO_SR16

    out_sr.spindle_ena = settings.spindle.invert.on;
    out_sr16_write(pio1, out_sr_sm, out_sr.value);

#endif
}

inline static void spindle_on (void)
{
#if SPINDLE_PORT == GPIO_OUTPUT

#ifdef SPINDLE_ENABLE_PIN
    DIGITAL_OUT(SPINDLE_ENABLE_BIT, On);
#endif

#elif SPINDLE_PORT == GPIO_IOEXPAND

    ioex_out(SPINDLE_ENABLE_PIN) = !settings.spindle.invert.on;
    ioexpand_out(io_expander);

#elif SPINDLE_PORT == GPIO_SR16

    out_sr.spindle_ena = !settings.spindle.invert.on;
    out_sr16_write(pio1, out_sr_sm, out_sr.value);

#endif
}

inline static void spindle_dir (bool ccw)
{
#if SPINDLE_PORT == GPIO_OUTPUT

#ifdef SPINDLE_DIRECTION_PIN
    DIGITAL_OUT(SPINDLE_DIRECTION_BIT, ccw);
#endif

#elif SPINDLE_PORT == GPIO_IOEXPAND

    ioex_out(SPINDLE_DIRECTION_PIN) = ccw ^ settings.spindle.invert.ccw;
    ioexpand_out(io_expander);

#elif SPINDLE_PORT == GPIO_SR16

    out_sr.spindle_dir = ccw ^ settings.spindle.invert.ccw;
    out_sr16_write(pio1, out_sr_sm, out_sr.value);

#endif
}

// Start or stop spindle
static void spindleSetState (spindle_ptrs_t *spindle, spindle_state_t state, float rpm)
{
    UNUSED(rpm);
    UNUSED(spindle);

    if(!state.on)
        spindle_off();
    else {
        spindle_dir(state.ccw);
        spindle_on();
    }
}

#if DRIVER_SPINDLE_PWM_ENABLE

// Variable spindle control functions

// Sets spindle speed
static void __not_in_flash_func(spindleSetSpeed)(spindle_ptrs_t *spindle, uint_fast16_t pwm_value)
{
    if (pwm_value == pwm(spindle)->off_value) {
        pwmEnabled = false;
        if(pwm(spindle)->settings->flags.enable_rpm_controlled) {
            if(pwm(spindle)->cloned)
                spindle_dir(false);
            else
                spindle_off();
        }
        if (pwm(spindle)->always_on)
            pwm_set_gpio_level(SPINDLE_PWM_PIN, pwm(spindle)->off_value);
        else
            pwm_set_gpio_level(SPINDLE_PWM_PIN, 0);
    } else {
        if (!pwmEnabled) {
            if(pwm(spindle)->cloned)
                spindle_dir(true);
            else
                spindle_on();
            pwmEnabled = true;
        }
        pwm_set_gpio_level(SPINDLE_PWM_PIN, pwm_value);
    }
}

static uint_fast16_t spindleGetPWM (spindle_ptrs_t *spindle, float rpm)
{
    return pwm(spindle)->compute_value(pwm(spindle), rpm, false);
}

// Start or stop spindle
static void spindleSetStateVariable (spindle_ptrs_t *spindle, spindle_state_t state, float rpm)
{
    if(state.on || pwm(spindle)->cloned)
        spindle_dir(state.ccw);

    if(!pwm(spindle)->settings->flags.enable_rpm_controlled) {
        if(state.on)
            spindle_on();
        else
            spindle_off();
    }

    spindleSetSpeed(spindle, state.on || (state.ccw && pwm(spindle)->cloned)
                              ? pwm(spindle)->compute_value(pwm(spindle), rpm, false)
                              : pwm(spindle)->off_value);
}

bool spindleConfig (spindle_ptrs_t *spindle)
{
    if (spindle == NULL)
        return false;

    uint32_t prescaler = settings.spindle.pwm_freq > 2000.0f ? 1 : (settings.spindle.pwm_freq > 200.0f ? 12 : 50);

    if (spindle_precompute_pwm_values(spindle, &spindle_pwm, &settings.spindle, clock_get_hz(clk_sys) / prescaler)) {

        spindle->set_state = spindleSetStateVariable;

        // Get the default config for
        pwm_config config = pwm_get_default_config();

        // Set divider, not using the 4 fractional bit part of the clock divider, only the integer part
        pwm_config_set_clkdiv_int(&config, prescaler);
        // Set the top value of the PWM => the period
        pwm_config_set_wrap(&config, spindle_pwm.period);
        // Set the off value of the PWM => off duty cycle (either 0 or the off value)
        pwm_set_gpio_level(SPINDLE_PWM_PIN, spindle_pwm.off_value);

        // Set polarity of the channel
        uint channel = pwm_gpio_to_channel(SPINDLE_PWM_PIN);                                                                        // Get which is associated with the PWM pin
        pwm_config_set_output_polarity(&config, (!channel & settings.spindle.invert.pwm), (channel & settings.spindle.invert.pwm)); // Set the polarity of the pin's channel

        // Load the configuration into our PWM slice, and set it running.
        pwm_init(pwm_gpio_to_slice_num(SPINDLE_PWM_PIN), &config, true);
    } else {
        if (pwmEnabled)
            spindle->set_state(spindle, (spindle_state_t){0}, 0.0f);
        spindle->set_state = spindleSetState;
    }

    spindle_update_caps(spindle, spindle->cap.variable ? &spindle_pwm : NULL);

    return true;
}

#if PPI_ENABLE

static void spindlePulseOn (uint_fast16_t pulse_length)
{
    //    PPI_TIMER->ARR = pulse_length;
    //    PPI_TIMER->EGR = TIM_EGR_UG;
    //    PPI_TIMER->CR1 |= TIM_CR1_CEN;
    spindle_on();
}

#endif // PPI_ENABLE

#endif // DRIVER_SPINDLE_PWM_ENABLE

// Returns spindle state in a spindle_state_t variable
static spindle_state_t spindleGetState (spindle_ptrs_t *spindle)
{
    spindle_state_t state = { settings.spindle.invert.mask };

    UNUSED(spindle);

#if SPINDLE_PORT == GPIO_OUTPUT

#ifdef SPINDLE_ENABLE_PIN
    state.on = DIGITAL_IN(SPINDLE_ENABLE_BIT);
#else
    state.on = pwmEnabled ^ settings.spindle.invert.on;
#endif
#ifdef SPINDLE_DIRECTION_PIN
    state.ccw = DIGITAL_IN(SPINDLE_DIRECTION_BIT);
#endif

#elif SPINDLE_PORT == GPIO_IOEXPAND

    state.on = ioex_in(SPINDLE_ENABLE_PIN);
    state.ccw = ioex_in(SPINDLE_DIRECTION_PIN);

#elif SPINDLE_PORT == GPIO_SR16

    state.on = out_sr.spindle_ena;
    state.ccw = out_sr.spindle_dir;

#endif

    state.value ^= settings.spindle.invert.mask;

    return state;
}

#endif // DRIVER_SPINDLE_ENABLE

// Start/stop coolant (and mist if enabled)
static void coolantSetState (coolant_state_t mode)
{
#if COOLANT_PORT == GPIO_OUTPUT

#ifdef COOLANT_FLOOD_PIN
    DIGITAL_OUT(COOLANT_FLOOD_BIT, mode.flood);
#endif
#ifdef COOLANT_MIST_PIN
    DIGITAL_OUT(COOLANT_MIST_BIT, mode.mist);
#endif

#elif COOLANT_PORT == GPIO_IOEXPAND

    mode.value ^= settings.coolant_invert.mask;
    ioex_out(COOLANT_FLOOD_PIN) = mode.flood;
#ifdef COOLANT_MIST_PIN
    ioex_out(COOLANT_MIST_PIN) = mode.mist;
#endif
    ioexpand_out(io_expander);

#elif COOLANT_PORT == GPIO_SR16

    mode.value ^= settings.coolant_invert.mask;
    out_sr.flood_ena = mode.flood;
    out_sr.mist_ena = mode.mist;
    out_sr16_write(pio1, out_sr_sm, out_sr.value);

#endif
}

// Returns coolant state in a coolant_state_t variable
static coolant_state_t coolantGetState (void)
{
    coolant_state_t state = { settings.coolant_invert.mask };

#if COOLANT_PORT == GPIO_OUTPUT

#ifdef COOLANT_FLOOD_PIN
    state.flood = DIGITAL_IN(COOLANT_FLOOD_BIT);
#endif
#ifdef COOLANT_MIST_PIN
    state.mist = DIGITAL_IN(COOLANT_MIST_BIT);
#endif

#elif COOLANT_PORT == GPIO_IOEXPAND

    ioexpand_t val = ioexpand_in();
    state.flood = ioex_in(COOLANT_FLOOD_PIN);
#ifdef COOLANT_MIST_PIN
    state.mist = ioex_in(COOLANT_MIST_PIN);
#endif

#elif COOLANT_PORT == GPIO_SR16

    state.flood = out_sr.flood_ena;
    state.mist = out_sr.mist_ena;

#endif

    state.value ^= settings.coolant_invert.mask;

    return state;
}

#if SPI_RST_PORT == GPIO_SR16

void spi_reset_out (bool on)
{
    out_sr.spi_reset = on;
    out_sr16_write(pio1, out_sr_sm, out_sr.value);
}

#endif

// Helper functions for setting/clearing/inverting individual bits atomically (uninterruptable)
static void bitsSetAtomic (volatile uint_fast16_t *ptr, uint_fast16_t bits)
{
    __disable_irq();
    *ptr |= bits;
    __enable_irq();
}

static uint_fast16_t bitsClearAtomic (volatile uint_fast16_t *ptr, uint_fast16_t bits)
{
    __disable_irq();
    uint_fast16_t prev = *ptr;
    *ptr &= ~bits;
    __enable_irq();

    return prev;
}

static uint_fast16_t valueSetAtomic (volatile uint_fast16_t *ptr, uint_fast16_t value)
{
    __disable_irq();
    uint_fast16_t prev = *ptr;
    *ptr = value;
    __enable_irq();

    return prev;
}

static uint64_t getElapsedMicros (void)
{
    return to_us_since_boot(get_absolute_time());
}

static uint32_t getElapsedTicks (void)
{
    return elapsed_ticks;
}

#if MPG_MODE == 1

static input_signal_t *mpg_pin = NULL;

static void mpg_select (void *data)
{
    stream_mpg_enable(DIGITAL_IN(mpg_pin->bit) == 0);

    pinEnableIRQ(mpg_pin, (mpg_pin->mode.irq_mode = sys.mpg_mode ? IRQ_Mode_Rising : IRQ_Mode_Falling));
}

static void mpg_enable (void *data)
{
    if (sys.mpg_mode != (DIGITAL_IN(mpg_pin->bit) == 0))
        mpg_select(data);
    else
        pinEnableIRQ(mpg_pin, (mpg_pin->mode.irq_mode = sys.mpg_mode ? IRQ_Mode_Rising : IRQ_Mode_Falling));
}

#endif
/*
// Save the gpio that has generated the IRQ together with the alarm pool id and the edge
static void debounce_alarm_pool_save_gpio (alarm_id_t id, uint pin, uint level)
{
    static volatile uint8_t index = 0;

    debounceAlarmPoolArray[index].id = id;
    debounceAlarmPoolArray[index].pin = pin;
    debounceAlarmPoolArray[index].level = level;
    index = (index + 1) % DEBOUNCE_ALARM_MAX_TIMER;
}
*/

void pinEnableIRQ (const input_signal_t *input, pin_irq_mode_t irq_mode)
{
    switch (irq_mode) {

        case IRQ_Mode_Rising:
            gpio_set_irq_enabled(input->pin, GPIO_IRQ_EDGE_RISE, true);
            break;

        case IRQ_Mode_Falling:
            gpio_set_irq_enabled(input->pin, GPIO_IRQ_EDGE_FALL, true);
            break;

        case IRQ_Mode_Change:
            gpio_set_irq_enabled(input->pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
            break;

        case IRQ_Mode_Low:
            gpio_set_irq_enabled(input->pin, GPIO_IRQ_LEVEL_LOW, true);
            break;

        case IRQ_Mode_High:
            gpio_set_irq_enabled(input->pin, GPIO_IRQ_LEVEL_HIGH, true);
            break;

        case IRQ_Mode_None:
            gpio_set_irq_enabled(input->pin, GPIO_IRQ_ALL, false);
            break;
    }
}

// Configures peripherals when settings are initialized or changed
void settings_changed (settings_t *settings, settings_changed_flags_t changed)
{
#if USE_STEPDIR_MAP
    stepdirmap_init(settings);
#endif

    if (IOInitDone) {

#if WIFI_ENABLE
        if(!net.wifi)
            net.wifi = wifi_start();
#endif

#if ETHERNET_ENABLE
        if(!net.ethernet)
            net.ethernet = enet_start();
#endif

#if BLUETOOTH_ENABLE == 1
        if(!net.bluetooth)
            net.bluetooth = bluetooth_start_local();
#endif

#if DRIVER_SPINDLE_PWM_ENABLE
        if(changed.spindle) {
            spindleConfig(spindle_get_hal(spindle_id, SpindleHAL_Configured));
            if(spindle_id == spindle_get_default())
                spindle_select(spindle_id);
        }
#endif

#ifdef NEOPIXELS_PIN

    if(neopixel.leds == NULL || hal.rgb.num_devices != settings->rgb_strip0_length) {

        if(settings->rgb_strip0_length == 0)
            settings->rgb_strip0_length = hal.rgb.num_devices;
        else
            hal.rgb.num_devices = settings->rgb_strip0_length;

        if(neopixel.leds) {
            free(neopixel.leds);
            neopixel.leds = NULL;
        }

        if(hal.rgb.num_devices) {
            neopixel.num_bytes = hal.rgb.num_devices * sizeof(uint32_t);
            if((neopixel.leds = calloc(neopixel.num_bytes, sizeof(uint8_t))) == NULL)
                hal.rgb.num_devices = 0;
        }

        neopixel.num_leds = hal.rgb.num_devices;
    }

#endif

#if SD_SHIFT_REGISTER
        pio_steps.length = (uint32_t)(10.0f * (settings->steppers.pulse_microseconds - 0.8f));
        pio_steps.delay = settings->steppers.pulse_delay_microseconds <= 0.8f
                              ? 2
                              : (uint32_t)(10.0f * (settings->steppers.pulse_delay_microseconds - 0.8f));
        sr_delay_set(pio0, 1, pio_steps.delay);
        sr_hold_set(pio0, 2, pio_steps.length);
        sd_sr.reset.x_step = settings->steppers.step_invert.x;
#ifdef X2_DIRECTION_PIN
        sd_sr.reset.m3_step = settings->steppers.step_invert.x;
#endif
        sd_sr.reset.y_step = settings->steppers.step_invert.y;
#ifdef Y2_DIRECTION_PIN
        sd_sr.reset.m3_step = settings->steppers.step_invert.y;
#endif
        sd_sr.reset.z_step = settings->steppers.step_invert.z;
#ifdef Z2_DIRECTION_PIN
        sd_sr.reset.m3_step = settings->steppers.step_invert.z;
#endif
#ifdef A_DIRECTION_PIN
        sd_sr.reset.m3_step = settings->steppers.step_invert.a;
#endif

#else // PIO step parameters init

        pio_steps.length = (uint32_t)(10.0f * (settings->steppers.pulse_microseconds)) - 1;
        pio_steps.delay = settings->steppers.pulse_delay_microseconds == 0.0f
                              ? 1
                              : (uint32_t)(10.0f * (settings->steppers.pulse_delay_microseconds)) - 1;
        pio_steps.reset = settings->steppers.step_invert.mask;
#ifdef X2_STEP_PIN
        if (settings->steppers.step_invert.x)
            pio_steps.reset |= X2_STEP_BIT;
#endif
#ifdef Y2_STEP_PIN
        if (settings->steppers.step_invert.y)
            pio_steps.reset |= Y2_STEP_BIT;
#endif
#ifdef Z2_STEP_PIN
        if (settings->steppers.step_invert.z)
            pio_steps.reset |= Z2_STEP_BIT;
#endif

#endif

        stepperSetStepOutputs((axes_signals_t){0});
        stepperSetDirOutputs((axes_signals_t){0});

        /***********************
         *  Input pins config  *
         ***********************/

        // Set the GPIO interrupt handler, the pin doesn't matter for now
        gpio_set_irq_enabled_with_callback(0, 0, false, gpio_int_handler);

        // Disable GPIO IRQ while initializing the input pins
        irq_set_enabled(IO_IRQ_BANK0, false);

        bool pullup;
        uint32_t i = sizeof(inputpin) / sizeof(input_signal_t);
        input_signal_t *input;

        control_signals_t control_fei;
        control_fei.mask = settings->control_disable_pullup.mask ^ settings->control_invert.mask;

        axes_signals_t limit_fei;
        limit_fei.mask = settings->limits.disable_pullup.mask ^ settings->limits.invert.mask;

        do {

            input = &inputpin[--i];

            if(input->group == PinGroup_AuxInputAnalog)
                continue;

            input->bit = 1 << input->pin;
            input->debounce = false;
            input->invert = false;
            input->mode.irq_mode = IRQ_Mode_None;
            pullup = input->group == PinGroup_AuxInput;

            gpio_init(input->pin);
            if (!(input->group == PinGroup_Limit || input->group == PinGroup_AuxInput))
                gpio_set_irq_enabled(input->pin, GPIO_IRQ_ALL, false);

            switch(input->id) {

                case Input_EStop:
                    pullup = !settings->control_disable_pullup.e_stop;
                    input->invert = control_fei.e_stop;
                    break;

                case Input_Reset:
                    pullup = !settings->control_disable_pullup.reset;
                    input->invert = control_fei.reset;
                    break;

                case Input_FeedHold:
                    pullup = !settings->control_disable_pullup.feed_hold;
                    input->invert = control_fei.feed_hold;
                    break;

                case Input_CycleStart:
                    pullup = !settings->control_disable_pullup.cycle_start;
                    input->invert = control_fei.cycle_start;
                    break;
    #if SAFETY_DOOR_BIT
                case Input_SafetyDoor:
                    safety_door = input;
                    pullup = !settings->control_disable_pullup.safety_door_ajar;
                    input->invert = control_fei.safety_door_ajar;
                    input->mode.irq_mode = safety_door->invert ? IRQ_Mode_Low : IRQ_Mode_High;
                    break;
    #endif
                case Input_Probe:
                    pullup = !settings->probe.disable_probe_pullup;
                    input->invert = settings->probe.invert_probe_pin;
                    break;

                case Input_LimitX:
                case Input_LimitX_2:
                case Input_LimitX_Max:
                    pullup = !settings->limits.disable_pullup.x;
                    input->invert = limit_fei.x;
                    break;

                case Input_LimitY:
                case Input_LimitY_2:
                case Input_LimitY_Max:
                    pullup = !settings->limits.disable_pullup.y;
                    input->invert = limit_fei.y;
                    break;

                case Input_LimitZ:
                case Input_LimitZ_2:
                case Input_LimitZ_Max:
                    pullup = !settings->limits.disable_pullup.z;
                    input->invert = limit_fei.z;
                    break;

                case Input_LimitA:
                case Input_LimitA_Max:
                    pullup = !settings->limits.disable_pullup.a;
                    input->invert = limit_fei.a;
                    break;

                case Input_LimitB:
                case Input_LimitB_Max:
                    pullup = !settings->limits.disable_pullup.b;
                    input->invert = limit_fei.b;
                    break;

                case Input_LimitC:
                case Input_LimitC_Max:
                    pullup = !settings->limits.disable_pullup.c;
                    input->invert = limit_fei.c;
                    break;
    #ifdef MPG_MODE_PIN
                case Input_MPGSelect:
                    pullup = true;
                    mpg_pin = input;
                    break;
    #endif
                case Input_I2CStrobe:
                    pullup = true;
                    input->mode.irq_mode = IRQ_Mode_Change;
                    break;

                case Input_SPIIRQ:
                    pullup = true;
                    input->mode.irq_mode = IRQ_Mode_Falling;
                    break;

                default:
                    break;
            }

            switch(input->group) {

                case PinGroup_Limit:
                case PinGroup_Control:
                    input->mode.irq_mode = input->invert ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                    break;

                case PinGroup_AuxInput:
                    pullup = true;
                    input->cap.irq_mode = IRQ_Mode_All;
                    break;

                default:
                    break;
            }

            gpio_set_pulls(input->pin, pullup, !pullup);
            gpio_set_inover(input->pin, input->invert ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);

            if (!(input->group == PinGroup_Limit || input->group == PinGroup_AuxInput))
                pinEnableIRQ(input, input->mode.irq_mode);

            if (input->id == Input_Probe)
                probeConfigure(false, false);
            else if(input->id == Input_SafetyDoor)
                input->active = DIGITAL_IN(input->bit);

            gpio_acknowledge_irq(input->pin, GPIO_IRQ_ALL);
        } while (i);

#if AUX_CONTROLS_ENABLED
        for(i = 0; i < AuxCtrl_NumEntries; i++) {
            if(aux_ctrl[i].enabled && aux_ctrl[i].irq_mode != IRQ_Mode_None) {
                if(aux_ctrl[i].irq_mode & (IRQ_Mode_Falling|IRQ_Mode_Rising))
                    aux_ctrl[i].irq_mode = (settings->control_invert.mask & aux_ctrl[i].cap.mask) ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                hal.port.register_interrupt_handler(aux_ctrl[i].port, aux_ctrl[i].irq_mode, aux_irq_handler);
            }
        }
#endif

        /*************************
         *  Output signals init  *
         *************************/

        for(uint_fast8_t i = 0; i < sizeof(outputpin) / sizeof(output_signal_t); i++) {
            if(outputpin[i].port == GPIO_OUTPUT)
              switch(outputpin[i].id) {

                case Output_SpindleOn:
                    gpio_set_outover(outputpin[i].pin, settings->spindle.invert.on ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
                    break;

                case Output_SpindleDir:
                    gpio_set_outover(outputpin[i].pin, settings->spindle.invert.ccw ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
                    break;

                case Output_CoolantMist:
                    gpio_set_outover(outputpin[i].pin, settings->coolant_invert.mist ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
                    break;

                case Output_CoolantFlood:
                    gpio_set_outover(outputpin[i].pin, settings->coolant_invert.flood ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
                    break;
            }
        }

        // Activate GPIO IRQ
        irq_set_priority(IO_IRQ_BANK0, NVIC_MEDIUM_LEVEL_PRIORITY); // By default all IRQ are medium priority but in case the GPIO IRQ would need high or low priority it can be done here
        irq_set_enabled(IO_IRQ_BANK0, true);                        // Enable GPIO IRQ
    }
}

static void enumeratePins (bool low_level, pin_info_ptr pin_info, void *data)
{
    static xbar_t pin = {0};
    uint32_t i = sizeof(inputpin) / sizeof(input_signal_t);

    pin.mode.input = On;

    for(i = 0; i < sizeof(inputpin) / sizeof(input_signal_t); i++) {
        pin.pin = inputpin[i].pin;
        pin.function = inputpin[i].id;
        pin.group = inputpin[i].group;
        //        pin.port = low_level ? NULL : (void *)port2char(inputpin[i].port);
        pin.mode.pwm = pin.group == PinGroup_SpindlePWM;
        pin.port = inputpin[i].port == GPIO_SR8 ? (void *)"SR8" : (inputpin[i].port == GPIO_SR16 ? (void *)"SR16" : NULL);
        pin.description = inputpin[i].description;

        pin_info(&pin, data);
    };

    pin.mode.mask = 0;
    pin.mode.output = On;

    for(i = 0; i < sizeof(outputpin) / sizeof(output_signal_t); i++) {
        pin.pin = outputpin[i].pin;
        pin.function = outputpin[i].id;
        pin.group = outputpin[i].group;
        pin.port = outputpin[i].port == GPIO_PIO ? (void *)"PIO" : (outputpin[i].port == GPIO_IOEXPAND ? (void *)"IOX" : (outputpin[i].port == GPIO_SR8 ? (void *)"SR8." : (outputpin[i].port == GPIO_SR16 ? (void *)"SR16." : NULL)));
        pin.description = outputpin[i].description;

        pin_info(&pin, data);
    };

    periph_signal_t *ppin = periph_pins;

    pin.port = NULL;

    if(ppin) do {
        pin.pin = ppin->pin.pin;
        pin.function = ppin->pin.function;
        pin.group = ppin->pin.group;
        pin.mode = ppin->pin.mode;
        pin.description = ppin->pin.description;

        pin_info(&pin, data);
    } while(ppin = ppin->next);
}

void registerPeriphPin (const periph_pin_t *pin)
{
    periph_signal_t *add_pin = malloc(sizeof(periph_signal_t));

    if(!add_pin)
        return;

    memcpy(&add_pin->pin, pin, sizeof(periph_pin_t));
    add_pin->next = NULL;

    if(periph_pins == NULL) {
        periph_pins = add_pin;
    } else {
        periph_signal_t *last = periph_pins;
        while (last->next)
            last = last->next;
        last->next = add_pin;
    }
}

void setPeriphPinDescription (const pin_function_t function, const pin_group_t group, const char *description)
{
    periph_signal_t *ppin = periph_pins;

    if(ppin) do {
        if(ppin->pin.function == function && ppin->pin.group == group) {
            ppin->pin.description = description;
            ppin = NULL;
        } else
            ppin = ppin->next;
    } while(ppin);
}

#ifdef NEOPIXELS_PIN

static void neopixels_write (void)
{
    // 50 us delay if busy? DMA?
    uint32_t *led = (uint32_t *)neopixel.leds;
//    uint64_t now = getElapsedMicros();
    
    while(pio_sm_get_tx_fifo_level(neop_pio, neop_sm) != 0);
//    while(getElapsedMicros() - now < 200);

    for(uint_fast16_t i = 0; i < neopixel.num_leds; i++)
        pio_sm_put_blocking(neop_pio, neop_sm, *led++);
}

static void neopixel_out_masked (uint16_t device, rgb_color_t color, rgb_color_mask_t mask)
{
    if(neopixel.num_leds && device < neopixel.num_leds) {

        uint8_t *led = &neopixel.leds[device * sizeof(uint32_t)] + 1;

        color = rgb_set_intensity(color, neopixel.intensity);

        if(mask.B)
            *led++ = color.B;
        else
            led++;     

        if(mask.R)
            *led++ = color.R;
        else
            led++;

        if(mask.G)
            *led = color.G;

       if(neopixel.num_leds == 1)
            neopixels_write();
    }
}

static void neopixel_out (uint16_t device, rgb_color_t color)
{
    neopixel_out_masked(device, color, (rgb_color_mask_t){ .mask = 0xFF });
}

static inline rgb_color_t rp_rgb_1bpp_unpack (uint8_t *led, uint8_t intensity)
{
    rgb_color_t color = {0};

    if(intensity) {

        color.B = *led++;
        color.R = *led++; 
        color.G = *led; 

        color = rgb_reset_intensity(color, intensity);
    }

    return color;
}

static uint8_t neopixels_set_intensity (uint8_t intensity)
{
    uint8_t prev = neopixel.intensity;

    if(neopixel.intensity != intensity) {

        neopixel.intensity = intensity;

        if(neopixel.num_leds) {

            uint_fast16_t device = neopixel.num_leds;
            do {
                device--;
                rgb_color_t color = rp_rgb_1bpp_unpack(&neopixel.leds[device * sizeof(uint32_t)] + 1, prev);
                neopixel_out(device, color);
            } while(device);

//            neopixels_write();
        }
    }

    return prev;
}

#elif defined(LED_G_PIN)

static void board_led_out (uint16_t device, rgb_color_t color)
{
    if(device == 0)
        DIGITAL_OUT(1 << LED_G_PIN, color.G != 0);
}

#elif WIFI_ENABLE || BLUETOOTH_ENABLE == 1

static void cyw43_led_on (void *data)
{
    if(net.value)
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, data != NULL);
}

static void cyw43_led_out (uint16_t device, rgb_color_t color)
{
    if(device == 0) {
        if(net.value) // Network stack is up?
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, color.G != 0);
        else
            protocol_enqueue_foreground_task(cyw43_led_on, color.G ? (void *)1 : NULL);
    }
}

#endif // NEOPIXELS_PIN

// Initializes MCU peripherals
static bool driver_setup (settings_t *settings)
{
    /*************************
     *  Output signals init  *
     *************************/

    for(uint_fast8_t i = 0; i < sizeof(outputpin) / sizeof(output_signal_t); i++) {
        if(outputpin[i].port == GPIO_OUTPUT && outputpin[i].group != PinGroup_AuxOutputAnalog) {
            outputpin[i].bit = 1 << outputpin[i].pin;

            gpio_init(outputpin[i].pin);
            if(outputpin[i].id == PinGroup_StepperEnable || outputpin[i].id == Output_SdCardCS)
                DIGITAL_OUT(outputpin[i].bit, 1);

            gpio_set_dir_out_masked(outputpin[i].bit);

            if(outputpin[i].group == PinGroup_SpindlePWM)
                gpio_set_function(outputpin[i].pin, GPIO_FUNC_PWM);
        }
    }

#if SDCARD_ENABLE
    sdcard_init();
#endif

#if MPG_MODE == 1
    gpio_init(MPG_MODE_PIN);
#endif

#if LITTLEFS_ENABLE && WEBUI_ENABLE
    fs_littlefs_mount("/littlefs", pico_littlefs_hal());
#endif

    IOInitDone = settings->version == 22;

    hal.settings_changed(settings, (settings_changed_flags_t){0});
    stepperSetDirOutputs((axes_signals_t){0});

#if PPI_ENABLE
    ppi_init();
#endif

    return IOInitDone;
}

static bool set_rtc_time (struct tm *time)
{
    static bool init_ok = false;

    if (!init_ok) {
        init_ok = true;
        rtc_init();
    }

    datetime_t dt = {0};
    dt.year = time->tm_year + 1900;
    dt.month = time->tm_mon + 1;
    dt.day = time->tm_mday;
    dt.hour = time->tm_hour;
    dt.min = time->tm_min;
    dt.sec = time->tm_sec;

    return rtc_set_datetime(&dt);
}

static bool get_rtc_time (struct tm *time)
{
    bool ok;
    datetime_t dt = {0};

    if((ok = rtc_running() && rtc_get_datetime(&dt))) {
        time->tm_year = dt.year - 1900;
        time->tm_mon = dt.month - 1;
        time->tm_mday = dt.day;
        time->tm_hour = dt.hour;
        time->tm_min = dt.min;
        time->tm_sec = dt.sec;
    }

    return ok;
}

extern char __StackLimit, __bss_end__;

uint32_t get_free_mem (void)
{
    return &__StackLimit - &__bss_end__ - mallinfo().uordblks;
}

#if STEP_PORT == GPIO_PIO_1

static bool assign_step_sm (PIO *pio, uint *sm, uint32_t pin)
{
    static uint offset = 0xFFFF;

    int32_t step_sm;

    if(offset == 0xFFFF)
        offset = pio_add_program(*pio, &step_pulse_program);

    if((step_sm = pio_claim_unused_sm(*pio, false)) == -1 && *pio == pio1) {
        *pio = pio0;
        step_sm = pio_claim_unused_sm(pio0, false);
        offset = pio_add_program(pio0, &step_pulse_program);
    }

    if(step_sm != -1) {
        *sm = (uint)step_sm;
        step_pulse_program_init(*pio, *sm, offset, pin, 1);
    }

    return step_sm != -1;
}

#endif

// Initialize HAL pointers, setup serial comms and enable EEPROM
// NOTE: grblHAL is not yet configured (from EEPROM data), driver_setup() will be called when done
bool driver_init (void)
{
    // Enable EEPROM and serial port here for grblHAL to be able to configure itself and report any errors

    // irq_set_exclusive_handler(-1, systick_handler);

    systick_hw->rvr = 999;
    systick_hw->cvr = 0;
    systick_hw->csr = M0PLUS_SYST_CSR_TICKINT_BITS | M0PLUS_SYST_CSR_ENABLE_BITS;

    hal.info = "RP2040";
    hal.driver_version = "240205";
    hal.driver_options = "SDK_" PICO_SDK_VERSION_STRING;
    hal.driver_url = GRBL_URL "/RP2040";
#ifdef BOARD_NAME
    hal.board = BOARD_NAME;
#endif
#ifdef BOARD_URL
    hal.board_url = BOARD_URL;
#endif

    hal.driver_setup = driver_setup;
    hal.f_step_timer = 10000000;
    hal.f_mcu = clock_get_hz(clk_sys) / 1000000UL;
    hal.rx_buffer_size = RX_BUFFER_SIZE;
    hal.get_free_mem = get_free_mem;
    hal.delay_ms = driver_delay;
    hal.settings_changed = settings_changed;

    hal.stepper.wake_up = stepperWakeUp;
    hal.stepper.go_idle = stepperGoIdle;
    hal.stepper.enable = stepperEnable;
    hal.stepper.cycles_per_tick = stepperCyclesPerTick;
    hal.stepper.pulse_start = stepperPulseStart;
    hal.stepper.motor_iterator = motor_iterator;
#ifdef GANGING_ENABLED
    hal.stepper.get_ganged = getGangedAxes;
#endif
#ifdef SQUARING_ENABLED
    hal.stepper.disable_motors = StepperDisableMotors;
#endif
#if STEP_INJECT_ENABLE
    hal.stepper.output_step = stepperOutputStep;
#endif

    hal.limits.enable = limitsEnable;
    hal.limits.get_state = limitsGetState;

    hal.coolant.set_state = coolantSetState;
    hal.coolant.get_state = coolantGetState;

#ifdef PROBE_PIN
    hal.probe.get_state = probeGetState;
    hal.probe.configure = probeConfigure;
#endif

    hal.control.get_state = systemGetState;

#if I2C_STROBE_BIT || SPI_IRQ_BIT
    hal.irq_claim = irq_claim;
#endif
    hal.get_micros = getElapsedMicros;
    hal.get_elapsed_ticks = getElapsedTicks;
    hal.set_bits_atomic = bitsSetAtomic;
    hal.clear_bits_atomic = bitsClearAtomic;
    hal.set_value_atomic = valueSetAtomic;
    hal.enumerate_pins = enumeratePins;
    hal.periph_port.register_pin = registerPeriphPin;
    hal.periph_port.set_pin_description = setPeriphPinDescription;

    hal.rtc.get_datetime = get_rtc_time;
    hal.rtc.set_datetime = set_rtc_time;

    serialRegisterStreams();

#if USB_SERIAL_CDC
    stream_connect(usb_serialInit());
#else
    if(!stream_connect_instance(SERIAL_STREAM, BAUD_RATE))
        while(true);
#endif

#ifdef I2C_PORT
    I2C_Init();
#endif

#if EEPROM_ENABLE
    i2c_eeprom_init();
#elif FLASH_ENABLE
    hal.nvs.type = NVS_Flash;
    hal.nvs.memcpy_from_flash = memcpy_from_flash;
    hal.nvs.memcpy_to_flash = memcpy_to_flash;
#else
    hal.nvs.type = NVS_None;
#endif

#if DRIVER_SPINDLE_ENABLE

 #if DRIVER_SPINDLE_PWM_ENABLE

    static const spindle_ptrs_t spindle = {
        .type = SpindleType_PWM,
        .config = spindleConfig,
        .set_state = spindleSetStateVariable,
        .get_state = spindleGetState,
        .get_pwm = spindleGetPWM,
        .update_pwm = spindleSetSpeed,
  #if PPI_ENABLE
        .pulse_on = spindlePulseOn,
  #endif
        .cap = {
            .gpio_controlled = On,
            .variable = On,
            .laser = On,
            .pwm_invert = On,
  #if DRIVER_SPINDLE_DIR_ENABLE
            .direction = On
  #endif
        }
    };

 #else

    static const spindle_ptrs_t spindle = {
        .type = SpindleType_Basic,
        .set_state = spindleSetState,
        .get_state = spindleGetState,
        .cap = {
            .gpio_controlled = On,
  #if DRIVER_SPINDLE_DIR_ENABLE
            .direction = On
  #endif
        }
    };

 #endif

    spindle_id = spindle_register(&spindle, DRIVER_SPINDLE_NAME);

#endif // DRIVER_SPINDLE_ENABLE

// driver capabilities

#if ESTOP_ENABLE
    hal.signals_cap.e_stop = On;
    hal.signals_cap.reset = Off;
#endif
    hal.limits_cap = get_limits_cap();
    hal.home_cap = get_home_cap();
#if defined(COOLANT_MIST_PIN) || OUT_SHIFT_REGISTER
    hal.driver_cap.mist_control = On;
#endif
    hal.driver_cap.software_debounce = On;
    hal.driver_cap.step_pulse_delay = On;
    hal.driver_cap.amass_level = 3;
    hal.driver_cap.control_pull_up = On;
    hal.driver_cap.limits_pull_up = On;
#ifdef PROBE_PIN
    hal.driver_cap.probe_pull_up = On;
#endif

    uint32_t i;
    input_signal_t *input;
    output_signal_t *output;

    static pin_group_pins_t aux_inputs = {0}, aux_outputs = {0}, aux_inputs_analog = {0}, aux_outputs_analog = {0};

    for(i = 0; i < sizeof(inputpin) / sizeof(input_signal_t); i++) {
        input = &inputpin[i];
        input->mode.input = input->cap.input = On;
        input->mode.pull_mode = input->cap.pull_mode = PullMode_Up;
        if(input->group == PinGroup_AuxInput) {
            if(aux_inputs.pins.inputs == NULL)
                aux_inputs.pins.inputs = input;
            input->id = Input_Aux0 + aux_inputs.n_pins++;
            input->cap.irq_mode = IRQ_Mode_All;
#if SAFETY_DOOR_ENABLE
            if(input->pin == SAFETY_DOOR_PIN)
                aux_ctrl[AuxCtrl_SafetyDoor].port = aux_inputs.n_pins - 1;
#endif
#if MOTOR_FAULT_ENABLE
            if(input->pin == MOTOR_FAULT_PIN)
                aux_ctrl[AuxCtrl_MotorFault].port = aux_inputs.n_pins - 1;
#endif
#if MOTOR_WARNING_ENABLE
            if(input->pin == MOTOR_WARNING_PIN)
                aux_ctrl[AuxCtrl_MotorWarning].port = aux_inputs.n_pins - 1;
#endif

        } else if(input->group == PinGroup_AuxInputAnalog) {
            if(aux_inputs_analog.pins.inputs == NULL)
                aux_inputs_analog.pins.inputs = input;
            input->id = (pin_function_t)(Input_Analog_Aux0 + aux_inputs_analog.n_pins++);
            input->bit = 1 << input->pin;
            input->mode.analog = input->cap.analog = On;
        } else if(input->group & (PinGroup_Limit|PinGroup_LimitMax)) {
            if(limit_inputs.pins.inputs == NULL)
                limit_inputs.pins.inputs = input;
            limit_inputs.n_pins++;
        }
#if SAFETY_DOOR_BIT
        if(input->id == Input_SafetyDoor)
            safety_door = input;
#endif
    }

    for(i = 0; i < sizeof(outputpin) / sizeof(output_signal_t); i++) {
        output = &outputpin[i];
        output->mode.output = On;
        if(output->group == PinGroup_AuxOutput) {
            if(aux_outputs.pins.outputs == NULL)
                aux_outputs.pins.outputs = output;
            output->id = Output_Aux0 + aux_outputs.n_pins++;
        } else if(output->group == PinGroup_AuxOutputAnalog) {
            if(aux_outputs_analog.pins.outputs == NULL)
                aux_outputs_analog.pins.outputs = output;
            output->mode.analog = On;
            output->id = Output_Analog_Aux0 + aux_outputs_analog.n_pins++;
        }
    }

#if !defined(HAS_BOARD_INIT) || !OUT_SHIFT_REGISTER
    if(aux_inputs.n_pins || aux_outputs.n_pins)
        ioports_init(&aux_inputs, &aux_outputs);
#endif

#ifdef HAS_BOARD_INIT
#if OUT_SHIFT_REGISTER
    board_init(&aux_inputs, &aux_outputs, &out_sr);
#else
    if(aux_inputs.n_pins || aux_outputs.n_pins)
        ioports_init(&aux_inputs, &aux_outputs);
    board_init();
#endif
#endif


  #ifndef MCP3221_ENABLE
    if(aux_inputs_analog.n_pins || aux_outputs_analog.n_pins)
  #endif
        ioports_init_analog(&aux_inputs_analog, &aux_outputs_analog);

#if SAFETY_DOOR_ENABLE
    aux_claim_explicit(&aux_ctrl[AuxCtrl_SafetyDoor]);
#elif defined(SAFETY_DOOR_PIN)
    hal.signals_cap.safety_door = On;
#endif

#if MOTOR_FAULT_ENABLE
    aux_claim_explicit(&aux_ctrl[AuxCtrl_MotorFault]);
#elif defined(MOTOR_FAULT_PIN)
    hal.signals_cap.motor_fault = On;
#endif

#if MOTOR_WARNING_ENABLE
    aux_claim_explicit(&aux_ctrl[AuxCtrl_MotorWarning]);
#elif defined(MOTOR_WARNING_PIN)
    hal.signals_cap.motor_warning = On;
#endif

#if AUX_CONTROLS_ENABLED
    for(i = AuxCtrl_ProbeDisconnect; i < AuxCtrl_NumEntries; i++) {
        if(aux_ctrl[i].enabled) {
            if((aux_ctrl[i].enabled = ioports_enumerate(Port_Digital, Port_Input, (pin_cap_t){ .irq_mode = aux_ctrl[i].irq_mode, .claimable = On }, aux_claim, (void *)&aux_ctrl[i])))
                hal.signals_cap.mask |= aux_ctrl[i].cap.mask;
        }
    }
#endif

#if MPG_MODE == 1
  #if KEYPAD_ENABLE == 2
    if((hal.driver_cap.mpg_mode = stream_mpg_register(stream_open_instance(MPG_STREAM, 115200, NULL, NULL), false, keypad_enqueue_keycode)))
        protocol_enqueue_foreground_task(mpg_enable, NULL);
  #else
    if((hal.driver_cap.mpg_mode = stream_mpg_register(stream_open_instance(MPG_STREAM, 115200, NULL, NULL), false, NULL)))
        protocol_enqueue_foreground_task(mpg_enable, NULL);
  #endif
#elif MPG_MODE == 2
    hal.driver_cap.mpg_mode = stream_mpg_register(stream_open_instance(MPG_STREAM, 115200, NULL, NULL), false, keypad_enqueue_keycode);
#elif MPG_MODE == 3
    hal.driver_cap.mpg_mode = stream_mpg_register(stream_open_instance(MPG_STREAM, 115200, NULL, NULL), false, stream_mpg_check_enable);
#elif KEYPAD_ENABLE == 2
    stream_open_instance(KEYPAD_STREAM, 115200, keypad_enqueue_keycode, "Keypad");
#endif

#if IOEXPAND_ENABLE
    ioexpand_init();
#endif

#if WIFI_ENABLE
    wifi_init();
#endif

#if ETHERNET_ENABLE
    enet_init();
#endif

#if BLUETOOTH_ENABLE == 1
    bluetooth_init_local();
#endif

// Stepper init

    uint32_t pio_offset;
#if WIFI_ENABLE || BLUETOOTH_ENABLE == 1
    pio_sm_claim(pio1, 0); // Reserve PIO state machine for cyw43 driver.
#endif

    stepper_timer_sm = pio_claim_unused_sm(pio1, false);
    stepper_timer_sm_offset = pio_add_program(pio1, &stepper_timer_program);
    stepper_timer_program_init(pio1, stepper_timer_sm, stepper_timer_sm_offset, 12.5f); // 10MHz

    //    irq_add_shared_handler(PIO1_IRQ_0, stepper_int_handler, 0);
    irq_set_exclusive_handler(PIO1_IRQ_0, stepper_int_handler);
    //    irq_set_priority(PIO1_IRQ_0, 0);

#if STEP_PORT == GPIO_PIO_1

    z_step_pio = pio1;

    assign_step_sm(&z_step_pio, &x_step_sm, X_STEP_PIN);
    assign_step_sm(&z_step_pio, &y_step_sm, Y_STEP_PIN);
    assign_step_sm(&z_step_pio, &z_step_sm, Z_STEP_PIN);

#if N_ABC_MOTORS

#if WIFI_ENABLE && N_ABC_MOTORS > 2
#error "Max number of motors with WIFI_ENABLE is 5"
#endif

#ifdef X2_STEP_PIN
    assign_step_sm(&z_step_pio, &x2_step_sm, X2_STEP_PIN);
#endif
#ifdef Y2_STEP_PIN
    assign_step_sm(&z_step_pio, &y2_step_sm, Y2_STEP_PIN);
#endif
#ifdef Z2_STEP_PIN
    assign_step_sm(&z_step_pio, &z2_step_sm, Z2_STEP_PIN);
#endif
#ifdef A_STEP_PIN
    assign_step_sm(&z_step_pio, &a_step_sm, A_STEP_PIN);
#endif
#ifdef B_STEP_PIN
    assign_step_sm(&z_step_pio, &b_step_sm, B_STEP_PIN);
#endif
#ifdef C_STEP_PIN
    assign_step_sm(&z_step_pio, &c_step_sm, C_STEP_PIN);
#endif

#endif // N_ABC_MOTORS

#elif STEP_PORT == GPIO_PIO

    pio_offset = pio_add_program(pio0, &step_pulse_program);
    step_pulse_program_init(pio0, 0, pio_offset, STEP_PINS_BASE, N_AXIS + N_GANGED);
    pio_sm_claim(pio0, 0);

#elif STEP_PORT == GPIO_SR8

    pio_offset = pio_add_program(pio0, &step_dir_sr4_program);
    step_dir_sr4_program_init(pio0, 0, pio_offset, SD_SR_DATA_PIN, SD_SR_SCK_PIN);

    pio_offset = pio_add_program(pio0, &sr_delay_program);
    sr_delay_program_init(pio0, 1, pio_offset, 11.65f);

    pio_offset = pio_add_program(pio0, &sr_hold_program);
    sr_hold_program_init(pio0, 2, pio_offset, 11.65f);

    pio_claim_sm_mask(pio0, 0b1111); // claim all state machines, no room for more programs

#endif

#if OUT_SHIFT_REGISTER
    out_sr_sm = (uint32_t)(pio_claim_unused_sm(pio1, false));
    pio_offset = pio_add_program(pio1, &out_sr16_program);
    out_sr16_program_init(pio1, out_sr_sm, pio_offset, OUT_SR_DATA_PIN, OUT_SR_SCK_PIN);
  #if SPI_RST_PORT == GPIO_SR16
    spi_reset_out(1);
  #endif
#endif

#ifdef NEOPIXELS_PIN

    int nsm;
    if((nsm = pio_claim_unused_sm((neop_pio = pio0), false)) == -1)
        nsm = pio_claim_unused_sm((neop_pio = pio1), false);

    if(nsm != -1 && pio_can_add_program(neop_pio, &ws2812_program)) {
        neop_sm = (uint)nsm;
        hal.rgb.out = neopixel_out;
        hal.rgb.out_masked = neopixel_out_masked;
        hal.rgb.write = neopixels_write;
        hal.rgb.set_intensity = neopixels_set_intensity;
        hal.rgb.num_devices = NEOPIXELS_NUM;
        hal.rgb.cap = (rgb_color_t){ .R = 255, .G = 255, .B = 255 };

        pio_offset = pio_add_program(neop_pio, &ws2812_program);
        ws2812_program_init(neop_pio, neop_sm, pio_offset, NEOPIXELS_PIN, 800000, false);
    } // else report unavailable?

#elif defined(LED_G_PIN)

    hal.rgb.out = board_led_out;
    hal.rgb.num_devices = 1;
    hal.rgb.cap = (rgb_color_t){ .R = 0, .G = 1, .B = 0 };

#elif WIFI_ENABLE || BLUETOOTH_ENABLE == 1

    hal.rgb.out = cyw43_led_out;
    hal.rgb.num_devices = 1;
    hal.rgb.cap = (rgb_color_t){ .R = 0, .G = 1, .B = 0 };

    hal.rgb.out(0, hal.rgb.cap);

#endif // NEOPIXELS_PIN

#include "grbl/plugins_init.h"

#if WIFI_ENABLE || BLUETOOTH_ENABLE == 1
    pio_sm_unclaim(pio1, 0);  // Release PIO state machine for cyw43 driver.
#endif

    //  debounceAlarmPool = alarm_pool_create(DEBOUNCE_ALARM_HW_TIMER, DEBOUNCE_ALARM_MAX_TIMER);

    // No need to move version check before init.
    // Compiler will fail any signature mismatch for existing entries.
    return hal.version == 10;
}

/* interrupt handlers */

// Main stepper driver
void __not_in_flash_func(stepper_int_handler)(void)
{
    stepper_timer_irq_clear(pio1);

    hal.stepper.interrupt_callback();
}

// Limit debounce callback
static int64_t __not_in_flash_func(limit_debounce_callback)(alarm_id_t id, void *input)
{
    if(((input_signal_t *)input)->debounce) {

        ((input_signal_t *)input)->debounce = false;
        gpio_set_irq_enabled(((input_signal_t *)input)->pin, ((input_signal_t *)input)->invert ? GPIO_IRQ_EDGE_FALL : GPIO_IRQ_EDGE_RISE, true);

        limit_signals_t state = limitsGetState();
        if(limit_signals_merge(state).value)
            hal.limits.interrupt_callback(state);
    }

    return 0;
}

// SR Latch callback - used to delay resetting of pins after they are triggered
static int64_t __not_in_flash_func(srLatch_debounce_callback)(alarm_id_t id, void *input)
{
    if(((input_signal_t *)input)->id == Input_Probe)
        gpio_set_irq_enabled(((input_signal_t *)input)->pin, probe.inverted ? GPIO_IRQ_LEVEL_HIGH : GPIO_IRQ_LEVEL_LOW, true);
    else if(((input_signal_t *)input)->id == Input_SafetyDoor)
        gpio_set_irq_enabled(((input_signal_t *)input)->pin, ((input_signal_t *)input)->invert ? GPIO_IRQ_LEVEL_HIGH : GPIO_IRQ_LEVEL_LOW, true);

    return 0;
}

#if PPI_ENABLE

// PPI timer interrupt handler
void __not_in_flash_func(PPI_TIMER_IRQHandler)(void)
{
    PPI_TIMER->SR = ~TIM_SR_UIF; // clear UIF flag;

    spindle_off();
}

#endif

// GPIO Interrupt handler
// TODO: bypass the Pico library interrupt handler.
void __not_in_flash_func(gpio_int_handler)(uint gpio, uint32_t events)
{
    input_signal_t *input = NULL;
    uint32_t i = sizeof(inputpin) / sizeof(input_signal_t);

    do {
        if(inputpin[--i].pin == gpio)
            input = &inputpin[i];
    } while(i && !input);

    if(input) switch (input->group) {

        case PinGroup_Control:

#ifdef SAFETY_DOOR_BIT
            if(input->id == Input_SafetyDoor) {
                gpio_set_irq_enabled(gpio, GPIO_IRQ_ALL, false);
                // If the input is active fire the control interrupt immediately and register an
                // alarm to reenable the interrupt after a short delay. Only after this delay has
                // expired can the safety door signal be set inactive. This is done to avoid stressing
                // the main state-machine.
                if((input->active = !!(events & GPIO_IRQ_LEVEL_HIGH) ^ input->invert)) {
                    hal.control.interrupt_callback(systemGetState());
                    if (!add_alarm_in_ms(DEBOUNCE_DELAY, srLatch_debounce_callback, (void *)input, false))
                        gpio_set_irq_enabled(gpio, GPIO_IRQ_LEVEL_HIGH, true); // Reenable the IRQ in case the alarm wasn't registered.
                } else
                    gpio_set_irq_enabled(gpio, !input->invert ? GPIO_IRQ_LEVEL_HIGH : GPIO_IRQ_LEVEL_LOW, true);
            } else
#endif
                hal.control.interrupt_callback(systemGetState());
            break;

#ifdef PROBE_PIN
        case PinGroup_Probe:
            gpio_set_irq_enabled(gpio, GPIO_IRQ_ALL, false);
            // If input is active set the probe signal active immediately and register an
            // alarm to reenable the interrupt after a short delay. Only after this delay has
            // expired can the probe signal be set inactive.
            if((probe.triggered = !!(events & GPIO_IRQ_LEVEL_HIGH) ^ probe.inverted)) {
                if(!add_alarm_in_ms(DEBOUNCE_DELAY, srLatch_debounce_callback, (void *)input, false))
                    gpio_set_irq_enabled(gpio, probe.inverted ? GPIO_IRQ_LEVEL_HIGH : GPIO_IRQ_LEVEL_LOW, true); // Reenable the IRQ in case the alarm wasn't registered.
            } else
                gpio_set_irq_enabled(gpio, probe.inverted ? GPIO_IRQ_LEVEL_LOW : GPIO_IRQ_LEVEL_HIGH, true);
            break;
#endif

        case PinGroup_Limit:
        case PinGroup_LimitMax:
        {
            // If debounce is enabled register an alarm to reenable the IRQ after the debounce delay has expired.
            // If the input is still active when the delay expires the limits interrupt will be fired.
            if(hal.driver_cap.software_debounce && add_alarm_in_ms(DEBOUNCE_DELAY, limit_debounce_callback, (void *)input, true)) {
                input->debounce = true;
                gpio_set_irq_enabled(gpio, GPIO_IRQ_ALL, false); // Disable the pin IRQ for the duration of the debounce delay.
            } else
                hal.limits.interrupt_callback(limitsGetState());
        }
        break;

        case PinGroup_AuxInput:
            ioports_event(input);
            break;

#if I2C_STROBE_BIT
        case PinGroup_I2C:
            if(input->id == Input_I2CStrobe && i2c_strobe.callback)
                i2c_strobe.callback(0, DIGITAL_IN(input->bit) == 0);
            break;
#endif
#if SPI_IRQ_BIT
        case PinGroup_SPI:
            if(input->id == Input_SPIIRQ && spi_irq.callback)
                spi_irq.callback(0, DIGITAL_IN(input->bit) == 0);
            break;
#endif
#if MPG_MODE == 1
        case PinGroup_MPG:
            pinEnableIRQ(input, IRQ_Mode_None);
            protocol_enqueue_foreground_task(mpg_select, NULL);
            break;
#endif
        default:
            break;
    }
}

// Interrupt handler for 1 ms interval timer
void __not_in_flash_func(isr_systick)(void)
{
    elapsed_ticks++;

#if SDCARD_ENABLE
    static uint32_t fatfs_ticks = 10;
    if(!(--fatfs_ticks)) {
        disk_timerproc();
        fatfs_ticks = 10;
    }
#endif
}
