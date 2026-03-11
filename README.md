# STM32F103C8T6 Blue Pill — Composite USB Device
### CDC ACM Serial + UVC Camera | Raw USB Registers | No HAL | No CubeMX | ST-Link flash

---

## What Is This Project?

This project turns a **STM32F103C8T6 "Blue Pill"** board into a **composite USB device** that presents two interfaces simultaneously when plugged into any PC:

- **CDC ACM Serial** → appears as a virtual COM port (`COMx` on Windows, `/dev/ttyACMx` on Linux)
- **UVC Camera** → appears as a webcam, streams a grayscale test pattern

No custom driver needed. Both CDC ACM and UVC are natively supported by every modern OS.

> **Goal:** Understand how composite USB, CDC ACM, and UVC work at the protocol level before moving to higher-end STM32 H-series hardware.

---

## Board Specs — STM32F103C8T6

| Property | Value | Notes |
|---|---|---|
| Core | ARM Cortex-M3 | No FPU |
| Clock | 48 MHz | HSE 8MHz × PLL×6 (USB requires crystal) |
| Flash | 64 KB | Some chips have 128 KB |
| RAM | 20 KB | Biggest constraint for UVC |
| USB | Full-Speed (FS) 12 Mbps | Built-in, no external PHY needed |
| USB Pins | PA11 (D−), PA12 (D+) | Fixed, cannot remap |
| USB Buffer SRAM (PMA) | 512 bytes | Shared across all endpoints |
| Package | LQFP48 | "Blue Pill" dev board |

---

## Why Raw Registers and NOT Arduino/HAL?

| Framework | USB Control | Code Size | Learning Value |
|---|---|---|---|
| STM32duino (Arduino) | Hidden | Large | Low |
| STM32 HAL (CubeMX) | Partial | Very large | Medium |
| **Raw registers (this project)** | **Full** | **Minimal** | **High** |

For learning USB at the protocol level, **you want to see every byte**. Raw register access means nothing is hidden.

---

## Device Layout

When plugged in, Windows/Linux sees one composite device with two child interfaces:

```
USB Composite Device  (VID=0xCAFE, PID=0x4002)
│
├── Interface 0+1: CDC ACM  →  "USB Serial Device (COMx)"
│     EP0: Control (shared)
│     EP1 IN/OUT: Bulk 64B   ← serial data
│     EP2 IN: Interrupt 8B   ← CDC notifications (NAK only)
│
└── Interface 2+3: UVC  →  "BeeComposite" camera
      EP0: Control (shared)
      EP3 IN: Isochronous 512B  ← video frames
```

---

## How It Works — Full Theory

### 1. The Composite Trick — IAD

A normal USB device has one function. A composite device has multiple functions (serial + camera) on one USB connection. The host needs to know which interfaces belong together. This is done via **Interface Association Descriptors (IAD)**:

```
Configuration Descriptor
├── IAD  (bFirstInterface=0, bInterfaceCount=2, Class=CDC)
│     └── tells host: "IF0 and IF1 are one CDC function"
├── Interface 0 (CDC Control)
├── Interface 1 (CDC Data)
├── IAD  (bFirstInterface=2, bInterfaceCount=2, Class=Video)
│     └── tells host: "IF2 and IF3 are one Video function"
├── Interface 2 (VideoControl)
└── Interface 3 (VideoStreaming)
```

The Device Descriptor must also declare `bDeviceClass=0xEF, SubClass=0x02, Protocol=0x01` to signal IAD support.

---

### 2. USB Enumeration — Step by Step

```
Host                                STM32
 |                                     |
 |──── USB Reset ─────────────────────>|
 |<─── Device Ready ───────────────────|  (D+ rises via 1.5kΩ pull-up on PA12)
 |                                     |
 |──── GET_DESCRIPTOR (Device) ───────>|
 |<─── 18 bytes ───────────────────────|  VID=0xCAFE, PID=0x4002, Class=0xEF
 |                                     |
 |──── SET_ADDRESS (e.g. addr=3) ─────>|
 |<─── ZLP ACK ────────────────────────|  ← address applied AFTER ZLP, not before!
 |                                     |
 |──── GET_DESCRIPTOR (Config) ───────>|
 |<─── 228 bytes ──────────────────────|  all interfaces, endpoints, class specifics
 |                                     |
 |──── GET_DESCRIPTOR (Strings) ──────>|  "BeeBotix", "BeeComposite", "001"
 |<─── UTF-16LE strings ───────────────|
 |                                     |
 |──── SET_CONFIGURATION (1) ─────────>|
 |<─── ZLP ACK ────────────────────────|  opens EP1, EP2, EP3
 |                                     |
 |  [Windows loads usbser.sys + usbvideo.sys — no install needed]
```

