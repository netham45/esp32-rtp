/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "usb/usb_helpers.h"
#include "usb/usb_types_ch9.h"
#include "esp_check.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

// ----------------------------------------------- Descriptor Printing -------------------------------------------------

static void print_ep_desc(const usb_ep_desc_t *ep_desc)
{
    const char *ep_type_str;
    int type = ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;

    switch (type) {
    case USB_BM_ATTRIBUTES_XFER_CONTROL:
        ep_type_str = "CTRL";
        break;
    case USB_BM_ATTRIBUTES_XFER_ISOC:
        ep_type_str = "ISOC";
        break;
    case USB_BM_ATTRIBUTES_XFER_BULK:
        ep_type_str = "BULK";
        break;
    case USB_BM_ATTRIBUTES_XFER_INT:
        ep_type_str = "INT";
        break;
    default:
        ep_type_str = NULL;
        break;
    }

    printf("\t\t*** Endpoint descriptor ***\n");
    printf("\t\tbLength %d\n", ep_desc->bLength);
    printf("\t\tbDescriptorType %d\n", ep_desc->bDescriptorType);
    printf("\t\tbEndpointAddress 0x%x\tEP %d %s\n", ep_desc->bEndpointAddress,
           USB_EP_DESC_GET_EP_NUM(ep_desc),
           USB_EP_DESC_GET_EP_DIR(ep_desc) ? "IN" : "OUT");
    printf("\t\tbmAttributes 0x%x\t%s\n", ep_desc->bmAttributes, ep_type_str);
    printf("\t\twMaxPacketSize %d\n", USB_EP_DESC_GET_MPS(ep_desc));
    printf("\t\tbInterval %d\n", ep_desc->bInterval);
}

static void usbh_print_intf_desc(const usb_intf_desc_t *intf_desc)
{
    printf("\t*** Interface descriptor ***\n");
    printf("\tbLength %d\n", intf_desc->bLength);
    printf("\tbDescriptorType %d\n", intf_desc->bDescriptorType);
    printf("\tbInterfaceNumber %d\n", intf_desc->bInterfaceNumber);
    printf("\tbAlternateSetting %d\n", intf_desc->bAlternateSetting);
    printf("\tbNumEndpoints %d\n", intf_desc->bNumEndpoints);
    printf("\tbInterfaceClass 0x%x\n", intf_desc->bInterfaceClass);
    printf("\tbInterfaceSubClass 0x%x\n", intf_desc->bInterfaceSubClass);
    printf("\tbInterfaceProtocol 0x%x\n", intf_desc->bInterfaceProtocol);
    printf("\tiInterface %d\n", intf_desc->iInterface);
}

static void usbh_print_cfg_desc(const usb_config_desc_t *cfg_desc)
{
    printf("*** Configuration descriptor ***\n");
    printf("bLength %d\n", cfg_desc->bLength);
    printf("bDescriptorType %d\n", cfg_desc->bDescriptorType);
    printf("wTotalLength %d\n", cfg_desc->wTotalLength);
    printf("bNumInterfaces %d\n", cfg_desc->bNumInterfaces);
    printf("bConfigurationValue %d\n", cfg_desc->bConfigurationValue);
    printf("iConfiguration %d\n", cfg_desc->iConfiguration);
    printf("bmAttributes 0x%x\n", cfg_desc->bmAttributes);
    printf("bMaxPower %dmA\n", cfg_desc->bMaxPower * 2);
}

static void print_iad_desc(const usb_iad_desc_t *iad_desc)
{
    printf("*** Interface Association Descriptor ***\n");
    printf("bLength %d\n", iad_desc->bLength);
    printf("bDescriptorType %d\n", iad_desc->bDescriptorType);
    printf("bFirstInterface %d\n", iad_desc->bFirstInterface);
    printf("bInterfaceCount %d\n", iad_desc->bInterfaceCount);
    printf("bFunctionClass 0x%x\n", iad_desc->bFunctionClass);
    printf("bFunctionSubClass 0x%x\n", iad_desc->bFunctionSubClass);
    printf("bFunctionProtocol 0x%x\n", iad_desc->bFunctionProtocol);
    printf("iFunction %d\n", iad_desc->iFunction);
}

