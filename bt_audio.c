/*
 * bt_audio.c - Bluetooth A2DP Sink + AVRCP Controller
 *
 * Core Bluetooth audio logic. Receives SBC-encoded audio via A2DP,
 * decodes it to 16-bit stereo PCM, applies AVRCP volume scaling,
 * and pushes the samples into the I2S output buffer pool.
 *
 * Based on BTstack's a2dp_sink_demo.c by BlueKitchen GmbH.
 */

#include "bt_audio.h"
#include "i2s_output.h"
#include "led_status.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "btstack.h"
#include "app_config.h"
#include "btstack_resample.h"
#include "btstack_ring_buffer.h"

// ============================================================================
// Constants
// ============================================================================

#define NUM_CHANNELS 2
#define BYTES_PER_FRAME (2 * NUM_CHANNELS) // 16-bit stereo = 4 bytes
#define MAX_SBC_FRAME_SIZE 120

// Ring buffer sizing for SBC frames
// Targets 60-80 frames in the buffer for smooth playback
#define OPTIMAL_FRAMES_MIN 60
#define OPTIMAL_FRAMES_MAX 80
#define ADDITIONAL_FRAMES 30

// Default Bluetooth device name (can be overridden via CMake)
#ifndef BT_DEVICE_NAME
#define BT_DEVICE_NAME "Pico2W-Audio"
#endif

// ============================================================================
// SBC Codec Capabilities
// ============================================================================

// Advertise maximum quality SBC configuration to the phone:
// - All sampling frequencies supported (16/32/44.1/48 kHz)
// - All channel modes (Mono/Dual/Stereo/Joint Stereo)
// - Bitpool range: 2 to 53 (53 = highest standard Joint Stereo quality, ~328
// kbps)
static uint8_t media_sbc_codec_capabilities[] = {
    0xFF, // Sampling frequencies + Channel modes: all supported
    0xFF, // Block lengths + Subbands + Allocation methods: all supported
    2,    // Min bitpool
    53,   // Max bitpool (A2DP spec recommended max for Joint Stereo)
};

// ============================================================================
// SDP Service Buffers
// ============================================================================

static uint8_t sdp_avdtp_sink_service_buffer[150];
static uint8_t sdp_avrcp_target_service_buffer[150];
static uint8_t sdp_avrcp_controller_service_buffer[200];
static uint8_t device_id_sdp_service_buffer[100];

// ============================================================================
// SBC Decoder State
// ============================================================================

static const btstack_sbc_decoder_t *sbc_decoder_instance;
static btstack_sbc_decoder_bluedroid_t sbc_decoder_context;

// Ring buffer for incoming SBC frames (not yet decoded)
static uint8_t sbc_frame_storage[(OPTIMAL_FRAMES_MAX + ADDITIONAL_FRAMES) *
                                 MAX_SBC_FRAME_SIZE];
static btstack_ring_buffer_t sbc_frame_ring_buffer;
static unsigned int sbc_frame_size;

// Ring buffer for decoded PCM audio (Must hold at least a few I2S buffers)
#define DECODED_AUDIO_FRAMES 4096
static uint8_t decoded_audio_storage[DECODED_AUDIO_FRAMES * BYTES_PER_FRAME];
static btstack_ring_buffer_t decoded_audio_ring_buffer;

// Resampler for rate matching
static btstack_resample_t resample_instance;

// ============================================================================
// Audio State
// ============================================================================

static int media_initialized = 0;
static int audio_stream_started = 0;
static bool audio_playback_active = false;

// Volume: 0-127 (AVRCP absolute volume scale)


// Temp storage for audio playback callback
static int16_t *request_buffer;
static int request_frames;

// ============================================================================
// Connection State
// ============================================================================

typedef struct {
  uint8_t reconfigure;
  uint8_t num_channels;
  uint16_t sampling_frequency;
  uint8_t block_length;
  uint8_t subbands;
  uint8_t min_bitpool_value;
  uint8_t max_bitpool_value;
  btstack_sbc_channel_mode_t channel_mode;
  btstack_sbc_allocation_method_t allocation_method;
} media_codec_configuration_sbc_t;

typedef enum {
  STREAM_STATE_CLOSED,
  STREAM_STATE_OPEN,
  STREAM_STATE_PLAYING,
  STREAM_STATE_PAUSED,
} stream_state_t;

typedef struct {
  uint8_t a2dp_local_seid;
  uint8_t media_sbc_codec_configuration[4];
} a2dp_sink_stream_endpoint_t;
static a2dp_sink_stream_endpoint_t stream_endpoints[MAX_CONNECTIONS];

