# LEGO Dimensions Toypad Emulation — Technical Reference

> **Audience**: Another AI coding agent.
> **Emulator**: This reference documents the Cemu fork's internals (source paths are Cemu's). The shadPS4 port's bridge is documented separately in `TOYPAD_LISTENER_Latest-09-2026.md`.
> **Scope**: LEGO Dimensions Toypad emulation only. Skylanders/Infinity files are out-of-scope unless they share infrastructure that the Toypad code depends on — those shared pieces are noted as dependencies.
> **Every claim is traceable to an actual file/symbol**. Where something could not be confirmed from source, it is explicitly marked.

---

## 1. File Inventory

All paths are relative to the repository root (`cemu-project/Cemu`).

### Core Toypad Files
| File | Role | Size |
|------|------|------|
| `src/Cafe/OS/libs/nsyshid/Dimensions.h` | Header: `DimensionsToypadDevice`, `DimensionsUSB`, `DimensionsMini` structs | 3.6 KB |
| `src/Cafe/OS/libs/nsyshid/Dimensions.cpp` | Full implementation: command parsing, crypto, figure I/O, figure name database | 36.3 KB |

### Shared nsyshid Infrastructure (Toypad depends on these)
| File | Role |
|------|------|
| `src/Cafe/OS/libs/nsyshid/Backend.h` | `Device` base class, `Backend` base class, `HID_t`, message structs |
| `src/Cafe/OS/libs/nsyshid/nsyshid.cpp` | HLE HID API exports, device registry, handle management, callback dispatch |
| `src/Cafe/OS/libs/nsyshid/nsyshid.h` | Minimal forward declarations for the nsyshid module |
| `src/Cafe/OS/libs/nsyshid/BackendEmulated.cpp` | Device factory: conditionally creates `DimensionsToypadDevice` |
| `src/Cafe/OS/libs/nsyshid/BackendEmulated.h` | `BackendEmulated` class header |
| `src/Cafe/OS/libs/nsyshid/AttachDefaultBackends.cpp` | Boot-time wiring: attaches libusb + emulated backends |
| `src/Cafe/OS/libs/nsyshid/Whitelist.cpp` / `.h` | Vendor/product ID whitelist; registers `(0x0e6f, 0x0241)` at line 16 |
| `src/Cafe/OS/libs/nsyshid/BackendLibusb.cpp` / `.h` | Physical USB passthrough; logs detection of `0x0e6f:0x0241` at line 267 but applies no Dimensions-specific logic |

### GUI and Config
| File | Role |
|------|------|
| `src/gui/wxgui/EmulatedUSBDevices/EmulatedUSBDeviceFrame.cpp` | wxWidgets GUI: load/create/clear/move figure UI, calls into `g_dimensionstoypad` |
| `src/gui/wxgui/EmulatedUSBDevices/EmulatedUSBDeviceFrame.h` | GUI frame class definition; `m_dimensionSlots[7]`, `m_dimSlots[7]` |
| `src/config/CemuConfig.h` | Config key: `emulated_usb_devices.emulate_dimensions_toypad` (line 536) |
| `src/config/CemuConfig.cpp` | Config persistence: key `"EmulateDimensionsToypad"` (lines 291, 455) |

### Build System
- `src/Cafe/CMakeLists.txt` lines 428–429 include `Dimensions.cpp` / `Dimensions.h`.
- `BackendLibusb` is conditionally compiled under `#ifdef HAS_LIBUSB` (line 635).

---

## 2. Class & Struct Definitions (Verbatim from Source)

### 2.1 `HID_t` — Guest-Visible Device Descriptor
`Backend.h:11–23`
```cpp
typedef struct
{
    /* +0x00 */ uint32be handle;
    /* +0x04 */ uint32 ukn04;
    /* +0x08 */ uint16 vendorId;  // little-endian ?
    /* +0x0A */ uint16 productId; // little-endian ?
    /* +0x0C */ uint8 ifIndex;
    /* +0x0D */ uint8 subClass;
    /* +0x0E */ uint8 protocol;
    /* +0x0F */ uint8 paddingGuessed0F;
    /* +0x10 */ uint16be maxPacketSizeRX;
    /* +0x12 */ uint16be maxPacketSizeTX;
} HID_t;
```

### 2.2 Transfer Messages
`Backend.h:25–69`
```cpp
struct TransferCommand { uint8* data; uint32 length; /* ... */ };
struct ReadMessage final : TransferCommand  { sint32 bytesRead; /* ... */ };
struct WriteMessage final : TransferCommand { sint32 bytesWritten; /* ... */ };
struct ReportMessage final : TransferCommand { uint8 reportType; uint8 reportId; /* ... */ };
```

