#include "EqEngine.h"

void EqEngine::prepare (double sr, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    sampleRate = sr;

    // The curve is a function of the coefficient magnitudes AT THIS RATE, so a
    // sample-rate change is a curve change (see EqEngine.h getCurveRevision()).
    bumpCurveRevision();

    // Reset filter processing state, but PRESERVE the band definitions.
    reset();

    // Recompute coefficients at the (possibly new) sample rate for any bands the
    // UI has already added. Without this, a sample-rate change leaves the band
    // coefficients computed for the previous rate.
    for (int i = 0; i < kMaxBands; ++i)
        if (bands[i].occupied.load (std::memory_order_relaxed))
            updateCoefficients (i);
}

void EqEngine::reset()
{
    // Clear every band's processing state (so no stale filter memory survives a
    // transport/sample-rate change). IMPORTANT: the band definitions
    // (occupied/type/freq/gain/q/slope/bypass) must be PRESERVED here. Clearing them
    // would wipe the user's EQ every time prepareToPlay/releaseResources runs,
    // and since the UI only sends AddBand once on mount, the bands would never
    // come back -- so setParam/process/getCurveGains all silently no-op.
    //
    // The coefficients are intentionally left in place (the band stays occupied and
    // its state is simply zeroed). We no longer use IIR::Filter here at all, so the
    // "reset() dereferences null coefficients" hazard is gone.
    for (int i = 0; i < kMaxBands; ++i)
        clearBandState (i);

    // Listen is transient audition state: a transport/sample-rate change clears it
    // (it is deliberately NOT persisted in getStateInformation).
    listenIndex.store (-1, std::memory_order_relaxed);

    // Restart every routing ramp from 0 (fully transparent) so the next process() call
    // fades the bands in rather than jumping straight to full gain.
    for (int i = 0; i < kMaxBands; ++i)
        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            bandMix[i][lane].reset (sampleRate, kMixRampSeconds);

#if TOREI_EQ_DEBUG_LOG
            // Explicitly initialise the published probe values: a default-constructed
            // std::atomic is not guaranteed to be zero.
            laneMix[i][lane].store (0.0f, std::memory_order_relaxed);
#endif
        }

#if TOREI_EQ_DEBUG_LOG
    // Solo-stage probe (§26) starts with no data so the SOLO log reports -120 dB until a
    // block has actually been auditioned.
    for (int lane = 0; lane < kNumLanes; ++lane)
        soloWeightCurrent[lane].store (0.0f, std::memory_order_relaxed);

    soloProbeL2.store (0.0f, std::memory_order_relaxed);
    soloProbeR2.store (0.0f, std::memory_order_relaxed);
    soloProbeLR.store (0.0f, std::memory_order_relaxed);
    soloProbeN .store (0,    std::memory_order_relaxed);
#endif

    // Solo audition stage: drop the bandpass and restart its ramps from "dry" so no
    // stale solo state or filter memory survives a transport/sample-rate change.
    // NOTE: soloLevelDb is intentionally NOT reset -- it is the user's solo playback
    // level (the UI owns it and keeps showing it), not transport state.
    soloCoeffs = nullptr;

    for (int lane = 0; lane < kNumLanes; ++lane)
    {
        soloState[lane][0] = 0.0f;
        soloState[lane][1] = 0.0f;
        soloWeight[lane].reset (sampleRate, kMixRampSeconds);
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        soloMix[ch].reset (sampleRate, kMixRampSeconds);
        soloGain[ch].reset (sampleRate, kMixRampSeconds);
    }

    // Output trim: restart the ramp already AT the current setting. reset() alone
    // would park it at unity gain, making the output audibly fade in from 0 dB after
    // every transport start / sample-rate change.
    const float outGain = juce::Decibels::decibelsToGain (outputGainDb.load (std::memory_order_relaxed));
    for (int ch = 0; ch < 2; ++ch)
    {
        outputGain[ch].reset (sampleRate, kOutputRampSeconds);
        outputGain[ch].setCurrentAndTargetValue (outGain);
    }
}

void EqEngine::updateSoloCoefficients()
{
    const int s = listenIndex.load (std::memory_order_relaxed);

    if (s < 0 || s >= kMaxBands || ! bands[s].occupied.load (std::memory_order_relaxed))
        return;   // leave any previous coefficients in place; the blend ramps out

    // Clamp defensively: the band's freq/Q come from the UI and JUCE asserts on an
    // out-of-range frequency (must stay below Nyquist).
    const float freq = juce::jlimit (20.0f,
                                     juce::jmin (20000.0f, (float) (sampleRate * 0.45)),
                                     bands[s].freq.load (std::memory_order_relaxed));
    const float q    = juce::jmax (0.1f, bands[s].q.load (std::memory_order_relaxed));

    // Constant-0 dB-peak bandpass (verified: |H| at the centre is 1.0 for any Q), so
    // no gain compensation is needed and Q alone sets the audible width.
    soloCoeffs = juce::dsp::IIR::Coefficients<float>::makeBandPass (sampleRate, freq, q);
}

EqEngine::Type EqEngine::typeFromString (const juce::String& s)
{
    if (s == "lowshelf")  return lowshelf;
    if (s == "highshelf") return highshelf;
    if (s == "lowpass")   return lowpass;
    if (s == "highpass")  return highpass;
    return peaking;
}

const char* EqEngine::typeToString (Type t)
{
    switch (t)
    {
        case lowshelf:  return "lowshelf";
        case highshelf: return "highshelf";
        case lowpass:   return "lowpass";
        case highpass:  return "highpass";
        case peaking:
        default:        return "peaking";
    }
}

EqEngine::Mode EqEngine::modeFromString (const juce::String& s)
{
    // Case-insensitive, and tolerant of a few spellings, so the UI can send "L"/"R"
    // (the documented contract) without us being brittle about it.
    const auto v = s.trim().toLowerCase();

    if (v == "l" || v == "left")   return modeLeft;
    if (v == "r" || v == "right")  return modeRight;
    if (v == "mid" || v == "m")    return modeMid;
    if (v == "side" || v == "s")   return modeSide;

    return modeStereo;   // "stereo", empty, or anything unrecognised
}

