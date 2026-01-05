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
// 1. CẤU HÌNH PHẦN CỨNG
#define RC522_SS   5
#define RC522_RST  25
#define OLED_SDA   21
#define OLED_SCL   22
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define FINGER_RX_PIN 16 
#define FINGER_TX_PIN 17 

// Keypad
const byte ROWS = 5; 
const byte COLS = 4; 
char keys[ROWS][COLS] = {
  {'A','B','#','*'},
  {'1','2','3','U'},
  {'4','5','6','D'},
  {'7','8','9','C'},
  {'L','0','R','E'}
};
byte rowPins[ROWS] = {13, 12, 14, 27, 26}; 
byte colPins[COLS] = {15, 4, 32, 33};
// 2. CONFIG WIFI & MQTT
const char* ssid          = "VILLA KT 301";
const char* wifi_password = "buong301@";
const char* mqtt_server   = "broker.hivemq.com";
const int   mqtt_port     = 1883;

// Topics
const char* mqtt_topic            = "esp32/lock";
const char* mqtt_rfid_add_topic   = "esp32/rfid_add";
const char* mqtt_finger_add_topic = "esp32/finger_add"; 
const char* mqtt_rfid_add_result  = "esp32/rfid_add_result";

// 3. EEPROM LAYOUT
#define EEPROM_SIZE 512
#define COUNT_ADDR 0
#define PASSWORD_ADDR 1
#define PASSWORD_LEN 5 
#define RECORDS_START 6
#define UID_LEN 4
#define RECORD_SIZE UID_LEN
#define MAX_USERS ((EEPROM_SIZE - RECORDS_START) / RECORD_SIZE)

// 4. OBJECTS & GLOBALS
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
MFRC522 mfrc522(RC522_SS, RC522_RST);
Adafruit_Fingerprint finger(&Serial2);
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
WiFiClient espClient;
PubSubClient client(espClient);

volatile bool wifiConnected = false;
volatile bool mqttConnected = false;
volatile bool addMode = false;       // RFID Add Flag
volatile bool addFingerMode = false; // Finger Add Flag
String correctPassword = "1234";

SemaphoreHandle_t displayMutex = NULL;
QueueHandle_t mqttQueue = NULL;
struct MQMsg { char txt[128]; };


// --- EEPROM ---
int getUserCount() {
  uint8_t c = EEPROM.read(COUNT_ADDR);
  if (c == 0xFF || c > MAX_USERS) return 0;
  return (int)c;
}

void setUserCount(int n) {
  if (n < 0) n = 0; if (n > MAX_USERS) n = MAX_USERS;
  EEPROM.write(COUNT_ADDR, (uint8_t)n);
  EEPROM.commit();
}

void readUserUID(int index, byte outUID[UID_LEN]) {
  memset(outUID, 0, UID_LEN);
  int total = getUserCount();
  if (index < 0 || index >= total) return;
  int base = RECORDS_START + index * RECORD_SIZE;
  for (int i = 0; i < UID_LEN; i++) outUID[i] = EEPROM.read(base + i);
}

bool uidEquals(const byte a[UID_LEN], const byte b[UID_LEN]) {
  for (int i = 0; i < UID_LEN; i++) if (a[i] != b[i]) return false;
  return true;
}

String uidToHexString(const byte uid[UID_LEN]) {
  String s = "";
  for (byte i = 0; i < UID_LEN; i++) {
    if (uid[i] < 0x10) s += "0"; s += String(uid[i], HEX);
  }
  s.toUpperCase(); return s;
}

int findUserIndexByUID(const byte uid[UID_LEN]) {
  int n = getUserCount();
  for (int i = 0; i < n; i++) {
    byte u[UID_LEN]; readUserUID(i, u);
    if (uidEquals(u, uid)) return i;
  }
  return -1;
}

int saveUser(const byte uid[UID_LEN]) {
  int n = getUserCount();
  if (n >= MAX_USERS) return -1;
  int base = RECORDS_START + n * RECORD_SIZE;
  for (int i = 0; i < UID_LEN; i++) EEPROM.write(base + i, uid[i]);
  setUserCount(n + 1); EEPROM.commit();
  return n;
}

