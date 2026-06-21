/*
 * main.c - Bluetooth Audio Receiver Entry Point
 *
 * Initializes the CYW43 wireless subsystem, I2S audio output,
 * LED status feedback, and Bluetooth A2DP Sink. Then enters the
 * BTstack run loop which handles all Bluetooth events.
 *
 * Target: Raspberry Pi Pico 2 W (RP2350 + CYW43439)
 * DAC:    CJMCU-1334 (UDA1334A) via I2S on GPIO 16/17/18
 *
 * Copyright (C) 2024 - Licensed under GPLv3
 */

#include "btstack.h"
#include "hardware/clocks.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <stdio.h>

#include "bt_audio.h"
#include "button_handler.h"
#include "i2s_output.h"
#include "led_status.h"

int main(void) {
  // Initialize stdio for USB debug output
  stdio_init_all();

  // Wait for USB enumeration
  sleep_ms(2000);

  puts(" ██▓███   ██▓ ▄████▄   ▒█████   ▄▄▄       █    ██ ▓█████▄  ██▓ ▒█████ "
       " ");
  puts("▓██░  ██▒▓██▒▒██▀ ▀█  ▒██▒  ██▒▒████▄     ██  ▓██▒▒██▀ ██▌▓██▒▒██▒  "
       "██▒");
  puts("▓██░ ██▓▒▒██▒▒▓█    ▄ ▒██░  ██▒▒██  ▀█▄  ▓██  ▒██░░██   █▌▒██▒▒██░  "
       "██▒");
  puts("▒██▄█▓▒ ▒░██░▒▓▓▄ ▄██▒▒██   ██░░██▄▄▄▄██ ▓▓█  ░██░░▓█▄   ▌░██░▒██   "
       "██░");
  puts("▒██▒ ░  ░░██░▒ ▓███▀ ░░ ████▓▒░ ▓█   ▓██▒▒▒█████▓ ░▒████▓ ░██░░ "
       "████▓▒░");
  puts("▒▓▒░ ░  ░░▓  ░ ░▒ ▒  ░░ ▒░▒░▒░  ▒▒   ▓▒█░░▒▓▒ ▒ ▒  ▒▒▓  ▒ ░▓  ░ "
       "▒░▒░▒░ ");
  puts("░▒ ░      ▒ ░  ░  ▒     ░ ▒ ▒░   ▒   ▒▒ ░░░▒░ ░ ░  ░ ▒  ▒  ▒ ░  ░ ▒ "
       "▒░ ");
  puts("░░        ▒ ░░        ░ ░ ░ ▒    ░   ▒    ░░░ ░ ░  ░ ░  ▒  ▒ ░░ ░ ░ ▒ "
       " ");
  puts("          ░  ░ ░          ░ ░        ░  ░   ░        ░     ░      ░ ░ "
       " ");
  puts("             ░                                   ░                   ");
  puts("======================================================================"
       "=");
  puts(" Bluetooth Audio Receiver (Pico 2 W + CJMCU-1334)");
  puts("======================================================================="
       "\n");

  // Initialize the CYW43 wireless subsystem
  // Using _none variant: no background Wi-Fi processing needed
  printf("[main] Initializing CYW43...\n");
  if (cyw43_arch_init()) {
    printf("[main] ERROR: CYW43 init failed\n");
    return -1;
  }
  printf("[main] CYW43 initialized\n");

  // Initialize LED status feedback
  led_status_init();
  led_status_set(LED_STATE_OFF);

  // Initialize I2S audio output to the DAC
  printf("[main] Initializing I2S output...\n");
  if (!i2s_output_init()) {
    printf("[main] FATAL: Failed to initialize I2S\n");
    return -1;
  }

  // Initialize Bluetooth A2DP Sink + AVRCP
  printf("[main] Initializing Bluetooth audio...\n");
  if (!bt_audio_init()) {
    printf("[main] ERROR: Bluetooth audio init failed\n");
    return -1;
  }

  // Initialize Physical Buttons (Must be after bt_audio_init which initializes
  // BTstack run loop)
  button_handler_init();

  // Launch the Dual-Core Audio Decoder on Core 1
  printf("[main] Launching Dual-Core Audio Decoder on Core 1...\n");
  multicore_launch_core1(core1_audio_decoder);

  printf("[main] All systems initialized. Starting BTstack...\n");
  printf("[main] Waiting for Bluetooth connection...\n");

  // Play powerup sound
  bt_audio_play_powerup_sound();

  // Power on Bluetooth and enter the BTstack run loop
  // This call does not return -- BTstack processes events in an infinite loop
  hci_power_control(HCI_POWER_ON);
  btstack_run_loop_execute();

  // Should never reach here
  return 0;
}
