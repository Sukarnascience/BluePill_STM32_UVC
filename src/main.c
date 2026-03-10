/*
 * main.c — STM32F103C8T6 UVC Camera (Test Pattern)
 *
 * What this does:
 *   1. Configure STM32F103 clocks: HSE 8MHz → PLL → 72MHz SYSCLK
 *      (also generates 48MHz for USB via PLL/1.5 = 48MHz)
 *   2. Initialize USB peripheral as UVC device
 *   3. Poll USB events forever in main loop
 *
 * Hardware:
 *   PA11 — USB D−
 *   PA12 — USB D+ (needs 1.5kΩ pull-up to 3.3V)
 *   PC13 — LED (active LOW on Blue Pill) — blinks when streaming
 *
 * libopencm3 clock note:
 *   STM32F103 USB requires exactly 48MHz on USBCLK.
 *   With 8MHz HSE: PLL multiplier = 9 → SYSCLK = 72MHz
 *                  USB prescaler = /1.5 → USBCLK = 48MHz ✓
 */

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/cm3/systick.h>
#include "usb_uvc.h"

/* ============================================================
   Simple ms counter using SysTick
   ============================================================ */
static volatile uint32_t s_ticks = 0;

void sys_tick_handler(void)
{
    s_ticks++;
}

static void systick_init(void)
{
    /* 72MHz / 72000 = 1kHz = 1ms tick */
    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
    systick_set_reload(72000 - 1);
    systick_interrupt_enable();
    systick_counter_enable();
}

static uint32_t millis(void)
{
    return s_ticks;
}

/* ============================================================
   Clock Configuration
   
   Target: SYSCLK = 72MHz, USBCLK = 48MHz
   
   HSE = 8MHz (Blue Pill crystal)
   PLL source = HSE
   PLL multiplier = 9 → PLLCLK = 72MHz
   AHB prescaler = 1  → HCLK = 72MHz
   APB1 prescaler = 2 → PCLK1 = 36MHz (max for APB1)
   APB2 prescaler = 1 → PCLK2 = 72MHz
   USB prescaler = /1.5 → USBCLK = 48MHz ✓
   ============================================================ */
static void clock_init(void)
{
    /* libopencm3 provides this exact config as a preset */
    rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]);
}

/* ============================================================
   GPIO — LED on PC13 (active LOW on Blue Pill)
   ============================================================ */
static void gpio_init(void)
{
    /* Enable GPIOC clock */
    rcc_periph_clock_enable(RCC_GPIOC);

    /* PC13 as output (LED) */
    gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ,
                  GPIO_CNF_OUTPUT_PUSHPULL, GPIO13);

    /* LED off initially (active LOW → set HIGH = off) */
    gpio_set(GPIOC, GPIO13);
}

static void led_on(void)  { gpio_clear(GPIOC, GPIO13); }  /* Active LOW */
static void led_off(void) { gpio_set(GPIOC, GPIO13);   }

/* ============================================================
   Main
   ============================================================ */
int main(void)
{
    clock_init();
    systick_init();
    gpio_init();

    /*
     * Initialize USB as UVC device.
     * This sets up descriptors, registers callbacks,
     * and starts the USB peripheral.
     *
     * After this, plugging the USB cable will cause enumeration.
     * The D+ pull-up on PA12 signals Full-Speed to the host.
     */
    usb_uvc_init();

    uint32_t last_blink = 0;
    int led_state = 0;

    while (1) {
        /*
         * usb_uvc_poll() MUST be called as fast as possible.
         * It drives the entire USB state machine:
         *   - Handles enumeration GET_DESCRIPTOR requests
         *   - Handles UVC Probe/Commit control requests
         *   - Triggers isochronous callbacks
         *
         * Any delay here = missed USB packets = glitchy video.
         * Do NOT put delays or heavy computation in this loop.
         */
        usb_uvc_poll();

        /*
         * LED blink — indicates streaming status:
         *   Fast blink (200ms) = streaming active
         *   Slow blink (1000ms) = idle / waiting for host
         */
        uint32_t blink_period = usb_uvc_is_streaming() ? 200 : 1000;
        uint32_t now = millis();

        if ((now - last_blink) >= blink_period) {
            last_blink = now;
            if (led_state) {
                led_on();
                led_state = 0;
            } else {
                led_off();
                led_state = 1;
            }
        }
    }

    return 0; /* Never reached */
}