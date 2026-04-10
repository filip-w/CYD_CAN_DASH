/*
 * Project:     CYD CAN DASH
 * Description: A dynamic CAN bus dashboard for the "Cheap Yellow Display" (ESP32).
 * Features:    SD-card based JSON configuration, real-time Motorola bit-parsing, 
 * and an auto-updating TFT interface.
 * Hardware:    ESP32-2432S028 (CYD), CAN Transceiver (e.g., SN65HVD230)
 * Author:      [Filip-w]
 * Date:        April 2026
 */

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "driver/twai.h"
#include <TFT_eSPI.h>
#include <SPI.h>
#include "FS.h"
#include "SD.h"
#include <ArduinoJson.h>
#include <vector>

// --- Configuration & Pins ---
#define RX_PIN          27
#define TX_PIN          22
#define POLLING_RATE_MS 1000

// --- Display Settings ---
const int margin     = 5;
const int fontSize   = 1;
const int lineHeight = 18; // Height of each line in pixels
int cursorY          = 30;

struct CANSignal {
  String name;
  uint32_t canId;
  int startBit;
  int bitLength;
  float factor;
  float offset;
  float min; // Added to scale the bar
  float max; // Added to scale the bar
  String unit;
  float currentValue;
  int displayY;
  bool isBarChart; // Toggle for display mode
};

// Global Objects
TFT_eSPI tft = TFT_eSPI();
DynamicJsonDocument doc(16384);
std::vector<CANSignal> dashboard; // Dynamic list of signals
static bool driver_installed = false;

// --- Activity & Timeout Variables ---
unsigned long lastMessageMillis = 0;
unsigned long lastBlinkMillis   = 0;
bool showIcon                   = false;
const int blinkInterval         = 500; 
const int timeoutThreshold      = 1000; // 1 second timeout

// --- Function Prototypes ---
void termPrint(String text);
void displaySystemInfo();
bool loadSystemConfig(const char* configFile);
bool loadDBCConfig(const char* filename);
void addSignalToList(String name, String mode, int yPos);
void updateSignalDisplay(CANSignal &sig);
float parseCANSignal(const uint8_t* frameData, int startBit, int bitLength, float factor, float offset);

/**
 * @brief Initializes all hardware components and system drivers.
 * Sets up TFT, Serial, TWAI (CAN) driver, SD card, and loads initial configurations.
 */
void setup() {
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  
  Serial.begin(115200);

  tft.setCursor(10, 0, 2);
  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("[ CYD CAN DASH ]");

  tft.setCursor(0, 40, 2);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  
  displaySystemInfo();
  termPrint("--- Configuring System ---");

  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)TX_PIN, (gpio_num_t)RX_PIN, TWAI_MODE_NORMAL);
  twai_timing_config_t t_config  = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config  = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    Serial.println("Driver installed");
    termPrint("Driver installed");
  } else {
    Serial.println("Failed to install driver");
    termPrint("Failed to install driver");
    return;
  }

  if (!initSDCard()) {
    tft.setTextColor(TFT_RED);
    termPrint("Critical SD Error!");
    return;
  }

  if (loadSystemConfig("/configuration.json")) {
    Serial.println("System Configured Successfully");
  } else {
    Serial.println("System Configuration Failed!");
    tft.setTextColor(TFT_RED);
    termPrint("Config Error!");
  }

  if (twai_start() == ESP_OK) {
    Serial.println("Driver started");
    termPrint("Driver started");
  } else {
    Serial.println("Failed to start driver");
    termPrint("Failed to start driver");
    return;
  }

uint32_t alerts_to_enable = TWAI_ALERT_RX_DATA | TWAI_ALERT_ERR_ACTIVE | 
                            TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_ERROR | 
                            TWAI_ALERT_TX_FAILED | TWAI_ALERT_RX_QUEUE_FULL;

  if (twai_reconfigure_alerts(alerts_to_enable, NULL) == ESP_OK) {
    Serial.println("CAN Alerts reconfigured");
    termPrint("CAN Alerts reconfigured");
  }

  driver_installed = true;
  delay(2000);
  tft.fillScreen(TFT_BLACK);
}

/**
 * @brief Main execution loop.
 * Monitors CAN bus status, handles alerts, and triggers processing of new RX data.
 */
