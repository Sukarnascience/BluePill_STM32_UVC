/*
 * usb_core.c — Composite CDC ACM + UVC (raw polling, no HAL PCD, no IRQ)
 *
 * One USB device → Windows sees:
 *   - "BeeSerial Device" (COMx)
 *   - "BeeCamera" (UVC, /dev/video0 on Linux)
 *
 * Interface layout:
 *   IAD 0: CDC (IF0=Control, IF1=Data)
 *     IF0: CDC Control  — EP2 IN Interrupt (notification, NAK only)
 *     IF1: CDC Data     — EP1 IN Bulk + EP1 OUT Bulk
 *   IAD 1: UVC (IF2=VC, IF3=VS)
 *     IF2: VideoControl — no endpoints
 *     IF3: VideoStream alt0 (zero-bw), alt1 — EP3 IN ISO 512B
 *
 * PMA layout (logical offsets):
 *   EP0 TX: 0x40   EP0 RX: 0x80
 *   EP1 TX: 0xC0   EP1 RX: 0x100   (CDC Bulk)
 *   EP2 TX: 0x140                   (CDC Interrupt IN)
 *   EP3 TX: 0x180                   (UVC ISO IN)
 */

#include "stm32f1xx_hal.h"
#include <string.h>
#include <stdint.h>

/* ---- USB registers ---- */
#define USB_BASE  0x40005C00UL
#define PMA_BASE  0x40006000UL

#define USB_CNTR   (*(volatile uint16_t*)(USB_BASE+0x40))
#define USB_ISTR   (*(volatile uint16_t*)(USB_BASE+0x44))
#define USB_DADDR  (*(volatile uint16_t*)(USB_BASE+0x4C))
#define USB_BTABLE (*(volatile uint16_t*)(USB_BASE+0x50))
#define USB_EPR(n) (*(volatile uint16_t*)(USB_BASE+(n)*4))

#define CNTR_FRES    (1u<<0)
#define CNTR_PDWN    (1u<<1)
#define CNTR_RESETM  (1u<<10)
#define CNTR_CTRM    (1u<<15)
#define ISTR_CTR     (1u<<15)
#define ISTR_RESET   (1u<<10)
#define ISTR_DIR     (1u<<4)
#define EP_CTR_RX    (1u<<15)
#define EP_DTOG_RX   (1u<<14)
#define EP_STAT_RX   (3u<<12)
#define EP_SETUP     (1u<<11)
#define EP_TYPE_MSK  (3u<<9)
#define EP_CTR_TX    (1u<<7)
#define EP_DTOG_TX   (1u<<6)
#define EP_STAT_TX   (3u<<4)
#define EP_TYPE_BULK (0u<<9)
#define EP_TYPE_CTRL (1u<<9)
#define EP_TYPE_ISO  (2u<<9)
#define EP_TYPE_INTR (3u<<9)
#define STAT_DIS  0u
#define STAT_STL  1u
#define STAT_NAK  2u
#define STAT_VLD  3u
#define EPR_RW    (EP_CTR_RX|EP_SETUP|EP_TYPE_MSK|EP_CTR_TX|0x0Fu)

/* ---- PMA helpers ---- */
#define PMA16(logi) ((volatile uint16_t*)(PMA_BASE+(uint32_t)(logi)*2u))
#define BT_ATXT(n)  (*PMA16((n)*8+0))
#define BT_CTXT(n)  (*PMA16((n)*8+2))
#define BT_ARXR(n)  (*PMA16((n)*8+4))
#define BT_CRXR(n)  (*PMA16((n)*8+6))

#define EP0TX 0x40u
#define EP0RX 0x80u
#define EP1TX 0xC0u
#define EP1RX 0x100u
#define EP2TX 0x140u
#define EP3TX 0x180u
#define RXBLK ((1u<<15)|(2u<<10))  /* BL_SIZE=1 NUM_BLOCK=2 → 64B */

static void pma_write(uint16_t logi, const uint8_t *s, uint16_t len)
{
    volatile uint16_t *d = PMA16(logi);
    for (uint16_t i=0; i<len; i+=2u) {
        uint16_t w = s[i];
        if (i+1u<len) w |= (uint16_t)(s[i+1u]<<8u);
        *d = w; d += 2u;
    }
}
static void pma_read(uint16_t logi, uint8_t *d, uint16_t len)
{
    volatile uint16_t *s = PMA16(logi);
    for (uint16_t i=0; i<len; i+=2u) {
        uint16_t w = *s; s += 2u;
        d[i] = (uint8_t)(w&0xFFu);
        if (i+1u<len) d[i+1u]=(uint8_t)(w>>8u);
    }
}