const char* EqEngine::modeToString (Mode m)
{
    switch (m)
    {
        case modeLeft:  return "L";
        case modeRight: return "R";
        case modeMid:   return "mid";
        case modeSide:  return "side";
        case modeStereo:
        default:        return "stereo";
    }
}

int EqEngine::getBandSnapshot (BandInfo* out, int maxBands) const
{
    if (out == nullptr || maxBands <= 0)
        return 0;

    int n = 0;

    for (int i = 0; i < kMaxBands && n < maxBands; ++i)
    {
        if (! bands[i].occupied.load (std::memory_order_relaxed))
            continue;   // empty slots are deliberately absent from the snapshot

        auto& e  = out[n++];
        e.index  = i;
        e.type   = bands[i].type;
        e.freq   = bands[i].freq.load   (std::memory_order_relaxed);
        e.gain   = bands[i].gain.load   (std::memory_order_relaxed);
        e.q      = bands[i].q.load      (std::memory_order_relaxed);
        e.slope  = bands[i].slope.load  (std::memory_order_relaxed);
        e.mode   = (Mode) bands[i].mode.load (std::memory_order_relaxed);
        e.bypass = bands[i].bypass.load (std::memory_order_relaxed);
    }

    return n;
}

int EqEngine::normaliseSlope (int slope)
{
    // Whitelist, not an index: an old UI sending `1` must become 12 dB/oct, not
    // "section 1". Anything unrecognised falls back to the historical default.
    return (slope == 6 || slope == 12 || slope == 24 || slope == 48) ? slope : 12;
}

int EqEngine::stageCountFor (Type type, int slope)
{
    // Only the cut types have a slope; everything else stays a single section and
    // keeps using Q for its shape.
    if (type != lowpass && type != highpass)
        return 1;

    switch (normaliseSlope (slope))
    {
        case 6:  return 1;   // one FIRST-ORDER section
        case 24: return 2;
        case 48: return 4;
        case 12:
        default: return 1;   // one second-order section
    }
}

void EqEngine::clearBandState (int index)
{
    for (int s = 0; s < kMaxCascades; ++s)
        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            filterState[index][s][lane][0] = 0.0f;
            filterState[index][s][lane][1] = 0.0f;
        }
}

void EqEngine::clearAllBands()
{
    // Solo/listen is transient audition state, so a wholesale state replace drops it
    // too. The audio thread snapshots `soloCoeffs` while holding a reference, so
    // clearing it here is safe even mid-block.
    listenIndex.store (-1, std::memory_order_relaxed);
    soloCoeffs = nullptr;

    for (int i = 0; i < kMaxBands; ++i)
    {
        bands[i].occupied.store     (false, std::memory_order_relaxed);
        bands[i].bypass.store       (false, std::memory_order_relaxed);
        bands[i].activeStages.store (0,     std::memory_order_relaxed);

        // Release every section's coefficients (the audio thread holds a reference to
        // whatever it snapshotted, so this is safe mid-block).
        for (int s = 0; s < kMaxCascades; ++s)
            coeffs[i][s] = nullptr;

        clearBandState (i);
    }

    // Every band is gone, so the curve is (or is about to be) flat again.
    bumpCurveRevision();

    for (int lane = 0; lane < kNumLanes; ++lane)
    {
        soloState[lane][0] = 0.0f;
        soloState[lane][1] = 0.0f;
    }
}

void EqEngine::updateCoefficients (int index)
{
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    // Non-const: we publish `activeStages` on this band at the end.
    Band& b = bands[index];
    const float freq = b.freq.load();
    const float q    = b.q.load();
    const float gain = b.gain.load();
    const int   slope = normaliseSlope (b.slope.load (std::memory_order_relaxed));

    const int stages = stageCountFor (b.type, slope);

    // Cut types are the only ones that cascade. `useFirstOrder` selects the 6 dB/oct
    // case, which is a single first-order section (a 1-pole filter has no Q).
    const bool isCut        = (b.type == lowpass || b.type == highpass);
    const bool useFirstOrder = isCut && slope == 6;

    // ---------------------------------------------------------------------------
    // Resonance handling for cascades.
    //
    // N identical sections each peaking at ~Q compound to a total peak of ~Q^N, so
    // passing the user's Q to every section unchanged is dangerous: at the UI's
    // maximum Q of 40 with 4 sections that is ~+128 dB at the cutoff. We therefore
    // spread the resonance: each section gets Q^(1/N), which makes the cascade's
    // total peak come out at ~Q, i.e. the same resonance the single-section case has.
    // N == 1 is untouched (stageQ == Q), so 12 dB/oct behaves exactly as before.
    // ---------------------------------------------------------------------------
    const float stageQ = (stages > 1 && q > 0.0f)
                           ? std::pow (q, 1.0f / (float) stages)
                           : q;

    Coeffs::Ptr c;

    switch (b.type)
    {
        case peaking:
            c = Coeffs::makePeakFilter (sampleRate, freq, q, juce::Decibels::decibelsToGain (gain));
            break;
        case lowshelf:
            c = Coeffs::makeLowShelf (sampleRate, freq, q, juce::Decibels::decibelsToGain (gain));
            break;
        case highshelf:
            c = Coeffs::makeHighShelf (sampleRate, freq, q, juce::Decibels::decibelsToGain (gain));
            break;
        case lowpass:
            c = useFirstOrder ? Coeffs::makeFirstOrderLowPass (sampleRate, freq)
                              : Coeffs::makeLowPass (sampleRate, freq, stageQ);
            break;
        case highpass:
            c = useFirstOrder ? Coeffs::makeFirstOrderHighPass (sampleRate, freq)
                              : Coeffs::makeHighPass (sampleRate, freq, stageQ);
            break;
    }

    // Publish the same coefficients to every active section (pointer swap is
    // real-time safe: the audio thread keeps using the previous objects until this
    // store lands) and drop the sections beyond the new count.
    for (int s = 0; s < kMaxCascades; ++s)
        coeffs[index][s] = (s < stages) ? c : nullptr;

    // Wipe this band's state ONLY when the cascade length actually changes.
    //
    // The length change is the case the handoff calls out (§4.5): a section that is
    // now unused, or newly reused, would otherwise keep stale filter memory and leak
    // a transient. But clearing on EVERY coefficient rebuild is a regression -- it
    // zeroes the filter memory on every single step of a frequency / gain / Q drag,
    // turning each step into a discontinuity, i.e. an audible click. It is worst on
    // shelves and cuts, where the state carries a lot of energy. A plain coefficient
    // swap (no clear) is the long-standing behaviour and stays click-free.
    const int previousStages = b.activeStages.load (std::memory_order_relaxed);

    if (stages != previousStages)
        clearBandState (index);

    // Publish last: the audio thread reads the stage count to decide how many
    // sections to run.
    b.activeStages.store (stages, std::memory_order_relaxed);
}

