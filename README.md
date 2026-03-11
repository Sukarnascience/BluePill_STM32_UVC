# STM32F103C8T6 Blue Pill — Composite USB Device
### CDC ACM Serial + UVC Camera | Raw USB Registers | No HAL | No CubeMX

---

## What Is This Project?

A **STM32F103C8T6 "Blue Pill"** presenting two USB functions simultaneously on one cable:

- **CDC ACM** → virtual COM port (`COMx` on Windows, `/dev/ttyACMx` on Linux)
- **UVC Camera** → webcam streaming 176×144 YUY2 grayscale test pattern

No custom driver needed. No HAL USB middleware. No libopencm3. Every USB register
write, every descriptor byte, every handshake is done by hand in C and documented here.

---

## How the STM32 Talks to the Computer — The Full Story

This is the part most documentation skips. Here is exactly what happens, byte by byte,
from the moment you plug in the USB cable to the moment Windows opens a COM port.

---

### Phase 0 — Physical Signal (Before Any Code Runs)

USB Full-Speed devices signal their presence by pulling **D+ high** through a 1.5kΩ
resistor. The host detects this rising edge and knows "a full-speed device just connected."

On Blue Pill, D+ is **PA12**. Our firmware controls the connect/disconnect:

```c
// Step 1: Pull PA12 LOW at startup — force D+ low = host sees nothing
GPIO_InitTypeDef g = {0};
g.Pin  = GPIO_PIN_12;
g.Mode = GPIO_MODE_OUTPUT_PP;
HAL_GPIO_Init(GPIOA, &g);
HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);  // D+ = 0V

// Step 2: Hold low for 500ms — if re-flashing, host sees clean disconnect
HAL_Delay(500);

// Step 3: Init USB peripheral registers, THEN release PA12
g.Mode = GPIO_MODE_INPUT;   // PA12 = input = floating
HAL_GPIO_Init(GPIOA, &g);
// Now the 1.5kΩ pull-up resistor on the Blue Pill board pulls D+ to 3.3V
// Host detects rising edge on D+ → "new full-speed device connected"
```

Why do this? If you just power up with D+ always high, the host may not reset its
device state properly after a reflash. The 500ms low pulse guarantees a clean connect.

---

### Phase 1 — USB Reset (Host Takes Control)

Within 100ms of detecting D+, the host forces a **USB Reset** — it drives both D+ and D−
to 0V (called SE0) for at least 10ms. This resets all device state.

Our USB peripheral fires the **RESET interrupt flag** in `USB_ISTR`. We handle it:

```c
// What we do on every USB Reset:
void handle_reset(void) {
    // 1. Tell peripheral where the Buffer Table lives in PMA
    USB_BTABLE = 0;  // BTABLE starts at PMA logical offset 0x00

    // 2. Set up EP0 TX buffer (where we put our replies)
    BTABLE_ADDR_TX(0) = 0x40;  // logical offset 0x40 in PMA
    BTABLE_CNT_TX(0)  = 0;     // nothing to send yet

    // 3. Set up EP0 RX buffer (where host SETUP packets land)
    BTABLE_ADDR_RX(0) = 0x80;  // logical offset 0x80 in PMA
    BTABLE_CNT_RX(0)  = (1<<15)|(2<<10);  // tell hardware: buffer = 64 bytes
    //  ^^ BL_SIZE=1 means 32-byte blocks, NUM_BLOCK=2 means 2 blocks = 64 bytes

    // 4. Configure EP0 as CONTROL type, address 0
    USB_EPR(0) = EP_TYPE_CTRL | 0x00;
    ep_stat_tx(0, STAT_NAK);    // TX: not ready yet
    ep_stat_rx(0, STAT_VALID);  // RX: ready to receive SETUP packets

    // 5. Enable device at address 0
    USB_DADDR = 0x80;  // bit 7 (EF) = 1 means "device enabled", addr = 0
}
```

After reset, device is at address 0, EP0 is open, everything else is closed.
The host can now begin the enumeration conversation.

---

### Phase 2 — Enumeration (The Question and Answer Session)

Enumeration is a strict sequence of control transfers on EP0. The host asks questions;
we answer with pre-built byte arrays stored in Flash. Here is every exchange:

