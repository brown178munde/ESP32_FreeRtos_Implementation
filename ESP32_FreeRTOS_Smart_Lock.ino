/*
  ESP32 FREE RTOS SMART LOCK
  Features:
  - RFID access
  - Keypad PIN access
  - Admin button override
  - Automatic relocking
  - OLED display
  - Red and green LEDs
  - Buzzer
  - Last 10 access events stored in RAM
  - Access summary shown for 10 seconds every 30 seconds

  BOARD: ESP32 Dev Module

  REQUIRED LIBRARIES:
  1. MFRC522
  2. Keypad
  3. Adafruit GFX
  4. Adafruit SSD1306
  5. ESP32Servo

  IMPORTANT:
  - Replace ALLOWED_RFID_UID with your actual card UID.
  - Change DEMO_PIN if required.
  - GPIO 34, 35 and 39 need external 10k pull-up resistors.
  - Servo needs a separate regulated power supply.
  - Connect external servo supply GND to ESP32 GND.
*/

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>

// =====================================================
// 1. PIN CONFIGURATION
// =====================================================

// RFID RC522
#define RFID_SS_PIN    5
#define RFID_RST_PIN   27
#define RFID_SCK_PIN   18
#define RFID_MISO_PIN  19
#define RFID_MOSI_PIN  23

// OLED
#define OLED_SDA_PIN   21
#define OLED_SCL_PIN   22

// Servo
#define SERVO_PIN      13

// LEDs and buzzer
#define GREEN_LED_PIN  2
#define RED_LED_PIN    15
#define BUZZER_PIN     12

// Push buttons
#define PB_RFID_PIN    34
#define PB_PIN_PIN     35
#define PB_ADMIN_PIN   39

// =====================================================
// 2. SYSTEM CONFIGURATION
// =====================================================

#define SERVO_LOCKED_ANGLE      10
#define SERVO_UNLOCKED_ANGLE    90

#define UNLOCK_TIME_MS          5000
#define PIN_SESSION_MS          15000
#define ADMIN_HOLD_MS           2000

#define MAX_FAILED_ATTEMPTS     3
#define LOCKOUT_TIME_MS         20000

// Change these credentials
const char DEMO_PIN[] = "2580";
const char ALLOWED_RFID_UID[] = "92AC491D";

// Access history configuration
#define LOG_CAPACITY                 10
#define LOG_SUMMARY_INTERVAL_MS      30000
#define LOG_SUMMARY_DURATION_MS      10000

// =====================================================
// 3. KEYPAD CONFIGURATION
// =====================================================

const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte rowPins[ROWS] = {4, 14, 16, 17};
byte colPins[COLS] = {26, 25, 33, 32};

Keypad keypad = Keypad(
  makeKeymap(keys),
  rowPins,
  colPins,
  ROWS,
  COLS
);

// =====================================================
// 4. HARDWARE OBJECTS
// =====================================================

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

Adafruit_SSD1306 display(
  128,
  64,
  &Wire,
  -1
);

Servo lockServo;

// =====================================================
// 5. RTOS DATA STRUCTURES
// =====================================================

enum class AccessSource : uint8_t {
  RFID,
  KEYPAD,
  ADMIN
};

enum class LockCommand : uint8_t {
  UNLOCK,
  FORCE_LOCK,
  ACCESS_DENIED
};

// Request sent to SecurityTask
struct AccessRequest {
  AccessSource source;
  bool credentialValid;
  char message[20];
};

// Access history record
struct AccessLogEntry {
  AccessSource source;
  bool granted;
  uint32_t uptimeSeconds;
  char message[20];
};

// Shared application state
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

// =====================================================
// 6. FREERTOS QUEUES, SEMAPHORES AND TIMER
// =====================================================

// Queues
QueueHandle_t accessQueue = nullptr;
QueueHandle_t lockQueue = nullptr;
QueueHandle_t logQueue = nullptr;

// Mutexes
SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t logMutex = nullptr;

// Binary semaphore
SemaphoreHandle_t logUpdatedSemaphore = nullptr;

// Software timer
TimerHandle_t unlockTimer = nullptr;

// Task handles
TaskHandle_t inputTaskHandle = nullptr;
TaskHandle_t rfidTaskHandle = nullptr;
TaskHandle_t keypadTaskHandle = nullptr;
TaskHandle_t securityTaskHandle = nullptr;
TaskHandle_t lockTaskHandle = nullptr;
TaskHandle_t logTaskHandle = nullptr;
TaskHandle_t displayTaskHandle = nullptr;
TaskHandle_t diagnosticsTaskHandle = nullptr;

