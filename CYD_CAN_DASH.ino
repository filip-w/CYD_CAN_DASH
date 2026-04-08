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

// --- Data Structures ---
struct CANSignal {
  String name;
  uint32_t canId;
  int startBit;
  int bitLength;
  float factor;
  float offset;
  String unit;
  float currentValue;
  int displayY;
};

// Global Objects
TFT_eSPI tft = TFT_eSPI();
DynamicJsonDocument doc(16384);
std::vector<CANSignal> dashboard; // Dynamic list of signals
static bool driver_installed = false;

// --- Function Prototypes ---
void termPrint(String text);
void displaySystemInfo();
bool loadSystemConfig(const char* configFile);
bool loadDBCConfig(const char* filename);
void addSignalToList(String name, int yPos);
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

  uint32_t alerts_to_enable = TWAI_ALERT_RX_DATA | TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_QUEUE_FULL;
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
    delay(1000);
    return;
  }

  uint32_t alerts_triggered;
  twai_read_alerts(&alerts_triggered, pdMS_TO_TICKS(POLLING_RATE_MS));
  twai_status_info_t twaistatus;
  twai_get_status_info(&twaistatus);

  if (alerts_triggered & TWAI_ALERT_ERR_PASS)      Serial.println("Alert: Error passive.");
  if (alerts_triggered & TWAI_ALERT_BUS_ERROR)     Serial.println("Alert: Bus error.");
  if (alerts_triggered & TWAI_ALERT_RX_QUEUE_FULL) {
    Serial.println("Alert: RX queue full.");
    twai_clear_receive_queue();
  }
  
  if (alerts_triggered & TWAI_ALERT_RX_DATA) {
    twai_message_t rx_msg;
    if (twai_receive(&rx_msg, 0) == ESP_OK) {
      if (!(rx_msg.rtr)) {
        processIncomingMessage(rx_msg);
      }
    }
  }
  delay(100);
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
        addSignalToList(sName, yOffset);
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
  DeserializationError error = deserializeJson(doc, file, DeserializationOption::Filter(filter));
  file.close();
  return !error;
}

/**
 * @brief Retrieves metadata for a specific signal name from the loaded JSON document.
 */
bool getSignalData(String targetName, uint32_t &canId, int &startBit, int &bitLength, float &factor, float &offset, String &unit) {
  JsonArray params = doc["params"];
  for (JsonObject param : params) {
    JsonArray signals = param["signals"];
    for (JsonObject signal : signals) {
      if (signal["name"] == targetName) {
        canId    = param["canId"];
        startBit = signal["startBit"];
        bitLength = signal["bitLength"];
        factor   = signal["factor"];
        offset   = signal["offset"];
        unit     = signal["sourceUnit"].as<String>();
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
  for (CANSignal &sig : dashboard) {
    if (msg.identifier == sig.canId) {
      sig.currentValue = parseCANSignal(msg.data, sig.startBit, sig.bitLength, sig.factor, sig.offset);
      updateSignalDisplay(sig);
    }
  }
}

/**
 * @brief Refreshes the display text for a specific signal.
 */
void updateSignalDisplay(CANSignal &sig) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, sig.displayY);
  tft.printf("%-12s: %7.2f %s    ", sig.name.c_str(), sig.currentValue, sig.unit.c_str());
}

/**
 * @brief Initializes a CANSignal struct and adds it to the active tracking vector.
 */
void addSignalToList(String name, int yPos) {
  uint32_t cid;
  int sb, bl;
  float f, o;
  String u;
  if (getSignalData(name, cid, sb, bl, f, o, u)) {
    CANSignal newSig;
    newSig.name         = name;
    newSig.canId        = cid;
    newSig.startBit     = sb;
    newSig.bitLength    = bl;
    newSig.factor       = f;
    newSig.offset       = o;
    newSig.unit         = u;
    newSig.currentValue = 0.0;
    newSig.displayY     = yPos;
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