---

#### Exchange 1 — GET_DESCRIPTOR (Device), first 8 bytes

The very first thing the host asks. It only requests 8 bytes initially because it needs
to know `bMaxPacketSize0` before requesting more.

**Host sends this SETUP packet (8 bytes arrive in our EP0 RX buffer):**
```
Byte  Value  Meaning
[0]   0x80   bmRequestType: direction=Device→Host, type=Standard, recipient=Device
[1]   0x06   bRequest: GET_DESCRIPTOR
[2]   0x00   wValue low: descriptor index = 0
[3]   0x01   wValue high: descriptor type = DEVICE (0x01)
[4]   0x00   wIndex = 0 (not used for device descriptor)
[5]   0x00
[6]   0x08   wLength = 8 (host only wants 8 bytes on first ask)
[7]   0x00
```

**We reply with first 8 bytes of our device descriptor:**
```
Byte  Value   Meaning
[0]   0x12    bLength = 18 (full descriptor is 18 bytes)
[1]   0x01    bDescriptorType = DEVICE
[2]   0x00    bcdUSB low
[3]   0x02    bcdUSB high → 0x0200 = USB 2.0
[4]   0xEF    bDeviceClass = 0xEF (Miscellaneous — required for IAD composite)
[5]   0x02    bDeviceSubClass = 0x02
[6]   0x01    bDeviceProtocol = 0x01 (IAD)
[7]   0x40    bMaxPacketSize0 = 64 bytes
```

Windows reads `bMaxPacketSize0=64` and knows EP0 can handle 64-byte packets.
It also reads `bDeviceClass=0xEF` and thinks: *"this is a composite IAD device,
I need to look at the config descriptor to find out what functions it has."*

**How we ACK at the hardware level:**

When we put data in the EP0 TX PMA buffer and call `ep_stat_tx(0, STAT_VALID)`,
the USB hardware automatically handles the low-level handshake:

```
Host sends:   SETUP token + 8 data bytes
STM32 HW:     sends ACK handshake automatically (hardware, not firmware)
Host sends:   IN token (asking for our response)
STM32 HW:     sends our 8 bytes from PMA + DATA1 PID
Host sends:   ACK handshake
STM32 HW:     fires CTR (Correct Transfer) flag in USB_ISTR
Our code:     sees CTR, clears it, continues
```

We never manually send ACK tokens — the USB hardware does that automatically.
Our job is only to put data in the PMA buffer and set `STAT_VALID`.

---

#### Exchange 2 — USB Reset (Again)

After reading the first 8 bytes, the host issues another USB Reset. This is normal —
Windows resets the device to "properly" start enumeration now that it knows the
max packet size. Our `handle_reset()` runs again, back to address 0.

---

#### Exchange 3 — GET_DESCRIPTOR (Device), full 18 bytes

Same request, `wLength=0x12` (18) this time. We send the complete device descriptor:

```
Byte  Value   Meaning
[0]   0x12    bLength = 18
[1]   0x01    DEVICE descriptor type
[2]   0x00    bcdUSB = 0x0200
[3]   0x02
[4]   0xEF    bDeviceClass: Miscellaneous (composite IAD)
[5]   0x02    bDeviceSubClass
[6]   0x01    bDeviceProtocol
[7]   0x40    bMaxPacketSize0 = 64
[8]   0xFE    idVendor low  = 0xCAFE
[9]   0xCA    idVendor high
[10]  0x02    idProduct low = 0x4002
[11]  0x40    idProduct high
[12]  0x00    bcdDevice low = 0x0100 (device version 1.0)
[13]  0x01    bcdDevice high
[14]  0x01    iManufacturer = string index 1
[15]  0x02    iProduct = string index 2
[16]  0x03    iSerialNumber = string index 3
[17]  0x01    bNumConfigurations = 1
```

Windows stores this and moves to the next question.

---

#### Exchange 4 — SET_ADDRESS

Windows assigns our device a unique address on the USB bus (e.g. address 5).

