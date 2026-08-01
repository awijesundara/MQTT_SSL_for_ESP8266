/*
IoT device firmware for SmartHome Gateway | originally 9/07/2019, modernized 2026
Anushka Wijesundara | MIT licensed | IoT device firmware for SmartHome Gateway

ESP8266 node that:
  - reads temperature/humidity from a DHT22 sensor
  - publishes readings over MQTT (TLS, via BearSSL) to a local broker
  - shows live status on an SSD1306 OLED display
  - listens for a firmware-update notification over MQTT and, if a newer
    version is announced, pulls a new binary over HTTP and flashes itself
    (ESP8266httpUpdate)

Libraries required (see platformio.ini for pinned versions):
  - ESP8266WiFi / ESP8266HTTPClient / ESP8266httpUpdate (ESP8266 Arduino core)
  - PubSubClient (Nick O'Leary)
  - ArduinoJson (Benoit Blanchon) - v7 API (JsonDocument)
  - DHT sensor library (Adafruit) + Adafruit Unified Sensor
  - esp8266-oled-ssd1306 (ThingPulse / Daniel Eichhorn / Fabrice Weinberg) for SSD1306Brzo
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <time.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include "SSD1306Brzo.h" // OLED display driver (I2C over the Brzo bus driver)

SSD1306Brzo display(0x3c, 5, 4); // I2C address 0x3C, SDA=GPIO5 (D1), SCL=GPIO4 (D2)

String WiFi_status_LCD_txt = "Connecting...";
String MQTT_status_LCD_txt = "Connecting...";
String Time_status_LCD_txt = "";
String Update_status_LCD_txt = "";

////--------------------------////
#define DHTPIN 12 // GPIO12 (D6)
#define DHTTYPE DHT22
#define STATUS_LED D0
DHT dht(DHTPIN, DHTTYPE);

const char ssid[] = "YOUR WIFI SSID";
const char pass[] = "YOUR WIFI PASSWORD";

#define HOSTNAME "IOT DEVICE HOSTNAME"

const char MQTT_HOST[] = "Smart-Home-Gateway.local";
const int MQTT_PORT = 8883;
const char MQTT_USER[] = "MQTT USERNAME"; // leave blank if no credentials used
const char MQTT_PASS[] = "MQTT PASSWORD"; // leave blank if no credentials used

const char IOT_FW_VER[] = "0.1.0"; // human-readable version string published over MQTT
#define FW_VERSION_NUMBER 1        // bump on every release; compared against the OTA payload
const int FW_VERSION = FW_VERSION_NUMBER;

const char *fwUrlBase = "http://Smart-Home-Gateway.local/firmwares/";

const char MQTT_SUB_TOPIC[] = "IoT/Firmware_Update/in";
const char MQTT_SUB_TOPIC_FW_UPDATE[] = "SH_Gateway/fw_update";
const char MQTT_PUB_TOPIC[] = HOSTNAME "/out";
const char MQTT_PUB_TOPIC_FW[] = HOSTNAME "/fw";

#define humidity_topic HOSTNAME "/humidity"
#define temperature_celsius_topic HOSTNAME "/temperature_c"
#define temperature_fahrenheit_topic HOSTNAME "/temperature_f"

/////////////// LOCAL ROOT CA - Manually Generated /////////////

static const char digicert[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----

YOUR SSL PUBLIC CERTIFICATE IN TEXT

-----END CERTIFICATE-----
)EOF";

//////////////////////////////////////////////////////////////////

BearSSL::WiFiClientSecure net;
PubSubClient client(net);

time_t now;
unsigned long lastPublishMillis = 0;

void checkForUpdates() {
  String mac = WiFi.macAddress();
  String fwURL = String(fwUrlBase);
  fwURL.concat(mac);
  String fwVersionURL = fwURL;
  fwVersionURL.concat(".version");

  Serial.println("Checking for firmware updates.");
  Serial.print("MAC address: ");
  Serial.println(mac);
  Serial.print("Firmware version URL: ");
  Serial.println(fwVersionURL);

  WiFiClient httpNet; // plain (non-TLS) client: the firmware server URL is http://, not https://
  HTTPClient httpClient;
  httpClient.begin(httpNet, fwVersionURL);
  int httpCode = httpClient.GET();
  if (httpCode == 200) {
    String newFWVersion = httpClient.getString();
    display.clear();
    LCD_txt_checking_for_update();
    drawText();
    display.display();
    Serial.print("Current firmware version: ");
    Serial.println(FW_VERSION);
    Serial.print("Available firmware version: ");
    Serial.println(newFWVersion);

    int newVersion = newFWVersion.toInt();

    if (newVersion > FW_VERSION) {
      Serial.println("Preparing to update");

      display.clear();
      LCD_txt_update_available();
      drawText();
      display.display();

      String fwImageURL = fwURL;
      fwImageURL.concat(".bin");
      t_httpUpdate_return ret = ESPhttpUpdate.update(httpNet, fwImageURL);

      switch (ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
          display.clear();
          LCD_txt_update_error();
          drawText();
          display.display();
          break;

        case HTTP_UPDATE_NO_UPDATES:
          Serial.println("HTTP_UPDATE_NO_UPDATES");
          display.clear();
          LCD_txt_update_error();
          drawText();
          display.display();
          break;
      }
    } else {
      Serial.println("Already on latest version");
      display.clear();
      LCD_txt_NO_update();
      drawText();
      display.display();
    }
  } else {
    Serial.print("Firmware version check failed, got HTTP response code ");
    Serial.println(httpCode);
    Serial.println("HTTP_UPDATE_NO_UPDATES");
    display.clear();
    LCD_txt_update_error();
    drawText();
    display.display();
  }
  httpClient.end();
}

void drawText() {
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "WiFi Status:");
  display.drawString(55, 0, WiFi_status_LCD_txt);
  display.drawString(0, 10, "MQTT Status:");
  display.drawString(65, 10, MQTT_status_LCD_txt);
  display.drawString(0, 20, "Last update:");
  display.drawString(0, 30, Time_status_LCD_txt);
  display.drawString(0, 40, "Firmware: ");
  display.drawString(45, 40, Update_status_LCD_txt);
  display.drawString(0, 50, "FW Ver: " + String(FW_VERSION));
}

void WIFI_OK() {
  WiFi_status_LCD_txt = " [ OK ]";
}

void MQTT_WAITING() {
  MQTT_status_LCD_txt = "Waiting...";
  Time_status_LCD_txt = "";
}

void MQTT_OK() {
  MQTT_status_LCD_txt = " [ OK ]";
}

void WIFI_CONNECTING() {
  WiFi_status_LCD_txt = "Connecting...";
}

void CURRENT_TIME() {
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  Time_status_LCD_txt = asctime(&timeinfo);
}

void LCD_txt_checking_for_update() {
  Update_status_LCD_txt = " [ Checking ]";
}

void LCD_txt_NO_update() {
  Update_status_LCD_txt = " [ Latest ]";
}

void LCD_txt_update_available() {
  Update_status_LCD_txt = " [ Preparing.. ]";
}

void LCD_txt_update_error() {
  Update_status_LCD_txt = " [ Error ! ]";
}

void mqtt_connect() {
  while (!client.connected()) {
    Serial.print("Time: ");
    Serial.print(ctime(&now));
    Serial.print("MQTT connecting ... ");
    if (client.connect(HOSTNAME, MQTT_USER, MQTT_PASS)) {
      Serial.println("connected.");
      display.clear();
      MQTT_OK();
      drawText();
      display.display();
      client.subscribe(MQTT_SUB_TOPIC);
      client.subscribe(MQTT_SUB_TOPIC_FW_UPDATE);
    } else {
      display.clear();
      MQTT_WAITING();
      drawText();
      display.display();
      Serial.print("failed, status code =");
      Serial.print(client.state());
      Serial.println(". Try again in 5 seconds.");
      /* Wait 5 seconds before retrying */
      delay(5000);
    }
  }
}

