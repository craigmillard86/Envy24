#include "AudioEngine.h"

#include <IOKit/IOLib.h>

#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IODMACommand.h>

#include <IOKit/pci/IOPCIDevice.h>
#include "regs.h"
#include "misc.h"
#include "Delta66.h"

#define INITIAL_SAMPLE_RATE	44100

#define FREQUENCIES 13

static const ULONG Frequencies[ FREQUENCIES ] =
{
    8000,
    9600,
    11025,
    12000,
    16000,
    22050,
    24000,
    32000,
    44100, // CD
    48000,
    64000,
    88200,
    96000
};

static const ULONG FrequencyBits[ FREQUENCIES ] =
{
    6,
    3,
    10,
    2,
    5,
    9,
    1,
    4,
    8,
    0,
    15,
    11,
    7
};

#define SPDIF_FREQUENCIES 7

static const ULONG SPDIF_Frequencies[ SPDIF_FREQUENCIES ] =
{
	32000,
    44100, // CD
    48000,
    88200,
    96000,
	176400,
    192000
};

static const ULONG SPDIF_FrequencyBits[ SPDIF_FREQUENCIES ] =
{
    3,
    0,
    2,
    4,
    5,
    7,
    6
};



#define super IOAudioEngine

OSDefineMetaClassAndStructors(Envy24AudioEngine, IOAudioEngine)

bool Envy24AudioEngine::init(struct CardData* i_card)
{
    bool result = false;
    
    DBGPRINT("Envy24AudioEngine[%p]::init(%p)\n", this, i_card);

    if (!super::init(NULL)) {
        goto Done;
    }

	if (!i_card) {
		goto Done;
	}
	card = i_card;
	
    result = true;
    
Done:

    return result;
}

