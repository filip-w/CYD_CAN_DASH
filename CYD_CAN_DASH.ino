/*
 * Project: CYD CAN DASH
 * Description: A dynamic CAN bus dashboard for the "Cheap Yellow Display" (ESP32).
 * Features include SD-card based JSON configuration, real-time 
 * Motorola bit-parsing, and an auto-updating TFT interface.
 * Hardware: ESP32-2432S028 (CYD), CAN Transceiver (e.g., SN65HVD230)
 * Author: [Filip-w]
 * Date: April 2026
 */

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "driver/twai.h"
#include <TFT_eSPI.h> // Graphics and font library for ILI9341 driver chip
#include <SPI.h>
#include "FS.h"
#include "SD.h"
#include <ArduinoJson.h>
#include <vector>

// Pins used to connect to CAN bus transceiver:
#define RX_PIN 27
#define TX_PIN 22

// Interval:
#define POLLING_RATE_MS 1000

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

// Terminal state
int cursorY = 30;
const int margin = 5;      // Small left margin
const int fontSize = 1;
const int lineHeight = 18; // Height of each line in pixels

// Increase this if you have a very large number of signals
DynamicJsonDocument doc(16384);
// A dynamic list to hold our signals
std::vector<CANSignal> dashboard;

static bool driver_installed = false;

TFT_eSPI tft = TFT_eSPI();

void setup() {
  tft.init();
  tft.setRotation(1);

  // Start Serial:
  Serial.begin(115200);

  // Fill screen with black
  tft.fillScreen(TFT_BLACK);
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
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    Serial.println("Driver installed");
    termPrint("Driver installed");
  } else {
    Serial.println("Failed to install driver");
    termPrint("Failed to install driver");
    return;
  }

  termPrint("Searching for SD Card");

  if (!SD.begin()) {
    termPrint("Card Mount Failed");
    return;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    termPrint("No SD card attached");
    return;
  }

  const char* typeStr;
  switch (cardType) {
    case CARD_MMC:  typeStr = "MMC";    break;
    case CARD_SD:   typeStr = "SDSC";   break;
    case CARD_SDHC: typeStr = "SDHC";   break;
    default:        typeStr = "UNKNOWN"; break;
  }

  char buffer[32];
  snprintf(buffer, sizeof(buffer), "SD Card Type: %s", typeStr);
  termPrint(buffer);

  // Single call to load the entire system configuration
  if (loadSystemConfig("/configuration.json")) {
    Serial.println("System Configured Successfully");
  } else {
    Serial.println("System Configuration Failed!");
    tft.setTextColor(TFT_RED);
    termPrint("Config Error!");
  }

  // Start TWAI driver
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
  } else {
    Serial.println("Failed to reconfigure alerts");
    termPrint("Failed to reconfigure alerts");
    return;
  }

  driver_installed = true;

  delay(2000); // Pause for 2 seconds so the user can read the last lines
  tft.fillScreen(TFT_BLACK);
}

void termPrint(String text) {
  if (cursorY + lineHeight > tft.height()) {
    delay(2000); // Pause for 2 seconds so the user can read the last lines
    tft.fillRect(0, 30, 320, 200, TFT_BLACK);
    cursorY = 30;
  }
  tft.drawString(text, margin, cursorY);
  cursorY += lineHeight;
}

void displaySystemInfo() {
  uint32_t chipId = 0;
  for (int i = 0; i < 17; i = i + 8) {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }

  String model = ESP.getChipModel();
  int revision = ESP.getChipRevision();
  int cores = ESP.getChipCores();
  uint32_t flashSize = ESP.getFlashChipSize() / (1024 * 1024);
  uint32_t heapSize = ESP.getHeapSize() / 1024;

  termPrint("--- SYSTEM INFO ---");
  char buffer[50];
  sprintf(buffer, "Model: %s (Rev %d)", model.c_str(), revision);
  termPrint(buffer);
  sprintf(buffer, "Cores: %d", cores);
  termPrint(buffer);
  sprintf(buffer, "Chip ID: %u", chipId);
  termPrint(buffer);
  sprintf(buffer, "Flash: %u MB", flashSize);
  termPrint(buffer);
  sprintf(buffer, "Heap: %u KB", heapSize);
  termPrint(buffer);
  termPrint("-------------------");
}

