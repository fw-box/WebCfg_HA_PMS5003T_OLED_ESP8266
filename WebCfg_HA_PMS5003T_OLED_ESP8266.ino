//
// Copyright (c) 2022 Fw-Box (https://fw-box.com)
// Author: Hartman Hsieh
//
// Description :
//   None
//
// Connections :
//
// Required Library :
//   https://github.com/fw-box/FwBox_Preferences
//   https://github.com/fw-box/FwBox_WebCfg
//   https://github.com/knolleary/pubsubclient
//   https://github.com/plapointe6/HAMqttDevice
//   https://github.com/fw-box/FwBox_PMSX003
//

#include <Wire.h>
#include "FwBox_PMSX003.h"
#include <U8g2lib.h>
#include "FwBox_UnifiedLcd.h"
#include "FwBox_WebCfg.h"
#include <PubSubClient.h>
#include "HAMqttDevice.h"

#define DEVICE_TYPE 37
#define FIRMWARE_VERSION "1.0.0"
#define VALUE_COUNT 5

#define PIN_A 34
#define ANALOG_MAX_VALUE 4096
#define ANALOG_VALUE_DEBOUNCING 8

#define VAL_PM1_0 0
#define VAL_PM2_5 1
#define VAL_PM10_0 2
#define VAL_TEMP 3
#define VAL_HUMI 4

#define TEXT_GAP 5
#define WORD_WIDTH 16
#define WORD_HEIGHT 16
#define LINE_HEIGHT 20

#define WEATHER_SYMBOL_BOTTOM 28 + (8*6)
#define BIG_TIME_BOTTOM 20 + (8*6)
#define DATE_TIME_BOTTOM (WEATHER_SYMBOL_BOTTOM + 16)
#define SMALL_ICON_LEFT 45
#define SMALL_ICON_BOTTOM 126

//
// WebCfg instance
//
FwBox_WebCfg WebCfg;

WiFiClient espClient;
PubSubClient MqttClient(espClient);
String MqttBrokerIp = "";
String MqttBrokerUsername = "";
String MqttBrokerPassword = "";
String DevName = "";

HAMqttDevice* HaDev = 0;
bool HAEnable = false;

//
// LCD 1602
//
FwBox_UnifiedLcd* Lcd = 0;

//
// OLED 128x128
//
U8G2_SSD1327_MIDAS_128X128_F_HW_I2C* u8g2 = 0;

//
// PMS5003T, PMS5003, PM3003
//
SoftwareSerial SerialSensor(13, 15); // RX:D7, TX:D8
FwBox_PMSX003 Pms(&SerialSensor);
//FwBox_PMSX003 Pms(&Serial2);

String RemoteMessage = "RemoteMessage";

//
// Set the unit of the values before "display".
//
// ValUnit[0] = "μg/m³";
// ValUnit[1] = "μg/m³";
// ValUnit[2] = "μg/m³";
// ValUnit[3] = "°C";
// ValUnit[4] = "%";
//
//String ValUnit[VALUE_COUNT] = {"μg/m³", "μg/m³", "μg/m³", "°C", "%"};
String ValUnit[VALUE_COUNT] = {"ug/m3", "ug/m3", "ug/m3", "C", "%"};
float Value[VALUE_COUNT] = {0.0,0.0,0.0,0.0,0.0};

unsigned long ReadingTime = 0;
unsigned long AttemptingMqttConnTime = 0;
int DisplayMode = 0;
int LastDisplayMode = -1;