### 2.3 `Device` Base Class
`Backend.h:76–142`

Key virtual methods a Toypad device must implement:
```cpp
virtual bool Open() = 0;
virtual void Close() = 0;
virtual bool IsOpened() = 0;
virtual ReadResult Read(ReadMessage* message) = 0;    // game reads FROM pad
virtual WriteResult Write(WriteMessage* message) = 0; // game writes TO pad
virtual bool GetDescriptor(uint8 descType, uint8 descIndex, uint16 lang, uint8* output, uint32 outputMaxLength) = 0;
virtual bool SetIdle(uint8 ifIndex, uint8 reportId, uint8 duration) = 0;
virtual bool SetProtocol(uint8 ifIndex, uint8 protocol) = 0;
virtual bool SetReport(ReportMessage* message) = 0;
```

### 2.4 `DimensionsToypadDevice` — HID Device Shell
`Dimensions.h:10–42`
```cpp
class DimensionsToypadDevice final : public Device
{
  public:
    DimensionsToypadDevice();                     // Initializes as Device(0x0E6F, 0x0241, 1, 2, 0)
    // ... all pure virtual overrides ...
  private:
    bool m_IsOpened;
};
```
The constructor (`Dimensions.cpp:382–386`) passes: `vendorId=0x0E6F`, `productId=0x0241`, `interfaceIndex=1`, `interfaceSubClass=2`, `protocol=0`.

### 2.5 `DimensionsUSB` — State Machine & Protocol Handler
`Dimensions.h:44–109`
```cpp
class DimensionsUSB
{
  public:
    struct DimensionsMini final
    {
        std::unique_ptr<FileStream> dimFile;               // Open file handle for .bin persistence
        std::array<uint8, 0x2D * 0x04> data{};             // 180 bytes = 45 pages × 4 bytes (NTAG213-like)
        uint8 index = 255;                                  // 1-based slot index; 255 = empty
        uint8 pad = 255;                                    // Pad region (1=left, 2=center, 3=right)
        uint32 id = 0;                                      // Decoded figure/vehicle ID
        void Save();                                        // Writes data[] back to dimFile
    };

    void SendCommand(std::span<const uint8, 32> buf);       // ENTRY: game command → toypad
    std::array<uint8, 32> GetStatus();                      // ENTRY: toypad → game (BLOCKS)

    // Protocol commands
    void GenerateRandomNumber(std::span<const uint8, 8> buf, uint8 sequence, std::array<uint8, 32>& replyBuf);
    void GetChallengeResponse(std::span<const uint8, 8> buf, uint8 sequence, std::array<uint8, 32>& replyBuf);
    void QueryBlock(uint8 index, uint8 page, std::array<uint8, 32>& replyBuf, uint8 sequence);
    void WriteBlock(uint8 index, uint8 page, std::span<const uint8, 4> toWriteBuf, std::array<uint8, 32>& replyBuf, uint8 sequence);
    void GetModel(std::span<const uint8, 8> buf, uint8 sequence, std::array<uint8, 32>& replyBuf);

    // Figure management — GUI calls these
    uint32 LoadFigure(const std::array<uint8, 0x2D * 0x04>& buf, std::unique_ptr<FileStream> file, uint8 pad, uint8 index);
    bool RemoveFigure(uint8 pad, uint8 index, bool fullRemove);
    bool TempRemove(uint8 index);
    bool CancelRemove(uint8 index);
    bool MoveFigure(uint8 pad, uint8 index, uint8 oldPad, uint8 oldIndex);
    bool CreateFigure(fs::path pathName, uint32 id);

    // Lookup helpers
    static std::map<const uint32, const char*> GetListMinifigs();
    static std::map<const uint32, const char*> GetListTokens();
    std::string FindFigure(uint32 figNum);

  protected:
    std::mutex m_dimensionsMutex;
    std::array<DimensionsMini, 7> m_figures{};   // 7 simultaneous figures

  private:
    // Crypto
    std::array<uint8, 8> Decrypt(std::span<const uint8, 8> buf, std::optional<std::array<uint8, 16>> key);
    std::array<uint8, 8> Encrypt(std::span<const uint8, 8> buf, std::optional<std::array<uint8, 16>> key);
    std::array<uint8, 16> GenerateFigureKey(const std::array<uint8, 0x2D * 0x04>& uid);
    std::array<uint8, 4> PWDGenerate(const std::array<uint8, 0x2D * 0x04>& uid);
    std::array<uint8, 4> DimensionsRandomize(const std::vector<uint8> key, uint8 count);
    uint32 GetFigureId(const std::array<uint8, 0x2D * 0x04>& buf);
    uint32 Scramble(const std::array<uint8, 7>& uid, uint8 count);

    // RNG
    void InitializeRNG(uint32 seed);
    uint32 GetNext();
    uint32 m_randomA, m_randomB, m_randomC, m_randomD;

    bool m_isAwake = false;
    std::queue<std::array<uint8, 32>> m_figureAddedRemovedResponses;
    std::queue<std::array<uint8, 32>> m_queries;

    // Helpers
    void RandomUID(std::array<uint8, 0x2D * 0x04>& uidBuffer);
    uint8 GenerateChecksum(const std::array<uint8, 32>& data, int numOfBytes) const;
    DimensionsMini& GetFigureByIndex(uint8 index);
};

extern DimensionsUSB g_dimensionstoypad;  // Global singleton (Dimensions.cpp:23)
```

