/* ----------------------------------------------------------------------------

   traKmeter
   =========
   Loudness meter for correctly setting up tracking and mixing levels

   Copyright (c) 2012-2015 Martin Zuther (http://www.mzuther.de/)

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

   Thank you for using free software!

---------------------------------------------------------------------------- */

#include "plugin_processor.h"
#include "plugin_editor.h"


/*==============================================================================

Flow of parameter processing:

  Editor:      buttonClicked(button) / sliderValueChanged(slider)
  Processor:   changeParameter(nIndex, fValue)
  Processor:   setParameter(nIndex, fValue)
  Parameters:  setFloat(nIndex, fValue)
  Editor:      actionListenerCallback(strMessage)
  Editor:      updateParameter(nIndex)

==============================================================================*/

TraKmeterAudioProcessor::TraKmeterAudioProcessor()
{
    bSampleRateIsValid = false;
    audioFilePlayer = nullptr;
    pRingBufferInput = nullptr;

    nNumInputChannels = 0;
    pMeterBallistics = nullptr;

    setLatencySamples(0);
    pPluginParameters = new TraKmeterPluginParameters();

    // depend on "TraKmeterPluginParameters"!
    bTransientMode = getBoolean(TraKmeterPluginParameters::selTransientMode);
    nCrestFactor = getRealInteger(TraKmeterPluginParameters::selCrestFactor);

    nDecibels = getRealInteger(TraKmeterPluginParameters::selGain);
    dGain = MeterBallistics::decibel2level_double(nDecibels);

    fProcessedSeconds = 0.0f;

    fPeakLevels = nullptr;
    fRmsLevels = nullptr;

    nOverflows = nullptr;

    pDither = new Dither(24);
}


TraKmeterAudioProcessor::~TraKmeterAudioProcessor()
{
    removeAllActionListeners();

    // call function "releaseResources()" by force to make sure all
    // allocated memory is freed
    releaseResources();

    delete pPluginParameters;
    pPluginParameters = nullptr;

    delete audioFilePlayer;
    audioFilePlayer = nullptr;

    delete pDither;
    pDither = nullptr;
}


//==============================================================================

const String TraKmeterAudioProcessor::getName() const
{
    return JucePlugin_Name;
}


int TraKmeterAudioProcessor::getNumParameters()
{
    return pPluginParameters->getNumParameters(false);
}


const String TraKmeterAudioProcessor::getParameterName(int nIndex)
{
    return pPluginParameters->getName(nIndex);
}


const String TraKmeterAudioProcessor::getParameterText(int nIndex)
{
    return pPluginParameters->getText(nIndex);
}


float TraKmeterAudioProcessor::getParameter(int nIndex)
{
    // This method will be called by the host, probably on the audio
    // thread, so it's absolutely time-critical. Don't use critical
    // sections or anything GUI-related, or anything at all that may
    // block in any way!

    return pPluginParameters->getFloat(nIndex);
}


void TraKmeterAudioProcessor::changeParameter(int nIndex, float fValue)
{
    // notify host of parameter change (this will automatically call
    // "setParameter"!)
    beginParameterChangeGesture(nIndex);
    setParameterNotifyingHost(nIndex, fValue);
    endParameterChangeGesture(nIndex);
}


void TraKmeterAudioProcessor::setParameter(int nIndex, float fValue)
{
    // This method will be called by the host, probably on the audio
    // thread, so it's absolutely time-critical. Don't use critical
    // sections or anything GUI-related, or anything at all that may
    // block in any way!

    // Please only call this method directly for non-automatable
    // values!

    pPluginParameters->setFloat(nIndex, fValue);

    if (nIndex == TraKmeterPluginParameters::selTransientMode)
    {
        setTransientMode(getBoolean(nIndex));
    }
    else if (nIndex == TraKmeterPluginParameters::selCrestFactor)
    {
        setCrestFactor(getRealInteger(nIndex));
    }
    else if (nIndex == TraKmeterPluginParameters::selGain)
    {
        nDecibels = getRealInteger(TraKmeterPluginParameters::selGain);
        dGain = MeterBallistics::decibel2level_double(nDecibels);
    }

    // notify plug-in editor of parameter change
    if (pPluginParameters->hasChanged(nIndex))
    {
        // for visible parameters, notify the editor of changes (this
        // will also clear the change flag)
        if (nIndex < pPluginParameters->getNumParameters(false))
        {
            // "PC" --> parameter changed, followed by a hash and the
            // parameter's ID
            sendActionMessage("PC#" + String(nIndex));
        }
        // for hidden parameters, we only have to clear the change
        // flag
        else
        {
            pPluginParameters->clearChangeFlag(nIndex);
        }
    }
}


void TraKmeterAudioProcessor::clearChangeFlag(int nIndex)
{
    pPluginParameters->clearChangeFlag(nIndex);
}


