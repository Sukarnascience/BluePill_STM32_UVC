/*
 * usb_uvc.c — UVC Descriptors + USB Control Handling
 *
 * This file contains:
 *   1. All USB/UVC descriptor byte arrays (stored in Flash)
 *   2. Probe/Commit negotiation handler
 *   3. Isochronous endpoint callback (feeds pixel data)
 *   4. libopencm3 USB device setup
 *
 * libopencm3 USB API:
 *   usbd_init()              — init USB peripheral
 *   usbd_register_set_config_callback() — called after SET_CONFIGURATION
 *   usbd_ep_setup()          — configure an endpoint
 *   usbd_ep_write_packet()   — write data to an IN endpoint
 *   usbd_poll()              — process pending USB events (call in loop)
 */

#include <string.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/usbstd.h>

#include "usb_uvc.h"
#include "uvc_payload.h"

/* ============================================================
   Internal State
   ============================================================ */

static usbd_device  *s_usbd_dev   = NULL;
static int           s_streaming  = 0;    /* 1 after VS_COMMIT received */
static uvc_streaming_control_t s_probe;   /* current probe/commit state */

/* ============================================================
   YUY2 GUID
   Per UVC 1.1 spec, Appendix A: {32595559-0000-0010-8000-00AA00389B71}
   Stored little-endian as bytes
   ============================================================ */
static const uint8_t yuy2_guid[16] = {
    'Y','U','Y','2',
    0x00, 0x00,
    0x10, 0x00,
    0x80, 0x00,
    0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};

/* ============================================================
   USB Device Descriptor
   ============================================================ */
