// Filth Octave v2 - octave up / octave down / fuzz / chainsaw EQ / gate
// Signal flow: gate -> [clean + octave up + octave down] -> fuzz -> chainsaw EQ -> amp saturation
//              (fuzz + chainsaw run at 4x speed) -> tone -> level -> mix

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>

class FilthOctaveProcessor : public juce::AudioProcessor
{
public:
    FilthOctaveProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          params (*this, nullptr, "PARAMS", createLayout())
    {
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using P = juce::AudioParameterFloat;
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        layout.add (std::make_unique<P> (juce::ParameterID { "gate", 1 },    "Gate",          0.0f, 1.0f, 0.25f));
        layout.add (std::make_unique<P> (juce::ParameterID { "clean", 1 },   "Clean Voice",   0.0f, 1.0f, 0.5f));
        layout.add (std::make_unique<P> (juce::ParameterID { "up", 1 },      "Octave Up",     0.0f, 1.0f, 0.7f));
        layout.add (std::make_unique<P> (juce::ParameterID { "down", 1 },    "Octave Down",   0.0f, 1.0f, 0.4f));
        layout.add (std::make_unique<P> (juce::ParameterID { "fuzz", 1 },    "Fuzz",          0.0f, 1.0f, 0.6f));
        layout.add (std::make_unique<P> (juce::ParameterID { "sawlow", 1 },  "Chainsaw Low",  0.0f, 1.0f, 0.0f));
        layout.add (std::make_unique<P> (juce::ParameterID { "sawhigh", 1 }, "Chainsaw High", 0.0f, 1.0f, 0.0f));
        layout.add (std::make_unique<P> (juce::ParameterID { "tone", 1 },    "Tone",          0.0f, 1.0f, 0.5f));
        layout.add (std::make_unique<P> (juce::ParameterID { "level", 1 },   "Level (dB)",
                                         juce::NormalisableRange<float> (-24.0f, 6.0f, 0.1f), -6.0f));
        layout.add (std::make_unique<P> (juce::ParameterID { "mix", 1 },     "Mix",           0.0f, 1.0f, 1.0f));

        return layout;
    }

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        sr = (float) sampleRate;

        oversampler.initProcessing ((size_t) samplesPerBlock);
        oversampler.reset();
        setLatencySamples ((int) std::round (oversampler.getLatencyInSamples()));