typedef struct {
  bd_addr_t addr;
  uint16_t a2dp_cid;
  uint8_t a2dp_local_seid;
  stream_state_t stream_state;
  media_codec_configuration_sbc_t sbc_configuration;
} a2dp_connection_t;

static a2dp_connection_t a2dp_connections[MAX_CONNECTIONS];
static uint16_t active_a2dp_cid = 0;

typedef struct {
  bd_addr_t addr;
  uint16_t avrcp_cid;
  bool playing;
  uint16_t notifications_supported_by_target;
  uint8_t volume;
} bt_avrcp_state_t;
static bt_avrcp_state_t avrcp_connections[MAX_CONNECTIONS];

static btstack_packet_callback_registration_t hci_event_callback_registration;

static void media_processing_init(media_codec_configuration_sbc_t *config);
static void media_processing_close(void);

static a2dp_connection_t * get_a2dp_connection(uint16_t cid) {
    for (int i=0; i<MAX_CONNECTIONS; i++) {
        if (a2dp_connections[i].a2dp_cid == cid) return &a2dp_connections[i];
    }
    return NULL;
}

static a2dp_connection_t * get_free_a2dp_connection(void) {
    for (int i=0; i<MAX_CONNECTIONS; i++) {
        if (a2dp_connections[i].a2dp_cid == 0) return &a2dp_connections[i];
    }
    return NULL;
}

static bt_avrcp_state_t * get_avrcp_connection(uint16_t cid) {
    for (int i=0; i<MAX_CONNECTIONS; i++) {
        if (avrcp_connections[i].avrcp_cid == cid) return &avrcp_connections[i];
    }
    return NULL;
}

static bt_avrcp_state_t * get_free_avrcp_connection(void) {
    for (int i=0; i<MAX_CONNECTIONS; i++) {
        if (avrcp_connections[i].avrcp_cid == 0) return &avrcp_connections[i];
    }
    return NULL;
}

static void switch_active_stream(void) {
    for (int i=0; i<MAX_CONNECTIONS; i++) {
        if (a2dp_connections[i].a2dp_cid != 0 && a2dp_connections[i].stream_state == STREAM_STATE_PLAYING) {
            active_a2dp_cid = a2dp_connections[i].a2dp_cid;
            printf("[bt] Switched active stream to CID 0x%04x\n", active_a2dp_cid);
            
            media_processing_close();
            media_processing_init(&a2dp_connections[i].sbc_configuration);
            
            return;
        }
    }
    active_a2dp_cid = 0;
    media_processing_close();
    led_status_set(LED_STATE_CONNECTED);
}

static uint8_t get_active_volume(void) {
    if (active_a2dp_cid == 0) return 127;

    a2dp_connection_t *a2dp = get_a2dp_connection(active_a2dp_cid);
    if (!a2dp) return 127;

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (avrcp_connections[i].avrcp_cid != 0 && 
            bd_addr_cmp(avrcp_connections[i].addr, a2dp->addr) == 0) {
            return avrcp_connections[i].volume > 0 ? avrcp_connections[i].volume : 127;
        }
    }
    return 127;
}

static void update_discoverability(void) {
    int active_connections = 0;
    for (int i=0; i<MAX_CONNECTIONS; i++) {
        if (a2dp_connections[i].a2dp_cid != 0) {
            active_connections++;
        }
    }
    
    if (active_connections >= MAX_CONNECTIONS) {
        printf("[bt] Max connections reached. Turning OFF discoverability.\n");
        gap_discoverable_control(0);
    } else {
        printf("[bt] Slot available. Turning ON discoverability.\n");
        gap_discoverable_control(1);
    }
    
    if (active_connections == 0) {
        led_status_set(LED_STATE_DISCOVERABLE);
    } else if (active_a2dp_cid == 0) {
        led_status_set(LED_STATE_CONNECTED);
    }
}



// ============================================================================
// Volume Scaling
// ============================================================================

// Apply AVRCP absolute volume (0-127) to a 16-bit PCM sample
// Uses a logarithmic (quadratic) curve to match natural human hearing response.
static inline int16_t apply_volume(int16_t sample, uint8_t volume) {
  if (volume == 0)
    return 0;
  if (volume >= 127)
    return sample;

  // x^2 curve for natural audio taper
  int32_t s = (int32_t)sample * volume * volume;
  return (int16_t)(s / (127 * 127));
}

// ============================================================================
// UI Sound Synthesizer (Retro Tones)
// ============================================================================