### 2.6 Global Singleton
`Dimensions.cpp:23`:
```cpp
DimensionsUSB g_dimensionstoypad;
```
This is a **file-scope global** in namespace `nsyshid`. It is **not** dynamically allocated. Its lifetime matches the process.

---

## 3. Complete Bidirectional Data Flow

### 3.1 Direction A: Game → Toypad (HID Write path)

```
Game (PPC guest) calls nsyshid::HIDWrite(handle, data, 32, callback, param)
    ↓
nsyshid.cpp::export_HIDWrite (L782)
    → GetDeviceByHandle(handle)    → finds DimensionsToypadDevice
    → spawns std::thread or std::async
        ↓
_hidWriteInternalSync (L722)
    → device->Write(&WriteMessage)
        ↓
DimensionsToypadDevice::Write (Dimensions.cpp:417)
    → validates message->length == 32
    → calls g_dimensionstoypad.SendCommand(span<const uint8, 32>)
        ↓
DimensionsUSB::SendCommand (Dimensions.cpp:540)
    → parses buf[2] as command, buf[3] as sequence
    → switch on command → dispatches to handler
    → pushes result to m_queries
```

### 3.2 Direction B: Toypad → Game (HID Read path)

```
Game (PPC guest) calls nsyshid::HIDRead(handle, data, maxLength, callback, param)
    ↓
nsyshid.cpp::export_HIDRead (L685)
    → GetDeviceByHandle(handle)    → finds DimensionsToypadDevice
    → spawns std::thread or std::async
        ↓
_hidReadInternalSync (L658)
    → device->Read(&ReadMessage)
        ↓
DimensionsToypadDevice::Read (Dimensions.cpp:410)
    → calls g_dimensionstoypad.GetStatus()    ← BLOCKS HERE
        ↓
DimensionsUSB::GetStatus (Dimensions.cpp:511)
    → polling loop:
        1. Check m_queries — if non-empty, pop and return
        2. Else check m_figureAddedRemovedResponses (only if m_isAwake) — pop and return
        3. Else sleep(100ms) and loop
    → returns std::array<uint8, 32>
```

> [!WARNING]
> **`GetStatus()` is a blocking call.** It spins in a `do/while` loop with 100ms sleeps until a response is available. It runs on a detached `std::thread` spawned by `export_HIDRead`. This means an external component injecting events must push to the queues, and `GetStatus()` will pick them up on its next iteration.

### 3.3 Direction C: User/External → Toypad State (Figure Loading)

```
GUI: user clicks "Load" for slot (pad, index)
    ↓
EmulatedUSBDeviceFrame::LoadMinifig(pad, index) (L607)
    → wxFileDialog for .bin files
    → LoadMinifigPath(path, pad, index) (L618)
        ↓
1. Opens file via FileStream::openFile2
2. Reads 180 bytes into std::array<uint8, 0x2D*0x04>
3. Calls ClearMinifig(pad, index) → g_dimensionstoypad.RemoveFigure(pad, index, true)
4. Calls g_dimensionstoypad.LoadFigure(file_data, dim_file, pad, index)
    ↓
DimensionsUSB::LoadFigure (Dimensions.cpp:620)
    → locks m_dimensionsMutex
    → calls GetFigureId(buf) to decrypt & extract the figure ID
    → stores data, file handle, pad, index, id in m_figures[index]
    → builds 0x56 response (figure-added event) and pushes to m_figureAddedRemovedResponses
    → returns figure ID
```

