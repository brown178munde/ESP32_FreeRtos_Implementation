/*
  FreeRTOS Demonstrations: Race Condition, Priority Inversion, Deadlock
  Board: classic ESP32 DevKit / ESP32-WROOM-32
  Open Serial Monitor at 115200 baud.

  Commands:
    R = race condition (unsafe then protected counter)
    I = priority inversion comparison (binary semaphore vs mutex)
    D = deadlock / bounded recovery demonstration
    H = help

  This is a separate test sketch. Do NOT merge it into the smart-lock sketch.
  All demo tasks are pinned to core 1 to make scheduling behavior easier to observe.
  Timing is illustrative and may vary with ESP32 Arduino core / FreeRTOS version.
*/

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

static volatile uint32_t sharedCounter = 0;
static SemaphoreHandle_t counterMutex;
static SemaphoreHandle_t demoDone;
static SemaphoreHandle_t startSignal;
static SemaphoreHandle_t inversionLock;
static SemaphoreHandle_t highAboutToLock;
static SemaphoreHandle_t lowHasLock;
static SemaphoreHandle_t mediumMayRun;
static SemaphoreHandle_t deadlockBarrier;

enum class InversionLockType : uint8_t { BinarySemaphore, Mutex };
static InversionLockType inversionMode = InversionLockType::BinarySemaphore;
static volatile uint32_t lowReleaseMs = 0;
static volatile uint32_t highWaitMs = 0;

// ---------- Race condition ----------
void unsafeCounterTask(void *arg) {
  (void)arg;
  for (uint32_t i = 0; i < 100000; ++i) {
    uint32_t temp = sharedCounter;   // read
    taskYIELD();                     // deliberately widen race window
    temp = temp + 1;                 // modify
    sharedCounter = temp;            // write
  }
  xSemaphoreGive(demoDone);
  vTaskDelete(nullptr);
}

void safeCounterTask(void *arg) {
  (void)arg;
  for (uint32_t i = 0; i < 100000; ++i) {
    if (xSemaphoreTake(counterMutex, portMAX_DELAY) == pdTRUE) {
      sharedCounter++;
      xSemaphoreGive(counterMutex);
    }
  }
  xSemaphoreGive(demoDone);
  vTaskDelete(nullptr);
}