#define ENABLE_UI_SOUNDS 1
// 16-bit PCM amplitude for the square wave (max is 32767).
// 50 provides a comfortable, quiet chime that won't blast the speakers.
#define UI_SOUND_VOLUME 50

#if ENABLE_UI_SOUNDS

typedef struct {
  uint16_t freq;
  uint16_t duration_ms;
} ui_tone_t;

// Power Up: Ascending Sequence (E4, G4, E5, C5, D5, G5)
static const ui_tone_t sound_powerup[] = {{330, 100}, {392, 100}, {659, 100},
                                          {523, 100}, {587, 100}, {784, 300},
                                          {0, 0}};

// Connect: High Chime (B5 -> E6)
static const ui_tone_t sound_connect[] = {{987, 100}, {1318, 300}, {0, 0}};

// Disconnect: Descending Sequence (E4 -> C4 -> G3)
static const ui_tone_t sound_disconnect[] = {
    {330, 150}, {261, 150}, {196, 300}, {0, 0}};

// Synthesizes a blocking square wave directly into the I2S hardware pool
static void play_ui_sound(const ui_tone_t *sequence) {
  audio_buffer_pool_t *pool = i2s_output_get_producer_pool();
  if (!pool)
    return;

  for (int i = 0; sequence[i].duration_ms > 0; i++) {
    uint32_t freq = sequence[i].freq;
    uint32_t duration_samples =
        (I2S_SAMPLE_RATE * sequence[i].duration_ms) / 1000;
    uint32_t samples_played = 0;
    uint32_t phase = 0;
    uint32_t period = freq > 0 ? (I2S_SAMPLE_RATE / freq) : 1;

    while (samples_played < duration_samples) {
      // Block until an I2S buffer is free to keep perfect hardware sync
      audio_buffer_t *audio_buf = take_audio_buffer(pool, true);
      if (!audio_buf)
        continue;

      int16_t *samples = (int16_t *)audio_buf->buffer->bytes;
      uint32_t to_play = audio_buf->max_sample_count;
      if (samples_played + to_play > duration_samples) {
        to_play = duration_samples - samples_played;
      }

      for (uint32_t j = 0; j < to_play * NUM_CHANNELS; j += NUM_CHANNELS) {
        int16_t val = 0;
        if (freq > 0) {
          val = (phase < period / 2) ? UI_SOUND_VOLUME
                                     : -UI_SOUND_VOLUME; // Square wave
          phase = (phase + 1) % period;
        }
        samples[j] = val;     // Left channel
        samples[j + 1] = val; // Right channel
      }

      // Pad remainder with zeros if it's a partial buffer
      for (uint32_t j = to_play * NUM_CHANNELS;
           j < audio_buf->max_sample_count * NUM_CHANNELS; j++) {
        samples[j] = 0;
      }

      audio_buf->sample_count = audio_buf->max_sample_count;
      give_audio_buffer(pool, audio_buf);
      samples_played += to_play;
    }
  }
}

// Public wrapper for main.c
void bt_audio_play_powerup_sound(void) { play_ui_sound(sound_powerup); }

#else

void bt_audio_play_powerup_sound(void) {}
#define play_ui_sound(seq)                                                     \
  do {                                                                         \
  } while (0)

#endif

// ============================================================================
// Audio Playback (PCM -> I2S)
// ============================================================================

static void playback_handler(int16_t *buffer, uint16_t num_frames) {
  // Store request for later filling
  request_buffer = buffer;
  request_frames = num_frames;

  uint32_t bytes_needed = num_frames * BYTES_PER_FRAME;
  uint32_t bytes_available =
      btstack_ring_buffer_bytes_available(&decoded_audio_ring_buffer);

  if (bytes_available >= bytes_needed) {
    uint32_t bytes_read = 0;
    btstack_ring_buffer_read(&decoded_audio_ring_buffer, (uint8_t *)buffer,
                             bytes_needed, &bytes_read);
  } else {
    // Underflow: fill with silence
    memset(buffer, 0, bytes_needed);
  }

  // Apply volume scaling to the output buffer
  uint8_t vol = get_active_volume();
  for (uint16_t i = 0; i < num_frames * NUM_CHANNELS; i++) {
    buffer[i] = apply_volume(buffer[i], vol);
  }
}

// ============================================================================
// SBC Decoder Callback
// ============================================================================