---

## 4. Command Protocol (Game → Toypad)

All commands arrive as 32-byte HID OUT reports. Parsed in `SendCommand`:

| Byte `buf[2]` | Name | Action |
|----------------|------|--------|
| `0xB0` | Wake | Returns hardcoded string: `55 0e 01 28 63 29 20 4c 45 47 4f 20 32 30 31 34 46` ("(c) LEGO 2014") |
| `0xB1` | Seed | TEA-decrypts `buf[4..11]` with `COMMAND_KEY`, extracts 32-bit seed, initializes RNG, returns encrypted confirmation |
| `0xB3` | Challenge | TEA-decrypts payload, advances RNG via `GetNext()`, returns encrypted next-random + confirmation. **Sets `m_isAwake = true`** — figure-added events are only sent after this |
| `0xC0` | Color | Acknowledge (`55 01 <seq> <checksum>`) + parsed into `LedPadState` — see `TOYPAD_LED_PROTOCOL.md` |
| `0xC1` | Get Pad Color | Acknowledge + parsed into `LedPadState` |
| `0xC2` | Fade | Acknowledge + parsed into `LedPadState` (captures from-color) |
| `0xC3` | Flash | Acknowledge + parsed into `LedPadState` |
| `0xC4` | Fade Random | Acknowledge + parsed into `LedPadState` (random target generated) |
| `0xC6` | Fade All | Acknowledge + parsed into `LedPadState` |
| `0xC7` | Flash All | Acknowledge + parsed into `LedPadState` |
| `0xC8` | Color All | Acknowledge + parsed into `LedPadState` |
| `0xD0` | Tag List | Unimplemented (logged) |
| `0xD2` | Read | Reads 4 pages (16 bytes) from `m_figures[buf[4]-1].data` starting at page `buf[5]` |
| `0xD3` | Write | Writes 4 bytes from `buf[6..9]` to page `buf[5]` of figure at `buf[4]`; calls `figure.Save()` |
| `0xD4` | Model | TEA-decrypts payload, extracts figure index, returns encrypted figure ID + confirmation |
| `0xE1` | PWD | Unimplemented |
| `0xE5` | Active | Unimplemented |
| `0xFF` | LEDS Query | Unimplemented |

### Response Format (Toypad → Game)

All responses are 32-byte arrays pushed to `m_queries`.

**Command responses** (from `SendCommand`):
```
[0]:  0x55          — Sync marker
[1]:  payload_len   — Length of following data (varies per command)
[2]:  sequence      — Echoed from the command's buf[3]
[3]:  status        — 0x00 for success (for D2/D3/D4)
[4+]: data payload
[N]:  checksum      — Sum of bytes [0..N-1] & 0xFF
```

**Figure added/removed events** (from `LoadFigure`/`RemoveFigure`):
```
[0]:  0x56          — Event marker (NOT 0x55)
[1]:  0x0b          — Payload length = 11
[2]:  pad           — Pad region (1/2/3)
[3]:  0x00          — Reserved
[4]:  figure.index  — 1-based slot index
[5]:  direction     — 0x00 = added, 0x01 = removed
[6]:  UID byte 0    — data[0]
[7]:  UID byte 1    — data[1]
[8]:  UID byte 2    — data[2]
[9]:  UID byte 3    — data[4]  (NOTE: skips data[3])
[10]: UID byte 4    — data[5]
[11]: UID byte 5    — data[6]
[12]: UID byte 6    — data[7]
[13]: checksum      — Sum of bytes [0..12] & 0xFF
```

> [!IMPORTANT]
> The UID extraction **skips byte 3** of the tag data (BCC byte in NTAG format). See `LoadFigure` line 634: `buf[0], buf[1], buf[2], buf[4], buf[5], buf[6], buf[7]`.

---

## 5. .bin File Format (NFC Tag Dump)

### 5.1 Structure

The `.bin` file is a raw 180-byte dump (45 pages × 4 bytes each) representing an NFC tag compatible with NTAG213-like structure:

```
Total size: 0x2D * 0x04 = 180 bytes (0xB4)

Page  0 (bytes 0-3):   UID byte 0, UID byte 1, UID byte 2, BCC0 (check byte)
Page  1 (bytes 4-7):   UID byte 3, UID byte 4, UID byte 5, UID byte 6
Page  2 (bytes 8-11):  Internal / lock bytes
...
Page 36 (bytes 144-147): Encrypted figure ID (first 4 bytes of 8-byte TEA block)
Page 37 (bytes 148-151): Encrypted figure ID (last 4 bytes of 8-byte TEA block)
Page 38 (bytes 152-155): Blank tag verification (byte 153 = 0x01 for blank tags)
...
Page 43 (bytes 172-175): NFC password (PWDGenerate output for characters, or game-written for vehicles)
Page 44 (bytes 176-179): Additional data
```

