#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "driver/twai.h"

// Pins used to connect to CAN bus transceiver:
#define RX_PIN 27
#define TX_PIN 22

// Interval:
#define POLLING_RATE_MS 1000

static bool driver_installed = false;


#include <TFT_eSPI.h> // Graphics and font library for ILI9341 driver chip
#include <SPI.h>
#include "FS.h"
#include "SD.h"
#include <ArduinoJson.h>
#include <vector>

#define TFT_GREY 0x5AEB // New colour

TFT_eSPI tft = TFT_eSPI();  // Invoke library

// Increase this if you have a very large number of signals
// 16KB is usually safe for ESP32/Mega
DynamicJsonDocument doc(16384);

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

// A dynamic list to hold our signals
std::vector<CANSignal> dashboard;

void setup() {

  tft.init();
  tft.setRotation(1);

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
  if (loadPDMConfig("/PDM.json")) {
    
    // 2. Extract whatever you need
    uint32_t cid;
    int sb, bl;
    float f, o;

    // Populate our dashboard list from JSON metadata
    addSignalToList("BatteryVoltage", 20);
    addSignalToList("FuseTrippedCh1", 40);
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


// The Parsing Logic
// This function uses a "bit-mask and shift" approach. 
// It gathers the bits from the buffer, masks them to the correct length, 
// and then applies the linear transformation: $PhysicalValue = (Raw \times Factor) + Offset$.
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

// Processing the List
// Processing function iterates through the std::vector. The beauty of this is that it doesn't matter how many signals you added in setup(); the loop handles them all automatically.
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

// The Population Function
// This function acts as the bridge between your JSON getter and your internal list. You can call this for every signal you want to track.
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
