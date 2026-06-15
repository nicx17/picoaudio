/*
 * bt_audio.h - Bluetooth A2DP Sink + AVRCP Controller
 *
 * Handles all Bluetooth audio functionality:
 *   - A2DP Sink: receives SBC-encoded audio from a phone
 *   - SBC Decoder: decodes to 16-bit PCM
 *   - AVRCP Controller: volume control and playback commands
 *   - Volume scaling: applies AVRCP absolute volume to PCM samples
 *   - Reconnection: watchdog reboot on disconnect
 */

#ifndef BT_AUDIO_H
#define BT_AUDIO_H

#include <stdbool.h>

/*
 * Initialize the Bluetooth audio subsystem.
 * Returns true on success.
 */
bool bt_audio_init(void);

/*
 * Play the powerup sound (blocking)
 */
void bt_audio_play_powerup_sound(void);

// Entry point for Core 1 (Dual-Core Audio Decoding)
void core1_audio_decoder(void);

// Button Command Handlers
void bt_audio_cmd_play_pause(void);
void bt_audio_cmd_mute(void);
void bt_audio_cmd_vol_up(void);
void bt_audio_cmd_vol_down(void);
void bt_audio_cmd_next(void);
void bt_audio_cmd_prev(void);
void bt_audio_cmd_pairing(void);

#endif // BT_AUDIO_H