static const struct usb_device_descriptor dev_descr = {
    .bLength            = USB_DT_DEVICE_SIZE,       /* 18 */
    .bDescriptorType    = USB_DT_DEVICE,
    .bcdUSB             = 0x0200,                   /* USB 2.0 */
    .bDeviceClass       = 0xEF,                     /* Miscellaneous */
    .bDeviceSubClass    = 0x02,                     /* Common Class */
    .bDeviceProtocol    = 0x01,                     /* IAD */
    .bMaxPacketSize0    = 64,                       /* EP0 max packet */
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,                   /* Device version 1.0 */
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

/* ============================================================
   VideoControl Interface Class-Specific Descriptors
   
   These go after the standard Interface Descriptor for Interface 0.
   They describe the camera topology: Input Terminal → Output Terminal.
   ============================================================ */

/*
 * VC Header Descriptor (UVC spec Table 3-3)
 * Declares UVC version and total size of VC class-specific descriptors.
 */
static const uint8_t vc_header[] = {
    13,             /* bLength */
    0x24,           /* bDescriptorType: CS_INTERFACE */
    0x01,           /* bDescriptorSubType: VC_HEADER */
    0x10, 0x01,     /* bcdUVC: UVC version 1.1 */
    /* wTotalLength: total bytes of all VC class-specific descriptors
       = VC_HEADER(13) + INPUT_TERMINAL(18) + OUTPUT_TERMINAL(9) = 40 */
    40, 0x00,
    /* dwClockFrequency: 48 MHz (not critical for test pattern) */
    0x80, 0x8D, 0x5B, 0x00,
    0x01,           /* bInCollection: 1 VideoStreaming interface */
    0x01,           /* baInterfaceNr(1): VS interface number = 1 */
};

/*
 * Input Terminal Descriptor — Camera (UVC spec Table 3-5)
 * Represents the camera sensor (even though we fake it).
 */
static const uint8_t vc_input_terminal[] = {
    18,             /* bLength */
    0x24,           /* bDescriptorType: CS_INTERFACE */
    0x02,           /* bDescriptorSubType: VC_INPUT_TERMINAL */
    0x01,           /* bTerminalID: 1 */
    0x01, 0x02,     /* wTerminalType: ITT_CAMERA (0x0201) */
    0x00,           /* bAssocTerminal: none */
    0x00,           /* iTerminal: no string */
    0x00, 0x00,     /* wObjectiveFocalLengthMin: 0 (not applicable) */
    0x00, 0x00,     /* wObjectiveFocalLengthMax: 0 */
    0x00, 0x00,     /* wOcularFocalLength: 0 */
    0x02,           /* bControlSize: 2 bytes of bmControls */
    0x00, 0x00,     /* bmControls: no controls supported */
};

/*
 * Output Terminal Descriptor (UVC spec Table 3-7)
 * Connects the Input Terminal to the USB streaming output.
 */
static const uint8_t vc_output_terminal[] = {
    9,              /* bLength */
    0x24,           /* bDescriptorType: CS_INTERFACE */
    0x03,           /* bDescriptorSubType: VC_OUTPUT_TERMINAL */
    0x02,           /* bTerminalID: 2 */
    0x01, 0x01,     /* wTerminalType: TT_STREAMING (0x0101) */
    0x00,           /* bAssocTerminal: none */
    0x01,           /* bSourceID: connected to Input Terminal (ID=1) */
    0x00,           /* iTerminal: no string */
};

/* ============================================================
   VideoStreaming Interface Class-Specific Descriptors
   ============================================================ */

/*
 * VS Input Header Descriptor (UVC spec Table 3-14)
 */
static const uint8_t vs_input_header[] = {
    14,             /* bLength */
    0x24,           /* bDescriptorType: CS_INTERFACE */
    0x01,           /* bDescriptorSubType: VS_INPUT_HEADER */
    0x01,           /* bNumFormats: 1 format (YUY2) */
    /* wTotalLength: total VS class-specific = InputHeader(14) + Format(27) + Frame(30) = 71 */
    71, 0x00,
    UVC_ENDPOINT_IN,/* bEndpointAddress: EP1 IN */
    0x00,           /* bmInfo: no dynamic format change */
    0x02,           /* bTerminalLink: Output Terminal ID = 2 */
    0x00,           /* bStillCaptureMethod: none */
    0x00,           /* bTriggerSupport: 0 */
    0x00,           /* bTriggerUsage: 0 */
    0x01,           /* bControlSize: 1 byte */
    0x00,           /* bmaControls: no controls */
};

/*
 * VS Format Descriptor — Uncompressed YUY2 (UVC spec Table 3-18)
 */
static const uint8_t vs_format_yuy2[] = {
    27,             /* bLength */
    0x24,           /* bDescriptorType: CS_INTERFACE */
    0x04,           /* bDescriptorSubType: VS_FORMAT_UNCOMPRESSED */
    0x01,           /* bFormatIndex: 1 */
    0x01,           /* bNumFrameDescriptors: 1 frame size */
    /* guidFormat: YUY2 GUID (16 bytes) */
    'Y','U','Y','2',
    0x00, 0x00,
    0x10, 0x00,
    0x80, 0x00,
    0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
    16,             /* bBitsPerPixel: 16 (YUY2 = 2 bytes/pixel average) */
    0x01,           /* bDefaultFrameIndex: use frame 1 */
    0,              /* bAspectRatioX: not required */
    0,              /* bAspectRatioY: not required */
    0x00,           /* bmInterlaceFlags: progressive */
    0x00,           /* bCopyProtect: no */
};

/*
 * VS Frame Descriptor — 176x144 (UVC spec Table 3-19)
 * Declares the one resolution we support.
 */
static const uint8_t vs_frame_176x144[] = {
    30,             /* bLength */
    0x24,           /* bDescriptorType: CS_INTERFACE */
    0x05,           /* bDescriptorSubType: VS_FRAME_UNCOMPRESSED */
    0x01,           /* bFrameIndex: 1 */
    0x00,           /* bmCapabilities: still image unsupported */

    /* wWidth, wHeight — little endian */
    UVC_FRAME_WIDTH  & 0xFF, (UVC_FRAME_WIDTH  >> 8) & 0xFF,
    UVC_FRAME_HEIGHT & 0xFF, (UVC_FRAME_HEIGHT >> 8) & 0xFF,

    /* dwMinBitRate = width * height * bpp * fps * 8 bits
       = 176*144*16*5 = 20,275,200 bps */
    0x00, 0xC8, 0x35, 0x01,

    /* dwMaxBitRate = same (fixed fps) */
    0x00, 0xC8, 0x35, 0x01,

    /* dwMaxVideoFrameBufferSize = 176*144*2 = 50688 = 0xC600 */
    0x00, 0xC6, 0x00, 0x00,

    /* dwDefaultFrameInterval = 10,000,000 / 5fps = 2,000,000 = 0x001E8480 */
    0x80, 0x84, 0x1E, 0x00,

    0x01,           /* bFrameIntervalType: 1 discrete interval */

    /* dwFrameInterval(1) = 2,000,000 */
    0x80, 0x84, 0x1E, 0x00,
};

/* ============================================================
   Endpoints
   ============================================================ */

/* Isochronous IN endpoint for video streaming (alternate setting 1) */
static const struct usb_endpoint_descriptor iso_endp[] = {{
    .bLength          = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType  = USB_DT_ENDPOINT,
    .bEndpointAddress = UVC_ENDPOINT_IN,
    .bmAttributes     = 0x01,       /* Isochronous, no sync, data */
    .wMaxPacketSize   = UVC_ISO_PACKET_SIZE,
    .bInterval        = 1,          /* Every USB frame (1ms) */
}};

/* ============================================================
   VideoControl Interface (Interface 0)
   ============================================================ */

/* Class-specific descriptor extras for VC interface */
static const uint8_t *vc_extras[] = {
    vc_header,
    vc_input_terminal,
    vc_output_terminal,
};
static const uint8_t vc_extra_sizes[] = {
    sizeof(vc_header),
    sizeof(vc_input_terminal),
    sizeof(vc_output_terminal),
};

/* We need to pass these as one flat extra block to libopencm3.
   Build it at init time into a static buffer. */
#define VC_EXTRAS_TOTAL (sizeof(vc_header) + sizeof(vc_input_terminal) + sizeof(vc_output_terminal))
static uint8_t vc_extra_blob[VC_EXTRAS_TOTAL];

static const struct usb_interface_descriptor vc_iface = {
    .bLength            = USB_DT_INTERFACE_SIZE,
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = UVC_INTF_VIDEO_CONTROL,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 0,        /* VC has no data endpoints */
    .bInterfaceClass    = 0x0E,     /* CC_VIDEO */
    .bInterfaceSubClass = 0x01,     /* SC_VIDEOCONTROL */
    .bInterfaceProtocol = 0x00,
    .iInterface         = 0,
    .endpoint           = NULL,
    .extra              = vc_extra_blob,
    .extralen           = VC_EXTRAS_TOTAL,
};

/* ============================================================
   VideoStreaming Interface (Interface 1)
   Two alternate settings:
     Alt 0 — zero bandwidth (idle, no endpoint)
     Alt 1 — active streaming (isochronous endpoint)
   ============================================================ */

#define VS_EXTRAS_TOTAL (sizeof(vs_input_header) + sizeof(vs_format_yuy2) + sizeof(vs_frame_176x144))
static uint8_t vs_extra_blob[VS_EXTRAS_TOTAL];

/* Alt setting 0 — idle, no endpoint */
static const struct usb_interface_descriptor vs_iface_alt0 = {
    .bLength            = USB_DT_INTERFACE_SIZE,
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = UVC_INTF_VIDEO_STREAMING,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 0,
    .bInterfaceClass    = 0x0E,     /* CC_VIDEO */
    .bInterfaceSubClass = 0x02,     /* SC_VIDEOSTREAMING */
    .bInterfaceProtocol = 0x00,
    .iInterface         = 0,
    .endpoint           = NULL,
    .extra              = vs_extra_blob,
    .extralen           = VS_EXTRAS_TOTAL,
};

/* Alt setting 1 — active streaming with isochronous endpoint */
static const struct usb_interface_descriptor vs_iface_alt1 = {
    .bLength            = USB_DT_INTERFACE_SIZE,
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = UVC_INTF_VIDEO_STREAMING,
    .bAlternateSetting  = 1,
    .bNumEndpoints      = 1,
    .bInterfaceClass    = 0x0E,
    .bInterfaceSubClass = 0x02,
    .bInterfaceProtocol = 0x00,
    .iInterface         = 0,
    .endpoint           = iso_endp,
    .extra              = NULL,
    .extralen           = 0,
};

/* ============================================================
   Interface Association Descriptor (IAD)
   Groups VC + VS interfaces as one "Video" function.
   ============================================================ */
static const struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bFirstInterface;
    uint8_t bInterfaceCount;
    uint8_t bFunctionClass;
    uint8_t bFunctionSubClass;
    uint8_t bFunctionProtocol;
    uint8_t iFunction;
} iad = {
    .bLength           = 8,
    .bDescriptorType   = 0x0B,   /* INTERFACE_ASSOCIATION */
    .bFirstInterface   = 0,
    .bInterfaceCount   = 2,
    .bFunctionClass    = 0x0E,   /* CC_VIDEO */
    .bFunctionSubClass = 0x03,   /* SC_VIDEO_INTERFACE_COLLECTION */
    .bFunctionProtocol = 0x00,
    .iFunction         = 0,
};

