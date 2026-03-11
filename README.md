# STM32F103C8T6 Blue Pill — Composite USB Device
### CDC ACM Serial + UVC Camera | Raw USB Registers | No HAL | No CubeMX | ST-Link flash

---

## What Does This Do?

Plug a **$2 Blue Pill** into any PC via USB. The computer sees **two devices at once** — no drivers to install, no software to run:

```
┌─────────────────────────────────────────────────────────────┐
│                    Your Computer                            │
│                                                             │
│   ┌──────────────────┐      ┌─────────────────────────┐    │
│   │  📟 COM Port      │      │  📷 Webcam              │    │
│   │  "USB Serial      │      │  "BeeComposite"         │    │
│   │   Device (COM10)" │      │  176×144 test pattern   │    │
│   └────────┬─────────┘      └────────────┬────────────┘    │
│            │    (usbser.sys)              │  (usbvideo.sys) │
│            └──────────────┬──────────────┘                  │
│                           │                                 │
│              USB Composite Device                           │
│              VID=0xCAFE  PID=0x4002                        │
└───────────────────────────┬─────────────────────────────────┘
                            │  USB cable
                    ┌───────┴───────┐
                    │  Blue Pill    │
                    │ STM32F103C8T6 │
                    │  48MHz / 20KB │
                    └───────────────┘
```

---

## Big Picture — How It's Built

```
┌─────────────────────────────────────────────────────────────────┐
│                    FIRMWARE LAYERS                              │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  main.c                                                 │   │
│  │  • HSE clock setup (8MHz × PLL6 = 48MHz)               │   │
│  │  • PA12 pull-down → 500ms → release  (USB connect)     │   │
│  │  • 3× LED blink on PC13                                 │   │
│  │  • Loop: usb_core_poll() every tick                    │   │
│  └──────────────────────┬──────────────────────────────────┘   │
│                         │                                       │
│  ┌──────────────────────▼──────────────────────────────────┐   │
│  │  usb_core.c   (the entire USB stack)                    │   │
│  │                                                         │   │
│  │  ┌────────────────┐    ┌──────────────────────────┐    │   │
│  │  │  Enumeration   │    │  Data Handling            │    │   │
│  │  │  handle_reset()│    │  CDC echo EP1 IN/OUT      │    │   │
│  │  │  handle_setup()│    │  UVC stream EP3 ISO       │    │   │
│  │  │  ep0_send()    │    │  build_uvc_payload()      │    │   │
│  │  │  ep0_zlp()     │    │                           │    │   │
│  │  └────────────────┘    └──────────────────────────┘    │   │
│  └──────────────────────┬──────────────────────────────────┘   │
│                         │                                       │
│  ┌──────────────────────▼──────────────────────────────────┐   │
│  │  usb_regs.h                                             │   │
│  │  • PMA read/write macros (×2 address trick)            │   │
│  │  • EPR toggle-bit helpers                               │   │
│  │  • BTABLE layout macros                                 │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  STM32F103 USB Hardware                                  │  │
│  │  USB Peripheral (0x40005C00) + PMA SRAM (0x40006000)    │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## What Happens When You Plug In

```
                    ┌──────────────────────────────────────────┐
  USB cable         │                                          │
  inserted   ──────►│  Phase 0: D+ rises → host sees device   │
                    └──────────────────┬───────────────────────┘
                                       │
                    ┌──────────────────▼───────────────────────┐
                    │  Phase 1: USB Reset                      │
                    │  Host resets bus, firmware runs          │
                    │  handle_reset() — opens EP0 only         │
                    └──────────────────┬───────────────────────┘
                                       │
                    ┌──────────────────▼───────────────────────┐
                    │  Phase 2: Enumeration (10–100ms)         │
                    │                                          │
                    │  Host asks:          STM32 replies:      │
                    │  GET_DESCRIPTOR ──► 18-byte Device desc  │
                    │  USB Reset again                         │
                    │  GET_DESCRIPTOR ──► 18-byte Device desc  │
                    │  SET_ADDRESS    ──► ZLP (then switch)    │
                    │  GET_DESCRIPTOR ──► 228-byte Config desc │
                    │  GET Strings    ──► "BeeBotix" etc.      │
                    │  SET_CONFIG(1)  ──► ZLP + open EPs       │
                    └──────────────────┬───────────────────────┘
                                       │
                    ┌──────────────────▼───────────────────────┐
                    │  Phase 3: CDC Driver Handshake           │
                    │  usbser.sys loads, sends:                │
                    │  SET_LINE_CODING ──► ZLP (store 115200)  │
                    │  SET_CTRL_STATE  ──► ZLP (note DTR=1)    │
                    │  GET_LINE_CODING ──► return stored vals  │
                    │                                          │
                    │  ✅ COM port appears in Device Manager   │
                    └──────────────────┬───────────────────────┘
                                       │
                    ┌──────────────────▼───────────────────────┐
                    │  Phase 4: UVC Driver Handshake           │
                    │  usbvideo.sys loads, sends:              │
                    │  SET_CUR PROBE  ──► ZLP (store params)   │
                    │  GET_CUR PROBE  ──► 26-byte uvc_probe_t  │
                    │  SET_CUR COMMIT ──► ZLP (lock params)    │
                    │  SET_INTERFACE(alt=1) ──► open EP3 ISO   │
                    │                                          │
                    │  ✅ Camera appears in Device Manager     │
                    └──────────────────┬───────────────────────┘
                                       │
                    ┌──────────────────▼───────────────────────┐
                    │  Phase 5: Streaming (continuous)         │
                    │  Every 1ms: EP3 ISO fires                │
                    │  STM32 puts 512B in PMA → host reads it  │
                    │  ✅ Video plays in Camera app / VLC      │
                    └──────────────────────────────────────────┘
