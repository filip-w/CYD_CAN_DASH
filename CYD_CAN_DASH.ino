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

// Increase this if you have a very large number of signals
// 16KB is usually safe for ESP32/Mega
DynamicJsonDocument doc(16384);

// A dynamic list to hold our signals
std::vector<CANSignal> dashboard;

static bool driver_installed = false;

TFT_eSPI tft = TFT_eSPI();  // Invoke library

void setup() {

  tft.init();
  tft.setRotation(0);

  // Start Serial:
  Serial.begin(115200);

  // Fill screen with black
  tft.fillScreen(TFT_BLACK);

  // Set "cursor" at top left corner of display (0,0) and select font 2
  // (cursor will move to next line automatically during printing with 'tft.println'
  //  or stay on the line is there is room for the text with tft.print)
  tft.setCursor(10, 0, 2);
  tft.setTextSize(2);
  // Set the font colour to be white with a black background, set text size multiplier to 1
  tft.setTextColor(TFT_GREEN,TFT_BLACK);  
  tft.println("CYD CAN DASH Project");
  tft.setCursor(10, 40, 2);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE,TFT_BLACK);
  tft.println("Setup");

  // Initialize configuration structures using macro initializers
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)TX_PIN, (gpio_num_t)RX_PIN, TWAI_MODE_NORMAL);
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();  //Look in the api-reference for other speed sets.
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  // Install TWAI driver
  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    Serial.println("Driver installed");
    tft.println("Driver installed");
  } else {
    Serial.println("Failed to install driver");
    tft.println("Failed to install driver");
    return;
  }

  tft.println("Searching for SD Card");

  if (!SD.begin()) {
    Serial.println("Card Mount Failed");
    return;
  }
    uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return;
  }

  Serial.print("SD Card Type: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }

  // 1. Load the file once
  //if (loadPDMConfig("/PDM.json")) {
    
    // 2. Extract whatever you need
    //uint32_t cid;
    //int sb, bl;
    //float f, o;

    // Populate our dashboard list from JSON metadata
    //addSignalToList("BatteryVoltage", 20);
    //addSignalToList("FuseTrippedCh1", 40);
   // if (!loadUserConfig("/configuration.json")) {
   //   Serial.println("Warning: Using default signal list.");
   // }
  //}

  // Single call to load the entire system configuration
  if (loadSystemConfig("/configuration.json")) {
    Serial.println("System Configured Successfully");
  } else {
    Serial.println("System Configuration Failed!");
    tft.setTextColor(TFT_RED);
    tft.println("Config Error!");
  }

  delay(2000);
  

  // Start TWAI driver
  if (twai_start() == ESP_OK) {
    Serial.println("Driver started");
    tft.println("Driver started");
  } else {
    Serial.println("Failed to start driver");
    tft.println("Failed to start driver");
    return;
  }

  // Reconfigure alerts to detect frame receive, Bus-Off error and RX queue full states
  uint32_t alerts_to_enable = TWAI_ALERT_RX_DATA | TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_QUEUE_FULL;
  if (twai_reconfigure_alerts(alerts_to_enable, NULL) == ESP_OK) {
    Serial.println("CAN Alerts reconfigured");
    tft.println("CAN Alerts reconfigured");
  } else {
    Serial.println("Failed to reconfigure alerts");
    tft.println("Failed to reconfigure alerts");
    return;
  }

  // TWAI driver is now successfully installed and started
  driver_installed = true;

  tft.fillScreen(TFT_BLACK);

}

/**
 * Reads configuration.json to find the reference to the PDM/DBC file,
 * then triggers the loading of that PDM file.
 */
bool loadSystemConfig(const char* configFile) {
  File file = SD.open(configFile);
  if (!file) {
    Serial.println("Error: Could not open configuration.json");
    return false;
  }

  // Use a temporary document to find the filename link
  StaticJsonDocument<512> tempDoc;
  DeserializationError error = deserializeJson(tempDoc, file);
  file.close();

  if (error) {
    Serial.print("Failed to parse configuration.json: ");
    Serial.println(error.f_str());
    return false;
  }

  // Extract the filename from the "external_references" object 
  const char* pdmPath = tempDoc["external_references"]["dbc_json_map"]; 
  
  if (pdmPath) {
    Serial.print("Found PDM link: ");
    Serial.println(pdmPath);
    
    // Prepend a slash if your SD library requires absolute paths
    String fullPath = "/";
    fullPath += pdmPath;

    // Run the existing loadPDMConfig function using the extracted name 
    if (loadPDMConfig(fullPath.c_str())) { 
      
      // Now that PDM data is loaded, populate the dashboard signals
      JsonArray signals = tempDoc["signals"]; 
      int yOffset = 20;

      for (JsonObject sig : signals) {
        String sName = sig["name"].as<String>(); 
        addSignalToList(sName, yOffset); 
        yOffset += 20; 
      }
      return true;
    }
  } else {
    Serial.println("Error: 'dbc_json_map' not found in config.");
  }

  return false;
}

/**
 * Loads the PDM configuration file from the SD card.
 * Uses a JSON filter to minimize memory usage by only parsing required fields.
 * @param filename Path to the .json file on the SD card.
 * @return True if file was opened and parsed successfully.
 */
bool loadPDMConfig(const char* filename) {
  File file = SD.open(filename);
  if (!file) {
    Serial.println("Error: Could not open file.");
    return false;
  }

  // OPTIONAL: Filter to save RAM. Only keeps the fields we actually use.
  StaticJsonDocument<200> filter;
  filter["params"][0]["canId"] = true;
  filter["params"][0]["signals"][0]["name"] = true;
  filter["params"][0]["signals"][0]["startBit"] = true;
  filter["params"][0]["signals"][0]["bitLength"] = true;
  filter["params"][0]["signals"][0]["factor"] = true;
  filter["params"][0]["signals"][0]["offset"] = true;
  filter["params"][0]["signals"][0]["sourceUnit"] = true;

  // Deserialize using the filter
  DeserializationError error = deserializeJson(doc, file, DeserializationOption::Filter(filter));
  file.close();

  if (error) {
    Serial.print("JSON Load Failed: ");
    Serial.println(error.f_str());
    return false;
  }
  return true;
}

/**
 * Loads the user-defined display configuration from configuration.json.
 * This determines which signals from the PDM.json are actually shown on screen.
 */
bool loadUserConfig(const char* filename) {
  File file = SD.open(filename);
  if (!file) {
    Serial.println("Error: Could not open configuration.json");
    return false;
  }

  // Allocate a temporary document for the user config
  // 2048 bytes is usually plenty for a list of signal names
  StaticJsonDocument<2048> userDoc;
  DeserializationError error = deserializeJson(userDoc, file);
  file.close();

  if (error) {
    Serial.print("User Config JSON Load Failed: ");
    Serial.println(error.f_str());
    return false;
  }

  JsonArray signals = userDoc["signals"];
  int yOffset = 20; // Starting Y position on the TFT

  for (JsonObject sig : signals) {
    String signalName = sig["name"].as<String>();
    
    // Add the signal to our active dashboard vector
    // We increment the Y position by 20 pixels for each new signal
    addSignalToList(signalName, yOffset);
    yOffset += 20; 

    Serial.print("Configured Display for: ");
    Serial.println(signalName);
  }

  return true;
}

bool getSignalData(String targetName, uint32_t &canId, int &startBit, int &bitLength, float &factor, float &offset, String &unit) {
  JsonArray params = doc["params"];
  
  for (JsonObject param : params) {
    JsonArray signals = param["signals"];
    
    for (JsonObject signal : signals) {
      if (signal["name"] == targetName) {
        canId = param["canId"]; // Taken from the parent param
        startBit = signal["startBit"];
        bitLength = signal["bitLength"];
        factor = signal["factor"];
        offset = signal["offset"];
        unit = signal["sourceUnit"].as<String>();


        // Debug print on one line
        Serial.print("DEBUG: Name: "); Serial.print(targetName);
        Serial.print(" | ID: 0x"); Serial.print(canId, HEX);
        Serial.print(" | Start: "); Serial.print(startBit);
        Serial.print(" | Len: "); Serial.print(bitLength);
        Serial.print(" | Factor: "); Serial.print(factor);
        Serial.print(" | Offset: "); Serial.println(offset);
        Serial.print(" | Unit: "); Serial.println(unit);

        return true;
      }
    }
  }
  return false;
}

static void handle_rx_message(twai_message_t &message) {
  // Process received message
  if (message.extd) {
    Serial.println("Message is in Extended Format");
  } else {
    Serial.println("Message is in Standard Format");
  }
  Serial.printf("ID: %lx\nByte:", message.identifier);
  if (!(message.rtr)) {
    for (int i = 0; i < message.data_length_code; i++) {
      Serial.printf(" %d = %02x,", i, message.data[i]);
    }
    Serial.println("");
  }
}


/**
 * Extracts and scales a specific signal from a raw 8-byte CAN data frame.
 * Implements Motorola (Big Endian) bit-order parsing and linear scaling.
 * Calculation: PhysicalValue = (Raw * Factor) + Offset
 * @param frameData Pointer to the 8-byte CAN data array.
 * @param startBit The starting bit position (Motorola MSB).
 * @param bitLength Number of bits comprising the signal.
 * @param factor The multiplier for scaling.
 * @param offset The value added after scaling.
 * @return The calculated physical value as a float.
 */
float parseCANSignal(const uint8_t* frameData, int startBit, int bitLength, float factor, float offset) {
    uint64_t rawValue = 0;
    Serial.println("--- Motorola Trace Start ---");

    for (int i = 0; i < bitLength; i++) {
        // Calculate the base byte from the start bit
        // Calculate the offset byte based on how many bits we've processed (i)
        int byteIdx = (startBit / 8) + (i / 8);
        
        // Motorola typically reads bits 7 down to 0 within each byte
        int bitInByte = 7 - (i % 8);

        // Debug line to monitor the pointer
        Serial.printf("Signal Bit[%2d] -> Looking at Byte[%d], Bit[%d]", i, byteIdx, bitInByte);

        if (byteIdx < 8) {
            bool bitValue = (frameData[byteIdx] & (1 << bitInByte)) != 0;
            Serial.printf(" | Value: %d\n", bitValue);
            
            if (bitValue) {
                // Shift the bits into the raw value (MSB first)
                rawValue |= (1ULL << (bitLength - 1 - i));
            }
        } else {
            Serial.println(" | ERROR: Pointer exceeded 8-byte boundary!");
        }
    }

    float finalVal = ((float)rawValue * factor) + offset;
    Serial.printf("Raw Hex: 0x%llX | Scaled Value: %.2f\n", rawValue, finalVal);
    Serial.println("--- Motorola Trace End ---");
    
    return finalVal;
}

// TWAI Reception Function
// This function checks if a message is waiting in the hardware buffer and returns true only if the ID matches your target.
bool getTWAIFrame(twai_message_t &message, uint32_t targetID) {
    if (twai_receive(&message, pdMS_TO_TICKS(1)) == ESP_OK) {
        return (message.identifier == targetID);
    }
    return false;
}

/**
 * Core logic for handling received TWAI frames.
 * Compares incoming IDs against the registered dashboard signals and 
 * triggers an update for any matching data.
 * @param msg Reference to the received TWAI message structure.
 */
void processIncomingMessage(twai_message_t &msg) {
    // Iterate through the vector using a reference to avoid copying
    for (CANSignal &sig : dashboard) {
        if (msg.identifier == sig.canId) {
            sig.currentValue = parseCANSignal(
                msg.data, 
                sig.startBit, 
                sig.bitLength, 
                sig.factor, 
                sig.offset
            );
            
            // Re-use the display function from before
            updateSignalDisplay(sig);
        }
    }
}

// Auto-populating the Display
// Instead of manual coordinates, this function uses the properties stored inside the signal struct.
void updateSignalDisplay(CANSignal &sig) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, sig.displayY);
    
    // Using fixed-width formatting to prevent "jumping" text
    // %-10s = Left aligned string, 10 chars wide
    // %7.2f = Float with 2 decimal places, 7 chars wide total
    tft.printf("%s: %7.2f %s    ", sig.name.c_str(), sig.currentValue, sig.unit.c_str());
}

/**
 * Bridges the JSON configuration and the active dashboard list.
 * Searches the JSON document for a signal name, retrieves its parameters,
 * and appends a new CANSignal object to the global dashboard vector.
 * @param name The "targetName" string to look for in the JSON.
 * @param yPos The vertical pixel coordinate for this signal on the TFT.
 */
void addSignalToList(String name, int yPos) {
    uint32_t cid;
    int sb, bl;
    float f, o;
    String u;

    // Your function call that reads from JSON
    // Assuming it returns true if the signal was found
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
    // Driver not installed
    delay(1000);
    return;
  }
  // Check if alert happened
  uint32_t alerts_triggered;
  twai_read_alerts(&alerts_triggered, pdMS_TO_TICKS(POLLING_RATE_MS));
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

    // Flush the software RX queue
    esp_err_t err = twai_clear_receive_queue();
    if (err == ESP_OK) {
      Serial.println("RX Queue cleared.");
    }
  }
  
  // Set "cursor" at top left corner of display (0,0) and select font 2
  // (cursor will move to next line automatically during printing with 'tft.println'
  //  or stay on the line is there is room for the text with tft.print)
  tft.setCursor(0, 0, 2);

    // Check if message is received
  if (alerts_triggered & TWAI_ALERT_RX_DATA) {
  twai_message_t rx_msg;
      
      // Check if a message was received (non-blocking)
      if (twai_receive(&rx_msg, 0) == ESP_OK) {
          // Standard data frame check
          if (!(rx_msg.rtr)) {
              processIncomingMessage(rx_msg);
          }
      }
    }
  
  delay(100);
}