bool Envy24AudioEngine::initHardware(IOService *provider)
{
    bool result = false;
    IOAudioSampleRate initialSampleRate;
    IOAudioStream *audioStream;
    IOWorkLoop *workLoop;
    
    DBGPRINT("Envy24AudioEngine[%p]::initHardware(%p)\n", this, provider);
    
    if (!super::initHardware(provider)) {
        goto Done;
    }
	
    // Setup the initial sample rate for the audio engine
    initialSampleRate.whole = INITIAL_SAMPLE_RATE;
    initialSampleRate.fraction = 0;
    
    //setDescription("Envy24 Audio Engine");
    
    setSampleRate(&initialSampleRate);
    
    // The number of sample frames in each buffer is set once the DMA buffers are allocated
	setSampleOffset(48);
	
    workLoop = getWorkLoop();
    if (!workLoop) {
	    IOLog("Couldn't get workloop!\n");
        goto Done;
    }
    
    // Create an interrupt event source through which to receive interrupt callbacks
    // In this case, we only want to do work at primary interrupt time, so
    // we create an IOFilterInterruptEventSource which makes a filtering call
    // from the primary interrupt who's purpose is to determine if
    // our secondary interrupt handler is to be called.  In our case, we
    // can do the work in the filter routine and then return false to
    // indicate that we do not want our secondary handler called
	
	    
    // Allocate our input and output buffers
	// The ICE1712 can only address the first 256 MB (28-bit DMA, see ALSA ice1712.c).
	// With VT-d active the IOMMU provides such an address for any buffer.
	// Never fall back to unrestricted memory: the chip would truncate the address.
	mach_vm_address_t physicalMask;
	physicalMask = 0x0FFFFFFCULL;

	// Contiguous memory below 256 MB is scarce once the system is up, so fall back
	// to smaller buffers rather than fail. 1024 frames is still 21 ms at 48 kHz.
	static const UInt32 frameChoices[] = { NUM_SAMPLE_FRAMES, 2048, 1024 };
	numSampleFrames = 0;

	for (UInt32 i = 0; i < sizeof(frameChoices) / sizeof(frameChoices[0]); i++)
	{
		card->Specific.BufferSize = frameChoices[i] * DMA_PLAY_SLOTS * sizeof(SInt32);
		card->Specific.BufferSizeRec = frameChoices[i] * DMA_REC_SLOTS * sizeof(SInt32);

		outputBuffer = (SInt32 *)IOMallocContiguousWithLimit(card->Specific.BufferSize, physicalMask, &physicalAddressOutput, &outputBufferDescriptor, &outputDMACommand);
		if (!outputBuffer)
			continue;

		inputBuffer = (SInt32 *)IOMallocContiguousWithLimit(card->Specific.BufferSizeRec, physicalMask, &physicalAddressInput, &inputBufferDescriptor, &inputDMACommand);
		if (!inputBuffer)
		{
			outputDMACommand->clearMemoryDescriptor();
			outputDMACommand->release();
			outputDMACommand = NULL;
			outputBufferDescriptor->release();
			outputBufferDescriptor = NULL;
			outputBuffer = NULL;
			continue;
		}

		numSampleFrames = frameChoices[i];
		break;
	}

	if (!numSampleFrames) {
	    IOLog("Envy24: couldn't allocate DMA buffers below 256 MB, even at 1024 frames\n");
	    goto Done;
	}

	IOLog("Envy24: DMA buffers %u frames, playback device address %llx, capture %llx\n", numSampleFrames,
	      (unsigned long long) physicalAddressOutput, (unsigned long long) physicalAddressInput);

	if ((physicalAddressOutput + card->Specific.BufferSize) > 0x10000000ULL ||
	    (physicalAddressInput + card->Specific.BufferSizeRec) > 0x10000000ULL)
	{
		IOLog("Envy24: DMA buffer above 256 MB, refusing to start\n");
		goto Done;
	}

	card->pci_dev->ioWrite32(MT_DMA_PB_ADDRESS, (UInt32) physicalAddressOutput, card->mtbase);
	card->pci_dev->ioWrite32(MT_DMA_REC_ADDRESS, (UInt32) physicalAddressInput, card->mtbase);
	card->pci_dev->ioWrite8(MT_SAMPLERATE, 8, card->mtbase); // initialize to 44100 Hz

    setNumSampleFramesPerBuffer(numSampleFrames);
	
    if (card->SubType == MAUDIO_DELTA66 || card->SubType == MAUDIO_DELTA66E)
    {
        // two AK4524 codecs on PSDOUT0/1 and PSDIN0/1; S/PDIF not enabled yet
        playChannels = 4;
        recChannels = 4;
    }
    else
    {
        playChannels = DMA_PLAY_SLOTS;
        recChannels = DMA_REC_SLOTS;
    }

    outputShadowSize = numSampleFrames * playChannels * sizeof(SInt32);
    inputShadowSize = numSampleFrames * recChannels * sizeof(SInt32);
    outputShadow = (SInt32 *)IOMalloc(outputShadowSize);
    inputShadow = (SInt32 *)IOMalloc(inputShadowSize);
    if (!outputShadow || !inputShadow) {
        IOLog("Envy24: couldn't allocate sample buffers\n");
        goto Done;
    }
    bzero(outputShadow, outputShadowSize);
    bzero(inputShadow, inputShadowSize);

    // Create an IOAudioStream for each buffer and add it to this audio engine
    audioStream = createNewAudioStream(kIOAudioStreamDirectionOutput, outputShadow, outputShadowSize, 0, playChannels);
    if (!audioStream) {
        goto Done;
    }
	
	addAudioStream(audioStream);
    audioStream->release();

	
    audioStream = createNewAudioStream(kIOAudioStreamDirectionInput, inputShadow, inputShadowSize, 1, recChannels);
    if (!audioStream) {
        goto Done;
    }
    
    addAudioStream(audioStream);
    audioStream->release();
	
	// the interruptEventSource needs to be enabled here, else IRQ sharing doesn't work
	
    // In order to allow the interrupts to be received, the interrupt event source must be
    // added to the IOWorkLoop
    // Additionally, interrupts will not be firing until the interrupt event source is 
    // enabled by calling interruptEventSource->enable() - this probably doesn't need to
    // be done until performAudioEngineStart() is called, and can probably be disabled
    // when performAudioEngineStop() is called and the audio engine is no longer running
    // Although this really depends on the specific hardware
	
	interruptEventSource = IOFilterInterruptEventSource::filterInterruptEventSource(this, 
                                    Envy24AudioEngine::interruptHandler, 
                                    Envy24AudioEngine::interruptFilter,
                                    audioDevice->getProvider());
    if (!interruptEventSource) {
	    IOLog("Error: no interrupt event source!\n");
        goto Done;
    }
	
	interruptEventSource->enable();

    workLoop->addEventSource(interruptEventSource);

    result = true;
    
Done:
	if (result == false)
	{
		IOLog("Something failed in the engine!\n");
	}
    return result;
}

