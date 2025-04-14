#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/timers.h>

#define DAC_CH1 25 

// Function declarations
void ecgWaveformTask(void *pvParameters);
void outputTask(void *pvParameters);

// Threading
TaskHandle_t waveform_task_handle;
TaskHandle_t output_task_handle;

// Global variables
float ecg_value = 0.0f; // Shared variable for the ECG value
SemaphoreHandle_t ecg_mutex; // Mutex to protect access to ecg_value
int samplingRate = 1000; // Sampling rate in Hz

void setup() {
  Serial.begin(115200);

  // Create a mutex for protecting ecg_value
  ecg_mutex = xSemaphoreCreateMutex();

  // Create tasks
  xTaskCreatePinnedToCore(
    ecgWaveformTask,    // Function to implement the task
    "ECG Waveform",     // Name of the task
    10000,              // Stack size in bytes
    NULL,               // Task input parameter
    1,                  // Priority of the task
    &waveform_task_handle, // Task handle
    0);                 // Core where the task should run

  xTaskCreatePinnedToCore(
    outputTask,         // Function to implement the task
    "Output Task",      // Name of the task
    10000,              // Stack size in bytes
    NULL,               // Task input parameter
    1,                  // Priority of the task
    &output_task_handle, // Task handle
    1);                 // Core where the task should run

  delay(1000);
  Serial.println("Setup complete. Starting tasks...");
}

void loop() {
  // Empty!
}

// Modified Version of Dr. Palmeri's digilent-ecg-script.txt
// Generates an ECG Waveform
// Original File in ./ref/digilent-ecg-script.txt
// Original Repo: https://gitlab.oit.duke.edu/kits/BME-554L-001-Sp25/ecg-temp-ble-project/-/blob/main/testing/digilent-ecg-script.txt
// Modified by: Nicholas Trigger

void ecgWaveformTask(void *pvParameters) {
  const float amplitude_max = 1.0f; // Maximum amplitude
  const float amplitude_variance_pct = 0.1f; // Amplitude variance percentage
  const float frequency = 1.0f; // Frequency in Hz

  const float A_P = 0.2f, mu_P = 0.15f, sigma_P = 0.03f;
  const float A_Q = -0.2f, mu_Q = 0.45f, sigma_Q = 0.015f;
  const float A_R = 1.0f, mu_R = 0.5f, sigma_R = 0.02f;
  const float A_S = -0.3f, mu_S = 0.55f, sigma_S = 0.015f;
  const float A_T = 0.4f, mu_T = 0.95f, sigma_T = 0.05f;
  const float E = 2.71828f;

  float X = 0.0f;
  const float step = 1.0f / samplingRate;

  while (1) {
    // Generate ECG waveform
    float P_wave = A_P * pow(E, -pow((X - mu_P), 2) / (2 * pow(sigma_P, 2)));
    float Q_wave = A_Q * pow(E, -pow((X - mu_Q), 2) / (2 * pow(sigma_Q, 2)));
    float R_wave = A_R * pow(E, -pow((X - mu_R), 2) / (2 * pow(sigma_R, 2)));
    float S_wave = A_S * pow(E, -pow((X - mu_S), 2) / (2 * pow(sigma_S, 2)));
    float T_wave = A_T * pow(E, -pow((X - mu_T), 2) / (2 * pow(sigma_T, 2)));
    float noise = 0.1f * ((float)random(-50, 50) / 100.0f);

    float waveform = P_wave + Q_wave + R_wave + S_wave + T_wave + noise;

    // Scale waveform by amplitude and frequency
    waveform *= amplitude_max * (1.0f + amplitude_variance_pct * ((float)random(-50, 50) / 100.0f));

    // Protect shared variable with mutex
    if (xSemaphoreTake(ecg_mutex, portMAX_DELAY)) {
      ecg_value = waveform;
      xSemaphoreGive(ecg_mutex);
    }

    X += step;
    if (X >= 1.0f) {
      X -= 1.0f;
    }

    vTaskDelay(pdMS_TO_TICKS(1)); // Delay for 1 ms
  }
}

void outputTask(void *pvParameters) {
  // Initialize DAC
  // Initialize DAC (ESP32 supports dacWrite directly, no additional configuration needed)
  dacWrite(DAC_CH1, 0); // Set initial value to 0

  while (1) {
    float value_to_output = 0.0f;

    // Protect shared variable with mutex
    if (xSemaphoreTake(ecg_mutex, portMAX_DELAY)) {
      value_to_output = ecg_value;
      xSemaphoreGive(ecg_mutex);
    }

    // Output the waveform to the DAC
    dacWrite(DAC_CH1, (uint8_t)(value_to_output * 127.5f + 127.5f)); // Scale to 8-bit range

    vTaskDelay(pdMS_TO_TICKS(1)); // Delay for 1 ms
  }
}