static void print_ac_header_desc(const uint8_t *buff)
{
    const uac_ac_header_desc_t *desc = (const uac_ac_header_desc_t *)buff;
    printf("\t*** Audio control header descriptor ***\n");
    printf("\tbLength %d\n", desc->bLength);
    printf("\tbDescriptorType 0x%x\n", desc->bDescriptorType);
    printf("\tbDescriptorSubtype 0x%x\n", desc->bDescriptorSubtype);
    printf("\tbcdADC 0x%x\n", desc->bcdADC);
    printf("\twTotalLength %d\n", desc->wTotalLength);
    printf("\tbInCollection %d\n", desc->bInCollection);
    if (desc->bInCollection) {
        const uint8_t *p_intf = desc->baInterfaceNr;
        for (int i = 0; i < desc->bInCollection; ++i) {
            printf("\t\tInterface number[%d] = %d\n", i, p_intf[i]);
        }
    }
}

static void print_ac_input_desc(const uint8_t *buff)
{
    const uac_ac_input_terminal_desc_t *desc = (const uac_ac_input_terminal_desc_t *)buff;
    printf("\t*** Audio control input terminal descriptor ***\n");
    printf("\tbLength %d\n", desc->bLength);
    printf("\tbDescriptorType 0x%x\n", desc->bDescriptorType);
    printf("\tbDescriptorSubtype 0x%x\n", desc->bDescriptorSubtype);
    printf("\tbTerminalID %d\n", desc->bTerminalID);
    printf("\twTerminalType 0x%x\n", desc->wTerminalType);
    printf("\tbAssocTerminal %d\n", desc->bAssocTerminal);
    printf("\tbNrChannels %d\n", desc->bNrChannels);
    printf("\twChannelConfig 0x%04x\n", desc->wChannelConfig);
    printf("\tiChannelNames %d\n", desc->iChannelNames);
    printf("\tiTerminal %d\n", desc->iTerminal);
}

static void print_ac_output_desc(const uint8_t *buff)
{
    const uac_ac_output_terminal_desc_t *desc = (const uac_ac_output_terminal_desc_t *)buff;
    printf("\t*** Audio control output terminal descriptor ***\n");
    printf("\tbLength %d\n", desc->bLength);
    printf("\tbDescriptorType 0x%x\n", desc->bDescriptorType);
    printf("\tbDescriptorSubtype 0x%x\n", desc->bDescriptorSubtype);
    printf("\tbTerminalID %d\n", desc->bTerminalID);
    printf("\twTerminalType 0x%x\n", desc->wTerminalType);
    printf("\tbAssocTerminal %d\n", desc->bAssocTerminal);
    printf("\tbSourceID %d\n", desc->bSourceID);
    printf("\tiTerminal %d\n", desc->iTerminal);
}

static void print_ac_feature_desc(const uint8_t *buff)
{
    const uac_ac_feature_unit_desc_t *desc = (const uac_ac_feature_unit_desc_t *)buff;
    printf("\t*** Audio control feature unit descriptor ***\n");
    printf("\tbLength %d\n", desc->bLength);
    printf("\tbDescriptorType 0x%x\n", desc->bDescriptorType);
    printf("\tbDescriptorSubtype 0x%x\n", desc->bDescriptorSubtype);
    printf("\tbUnitID %d\n", desc->bUnitID);
    printf("\tbSourceID %d\n", desc->bSourceID);
    printf("\tbControlSize %d\n", desc->bControlSize);
    for (size_t i = 0; i < (desc->bLength - 7) / desc->bControlSize; i += desc->bControlSize) {
        printf("\tbmaControls[ch%d] 0x%x\n", i, desc->bmaControls[i]);
    }
    printf("\tiFeature %d\n", desc->iFeature);
}

static void print_ac_mix_desc(const uint8_t *buff)
{
    const uac_ac_mixer_unit_desc_t *desc = (const uac_ac_mixer_unit_desc_t *)buff;
    printf("\t*** Audio control mixer unit descriptor ***\n");
    printf("\tbLength %d\n", desc->bLength);
    printf("\tbDescriptorType 0x%x\n", desc->bDescriptorType);
    printf("\tbDescriptorSubtype 0x%x\n", desc->bDescriptorSubtype);
    printf("\tbUnitID %d\n", desc->bUnitID);
    printf("\tbNrInPins %d\n", desc->bNrInPins);
    for (size_t i = 0; i < desc->bNrInPins; ++i) {
        printf("\tbSourceID[%d] %d\n", i, desc->baSourceID[i]);
    }
    printf("\tbNrChannels %d\n", buff[5 + desc->bNrInPins]);
    printf("\twChannelConfig 0x%x\n", buff[6 + desc->bNrInPins] | (buff[7 + desc->bNrInPins] << 8));
    printf("\tiChannelNames %d\n", buff[8 + desc->bNrInPins]);
    printf("\tbmControls 0x%x\n", buff[9 + desc->bNrInPins]);
    printf("\tiMixer %d\n", buff[10 + desc->bNrInPins]);
}

