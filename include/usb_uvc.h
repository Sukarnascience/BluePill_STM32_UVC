#ifndef USB_UVC_H
#define USB_UVC_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
   UVC Camera Descriptor Constants
   ============================================================ */

/* USB IDs — use 0xCAFE/0x4010 for development (not registered) */
#define USB_VID                     0xCAFE
#define USB_PID                     0x4010

/* Video resolution — minimum that Windows 11 accepts */
#define UVC_FRAME_WIDTH             176
#define UVC_FRAME_HEIGHT            144

/* Frames per second — low because USB FS + RAM constraints */
#define UVC_FPS                     5

/* Frame interval in 100ns units: 10,000,000 / FPS */
#define UVC_FRAME_INTERVAL          (10000000 / UVC_FPS)

/*
 * YUY2 = 2 bytes per pixel (Y0 U Y1 V per 2 pixels)
 * Total frame: 176 * 144 * 2 = 50,688 bytes
 */
#define UVC_BYTES_PER_PIXEL         2
#define UVC_FRAME_SIZE              (UVC_FRAME_WIDTH * UVC_FRAME_HEIGHT * UVC_BYTES_PER_PIXEL)

/*
 * Max bytes per isochronous packet.
 * USB FS max = 1023. We use 512 to be conservative.
 * 2 bytes UVC header + 510 bytes payload per packet.
 */
#define UVC_ISO_PACKET_SIZE         512
#define UVC_HEADER_SIZE             2
#define UVC_PAYLOAD_PER_PACKET      (UVC_ISO_PACKET_SIZE - UVC_HEADER_SIZE)

/* Interface numbers */
#define UVC_INTF_VIDEO_CONTROL      0
#define UVC_INTF_VIDEO_STREAMING    1

/* Endpoint address: EP1 IN (device→host) */
#define UVC_ENDPOINT_IN             0x81

/* ============================================================
   UVC bmHeaderInfo bits (Payload Header byte 0)
   ============================================================ */
#define UVC_HDR_FID                 (1 << 0)   /* Frame ID — toggle each frame */
#define UVC_HDR_EOF                 (1 << 1)   /* End of Frame */
#define UVC_HDR_PTS                 (1 << 2)   /* PTS present */
#define UVC_HDR_SCR                 (1 << 3)   /* SCR present */
#define UVC_HDR_RES                 (1 << 4)   /* reserved */
#define UVC_HDR_STI                 (1 << 5)   /* Still image */
#define UVC_HDR_ERR                 (1 << 6)   /* Error */
#define UVC_HDR_EOH                 (1 << 7)   /* End of header */

/* ============================================================
   UVC Probe/Commit Control Structure (UVC 1.1 spec, 34 bytes)
   ============================================================ */
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
} uvc_streaming_control_t;

/* ============================================================
   UVC VideoControl Request Codes
   ============================================================ */
#define UVC_SET_CUR                 0x01
#define UVC_GET_CUR                 0x81
#define UVC_GET_MIN                 0x82
#define UVC_GET_MAX                 0x83
#define UVC_GET_DEF                 0x87

/* VideoStreaming control selectors */
#define UVC_VS_PROBE_CONTROL        0x01
#define UVC_VS_COMMIT_CONTROL       0x02

/* ============================================================
   Public API
   ============================================================ */

/**
 * usb_uvc_init() — Initialize USB peripheral and UVC descriptors.
 * Call once at startup after clock configuration.
 */
void usb_uvc_init(void);

/**
 * usb_uvc_poll() — Must be called repeatedly in main loop.
 * Handles USB events, control requests, and isochronous feeding.
 */
void usb_uvc_poll(void);

/**
 * usb_uvc_is_streaming() — Returns 1 if host has committed to stream.
 * Use this to know when to start generating test pattern data.
 */
int usb_uvc_is_streaming(void);

#endif /* USB_UVC_H */