        const auto osFactor = oversampler.getOversamplingFactor();
        osRate = sr * (float) osFactor;

        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, 2 };

        tone.prepare (spec);
        tone.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        tone.reset();

        dcOut.prepare (spec);
        dcOut.setType (juce::dsp::StateVariableTPTFilterType::highpass);
        dcOut.setCutoffFrequency (30.0f);
        dcOut.reset();

        // Chainsaw EQ filters run at the oversampled rate, one filter per channel
        lastLowDb = -100.0f;
        lastHighDb = -100.0f;
        updateSawFilters (0.0f, 0.0f);

        juce::dsp::ProcessSpec osSpec { (double) osRate, (juce::uint32) ((size_t) samplesPerBlock * osFactor), 1 };
        for (auto& f : sawLowF)  { f.prepare (osSpec); f.reset(); }
        for (auto& f : sawHighF) { f.prepare (osSpec); f.reset(); }

        dryCopy.setSize (2, samplesPerBlock);

        dcCoef     = onePole (30.0f);    // removes rumble from the octave-up voice
        trackCoef  = onePole (250.0f);   // smooths the signal so the octave-down tracks the note
        envAttack  = onePole (200.0f);   // how fast the sub-octave follows your picking
        envRelease = onePole (8.0f);     // how slowly it fades out
        subCoef    = onePole (1500.0f);  // softens the sub-octave's square wave

        gateEnvAttack  = onePole (1000.0f); // gate hears your pick almost instantly
        gateEnvRelease = onePole (20.0f);
        gateOpenCoef   = onePole (300.0f);  // gate opens fast
        gateCloseCoef  = onePole (6.0f);    // and shuts quickly but without clicking

        for (auto& s : state)
            s = ChannelState {};
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        auto out = layouts.getMainOutputChannelSet();
        if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
            return false;
        return out == layouts.getMainInputChannelSet();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (buffer.getNumChannels(), 2);

        for (int ch = getTotalNumInputChannels(); ch < buffer.getNumChannels(); ++ch)
            buffer.clear (ch, 0, numSamples);

        const float gateP   = params.getRawParameterValue ("gate")->load();
        const float clean   = params.getRawParameterValue ("clean")->load();
        const float up      = params.getRawParameterValue ("up")->load();
        const float down    = params.getRawParameterValue ("down")->load();
        const float fuzz    = params.getRawParameterValue ("fuzz")->load();
        const float sawLow  = params.getRawParameterValue ("sawlow")->load();
        const float sawHigh = params.getRawParameterValue ("sawhigh")->load();
        const float toneP   = params.getRawParameterValue ("tone")->load();
        const float level   = params.getRawParameterValue ("level")->load();
        const float mix     = params.getRawParameterValue ("mix")->load();

        if (dryCopy.getNumSamples() < numSamples)
            dryCopy.setSize (2, numSamples, false, false, true);

        for (int ch = 0; ch < numCh; ++ch)
            dryCopy.copyFrom (ch, 0, buffer, ch, 0, numSamples);

        // Gate: knob at 0 = off, then threshold rises from -80 dB to -30 dB
        const bool gateOn = gateP > 0.0f;
        const float gateThresh = juce::Decibels::decibelsToGain (-80.0f + 50.0f * gateP);

        // ---- Stage 1: gate, build the three voices and blend them ----
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            auto& s = state[(size_t) ch];

            for (int i = 0; i < numSamples; ++i)
            {
                const float x = d[i];
                const float rect = std::abs (x);

                // Noise gate (opens above threshold, closes at half the threshold so it doesn't chatter)
                const float gc = rect > s.gateEnv ? gateEnvAttack : gateEnvRelease;
                s.gateEnv = gc * s.gateEnv + (1.0f - gc) * rect;

                if (! gateOn)                              s.gateIsOpen = true;
                else if (s.gateEnv > gateThresh)           s.gateIsOpen = true;
                else if (s.gateEnv < gateThresh * 0.5f)    s.gateIsOpen = false;

                const float target = s.gateIsOpen ? 1.0f : 0.0f;
                const float gg = target > s.gateGain ? gateOpenCoef : gateCloseCoef;
                s.gateGain = gg * s.gateGain + (1.0f - gg) * target;

                // Octave up: fold the wave (full-wave rectify), then remove the DC offset
                const float upSig = rect - s.upX1 + dcCoef * s.upY1;
                s.upX1 = rect;
                s.upY1 = upSig;

                // Octave down: follow the note's loudness...
                const float envC = rect > s.env ? envAttack : envRelease;
                s.env = envC * s.env + (1.0f - envC) * rect;

                // ...find each cycle of the note, and flip a switch every cycle (= half the frequency)
                s.lp1 = trackCoef * s.lp1 + (1.0f - trackCoef) * x;
                s.lp2 = trackCoef * s.lp2 + (1.0f - trackCoef) * s.lp1;
                const float hyst = 0.02f * s.env + 1.0e-5f;

                if (! s.wasPositive && s.lp2 > hyst)
                {
                    s.wasPositive = true;
                    s.flip = -s.flip;
                }
                else if (s.wasPositive && s.lp2 < -hyst)
                {
                    s.wasPositive = false;
                }

                const float sub = s.flip * s.env;
                s.subLp = subCoef * s.subLp + (1.0f - subCoef) * sub;

                d[i] = (clean * x + up * 2.0f * upSig + down * 1.5f * s.subLp) * s.gateGain;
            }
        }

        // ---- Stage 2: fuzz + chainsaw, run at 4x speed to keep it from sounding fizzy/digital ----
        juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) numCh, (size_t) numSamples);
        auto osBlock = oversampler.processSamplesUp (block);

        const float gain = juce::Decibels::decibelsToGain (fuzz * 50.0f);
        const float bias = 0.3f;                 // lopsided clipping = gnarlier, more "transistor" sound
        const float biasOffset = std::tanh (bias);

        updateSawFilters (sawLow * 15.0f, sawHigh * 15.0f);   // up to +15 dB boosts
        const float sawAmount = juce::jmin (1.0f, sawLow + sawHigh);
        const float ampDrive = 1.0f + 1.5f * (sawLow + sawHigh);

        for (size_t ch = 0; ch < osBlock.getNumChannels(); ++ch)
        {
            auto* p = osBlock.getChannelPointer (ch);
            auto& fLow  = sawLowF[ch];
            auto& fHigh = sawHighF[ch];

            for (size_t i = 0; i < osBlock.getNumSamples(); ++i)
            {
                float y = std::tanh (gain * p[i] + bias) - biasOffset;
                y = std::tanh (2.5f * y);        // second clipping stage for extra grind

                // Chainsaw: big low + upper-mid boosts, then slam them into "amp" saturation
                const float eq = fHigh.processSample (fLow.processSample (y));
                const float amp = std::tanh (ampDrive * eq);
                p[i] = y + sawAmount * (amp - y);
            }
        }

        oversampler.processSamplesDown (block);

        // ---- Stage 3: tone, level, mix ----
        tone.setCutoffFrequency (800.0f * std::pow (15.0f, toneP));   // 800 Hz (dark) to 12 kHz (bright)
        const float outGain = juce::Decibels::decibelsToGain (level);

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            const auto* dry = dryCopy.getReadPointer (ch);

            for (int i = 0; i < numSamples; ++i)
            {
                float y = dcOut.processSample (ch, d[i]);
                y = tone.processSample (ch, y) * outGain;
                d[i] = mix * y + (1.0f - mix) * dry[i];
            }
        }
    }

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        if (auto xml = params.copyState().createXml())
            copyXmlToBinary (*xml, destData);
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        if (auto xml = getXmlFromBinary (data, sizeInBytes))
            params.replaceState (juce::ValueTree::fromXml (*xml));
    }

private:
    struct ChannelState
    {
        float upX1 = 0.0f, upY1 = 0.0f;
        float lp1 = 0.0f, lp2 = 0.0f;
        float env = 0.0f;
        float flip = 1.0f;
        bool wasPositive = false;
        float subLp = 0.0f;
        float gateEnv = 0.0f;
        float gateGain = 0.0f;
        bool gateIsOpen = false;
    };

    float onePole (float hz) const
    {
        return std::exp (-juce::MathConstants<float>::twoPi * hz / sr);
    }

    // Only rebuilds the EQ when a Chainsaw knob actually moves
    void updateSawFilters (float lowDb, float highDb)
    {
        using Coefs = juce::dsp::IIR::Coefficients<float>;

        if (lowDb != lastLowDb)
        {
            auto c = Coefs::makePeakFilter ((double) osRate, 100.0f, 0.9f, juce::Decibels::decibelsToGain (lowDb));
            for (auto& f : sawLowF)
                f.coefficients = c;
            lastLowDb = lowDb;
        }

        if (highDb != lastHighDb)
        {
            auto c = Coefs::makePeakFilter ((double) osRate, 1200.0f, 1.2f, juce::Decibels::decibelsToGain (highDb));
            for (auto& f : sawHighF)
                f.coefficients = c;
            lastHighDb = highDb;
        }
    }

    juce::AudioProcessorValueTreeState params;

    juce::dsp::Oversampling<float> oversampler { 2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true };
    juce::dsp::StateVariableTPTFilter<float> tone, dcOut;
    std::array<juce::dsp::IIR::Filter<float>, 2> sawLowF, sawHighF;
    juce::AudioBuffer<float> dryCopy;
    std::array<ChannelState, 2> state;

    float sr = 44100.0f, osRate = 176400.0f;
    float lastLowDb = -100.0f, lastHighDb = -100.0f;
    float dcCoef = 0.0f, trackCoef = 0.0f, envAttack = 0.0f, envRelease = 0.0f, subCoef = 0.0f;
    float gateEnvAttack = 0.0f, gateEnvRelease = 0.0f, gateOpenCoef = 0.0f, gateCloseCoef = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilthOctaveProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FilthOctaveProcessor();
}