---

### 3. CDC ACM — Virtual Serial Port

CDC ACM (Communications Device Class, Abstract Control Model) makes the device appear as a COM port. No baud rate is actually implemented in hardware — the "line coding" metadata (115200/8N1) is stored and echoed back, but data flows at USB speed.

**Class requests after enumeration:**

```
Host                                STM32
 |──── SET_LINE_CODING ───────────>|  "Use 115200 baud, 8N1"
 |<─── ZLP ACK ─────────────────── |  we store it, never configure a UART
 |                                  |
 |──── SET_CONTROL_LINE_STATE ────>|  "DTR=1 (terminal open)"
 |<─── ZLP ACK ─────────────────── |  we note it, no hardware lines to toggle
 |                                  |
 |──── GET_LINE_CODING ───────────>|  "Confirm your settings"
 |<─── 7 bytes ─────────────────── |  return stored line coding
```

**Data flow (echo firmware):**

```
Host types "Hello"
  → EP1 OUT bulk packet (64B max) → PMA at logical 0x100
  → firmware reads PMA, copies to TX PMA at 0xC0
  → EP1 IN bulk packet → host receives "Hello"
```

---

### 4. UVC — USB Video Class

#### 4.1 Descriptor Hierarchy

```
Interface 2: VideoControl
│
├── VC Header        — UVC version 1.0, total VC length, lists VS interfaces
├── Input Terminal   — bTerminalID=1, type=ITT_CAMERA (0x0201)
│                      "I have a camera sensor"
└── Output Terminal  — bTerminalID=2, type=TT_STREAMING (0x0101)
                       bSourceID=1 → linked to Input Terminal
                       "output goes to USB"

Interface 3 alt0: VideoStreaming (zero bandwidth, default)
Interface 3 alt1: VideoStreaming (active)
│
├── VS Input Header  — bNumFormats=1, bEndpointAddress=0x83, bTerminalLink=2
├── VS Format        — guidFormat=YUY2, bBitsPerPixel=16
└── VS Frame         — 176×144, 5fps, dwMaxVideoFrameSize=50688
```

#### 4.2 Probe & Commit Negotiation

Before any frame data flows, host and device negotiate stream parameters:

```
Host                                      STM32
 |──── SET_CUR VS_PROBE_CONTROL ─────────>|  "Can you stream 176x144 YUY2 at 5fps?"
 |<─── ZLP ACK ────────────────────────── |  we store the request
 |                                         |
 |──── GET_CUR VS_PROBE_CONTROL ─────────>|  "What can you actually do?"
 |<─── 26 bytes (uvc_probe_t) ─────────── |  we return our supported params
 |                                         |
 |──── SET_CUR VS_COMMIT_CONTROL ────────>|  "Locked in. Start streaming."
 |<─── ZLP ACK ────────────────────────── |
 |                                         |
 |──── SET_INTERFACE (IF3, alt=1) ────────>|  "Open the ISO endpoint"
 |<─── ZLP ACK ────────────────────────── |  we enable EP3, set STAT_VALID
```

#### 4.3 UVC Payload Header

Every USB packet sent on EP3 starts with a 2-byte UVC payload header:

```
Byte 0: HLE = 0x02  (header length = 2 bytes)
Byte 1: BFH flags
        bit 0 = FID  (Frame ID — toggles 0→1→0 on every new frame)
        bit 1 = EOF  (End of Frame — set on last packet of each frame)
        bits 2-7 = 0

Bytes 2..511: YUY2 pixel data (510 bytes per packet)
```

**Example — two frames:**
```
Packet 1: [0x02, 0x00, pixels...]   FID=0, middle of frame 1
Packet 2: [0x02, 0x02, pixels...]   FID=0, EOF=1, last packet of frame 1
Packet 3: [0x02, 0x01, pixels...]   FID=1, start of frame 2  ← FID toggled!
Packet 4: [0x02, 0x03, pixels...]   FID=1, EOF=1, last packet of frame 2
```

The OS uses FID to detect frame boundaries. Wrong FID = frozen or corrupted video.

