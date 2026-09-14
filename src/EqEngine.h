#pragma once
#include <JuceHeader.h>
#include <array>

#include "EqConfig.h"   // TOREI_EQ_DEBUG_LOG (diagnostic-gating switch)

// Static EQ engine: up to 24 bands, each a biquad (RBJ) filter. Band parameters
// are updated on the message thread (from the WebView UI) and applied on the
// audio thread via the JUCE IIR::Filter coefficient pointer swap (real-time safe).
class EqEngine
{
public:
    static constexpr int kMaxBands    = 24;
    static constexpr int kCurvePoints = 512;   // magnitude-response samples for the UI

    // Maximum cascaded sections per band. 48 dB/oct = 4 second-order sections.
    static constexpr int kMaxCascades = 4;

    // Per-band channel routing (MID_SIDE_HANDOFF.md). A band may act on the L/R pair
    // or on the M/S pair, so four filter lanes are maintained per band:
    //   0 = L, 1 = R, 2 = Mid, 3 = Side
    static constexpr int kNumLanes = 4;
    enum Lane { laneL = 0, laneR = 1, laneM = 2, laneS = 3 };

    // Filter types, matching the UI's string identifiers.
    enum Type
    {
        peaking = 0,
        lowshelf,
        highshelf,
        lowpass,
        highpass
    };

    // Per-band channel mode, matching the UI's identifiers exactly.
    enum Mode
    {
        modeStereo = 0,   // "stereo"  - both channels (historical behaviour)
        modeLeft,         // "L"
        modeRight,        // "R"
        modeMid,          // "mid"
        modeSide          // "side"
    };

    // Deep/odd spellings tolerated on input; anything unknown falls back to stereo.
    static Mode modeFromString (const juce::String& s);
    // Round-trips with modeFromString: "stereo" / "L" / "R" / "mid" / "side".
    static const char* modeToString (Mode m);

    // Which bands contribute to a curve (MID_SIDE_HANDOFF.md §5). `stereo` bands
    // count towards every group, because they act on all channels.
    enum CurveGroup
    {
        curveAll = 0,   // every band (legacy EQ_Curve_Data semantics)
        curveMid,       // stereo + mid
        curveSide,      // stereo + side
        curveLeft,      // stereo + L
        curveRight      // stereo + R
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
        int   slope  = 12;      // dB/oct, only meaningful for lowpass/highpass
        Mode  mode   = modeStereo;
        bool  bypass = false;
    };

    // Slope values accepted for lowpass/highpass, in dB/oct. Anything else falls
    // back to 12 (the historical behaviour, one biquad).
    static int normaliseSlope (int slope);
    // Number of cascaded sections a band uses: 1 for every non-cut type, and
    // 1/1/2/4 for lowpass/highpass at 6/12/24/48 dB/oct.
    static int stageCountFor (Type type, int slope);

    void prepare (double sampleRate, int samplesPerBlock);
    void reset();

    // --- Message-thread API (called from the UI event handlers) ---

    void addBand    (int index, Type type, float freq, float gain, float q);
    void removeBand (int index);

    // Sets a single parameter. `param` is one of: "freq", "gain", "q", "type",
    // "slope", "mode", "bypass". `value` may be a number, bool, or string (for
    // "type" and "mode").
    void setParam (int index, const juce::String& param, const juce::var& value);

    // --- Audio-thread API ---
    void process (juce::AudioBuffer<float>& buffer);

    // --- Curve (message thread) ---
    // Fills `gains` with the EQ magnitude response in dB at `maxPoints` log-spaced
    // frequencies from 20 Hz to 20 kHz, summing the bands that belong to `group`.
    // Returns the number written.
    int getCurveGains (float* gains, int maxPoints, CurveGroup group = curveAll) const;

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

    // --- M/S runtime probe (MID_SIDE_HANDOFF.md §13) ---
    // Compiled ONLY when TOREI_EQ_DEBUG_LOG is 1 (see EqConfig.h). It exists purely to
    // produce numeric evidence for the M/S verification round, which is now complete
    // (§18), so in a normal build this API and ALL of its supporting state compile away
    // -- no probe passes, no atomics, no log traffic (§19).
#if TOREI_EQ_DEBUG_LOG
    enum ProbeChannel { probeL = 0, probeR, probeM, probeS };

    // Latest block's RMS level in dBFS for one probe channel, measured at the chain
    // input (post == false) or output (post == true). Read on the message thread.
    float getProbeDb (bool post, int probeChannel) const;

