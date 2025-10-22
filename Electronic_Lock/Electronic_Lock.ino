#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Fingerprint.h>
#include <EEPROM.h>
#include <Keypad.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>

// ---------- Pin mapping ----------
#define RC522_SS   5
#define RC522_RST  25
#define OLED_SDA   21
#define OLED_SCL   22
#define FINGER_RX_PIN 16
#define FINGER_TX_PIN 17

// ---------- OLED ----------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------- RFID ----------
MFRC522 mfrc522(RC522_SS, RC522_RST);

// ---------- Fingerprint ----------
Adafruit_Fingerprint finger(&Serial2);

// ---------- EEPROM ----------
#define EEPROM_SIZE 512
#define UID_ADDR 0

// ---------- Keypad ----------
const byte ROWS = 5;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'A','B','#','*'},
  {'1','2','3','U'},
  {'4','5','6','D'},
  {'7','8','9','C'},
  {'L','0','R','E'}
};
byte rowPins[ROWS] = {13,12,14,27,26};
byte colPins[COLS] = {15,4,32,33};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---------- WiFi & MQTT ----------
const char* ssid = "VILLA KT 301";
const char* wifi_password = "buong301@";
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic = "esp32/lock";

WiFiClient espClient;
PubSubClient client(espClient);

// ---------- Password ----------
String passwordInput = "";
String correctPassword = "1234";

// ---------- Helper ----------
void readStoredUID(byte outUID[4]){
  for(int i=0;i<4;i++) outUID[i]=EEPROM.read(UID_ADDR+i);
}

bool uidEquals(const byte a[4], const byte b[4]){
  for(int i=0;i<4;i++) if(a[i]!=b[i]) return false;
  return true;
}

int getFingerprintID(){
  uint8_t p=finger.getImage();
  if(p==FINGERPRINT_NOFINGER) return -2;
  if(p!=FINGERPRINT_OK) return -2;
  p=finger.image2Tz();
  if(p!=FINGERPRINT_OK) return -2;
  p=finger.fingerFastSearch();
  if(p==FINGERPRINT_OK) return finger.fingerID;
  return -1;
}

String uidToHexString(const byte uid[4], byte len=4){
  String s="";
  for(byte i=0;i<len;i++){
    if(uid[i]<0x10) s+="0";
    s+=String(uid[i],HEX);
  }
  s.toUpperCase();
  return s;
}

String getTimestamp(){
  struct tm timeinfo;
  char buf[30];
  if(getLocalTime(&timeinfo)){
    strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",&timeinfo);
  } else {
    snprintf(buf,sizeof(buf),"uptime_%lus",millis()/1000);
  }
  return String(buf);
}

void publishUnlock(const String &msg){
  if(client.connected()){
    client.publish(mqtt_topic,msg.c_str());
    Serial.println("Published: "+msg);
  }
}

void postShowDelay(){
  delay(1500);
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("Ready to scan:");
  display.println("- RFID / Finger / Keypad");
  display.display();
}

void setup_wifi(){
  Serial.println("Connecting WiFi...");
  WiFi.begin(ssid,wifi_password);
  unsigned long start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<20000){
    delay(250); Serial.print(".");
  }
  if(WiFi.status()==WL_CONNECTED){
    Serial.println("\nWiFi connected: "+WiFi.localIP().toString());
  } else Serial.println("\nWiFi not connected");
}

void setup_time(){
  configTime(7*3600,0,"pool.ntp.org","time.google.com");
  struct tm timeinfo;
  if(getLocalTime(&timeinfo,5000)) Serial.println("NTP time OK");
  else Serial.println("NTP time fail");
}

void reconnectMQTT(){
  while(!client.connected()){
    Serial.print("Connecting MQTT...");
    String clientId="ESP32Client-"+String((uint16_t)random(0xffff),HEX);
    if(client.connect(clientId.c_str())){
      Serial.println("connected");
      break;
    } else {
      Serial.print("failed rc="); Serial.print(client.state()); Serial.println(" retry 5s");
      delay(5000);
    }
  }
}

// ---------- Keypad handler ----------
void handleKeypad(){
  char key=keypad.getKey();
  if(!key) return;
  if(key>='0' && key<='9') {if(passwordInput.length()<6) passwordInput+=key;}
  else if(key=='C'){passwordInput="";}
  else if(key=='E'){
    display.clearDisplay();
    display.setTextSize(2); display.setCursor(0,16);
    if(passwordInput==correctPassword){
      display.println("PASS OK"); display.display();
      String ts=getTimestamp();
      String msg="PASSWORD unlock "+ts;
      publishUnlock(msg);
    } else display.println("SAI PASS"); display.display();
    delay(1200); passwordInput="";
    postShowDelay();
  }
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(0,0);
  display.println("Nhap mat khau:");
  display.setTextSize(2); display.setCursor(0,20);
  for(unsigned int i=0;i<passwordInput.length();i++) display.print('*');
  display.display();
}

// ---------- Setup ----------
void setup(){
  Serial.begin(115200); delay(100);
  EEPROM.begin(EEPROM_SIZE);

  Wire.begin(OLED_SDA,OLED_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC,0x3C)){Serial.println("OLED not found!");while(1)delay(10);}
  display.clearDisplay(); display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1); display.setCursor(0,0);
  display.println("Init system..."); display.display();

  SPI.begin(); mfrc522.PCD_Init();
  Serial2.begin(57600,SERIAL_8N1,FINGER_RX_PIN,FINGER_TX_PIN);
  finger.begin(57600);
  if(finger.verifyPassword()) Serial.println("Fingerprint sensor found!"); else Serial.println("Fingerprint sensor not found!");

  setup_wifi(); setup_time();
  client.setServer(mqtt_server,mqtt_port);

  display.clearDisplay(); display.setCursor(0,0);
  display.println("Ready to scan:"); display.println("- RFID / Finger / Keypad");
  display.display();
}

// ---------- Loop ----------
void loop(){
  if(!client.connected()) reconnectMQTT();
  client.loop();

  // --- Check RFID ---
  if(mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()){
    byte readUID[4]={0};
    for(byte i=0;i<4 && i<mfrc522.uid.size;i++) readUID[i]=mfrc522.uid.uidByte[i];

    byte stored[4]; readStoredUID(stored);
    bool match=uidEquals(readUID,stored);

    display.clearDisplay(); display.setTextSize(2); display.setCursor(0,16);
    if(match){
      display.println("RFID: TRUNG"); display.display();
      String idHex=uidToHexString(readUID,4);
      String msg="RFID "+idHex+" unlock "+getTimestamp();
      publishUnlock(msg);
    } else { display.println("RFID: KHONG"); display.display();}
    mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
    postShowDelay();
  }

  // --- Check Fingerprint ---
  int f_id=getFingerprintID();
  if(f_id>=0){
    display.clearDisplay(); display.setTextSize(2); display.setCursor(0,8);
    display.print("Finger ID "); display.println(f_id); display.display();
    String msg="Finger ID "+String(f_id)+" unlock "+getTimestamp();
    publishUnlock(msg);
    postShowDelay();
  }

  // --- Keypad ---
  handleKeypad();

  delay(100);
}