void EqEngine::addBand (int index, Type type, float freq, float gain, float q)
{
    if (index < 0 || index >= kMaxBands)
        return;

    Band& b = bands[index];
    b.type = type;
    b.freq.store (freq);
    b.gain.store (gain);
    b.q.store (q);
    b.slope.store (12);              // fresh bands are 12 dB/oct, the historical default
    b.mode.store (modeStereo);       // and act on both channels, as before
    b.bypass.store (false);
    updateCoefficients (index);
    b.occupied.store (true);

    // A new occupied band changes the curve, so let the editor know it must re-push.
    bumpCurveRevision();
}

void EqEngine::removeBand (int index)
{
    if (index < 0 || index >= kMaxBands)
        return;

    bands[index].occupied.store (false);
    bands[index].activeStages.store (0, std::memory_order_relaxed);

    // The band no longer contributes to any curve group.
    bumpCurveRevision();

    // Never leave the listen target pointing at a slot that no longer exists.
    if (listenIndex.load (std::memory_order_relaxed) == index)
        listenIndex.store (-1, std::memory_order_relaxed);

    // Clear this band's filter state. The coefficients are kept (see reset() for
    // why), and there is nothing to dereference here any more -- the old
    // IIR::Filter::reset() null-coefficient crash is gone with the IIR::Filter.
    clearBandState (index);
}

void EqEngine::setParam (int index, const juce::String& param, const juce::var& value)
{
    // --- Solo audition volume (§0.10 item 3) ---
    // Handled BEFORE the index/occupancy guard because the UI sends it with
    // index = -1 (it is a global setting, not a per-band one).
    if (param == "soloLevel")
    {
        const float db = juce::jlimit (-kSoloLevelMaxDb, kSoloLevelMaxDb, (float) (double) value);
        soloLevelDb.store (db, std::memory_order_relaxed);
        return;
    }

    // --- Output (trim) gain (OUTPUT_GAIN_HANDOFF.md) ---
    // Same reason: global setting, sent with index = -1, so it must be handled before
    // the index guard below.
    if (param == "outputGain")
    {
        setOutputGainDb ((float) (double) value);
        return;
    }

    if (index < 0 || index >= kMaxBands || ! bands[index].occupied.load())
        return;

    // One unconditional bump for EVERY per-band parameter, before the branches below.
    // Deliberately coarse (and cheap: one relaxed fetch_add):
    //   * `mode` and `bypass` do NOT rebuild coefficients, yet they DO change the curve
    //     (the group a band appears in, and whether it appears at all), so hanging the
    //     revision off updateCoefficients() alone would miss them;
    //   * a branch added here later automatically bumps without anyone remembering to.
    // A redundant bump only costs one extra curve push; a missing one would freeze the
    // displayed curve. See EqEngine.h getCurveRevision().
    bumpCurveRevision();

    Band& b = bands[index];

    if (param == "freq")
    {
        b.freq.store ((float) (double) value);
        updateCoefficients (index);

        // While soloing, dragging the band moves the solo centre frequency, so the
        // bandpass must follow (see §0.10 item 2).
        if (listenIndex.load (std::memory_order_relaxed) == index)
            updateSoloCoefficients();
    }
    else if (param == "gain")
    {
        // NOTE: the solo bandpass deliberately ignores `gain` -- in solo you hear
        // the frequency content, not the band's EQ gain (§0.10).
        b.gain.store ((float) (double) value);
        updateCoefficients (index);
    }
    else if (param == "q")
    {
        b.q.store ((float) (double) value);
        updateCoefficients (index);

        // Q sets the audible solo bandwidth, so it must follow too.
        if (listenIndex.load (std::memory_order_relaxed) == index)
            updateSoloCoefficients();
    }
    else if (param == "type")
    {
        b.type = typeFromString (value.toString());
        updateCoefficients (index);
    }
    else if (param == "slope")
    {
        // dB/oct, whitelisted (6/12/24/48); anything else -> 12. Only lowpass/highpass
        // act on it -- stageCountFor() returns 1 for every other type, so those bands
        // simply rebuild their single section and ignore the value.
        b.slope.store (normaliseSlope ((int) (double) value), std::memory_order_relaxed);
        updateCoefficients (index);
    }
    else if (param == "mode")
    {
        // Channel routing: "stereo" / "L" / "R" / "mid" / "side" (string, like "type").
        //
        // Deliberately NOTHING else happens here: no coefficient rebuild, no state
        // clear. The mode only changes which lane's routing ramp runs to 1 (see
        // process()), and because each lane has its own 10 ms ramp this is a crossfade
        // -- which is exactly why switching mode cannot click. Rebuilding coefficients
        // or clearing state here is what caused both earlier click regressions.
        b.mode.store ((int) modeFromString (value.toString()), std::memory_order_relaxed);
    }
    else if (param == "bypass")
    {
        const bool bypassed = (bool) value;
        b.bypass.store (bypassed);

        // A bypassed band has nothing to audition.
        if (bypassed && listenIndex.load (std::memory_order_relaxed) == index)
            listenIndex.store (-1, std::memory_order_relaxed);
    }
    else if (param == "listen")
    {
        // Hold-to-listen: `true` moves the single listen target to this band,
        // `false` clears it only if this band is the one being listened to
        // (the UI sends the old band's `false` before the new band's `true`).
        //
        // Deliberately NO updateCoefficients() here: solo does not touch the band's
        // EQ coefficients at all (it bandpasses the dry signal instead). What it
        // DOES need is the solo bandpass built for the new target band.
        if ((bool) value)
            listenIndex.store (index, std::memory_order_relaxed);
        else if (listenIndex.load (std::memory_order_relaxed) == index)
            listenIndex.store (-1, std::memory_order_relaxed);

        updateSoloCoefficients();
    }
}