// =====================================================
// 7. ACCESS HISTORY STORAGE
// =====================================================

// Circular buffer stores the latest 10 access events.
AccessLogEntry accessHistory[LOG_CAPACITY] = {};

uint8_t logWriteIndex = 0;
uint8_t logCount = 0;

// Latest successful entry is displayed on the OLED.
AccessLogEntry latestSuccessfulEntry = {};

bool hasSuccessfulEntry = false;

// =====================================================
// 8. HELPER FUNCTIONS
// =====================================================

// Update shared application status safely.
void setStatus(const char *status, const char *eventText) {

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {

    snprintf(
      appState.status,
      sizeof(appState.status),
      "%s",
      status
    );

    snprintf(
      appState.lastEvent,
      sizeof(appState.lastEvent),
      "%s",
      eventText
    );

    xSemaphoreGive(stateMutex);
  }

  Serial.printf(
    "[%lu ms] %s: %s\n",
    (unsigned long)millis(),
    status,
    eventText
  );
}


// Send an access request to SecurityTask.
void submitAccess(
  AccessSource source,
  bool valid,
  const char *message
) {

  AccessRequest request{};

  request.source = source;
  request.credentialValid = valid;

  snprintf(
    request.message,
    sizeof(request.message),
    "%s",
    message
  );

  if (xQueueSend(
        accessQueue,
        &request,
        pdMS_TO_TICKS(50)
      ) != pdTRUE) {

    Serial.println("[ERROR] Access queue full.");

    setStatus(
      "Busy",
      "Access queue full"
    );
  }
}


// Read RFID card UID.
String currentUidString() {

  String uid = "";

  for (byte i = 0; i < rfid.uid.size; i++) {

    if (rfid.uid.uidByte[i] < 0x10) {
      uid += '0';
    }

    uid += String(
      rfid.uid.uidByte[i],
      HEX
    );
  }

  uid.toUpperCase();

  return uid;
}


// Convert access source to readable text.
const char *sourceName(AccessSource source) {

  switch (source) {

    case AccessSource::RFID:
      return "RFID";

    case AccessSource::KEYPAD:
      return "KEYPAD";

    case AccessSource::ADMIN:
      return "ADMIN";

    default:
      return "UNKNOWN";
  }
}


// Convert uptime seconds into HH:MM:SS.
void formatUptime(
  uint32_t totalSeconds,
  char *output,
  size_t outputSize
) {

  uint32_t hours = totalSeconds / 3600;
  uint32_t minutes = (totalSeconds % 3600) / 60;
  uint32_t seconds = totalSeconds % 60;

  snprintf(
    output,
    outputSize,
    "%02lu:%02lu:%02lu",
    (unsigned long)hours,
    (unsigned long)minutes,
    (unsigned long)seconds
  );
}


// =====================================================
// 9. LOCK AND INDICATOR FUNCTIONS
// =====================================================

// Green LED = unlocked
// Red LED = locked
void setLockIndicators(bool unlocked) {

  digitalWrite(
    GREEN_LED_PIN,
    unlocked ? HIGH : LOW
  );

  digitalWrite(
    RED_LED_PIN,
    unlocked ? LOW : HIGH
  );
}


// Buzzer beep function.
// Call only from a FreeRTOS task.
void buzzerBeep(
  uint8_t count,
  uint16_t onMs = 90,
  uint16_t offMs = 100
) {

  for (uint8_t i = 0; i < count; i++) {

    digitalWrite(BUZZER_PIN, HIGH);

    vTaskDelay(pdMS_TO_TICKS(onMs));

    digitalWrite(BUZZER_PIN, LOW);

    if (i + 1 < count) {
      vTaskDelay(pdMS_TO_TICKS(offMs));
    }
  }
}


// Timer callback.
// It only notifies LockTask.
// It does NOT directly control the servo.
void unlockTimerCallback(TimerHandle_t timer) {

  (void)timer;

  if (lockTaskHandle != nullptr) {
    xTaskNotifyGive(lockTaskHandle);
  }
}


// =====================================================
// 10. ACCESS HISTORY FUNCTIONS
// =====================================================

