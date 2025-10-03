/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "sdkconfig.h"
#include "usb/usb_types_ch9.h"

#ifdef __cplusplus
extern "C" {
#endif

/********************************* Refer audio10.pdf ***************************************************/

/**
 * @brief Audio Interface Subclass Codes
 *
 * @see Table A-2 of audio10.pdf
 */
typedef enum {
    UAC_SUBCLASS_UNDEFINED                            = 0x00,
    UAC_SUBCLASS_AUDIOCONTROL                         = 0x01,
    UAC_SUBCLASS_AUDIOSTREAMING                       = 0x02,
    UAC_SUBCLASS_MIDISTREAMING                        = 0x03,
} uac_subclass_t;

/**
 * @brief Audio Interface Protocol Codes
 *
 * @see Table A-3 of audio10.pdf
 */
typedef enum {
    UAC_PROTOCOL_UNDEFINED                            = 0x00,
    UAC_PROTOCOL_v20                                  = 0x20,     // refer UAC v2.0
} uac_protocol_t;

/**
 * @brief Audio Class-Specific Descriptor Types
 *
 * @see Table A-4 of audio10.pdf
 */
typedef enum {
    UAC_CS_UNDEFINED                                  = 0x20,
    UAC_CS_DEVICE                                     = 0x21,
    UAC_CS_CONFIGURATION                              = 0x22,
    UAC_CS_STRING                                     = 0x23,
    UAC_CS_INTERFACE                                  = 0x24,
    UAC_CS_ENDPOINT                                   = 0x25
} uac_cs_descriptor_type_t;

/**
 * @brief Audio Class-Specific AC Interface Descriptor Subtypes
 *
 * @see Table A-5 of audio10.pdf
 */
typedef enum {
    UAC_AC_DESCRIPTOR_UNDEFINED                       = 0x00,
    UAC_AC_HEADER                                     = 0x01,
    UAC_AC_INPUT_TERMINAL                             = 0x02,
    UAC_AC_OUTPUT_TERMINAL                            = 0x03,
    UAC_AC_MIXER_UNIT                                 = 0x04,
    UAC_AC_SELECTOR_UNIT                              = 0x05,
    UAC_AC_FEATURE_UNIT                               = 0x06,
    UAC_AC_PROCESSING_UNIT                            = 0x07,
    UAC_AC_EXTENSION_UNIT                             = 0x08
} uac_ac_descriptor_subtype_t;

/**
 * @brief Audio Class-Specific AS Interface Descriptor Subtypes
 *
 * @see Table A-6 of audio10.pdf
 */
typedef enum {
    UAC_AS_DESCRIPTOR_UNDEFINED                       = 0x00,
    UAC_AS_GENERAL                                    = 0x01,
    UAC_AS_FORMAT_TYPE                                = 0x02,
    UAC_AS_FORMAT_SPECIFIC                            = 0x03
} uac_as_descriptor_subtype_t;

/**
 * @brief Processing Unit Process Types
 *
 * @see Table A-7 of audio10.pdf
 */
typedef enum {
    UAC_PROCESS_UNDEFINED                             = 0x00,
    UAC_UP_DOWNMIX_PROCESS                            = 0x01,
    UAC_DOLBY_PROLOGIC_PROCESS                        = 0x02,
    UAC_3D_STEREO_EXTENDER_PROCESS                    = 0x03,
    UAC_REVERBERATION_PROCESS                         = 0x04,
    UAC_CHORUS_PROCESS                                = 0x05,
    UAC_DYN_RANGE_COMP_PROCESS                        = 0x06
} uac_process_type_t;

/**
 * @brief Audio Class-Specific Endpoint Descriptor Subtypes
 *
 * @see Table A-8 of audio10.pdf
 */
typedef enum {
    UAC_EP_DESCRIPTOR_UNDEFINED                       = 0x00,
    UAC_EP_GENERAL                                    = 0x01
} uac_ep_descriptor_subtype_t;

/**
 * @brief Audio Class-Specific Request Codes
 *
 * @see Table A-9 of audio10.pdf
 */
typedef enum {
    UAC_REQUEST_UNDEFINED                             = 0x00,
    UAC_SET_CUR                                       = 0x01,
    UAC_GET_CUR                                       = 0x81,
    UAC_SET_MIN                                       = 0x02,
    UAC_GET_MIN                                       = 0x82,
    UAC_SET_MAX                                       = 0x03,
    UAC_GET_MAX                                       = 0x83,
    UAC_SET_RES                                       = 0x04,
    UAC_GET_RES                                       = 0x84,
    UAC_SET_MEM                                       = 0x05,
    UAC_GET_MEM                                       = 0x85,
    UAC_GET_STAT                                      = 0xFF
} uac_request_code_t;

/********************************* A.10 Control Selector Codes ****************************/

/**
 * @brief Terminal Control Selectors
 *
 * @see Table A-10 of audio10.pdf
 */
typedef enum {
    UAC_TE_CONTROL_UNDEFINED                          = 0x00,
    UAC_COPY_PROTECT_CONTROL                          = 0x01
} uac_te_control_selector_t;

/**
 * @brief Feature Unit Control Selectors
 *
 * @see Table A-11 of audio10.pdf
 */
typedef enum {
    UAC_FU_CONTROL_UNDEFINED                          = 0x00,
    UAC_MUTE_CONTROL                                  = 0x01,
    UAC_VOLUME_CONTROL                                = 0x02,
    UAC_BASS_CONTROL                                  = 0x03,
    UAC_MID_CONTROL                                   = 0x04,
    UAC_TREBLE_CONTROL                                = 0x05,
    UAC_GRAPHIC_EQUALIZER_CONTROL                     = 0x06,
    UAC_AUTOMATIC_GAIN_CONTROL                        = 0x07,
    UAC_DELAY_CONTROL                                 = 0x08,
    UAC_BASS_BOOST_CONTROL                            = 0x09,
    UAC_LOUDNESS_CONTROL                              = 0x0A
} uac_fu_control_selector_t;

/******************************** A.10.3 Processing Unit Control Selectors *********************/

/**
 * @brief UP/DOWNMIX Processing Unit Control Selectors
 *
 * @see Table A-12 of audio10.pdf
 */
typedef enum {
    UAC_UD_CONTROL_UNDEFINED                          = 0x00,
    UAC_UD_ENABLE_CONTROL                             = 0x01,
    UAC_UD_MODE_SELECT_CONTROL                        = 0x02
} uac_ud_control_selector_t;

/**
 * @brief Dolby Prologic Processing Unit Control Selectors
 *
 * @see Table A-13 of audio10.pdf
 */
typedef enum {
    UAC_DP_CONTROL_UNDEFINED                          = 0x00,
    UAC_DP_ENABLE_CONTROL                             = 0x01,
    UAC_DP_MODE_SELECT_CONTROL                        = 0x02
} uac_dp_control_selector_t;

/**
 * @brief 3D Stereo Extender Processing Unit Control Selectors
 *
 * @see Table A-14 of audio10.pdf
 */
typedef enum {
    UAC_3DSE_CONTROL_UNDEFINED                        = 0x00,
    UAC_3DSE_ENABLE_CONTROL                           = 0x01,
    UAC_3DSE_SPACIOUSNESS_CONTROL                     = 0x03
} uac_3dse_control_selector_t;

/**
 * @brief Reverberation Processing Unit Control Selectors
 *
 * @see Table A-15 of audio10.pdf
 */
typedef enum {
    UAC_RV_CONTROL_UNDEFINED                          = 0x00,
    UAC_RV_ENABLE_CONTROL                             = 0x01,
    UAC_REVERB_LEVEL_CONTROL                          = 0x02,
    UAC_REVERB_TIME_CONTROL                           = 0x03,
    UAC_REVERB_FEEDBACK_CONTROL                       = 0x04
} uac_rv_control_selector_t;

/**
 * @brief Chorus Processing Unit Control Selectors
 *
 * @see Table A-16 of audio10.pdf
 */
typedef enum {
    UAC_CH_CONTROL_UNDEFINED                          = 0x00,
    UAC_CH_ENABLE_CONTROL                             = 0x01,
    UAC_CHORUS_LEVEL_CONTROL                          = 0x02,
    UAC_CHORUS_RATE_CONTROL                           = 0x03,
    UAC_CHORUS_DEPTH_CONTROL                          = 0x04
} uac_ch_control_selector_t;

/**
 * @brief Dynamic Range Compressor Processing Unit Control Selectors
 *
 * @see Table A-17 of audio10.pdf
 */
typedef enum {
    UAC_DR_CONTROL_UNDEFINED                          = 0x00,
    UAC_DR_ENABLE_CONTROL                             = 0x01,
    UAC_COMPRESSION_RATE_CONTROL                      = 0x02,
    UAC_MAXAMPL_CONTROL                               = 0x03,
    UAC_THRESHOLD_CONTROL                             = 0x04,
    UAC_ATTACK_TIME                                   = 0x05,
    UAC_RELEASE_TIME                                  = 0x06
} uac_dr_control_selector_t;

/******************************** A.10.4 Extension Unit Control Selectors *********************/

/**
 * @brief Extension Unit Control Selectors
 *
 * @see Table A-18 of audio10.pdf
 *
 */
typedef enum {
    UAC_XU_CONTROL_UNDEFINED                          = 0x00,
    UAC_XU_ENABLE_CONTROL                             = 0x01
} uac_xu_control_selector_t;

/******************************** A.10.5 Endpoint Control Selectors ****************************/

/**
 * @brief Endpoint Control Selectors
 *
 * @see Table A-19 of audio10.pdf
 */
typedef enum {
    UAC_EP_CONTROL_UNDEFINED                          = 0x00,
    UAC_SAMPLING_FREQ_CONTROL                         = 0x01,
    UAC_PITCH_CONTROL                                 = 0x02
} uac_ep_control_selector_t;

/**
 * @brief Feature Unit Control Position
 *
 * @see Table 4-7 of audio10.pdf
 */
typedef enum {
    UAC_FU_CONTROL_POS_MUTE                           = 0x0001,
    UAC_FU_CONTROL_POS_VOLUME                         = 0x0002,
    UAC_FU_CONTROL_POS_BASS                           = 0x0004,
    UAC_FU_CONTROL_POS_MID                            = 0x0008,
    UAC_FU_CONTROL_POS_TREBLE                         = 0x0010,
    UAC_FU_CONTROL_POS_GRAPHIC_EQUALIZER              = 0x0020,
    UAC_FU_CONTROL_POS_AUTOMATIC_GAIN                 = 0x0040,
    UAC_FU_CONTROL_POS_DELAY                          = 0x0080,
    UAC_FU_CONTROL_POS_BASS_BOOST                     = 0x0100,
    UAC_FU_CONTROL_POS_LOUDNESS                       = 0x0200,
} uac_fu_control_pos_t;

/******************************** Refer termt10.pdf ***************************************************/

/**
* @brief USB Terminal Types
*
* @see Table 2-1 of termt10.pdf
*/
typedef enum {
    UAC_USB_TERMINAL_TYPE_USB_UNDEFINED               = 0x0100,
    UAC_USB_TERMINAL_TYPE_USB_STREAMING               = 0x0101,
    UAC_USB_TERMINAL_TYPE_VENDOR_SPECIFIC             = 0x01FF
} uac_usb_terminal_type_t;

/**
* @brief Input Terminal Types
*
* @see Table 2-2 of termt10.pdf
*/
typedef enum {
    UAC_INPUT_TERMINAL_UNDEFINED                      = 0x0200,
    UAC_INPUT_TERMINAL_MICROPHONE                     = 0x0201,
    UAC_INPUT_TERMINAL_DESKTOP_MICROPHONE             = 0x0202,
    UAC_INPUT_TERMINAL_PERSONAL_MICROPHONE            = 0x0203,
    UAC_INPUT_TERMINAL_OMNI_DIRECTIONAL_MICROPHONE    = 0x0204,
    UAC_INPUT_TERMINAL_MICROPHONE_ARRAY               = 0x0205,
    UAC_INPUT_TERMINAL_PROCESSING_MICROPHONE_ARRAY    = 0x0206
} uac_input_terminal_type_t;

/**
 * @brief Output Terminal Types
 *
 * @see Table 2-3 of termt10.pdf
 */
typedef enum {
    UAC_OUTPUT_TERMINAL_UNDEFINED                     = 0x0300,
    UAC_OUTPUT_TERMINAL_SPEAKER                       = 0x0301,
    UAC_OUTPUT_TERMINAL_HEADPHONES                    = 0x0302,
    UAC_OUTPUT_TERMINAL_HEAD_MOUNTED_DISPLAY_AUDIO    = 0x0303,
    UAC_OUTPUT_TERMINAL_DESKTOP_SPEAKER               = 0x0304,
    UAC_OUTPUT_TERMINAL_ROOM_SPEAKER                  = 0x0305,
    UAC_OUTPUT_TERMINAL_COMMUNICATION_SPEAKER         = 0x0306,
    UAC_OUTPUT_TERMINAL_LOW_FREQUENCY_EFFECTS_SPEAKER = 0x0307
} uac_output_terminal_type_t;

/******************************** Refer frmts10.pdf ***************************************************/

/**
 * @brief Audio Data Format Type I Codes
 *
 * @see Table A-1 of frmts10.pdf
 */
typedef enum {
    UAC_TYPE_I_UNDEFINED                              = 0x0000,
    UAC_TYPE_I_PCM                                    = 0x0001,
    UAC_TYPE_I_PCM8                                   = 0x0002,
    UAC_TYPE_I_IEEE_FLOAT                             = 0x0003,
    UAC_TYPE_I_ALAW                                   = 0x0004,
    UAC_TYPE_I_MULAW                                  = 0x0005
} uac_type_i_format_t;

/**
 * @brief Format Type Codes
 *
 *  @see Table A-4 of frmts10.pdf
 */
typedef enum {
    UAC_FORMAT_TYPE_UNDEFINED                         = 0x00,
    UAC_FORMAT_TYPE_I                                 = 0x01,
    UAC_FORMAT_TYPE_II                                = 0x02,
    UAC_FORMAT_TYPE_III                               = 0x03
} uac_format_type_t;

/**
 * @brief Audio Class-Specific Interface Descriptor Common Header
 *
 */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
} __attribute__((packed)) uac_desc_header_t;

