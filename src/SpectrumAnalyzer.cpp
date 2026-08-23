#include "SpectrumAnalyzer.h"
#include <JuceHeader.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>

namespace
{
    constexpr int fftOrder     = 12;                 // 4096-point FFT
    constexpr int fftSize      = 1 << fftOrder;      // 4096
    constexpr int numBins      = fftSize / 2;        // 2048 positive bins
    constexpr int numPoints    = kSpectrumPointCount;// 256 log-spaced output points
    constexpr int fifoCapacity = 16384;              // ~4 FFT windows of headroom

    constexpr double fMin       = 20.0;              // lowest displayed frequency (Hz)
    constexpr double fMax       = 20000.0;           // highest displayed frequency (Hz)
    constexpr float  floorDb    = -120.0f;

    // Spatial smoothing radii (log-frequency domain), runtime-adjustable so the
    // user can tune them live in the host. Defaults are light (peaks/valleys
    // survive).
    std::atomic<int>   gDilateRadius { 0 };
    std::atomic<int>   gBlurRadius   { 0 };

    // Ballistic envelope, runtime-adjustable.
    std::atomic<float> gAttack  { 0.50f };
    std::atomic<float> gRelease { 0.96f };

    // Per-point frequency band half-width (octaves). Runtime-adjustable: the
    // smaller it is, the narrower (finer) each point's averaging band.
    std::atomic<float> gBandHalfOct { 0.02f };

    class SpectrumAnalyzer
    {
    public:
        void prepare (double sampleRate, int samplesPerBlock)
        {
            juce::ignoreUnused (samplesPerBlock);

            fifo.reset();
            ringBuffer.calloc (fifoCapacity);
            rollBuffer.calloc (fftSize);
            windowBuffer.calloc (fftSize);
            fftWorkspace.calloc ((size_t) fftSize * 2);
            magDb.calloc (numBins);
            rawPoints.calloc (numPoints);
            dilatedPoints.calloc (numPoints);
            envelope.calloc (numPoints);
            binCenterArr.calloc (numPoints);

            // 4-term Blackman-Harris window, un-normalised: we correct for the
            // coherent gain ourselves so peak amplitudes map directly to dBFS.
            juce::dsp::WindowingFunction<float>::fillWindowingTables (
                windowBuffer.get(), (size_t) fftSize,
                juce::dsp::WindowingFunction<float>::blackmanHarris, false);

            double windowSum = 0.0;
            for (int i = 0; i < fftSize; ++i)
                windowSum += windowBuffer[i];
            windowSumFloat = (float) windowSum;

            // Precompute each output point's centre frequency (in bins). The band
            // width is applied at runtime from the tunable parameter, so this is
            // all we need to store here.
            const double nyquist = sampleRate * 0.5;

            for (int i = 0; i < numPoints; ++i)
            {
                const double xCenter = ((double) i + 0.5) / numPoints;
                const double freqCenter = fMin * std::pow (10.0, xCenter * 3.0);
                binCenterArr[i] = (float) (freqCenter / nyquist * (numBins - 1));
            }

            fft = std::make_unique<juce::dsp::FFT> (fftOrder);

            rollPos = 0;
            rollFilled = 0;
            hasFrame = false;
            prepared.store (true, std::memory_order_release);
        }

        void reset()
        {
            prepared.store (false, std::memory_order_release);
            fifo.reset();
            rollFilled = 0;
            hasFrame = false;

            // Destroy the FFT now rather than at process teardown. juce::dsp::FFT
            // uses a LeakedObjectDetector, and gAnalyzer is a process-lifetime
            // global, so holding the FFT until static teardown races JUCE's FFT
            // leak-counter destructor and fires a spurious "leaked FFT" assert on
            // close. prepare() recreates it on the next run.
            fft.reset();
        }

        // Real-time safe: no allocation, no lock, bounded work.
        void push (const float* const* channelData, int numChannels, int numSamples)
        {
            if (! prepared.load (std::memory_order_acquire) || numChannels <= 0 || numSamples <= 0)
                return;

            const auto writeHandle = fifo.write (numSamples);

            const auto downmix = [&] (int startIndex, int blockSize, int srcOffset)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const int src = srcOffset + i;
                    float mono = channelData[0][src];

                    for (int ch = 1; ch < numChannels; ++ch)
                        mono += channelData[ch][src];

                    ringBuffer[startIndex + i] = mono / (float) numChannels;
                }
            };