// --- PASSWORD ---
void loadPassword() {
  char buf[PASSWORD_LEN + 1];
  for (int i = 0; i < PASSWORD_LEN; i++) {
    uint8_t v = EEPROM.read(PASSWORD_ADDR + i);
    buf[i] = (v == 0xFF) ? 0 : (char)v;
  }
  buf[PASSWORD_LEN] = 0;
  String p = String(buf);
  correctPassword = (p.length() > 0) ? p : "1234";
  Serial.println("Loaded Pass: " + correctPassword);
}

void savePassword(const String &pass) {
  for (int i = 0; i < PASSWORD_LEN; i++) EEPROM.write(PASSWORD_ADDR + i, (i < pass.length()) ? pass[i] : 0);
  EEPROM.commit();
  correctPassword = pass;
}

// --- SYSTEM ---
String getTimestamp() {
  struct tm timeinfo; char buf[30];
  if (getLocalTime(&timeinfo)) strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  else snprintf(buf, sizeof(buf), "uptime_%lus", millis() / 1000);
  return String(buf);
}

void queueMessage(const String &s) {
  if (!mqttQueue) return;
  MQMsg m; memset(m.txt, 0, sizeof(m.txt));
  size_t n = s.length(); if (n >= sizeof(m.txt)) n = sizeof(m.txt) - 1;
  memcpy(m.txt, s.c_str(), n);
  xQueueSend(mqttQueue, &m, 0);
}

void safeDisplay(const String &line1, const String &line2 = "") {
  if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200))) {
    display.clearDisplay(); display.setTextSize(1); display.setCursor(0, 0);
    display.println(line1);
    if (line2.length()) { display.setCursor(0, 16); display.println(line2); }
    display.display(); xSemaphoreGive(displayMutex);
  }
}

void showReady() {
  vTaskDelay(pdMS_TO_TICKS(1500));
  safeDisplay("READY TO SCAN", "RFID / Finger / Pass");
}

// 6. WIFI & MQTT & CALLBACK
void setup_wifi() {
  Serial.print("WiFi Connecting");
  WiFi.begin(ssid, wifi_password);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    vTaskDelay(pdMS_TO_TICKS(250)); Serial.print("."); retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true; Serial.println("\nWiFi OK");
    configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
    safeDisplay("WiFi: OK", WiFi.localIP().toString());
  } else {
    wifiConnected = false; Serial.println("\nWiFi Fail");
    safeDisplay("WiFi: ERROR", "Offline Mode");
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Tạo biến msg từ payload
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  String t = String(topic);

  // Xử lý logic
  if (t == mqtt_rfid_add_topic) {
    addMode = true;
    Serial.println("CMD: Add User Mode ON");
    safeDisplay("ADD MODE ON", "Moi quet the...");
    if (client.connected()) client.publish(mqtt_rfid_add_result, "{\"status\":\"waiting_tag\"}");
  }
  else if (t == mqtt_finger_add_topic) {
     if (msg == "CMD_ADD_FINGER") {
        addFingerMode = true; 
        Serial.println("LENH: Them Van Tay");
        safeDisplay("ADD MODE ON", "Moi them van tay...");
     }
  }
}

void reconnectMQTT() {
  if (!wifiConnected) return;
  if (client.connected()) return;
  
  String clientId = "ESP32Lock-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (client.connect(clientId.c_str())) {
    mqttConnected = true; Serial.println("MQTT OK");
    client.subscribe(mqtt_rfid_add_topic);
    client.subscribe(mqtt_finger_add_topic);
    safeDisplay("MQTT: Connected", "");
  } else {
    mqttConnected = false;
    Serial.print("MQTT Fail rc="); Serial.println(client.state());
  }
}

// FINGERPRINT LOGIC 
int getNextFreeID() {
  for (int i = 1; i <= 127; i++) {
    uint8_t p = finger.loadModel(i);
    if (p != FINGERPRINT_OK) return i; 
  }
  return -1; 
}

int getFingerID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) return finger.fingerID;
  return -1;
}

