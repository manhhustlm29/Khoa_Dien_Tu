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

// ---------- EEPROM layout ----------
#define EEPROM_SIZE 512

// Byte đầu: số lượng user
#define COUNT_ADDR 0

// Mật khẩu: 4 ký tự + '\0' (5 byte) — giữ nguyên ở byte 1..5
#define PASSWORD_ADDR 1
#define PASSWORD_LEN 5   // 4 số + null

// Danh sách user bắt đầu tại byte 6
#define RECORDS_START 6

// Thông tin người dùng
#define NAME_MAX_LEN 20   // tên tối đa 20 byte
#define UID_LEN 4         // UID RFID chuẩn 4 byte

#define RECORD_SIZE (UID_LEN + NAME_MAX_LEN)

// Số lượng user tối đa (tự tính)
#define MAX_USERS ((EEPROM_SIZE - RECORDS_START) / RECORD_SIZE)


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
const char* mqtt_server = "test.mosquitto.org";
const int mqtt_port = 1883;
const char* mqtt_topic = "esp32/lock";
const char* mqtt_rfid_add_topic = "esp32/rfid_add";             // app -> esp: payload = name
const char* mqtt_rfid_add_result = "esp32/rfid_add_result";     // esp -> app: json result

WiFiClient espClient;
PubSubClient client(espClient);

// ---------- Flags ----------
volatile bool wifiConnected = false;
volatile bool mqttConnected = false;
volatile bool addMode = false;       // true when waiting to add UID from app command
String pendingAddName = "";          // name provided by app to associate with next scanned tag

// ---------- FreeRTOS objects ----------
SemaphoreHandle_t displayMutex = NULL;
QueueHandle_t mqttQueue = NULL; // queue of messages to send to MQTT

// ---------- Password ----------
String passwordInput = "";
String correctPassword = "1234"; // will be loaded from EEPROM at setup
volatile bool changePassMode = false; // true when in change-pass flow
String newPassBuffer = ""; // used as state marker during change

// ---------- MQTT message struct ----------
struct MQMsg {
  char txt[128];
};

// ---------- Helper functions for EEPROM users ----------
int getUserCount() {
  uint8_t c = EEPROM.read(COUNT_ADDR);
  if (c == 0xFF) return 0; // erased state
  return (int)c;
}

void setUserCount(int n) {
  if (n < 0) n = 0;
  if (n > MAX_USERS) n = MAX_USERS;
  EEPROM.write(COUNT_ADDR, (uint8_t)n);
  EEPROM.commit();
}

void readUserRecord(int index, byte outUID[UID_LEN], char outName[NAME_MAX_LEN+1]) {
  memset(outUID, 0, UID_LEN);
  outName[0] = 0;
  int total = getUserCount();
  if (index < 0 || index >= total) return;
  int base = RECORDS_START + index * RECORD_SIZE;
  for (int i = 0; i < UID_LEN; i++) outUID[i] = EEPROM.read(base + i);
  for (int i = 0; i < NAME_MAX_LEN; i++) {
    uint8_t c = EEPROM.read(base + UID_LEN + i);
    if (c == 0xFF || c == 0) { outName[i] = 0; break; }
    outName[i] = (char)c;
    if (i == NAME_MAX_LEN-1) outName[NAME_MAX_LEN] = 0;
  }
}

bool uidEquals(const byte a[UID_LEN], const byte b[UID_LEN]) {
  for (int i = 0; i < UID_LEN; i++) if (a[i] != b[i]) return false;
  return true;
}