void loop() {
  if (!driver_installed) {
    Serial.println("Driver not installed");
    delay(1000);
    return;
  }

  updateActivityStatus();

  uint32_t alerts_triggered;
  // Use a short timeout to keep the loop responsive
  twai_read_alerts(&alerts_triggered, pdMS_TO_TICKS(10));
  twai_status_info_t twaistatus;
  twai_get_status_info(&twaistatus);

  // Handle alerts
  if (alerts_triggered & TWAI_ALERT_ERR_PASS) {
    Serial.println("Alert: TWAI controller has become error passive.");
  }
  if (alerts_triggered & TWAI_ALERT_BUS_ERROR) {
    Serial.println("Alert: A (Bit, Stuff, CRC, Form, ACK) error has occurred on the bus.");
    Serial.printf("Bus error count: %lu\n", twaistatus.bus_error_count);
  }
  if (alerts_triggered & TWAI_ALERT_RX_QUEUE_FULL) {
    Serial.println("Alert: The RX queue is full causing a received frame to be lost.");
    Serial.printf("RX buffered: %lu\t", twaistatus.msgs_to_rx);
    Serial.printf("RX missed: %lu\t", twaistatus.rx_missed_count);
    Serial.printf("RX overrun %lu\n", twaistatus.rx_overrun_count);
  }

  // Process all pending messages as fast as possible [cite: 34]
  twai_message_t message;
  while (twai_receive(&message, 0) == ESP_OK) {
    processIncomingMessage(message);
  }

  // Only update the screen every 100ms
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 100) {
    for (CANSignal &sig : dashboard) {
      updateSignalDisplay(sig); 
    }
    lastUpdate = millis();
  }
}

/**
 * @brief Prints logging text to the display in a terminal-like fashion.
 * Handles screen wrapping by resetting cursor and clearing space when full.
 */
void termPrint(String text) {
  if (cursorY + lineHeight > tft.height()) {
    delay(2000);
    tft.fillRect(0, 30, 320, 200, TFT_BLACK);
    cursorY = 30;
  }
  tft.drawString(text, margin, cursorY);
  cursorY += lineHeight;
}

/**
 * @brief Handles the top-right status icon and Serial diagnostics.
 * Priority: Red Cross (Hardware) > Red Square (Timeout) > Blinking Triangle (OK).
 */
/**
 * @brief Handles the top-right status icon and prints detailed TWAI status to Serial.
 * Now includes proactive checks for RX errors and Bus error counts.
 */
void updateActivityStatus() {
  unsigned long currentMillis = millis();
  twai_status_info_t status;
  
  if (twai_get_status_info(&status) != ESP_OK) return;

  // Define icon area 
  const int iconX = 300;
  const int iconY = 5;
  const int iconW = 18; 
  const int iconH = 18;

  // 1. PROACTIVE ERROR CHECK (Red Cross)
  // Trigger if state is failing OR RX/Bus errors are accumulating
  if (status.state >= TWAI_ERROR_PASSIVE || 
      status.rx_error_counter > 10) {
    
    tft.fillRect(iconX, iconY, iconW, iconH, TFT_BLACK); 
    tft.drawLine(iconX, iconY, iconX + 15, iconY + 15, TFT_RED);
    tft.drawLine(iconX + 15, iconY, iconX, iconY + 15, TFT_RED);
    return;
  }
  
  // 2. TIMEOUT CHECK (Orange Pause)
  else if (currentMillis - lastMessageMillis > timeoutThreshold) {
    tft.fillRect(iconX, iconY, iconW, iconH, TFT_BLACK);
    tft.fillRect(iconX + 2, iconY, 5, 15, TFT_ORANGE);
    tft.fillRect(iconX + 10, iconY, 5, 15, TFT_ORANGE);
    return;
  }

  // 3. ACTIVE CHECK (Blinking Green Triangle)
  else {
    if (currentMillis - lastBlinkMillis >= blinkInterval) {
      lastBlinkMillis = currentMillis;
      showIcon = !showIcon;
      tft.fillRect(iconX, iconY, iconW, iconH, TFT_BLACK);
      if (showIcon) {
        tft.fillTriangle(iconX, iconY, iconX, iconY + 15, iconX + 15, iconY + 7, TFT_GREEN);
      }
    }
  }
}

/**
 * @brief Displays diagnostic chip and memory information to the screen.
 */