/* ============================================================
   USB String Descriptors
   ============================================================ */
static const char *usb_strings[] = {
    "BeeBotix",          /* iManufacturer */
    "UVC Test Camera",   /* iProduct */
    "001",               /* iSerialNumber */
};

/* ============================================================
   Interface and Configuration Assembly
   ============================================================ */

/* VS interface has 2 alternate settings */
static const struct usb_interface_descriptor vs_altsettings[] = {
    vs_iface_alt0,
    vs_iface_alt1,
};

static const struct usb_interface interfaces[] = {
    /* Interface 0: VideoControl */
    {
        .num_altsetting = 1,
        .altsetting     = &vc_iface,
    },
    /* Interface 1: VideoStreaming (2 alt settings) */
    {
        .num_altsetting = 2,
        .altsetting     = vs_altsettings,
    },
};

static const struct usb_config_descriptor config_descr = {
    .bLength             = USB_DT_CONFIGURATION_SIZE,
    .bDescriptorType     = USB_DT_CONFIGURATION,
    .wTotalLength        = 0,   /* libopencm3 fills this in */
    .bNumInterfaces      = 2,
    .bConfigurationValue = 1,
    .iConfiguration      = 0,
    .bmAttributes        = 0x80, /* Bus powered */
    .bMaxPower           = 50,   /* 100mA */
    .interface           = interfaces,
    .extra               = &iad,
    .extralen            = sizeof(iad),
};