String uidToHexString(const byte uid[UID_LEN], byte len = UID_LEN) {
  String s = "";
  for (byte i = 0; i < len; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// check if uid already exists and return index, or -1
int findUserIndexByUID(const byte uid[UID_LEN]) {
  int n = getUserCount();
  for (int i = 0; i < n; i++) {
    byte u[UID_LEN];
    char nm[NAME_MAX_LEN+1];
    readUserRecord(i, u, nm);
    if (uidEquals(u, uid)) return i;
  }
  return -1;
}

// save a new user (append). returns index saved or -1 on error (full)
int saveUser(const byte uid[UID_LEN], const String &name) {
  int n = getUserCount();
  if (n >= MAX_USERS) return -1;
  int base = RECORDS_START + n * RECORD_SIZE;
  // write UID
  for (int i = 0; i < UID_LEN; i++) EEPROM.write(base + i, uid[i]);
  // write name (truncated to NAME_MAX_LEN)
  for (int i = 0; i < NAME_MAX_LEN; i++) {
    if (i < name.length()) EEPROM.write(base + UID_LEN + i, (uint8_t)name[i]);
    else EEPROM.write(base + UID_LEN + i, 0);
  }
  setUserCount(n + 1);
  EEPROM.commit();
  return n;
}

// ---------- Password EEPROM helpers ----------
void loadPasswordFromEEPROM() {
  char buf[PASSWORD_LEN + 1];
  for (int i = 0; i < PASSWORD_LEN; i++) {
    uint8_t v = EEPROM.read(PASSWORD_ADDR + i);
    if (v == 0xFF) buf[i] = 0;
    else buf[i] = (char)v;
  }
  buf[PASSWORD_LEN] = 0;
  String p = String(buf);
  if (p.length() == 0) {
    correctPassword = "1234"; // fallback default
  } else {
    correctPassword = p;
  }
  Serial.print("Loaded password: ");
  Serial.println(correctPassword);
}

void savePasswordToEEPROM(const String &pass) {
  // write up to PASSWORD_LEN bytes (including null)
  for (int i = 0; i < PASSWORD_LEN; i++) {
    if (i < pass.length()) EEPROM.write(PASSWORD_ADDR + i, pass[i]);
    else EEPROM.write(PASSWORD_ADDR + i, 0);
  }
  EEPROM.commit();
  Serial.print("Saved password: ");
  Serial.println(pass);
}

// ---------- Other helpers ----------
String getTimestamp() {
  struct tm timeinfo;
  char buf[30];
  if (getLocalTime(&timeinfo)) {
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  } else {
    snprintf(buf, sizeof(buf), "uptime_%lus", millis() / 1000);
  }
  return String(buf);
}

// Push message into mqttQueue (non-blocking). If queue full, prints to Serial.
void queueMessage(const String &s) {
  if (!mqttQueue) {
    Serial.println("MQ queue not initialized");
    return;
  }
  MQMsg m;
  memset(m.txt, 0, sizeof(m.txt));
  size_t n = s.length();
  if (n >= sizeof(m.txt)) n = sizeof(m.txt) - 1;
  memcpy(m.txt, s.c_str(), n);
  if (xQueueSend(mqttQueue, &m, 0) != pdTRUE) {
    Serial.println("MQ queue full, offline log: " + s);
  }
}

// Display helpers with mutex protection
void safeDisplayClearAndPrint(const String &line1, const String &line2 = "") {
  if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200))) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(line1);
    if (line2.length()) {
      display.println(line2);
    }
    display.display();
    xSemaphoreGive(displayMutex);
  } else {
    Serial.println("Display busy - " + line1 + " / " + line2);
  }
}

void postShowDelay() {
  vTaskDelay(pdMS_TO_TICKS(1500));
  safeDisplayClearAndPrint("Ready to scan:", "- RFID / Finger / Keypad");
}

// ---------- WiFi ----------
void setup_wifi() {
  Serial.println("Connecting WiFi...");
  WiFi.begin(ssid, wifi_password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    vTaskDelay(pdMS_TO_TICKS(250));
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
    safeDisplayClearAndPrint("WiFi: CONNECTED", WiFi.localIP().toString());
  } else {
    wifiConnected = false;
    Serial.println("\nWiFi not connected — OFFLINE MODE");
    safeDisplayClearAndPrint("WiFi: OFFLINE", "Ready to scan:");
  }
}

// ---------- NTP ----------
void setup_time() {
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    Serial.println("NTP time OK");
  } else {
    Serial.println("NTP time fail");
  }
}

// ---------- MQTT callback ----------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  String t = String(topic);

  if (t == mqtt_rfid_add_topic) {
    // payload expected: name (plain text). We'll set addMode and wait for next RFID scan.
    pendingAddName = msg;
    addMode = true;
    Serial.println("Add request received. Name: " + pendingAddName);
    safeDisplayClearAndPrint("ADD MODE: Scan tag", pendingAddName);
    // Optionally send ack back
    String ack = "{\"status\":\"waiting_for_tag\",\"name\":\"" + pendingAddName + "\"}";
    client.publish(mqtt_rfid_add_result, ack.c_str());
  }
}

// ---------- MQTT reconnect (non-blocking style) ----------
void mqttConnectOnce() {
  if (!wifiConnected) { mqttConnected = false; return; }
  if (client.connected()) { mqttConnected = true; return; }

  Serial.print("Connecting MQTT...");
  String clientId = "ESP32Client-" + String((uint16_t)random(0xffff), HEX);
  if (client.connect(clientId.c_str())) {
    mqttConnected = true;
    Serial.println("MQTT connected");
    safeDisplayClearAndPrint("MQTT: CONNECTED", mqtt_server);
    client.subscribe(mqtt_rfid_add_topic);
  } else {
    mqttConnected = false;
    Serial.print("MQTT connect fail rc=");
    Serial.println(client.state());
  }
}

// ---------- Task prototypes ----------
void TaskRFID(void *pvParameters);
void TaskFinger(void *pvParameters);
void TaskKeypad(void *pvParameters);
void TaskMQTT(void *pvParameters);

// ---------- Keypad handling (used inside keypad task) ----------
void handleKeypadInternal() {
  char key = keypad.getKey();
  if (!key) return;

  // If user pressed A -> enter change password flow
  if (key == 'A') {
    passwordInput = "";
    newPassBuffer = ""; // expect old pass next
    changePassMode = true;
    safeDisplayClearAndPrint("Doi mat khau:", "Nhap pass cu");
    return;
  }

  // Only accept digits for password entry
  if (key >= '0' && key <= '9') {
    // limit to PASSWORD_LEN - 1 digits (4 digits)
    if (passwordInput.length() < (PASSWORD_LEN - 1)) passwordInput += key;
  } else if (key == 'C') {
    passwordInput = "";
    // cancel change pass if user clears
    if (changePassMode) {
      changePassMode = false;
      newPassBuffer = "";
      postShowDelay();
      return;
    }
  } else if (key == 'E') {
    // If we are in change password flow
    if (changePassMode) {
      // Step 1: verify old password
      if (newPassBuffer == "") {
        // require exactly 4 digits to verify
        if (passwordInput.length() != (PASSWORD_LEN - 1)) {
          safeDisplayClearAndPrint("Nhap 4 so", "Nhap lai");
          passwordInput = "";
          vTaskDelay(pdMS_TO_TICKS(1200));
          safeDisplayClearAndPrint("Doi mat khau:", "Nhap pass cu");
          return;
        }

        if (passwordInput == correctPassword) {
          newPassBuffer = "OK"; // mark that old pass verified
          passwordInput = "";
          safeDisplayClearAndPrint("Doi mat khau:", "Nhap pass moi");
        } else {
          safeDisplayClearAndPrint("Sai pass cu", "Quay ve... ");
          changePassMode = false;
          newPassBuffer = "";
          passwordInput = "";
          vTaskDelay(pdMS_TO_TICKS(1200));
          postShowDelay();
        }
      }
      // Step 2: save new password
      else {
        // ensure new password is 4 digits
        if (passwordInput.length() != (PASSWORD_LEN - 1)) {
          safeDisplayClearAndPrint("Pass phai 4 so", "");
          passwordInput = "";
          vTaskDelay(pdMS_TO_TICKS(1200));
          safeDisplayClearAndPrint("Doi mat khau:", "Nhap pass moi");
          return;
        }
        // persist to EEPROM (bytes 1..5)
        correctPassword = passwordInput;
        savePasswordToEEPROM(correctPassword);
        safeDisplayClearAndPrint("Luu thanh cong", "");
        changePassMode = false;
        newPassBuffer = "";
        passwordInput = "";
        vTaskDelay(pdMS_TO_TICKS(1200));
        postShowDelay();
      }
      return;
    }

    // Normal verify flow (not changing password)
    // require exactly 4 digits for normal verify as well
    if (passwordInput.length() != (PASSWORD_LEN - 1)) {
      safeDisplayClearAndPrint("Nhap 4 so", "");
      vTaskDelay(pdMS_TO_TICKS(900));
      passwordInput = "";
      postShowDelay();
      return;
    }

    if (passwordInput == correctPassword) {
      safeDisplayClearAndPrint("PASS OK", "");
      String ts = getTimestamp();
      String msg = "PASSWORD unlock " + ts;
      queueMessage(msg);
    } else {
      safeDisplayClearAndPrint("SAI PASS", "");
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
    passwordInput = "";
    postShowDelay();
    return;
  }

  // show password masked
  if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200))) {
    display.clearDisplay();
    display.setTextSize(1); display.setCursor(0, 0);
    if (changePassMode) {
      if (newPassBuffer == "") display.println("Doi mat khau: (nhap cu)");
      else display.println("Doi mat khau: (nhap moi)");
    } else display.println("Nhap mat khau:");
    display.setTextSize(2); display.setCursor(0, 20);
    for (unsigned int i = 0; i < passwordInput.length(); i++) display.print('*');
    display.display();
    xSemaphoreGive(displayMutex);
  }
}

// ---------- Tasks ----------

// RFID Task
void TaskRFID(void *pvParameters) {
  (void) pvParameters;
  for (;;) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      byte readUID[UID_LEN] = {0};
      for (byte i = 0; i < UID_LEN && i < mfrc522.uid.size; i++) readUID[i] = mfrc522.uid.uidByte[i];

      if (addMode) {
        // Add user flow
        int existing = findUserIndexByUID(readUID);
        if (existing >= 0) {
          // already exists -> update name in place
          int base = RECORDS_START + existing * RECORD_SIZE;
          for (int i = 0; i < NAME_MAX_LEN; i++) {
            if (i < pendingAddName.length()) EEPROM.write(base + UID_LEN + i, (uint8_t)pendingAddName[i]);
            else EEPROM.write(base + UID_LEN + i, 0);
          }
          EEPROM.commit();
          // publish result
          String idHex = uidToHexString(readUID, UID_LEN);
          String out = "{\"status\":\"updated\",\"uid\":\"" + idHex + "\",\"name\":\"" + pendingAddName + "\"}";
          if (client.connected()) client.publish(mqtt_rfid_add_result, out.c_str());
          safeDisplayClearAndPrint("Cập nhật user", pendingAddName);
        } else {
          int idx = saveUser(readUID, pendingAddName);
          if (idx >= 0) {
            String idHex = uidToHexString(readUID, UID_LEN);
            String out = "{\"status\":\"ok\",\"uid\":\"" + idHex + "\",\"name\":\"" + pendingAddName + "\"}";
            if (client.connected()) client.publish(mqtt_rfid_add_result, out.c_str());
            safeDisplayClearAndPrint("Added user:", pendingAddName);
          } else {
            String out = "{\"status\":\"full\",\"msg\":\"EEPROM full\"}";
            if (client.connected()) client.publish(mqtt_rfid_add_result, out.c_str());
            safeDisplayClearAndPrint("EEPROM FULL", "");
          }
        }
        // clear add mode
        addMode = false;
        pendingAddName = "";
        vTaskDelay(pdMS_TO_TICKS(800));
        postShowDelay();
      } else {
        // Normal unlock check across all users
        int idx = findUserIndexByUID(readUID);
        if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200))) {
          display.clearDisplay();
          display.setTextSize(2);
          display.setCursor(0, 16);
          if (idx >= 0) {
            char nm[NAME_MAX_LEN+1];
            byte u[UID_LEN];
            readUserRecord(idx, u, nm);
            display.println("RFID: OK");
            display.setTextSize(1);
            display.println(nm);
          } else {
            display.println("RFID: KHONG");
          }
          display.display();
          xSemaphoreGive(displayMutex);
        } else {
          Serial.println("Display busy - RFID result");
        }

        String idHex = uidToHexString(readUID, UID_LEN);
        if (idx >= 0) {
          char nm[NAME_MAX_LEN+1]; byte u[UID_LEN];
          readUserRecord(idx, u, nm);
          String msg = "RFID " + idHex + " unlock " + String(nm) + " " + getTimestamp();
          queueMessage(msg);
        }
      }

      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      vTaskDelay(pdMS_TO_TICKS(600));
      postShowDelay();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Fingerprint Task (uses getFingerprintID helper)
int getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) return -2;
  if (p != FINGERPRINT_OK) return -2;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -2;
  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) return finger.fingerID;
  return -1;
}

void TaskFinger(void *pvParameters) {
  (void) pvParameters;
  for (;;) {
    int f_id = getFingerprintID();
    if (f_id >= 0) {
      if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200))) {
        display.clearDisplay();
        display.setTextSize(2); display.setCursor(0, 8);
        display.print("Finger ID "); display.println(f_id);
        display.display();
        xSemaphoreGive(displayMutex);
      }
      String msg = "Finger ID " + String(f_id) + " unlock " + getTimestamp();
      queueMessage(msg);
      vTaskDelay(pdMS_TO_TICKS(800));
      postShowDelay();
    } else {
      vTaskDelay(pdMS_TO_TICKS(150));
    }
  }
}

// Keypad Task
void TaskKeypad(void *pvParameters) {
  (void) pvParameters;
  for (;;) {
    handleKeypadInternal();
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}

// MQTT Task - consumes queue and publishes
void TaskMQTT(void *pvParameters) {
  (void) pvParameters;
  MQMsg recv;
  for (;;) {
    // ensure wifi
    if (!wifiConnected) {
      if (WiFi.status() != WL_CONNECTED) {
        setup_wifi();
        if (wifiConnected) setup_time();
      } else {
        wifiConnected = true;
      }
    }

    // ensure mqtt
    if (wifiConnected) mqttConnectOnce();

    if (client.connected()) {
      client.loop();
    }

    // publish queued messages
    if (mqttQueue && xQueueReceive(mqttQueue, &recv, pdMS_TO_TICKS(500)) == pdTRUE) {
      String s = String(recv.txt);
      if (wifiConnected && client.connected()) {
        boolean ok = client.publish(mqtt_topic, recv.txt);
        if (ok) {
          Serial.println("Published: " + s);
        } else {
          Serial.println("Publish failed, offline log: " + s);
        }
      } else {
        Serial.println("Offline log: " + s);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(100);

  // init EEPROM
  EEPROM.begin(EEPROM_SIZE);

  // init display
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found! Stopping.");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  display.clearDisplay(); display.setTextColor(SSD1306_WHITE);

  // create mutex & queue
  displayMutex = xSemaphoreCreateMutex();
  mqttQueue = xQueueCreate(8, sizeof(MQMsg)); // 8 messages, each 128 bytes

  // basic init messages
  if (displayMutex) {
    xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200));
    display.setTextSize(1); display.setCursor(0, 0);
    display.println("Init system...");
    display.display();
    xSemaphoreGive(displayMutex);
  }

  // init devices
  SPI.begin(); mfrc522.PCD_Init();
  Serial2.begin(57600, SERIAL_8N1, FINGER_RX_PIN, FINGER_TX_PIN);
  finger.begin(57600);

  if (finger.verifyPassword()) Serial.println("Fingerprint sensor found!");
  else Serial.println("Fingerprint sensor not found!");

  // load password from EEPROM (bytes 1..5)
  loadPasswordFromEEPROM();

  // wifi + mqtt setup (non-blocking)
  setup_wifi();
  if (wifiConnected) setup_time();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  // show ready
  safeDisplayClearAndPrint("Ready to scan:", "- RFID / Finger / Keypad");

  // create tasks
  xTaskCreatePinnedToCore(TaskRFID, "TaskRFID", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskFinger, "TaskFinger", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskKeypad, "TaskKeypad", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskMQTT, "TaskMQTT", 4096, NULL, 1, NULL, 0);
}

// ---------- Loop ----------
void loop() {
  vTaskDelay(pdMS_TO_TICKS(2000));
}