void displaySystemInfo() {
  uint32_t chipId = 0;
  for (int i = 0; i < 17; i = i + 8) {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }

  termPrint("--- SYSTEM INFO ---");
  char buffer[64];

  snprintf(buffer, sizeof(buffer), "Model: %s (Rev %d)", ESP.getChipModel(), ESP.getChipRevision());
  termPrint(buffer);
  snprintf(buffer, sizeof(buffer), "Cores: %d", ESP.getChipCores());
  termPrint(buffer);

  snprintf(buffer, sizeof(buffer), "Chip ID: %u", chipId);
  termPrint(buffer);
  snprintf(buffer, sizeof(buffer), "Flash: %u MB", (uint32_t)(ESP.getFlashChipSize() / (1024 * 1024)));
  termPrint(buffer);

  snprintf(buffer, sizeof(buffer), "Heap: %u KB", (uint32_t)(ESP.getHeapSize() / 1024));
  termPrint(buffer);
  termPrint("-------------------");
}

/**
 * @brief Reads system configuration from SD card to find DBC file paths and signal lists.
 */
bool loadSystemConfig(const char* configFile) {
  File file = SD.open(configFile);
  if (!file) {
    Serial.println("Error: Could not open configuration.json");
    return false;
  }

  StaticJsonDocument<512> tempDoc;
  DeserializationError error = deserializeJson(tempDoc, file);
  file.close();

  if (error) {
    Serial.printf("JSON Error: %s\n", error.c_str());
    return false;
  }

  const char* dbcPath = tempDoc["external_references"]["dbc_json_map"];
  if (dbcPath) {
    String fullPath = (dbcPath[0] == '/') ? String(dbcPath) : "/" + String(dbcPath);

    if (loadDBCConfig(fullPath.c_str())) {
      JsonArray signals = tempDoc["signals"];
      int yOffset = 0;

      termPrint("DBC loaded, adding signals:");
      for (JsonObject sig : signals) {
        String sName = sig["name"].as<String>();
        String sMode = sig["mode"].as<String>();
        addSignalToList(sName, sMode, yOffset);
        termPrint(sName);
        yOffset += 18;
      }
      return true;
    }
  }
  return false;
}

/**
 * @brief Loads the CAN signal mapping definitions (DBC data) from a JSON file.
 */
bool loadDBCConfig(const char* filename) {
  File file = SD.open(filename);
  if (!file) return false;

  StaticJsonDocument<200> filter;
  filter["params"][0]["canId"] = true;
  filter["params"][0]["signals"][0]["name"] = true;
  filter["params"][0]["signals"][0]["startBit"] = true;
  filter["params"][0]["signals"][0]["bitLength"] = true;
  filter["params"][0]["signals"][0]["factor"] = true;
  filter["params"][0]["signals"][0]["offset"] = true;
  filter["params"][0]["signals"][0]["sourceUnit"] = true;
  filter["params"][0]["signals"][0]["min"] = true;
  filter["params"][0]["signals"][0]["max"] = true;
  DeserializationError error = deserializeJson(doc, file, DeserializationOption::Filter(filter));
  file.close();
  return !error;
}

/**
 * @brief Retrieves metadata for a specific signal name from the loaded JSON document.
 */
bool getSignalData(String targetName, uint32_t &canId, int &startBit, int &bitLength, float &factor, float &offset, float &min, float &max, String &unit) {
  JsonArray params = doc["params"];
  for (JsonObject param : params) {
    JsonArray signals = param["signals"];
    for (JsonObject signal : signals) {
      if (signal["name"] == targetName) {
        canId     = param["canId"];
        startBit  = signal["startBit"];
        bitLength = signal["bitLength"];
        factor    = signal["factor"];
        offset    = signal["offset"];
        min       = signal["min"] | 0.0;     // Default to 0 if missing
        max       = signal["max"] | 100.0;   // Default to 100 if missing
        unit      = signal["sourceUnit"].as<String>();
        return true;
      }
    }
  }
  return false;
}

/**
 * @brief Decodes a value from a CAN frame using Motorola (Big Endian) bit-parsing logic.
 */
float parseCANSignal(const uint8_t* frameData, int startBit, int bitLength, float factor, float offset) {
  uint64_t rawValue = 0;
  for (int i = 0; i < bitLength; i++) {
    int byteIdx = (startBit / 8) + (i / 8);
    int bitInByte = 7 - (i % 8);
    if (byteIdx < 8) {
      if ((frameData[byteIdx] & (1 << bitInByte)) != 0) {
        rawValue |= (1ULL << (bitLength - 1 - i));
      }
    }
  }
  return ((float)rawValue * factor) + offset;
}

/**
 * @brief Filters incoming CAN messages and updates dashboard signals with new data.
 */
