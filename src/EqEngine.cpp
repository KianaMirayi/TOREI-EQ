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
}

EqEngine::Type EqEngine::typeFromString (const juce::String& s)
{
    if (s == "lowshelf")  return lowshelf;
    if (s == "highshelf") return highshelf;
    if (s == "lowpass")   return lowpass;
    if (s == "highpass")  return highpass;
    return peaking;
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

    // Reset the processing state but keep `coefficients` (see reset() for why).
    filters[index].reset();
}

void EqEngine::setParam (int index, const juce::String& param, const juce::var& value)
{
    if (index < 0 || index >= kMaxBands || ! bands[index].occupied.load())
        return;

    Band& b = bands[index];

    if (param == "freq")
    {
        b.freq.store ((float) (double) value);
        updateCoefficients (index);
    }
    else if (param == "gain")
    {
        b.gain.store ((float) (double) value);
        updateCoefficients (index);
    }
    else if (param == "q")
    {
        b.q.store ((float) (double) value);
        updateCoefficients (index);
    }
    else if (param == "type")
    {
        b.type = typeFromString (value.toString());
        updateCoefficients (index);
    }
    else if (param == "bypass")
    {
        b.bypass.store ((bool) value);
    }
}

void EqEngine::process (juce::AudioBuffer<float>& buffer)
{
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = std::min (buffer.getNumChannels(), 2);

    for (int i = 0; i < kMaxBands; ++i)
    {
        if (! bands[i].occupied.load (std::memory_order_relaxed)
            || bands[i].bypass.load (std::memory_order_relaxed))
            continue;

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
            float* data = buffer.getWritePointer (ch);
            float z1 = filterState[i][ch][0];
            float z2 = filterState[i][ch][1];

            for (int n = 0; n < numSamples; ++n)
            {
                const float x = data[n];
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                data[n] = y;
            }

            filterState[i][ch][0] = z1;
            filterState[i][ch][1] = z2;
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

    return juce::String ("activeBands=") + juce::String (active) + bandsDesc;
}