### 5.2 UID Layout

The 7-byte UID used throughout the code is extracted as:
```
uid[0] = data[0]    (Page 0, byte 0)
uid[1] = data[1]    (Page 0, byte 1)
uid[2] = data[2]    (Page 0, byte 2)
uid[3] = data[4]    (Page 1, byte 0) — skips data[3] which is BCC0
uid[4] = data[5]    (Page 1, byte 1)
uid[5] = data[6]    (Page 1, byte 2)
uid[6] = data[7]    (Page 1, byte 3)
```
When creating new figures, `data[0]` is always set to `0x04` (NXP manufacturer code) and `data[7]` is set to `0x80`. Bytes 1, 2, 4, 5, 6 are randomized.

### 5.3 Validation / Crypto Applied on Load

> [!IMPORTANT]
> **Cemu performs NO validation or checksum verification when loading a `.bin` file.** The code in `LoadMinifigPath` simply reads 180 bytes and passes them directly to `LoadFigure`. No CRC, no BCC check, no signature verification.

However, the following crypto IS applied when **reading the figure ID**:
- `GetFigureId`: Generates a per-figure key from the UID via `GenerateFigureKey`, then TEA-decrypts pages 36-37. If the decrypted value is < 1000 it's treated as a character ID (encrypted). Otherwise pages 36-37 are read as little-endian uint32 directly (vehicles/gadgets store their ID unencrypted).

### 5.4 Figure ID Ranges

- **0**: Blank tag
- **1–999**: Characters (minifigs) — ID is TEA-encrypted in pages 36-37
- **1000+**: Vehicles/Gadgets — ID is stored as plaintext little-endian uint32 in page 36

---

## 6. Encryption Details

### 6.1 TEA (Tiny Encryption Algorithm)

`Decrypt` / `Encrypt`

- Standard TEA with 32 rounds
- `delta = 0x9E3779B9`
- Operates on two 32-bit little-endian words
- Key: either `COMMAND_KEY` (for protocol messages when `key == std::nullopt`) or a per-figure key
- Data is 8 bytes (two uint32 LE)

### 6.2 `COMMAND_KEY` (Protocol Authentication)
`Dimensions.cpp:13-14`:
```cpp
static constexpr std::array<uint8, 16> COMMAND_KEY = {
    0x55, 0xFE, 0xF6, 0xB0, 0x62, 0xBF, 0x0B, 0x41,
    0xC9, 0xB3, 0x7C, 0xB4, 0x97, 0x3E, 0x29, 0x7B};
```

### 6.3 Per-Figure Key Generation
`GenerateFigureKey`:
1. Extract 7-byte UID from figure data
2. Call `Scramble(uid, N)` for N = 3, 4, 5, 6 — each produces a big-endian uint32
3. Concatenate four uint32 values → 16-byte key

`Scramble` concatenates UID + `CHAR_CONSTANT` (17 bytes), modifies a position, and calls `DimensionsRandomize`.

### 6.4 `CHAR_CONSTANT` and `PWD_CONSTANT`
```cpp
CHAR_CONSTANT = {0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA, 0x3C, 0xA8,
                 0xD8, 0x75, 0x47, 0x68, 0xCF, 0x23, 0xE9, 0xFE, 0xAA};

PWD_CONSTANT = {0x28, 0x63, 0x29, 0x20, 0x43, 0x6F, 0x70, 0x79,   // "(c) Copy"
                0x72, 0x69, 0x67, 0x68, 0x74, 0x20, 0x4C, 0x45,   // "right LE"
                0x47, 0x4F, 0x20, 0x32, 0x30, 0x31, 0x34, 0xAA, 0xAA}; // "GO 2014\xAA\xAA"
```

### 6.5 RNG (Jenkins small fast PRNG)
`InitializeRNG` / `GetNext`:
```cpp
void InitializeRNG(uint32 seed) {
    m_randomA = 0xF1EA5EED; m_randomB = m_randomC = m_randomD = seed;
    for (int i = 0; i < 42; i++) GetNext();
}
uint32 GetNext() {
    uint32 e = m_randomA - std::rotl(m_randomB, 21);
    m_randomA = m_randomB ^ std::rotl(m_randomC, 19);
    m_randomB = m_randomC + std::rotl(m_randomD, 6);
    m_randomC = m_randomD + e;
    m_randomD = e + m_randomA;
    return m_randomD;
}
```

