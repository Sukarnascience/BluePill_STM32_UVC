/*
 * main.c — Blue Pill CDC
 * KEY FIX: Use HSE (8MHz crystal) × 6 = 48MHz, NOT HSI
 * HSI is ±1% — too inaccurate for USB (needs ±0.25%)
 * HSE crystal on Blue Pill is 8MHz → PLL × 6 = 48MHz
 */

#include "stm32f1xx_hal.h"

void usb_core_init(void);
void usb_core_poll(void);
void SysTick_Handler(void) { HAL_IncTick(); }

int main(void)
{
    HAL_Init();

    /* Pull D+ low immediately before anything else */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_12; g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &g);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);

    /* LED on */
    __HAL_RCC_GPIOC_CLK_ENABLE();
    g.Pin = GPIO_PIN_13;
    HAL_GPIO_Init(GPIOC, &g);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);

    /*
     * Clock: HSE 8MHz × PLL6 = 48MHz
     * Blue Pill has 8MHz crystal — this is what the bootloader uses too.
     * USB REQUIRES crystal-based clock. HSI is too inaccurate.
     */
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL     = RCC_PLL_MUL6;   /* 8MHz × 6 = 48MHz */
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        /* HSE failed — blink fast forever to signal error */
        while (1) {
            HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
            for (volatile int i = 0; i < 100000; i++);
        }
    }

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                         RCC_CLOCKTYPE_PCLK1  | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;   /* APB1 max 36MHz → 24MHz */
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_1);

    /* USB clock = SYSCLK (48MHz) — no divider needed */
    RCC_PeriphCLKInitTypeDef pclk = {0};
    pclk.PeriphClockSelection = RCC_PERIPHCLK_USB;
    pclk.UsbClockSelection    = RCC_USBCLKSOURCE_PLL;  /* PLL = 48MHz direct */
    HAL_RCCEx_PeriphCLKConfig(&pclk);

    /* 3 blinks to confirm we got past clock init */
    for (int i = 0; i < 3; i++) {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
        HAL_Delay(150);
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
        HAL_Delay(150);
    }

    /* Init USB — releases D+ at end */
    usb_core_init();

    uint32_t last = 0; int st = 0;
    while (1) {
        usb_core_poll();
        if (HAL_GetTick() - last >= 500u) {
            last = HAL_GetTick();
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13,
                st ? GPIO_PIN_RESET : GPIO_PIN_SET);
            st ^= 1;
        }
    }
}