void Envy24AudioEngine::free()
{
    DBGPRINT("Envy24AudioEngine[%p]::free()\n", this);
    
    // We need to free our resources when we're going away
    
    if (interruptEventSource) {
	    interruptEventSource->disable();
        interruptEventSource->release();
        interruptEventSource = NULL;
    }
    
    if (outputDMACommand) {
        outputDMACommand->clearMemoryDescriptor();
        outputDMACommand->release();
        outputDMACommand = NULL;
    }

    if (inputDMACommand) {
        inputDMACommand->clearMemoryDescriptor();
        inputDMACommand->release();
        inputDMACommand = NULL;
    }

    if (outputBufferDescriptor) {
        outputBufferDescriptor->release();
        outputBufferDescriptor = NULL;
        outputBuffer = NULL;
    }
    
    if (inputBufferDescriptor) {
        inputBufferDescriptor->release();
        inputBufferDescriptor = NULL;
        inputBuffer = NULL;
    }

    if (outputShadow) {
        IOFree(outputShadow, outputShadowSize);
        outputShadow = NULL;
    }

    if (inputShadow) {
        IOFree(inputShadow, inputShadowSize);
        inputShadow = NULL;
    }
    
    super::free();
}

IOAudioStream *Envy24AudioEngine::createNewAudioStream(IOAudioStreamDirection direction,
														 void *sampleBuffer,
														 UInt32 sampleBufferSize,
														 UInt32 channel,
														 UInt32 channels)
{
    IOAudioStream *audioStream;
	
    // For this sample device, we are only creating a single format and allowing 44.1KHz and 48KHz
    audioStream = new IOAudioStream;

	if (audioStream) {
        if (!audioStream->initWithAudioEngine(this, direction, 1)) {
		    IOLog("initWithAudioEngine failed\n");
			IOSleep(3000);
            audioStream->release();
        } else {
            IOAudioSampleRate rate;
            // The ICE1712 uses 32-bit little-endian slots with 24 significant bits at the top
            IOAudioStreamFormat format = {
                channels,										// num channels
                kIOAudioStreamSampleFormatLinearPCM,			// sample format
                kIOAudioStreamNumericRepresentationSignedInt,	// numeric format
                24,												// bit depth
                32,												// bit width
                kIOAudioStreamAlignmentHighByte,				// 24 bits in the high bytes of 32
                kIOAudioStreamByteOrderLittleEndian,			// little endian
                true,											// format is mixable
                channel											// driver tag
            };

            // As part of creating a new IOAudioStream, its sample buffer needs to be set
            // It will automatically create a mix buffer should it be needed
            audioStream->setSampleBuffer(sampleBuffer, sampleBufferSize);
			
            rate.fraction = 0;

			for (int i = 0; i < FREQUENCIES; i++)
			{
				// first pass for the Delta 66: 44.1 and 48 kHz only
				if ((card->SubType == MAUDIO_DELTA66 || card->SubType == MAUDIO_DELTA66E) &&
				    Frequencies[i] != 44100 && Frequencies[i] != 48000)
					continue;

				rate.whole = Frequencies[i];
				audioStream->addAvailableFormat(&format, &rate, &rate, NULL, 0);
			}
						
			// Finally, the IOAudioStream's current format needs to be indicated
            audioStream->setFormat(&format, false);
        }
    }
	else
	{
		IOLog("Couldn't allocate IOAudioStream\n");
		IOSleep(3000);
    }

    return audioStream;
}

