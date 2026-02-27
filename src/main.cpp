/*
 * SOS Blinking LED with OTA Update - ESP32-C3
 * 
 * Firmware Version: 1.0
 * Build Date: 2026-02-27
 * 
 * Board: LuatOS CORE-ESP32 C3
 * MCU: ESP32-C3 (160 MHz, RISC-V)
 * 
 * Features:
 * - FreeRTOS multi-task architecture
 * - SOS blinking pattern (Morse code)
 * - WiFi auto-connect with retry
 * - HTTP OTA updates with SHA-256 verification
 * - Queue-based serial logging
 * - Task-based architecture (non-blocking)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <time.h>

// ============================================================================
// CONFIGURATION PARAMETERS
// ============================================================================

#define LED_PIN                 13          // GPIO 13 (D5)
#define WIFI_SSID               "YOUR_SSID"
#define WIFI_PASSWORD           "YOUR_PASSWORD"
#define OTA_URL                 "http://example.com/firmware.bin"
#define OTA_EXPECTED_SHA256     "0000000000000000000000000000000000000000000000000000000000000000"  // 64-char hex
#define SERIAL_BAUD             115200

// Timing parameters (in milliseconds)
#define LED_TIME_UNIT_MS        300
#define WIFI_RETRY_INTERVAL_MS  10000       // 10 seconds
#define WIFI_TIMEOUT_MS         30000       // 30 seconds
#define OTA_TIMEOUT_MS          60000       // 60 seconds

// Task stack sizes (in bytes)
#define LED_TASK_STACK          2048
#define WIFI_TASK_STACK         4096
#define OTA_TASK_STACK          6144
#define LOGGER_TASK_STACK       2048

// Queue sizes
#define LOG_QUEUE_SIZE          16
#define LOG_MESSAGE_SIZE        256

// Task priorities (FreeRTOS: higher number = higher priority)
#define TASK_PRIORITY_LED       1
#define TASK_PRIORITY_LOGGER    2
#define TASK_PRIORITY_OTA       3
#define TASK_PRIORITY_WIFI      3

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef struct {
    uint32_t timestamp;
    char level[8];      // DEBUG, INFO, ERROR
    char module[16];    // WIFI, OTA, LED, etc.
    char message[LOG_MESSAGE_SIZE - 64];
} LogMessage;

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_ERROR = 2
} LogLevel;

// Global state
volatile bool isWiFiConnected = false;
volatile bool otaUpdateRequested = false;
uint32_t bootTime = 0;
QueueHandle_t logQueue = NULL;
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t otaTaskHandle = NULL;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

/**
 * Get milliseconds since boot
 */
uint32_t getTimestamp() {
    return millis();
}

/**
 * Queue a log message
 */
void logMessage(LogLevel level, const char* module, const char* format, ...) {
    if (logQueue == NULL) return;
    
    LogMessage msg;
    msg.timestamp = getTimestamp();
    
    // Set level string
    switch (level) {
        case LOG_LEVEL_DEBUG: strcpy(msg.level, "DEBUG"); break;
        case LOG_LEVEL_INFO:  strcpy(msg.level, "INFO"); break;
        case LOG_LEVEL_ERROR: strcpy(msg.level, "ERROR"); break;
        default:              strcpy(msg.level, "INFO"); break;
    }
    
    // Set module
    strncpy(msg.module, module, sizeof(msg.module) - 1);
    msg.module[sizeof(msg.module) - 1] = '\0';
    
    // Format message
    va_list args;
    va_start(args, format);
    vsnprintf(msg.message, sizeof(msg.message) - 1, format, args);
    va_end(args);
    msg.message[sizeof(msg.message) - 1] = '\0';
    
    // Send to queue (non-blocking, discard if full)
    xQueueSendToBackFromISR(logQueue, &msg, NULL);
}

/**
 * Convert hex string to bytes
 */
void hexStrToBytes(const char* hexStr, uint8_t* bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        sscanf(hexStr + 2 * i, "%2hhx", &bytes[i]);
    }
}

/**
 * Compute SHA-256 of a buffer (for OTA verification)
 */
void computeSHA256(const uint8_t* data, size_t len, uint8_t* hash) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA256, 1 = SHA224
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
}

/**
 * Print formatted log output to serial (used by logger task)
 */
void printLogMessage(const LogMessage* msg) {
    char buffer[LOG_MESSAGE_SIZE + 32];
    snprintf(buffer, sizeof(buffer), "[%10u] [%-5s] %s: %s\r\n",
             msg->timestamp, msg->level, msg->module, msg->message);
    Serial.print(buffer);
}

// ============================================================================
// LED TASK - SOS Morse Code Pattern Generation
// ============================================================================