void setup()
{
  //Wire.begin(400000);
  Wire.begin();
  Serial.begin(115200);
  WebCfg.earlyBegin();

  pinMode(LED_BUILTIN, OUTPUT);


  //
  // Initialize the LCD1602
  //
  Lcd = new FwBox_UnifiedLcd(16, 2);
  if (Lcd->begin() != 0) {
    //
    // LCD1602 doesn't exist, delete it.
    //
    delete Lcd;
    Lcd = 0;
    Serial.println("LCD1602 initialization failed.");
  }
  if (Lcd > 0) {
    delay(500);
    Lcd->setCursor(0, 0);
    Lcd->print("...             ");
    Lcd->setCursor(0, 1);
    Lcd->print("                ");
  }


  //
  // Detect the I2C address of OLED.
  //
  Wire.beginTransmission(0x78>>1);
  if (Wire.endTransmission() == 0) {
    u8g2 = new U8G2_SSD1327_MIDAS_128X128_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE);
    u8g2->begin();
    u8g2->enableUTF8Print();

    delay(100);
    u8g2->setFont(u8g2_font_unifont_t_chinese1);
    u8g2->setFontDirection(0);
    u8g2->clearBuffer();
    u8g2->drawUTF8(TEXT_GAP, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 0), "Hello...");
    u8g2->sendBuffer();
  }
  else {
    Serial.println("U8G2_SSD1327_MIDAS_128X128_F_HW_I2C is not found.");
    u8g2 = 0;
  }


  //
  // Initialize the PMSX003 Sensor
  //
  Pms.begin();
  
  //
  // Create 5 inputs in web page.
  //
  WebCfg.setItem(0, "MQTT Broker IP", "MQTT_IP"); // string input
  WebCfg.setItem(1, "MQTT Broker Username", "MQTT_USER"); // string input
  WebCfg.setItem(2, "MQTT Broker Password", "MQTT_PASS"); // string input
  WebCfg.setItem(3, "Home Assistant", "HA_EN_DIS", ITEM_TYPE_EN_DIS); // enable/disable select input
  WebCfg.setItem(4, "Device Name", "DEV_NAME"); // string input
  WebCfg.begin();

  //
  // Get the value of "MQTT_IP" from web input.
  //
  MqttBrokerIp = WebCfg.getItemValueString("MQTT_IP");
  Serial.printf("MQTT Broker IP = %s\n", MqttBrokerIp.c_str());

  MqttBrokerUsername = WebCfg.getItemValueString("MQTT_USER");
  Serial.printf("MQTT Broker Username = %s\n", MqttBrokerUsername.c_str());

  MqttBrokerPassword = WebCfg.getItemValueString("MQTT_PASS");
  Serial.printf("MQTT Broker Password = %s\n", MqttBrokerPassword.c_str());

  DevName = WebCfg.getItemValueString("DEV_NAME");
  Serial.printf("Device Name = %s\n", DevName.c_str());
  if (DevName.length() <= 0) {
    DevName = "fwbox_dust_sensor_";
    String str_mac = WiFi.macAddress();
    str_mac.replace(":", "");
    str_mac.toLowerCase();
    if (str_mac.length() >= 12) {
      DevName = DevName + str_mac.substring(8);
      Serial.printf("Auto generated Device Name = %s\n", DevName.c_str());
    }
  }

  //
  // Check the user input value of 'Home Assistant', 'Enable' or 'Disable'.
  //
  if (WebCfg.getItemValueInt("HA_EN_DIS", 0) == 1) {
    HAEnable = true;
    HaDev = new HAMqttDevice(DevName, HAMqttDevice::SENSOR, "homeassistant");

    //HaDev->enableCommandTopic();
    //HaDev->enableAttributesTopic();

    //HaDev->addConfigVar("device_class", "door")
    //HaDev->addConfigVar("retain", "false");

    Serial.println("HA Config topic : " + HaDev->getConfigTopic());
    Serial.println("HA Config payload : " + HaDev->getConfigPayload());
    Serial.println("HA State topic : " + HaDev->getStateTopic());
    Serial.println("HA Command topic : " + HaDev->getCommandTopic());
    Serial.println("HA Attributes topic : " + HaDev->getAttributesTopic());

    if (WiFi.status() == WL_CONNECTED) {
      //
      // Connect to Home Assistant MQTT broker.
      //
      HaMqttConnect(MqttBrokerIp, MqttBrokerUsername, MqttBrokerPassword, HaDev->getConfigTopic(), HaDev->getConfigPayload(), &AttemptingMqttConnTime);
    }
  }
  Serial.printf("Home Assistant = %d\n", HAEnable);


  //
  // Display the screen
  //
  display(analogRead(PIN_A)); // Read 'PIN_A' to change the display mode.

} // void setup()