            downmix (writeHandle.startIndex1, writeHandle.blockSize1, 0);
            downmix (writeHandle.startIndex2, writeHandle.blockSize2, writeHandle.blockSize1);
        }

        int read (float* dest, int destSize)
        {
            if (! prepared.load (std::memory_order_acquire))
                return 0;

            const int points = std::min (numPoints, destSize);

            // Drain everything the audio thread produced since the last read into
            // the rolling window, so consecutive FFTs overlap by roughly one timer
            // tick and the display refreshes at the timer rate (~40 Hz).
            const int ready = fifo.getNumReady();

            if (ready > 0)
            {
                const auto readHandle = fifo.read (ready);

                for (int i = 0; i < readHandle.blockSize1; ++i)
                    pushRoll (ringBuffer[readHandle.startIndex1 + i]);

                for (int i = 0; i < readHandle.blockSize2; ++i)
                    pushRoll (ringBuffer[readHandle.startIndex2 + i]);
            }

            if (rollFilled < fftSize)
                return 0;

            // Copy the rolling window into the workspace in chronological order
            // and apply the window in one pass.
            for (int i = 0; i < fftSize; ++i)
                fftWorkspace[i] = rollBuffer[(rollPos + i) & (fftSize - 1)] * windowBuffer[i];

            // Magnitude spectrum, positive frequencies only (bins 0 .. fftSize/2).
            fft->performFrequencyOnlyForwardTransform (fftWorkspace.get(), true);

            // 1. Per-bin dBFS (coherent-gain corrected).
            for (int k = 0; k < numBins; ++k)
            {
                const float amp = 2.0f * fftWorkspace[k] / windowSumFloat;
                magDb[k] = std::max (floorDb, 20.0f * std::log10 (std::max (amp, 1e-9f)));
            }

            // 2. Per-point band: cosine interpolation when the band is narrower
            //    than 1 bin, otherwise power AVERAGE over the band. Band width is
            //    applied here at runtime (tunable).
            const float bandHalfOct = gBandHalfOct.load (std::memory_order_relaxed);
            const double bandFactor = std::pow (2.0, (double) bandHalfOct);

            for (int i = 0; i < points; ++i)
            {
                const double bc  = binCenterArr[i];
                const double loF = bc / bandFactor;
                const double hiF = bc * bandFactor;

                if (hiF - loF < 1.0)
                {
                    const int il = (int) std::floor (bc);
                    const int ilc = std::max (0, std::min (numBins - 2, il));
                    const float frac = (float) (bc - (double) il);
                    const float vL = magDb[ilc];
                    const float vH = magDb[ilc + 1];
                    const float mu = (1.0f - std::cos (frac * juce::MathConstants<float>::pi)) * 0.5f;
                    rawPoints[i] = vL * (1.0f - mu) + vH * mu;
                }
                else
                {
                    const int blo = std::max (0, (int) std::floor (loF));
                    const int bhi = std::min (numBins - 1, (int) std::ceil (hiF));

                    double powerSum = 0.0;
                    int count = 0;
                    for (int b = blo; b <= bhi; ++b)
                    {
                        powerSum += std::pow (10.0, (double) magDb[b] / 10.0);
                        ++count;
                    }
                    const double powerAvg = powerSum / (double) count;
                    rawPoints[i] = (float) (10.0 * std::log10 (std::max (powerAvg, 1e-12)));
                }
            }

            // 3. Morphological dilation (widens thin peaks; radius runtime-adjustable).
            const int dilateRadius = gDilateRadius.load (std::memory_order_relaxed);
            for (int p = 0; p < points; ++p)
            {
                float mx = floorDb;
                for (int dp = -dilateRadius; dp <= dilateRadius; ++dp)
                {
                    const int idx = p + dp;
                    if (idx >= 0 && idx < points && rawPoints[idx] > mx)
                        mx = rawPoints[idx];
                }
                dilatedPoints[p] = mx;
            }

            // 4. Gaussian blur + ballistic attack/release envelope (all runtime-adjustable).
            const int   blurRadius = gBlurRadius.load (std::memory_order_relaxed);
            const float attackMul  = gAttack.load  (std::memory_order_relaxed);
            const float releaseMul = gRelease.load (std::memory_order_relaxed);
            const float sigma2 = (float) (blurRadius * blurRadius) / 4.0f;

            for (int p = 0; p < points; ++p)
            {
                float target;

                if (blurRadius <= 0)
                {
                    target = dilatedPoints[p];
                }
                else
                {
                    float sum = 0.0f, weightSum = 0.0f;

                    for (int dp = -blurRadius; dp <= blurRadius; ++dp)
                    {
                        const int idx = p + dp;
                        if (idx >= 0 && idx < points)
                        {
                            const float w = std::exp (-(float) (dp * dp) / sigma2);
                            sum += dilatedPoints[idx] * w;
                            weightSum += w;
                        }
                    }

                    target = sum / weightSum;
                }

                const float prev = envelope[p];

                if (! hasFrame)
                {
                    envelope[p] = target;
                }
                else if (target > prev)
                {
                    envelope[p] = target * attackMul + prev * (1.0f - attackMul);
                }
                else
                {
                    envelope[p] = target * (1.0f - releaseMul) + prev * releaseMul;
                }

                dest[p] = envelope[p];
            }

            hasFrame = true;

            return points;
        }

    private:
        void pushRoll (float sample)
        {
            rollBuffer[rollPos] = sample;
            rollPos = (rollPos + 1) & (fftSize - 1);

            if (rollFilled < fftSize)
                ++rollFilled;
        }

        juce::AbstractFifo fifo { fifoCapacity };
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

        float windowSumFloat = 1.0f;
        int   rollPos = 0;
        int   rollFilled = 0;
        bool  hasFrame = false;
        std::atomic<bool> prepared { false };
    };

    SpectrumAnalyzer gAnalyzer;
}

void prepareSpectrum (double sampleRate, int samplesPerBlock)
{
    gAnalyzer.prepare (sampleRate, samplesPerBlock);
}

void resetSpectrum()
{
    gAnalyzer.reset();
}

void pushAudioToSpectrum (const float* const* channelData, int numChannels, int numSamples)
{
    gAnalyzer.push (channelData, numChannels, numSamples);
}

int readSpectrum (float* dest, int destSize)
{
    return gAnalyzer.read (dest, destSize);
}

void setSpectrumSmoothing (float attack, float release, int blurRadius, int dilateRadius, float bandHalfOct)
{
    gAttack.store       (attack,       std::memory_order_relaxed);
    gRelease.store      (release,      std::memory_order_relaxed);
    gBlurRadius.store   (blurRadius,   std::memory_order_relaxed);
    gDilateRadius.store (dilateRadius, std::memory_order_relaxed);
    gBandHalfOct.store  (bandHalfOct,  std::memory_order_relaxed);
}