/**
 * SOS Pattern State Machine
 * Time unit = 300ms
 * S = dot-dot-dot (300ms ON, 300ms OFF) x3, then 900ms total gap (includes last symbol gap)
 * O = dash-dash-dash (900ms ON, 300ms OFF) x3, then 900ms total gap
 * S = dot-dot-dot, then 2100ms gap before repeat
 */
void ledTask(void* parameter) {
    logMessage(LOG_LEVEL_INFO, "LED", "LED task started");
    
    // Initialize GPIO
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    // SOS sequence: each element is (duration_ms, state)
    // S: dot, dot, dot (300 each) with 300ms gaps = 1800ms total
    // O: dash, dash, dash (900 each) with 300ms gaps = 3600ms total
    // Pattern cycles every 11100ms
    
    const uint16_t sosSequence[][2] = {
        // Letter 'S' - dot-dot-dot
        {300, 1},   // dot ON
        {300, 0},   // symbol gap OFF
        {300, 1},   // dot ON
        {300, 0},   // symbol gap OFF
        {300, 1},   // dot ON
        {900, 0},   // letter gap OFF (includes symbol gap)
        
        // Letter 'O' - dash-dash-dash
        {900, 1},   // dash ON
        {300, 0},   // symbol gap OFF
        {900, 1},   // dash ON
        {300, 0},   // symbol gap OFF
        {900, 1},   // dash ON
        {900, 0},   // letter gap OFF (includes symbol gap)
        
        // Letter 'S' - dot-dot-dot
        {300, 1},   // dot ON
        {300, 0},   // symbol gap OFF
        {300, 1},   // dot ON
        {300, 0},   // symbol gap OFF
        {300, 1},   // dot ON
        {2100, 0},  // cycle gap OFF (before repeat)
    };
    
    const uint16_t seqLen = sizeof(sosSequence) / sizeof(sosSequence[0]);
    uint16_t seqIndex = 0;
    
    while (1) {
        if (seqIndex >= seqLen) {
            seqIndex = 0;
            logMessage(LOG_LEVEL_DEBUG, "LED", "SOS cycle restart");
        }
        
        uint16_t duration = sosSequence[seqIndex][0];
        uint8_t state = sosSequence[seqIndex][1];
        
        // Set GPIO
        digitalWrite(LED_PIN, state ? HIGH : LOW);
        logMessage(LOG_LEVEL_DEBUG, "LED", "GPIO=%d, duration=%u ms", state, duration);
        
        // Delay with task yield
        vTaskDelay(pdMS_TO_TICKS(duration));
        seqIndex++;
    }
}

// ============================================================================
// WIFI TASK - WiFi Connection & Maintenance
// ============================================================================