**SETUP packet:**
```
[0] 0x00   bmRequestType: Host→Device, Standard, Device
[1] 0x05   bRequest: SET_ADDRESS
[2] 0x05   wValue = 5 (new address)
[3] 0x00
[4] 0x00   wIndex = 0
...
[6] 0x00   wLength = 0 (no data phase)
```

**Critical timing — why we use `pending_addr`:**

```c
// WRONG — applying address immediately breaks enumeration:
USB_DADDR = 0x80 | 5;  // ← DON'T do this here
ep0_zlp();             // ZLP sent from wrong address, host misses it

// CORRECT:
pending_addr = 5;   // save it
ep0_zlp();          // send ZLP ACK from address 0 (host still listening on 0)
// ... in EP0 IN complete handler, AFTER ZLP is confirmed sent:
USB_DADDR = 0x80 | pending_addr;  // NOW switch to address 5
pending_addr = 0;
```

The ZLP (zero-length packet) is the ACK for SET_ADDRESS. It must leave from
**address 0** because the host is still listening there. Only after the host
receives the ZLP does it start talking to address 5.

After this, all further traffic goes to address 5. Any packet to address 0 is ignored.

---

#### Exchange 5 — GET_DESCRIPTOR (Configuration), first 9 bytes

Windows asks for the configuration descriptor, `wLength=9` first to get the total length.

**We reply with first 9 bytes:**
```
Byte  Value  Meaning
[0]   0x09   bLength = 9
[1]   0x02   bDescriptorType = CONFIGURATION
[2]   0xE4   wTotalLength low = 228  ← total bytes of config + all child descriptors
[3]   0x00   wTotalLength high
[4]   0x04   bNumInterfaces = 4 (IF0, IF1 = CDC; IF2, IF3 = UVC)
[5]   0x01   bConfigurationValue = 1
[6]   0x00   iConfiguration = 0 (no string)
[7]   0x80   bmAttributes: bus-powered, no remote wakeup
[8]   0xFA   bMaxPower = 250 × 2mA = 500mA max draw
```

Windows reads `wTotalLength=228` and thinks: *"I need to ask for 228 bytes to get
the full picture."*

---

#### Exchange 6 — GET_DESCRIPTOR (Configuration), full 228 bytes

Windows asks again with `wLength=228`. We send all 228 bytes. Because EP0 TX is
64 bytes max, we send it in **4 chunks**: 64 + 64 + 64 + 36 bytes.

Our `ep0_next()` function handles this automatically:

```c
// Called on every EP0 IN complete interrupt:
void ep0_next(void) {
    if (ep0_sent < ep0_len) {
        uint16_t remaining = ep0_len - ep0_sent;
        uint16_t chunk = remaining > 64 ? 64 : remaining;
        pma_write(EP0TX, ep0_ptr + ep0_sent, chunk);
        BTABLE_CNT_TX(0) = chunk;
        ep_stat_tx(0, STAT_VALID);  // release chunk to host
        ep0_sent += chunk;
    }
    // if ep0_sent == ep0_len: nothing more to send, host sends STATUS OUT
}
```

**Inside the 228 bytes, Windows finds the IAD descriptors:**

```
Offset 9:  IAD — bFirstInterface=0, bInterfaceCount=2, bFunctionClass=0x02 (CDC)
           → "interfaces 0 and 1 together = one CDC function"

Offset 17: Interface 0 (CDC Control) — Windows loads usbser.sys for this IAD group
Offset 26: CDC functional descriptors (Header, CallMgmt, ACM, Union)
Offset 45: EP2 Interrupt IN — CDC notification endpoint
Offset 52: Interface 1 (CDC Data)
Offset 61: EP1 Bulk OUT — serial data from host to device
Offset 68: EP1 Bulk IN  — serial data from device to host

Offset 75: IAD — bFirstInterface=2, bInterfaceCount=2, bFunctionClass=0x0E (Video)
           → "interfaces 2 and 3 together = one Video function"

Offset 83: Interface 2 (VideoControl) — Windows loads usbvideo.sys for this IAD group
Offset 92: VC Header descriptor — UVC version, total VC length
Offset 105: Input Terminal — "I have a camera, type=ITT_CAMERA"
Offset 123: Output Terminal — "output goes to USB, linked to terminal 1"
Offset 132: Interface 3 alt0 (VideoStreaming, zero bandwidth)
Offset 141: Interface 3 alt1 (VideoStreaming, active, has ISO endpoint)
Offset 150: EP3 ISO IN — 512 bytes, every 1ms
Offset 157: VS Input Header — format count, endpoint address, terminal link
Offset 171: VS Format — YUY2 GUID, bits per pixel
Offset 198: VS Frame — 176×144, 5fps, frame buffer size
```

