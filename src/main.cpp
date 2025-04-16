#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/timers.h>

#define DAC_CH1 25
#define DAC_CH2 26
#define LED_BUILTIN 2

// Function declarations
void ecgWaveformTask(void *pvParameters);
void outputTask(void *pvParameters);
void sendScreenStatus();
void blinkLedCallback(TimerHandle_t xTimer);
void setupBlinkLedTimer();

// Threading
TaskHandle_t waveform_task_handle;
TaskHandle_t output_task_handle;

// Global variables
float ecg_value = 0.0f;      // Shared variable for the ECG value
SemaphoreHandle_t ecg_mutex; // Mutex to protect access to ecg_value
int samplingRate = 1000;     // Sampling rate in Hz

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
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

  delay(1000);
  Serial.println("Setup complete. Starting tasks...");
  setupBlinkLedTimer(); // Setup the LED blink timer
}

void loop()
{
  // Empty!
}

// Modified Version of Dr. Palmeri's digilent-ecg-script.txt
// Generates an ECG Waveform
// Original File in ../ref/digilent-ecg-script.txt
// Original Repo: https://gitlab.oit.duke.edu/kits/BME-554L-001-Sp25/ecg-temp-ble-project/-/blob/main/testing/digilent-ecg-script.txt
// Modified by: Nicholas Trigger

void ecgWaveformTask(void *pvParameters)
{
  const float r_wave_avg_amplitude = 1.0f;   // Average amplitude of the R-wave
  const float amplitude_variance_pct = 0.0f; // Amplitude variance percentage (changes R and T wave amplitude)

  // ------------------------------------------
  // ECG Waveform Parameters
  // AMP, START, WIDTH
  // ------------------------------------------

  // P-wave (atrial depolarization)
  const float A_P = 0.25f, mu_P = 0.15f, sigma_P = 0.025f;  // P
  const float A_Q = -0.12f, mu_Q = 0.33f, sigma_Q = 0.018f; // Q
  const float A_R = 1.1f, mu_R = 0.355f, sigma_R = 0.022f;  // R
  const float A_S = -0.2f, mu_S = 0.40f, sigma_S = 0.035f;  // S
  const float A_T = 0.38f, mu_T = 0.58f, sigma_T = 0.05f;   // Q

  // ST Bridge (smooths S-to-T transition)
  const float A_ST = 0.08f, mu_ST = 0.45f, sigma_ST = 0.12f;

  float X = 0.0f;
  const float step = 1.0f / samplingRate;
  // Heart rate control
  const int heartRate = 60;                     // Target heart rate in beats per minute
  const float beatInterval = 60.0f / heartRate; // Interval between beats in seconds
  float timeSinceLastBeat = 0.0f;
  float noiseAmplitude = 0.0f; // Noise amplitude pct

  while (1)
  {
    float varianceSeed = ((float)random(-50, 50) / 100.0f);
    float scaled_A_R = A_R * (1.0f + (amplitude_variance_pct * varianceSeed));
    float scaled_A_T = A_T * (1.0f + (amplitude_variance_pct * varianceSeed));

    // Generate ECG waveform
    double P_wave = A_P * expf(-pow((X - mu_P), 2) / (2 * pow(sigma_P, 2)));
    double Q_wave = A_Q * expf(-pow((X - mu_Q), 2) / (2 * pow(sigma_Q, 2)));
    double R_wave = scaled_A_R * expf(-pow((X - mu_R), 2) / (2 * pow(sigma_R, 2)));
    double S_wave = A_S * expf(-pow((X - mu_S), 2) / (2 * pow(sigma_S, 2)));
    double T_wave = scaled_A_T * expf(-pow((X - mu_T), 2) / (2 * pow(sigma_T, 2)));
    double ST_bridge = A_ST * expf(-pow((X - mu_ST), 2) / (2 * pow(sigma_ST, 2)));
    double noise = noiseAmplitude * ((float)random(-50, 50) / 100.0f);
    double baseline = 0.05 * sin(2 * PI * 0.33 * X); // 0.33 Hz baseline wander (breathing)

    double waveform = P_wave + Q_wave + R_wave + S_wave + T_wave + ST_bridge + noise + baseline;

    waveform *= r_wave_avg_amplitude;

    // Protect shared variable with mutex
    if (xSemaphoreTake(ecg_mutex, portMAX_DELAY))
    {
      ecg_value = waveform;
      xSemaphoreGive(ecg_mutex);
    }

    X += step; // X is the time variable, where waveform at a time is calculated
    timeSinceLastBeat += step;

    // Reset X to ensure R-wave occurs at the correct interval
    if (timeSinceLastBeat >= beatInterval)
    {
      X = 0.0f;
      timeSinceLastBeat -= beatInterval;
    }

    Serial.print(">ecg:");
    Serial.println(ecg_value);

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
    const float voltage_range = 2.0f;
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
      // Negative: map 0V to -2V to 0–255 (invert for op-amp)
      uint8_t dac2_val = (uint8_t)((-outputValue / voltage_range) * 255.0f);
      dacWrite(DAC_CH2, dac2_val);
      dacWrite(DAC_CH1, 0); // Ensure positive channel is off
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void sendScreenStatus()
{
}

void blinkLedCallback(TimerHandle_t xTimer)
{
  static bool ledState = false;
  digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
  ledState = !ledState;
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