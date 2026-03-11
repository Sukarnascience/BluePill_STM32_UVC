#ifndef UVC_DESC_H
#define UVC_DESC_H

#include <stdint.h>

/* ---- Video dimensions ---- */
#define UVC_WIDTH       176
#define UVC_HEIGHT      144
#define UVC_FPS         5
#define UVC_EP_IN       0x81   /* EP1 IN */
#define UVC_ISO_SIZE    512    /* bytes per ISO packet */

/* ---- Frame size: YUY2 = 2 bytes/pixel ---- */
#define UVC_FRAME_SIZE  (UVC_WIDTH * UVC_HEIGHT * 2)

/* ---- Frame interval in 100ns units ---- */
#define UVC_INTERVAL    (10000000UL / UVC_FPS)

/* ---- Bit rates ---- */
#define UVC_BITRATE     (UVC_WIDTH * UVC_HEIGHT * 16 * UVC_FPS)

/* Helpers */
#define U16L(x)  ((x) & 0xFF)
#define U16H(x)  (((x) >> 8) & 0xFF)
#define U32_0(x) ((x) & 0xFF)
#define U32_1(x) (((x) >>  8) & 0xFF)
#define U32_2(x) (((x) >> 16) & 0xFF)
#define U32_3(x) (((x) >> 24) & 0xFF)

/*
 * Full Configuration Descriptor — 162 bytes
 * Layout:
 *   [0]   Config Descriptor        9
 *   [1]   IAD                      8
 *   [2]   VC Interface             9
 *   [3]   VC Header               13
 *   [4]   Input Terminal          18
 *   [5]   Output Terminal          9
 *   [6]   VS Interface Alt0        9
 *   [7]   VS Input Header         14
 *   [8]   VS Format YUY2          27
 *   [9]   VS Frame 176x144        30
 *   [10]  VS Interface Alt1        9
 *   [11]  ISO Endpoint             7
 *        Total                   162
 */
#define CFG_LEN  162

extern const uint8_t uvc_device_descriptor[18];
extern const uint8_t uvc_config_descriptor[CFG_LEN];
extern const uint8_t uvc_lang_descriptor[];
extern const uint8_t uvc_mfr_descriptor[];
extern const uint8_t uvc_prod_descriptor[];
extern const uint8_t uvc_serial_descriptor[];

/* Probe/Commit control structure — UVC 1.1 §4.3.1.1 */
typedef struct __attribute__((packed)) {
    uint16_t bmHint;
    uint8_t  bFormatIndex;
    uint8_t  bFrameIndex;
    uint32_t dwFrameInterval;
    uint16_t wKeyFrameRate;
    uint16_t wPFrameRate;
    uint16_t wCompQuality;
    uint16_t wCompWindowSize;
    uint16_t wDelay;
    uint32_t dwMaxVideoFrameSize;
    uint32_t dwMaxPayloadTransferSize;
    uint32_t dwClockFrequency;
    uint8_t  bmFramingInfo;
    uint8_t  bPreferedVersion;
    uint8_t  bMinVersion;
    uint8_t  bMaxVersion;
} uvc_probe_t;

extern uvc_probe_t uvc_probe_commit;

#endif /* UVC_DESC_H */