#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include <stdbool.h>

// GPIO definitions for buttons
#define BTN_GPIO_PLAY_PAUSE 2
#define BTN_GPIO_MUTE       3
#define BTN_GPIO_VOL_UP     4
#define BTN_GPIO_VOL_DOWN   5
#define BTN_GPIO_NEXT       6
#define BTN_GPIO_PREV       7
#define BTN_GPIO_PAIRING    8

// Initialize the button handler (configures GPIOs and starts polling timer)
void button_handler_init(void);

#endif // BUTTON_HANDLER_H