void EqEngine::process (juce::AudioBuffer<float>& buffer)
{
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = std::min (buffer.getNumChannels(), 2);

    // Listen target for this block (-1 = off). Read once; the audio thread is the
    // only writer of the per-band mix ramps below.
    const int listen = listenIndex.load (std::memory_order_relaxed);

    // --- M/S runtime probe (MID_SIDE_HANDOFF.md §13) ---
    // Measure the chain's input and output L/R/M/S levels so the effect of a routed band
    // can be verified numerically. Only compiled when TOREI_EQ_DEBUG_LOG is 1: the M/S
    // verification is complete (§18), so a normal build does not run these passes at all.
#if TOREI_EQ_DEBUG_LOG
    const bool probe = hasNonStereoBand();

    if (probe)
        updateProbe (buffer, false);   // chain input
#endif

    for (int i = 0; i < kMaxBands; ++i)
    {
        // Unoccupied slots have no coefficients and nothing to process.
        if (! bands[i].occupied.load (std::memory_order_relaxed))
            continue;

        // --- Channel routing (MID_SIDE_HANDOFF.md) -------------------------------
        // Each band maintains four filter lanes (L, R, Mid, Side). `mode` chooses which
        // lane's routing ramp runs to 1; all the others run to 0. Because every lane has
        // its own 10 ms ramp, changing mode is a plain crossfade -- no coefficient
        // rebuild and no state clear, which is exactly why it cannot click.
        //
        //   stereo -> L:1 R:1     L -> L:1     R -> R:1
        //   mid    -> M:1         side -> S:1
        const bool bypassed = bands[i].bypass.load (std::memory_order_relaxed);

        // A band participates at all only when it is not bypassed and nothing is being
        // soloed. While listening EVERY band goes transparent, including the listened
        // one: with all bands transparent the chain output IS the untouched dry signal,
        // which the solo stage below then auditions -- that is why solo needs no
        // separate dry-signal buffer.
        const bool enabled = (listen < 0) && ! bypassed;

        // When the band is disabled the mode is irrelevant; assume stereo so that the
        // L/R lanes (the native domain) are the ones kept warm.
        const Mode mode = enabled ? (Mode) bands[i].mode.load (std::memory_order_relaxed)
                                  : modeStereo;

        float laneTarget[kNumLanes] = {};

        if (enabled)
        {
            switch (mode)
            {
                case modeLeft:  laneTarget[laneL] = 1.0f; break;
                case modeRight: laneTarget[laneR] = 1.0f; break;
                case modeMid:   laneTarget[laneM] = 1.0f; break;
                case modeSide:  laneTarget[laneS] = 1.0f; break;
                case modeStereo:
                default:        laneTarget[laneL] = 1.0f;
                                laneTarget[laneR] = 1.0f; break;
            }
        }

        // The lanes of the band's CURRENT domain always run, even at ramp 0, so a
        // bypass fade-out / fade-in never resumes from frozen filter memory. The other
        // domain's lanes only run while their ramp is non-zero (i.e. during and after a
        // mode switch). That is what keeps the common all-stereo case at exactly the
        // previous cost -- 2 lanes, not 4 -- while still being click-free on a switch.
        const bool msDomain = (mode == modeMid || mode == modeSide);
        const bool alwaysRun[kNumLanes] = { ! msDomain, ! msDomain, msDomain, msDomain };

        // Number of cascaded sections this band runs (1 for every non-cut type, and
        // 1/1/2/4 for lowpass/highpass at 6/12/24/48 dB/oct).
        const int stages = juce::jlimit (0, kMaxCascades,
                                         bands[i].activeStages.load (std::memory_order_relaxed));

        if (stages <= 0)
            continue;

        // Snapshot every active section's coefficients up front. Copying each
        // ReferenceCountedObjectPtr holds a reference, so a concurrent coefficient
        // swap on the UI/WebView2 thread cannot free an object while we use it (this
        // is what juce's IIR::Filter::processSample does NOT do, and is the source of
        // a heap overrun when coefficients are swapped during real-time processing).
        juce::dsp::IIR::Coefficients<float>::Ptr stagePtrs[kMaxCascades];

        float sB0[kMaxCascades] = {}, sB1[kMaxCascades] = {}, sB2[kMaxCascades] = {};
        float sA1[kMaxCascades] = {}, sA2[kMaxCascades] = {};

        bool coeffsReady = true;

        for (int s = 0; s < stages; ++s)
        {
            stagePtrs[s] = coeffs[i][s];

            if (stagePtrs[s] == nullptr)
            {
                coeffsReady = false;
                break;
            }

            // Read by ORDER, not a fixed stride: getRawCoefficients() has length
            // 2*order + 1 *minus* a0 (JUCE's assignImpl drops the a0 entry and
            // normalises by it), so a second-order section is [b0,b1,b2,a1,a2] (5)
            // but a FIRST-order section is only [b0,b1,a1] (3). The previous
            // fixed c[0..4] read would have run off the end of the array for the
            // 6 dB/oct case. Setting b2/a2 to 0 lets the cascade loop below stay
            // uniform: the difference equation then degenerates correctly.
            const int order = (int) stagePtrs[s]->getFilterOrder();
            const auto* c   = stagePtrs[s]->getRawCoefficients();

            sB0[s] = c[0];
            sB1[s] = c[1];
            sB2[s] = (order >= 2) ? c[2] : 0.0f;
            sA1[s] = c[(order >= 2) ? 3 : 2];
            sA2[s] = (order >= 2) ? c[4] : 0.0f;
        }

        if (! coeffsReady)
            continue;

        float* dL = (numChannels > 0) ? buffer.getWritePointer (0) : nullptr;
        float* dR = (numChannels > 1) ? buffer.getWritePointer (1) : nullptr;

        if (dL == nullptr)
            continue;

        // Lane state, hoisted out of the sample loop and written back afterwards.
        float z1[kNumLanes][kMaxCascades] = {};
        float z2[kNumLanes][kMaxCascades] = {};

        // Routing coefficient per lane, also hoisted so the end-of-block values can be
        // published into `laneMix` for the message-thread probe log (§13).
        float c[kNumLanes] = {};

        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            bandMix[i][lane].setTargetValue (laneTarget[lane]);

            for (int s = 0; s < stages; ++s)
            {
                z1[lane][s] = filterState[i][s][lane][0];
                z2[lane][s] = filterState[i][s][lane][1];
            }
        }

        for (int n = 0; n < numSamples; ++n)
        {
            const float xL = dL[n];

            // Mono bus: the single channel IS the mid content, so treat R == L. The
            // M/S formulas then yield M = xL and S = 0 by themselves, which is exactly
            // the documented mono behaviour -- no special case needed.
            const float xR = (dR != nullptr) ? dR[n] : xL;

            const float mid  = 0.5f * (xL + xR);
            const float side = 0.5f * (xL - xR);

            const float laneIn[kNumLanes] = { xL, xR, mid, side };
            float laneOut[kNumLanes] = { xL, xR, mid, side };   // passthrough default

            // The routing ramps must advance exactly once per sample for EVERY lane, so
            // even a skipped lane still reaches its target. `c` outlives the sample loop
            // so the last values can be published for the message-thread probe log.
            for (int lane = 0; lane < kNumLanes; ++lane)
                c[lane] = bandMix[i][lane].getNextValue();

            for (int lane = 0; lane < kNumLanes; ++lane)
            {
                // Skip the maths entirely when this lane is transparent AND not part of
                // the current domain. Skipping is exact: its term in the output matrix
                // below is multiplied by c == 0.
                if (c[lane] == 0.0f && ! alwaysRun[lane])
                    continue;

                // Run the section cascade. Order is irrelevant to the result (linear
                // system) but is fixed so each section keeps its own state. The cascade
                // always runs and its state is always written back even when the lane is
                // transparent, so the ramp returning does not hit stale memory.
                float y = laneIn[lane];

                for (int s = 0; s < stages; ++s)
                {
                    const float in  = y;
                    const float out = sB0[s] * in + z1[lane][s];
                    z1[lane][s] = sB1[s] * in - sA1[s] * out + z2[lane][s];
                    z2[lane][s] = sB2[s] * in - sA2[s] * out;
                    y = out;
                }

                laneOut[lane] = y;
            }

            // --- Output matrix ---------------------------------------------------
            // IMPORTANT: the Mid and Side results belong to BOTH output channels.
            // Mid is added to L and R with the same sign; Side is added to L and
            // SUBTRACTED from R. Wiring this as "one lane per output channel" is the
            // classic way to end up with a mid band that only affects the left side.
            //
            //   L_out = L + cL(yL-L) + cM(yM-M) + cS(yS-S)
            //   R_out = R + cR(yR-R) + cM(yM-M) - cS(yS-S)
            //
            // Which is exact for every mode: e.g. mid (cM=1) gives
            // L_out = L + (H(M)-M) = H(M)+S and R_out = H(M)-S, as derived in
            // MID_SIDE_HANDOFF.md §9.
            const float midDelta  = c[laneM] * (laneOut[laneM] - mid);
            const float sideDelta = c[laneS] * (laneOut[laneS] - side);

            const float outL = xL + c[laneL] * (laneOut[laneL] - xL) + midDelta + sideDelta;
            const float outR = xR + c[laneR] * (laneOut[laneR] - xR) + midDelta - sideDelta;

            dL[n] = outL;

            if (dR != nullptr)
                dR[n] = outR;
        }

        for (int lane = 0; lane < kNumLanes; ++lane)
            for (int s = 0; s < stages; ++s)
            {
                filterState[i][s][lane][0] = z1[lane][s];
                filterState[i][s][lane][1] = z2[lane][s];
            }

#if TOREI_EQ_DEBUG_LOG
        // Publish the routing actually used (end of block) for the probe log. Plain
        // atomic stores: no allocation, no lock, no IO on the audio thread.
        for (int lane = 0; lane < kNumLanes; ++lane)
            laneMix[i][lane].store (c[lane], std::memory_order_relaxed);
#endif
    }

    // --- Solo audition stage (§0.10 / §22) ---
    // Appends a bandpass of the DRY signal (the chain above is fully transparent while
    // listening), so you hear the frequency content around the listened band with none
    // of the EQ gain.
    //
    // The audition is built from the LANES the listened band acts on (§22). That is what
    // makes L solo only the left ear, R only the right, and mid/side mono (both output
    // channels identical) -- matching Pro-Q, where soloing a band plays the spectrum
    // that band affects, and only that.
    //
    //   stereo -> bp(L) on L, bp(R) on R
    //   L      -> bp(L) on L, silence on R
    //   R      -> silence on L, bp(R) on R
    //   mid    -> bp(M) on BOTH        (M = (L+R)/2)
    //   side   -> bp(S) on BOTH        (S = (L-R)/2)
    //
    // The blend still has the same shape as before -- out = (1-m)*x + m*t -- so all the
    // existing ramp behaviour (and the no-click guarantee) is unchanged. For `stereo`
    // the weights multiply by 1.0 and the extra lanes contribute exactly 0, so the
    // result is bit-identical to the previous implementation.
    {
        // Snapshot: holding the reference keeps the object alive if the message
        // thread swaps the coefficients mid-block.
        auto sc = soloCoeffs;

        const float targetSoloMix  = (listen >= 0) ? 1.0f : 0.0f;
        const float targetSoloGain = juce::Decibels::decibelsToGain (soloLevelDb.load (std::memory_order_relaxed));

        // Same routing table the band chain uses, read from the listened band's mode.
        float weightTarget[kNumLanes] = {};

        if (listen >= 0 && listen < kMaxBands)
        {
            switch ((Mode) bands[listen].mode.load (std::memory_order_relaxed))
            {
                case modeLeft:  weightTarget[laneL] = 1.0f; break;
                case modeRight: weightTarget[laneR] = 1.0f; break;
                case modeMid:   weightTarget[laneM] = 1.0f; break;
                case modeSide:  weightTarget[laneS] = 1.0f; break;
                case modeStereo:
                default:        weightTarget[laneL] = 1.0f;
                                weightTarget[laneR] = 1.0f; break;
            }
        }

        if (sc != nullptr)
        {
            // Read by ORDER, like the band chain: a second-order section is
            // [b0,b1,b2,a1,a2] (5 values) but a first-order one is [b0,b1,a1] (3).
            const int order = (int) sc->getFilterOrder();
            const auto* c   = sc->getRawCoefficients();

            const float sb0 = c[0];
            const float sb1 = c[1];
            const float sb2 = (order >= 2) ? c[2] : 0.0f;
            const float sa1 = c[(order >= 2) ? 3 : 2];
            const float sa2 = (order >= 2) ? c[4] : 0.0f;

            for (int lane = 0; lane < kNumLanes; ++lane)
                soloWeight[lane].setTargetValue (weightTarget[lane]);

            for (int ch = 0; ch < 2; ++ch)
            {
                soloMix[ch].setTargetValue (targetSoloMix);
                soloGain[ch].setTargetValue (targetSoloGain);
            }

            float* dL = (numChannels > 0) ? buffer.getWritePointer (0) : nullptr;
            float* dR = (numChannels > 1) ? buffer.getWritePointer (1) : nullptr;

            float z1[kNumLanes], z2[kNumLanes];

            for (int lane = 0; lane < kNumLanes; ++lane)
            {
                z1[lane] = soloState[lane][0];
                z2[lane] = soloState[lane][1];
            }

            // Hoisted out of the sample loop so the end-of-block values can be published
            // for the SOLO log (§26).
            float w[kNumLanes] = {};

#if TOREI_EQ_DEBUG_LOG
            float sumL2 = 0.0f, sumR2 = 0.0f, sumLR = 0.0f;
#endif

            for (int n = 0; n < numSamples && dL != nullptr; ++n)
            {
                const float xL = dL[n];
                // Mono: R == L, so M = L and S = 0 -- the same convention as the chain.
                const float xR = (dR != nullptr) ? dR[n] : xL;

                const float laneIn[kNumLanes] = { xL, xR,
                                                  0.5f * (xL + xR),   // mid
                                                  0.5f * (xL - xR) }; // side

                float bp[kNumLanes];

                for (int lane = 0; lane < kNumLanes; ++lane)
                {
                    // The bandpass ALWAYS runs so its state stays continuous across solo,
                    // dry and mode changes (a frozen state would produce a transient).
                    const float in  = laneIn[lane];
                    const float out = sb0 * in + z1[lane];
                    z1[lane] = sb1 * in - sa1 * out + z2[lane];
                    z2[lane] = sb2 * in - sa2 * out;

                    bp[lane] = out;
                    w[lane]  = soloWeight[lane].getNextValue();
                }

                const float mixL = soloMix[0].getNextValue();
                const float mixR = soloMix[1].getNextValue();
                const float gL   = soloGain[0].getNextValue();
                const float gR   = soloGain[1].getNextValue();

                // Mid lands on BOTH outputs with the same sign; Side lands on L and is
                // SUBTRACTED from R (§25). That mirrors the main chain's own matrix and,
                // more importantly, how each component really exists in the signal
                // (L = M+S, R = M-S): a side band lives as +S on the left and -S on the
                // right. So side solo is ANTI-PHASE and therefore NULLS in mono -- the
                // Pro-Q "perfect null", and the only way to tell side from mid by ear.
                // (An in-phase side solo would instead sum to 2S in mono, i.e. LOUDER.)
                //
                // The per-term weight tests are deliberate rather than plain multiplies:
                // bp*0 is NOT 0 when bp is NaN/Inf, which would let a pathological sample
                // in an UNUSED lane poison the output.
                const float midTerm  = (w[laneM] != 0.0f) ? bp[laneM] * w[laneM] : 0.0f;
                const float sideTerm = (w[laneS] != 0.0f) ? bp[laneS] * w[laneS] : 0.0f;

                const float tL = gL * (bp[laneL] * w[laneL] + midTerm + sideTerm);
                const float tR = gR * (bp[laneR] * w[laneR] + midTerm - sideTerm);

                const float outL = xL + mixL * (tL - xL);
                const float outR = xR + mixR * (tR - xR);

                dL[n] = outL;

                if (dR != nullptr)
                    dR[n] = outR;

#if TOREI_EQ_DEBUG_LOG
                // Solo-stage output probe (§26). Sigma(outL*outR) is what makes mid and
                // side distinguishable numerically: same RMS, opposite correlation.
                sumL2 += outL * outL;
                sumR2 += outR * outR;
                sumLR += outL * outR;
#endif
            }

            for (int lane = 0; lane < kNumLanes; ++lane)
            {
                soloState[lane][0] = z1[lane];
                soloState[lane][1] = z2[lane];
            }

#if TOREI_EQ_DEBUG_LOG
            // Publish for the message-thread SOLO log. Plain atomic stores: the audio
            // thread never allocates, locks or does IO (§13.2).
            for (int lane = 0; lane < kNumLanes; ++lane)
                soloWeightCurrent[lane].store (w[lane], std::memory_order_relaxed);

            soloProbeL2.store (sumL2, std::memory_order_relaxed);
            soloProbeR2.store (sumR2, std::memory_order_relaxed);
            soloProbeLR.store (sumLR, std::memory_order_relaxed);
            soloProbeN .store ((dL != nullptr) ? numSamples : 0, std::memory_order_relaxed);
#endif
        }
        else
        {
            // No bandpass (never soloed, or cleared by clearAllBands). SNAP the blend to
            // fully dry rather than ramping: there is nothing to blend anyway, and
            // snapping guarantees that when a bandpass reappears the stage still starts
            // from dry instead of jumping straight to full solo.
            for (int ch = 0; ch < 2; ++ch)
                soloMix[ch].setCurrentAndTargetValue (0.0f);
        }
    }

    // --- Output (trim) gain: the FINAL stage of the chain ---
    // Placed after the solo stage so it is a true global output level (it applies
    // while soloing too), and inside process() so it sits BEFORE analyzerPost is fed
    // in PluginProcessor::processBlock -- the post spectrum must shift with it.
    // 20 ms ramp keeps dragging the Output control free of zipper noise.
    {
        const float target = juce::Decibels::decibelsToGain (outputGainDb.load (std::memory_order_relaxed));

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& ramp = outputGain[ch];
            ramp.setTargetValue (target);

            float* data = buffer.getWritePointer (ch);

            for (int n = 0; n < numSamples; ++n)
                data[n] *= ramp.getNextValue();
        }
    }

