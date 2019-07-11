/**
   A BLE client for the Xiaomi Mi Plant Sensor, pushing measurements to an MQTT server.

   See https://github.com/nkolban/esp32-snippets/blob/master/Documentation/BLE%20C%2B%2B%20Guide.pdf
   on how bluetooth low energy and the library used are working.

   See https://github.com/ChrisScheffler/miflora/wiki/The-Basics for details on how the 
   protocol is working.
   
   MIT License

   Copyright (c) 2017 Sven Henkel
   Multiple units reading by Grega Lebar 2018

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

#include "BLEDevice.h"
#include <WiFi.h>
#include <PubSubClient.h>

#include "config.h"

// boot count used to check if battery status should be read
RTC_DATA_ATTR int bootCount = 0;

// device count
static int deviceCount = sizeof FLORA_DEVICES / sizeof FLORA_DEVICES[0];

// the remote service we wish to connect to
static BLEUUID serviceUUID("00001204-0000-1000-8000-00805f9b34fb");

// the characteristic of the remote service we are interested in
static BLEUUID uuid_version_battery("00001a02-0000-1000-8000-00805f9b34fb");
static BLEUUID uuid_sensor_data("00001a01-0000-1000-8000-00805f9b34fb");
static BLEUUID uuid_write_mode("00001a00-0000-1000-8000-00805f9b34fb");

TaskHandle_t hibernateTaskHandle = NULL;

WiFiClient espClient;
PubSubClient client(espClient);

typedef struct floraData {
  float temperature;
  int moisture;
  int light;
  int conductivity;
  int battery;
  bool success;
} floraData;

void connectWifi() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("");
}

void disconnectWifi() {
  WiFi.disconnect(true);
  Serial.println("WiFi disonnected");
}

void connectMqtt() {
  Serial.println("Connecting to MQTT...");
  client.setServer(MQTT_HOST, MQTT_PORT);

  while (!client.connected()) {
    if (!client.connect(MQTT_CLIENTID, MQTT_USERNAME, MQTT_PASSWORD)) {
      Serial.print("MQTT connection failed:");
      Serial.print(client.state());
      Serial.println("Retrying...");
      delay(MQTT_RETRY_WAIT);
    }
  }

  Serial.println("MQTT connected");
  Serial.println("");
}

void disconnectMqtt() {
  client.disconnect();
  Serial.println("MQTT disconnected");
}

BLEClient* getFloraClient(BLEAddress floraAddress) {
  BLEClient* floraClient = BLEDevice::createClient();

  if (!floraClient->connect(floraAddress)) {
    Serial.println("- Connection failed, skipping");
    return nullptr;
  }

  Serial.println("- Connection successful");
  return floraClient;
}

BLERemoteService* getFloraService(BLEClient* floraClient) {
  BLERemoteService* floraService = nullptr;

  try {
    floraService = floraClient->getService(serviceUUID);
  }
  catch (...) {
    // something went wrong
  }
  if (floraService == nullptr) {
    Serial.println("- Failed to find data service");
  }
  else {
    Serial.println("- Found data service");
  }

  return floraService;
}

bool forceFloraServiceDataMode(BLERemoteService* floraService) {
  BLERemoteCharacteristic* floraCharacteristic;
  
  // get device mode characteristic, needs to be changed to read data
  Serial.println("- Force device in data mode");
  floraCharacteristic = nullptr;
  try {
    floraCharacteristic = floraService->getCharacteristic(uuid_write_mode);
  }
  catch (...) {
    // something went wrong
  }
  if (floraCharacteristic == nullptr) {
    Serial.println("-- Failed, skipping device");
    return false;
  }

  // write the magic data
  uint8_t buf[2] = {0xA0, 0x1F};
  floraCharacteristic->writeValue(buf, 2, true);

  delay(500);
  return true;
}

bool readFloraDataCharacteristic(BLERemoteService* floraService, struct floraData* retData) {
  BLERemoteCharacteristic* floraCharacteristic = nullptr;

  // get the main device data characteristic
  Serial.println("- Access characteristic from device");
  try {
    floraCharacteristic = floraService->getCharacteristic(uuid_sensor_data);
  }
  catch (...) {
    // something went wrong
  }
  if (floraCharacteristic == nullptr) {
    Serial.println("-- Failed, skipping device");
    return false;
  }

  // read characteristic value
  Serial.println("- Read value from characteristic");
  std::string value;
  try{
    value = floraCharacteristic->readValue();
  }
  catch (...) {
    // something went wrong
    Serial.println("-- Failed, skipping device");
    return false;
  }
  const char *val = value.c_str();

  Serial.print("Hex: ");
  for (int i = 0; i < 16; i++) {
    Serial.print((int)val[i], HEX);
    Serial.print(" ");
  }
  Serial.println(" ");

  int16_t* temp_raw = (int16_t*)val;
  float temperature = (*temp_raw) / ((float)10.0);
  Serial.print("-- Temperature: ");
  Serial.println(temperature);

  int moisture = val[7];
  Serial.print("-- Moisture: ");
  Serial.println(moisture);

  int light = val[3] + val[4] * 256;
  Serial.print("-- Light: ");
  Serial.println(light);
 
  int conductivity = val[8] + val[9] * 256;
  Serial.print("-- Conductivity: ");
  Serial.println(conductivity);

  if ((temperature > 200) || (temperature < -100)) {
    Serial.println("-- Unreasonable values received, skip publish");
    return false;
  }

  retData->temperature = temperature;
  retData->moisture = moisture;
  retData->light = light;
  retData->conductivity = conductivity;

  return true;
}

bool readFloraBatteryCharacteristic(BLERemoteService* floraService, struct floraData* retData) {
  BLERemoteCharacteristic* floraCharacteristic = nullptr;

  // get the device battery characteristic
  Serial.println("- Access battery characteristic from device");
  try {
    floraCharacteristic = floraService->getCharacteristic(uuid_version_battery);
  }
  catch (...) {
    // something went wrong
  }
  if (floraCharacteristic == nullptr) {
    Serial.println("-- Failed, skipping battery level");
    return false;
  }

  // read characteristic value
  Serial.println("- Read value from characteristic");
  std::string value;
  try{
    value = floraCharacteristic->readValue();
  }
  catch (...) {
    // something went wrong
    Serial.println("-- Failed, skipping battery level");
    return false;
  }
  const char *val2 = value.c_str();
  int battery = val2[0];

  Serial.print("-- Battery: ");
  Serial.println(battery);
  retData->battery = battery;

  return true;
}

bool processFloraService(BLERemoteService* floraService, bool readBattery, struct floraData* retData) {
  // set device in data mode
  if (!forceFloraServiceDataMode(floraService)) {
    return false;
  }

  bool dataSuccess = readFloraDataCharacteristic(floraService, retData);

  bool batterySuccess = true;
  if (readBattery) {
    batterySuccess = readFloraBatteryCharacteristic(floraService, retData);
  }

  retData->success = dataSuccess && batterySuccess;
  return retData->success;
}

bool processFloraDevice(BLEAddress floraAddress, bool getBattery, int tryCount, struct floraData* retData) {
  Serial.print("Processing Flora device at ");
  Serial.print(floraAddress.toString().c_str());
  Serial.print(" (try ");
  Serial.print(tryCount);
  Serial.println(")");

  // connect to flora ble server
  BLEClient* floraClient = getFloraClient(floraAddress);
  if (floraClient == nullptr) {
    return false;
  }

  // connect data service
  BLERemoteService* floraService = getFloraService(floraClient);
  if (floraService == nullptr) {
    floraClient->disconnect();
    return false;
  }

  // process devices data
  bool success = processFloraService(floraService, getBattery, retData);

  // disconnect from device
  floraClient->disconnect();

  return success;
}

void hibernate() {
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION * 1000000ll);
  Serial.println("Going to sleep now.");
  delay(100);
  esp_deep_sleep_start();
}

void delayedHibernate(void *parameter) {
  delay(EMERGENCY_HIBERNATE*1000); // delay for five minutes
  Serial.println("Something got stuck, entering emergency hibernate...");
  hibernate();
}

void setup() {
  // all action is done when device is woken up
  Serial.begin(115200);
  delay(1000);

  // check if battery status should be read - based on boot count
  bool readBattery = ((bootCount % BATTERY_INTERVAL) == 0);

  Serial.print("Boot number: ");
  Serial.println(bootCount);

  // increase boot count
  bootCount++;

  // create a hibernate task in case something gets stuck
  xTaskCreate(delayedHibernate, "hibernate", 4096, NULL, 1, &hibernateTaskHandle);

  Serial.println("Initialize BLE client...");
  BLEDevice::init("");
  BLEDevice::setPower(ESP_PWR_LVL_P7);

  struct floraData* deviceData = (struct floraData*)malloc(deviceCount * sizeof(struct floraData));

  // process devices
  for (int i=0; i<deviceCount; i++) {
    int tryCount = 0;
    char* deviceMacAddress = FLORA_DEVICES[i];
    BLEAddress floraAddress(deviceMacAddress);

    while (tryCount < RETRY) {
      tryCount++;
      if (processFloraDevice(floraAddress, readBattery, tryCount, &(deviceData[i]))) {
        break;
      }
      delay(1000);
    }
    delay(1500);
  }

  // connecting wifi and mqtt server
  connectWifi();
  connectMqtt();

  delay(1000);

  for (int i=0; i<deviceCount; i++) {
    if (deviceData[i].success) {
      char* deviceMacAddress = FLORA_DEVICES[i];
      String baseTopic = MQTT_BASE_TOPIC + "/" + deviceMacAddress + "/";

      Serial.print("Publishing data for ");
      Serial.print(deviceMacAddress);
      Serial.print(" to ");
      Serial.println(baseTopic);

      char buffer[64];

      snprintf(buffer, 64, "%f", deviceData[i].temperature);
      client.publish((baseTopic + "temperature").c_str(), buffer);
      snprintf(buffer, 64, "%d", deviceData[i].moisture);
      client.publish((baseTopic + "moisture").c_str(), buffer);
      snprintf(buffer, 64, "%d", deviceData[i].light);
      client.publish((baseTopic + "light").c_str(), buffer);
      snprintf(buffer, 64, "%d", deviceData[i].conductivity);
      client.publish((baseTopic + "conductivity").c_str(), buffer);
      if (readBattery) {
        snprintf(buffer, 64, "%d", deviceData[i].battery);
        client.publish((baseTopic + "battery").c_str(), buffer);
      }
      delay(200);
    }
  }

  delay(1000);

  // disconnect wifi and mqtt
  disconnectWifi();
  disconnectMqtt();

  // delete emergency hibernate task
  vTaskDelete(hibernateTaskHandle);

  // go to sleep now
  hibernate();
}

void loop() {
  /// we're not doing anything in the loop, only on device wakeup
  delay(10000);
}