void loop()
{
  bool refresh_display = false;
  
  WebCfg.handle();
  
  if((millis() - ReadingTime) > 5000) { // Read and send the sensor data every 5 seconds.
    //
    // Read the sensors
    //
    if(readSensor() == 0) { // Success
      Serial.println("Success to read sensor data.");

      /*Serial.print("#PM1.0=");
      Serial.println(Pms.pm1_0());
      Serial.print("#PM2.5=");
      Serial.println(Pms.pm2_5());
      Serial.print("#PM10=");
      Serial.println(Pms.pm10_0());
      Serial.print("#Temperature=");
      Serial.println(Pms.temp());
      Serial.print("#Humidity=");
      Serial.println(Pms.humi());*/
      Value[VAL_PM1_0] = Pms.pm1_0();
      Value[VAL_PM2_5] = Pms.pm2_5();
      Value[VAL_PM10_0] = Pms.pm10_0();
      Value[VAL_TEMP] = Pms.temp();
      Value[VAL_HUMI] = Pms.humi();

      Serial.printf("WL_CONNECTED=%d\n", WL_CONNECTED);
      Serial.printf("WiFi.status()=%d, MqttClient.connected()=%d\n", WiFi.status(), MqttClient.connected());

      //
      // If user enable the HA connection.
      //
      if (HAEnable == true) {
        if (WiFi.status() == WL_CONNECTED) {
          if (MqttClient.connected()) {
            String str_payload = "{";
            str_payload +=  "\"pm1_0\":";
            str_payload += (int)Value[VAL_PM1_0];
            str_payload += ",\"pm2_5\":";
            str_payload += (int)Value[VAL_PM2_5];
            str_payload += ",\"pm10_0\":";
            str_payload += (int)Value[VAL_PM10_0];
            if(Pms.readDeviceType() == FwBox_PMSX003::PMS5003T) {
              str_payload += ",\"temp\":";
              str_payload += (int)Value[VAL_TEMP];
              str_payload += ",\"humi\":";
              str_payload += (int)Value[VAL_HUMI];
            }
            str_payload += "}";
            Serial.println(HaDev->getStateTopic());
            Serial.println(str_payload);
            bool result_publish = MqttClient.publish(HaDev->getStateTopic().c_str(), str_payload.c_str());
            Serial.printf("result_publish=%d\n", result_publish);
          } // END OF "if (MqttClient.connected())"
          else {
            Serial.print("Try to connect MQTT broker - ");
            Serial.printf("%s, %s, %s\n", MqttBrokerIp.c_str(), MqttBrokerUsername.c_str(), MqttBrokerPassword.c_str());
            HaMqttConnect(MqttBrokerIp, MqttBrokerUsername, MqttBrokerPassword, HaDev->getConfigTopic(), HaDev->getConfigPayload(), &AttemptingMqttConnTime);
            Serial.println("Done");
          }
        } // END OF "if (WiFi.status() == WL_CONNECTED)"
      }
    }

    ReadingTime = millis();
  }

  //
  // Display the screen
  //
  display(analogRead(PIN_A)); // Read 'PIN_A' to change the display mode.

} // END OF "void loop()"

uint8_t readSensor()
{
  //
  // Running readPms before running pm2_5, temp, humi and readDeviceType.
  //
  if(Pms.readPms()) {
    if(Pms.readDeviceType() == FwBox_PMSX003::PMS5003T) {
      Serial.println("PMS5003T is detected.");
      if((Pms.pm1_0() == 0) && (Pms.pm2_5() == 0) && (Pms.pm10_0() == 0) && (Pms.temp() == 0) && (Pms.humi() == 0)) {
        Serial.println("PMS data format is wrong.");
      }
      else {
        Serial.print("PM1.0=");
        Serial.println(Pms.pm1_0());
        Serial.print("PM2.5=");
        Serial.println(Pms.pm2_5());
        Serial.print("PM10=");
        Serial.println(Pms.pm10_0());
        Serial.print("Temperature=");
        Serial.println(Pms.temp());
        Serial.print("Humidity=");
        Serial.println(Pms.humi());
        return 0; // Success
      }
    }
    else if(Pms.readDeviceType() == FwBox_PMSX003::PMS5003) {
      Serial.println("PMS5003 is detected.");
      if((Pms.pm1_0() == 0) && (Pms.pm2_5() == 0) && (Pms.pm10_0() == 0)) {
        Serial.println("PMS data format is wrong.");
      }
      else {
        Serial.print("PM1.0=");
        Serial.println(Pms.pm1_0());
        Serial.print("PM2.5=");
        Serial.println(Pms.pm2_5());
        Serial.print("PM10=");
        Serial.println(Pms.pm10_0());
        return 0; // Success
      }
    }
    else if(Pms.readDeviceType() == FwBox_PMSX003::PMS3003) {
      Serial.println("PMS3003 is detected.");
      if((Pms.pm1_0() == 0) && (Pms.pm2_5() == 0) && (Pms.pm10_0() == 0)) {
        Serial.println("PMS data format is wrong.");
      }
      else {
        Serial.print("PM1.0=");
        Serial.println(Pms.pm1_0());
        Serial.print("PM2.5=");
        Serial.println(Pms.pm2_5());
        Serial.print("PM10=");
        Serial.println(Pms.pm10_0());
        /*Serial.print("Temperature=");
        Serial.println(Pms.temp());
        Serial.print("Humidity=");
        Serial.println(Pms.humi());*/
        return 0; // Success
      }
    }
  }
  else {
    Serial.println("Failed to read PSMX003Y.");
  }

  return 1; // Error
}