    // Routing coefficient a lane actually reached at the end of the last block
    // (0 = transparent, 1 = fully applied). Used to confirm the ramps converged.
    float getLaneMix (int band, int lane) const;

    // True when at least one occupied, non-bypassed band is routed away from stereo.
    // Drives both the probe measurement and the log gating (all-stereo = silence).
    bool hasNonStereoBand() const;

    // Index of the first routed band, or -1.
    int getFirstNonStereoBand() const;

    // Every occupied, non-bypassed band routed away from stereo, and the count. The
    // probe log lists all of them: otherwise the first routed band monopolises the line
    // and a second one (e.g. a `side` band) is never reported -- and `side` is exactly
    // the case that most needs numeric evidence (MID_SIDE_HANDOFF §16.3).
    int getRoutedBands (int* out, int maxBands) const;

    // Channel mode of one band (modeStereo when the slot is empty/out of range).
    Mode getBandMode (int index) const;
#endif

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
        std::atomic<int>   slope { 12 };          // dB/oct; lowpass/highpass only
        std::atomic<int>   mode  { modeStereo };  // channel routing (Mode enum)
        std::atomic<bool>  occupied { false };   // slot has a live band
        std::atomic<bool>  bypass   { false };

        // How many cascaded sections this band currently uses (mirrors
        // stageCountFor(type, slope), but published as an atomic so the audio thread
        // never has to read the non-atomic `type`).
        std::atomic<int> activeStages { 0 };
    };

    void updateCoefficients (int index);
    void clearBandState (int index);

#if TOREI_EQ_DEBUG_LOG
    // Audio-thread probe accumulation: writes the block's L/R/M/S RMS (dBFS) into the
    // probe atomics. No allocation, no lock, no logging.
    void updateProbe (const juce::AudioBuffer<float>& buffer, bool post);
#endif

    std::array<Band, kMaxBands> bands;

    // Per-band cascaded filter coefficients, one entry per section. Rebuilt on the
    // message thread and published by pointer swap, exactly like the old
    // IIR::Filter::coefficients (the audio thread snapshots each Ptr, holding a
    // reference, so a concurrent swap can never free an object still in use).
    //
    // NOT per lane: the same H applies to whichever lane the band is routed to.
    std::array<std::array<juce::dsp::IIR::Coefficients<float>::Ptr, kMaxCascades>, kMaxBands> coeffs;

    // Per-band-per-section-per-LANE direct-form state (z1, z2) for the manual
    // real-time-safe processing in process(). Four lanes: L, R, Mid, Side.
    // Indexing: [band][section][lane][0 = z1, 1 = z2].
    std::array<std::array<std::array<std::array<float, 2>, kNumLanes>, kMaxCascades>, kMaxBands> filterState;

    // Per-instance listen target (see setListenIndex above).
    std::atomic<int> listenIndex { -1 };

    // Per-band-per-LANE routing ramp. This is both the dry/wet blend (1 = band
    // applied, 0 = transparent) AND the channel routing:
    //
    //   stereo -> L:1 R:1 M:0 S:0      mid -> M:1 (others 0)
    //   L      -> L:1 (others 0)       side-> S:1 (others 0)
    //   R      -> R:1 (others 0)
    //
    // Because each lane has its own ramp, switching mode is a crossfade, so it never
    // clicks -- and no coefficient rebuild or state clear is needed (those were the
    // sources of both earlier click regressions).
    std::array<std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, kNumLanes>, kMaxBands> bandMix;

#if TOREI_EQ_DEBUG_LOG
    // Probe state, written by the audio thread and read by the message thread (§13).
    // Plain atomics: the audio thread only stores, never allocates or locks.
    std::array<std::array<std::atomic<float>, kNumLanes>, kMaxBands> laneMix;
    std::atomic<float> probeInL  { -120.0f };
    std::atomic<float> probeInR  { -120.0f };
    std::atomic<float> probeInM  { -120.0f };
    std::atomic<float> probeInS  { -120.0f };
    std::atomic<float> probeOutL { -120.0f };
    std::atomic<float> probeOutR { -120.0f };
    std::atomic<float> probeOutM { -120.0f };
    std::atomic<float> probeOutS { -120.0f };
#endif

    // --- Solo audition stage (Pro-Q "solo", §0.10) ---
    // Bandpass coefficients for the listened band, rebuilt on the message thread
    // whenever the listen target or its freq/Q changes. Published by pointer swap,
    // and snapshotted (reference held) on the audio thread just like `coeffs`.
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
