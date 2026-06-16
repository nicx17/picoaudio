#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/**
 * ==========================================
 * BLUETOOTH RECEIVER CONFIGURATION
 * ==========================================
 *
 * Edit this file to easily customize your receiver!
 */

// The Bluetooth name of your device (visible to phones during pairing)
#define BT_DEVICE_NAME "PCRXT"

// Maximum number of simultaneous Bluetooth connections (1 or 2).
// Set to 1 if you only want a single device connected at a time.
// Set to 2 to enable Multipoint Bluetooth features.
#define MAX_CONNECTIONS 2

// UI Sound Synthesizer Enable
// Set to 1 to enable the synthesizer, 0 to disable.
#ifndef ENABLE_UI_SOUNDS
#define ENABLE_UI_SOUNDS 1
#endif

// UI Sound Volume (16-bit PCM amplitude)
// Max is 32767. 50 provides a comfortable quiet chime.
#ifndef UI_SOUND_VOLUME
#define UI_SOUND_VOLUME 300
#endif

#endif // APP_CONFIG_H