void display(int analogValue)
{
  //
  // Draw the LCD1602
  //
  //Serial.printf("Lcd = 0x%x\n", Lcd);
  if(Lcd != 0) {
    char buff[32];
    //float pre_temp = 0.0;
    //float pre_humi = 0.0;
    static float pre_pm2_5 = 0.0;
    static float pre_temp = 0.0;
    static float pre_humi = 0.0;
    static int pre_second = 0;

    /*if (Pms.temp() != pre_temp || Pms.humi() != pre_humi) {
      memset(&(buff[0]), 0, 32);
      sprintf(buff, "%.2fC %.2f%%", Pms.temp(), Pms.humi());
      Serial.println(buff);
      Lcd->printAtCenter(0, buff); // Center the string.
    }*/

    if (Value[VAL_PM2_5] != pre_pm2_5 ||
        Value[VAL_TEMP] != pre_temp ||
        Value[VAL_HUMI] != pre_humi) {
      memset(&(buff[0]), 0, 32);
      sprintf(buff, "%d%s", (int)Value[VAL_PM2_5], ValUnit[VAL_PM2_5].c_str());
      //Serial.println(buff);
      Lcd->setCursor(0, 0);
      Lcd->printAtCenter(0, buff); // Center the string.

      if (Value[VAL_TEMP] != 0 || Value[VAL_HUMI] != 0) {
        memset(&(buff[0]), 0, 32);
        sprintf(buff, "%d%s %d%s", (int)Value[VAL_TEMP], ValUnit[VAL_TEMP].c_str(), (int)Value[VAL_HUMI], ValUnit[VAL_HUMI].c_str());
        //Serial.println(buff);
        Lcd->setCursor(0, 1);
        Lcd->printAtCenter(1, buff); // Center the string.
      }
      
      pre_pm2_5 = Value[VAL_PM2_5];
      pre_temp = Value[VAL_TEMP];
      pre_humi = Value[VAL_HUMI];
    }

    /*if (second() != pre_second) {
      memset(&(buff[0]), 0, 32);
      sprintf(buff, "%d-%02d-%02d %02d:%02d", year(), month(), day(), hour(), minute());
      Lcd->printAtCenter(0, buff); // Center the string.
      
      memset(&(buff[0]), 0, 32);
      sprintf(buff, "%02d", second());
      Lcd->setCursor(14, 1);
      Lcd->print(buff);
      
      pre_second = second();
    }*/
  }

  //
  // Draw the OLED
  //
  //Serial.printf("u8g2 = 0x%x\n", u8g2);
  if (u8g2 != 0) {
    //
    // Change the display mode according to the value of PIN - 'PIN_A'.
    //
    DisplayMode = getDisplayMode(4, analogValue);
    Serial.printf("analogValue=%d\n", analogValue);
    Serial.printf("DisplayMode=%d\n", DisplayMode);

    switch (DisplayMode) {
      case 1:
        OledDisplayType1();
        break;
      case 2:
        OledDisplayType2();
        break;
      case 3:
        OledDisplayType3();
        break;
      case 4:
        OledDisplayType4();
        break;
      default:
        OledDisplayType1();
        break;
    }
  }
}