#if TOREI_EQ_DEBUG_LOG
    if (probe)
        updateProbe (buffer, true);    // chain output
#endif
}

#if TOREI_EQ_DEBUG_LOG
void EqEngine::updateProbe (const juce::AudioBuffer<float>& buffer, bool post)
{
    const int n = buffer.getNumSamples();

    if (n <= 0 || buffer.getNumChannels() <= 0)
        return;

    const float* dL = buffer.getReadPointer (0);
    // Mono: R == L, which yields M = L, S = 0 -- the same convention process() uses.
    const float* dR = (buffer.getNumChannels() > 1) ? buffer.getReadPointer (1) : dL;

    double sL = 0.0, sR = 0.0, sM = 0.0, sS = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double l = dL[i];
        const double r = dR[i];
        const double m = 0.5 * (l + r);
        const double s = 0.5 * (l - r);

        sL += l * l;
        sR += r * r;
        sM += m * m;
        sS += s * s;
    }

    const float inv = 1.0f / (float) n;

    const float dbL = juce::Decibels::gainToDecibels (std::sqrt ((float) (sL * inv)), -120.0f);
    const float dbR = juce::Decibels::gainToDecibels (std::sqrt ((float) (sR * inv)), -120.0f);
    const float dbM = juce::Decibels::gainToDecibels (std::sqrt ((float) (sM * inv)), -120.0f);
    const float dbS = juce::Decibels::gainToDecibels (std::sqrt ((float) (sS * inv)), -120.0f);

    if (post)
    {
        probeOutL.store (dbL, std::memory_order_relaxed);
        probeOutR.store (dbR, std::memory_order_relaxed);
        probeOutM.store (dbM, std::memory_order_relaxed);
        probeOutS.store (dbS, std::memory_order_relaxed);
    }
    else
    {
        probeInL.store (dbL, std::memory_order_relaxed);
        probeInR.store (dbR, std::memory_order_relaxed);
        probeInM.store (dbM, std::memory_order_relaxed);
        probeInS.store (dbS, std::memory_order_relaxed);
    }
}