// Copy latest successful entry while protecting shared data.
bool copyLatestSuccessfulEntry(
  AccessLogEntry &copy
) {

  if (xSemaphoreTake(
        logMutex,
        pdMS_TO_TICKS(50)
      ) != pdTRUE) {

    return false;
  }

  bool available = hasSuccessfulEntry;

  if (available) {
    copy = latestSuccessfulEntry;
  }

  xSemaphoreGive(logMutex);

  return available;
}


// Print the last 10 access events on Serial Monitor.
void printAccessHistory() {

  AccessLogEntry snapshot[LOG_CAPACITY];

  uint8_t count = 0;

  if (xSemaphoreTake(
        logMutex,
        pdMS_TO_TICKS(100)
      ) != pdTRUE) {

    Serial.println("[LOG ERROR] Could not obtain log mutex.");

    return;
  }

  count = logCount;

  // Calculate oldest record in the circular buffer.
  uint8_t oldest =
    (logWriteIndex + LOG_CAPACITY - logCount)
    % LOG_CAPACITY;

  for (uint8_t i = 0; i < count; i++) {

    snapshot[i] =
      accessHistory[(oldest + i) % LOG_CAPACITY];
  }

  xSemaphoreGive(logMutex);

  Serial.println();
  Serial.println("========== LAST ACCESS EVENTS ==========");

  if (count == 0) {
    Serial.println("No access events recorded.");
  }

  for (uint8_t i = 0; i < count; i++) {

    char timeText[16];

    formatUptime(
      snapshot[i].uptimeSeconds,
      timeText,
      sizeof(timeText)
    );

    Serial.printf(
      "%s | %-6s | %-7s | %s\n",
      timeText,
      sourceName(snapshot[i].source),
      snapshot[i].granted ? "GRANTED" : "DENIED",
      snapshot[i].message
    );
  }

  Serial.println("========================================");
  Serial.println();
}


// =====================================================
// 11. OLED DISPLAY FUNCTIONS
// =====================================================

// Normal main display.
void drawNormalDisplay(const AppState &snapshot) {

  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);

  display.println("FREE RTOS SMART LOCK");
  display.println("--------------------");

  display.print("Lock: ");

  display.println(
    snapshot.unlocked ? "UNLOCKED" : "LOCKED"
  );

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


// Access summary display.
void drawAccessSummaryDisplay(
  const AppState &snapshot
) {

  AccessLogEntry lastEntry{};

  bool hasEntry =
    copyLatestSuccessfulEntry(lastEntry);

  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);

  display.println("SMART LOCK");
  display.println("------------");

  display.println(
    snapshot.unlocked ? "UNLOCKED" : "LOCKED"
  );

  display.println();
  display.println("LAST ENTRY:");

  if (hasEntry) {

    char timeText[16];

    formatUptime(
      lastEntry.uptimeSeconds,
      timeText,
      sizeof(timeText)
    );

    display.println(sourceName(lastEntry.source));
    display.println(timeText);
    display.println("ACCESS: GRANTED");

  } else {

    display.println("NO ENTRY YET");
    display.println("--:--:--");
    display.println("ACCESS: --");
  }

  display.display();
}


// =====================================================
// 12. DISPLAY SELECTION AND TIMING
// =====================================================

void drawDisplay() {

  AppState snapshot;

  // Take a safe copy of application state.
  if (xSemaphoreTake(
        stateMutex,
        pdMS_TO_TICKS(50)
      ) != pdTRUE) {

    Serial.println("[DISPLAY] State mutex timeout.");

    return;
  }

  snapshot = appState;

  xSemaphoreGive(stateMutex);


  // Time since ESP32 boot.
  uint32_t uptimeMs = millis();


  /*
    DISPLAY TIMING:

    0 - 30 seconds:
      Main display

    30 - 40 seconds:
      Access summary

    40 - 60 seconds:
      Main display

    60 - 70 seconds:
      Access summary

    This repeats every 30 seconds.

    IMPORTANT:
    The modulo operation creates the repeating cycle.
  */

  bool showSummary =
    uptimeMs >= LOG_SUMMARY_INTERVAL_MS &&
    (uptimeMs % LOG_SUMMARY_INTERVAL_MS)
      < LOG_SUMMARY_DURATION_MS;


  if (showSummary) {

    drawAccessSummaryDisplay(snapshot);

  } else {

    drawNormalDisplay(snapshot);
  }
}