/* Control buffer required by libopencm3 USB stack */
static uint8_t usbd_control_buffer[128];

/* ============================================================
   Probe/Commit Default Values
   ============================================================ */
static void init_probe_commit(uvc_streaming_control_t *sc)
{
    memset(sc, 0, sizeof(*sc));
    sc->bmHint                    = 1;   /* dwFrameInterval is fixed */
    sc->bFormatIndex              = 1;
    sc->bFrameIndex               = 1;
    sc->dwFrameInterval           = UVC_FRAME_INTERVAL;
    sc->dwMaxVideoFrameSize       = UVC_FRAME_SIZE;
    sc->dwMaxPayloadTransferSize  = UVC_ISO_PACKET_SIZE;
    sc->dwClockFrequency          = 48000000;
    sc->bmFramingInfo             = 0x01; /* frame ID used */
    sc->bPreferedVersion          = 1;
    sc->bMinVersion               = 1;
    sc->bMaxVersion               = 1;
}

/* ============================================================
   UVC Class Control Request Handler
   
   Called by libopencm3 for class requests on the VC/VS interfaces.
   Handles:
     - VS_PROBE_CONTROL GET/SET
     - VS_COMMIT_CONTROL GET/SET
   ============================================================ */
static enum usbd_request_return_codes
uvc_control_request(usbd_device *usbd_dev,
                    struct usb_setup_data *req,
                    uint8_t **buf,
                    uint16_t *len,
                    usbd_control_complete_callback *complete)
{
    (void)usbd_dev;
    (void)complete;

    uint8_t interface = req->wIndex & 0xFF;
    uint8_t selector  = (req->wValue >> 8) & 0xFF;

    /* Only handle VideoStreaming interface (Interface 1) */
    if (interface != UVC_INTF_VIDEO_STREAMING) {
        return USBD_REQ_NEXT_CALLBACK;
    }

    /* Only handle VS_PROBE and VS_COMMIT selectors */
    if (selector != UVC_VS_PROBE_CONTROL &&
        selector != UVC_VS_COMMIT_CONTROL) {
        return USBD_REQ_NEXT_CALLBACK;
    }

    if ((req->bmRequestType & 0x80) == 0) {
        /* SET request — host is sending us data */
        if (*len >= sizeof(uvc_streaming_control_t)) {
            memcpy(&s_probe, *buf, sizeof(uvc_streaming_control_t));
        }
        if (selector == UVC_VS_COMMIT_CONTROL) {
            /* Host committed — start streaming */
            s_streaming = 1;
            uvc_payload_reset();
        }
    } else {
        /* GET request — host is asking for our values */
        *buf = (uint8_t *)&s_probe;
        *len = sizeof(uvc_streaming_control_t);
    }

    return USBD_REQ_HANDLED;
}

