#include "AudioEngine.h"
#include <IOKit/IOLib.h>


#define super IOAudioEngine

// The function clipOutputSamples() is called to clip and convert samples from the float mix buffer into the actual
// hardware sample buffer.  The samples to be clipped, are guaranteed not to wrap from the end of the buffer to the
// beginning.
// This implementation is very inefficient, but illustrates the clip and conversion process that must take place.
// Each floating-point sample must be clipped to a range of -1.0 to 1.0 and then converted to the hardware buffer
// format

// The parameters are as follows:
//		mixBuf - a pointer to the beginning of the float mix buffer - its size is based on the number of sample frames
// 					times the number of channels for the stream
//		sampleBuf - a pointer to the beginning of the hardware formatted sample buffer - this is the same buffer passed
//					to the IOAudioStream using setSampleBuffer()
//		firstSampleFrame - this is the index of the first sample frame to perform the clipping and conversion on
//		numSampleFrames - the total number of sample frames to clip and convert
//		streamFormat - the current format of the IOAudioStream this function is operating on
//		audioStream - the audio stream this function is operating on
IOReturn Envy24AudioEngine::clipOutputSamples(const void *mixBuf, void *sampleBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream)
{
    const float *floatMixBuf = (const float *)mixBuf;
    const UInt32 channels = streamFormat->fNumChannels;
    UInt32 frame, ch;

    // sampleBuf is the Core Audio shadow buffer and is not used: samples go straight
    // into the DMA buffer, whose frames are always DMA_PLAY_SLOTS wide. Core Audio
    // channel n is DMA slot n, so the first two codecs get channels 1-4.
    for (frame = firstSampleFrame; frame < firstSampleFrame + numSampleFrames; frame++) {
        const float *in = &floatMixBuf[frame * channels];
        SInt32 *out = &outputBuffer[frame * DMA_PLAY_SLOTS];

        for (ch = 0; ch < channels; ch++) {
            float inSample = in[ch];
            SInt32 value;

            // Clip to -1.0 .. 1.0 and scale to 24 bits. Scaling by 2^31 in float
            // overflows SInt32 at +1.0 and produces a full-scale negative sample.
            if (inSample > 1.0f)
                inSample = 1.0f;
            else if (inSample < -1.0f)
                inSample = -1.0f;

            if (inSample >= 0)
                value = (SInt32) (inSample * 8388607.0f);
            else
                value = (SInt32) (inSample * 8388608.0f);

            // 24 significant bits at the top of the 32-bit slot
            out[ch] = (SInt32) ((UInt32) value << 8);
        }
    }

    return kIOReturnSuccess;
}

// Erasing has to clear the DMA frames too, because the stream's sample buffer is only a shadow.
IOReturn Envy24AudioEngine::eraseOutputSamples(const void *mixBuf, void *sampleBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream)
{
    IOReturn result = super::eraseOutputSamples(mixBuf, sampleBuf, firstSampleFrame, numSampleFrames, streamFormat, audioStream);

    bzero(&outputBuffer[firstSampleFrame * DMA_PLAY_SLOTS], numSampleFrames * DMA_PLAY_SLOTS * sizeof(SInt32));

    return result;
}

// The function convertInputSamples() is responsible for converting from the hardware format 
// in the input sample buffer to float samples in the destination buffer and scale the samples 
// to a range of -1.0 to 1.0.  This function is guaranteed not to have the samples wrapped
// from the end of the buffer to the beginning.
// This function only needs to be implemented if the device has any input IOAudioStreams

// This implementation is very inefficient, but illustrates the conversion and scaling that needs to take place.

// The parameters are as follows:
//		sampleBuf - a pointer to the beginning of the hardware formatted sample buffer - this is the same buffer passed
//					to the IOAudioStream using setSampleBuffer()
//		destBuf - a pointer to the float destination buffer - this is the buffer that the CoreAudio.framework uses
//					its size is numSampleFrames * numChannels * sizeof(float)
//		firstSampleFrame - this is the index of the first sample frame to the input conversion on
//		numSampleFrames - the total number of sample frames to convert and scale
//		streamFormat - the current format of the IOAudioStream this function is operating on
//		audioStream - the audio stream this function is operating on
IOReturn Envy24AudioEngine::convertInputSamples(const void *sampleBuf, void *destBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream)
{
    float *floatDestBuf = (float *)destBuf;
    const UInt32 channels = streamFormat->fNumChannels;
    UInt32 frame, ch;

    // sampleBuf is the Core Audio shadow buffer and is not used: samples come straight
    // from the DMA buffer, whose frames are always DMA_REC_SLOTS wide. destBuf starts
    // at firstSampleFrame and holds numSampleFrames * channels floats.
    for (frame = 0; frame < numSampleFrames; frame++) {
        const SInt32 *in = &inputBuffer[(firstSampleFrame + frame) * DMA_REC_SLOTS];

        for (ch = 0; ch < channels; ch++) {
            // 24 significant bits at the top of the slot; scale the full 32-bit value
            *floatDestBuf++ = (float) in[ch] * (1.0f / 2147483648.0f);
        }
    }

    return kIOReturnSuccess;
}