static void handle_pcm_data(int16_t *data, int num_frames, int num_channels,
                            int sample_rate, void *context) {
  (void)context;
  (void)sample_rate;
  (void)num_channels;

  // Resample the audio to handle clock drift between the phone and the Pico
  // The SBC frame max size is 128 frames. Adding 16 frames headroom for
  // stretching.
  int16_t output_buffer[(128 + 16) * NUM_CHANNELS];
  uint32_t resampled_frames = btstack_resample_block(&resample_instance, data,
                                                     num_frames, output_buffer);

  // Store resampled PCM in the ring buffer
  uint32_t bytes_to_write = resampled_frames * BYTES_PER_FRAME;
  uint32_t bytes_free =
      btstack_ring_buffer_bytes_free(&decoded_audio_ring_buffer);

  if (bytes_to_write <= bytes_free) {
    btstack_ring_buffer_write(&decoded_audio_ring_buffer,
                              (uint8_t *)output_buffer, bytes_to_write);
  }
  // If buffer is full, drop the frames (prevents stalling the decoder)
}

// ============================================================================
// Media Processing
// ============================================================================

static void media_processing_init(media_codec_configuration_sbc_t *config) {
  if (media_initialized)
    return;

  printf("[bt] Media init: %u Hz, %u channels, bitpool %u-%u\n",
         config->sampling_frequency, config->num_channels,
         config->min_bitpool_value, config->max_bitpool_value);

  // Update I2S sample rate if different from default
  i2s_output_set_sample_rate(config->sampling_frequency);

  // Initialize SBC decoder
  sbc_decoder_instance =
      btstack_sbc_decoder_bluedroid_init_instance(&sbc_decoder_context);
  sbc_decoder_instance->configure(&sbc_decoder_context, SBC_MODE_STANDARD,
                                  &handle_pcm_data, NULL);

  // Initialize ring buffers
  btstack_ring_buffer_init(&sbc_frame_ring_buffer, sbc_frame_storage,
                           sizeof(sbc_frame_storage));
  btstack_ring_buffer_init(&decoded_audio_ring_buffer, decoded_audio_storage,
                           sizeof(decoded_audio_storage));

  // Initialize resampler (1:1 ratio initially)
  btstack_resample_init(&resample_instance, config->num_channels);

  media_initialized = 1;
}

static void media_processing_close(void) {
  if (!media_initialized)
    return;

  media_initialized = 0;
  audio_stream_started = 0;
  audio_playback_active = false;

  btstack_ring_buffer_reset(&decoded_audio_ring_buffer);
  btstack_ring_buffer_reset(&sbc_frame_ring_buffer);

  printf("[bt] Media processing closed\n");
}

// ============================================================================
// L2CAP Media Data Handler (SBC frames arrive here)
// ============================================================================