static void ep_stat_tx(uint8_t ep, uint16_t s)
{
    uint16_t v=USB_EPR(ep);
    uint16_t w=(v&EPR_RW)|EP_CTR_RX|EP_CTR_TX;
    w ^= ((v^(s<<4u))&EP_STAT_TX);
    USB_EPR(ep)=w;
}
static void ep_stat_rx(uint8_t ep, uint16_t s)
{
    uint16_t v=USB_EPR(ep);
    uint16_t w=(v&EPR_RW)|EP_CTR_RX|EP_CTR_TX;
    w ^= ((v^(s<<12u))&EP_STAT_RX);
    USB_EPR(ep)=w;
}
static void ep_clr_tx(uint8_t ep)
{ uint16_t v=USB_EPR(ep); USB_EPR(ep)=(v&EPR_RW&~EP_CTR_TX)|EP_CTR_RX; }
static void ep_clr_rx(uint8_t ep)
{ uint16_t v=USB_EPR(ep); USB_EPR(ep)=(v&EPR_RW&~EP_CTR_RX)|EP_CTR_TX; }

/* ---- UVC frame parameters ---- */
#define UVC_W       176u
#define UVC_H       144u
#define UVC_FPS     5u
#define UVC_ISO_SZ  512u
#define UVC_FRAME_SZ (UVC_W*UVC_H*2u)          /* YUY2 */
#define UVC_INTERVAL (10000000u/UVC_FPS)        /* 100ns units */

/* ---- Descriptors ---- */
static const uint8_t dev_desc[18] = {
    18,0x01,0x00,0x02,
    0xEF,0x02,0x01,        /* Misc/IAD composite */
    64,
    0xFE,0xCA,             /* VID=0xCAFE */
    0x02,0x40,             /* PID=0x4002 */
    0x00,0x01,
    0x01,0x02,0x03,0x01    /* iMfr=1 iProd=2 iSer=3 */
};

/* Build config descriptor as flat array */
/* Total = 9(cfg)+8(IAD-CDC)+9(IF0)+5+5+4+5(CDC fn)+7(EP2)+9(IF1)+7(EP1out)+7(EP1in)
         +8(IAD-UVC)+9(IF2-VC)+13(VC hdr)+9(IF3-VS alt0)+9(IF3-VS alt1)+7(EP3)
         +26(VS hdr)+27(frame)
   = 9+8+9+5+5+4+5+7+9+7+7 + 8+9+13+9+9+7+26+27 = 67+108 = wait let me count carefully */

/* CDC block = 8+9+5+5+4+5+7+9+7+7 = 66 */
/* UVC block = 8+9+13+9+9+7+26+27  = 117 — too big for FS, trim frame desc */
/* UVC minimal = 8(IAD)+9(VC IF)+13(VC hdr)+9(VS IF alt0)+9(VS IF alt1)+7(EP3)+25(VS input hdr)+26(frame) */
/* = 8+9+13+9+9+7+25+26 = 106 */
/* Total = 9 + 66 + 106 = 181 */

/* Pre-computed flat array — Python-verified walk OK, CFG_TOTAL=228 */
#define CFG_TOTAL 228u

static const uint8_t cfg_desc[CFG_TOTAL] = {
    0x09,0x02,0xE4,0x00,0x04,0x01,0x00,0x80,0xFA,0x08,0x0B,0x00,
    0x02,0x02,0x02,0x01,0x00,0x09,0x04,0x00,0x00,0x01,0x02,0x02,
    0x01,0x00,0x05,0x24,0x00,0x10,0x01,0x05,0x24,0x01,0x00,0x01,
    0x04,0x24,0x02,0x02,0x05,0x24,0x06,0x00,0x01,0x07,0x05,0x82,
    0x03,0x08,0x00,0xFF,0x09,0x04,0x01,0x00,0x02,0x0A,0x00,0x00,
    0x00,0x07,0x05,0x01,0x02,0x40,0x00,0x00,0x07,0x05,0x81,0x02,
    0x40,0x00,0x00,0x08,0x0B,0x02,0x02,0x0E,0x03,0x00,0x00,0x09,
    0x04,0x02,0x00,0x00,0x0E,0x01,0x00,0x00,0x0D,0x24,0x01,0x00,
    0x01,0x28,0x00,0x80,0x8D,0x5B,0x00,0x01,0x03,0x12,0x24,0x02,
    0x01,0x01,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,
    0x00,0x00,0x00,0x09,0x24,0x03,0x02,0x01,0x01,0x00,0x01,0x00,
    0x09,0x04,0x03,0x00,0x00,0x0E,0x02,0x00,0x00,0x09,0x04,0x03,
    0x01,0x01,0x0E,0x02,0x00,0x00,0x07,0x05,0x83,0x05,0x00,0x02,
    0x01,0x0E,0x24,0x01,0x01,0x47,0x00,0x83,0x00,0x02,0x00,0x00,
    0x00,0x01,0x00,0x1B,0x24,0x04,0x01,0x01,0x59,0x55,0x59,0x32,
    0x00,0x00,0x10,0x00,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71,
    0x10,0x01,0x00,0x00,0x00,0x00,0x1E,0x24,0x05,0x01,0x00,0xB0,
    0x00,0x90,0x00,0x00,0xF0,0x1E,0x00,0x00,0xF0,0x1E,0x00,0x00,
    0xC6,0x00,0x00,0x80,0x84,0x1E,0x00,0x01,0x80,0x84,0x1E,0x00,
};

