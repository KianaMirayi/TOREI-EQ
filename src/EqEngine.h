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
    static constexpr int kCurvePoints = 512;   // magnitude-response samples for the UI

    // Filter types, matching the UI's string identifiers.
    enum Type
    {
        peaking = 0,
        lowshelf,
        highshelf,
        lowpass,
        highpass
    };

    // Plain (non-atomic) copy of one band's state, used to hand the whole EQ to the
    // message thread for serialisation / UI mirroring.
    struct BandInfo
    {
        int   index  = -1;
        Type  type   = peaking;
        float freq   = 1000.0f;
        float gain   = 0.0f;
        float q      = 1.0f;
        bool  bypass = false;
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

    // --- Listen / hold-to-listen (message thread sets, audio thread reads) ---
    // `-1` = no listen; otherwise the slot of the single listened band.
    //
    // v2 semantics (Pro-Q "solo", see LISTEN_FEATURE_HANDOFF.md §0.10): while
    // listening you hear the frequency CONTENT around that band (a bandpass of the
    // dry signal), and NONE of the EQ gain -- not even the listened band's own
    // gain. So every band ramps to transparent and a dedicated solo bandpass is
    // blended in afterwards.
    //
    // PER-INSTANCE on purpose: a file-level global here would re-introduce the
    // multi-instance cross-talk fixed in SPECTRUM_MULTI_INSTANCE_FIX.md (two plugin
    // instances in one host would share a single listen target).
    //
    // A single atomic<int> (not one atomic<bool> per band) so only one band can ever
    // be listened to at a time, without the UI having to guarantee mutual exclusion.
    void setListenIndex (int index) { listenIndex.store (index, std::memory_order_relaxed); }
    int  getListenIndex() const     { return listenIndex.load (std::memory_order_relaxed); }

    // Solo audition output volume in dB, spanning +-24 dB. This is the playback
    // level of the solo effect, NOT an EQ parameter, and it is deliberately not
    // saved in getStateInformation. Set from the UI via
    // setParam (-1, "soloLevel", <dB>).
    float getSoloLevelDb() const { return soloLevelDb.load (std::memory_order_relaxed); }

    // Overall output (trim) gain in dB, spanning +-24 dB, applied at the very end of
    // the chain (after the bands and after the solo stage). Unlike the solo level
    // this is a real user setting and IS persisted in getStateInformation.
    float getOutputGainDb() const { return outputGainDb.load (std::memory_order_relaxed); }
    void  setOutputGainDb (float db)
    {
        outputGainDb.store (juce::jlimit (-kOutputGainMaxDb, kOutputGainMaxDb, db),
                            std::memory_order_relaxed);
    }

    static Type typeFromString (const juce::String& s);

    // Inverse of typeFromString; round-trips exactly with it. Returns one of
    // "peaking" / "lowshelf" / "highshelf" / "lowpass" / "highpass" (the identifiers
    // the Web UI uses).
    static const char* typeToString (Type t);

    // --- Whole-state access (message thread only) ---
    // Copies every OCCUPIED band (bypassed ones included, with bypass=true) into
    // `out`, in ascending slot order. Returns the number of bands written; empty
    // slots are skipped. No allocation, no audio-thread involvement.
    int getBandSnapshot (BandInfo* out, int maxBands) const;

    // Removes every band and clears the solo/listen state -- used by
    // setStateInformation before restoring a saved state, so slots absent from the
    // incoming state cannot linger and blend with it.
    void clearAllBands();

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

    // Per-instance listen target (see setListenIndex above).
    std::atomic<int> listenIndex { -1 };

    // Per-band-per-channel dry/wet ramp (1 = band applied, 0 = transparent).
    // Listen and bypass both change this target, so switching either one fades
    // instead of hard-cutting (which would click). SmoothedValue is allocation-free
    // and safe to advance on the audio thread.
    std::array<std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, 2>, kMaxBands> bandMix;

    // --- Solo audition stage (Pro-Q "solo", §0.10) ---
    // Bandpass coefficients for the listened band, rebuilt on the message thread
    // whenever the listen target or its freq/Q changes. Published by pointer swap,
    // and snapshotted (reference held) on the audio thread just like `filters`.
    juce::dsp::IIR::Coefficients<float>::Ptr soloCoeffs;
    std::array<std::array<float, 2>, 2> soloState;   // [channel][z1/z2]

    // Ramped so entering/leaving solo, and dragging the solo volume, never click or
    // zipper. `soloMix` blends the bandpass in (0 = dry, 1 = solo); `soloGain`
    // carries the +-24 dB solo level.
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, 2> soloMix;
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, 2> soloGain;

    std::atomic<float> soloLevelDb { 0.0f };   // +-24 dB, playback level only

    // --- Output (trim) gain: the last stage of the whole chain ---
    // 20 ms ramp per channel so dragging the Output control never zippers. Per
    // instance, and persisted by the processor.
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, 2> outputGain;
    std::atomic<float> outputGainDb { 0.0f };

    // Rebuilds `soloCoeffs` from the listened band's freq/Q (message thread).
    void updateSoloCoefficients();

    static constexpr double kMixRampSeconds    = 0.010;   // 10 ms
    static constexpr double kOutputRampSeconds = 0.020;   // 20 ms
    static constexpr float  kSoloLevelMaxDb    = 24.0f;
    static constexpr float  kOutputGainMaxDb   = 24.0f;

    double sampleRate = 44100.0;
};