static void handle_l2cap_media_data_packet(uint8_t seid, uint8_t *packet,
                                           uint16_t size) {
  // Find active SEID
  uint8_t active_seid_for_cid = 0;
  for (int i=0; i<MAX_CONNECTIONS; i++) {
      if (a2dp_connections[i].a2dp_cid == active_a2dp_cid) {
          active_seid_for_cid = a2dp_connections[i].a2dp_local_seid;
          break;
      }
  }

  // Drop packet if not from the active audio source
  if (seid != active_seid_for_cid) {
      return;
  }

  // Parse the media packet header
  // First byte is the media packet header (contains fragment/start/last info)
  int pos = 0;

  if (size < 13)
    return;

  // Skip RTP header (12 bytes) + 1 byte SBC media header
  pos = 13;

  // Read the number of SBC frames
  uint8_t num_sbc_frames = packet[12] & 0x0F;

  if (num_sbc_frames == 0)
    return;

  // Calculate individual frame size
  int frame_size = (size - pos) / num_sbc_frames;
  sbc_frame_size = frame_size;

  // Queue each SBC frame in the ring buffer
  for (int i = 0; i < num_sbc_frames; i++) {
    uint32_t bytes_free =
        btstack_ring_buffer_bytes_free(&sbc_frame_ring_buffer);
    if (bytes_free >= (uint32_t)(frame_size + 1)) {
      // Store frame size as a 1-byte prefix, then the frame data
      uint8_t size_byte = (uint8_t)frame_size;
      btstack_ring_buffer_write(&sbc_frame_ring_buffer, &size_byte, 1);
      btstack_ring_buffer_write(&sbc_frame_ring_buffer, packet + pos,
                                frame_size);
    }
    pos += frame_size;
  }

  // Decide on audio sync drift based on number of SBC frames in queue
  int sbc_frames_in_buffer =
      btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer) /
      (sbc_frame_size + 1);

  uint32_t resampling_factor;
  uint32_t nominal_factor = 0x10000;
  uint32_t compensation = 0x00100;

  if (sbc_frames_in_buffer < OPTIMAL_FRAMES_MIN) {
    resampling_factor = nominal_factor - compensation; // stretch samples
  } else if (sbc_frames_in_buffer <= OPTIMAL_FRAMES_MAX) {
    resampling_factor = nominal_factor; // nothing to do
  } else {
    resampling_factor = nominal_factor + compensation; // compress samples
  }
  btstack_resample_set_factor(&resample_instance, resampling_factor);

  // Only start decoding and feeding I2S if we have buffered enough frames
  if (!audio_playback_active) {
    if (sbc_frames_in_buffer >= OPTIMAL_FRAMES_MIN) {
      audio_playback_active = true;
    } else {
      return; // Wait for more frames to buffer
    }
  } else if (sbc_frames_in_buffer < 10) {
    // Severe buffer underrun detected (connection struggling).
    // Pause playback to force a complete re-buffer (up to 60 frames)
    // rather than suffering continuous micro-stutters and pops.
    audio_playback_active = false;
    return;
  }

  // Decode queued SBC frames into PCM
  while (btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer) > 0) {
    // Ensure there is enough space in the decoded ring buffer for a maximum
    // resampled frame (128 samples + 16 for stretching) * 4 bytes per frame =
    // 576 bytes
    if (btstack_ring_buffer_bytes_free(&decoded_audio_ring_buffer) <
        (128 + 16) * BYTES_PER_FRAME) {
      break;
    }

    uint32_t bytes_read = 0;
    uint8_t frame_size_byte;
    btstack_ring_buffer_read(&sbc_frame_ring_buffer, &frame_size_byte, 1,
                             &bytes_read);
    if (bytes_read == 0)
      break;

    uint8_t sbc_frame[MAX_SBC_FRAME_SIZE];
    btstack_ring_buffer_read(&sbc_frame_ring_buffer, sbc_frame, frame_size_byte,
                             &bytes_read);
    if (bytes_read == 0)
      break;

    sbc_decoder_instance->decode_signed_16(&sbc_decoder_context, 0, sbc_frame,
                                           (uint16_t)frame_size_byte);
  }

  // Feed decoded PCM to the I2S output
  audio_buffer_pool_t *pool = i2s_output_get_producer_pool();
  if (!pool)
    return;

  while (btstack_ring_buffer_bytes_available(&decoded_audio_ring_buffer) >=
         I2S_SAMPLES_PER_BUFFER * BYTES_PER_FRAME) {
    audio_buffer_t *audio_buf = take_audio_buffer(pool, false);
    if (!audio_buf)
      break;

    int16_t *samples = (int16_t *)audio_buf->buffer->bytes;
    uint32_t bytes_to_read = audio_buf->max_sample_count * BYTES_PER_FRAME;
    uint32_t bytes_read = 0;

    btstack_ring_buffer_read(&decoded_audio_ring_buffer, (uint8_t *)samples,
                             bytes_to_read, &bytes_read);

    uint32_t samples_read = bytes_read / BYTES_PER_FRAME;

    // Apply volume scaling
    uint8_t active_vol = get_active_volume();
    for (uint32_t i = 0; i < samples_read * NUM_CHANNELS; i++) {
      samples[i] = apply_volume(samples[i], active_vol);
    }

    audio_buf->sample_count = samples_read;
    give_audio_buffer(pool, audio_buf);
  }
}

// ============================================================================
// A2DP Sink Event Handler
// ============================================================================

