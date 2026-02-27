# SOS Blinking IoT AI Coding Guide

## Project Overview
Embedded IoT device firmware for AirM2M Core ESP32-C3 microcontroller. Implements SOS blinking pattern (LED signals in Morse code). Built with PlatformIO and Arduino framework in C++.

**Tech Stack**: PlatformIO, Arduino Framework, ESP32-C3, C++  
**Board**: AirM2M Core ESP32-C3 (low-power IoT device)

## Architecture

### ESP32-C3 Platform Specifics
- **CPU**: RISC-V single-core 160 MHz processor
- **Memory**: 384 KB internal SRAM, 4 MB flash
- **Connectivity**: 802.11 b/g/n Wi-Fi, BLE 5.0
- **Power**: Runs on battery/USB; very low power consumption critical
- **GPIO**: Limited pins; check datasheet for available I/O

### Project Structure
- **src/main.cpp**: Arduino sketch entry point (required by PlatformIO)
- **platformio.ini**: Build configuration specifying target board and framework
- **include/**: Header files for custom libraries/components
- **lib/**: Reusable PlatformIO libraries (if needed)
- **test/**: Unit tests using PlatformIO Test framework

### Current Implementation
- Boilerplate Arduino sketch with setup() and loop()
- SOS pattern not yet implemented (see: setup/loop/myFunction)

## Development Workflows

### Build and Upload
```bash
# Build firmware
pio run

# Build and upload to device
pio run -t upload

# Build specific environment
pio run -e airm2m_core_esp32c3
```

### Monitoring & Debugging
```bash
# Open serial monitor (default 115200 baud)
pio device monitor

# Monitor with custom baud rate
pio device monitor --baud 9600

# View serial output with upload
pio run -t upload -v  # -v for verbose
```

### Dependency Management
```bash
# Update/install PlatformIO libraries
pio lib install

# Search for libraries
pio lib search <keyword>

# Show dependency tree
pio lib list --depth=10
```

## Coding Patterns & Conventions

### Arduino Sketch Structure
```cpp
// Include required headers
#include <Arduino.h>

// Global variables and pin definitions
#define LED_PIN 3

// Setup: runs once on boot
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
}

// Loop: runs repeatedly
void loop() {
  // Main logic here
}
```

### SOS Pattern Implementation (Reference)
- **S** (dot-dot-dot): 3x 200ms ON
- **O** (dash-dash-dash): 3x 600ms ON
- **S** (dot-dot-dot): 3x 200ms ON
- 3 second gap between repetitions

### ESP32-C3 Specific Considerations
- **GPIO Pins**: Use pins available on AirM2M Core board (check pinout diagram)
- **Timers**: Prefer `millis()` over `delay()` for long waits to reduce power consumption
- **Serial**: Built-in USB-Serial bridge; use Serial.begin(115200)
- **Power**: Minimize active circuit time; use `delay()` only for brief operations

### Code Style
- Use descriptive constant names: `#define SOS_DOT_MS 200`
- Keep setup() minimal; initialization should not block
- Use blocking `delay()` only for timing critical operations
- Avoid global state; prefer local state in loop()

## platformio.ini Configuration
```ini
[env:airm2m_core_esp32c3]
platform = espressif32      # ESP32 platform provider
board = airm2m_core_esp32c3  # Target board
framework = arduino          # Arduino API compatibility
```

**Modify board/platform only for hardware target changes**

## Testing
- Use PlatformIO's native unit testing:
```bash
pio test -e airm2m_core_esp32c3
```
- Place tests in `test/` directory matching `.cpp` source files
- Tests run on device, not the local machine

## Common Tasks

### Adding External Library
```bash
pio lib install "library_name"
```

### Serial Communication
```cpp
Serial.println("Debug: SOS loop");
Serial.print("Voltage: ");
Serial.println(analogRead(A0));
```

### GPIO Control
```cpp
digitalWrite(LED_PIN, HIGH);   // Turn LED on
delay(200);
digitalWrite(LED_PIN, LOW);    // Turn LED off
delay(100);
```

### Flash & Memory
- Check free memory: `ESP.getFreeHeap()`
- Reduce binary size: compile with `Release` profile in platformio.ini
- Store large constants in PROGMEM or SPIFFS

## Build Configuration
- **platformio.ini**: Primary configuration (board, platform, dependencies)
- **src/main.cpp**: Arduino sketch code must contain setup() and loop()
- Platform features configured via defines in source or platformio.ini

## Device Connection
- USB connection required for upload via PlatformIO
- Serial monitor: `pio device monitor --port <COM_PORT>`
- Check available ports: `pio device list`
