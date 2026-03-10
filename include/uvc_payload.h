#ifndef UVC_PAYLOAD_H
#define UVC_PAYLOAD_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
   Test Pattern Payload Generator
   
   Generates YUY2 test pattern data one packet at a time.
   Never allocates a full frame — RAM safe on Blue Pill.
   ============================================================ */

/**
 * uvc_payload_reset() — Call at start of each new frame.
 * Resets byte offset counter, toggles Frame ID.
 */
void uvc_payload_reset(void);

/**
 * uvc_payload_get_packet() — Fill one ISO packet buffer.
 *
 * @param buf      Output buffer (must be at least UVC_ISO_PACKET_SIZE bytes)
 * @param buf_size Size of output buffer
 * @return         Number of bytes written (2 header + payload), or 0 if frame done
 *
 * Usage in ISO callback:
 *   uint8_t pkt[UVC_ISO_PACKET_SIZE];
 *   int len = uvc_payload_get_packet(pkt, sizeof(pkt));
 *   if (len == 0) {
 *       uvc_payload_reset();          // Start new frame
 *       len = uvc_payload_get_packet(pkt, sizeof(pkt));
 *   }
 *   usbd_ep_write_packet(usbd_dev, UVC_ENDPOINT_IN, pkt, len);
 */
int uvc_payload_get_packet(uint8_t *buf, size_t buf_size);

/**
 * uvc_payload_bytes_remaining() — How many bytes left in current frame.
 * Returns 0 when full frame has been sent.
 */
uint32_t uvc_payload_bytes_remaining(void);

#endif /* UVC_PAYLOAD_H */