static void a2dp_sink_packet_handler(uint8_t packet_type, uint16_t channel,
                                     uint8_t *packet, uint16_t size) {
  (void)channel;
  (void)size;

  if (packet_type != HCI_EVENT_PACKET)
    return;
  if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META)
    return;

  uint8_t subevent = hci_event_a2dp_meta_get_subevent_code(packet);

  switch (subevent) {
  case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED: {
    uint8_t status =
        a2dp_subevent_signaling_connection_established_get_status(packet);
    uint16_t cid =
        a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);

    if (status != ERROR_CODE_SUCCESS) {
      printf("[bt] A2DP connection FAILED: status 0x%02x\n", status);
      return;
    }

    a2dp_connection_t * conn = get_free_a2dp_connection();
    if (!conn) {
      printf("[bt] No free A2DP connection slots\n");
      // Could disconnect here, but let BTstack handle rejection
      return;
    }

    conn->a2dp_cid = cid;
    a2dp_subevent_signaling_connection_established_get_bd_addr(
        packet, conn->addr);

    printf("[bt] A2DP connected: cid 0x%04x, addr %s\n", cid,
           bd_addr_to_str(conn->addr));

    update_discoverability();
    play_ui_sound(sound_connect);
    break;
  }

  case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
    uint16_t cid = a2dp_subevent_signaling_media_codec_sbc_configuration_get_a2dp_cid(packet);
    a2dp_connection_t * conn = get_a2dp_connection(cid);
    if (!conn) break;

    media_codec_configuration_sbc_t *config = &conn->sbc_configuration;

    config->reconfigure =
        a2dp_subevent_signaling_media_codec_sbc_configuration_get_reconfigure(
            packet);
    config->num_channels =
        a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(
            packet);
    config->sampling_frequency =
        a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(
            packet);
    config->block_length =
        a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(
            packet);
    config->subbands =
        a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(
            packet);
    config->min_bitpool_value =
        a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(
            packet);
    config->max_bitpool_value =
        a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(
            packet);
    config->channel_mode = (btstack_sbc_channel_mode_t)
        a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(
            packet);
    config->allocation_method = (btstack_sbc_allocation_method_t)
        a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(
            packet);

    printf("[bt] SBC config: %u Hz, %u ch, bitpool %u-%u\n",
           config->sampling_frequency, config->num_channels,
           config->min_bitpool_value, config->max_bitpool_value);
    break;
  }

  case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
    uint8_t status = a2dp_subevent_stream_established_get_status(packet);
    if (status != ERROR_CODE_SUCCESS) {
      printf("[bt] Stream setup FAILED: status 0x%02x\n", status);
      return;
    }

    uint16_t cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);
    a2dp_connection_t * conn = get_a2dp_connection(cid);
    if (!conn) return;

    conn->stream_state = STREAM_STATE_OPEN;
    conn->a2dp_local_seid =
        a2dp_subevent_stream_established_get_local_seid(packet);

    printf("[bt] A2DP stream established, seid %u\n",
           conn->a2dp_local_seid);
    break;
  }

  case A2DP_SUBEVENT_STREAM_STARTED: {
    uint16_t cid = a2dp_subevent_stream_started_get_a2dp_cid(packet);
    a2dp_connection_t * conn = get_a2dp_connection(cid);
    if (!conn) break;

    printf("[bt] Stream STARTED for cid 0x%04x\n", cid);
    conn->stream_state = STREAM_STATE_PLAYING;
    
    if (active_a2dp_cid == 0) {
        active_a2dp_cid = cid;
        media_processing_close();
        media_processing_init(&conn->sbc_configuration);
        audio_stream_started = 1;
        led_status_set(LED_STATE_STREAMING);
    }
    break;
  }

  case A2DP_SUBEVENT_STREAM_SUSPENDED: {
    uint16_t cid = a2dp_subevent_stream_suspended_get_a2dp_cid(packet);
    a2dp_connection_t * conn = get_a2dp_connection(cid);
    if (!conn) break;

    printf("[bt] Stream PAUSED for cid 0x%04x\n", cid);
    conn->stream_state = STREAM_STATE_PAUSED;

    if (active_a2dp_cid == cid) {
        switch_active_stream();
    }
    break;
  }

  case A2DP_SUBEVENT_STREAM_STOPPED: {
    uint16_t cid = a2dp_subevent_stream_stopped_get_a2dp_cid(packet);
    a2dp_connection_t * conn = get_a2dp_connection(cid);
    if (!conn) break;

    printf("[bt] Stream STOPPED for cid 0x%04x\n", cid);
    conn->stream_state = STREAM_STATE_PAUSED;
    
    if (active_a2dp_cid == cid) {
        switch_active_stream();
    }
    break;
  }

  case A2DP_SUBEVENT_STREAM_RELEASED: {
    uint16_t cid = a2dp_subevent_stream_released_get_a2dp_cid(packet);
    a2dp_connection_t * conn = get_a2dp_connection(cid);
    if (!conn) break;

    printf("[bt] Stream RELEASED for cid 0x%04x\n", cid);
    conn->stream_state = STREAM_STATE_PAUSED;

    if (active_a2dp_cid == cid) {
        switch_active_stream();
    }
    break;
  }

  case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED: {
    uint16_t cid = a2dp_subevent_signaling_connection_released_get_a2dp_cid(packet);
    a2dp_connection_t * conn = get_a2dp_connection(cid);
    if (!conn) break;

    printf("[bt] A2DP disconnected for cid 0x%04x\n", cid);
    
    conn->a2dp_cid = 0;
    conn->stream_state = STREAM_STATE_CLOSED;
    
    if (active_a2dp_cid == cid) {
        switch_active_stream();
    }
    
    update_discoverability();
    
    int active_connections = 0;
    for (int i=0; i<MAX_CONNECTIONS; i++) {
        if (a2dp_connections[i].a2dp_cid != 0) active_connections++;
    }
    if (active_connections == 0) {
        play_ui_sound(sound_disconnect);
    }
    break;
  }

  default:
    break;
  }
}