void receivedCallback(char *topic, byte *payload, unsigned int length) {
  JsonDocument doc; // ArduinoJson v7: replaces the fixed-capacity StaticJsonDocument
  deserializeJson(doc, payload, length);
  const int fw_version = doc["fw_version"];
  const char *fw_url = doc["fw_url"];
  Serial.println(topic);
  Serial.println(fw_version);
  Serial.println(fw_url);
  if (fw_version > FW_VERSION) {
    Serial.println("New version detected !");
    checkForUpdates();
  }
}

void setup() {
  display.init();
  Serial.begin(115200);
  Serial.println();
  Serial.println();
  Serial.print("Attempting to connect to SSID: ");
  Serial.print(ssid);
  Serial.print("  ");
  WiFi.hostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    display.clear();
    WIFI_CONNECTING();
    drawText();
    display.display();
    Serial.print(".");
    delay(1000);
  }
  display.clear();
  WIFI_OK();
  drawText();
  display.display();
  Serial.println("  CONNECTED !");
  Serial.print("IoT device IP -->  ");
  Serial.println(WiFi.localIP());
  Serial.println("");
  Serial.print("Firmware Version --> ");
  Serial.println(FW_VERSION);
  Serial.println("");
  Serial.print("IoT device MAC --> ");
  Serial.println(WiFi.macAddress());
  Serial.println("");
  Serial.print("Setting time using SNTP -->  ");
  configTime(+9 * 3600, 0, "Smart-Home-Gateway.local", "Vendor.local");
  now = time(nullptr);
  while (now < 1510592825) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("  OK");
  Serial.println("");
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  Serial.print("Current time: ");
  Serial.print(asctime(&timeinfo));
  pinMode(STATUS_LED, OUTPUT);

  ///////// ROOT CA - Manual ////////////
  BearSSL::X509List cert(digicert);
  net.setTrustAnchors(&cert);
  ///////////////////////////////////////

  client.setServer(MQTT_HOST, MQTT_PORT);
  client.setCallback(receivedCallback);
  mqtt_connect();
  checkForUpdates();
}