#### 4.4 YUY2 Pixel Format

```
4 bytes = 2 pixels:

  [Y0] [U] [Y1] [V]
   │    │   │    └─ chroma red  (shared by both pixels)
   │    │   └────── luma pixel 1
   │    └────────── chroma blue (shared by both pixels)
   └─────────────── luma pixel 0

Grayscale: Y = brightness (0-255), U = 128, V = 128
```

Frame size: `176 × 144 × 2 = 50,688 bytes`

#### 4.5 RAM Strategy — Streaming Without Buffering

```
Full frame = 50,688 bytes >> 20,480 bytes RAM → impossible to buffer

Solution: generate pixels mathematically on the fly, 510 bytes at a time.
Each time the ISO endpoint fires (every 1ms), compute the next 510 bytes
of test pattern and send immediately. Never store the whole frame.
```

---

### 5. USB Hardware — STM32F103 PMA

The STM32F103 USB peripheral has 512 bytes of **Packet Memory Area (PMA)** at `0x40006000`. This is where USB data physically lives.

**Critical addressing rule:**
```
PMA is 16-bit wide on a 32-bit AHB bus.
Each 16-bit word occupies a 32-bit slot.
Logical byte offset L → physical address = 0x40006000 + L×2
```

**Our PMA layout:**

| Logical Offset | Size | Contents |
|---|---|---|
| `0x000–0x03F` | 64B | BTABLE (buffer descriptor table, 4 EPs × 8 bytes) |
| `0x040–0x07F` | 64B | EP0 TX (control IN) |
| `0x080–0x0BF` | 64B | EP0 RX (control OUT / SETUP) |
| `0x0C0–0x0FF` | 64B | EP1 TX (CDC bulk IN) |
| `0x100–0x13F` | 64B | EP1 RX (CDC bulk OUT) |
| `0x140–0x17F` | 64B | EP2 TX (CDC interrupt, NAK only) |
| `0x180–0x37F` | 512B | EP3 TX (UVC ISO IN) |

---

### 6. EPR Register — The Toggle-Bit Problem

The Endpoint Register (`USB_EPR`) is not a normal read/write register. Writing it incorrectly silently corrupts endpoint state:

```
Bits 15,7  (CTR_RX, CTR_TX):  write-0-to-clear  → must write 1 to preserve
Bits 14,6  (DTOG_RX, DTOG_TX): toggle-on-write  → XOR trick required
Bits 13:12 (STAT_RX):          toggle-on-write  → XOR to reach desired value
Bits 11,10,9,8,3:2,1,0:        normal R/W       → write desired value directly
Bits 6:4   (STAT_TX):          toggle-on-write  → XOR to reach desired value
```

Correct pattern to set STAT_TX to VALID (0b11):
```c
uint16_t v = USB_EPR(ep);
uint16_t w = (v & EPR_RW) | EP_CTR_RX | EP_CTR_TX;  // preserve, keep CTR high
w ^= ((v ^ (STAT_VALID << 4)) & EP_STAT_TX);          // XOR only STAT_TX bits
USB_EPR(ep) = w;
```

---

## PMA Layout Diagram

```
0x40006000  ┌─────────────────┐
            │  BTABLE EP0     │  ADDR_TX, CNT_TX, ADDR_RX, CNT_RX
0x40006010  ├─────────────────┤
            │  BTABLE EP1     │
0x40006020  ├─────────────────┤
            │  BTABLE EP2     │
0x40006030  ├─────────────────┤
            │  BTABLE EP3     │
0x40006040  ├─────────────────┤  ← logical 0x40 × 2 = physical +0x80
            │  EP0 TX (64B)   │  control responses
0x400060C0  ├─────────────────┤  ← logical 0x80 × 2 = physical +0x100
            │  EP0 RX (64B)   │  SETUP + control OUT
0x40006140  ├─────────────────┤  ← logical 0xC0 × 2 = physical +0x180
            │  EP1 TX (64B)   │  CDC serial → host
0x400061C0  ├─────────────────┤
            │  EP1 RX (64B)   │  CDC serial ← host
0x40006240  ├─────────────────┤
            │  EP2 TX (64B)   │  CDC notification (NAK)
0x400062C0  ├─────────────────┤
            │  EP3 TX (512B)  │  UVC ISO video frames
0x400064C0  └─────────────────┘  (end of 512B PMA, tight fit!)
```

---

## Clock — Why HSE Is Mandatory