### 6.6 Checksum
`GenerateChecksum`: Simple sum of bytes `[0..N-1]` masked to 8 bits. Not cryptographic.

---

## 7. LED/Light Command Subsystem

### Current Implementation

LED commands (`0xC0` through `0xC8`) are **acknowledged and parsed**. The emulation still sends back a 4-byte acknowledgment:

```cpp
// Dimensions.cpp:578-581
q_result = {0x55, 0x01, sequence};
q_result[3] = GenerateChecksum(q_result, 3);
```

`SendCommand` additionally routes `0xC0`–`0xC8` to `HandleLedCommand`, which parses the payload into a per-pad `LedPadState` (mode, target colour, from-colour, timing). When a Fade-family command lands, `SetLedState` snapshots the pad's previous colour into the `from*` fields before overwriting the target. Full detail — including the hardware payload layout, fade/flash semantics, and the renderer-side animation math — is in **`TOYPAD_LED_PROTOCOL.md`**.

### Command Semantics (confirmed)

| Cmd | Name | Payload (bytes 4+) |
|-----|------|----------------------------|
| `0xC0` | Color | Pad ID, R, G, B |
| `0xC1` | Get Pad Color | Pad ID |
| `0xC2` | Fade | Pad ID, speed, cycle_count, R, G, B |
| `0xC3` | Flash | Pad ID, on_duration, off_duration, count, R, G, B |
| `0xC4` | Fade Random | Pad ID, speed, cycle_count |
| `0xC6` | Fade All | R1, G1, B1, speed1, ... (for each pad) |
| `0xC7` | Flash All | Similar to Flash but for all pads |
| `0xC8` | Color All | R1, G1, B1, R2, G2, B2, R3, G3, B3 |

> [!NOTE]
> The payload layout is now confirmed against two independent reverse-engineering sources and the emulator's parser — see `TOYPAD_LED_PROTOCOL.md` §1.

### Impact on External Integration

For a figure-injection-only tool, **LED commands can still be ignored** — the game does not condition figure detection on LED state, and the lighting is cosmetic. LED state is now exposed outward through the network listener's `GET_LED` request for a companion renderer; see `TOYPAD_LED_PROTOCOL.md` §3 for the wire format.

---

## 8. Multi-Slot and Move Behavior

### Slot Layout

The physical Dimensions Toypad has 3 pad regions, but Cemu models **7 slots** total:

| Index | Pad | Position |
|-------|-----|----------|
| 0 | 2 (center) | Center |
| 1 | 1 (left) | Left top |
| 2 | 3 (right) | Right top |
| 3 | 2 (center) | Center-left bottom |
| 4 | 2 (center) | Center-right bottom |
| 5 | 3 (right) | Right-left bottom |
| 6 | 3 (right) | Right-right bottom |

These mappings come from `AddDimensionsPage`.

### Move Sequence

`MoveFigure`:
1. If `oldIndex == index` (same slot): just `CancelRemove(index)` → sends re-placement event
2. Otherwise:
   - `RemoveFigure(pad, index, true)` — evicts any figure at the destination
   - Save data + file from `m_figures[oldIndex]`
   - `RemoveFigure(oldPad, oldIndex, false)` — sends removal event but keeps file handle alive
   - `LoadFigure(data, file, pad, index)` — places figure at new slot, sends added event

The GUI-side flow:
1. `TempRemove(index)` — sends removal event (figure "picked up")
2. Modal dialog: user selects destination
3. On confirm: `MoveFigure(newPad, newIndex, oldPad, oldIndex)`
4. On cancel: `CancelRemove(index)` — sends re-placement event

---

## 9. Threading Model

| Thread | What it does |
|--------|-------------|
| **Guest game thread** (PPC) | Calls `HIDRead` / `HIDWrite` via HLE |
| **nsyshid I/O thread** | Detached `std::thread` per `HIDRead`/`HIDWrite` call; calls `Device::Read`/`Write` |
| **GUI thread** (wxWidgets main loop) | Calls `g_dimensionstoypad.LoadFigure()` etc. directly |

### Synchronization