// ============================================================================
// AVRCP Event Handlers
// ============================================================================

static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size) {
  (void)channel;
  (void)size;

  if (packet_type != HCI_EVENT_PACKET)
    return;
  if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META)
    return;

  uint8_t subevent = hci_event_avrcp_meta_get_subevent_code(packet);

  switch (subevent) {
  case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED: {
    uint16_t cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
    bt_avrcp_state_t * conn = get_free_avrcp_connection();
    if (!conn) return;

    conn->avrcp_cid = cid;
    conn->volume = 100; // Default volume per connection
    avrcp_subevent_connection_established_get_bd_addr(packet, conn->addr);

    uint8_t status = avrcp_subevent_connection_established_get_status(packet);
    if (status != ERROR_CODE_SUCCESS) {
      printf("[avrcp] Connection FAILED: 0x%02x\n", status);
      return;
    }

    printf("[avrcp] Connected: cid 0x%04x\n", cid);

    // Register for volume change notifications from the phone
    avrcp_controller_enable_notification(
        cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
    avrcp_controller_enable_notification(
        cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
    break;
  }

  case AVRCP_SUBEVENT_CONNECTION_RELEASED: {
    uint16_t cid = avrcp_subevent_connection_released_get_avrcp_cid(packet);
    bt_avrcp_state_t * conn = get_avrcp_connection(cid);
    if (conn) {
        conn->avrcp_cid = 0;
    }
    printf("[avrcp] Disconnected\n");
    break;
  }

  default:
    break;
  }
}

static void avrcp_controller_packet_handler(uint8_t packet_type,
                                            uint16_t channel, uint8_t *packet,
                                            uint16_t size) {
  (void)channel;
  (void)size;

  if (packet_type != HCI_EVENT_PACKET)
    return;
  if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META)
    return;

  uint8_t subevent = hci_event_avrcp_meta_get_subevent_code(packet);

  switch (subevent) {
  case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED: {
    uint8_t vol =
        avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);

    printf("[avrcp] Volume changed: %u/127 (%u%%)\n", vol, (vol * 100) / 127);
    break;
  }

  case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED: {
    uint8_t status =
        avrcp_subevent_notification_playback_status_changed_get_play_status(
            packet);
    uint16_t cid = avrcp_subevent_notification_playback_status_changed_get_avrcp_cid(packet);
    bt_avrcp_state_t * conn = get_avrcp_connection(cid);
    if (conn) {
        conn->playing = (status == AVRCP_PLAYBACK_STATUS_PLAYING);
    }
    break;
  }

  default:
    break;
  }
}

static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel,
                                        uint8_t *packet, uint16_t size) {
  (void)channel;
  (void)size;

  if (packet_type != HCI_EVENT_PACKET)
    return;
  if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META)
    return;

  uint8_t subevent = hci_event_avrcp_meta_get_subevent_code(packet);

  switch (subevent) {
  case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED: {
    uint8_t vol =
        avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);
    uint16_t cid = avrcp_subevent_notification_volume_changed_get_avrcp_cid(packet);
    bt_avrcp_state_t *conn = get_avrcp_connection(cid);
    
    if (conn) {
        conn->volume = vol;
    }

    printf("[avrcp-target] Volume set to: %u/127 for cid 0x%04x\n", vol, cid);
    break;
  }

  default:
    break;
  }
}

// ============================================================================
// HCI Event Handler (pairing, discoverability)
// ============================================================================

static void hci_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size) {
  (void)channel;
  (void)size;

  if (packet_type != HCI_EVENT_PACKET)
    return;

  uint8_t event_type = hci_event_packet_get_type(packet);

  switch (event_type) {
  case BTSTACK_EVENT_STATE:
    if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
      bd_addr_t local_addr;
      gap_local_bd_addr(local_addr);
      printf("[bt] BTstack up and running on %s\n", bd_addr_to_str(local_addr));
      printf("[bt] Device name: %s\n", BT_DEVICE_NAME);

      // Make discoverable and connectable
      gap_discoverable_control(1);
      gap_set_class_of_device(0x200414); // Audio: Loudspeaker
      gap_set_local_name(BT_DEVICE_NAME);

      led_status_set(LED_STATE_DISCOVERABLE);
    }
    break;

  case HCI_EVENT_PIN_CODE_REQUEST:
    printf("[bt] PIN code request - using '0000'\n");
    {
      bd_addr_t addr;
      hci_event_pin_code_request_get_bd_addr(packet, addr);
      gap_pin_code_response(addr, "0000");
    }
    break;

  case HCI_EVENT_USER_CONFIRMATION_REQUEST:
    printf("[bt] SSP user confirmation request - auto-accepting\n");
    {
      bd_addr_t addr;
      hci_event_user_confirmation_request_get_bd_addr(packet, addr);
      gap_ssp_confirmation_response(addr);
    }
    break;

  default:
    break;
  }
}