void TraKmeterAudioProcessor::setChangeFlag(int nIndex)
{
    pPluginParameters->setChangeFlag(nIndex);
}


bool TraKmeterAudioProcessor::hasChanged(int nIndex)
{
    return pPluginParameters->hasChanged(nIndex);
}


void TraKmeterAudioProcessor::updateParameters(bool bIncludeHiddenParameters)
{
    int nNumParameters = pPluginParameters->getNumParameters(false);

    for (int nIndex = 0; nIndex < nNumParameters; nIndex++)
    {
        if (pPluginParameters->hasChanged(nIndex))
        {
            float fValue = pPluginParameters->getFloat(nIndex);
            changeParameter(nIndex, fValue);
        }
    }

    if (bIncludeHiddenParameters)
    {
        // handle hidden parameters here!

        // the following parameters need no updating:
        //
        // * selValidationFileName
        // * selValidationSelectedChannel
        // * selValidationAverageMeterLevel
        // * selValidationPeakMeterLevel
        // * selValidationCSVFormat

    }
}


bool TraKmeterAudioProcessor::getBoolean(int nIndex)
{
    // This method will be called by the host, probably on the audio
    // thread, so it's absolutely time-critical. Don't use critical
    // sections or anything GUI-related, or anything at all that may
    // block in any way!

    return pPluginParameters->getBoolean(nIndex);
}


int TraKmeterAudioProcessor::getRealInteger(int nIndex)
{
    // This method will be called by the host, probably on the audio
    // thread, so it's absolutely time-critical. Don't use critical
    // sections or anything GUI-related, or anything at all that may
    // block in any way!

    return pPluginParameters->getRealInteger(nIndex);
}


File TraKmeterAudioProcessor::getParameterValidationFile()
{
    // This method will be called by the host, probably on the audio
    // thread, so it's absolutely time-critical. Don't use critical
    // sections or anything GUI-related, or anything at all that may
    // block in any way!

    return pPluginParameters->getValidationFile();
}


void TraKmeterAudioProcessor::setParameterValidationFile(File &fileValidation)
{
    // This method will be called by the host, probably on the audio
    // thread, so it's absolutely time-critical. Don't use critical
    // sections or anything GUI-related, or anything at all that may
    // block in any way!

    pPluginParameters->setValidationFile(fileValidation);
}


const String TraKmeterAudioProcessor::getInputChannelName(int channelIndex) const
{
    return "Input " + String(channelIndex + 1);
}


const String TraKmeterAudioProcessor::getOutputChannelName(int channelIndex) const
{
    return "Output " + String(channelIndex + 1);
}


bool TraKmeterAudioProcessor::isInputChannelStereoPair(int nIndex) const
{
    return true;
}


bool TraKmeterAudioProcessor::isOutputChannelStereoPair(int nIndex) const
{
    return true;
}


bool TraKmeterAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}


bool TraKmeterAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}


bool TraKmeterAudioProcessor::silenceInProducesSilenceOut() const
{
    return true;
}


double TraKmeterAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}


int TraKmeterAudioProcessor::getNumChannels()
{
    return nNumInputChannels;
}


int TraKmeterAudioProcessor::getNumPrograms()
{
    return 0;
}


int TraKmeterAudioProcessor::getCurrentProgram()
{
    return 0;
}


void TraKmeterAudioProcessor::setCurrentProgram(int nIndex)
{
}


const String TraKmeterAudioProcessor::getProgramName(int nIndex)
{
    return String::empty;
}


void TraKmeterAudioProcessor::changeProgramName(int nIndex, const String &newName)
{
}

//==============================================================================

void TraKmeterAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Use this method as the place to do any pre-playback
    // initialisation that you need..

    DBG("[traKmeter] in method TraKmeterAudioProcessor::prepareToPlay()");

    if ((sampleRate < 44100) || (sampleRate > 192000))
    {
        Logger::outputDebugString("[traKmeter] WARNING: sample rate of " + String(sampleRate) + " Hz not supported");
        bSampleRateIsValid = false;
        return;
    }
    else
    {
        bSampleRateIsValid = true;
    }

    nNumInputChannels = getNumInputChannels();

    if (nNumInputChannels < 1)
    {
        nNumInputChannels = JucePlugin_MaxNumInputChannels;
        DBG("[traKmeter] no input channels detected, correcting this");
    }

    DBG("[traKmeter] number of input channels: " + String(nNumInputChannels));

    pMeterBallistics = new MeterBallistics(nNumInputChannels, nCrestFactor, true, false, bTransientMode);

    fPeakLevels = new float[nNumInputChannels];
    fRmsLevels = new float[nNumInputChannels];

    nOverflows = new int[nNumInputChannels];

    for (int nChannel = 0; nChannel < nNumInputChannels; nChannel++)
    {
        fPeakLevels[nChannel] = 0.0f;
        fRmsLevels[nChannel] = 0.0f;

        nOverflows[nChannel] = 0;
    }

    // make sure that ring buffer can hold at least
    // TRAKMETER_BUFFER_SIZE samples and is large enough to receive a
    // full block of audio
    nSamplesInBuffer = 0;
    unsigned int uRingBufferSize = (samplesPerBlock > TRAKMETER_BUFFER_SIZE) ? samplesPerBlock : TRAKMETER_BUFFER_SIZE;

    pRingBufferInput = new AudioRingBuffer("Input ring buffer", nNumInputChannels, uRingBufferSize, TRAKMETER_BUFFER_SIZE, TRAKMETER_BUFFER_SIZE);
    pRingBufferInput->setCallbackClass(this);
}


void TraKmeterAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free
    // up any spare memory, etc.

    DBG("[traKmeter] in method TraKmeterAudioProcessor::releaseResources()");

    if (!bSampleRateIsValid)
    {
        return;
    }

    delete pMeterBallistics;
    pMeterBallistics = nullptr;

    delete pRingBufferInput;
    pRingBufferInput = nullptr;

    delete [] fPeakLevels;
    fPeakLevels = nullptr;

    delete [] fRmsLevels;
    fRmsLevels = nullptr;

    delete [] nOverflows;
    nOverflows = nullptr;

    delete audioFilePlayer;
    audioFilePlayer = nullptr;
}


void TraKmeterAudioProcessor::processBlock(AudioSampleBuffer &buffer, MidiBuffer &midiMessages)
{
    // This is the place where you'd normally do the guts of your
    // plug-in's audio processing...

    int nNumSamples = buffer.getNumSamples();

    if (!bSampleRateIsValid)
    {
        for (int nChannel = 0; nChannel < getNumOutputChannels(); nChannel++)
        {
            buffer.clear(nChannel, 0, nNumSamples);
        }

        return;
    }

    if (nNumInputChannels < 1)
    {
        Logger::outputDebugString("[traKmeter] nNumInputChannels < 1");
        return;
    }

    // In case we have more outputs than inputs, we'll clear any
    // output channels that didn't contain input data, because these
    // aren't guaranteed to be empty -- they may contain garbage.

    for (int nChannel = nNumInputChannels; nChannel < getNumOutputChannels(); nChannel++)
    {
        buffer.clear(nChannel, 0, nNumSamples);
    }

    if (audioFilePlayer)
    {
        audioFilePlayer->fillBufferChunk(&buffer);
    }

    bool bMixMode = getBoolean(TraKmeterPluginParameters::selMixMode);

    if (bMixMode && (nDecibels != 0))
    {
        for (int nChannel = 0; nChannel < buffer.getNumChannels(); nChannel++)
        {
            for (int nSample = 0; nSample < buffer.getNumSamples(); nSample++)
            {
                double dSampleValue = buffer.getSample(nChannel, nSample);
                float fNewSampleValue = pDither->dither(dSampleValue * dGain);
                buffer.setSample(nChannel, nSample, fNewSampleValue);
            }
        }
    }

    pRingBufferInput->addSamples(buffer, 0, nNumSamples);

    nSamplesInBuffer += nNumSamples;
    nSamplesInBuffer %= TRAKMETER_BUFFER_SIZE;
}


void TraKmeterAudioProcessor::processBufferChunk(AudioSampleBuffer &buffer, const unsigned int uChunkSize, const unsigned int uBufferPosition, const unsigned int uProcessedSamples)
{
    bool hasOpenEditor = (getActiveEditor() != nullptr);

    if (hasOpenEditor)
    {
        unsigned int uPreDelay = uChunkSize / 2;

        // length of buffer chunk in fractional seconds
        // (1024 samples / 44100 samples/s = 23.2 ms)
        fProcessedSeconds = (float) uChunkSize / (float) getSampleRate();

        for (int nChannel = 0; nChannel < nNumInputChannels; nChannel++)
        {
            // determine peak level for uChunkSize samples (use
            // pre-delay)
            fPeakLevels[nChannel] = pRingBufferInput->getMagnitude(nChannel, uChunkSize, uPreDelay);

            // determine peak level for uChunkSize samples (use
            // pre-delay)
            fRmsLevels[nChannel] = pRingBufferInput->getRMSLevel(nChannel, uChunkSize, uPreDelay);

            // determine overflows for uChunkSize samples (use
            // pre-delay)
            nOverflows[nChannel] = countOverflows(pRingBufferInput, nChannel, uChunkSize, uPreDelay);

            // apply meter ballistics and store values so that the
            // editor can access them
            pMeterBallistics->updateChannel(nChannel, fProcessedSeconds, fPeakLevels[nChannel], fRmsLevels[nChannel], nOverflows[nChannel]);
        }

        // "UM" --> update meters
        sendActionMessage("UM");
    }

    AudioSampleBuffer TempAudioBuffer = AudioSampleBuffer(nNumInputChannels, uChunkSize);
    pRingBufferInput->copyToBuffer(TempAudioBuffer, 0, uChunkSize, 0);
}