```

---

## Device Structure — What the Computer Sees

```
USB Composite Device  [VID=0xCAFE  PID=0x4002]
│
├─── IAD Group 0: CDC Function  ──────────────────────────────────────┐
│    bFunctionClass=0x02 → Windows loads usbser.sys automatically     │
│                                                                      │
│    Interface 0: CDC Control                                          │
│    │   EP2 IN  Interrupt 8B  (NAK — notifications not used)        │
│    │                                                                 │
│    Interface 1: CDC Data                                             │
│        EP1 OUT Bulk 64B  ◄── serial bytes from PC                  │
│        EP1 IN  Bulk 64B   ──► serial bytes to PC                   │
│                                                                      │
│    Result: COM10 in Device Manager                                   │
│    Use: PuTTY / Python serial / any terminal                         │
└──────────────────────────────────────────────────────────────────────┘
│
└─── IAD Group 1: Video Function  ────────────────────────────────────┐
     bFunctionClass=0x0E → Windows loads usbvideo.sys automatically   │
                                                                       │
     Interface 2: VideoControl                                         │
     │   VC Header   — UVC version, total VC descriptor size          │
     │   Input Term  — bTerminalID=1, type=ITT_CAMERA                 │
     │   Output Term — bTerminalID=2, bSourceID=1 → USB out           │
     │                                                                 │
     Interface 3 alt0: VideoStreaming (zero bandwidth, default)        │
     Interface 3 alt1: VideoStreaming (active)                         │
         EP3 IN  ISO 512B  ──► video frames to PC                    │
                                                                       │
     Result: "BeeComposite" in Cameras / Imaging Devices              │
     Use: Camera app / VLC / OpenCV                                    │
└──────────────────────────────────────────────────────────────────────┘
```

---

## Memory Map — Where Data Lives

```
STM32F103 USB Packet Memory Area (PMA) — 512 bytes total
Physical base: 0x40006000

Logical   Physical     Size   Contents
Offset    Address
──────────────────────────────────────────────────────────
0x000  →  0x40006000   64B    BTABLE (buffer descriptor table)
                              4 endpoints × 8 bytes each:
                              [ADDR_TX | CNT_TX | ADDR_RX | CNT_RX]

0x040  →  0x40006080   64B    EP0 TX  — control replies going to host
                              (descriptor bytes, ZLPs, CDC line coding)

