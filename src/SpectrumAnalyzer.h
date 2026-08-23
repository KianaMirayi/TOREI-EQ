#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <memory>

// Real-time-safe spectrum analysis bridge between the audio thread and the UI.
//
// IMPORTANT (multi-instance safety): each SpectrumAnalyzer belongs to ONE plugin
// instance, owned as a member of ToreiEQAudioProcessor. The old file-global
// singletons (gAnalyzerPre/gAnalyzerPost) were SHARED by every instance in the
// host process, so two plugins (channels A, B) wrote their samples into the same
// ring buffer and showed each other's spectra. A per-instance analyser fixes that.
// See SPECTRUM_MULTI_INSTANCE_FIX.md.
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
// Threading contract (single instance):
//   - prepare / reset: audio thread, on transport state changes
//     (prepareToPlay / releaseResources / the processor destructor).
//   - push: audio thread (processBlock).
//   - read / setSmoothing: message thread (editor timer).
// The only data shared between the two threads is the AbstractFifo-backed ring
// buffer, which is safe for this single-producer / single-consumer pattern.

// Number of log-spaced output points covering 20 Hz .. 20 kHz.
constexpr int kSpectrumPointCount = 512;

class SpectrumAnalyzer
{
public:
    void prepare (double sampleRate, int samplesPerBlock);
    void reset();

    // Real-time safe: no allocation, no lock, bounded work.
    void push (const float* const* channelData, int numChannels, int numSamples);

    // Runtime tuning: adjusts the smoothing parameters live (safe to call any
    // time from the message thread). Drives THIS analyser only.
    void setSmoothing (float attack, float release, int blurRadius, int dilateRadius, float bandHalfOct);

    // Writes up to destSize smoothed dBFS values (point 0 = 20 Hz .. point N-1 =
    // 20 kHz, log-spaced) into dest, and returns the number of points written, or
    // 0 if no complete frame is ready yet.
    int read (float* dest, int destSize);

private:
    void pushRoll (float sample);

    juce::AbstractFifo fifo { 16384 };   // fifoCapacity

    juce::HeapBlock<float> ringBuffer;
    juce::HeapBlock<float> rollBuffer;    // most recent fftSize samples
    juce::HeapBlock<float> windowBuffer;  // Blackman-Harris table
    juce::HeapBlock<float> fftWorkspace;  // 2 * fftSize floats
    juce::HeapBlock<float> magDb;         // per-bin dBFS (numBins)

    juce::HeapBlock<float> rawPoints;     // adaptive-sampled log points
    juce::HeapBlock<float> dilatedPoints; // after dilation
    juce::HeapBlock<float> envelope;      // persistent temporal envelope

    juce::HeapBlock<float> binCenterArr;   // centre frequency (in bins) per point

    std::unique_ptr<juce::dsp::FFT> fft;

    // Per-instance smoothing parameters (no cross-instance sharing). Read with
    // relaxed ordering on the message thread after being set there; consumed with
    // relaxed ordering on the audio thread in read().
    std::atomic<float> smoothAttack      { 0.50f };
    std::atomic<float> smoothRelease     { 0.96f };
    std::atomic<int>   smoothBlurRadius  { 0 };
    std::atomic<int>   smoothDilateRadius{ 0 };
    std::atomic<float> smoothBandHalfOct { 0.02f };

    float windowSumFloat = 1.0f;
    int   rollPos = 0;
    int   rollFilled = 0;
    bool  hasFrame = false;
    std::atomic<bool> prepared { false };
};
