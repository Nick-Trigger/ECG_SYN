#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/timers.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

#define DAC_CH1 25
#define DAC_CH2 26
#define LED_BUILTIN 2
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SCREEN_ADDRESS 0x3C
#define OLED_RESET -1

// Function declarations
void ecgWaveformTask(void *pvParameters);
void outputTask(void *pvParameters);
void sendScreenStatus();
void blinkLedCallback(TimerHandle_t xTimer);
void setupBlinkLedTimer();
void oledTask(void *pvParameters);
void updateHeartRate(int newRate);

// Threading
TaskHandle_t waveform_task_handle;
TaskHandle_t output_task_handle;
TaskHandle_t oled_task_handle;

// Global variables
float ecg_value = 0.0f;             // Shared variable for the ECG value
SemaphoreHandle_t ecg_mutex;        // Mutex to protect access to ecg_value
int samplingRate = 1000;            // Sampling rate in Hz
int target_heart_rate = 60;         // Default 60 BPM
unsigned long lastRWaveTime = 0;    // Heart Animation
const int heartBlinkDuration = 200; // ms

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  Wire.begin(21, 22); // SDA, SCL pins for ESP32
  Serial.begin(115200);

  // Create a mutex for protecting ecg_value
  ecg_mutex = xSemaphoreCreateMutex();

  // Create tasks
  xTaskCreatePinnedToCore(
      ecgWaveformTask,       // Function to implement the task
      "ECG Waveform",        // Name of the task
      10000,                 // Stack size in bytes
      NULL,                  // Task input parameter
      1,                     // Priority of the task
      &waveform_task_handle, // Task handle
      0);                    // Core where the task should run

  xTaskCreatePinnedToCore(
      outputTask,          // Function to implement the task
      "Output Task",       // Name of the task
      10000,               // Stack size in bytes
      NULL,                // Task input parameter
      1,                   // Priority of the task
      &output_task_handle, // Task handle
      1);                  // Core where the task should run

  xTaskCreatePinnedToCore(
      oledTask,          // Function to implement the task
      "OLED Display",    // Name of the task
      10000,             // Stack size in bytes
      NULL,              // Task input parameter
      2,                 // Priority of the task
      &oled_task_handle, // Task handle
      1);                // Core where the task should run

  // Init Screen
  setupBlinkLedTimer();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    for (;;)
      ;
  }

  display.setTextSize(1);
  display.display();
  display.clearDisplay();
  display.display();

  delay(1000);
  Serial.println("Setup complete. Starting tasks...");

  display.clearDisplay();
}

void loop()
{
  // Empty!
}

// Heavily Modified/Ported Version of Dr. Palmeri's digilent-ecg-script.txt
// Generates an ECG Waveform
// Original File in ../ref/digilent-ecg-script.txt
// Original Repo: https://gitlab.oit.duke.edu/kits/BME-554L-001-Sp25/ecg-temp-ble-project/-/blob/main/testing/digilent-ecg-script.txt
// Modified by: Nicholas Trigger