float EqEngine::getProbeDb (bool post, int probeChannel) const
{
    switch (probeChannel)
    {
        case probeR: return post ? probeOutR.load (std::memory_order_relaxed)
                                 : probeInR.load  (std::memory_order_relaxed);
        case probeM: return post ? probeOutM.load (std::memory_order_relaxed)
                                 : probeInM.load  (std::memory_order_relaxed);
        case probeS: return post ? probeOutS.load (std::memory_order_relaxed)
                                 : probeInS.load  (std::memory_order_relaxed);
        case probeL:
        default:     return post ? probeOutL.load (std::memory_order_relaxed)
                                 : probeInL.load  (std::memory_order_relaxed);
    }
}

float EqEngine::getLaneMix (int band, int lane) const
{
    if (band < 0 || band >= kMaxBands || lane < 0 || lane >= kNumLanes)
        return 0.0f;

    return laneMix[band][lane].load (std::memory_order_relaxed);
}

bool EqEngine::hasNonStereoBand() const
{
    return getFirstNonStereoBand() >= 0;
}

int EqEngine::getFirstNonStereoBand() const
{
    for (int i = 0; i < kMaxBands; ++i)
        if (bands[i].occupied.load (std::memory_order_relaxed)
            && ! bands[i].bypass.load (std::memory_order_relaxed)
            && (Mode) bands[i].mode.load (std::memory_order_relaxed) != modeStereo)
            return i;

    return -1;
}

