/*
 * Copyright 2021 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* FreeRTOS kernel includes. */
#include "FreeRTOS.h"
#include "task.h"

/* harpoon-apps includes. */
#include "board.h"
#include "board_test.h"
#include "clock_config.h"
#include "i2c_test.h"
#ifdef I2C_USE_IRQ
#include "irq.h"
#endif
#include "pin_mux.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define test_task_PRIORITY (configMAX_PRIORITIES - 1)

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void test_task(void *pvParameters);

/*******************************************************************************
 * Code
 ******************************************************************************/
/*!
 * @brief Application entry point.
 */
int main(void)
{
    BaseType_t xResult;

    /* Init board cpu and hardware. */
    BOARD_InitMemory();
    BOARD_InitDebugConsole();
    BOARD_InitPins();

    board_clock_setup();

    xResult = xTaskCreate(test_task, "driver_test_task", configMINIMAL_STACK_SIZE + 100, NULL, test_task_PRIORITY, NULL);
    assert(xResult == pdPASS);

    vTaskStartScheduler();

    for (;;)
        ;

    return xResult;
}

#ifdef I2C_USE_IRQ
static void i2c_irq_handler(void *data)
{
    i2c_test_irq_handler();
}
#endif

static void test_i2c(void)
{
#ifdef I2C_USE_IRQ
    /* Register I2C3 interrupt */
    irq_register(I2C3_IRQn, i2c_irq_handler, NULL, 0);
#endif

    /* Run i2c tests */
    i2c_test();

#ifdef I2C_USE_IRQ
    /* Unregister I2C3 interrupt */
    irq_unregister(I2C3_IRQn);
#endif
}

static void common_test_list(void)
{
    test_i2c();
}

static void test_task(void *pvParameters)
{
    common_test_list();
    board_test_list();

    for (;;)
        ;
}
