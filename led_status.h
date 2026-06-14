/*
 * led_status.h - Onboard LED status feedback
 *
 * Uses the Pico 2 W's onboard LED (controlled via CYW43 GPIO)
 * to indicate Bluetooth connection and streaming state.
 */

#ifndef LED_STATUS_H
#define LED_STATUS_H

typedef enum {
  LED_STATE_OFF,          // Error or not initialized
  LED_STATE_DISCOVERABLE, // Slow blink (1 Hz) - waiting for pairing
  LED_STATE_CONNECTED,    // Fast blink (4 Hz) - connected, not streaming
  LED_STATE_STREAMING,    // Solid ON - connected and streaming audio
} led_state_t;

/*
 * Initialize the LED status system.
 * Sets up a repeating timer to drive the blink patterns.
 */
void led_status_init(void);

/*
 * Set the current LED state.
 * Can be called from any context (ISR-safe).
 */
void led_status_set(led_state_t state);

#endif // LED_STATUS_H
