/* ESP32 TWAI receive example.
  Receive messages and sends them over serial.

  Connect a CAN bus transceiver to the RX/TX pins.
  For example: SN65HVD230

  TWAI_MODE_LISTEN_ONLY is used so that the TWAI controller will not influence the bus.

  The API gives other possible speeds and alerts:
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/twai.html

  Example output from a can bus message:
  -> Message received
  -> Message is in Standard Format
  -> ID: 604
  -> Byte: 0 = 00, 1 = 0f, 2 = 13, 3 = 02, 4 = 00, 5 = 00, 6 = 08, 7 = 00

  Example output with alerts:
  -> Alert: A (Bit, Stuff, CRC, Form, ACK) error has occurred on the bus.
  -> Bus error count: 171
  -> Alert: The RX queue is full causing a received frame to be lost.
  -> RX buffered: 4  RX missed: 46 RX overrun 0

  created 05-11-2022 by Stephan Martin (designer2k2)
*/

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

#define TFT_GREY 0x5AEB // New colour

TFT_eSPI tft = TFT_eSPI();  // Invoke library

void setup() {

  tft.init();
  tft.setRotation(1);
  // Start Serial:
  Serial.begin(115200);

  // Fill screen with black
  tft.fillScreen(TFT_BLACK);

  tft.println("Setup");

  // Set "cursor" at top left corner of display (0,0) and select font 2
  // (cursor will move to next line automatically during printing with 'tft.println'
  //  or stay on the line is there is room for the text with tft.print)
  tft.setCursor(0, 0, 1);
  // Set the font colour to be white with a black background, set text size multiplier to 1
  tft.setTextColor(TFT_WHITE,TFT_BLACK);  

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
  delay(2000);
  tft.fillScreen(TFT_BLACK);
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
  }
  
  // Set "cursor" at top left corner of display (0,0) and select font 2
  // (cursor will move to next line automatically during printing with 'tft.println'
  //  or stay on the line is there is room for the text with tft.print)
  tft.setCursor(0, 0, 2);

    // Check if message is received
  if (alerts_triggered & TWAI_ALERT_RX_DATA) {
    // One or more messages received. Handle all.
    twai_message_t message;
    while (twai_receive(&message, 0) == ESP_OK) {
      handle_rx_message(message);
      tft.setCursor(0, 0, 2);
      tft.print("Byte 7:");
      tft.println(message.data[7]);
      tft.print("Byte 6:");
      tft.println(message.data[6]);
    }
  }
  
  delay(1000);
}
