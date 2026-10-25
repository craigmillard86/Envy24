#ifndef _Envy24AudioEngine_H
#define _Envy24AudioEngine_H

#include <IOKit/audio/IOAudioEngine.h>

#include "AudioDevice.h"

#define Envy24AudioEngine com_Envy24AudioEngine

// The ICE1712 multi-track DMA always moves full frames:
// playback = PSDOUT0-3 L/R + S/PDIF L/R, capture = PSDIN0-3 L/R + S/PDIF L/R + digital mixer L/R
#define DMA_PLAY_SLOTS	10
#define DMA_REC_SLOTS	12

class IOFilterInterruptEventSource;
class IOBufferMemoryDescriptor;
class IODMACommand;
class IOInterruptEventSource;

class Envy24AudioEngine : public IOAudioEngine
{
    OSDeclareDefaultStructors(Envy24AudioEngine)
    
public:

    virtual bool	init(struct CardData* i_card);
    virtual void	free();
    
    virtual bool	initHardware(IOService *provider);
    virtual void	stop(IOService *provider);
	
	UInt32 lookUpFrequencyBits(UInt32 Frequency, const UInt32* FreqList, const UInt32* FreqBitList, UInt32 ListSize, UInt32 Default);
    virtual void	dumpRegisters();

	virtual IOAudioStream *createNewAudioStream(IOAudioStreamDirection direction, void *sampleBuffer, UInt32 sampleBufferSize, UInt32 channel, UInt32 channels);

    virtual IOReturn performAudioEngineStart();
    virtual IOReturn performAudioEngineStop();
    
    virtual UInt32 getCurrentSampleFrame();
    
    virtual IOReturn performFormatChange(IOAudioStream *audioStream, const IOAudioStreamFormat *newFormat, const IOAudioSampleRate *newSampleRate);

    virtual IOReturn clipOutputSamples(const void *mixBuf, void *sampleBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream);
    virtual IOReturn convertInputSamples(const void *sampleBuf, void *destBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream);
    virtual IOReturn eraseOutputSamples(const void *mixBuf, void *sampleBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream);
    
    static void interruptHandler(OSObject *owner, IOInterruptEventSource *source, int count);
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *source);
    virtual void filterInterrupt(int index);
	
private:
	struct CardData				   *card;
	UInt32							currentSampleRate;
    
	SInt32							*inputBuffer;
    SInt32							*outputBuffer;

	// Core Audio sees only the first playChannels/recChannels slots of each DMA frame.
	// Its sample buffers are these shadows; clip/convert move data to and from the DMA buffers.
	UInt32							numSampleFrames;
	UInt32							playChannels;
	UInt32							recChannels;
	SInt32							*outputShadow;
	SInt32							*inputShadow;
	UInt32							outputShadowSize;
	UInt32							inputShadowSize;
    IOBufferMemoryDescriptor		*inputBufferDescriptor;
    IOBufferMemoryDescriptor		*outputBufferDescriptor;
    IODMACommand					*inputDMACommand;
    IODMACommand					*outputDMACommand;
    
	IOPhysicalAddress               physicalAddressInput;
	IOPhysicalAddress               physicalAddressOutput;
    
    IOFilterInterruptEventSource	*interruptEventSource;
	UInt8 lastirq;
	
	void * IOMallocContiguousWithLimit(vm_size_t size,
									   mach_vm_address_t          physicalMask,
									   IOPhysicalAddress * physicalAddress,
									   IOBufferMemoryDescriptor ** descriptor,
									   IODMACommand ** command);
};

#endif /* _Envy24AudioEngine_H */