/**
 * Reads configuration.json to find the reference to the DBC file,
 * then triggers the loading of that DBC file.
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
    Serial.print("Failed to parse configuration.json: ");
    Serial.println(error.f_str());
    return false;
  }

  const char* dbcPath = tempDoc["external_references"]["dbc_json_map"];
  if (dbcPath) {
    Serial.print("Found DBC link: ");
    Serial.println(dbcPath);
    String fullPath = "/";
    fullPath += dbcPath;

    // Run the loadDBCConfig function using the extracted name 
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
  } else {
    Serial.println("Error: 'dbc_json_map' not found in config.");
  }
  return false;
}

/**
 * Loads the DBC configuration file from the SD card.
 * Uses a JSON filter to minimize memory usage.
 */
bool loadDBCConfig(const char* filename) {
  File file = SD.open(filename);
  if (!file) {
    Serial.println("Error: Could not open DBC file.");
    return false;
  }

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

  if (error) {
    Serial.print("DBC JSON Load Failed: ");
    Serial.println(error.f_str());
    return false;
  }
  return true;
}

bool getSignalData(String targetName, uint32_t &canId, int &startBit, int &bitLength, float &factor, float &offset, String &unit) {
  JsonArray params = doc["params"];
  for (JsonObject param : params) {
    JsonArray signals = param["signals"];
    for (JsonObject signal : signals) {
      if (signal["name"] == targetName) {
        canId = param["canId"];
        startBit = signal["startBit"];
        bitLength = signal["bitLength"];
        factor = signal["factor"];
        offset = signal["offset"];
        unit = signal["sourceUnit"].as<String>();
        return true;
      }
    }
  }
  return false;
}

float parseCANSignal(const uint8_t* frameData, int startBit, int bitLength, float factor, float offset) {
    uint64_t rawValue = 0;
    for (int i = 0; i < bitLength; i++) {
        int byteIdx = (startBit / 8) + (i / 8);
        int bitInByte = 7 - (i % 8);
        if (byteIdx < 8) {
            bool bitValue = (frameData[byteIdx] & (1 << bitInByte)) != 0;
            if (bitValue) {
                rawValue |= (1ULL << (bitLength - 1 - i));
            }
        }
    }
    return ((float)rawValue * factor) + offset;
}

void processIncomingMessage(twai_message_t &msg) {
    for (CANSignal &sig : dashboard) {
        if (msg.identifier == sig.canId) {
            sig.currentValue = parseCANSignal(msg.data, sig.startBit, sig.bitLength, sig.factor, sig.offset);
            updateSignalDisplay(sig);
        }
    }
}

void updateSignalDisplay(CANSignal &sig) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, sig.displayY);
    tft.printf("%-12s: %7.2f %s    ", sig.name.c_str(), sig.currentValue, sig.unit.c_str());
}

void addSignalToList(String name, int yPos) {
    uint32_t cid;
    int sb, bl;
    float f, o;
    String u;
    if (getSignalData(name, cid, sb, bl, f, o, u)) {
        CANSignal newSig;
        newSig.name = name;
        newSig.canId = cid;
        newSig.startBit = sb;
        newSig.bitLength = bl;
        newSig.factor = f;
        newSig.offset = o;
        newSig.unit = u;
        newSig.currentValue = 0.0;
        newSig.displayY = yPos;
        dashboard.push_back(newSig);
    }
}

void loop() {
  if (!driver_installed) {
    delay(1000);
    return;
  }

  uint32_t alerts_triggered;
  twai_read_alerts(&alerts_triggered, pdMS_TO_TICKS(POLLING_RATE_MS));
  twai_status_info_t twaistatus;
  twai_get_status_info(&twaistatus);

  if (alerts_triggered & TWAI_ALERT_ERR_PASS) {
    Serial.println("Alert: TWAI controller has become error passive.");
  }
  if (alerts_triggered & TWAI_ALERT_BUS_ERROR) {
    Serial.println("Alert: Bus error occurred.");
  }
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