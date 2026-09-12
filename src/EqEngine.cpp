#include "EqEngine.h"

void EqEngine::prepare (double sr, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    sampleRate = sr;

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
    // Reset each filter's processing state (so no stale filter memory survives a
    // transport/sample-rate change). IMPORTANT: the band definitions
    // (occupied/type/freq/gain/q/bypass) must be PRESERVED here. Clearing them
    // would wipe the user's EQ every time prepareToPlay/releaseResources runs,
    // and since the UI only sends AddBand once on mount, the bands would never
    // come back -- so setParam/process/getCurveGains all silently no-op.
    //
    // `IIR::Filter::reset()` dereferences `coefficients`, so it must NOT be null
    // here; we intentionally leave the coefficients in place (the band stays
    // occupied, and its processing state is simply cleared).
    for (int i = 0; i < kMaxBands; ++i)
        filters[i].reset();

    // Zero the manual biquad state used by process().
    for (int i = 0; i < kMaxBands; ++i)
        for (int ch = 0; ch < 2; ++ch)
        {
            filterState[i][ch][0] = 0.0f;
            filterState[i][ch][1] = 0.0f;
        }

    // Listen is transient audition state: a transport/sample-rate change clears it
    // (it is deliberately NOT persisted in getStateInformation).
    listenIndex.store (-1, std::memory_order_relaxed);

    // Restart every mix ramp from 0 (fully transparent) so the next process() call
    // fades the bands in rather than jumping straight to full gain.
    for (int i = 0; i < kMaxBands; ++i)
        for (int ch = 0; ch < 2; ++ch)
            bandMix[i][ch].reset (sampleRate, kMixRampSeconds);

    // Solo audition stage: drop the bandpass and restart its ramps from "dry" so no
    // stale solo state or filter memory survives a transport/sample-rate change.
    // NOTE: soloLevelDb is intentionally NOT reset -- it is the user's solo playback
    // level (the UI owns it and keeps showing it), not transport state.
    soloCoeffs = nullptr;
    for (int ch = 0; ch < 2; ++ch)
    {
        soloState[ch][0] = 0.0f;
        soloState[ch][1] = 0.0f;
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
        e.bypass = bands[i].bypass.load (std::memory_order_relaxed);
    }

    return n;
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
        bands[i].occupied.store (false, std::memory_order_relaxed);
        bands[i].bypass.store   (false, std::memory_order_relaxed);

        // IIR::Filter::reset() dereferences `coefficients` unconditionally, so it is
        // only safe on slots that actually hold some.
        if (filters[i].coefficients != nullptr)
            filters[i].reset();

        for (int ch = 0; ch < 2; ++ch)
        {
            filterState[i][ch][0] = 0.0f;
            filterState[i][ch][1] = 0.0f;
        }
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        soloState[ch][0] = 0.0f;
        soloState[ch][1] = 0.0f;
    }
}

void EqEngine::updateCoefficients (int index)
{
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    const Band& b = bands[index];
    const float freq = b.freq.load();
    const float q    = b.q.load();
    const float gain = b.gain.load();

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
            c = Coeffs::makeLowPass (sampleRate, freq, q);
            break;
        case highpass:
            c = Coeffs::makeHighPass (sampleRate, freq, q);
            break;
    }

    // Pointer swap is real-time safe: the audio thread reads the previous
    // coefficients until the new ones are published.
    filters[index].coefficients = std::move (c);
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
    b.bypass.store (false);
    updateCoefficients (index);
    b.occupied.store (true);
}

