#include "RTE_Components.h"
#include CMSIS_device_header


#include "board_config.h"
#include "Driver_IO.h"
#include "pinconf.h"
#include "ospi_xip_user.h"
#include "stdio.h"

#define OSPI_RESET_PORT 15
#define OSPI_RESET_PIN  7

extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(OSPI_RESET_PORT);
ARM_DRIVER_GPIO       *GPIODrv = &ARM_Driver_GPIO_(OSPI_RESET_PORT);

int init_ospi_flash(void)
{
    int32_t ret;

        ret = pinconf_set(OSPI1_D0_PORT,
                      OSPI1_D0_PIN,
                      OSPI1_D0_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA | PADCTRL_SLEW_RATE_FAST |
                          PADCTRL_READ_ENABLE);
    if (ret) {
        return 0;
    }

    ret = pinconf_set(OSPI1_D1_PORT,
                      OSPI1_D1_PIN,
                      OSPI1_D1_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA | PADCTRL_SLEW_RATE_FAST |
                          PADCTRL_READ_ENABLE);
    if (ret) {
        return 0;
    }

    ret = pinconf_set(OSPI1_D2_PORT,
                      OSPI1_D2_PIN,
                      OSPI1_D2_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA | PADCTRL_SLEW_RATE_FAST |
                          PADCTRL_READ_ENABLE);
    if (ret) {
        return 0;
    }

    ret = pinconf_set(OSPI1_D3_PORT,
                      OSPI1_D3_PIN,
                      OSPI1_D3_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA | PADCTRL_SLEW_RATE_FAST |
                          PADCTRL_READ_ENABLE);
    if (ret) {
        return 0;
    }

    ret = pinconf_set(OSPI1_D4_PORT,
                      OSPI1_D4_PIN,
                      OSPI1_D4_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA | PADCTRL_SLEW_RATE_FAST |
                          PADCTRL_READ_ENABLE);
    if (ret) {
        return 0;
    }

    ret = pinconf_set(OSPI1_D5_PORT,
                      OSPI1_D5_PIN,
                      OSPI1_D5_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA | PADCTRL_SLEW_RATE_FAST |
                          PADCTRL_READ_ENABLE);
    if (ret) {
        return 0;
    }

    ret = pinconf_set(OSPI1_D6_PORT,
                      OSPI1_D6_PIN,
                      OSPI1_D6_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA | PADCTRL_SLEW_RATE_FAST |
                          PADCTRL_READ_ENABLE);
    if (ret) {
        return 0;
    }

    ret = pinconf_set(OSPI1_D7_PORT,
                      OSPI1_D7_PIN,
                      OSPI1_D7_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA | PADCTRL_SLEW_RATE_FAST |
                          PADCTRL_READ_ENABLE);
    if (ret) {
        return 0;
    }

    ret = pinconf_set(OSPI1_RXDS_PORT,
                      OSPI1_RXDS_PIN,
                      OSPI1_RXDS_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA | PADCTRL_SLEW_RATE_FAST |
                          PADCTRL_READ_ENABLE);
    if (ret) {
        return 0;
    }

    ret = pinconf_set(OSPI1_SCLK_PORT,
                      OSPI1_SCLK_PIN,
                      OSPI1_SCLK_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA | PADCTRL_SLEW_RATE_FAST);
    if (ret) {
        return 0;
    }

    ret = pinconf_set(OSPI1_CS_PORT,
                      OSPI1_CS_PIN,
                      OSPI1_CS_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA);
    if (ret) {
        return 0;
    }

    ret = pinconf_set(OSPI1_SCLKN_PORT,
                      OSPI1_SCLKN_PIN,
                      OSPI1_SCLKN_PIN_FUNCTION,
                      PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA);
    if (ret) {
        return 0;
    }

    ret = GPIODrv->Initialize(OSPI_RESET_PIN, NULL);
    if (ret != ARM_DRIVER_OK) {
        printf("Failed to initialize GPIO for OSPI reset\n");
        return 0;
    }

    ret = GPIODrv->PowerControl(OSPI_RESET_PIN, ARM_POWER_FULL);
    if (ret != ARM_DRIVER_OK) {
        printf("Failed to set power for GPIO OSPI reset\n");
        return 0;
    }

    ret = GPIODrv->SetDirection(OSPI_RESET_PIN, GPIO_PIN_DIRECTION_OUTPUT);
    if (ret != ARM_DRIVER_OK) {
        printf("Failed to set direction for GPIO OSPI reset\n");
        return 0;
    }

    ret = GPIODrv->SetValue(OSPI_RESET_PIN, GPIO_PIN_OUTPUT_STATE_LOW);
    if (ret != ARM_DRIVER_OK) {
        printf("Failed to set value LOW for GPIO OSPI reset\n");
        return 0;
    }

    ret = GPIODrv->SetValue(OSPI_RESET_PIN, GPIO_PIN_OUTPUT_STATE_HIGH);
    if (ret != ARM_DRIVER_OK) {
        printf("Failed to set value HIGH for GPIO OSPI reset\n");
        return 0;
    }

    return 1;
}