void TraKmeterAudioProcessor::startValidation(File fileAudio, int nSelectedChannel, bool bReportCSV, bool bAverageMeterLevel, bool bPeakMeterLevel)
{
    audioFilePlayer = new AudioFilePlayer(fileAudio, (int) getSampleRate(), pMeterBallistics, nCrestFactor);
    audioFilePlayer->setReporters(nSelectedChannel, bReportCSV, bAverageMeterLevel, bPeakMeterLevel);

    // reset all meters before we start the validation
    pMeterBallistics->reset();

    // refresh editor; "V+" --> validation started
    sendActionMessage("V+");
}


void TraKmeterAudioProcessor::stopValidation()
{
    delete audioFilePlayer;
    audioFilePlayer = nullptr;

    // refresh editor; "V-" --> validation stopped
    sendActionMessage("V-");
}


bool TraKmeterAudioProcessor::isValidating()
{
    if (audioFilePlayer == nullptr)
    {
        return false;
    }
    else
    {
        if (audioFilePlayer->isPlaying())
        {
            return true;
        }
        else
        {
            stopValidation();
            return false;
        }
    }
}


int TraKmeterAudioProcessor::countOverflows(AudioRingBuffer *ring_buffer, const unsigned int channel, const unsigned int length, const unsigned int pre_delay)
{
    // initialise number of overflows in this buffer
    int nOverflows = 0;

    // loop through samples of buffer
    for (unsigned int uSample = 0; uSample < length; uSample++)
    {
        // get current sample value
        float fSampleValue = ring_buffer->getSample(channel, uSample, pre_delay);

        // in the 16-bit domain, full scale corresponds to an absolute
        // integer value of 32'767 or 32'768, so we'll treat absolute
        // levels of 32'767 and above as overflows; this corresponds
        // to a floating-point level of 32'767 / 32'768 = 0.9999694
        // (approx. -0.001 dBFS).
        if ((fSampleValue < -0.9999f) || (fSampleValue > 0.9999f))
        {
            nOverflows++;
        }
    }

    // return number of overflows in this buffer
    return nOverflows;
}


MeterBallistics *TraKmeterAudioProcessor::getLevels()
{
    return pMeterBallistics;
}


bool TraKmeterAudioProcessor::getTransientMode()
{
    return bTransientMode;
}


void TraKmeterAudioProcessor::setTransientMode(const bool transient_mode)
{
    if (transient_mode != bTransientMode)
    {
        bTransientMode = transient_mode;

        if (pMeterBallistics)
        {
            delete pMeterBallistics;
            pMeterBallistics = nullptr;

            pMeterBallistics = new MeterBallistics(nNumInputChannels, nCrestFactor, true, false, bTransientMode);
        }
    }
}


int TraKmeterAudioProcessor::getCrestFactor()
{
    return nCrestFactor;
}


void TraKmeterAudioProcessor::setCrestFactor(const int crest_factor)
{
    if (crest_factor != nCrestFactor)
    {
        nCrestFactor = crest_factor;

        if (pMeterBallistics)
        {
            pMeterBallistics->setCrestFactor(nCrestFactor);
        }

        if (audioFilePlayer)
        {
            audioFilePlayer->setCrestFactor(nCrestFactor);
        }
    }
}


//==============================================================================

AudioProcessorEditor *TraKmeterAudioProcessor::createEditor()
{
    //  meter ballistics are not updated when the editor is closed, so
    //  reset them here
    if (pMeterBallistics)
    {
        pMeterBallistics->reset();
    }

    if (nNumInputChannels > 0)
    {
        return new TraKmeterAudioProcessorEditor(this, pPluginParameters, nNumInputChannels, nCrestFactor);
    }
    else
    {
        return new TraKmeterAudioProcessorEditor(this, pPluginParameters, JucePlugin_MaxNumInputChannels, nCrestFactor);
    }
}


bool TraKmeterAudioProcessor::hasEditor() const
{
    return true;
}


//==============================================================================

void TraKmeterAudioProcessor::getStateInformation(MemoryBlock &destData)
{
    copyXmlToBinary(pPluginParameters->storeAsXml(), destData);
}


void TraKmeterAudioProcessor::setStateInformation(const void *data, int sizeInBytes)
{
    ScopedPointer<XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    pPluginParameters->loadFromXml(xml);

    updateParameters(true);
}

//==============================================================================

// This creates new instances of the plug-in.
AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
    return new TraKmeterAudioProcessor();
}


// Local Variables:
// ispell-local-dictionary: "british"
// End:
