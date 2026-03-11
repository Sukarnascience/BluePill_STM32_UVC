#ifndef USB_REGS_H
#define USB_REGS_H

/*
 * usb_regs.h — STM32F103 USB Peripheral Accessors
 *
 * We deliberately DO NOT redefine bit-field macros that stm32f103xb.h
 * already provides (USB_CNTR_*, USB_ISTR_*, USB_EP_*, USB_DADDR_*).
 * We only add what HAL doesn't give us:
 *   - Register address macros (USB_CNTR, USB_ISTR, etc.)
 *   - Endpoint register array accessor USB_EPR(n)
 *   - EP STAT/TYPE numeric values with our own prefix
 *   - Packet SRAM layout and accessors
 *   - Toggle-safe EP macros
 */

#include <stdint.h>

/* ---- USB SRAM base (packet buffer area) ---- */
#define USB_SRAM_BASE   0x40006000UL

/* ---- USB Control Register accessors ---- */
/* HAL defines USB_BASE already — use it */
#define REG_USB_CNTR    (*(volatile uint16_t*)(USB_BASE + 0x40))
#define REG_USB_ISTR    (*(volatile uint16_t*)(USB_BASE + 0x44))
#define REG_USB_FNR     (*(volatile uint16_t*)(USB_BASE + 0x48))
#define REG_USB_DADDR   (*(volatile uint16_t*)(USB_BASE + 0x4C))
#define REG_USB_BTABLE  (*(volatile uint16_t*)(USB_BASE + 0x50))

/* ---- Endpoint Register array ---- */
#define USB_EPR(n)      (*(volatile uint16_t*)(USB_BASE + (n)*4))

/* ---- EP STAT numeric values (our prefix, no conflict) ---- */
#define EPSTAT_DISABLED  0U
#define EPSTAT_STALL     1U
#define EPSTAT_NAK       2U
#define EPSTAT_VALID     3U

/* ---- EP TYPE numeric values (our prefix, no conflict) ---- */
#define EPTYPE_BULK      0U
#define EPTYPE_CONTROL   1U
#define EPTYPE_ISO       2U
#define EPTYPE_INTERRUPT 3U

/* ---- Bit positions (safe numeric literals, no _Pos dependency) ---- */
#define USB_EP_STAT_TX_Pos   4U
#define USB_EP_STAT_RX_Pos  12U

/* Use HAL _Msk defines for bit fields — guaranteed in stm32f103xb.h */
/* USB_EP_STAT_TX_Msk, USB_EP_STAT_RX_Msk,   */
/* USB_EP_DTOG_TX_Msk, USB_EP_DTOG_RX_Msk,   */
/* USB_EP_CTR_TX_Msk,  USB_EP_CTR_RX_Msk,    */
/* USB_EP_SETUP_Msk    — all from stm32f103xb.h */

/*
 * EP STAT/DTOG bits — these are toggle-on-write.
 * To SET a STAT field to a desired value you must XOR with current value.
 * We must preserve CTR bits (write 1 to keep, write 0 to clear).
 * Safe pattern: keep CTR_RX and CTR_TX as-is, zero DTOG, XOR STAT.
 */
/*
 * EP register bit masks — plain numbers, zero HAL dependencies
 *
 * EP register layout (16-bit):
 *   [15]    CTR_RX   — rc_w0 (read/clear by writing 0)
 *   [14]    DTOG_RX  — toggle on write 1
 *   [13:12] STAT_RX  — toggle on write 1
 *   [11]    SETUP    — read only
 *   [10:9]  EP_TYPE  — read/write
 *   [8]     EP_KIND  — read/write
 *   [7]     CTR_TX   — rc_w0
 *   [6]     DTOG_TX  — toggle on write 1
 *   [5:4]   STAT_TX  — toggle on write 1
 *   [3:0]   EA       — read/write
 *
 * Rule for toggle bits (DTOG, STAT):
 *   Write 0 → no change
 *   Write 1 → toggles
 *
 * Rule for CTR bits:
 *   Write 1 → no change (preserved)
 *   Write 0 → cleared
 *
 * Safe EPR write pattern:
 *   Always write 1 to both CTR bits (preserve them)
 *   Write 0 to DTOG bits (no toggle)
 *   XOR the STAT bits with current to reach desired value
 */
