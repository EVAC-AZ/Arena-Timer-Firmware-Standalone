# Arena Timer Firmware

A networked countdown timer, originally built for combat robotics. Runs on a Waveshare RP2040-Zero driving a 64x32 RGB LED matrix, with a web-based control panel and optional WebSocket connection to PongAlmighty's [FightTimer](https://github.com/PongAlmighty/FightTimer).

## Features

- **64x32 RGB LED Matrix** — HUB75 interface, double-buffered via Adafruit Protomatter
- **Configurable Countdown** — up to 60 minutes, deci-second display under 1 minute
- **Color Thresholds** — automatic color changes as time decreases (up to 10 thresholds)
- **Web Control Panel** — responsive three-column layout for timer, colors, and system settings
- **Real-Time WebSocket Push** — browser updates via WebSocket with client-side interpolation for low-lag display
- **Persistent Settings** — saves to flash (RP2040 EEPROM emulation) with explicit Save button
- **Multiple Fonts** — Sans, Serif, Mono, Retro/Pixel, and custom Aquire variants
- **Display Options** — adjustable brightness, letter spacing, 180° rotation
- **FightTimer Integration** — syncs start/stop/reset/duration via Socket.IO
- **mDNS** — accessible at `http://arenatimer.local` from PCs (see [Phone Access](#phone-access))

## Hardware

### Required Components
- **Waveshare RP2040-Zero** microcontroller
- **64x32 RGB LED Matrix Panel** (HUB75, P5 pitch recommended)
- **W5500 Ethernet Module** (SPI interface)
- **5V Power Supply** (minimum 2A, 4A recommended for full brightness)
  - I used a 5V 20W PoE splitter mounted to the back of the enclosure

### 3D Enclosure
3d model files in `3d-models/`:
- `Timer Assembly v23.step` — STEP format for CAD editing
- `Timer Assembly v23.f3z` — Fusion 360 archive
- 
Note that these models are built to accommodate my particular electronics prototype. A proper PCB and more integrated mounting solution is coming soon!

If you plan on using this outside in the sun, I'd recommend printing out of a high-temperature, UV-resistant filament like ASA. Use PLA at your own risk...

## Quick Start

### 1. Flash Firmware
```bash
pio run --target upload
```

### 2. Network
The timer uses DHCP by default (10 second timeout). If DHCP fails, it falls back to `10.0.0.21`. The assigned IP is shown on the LED matrix for 10 seconds at startup.

Edit `src/main.cpp` to change the fallback IP or hostname:
```cpp
uint8_t static_ip[] = {10, 0, 0, 21};
const char* hostname = "arenatimer";
```

### 3. Web Interface
Open in a browser:
- `http://arenatimer.local` (mDNS — works on PCs, not mobile)
- `http://10.0.0.21` (direct IP — works everywhere)

### Phone Access

**mDNS (`.local`) does not work on mobile.** Use the IP address directly.

If you're on a dedicated arena network with no internet access, your phone may prefer mobile data over WiFi. To fix this, you may need to disable mobile data. A more comprehensive fix is coming in the future, but may depend on additional hardware...

### 4. FightTimer Integration
In the **WebSocket Connection** section of the web UI:
1. Enter the FightTimer host IP, port (`8765`), and path (`/socket.io/`)
2. Click **Connect**

Syncs: start/stop/reset commands, duration changes, time updates, expired/paused states.

## Configuration

### Network Settings
The MAC address is auto-generated from each RP2040's unique flash serial number — no manual configuration needed, no collisions between multiple timers.

Fallback IP and hostname can be changed in `src/main.cpp`:
```cpp
uint8_t static_ip[] = {10, 0, 0, 21};
const char* hostname = "arenatimer";
```

### Display Defaults
In `src/main.cpp` (overridden by saved settings on subsequent boots):
```cpp
timerDisplay.setFont(&FreeSansBold12pt7b);
timerDisplay.getTimer().setDuration({3, 0, 0});  // 3 minutes
```

### Available Fonts
| Category | Fonts |
|----------|-------|
| Sans | `FreeSans9pt7b`, `FreeSans12pt7b`, `FreeSansBold9pt7b`, `FreeSansBold12pt7b` |
| Serif | `FreeSerif9pt7b`, `FreeSerif12pt7b`, `FreeSerifBold9pt7b`, `FreeSerifBold12pt7b` |
| Mono | `FreeMono9pt7b`, `FreeMono12pt7b`, `FreeMonoBold9pt7b`, `FreeMonoBold12pt7b` |
| Retro | `Org_01`, `Picopixel`, `TomThumb` |
| Custom | `Aquire_BW0ox12pt7b`, `AquireBold_8Ma6012pt7b`, `AquireLight_YzE0o12pt7b` |

### Debug Output
Disable all serial output for production (eliminates timing overhead):
```cpp
// src/main.cpp, WebServer.cpp, WebSocketClient.cpp
#define DEBUG_MAIN false
#define DEBUG_WEBSERVER false
#define DEBUG_WEBSOCKET false
```

## Architecture

### Communication
- **HTTP (port 80)** — serves the HTML/CSS/JS control page only
- **WebSocket Server (port 81)** — all real-time communication with browsers (push-based, no polling)
- **WebSocket Client** — outbound connection to FightTimer (port 8765)

### Display Pipeline
The main loop detects content changes (timer tick, blink state, settings change) and only redraws when needed. Network operations are staggered across 10ms slots to prevent SPI contention with the Protomatter display ISR.

### Settings Persistence
Settings (duration, font, spacing, brightness, orientation, colors, thresholds) are stored in RP2040 flash-emulated EEPROM with magic number validation. Changes are applied live but only written to flash when the user clicks **Save Settings**.

## Development

### Build Environment
- **Framework**: Arduino (earlephilhower RP2040 core)
- **Build Tool**: PlatformIO
- **Key Libraries**: Adafruit Protomatter, Ethernet (W5500), WebSockets (links2004), ArduinoJson, EthernetBonjour, EEPROM

### Building
```bash
git clone https://github.com/EVAC-AZ/Arena-Timer-Firmware.git
cd Arena-Timer-Firmware
pio pkg install
pio run --target upload
pio device monitor  # optional serial debug
```

### Project Structure
```
├── src/
│   ├── main.cpp              # Setup, loop, display refresh logic
│   ├── Timer.cpp             # Countdown timer state machine
│   ├── TimerDisplay.cpp      # LED matrix rendering, blink logic
│   ├── RGBMatrix.cpp         # Protomatter matrix driver wrapper
│   ├── WebServer.cpp         # HTTP server, WebSocket server, web UI
│   ├── WebSocketClient.cpp   # FightTimer Socket.IO client
│   └── Settings.cpp          # Flash persistence (EEPROM)
├── include/
│   ├── Timer.h, TimerDisplay.h, RGBMatrix.h
│   ├── WebServer.h, WebSocketClient.h, Settings.h
│   └── CustomFonts/
├── 3d-models/
├── platformio.ini
└── README.md
```

## Troubleshooting

| Problem | Fix |
|---------|-----|
| Blank display | Check 5V power (minimum 2A) |
| Corrupted display | Verify HUB75 cable connections |
| Can't access web UI | Check Ethernet cable; IP shows on matrix at startup |
| mDNS not resolving | Use direct IP; mDNS doesn't work on Android |
| Phone won't connect | Disable mobile data or accept "no internet" WiFi prompt |
| FightTimer won't connect | Verify FightTimer is running and host/port are correct |
| Display flickers | Normal if heavy network activity; staggered SPI mitigates this |

## Credits

- **FightTimer**: [PongAlmighty/FightTimer](https://github.com/PongAlmighty/FightTimer)
- **RGB Matrix**: [Adafruit Protomatter](https://github.com/adafruit/Adafruit_Protomatter)
- **WebSockets**: [links2004/arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets)

## License

See [LICENSE](LICENSE) for details.