int EqEngine::getRoutedBands (int* out, int maxBands) const
{
    if (out == nullptr || maxBands <= 0)
        return 0;

    int n = 0;

    for (int i = 0; i < kMaxBands && n < maxBands; ++i)
        if (bands[i].occupied.load (std::memory_order_relaxed)
            && ! bands[i].bypass.load (std::memory_order_relaxed)
            && (Mode) bands[i].mode.load (std::memory_order_relaxed) != modeStereo)
            out[n++] = i;

    return n;
}

EqEngine::Mode EqEngine::getBandMode (int index) const
{
    if (index < 0 || index >= kMaxBands)
        return modeStereo;

    return (Mode) bands[index].mode.load (std::memory_order_relaxed);
}

float EqEngine::getSoloWeight (int lane) const
{
    if (lane < 0 || lane >= kNumLanes)
        return 0.0f;

    return soloWeightCurrent[lane].load (std::memory_order_relaxed);
}

float EqEngine::getSoloOutDb (bool right) const
{
    const int n = soloProbeN.load (std::memory_order_relaxed);

    if (n <= 0)
        return -120.0f;

    const float sum = right ? soloProbeR2.load (std::memory_order_relaxed)
                            : soloProbeL2.load (std::memory_order_relaxed);

    return juce::Decibels::gainToDecibels (std::sqrt (sum / (float) n), -120.0f);
}