0x080  →  0x40006100   64B    EP0 RX  — SETUP packets from host
                              (GET_DESCRIPTOR, SET_ADDRESS, etc.)

0x0C0  →  0x40006180   64B    EP1 TX  — CDC serial bytes going to host
                              (echo of what you typed)

0x100  →  0x40006200   64B    EP1 RX  — CDC serial bytes from host
                              (what you type in PuTTY)

0x140  →  0x40006280   64B    EP2 TX  — CDC notification (always NAK)

0x180  →  0x40006300  512B    EP3 TX  — UVC ISO video frame data
                              (2-byte header + 510 bytes YUY2 pixels)
                                                         ▲
                                                         └── tight fit!
                                                         0x380 = end of 512B PMA
──────────────────────────────────────────────────────────
NOTE: Logical offset N → Physical = 0x40006000 + N×2
      (16-bit words in 32-bit slots — the ×2 rule)
```

---

## How the STM32 ACKs — The Hardware Handshake

This is what most tutorials skip. Our firmware **never sends ACK tokens manually.**
The USB hardware does it automatically. Here is the full picture:

```
                    USB Wire                    STM32 Hardware          Our Firmware
                                                                        (usb_core.c)
                        │                            │                       │
Host wants data:        │                            │                       │
                        │                            │  We called:           │
                        │                            │  pma_write(EP0TX,...) │
                        │                            │  BTABLE_CNT_TX = N   │
                        │                            │  ep_stat_tx(VALID)   │
                        │                            │                       │
  ──── IN token ───────►│                            │                       │
                        │◄── DATA1 + N bytes ────────│  (hardware reads PMA)│
  ──── ACK ────────────►│                            │                       │
                        │                            │  Sets CTR_TX flag    │
                        │                            │  in USB_ISTR         │
                        │                            │          │            │
                        │                            │          └───────────►│
                        │                            │                       │ usb_core_poll()
                        │                            │                       │ sees CTR flag
                        │                            │                       │ calls ep0_next()
                        │                            │                       │ or handle_ep1_in()
```

**Key insight:** We put data in PMA → set STAT=VALID → hardware takes over,
sends the data, receives ACK from host, then tells us via CTR flag.
We react to CTR, not to ACK directly.

---

## The Composite Trick — IAD Explained

Without IAD, the host sees all 4 interfaces as unrelated functions.
With IAD, the host knows which interfaces belong together:

```
WITHOUT IAD (wrong):                  WITH IAD (correct):
                                       
  IF0 → ??? (orphan)                  IAD[0]: IF0+IF1 = CDC function
  IF1 → ??? (orphan)                    IF0 → CDC Control
  IF2 → ??? (orphan)                    IF1 → CDC Data
  IF3 → ??? (orphan)                  IAD[1]: IF2+IF3 = Video function
                                         IF2 → VideoControl
  Windows: "I don't know what            IF3 → VideoStreaming
  drivers to load"
                                      Windows: "I know exactly what
                                      to load for each group"