void Envy24AudioEngine::stop(IOService *provider)
{
    DBGPRINT("Envy24AudioEngine[%p]::stop(%p)\n", this, provider);
    
    // When our device is being stopped and torn down, we should go ahead and remove
    // the interrupt event source from the IOWorkLoop
    // Additionally, we'll go ahead and release the interrupt event source since it isn't
    // needed any more
    if (interruptEventSource) {
        IOWorkLoop *wl;
        
        wl = getWorkLoop();
        if (wl) {
            wl->removeEventSource(interruptEventSource);
        }
        
        interruptEventSource->release();
        interruptEventSource = NULL;
    }
    
    // Add code to shut down hardware (beyond what is needed to simply stop the audio engine)
    // There may be nothing needed here

    super::stop(provider);
}
    
IOReturn Envy24AudioEngine::performAudioEngineStart()
{
    DBGPRINT("Envy24AudioEngine[%p]::performAudioEngineStart()\n", this);
	IOPCIDevice *dev = card->pci_dev;
	
    ClearMask8(dev, card->mtbase, MT_DMA_CONTROL, MT_PLAY_START | MT_REC_START); // stop first
	ClearMask8(dev, card->mtbase, MT_INTR_MASK_STATUS, MT_PLAY_MASK); // | MT_REC_MASK);
	
	// Play
	clearAllSampleBuffers();
    UInt32 BufferSize32 = (card->Specific.BufferSize / 4) - 1;
	UInt16 BufferSize16 = BufferSize32 & 0xFFFF;
	
	dev->ioWrite16(MT_DMA_PB_LENGTH, BufferSize16, card->mtbase);
    dev->ioWrite16(MT_DMA_PB_INTLEN, BufferSize16, card->mtbase);
	//IOLog("Buffer size = %ld (%lx), BufferSize16 = %u\n", card->Specific.BufferSize, card->Specific.BufferSize, BufferSize16); 
    
	
	// REC
	BufferSize16 = (card->Specific.BufferSizeRec / 4) - 1;
	dev->ioWrite16(MT_DMA_REC_LENGTH, BufferSize16, card->mtbase);
    dev->ioWrite16(MT_DMA_REC_INTLEN, BufferSize16, card->mtbase);

    //dumpRegisters();
    //IOLog("START\n");

    // When performAudioEngineStart() gets called, the audio engine should be started from the beginning
    // of the sample buffer.  Because it is starting on the first sample, a new timestamp is needed
    // to indicate when that sample is being read from/written to.  The function takeTimeStamp() 
    // is provided to do that automatically with the current time.
    // By default takeTimeStamp() will increment the current loop count in addition to taking the current
    // timestamp.  Since we are starting a new audio engine run, and not looping, we don't want the loop count
    // to be incremented.  To accomplish that, false is passed to takeTimeStamp(). 
    takeTimeStamp(false);

    // Add audio - I/O start code here
	WriteMask8(card->pci_dev, card->mtbase, MT_DMA_CONTROL, MT_PLAY_START | MT_REC_START);

    return kIOReturnSuccess;
}

IOReturn Envy24AudioEngine::performAudioEngineStop()
{
    
	DBGPRINT("Envy24AudioEngine[%p]::performAudioEngineStop()\n", this);

    // Add audio - I/O stop code here
    ClearMask8(card->pci_dev, card->mtbase, MT_DMA_CONTROL, MT_PLAY_START | MT_REC_START);
    WriteMask8(card->pci_dev, card->mtbase, MT_INTR_MASK_STATUS, MT_PLAY_MASK | MT_REC_MASK);
	
	
    return kIOReturnSuccess;
}
    
UInt32 Envy24AudioEngine::getCurrentSampleFrame()
{
    
    // In order for the erase process to run properly, this function must return the current location of
    // the audio engine - basically a sample counter
    // It doesn't need to be exact, but if it is inexact, it should err towards being before the current location
    // rather than after the current location.  The erase head will erase up to, but not including the sample
    // frame returned by this function.  If it is too large a value, sound data that hasn't been played will be 
    // erased.

    // Change to return the real value
	const UInt32 div = DMA_PLAY_SLOTS * (32 / 8);
	UInt32 current_address = card->pci_dev->ioRead32(MT_DMA_PB_ADDRESS, card->mtbase);
	UInt32 diff = (current_address - ((UInt32) physicalAddressOutput)) / div;
    //IOLog("diff = %lu\n", diff);
		
	return diff;
}
    