#define EPR_CTR_RX      ((uint16_t)0x8000u)
#define EPR_DTOG_RX     ((uint16_t)0x4000u)
#define EPR_STAT_RX     ((uint16_t)0x3000u)
#define EPR_SETUP       ((uint16_t)0x0800u)
#define EPR_EP_TYPE     ((uint16_t)0x0600u)
#define EPR_EP_KIND     ((uint16_t)0x0100u)
#define EPR_CTR_TX      ((uint16_t)0x0080u)
#define EPR_DTOG_TX     ((uint16_t)0x0040u)
#define EPR_STAT_TX     ((uint16_t)0x0030u)
#define EPR_EA          ((uint16_t)0x000Fu)

/* Invariant bits — read/write directly, no toggle behavior */
#define EPR_RW_MASK     (EPR_EP_TYPE | EPR_EP_KIND | EPR_EA)

/*
 * EP_SET_STAT_TX(ep, desired_stat):
 *   - Preserve CTR_RX and CTR_TX by writing 1 to both
 *   - Write 0 to DTOG_TX and DTOG_RX (no accidental toggle)
 *   - XOR current STAT_TX with desired to toggle to target value
 */
#define EP_SET_STAT_TX(ep, stat) do { \
    uint16_t _v = USB_EPR(ep); \
    /* Build base: keep RW bits, set both CTR to 1, zero DTOG */ \
    uint16_t _w = (_v & EPR_RW_MASK) | EPR_CTR_RX | EPR_CTR_TX; \
    /* XOR current STAT_TX with desired — only bits that differ get toggled */ \
    _w ^= ((_v ^ ((uint16_t)((stat) & 0x3u) << 4)) & EPR_STAT_TX); \
    USB_EPR(ep) = _w; \
} while(0)

#define EP_SET_STAT_RX(ep, stat) do { \
    uint16_t _v = USB_EPR(ep); \
    uint16_t _w = (_v & EPR_RW_MASK) | EPR_CTR_RX | EPR_CTR_TX; \
    _w ^= ((_v ^ ((uint16_t)((stat) & 0x3u) << 12)) & EPR_STAT_RX); \
    USB_EPR(ep) = _w; \
} while(0)

/* Clear CTR_TX: write 0 to CTR_TX, write 1 to CTR_RX, preserve RW bits, zero toggles */
#define EP_CLEAR_CTR_TX(ep) do { \
    uint16_t _v = USB_EPR(ep); \
    USB_EPR(ep) = (_v & EPR_RW_MASK) | EPR_CTR_RX; \
} while(0)

/* Clear CTR_RX: write 0 to CTR_RX, write 1 to CTR_TX, preserve RW bits, zero toggles */
#define EP_CLEAR_CTR_RX(ep) do { \
    uint16_t _v = USB_EPR(ep); \
    USB_EPR(ep) = (_v & EPR_RW_MASK) | EPR_CTR_TX; \
} while(0)

/* ============================================================
   Packet Buffer SRAM layout
   BTABLE at offset 0 in SRAM — 8 entries × 8 bytes = 64 bytes

   Each entry (16-bit fields at 32-bit spacing in SRAM):
     +0: ADDR_TX
     +2: COUNT_TX
     +4: ADDR_RX
     +6: COUNT_RX

   Our buffer allocations:
     EP0 TX: SRAM offset 0x40, size 64
     EP0 RX: SRAM offset 0x80, size 64
     EP1 TX: SRAM offset 0xC0, size 512 (ISO)
   ============================================================ */