```

The Device Descriptor must also declare:
```
bDeviceClass    = 0xEF   ← "Miscellaneous" — signals IAD is in use
bDeviceSubClass = 0x02
bDeviceProtocol = 0x01
```

Without these three bytes set correctly, Windows ignores the IADs entirely.

---

## UVC — How Video Actually Flows

```
  ┌─────────────────────────────────────────────────────────────────┐
  │                    UVC DESCRIPTOR CHAIN                         │
  │                                                                 │
  │  Interface 2: VideoControl                                      │
  │  │                                                              │
  │  ├── VC Header          "I speak UVC 1.0, here are my units"  │
  │  │                                                              │
  │  ├── Input Terminal     "I have a camera"                      │
  │  │   ID=1  type=ITT_CAMERA                                     │
  │  │                       ▼                                      │
  │  └── Output Terminal    "camera output → USB"                  │
  │      ID=2  type=TT_STREAMING  sourceID=1                       │
  │                               ▼                                 │
  │  Interface 3: VideoStreaming                                     │
  │  │                                                              │
  │  ├── VS Input Header    bTerminalLink=2 ◄── must match ID=2   │
  │  │                      bEndpointAddress=0x83 (EP3 IN)         │
  │  │                                                              │
  │  ├── VS Format          guidFormat=YUY2                        │
  │  │                      bBitsPerPixel=16                       │
  │  │                                                              │
  │  └── VS Frame           176×144px  5fps                       │
  │                         dwMaxVideoFrameSize=50688              │
  └─────────────────────────────────────────────────────────────────┘

  ┌─────────────────────────────────────────────────────────────────┐
  │                    PROBE / COMMIT                               │
  │                                                                 │
  │  Host:  SET_CUR PROBE  ──►  "Can you do 176×144 YUY2 at 5fps?"│
  │  STM32: ZLP             ◄── stores request                     │
  │  Host:  GET_CUR PROBE  ──►  "What can you actually do?"        │
  │  STM32: 26-byte struct  ◄── returns supported params           │
  │  Host:  SET_CUR COMMIT ──►  "Locked. Start."                   │
  │  STM32: ZLP             ◄──                                    │
  │  Host:  SET_INTERFACE(1)──►  "Open EP3"                        │
  │  STM32: ZLP + EP3 open  ◄──  streaming = 1                     │
  └─────────────────────────────────────────────────────────────────┘

  ┌─────────────────────────────────────────────────────────────────┐
  │                    FRAME STREAMING                              │
  │                                                                 │
  │  Frame = 176×144×2 = 50,688 bytes                              │
  │  Each ISO packet = 512 bytes (2 header + 510 pixels)           │
  │  Packets per frame = 50688 / 510 = ~100 packets                │
  │                                                                 │
  │  Packet N (mid-frame):   ┌──────────────────────────────────┐  │
  │                          │0x02│0x00│ Y U Y V Y U Y V ... │  │  │
  │                          └──┬──┴──┬─┴────────────────────┘  │  │
  │                             │     └── BFH: FID=0, EOF=0      │  │
  │                             └──────── HLE=2 (header=2 bytes) │  │
  │                                                               │  │
  │  Packet N+1 (last of frame): ┌─────────────────────────────┐ │  │
  │                          │0x02│0x02│ Y U Y V ...           │ │  │
  │                          └──┬──┴──┬─┘                       │  │
  │                             │     └── BFH: FID=0, EOF=1 ✓   │  │
  │                             └──────── HLE=2                  │  │
  │                                                               │  │
  │  Packet N+2 (first of NEXT frame):                           │  │
  │                          │0x02│0x01│ Y U Y V ...            │  │
  │                          └──┬──┴──┬─┘                       │  │
  │                             │     └── BFH: FID=1 ← TOGGLED! │  │
  │                             └──────── EOF=0                  │  │
  │                                                               │  │
  │  OS sees FID change 0→1 = "new frame started"                │  │
  └─────────────────────────────────────────────────────────────────┘
```

---

## RAM Strategy — Why We Can't Buffer a Frame

```
  ┌──────────────────────────────────────────────────────────────┐
  │  MEMORY PROBLEM                                              │
  │                                                              │
  │  Full YUY2 frame:   176 × 144 × 2  =  50,688 bytes needed  │
  │  STM32 total RAM:                     20,480 bytes total    │
  │                                                              │
  │  50,688 > 20,480 → IMPOSSIBLE to hold full frame in RAM     │
  └──────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────┐
  │  SOLUTION: GENERATE ON THE FLY                               │
  │                                                              │
  │  Every 1ms when EP3 ISO fires:                               │
  │                                                              │
  │  1. Calculate which pixel offset we're at                    │
  │     (frame_byte_offset → which row, which column)           │
  │                                                              │
  │  2. Generate 510 bytes of test pattern mathematically        │
  │     Y = (col + frame_counter) & 0xFF  ← scrolling gradient  │
  │     U = 128, V = 128                  ← no color tint        │
  │                                                              │
  │  3. Prepend 2-byte UVC header                                │
  │                                                              │
  │  4. Write 512 bytes to PMA → release EP3                    │
  │                                                              │
  │  5. Repeat next millisecond                                  │
  │                                                              │
  │  RAM used: one 512-byte packet buffer only ✓                 │
  └──────────────────────────────────────────────────────────────┘