This is how Windows knows what drivers to load — it reads `bFunctionClass` from
each IAD and loads the matching inbox driver.

---

#### Exchange 7 — GET_DESCRIPTOR (String 0 — Language List)

```
Host asks: wValue=0x0300 (string descriptor, index 0)
We reply:  [0x04, 0x03, 0x09, 0x04]
            bLength=4, STRING type, wLANGID=0x0409 (English US)
```

Windows now knows we support English strings. It will ask for string indices 1, 2, 3.

---

#### Exchange 8, 9, 10 — GET_DESCRIPTOR (Strings 1, 2, 3)

All strings are **UTF-16LE** encoded — every ASCII character takes 2 bytes:

```c
// "BeeBotix" in UTF-16LE:
static const uint8_t s_mfr[] = {
    18, 0x03,                           // bLength=18, STRING type
    'B',0, 'e',0, 'e',0, 'B',0,        // B e e B
    'o',0, 't',0, 'i',0, 'x',0         // o t i x
};
// 2 + 8×2 = 18 bytes total ✓

// "BeeComposite" in UTF-16LE:
static const uint8_t s_prod[] = {
    26, 0x03,
    'B',0,'e',0,'e',0,'C',0,'o',0,'m',0,
    'p',0,'o',0,'s',0,'i',0,'t',0,'e',0
};
// 2 + 12×2 = 26 bytes total ✓
```

Windows shows these strings in Device Manager under "Properties".

---

#### Exchange 11 — SET_CONFIGURATION (1)

```
Host sends:
[0] 0x00   Host→Device, Standard, Device
[1] 0x09   SET_CONFIGURATION
[2] 0x01   bConfigurationValue = 1 (activate config 1)
[3] 0x00
...
[6] 0x00   wLength = 0
```

This is the "go" signal. We open all the data endpoints:

```c
// Open EP1 for CDC bulk data
USB_EPR(1) = EP_TYPE_BULK | 0x01;
BTABLE_ADDR_TX(1) = 0xC0;              // TX buffer at PMA logical 0xC0
BTABLE_ADDR_RX(1) = 0x100;             // RX buffer at PMA logical 0x100
BTABLE_CNT_RX(1)  = (1<<15)|(2<<10);  // 64 bytes
ep_stat_tx(1, STAT_NAK);    // nothing to send yet
ep_stat_rx(1, STAT_VALID);  // ready to receive from host

// Open EP2 for CDC interrupt notifications (we just NAK it forever)
USB_EPR(2) = EP_TYPE_INTR | 0x82;
ep_stat_tx(2, STAT_NAK);

// EP3 (UVC ISO) stays DISABLED until SET_INTERFACE alt=1

ep0_zlp();  // ACK the SET_CONFIGURATION
```

After this ZLP, **enumeration is complete**. Windows fires the "device connected" sound.

---

### Phase 3 — CDC Driver Handshake (usbser.sys)

Windows loaded `usbser.sys` for the CDC IAD. Before exposing the COM port to applications,
the driver sends three class-specific requests:

#### SET_LINE_CODING (0x20)

```
Host sends 7-byte payload over EP0 OUT:
Bytes 0-3:  dwDTERate   = 0x00, 0xC2, 0x01, 0x00  → 115200 baud (little-endian)
Byte  4:    bCharFormat = 0x00  → 1 stop bit
Byte  5:    bParityType = 0x00  → no parity
Byte  6:    bDataBits   = 0x08  → 8 data bits
```

We store this and reply with a ZLP. We never configure a UART — data flows at USB speed.
The baud rate is metadata only, for application-layer compatibility.

#### SET_CONTROL_LINE_STATE (0x22)

```
Host sends SETUP with wValue:
  bit 0 = DTR (1 = terminal app opened the port)
  bit 1 = RTS

We reply ZLP. A real modem would assert hardware flow control lines.
We ignore it — no hardware lines to toggle.
```

#### GET_LINE_CODING (0x21)

```
Host asks: "confirm your line coding"
We reply:  same 7 bytes we stored from SET_LINE_CODING
```

After these three exchanges, **the COM port appears in Device Manager**.
Any application (PuTTY, Python, your own code) can now `open("COM10")`.

---

### Phase 4 — UVC Driver Handshake (usbvideo.sys)

Windows loaded `usbvideo.sys` for the Video IAD. Before any video flows:

#### Probe & Commit Negotiation

```
Host                                           STM32
 |                                                |
 |── SET_CUR VS_PROBE_CONTROL ──────────────────>|
 |   26-byte uvc_probe_t on EP0 OUT               |  "Can you stream 176×144 YUY2 at 5fps?"
 |<── ZLP ACK ─────────────────────────────────── |  we store it
 |                                                |
 |── GET_CUR VS_PROBE_CONTROL ──────────────────>|  "What can you actually do?"
 |<── 26-byte uvc_probe_t on EP0 IN ─────────── |  we return our supported params
 |                                                |
 |── SET_CUR VS_COMMIT_CONTROL ─────────────────>|  "Locked in. These are the params."
 |<── ZLP ACK ─────────────────────────────────── |
 |                                                |
 |── SET_INTERFACE (IF3, alt=1) ─────────────────>|  "Open the ISO endpoint, start streaming"
 |<── ZLP ACK ─────────────────────────────────── |
```

The `uvc_probe_t` structure (26 bytes) contains:

```c
typedef struct __attribute__((packed)) {
    uint16_t bmHint;                    // 0x0001 = dwFrameInterval is fixed
    uint8_t  bFormatIndex;              // 1 = our YUY2 format
    uint8_t  bFrameIndex;               // 1 = our 176×144 frame
    uint32_t dwFrameInterval;           // 2000000 = 5fps (in 100ns units)
    uint16_t wKeyFrameRate;             // 0
    uint16_t wPFrameRate;               // 0
    uint16_t wCompQuality;              // 0
    uint16_t wCompWindowSize;           // 0
    uint16_t wDelay;                    // 0
    uint32_t dwMaxVideoFrameSize;       // 50688 = 176×144×2
    uint32_t dwMaxPayloadTransferSize;  // 512 = our ISO packet size
} uvc_probe_t;
```

When `SET_INTERFACE alt=1` arrives, we enable EP3:

```c
USB_EPR(3) = EP_TYPE_ISO | 0x03;
BTABLE_ADDR_TX(3) = 0x180;  // ISO TX buffer at PMA logical 0x180
BTABLE_CNT_TX(3)  = 0;
ep_stat_tx(3, STAT_VALID);  // release to host — ISO starts immediately
streaming = 1;
```

---

### Phase 5 — Isochronous Video Stream

Every 1ms the USB host sends a Start-Of-Frame (SOF) token. Our EP3 fires. We put
the next 512 bytes of video data in PMA and release it.

**Every packet structure:**
```
Byte 0:  0x02           HLE — header is 2 bytes long
Byte 1:  BFH flags
           bit 0 = FID  — Frame ID, toggles 0→1→0 every new frame
           bit 1 = EOF  — 1 on the last packet of each frame
Bytes 2-511: YUY2 pixel data (510 bytes of actual image)
```

**Frame boundary detection by the OS:**

```
Packet N:    BFH = 0x00  (FID=0, not EOF) — middle of frame
Packet N+1:  BFH = 0x02  (FID=0, EOF=1)  — last packet of this frame
Packet N+2:  BFH = 0x01  (FID=1, not EOF) — first packet of NEXT frame ← FID toggled!
```

The OS watches for FID toggle to know "a new frame just started." If FID never
toggles, the OS thinks it's one endless frame — video appears frozen.

**YUY2 grayscale test pattern:**

```
4 bytes = 2 pixels:
[Y0] [U=128] [Y1] [V=128]

Y = (pixel_index + frame_counter) & 0xFF
  → brightness ramps 0→255 across the frame, scrolls each frame
  → appears as diagonal gray gradient moving across screen
U = V = 128 → neutral chroma = no color tint
```

**Why we can't buffer a full frame (RAM limit):**

```
Full frame:  176 × 144 × 2 = 50,688 bytes needed
STM32 RAM:   20,480 bytes total

50,688 > 20,480 → impossible to hold entire frame in RAM

Solution: compute each 510-byte chunk mathematically as needed.
No frame buffer. No DMA. CPU generates pixels on the fly each ISO slot.
```

---

### Phase 6 — What Windows Does With All This

Here is what Windows does at each stage of the above exchanges:

```
After Device Descriptor:
  → Reads VID=0xCAFE, PID=0x4002, Class=0xEF
  → Looks up in driver database: no match for VID/PID (custom device)
  → Reads Class=0xEF: "composite IAD device, check config for functions"

After Configuration Descriptor:
  → Walks all 228 bytes, finds IAD[0]: Class=0x02 (CDC)
    → Loads usbser.sys, assigns it IF0+IF1
  → Finds IAD[1]: Class=0x0E (Video)
    → Loads usbvideo.sys, assigns it IF2+IF3
  → Creates three device nodes in Device Manager:
      USB\VID_CAFE&PID_4002\001          (composite parent)
      USB\VID_CAFE&PID_4002&MI_00\...   (CDC child → COM port)
      USB\VID_CAFE&PID_4002&MI_02\...   (UVC child → camera)

After String Descriptors:
  → Shows "BeeComposite" as device friendly name
  → Shows "BeeBotix" as manufacturer

After SET_CONFIGURATION:
  → Opens all endpoints, device is live

After CDC class requests:
  → COM port appears in "Ports (COM & LPT)"
  → Applications can open COMx

After UVC Probe/Commit + SET_INTERFACE:
  → Camera appears in "Cameras" or "Imaging devices"
  → Camera app / VLC / OpenCV can open the video stream
```

---

## PMA — How Data Actually Moves

The STM32F103 USB peripheral has 512 bytes of dedicated **Packet Memory Area (PMA)**
at physical address `0x40006000`. This SRAM is shared between CPU and USB hardware.

**The CPU writes here to send data. The USB hardware reads from here and puts it on the wire.**
**The USB hardware writes here when it receives data. The CPU reads from here.**

Critical hardware detail — PMA is 16-bit wide on a 32-bit bus:

```
Each 16-bit word occupies a 32-bit physical slot:
  Logical offset 0  → physical 0x40006000
  Logical offset 2  → physical 0x40006004  (NOT 0x40006002!)
  Logical offset 4  → physical 0x40006008
  Logical offset N  → physical 0x40006000 + N×2

This is why all PMA writes use ×2:
```

```c
static void pma_write(uint16_t logical_offset, const uint8_t *src, uint16_t len) {
    // Convert logical offset to physical pointer (×2 for 32-bit slot spacing)
    volatile uint16_t *dst = (volatile uint16_t*)(0x40006000 + logical_offset * 2);
    for (uint16_t i = 0; i < len; i += 2) {
        uint16_t word = src[i];
        if (i+1 < len) word |= (src[i+1] << 8);
        *dst = word;
        dst += 2;  // advance by 4 bytes (one 32-bit slot)
    }
}
```

Getting the ×2 wrong means data lands at a completely different address in PMA —
the device descriptor ends up where the RX buffer should be. This was the hardest
bug in this project to find.

---

## EPR Register — Why It's Tricky

`USB_EPR(n)` at `0x40005C00 + n×4` is not a normal read/write register.
It has three different write behaviors on different bits:

```
Bit 15  CTR_RX  : read-only (write 0 to clear, write 1 = no effect)
Bit 14  DTOG_RX : toggle-on-write (write 1 = toggle, write 0 = no change)
Bit 13:12 STAT_RX: toggle-on-write (XOR to reach desired state)
Bit 11  SETUP   : read-only
Bit 10:9 EP_TYPE: normal read/write
Bit 8   EP_KIND : normal read/write
Bit 7   CTR_TX  : read-only (write 0 to clear, write 1 = no effect)
Bit 6   DTOG_TX : toggle-on-write
Bit 6:4 STAT_TX : toggle-on-write
Bit 3:0 EA      : normal read/write (endpoint address)
```