void loop() {
  display.clear();
  CURRENT_TIME();
  drawText();
  display.display();
  now = time(nullptr);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("Checking wifi");
    while (WiFi.waitForConnectResult() != WL_CONNECTED) {
      WiFi.begin(ssid, pass);
      Serial.print(".");
      delay(10);
    }
    Serial.println("connected");
  } else {
    if (!client.connected()) {
      mqtt_connect();
    } else {
      client.loop();
    }
  }

  if (millis() - lastPublishMillis > 5000) {
    lastPublishMillis = millis();
    client.publish(MQTT_PUB_TOPIC, ctime(&now), false);
    client.publish(MQTT_PUB_TOPIC_FW, IOT_FW_VER, false);

    // Reading temperature or humidity takes about 250 milliseconds!
    // Sensor readings may also be up to 2 seconds 'old' (it's a very slow sensor)
    float h = dht.readHumidity();
    float t = dht.readTemperature();        // Celsius
    float f = dht.readTemperature(true);    // Fahrenheit

    // Check if any reads failed and exit early (to try again).
    if (isnan(h) || isnan(t) || isnan(f)) {
      Serial.println("Failed to read from DHT sensor!");
      return;
    }

    Serial.print("Temperature in Celsius:");
    Serial.println(String(t).c_str());
    client.publish(temperature_celsius_topic, String(t).c_str(), true);

    Serial.print("Temperature in Fahrenheit:");
    Serial.println(String(f).c_str());
    client.publish(temperature_fahrenheit_topic, String(f).c_str(), true);

    Serial.print("Humidity:");
    Serial.println(String(h).c_str());
    client.publish(humidity_topic, String(h).c_str(), true);
  }

  digitalWrite(STATUS_LED, HIGH);
  delay(1000);
  digitalWrite(STATUS_LED, LOW);
  delay(30);
}