void OledDisplayType1()
{

  int line_index = 0;
  u8g2->setFont(u8g2_font_unifont_t_chinese1);
  u8g2->setFontDirection(0);

  if (DisplayMode != LastDisplayMode) {
    u8g2->clearBuffer();
    
    u8g2->drawUTF8(TEXT_GAP, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 0), "PM1.0");
    u8g2->drawUTF8(TEXT_GAP, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 1), "PM2.5");
    u8g2->drawUTF8(TEXT_GAP, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 2), "PM10");
    u8g2->drawUTF8(TEXT_GAP, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 3), "溫度");
    u8g2->drawUTF8(TEXT_GAP, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 4), "溼度");
    
    /*int str_width = u8g2->getStrWidth("00.00");
    int text_left = TEXT_GAP + u8g2->getStrWidth("123456") + str_width;
    u8g2->drawUTF8(text_left, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 0), "μg/m³");
    u8g2->drawUTF8(text_left, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 1), "μg/m³");
    u8g2->drawUTF8(text_left, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 2), "μg/m³");
    u8g2->drawUTF8(text_left, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 3), "°C");
    u8g2->drawUTF8(text_left, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 4), "%");*/

    u8g2->sendBuffer();
    LastDisplayMode = DisplayMode;
  }
  else {
    int text_left = TEXT_GAP + u8g2->getStrWidth("123456");
    //u8g2->clearBuffer();
    int str_width = u8g2->getStrWidth("00.00 °C");
    char buff[32];
    u8g2->setDrawColor(0);
    u8g2->drawBox(text_left, 0, str_width, 128);
    u8g2->setDrawColor(1);
    sprintf(buff, "%d μg/m³", (int)Value[VAL_PM1_0]);
    u8g2->drawUTF8(text_left, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 0), buff);
    sprintf(buff, "%d μg/m³", (int)Value[VAL_PM2_5]);
    u8g2->drawUTF8(text_left, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 1), buff);
    sprintf(buff, "%d μg/m³", (int)Value[VAL_PM10_0]);
    u8g2->drawUTF8(text_left, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 2), buff);
    sprintf(buff, "%.2f °C", Value[VAL_TEMP]);
    u8g2->drawUTF8(text_left, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 3), buff);
    sprintf(buff, "%.2f %%", Value[VAL_HUMI]);
    u8g2->drawUTF8(text_left, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 4), buff);
    sprintf(buff, "%d", (millis()/1000));
    u8g2->drawUTF8(text_left, WORD_HEIGHT + TEXT_GAP + (LINE_HEIGHT * 5), buff);
    u8g2->sendBuffer();
    //u8g2->updateDisplayArea(text_left, 0, str_width, 128);
  }

  return;

}

void OledDisplayType2()
{
  u8g2->firstPage();
  do {
    //drawPage128X128Wether(u8g2, &WeatherResult, (WiFi.status() == WL_CONNECTED), (FwBoxIns.getServerStatus() == SERVER_STATUS_OK));

    u8g2->setFont(u8g2_font_unifont_t_chinese1);
    u8g2->setCursor(0, SMALL_ICON_BOTTOM - LINE_HEIGHT+3);
    u8g2->print("空氣品質");
    u8g2->setCursor(0, SMALL_ICON_BOTTOM);
    if(Pms.pm2_5() < 15)
      u8g2->print("良好");
    else if(Value[VAL_PM2_5] >= 15 && Value[VAL_PM2_5] < 35)
      u8g2->print("普通");
    else if(Value[VAL_PM2_5] >= 35 && Value[VAL_PM2_5] < 54)
      u8g2->print("不佳");
    else if(Value[VAL_PM2_5] >= 54 && Value[VAL_PM2_5] < 150)
      u8g2->print("糟糕");
    else
      u8g2->print("危害");
  }
  while (u8g2->nextPage());

  LastDisplayMode = DisplayMode;
}

