/*
 * Adafruit Feather M4 CAN Transceiver Example
 */

#include <CANSAME5x.h>
#include <Adafruit_NeoPixel.h>

CANSAME5x CAN;

Adafruit_NeoPixel strip(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

#define MY_PACKET_ID 0xAF

uint32_t timestamp;

void setup() {
  Serial.begin(115200);
  //while (!Serial) delay(10);

  Serial.println("CAN NeoPixel Potentiometer RX/TX demo");

  pinMode(PIN_CAN_STANDBY, OUTPUT);
  digitalWrite(PIN_CAN_STANDBY, false); // turn off STANDBY
  pinMode(PIN_CAN_BOOSTEN, OUTPUT);
  digitalWrite(PIN_CAN_BOOSTEN, true); // turn on booster

  strip.begin();
  strip.setBrightness(50);

  // start the CAN bus at 500 kbps
  if (!CAN.begin(500000)) {
    Serial.println("Starting CAN failed!");
    while (1) delay(10);
  }

  timestamp = millis();
}

void loop() {
  // try to parse any incoming packet
  int packetSize = CAN.parsePacket();

  if (packetSize) {
    // received a packet
    Serial.print("Received ");

    if (CAN.packetExtended()) {
      Serial.print("extended ");
    }

    if (CAN.packetRtr()) {
      // Remote transmission request, packet contains no data
      Serial.print("RTR ");
    }

    Serial.print("packet with id 0x");
    Serial.print(CAN.packetId(), HEX);

    if (CAN.packetRtr()) {
      Serial.print(" and requested length ");
      Serial.println(CAN.packetDlc());
    } else {
      Serial.print(" and length ");
      Serial.println(packetSize);

      uint8_t receivedData[packetSize];
      for (int i=0; i<packetSize; i++) {
        receivedData[i] = CAN.read();
        Serial.print("0x");
        Serial.print(receivedData[i], HEX);
        Serial.print(", ");
      }
      Serial.println();

      uint8_t led_state = receivedData[0];
      strip.setPixelColor(0, led_state ? 0xff : 0, 0, 0);
      strip.show();
    }

    Serial.println();
  }
}