void enrollFingerFlow() {
  int id = getNextFreeID();
  if (id == -1) {
    client.publish(mqtt_rfid_add_result, "{\"status\":\"full\",\"msg\":\"Finger Full\"}");
    safeDisplay("FULL FINGER", ""); addFingerMode = false; return;
  }

  safeDisplay("THEM VAN TAY", "Dat ngon tay lan 1");
  client.publish(mqtt_rfid_add_result, "{\"status\":\"step1\",\"msg\":\"Place finger\"}");

  int p = -1; unsigned long start = millis();
  while (p != FINGERPRINT_OK) {
    if (millis() - start > 10000) { safeDisplay("Timeout", ""); addFingerMode=false; return; }
    p = finger.getImage(); vTaskDelay(100);
  }
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) { safeDisplay("Loi hinh anh", ""); addFingerMode=false; return; }

  safeDisplay("NHA TAY RA", "Lay tay ra...");
  client.publish(mqtt_rfid_add_result, "{\"status\":\"step2\",\"msg\":\"Remove finger\"}");
  vTaskDelay(2000);
  
  while (finger.getImage() != FINGERPRINT_NOFINGER) { vTaskDelay(50); }

  safeDisplay("XAC NHAN LAI", "Dat lai lan 2");
  client.publish(mqtt_rfid_add_result, "{\"status\":\"step3\",\"msg\":\"Place again\"}");
  
  p = -1; start = millis();
  while (p != FINGERPRINT_OK) {
    if (millis() - start > 10000) { safeDisplay("Timeout", ""); addFingerMode=false; return; }
    p = finger.getImage(); vTaskDelay(100);
  }
  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) { safeDisplay("Loi hinh anh", ""); addFingerMode=false; return; }

  p = finger.createModel();
  if (p == FINGERPRINT_OK) {
    p = finger.storeModel(id);
    if (p == FINGERPRINT_OK) {
      safeDisplay("THEM THANH CONG", "ID: " + String(id));
      String json = "{\"status\":\"success\",\"type\":\"finger\",\"id\":\"" + String(id) + "\"}";
      client.publish(mqtt_rfid_add_result, json.c_str());
    } else safeDisplay("LOI LUU TRU", "");
  } else {
    safeDisplay("KHONG KHOP", "Thu lai");
    client.publish(mqtt_rfid_add_result, "{\"status\":\"fail\",\"msg\":\"Mismatch\"}");
  }
  
  addFingerMode = false; vTaskDelay(2000); showReady();
}

// 8. TASKS