- `m_dimensionsMutex` (plain `std::mutex`): guards `m_figures`, and some queue operations in `GetStatus()` for `m_figureAddedRemovedResponses`. **Not all queue accesses are guarded consistently** — `m_queries` is pushed/popped without the mutex in `SendCommand`/`GetStatus`.
- `hidMutex` (global `std::recursive_mutex` in `nsyshid.cpp`): guards `deviceList`, `backendList`, `HIDClientList`.
- `GetStatus()` polling: no condition variable. The I/O thread sleeps 100ms between polls. This introduces up to 100ms latency for figure-added events.

> [!WARNING]
> **Thread safety concern**: The GUI thread calls `LoadFigure`/`RemoveFigure`/`MoveFigure` which push to `m_figureAddedRemovedResponses` under `m_dimensionsMutex`. However, `SendCommand` pushes to `m_queries` **without** locking. `GetStatus()` only locks `m_dimensionsMutex` when popping from `m_figureAddedRemovedResponses`, not when popping from `m_queries`. An external component should match this existing pattern.

---

## 10. Dependencies

### Toypad → nsyshid
- `DimensionsToypadDevice` inherits from `nsyshid::Device` (`Backend.h`)
- `g_dimensionstoypad` is used by `DimensionsToypadDevice::Read`/`Write` (static coupling)
- Device factory is in `BackendEmulated::AttachVisibleDevices` (`BackendEmulated.cpp:48`)

### Toypad → Config
- `GetConfig().emulated_usb_devices.emulate_dimensions_toypad` (`CemuConfig.h:536`)

### Toypad → GUI
- `EmulatedUSBDeviceFrame` uses `nsyshid::g_dimensionstoypad` directly (hard-coded global)
- GUI toolkit: **wxWidgets** (confirmed by `#include <wx/frame.h>` etc.)

### Toypad → FileStream
- `Common/FileStream.h` — `FileStream::openFile2`, `FileStream::createFile2`, `readData`, `writeData`, `SetPosition`

### Priority Logic (Physical vs. Emulated)
`BackendEmulated.cpp:48`: If `FindDeviceById(0x0E6F, 0x0241)` finds a device (e.g. physical USB via libusb), the emulated device is NOT created. Physical hardware takes priority.

---

## 11. Insertion Point for External Figure Source

### The Target Function

The minimal, GUI-independent entry point for placing a figure on the virtual toypad:

```cpp
// Dimensions.h:75
uint32 LoadFigure(const std::array<uint8, 0x2D * 0x04>& buf,
                  std::unique_ptr<FileStream> file,
                  uint8 pad,
                  uint8 index);
```

**Parameters:**
- `buf`: 180-byte NFC tag dump data (the full `.bin` contents)
- `file`: `std::unique_ptr<FileStream>` — open file handle for write-back when the game writes to the tag. **Can potentially be nullptr if you don't need persistence**, but `Save()` checks for null and skips if so. See `DimensionsMini::Save()`.
- `pad`: Pad region (1=left, 2=center, 3=right)
- `index`: 0-based slot index (0–6)

**Returns**: `uint32` — the decoded figure ID

**Side effects:**
1. Locks `m_dimensionsMutex`
2. Stores figure data in `m_figures[index]`
3. Pushes a 0x56 figure-added event to `m_figureAddedRemovedResponses`

### Also Needed: RemoveFigure

```cpp
// Dimensions.h:72
bool RemoveFigure(uint8 pad, uint8 index, bool fullRemove);
```
- `fullRemove = true`: sends removal event, saves file, releases file handle, resets slot
- `fullRemove = false`: sends removal event, resets slot, keeps file handle alive (for moves)

### Integration Pattern for External Source

An external component (e.g. a network listener) would:

1. Receive 180 bytes of NFC tag data + target pad + target index
2. Call `g_dimensionstoypad.RemoveFigure(pad, index, true)` to clear any existing figure
3. Call `g_dimensionstoypad.LoadFigure(data, nullptr, pad, index)`
   - Pass `nullptr` for the `FileStream` if no local file persistence is needed
   - The `Save()` call inside `WriteBlock` will be a no-op with a null file handle
4. The `GetStatus()` polling loop will automatically pick up the figure-added event and deliver it to the game within ~100ms

### Thread Safety for External Caller

The external caller should:
- Call `LoadFigure` / `RemoveFigure` from any thread — these functions lock `m_dimensionsMutex` internally
- Not need to interact with `m_queries` or `SendCommand` at all — these are internal to the HID protocol
- Not need to worry about the HID device lifecycle — it's managed separately

### Can LED Commands Be Left Untouched?

