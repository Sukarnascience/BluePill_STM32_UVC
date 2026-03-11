/*
 * uvc_desc.c — All USB + UVC descriptors as raw byte arrays
 */
#include "uvc_desc.h"

/* ---- Device Descriptor ---- */
const uint8_t uvc_device_descriptor[18] = {
    18,           /* bLength */
    0x01,         /* bDescriptorType: DEVICE */
    0x00, 0x02,   /* bcdUSB: 2.0 */
    0xEF,         /* bDeviceClass: Misc */
    0x02,         /* bDeviceSubClass */
    0x01,         /* bDeviceProtocol: IAD */
    64,           /* bMaxPacketSize0 */
    0xFE, 0xCA,   /* idVendor:  0xCAFE */
    0x10, 0x40,   /* idProduct: 0x4010 */
    0x00, 0x01,   /* bcdDevice: 1.0 */
    0x01,         /* iManufacturer */
    0x02,         /* iProduct */
    0x03,         /* iSerialNumber */
    0x01,         /* bNumConfigurations */
};

/* ---- Configuration Descriptor (162 bytes) ---- */
const uint8_t uvc_config_descriptor[CFG_LEN] = {

    /* [1] Configuration — 9 bytes */
    9, 0x02,
    U16L(CFG_LEN), U16H(CFG_LEN),
    2, 1, 0, 0x80, 50,

    /* [2] IAD — 8 bytes */
    8, 0x0B, 0, 2, 0x0E, 0x03, 0x00, 0,

    /* [3] VC Interface — 9 bytes */
    9, 0x04, 0, 0, 0, 0x0E, 0x01, 0x00, 0,

    /* [4] VC Header — 13 bytes */
    13, 0x24, 0x01,
    0x10, 0x01,       /* bcdUVC 1.1 */
    40, 0x00,         /* wTotalLength: 13+18+9=40 */
    0x80,0x8D,0x5B,0x00, /* dwClockFreq */
    1, 1,             /* bInCollection, baInterfaceNr */

    /* [5] Input Terminal (Camera) — 18 bytes */
    18, 0x24, 0x02, 1, 0x01,0x02, 0, 0,
    0,0, 0,0, 0,0, 2, 0,0,

    /* [6] Output Terminal — 9 bytes */
    9, 0x24, 0x03, 2, 0x01,0x01, 0, 1, 0,

    /* [7] VS Interface Alt0 — 9 bytes */
    9, 0x04, 1, 0, 0, 0x0E, 0x02, 0x00, 0,

    /* [8] VS Input Header — 14 bytes */
    14, 0x24, 0x01, 1,
    71, 0x00,         /* wTotalLength: 14+27+30=71 */
    UVC_EP_IN, 0x00, 2, 0, 0, 0, 1, 0,

    /* [9] VS Format YUY2 — 27 bytes */
    27, 0x24, 0x04, 1, 1,
    'Y','U','Y','2',
    0x00,0x00,0x10,0x00,
    0x80,0x00,0x00,0xAA,
    0x00,0x38,0x9B,0x71,
    16, 1, 0, 0, 0, 0,

    /* [10] VS Frame 176x144 — 30 bytes */
    30, 0x24, 0x05, 1, 0,
    U16L(UVC_WIDTH),  U16H(UVC_WIDTH),
    U16L(UVC_HEIGHT), U16H(UVC_HEIGHT),
    U32_0(UVC_BITRATE), U32_1(UVC_BITRATE),
    U32_2(UVC_BITRATE), U32_3(UVC_BITRATE),
    U32_0(UVC_BITRATE), U32_1(UVC_BITRATE),
    U32_2(UVC_BITRATE), U32_3(UVC_BITRATE),
    U32_0(UVC_FRAME_SIZE), U32_1(UVC_FRAME_SIZE),
    U32_2(UVC_FRAME_SIZE), U32_3(UVC_FRAME_SIZE),
    U32_0(UVC_INTERVAL), U32_1(UVC_INTERVAL),
    U32_2(UVC_INTERVAL), U32_3(UVC_INTERVAL),
    1,
    U32_0(UVC_INTERVAL), U32_1(UVC_INTERVAL),
    U32_2(UVC_INTERVAL), U32_3(UVC_INTERVAL),

    /* [11] VS Interface Alt1 — 9 bytes */
    9, 0x04, 1, 1, 1, 0x0E, 0x02, 0x00, 0,

    /* [12] ISO Endpoint — 7 bytes */
    7, 0x05, UVC_EP_IN, 0x01,
    U16L(UVC_ISO_SIZE), U16H(UVC_ISO_SIZE),
    1,
};

/* ---- String Descriptors ---- */
/* Language: English (0x0409) */
const uint8_t uvc_lang_descriptor[] = {
    4, 0x03, 0x09, 0x04
};

/* Manufacturer: "BeeBotix" */
const uint8_t uvc_mfr_descriptor[] = {
    18, 0x03,
    'B',0,'e',0,'e',0,'B',0,'o',0,'t',0,'i',0,'x',0
};

/* Product: "UVC Camera" */
const uint8_t uvc_prod_descriptor[] = {
    22, 0x03,
    'U',0,'V',0,'C',0,' ',0,'C',0,'a',0,'m',0,'e',0,'r',0,'a',0
};

/* Serial: "001" */
const uint8_t uvc_serial_descriptor[] = {
    8, 0x03,
    '0',0,'0',0,'1',0
};

/* ---- Probe/Commit default values ---- */
uvc_probe_t uvc_probe_commit = {
    .bmHint                   = 1,
    .bFormatIndex             = 1,
    .bFrameIndex              = 1,
    .dwFrameInterval          = UVC_INTERVAL,
    .dwMaxVideoFrameSize      = UVC_FRAME_SIZE,
    .dwMaxPayloadTransferSize = UVC_ISO_SIZE,
    .dwClockFrequency         = 48000000UL,
    .bmFramingInfo            = 0x01,
    .bPreferedVersion         = 1,
    .bMinVersion              = 1,
    .bMaxVersion              = 1,
};