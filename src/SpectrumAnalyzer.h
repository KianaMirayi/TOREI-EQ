#pragma once

// Real-time-safe spectrum analysis bridge between the audio thread and the UI.
//
// The audio thread pushes mono samples into a lock-free ring buffer (no
// allocation, no locking). The message thread (the editor timer, ~40 Hz) drains
// those samples into a rolling FFT-sized window and, on every call, computes a
// Blackman-Harris windowed forward FFT and turns it into a fixed set of
// LOG-SPACED, already-smoothed output points covering 20 Hz .. 20 kHz.
//
// All visual processing happens here in C++ (not in the Web UI):
//   1. adaptive sampling — cosine interpolation for sparse low-frequency bins,
//      max-hold for dense high-frequency bins (preserves transient peaks);
//   2. spatial smoothing — morphological dilation + Gaussian blur;
//   3. temporal smoothing — asymmetric attack/release (ballistic) envelope.
//
// The UI only draws the resulting smooth curve.
//
// Threading contract:
//   - prepareSpectrum / resetSpectrum: audio thread, on transport state changes
//     (prepareToPlay / releaseResources).
//   - pushAudioToSpectrum: audio thread (processBlock).
//   - readSpectrum: message thread (editor timer).
// The only data shared between the two threads is the AbstractFifo-backed ring
// buffer, which is safe for this single-producer / single-consumer pattern.

// Number of log-spaced output points covering 20 Hz .. 20 kHz.
constexpr int kSpectrumPointCount = 512;

void prepareSpectrum (double sampleRate, int samplesPerBlock);
void resetSpectrum();

void pushAudioToSpectrum (const float* const* channelData, int numChannels, int numSamples);

// Runtime tuning: adjusts the smoothing parameters live (safe to call any time).
void setSpectrumSmoothing (float attack, float release, int blurRadius, int dilateRadius, float bandHalfOct);

// Writes up to destSize smoothed dBFS values (point 0 = 20 Hz .. point N-1 =
// 20 kHz, log-spaced) into dest, and returns the number of points written, or 0
// if no complete frame is ready yet.
int readSpectrum (float* dest, int destSize);