/**
 * @brief Audio Class-Specific AC Interface Header Descriptor (bInCollection=2)
 *
 * @see Table 4-2 of audio10.pdf
 */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint16_t bcdADC;
    uint16_t wTotalLength;
    uint8_t bInCollection;
    uint8_t baInterfaceNr[2];
} __attribute__((packed)) uac_ac_header_desc_t;

/**
 * @brief Audio Class-Specific AC Input Terminal Descriptor
 *
 * @see Table 4-3 of audio10.pdf
 */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bTerminalID;
    uint16_t wTerminalType;
    uint8_t bAssocTerminal;
    uint8_t bNrChannels;
    uint16_t wChannelConfig;
    uint8_t iChannelNames;
    uint8_t iTerminal;
} __attribute__((packed)) uac_ac_input_terminal_desc_t;

/**
 * @brief Audio Class-Specific AC Output Terminal Descriptor
 *
 * @see Table 4-4 of audio10.pdf
 */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bTerminalID;
    uint16_t wTerminalType;
    uint8_t bAssocTerminal;
    uint8_t bSourceID;
    uint8_t iTerminal;
} __attribute__((packed)) uac_ac_output_terminal_desc_t;

/**
 * @brief Audio Class-Specific AC Mixer Unit Descriptor
 *
 * @see Table 4-5 of audio10.pdf
 */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bUnitID;
    uint8_t bNrInPins;
    uint8_t baSourceID[2];
    uint8_t bNrChannels;
    uint16_t wChannelConfig;
    uint8_t iChannelNames;
    uint8_t bmControls;
    uint8_t iMixer;
} __attribute__((packed)) uac_ac_mixer_unit_desc_t;