float EqEngine::getSoloOutCorr() const
{
    const float l2 = soloProbeL2.load (std::memory_order_relaxed);
    const float r2 = soloProbeR2.load (std::memory_order_relaxed);
    const float lr = soloProbeLR.load (std::memory_order_relaxed);

    // Correlation of the audition's two outputs. mid (in phase) -> +1, side (anti phase)
    // -> -1, which is the one numeric quantity that separates them.
    const float den = std::sqrt (l2 * r2);

    if (den <= 1.0e-12f)
        return 0.0f;   // one side is silent (e.g. L/R modes): correlation undefined

    return juce::jlimit (-1.0f, 1.0f, lr / den);
}
#endif   // TOREI_EQ_DEBUG_LOG

int EqEngine::getCurveGains (float* gains, int maxPoints, CurveGroup group) const
{
    const int n = std::min (maxPoints, kCurvePoints);

    for (int i = 0; i < n; ++i)
    {
        const double freq = 20.0 * std::pow (10.0, (double) i / (double) (n - 1) * 3.0);

        double totalDb = 0.0;

        for (int b = 0; b < kMaxBands; ++b)
        {
            if (! bands[b].occupied.load (std::memory_order_relaxed)
                || bands[b].bypass.load (std::memory_order_relaxed))
                continue;

            // Which curves does this band show up in? (MID_SIDE_HANDOFF.md §5)
            //   stereo bands count towards EVERY group -- they act on all channels --
            //   while a routed band only counts towards its own group. Grouping by
            //   direct scope like this is what makes the display intuitive: stereo
            //   bands shift both lines together and never change the GAP between them,
            //   so the gap is determined purely by the mid/side/L/R bands.
            const Mode bandMode = (Mode) bands[b].mode.load (std::memory_order_relaxed);

            bool include = (group == curveAll);

            if (! include)
            {
                switch (bandMode)
                {
                    case modeStereo: include = true; break;
                    case modeLeft:   include = (group == curveLeft);  break;
                    case modeRight:  include = (group == curveRight); break;
                    case modeMid:    include = (group == curveMid);   break;
                    case modeSide:   include = (group == curveSide);  break;
                }
            }

            if (! include)
                continue;

            // A band can be a cascade of up to 4 sections (slope), and magnitude
            // responses multiply -- in dB that is a sum. This is what makes a
            // 48 dB/oct cut visibly steeper on the curve than a 12 dB/oct one.
            const int stages = juce::jlimit (0, kMaxCascades,
                                             bands[b].activeStages.load (std::memory_order_relaxed));

            for (int s = 0; s < stages; ++s)
            {
                // Hold a reference so a concurrent coefficient swap (message/WebView2
                // thread) cannot free the object while we read it.
                if (auto cd = coeffs[b][s])
                {
                    const double mag = cd->getMagnitudeForFrequency (freq, sampleRate);
                    totalDb += 20.0 * std::log10 (std::max (mag, 1e-12));
                }
            }
        }

        gains[i] = (float) totalDb;
    }

    // Light sliding average to remove the per-point jitter produced by summing
    // many band magnitude responses. With a flat section this would otherwise
    // surface as a small ripple once Catmull-Rom interpolation is applied in the
    // UI. We fold it in on the C++ side so the data source is clean; the first and
    // last points are left untouched so the curve endpoints stay exact.
    for (int i = 1; i < n - 1; ++i)
        gains[i] = (gains[i - 1] + gains[i] + gains[i + 1]) / 3.0f;

    return n;
}

int EqEngine::activeBandCount() const
{
    int n = 0;
    for (int i = 0; i < kMaxBands; ++i)
        if (bands[i].occupied.load (std::memory_order_relaxed)
            && ! bands[i].bypass.load (std::memory_order_relaxed))
            ++n;
    return n;
}

bool EqEngine::hasNonUnityBand() const
{
    for (int i = 0; i < kMaxBands; ++i)
        if (bands[i].occupied.load (std::memory_order_relaxed)
            && ! bands[i].bypass.load (std::memory_order_relaxed)
            && std::fabs (bands[i].gain.load (std::memory_order_relaxed)) >= 1.0f)
            return true;
    return false;
}

juce::String EqEngine::describe() const
{
    int active = 0;
    juce::String bandsDesc;

    for (int i = 0; i < kMaxBands; ++i)
    {
        if (! bands[i].occupied.load (std::memory_order_relaxed)
            || bands[i].bypass.load (std::memory_order_relaxed))
            continue;

        ++active;

        const int stages = juce::jlimit (0, kMaxCascades,
                                         bands[i].activeStages.load (std::memory_order_relaxed));

        // Hold a reference so a concurrent coefficient swap (message/WebView2
        // thread) cannot free the object while we read it.
        if (auto cd = coeffs[i][0])
        {
            const auto* raw = cd->getRawCoefficients();
            const float f = bands[i].freq.load();

            // Report the CASCADE's total response at the band's own frequency, which
            // for a cut filter is the whole point of the slope control.
            double totalMag = 1.0;
            for (int s = 0; s < stages; ++s)
                if (auto cs = coeffs[i][s])
                    totalMag *= cs->getMagnitudeForFrequency (f, sampleRate);

            bandsDesc << "  b" << i
                      << " type=" << (int) bands[i].type
                      << " f=" << juce::String (f)
                      << " g=" << juce::String (bands[i].gain.load())
                      << " q=" << juce::String (bands[i].q.load())
                      << " slope=" << juce::String (bands[i].slope.load())
                      << " mode=" << juce::String (modeToString ((Mode) bands[i].mode.load()))
                      << " stages=" << juce::String (stages)
                      << " b0=" << juce::String (raw[0])
                      << " |H|@" << juce::String (f) << "="
                      << juce::String (20.0 * std::log10 (std::max (totalMag, 1e-12)))
                      << "dB";
        }
        else
        {
            bandsDesc << "  b" << i << " coefficients=NULL";
        }
    }

    return juce::String ("activeBands=") + juce::String (active)
             + " listenIndex=" + juce::String (listenIndex.load (std::memory_order_relaxed))
             + " soloLevelDb=" + juce::String (soloLevelDb.load (std::memory_order_relaxed))
             + " outputGainDb=" + juce::String (outputGainDb.load (std::memory_order_relaxed))
             + " soloBp=" + (soloCoeffs != nullptr ? "ok" : "null")
             + bandsDesc;
}