```

---

## Clock — The Silent Killer

```
  ┌─────────────────────────────────────────────────────────────┐
  │  WHY CLOCK SOURCE MATTERS FOR USB                           │
  │                                                             │
  │  USB Full-Speed bit period = 1 / 12,000,000 = 83.3ns       │
  │  USB spec tolerance: ±500ppm = ±0.05%                       │
  │  (roughly ±0.04ns per bit)                                  │
  │                                                             │
  │  HSI (internal RC oscillator):                              │
  │  ┌─────────────────────────────────────────────────┐       │
  │  │  Accuracy: ±1% = ±10,000ppm                     │       │
  │  │  20× worse than USB requires                    │       │
  │  │  Result: host sees timing errors                │       │
  │  │  → "Device Descriptor Request Failed"           │       │
  │  │  → VID=0000 PID=0002 in Device Manager ❌       │       │
  │  └─────────────────────────────────────────────────┘       │
  │                                                             │
  │  HSE (external 8MHz crystal — Blue Pill has this):          │
  │  ┌─────────────────────────────────────────────────┐       │
  │  │  Accuracy: ±50ppm = 200× better than USB needs  │       │
  │  │  Result: perfect enumeration ✅                  │       │
  │  └─────────────────────────────────────────────────┘       │
  │                                                             │
  │  PLL chain:                                                 │
  │  HSE 8MHz ──► PLL ×6 ──► 48MHz SYSCLK                     │
  │                      └──► 48MHz USB clock (÷1)             │
  │                                                             │
  │  USB hardware requires exactly 48MHz. No rounding.         │
  └─────────────────────────────────────────────────────────────┘
```

---

## Technical Deep Dive

### EPR Register — The Toggle-Bit Trap

The Endpoint Register is not a normal read/write register. Different bits behave differently:

```
Bit 15  CTR_RX  : write-0-to-clear  (write 1 = no change, hardware clears it)
Bit 14  DTOG_RX : toggle-on-write   (write 1 = flip the bit, write 0 = no change)
Bit 13:12 STAT_RX: toggle-on-write  (XOR to reach desired state)
Bit 7   CTR_TX  : write-0-to-clear
Bit 6   DTOG_TX : toggle-on-write
Bit 6:4 STAT_TX : toggle-on-write
Bit 10:9 EP_TYPE: normal R/W
Bit 3:0 EA      : normal R/W (endpoint address)
```

To set STAT_TX to VALID (0b11) without disturbing anything else:

```c
void ep_stat_tx(uint8_t ep, uint16_t desired) {
    uint16_t current = USB_EPR(ep);
    uint16_t write   = (current & EPR_RW_MASK) | EP_CTR_RX | EP_CTR_TX;
    write ^= ((current ^ (desired << 4)) & EP_STAT_TX);
    USB_EPR(ep) = write;
}
```

Writing `USB_EPR(ep) = 0x0320` directly would accidentally clear CTR_TX
(killing the "transfer complete" signal) and randomly toggle DTOG.

---

### PMA Addressing — The ×2 Rule

```c
// PMA is 16-bit wide on a 32-bit AHB bus.
// Each 16-bit word sits in a 32-bit slot.
// Logical offset L → Physical address = 0x40006000 + L×2

static void pma_write(uint16_t L, const uint8_t *src, uint16_t len) {
    volatile uint16_t *dst = (volatile uint16_t*)(0x40006000 + L * 2);
    for (uint16_t i = 0; i < len; i += 2) {
        uint16_t word = src[i] | (i+1 < len ? src[i+1] << 8 : 0);
        *dst = word;
        dst += 2;   // ← advance by 4 bytes (one 32-bit slot)
    }
}
```

Getting this wrong puts your device descriptor in the middle of the RX buffer.
The device enumerates with garbage data and Windows shows Code 43.

---

### SET_ADDRESS Timing

```c
// In SETUP handler — save address, don't apply yet:
case REQ_SET_ADDRESS:
    pending_addr = setup.wValue & 0x7F;
    ep0_zlp();          // ZLP sent from address 0 (host still on addr 0)
    break;