/* ============================================================
   Isochronous Endpoint Callback
   
   Called by libopencm3 every time the isochronous IN endpoint
   is ready to accept a packet (every 1ms USB frame).
   We fill one packet of test pattern data here.
   ============================================================ */
static uint8_t iso_buf[UVC_ISO_PACKET_SIZE];

static void iso_tx_cb(usbd_device *usbd_dev, uint8_t ep)
{
    (void)ep;

    if (!s_streaming) return;

    int len = uvc_payload_get_packet(iso_buf, sizeof(iso_buf));
    if (len == 0) {
        /* Frame complete — start next frame */
        uvc_payload_reset();
        len = uvc_payload_get_packet(iso_buf, sizeof(iso_buf));
    }

    if (len > 0) {
        usbd_ep_write_packet(usbd_dev, UVC_ENDPOINT_IN, iso_buf, (uint16_t)len);
    }
}

/* ============================================================
   SET_CONFIGURATION Callback
   
   Called by libopencm3 after host sends SET_CONFIGURATION.
   We register our class request handler and endpoint callback here.
   ============================================================ */
static void set_config_cb(usbd_device *usbd_dev, uint16_t wValue)
{
    (void)wValue;

    /* Setup isochronous IN endpoint */
    usbd_ep_setup(usbd_dev,
                  UVC_ENDPOINT_IN,
                  USB_ENDPOINT_ATTR_ISOCHRONOUS,
                  UVC_ISO_PACKET_SIZE,
                  iso_tx_cb);

    /* Register class request handler for both VC and VS interfaces */
    usbd_register_control_callback(
        usbd_dev,
        USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
        USB_REQ_TYPE_TYPE  | USB_REQ_TYPE_RECIPIENT,
        uvc_control_request);

    /* Start sending packets immediately so host sees data when it switches alt setting */
    uvc_payload_reset();
    int len = uvc_payload_get_packet(iso_buf, sizeof(iso_buf));
    if (len > 0) {
        usbd_ep_write_packet(usbd_dev, UVC_ENDPOINT_IN, iso_buf, (uint16_t)len);
    }
}

/* ============================================================
   Public API
   ============================================================ */

void usb_uvc_init(void)
{
    /* Build flat extra blobs from individual descriptor arrays */
    uint8_t *p;

    p = vc_extra_blob;
    memcpy(p, vc_header,          sizeof(vc_header));          p += sizeof(vc_header);
    memcpy(p, vc_input_terminal,  sizeof(vc_input_terminal));  p += sizeof(vc_input_terminal);
    memcpy(p, vc_output_terminal, sizeof(vc_output_terminal));

    p = vs_extra_blob;
    memcpy(p, vs_input_header,    sizeof(vs_input_header));    p += sizeof(vs_input_header);
    memcpy(p, vs_format_yuy2,     sizeof(vs_format_yuy2));     p += sizeof(vs_format_yuy2);
    memcpy(p, vs_frame_176x144,   sizeof(vs_frame_176x144));

    /* Initialize probe/commit with defaults */
    init_probe_commit(&s_probe);

    /* Initialize libopencm3 USB stack */
    s_usbd_dev = usbd_init(
        &st_usbfs_v1_usb_driver,   /* STM32F1 full-speed driver */
        &dev_descr,
        &config_descr,
        usb_strings,
        3,                          /* number of strings */
        usbd_control_buffer,
        sizeof(usbd_control_buffer)
    );

    usbd_register_set_config_callback(s_usbd_dev, set_config_cb);
}

void usb_uvc_poll(void)
{
    if (s_usbd_dev) {
        usbd_poll(s_usbd_dev);
    }
}

int usb_uvc_is_streaming(void)
{
    return s_streaming;
}