void ecgWaveformTask(void *pvParameters)
{
  const float r_wave_avg_amplitude = 1.0f;   // Average amplitude of the R-wave
  const float amplitude_variance_pct = 0.1f; // Amplitude variance percentage (changes R and T wave amplitude)

  const float RR = 60.0f / target_heart_rate;
  const float QT = 0.4f * sqrt(RR); // Bazett's formula (QTc=400ms)

  // ------------------------------------------
  // ECG Waveform Parameters
  // A_* = AMP,  mu_* = MIDDLE, sigma_* = WIDTH
  // ------------------------------------------

  const float A_P = 0.15f, sigma_P = 0.025f; // P
  const float A_Q = -0.2f, sigma_Q = 0.03f;  // Q
  const float A_S = -0.3f, sigma_S = 0.035f; // S
  const float A_R = 1.0f, sigma_R = 0.015f;  // R
  const float A_T = 0.3f, sigma_T = 0.05f;   // T

  float X = 0.0f;
  const float step = 1.0f / samplingRate;

  // Heart rate control
  float timeSinceLastBeat = 0.0f;
  float noiseAmplitude = 0.1f;    // Noise amplitude pct
  float baselineAmplitude = 0.2f; // Baseline wander amplitude pct (breathing)

  while (1)
  {
    // Random Amplitudes
    float varianceSeed = ((float)random(-50, 50) / 100.0f);
    float scaled_A_R = A_R * (1.0f + (amplitude_variance_pct * varianceSeed));
    float scaled_A_T = A_T * (1.0f + (amplitude_variance_pct * varianceSeed));

    // Waveform Timing Params
    const float RR = 60.0f / target_heart_rate;
    const float beatInterval = RR;
    const float QT = 0.4f * sqrt(RR);

    // Wave Positions
    float mu_P = 0.15f * RR;
    float mu_Q = mu_P + 0.16f * sqrt(RR);
    float mu_S = mu_Q + 0.10f;
    float mu_R = (mu_Q + mu_S) / 2.0f;
    float mu_T = mu_Q + QT - 0.05f;

    // Boundary checks
    if (mu_T + 3 * sigma_T > RR)
      mu_T = RR - 3 * sigma_T - 0.02f;
    if (mu_Q < mu_P + 0.12f)
    {
      mu_Q = mu_P + 0.12f;
      mu_S = mu_Q + 0.10f;
      mu_R = (mu_Q + mu_S) / 2.0f;
    }

    // Generate ECG waveform
    double P_wave = A_P * expf(-pow((X - mu_P), 2) / (2 * pow(sigma_P, 2)));
    double Q_wave = A_Q * expf(-pow((X - mu_Q), 2) / (2 * pow(sigma_Q, 2)));
    double R_wave = scaled_A_R * expf(-pow((X - mu_R), 2) / (2 * pow(sigma_R, 2)));
    double S_wave = A_S * expf(-pow((X - mu_S), 2) / (2 * pow(sigma_S, 2)));
    double T_wave = scaled_A_T * expf(-pow((X - mu_T), 2) / (2 * pow(sigma_T, 2)));

    // Noise and Baseline
    double noise = noiseAmplitude * ((float)random(-50, 50) / 100.0f);
    double baseline = baselineAmplitude * sin(2 * PI * 0.33 * X); // 0.33 Hz baseline wander (breathing)

    // Create The Waveform
    double waveform = P_wave + Q_wave + R_wave + S_wave + T_wave + noise + baseline;

    // Normalize the waveform to the average amplitude of the R-wave
    waveform *= r_wave_avg_amplitude;

    // Protect shared variable with mutexS
    if (xSemaphoreTake(ecg_mutex, portMAX_DELAY))
    {
      ecg_value = waveform;
      xSemaphoreGive(ecg_mutex);
    }

    // Timing control
    X += step;
    timeSinceLastBeat += step;

    if (timeSinceLastBeat >= beatInterval)
    {
      X -= beatInterval; // Carry over fractional timing error
      timeSinceLastBeat = 0.0f;
    }

    Serial.print(">ecg:");
    Serial.println(waveform);

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void outputTask(void *pvParameters)
{
  // Initialize both DACs to 0
  dacWrite(DAC_CH1, 0);
  dacWrite(DAC_CH2, 0);

  while (1)
  {
    float outputValue = 0.0f;

    // Get the current waveform value, protected by mutex
    if (xSemaphoreTake(ecg_mutex, portMAX_DELAY))
    {
      outputValue = ecg_value;
      xSemaphoreGive(ecg_mutex);
    }

    // Clamp to ±2V range
    const float voltage_range = 2.5f;
    outputValue = constrain(outputValue, -voltage_range, voltage_range);

    if (outputValue >= 0.0f)
    {
      // Positive: map 0V to +2V to 0–255
      uint8_t dac1_val = (uint8_t)((outputValue / voltage_range) * 255.0f);
      dacWrite(DAC_CH1, dac1_val);
      dacWrite(DAC_CH2, 0); // Ensure negative channel is off
    }
    else
    {
      // Negative: map 0V to -2V to 0–255 (invert w/op amp)
      uint8_t dac2_val = (uint8_t)((-outputValue / voltage_range) * 255.0f);
      dacWrite(DAC_CH2, dac2_val);
      dacWrite(DAC_CH1, 0); // Ensure positive channel is off
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void updateHeartRate(int newRate)
{
  if (newRate < 30)
    newRate = 30;
  if (newRate > 180)
    newRate = 180;
  target_heart_rate = newRate;
}

void blinkLedCallback(TimerHandle_t xTimer)
{
  static bool ledState = false;
  digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
  ledState = !ledState;
}

void oledTask(void *pvParameters)
{
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  display.clearDisplay();
  display.display();

  int lastDisplayedHR = -1;
  bool heartVisible = false;

  while (1)
  {
    // Detect R-wave (simple threshold)
    float currentECG = 0.0;
    if (xSemaphoreTake(ecg_mutex, portMAX_DELAY))
    {
      currentECG = ecg_value;
      xSemaphoreGive(ecg_mutex);
    }

    // Update heart blink
    if (currentECG > 0.8f)
    { // Adjust threshold based on your R-wave amplitude
      lastRWaveTime = millis();
    }

    // Update display only when needed
    if (millis() - lastRWaveTime < heartBlinkDuration ||
        target_heart_rate != lastDisplayedHR)
    {

      display.clearDisplay();

      // Display heart rate
      display.setTextSize(2);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 25);
      display.print(target_heart_rate);
      display.print(" BPM");

      display.display();
      lastDisplayedHR = target_heart_rate;
    }

    vTaskDelay(pdMS_TO_TICKS(50)); // Refresh rate
  }
}

void setupBlinkLedTimer()
{
  pinMode(LED_BUILTIN, OUTPUT);

  TimerHandle_t blinkLedTimer = xTimerCreate(
      "Blink LED Timer",   // Timer name
      pdMS_TO_TICKS(1000), // Timer period (1 second)
      pdTRUE,              // Auto-reload
      (void *)0,           // Timer ID
      blinkLedCallback     // Callback function
  );

  if (blinkLedTimer != NULL)
  {
    xTimerStart(blinkLedTimer, 0); // Start the timer
  }
}