/* String descriptors */
static const uint8_t s_lang[]   = {4,0x03,0x09,0x04};
static const uint8_t s_mfr[]    = {18,0x03,'B',0,'e',0,'e',0,'B',0,'o',0,'t',0,'i',0,'x',0};
static const uint8_t s_prod[]   = {26,0x03,
    'B',0,'e',0,'e',0,'C',0,'o',0,'m',0,'p',0,'o',0,'s',0,'i',0,'t',0,'e',0};
static const uint8_t s_serial[] = {8, 0x03,'0',0,'0',0,'1',0};

/* UVC probe/commit */
typedef struct __attribute__((packed)) {
    uint16_t bmHint;
    uint8_t  bFormatIdx;
    uint8_t  bFrameIdx;
    uint32_t dwFrameInterval;
    uint16_t wKeyFrameRate;
    uint16_t wPFrameRate;
    uint16_t wCompQuality;
    uint16_t wCompWindowSize;
    uint16_t wDelay;
    uint32_t dwMaxVideoFrameSize;
    uint32_t dwMaxPayloadTransferSize;
} uvc_probe_t;

static uvc_probe_t uvc_probe = {
    .bmHint                  = 0x0001,
    .bFormatIdx              = 1,
    .bFrameIdx               = 1,
    .dwFrameInterval         = UVC_INTERVAL,
    .dwMaxVideoFrameSize     = UVC_FRAME_SZ,
    .dwMaxPayloadTransferSize= UVC_ISO_SZ,
};

/* ---- State ---- */
static uint8_t  pending_addr = 0;
static uint8_t  streaming    = 0;
static uint8_t  setup_pkt[8];
static uint8_t  rx_buf[64];

static const uint8_t *ep0_ptr  = NULL;
static uint16_t       ep0_len  = 0;
static uint16_t       ep0_sent = 0;

/* UVC frame generator — YUY2 grayscale ramp */
static uint32_t frame_pos  = 0;
static uint8_t  frame_id   = 0;
static uint32_t frame_byte = 0;

static void ep0_queue(const uint8_t *buf, uint16_t len, uint16_t wlen)
{
    if (len>wlen) len=wlen;
    ep0_ptr=buf; ep0_len=len; ep0_sent=0;
    uint16_t c=len>64u?64u:len;
    pma_write(EP0TX,buf,c);
    BT_CTXT(0)=c; ep_stat_tx(0,STAT_VLD); ep0_sent=c;
}
static void ep0_next(void)
{
    if (ep0_sent<ep0_len) {
        uint16_t r=ep0_len-ep0_sent, c=r>64u?64u:r;
        pma_write(EP0TX,ep0_ptr+ep0_sent,c);
        BT_CTXT(0)=c; ep_stat_tx(0,STAT_VLD); ep0_sent+=c;
    }
}
static void ep0_zlp(void)
{ ep0_len=0;ep0_sent=0;BT_CTXT(0)=0;ep_stat_tx(0,STAT_VLD); }