static void print_ac_selector_desc(const uint8_t *buff)
{
    const uac_ac_selector_unit_desc_t *desc = (const uac_ac_selector_unit_desc_t *)buff;
    printf("\t*** Audio control selector unit descriptor ***\n");
    printf("\tbLength %d\n", desc->bLength);
    printf("\tbDescriptorType 0x%x\n", desc->bDescriptorType);
    printf("\tbDescriptorSubtype 0x%x\n", desc->bDescriptorSubtype);
    printf("\tbUnitID %d\n", desc->bUnitID);
    printf("\tbNrInPins %d\n", desc->bNrInPins);
    for (size_t i = 0; i < desc->bNrInPins; ++i) {
        printf("\tbSourceID[%d] %d\n", i, desc->baSourceID[i]);
    }
    printf("\tiSelector %d\n", buff[5 + desc->bNrInPins]);
}

static void parse_as_ep_general_desc(const uint8_t *buff)
{
    const uac_as_cs_ep_desc_t *desc = (const uac_as_cs_ep_desc_t *)buff;
    printf("\t\t*** Audio stream endpoint general descriptor ***\n");
    printf("\t\tbLength %d\n", desc->bLength);
    printf("\t\tbDescriptorType 0x%x\n", desc->bDescriptorType);
    printf("\t\tbDescriptorSubtype 0x%x\n", desc->bDescriptorSubtype);
    printf("\t\tbmAttributes 0x%x\n", desc->bmAttributes);
    printf("\t\tbLockDelayUnits %d\n", desc->bLockDelayUnits);
    printf("\t\twLockDelay %d\n", desc->wLockDelay);
}

static void parse_as_general_desc(const uint8_t *buff)
{
    const uac_as_general_desc_t *desc = (const uac_as_general_desc_t *)buff;
    printf("\t*** Audio stream general descriptor ***\n");
    printf("\tbLength %d\n", desc->bLength);
    printf("\tbDescriptorType 0x%x\n", desc->bDescriptorType);
    printf("\tbDescriptorSubtype 0x%x\n", desc->bDescriptorSubtype);
    printf("\tbTerminalLink %d\n", desc->bTerminalLink);
    printf("\tbDelay %d\n", desc->bDelay);
    printf("\twFormatTag %d\n", desc->wFormatTag);
}

static void parse_as_type_desc(const uint8_t *buff)
{
    const uac_as_type_I_format_desc_t *desc = (const uac_as_type_I_format_desc_t *)buff;
    printf("\t*** Audio stream format type descriptor ***\n");
    printf("\tbLength %d\n", desc->bLength);
    printf("\tbDescriptorType 0x%x\n", desc->bDescriptorType);
    printf("\tbDescriptorSubtype 0x%x\n", desc->bDescriptorSubtype);
    printf("\tbFormatType %d\n", desc->bFormatType);
    printf("\tbNrChannels %d\n", desc->bNrChannels);
    printf("\tbSubframeSize %d\n", desc->bSubframeSize);
    printf("\tbBitResolution %d\n", desc->bBitResolution);
    printf("\tbSamFreqType %d\n", desc->bSamFreqType);
    if (desc->bSamFreqType == 0) {
        // Continuous Frame Intervals
        const uint8_t *p_samfreq = desc->tSamFreq;
        uint32_t min_samfreq = (p_samfreq[2] << 16) + (p_samfreq[1] << 8) + p_samfreq[0];
        uint32_t max_samfreq = (p_samfreq[5] << 16) + (p_samfreq[4] << 8) + p_samfreq[3];
        printf("\ttLowerSamFreq %"PRIu32"\n", min_samfreq);
        printf("\ttUpperSamFreq %"PRIu32"\n", max_samfreq);
    } else {
        const uint8_t *p_samfreq = desc->tSamFreq;
        for (int i = 0; i < desc->bSamFreqType; ++i) {
            printf("\ttSamFreq[%d] %"PRIu32"\n", i, (uint32_t)((p_samfreq[3 * i + 2] << 16) + (p_samfreq[3 * i + 1] << 8) + p_samfreq[3 * i]));
        }
    }
}