IOReturn Envy24AudioEngine::performFormatChange(IOAudioStream *audioStream, const IOAudioStreamFormat *newFormat, const IOAudioSampleRate *newSampleRate)
{
    DBGPRINT("Envy24AudioEngine[%p]::peformFormatChange(%p, %p, %p)\n", this, audioStream, newFormat, newSampleRate);
    
	// A format change without a new rate must leave the hardware rate alone
	if (!newSampleRate)
	{
		return kIOReturnSuccess;
	}

	currentSampleRate = newSampleRate->whole;
	
	UInt32 FreqBits = lookUpFrequencyBits(currentSampleRate, Frequencies, FrequencyBits, FREQUENCIES, 0x08);
	card->pci_dev->ioWrite8(MT_SAMPLERATE, (UInt8) FreqBits, card->mtbase);
	IOLog("Freq = %x\n", FreqBits);

	// ALSA calls the codec set_rate_val after programming the rate register
	if (card->SubType == MAUDIO_DELTA66 || card->SubType == MAUDIO_DELTA66E)
	{
		Delta66_set_rate_val(card, currentSampleRate);
	}

	/*UInt32 SPDIFBits = lookUpFrequencyBits(currentSampleRate, SPDIF_Frequencies, SPDIF_FrequencyBits, SPDIF_FREQUENCIES, 1000);
	ClearMask8(card->pci_dev, card->iobase, CCS_SPDIF_CONFIG, CCS_SPDIF_INTEGRATED);
	if (SPDIFBits != 1000)
	{
		card->pci_dev->ioWrite16(MT_SPDIF_TRANSMIT, 0x04 | 1 << 5 | (SPDIFBits << 12), card->mtbase);
		WriteMask8(card->pci_dev, card->iobase, CCS_SPDIF_CONFIG, CCS_SPDIF_INTEGRATED);
		//IOLog("Enabled SPDIF %lu\n", SPDIFBits);
	}
	card->SPDIF_RateSupported = (SPDIFBits != 1000);*/
	
    return kIOReturnSuccess;
}


void Envy24AudioEngine::interruptHandler(OSObject * owner, IOInterruptEventSource* source, int /*count*/)
{
}

bool Envy24AudioEngine::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *source)
{
    Envy24AudioEngine *audioEngine = OSDynamicCast(Envy24AudioEngine, owner);

    // We've cast the audio engine from the owner which we passed in when we created the interrupt
    // event source
    if (audioEngine) {
        // Then, filterInterrupt() is called on the specified audio engine
		audioEngine->filterInterrupt(source->getIntIndex());
    }
    
    return false;
}

void Envy24AudioEngine::filterInterrupt(int index)
{
    // In the case of our simple device, we only get interrupts when the audio engine loops to the
    // beginning of the buffer.  When that happens, we need to take a timestamp and increment
    // the loop count.  The function takeTimeStamp() does both of those for us.  Additionally,
    // if a different timestamp is to be used (other than the current time), it can be passed
    // in to takeTimeStamp()

	UInt8 intreq;
	
	if ( ( intreq = card->pci_dev->ioRead8(CCS_INTR_STATUS, card->iobase) ) != 0 )
	{
	    card->pci_dev->ioWrite8(CCS_INTR_STATUS, intreq, card->iobase); // clear it
		
	    if (intreq & CCS_INTR_PRO_MACRO)
        {
		   UInt8 mtstatus = card->pci_dev->ioRead8(MT_INTR_MASK_STATUS, card->mtbase);
		   	   
		   card->pci_dev->ioWrite8(MT_INTR_MASK_STATUS, mtstatus, card->mtbase); // clear interrupt
		   
		   if (mtstatus & MT_PLAY_STATUS)
           {
	           takeTimeStamp();
		   }
        }
    }
			   
	return;
}