/* ---- SETUP handler ---- */
static void handle_setup(void)
{
    pma_read(EP0RX,setup_pkt,8);
    uint8_t  bmRT=setup_pkt[0], bReq=setup_pkt[1];
    uint16_t wVal=(uint16_t)setup_pkt[2]|((uint16_t)setup_pkt[3]<<8);
    uint16_t wLen=(uint16_t)setup_pkt[6]|((uint16_t)setup_pkt[7]<<8);

    /* GET_DESCRIPTOR */
    if (bmRT==0x80u&&bReq==0x06u) {
        switch (wVal>>8) {
            case 1: ep0_queue(dev_desc,sizeof(dev_desc),wLen); return;
            case 2: ep0_queue(cfg_desc,CFG_TOTAL,wLen);        return;
            case 3:
                switch (wVal&0xFF) {
                    case 0: ep0_queue(s_lang,  sizeof(s_lang),  wLen); return;
                    case 1: ep0_queue(s_mfr,   sizeof(s_mfr),   wLen); return;
                    case 2: ep0_queue(s_prod,  sizeof(s_prod),  wLen); return;
                    case 3: ep0_queue(s_serial,sizeof(s_serial),wLen); return;
                }
                break;
        }
        ep_stat_tx(0,STAT_STL); return;
    }

    /* SET_ADDRESS */
    if (bmRT==0x00u&&bReq==0x05u) {
        pending_addr=(uint8_t)(wVal&0x7Fu); ep0_zlp(); return;
    }

    /* SET_CONFIGURATION */
    if (bmRT==0x00u&&bReq==0x09u) {
        /* EP1 Bulk CDC */
        USB_EPR(1)=(uint16_t)(EP_TYPE_BULK|0x01u);
        BT_ATXT(1)=EP1TX; BT_CTXT(1)=0;
        BT_ARXR(1)=EP1RX; BT_CRXR(1)=RXBLK;
        ep_stat_tx(1,STAT_NAK); ep_stat_rx(1,STAT_VLD);
        /* EP2 Interrupt CDC notification */
        USB_EPR(2)=(uint16_t)(EP_TYPE_INTR|0x82u);
        BT_ATXT(2)=EP2TX; BT_CTXT(2)=0;
        ep_stat_tx(2,STAT_NAK);
        /* EP3 ISO UVC — starts in alt0 (no data) */
        ep_stat_tx(3,STAT_DIS);
        ep0_zlp(); return;
    }

    /* SET_INTERFACE — alt1 enables UVC streaming */
    if (bmRT==0x01u&&bReq==0x0Bu) {
        uint16_t alt=(uint16_t)setup_pkt[2];
        uint16_t iface=(uint16_t)setup_pkt[4];
        if (iface==3u) {
            if (alt==1u) {
                /* Open ISO endpoint */
                USB_EPR(3)=(uint16_t)(EP_TYPE_ISO|0x03u);
                BT_ATXT(3)=EP3TX; BT_CTXT(3)=0;
                ep_stat_tx(3,STAT_VLD);
                streaming=1; frame_pos=0; frame_byte=0;
            } else {
                ep_stat_tx(3,STAT_DIS);
                streaming=0;
            }
        }
        ep0_zlp(); return;
    }

    /* SET_INTERFACE for other interfaces */
    if (bmRT==0x01u&&bReq==0x0Bu) { ep0_zlp(); return; }

    /* CDC GET_LINE_CODING */
    if (bmRT==0xA1u&&bReq==0x21u) {
        static uint8_t lc[7]={0x00,0xC2,0x01,0x00,0x00,0x00,0x08}; /* 115200 */
        ep0_queue(lc,7,wLen); return;
    }

    /* CDC SET_LINE_CODING / SET_CONTROL_LINE_STATE */
    if (bmRT==0x21u&&(bReq==0x20u||bReq==0x22u)) {
        if (bReq==0x20u) { BT_CRXR(0)=RXBLK; ep_stat_rx(0,STAT_VLD); }
        ep0_zlp(); return;
    }

    /* UVC GET/SET probe-commit */
    if ((bmRT==0xA1u||bmRT==0x21u) && (bReq==0x81u||bReq==0x01u||bReq==0x82u||bReq==0x02u)) {
        if (bmRT==0xA1u) {
            ep0_queue((uint8_t*)&uvc_probe,sizeof(uvc_probe),wLen);
        } else {
            BT_CRXR(0)=RXBLK; ep_stat_rx(0,STAT_VLD);
            ep0_zlp();
        }
        return;
    }

    ep_stat_tx(0,STAT_STL); ep_stat_rx(0,STAT_STL);
}