To set STAT_TX to VALID (0b11) without disturbing other bits:

```c
void ep_stat_tx(uint8_t ep, uint16_t desired) {
    uint16_t current = USB_EPR(ep);

    // Build write value:
    // - Keep all normal R/W bits as-is
    // - Write 1 to CTR bits (to NOT clear them)
    // - XOR STAT_TX: current XOR (current XOR desired) = desired
    uint16_t write = (current & EPR_RW_MASK) | EP_CTR_RX | EP_CTR_TX;
    write ^= ((current ^ (desired << 4)) & EP_STAT_TX);

    USB_EPR(ep) = write;
}
```

If you just do `USB_EPR(ep) = 0x0320` to set VALID+CONTROL, you'll accidentally
clear CTR_TX (which was set by hardware to signal "transfer complete") and accidentally
toggle DTOG. The result: missed transfers and data corruption that's very hard to debug.

---

## Clock — The Silent Killer

```
First attempt used HSI (internal RC oscillator):
  Accuracy: ±1%
  USB spec requires: ±0.25%
  What happened: host saw garbled bit timing, responded with "Device Descriptor Request Failed"

Fixed by switching to HSE (8MHz external crystal on Blue Pill):
  Accuracy: ±50ppm = ±0.005%
  USB works perfectly

PLL configuration for exactly 48MHz:
  HSE = 8MHz
  PLL multiplier = ×6
  SYSCLK = 48MHz
  USB clock source = PLL (÷1) = 48MHz  ← USB hardware requires exactly 48MHz
```

```c
osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
osc.PLL.PLLMUL    = RCC_PLL_MUL6;      // 8MHz × 6 = 48MHz
```

The original bootloader always worked because it uses HSE. Our first firmware used HSI
and produced `VID_0000&PID_0002` (Windows code for "I got garbage and gave up").

---

## Build & Flash

```bash
pio run --target upload
```

```powershell
# Verify (Windows)
Get-PnpDevice | Where-Object { $_.InstanceId -like "USB\VID_CAFE*" } |
    Select-Object Status, FriendlyName, InstanceId

# Expected:
# OK  USB Composite Device       USB\VID_CAFE&PID_4002\001
# OK  USB Serial Device (COM10)  USB\VID_CAFE&PID_4002&MI_00\...
# OK  BeeComposite                USB\VID_CAFE&PID_4002&MI_02\...

# Test serial echo
$p = New-Object System.IO.Ports.SerialPort "COM10",115200
$p.Open(); $p.Write("Hello"); Start-Sleep -ms 100; $p.ReadExisting(); $p.Close()

# View camera (Python)
pip install opencv-python
python view_uvc.py
```

---

## File Structure

```
bluepill-uvc/
├── README.md
├── platformio.ini
├── src/
│   ├── main.c       — HSE clock, D+ disconnect, 3 blinks, poll loop
│   └── usb_core.c   — Complete USB stack: enumeration, CDC, UVC, streaming
└── include/
    ├── usb_regs.h   — PMA macros, EPR helpers, buffer offsets
    └── uvc_desc.h   — uvc_probe_t struct, UVC constants
```

---

## Limitations vs H-Series

| | Blue Pill | STM32H7 |
|---|---|---|
| RAM | 20KB — no frame buffer | 1MB+ — triple buffer |
| USB | FS 12Mbps — ~10fps | HS 480Mbps — 60fps+ |
| Camera | Test pattern (no sensor) | DCMI/CSI — real sensor |
| CPU | 48MHz for this project | 480MHz + HW JPEG |

All concepts here — descriptors, Probe/Commit, FID toggling, EPR bits, PMA layout —
are identical on H-series. Only the peripheral register addresses change.

---

## References

- USB 2.0 Specification — usb.org
- USB Video Class Specification 1.5 — usb.org
- USB CDC Specification 1.2 — usb.org
- STM32F103 Reference Manual RM0008 — st.com