void processIncomingMessage(twai_message_t &msg) {
  // Reset the timeout timer 
  lastMessageMillis = millis();

  for (CANSignal &sig : dashboard) {
    if (msg.identifier == sig.canId) {
      sig.currentValue = parseCANSignal(msg.data, sig.startBit, sig.bitLength, sig.factor, sig.offset);
      //updateSignalDisplay(sig);
    }
  }
}

/**
 * @brief Refreshes the display text for a specific signal.
 */
void updateSignalDisplay(CANSignal &sig) {
  if (sig.isBarChart) {
    drawSignalBar(sig);
  } else {
    // 1. Set text color AND background color (TFT_BLACK)
    // This tells the library to draw a solid background for every character/space
    tft.setTextColor(TFT_WHITE, TFT_BLACK); 

    // 2. Draw the label at the margin [cite: 3, 91]
    tft.setCursor(margin, sig.displayY);
    tft.printf("%s ", sig.name.c_str());

    // 3. Jump to the 140px alignment point [cite: 93]
    tft.setCursor(140, sig.displayY);
    
    // 4. Use a fixed-width formatter with a minus sign (left-aligned)
    // %-10.2f ensures the value always occupies at least 10 characters.
    // If the number is "1.00", it adds 6 spaces at the end to "wipe" old digits.
    tft.printf("%-10.2f %s   ", sig.currentValue, sig.unit.c_str());
  }
}

void drawSignalBar(CANSignal &sig) {
  const int barWidth = 120;
  const int barHeight = 10;
  const int barX = 140; // Horizontal start of the bar

  // 1. Draw Signal Name
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(margin, sig.displayY);
  tft.printf("%-12s", sig.name.c_str());

  // 2. Calculate Fill (Clamp percentage between 0.0 and 1.0)
  float range = sig.max - sig.min;
  float percentage = (range > 0) ? (sig.currentValue - sig.min) / range : 0;
  if (percentage > 1.0) percentage = 1.0;
  if (percentage < 0.0) percentage = 0.0;
  
  int fillWidth = (int)(percentage * barWidth);

  // 3. Render Graphics
  tft.drawRect(barX, sig.displayY, barWidth, barHeight, TFT_WHITE); // Border
  tft.fillRect(barX + 1, sig.displayY + 1, fillWidth, barHeight - 2, TFT_GREEN); // Fill
  tft.fillRect(barX + 1 + fillWidth, sig.displayY + 1, barWidth - 2 - fillWidth, barHeight - 2, TFT_BLACK); // Background
}

/**
 * @brief Initializes a CANSignal struct and adds it to the active tracking vector.
 */
void addSignalToList(String name, String mode, int yPos) {
  uint32_t cid;
  int sb, bl;
  float f, o, mn, mx;
  String u;

  if (getSignalData(name, cid, sb, bl, f, o, mn, mx, u)) {
    CANSignal newSig;
    newSig.name         = name;
    newSig.canId        = cid;
    newSig.startBit     = sb;
    newSig.bitLength    = bl;
    newSig.factor       = f;
    newSig.offset       = o;
    newSig.min          = mn;
    newSig.max          = mx;
    newSig.unit         = u;
    newSig.currentValue = 0.0;
    newSig.displayY     = yPos;
    newSig.isBarChart   = (mode == "bar");
    dashboard.push_back(newSig);
  }
}

/**
 * @brief Mounts the SD card and prints detailed storage diagnostics.
 */
bool initSDCard() {
  termPrint("Searching for SD Card...");
  if (!SD.begin()) {
    termPrint("Card Mount Failed");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    termPrint("No SD card attached");
    return false;
  }

  const char* typeStr;
  switch (cardType) {
    case CARD_MMC:  typeStr = "MMC";    break;
    case CARD_SD:   typeStr = "SDSC";
                    break;
    case CARD_SDHC: typeStr = "SDHC";   break;
    default:        typeStr = "UNKNOWN"; break;
  }

  char buffer[64];
  snprintf(buffer, sizeof(buffer), "Type: %s", typeStr);
  termPrint(buffer);

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  snprintf(buffer, sizeof(buffer), "Capacity: %llu MB", cardSize);
  termPrint(buffer);

  uint64_t totalBytes = SD.totalBytes() / (1024 * 1024);
  uint64_t usedBytes  = SD.usedBytes() / (1024 * 1024);
  
  snprintf(buffer, sizeof(buffer), "Used: %llu / %llu MB", usedBytes, totalBytes);
  termPrint(buffer);
  return true;
}