/* ---- Reset ---- */
static void handle_reset(void)
{
    pending_addr=0; streaming=0;
    USB_BTABLE=0;
    BT_ATXT(0)=EP0TX; BT_CTXT(0)=0;
    BT_ARXR(0)=EP0RX; BT_CRXR(0)=RXBLK;
    USB_EPR(0)=(uint16_t)(EP_TYPE_CTRL|0x00u);
    ep_stat_tx(0,STAT_NAK); ep_stat_rx(0,STAT_VLD);
    USB_DADDR=0x80u;
}

/* ---- UVC payload builder ---- */
/* Fills buf with UVC payload header + YUY2 data, returns byte count */
static uint16_t build_uvc_payload(uint8_t *buf, uint16_t max)
{
    uint16_t total_sz = UVC_FRAME_SZ;
    uint8_t hdr_len = 2u;
    uint16_t data_max = max - hdr_len;

    /* UVC payload header: HLE, BFH */
    buf[0] = hdr_len;
    buf[1] = (uint8_t)(0x00u | (frame_id & 0x01u)); /* FID */

    if (frame_byte >= total_sz) {
        /* End of frame — send header-only with EOF bit */
        buf[1] |= 0x02u; /* EOF */
        frame_id ^= 1u;
        frame_byte = 0;
        BT_CTXT(3) = hdr_len;
        pma_write(EP3TX, buf, hdr_len);
        return hdr_len;
    }

    /* Fill YUY2 data — grayscale ramp */
    uint16_t n = 0;
    while (n < data_max && frame_byte < total_sz) {
        uint32_t pixel = frame_byte / 2u;
        uint8_t  y = (uint8_t)((pixel + frame_pos) & 0xFFu);
        if ((frame_byte & 1u) == 0u) {
            buf[hdr_len + n] = y;     /* Y0 */
        } else {
            buf[hdr_len + n] = 0x80u; /* U or V — neutral gray */
        }
        n++; frame_byte++;
    }

    pma_write(EP3TX, buf, hdr_len + n);
    BT_CTXT(3) = hdr_len + n;
    return hdr_len + n;
}

/* ---- Poll ---- */
static uint8_t uvc_buf[UVC_ISO_SZ];

void usb_core_poll(void)
{
    uint16_t istr = USB_ISTR;

    if (istr & ISTR_RESET) {
        USB_ISTR=(uint16_t)~ISTR_RESET;
        handle_reset(); return;
    }

    if (istr & ISTR_CTR) {
        uint8_t ep  = (uint8_t)(istr&0x0Fu);
        uint8_t dir = (istr&ISTR_DIR)?1u:0u;

        if (ep==0u) {
            uint16_t epr=USB_EPR(0);
            if (epr&EP_SETUP) {
                ep_clr_rx(0); handle_setup(); ep_stat_rx(0,STAT_VLD);
            } else if (dir) {
                ep_clr_rx(0); ep_stat_rx(0,STAT_VLD);
            } else {
                ep_clr_tx(0);
                if (pending_addr) { USB_DADDR=(uint16_t)(0x80u|pending_addr); pending_addr=0; }
                ep0_next();
            }
        } else if (ep==1u) {
            if (dir) {
                ep_clr_rx(1);
                uint16_t cnt=BT_CRXR(1)&0x3FFu;
                if (cnt&&cnt<=64u) {
                    pma_read(EP1RX,rx_buf,cnt);
                    pma_write(EP1TX,rx_buf,cnt);
                    BT_CTXT(1)=cnt; ep_stat_tx(1,STAT_VLD);
                }
                ep_stat_rx(1,STAT_VLD);
            } else { ep_clr_tx(1); }
        } else if (ep==2u) {
            ep_clr_tx(2);
        } else if (ep==3u) {
            /* ISO IN complete — queue next payload */
            ep_clr_tx(3);
            if (streaming) {
                build_uvc_payload(uvc_buf, UVC_ISO_SZ);
                ep_stat_tx(3, STAT_VLD);
                frame_pos++;
            }
        }
        USB_ISTR=0;
    }
}

/* ---- Init ---- */
void usb_core_init(void)
{
    __HAL_RCC_USB_CLK_ENABLE();
    USB_CNTR=CNTR_FRES|CNTR_PDWN;
    HAL_Delay(5);
    USB_CNTR=CNTR_FRES;
    HAL_Delay(5);
    USB_CNTR=0;
    USB_ISTR=0;
    handle_reset();
    USB_CNTR=CNTR_RESETM|CNTR_CTRM;

    GPIO_InitTypeDef g={0};
    g.Pin=GPIO_PIN_12; g.Mode=GPIO_MODE_INPUT; g.Pull=GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA,&g);
}