void EqEngine::removeBand (int index)
{
    if (index < 0 || index >= kMaxBands)
        return;

    bands[index].occupied.store (false);

    // Never leave the listen target pointing at a slot that no longer exists.
    if (listenIndex.load (std::memory_order_relaxed) == index)
        listenIndex.store (-1, std::memory_order_relaxed);

    // Reset the processing state but keep `coefficients` (see reset() for why).
    // Guarded: IIR::Filter::reset() dereferences `coefficients`, so calling it on a
    // slot that never had a band would be a null dereference.
    if (filters[index].coefficients != nullptr)
        filters[index].reset();
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

    for (int i = 0; i < kMaxBands; ++i)
    {
        // Unoccupied slots have no coefficients and nothing to process.
        if (! bands[i].occupied.load (std::memory_order_relaxed))
            continue;

        // Target mix for this band: 1 = fully applied, 0 = transparent.
        //   - listening -> EVERY band goes transparent, including the listened one.
        //     With all bands at mix 0 the chain output is the untouched dry signal,
        //     which the solo bandpass stage below then auditions (§0.10). That is
        //     why solo needs no separate dry-signal buffer: mix=0 IS the dry path.
        //   - not listening -> normal behaviour (bypass means transparent).
        // Driven through a ramp rather than a hard `continue`, because switching
        // either listen or bypass instantly would click. Hard-skipping a bypassed
        // band is also what used to make bypass itself pop.
        const bool bypassed = bands[i].bypass.load (std::memory_order_relaxed);
        const float targetMix = (listen >= 0) ? 0.0f : (bypassed ? 0.0f : 1.0f);

        // Snapshot the coefficients. Copying the ReferenceCountedObjectPtr holds a
        // reference, so a concurrent coefficient swap on the UI/WebView2 thread
        // cannot free the object while we use it (this is what juce's
        // IIR::Filter::processSample does NOT do, and is the source of a heap
        // overrun when the coefficients are swapped during real-time processing).
        auto cd = filters[i].coefficients;
        if (cd == nullptr)
            continue;

        const auto* c = cd->getRawCoefficients();
        const float b0 = c[0], b1 = c[1], b2 = c[2];
        const float a1 = c[3], a2 = c[4];

        for (int ch = 0; ch < numChannels; ++ch)
        {
            bandMix[i][ch].setTargetValue (targetMix);

            float* data = buffer.getWritePointer (ch);
            float z1 = filterState[i][ch][0];
            float z2 = filterState[i][ch][1];

            for (int n = 0; n < numSamples; ++n)
            {
                const float x = data[n];
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;

                // The filter ALWAYS runs and its state is ALWAYS written back, even
                // when the band is mixed out. Freezing the state would leave stale
                // filter memory that produces a transient when the ramp comes back.
                const float m = bandMix[i][ch].getNextValue();
                data[n] = x + m * (y - x);
            }

            filterState[i][ch][0] = z1;
            filterState[i][ch][1] = z2;
        }
    }

    // --- Solo audition stage (§0.10, Pro-Q "solo" semantics) ---
    // Appends a bandpass of the DRY signal (the chain above is fully transparent
    // while listening), so you hear the frequency content around the listened band
    // with none of the EQ gain. Both the blend and the level are ramped, so
    // entering/leaving solo and dragging the solo volume never click or zipper.
    {
        // Snapshot: holding the reference keeps the object alive if the message
        // thread swaps the coefficients mid-block.
        auto sc = soloCoeffs;

        const float targetSoloMix  = (listen >= 0) ? 1.0f : 0.0f;
        const float targetSoloGain = juce::Decibels::decibelsToGain (soloLevelDb.load (std::memory_order_relaxed));

        if (sc != nullptr)
        {
            const auto* c = sc->getRawCoefficients();
            const float b0 = c[0], b1 = c[1], b2 = c[2];
            const float a1 = c[3], a2 = c[4];

            for (int ch = 0; ch < numChannels; ++ch)
            {
                soloMix[ch].setTargetValue (targetSoloMix);
                soloGain[ch].setTargetValue (targetSoloGain);

                float* data = buffer.getWritePointer (ch);
                float z1 = soloState[ch][0];
                float z2 = soloState[ch][1];

                for (int n = 0; n < numSamples; ++n)
                {
                    const float x  = data[n];
                    const float bp = b0 * x + z1;      // bandpass, always running so its
                    z1 = b1 * x - a1 * bp + z2;        // state stays continuous across
                    z2 = b2 * x - a2 * bp;             // both solo and dry

                    const float m = soloMix[ch].getNextValue();
                    const float g = soloGain[ch].getNextValue();
                    data[n] = x + m * (bp * g - x);
                }

                soloState[ch][0] = z1;
                soloState[ch][1] = z2;
            }
        }
        else
        {
            // No bandpass (never soloed, or cleared by clearAllBands). SNAP the blend
            // to fully dry rather than ramping: there is nothing to blend anyway, and
            // snapping guarantees that when a bandpass reappears the stage still
            // starts from dry instead of jumping straight to full solo.
            for (int ch = 0; ch < numChannels; ++ch)
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
}

int EqEngine::getCurveGains (float* gains, int maxPoints) const
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

            if (auto cd = filters[b].coefficients)
            {
                const double mag = cd->getMagnitudeForFrequency (freq, sampleRate);
                totalDb += 20.0 * std::log10 (std::max (mag, 1e-12));
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

        // Hold a reference so a concurrent coefficient swap (message/WebView2
        // thread) cannot free the object while we read it.
        if (auto cd = filters[i].coefficients)
        {
            const auto* raw = cd->getRawCoefficients();
            const float f = bands[i].freq.load();
            const double mag = cd->getMagnitudeForFrequency (f, sampleRate);

            bandsDesc << "  b" << i
                      << " type=" << (int) bands[i].type
                      << " f=" << juce::String (f)
                      << " g=" << juce::String (bands[i].gain.load())
                      << " q=" << juce::String (bands[i].q.load())
                      << " b0=" << juce::String (raw[0])
                      << " |H|@" << juce::String (f) << "="
                      << juce::String (20.0 * std::log10 (std::max (mag, 1e-12)))
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
             + " soloBp=" + (soloCoeffs != nullptr ? "ok" : "null")
             + bandsDesc;
}
