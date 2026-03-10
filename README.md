# STM32F103C8T6 Blue Pill — UVC Camera (Test Pattern)
### PlatformIO + libopencm3 | No HAL | No CubeMX | ST-Link flash

---

## What Is This Project?

This project turns a **STM32F103C8T6 "Blue Pill"** board into a USB Video Class (UVC) camera
that streams a grayscale test pattern. When plugged into any PC:

- **Windows 11** → plays camera connect sound, shows as "USB Video Device"
- **Linux** → creates `/dev/video0`, visible in `v4l2-ctl --list-devices`
- **macOS** → appears in any camera app

No custom driver needed. UVC is natively supported by every modern OS.

> Goal: Understand how UVC works at the protocol level before moving to higher-end STM32 H-series hardware.

---

## Board Specs — STM32F103C8T6

| Property | Value | Notes |
|---|---|---|
| Core | ARM Cortex-M3 | No FPU |
| Clock | 72 MHz max | Via PLL from 8MHz HSE |
| Flash | 64 KB | Some chips have 128 KB (C8T6 variant) |
| RAM | 20 KB | Biggest constraint for UVC |
| USB | Full-Speed (FS) 12 Mbps | Built-in, no external PHY needed |
| USB Pins | PA11 (D−), PA12 (D+) | Fixed, cannot remap |
| USB Buffer SRAM | 512 bytes | Shared across all endpoints |
| Package | LQFP48 | "Blue Pill" dev board |
| Operating Voltage | 3.3V | USB D+ pull-up trick needed |

---

## Why libopencm3 and NOT Arduino/HAL?

| Framework | USB Control | Code Size | UVC Support | Learning Value |
|---|---|---|---|---|
| STM32duino (Arduino) | Hidden behind abstraction | Large | No native UVC | Low |
| STM32 HAL (CubeMX) | Partial control | Very large | No native UVC | Medium |
| **libopencm3** | **Full register control** | **Minimal** | **You build it** | **High** |
| TinyUSB | Good abstraction | Medium | Yes, but PIO+F103 has issues | Medium |

For learning UVC, **you want to see every byte**. libopencm3 lets you do that.

---

## How UVC Works — Full Theory

### 1. USB Enumeration (The "Who Are You?" Phase)

When you plug in, USB resets the device, then the host asks a series of standard questions.
Your firmware answers with hardcoded **descriptor** byte arrays stored in Flash.

```
Host                          STM32
 |                               |
 |------ USB Reset ------------->|
 |<----- Device Ready -----------|
 |                               |
 |------ GET_DESCRIPTOR -------->|  "Give me Device Descriptor"
 |<----- 18 bytes ---------------|  VID, PID, USB version, class
 |                               |
 |------ GET_DESCRIPTOR -------->|  "Give me Configuration Descriptor"
 |<----- N bytes ----------------|  All interfaces, endpoints, class specifics
 |                               |
 |------ SET_CONFIGURATION ----->|  "I accept, use config #1"
 |<----- ACK --------------------|
 |                               |
 |  [OS loads built-in UVC driver — no install needed]
```

### 2. UVC Descriptor Hierarchy

This is the complete tree of descriptors your firmware must provide:

```
Device Descriptor
│   bDeviceClass    = 0xEF   (Miscellaneous Device)
│   bDeviceSubClass = 0x02
│   bDeviceProtocol = 0x01   (IAD composite device)
│   idVendor        = 0xCAFE (fake, for development)
│   idProduct       = 0x4010
│   bcdUSB          = 0x0200 (USB 2.0)
│
└── Configuration Descriptor
    │
    └── Interface Association Descriptor (IAD)
        │   bFirstInterface    = 0
        │   bInterfaceCount    = 2
        │   bFunctionClass     = 0x0E  (Video)
        │   bFunctionSubClass  = 0x03  (Video Interface Collection)
        │   bFunctionProtocol  = 0x00
        │
        ├── Interface 0: VideoControl (VC)
        │   │   bInterfaceClass    = 0x0E (Video)
        │   │   bInterfaceSubClass = 0x01 (VideoControl)
        │   │
        │   ├── VC Header Descriptor
        │   │     UVC version = 0x0110 (UVC 1.1)
        │   │     Total length of class-specific descriptors
        │   │
        │   ├── Input Terminal Descriptor (Camera)
        │   │     bTerminalID   = 1
        │   │     wTerminalType = 0x0201 (ITT_CAMERA)
        │   │     "I have a camera sensor"
        │   │
        │   └── Output Terminal Descriptor
        │         bTerminalID   = 2
        │         wTerminalType = 0x0101 (TT_STREAMING)
        │         bSourceID     = 1  (connected to Input Terminal)
        │         "Output goes to USB"
        │
        └── Interface 1: VideoStreaming (VS)
            │   bInterfaceClass    = 0x0E (Video)
            │   bInterfaceSubClass = 0x02 (VideoStreaming)
            │
            ├── VS Input Header Descriptor
            │     bNumFormats = 1
            │     bEndpointAddress = 0x81 (EP1 IN)
            │
            ├── VS Format Descriptor — Uncompressed (YUY2)
            │     bFormatIndex = 1
            │     guidFormat   = {YUY2 GUID}
            │     "I send YUY2 (YUYV) packed pixels"
            │
            ├── VS Frame Descriptor — 176x144
            │     bFrameIndex         = 1
            │     wWidth              = 176
            │     wHeight             = 144
            │     dwMinBitRate        = calculated
            │     dwMaxBitRate        = calculated
            │     dwMaxVideoFrameBufferSize = 176*144*2 = 50688
            │     dwDefaultFrameInterval   = 10000000/5 = 2000000 (5fps in 100ns units)
            │
            └── Alternate Setting 1 (active streaming)
                  └── Endpoint 0x81
                        bmAttributes     = 0x01 (Isochronous)
                        wMaxPacketSize   = 512  (FS max for iso = 1023, we use 512)
                        bInterval        = 1    (every USB frame = every 1ms)
```

### 3. UVC Probe & Commit Negotiation

After enumeration, before any frame data flows, the host and device negotiate
stream parameters using **control transfers** on the VideoStreaming interface:

```c
// Host sends this to ask: "Can you stream with these params?"
struct uvc_streaming_control {
    uint16_t bmHint;                    // which fields are fixed
    uint8_t  bFormatIndex;              // 1 = YUY2
    uint8_t  bFrameIndex;               // 1 = 176x144
    uint32_t dwFrameInterval;           // 2000000 = 5fps (100ns units)
    uint16_t wKeyFrameRate;             // 0 = not applicable
    uint16_t wPFrameRate;               // 0
    uint16_t wCompQuality;              // 0
    uint16_t wCompWindowSize;           // 0
    uint16_t wDelay;                    // 0
    uint32_t dwMaxVideoFrameSize;       // 176*144*2 = 50688
    uint32_t dwMaxPayloadTransferSize;  // 512 (our EP packet size)
};

// Flow:
// 1. Host: SET_CUR VS_PROBE_CONTROL   → "Can you do this?"
// 2. STM32: stores it, clamps to what we support
// 3. Host: GET_CUR VS_PROBE_CONTROL   → "What do you actually support?"
// 4. STM32: returns clamped values
// 5. Host: SET_CUR VS_COMMIT_CONTROL  → "OK, locked in, start streaming"
// 6. STM32: activates isochronous endpoint
```

### 4. UVC Payload Format — The Frame ID Bit

Every chunk of video data sent over USB must have a **2-byte UVC payload header**:

```
Byte 0: bmHeaderInfo
        ┌─────────────────────────────────────────┐
        │ Bit 7 │ Bit 6 │ ... │ Bit 1 │  Bit 0   │
        │  ERR  │  STI  │ ... │  EOF  │  FID     │
        └─────────────────────────────────────────┘
        
        FID (Frame ID) — MOST IMPORTANT BIT
            Toggle 0→1→0→1 every time a NEW frame starts
            OS uses this to detect frame boundaries
            Get this wrong → OS shows corrupted/frozen video
            
        EOF (End of Frame)
            Set to 1 on the LAST packet of each frame
            
        ERR — set if payload has errors (we never set this)

Byte 1: bHeaderLength = 2 (we use minimal header, no timestamps)

Bytes 2..N: Raw pixel data (YUY2 bytes)
```

**Example — streaming frame 1 then frame 2:**

```
Packet 1 (frame 1, not last):  [0x80, 0x02, Y,U,Y,V, Y,U,Y,V, ...]  FID=0, EOF=0
Packet 2 (frame 1, last):      [0x82, 0x02, Y,U,Y,V, Y,U,Y,V, ...]  FID=0, EOF=1
Packet 3 (frame 2, not last):  [0x81, 0x02, Y,U,Y,V, Y,U,Y,V, ...]  FID=1, EOF=0  ← FID toggled!
Packet 4 (frame 2, last):      [0x83, 0x02, Y,U,Y,V, Y,U,Y,V, ...]  FID=1, EOF=1
```

### 5. YUY2 Pixel Format

YUY2 (also called YUYV) is the most universally supported raw video format.
Every UVC-capable OS supports it natively. It encodes 2 pixels in 4 bytes:

```
4 bytes → 2 pixels

Byte 0: Y0  (luma for pixel 0)
Byte 1: U   (chroma blue, shared by pixels 0 and 1)
Byte 2: Y1  (luma for pixel 1)
Byte 3: V   (chroma red, shared by pixels 0 and 1)

For pure grayscale: U=128, V=128 (neutral chroma)
For greyscale bars: vary Y, keep U=128, V=128
For color:         set U and V to color values
```

Total frame size for 176×144 YUY2:
```
176 × 144 × 2 bytes/pixel = 50,688 bytes
```

### 6. Isochronous Endpoint — Timing

UVC uses **isochronous** USB transfers, not bulk:

```
Bulk (flash drives):
    ├── Guaranteed delivery (retry on error)
    ├── Variable timing
    └── Best for data integrity

Isochronous (audio/video):
    ├── Guaranteed timing slot every 1ms (USB FS)
    ├── No retry on error (drop it and move on)
    ├── Max 1023 bytes per packet (USB FS)
    └── Best for real-time streams
    
Our endpoint: 0x81
    bInterval        = 1    → fires every 1ms
    wMaxPacketSize   = 512  → 512 bytes per slot
    
512 bytes/slot × 1000 slots/sec = 512,000 bytes/sec usable
50,688 bytes/frame ÷ 512,000 bytes/sec ≈ 10fps theoretical max
```

### 7. RAM Strategy — Line Streaming

Full frame (50,688 bytes) >> RAM (20,480 bytes). Solution: **never buffer the full frame.**

```c
// WRONG — impossible on Blue Pill:
uint8_t frame[50688];  // 50KB >> 20KB RAM — won't fit!

// CORRECT — compute on the fly, 512 bytes at a time:
uint8_t packet[514];   // 2 header + 512 pixels — fits easily!

// Each time isochronous slot fires:
// 1. Fill 512 bytes of test pattern (computed mathematically)
// 2. Prepend 2-byte UVC header
// 3. Send 514 bytes
// 4. Repeat until full frame sent
// 5. Toggle FID, start next frame
```

---

## Project File Structure

```
bluepill-uvc/
│
├── README.md               ← This file
├── platformio.ini          ← Board, framework, ST-Link config
│
├── src/
│   ├── main.c              ← Clock init, USB init, main loop
│   ├── usb_uvc.c           ← All USB + UVC descriptor + control handling
│   └── uvc_payload.c       ← Test pattern pixel generator
│
└── include/
    ├── usb_uvc.h           ← UVC structs, function declarations
    └── uvc_payload.h       ← Payload function declarations
```

---

## Hardware Setup

### Blue Pill USB Wiring