// In EP0 IN-complete handler — apply AFTER ZLP confirmed sent:
if (pending_addr) {
    USB_DADDR = 0x80 | pending_addr;   // NOW switch address
    pending_addr = 0;
}
```

If you apply the new address before sending the ZLP, the ZLP leaves from the
new address. The host is still listening on address 0 and misses it.
Result: enumeration hangs, Windows retries three times and gives up.

---

### Descriptor Layout (228 bytes total, Python-verified)

```
Offset  Length  Type
──────────────────────────────────────────────────────
  0       9     Configuration Descriptor
  9       8     IAD[0] — CDC function (IF0+IF1)
 17       9     Interface 0 — CDC Control
 26       5     CDC Header Functional
 31       5     CDC Call Management Functional
 36       4     CDC Abstract Control Model Functional
 41       5     CDC Union Functional
 46       7     EP2 IN Interrupt (CDC notification)
 53       9     Interface 1 — CDC Data
 62       7     EP1 OUT Bulk (serial host→device)
 69       7     EP1 IN  Bulk (serial device→host)
 76       8     IAD[1] — Video function (IF2+IF3)
 84       9     Interface 2 — VideoControl
 93      13     VC Header (UVC 1.0, total VC len=40)
106      18     Input Terminal (bLen=18, bControlSize=3)
124       9     Output Terminal
133       9     Interface 3 alt0 — VS zero-bandwidth
142       9     Interface 3 alt1 — VS active
151       7     EP3 IN Isochronous 512B
158      13     EP3 Audio Class Companion (required)
171      14     VS Input Header
185      27     VS Format Uncompressed (YUY2 GUID)
212      30     VS Frame 176×144 (bLen=30, 1 interval)
──────────────────────────────────────────────────────
         228    TOTAL  ← matches wTotalLength in Config
```

---

## Build & Flash

```bash
# Build
pio run

# Flash
pio run --target upload
```

```powershell
# Verify Windows sees all three nodes
Get-PnpDevice | Where-Object { $_.InstanceId -like "USB\VID_CAFE*" } |
    Select-Object Status, FriendlyName, InstanceId

# Expected output:
# OK   USB Composite Device       USB\VID_CAFE&PID_4002\001
# OK   USB Serial Device (COM10)  USB\VID_CAFE&PID_4002&MI_00\...
# OK   BeeComposite                USB\VID_CAFE&PID_4002&MI_02\...

# Test echo
$p = New-Object System.IO.Ports.SerialPort "COM10",115200
$p.Open(); $p.Write("Hello BeeBotix"); Start-Sleep -ms 100; $p.ReadExisting(); $p.Close()

# View camera
python view_uvc.py
```

---

## Limitations vs H-Series

```
  Blue Pill (learning platform)          STM32H7 (production)
  ────────────────────────────────────────────────────────────
  20KB RAM → stream on the fly     →    1MB+ RAM, full DMA frame buffers
  USB FS 12Mbps → ~10fps max       →    USB HS 480Mbps → 30–60fps
  No DCMI → test pattern only      →    DCMI/CSI → real camera sensor
  48MHz → CPU generates pixels     →    480MHz + hardware JPEG encoder
  512B PMA → tight endpoint budget →    Larger PMA, more endpoints

  Everything learned here maps directly to H7:
  descriptors, Probe/Commit, FID toggling, EPR bits, PMA layout.
  Only the peripheral register addresses change.
```

---

## File Structure

```
bluepill-uvc/
├── README.md
├── platformio.ini          ← stm32cube, stlink, 48MHz
├── src/
│   ├── main.c              ← clock, D+ control, blink, poll loop
│   └── usb_core.c          ← full USB stack (CDC + UVC)
└── include/
    ├── usb_regs.h          ← PMA macros, EPR helpers
    └── uvc_desc.h          ← uvc_probe_t, UVC constants
```

---

## References

- USB 2.0 Specification — usb.org
- USB Video Class 1.5 — usb.org
- USB CDC 1.2 — usb.org
- STM32F103 Reference Manual RM0008 — st.com