**Yes, for a figure-injection-only tool.** The game sends LED commands via `HIDWrite` → `SendCommand`, and the game does not condition figure detection on LED state, so ignoring them has no effect on figure placement/detection gameplay. Note that this Cemu fork now *does* parse them into `LedPadState` and exposes that state to a companion renderer over the listener's `GET_LED` request (see `TOYPAD_LED_PROTOCOL.md`); a figure-only tool can ignore all of it.

### Minimal External Interface Summary

```
// What the external component needs to call:
nsyshid::g_dimensionstoypad.LoadFigure(data_180bytes, nullptr, pad, index)
nsyshid::g_dimensionstoypad.RemoveFigure(pad, index, true)

// What data it needs to provide:
std::array<uint8, 180>   — raw NFC tag dump
uint8 pad                — 1, 2, or 3
uint8 index              — 0 through 6

// What it does NOT need to touch:
- HID device creation/registration (handled by BackendEmulated)
- HID protocol (handled by SendCommand/GetStatus)
- LED/light commands (parsed into LedPadState; see TOYPAD_LED_PROTOCOL.md)
- Crypto (handled internally by DimensionsUSB)
```

---

## 12. "If You Change X, You Must Also Update Y"

| Change | Required Updates |
|--------|-----------------|
| Add/remove pad slots (change 7-slot array) | Update `m_figures` array size in `Dimensions.h:84`, `m_dimensionSlots` and `m_dimSlots` in `EmulatedUSBDeviceFrame.h:33-35`, GUI layout in `AddDimensionsPage`, and bounds checks in `GetFigureByIndex`, `QueryBlock`, `WriteBlock`, `GetModel` (all check `< 7`) |
| Change figure data size from 180 bytes | Update `0x2D * 0x04` everywhere (type alias would help), `LoadMinifigPath` read size, `CreateFigure` write size, `RandomUID`, all page arithmetic |
| Modify `LoadFigure` signature | Update call sites: `EmulatedUSBDeviceFrame::LoadMinifigPath` (L639), `MoveFigure` (L770) |
| Change LED state layout | Update `LedPadState` in `Dimensions.cpp` and bump the `GET_LED` wire-format version in `TOYPAD_LED_PROTOCOL.md` §3 |
| Change config key name | Update `CemuConfig.cpp` (L291, L455) and `BackendEmulated.cpp` (L48) |
| Change `DimensionsToypadDevice` constructor USB IDs | Must also update `BackendEmulated.cpp:48` (`FindDeviceById`), `BackendLibusb.cpp:267` (log check), `Whitelist.cpp:16` |
| Add a new command handler | Add case in `SendCommand` switch, ensure response is pushed to `m_queries`, verify checksum is appended |
| Change threading for `GetStatus()` | Currently polling — switching to condition_variable requires notifying from `SendCommand`, `LoadFigure`, `RemoveFigure`, `TempRemove`, `CancelRemove` |

---

## 13. Gotchas

1. **Byte order**: Tag data is little-endian. TEA operations use LE uint32 via `(uint32&)buf[N]`. HID_t fields `handle`, `maxPacketSizeRX/TX` are big-endian (`uint32be`, `uint16be`). The figure key from `GenerateFigureKey` is stored in big-endian order.

2. **1-based vs 0-based indexing**: The game uses 1-based indices in commands (D2, D3, D4). The code subtracts 1 before accessing `m_figures[]`. But `LoadFigure` sets `figure.index = index + 1` (converting back to 1-based for events). The GUI and external callers use 0-based indices for the slot parameter.

3. **`m_isAwake` gate**: Figure-added events are only delivered to the game after the Challenge command (`0xB3`) sets `m_isAwake = true`. If you inject a figure before the game sends the wake/challenge sequence, the event will sit in the queue but won't be delivered until the game completes the handshake.

4. **`nullptr` FileStream**: When passing `nullptr` as the file handle to `LoadFigure`, the `Save()` function in `WriteBlock` and `RemoveFigure` will silently skip the disk write. This means game-initiated tag writes (D3 command) will modify the in-memory data but not persist to disk. This is acceptable for external figure injection where persistence isn't needed.

5. **No validation on load**: There is no BCC check, no ATQA/SAK validation, and no file magic number check. Any 180-byte file will be accepted.

6. **Queue access inconsistency**: `m_queries` is accessed without mutex protection in both `SendCommand` (push) and `GetStatus` (pop). Under normal operation this works because `SendCommand` is called synchronously from the Write handler, and `GetStatus` is called from the Read handler, and there's typically one outstanding read at a time. But concurrent access could theoretically race.
