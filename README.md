# ECG_SYN

An ESP32-based ECG signal synthesizer that generates realistic electrocardiogram waveforms and outputs them as analog signals via dual-channel DAC.

## Overview

ECG_SYN synthesizes PQRST cardiac waveforms using Gaussian basis functions (based on the [Digilent/Duke ECG algorithm](ref/digilent-ecg-script.txt)) and drives two DAC outputs on an ESP32 for use as a reference signal source in ECG hardware development, signal processing, and testing.

## Features

- Realistic PQRST waveform synthesis via Gaussian basis functions
- Dual-channel bipolar DAC output (GPIO 25 / GPIO 26)
- Configurable heart rate (default 60 BPM) and sampling rate (default 1000 Hz)
- ±10% amplitude and noise variation per beat for realism
- FreeRTOS dual-core architecture: generation on Core 0, output on Core 1
- Serial monitor stream at 115200 baud for real-time waveform inspection

## Hardware

| Signal | GPIO | Description |
| ------ | ---- | ----------- |
| DAC CH1 | 25 | Positive half of bipolar output |
| DAC CH2 | 26 | Negative half of bipolar output |
| LED | 2 | Heartbeat indicator (1 Hz blink) |

The KiCAD schematic is in [ref/circuitschem.kicad_sch](ref/circuitschem.kicad_sch).

## Waveform Model

Each beat is composed of five Gaussian components:

| Wave | Amplitude | Center | Width | Physiological meaning |
| ---- | --------- | ------ | ----- | --------------------- |
| P | 0.25 | 0.15 s | 0.09 | Atrial depolarization |
| Q | −0.2 | 0.45 s | 0.03 | Septal depolarization |
| R | 1.0 | 0.5 s | 0.03 | Ventricular depolarization |
| S | −0.3 | 0.55 s | 0.03 | Late ventricular depolarization |
| T | 0.45 | 0.65 s | 0.08 | Ventricular repolarization |

Formula: `A × exp(−(x − μ)² / (2σ²))`

The normalized float output `[-1.0, 1.0]` is mapped to DAC byte range `[0, 255]`, split across the two channels for bipolar drive.

## Build & Flash

Requires [PlatformIO](https://platformio.org/).

```sh
# Build
pio run

# Upload
pio run --target upload

# Monitor serial output
pio device monitor
```

Target: `espressif32` / `denky32` (ESP32-WROOM-32), Arduino framework.

## Configuration

Edit the constants at the top of [src/main.cpp](src/main.cpp):

```cpp
const float heartRate = 60.0;   // BPM
const int samplingRate = 1000;  // Hz
```

## Reference

- [ECG timing reference image](ref/ecgtimings.png)
- [Timing sources](ref/timingSources.md)
- [Original Digilent ECG script](ref/digilent-ecg-script.txt)