void wifiTask(void* parameter) {
    logMessage(LOG_LEVEL_INFO, "WIFI", "WiFi task started");
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    uint32_t lastRetryTime = getTimestamp();
    
    while (1) {
        if (WiFi.status() == WL_CONNECTED) {
            if (!isWiFiConnected) {
                isWiFiConnected = true;
                int32_t rssi = WiFi.RSSI();
                IPAddress ip = WiFi.localIP();
                logMessage(LOG_LEVEL_INFO, "WIFI", "Connected to %s, IP: %d.%d.%d.%d, RSSI: %d dBm",
                           WIFI_SSID, ip[0], ip[1], ip[2], ip[3], rssi);
                
                // Trigger automatic OTA update on boot
                if (otaTaskHandle != NULL) {
                    otaUpdateRequested = true;
                }
            }
        } else {
            if (isWiFiConnected) {
                isWiFiConnected = false;
                logMessage(LOG_LEVEL_ERROR, "WIFI", "Connection lost, status: %d", WiFi.status());
            }
            
            // Retry every 10 seconds
            uint32_t now = getTimestamp();
            if (now - lastRetryTime >= WIFI_RETRY_INTERVAL_MS) {
                lastRetryTime = now;
                logMessage(LOG_LEVEL_DEBUG, "WIFI", "Attempting connection to %s", WIFI_SSID);
                WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds
    }
}

// ============================================================================
// OTA TASK - Firmware Update via HTTP
// ============================================================================

/**
 * Download firmware via HTTP and verify SHA-256
 */
bool downloadAndVerifyFirmware(const char* url, uint8_t* firmwareBuffer, size_t maxSize, size_t& downloadedSize) {
    HTTPClient http;
    http.setTimeout(OTA_TIMEOUT_MS);
    http.begin(url);
    
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        logMessage(LOG_LEVEL_ERROR, "OTA", "HTTP error: %d", httpCode);
        http.end();
        return false;
    }
    
    int len = http.getSize();
    if (len <= 0) {
        logMessage(LOG_LEVEL_ERROR, "OTA", "Invalid content length");
        http.end();
        return false;
    }
    
    if (len > maxSize) {
        logMessage(LOG_LEVEL_ERROR, "OTA", "Firmware too large: %d > %zu", len, maxSize);
        http.end();
        return false;
    }
    
    logMessage(LOG_LEVEL_INFO, "OTA", "Downloading firmware, size: %d bytes", len);
    
    WiFiClient* stream = http.getStreamPtr();
    size_t written = 0;
    uint8_t buffer[512];
    
    while (http.connected() && written < len) {
        size_t available = stream->available();
        if (available) {
            int c = stream->readBytes(buffer, min((int)sizeof(buffer), (int)(len - written)));
            if (c > 0) {
                memcpy(firmwareBuffer + written, buffer, c);
                written += c;
                logMessage(LOG_LEVEL_DEBUG, "OTA", "Downloaded %zu / %d bytes", written, len);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    http.end();
    
    if (written != len) {
        logMessage(LOG_LEVEL_ERROR, "OTA", "Incomplete download: %zu / %d", written, len);
        return false;
    }
    
    downloadedSize = len;
    
    // Verify SHA-256
    logMessage(LOG_LEVEL_INFO, "OTA", "Verifying SHA-256...");
    uint8_t computedHash[32];
    computeSHA256(firmwareBuffer, written, computedHash);
    
    uint8_t expectedHash[32];
    hexStrToBytes(OTA_EXPECTED_SHA256, expectedHash, 32);
    
    if (memcmp(computedHash, expectedHash, 32) != 0) {
        logMessage(LOG_LEVEL_ERROR, "OTA", "SHA-256 mismatch!");
        return false;
    }
    
    logMessage(LOG_LEVEL_INFO, "OTA", "SHA-256 verified successfully");
    return true;
}

void otaTask(void* parameter) {
    logMessage(LOG_LEVEL_INFO, "OTA", "OTA task started");
    
    // Allocate buffer for firmware (use PSRAM if available, or static allocation)
    const size_t MAX_FIRMWARE_SIZE = 2 * 1024 * 1024;  // 2 MB max
    uint8_t* firmwareBuffer = (uint8_t*)malloc(MAX_FIRMWARE_SIZE);
    
    if (firmwareBuffer == NULL) {
        logMessage(LOG_LEVEL_ERROR, "OTA", "Failed to allocate firmware buffer");
        vTaskDelete(NULL);
        return;
    }
    
    while (1) {
        // Wait for OTA request
        if (!otaUpdateRequested) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        otaUpdateRequested = false;
        
        // Check WiFi connection
        if (!isWiFiConnected) {
            logMessage(LOG_LEVEL_ERROR, "OTA", "WiFi not connected, aborting OTA");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        logMessage(LOG_LEVEL_INFO, "OTA", "Starting firmware update");
        
        // Suspend LED task to prevent interference
        if (ledTaskHandle != NULL) {
            vTaskSuspend(ledTaskHandle);
            logMessage(LOG_LEVEL_INFO, "OTA", "LED task suspended");
        }
        
        // Force LED off
        digitalWrite(LED_PIN, LOW);
        logMessage(LOG_LEVEL_DEBUG, "OTA", "LED forced OFF");
        
        // Download and verify
        size_t downloadedSize = 0;
        bool success = downloadAndVerifyFirmware(OTA_URL, firmwareBuffer, MAX_FIRMWARE_SIZE, downloadedSize);
        
        if (success) {
            logMessage(LOG_LEVEL_INFO, "OTA", "Firmware verified, writing to flash");
            
            // Disable interrupts and write to OTA partition
            if (Update.begin(downloadedSize, U_FLASH)) {
                if (Update.write(firmwareBuffer, downloadedSize) == downloadedSize) {
                    if (Update.end()) {
                        logMessage(LOG_LEVEL_INFO, "OTA", "Firmware flashed successfully");
                        logMessage(LOG_LEVEL_INFO, "OTA", "Reboot in 2 seconds...");
                        
                        // Resume LED task before reboot (though it won't matter)
                        if (ledTaskHandle != NULL) {
                            vTaskResume(ledTaskHandle);
                        }
                        
                        // Delay then reboot
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        ESP.restart();
                    } else {
                        logMessage(LOG_LEVEL_ERROR, "OTA", "Update.end() failed: %s", Update.errorString());
                    }
                } else {
                    logMessage(LOG_LEVEL_ERROR, "OTA", "Update.write() failed");
                }
            } else {
                logMessage(LOG_LEVEL_ERROR, "OTA", "Update.begin() failed");
            }
        } else {
            logMessage(LOG_LEVEL_ERROR, "OTA", "Firmware verification failed");
        }
        
        // Resume LED task
        if (ledTaskHandle != NULL) {
            vTaskResume(ledTaskHandle);
            logMessage(LOG_LEVEL_INFO, "OTA", "LED task resumed");
        }
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    free(firmwareBuffer);
}

// ============================================================================
// LOGGER TASK - Serial Output (Queue-Based)
// ============================================================================

void loggerTask(void* parameter) {
    logMessage(LOG_LEVEL_INFO, "LOGGER", "Logger task started");
    
    LogMessage msg;
    
    while (1) {
        // Block and wait for log message (100ms timeout)
        if (xQueueReceive(logQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            printLogMessage(&msg);
        }
    }
}

// ============================================================================
// SERIAL COMMAND PARSER - Main Loop
// ============================================================================

void parseSerialCommand(const String& command) {
    String cmd = command;
    cmd.trim();
    cmd.toUpperCase();
    
    if (cmd.length() == 0) {
        return;
    }
    
    if (cmd == "OTA_UPDATE") {
        logMessage(LOG_LEVEL_INFO, "SERIAL", "OTA update triggered");
        otaUpdateRequested = true;
    } else {
        logMessage(LOG_LEVEL_ERROR, "SERIAL", "Unknown command: %s", cmd.c_str());
    }
}

// ============================================================================
// SETUP & LOOP
// ============================================================================

void setup() {
    // Initialize serial
    Serial.begin(SERIAL_BAUD);
    delay(1000);
    
    bootTime = getTimestamp();
    
    // Create log queue
    logQueue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(LogMessage));
    if (logQueue == NULL) {
        Serial.println("ERROR: Failed to create log queue");
        return;
    }
    
    // Log startup message
    logMessage(LOG_LEVEL_INFO, "SETUP", "Firmware built 2026-02-27, version 1.0");
    logMessage(LOG_LEVEL_INFO, "SETUP", "ESP32-C3 @ %u MHz", getCpuFrequencyMhz());
    
    // Create logger task (must be before other logging tasks)
    xTaskCreatePinnedToCore(
        loggerTask,
        "loggerTask",
        LOGGER_TASK_STACK,
        NULL,
        TASK_PRIORITY_LOGGER,
        NULL,
        0
    );
    
    delay(100);
    logMessage(LOG_LEVEL_INFO, "SETUP", "Logger task created");
    
    // Create LED task
    xTaskCreatePinnedToCore(
        ledTask,
        "ledTask",
        LED_TASK_STACK,
        NULL,
        TASK_PRIORITY_LED,
        &ledTaskHandle,
        0
    );
    logMessage(LOG_LEVEL_INFO, "SETUP", "LED task created");
    
    // Create WiFi task
    xTaskCreatePinnedToCore(
        wifiTask,
        "wifiTask",
        WIFI_TASK_STACK,
        NULL,
        TASK_PRIORITY_WIFI,
        NULL,
        0
    );
    logMessage(LOG_LEVEL_INFO, "SETUP", "WiFi task created");
    
    // Create OTA task
    xTaskCreatePinnedToCore(
        otaTask,
        "otaTask",
        OTA_TASK_STACK,
        NULL,
        TASK_PRIORITY_OTA,
        &otaTaskHandle,
        0
    );
    logMessage(LOG_LEVEL_INFO, "SETUP", "OTA task created");
    
    logMessage(LOG_LEVEL_INFO, "SETUP", "System initialization complete");
}

void loop() {
    // Monitor serial for commands
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        parseSerialCommand(command);
    }
    
    delay(10);  // Yield to other tasks
}

/*
 * Notes for Deployment:
 * 
 * 1. Update WiFi credentials:
 *    - Replace WIFI_SSID and WIFI_PASSWORD with actual network credentials
 * 
 * 2. Set OTA URL and firmware hash:
 *    - Replace OTA_URL with the download URL for .bin firmware file
 *    - Calculate SHA-256 of the .bin file and replace OTA_EXPECTED_SHA256
 *    - Command to calculate hash on Linux/Mac: sha256sum firmware.bin
 *    - Command on Windows: certUtil -hashfile firmware.bin SHA256
 * 
 * 3. Build and upload:
 *    - pio run
 *    - pio run -t upload
 * 
 * 4. Monitor serial output:
 *    - pio device monitor --baud 115200
 * 
 * 5. Trigger OTA manually:
 *    - Send command: OTA_UPDATE\n
 *    - Or wait for automatic OTA on WiFi connection
 * 
 * 6. Task scheduling:
 *    - All tasks run on core 0 (xTaskCreatePinnedToCore with coreID=0)
 *    - Task priorities: LED(1) < LOGGER(2) < WiFi(3), OTA(3)
 *    - WiFi and OTA run at same priority; FreeRTOS time-shares them
 */