void TaskRFID(void *pvParameters) {
  (void) pvParameters;
  for (;;) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      byte readUID[UID_LEN];
      for (byte i = 0; i < UID_LEN; i++) readUID[i] = mfrc522.uid.uidByte[i];
      String idHex = uidToHexString(readUID);

      if (addMode) {
        int exist = findUserIndexByUID(readUID);
        if (exist >= 0) {
          safeDisplay("The da ton tai!", idHex);
          String out = "{\"status\":\"exist\",\"uid\":\"" + idHex + "\"}";
          if(client.connected()) client.publish(mqtt_rfid_add_result, out.c_str());
        } else {
          int idx = saveUser(readUID);
          if (idx >= 0) {
            safeDisplay("Luu thanh cong", idHex);
            String out = "{\"status\":\"success\",\"uid\":\"" + idHex + "\"}";
            if(client.connected()) client.publish(mqtt_rfid_add_result, out.c_str());
          } else {
            safeDisplay("EEPROM FULL", "");
            if(client.connected()) client.publish(mqtt_rfid_add_result, "{\"status\":\"full\"}");
          }
        }
        addMode = false; showReady();
      } else {
        int idx = findUserIndexByUID(readUID);
        if (idx >= 0) {
          safeDisplay("UNLOCK OK", "ID: " + idHex);
          String msg = "RFID " + idHex + " unlock " + getTimestamp();
          queueMessage(msg);
        } else {
          safeDisplay("ACCESS DENIED", "ID: " + idHex);
        }
        showReady();
      }
      mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void TaskFinger(void *pvParameters) {
  (void) pvParameters;
  for (;;) {
    if (addFingerMode) {
      enrollFingerFlow(); 
    } else {
      int fid = getFingerID();
      if (fid >= 0) {
        safeDisplay("FINGER OK", "ID: " + String(fid));
        String msg = "FINGER " + String(fid) + " unlock " + getTimestamp();
        queueMessage(msg);
        showReady();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void TaskKeypad(void *pvParameters) {
  (void) pvParameters;
  String input = ""; bool changingPass = false; String oldPassCheck = "";

  for (;;) {
    char key = keypad.getKey();
    if (key) {
      if (key == 'A') { 
        changingPass = true; input = ""; oldPassCheck = "";
        safeDisplay("DOI MAT KHAU", "Nhap pass cu:");
      } 
      else if (key == 'C') { input = ""; changingPass = false; showReady(); }
      else if (key >= '0' && key <= '9') {
        if (input.length() < 4) input += key;
        if (displayMutex && xSemaphoreTake(displayMutex, 100)) {
          display.clearDisplay(); display.setCursor(0,0);
          display.println(changingPass ? "DOI MAT KHAU" : "NHAP MAT KHAU");
          display.setCursor(0,16); for(unsigned int i=0; i<input.length(); i++) display.print("*");
          display.display(); xSemaphoreGive(displayMutex);
        }
      }
      else if (key == 'E') {
        if (input.length() != 4) { safeDisplay("LOI: Phai 4 so", ""); input = ""; continue; }
        if (changingPass) {
          if (oldPassCheck == "") {
             if (input == correctPassword) {
               oldPassCheck = "OK"; input = ""; safeDisplay("Pass cu OK", "Nhap pass moi:");
             } else {
               safeDisplay("Sai pass cu", ""); changingPass = false; showReady();
             }
          } else {
            savePassword(input); safeDisplay("Luu thanh cong", ""); changingPass = false; showReady();
          }
        } else {
          if (input == correctPassword) {
            safeDisplay("PASSWORD OK", ""); queueMessage("PASSWORD unlock " + getTimestamp());
          } else safeDisplay("WRONG PASSWORD", "");
          input = ""; showReady();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void TaskMQTT(void *pvParameters) {
  (void) pvParameters;
  MQMsg recv;
  for (;;) {
    // Kiểm tra trạng thái WiFi
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
    } else {
        wifiConnected = false;
    }

    // Xử lý kết nối MQTT
    if (wifiConnected) {
        if (!client.connected()) {
            reconnectMQTT();
            // Nếu connect fail, phải delay 5 giây để tránh spam và nhường CPU
            if (!client.connected()) { 
                vTaskDelay(pdMS_TO_TICKS(5000)); 
            }
        } else {
            // Nếu đã connect, gọi loop để duy trì keep-alive
            client.loop();
        }
    } else {
        // Nếu mất WiFi, đợi 1s rồi check lại
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    //  Xử lý Queue (Chỉ gửi khi có MQTT)
    // Lưu ý: xQueueReceive đã có timeout 100ms, đây cũng coi là 1 dạng delay ngắn
    // Nhưng nếu logic trên kia chiếm hết thời gian thì code không chạy xuống đây được.
    if (client.connected()) {
        if (xQueueReceive(mqttQueue, &recv, pdMS_TO_TICKS(10)) == pdTRUE) {
            client.publish(mqtt_topic, recv.txt);
        }
    } else {
        // Nếu không kết nối, xả queue hoặc delay nhẹ để không treo
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
  }
}

// 9. SETUP & LOOP
void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  if (EEPROM.read(COUNT_ADDR) > MAX_USERS) {
    Serial.println("EEPROM garbage detected. Resetting."); setUserCount(0);
  }

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { Serial.println("OLED Fail"); for(;;); }
  display.clearDisplay(); display.setTextColor(WHITE); display.display();

  SPI.begin(); mfrc522.PCD_Init();
  
  Serial2.begin(57600, SERIAL_8N1, FINGER_RX_PIN, FINGER_TX_PIN);
  finger.begin(57600);
  if (finger.verifyPassword()) Serial.println("Finger OK");
  else Serial.println("Finger Fail");

  displayMutex = xSemaphoreCreateMutex();
  mqttQueue = xQueueCreate(10, sizeof(MQMsg));

  loadPassword();
  setup_wifi();
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  safeDisplay("System Ready", "");
  
  xTaskCreatePinnedToCore(TaskRFID,   "RFID",   4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskFinger, "Finger", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskKeypad, "Keypad", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskMQTT,   "MQTT",   4096, NULL, 1, NULL, 0);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }