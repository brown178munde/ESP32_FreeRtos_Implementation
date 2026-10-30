/*
  ESP32 FreeRTOS Smart Lock - simplified teaching version
  Board: classic ESP32 DevKit / ESP32-WROOM-32

  Libraries:
    - MFRC522
    - Keypad
    - Adafruit GFX
    - Adafruit SSD1306
    - ESP32Servo

  IMPORTANT:
    1. Replace ALLOWED_RFID_UID with your card's UID (uppercase hex, no spaces).
    2. Change DEMO_PIN before using.
    3. This is a teaching prototype, not a production security system.
    4. Servo power should come from a suitable external regulated supply.
       Connect external supply GND to ESP32 GND.
    5. GPIO 34/35/39 do not have internal pull-ups on classic ESP32.
       Use external 10k pull-up resistors; buttons connect the GPIO to GND.
*/

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>

// ---------- Pin map ----------
static constexpr uint8_t RFID_SS_PIN   = 5;
static constexpr uint8_t RFID_RST_PIN  = 27;
static constexpr uint8_t RFID_SCK_PIN  = 18;
static constexpr uint8_t RFID_MISO_PIN = 19;
static constexpr uint8_t RFID_MOSI_PIN = 23;

static constexpr uint8_t OLED_SDA_PIN = 21;
static constexpr uint8_t OLED_SCL_PIN = 22;
static constexpr uint8_t SERVO_PIN    = 13;

// Status indicators (classic ESP32 strapping-pin precautions apply)
static constexpr uint8_t GREEN_LED_PIN = 2;
static constexpr uint8_t RED_LED_PIN   = 15;
static constexpr uint8_t BUZZER_PIN    = 12; // drive an active buzzer through an NPN transistor

static constexpr uint8_t PB_RFID_PIN  = 34; // external pull-up required
static constexpr uint8_t PB_PIN_PIN   = 35; // external pull-up required
static constexpr uint8_t PB_ADMIN_PIN = 39; // external pull-up required

static constexpr uint8_t SERVO_LOCKED_ANGLE   = 10;
static constexpr uint8_t SERVO_UNLOCKED_ANGLE = 90;
static constexpr uint32_t UNLOCK_TIME_MS = 5000;
static constexpr uint32_t PIN_SESSION_MS = 15000;
static constexpr uint32_t ADMIN_HOLD_MS  = 2000;
static constexpr uint8_t MAX_FAILED_ATTEMPTS = 3;
static constexpr uint32_t LOCKOUT_TIME_MS = 20000;
static constexpr char DEMO_PIN[] = "2580";
static constexpr char ALLOWED_RFID_UID[] = "DEADBEEF"; // replace with your card UID

// 4x4 keypad: rows then columns
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {4, 14, 16, 17};
byte colPins[COLS] = {25, 26, 32, 33};

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
Servo lockServo;

// ---------- RTOS objects ----------
enum class AccessSource : uint8_t { RFID, KEYPAD, ADMIN };
struct AccessRequest {
  AccessSource source;
  bool credentialValid;
  char message[20];
};
enum class LockCommand : uint8_t { UNLOCK, FORCE_LOCK, ACCESS_DENIED };

QueueHandle_t accessQueue = nullptr;
QueueHandle_t lockQueue = nullptr;
SemaphoreHandle_t stateMutex = nullptr;
TimerHandle_t unlockTimer = nullptr;

TaskHandle_t inputTaskHandle = nullptr;
TaskHandle_t rfidTaskHandle = nullptr;
TaskHandle_t keypadTaskHandle = nullptr;
TaskHandle_t lockTaskHandle = nullptr;

struct AppState {
  bool unlocked = false;
  bool lockout = false;
  uint8_t failedAttempts = 0;
  uint32_t lockoutEndsAt = 0;
  char status[22] = "Starting...";
  char lastEvent[22] = "System boot";
  uint32_t accepted = 0;
  uint32_t rejected = 0;
};
AppState appState;

// ---------- Helpers ----------
void setStatus(const char *status, const char *eventText) {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snprintf(appState.status, sizeof(appState.status), "%s", status);
    snprintf(appState.lastEvent, sizeof(appState.lastEvent), "%s", eventText);
    xSemaphoreGive(stateMutex);
  }
  Serial.printf("[%lu ms] %s: %s\n", millis(), status, eventText);
}

void submitAccess(AccessSource source, bool valid, const char *message) {
  AccessRequest req{};
  req.source = source;
  req.credentialValid = valid;
  snprintf(req.message, sizeof(req.message), "%s", message);
  if (xQueueSend(accessQueue, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
    setStatus("Busy", "Access queue full");
  }
}

String currentUidString() {
  String uid;
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += '0';
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void unlockTimerCallback(TimerHandle_t timer) {
  (void)timer;
  // Notify the lock-owning task; timer callback does not move the servo.
  if (lockTaskHandle != nullptr) {
    xTaskNotifyGive(lockTaskHandle);
  }
}

void setLockIndicators(bool unlocked) {
  digitalWrite(GREEN_LED_PIN, unlocked ? HIGH : LOW);
  digitalWrite(RED_LED_PIN, unlocked ? LOW : HIGH);
}

void buzzerBeep(uint8_t count, uint16_t onMs = 90, uint16_t offMs = 100) {
  for (uint8_t i = 0; i < count; ++i) {
    digitalWrite(BUZZER_PIN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(onMs));
    digitalWrite(BUZZER_PIN, LOW);
    if (i + 1 < count) vTaskDelay(pdMS_TO_TICKS(offMs));
  }
}

void drawDisplay() {
  AppState snapshot;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  snapshot = appState;
  xSemaphoreGive(stateMutex);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("FREE RTOS SMART LOCK");
  display.println("--------------------");
  display.print("Lock: ");
  display.println(snapshot.unlocked ? "UNLOCKED" : "LOCKED");
  display.print("State: ");
  display.println(snapshot.status);
  display.print("Accepted: ");
  display.println(snapshot.accepted);
  display.print("Rejected: ");
  display.println(snapshot.rejected);
  display.println("Last event:");
  display.println(snapshot.lastEvent);
  display.display();
}

// ---------- Tasks ----------
void InputTask(void *parameter) {
  (void)parameter;
  // Polling with debounce is intentionally used here to keep button handling
  // understandable. The other tasks remain independently scheduled.
  bool stable[3] = {HIGH, HIGH, HIGH};
  bool lastRaw[3] = {HIGH, HIGH, HIGH};
  uint32_t changedAt[3] = {0, 0, 0};
  const uint8_t pins[3] = {PB_RFID_PIN, PB_PIN_PIN, PB_ADMIN_PIN};
  bool adminTiming = false;
  uint32_t adminPressedAt = 0;
  bool adminSent = false;

  for (;;) {
    uint32_t now = millis();

    for (int i = 0; i < 2; ++i) {
      bool raw = digitalRead(pins[i]);
      if (raw != lastRaw[i]) {
        lastRaw[i] = raw;
        changedAt[i] = now;
      }
      if ((now - changedAt[i]) >= 35 && raw != stable[i]) {
        bool old = stable[i];
        stable[i] = raw;
        if (old == HIGH && stable[i] == LOW) {
          if (i == 0 && rfidTaskHandle) xTaskNotifyGive(rfidTaskHandle);
          if (i == 1 && keypadTaskHandle) xTaskNotifyGive(keypadTaskHandle);
        }
      }
    }

    bool adminRaw = digitalRead(PB_ADMIN_PIN);
    if (adminRaw == LOW) {
      if (!adminTiming) {
        adminTiming = true;
        adminPressedAt = now;
        adminSent = false;
      } else if (!adminSent && now - adminPressedAt >= ADMIN_HOLD_MS) {
        adminSent = true;
        submitAccess(AccessSource::ADMIN, true, "Admin override");
      }
    } else {
      adminTiming = false;
      adminSent = false;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void RFIDTask(void *parameter) {
  (void)parameter;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    setStatus("Scanning RFID", "Present card");
    uint32_t start = millis();
    bool found = false;

    while (millis() - start < 5000) {
      if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        String uid = currentUidString();
        bool valid = uid.equals(ALLOWED_RFID_UID);
        submitAccess(AccessSource::RFID, valid, valid ? "RFID accepted" : "RFID denied");
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
        found = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (!found) setStatus("No card", "RFID scan timed out");
  }
}

void KeypadTask(void *parameter) {
  (void)parameter;
  char entered[sizeof(DEMO_PIN)] = {0};

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    setStatus("Enter PIN", "Keypad session started");
    entered[0] = '\0';
    size_t length = 0;
    uint32_t startedAt = millis();

    while (millis() - startedAt < PIN_SESSION_MS) {
      char key = keypad.getKey();
      if (key) {
        if (key == '*') {
          length = 0;
          entered[0] = '\0';
        } else if (key == '#') {
          bool valid = strcmp(entered, DEMO_PIN) == 0;
          submitAccess(AccessSource::KEYPAD, valid, valid ? "PIN accepted" : "PIN denied");
          break;
        } else if (key >= '0' && key <= '9' && length < sizeof(entered) - 1) {
          entered[length++] = key;
          entered[length] = '\0';
        }
      }
      vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (millis() - startedAt >= PIN_SESSION_MS) {
      setStatus("PIN timeout", "Keypad session expired");
    }
  }
}

void SecurityTask(void *parameter) {
  (void)parameter;
  AccessRequest request{};

  for (;;) {
    if (xQueueReceive(accessQueue, &request, portMAX_DELAY) != pdTRUE) continue;

    bool allow = false;
    uint32_t now = millis();

    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
      if (request.source == AccessSource::ADMIN) {
        // Admin override intentionally bypasses demo lockout. In a real product,
        // replace this with a stronger independent admin authentication method.
        appState.lockout = false;
        appState.failedAttempts = 0;
        allow = true;
      } else if (appState.lockout && (int32_t)(appState.lockoutEndsAt - now) > 0) {
        allow = false;
      } else {
        appState.lockout = false;
        allow = request.credentialValid;
      }

      if (allow) {
        appState.failedAttempts = 0;
        appState.accepted++;
        snprintf(appState.status, sizeof(appState.status), "Access granted");
        snprintf(appState.lastEvent, sizeof(appState.lastEvent), "%s", request.message);
      } else {
        appState.rejected++;
        if (request.source != AccessSource::ADMIN) {
          appState.failedAttempts++;
          if (appState.failedAttempts >= MAX_FAILED_ATTEMPTS) {
            appState.lockout = true;
            appState.lockoutEndsAt = now + LOCKOUT_TIME_MS;
            appState.failedAttempts = 0;
          }
        }
        snprintf(appState.status, sizeof(appState.status), "%s",
                 appState.lockout ? "LOCKOUT" : "Access denied");
        snprintf(appState.lastEvent, sizeof(appState.lastEvent), "%s", request.message);
      }
      xSemaphoreGive(stateMutex);
    }

    Serial.printf("Security decision: %s (%s)\n", allow ? "ALLOW" : "DENY", request.message);
    if (allow) {
      LockCommand command = LockCommand::UNLOCK;
      if (xQueueSend(lockQueue, &command, pdMS_TO_TICKS(100)) != pdTRUE) {
        setStatus("Lock busy", "Could not queue unlock");
      }
    } else {
      // Queue audible denial indication; LockTask owns indicator outputs.
      LockCommand command = LockCommand::ACCESS_DENIED;
      xQueueSend(lockQueue, &command, pdMS_TO_TICKS(100));
      Serial.printf("Access denied: %s\n", request.message);
    }
  }
}

void LockTask(void *parameter) {
  (void)parameter;
  LockCommand command;

  for (;;) {
    if (xQueueReceive(lockQueue, &command, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (command == LockCommand::ACCESS_DENIED) {
        buzzerBeep(3, 70, 90); // three short beeps for denied access
      } else if (command == LockCommand::UNLOCK) {
        lockServo.write(SERVO_UNLOCKED_ANGLE);
        setLockIndicators(true);
        buzzerBeep(1, 100, 0); // one short beep for granted access
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          appState.unlocked = true;
          snprintf(appState.status, sizeof(appState.status), "UNLOCKED");
          xSemaphoreGive(stateMutex);
        }
        xTimerStop(unlockTimer, 0);
        xTimerChangePeriod(unlockTimer, pdMS_TO_TICKS(UNLOCK_TIME_MS), 0);
        xTimerStart(unlockTimer, 0);
        Serial.println("Servo unlocked; automatic relock timer started.");
      } else if (command == LockCommand::FORCE_LOCK) {
        xTimerStop(unlockTimer, 0);
        lockServo.write(SERVO_LOCKED_ANGLE);
        setLockIndicators(false);
      }
    }

    // Timer callback wakes this task. Only this task drives the servo.
    if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
      lockServo.write(SERVO_LOCKED_ANGLE);
      setLockIndicators(false);
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        appState.unlocked = false;
        snprintf(appState.status, sizeof(appState.status), "LOCKED");
        snprintf(appState.lastEvent, sizeof(appState.lastEvent), "Auto relocked");
        xSemaphoreGive(stateMutex);
      }
      Serial.println("Automatic relock complete.");
    }
  }
}

void DisplayTask(void *parameter) {
  (void)parameter;
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    drawDisplay();
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(500));
  }
}

void DiagnosticsTask(void *parameter) {
  (void)parameter;
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    Serial.printf("\n[Diagnostics] free heap=%u bytes, min-ever=%u bytes\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    Serial.printf("Queues: access=%u, lock=%u\n",
                  (unsigned)uxQueueMessagesWaiting(accessQueue),
                  (unsigned)uxQueueMessagesWaiting(lockQueue));
    Serial.printf("Stack high-water (words): Input=%u RFID=%u Keypad=%u Security=%u Lock=%u OLED=%u\n",
                  (unsigned)uxTaskGetStackHighWaterMark(inputTaskHandle),
                  (unsigned)uxTaskGetStackHighWaterMark(rfidTaskHandle),
                  (unsigned)uxTaskGetStackHighWaterMark(keypadTaskHandle),
                  (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("Security")),
                  (unsigned)uxTaskGetStackHighWaterMark(lockTaskHandle),
                  (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("Display")));
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(5000));
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PB_RFID_PIN, INPUT);  // external pull-up
  pinMode(PB_PIN_PIN, INPUT);   // external pull-up
  pinMode(PB_ADMIN_PIN, INPUT); // external pull-up

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, HIGH); // start in locked indication
  digitalWrite(BUZZER_PIN, LOW);

  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed; continuing without display.");
  } else {
    display.clearDisplay();
    display.display();
  }

  lockServo.setPeriodHertz(50);
  lockServo.attach(SERVO_PIN, 500, 2400);
  lockServo.write(SERVO_LOCKED_ANGLE);
  setLockIndicators(false);

  stateMutex = xSemaphoreCreateMutex();
  accessQueue = xQueueCreate(8, sizeof(AccessRequest));
  lockQueue = xQueueCreate(4, sizeof(LockCommand));
  unlockTimer = xTimerCreate("UnlockTimer", pdMS_TO_TICKS(UNLOCK_TIME_MS),
                             pdFALSE, nullptr, unlockTimerCallback);

  if (!stateMutex || !accessQueue || !lockQueue || !unlockTimer) {
    Serial.println("FATAL: RTOS object creation failed.");
    while (true) delay(1000);
  }

  // Priority is illustrative. Validate timing and stack use on the actual board.
  xTaskCreatePinnedToCore(InputTask,      "Input",       3072, nullptr, 3, &inputTaskHandle, 1);
  xTaskCreatePinnedToCore(RFIDTask,        "RFID",        4096, nullptr, 2, &rfidTaskHandle, 1);
  xTaskCreatePinnedToCore(KeypadTask,      "Keypad",      4096, nullptr, 2, &keypadTaskHandle, 1);
  xTaskCreatePinnedToCore(SecurityTask,    "Security",    4096, nullptr, 4, nullptr, 1);
  xTaskCreatePinnedToCore(LockTask,        "Lock",        3072, nullptr, 3, &lockTaskHandle, 1);
  xTaskCreatePinnedToCore(DisplayTask,     "Display",     3072, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(DiagnosticsTask, "Diagnostics", 3072, nullptr, 1, nullptr, 1);

  setStatus("LOCKED", "System ready");
}

void loop() {
  // All application work is handled by FreeRTOS tasks.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
