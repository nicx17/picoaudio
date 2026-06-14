/*
 * led_status.c - Onboard LED status feedback
 *
 * Drives the Pico 2 W onboard LED via cyw43_arch_gpio_put().
 * A repeating timer runs at 50ms intervals to handle blink patterns
 * without blocking the main loop.
 */

#include "led_status.h"

#include "btstack_run_loop.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include <stdio.h>

// Current state
static led_state_t current_state = LED_STATE_OFF;

// Timer tick counter for blink patterns
static uint32_t tick_count = 0;

// BTstack timer
static btstack_timer_source_t led_timer;

static void led_timer_handler(btstack_timer_source_t *ts) {
  tick_count++;

  bool led_on = false;

  switch (current_state) {
  case LED_STATE_OFF:
    led_on = false;
    break;

  case LED_STATE_DISCOVERABLE:
    // Slow blink: 1 Hz (500ms on, 500ms off)
    // At 50ms per tick: 10 ticks per half-cycle
    led_on = (tick_count % 20) < 10;
    break;

  case LED_STATE_CONNECTED:
    // Fast blink: 4 Hz (125ms on, 125ms off)
    // At 50ms per tick: ~2-3 ticks per half-cycle
    led_on = (tick_count % 5) < 3;
    break;

  case LED_STATE_STREAMING:
    led_on = true;
    break;
  }

  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);

  // Re-arm timer for 50ms
  btstack_run_loop_set_timer(ts, 50);
  btstack_run_loop_add_timer(ts);
}

void led_status_init(void) {
  printf("[led] Initializing LED status\n");

  // Setup BTstack timer
  btstack_run_loop_set_timer_handler(&led_timer, &led_timer_handler);
  btstack_run_loop_set_timer(&led_timer, 50);
  btstack_run_loop_add_timer(&led_timer);

  // Start in OFF state
  current_state = LED_STATE_OFF;
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
}

void led_status_set(led_state_t state) {
  if (current_state != state) {
    current_state = state;
    tick_count = 0; // Reset blink phase on state change

    const char *state_names[] = {"OFF", "DISCOVERABLE", "CONNECTED",
                                 "STREAMING"};
    printf("[led] State -> %s\n", state_names[state]);
  }
}
