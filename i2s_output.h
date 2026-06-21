/*
 * i2s_output.h - PIO-based I2S audio output for CJMCU-1334 DAC
 *
 * Configures the RP2350 PIO to generate I2S signals (BCLK, WSEL, DIN)
 * and uses DMA to stream PCM audio data without CPU intervention.
 */

#ifndef I2S_OUTPUT_H
#define I2S_OUTPUT_H

#include "pico/audio_i2s.h"
#include <stdbool.h>
#include <stdint.h>

// Audio format parameters
#define I2S_SAMPLE_RATE     44100
#define I2S_BITS_PER_SAMPLE 16
#define I2S_NUM_CHANNELS    2

// Buffer configuration
#define I2S_BUFFER_COUNT       4
#define I2S_SAMPLES_PER_BUFFER 256

// DAC Mute Pin
#define DAC_MUTE_PIN 15

/*
 * Initialize the I2S output hardware.
 * Sets up PIO state machine, DMA channel, and audio buffer pool.
 * Pin assignments are taken from compile definitions:
 * - PICO_AUDIO_I2S_DATA_PIN
 * - PICO_AUDIO_I2S_CLOCK_PIN_BASE
 * Returns true if successful.
 */
bool i2s_output_init(void);

/*
 * Set the hardware mute state of the DAC.
 * true = muted, false = unmuted.
 */
void i2s_output_set_mute(bool mute);

/*
 * Update the sample rate if the A2DP source negotiates a different rate.
 * Supported values: 44100, 48000
 */
void i2s_output_set_sample_rate(uint32_t sample_rate);

/*
 * Get the audio buffer pool for writing PCM samples.
 * The BT audio module uses this to feed decoded PCM data to the I2S output.
 */
audio_buffer_pool_t *i2s_output_get_producer_pool(void);

// Play a 440Hz test tone for 1 second
void play_test_tone(void);

#endif // I2S_OUTPUT_H
