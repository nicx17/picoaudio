/*
 * i2s_output.c - PIO-based I2S audio output for CJMCU-1334 DAC
 *
 * Uses pico_audio_i2s (from pico-extras) to configure a PIO state machine
 * that generates I2S BCLK, WSEL, and DIN signals. DMA transfers PCM samples
 * from a buffer pool to the PIO TX FIFO without CPU intervention.
 */

#include "i2s_output.h"

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/audio_i2s.h"

// Audio format descriptor (mutable, updated when sample rate changes)
static audio_format_t audio_format = {
    .format = AUDIO_BUFFER_FORMAT_PCM_S16,
    .sample_freq = I2S_SAMPLE_RATE,
    .channel_count = I2S_NUM_CHANNELS,
};

// Buffer format wraps the audio format with stride info
static audio_buffer_format_t producer_buffer_format = {
    .format = &audio_format,
    .sample_stride = I2S_NUM_CHANNELS * (I2S_BITS_PER_SAMPLE / 8),
};

// The producer pool: BT audio module writes decoded PCM here
static audio_buffer_pool_t *producer_pool = NULL;

bool i2s_output_init(void) {
  printf("[i2s] Initializing I2S output: %u Hz, %d-bit, %d channels\n",
         audio_format.sample_freq, I2S_BITS_PER_SAMPLE, I2S_NUM_CHANNELS);

  // Create the producer buffer pool
  producer_pool = audio_new_producer_pool(
      &producer_buffer_format, I2S_BUFFER_COUNT, I2S_SAMPLES_PER_BUFFER);

  if (!producer_pool) {
    printf("[i2s] ERROR: Failed to create audio producer pool\n");
    return false;
  }

  // Dynamically find free PIO State Machine and DMA channel
  // We claim them and unclaim immediately so audio_i2s_setup can claim them
  // internally
  int free_sm = pio_claim_unused_sm(pio0, true);
  pio_sm_unclaim(pio0, free_sm);

  int free_dma = dma_claim_unused_channel(true);
  dma_channel_unclaim(free_dma);

  // Configure the I2S hardware (PIO + DMA)
  audio_i2s_config_t i2s_config = {
      .data_pin = PICO_AUDIO_I2S_DATA_PIN,
      .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
      .dma_channel = free_dma,
      .pio_sm = free_sm,
  };

  const audio_format_t *output_format =
      audio_i2s_setup(&audio_format, &i2s_config);
  if (!output_format) {
    printf("[i2s] ERROR: audio_i2s_setup failed\n");
    return false;
  }

  // Connect the producer pool to the I2S consumer
  bool ok = audio_i2s_connect(producer_pool);
  if (!ok) {
    printf("[i2s] ERROR: audio_i2s_connect failed\n");
    return false;
  }

  // Enable the I2S output (starts DMA transfers)
  audio_i2s_set_enabled(true);

  printf("[i2s] I2S output initialized successfully\n");
  printf("[i2s]   Data pin:       GPIO %d\n", PICO_AUDIO_I2S_DATA_PIN);
  printf("[i2s]   Clock pin base: GPIO %d (BCLK=%d, WSEL=%d)\n",
         PICO_AUDIO_I2S_CLOCK_PIN_BASE, PICO_AUDIO_I2S_CLOCK_PIN_BASE,
         PICO_AUDIO_I2S_CLOCK_PIN_BASE + 1);

  // Initialize Mute GPIO
  gpio_init(DAC_MUTE_PIN);
  gpio_set_dir(DAC_MUTE_PIN, GPIO_OUT);
  gpio_put(DAC_MUTE_PIN, 0); // Active HIGH (0 = Unmuted, 1 = Muted)

  return true;
}

void i2s_output_set_mute(bool mute) {
  // CJMCU-1334 MUTE is active HIGH
  gpio_put(DAC_MUTE_PIN, mute ? 1 : 0);
  printf("[i2s] DAC Hardware Mute: %s\n", mute ? "ON" : "OFF");
}

void i2s_output_set_sample_rate(uint32_t sample_rate) {
  if (sample_rate != audio_format.sample_freq) {
    printf("[i2s] Updating sample rate: %lu -> %lu Hz\n",
           (unsigned long)audio_format.sample_freq, (unsigned long)sample_rate);
    audio_format.sample_freq = sample_rate;
  }
}

audio_buffer_pool_t *i2s_output_get_producer_pool(void) {
  return producer_pool;
}

#include <math.h>

void play_test_tone(void) {
  printf("[i2s] Playing 440Hz test tone for 1 second...\n");
  float freq = 440.0f;
  float phase = 0.0f;
  float phase_inc = 2.0f * (float)M_PI * freq / audio_format.sample_freq;

  // Play for ~1 second (assuming 44100 Hz, roughly 115 iterations of 384
  // samples)
  int iterations = audio_format.sample_freq / I2S_SAMPLES_PER_BUFFER;
  for (int i = 0; i < iterations; i++) {
    audio_buffer_t *buffer = take_audio_buffer(producer_pool, true);
    if (!buffer)
      continue;

    int16_t *samples = (int16_t *)buffer->buffer->bytes;
    for (int j = 0; j < buffer->max_sample_count; j++) {
      int16_t val = (int16_t)(sinf(phase) *
                              8192.0f); // 25% volume to avoid blowing speakers
      samples[j * 2] = val;             // Left
      samples[j * 2 + 1] = val;         // Right
      phase += phase_inc;
      if (phase >= 2.0f * (float)M_PI)
        phase -= 2.0f * (float)M_PI;
    }
    buffer->sample_count = buffer->max_sample_count;
    give_audio_buffer(producer_pool, buffer);
  }
  printf("[i2s] Test tone complete\n");
}