```
HSI (internal RC oscillator):
  Accuracy: ±1%
  USB requires: ±0.25%
  Result: HOST REJECTS DEVICE  ← our initial bug

HSE (external 8MHz crystal, Blue Pill has this):
  Accuracy: ±50ppm = ±0.005%
  Result: USB works perfectly

Our PLL config:
  HSE = 8MHz → PLL × 6 = 48MHz SYSCLK
  USB clock = PLL / 1 = 48MHz  ← USB requires exactly 48MHz
```

---

## Project File Structure

```
bluepill-uvc/
│
├── README.md
├── platformio.ini          ← stm32cube framework, stlink upload
│
├── src/
│   ├── main.c              ← HSE clock init, D+ pull-down, 3 blinks, poll loop
│   └── usb_core.c          ← Complete USB stack: CDC ACM + UVC, raw registers
│
└── include/
    ├── usb_regs.h           ← PMA macros, EPR toggle helpers, buffer offsets
    └── uvc_desc.h           ← UVC structs (uvc_probe_t), constants
```

---

## Build & Flash

```bash
# Build
pio run

# Flash via ST-Link
pio run --target upload

# Verify enumeration (Windows PowerShell)
Get-PnpDevice | Where-Object { $_.InstanceId -like "USB\VID_CAFE*" } |
    Select-Object Status, FriendlyName, InstanceId

# Test serial echo (replace COM10 with your port)
$p = New-Object System.IO.Ports.SerialPort "COM10",115200
$p.Open(); $p.Write("Hello"); Start-Sleep -ms 100; $p.ReadExisting(); $p.Close()
```

---

## Verify It Works

### Windows
```
Device Manager:
  Ports (COM & LPT)         → USB Serial Device (COMx)   ✓
  Cameras                   → BeeComposite                ✓
  Universal Serial Bus       → USB Composite Device       ✓

Camera app: shows scrolling gray gradient pattern
```

### Linux
```bash
lsusb | grep CAFE
# Bus 001 Device 005: ID cafe:4002

ls /dev/ttyACM*   # serial
ls /dev/video*    # camera

# View camera stream
ffplay /dev/video0
# or
vlc v4l2:///dev/video0
```

---

## Expected Camera Output

A **diagonal grayscale gradient** that scrolls across the frame. This proves:

- USB enumeration succeeded ✅
- IAD composite device recognized ✅
- UVC descriptors valid (Input + Output terminals present) ✅
- Probe/Commit negotiation completed ✅
- ISO endpoint streaming data ✅
- Frame ID toggling correct ✅
- OS UVC driver decoded YUY2 correctly ✅

---

## Limitations vs H-Series

| Limitation | Blue Pill | H7 / H5 Solution |
|---|---|---|
| 20 KB RAM | No frame buffer, stream live | 1MB+ RAM, DMA frame buffers |
| 12 Mbps USB FS | ~10fps max at 176×144 | USB HS 480Mbps → 30-60fps+ |
| No DCMI | Test pattern only | DCMI/CSI for real camera sensor |
| 72 MHz CPU | Pattern generation only | 480-550 MHz + HW JPEG encoder |
| 512B PMA | Tight endpoint budget | Larger PMA, more endpoints |

Everything learned here — descriptors, Probe/Commit, payload headers, FID toggling, EPR toggle bits — is **identical on H-series**. Only peripheral registers change.

---

## Debugging

```powershell
# Full device details
Get-PnpDevice -InstanceId "USB\VID_CAFE&PID_4002\001" | Format-List *

# Check UVC error code
Get-PnpDeviceProperty -InstanceId "USB\VID_CAFE&PID_4002&MI_02\..." `
    -KeyName DEVPKEY_Device_ProblemCode | Select-Object Data
# Code 10 = descriptor issue
# Code 28 = no driver matched
# Code 43 = device rejected by driver

# OpenOCD memory dump (run from project dir)
C:\Users\..\.platformio\packages\tool-openocd\bin\openocd.exe `
    -f interface/stlink.cfg -f target/stm32f1x.cfg `
    -c "init; halt; sleep 200; mdw 0x40005C00 8; mdw 0x40006000 16; resume; shutdown"
```

---

## References

- USB Video Class Specification 1.5 — usb.org
- USB CDC Specification 1.2 — usb.org
- STM32F103 Reference Manual RM0008 — st.com
- USB 2.0 Specification — usb.org/document-library