/*
 * STM32F103 USB SRAM — CORRECT LAYOUT
 *
 * Physical memory: 0x40006000 to 0x400063FF (1KB)
 * 16-bit wide, accessed at 32-bit boundaries (each 16-bit word at 4-byte aligned addr)
 *
 * BTABLE stores BYTE offsets into this SRAM.
 * USB peripheral accesses data at: USB_SRAM_BASE + ADDR_TX_value
 * But since it's 16-bit wide with 32-bit spacing:
 *   byte offset 0   → physical 0x40006000
 *   byte offset 2   → physical 0x40006004
 *   byte offset 4   → physical 0x40006008
 *   byte offset N   → physical 0x40006000 + (N/2)*4 = 0x40006000 + N*2
 *
 * So ADDR_TX = 0x40 means data starts at physical 0x40006000 + 0x40*2 = 0x40006080
 * And usb_sram_write(0x40,...) must also write to physical 0x40006080
 * Therefore: physical_address = USB_SRAM_BASE + logical_offset * 2  ← CORRECT
 *
 * BTABLE itself:
 *   EP0 entry at BTABLE+0: ADDR_TX(0x00), COUNT_TX(0x04), ADDR_RX(0x08), COUNT_RX(0x0C)
 *   EP1 entry at BTABLE+8: ADDR_TX(0x08)... wait — each BTABLE entry is 8 LOGICAL bytes
 *   Physical: entry 0 at offset 0x00, entry 1 at offset 0x10 (because 8 logical = 16 physical)
 *
 * Buffer layout (logical byte offsets = values stored in BTABLE):
 *   BTABLE:  0x00  (occupies logical 0x00..0x1F for up to 4 EPs)
 *   EP0 TX:  0x40  → physical 0x40006080
 *   EP0 RX:  0x80  → physical 0x40006100
 *   EP1 TX:  0xC0  → physical 0x40006180
 *   EP1 RX:  0x100 → physical 0x40006200
 *   EP2 TX:  0x140 → physical 0x40006280
 */

/* BTABLE field accessors
 * Each entry = 4 fields × 2 bytes logical = 4 fields × 4 bytes physical = 16 bytes physical per EP
 * EP n physical base = USB_SRAM_BASE + n*16
 * Fields at physical offsets: +0, +4, +8, +12
 */
#define BTABLE_ADDR_TX(ep)  (*(volatile uint16_t*)(USB_SRAM_BASE + (ep)*16u +  0u))
#define BTABLE_COUNT_TX(ep) (*(volatile uint16_t*)(USB_SRAM_BASE + (ep)*16u +  4u))
#define BTABLE_ADDR_RX(ep)  (*(volatile uint16_t*)(USB_SRAM_BASE + (ep)*16u +  8u))
#define BTABLE_COUNT_RX(ep) (*(volatile uint16_t*)(USB_SRAM_BASE + (ep)*16u + 12u))

/* Buffer logical byte offsets — written into BTABLE ADDR fields */
#define BTABLE_OFFSET       0x00u
#define EP0_TX_OFFSET       0x40u
#define EP0_RX_OFFSET       0x80u
#define EP1_TX_OFFSET       0xC0u
#define EP1_RX_OFFSET       0x100u
#define EP2_TX_OFFSET       0x140u

#define EP0_TX_SIZE         64u
#define EP0_RX_SIZE         64u
#define EP1_TX_SIZE         64u
#define EP1_RX_SIZE         64u

/*
 * usb_sram_write / usb_sram_read
 * logical_offset = value stored in BTABLE ADDR field
 * physical address = USB_SRAM_BASE + logical_offset * 2
 * (16-bit words at 32-bit spacing — factor of 2)
 */
static inline void usb_sram_write(uint32_t logi_off, const uint8_t *src, uint16_t len)
{
    volatile uint16_t *dst = (volatile uint16_t*)(USB_SRAM_BASE + logi_off * 2u);
    for (uint16_t i = 0; i < len; i += 2u) {
        uint16_t w = (uint16_t)src[i];
        if ((i + 1u) < len) w |= (uint16_t)((uint16_t)src[i + 1u] << 8u);
        *dst = w;
        dst += 2u;
    }
}

static inline void usb_sram_read(uint32_t logi_off, uint8_t *dst, uint16_t len)
{
    volatile uint16_t *src = (volatile uint16_t*)(USB_SRAM_BASE + logi_off * 2u);
    for (uint16_t i = 0; i < len; i += 2u) {
        uint16_t w = *src;
        src += 2u;
        dst[i] = (uint8_t)(w & 0xFFu);
        if ((i + 1u) < len) dst[i + 1u] = (uint8_t)(w >> 8u);
    }
}

#endif /* USB_REGS_H */