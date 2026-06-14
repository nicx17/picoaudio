/*
 * btstack_config.h - BTstack configuration for Bluetooth Audio Receiver
 *
 * Enables Bluetooth Classic with A2DP Sink and AVRCP Controller profiles.
 * Configured for the CYW43439 radio on the Raspberry Pi Pico 2 W.
 *
 * NOTE: ENABLE_CLASSIC is defined automatically by the pico_btstack_classic
 * CMake target. Do NOT redefine it here or you will get redefinition warnings.
 */

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// BTstack features
#define ENABLE_LOG_INFO
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

// L2CAP
#define ENABLE_L2CAP
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE

// A2DP Sink
#define ENABLE_A2DP_SINK

// AVRCP (for volume control and playback commands)
#define ENABLE_AVRCP
#define ENABLE_AVRCP_CONTROLLER
#define ENABLE_AVRCP_TARGET

// SBC Codec (required for A2DP audio decoding)
#define ENABLE_SBC_DECODER

// CYW43 HCI transport requirements
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_INCOMING_PRE_BUFFER_SIZE 4

// Memory configuration
#define MAX_NR_BTSTACK_LINK_KEYS 4
#define NVM_NUM_LINK_KEYS 4
#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_L2CAP_CHANNELS 10
#define MAX_NR_L2CAP_SERVICES 4
#define MAX_NR_AVDTP_CONNECTIONS 2
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 2
#define MAX_NR_AVRCP_CONNECTIONS 2

// HCI ACL buffer configuration
// Sized for A2DP audio streaming (SBC frames arrive in L2CAP packets)
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

// BTstack memory pools
#define MAX_NR_WHITELIST_ENTRIES 1
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_SERVICE_RECORD_ITEMS 4

// Security
#define ENABLE_SOFTWARE_AES128

#endif // BTSTACK_CONFIG_H