static void print_unknown_desc(const uac_desc_header_t *desc)
{
    printf("\t*** Unknown descriptor ***\n");
    printf("\tbLength %d\n", desc->bLength);
    printf("\tbDescriptorType 0x%x\n", desc->bDescriptorType);
    printf("\tbDescriptorSubtype 0x%x\n", desc->bDescriptorSubtype);
}

static void print_uac_class_descriptors(const usb_standard_desc_t *desc, uint8_t class, uint8_t subclass, uint8_t protocol)
{
    if (class != USB_CLASS_AUDIO) {
        return;
    }
    const uint8_t *buff = (const uint8_t *)desc;
    uac_desc_header_t *header = (uac_desc_header_t *)desc;
    if (subclass == UAC_SUBCLASS_AUDIOCONTROL) {
        switch (header->bDescriptorSubtype) {
        case UAC_AC_HEADER:
            print_ac_header_desc(buff);
            break;
        case UAC_AC_INPUT_TERMINAL:
            print_ac_input_desc(buff);
            break;
        case UAC_AC_OUTPUT_TERMINAL:
            print_ac_output_desc(buff);
            break;
        case UAC_AC_FEATURE_UNIT:
            print_ac_feature_desc(buff);
            break;
        case UAC_AC_MIXER_UNIT:
            print_ac_mix_desc(buff);
            break;
        case UAC_AC_SELECTOR_UNIT:
            print_ac_selector_desc(buff);
            break;
        default:
            goto unknown;
            break;
        }
    } else if (subclass == UAC_SUBCLASS_AUDIOSTREAMING && desc->bDescriptorType == UAC_CS_INTERFACE) {
        switch (header->bDescriptorSubtype) {
        case UAC_AS_GENERAL:
            parse_as_general_desc(buff);
            break;
        case UAC_AS_FORMAT_TYPE:
            parse_as_type_desc(buff);
            break;
        default:
            goto unknown;
            break;
        }
    } else if (subclass == UAC_SUBCLASS_AUDIOSTREAMING && desc->bDescriptorType == UAC_CS_ENDPOINT) {
        switch (header->bDescriptorSubtype) {
        case UAC_EP_GENERAL:
            parse_as_ep_general_desc(buff);
            break;
        default:
            break;
        }
    } else {
        printf("\tUnknown subclass 0x%x\n", subclass);
        goto unknown;
    }
    return;
unknown:
    print_unknown_desc(header);
}

// Print the configuration descriptor and all its sub-descriptors with the given class-specific callback
// The subclass and protocol are passed to the class_specific_cb to allow it to interpret the descriptors more accurately
// This function better be added to usb_helpers.c
static void usb_print_config_descriptor_with_context(const usb_config_desc_t *cfg_desc, print_class_descriptor_with_context_cb class_specific_cb)
{
    int offset = 0;
    uint16_t wTotalLength = cfg_desc->wTotalLength;
    const usb_standard_desc_t *next_desc = (const usb_standard_desc_t *)cfg_desc;
    uint8_t class = 0;
    uint8_t subclass = 0;
    uint8_t protocol = 0;

    do {
        switch (next_desc->bDescriptorType) {
        case USB_B_DESCRIPTOR_TYPE_CONFIGURATION:
            usbh_print_cfg_desc((const usb_config_desc_t *)next_desc);
            break;
        case USB_B_DESCRIPTOR_TYPE_INTERFACE: {
            const usb_intf_desc_t *intf_desc = (const usb_intf_desc_t *)next_desc;
            usbh_print_intf_desc(intf_desc);
            class = intf_desc->bInterfaceClass;
            subclass = intf_desc->bInterfaceSubClass;
            protocol = intf_desc->bInterfaceProtocol;
            break;
        }
        case USB_B_DESCRIPTOR_TYPE_ENDPOINT:
            print_ep_desc((const usb_ep_desc_t *)next_desc);
            break;
        case USB_B_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION:
            print_iad_desc((const usb_iad_desc_t *)next_desc);
            break;
        default:
            if (class_specific_cb) {
                class_specific_cb(next_desc, class, subclass, protocol);
            }
            break;
        }

        next_desc = usb_parse_next_descriptor(next_desc, wTotalLength, &offset);

    } while (next_desc != NULL);
}

void print_uac_descriptors(const usb_config_desc_t *cfg_desc)
{
    usb_print_config_descriptor_with_context(cfg_desc, print_uac_class_descriptors);
}
