#pragma once
#include <JuceHeader.h>
#include <array>

// Static EQ engine: up to 24 bands, each a biquad (RBJ) filter. Band parameters
// are updated on the message thread (from the WebView UI) and applied on the
// audio thread via the JUCE IIR::Filter coefficient pointer swap (real-time safe).
class EqEngine
{
public:
    static constexpr int kMaxBands    = 24;
    static constexpr int kCurvePoints = 200;   // magnitude-response samples for the UI

    // Filter types, matching the UI's string identifiers.
    enum Type
    {
        peaking = 0,
        lowshelf,
        highshelf,
        lowpass,
        highpass
    };

    void prepare (double sampleRate, int samplesPerBlock);
    void reset();

    // --- Message-thread API (called from the UI event handlers) ---

    void addBand    (int index, Type type, float freq, float gain, float q);
    void removeBand (int index);

    // Sets a single parameter. `param` is one of: "freq", "gain", "q", "type",
    // "bypass". `value` may be a number, bool, or string (for "type").
    void setParam (int index, const juce::String& param, const juce::var& value);

    // --- Audio-thread API ---
    void process (juce::AudioBuffer<float>& buffer);

    // --- Curve (message thread) ---
    // Fills `gains` with the total EQ magnitude response in dB at `maxPoints`
    // log-spaced frequencies from 20 Hz to 20 kHz. Returns the number written.
    int getCurveGains (float* gains, int maxPoints) const;

    static Type typeFromString (const juce::String& s);

    // Returns the number of occupied, non-bypassed bands.
    int activeBandCount() const;

    // Returns true if any occupied band is boosting/cutting by at least 1 dB.
    bool hasNonUnityBand() const;

    // Temporary diagnostic (Phase 1): one-line description of the active bands,
    // including the first active band's b0 coefficient and its magnitude response
    // at its own centre frequency (in dB). Confirm the coefficients are non-unity.
    juce::String describe() const;

private:
    struct Band
    {
        Type  type = peaking;
        std::atomic<float> freq { 1000.0f };
        std::atomic<float> gain { 0.0f };
        std::atomic<float> q    { 1.0f };
        std::atomic<bool>  occupied { false };   // slot has a live band
        std::atomic<bool>  bypass   { false };
    };

    void updateCoefficients (int index);

    std::array<Band, kMaxBands> bands;
    std::array<juce::dsp::IIR::Filter<float>, kMaxBands> filters;

    // Per-band-per-channel biquad state (z1, z2) for the manual real-time-safe
    // processing in process(). [band][channel][0]=z1, [channel][1]=z2.
    std::array<std::array<std::array<float, 2>, 2>, kMaxBands> filterState;

    double sampleRate = 44100.0;
};
