#include "button_handler.h"
#include "bt_audio.h"
#include "btstack.h"
#include "pico/stdlib.h"
#include <stdio.h>

// Define the buttons we are tracking
typedef struct {
  uint gpio;
  bool last_state; // True if pressed
  uint8_t debounce_count;
  void (*on_press)(void);
} button_t;

static button_t buttons[] = {
    {BTN_GPIO_PLAY_PAUSE, false, 0, bt_audio_cmd_play_pause},
    {BTN_GPIO_MUTE, false, 0, bt_audio_cmd_mute},
    {BTN_GPIO_VOL_UP, false, 0, bt_audio_cmd_vol_up},
    {BTN_GPIO_VOL_DOWN, false, 0, bt_audio_cmd_vol_down},
    {BTN_GPIO_NEXT, false, 0, bt_audio_cmd_next},
    {BTN_GPIO_PREV, false, 0, bt_audio_cmd_prev},
    {BTN_GPIO_PAIRING, false, 0, bt_audio_cmd_pairing}};
#define NUM_BUTTONS (sizeof(buttons) / sizeof(buttons[0]))

static btstack_timer_source_t button_timer;

static void button_timer_handler(btstack_timer_source_t *ts) {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    // Read active-low GPIO (Pull-up means LOW is pressed)
    bool current_state = !gpio_get(buttons[i].gpio);

    if (current_state == buttons[i].last_state) {
      buttons[i].debounce_count = 0;
    } else {
      buttons[i].debounce_count++;
      // If stable for 2 consecutive polls (40ms)
      if (buttons[i].debounce_count >= 2) {
        buttons[i].last_state = current_state;
        buttons[i].debounce_count = 0;

        // Log the state change
        printf("[btn] GPIO %d is now %s\n", buttons[i].gpio,
               current_state ? "PRESSED" : "RELEASED");

        // Trigger on press
        if (current_state && buttons[i].on_press) {
          buttons[i].on_press();
        }
      }
    }
  }

  // Restart timer
  btstack_run_loop_set_timer(ts, 5);
  btstack_run_loop_add_timer(ts);
}

void button_handler_init(void) {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    gpio_init(buttons[i].gpio);
    gpio_set_dir(buttons[i].gpio, GPIO_IN);
    gpio_pull_up(buttons[i].gpio);
  }

  // Setup BTstack timer
  btstack_run_loop_set_timer_handler(&button_timer, button_timer_handler);
  btstack_run_loop_set_timer(&button_timer, 5);
  btstack_run_loop_add_timer(&button_timer);
}