// =====================================================
// 13. INPUT TASK
// =====================================================

// Reads RFID, PIN and Admin buttons.
void InputTask(void *parameter) {

  (void)parameter;

  bool stable[3] = {
    HIGH,
    HIGH,
    HIGH
  };

  bool lastRaw[3] = {
    HIGH,
    HIGH,
    HIGH
  };

  uint32_t changedAt[3] = {
    0,
    0,
    0
  };

  const uint8_t pins[3] = {
    PB_RFID_PIN,
    PB_PIN_PIN,
    PB_ADMIN_PIN
  };

  bool adminTiming = false;
  uint32_t adminPressedAt = 0;
  bool adminSent = false;


  for (;;) {

    uint32_t now = millis();


    // RFID and PIN button debounce.
    for (int i = 0; i < 2; i++) {

      bool raw = digitalRead(pins[i]);

      if (raw != lastRaw[i]) {

        lastRaw[i] = raw;
        changedAt[i] = now;
      }

      if (
        (now - changedAt[i]) >= 35 &&
        raw != stable[i]
      ) {

        bool old = stable[i];

        stable[i] = raw;

        // Detect button press.
        if (old == HIGH && stable[i] == LOW) {

          if (i == 0 && rfidTaskHandle != nullptr) {
            xTaskNotifyGive(rfidTaskHandle);
          }

          if (i == 1 && keypadTaskHandle != nullptr) {
            xTaskNotifyGive(keypadTaskHandle);
          }
        }
      }
    }


    // Admin button requires a long press.
    bool adminRaw = digitalRead(PB_ADMIN_PIN);

    if (adminRaw == LOW) {

      if (!adminTiming) {

        adminTiming = true;
        adminPressedAt = now;
        adminSent = false;

      } else if (
        !adminSent &&
        now - adminPressedAt >= ADMIN_HOLD_MS
      ) {

        adminSent = true;

        submitAccess(
          AccessSource::ADMIN,
          true,
          "Admin override"
        );
      }

    } else {

      adminTiming = false;
      adminSent = false;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


// =====================================================
// 14. RFID TASK
// =====================================================

void RFIDTask(void *parameter) {

  (void)parameter;

  for (;;) {

    // Sleep until InputTask notifies this task.
    ulTaskNotifyTake(
      pdTRUE,
      portMAX_DELAY
    );

    setStatus(
      "Scanning RFID",
      "Present card"
    );

    uint32_t start = millis();

    bool found = false;


    // Wait up to 5 seconds for a card.
    while (millis() - start < 5000) {

      if (
        rfid.PICC_IsNewCardPresent() &&
        rfid.PICC_ReadCardSerial()
      ) {

        String uid = currentUidString();

        Serial.print("[RFID] Card UID: ");
        Serial.println(uid);

        bool valid =
          uid.equals(ALLOWED_RFID_UID);


        submitAccess(
          AccessSource::RFID,
          valid,
          valid ? "RFID accepted" : "RFID denied"
        );

        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();

        found = true;

        break;
      }

      vTaskDelay(pdMS_TO_TICKS(30));
    }


    if (!found) {

      Serial.println("[RFID ERROR] Scan timed out.");

      setStatus(
        "No card",
        "RFID scan timed out"
      );
    }
  }
}


// =====================================================
// 15. KEYPAD TASK
// =====================================================

void KeypadTask(void *parameter) {

  (void)parameter;

  char entered[sizeof(DEMO_PIN)] = {0};


  for (;;) {

    // Wait for PIN button notification.
    ulTaskNotifyTake(
      pdTRUE,
      portMAX_DELAY
    );

    setStatus(
      "Enter PIN",
      "Keypad session started"
    );

    entered[0] = '\0';

    size_t length = 0;

    uint32_t startedAt = millis();


    while (millis() - startedAt < PIN_SESSION_MS) {

      char key = keypad.getKey();

      if (key) {

        Serial.print("[KEYPAD] Key pressed: ");
        Serial.println(key);


        // Clear entered PIN.
        if (key == '*') {

          length = 0;
          entered[0] = '\0';

          Serial.println("[KEYPAD] PIN cleared.");
        }


        // Submit PIN.
        else if (key == '#') {

          bool valid =
            strcmp(entered, DEMO_PIN) == 0;

          Serial.println(
            valid
              ? "[KEYPAD] Correct PIN."
              : "[KEYPAD] Incorrect PIN."
          );

          submitAccess(
            AccessSource::KEYPAD,
            valid,
            valid ? "PIN accepted" : "PIN denied"
          );

          break;
        }


        // Accept numeric characters.
        else if (
          key >= '0' &&
          key <= '9' &&
          length < sizeof(entered) - 1
        ) {

          entered[length++] = key;

          entered[length] = '\0';
        }
      }

      vTaskDelay(pdMS_TO_TICKS(25));
    }


    if (millis() - startedAt >= PIN_SESSION_MS) {

      Serial.println("[KEYPAD] PIN session timed out.");

      setStatus(
        "PIN timeout",
        "Keypad session expired"
      );
    }
  }
}


// =====================================================
// 16. LOG TASK
// =====================================================

// LogTask is the only task that writes to the access history.
void LogTask(void *parameter) {

  (void)parameter;

  AccessLogEntry entry{};


  for (;;) {

    // Wait until SecurityTask sends an access event.
    if (
      xQueueReceive(
        logQueue,
        &entry,
        portMAX_DELAY
      ) != pdTRUE
    ) {
      continue;
    }


    // Protect shared history using mutex.
    if (
      xSemaphoreTake(
        logMutex,
        portMAX_DELAY
      ) == pdTRUE
    ) {

      // Insert new event into circular buffer.
      accessHistory[logWriteIndex] = entry;

      logWriteIndex =
        (logWriteIndex + 1) % LOG_CAPACITY;

      if (logCount < LOG_CAPACITY) {
        logCount++;
      }


      // Update latest successful entry only.
      if (entry.granted) {

        latestSuccessfulEntry = entry;

        hasSuccessfulEntry = true;
      }

      xSemaphoreGive(logMutex);
    }


    char timeText[16];

    formatUptime(
      entry.uptimeSeconds,
      timeText,
      sizeof(timeText)
    );


    Serial.printf(
      "[ACCESS LOG] %s | %s | %s | %s\n",
      timeText,
      sourceName(entry.source),
      entry.granted ? "GRANTED" : "DENIED",
      entry.message
    );


    // Notify DisplayTask that a new log was stored.
    xSemaphoreGive(logUpdatedSemaphore);


    // Print all stored access events.
    printAccessHistory();
  }
}


// =====================================================
// 17. SECURITY TASK
// =====================================================

// Makes the final access decision.
void SecurityTask(void *parameter) {

  (void)parameter;

  AccessRequest request{};


  for (;;) {

    // Wait for an access request.
    if (
      xQueueReceive(
        accessQueue,
        &request,
        portMAX_DELAY
      ) != pdTRUE
    ) {
      continue;
    }


    bool allow = false;

    uint32_t now = millis();


    // Protect application state.
    if (
      xSemaphoreTake(
        stateMutex,
        portMAX_DELAY
      ) == pdTRUE
    ) {

      // Admin override bypasses lockout.
      if (request.source == AccessSource::ADMIN) {

        appState.lockout = false;

        appState.failedAttempts = 0;

        allow = true;
      }


      // Check whether lockout is active.
      else if (
        appState.lockout &&
        (int32_t)(appState.lockoutEndsAt - now) > 0
      ) {

        allow = false;
      }


      // Normal credential validation.
      else {

        appState.lockout = false;

        allow = request.credentialValid;
      }


      // Access granted.
      if (allow) {

        appState.failedAttempts = 0;

        appState.accepted++;

        snprintf(
          appState.status,
          sizeof(appState.status),
          "Access granted"
        );

        snprintf(
          appState.lastEvent,
          sizeof(appState.lastEvent),
          "%s",
          request.message
        );
      }


      // Access denied.
      else {

        appState.rejected++;

        if (request.source != AccessSource::ADMIN) {

          appState.failedAttempts++;

          if (
            appState.failedAttempts >=
            MAX_FAILED_ATTEMPTS
          ) {

            appState.lockout = true;

            appState.lockoutEndsAt =
              now + LOCKOUT_TIME_MS;

            appState.failedAttempts = 0;

            Serial.println(
              "[SECURITY] LOCKOUT ACTIVATED."
            );
          }
        }

        snprintf(
          appState.status,
          sizeof(appState.status),
          "%s",
          appState.lockout
            ? "LOCKOUT"
            : "Access denied"
        );

        snprintf(
          appState.lastEvent,
          sizeof(appState.lastEvent),
          "%s",
          request.message
        );
      }

      xSemaphoreGive(stateMutex);
    }


    // Create a log record for every access decision.
    AccessLogEntry logEntry{};

    logEntry.source = request.source;

    logEntry.granted = allow;

    logEntry.uptimeSeconds = millis() / 1000UL;

    snprintf(
      logEntry.message,
      sizeof(logEntry.message),
      "%s",
      request.message
    );


    // Send log record to LogTask.
    if (
      xQueueSend(
        logQueue,
        &logEntry,
        pdMS_TO_TICKS(100)
      ) != pdTRUE
    ) {

      Serial.println(
        "[WARNING] Access log queue full."
      );
    }


    Serial.printf(
      "[SECURITY] Decision: %s | %s\n",
      allow ? "GRANTED" : "DENIED",
      request.message
    );


    // Send command to LockTask.
    if (allow) {

      LockCommand command = LockCommand::UNLOCK;

      if (
        xQueueSend(
          lockQueue,
          &command,
          pdMS_TO_TICKS(100)
        ) != pdTRUE
      ) {

        Serial.println(
          "[ERROR] Could not queue unlock command."
        );

        setStatus(
          "Lock busy",
          "Could not queue unlock"
        );
      }

    } else {

      LockCommand command =
        LockCommand::ACCESS_DENIED;

      xQueueSend(
        lockQueue,
        &command,
        pdMS_TO_TICKS(100)
      );

      Serial.println("[SECURITY] Access denied.");
    }
  }
}


// =====================================================
// 18. LOCK TASK
// =====================================================

// Only LockTask controls the servo and indicator outputs.
void LockTask(void *parameter) {

  (void)parameter;

  LockCommand command;


  for (;;) {

    // Check for commands from SecurityTask.
    if (
      xQueueReceive(
        lockQueue,
        &command,
        pdMS_TO_TICKS(100)
      ) == pdTRUE
    ) {


      // Access denied: three beeps.
      if (command == LockCommand::ACCESS_DENIED) {

        buzzerBeep(3, 70, 90);
      }


      // Access granted: unlock.
      else if (command == LockCommand::UNLOCK) {

        Serial.println("[LOCK] Unlocking servo.");

        lockServo.write(SERVO_UNLOCKED_ANGLE);

        setLockIndicators(true);

        buzzerBeep(1, 100, 0);


        if (
          xSemaphoreTake(
            stateMutex,
            pdMS_TO_TICKS(50)
          ) == pdTRUE
        ) {

          appState.unlocked = true;

          snprintf(
            appState.status,
            sizeof(appState.status),
            "UNLOCKED"
          );

          xSemaphoreGive(stateMutex);
        }


        // Restart automatic relock timer.
        xTimerStop(unlockTimer, 0);

        xTimerChangePeriod(
          unlockTimer,
          pdMS_TO_TICKS(UNLOCK_TIME_MS),
          0
        );

        xTimerStart(unlockTimer, 0);

        Serial.println(
          "[LOCK] Auto relock timer started."
        );
      }


      // Force lock command.
      else if (command == LockCommand::FORCE_LOCK) {

        xTimerStop(unlockTimer, 0);

        lockServo.write(SERVO_LOCKED_ANGLE);

        setLockIndicators(false);

        Serial.println("[LOCK] Force locked.");
      }
    }


    // Check whether software timer has expired.
    if (ulTaskNotifyTake(pdTRUE, 0) > 0) {

      Serial.println(
        "[LOCK] Timer expired. Relocking."
      );

      lockServo.write(SERVO_LOCKED_ANGLE);

      setLockIndicators(false);


      if (
        xSemaphoreTake(
          stateMutex,
          pdMS_TO_TICKS(50)
        ) == pdTRUE
      ) {

        appState.unlocked = false;

        snprintf(
          appState.status,
          sizeof(appState.status),
          "LOCKED"
        );

        snprintf(
          appState.lastEvent,
          sizeof(appState.lastEvent),
          "Auto relocked"
        );

        xSemaphoreGive(stateMutex);
      }

      Serial.println("[LOCK] Automatic relock complete.");
    }
  }
}


// =====================================================
// 19. DISPLAY TASK
// =====================================================

// Refreshes OLED twice per second.
// Also refreshes immediately when a new access log arrives.
void DisplayTask(void *parameter) {

  (void)parameter;

  for (;;) {

    // Wait for new log event or timeout after 500 ms.
    xSemaphoreTake(
      logUpdatedSemaphore,
      pdMS_TO_TICKS(500)
    );

    // Select main screen or summary screen.
    drawDisplay();
  }
}


// =====================================================
// 20. DIAGNOSTICS TASK
// =====================================================

// Prints system health information every 5 seconds.
void DiagnosticsTask(void *parameter) {

  (void)parameter;

  TickType_t lastWake = xTaskGetTickCount();


  for (;;) {

    Serial.println();
    Serial.println("========== SYSTEM DIAGNOSTICS ==========");


    // Heap memory information.
    Serial.printf(
      "Free heap: %u bytes\n",
      (unsigned)ESP.getFreeHeap()
    );

    Serial.printf(
      "Minimum free heap: %u bytes\n",
      (unsigned)ESP.getMinFreeHeap()
    );


    // Queue information.
    Serial.printf(
      "Queue waiting: Access=%u Lock=%u Log=%u\n",
      (unsigned)uxQueueMessagesWaiting(accessQueue),
      (unsigned)uxQueueMessagesWaiting(lockQueue),
      (unsigned)uxQueueMessagesWaiting(logQueue)
    );


    // Read log count safely.
    uint8_t currentLogCount = 0;

    if (
      xSemaphoreTake(
        logMutex,
        pdMS_TO_TICKS(50)
      ) == pdTRUE
    ) {

      currentLogCount = logCount;

      xSemaphoreGive(logMutex);
    }

    Serial.printf(
      "Stored access events: %u/%u\n",
      currentLogCount,
      LOG_CAPACITY
    );


    // Stack high-water marks.
    // Smaller values mean less remaining stack space.
    Serial.println("Task stack high-water marks (words):");

    Serial.printf(
      "Input: %u | RFID: %u | Keypad: %u\n",
      (unsigned)uxTaskGetStackHighWaterMark(inputTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(rfidTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(keypadTaskHandle)
    );

    Serial.printf(
      "Security: %u | Lock: %u | Log: %u\n",
      (unsigned)uxTaskGetStackHighWaterMark(securityTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(lockTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(logTaskHandle)
    );

    Serial.printf(
      "Display: %u\n",
      (unsigned)uxTaskGetStackHighWaterMark(displayTaskHandle)
    );

    Serial.println("========================================");
    Serial.println();


    // Run diagnostics every 5 seconds.
    vTaskDelayUntil(
      &lastWake,
      pdMS_TO_TICKS(5000)
    );
  }
}


// =====================================================
// 21. SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(300);

  Serial.println();
  Serial.println("================================");
  Serial.println(" ESP32 FREE RTOS SMART LOCK");
  Serial.println(" Starting system...");
  Serial.println("================================");


  // ---------------------------------------------------
  // Configure buttons.
  // External pull-ups are required for GPIO 34/35/39.
  // ---------------------------------------------------

  pinMode(PB_RFID_PIN, INPUT);
  pinMode(PB_PIN_PIN, INPUT);
  pinMode(PB_ADMIN_PIN, INPUT);


  // ---------------------------------------------------
  // Configure LEDs and buzzer.
  // ---------------------------------------------------

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, LOW);


  // ---------------------------------------------------
  // Initialize RFID.
  // ---------------------------------------------------

  Serial.println("[INIT] Initializing RFID...");

  SPI.begin(
    RFID_SCK_PIN,
    RFID_MISO_PIN,
    RFID_MOSI_PIN,
    RFID_SS_PIN
  );

  rfid.PCD_Init();

  Serial.println("[INIT] RFID initialized.");


  // ---------------------------------------------------
  // Initialize OLED.
  // ---------------------------------------------------

  Serial.println("[INIT] Initializing OLED...");

  Wire.begin(
    OLED_SDA_PIN,
    OLED_SCL_PIN
  );

  if (!display.begin(
        SSD1306_SWITCHCAPVCC,
        0x3C
      )) {

    Serial.println(
      "[ERROR] OLED initialization failed."
    );

  } else {

    Serial.println("[INIT] OLED initialized.");

    display.clearDisplay();
    display.display();
  }


  // ---------------------------------------------------
  // Initialize servo.
  // ---------------------------------------------------

  Serial.println("[INIT] Initializing servo...");

  lockServo.setPeriodHertz(50);

  lockServo.attach(
    SERVO_PIN,
    500,
    2400
  );

  lockServo.write(SERVO_LOCKED_ANGLE);

  setLockIndicators(false);


  // ---------------------------------------------------
  // Create RTOS synchronization objects.
  // ---------------------------------------------------

  Serial.println("[INIT] Creating RTOS objects...");

  stateMutex = xSemaphoreCreateMutex();

  logMutex = xSemaphoreCreateMutex();

  logUpdatedSemaphore = xSemaphoreCreateBinary();


  // ---------------------------------------------------
  // Create queues.
  // ---------------------------------------------------

  accessQueue = xQueueCreate(
    8,
    sizeof(AccessRequest)
  );

  lockQueue = xQueueCreate(
    4,
    sizeof(LockCommand)
  );

  logQueue = xQueueCreate(
    10,
    sizeof(AccessLogEntry)
  );


  // ---------------------------------------------------
  // Create automatic relock software timer.
  // ---------------------------------------------------

  unlockTimer = xTimerCreate(
    "UnlockTimer",
    pdMS_TO_TICKS(UNLOCK_TIME_MS),
    pdFALSE,
    nullptr,
    unlockTimerCallback
  );


  // Check whether all RTOS objects were created.
  if (
    stateMutex == nullptr ||
    logMutex == nullptr ||
    logUpdatedSemaphore == nullptr ||
    accessQueue == nullptr ||
    lockQueue == nullptr ||
    logQueue == nullptr ||
    unlockTimer == nullptr
  ) {

    Serial.println(
      "[FATAL ERROR] RTOS object creation failed."
    );

    while (true) {
      delay(1000);
    }
  }

  Serial.println("[INIT] RTOS objects created.");


  // ---------------------------------------------------
  // Create FreeRTOS tasks.
  // ---------------------------------------------------

  Serial.println("[INIT] Creating FreeRTOS tasks...");

  BaseType_t taskOK = pdPASS;

  taskOK &= xTaskCreatePinnedToCore(
    InputTask,
    "Input",
    3072,
    nullptr,
    3,
    &inputTaskHandle,
    1
  );

  taskOK &= xTaskCreatePinnedToCore(
    RFIDTask,
    "RFID",
    4096,
    nullptr,
    2,
    &rfidTaskHandle,
    1
  );

  taskOK &= xTaskCreatePinnedToCore(
    KeypadTask,
    "Keypad",
    4096,
    nullptr,
    2,
    &keypadTaskHandle,
    1
  );

  taskOK &= xTaskCreatePinnedToCore(
    SecurityTask,
    "Security",
    4096,
    nullptr,
    4,
    &securityTaskHandle,
    1
  );

  taskOK &= xTaskCreatePinnedToCore(
    LockTask,
    "Lock",
    3072,
    nullptr,
    3,
    &lockTaskHandle,
    1
  );

  taskOK &= xTaskCreatePinnedToCore(
    LogTask,
    "Log",
    3072,
    nullptr,
    1,
    &logTaskHandle,
    1
  );

  taskOK &= xTaskCreatePinnedToCore(
    DisplayTask,
    "Display",
    3072,
    nullptr,
    1,
    &displayTaskHandle,
    1
  );

  taskOK &= xTaskCreatePinnedToCore(
    DiagnosticsTask,
    "Diagnostics",
    3072,
    nullptr,
    1,
    &diagnosticsTaskHandle,
    1
  );


  // Check task creation.
  if (taskOK != pdPASS) {

    Serial.println(
      "[FATAL ERROR] One or more tasks failed to start."
    );

    while (true) {
      delay(1000);
    }
  }


  // ---------------------------------------------------
  // System ready.
  // ---------------------------------------------------

  setStatus(
    "LOCKED",
    "System ready"
  );

  Serial.println();
  Serial.println("================================");
  Serial.println(" SMART LOCK READY");
  Serial.println(" Tasks started: 8");
  Serial.println(" Access history: Last 10 events");
  Serial.println(" OLED summary: 10 sec every 30 sec");
  Serial.println(" Serial Monitor: 115200 baud");
  Serial.println("================================");
}


// =====================================================
// 22. MAIN LOOP
// =====================================================

// All application operations run inside FreeRTOS tasks.
// Arduino loop does not perform application processing.
void loop() {

  vTaskDelay(pdMS_TO_TICKS(1000));
}
