/*
 * usb_core.c — Minimal raw USB CDC, polling only, no IRQ
 *
 * Deliberately simple — no HAL_PCD, no interrupts.
 * Just polls ISTR in main loop.
 *
 * PMA layout (logical byte offsets written to BTABLE):
 *   BTABLE at physical 0x40006000 (REG_BTABLE=0)
 *   EP0 TX: logical 0x40  physical 0x40006080
 *   EP0 RX: logical 0x80  physical 0x40006100
 *   EP1 TX: logical 0xC0  physical 0x40006180
 *   EP1 RX: logical 0x100 physical 0x40006200
 */

#include "stm32f1xx_hal.h"
#include <string.h>
#include <stdint.h>

/* ---- USB register base ---- */
#define USB_BASE_ADDR   0x40005C00UL
#define PMA_BASE        0x40006000UL

/* USB registers */
#define USB_CNTR    (*(volatile uint16_t*)(USB_BASE_ADDR + 0x40))
#define USB_ISTR    (*(volatile uint16_t*)(USB_BASE_ADDR + 0x44))
#define USB_DADDR   (*(volatile uint16_t*)(USB_BASE_ADDR + 0x4C))
#define USB_BTABLE  (*(volatile uint16_t*)(USB_BASE_ADDR + 0x50))
#define USB_EPR(n)  (*(volatile uint16_t*)(USB_BASE_ADDR + (n)*4))

/* CNTR bits */
#define CNTR_FRES   (1u<<0)
#define CNTR_PDWN   (1u<<1)
#define CNTR_RESETM (1u<<10)
#define CNTR_CTRM   (1u<<15)

/* ISTR bits */
#define ISTR_CTR    (1u<<15)
#define ISTR_RESET  (1u<<10)
#define ISTR_DIR    (1u<<4)

/* EPR bits */
#define EP_CTR_RX   (1u<<15)
#define EP_DTOG_RX  (1u<<14)
#define EP_STAT_RX  (3u<<12)
#define EP_SETUP    (1u<<11)
#define EP_TYPE     (3u<<9)
#define EP_CTR_TX   (1u<<7)
#define EP_DTOG_TX  (1u<<6)
#define EP_STAT_TX  (3u<<4)

#define EP_TYPE_BULK    (0u<<9)
#define EP_TYPE_CTRL    (1u<<9)
#define EP_TYPE_ISO     (2u<<9)
#define EP_TYPE_INTR    (3u<<9)

#define STAT_DISABLED   0u
#define STAT_STALL      1u
#define STAT_NAK        2u
#define STAT_VALID      3u

/* RW-safe bits mask (bits that are plain R/W, not toggle) */
#define EPR_NOTOG_MASK  (EP_CTR_RX | EP_SETUP | EP_TYPE | EP_CTR_TX | 0x0Fu)

/* ---- PMA access ----
 * PMA is 16-bit wide on 32-bit bus.
 * logical offset L → physical address = PMA_BASE + L*2
 * BTABLE stores logical offsets.
 */
#define PMA16(logi)  ((volatile uint16_t*)(PMA_BASE + (uint32_t)(logi)*2u))

/* BTABLE fields for EP n (BTABLE at logical 0, physical 0x40006000) */
#define BTABLE_ADDR_TX(n)  (*PMA16((n)*8 + 0))
#define BTABLE_CNT_TX(n)   (*PMA16((n)*8 + 2))
#define BTABLE_ADDR_RX(n)  (*PMA16((n)*8 + 4))
#define BTABLE_CNT_RX(n)   (*PMA16((n)*8 + 6))

/* Buffer logical offsets */
#define EP0TX  0x40u
#define EP0RX  0x80u
#define EP1TX  0xC0u
#define EP1RX  0x100u

static void pma_write(uint16_t logi, const uint8_t *src, uint16_t len)
{
    volatile uint16_t *dst = PMA16(logi);
    for (uint16_t i = 0; i < len; i += 2u) {
        uint16_t w = src[i];
        if (i+1u < len) w |= (uint16_t)(src[i+1u] << 8u);
        *dst = w;
        dst += 2u; /* skip to next 32-bit slot */
    }
}

static void pma_read(uint16_t logi, uint8_t *dst, uint16_t len)
{
    volatile uint16_t *src = PMA16(logi);
    for (uint16_t i = 0; i < len; i += 2u) {
        uint16_t w = *src; src += 2u;
        dst[i] = (uint8_t)(w & 0xFFu);
        if (i+1u < len) dst[i+1u] = (uint8_t)(w >> 8u);
    }
}

/* ---- EPR toggle helpers ---- */
static void ep_set_stat_tx(uint8_t ep, uint16_t stat)
{
    uint16_t v = USB_EPR(ep);
    /* Keep CTR bits, clear toggle bits we don't want to flip */
    uint16_t w = (v & EPR_NOTOG_MASK) | EP_CTR_RX | EP_CTR_TX;
    /* XOR only STAT_TX field to reach desired value */
    w ^= ((v ^ (stat << 4u)) & EP_STAT_TX);
    USB_EPR(ep) = w;
}

static void ep_set_stat_rx(uint8_t ep, uint16_t stat)
{
    uint16_t v = USB_EPR(ep);
    uint16_t w = (v & EPR_NOTOG_MASK) | EP_CTR_RX | EP_CTR_TX;
    w ^= ((v ^ (stat << 12u)) & EP_STAT_RX);
    USB_EPR(ep) = w;
}

static void ep_clr_ctr_tx(uint8_t ep)
{
    uint16_t v = USB_EPR(ep);
    /* Write 0 to CTR_TX, preserve CTR_RX, preserve RW bits, don't toggle */
    USB_EPR(ep) = (v & EPR_NOTOG_MASK & ~EP_CTR_TX) | EP_CTR_RX;
}

static void ep_clr_ctr_rx(uint8_t ep)
{
    uint16_t v = USB_EPR(ep);
    USB_EPR(ep) = (v & EPR_NOTOG_MASK & ~EP_CTR_RX) | EP_CTR_TX;
}

/* ---- Descriptors ---- */
static const uint8_t dev_desc[18] = {
    18,0x01,0x00,0x02,
    0x02,0x00,0x00,64,
    0xFE,0xCA, 0x01,0x40,
    0x00,0x01,
    0x01,0x02,0x00,0x01
};

#define CFG_LEN 67
static const uint8_t cfg_desc[CFG_LEN] = {
    9,0x02,CFG_LEN,0,2,1,0,0x80,50,
    9,0x04,0,0,1,0x02,0x02,0x01,0,
    5,0x24,0x00,0x10,0x01,
    5,0x24,0x01,0x00,0x01,
    4,0x24,0x02,0x02,
    5,0x24,0x06,0x00,0x01,
    7,0x05,0x82,0x03,8,0,255,
    9,0x04,1,0,2,0x0A,0x00,0x00,0,
    7,0x05,0x01,0x02,64,0,0,
    7,0x05,0x81,0x02,64,0,0,
};

static const uint8_t lang_desc[] = {4,0x03,0x09,0x04};
static const uint8_t mfr_desc[]  = {18,0x03,'B',0,'e',0,'e',0,'B',0,'o',0,'t',0,'i',0,'x',0};
static const uint8_t prod_desc[] = {18,0x03,'B',0,'P',0,'C',0,'D',0,'C',0,' ',0,'v',0,'1',0};
static uint8_t line_coding[7]    = {0x80,0x25,0x00,0x00,0x00,0x00,0x08};

/* ---- State ---- */
static uint8_t  pending_addr = 0;
static uint8_t  setup_pkt[8];
static uint8_t  rx_buf[64];

static const uint8_t *ep0_tx_ptr = NULL;
static uint16_t       ep0_tx_len = 0;
static uint16_t       ep0_tx_done = 0;

static void ep0_queue(const uint8_t *buf, uint16_t len, uint16_t wlen)
{
    if (len > wlen) len = wlen;
    ep0_tx_ptr  = buf;
    ep0_tx_len  = len;
    ep0_tx_done = 0;
    uint16_t chunk = len > 64u ? 64u : len;
    pma_write(EP0TX, buf, chunk);
    BTABLE_CNT_TX(0) = chunk;
    ep_set_stat_tx(0, STAT_VALID);
    ep0_tx_done = chunk;
}

static void ep0_tx_next(void)
{
    if (ep0_tx_done < ep0_tx_len) {
        uint16_t rem   = ep0_tx_len - ep0_tx_done;
        uint16_t chunk = rem > 64u ? 64u : rem;
        pma_write(EP0TX, ep0_tx_ptr + ep0_tx_done, chunk);
        BTABLE_CNT_TX(0) = chunk;
        ep_set_stat_tx(0, STAT_VALID);
        ep0_tx_done += chunk;
    }
}

static void ep0_zlp(void)
{
    ep0_tx_len = 0; ep0_tx_done = 0;
    BTABLE_CNT_TX(0) = 0;
    ep_set_stat_tx(0, STAT_VALID);
}

/* ---- SETUP ---- */
static void handle_setup(void)
{
    pma_read(EP0RX, setup_pkt, 8);
    uint8_t  bmRT = setup_pkt[0];
    uint8_t  bReq = setup_pkt[1];
    uint16_t wVal = (uint16_t)setup_pkt[2] | ((uint16_t)setup_pkt[3]<<8);
    uint16_t wLen = (uint16_t)setup_pkt[6] | ((uint16_t)setup_pkt[7]<<8);

    /* GET_DESCRIPTOR */
    if (bmRT==0x80u && bReq==0x06u) {
        switch (wVal>>8) {
            case 1: ep0_queue(dev_desc, sizeof(dev_desc), wLen); return;
            case 2: ep0_queue(cfg_desc, CFG_LEN,          wLen); return;
            case 3:
                switch (wVal&0xFF) {
                    case 0: ep0_queue(lang_desc, sizeof(lang_desc), wLen); return;
                    case 1: ep0_queue(mfr_desc,  sizeof(mfr_desc),  wLen); return;
                    case 2: ep0_queue(prod_desc, sizeof(prod_desc), wLen); return;
                }
                break;
        }
        ep_set_stat_tx(0, STAT_STALL);
        return;
    }

    /* SET_ADDRESS */
    if (bmRT==0x00u && bReq==0x05u) {
        pending_addr = (uint8_t)(wVal & 0x7Fu);
        ep0_zlp(); return;
    }

    /* SET_CONFIGURATION */
    if (bmRT==0x00u && bReq==0x09u) {
        /* EP1 Bulk */
        USB_EPR(1) = (uint16_t)(EP_TYPE_BULK | 0x01u);
        BTABLE_ADDR_TX(1) = EP1TX;
        BTABLE_CNT_TX(1)  = 0;
        BTABLE_ADDR_RX(1) = EP1RX;
        BTABLE_CNT_RX(1)  = (1u<<15)|(2u<<10); /* BL_SIZE=1, NBLOCKS=2 → 64B */
        ep_set_stat_tx(1, STAT_NAK);
        ep_set_stat_rx(1, STAT_VALID);
        /* EP2 Interrupt IN (CDC notification) */
        USB_EPR(2) = (uint16_t)(EP_TYPE_INTR | 0x82u);
        BTABLE_ADDR_TX(2) = 0x140u;
        BTABLE_CNT_TX(2)  = 0;
        ep_set_stat_tx(2, STAT_NAK);
        ep0_zlp(); return;
    }

    /* SET_INTERFACE */
    if (bmRT==0x01u && bReq==0x0Bu) { ep0_zlp(); return; }

    /* CDC GET_LINE_CODING */
    if (bmRT==0xA1u && bReq==0x21u) {
        ep0_queue(line_coding, sizeof(line_coding), wLen); return;
    }

    /* CDC SET_LINE_CODING */
    if (bmRT==0x21u && bReq==0x20u) {
        BTABLE_CNT_RX(0) = (1u<<15)|(2u<<10);
        ep_set_stat_rx(0, STAT_VALID);
        ep0_zlp(); return;
    }

    /* CDC SET_CONTROL_LINE_STATE */
    if (bmRT==0x21u && bReq==0x22u) { ep0_zlp(); return; }

    ep_set_stat_tx(0, STAT_STALL);
    ep_set_stat_rx(0, STAT_STALL);
}

/* ---- Reset handler ---- */
static void handle_reset(void)
{
    pending_addr = 0;

    USB_BTABLE = 0;

    BTABLE_ADDR_TX(0) = EP0TX;
    BTABLE_CNT_TX(0)  = 0;
    BTABLE_ADDR_RX(0) = EP0RX;
    BTABLE_CNT_RX(0)  = (1u<<15)|(2u<<10); /* 64 bytes */

    USB_EPR(0) = (uint16_t)(EP_TYPE_CTRL | 0x00u);
    ep_set_stat_tx(0, STAT_NAK);
    ep_set_stat_rx(0, STAT_VALID);

    USB_DADDR = 0x80u; /* EF=1, ADDR=0 */
}

/* ---- Poll ---- */
void usb_core_poll(void)
{
    uint16_t istr = USB_ISTR;

    if (istr & ISTR_RESET) {
        USB_ISTR = (uint16_t)~ISTR_RESET;
        handle_reset();
        return;
    }

    if (!(istr & ISTR_CTR)) return;

    uint8_t ep  = (uint8_t)(istr & 0x0Fu);
    uint8_t dir = (istr & ISTR_DIR) ? 1u : 0u;

    if (ep == 0u) {
        uint16_t epr = USB_EPR(0);
        if (epr & EP_SETUP) {
            ep_clr_ctr_rx(0);
            handle_setup();
            ep_set_stat_rx(0, STAT_VALID);
        } else if (dir) {
            ep_clr_ctr_rx(0);
            ep_set_stat_rx(0, STAT_VALID);
        } else {
            ep_clr_ctr_tx(0);
            if (pending_addr) {
                USB_DADDR = (uint16_t)(0x80u | pending_addr);
                pending_addr = 0;
            }
            ep0_tx_next();
        }
    } else if (ep == 1u) {
        if (dir) {
            ep_clr_ctr_rx(1);
            uint16_t cnt = BTABLE_CNT_RX(1) & 0x3FFu;
            if (cnt && cnt <= 64u) {
                pma_read(EP1RX, rx_buf, cnt);
                pma_write(EP1TX, rx_buf, cnt);
                BTABLE_CNT_TX(1) = cnt;
                ep_set_stat_tx(1, STAT_VALID);
            }
            ep_set_stat_rx(1, STAT_VALID);
        } else {
            ep_clr_ctr_tx(1);
        }
    } else if (ep == 2u) {
        ep_clr_ctr_tx(2);
    }

    USB_ISTR = 0;
}

/* ---- Init ---- */
void usb_core_init(void)
{
    __HAL_RCC_USB_CLK_ENABLE();

    USB_CNTR = CNTR_FRES | CNTR_PDWN;
    HAL_Delay(5);
    USB_CNTR = CNTR_FRES;
    HAL_Delay(5);
    USB_CNTR = 0;
    USB_ISTR = 0;

    handle_reset();

    USB_CNTR = CNTR_RESETM | CNTR_CTRM;

    /* Release D+ — host sees connect */
    GPIO_InitTypeDef g = {0};
    g.Pin  = GPIO_PIN_12;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &g);
}