The Blue Pill needs a **D+ pull-up resistor** to signal Full-Speed to the host.
Some Blue Pill boards have this on-board; many do not.

```
STM32F103          USB Type-A connector
PA12 (D+) ────┬──── D+ (data+)     ← also needs 1.5kΩ pull-up to 3.3V
              │
             1.5kΩ
              │
            3.3V

PA11 (D−) ───────── D− (data-)
GND ─────────────── GND
5V (from USB) ────── VBUS (pin 1)   ← powers the board
```

**Check your Blue Pill:** if it has a resistor labeled R10 between PA12 and 3.3V,
the pull-up is already there. If not, solder a 1.5kΩ between PA12 and the 3.3V pin.

### ST-Link Wiring (for flashing)

```
ST-Link v2          Blue Pill
SWDIO  ──────────── PA13 (SWDIO)
SWCLK  ──────────── PA14 (SWCLK)
GND    ──────────── GND
3.3V   ──────────── 3.3V   (power from ST-Link, OR use USB 5V)
```

---

## Build & Flash

```bash
# Install PlatformIO CLI (if not done via VS Code extension)
pip install platformio

# Clone / open this project in VS Code
# PlatformIO extension auto-detects platformio.ini

# Build
pio run

# Flash via ST-Link
pio run --target upload

# Monitor serial (optional debug output)
pio device monitor --baud 115200
```

---

## Verify It Works

### Linux
```bash
# Check if device enumerates as UVC
lsusb
# Should show: Bus 001 Device 00X: ID cafe:4010 ...

# Check video device
v4l2-ctl --list-devices
# Should show: /dev/video0

# Check supported formats
v4l2-ctl -d /dev/video0 --list-formats-ext

# View the stream
ffplay /dev/video0
# OR
vlc v4l2:///dev/video0
```

### Windows
```
Device Manager → Cameras → "USB Video Device"
Open Camera app → should show test pattern
```

---

## Expected Output

When working correctly you will see **horizontal grayscale bars** — alternating
light and dark bands that span the full width of the frame. This proves:

- USB enumeration succeeded ✅
- UVC descriptors are valid ✅
- Probe/Commit negotiation completed ✅
- Isochronous endpoint is feeding data ✅
- Frame ID toggling works ✅
- OS UVC driver decoded YUY2 correctly ✅

---

## Limitations of Blue Pill for UVC

| Limitation | Impact | Solution on H-Series |
|---|---|---|
| 20 KB RAM | No frame buffering, must stream live | 1MB+ RAM, full DMA |
| 12 Mbps USB FS | Max ~10fps at 176x144 | USB HS (480 Mbps) = 100fps+ |
| No DCMI | Can't connect a real camera sensor | DCMI/CSI on H7/H5 |
| 72 MHz CPU | Enough for test pattern only | 480-550 MHz, JPEG HW encoder |
| No DMA to USB | CPU copies every byte | USB HS + DMA on H7 |

---

## Next Step — Moving to H-Series

Once you understand UVC on Blue Pill, the same concepts apply on H7:

```
Blue Pill (what you learn here)    →    STM32H743 / H750 (production)
─────────────────────────────────────────────────────────────────────
libopencm3 / bare metal USB        →    TinyUSB or USB HS HAL
Test pattern (computed)            →    Real camera via DCMI + DMA
176×144 YUY2                       →    1920×1080 MJPEG (HW encoder)
USB Full-Speed 12 Mbps             →    USB High-Speed 480 Mbps
Line-streaming workaround          →    Triple frame buffer in RAM
~5-10 fps                          →    30-60 fps
```

Everything you learn here — descriptors, Probe/Commit, payload headers,
Frame ID toggling — is identical on H-series. Only the peripheral registers change.

---

## References

- USB Video Class Specification 1.1 — USB.org
- libopencm3 STM32F1 USB examples — github.com/libopencm3/libopencm3-examples
- USB 2.0 Specification — usb.org/document-library
- STM32F103 Reference Manual (RM0008) — st.com