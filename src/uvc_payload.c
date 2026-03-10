/*
 * uvc_payload.c — YUY2 Test Pattern Generator
 *
 * Generates a horizontal grayscale bar pattern without
 * ever allocating a full frame buffer. Each call to
 * uvc_payload_get_packet() computes pixels on the fly
 * and fills exactly one isochronous USB packet.
 *
 * YUY2 format recap:
 *   4 bytes = 2 pixels
 *   [Y0][U][Y1][V]
 *   Y = luma (brightness, 0=black, 255=white)
 *   U = chroma blue  (128 = neutral)
 *   V = chroma red   (128 = neutral)
 *   For greyscale: U=128, V=128 always
 */

#include "uvc_payload.h"
#include "usb_uvc.h"    /* for UVC_FRAME_WIDTH, HEIGHT, etc. */

/* ============================================================
   State — only a few bytes of RAM needed
   ============================================================ */

/* Current byte offset within the current frame (0..UVC_FRAME_SIZE-1) */
static uint32_t s_byte_offset = 0;

/*
 * Frame ID bit — toggles 0/1 every new frame.
 * Stored as the actual bit value to OR into bmHeaderInfo.
 * UVC_HDR_FID = bit 0 = 0x01
 */
static uint8_t s_frame_id = 0;

/* ============================================================
   Test Pattern — Horizontal Grayscale Bars
   
   Divides frame height into 8 bands.
   Band 0 (top) = black, Band 7 (bottom) = near white.
   ============================================================ */
#define NUM_BARS        8
#define BAR_HEIGHT      (UVC_FRAME_HEIGHT / NUM_BARS)   /* 144/8 = 18 lines per bar */

/*
 * get_luma_for_byte_offset() — compute Y value for a given byte offset.
 *
 * YUY2 layout:  [Y0 U Y1 V] per 4 bytes per 2 pixels per row
 * Byte offset within frame → which row → which bar → which luma
 *
 * bytes per row = UVC_FRAME_WIDTH * 2 (YUY2 = 2 bytes per pixel)
 */
static uint8_t get_luma_for_byte_offset(uint32_t offset)
{
    /* Which row (line) are we on? */
    uint32_t bytes_per_row = UVC_FRAME_WIDTH * 2;
    uint32_t row = offset / bytes_per_row;

    /* Which bar? */
    uint32_t bar_index = row / BAR_HEIGHT;
    if (bar_index >= NUM_BARS) bar_index = NUM_BARS - 1;

    /* Luma: 0 (black) to 224 (bright), step 32 per bar */
    return (uint8_t)(bar_index * 32);
}

/* ============================================================
   Public API Implementation
   ============================================================ */

void uvc_payload_reset(void)
{
    s_byte_offset = 0;
    /* Toggle FID bit for new frame */
    s_frame_id ^= UVC_HDR_FID;
}

uint32_t uvc_payload_bytes_remaining(void)
{
    if (s_byte_offset >= UVC_FRAME_SIZE) return 0;
    return UVC_FRAME_SIZE - s_byte_offset;
}

int uvc_payload_get_packet(uint8_t *buf, size_t buf_size)
{
    /* Nothing left in this frame */
    if (s_byte_offset >= UVC_FRAME_SIZE) {
        return 0;
    }

    /* How many payload bytes fit in this packet? */
    uint32_t remaining   = UVC_FRAME_SIZE - s_byte_offset;
    uint32_t payload_max = (uint32_t)(buf_size - UVC_HEADER_SIZE);
    uint32_t payload_len = (remaining < payload_max) ? remaining : payload_max;

    /* Is this the last packet of the frame? */
    uint8_t is_last = (payload_len == remaining) ? 1 : 0;

    /* ---- Build UVC payload header (2 bytes) ---- */
    uint8_t bm = s_frame_id;           /* FID bit */
    if (is_last) bm |= UVC_HDR_EOF;   /* EOF bit on last packet */
    buf[0] = bm;
    buf[1] = UVC_HEADER_SIZE;          /* bHeaderLength = 2 */

    /* ---- Fill YUY2 pixel data ---- */
    /*
     * YUY2 byte layout within a row:
     *   byte 0: Y0  (luma pixel 0)
     *   byte 1: U   (chroma, shared)
     *   byte 2: Y1  (luma pixel 1)
     *   byte 3: V   (chroma, shared)
     *
     * We step through 4 bytes at a time for pairs of pixels.
     * s_byte_offset must stay aligned to YUY2 macro-pixel (4 bytes).
     */
    uint8_t *payload = buf + UVC_HEADER_SIZE;
    uint32_t i = 0;

    while (i < payload_len) {
        uint32_t frame_byte = s_byte_offset + i;

        /* Position within 4-byte YUY2 macro-pixel */
        uint32_t sub = frame_byte % 4;

        if (sub == 0) {
            /* Y0 */
            payload[i] = get_luma_for_byte_offset(frame_byte);
        } else if (sub == 1) {
            /* U — neutral chroma */
            payload[i] = 128;
        } else if (sub == 2) {
            /* Y1 — same bar as Y0 (same row) */
            payload[i] = get_luma_for_byte_offset(frame_byte - 2);
        } else {
            /* V — neutral chroma */
            payload[i] = 128;
        }

        i++;
    }

    s_byte_offset += payload_len;

    /* Return total bytes written including header */
    return (int)(UVC_HEADER_SIZE + payload_len);
}