void runRaceDemo() {
  Serial.println("\n=== RACE CONDITION ===");
  Serial.println("Two tasks increment the same counter 100000 times each.");
  Serial.println("Expected result: 200000. First run is intentionally unsafe.");

  sharedCounter = 0;
  xTaskCreatePinnedToCore(unsafeCounterTask, "RaceA", 2048, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(unsafeCounterTask, "RaceB", 2048, nullptr, 2, nullptr, 1);
  xSemaphoreTake(demoDone, portMAX_DELAY);
  xSemaphoreTake(demoDone, portMAX_DELAY);
  Serial.printf("Without mutex: counter=%lu (expected 200000)\n",
                (unsigned long)sharedCounter);

  Serial.println("Second run uses a mutex around the read-modify-write operation.");
  sharedCounter = 0;
  xTaskCreatePinnedToCore(safeCounterTask, "SafeA", 2048, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(safeCounterTask, "SafeB", 2048, nullptr, 2, nullptr, 1);
  xSemaphoreTake(demoDone, portMAX_DELAY);
  xSemaphoreTake(demoDone, portMAX_DELAY);
  Serial.printf("With mutex:    counter=%lu (expected 200000)\n",
                (unsigned long)sharedCounter);
  Serial.println("A single unsafe run can sometimes reach the expected value; repeat to observe variation.");
}

// ---------- Priority inversion ----------
void takeInversionLock() {
  xSemaphoreTake(inversionLock, portMAX_DELAY);
}

void releaseInversionLock() {
  xSemaphoreGive(inversionLock);
}

void lowPriorityTask(void *arg) {
  (void)arg;
  takeInversionLock();
  xSemaphoreGive(lowHasLock);
  // Wait until the high-priority task is about to request the same lock.
  xSemaphoreTake(startSignal, portMAX_DELAY);

  // CPU-bound work while holding the lock. A medium-priority task can preempt
  // this task if priority inheritance is not available.
  const uint32_t workMs = 450;
  uint32_t started = millis();
  while (millis() - started < workMs) {
    asm volatile("nop");
  }
  lowReleaseMs = millis();
  releaseInversionLock();
  xSemaphoreGive(demoDone);
  vTaskDelete(nullptr);
}

void highPriorityTask(void *arg) {
  (void)arg;
  xSemaphoreTake(highAboutToLock, portMAX_DELAY);
  uint32_t start = millis();
  xSemaphoreGive(startSignal);
  takeInversionLock(); // blocks while low-priority task owns the lock
  highWaitMs = millis() - start;
  releaseInversionLock();
  xSemaphoreGive(demoDone);
  vTaskDelete(nullptr);
}

void mediumPriorityTask(void *arg) {
  (void)arg;
  xSemaphoreTake(mediumMayRun, portMAX_DELAY);
  // Run long enough to delay Low when there is no priority inheritance.
  uint32_t started = millis();
  while (millis() - started < 900) {
    asm volatile("nop");
  }
  xSemaphoreGive(demoDone);
  vTaskDelete(nullptr);
}

void runOneInversionTest(bool useMutex) {
  inversionMode = useMutex ? InversionLockType::Mutex : InversionLockType::BinarySemaphore;
  if (inversionLock) vSemaphoreDelete(inversionLock);
  inversionLock = useMutex ? xSemaphoreCreateMutex() : xSemaphoreCreateBinary();

  // For a binary semaphore, give it once to make it available.
  if (!useMutex) xSemaphoreGive(inversionLock);

  // Recreate one-shot coordination semaphores for each test.
  if (highAboutToLock) vSemaphoreDelete(highAboutToLock);
  if (lowHasLock) vSemaphoreDelete(lowHasLock);
  if (mediumMayRun) vSemaphoreDelete(mediumMayRun);
  if (startSignal) vSemaphoreDelete(startSignal);
  highAboutToLock = xSemaphoreCreateBinary();
  lowHasLock = xSemaphoreCreateBinary();
  mediumMayRun = xSemaphoreCreateBinary();
  startSignal = xSemaphoreCreateBinary();
  lowReleaseMs = 0;
  highWaitMs = 0;

  Serial.printf("\n--- %s test ---\n", useMutex ? "Mutex (priority inheritance)" :
                                            "Binary semaphore (no priority inheritance)");
  // Low=1, Medium=3, High=4. Same core is important for this demonstration.
  xTaskCreatePinnedToCore(lowPriorityTask,    "Low",    2048, nullptr, 1, nullptr, 1);
  xSemaphoreTake(lowHasLock, portMAX_DELAY);
  xTaskCreatePinnedToCore(highPriorityTask,   "High",   2048, nullptr, 4, nullptr, 1);
  xTaskCreatePinnedToCore(mediumPriorityTask, "Medium", 2048, nullptr, 3, nullptr, 1);

  // Allow the medium task to run after high has begun requesting the lock.
  xSemaphoreGive(highAboutToLock);
  vTaskDelay(pdMS_TO_TICKS(50));
  xSemaphoreGive(mediumMayRun);

  xSemaphoreTake(demoDone, portMAX_DELAY); // Low
  xSemaphoreTake(demoDone, portMAX_DELAY); // High
  xSemaphoreTake(demoDone, portMAX_DELAY); // Medium

  Serial.printf("High-priority task waited about %lu ms for the lock.\n",
                (unsigned long)highWaitMs);
  Serial.println("Compare the two runs; scheduler timing and ESP32 core behavior affect exact values.");
}

void runPriorityInversionDemo() {
  Serial.println("\n=== PRIORITY INVERSION ===");
  Serial.println("Low priority owns a lock; High priority blocks on it; Medium does CPU work.");
  Serial.println("First: binary semaphore has no priority inheritance.");
  runOneInversionTest(false);
  vTaskDelay(pdMS_TO_TICKS(250));
  Serial.println("Second: FreeRTOS mutex supports priority inheritance.");
  runOneInversionTest(true);
  Serial.println("Expected concept: with a mutex, Low is temporarily boosted so it can release the lock.");
  Serial.println("Note: this is an educational timing demo, not a hard real-time benchmark.");
}

// ---------- Deadlock / bounded recovery ----------
SemaphoreHandle_t mutexA;
SemaphoreHandle_t mutexB;
SemaphoreHandle_t deadlockReady;

void deadlockTaskA(void *arg) {
  (void)arg;
  xSemaphoreTake(mutexA, portMAX_DELAY);
  Serial.println("Task A acquired mutex A.");
  xSemaphoreGive(deadlockReady);
  xSemaphoreTake(deadlockBarrier, portMAX_DELAY);
  Serial.println("Task A attempting mutex B (bounded wait).");
  if (xSemaphoreTake(mutexB, pdMS_TO_TICKS(500)) == pdTRUE) {
    Serial.println("Task A acquired B.");
    xSemaphoreGive(mutexB);
  } else {
    Serial.println("Task A timed out waiting for B; recovering.");
  }
  xSemaphoreGive(mutexA);
  xSemaphoreGive(demoDone);
  vTaskDelete(nullptr);
}

void deadlockTaskB(void *arg) {
  (void)arg;
  xSemaphoreTake(mutexB, portMAX_DELAY);
  Serial.println("Task B acquired mutex B.");
  xSemaphoreGive(deadlockReady);
  xSemaphoreTake(deadlockBarrier, portMAX_DELAY);
  Serial.println("Task B attempting mutex A (bounded wait).");
  if (xSemaphoreTake(mutexA, pdMS_TO_TICKS(500)) == pdTRUE) {
    Serial.println("Task B acquired A.");
    xSemaphoreGive(mutexA);
  } else {
    Serial.println("Task B timed out waiting for A; recovering.");
  }
  xSemaphoreGive(mutexB);
  xSemaphoreGive(demoDone);
  vTaskDelete(nullptr);
}

void runDeadlockDemo() {
  Serial.println("\n=== DEADLOCK / RECOVERY ===");
  Serial.println("A takes mutex A; B takes mutex B; then each requests the other's mutex.");
  Serial.println("This uses a timeout to recover, so it demonstrates a deadlock condition and prevention/recovery,");
  Serial.println("not a permanent system hang.");

  mutexA = xSemaphoreCreateMutex();
  mutexB = xSemaphoreCreateMutex();
  deadlockReady = xSemaphoreCreateCounting(2, 0);
  deadlockBarrier = xSemaphoreCreateCounting(2, 0);

  if (!mutexA || !mutexB || !deadlockReady || !deadlockBarrier) {
    Serial.println("Failed to allocate deadlock demo objects.");
    return;
  }

  xTaskCreatePinnedToCore(deadlockTaskA, "DeadA", 2048, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(deadlockTaskB, "DeadB", 2048, nullptr, 2, nullptr, 1);

  xSemaphoreTake(deadlockReady, portMAX_DELAY);
  xSemaphoreTake(deadlockReady, portMAX_DELAY);
  // Release both tasks from the barrier so both attempt the opposite lock.
  xSemaphoreGive(deadlockBarrier);
  xSemaphoreGive(deadlockBarrier);

  xSemaphoreTake(demoDone, portMAX_DELAY);
  xSemaphoreTake(demoDone, portMAX_DELAY);

  vSemaphoreDelete(mutexA);
  vSemaphoreDelete(mutexB);
  vSemaphoreDelete(deadlockReady);
  vSemaphoreDelete(deadlockBarrier);
  mutexA = mutexB = deadlockReady = deadlockBarrier = nullptr;
  Serial.println("Deadlock demo finished; tasks recovered through timeout.");
}

void printHelp() {
  Serial.println("\nCommands: R=race condition, I=priority inversion, D=deadlock, H=help");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  counterMutex = xSemaphoreCreateMutex();
  demoDone = xSemaphoreCreateCounting(10, 0);
  startSignal = xSemaphoreCreateBinary();
  highAboutToLock = xSemaphoreCreateBinary();
  lowHasLock = xSemaphoreCreateBinary();
  mediumMayRun = xSemaphoreCreateBinary();

  if (!counterMutex || !demoDone || !startSignal || !highAboutToLock ||
      !lowHasLock || !mediumMayRun) {
    Serial.println("Failed to create RTOS objects.");
    while (true) delay(1000);
  }
  Serial.println("\nFreeRTOS demonstration sketch ready.");
  printHelp();
}

void loop() {
  if (Serial.available()) {
    char c = (char)toupper(Serial.read());
    if (c == 'R') runRaceDemo();
    else if (c == 'I') runPriorityInversionDemo();
    else if (c == 'D') runDeadlockDemo();
    else if (c == 'H') printHelp();
  }
  vTaskDelay(pdMS_TO_TICKS(20));
}