void OledDisplayType3()
{
  u8g2->firstPage();
  do {
    //drawPage128X128Time(u8g2, &WeatherResult, (WiFi.status() == WL_CONNECTED), (FwBoxIns.getServerStatus() == SERVER_STATUS_OK));

    u8g2->setFont(u8g2_font_unifont_t_chinese1);
    u8g2->setCursor(0, SMALL_ICON_BOTTOM - LINE_HEIGHT+3);
    u8g2->print("空氣品質");
    u8g2->setCursor(0, SMALL_ICON_BOTTOM);
    if(Value[VAL_PM2_5] < 15)
      u8g2->print("良好");
    else if(Value[VAL_PM2_5] >= 15 && Value[VAL_PM2_5] < 35)
      u8g2->print("普通");
    else if(Value[VAL_PM2_5] >= 35 && Value[VAL_PM2_5] < 54)
      u8g2->print("不佳");
    else if(Value[VAL_PM2_5] >= 54 && Value[VAL_PM2_5] < 150)
      u8g2->print("糟糕");
    else
      u8g2->print("危害");
  }
  while (u8g2->nextPage());

  LastDisplayMode = DisplayMode;
}

void OledDisplayType4()
{
  u8g2->setFont(u8g2_font_unifont_t_chinese2);  // use chinese2 for all the glyphs of "你好世界"
  u8g2->setFontDirection(0);
  u8g2->clearBuffer();
  u8g2->setCursor(0, 15);
  u8g2->print("Hello World!");
  u8g2->setCursor(0, 40);
  u8g2->print("你好世界");    // Chinese "Hello World" 
  u8g2->sendBuffer();
  LastDisplayMode = DisplayMode;
}

int getDisplayMode(int pageCount,int analogValue)
{
  int page_width = ANALOG_MAX_VALUE / pageCount;

  for (int i = 0; i < pageCount; i++) {
    if (i == 0) {
      if (analogValue < (page_width*(i+1))-ANALOG_VALUE_DEBOUNCING) { // The value - '5' is for debouncing.
        return i + 1;
      }
    }
    else if (i == (pageCount - 1)) {
      if (analogValue >= (page_width*i)+ANALOG_VALUE_DEBOUNCING) { // The value - '5' is for debouncing.
        return i + 1;
      }
    }
    else {
      if ((analogValue >= (page_width*i)+ANALOG_VALUE_DEBOUNCING) && (analogValue < (page_width*(i+1))-ANALOG_VALUE_DEBOUNCING)) { // The value - '5' is for debouncing.
        return i + 1;
      }
    }
  }

  return 1; // default page
}

void PrintLcdDigits(int digits)
{
  if (digits < 10)
    Lcd->print('0');
  Lcd->print(digits);
}

int HaMqttConnect(const String& brokerIp, const String& brokerUsername, const String& brokerPassword, const String& configTopic, const String& configPayload, unsigned long* attemptingTime)
{
  if (millis() - *attemptingTime < 10 * 1000) {
    return 5; // attemptingTime is too short
  }

  if (brokerIp.length() > 0) {
    MqttClient.setServer(brokerIp.c_str(), 1883);
    MqttClient.setCallback(MqttCallback);

    if (!MqttClient.connected()) {
      Serial.print("Attempting MQTT connection...\n");
      Serial.print("MQTT Broker Ip : ");
      Serial.println(brokerIp);
      // Create a random client ID
      String str_mac = WiFi.macAddress();
      str_mac.replace(":", "");
      str_mac.toUpperCase();
      String client_id = "Fw-Box-";
      client_id += str_mac;
      Serial.println("client_id :" + client_id);
      // Attempt to connect
      *attemptingTime = millis();
      if (MqttClient.connect(client_id.c_str(), brokerUsername.c_str(), brokerPassword.c_str())) {
        Serial.println("connected");
      } else {
        Serial.print("failed, rc=");
        Serial.print(MqttClient.state());
        Serial.println(" try again in 5 seconds");
        return 1; // Failed
      }
    }

    if (MqttClient.connected()) {
      Serial.printf("configTopic.c_str()=%s\n", configTopic.c_str());
      Serial.printf("configPayload.c_str()=%s\n", configPayload.c_str());
      bool result_publish = MqttClient.publish(configTopic.c_str(), configPayload.c_str());
      Serial.printf("result_publish=%d\n", result_publish);
      return 0; // Success
    }
    else {
      return 3; // Still trying
    }
  } // END OF "if (brokerIp.length() > 0)"
  else {
    return 2; // Failed, broker IP is empty.
  }
}

//
// Callback function for MQTT subscribe.
//
void MqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
}