/**
 * @brief Audio Class-Specific AC Selector Unit Descriptor
 *
 * @see Table 4-6 of audio10.pdf
 */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bUnitID;
    uint8_t bNrInPins;
    uint8_t baSourceID[2];
    uint8_t iSelector;
} __attribute__((packed)) uac_ac_selector_unit_desc_t;

/**
 * @brief Audio Class-Specific AC Feature Unit Descriptor (ch=2, bControlSize=2)
 *
 * @see Table 4-7 of audio10.pdf
 */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bUnitID;
    uint8_t bSourceID;
    uint8_t bControlSize;
    uint8_t bmaControls[3 * 2]; // 2 channels + channel 0, 2 bytes each
    uint8_t iFeature;
} __attribute__((packed)) uac_ac_feature_unit_desc_t;

/**
 * @brief Audio Class-Specific AS General Descriptor
 *
 * @see Table 4-19 of audio10.pdf
 */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bTerminalLink;
    uint8_t bDelay;
    uint16_t wFormatTag;
} __attribute__((packed)) uac_as_general_desc_t;

#define UAC_FREQ_NUM_MAX           CONFIG_UAC_FREQ_NUM_MAX
/**
 * @brief Audio Class-Specific AS Type I Format Type Descriptor
 *
 * @see Table 2-1 of frmts10.pdf
 */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bFormatType;
    uint8_t bNrChannels;
    uint8_t bSubframeSize;
    uint8_t bBitResolution;
    uint8_t bSamFreqType;
    uint8_t tSamFreq[3 * UAC_FREQ_NUM_MAX];
} __attribute__((packed)) uac_as_type_I_format_desc_t;

/**
 * @brief Audio Class-Specific AS Isochronous Audio Data Endpoint Descriptor
 *
 * @see Table 4-21 of audio10.pdf
 */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bmAttributes;
    uint8_t bLockDelayUnits;
    uint16_t wLockDelay;
} __attribute__((packed)) uac_as_cs_ep_desc_t;

/**
 * @brief Print UAC device full configuration descriptor
 *
 * @param cfg_desc
 */
void print_uac_descriptors(const usb_config_desc_t *cfg_desc);

#ifdef __cplusplus
}
#endif //__cplusplus