// ============================================================================
// Initialization
// ============================================================================

bool bt_audio_init(void) {
  printf("[bt] Initializing Bluetooth Audio\n");

  // Init BTstack protocols
  l2cap_init();
  sdp_init();

  // Init A2DP Sink
  a2dp_sink_init();

  // Register packet handlers
  a2dp_sink_register_packet_handler(&a2dp_sink_packet_handler);
  a2dp_sink_register_media_handler(&handle_l2cap_media_data_packet);

  // Create multiple A2DP stream endpoints with SBC codec
  for (int i=0; i<MAX_CONNECTIONS; i++) {
      avdtp_stream_endpoint_t *local_stream_endpoint =
          a2dp_sink_create_stream_endpoint(
              AVDTP_AUDIO, AVDTP_CODEC_SBC, media_sbc_codec_capabilities,
              sizeof(media_sbc_codec_capabilities),
              stream_endpoints[i].media_sbc_codec_configuration,
              sizeof(stream_endpoints[i].media_sbc_codec_configuration));

      if (!local_stream_endpoint) {
        printf("[bt] ERROR: Failed to create A2DP stream endpoint %d\n", i);
        return false;
      }

      stream_endpoints[i].a2dp_local_seid = avdtp_local_seid(local_stream_endpoint);
      printf("[bt] A2DP Sink SEID created: %u\n", stream_endpoints[i].a2dp_local_seid);
  }

  // Init AVRCP
  avrcp_init();
  avrcp_controller_init();
  avrcp_target_init();

  avrcp_register_packet_handler(&avrcp_packet_handler);
  avrcp_controller_register_packet_handler(&avrcp_controller_packet_handler);
  avrcp_target_register_packet_handler(&avrcp_target_packet_handler);

  // Register SDP services
  // A2DP Sink
  memset(sdp_avdtp_sink_service_buffer, 0,
         sizeof(sdp_avdtp_sink_service_buffer));
  a2dp_sink_create_sdp_record(sdp_avdtp_sink_service_buffer,
                              sdp_create_service_record_handle(),
                              AVDTP_SINK_FEATURE_MASK_HEADPHONE, NULL, NULL);
  sdp_register_service(sdp_avdtp_sink_service_buffer);

  // AVRCP Controller
  memset(sdp_avrcp_controller_service_buffer, 0,
         sizeof(sdp_avrcp_controller_service_buffer));
  uint16_t controller_features =
      (1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_CATEGORY_PLAYER_OR_RECORDER);
  avrcp_controller_create_sdp_record(sdp_avrcp_controller_service_buffer,
                                     sdp_create_service_record_handle(),
                                     controller_features, NULL, NULL);
  sdp_register_service(sdp_avrcp_controller_service_buffer);

  // AVRCP Target
  memset(sdp_avrcp_target_service_buffer, 0,
         sizeof(sdp_avrcp_target_service_buffer));
  uint16_t target_features =
      (1 << AVRCP_TARGET_SUPPORTED_FEATURE_CATEGORY_MONITOR_OR_AMPLIFIER);
  avrcp_target_create_sdp_record(sdp_avrcp_target_service_buffer,
                                 sdp_create_service_record_handle(),
                                 target_features, NULL, NULL);
  sdp_register_service(sdp_avrcp_target_service_buffer);

  // Device ID
  memset(device_id_sdp_service_buffer, 0, sizeof(device_id_sdp_service_buffer));
  device_id_create_sdp_record(device_id_sdp_service_buffer,
                              sdp_create_service_record_handle(),
                              DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH,
                              0x0001, // Vendor ID
                              0x0001, // Product ID
                              0x0001  // Version
  );
  sdp_register_service(device_id_sdp_service_buffer);

  // Register HCI event handler
  hci_event_callback_registration.callback = &hci_packet_handler;
  hci_add_event_handler(&hci_event_callback_registration);

  // Set SSP (Secure Simple Pairing) IO capability
  gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);

  // Enable SSP auto-accept
  gap_ssp_set_auto_accept(1);

  printf("[bt] Bluetooth Audio initialized\n");
  return true;
}