UInt32 Envy24AudioEngine::lookUpFrequencyBits(UInt32 Frequency,
												const UInt32* FreqList,
												const UInt32* FreqBitList,
												UInt32 ListSize,
												UInt32 Default)
{
	UInt32 FreqBit = Default;

	for (UInt32 i = 0; i < ListSize; i++)
	{
		if (FreqList[i] == Frequency)
		{
			return FreqBitList[i];
		}
	}

	return FreqBit;
}


void * Envy24AudioEngine::IOMallocContiguousWithLimit(vm_size_t size,
													  mach_vm_address_t physicalMask,
						                              IOPhysicalAddress * physicalAddress,
						                              IOBufferMemoryDescriptor ** descriptor,
						                              IODMACommand ** command)
{
	IOBufferMemoryDescriptor *bmd;
	IODMACommand *dma;
	IODMACommand::Segment64 segment;
	UInt64 offset = 0;
	UInt32 numSegments = 1;

	if (size == 0)
		return NULL;

	// With an IOMMU (VT-d) the buffer can be anywhere in RAM; the mask then only limits
	// the device address the mapper hands out. Without one, the buffer itself has to be
	// physically contiguous below the mask.
	bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
			kIOMemoryPhysicallyContiguous | kIODirectionInOut, size, physicalMask);
	if (!bmd)
	{
		IOLog("Failed to allocate memory within the first 256MB address space!\n");
		return NULL;
	}

	// The address the card must use: an IOMMU address if VT-d is active, else physical.
	// 28 address bits, one segment, as ALSA's DMA_BIT_MASK(28).
	dma = IODMACommand::withSpecification(kIODMACommandOutputHost64, 28, 0,
			IODMACommand::kMapped, 0, 1);
	if (!dma || dma->setMemoryDescriptor(bmd) != kIOReturnSuccess)
	{
		IOLog("Envy24: couldn't prepare DMA command\n");
		if (dma)
			dma->release();
		bmd->release();
		return NULL;
	}

	if (dma->gen64IOVMSegments(&offset, &segment, &numSegments) != kIOReturnSuccess ||
	    numSegments != 1 || segment.fLength < size ||
	    (segment.fIOVMAddr + size) > (physicalMask | 0x3) + 1)
	{
		IOLog("Envy24: DMA address unusable (%u segments, addr %llx, len %llx)\n",
		      numSegments, segment.fIOVMAddr, segment.fLength);
		dma->clearMemoryDescriptor();
		dma->release();
		bmd->release();
		return NULL;
	}

	*physicalAddress = segment.fIOVMAddr;
	*descriptor = bmd;
	*command = dma;

	IOLog("Envy24: DMA buffer %llx bytes, device address %llx, virt %p\n",
	      (unsigned long long) size, segment.fIOVMAddr, bmd->getBytesNoCopy());

	return bmd->getBytesNoCopy();
}


void Envy24AudioEngine::dumpRegisters()
{
    DBGPRINT("Envy24AudioEngine[%p]::dumpRegisters()\n", this);
	int i;
	
	DBGPRINT("config 0x60 = %x\n", card->pci_dev->configRead8(0x60));
	DBGPRINT("config 0x61 = %x\n", card->pci_dev->configRead8(0x61));
	DBGPRINT("config 0x62 = %x\n", card->pci_dev->configRead8(0x62));
	DBGPRINT("config 0x63 = %x\n", card->pci_dev->configRead8(0x63));
	
	DBGPRINT("---\n");
	for (i = 0; i <= 0x1F; i++)
	{
	  DBGPRINT("CCS %02x: %02x\n", i, card->pci_dev->ioRead8(i, card->iobase));
	}
	IOSleep(100);
	
	DBGPRINT("---\n");
	for (i = 0; i <= 0x31; i++)
	{
    	DBGPRINT("CCI %02x: %02x\n", i, ReadCCI(card, i));
	}
	IOSleep(100);
    
	DBGPRINT("---\n");
	for (i = 0; i <= 0x3F; i++)
	{
	  DBGPRINT("MT %02x: %02x\n", i, card->pci_dev->ioRead8(i, card->mtbase));
  	}
	
    IOSleep(1000);
}