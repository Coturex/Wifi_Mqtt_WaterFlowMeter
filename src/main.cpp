/*
 * Copyright (C) 2021-2022 Coturex - F5RQG
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Wemos pin use : it's best to avoid GPIO 0, 2 and 15 (D3, D4, D8)
// D5 : waterFlow sensor
// D6 : 
// D1 : I2C clock - OLED
// D2 : I2C data  - OLED

// waterFlow Sensor : YF-B10
// F = 5 x Q  (L/min)
// 1L = 300Hz
// Q / 60 = L


#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>       // Mqtt lib
#include <SoftwareSerial.h>     // ESP8266/wemos requirement
#include <WiFiManager.h>        // Manage Wifi Access Point if wifi connect failure (todo : and mqtt failure)
#include "Adafruit_SSD1306.h"   // OLED requirement

#define USEOTA // enable On The Air firmware flash 
#ifdef USEOTA
#include "WebOTA.h"
#endif

//#include "myConfig_sample.h"  // Personnal settings - 'gited file'
//#include "myConfig.h"           // Personnal settings - Not 'gited file'
#include <EEPROM.h>             // EEPROM access...
#define FW_VERSION "1.1"
#define DOMO_TOPIC "domoticz/in" 

// I2C OLED screen stuff
#define OLED_RESET 0  // GPIO0
Adafruit_SSD1306 display(OLED_RESET); // Wemos I2C : D1-SCL D2-SDA

// WiFi + MQTT stuff.
WiFiClient espClient ;
PubSubClient mqtt_client(espClient);  
String cmdTopic;
String outTopic;
#define MQTT_RETRY 5     // How many retries before starting AccessPoint
#define PERIOD 500

bool DEBUG = true ;
bool TEST_CP         = false; // AP : always start the ConfigPortal, even if ap found
int  TESP_CP_TIMEOUT = 30;    // AP : AccessPoint timeout and leave AP
bool ALLOWONDEMAND   = true;  // AP : enable on demand - e.g Trigged on Gpio Input, On Mqtt Msg etc...

#define MAX_STRING_LENGTH 35         
struct { 
    char name[MAX_STRING_LENGTH] = "";
    char mqtt_server[MAX_STRING_LENGTH] = "";
    char mqtt_port[MAX_STRING_LENGTH] = "";
    char water_topic[MAX_STRING_LENGTH] = "";
    char water_id[MAX_STRING_LENGTH] = "";
    char idx_water[MAX_STRING_LENGTH] = "";
    int  domoPubTimer = 0;
    int  AP = 0 ;
  } settings;


WiFiManager wm;
WiFiManagerParameter custom_name("name", "Oled Title", "", 15);
WiFiManagerParameter custom_mqtt_server("mqtt_server", "mqtt IP server", "", 15);
WiFiManagerParameter custom_mqtt_port("mqtt_port", "mqtt Port", "", 4);
WiFiManagerParameter custom_water_topic("water_topic", "WaterFlow Topic", "", 35);
WiFiManagerParameter custom_water_id("water_id", "WaterFlow ID", "", 15);
WiFiManagerParameter custom_idx_water("idx_water", "Domoticz idx power", "", 4); 
WiFiManagerParameter custom_domoPubTimer("domoPubTimer", "Domoticz publish timer (s)", "", 4);

// domoPubTimer is Used to reduce telemetry sent to Domoticz/InfluxDb
//      if empty -> synchronise Domoticz publication on PERIOD sample rate
//      if x sec -> Domoticz publication every x seconds (Power is then averaged). 

int  pulseCounter ;
void pulseSensor() {
  pulseCounter ++ ;
}

void oled_cls(int size) {
    // OLED : set cursor on top left corner and clear
    display.clearDisplay();
    display.setTextSize(size);
    display.setTextColor(WHITE); // seems only WHITE exist on this oled model :(
    display.setCursor(0,0);
}

void saveWifiCallback() { // Save settings to EEPROM
    unsigned int addr=0 ;
    String srate_str ;
    Serial.println("[CALLBACK] saveParamCallback fired"); 
    strncpy(settings.name, custom_name.getValue(), MAX_STRING_LENGTH);  
    strncpy(settings.mqtt_server, custom_mqtt_server.getValue(), MAX_STRING_LENGTH);  
    strncpy(settings.mqtt_port, custom_mqtt_port.getValue(), MAX_STRING_LENGTH);  
    strncpy(settings.water_topic, custom_water_topic.getValue(), MAX_STRING_LENGTH);  
    strncpy(settings.water_id, custom_water_id.getValue(), MAX_STRING_LENGTH);  
    strncpy(settings.idx_water, custom_idx_water.getValue(), MAX_STRING_LENGTH);  
    char timerChar[MAX_STRING_LENGTH] = "";
    strncpy(timerChar, custom_domoPubTimer.getValue(), MAX_STRING_LENGTH);
    settings.domoPubTimer = String(timerChar).toInt() * 1000 ;

    settings.AP = 0 ;  
    EEPROM.put(addr, settings); //write data to array in ram 
    EEPROM.commit();  //write data from ram to flash memory. Do nothing if there are no changes to EEPROM data in ram
}

void read_Settings () { // From EEPROM
    unsigned int addr=0 ;  
    //Serial.println("[READ EEPROM] read_Settings");  
    EEPROM.get(addr, settings); //read data from array in ram and cast it to settings
    Serial.println("[READ EEPROM] Oled name : " + String(settings.name) ) ;
    Serial.println("[READ EEPROM] mqtt_server : " + String(settings.mqtt_server) ) ;
    Serial.println("[READ EEPROM] mqtt_port : " + String(settings.mqtt_port) ) ;
    Serial.println("[READ EEPROM] water_topic : " + String(settings.water_topic) ) ;
    Serial.println("[READ EEPROM] water_id : " + String(settings.water_id) ) ;
    Serial.println("[READ EEPROM] idx_water : " + String(settings.idx_water) ) ;
    Serial.println("[READ EEPROM] domoPubTimer   : " + String(settings.domoPubTimer) ) ;
    }

void wifi_connect () {
    // Wait for connection (even it's already done)
    while (WiFi.status() != WL_CONNECTED) {
        oled_cls(1);
        display.println("Connecting");
        display.println("wifi");
        display.display();
        delay(250);
        Serial.print(".");
        delay(250);
    }

    Serial.println("");
    Serial.print("Wifi Connected");
    Serial.println("");
    Serial.print("Connected to Network : ");
    Serial.println(WiFi.localIP());  //IP address assigned to ESP
    oled_cls(1);
    display.println("Wifi on");
    display.println(WiFi.localIP());
    display.display();
}

void setup_wifi () {
    delay(10);
    WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP   
    // WiFi.setSleepMode(WIFI_NONE_SLEEP); // disable sleep, can improve ap stability

    // wm.debugPlatformInfo();
    //reset settings - for testing
    // wm.resetSettings();
    // wm.erase();  

    // setup some parameters
    WiFiManagerParameter custom_html("<p>EEPROM Custom Parameters</p>"); // only custom html
    wm.addParameter(&custom_html);
    wm.addParameter(&custom_name);   
    wm.addParameter(&custom_mqtt_server);
    wm.addParameter(&custom_mqtt_port);
    wm.addParameter(&custom_water_topic);
    wm.addParameter(&custom_water_id);
    wm.addParameter(&custom_idx_water);
    wm.addParameter(&custom_domoPubTimer);
    // callbacks
    //wm.setAPCallback(configModeCallback);
    wm.setSaveConfigCallback(saveWifiCallback);
    wm.setBreakAfterConfig(true); // needed to use saveWifiCallback
    
    // set values later if I want
    // custom_html.setValue("test",4);
    // custom_token.setValue("test",4);

    //sets timeout until configuration portal gets turned off
    //useful to make it all retry or go to sleep in seconds
    wm.setConfigPortalTimeout(120); // run AccessPoint for 120s
    WiFi.printDiag(Serial);
    if(!wm.autoConnect("waterFlow_AP","admin")) {
        Serial.println("failed to connect and hit timeout");
    } 
    else if(TEST_CP or settings.AP) {
        // start configportal always
        delay(1000);
        wm.setConfigPortalTimeout(TESP_CP_TIMEOUT);
        switch (settings.AP) {
            case 1: 
                wm.startConfigPortal("request_waterFlow_AP");
                Serial.println("AP Config Portal : requested on topic/cmd");
                break ;
            case 2:    
                wm.startConfigPortal("mqtt_waterFlow_AP");
                Serial.println("AP Config Portal : mqtt connection failure");
                break ;
        } 
    }
    else {
        //Here connected to the WiFi
        Serial.println("connected...yeaaa :)");
    }
    WiFi.printDiag(Serial);
}

bool mqtt_connect(int retry) {
    bool ret = false ;
    while (!mqtt_client.connected() && WiFi.status() == WL_CONNECTED && retry) {
        String clientId = "waterFlow-"+String(settings. water_id);
        Serial.print("[mqtt_connect] (re)connecting (" + String(retry) + " left) ") ;
        retry--;
        Serial.println("[mqtt_connect]"+String(settings.mqtt_server)+":"+String(settings.mqtt_port)) ; 
        oled_cls(1);
        display.println("Connecting");
        display.println("mqtt - (" + String(retry)+")");  
        display.println("idx :" + String (settings.idx_water) );
        display.display();
        if (!mqtt_client.connect(clientId.c_str())) {
            ret = false ;
            delay(4000);
        } else {
            ret = true ;
            Serial.println("[mqtt_connect] Subscribing : "+ cmdTopic) ; 
            delay(2000);
            mqtt_client.subscribe(cmdTopic.c_str());
            delay(2000);
        }
    }
    return ret ;
}

void bootPub() {
        String  msg = "{\"type\": \"waterFlow\"";	
                msg += ", \"id\": ";
                msg += "\"" + String(settings.water_id) + "\"" ;
                msg += ", \"fw_version\": ";
                msg += "\"" + String(FW_VERSION) + "\"" ;
                msg += ", \"waterFlow_version\": ";
                msg += "\"YF-Bx\"" ;
                msg += ", \"water_idx1\": ";
                msg += "\"" + String(settings.idx_water) + "\"" ;
                msg += ", \"DomoPubTimer\": ";
                msg += "\"" + String(settings.domoPubTimer/1000) + "\"" ;
                msg += ", \"ip\": ";  
                msg += WiFi.localIP().toString().c_str() ;
                msg += "}" ;
        if (DEBUG) { Serial.println("Sending Bootstrap on topic : " + String(settings.water_topic));}
        mqtt_client.publish(String(settings.water_topic).c_str(), msg.c_str()); 
}

void domoPub(String idx, float value) {
    if (idx.toInt()> 0) {
      String msg = "{\"idx\": ";	 // {"idx": 209, "nvalue": 0, "svalue": "2052"}
      msg += idx;
      msg += ", \"nvalue\": 0, \"svalue\": \"";
      msg += value ;
      msg += "\"}";

      String domTopic = DOMO_TOPIC;             // domoticz topic
      if (DEBUG) {
        Serial.println("domoPub on topic : " + domTopic);
        Serial.println("domoPub : " + msg);
      }   
      mqtt_client.publish(domTopic.c_str(), msg.c_str()); 
    }
}

void statusPub(float value) {
    String msg ;
    msg = "{";
    msg += "\"counter\": ";
    msg += String(value) ;
    msg += "}";

    String topic = String(settings.water_topic)+"/"+String(settings.water_id) ;
    if (DEBUG) {
        Serial.println("statusPub on topic : " + topic);
        Serial.println("statusPub : " + msg);
      } 
    mqtt_client.publish(String(topic).c_str(), msg.c_str()); 
}

void rebootOnAP(int ap){
        Serial.println("Force Rebooting on Acess Point");
        settings.AP = ap ;
        unsigned int addr=0 ;
        EEPROM.put(addr, settings); //write data to array in ram 
        EEPROM.commit();  // write data from ram to flash memory. Do nothing if there are no changes to EEPROM data in ram
        ESP.restart();    // call AP directly doesn't works cleanly, a reboot is needed
}

void on_message(char* topic, byte* payload, unsigned int length) {
    if (DEBUG) { Serial.println("Receiving msg on topic : " + String(topic));}; 
    char buffer[length+1];
    memcpy(buffer, payload, length);
    buffer[length] = '\0';
    if (DEBUG) { Serial.println("  msg : {" + String(buffer) + "}");}; 
    if (String(buffer) == "bs") { // Bootstrap is requested
            if (DEBUG) { Serial.println("     Bootstrap resquested") ; } ;
            bootPub();
    } else if (String(buffer) == "ap") { // AccessPoint is requested
            if (DEBUG) { Serial.println("     AccessPoint resquested") ; } ;
            rebootOnAP(1);
    } else if (String(buffer) == "reboot") { // Reboot is requested
            if (DEBUG) { Serial.println("     Reboot resquested") ; } ;
            ESP.restart();
    } else if (String(buffer) == "reset") { // Reset waterFlow pulse counter
            if (DEBUG) { Serial.println("     Reset resquested") ; } ;
            pulseCounter = 0 ;
    } else if (String(buffer) == "counter") { // Request waterFlow pulse counter
            if (DEBUG) { Serial.println("     Counter value resquested") ; } ;
            domoPub(String(settings.idx_water),pulseCounter); 
    } else {
        if (DEBUG) { Serial.println("     Nothing to do") ; } ;
    }
}

void setup() {  
    randomSeed(micros());  // initializes the pseudo-random number generator
    Serial.begin(115200);

    // WaterFlow Sensor 
    pinMode(D5, INPUT) ;  // Connect D5 as Input to waterFlow sensor
    attachInterrupt (0,pulseSensor,RISING) ;

    // OLED Shield 
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3C (for the 64x48)
    display.display();
    if (DEBUG) {delay(10000);} 
    
    //load eeprom data (sizeof(settings) bytes) from flash memory into ram
    EEPROM.begin(sizeof(settings));
    Serial.println("EEPROM size: " + String(sizeof(settings)) + " bytes");
    read_Settings(); // read EEPROM

    setup_wifi() ;
    delay(5000) ;
    uint16_t port ;
    char *ptr ;
    port = strtoul(settings.mqtt_port,&ptr,10) ;
    cmdTopic = String(settings.water_topic) + "/" + String(settings.water_id) +"/cmd";  //e.g topic :regul/vload/id-00/cmd
    outTopic = String(settings.water_topic) + "/" + String(settings.water_id) ;         //e.g topic :regul/vload/id-00/

    mqtt_client.setServer(settings.mqtt_server, port); // data will be published
    mqtt_client.setCallback(on_message); // subscribing/listening mqtt cmdTopic
    // OTA 
    #ifdef USEOTA
    webota.init(8080,"/update"); // Init WebOTA server 
    #endif
}

unsigned long startLoopDomoPubTimer = millis();
unsigned long spendTimeDomoPubTimer ;
int countLoopDomoPubTimer = 0 ;
    
void loop() {
    unsigned long loopTime = millis() ;
    if (DEBUG) {Serial.println("--") ;} ;
    if (WiFi.status() != WL_CONNECTED) {
        wifi_connect();
    }
    if (!mqtt_client.connected() && WiFi.status() == WL_CONNECTED ) {
       if (mqtt_connect(MQTT_RETRY)) { 
           bootPub();
        } else {
            rebootOnAP(2);
        }
    }
    mqtt_client.loop(); // seems it blocks for 100ms
    
    countLoopDomoPubTimer++ ;
    if (settings.domoPubTimer > 0) { // domoPubTimer settings is not Empty
        spendTimeDomoPubTimer = millis() - startLoopDomoPubTimer ;
        if (DEBUG) { Serial.println("spendTimeDomoPubTimer : " + String(spendTimeDomoPubTimer)) ;} ; 
        if (spendTimeDomoPubTimer >=  settings.domoPubTimer) {
            domoPub(String(settings.idx_water),pulseCounter); 
            statusPub(pulseCounter);
            startLoopDomoPubTimer = millis();
            countLoopDomoPubTimer = 0 ;
            }
    } 

    #ifdef USEOTA
    webota.handle(); 
    webota.delay(PERIOD);
    #else
    delay(PERIOD) ;
    #endif

    if (DEBUG) {
        Serial.print("loop time (ms) : ") ;
        Serial.println((millis()-loopTime)); // print spare time in loop 
        }   
}

