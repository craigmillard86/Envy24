#include "AudioDevice.h"

#include "AudioEngine.h"

#include <IOKit/audio/IOAudioControl.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioSelectorControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <IOKit/audio/IOAudioDefines.h>

#include <IOKit/IOLib.h>

#include <IOKit/pci/IOPCIDevice.h>
#include "misc.h"
#include "regs.h"
#include "Delta66.h"


#define super IOAudioDevice

OSDefineMetaClassAndStructors(Envy24AudioDevice, IOAudioDevice)

bool Envy24AudioDevice::initHardware(IOService *provider)
{
    bool result = false;
    
	//IOLog("Envy24AudioDevice[%p]::initHardware(%p)\n", this, provider);
	
    if (!super::initHardware(provider)) {
	    IOLog("Couldn't perform initHardware on superclass of Envy24AudioDevice!\n");
        goto Done;
    }
	
	card = new CardData;
	if (!card)
	{
	  IOLog("Couldn't allocate memory for CardData!\n");
	  IOSleep(3000);
	  goto Done;
	}
	
	card->pci_dev = OSDynamicCast(IOPCIDevice, provider);
	if (!card->pci_dev)
	{
	  IOLog("Couldn't get pci_dev!\n");
	  IOSleep(3000);
	  goto Done;
	}
	
	card->pci_dev->setIOEnable(true);
    card->pci_dev->setBusMasterEnable(true);
    

    // Config a map for the PCI config base registers
    // We need to keep this map around until we're done accessing the registers
    card->iobase = card->pci_dev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!card->iobase) {
	    IOLog("Couldn't assign iobase!\n");
        goto Done;
		}
	
	card->mtbase = card->pci_dev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress3);
    if (!card->mtbase) {
	    IOLog("Couldn't assign mtbase!\n");
        goto Done;
    }

	
	if (AllocDriverData(card->pci_dev, card) == NULL)
	{
	    IOLog("AllocDriverData() failed!\n");
		goto Done;
	}

    // The board is only known once card_init() has read the EEPROM
    if (card->SubType == MAUDIO_DELTA66 || card->SubType == MAUDIO_DELTA66E)
    {
        setDeviceName("M-Audio Delta 66");
        setDeviceShortName("Delta 66");
        setManufacturerName("M-Audio");
    }
    else
    {
        setDeviceName("Envy24");
        setDeviceShortName("Envy24");
        setManufacturerName("VIA/ICE");
    }

    if (!createAudioEngine()) {
        goto Done;
    }
    
    result = true;
    
Done:

    if (!result) {
	    IOLog("Something failed!\n");
		IOSleep(1000);
        if (card && card->iobase) {
            card->iobase->release();
            card->iobase = NULL;
        }
		if (card && card->mtbase) {
            card->mtbase->release();
            card->mtbase = NULL;
        }

		if (card)
		{
			FreeDriverData(card);
			delete card;
	    }
    }

	//IOLog("Envy24AudioDevice::initHardware returns %d\n", result);
	
    return result;
}

void Envy24AudioDevice::free()
{
    DBGPRINT("Envy24AudioDevice[%p]::free()\n", this);
    
	if (card)
	{
      if (card->iobase) {
          card->iobase->release();
          card->iobase = NULL;
      }
	
	  if (card->mtbase) {
          card->mtbase->release();
          card->mtbase = NULL;
      }
	  
	  FreeDriverData(card);
	
	  delete card;
	}
	
    
    super::free();
}


bool Envy24AudioDevice::createAudioEngine()
{
    bool result = false;
    Envy24AudioEngine *audioEngine = NULL;
    IOAudioControl *control;
	struct Parm *p = card->ParmList;
    
    DBGPRINT("Envy24AudioDevice[%p]::createAudioEngine()\n", this);
    
    audioEngine = new Envy24AudioEngine;
    if (!audioEngine) {
        goto Done;
    }
    
    // Init the new audio engine with the device registers so it can access them if necessary
    // The audio engine subclass could be defined to take any number of parameters for its
    // initialization - use it like a constructor
    if (!audioEngine->init(card)) {
        goto Done;
    }


    while (p != NULL)
    {
        control = IOAudioLevelControl::createVolumeControl(p->InitialValue,	// Initial value
                                                           p->MinValue,		// min value
                                                           p->MaxValue,		// max value
                                                           p->MindB,	// -144 in IOFixed (16.16)
                                                           p->MaxdB,		// max 0.0 in IOFixed
                                                           p->ChannelID,
                                                           p->Name,
                                                           p->ControlID,		// control ID - driver-defined,
                                                           p->Usage);
        if (!control) {
            IOLog("Failed to create control!\n");
            goto Done;
        }
        
        control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)volumeChangeHandler, this);
        audioEngine->addDefaultAudioControl(control);
        control->release();

        p = p->Next;
    }
    
    // Classic Delta 66: choose between the card's own clock and the S/PDIF input's
    if (card->SubType == MAUDIO_DELTA66 && card->Specific.HasSPDIF)
    {
        IOAudioSelectorControl *clock = IOAudioSelectorControl::create(0, kIOAudioControlChannelIDAll,
                                                                       "Clock Source", 100,
                                                                       kIOAudioSelectorControlSubTypeClockSource,
                                                                       kIOAudioControlUsageInput);
        if (!clock) {
            IOLog("Failed to create clock source control!\n");
            goto Done;
        }
        clock->addAvailableSelection(0, "Internal");
        clock->addAvailableSelection(1, "S/PDIF");
        clock->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)clockSourceChangeHandler, this);
        audioEngine->addDefaultAudioControl(clock);
        clock->release();
    }

    // Active the audio engine - this will cause the audio engine to have start() and initHardware() called on it
    // After this function returns, that audio engine should be ready to begin vending audio services to the system
    activateAudioEngine(audioEngine);
    // Once the audio engine has been activated, release it so that when the driver gets terminated,
    // it gets freed

    audioEngine->release();
    
    result = true;
    
Done:

    if (!result && (audioEngine != NULL)) {
        audioEngine->release();
    }

    return result;
}

// ALSA snd_ice1712_pro_internal_clock_put(): selecting S/PDIF sets MT_SPDIF_MASTER and
// tells the AK4524s there is no rate hint (set_rate_val(0)); selecting Internal
// rewrites the rate register without the master bit, as set_pro_rate() does.
IOReturn Envy24AudioDevice::clockSourceChangeHandler(IOService *target, IOAudioControl *control, SInt32 oldValue, SInt32 newValue)
{
    Envy24AudioDevice *dev = (Envy24AudioDevice *)target;
    struct CardData *card = dev ? dev->card : NULL;

    if (!card || (newValue != 0 && newValue != 1))
        return kIOReturnBadArgument;

    UInt8 rate = card->pci_dev->ioRead8(MT_SAMPLERATE, card->mtbase);
    card->Specific.SpdifMaster = (newValue == 1);
    if (newValue == 1)
    {
        card->pci_dev->ioWrite8(MT_SAMPLERATE, rate | MT_SPDIF_MASTER, card->mtbase);
        Delta66_set_rate_val(card, 0);
    }
    else
    {
        card->pci_dev->ioWrite8(MT_SAMPLERATE, rate & ~MT_SPDIF_MASTER, card->mtbase);
    }
    IOLog("Envy24: clock source %s\n", newValue ? "S/PDIF" : "internal");
    return kIOReturnSuccess;
}

IOReturn Envy24AudioDevice::volumeChangeHandler(IOService *target, IOAudioControl *volumeControl, SInt32 oldValue, SInt32 newValue)
{
    IOReturn result = kIOReturnBadArgument;
    Envy24AudioDevice *audioDevice;

    audioDevice = (Envy24AudioDevice *)target;
    if (audioDevice) {
        result = audioDevice->volumeChanged(volumeControl, oldValue, newValue);
    }
  
    return result;
}

IOReturn Envy24AudioDevice::volumeChanged(IOAudioControl *volumeControl, SInt32 oldValue, SInt32 newValue)
{
    //IOLog("Envy24AudioDevice[%p]::volumeChanged(%p, %ld, %ld)\n", this, volumeControl, oldValue, newValue);
    
    // Add hardware volume code change 
	if (oldValue != newValue) {
        struct Parm *p = card->ParmList;
        
        while (p != NULL) // look up parm
		{
            if (volumeControl->getControlID() == p->ControlID) 
            {
                unsigned char val = newValue;
                
                //IOLog("write reg %d, val %d\n", p->reg, val);
                if (p->conversion == PARM_AK_DATT || p->conversion == PARM_AK_ADC)
                {
                    // 0.5 dB steps, 0 = mute (ALSA ak4xxx-adda.c)
                    val = ak4524_level_register(newValue, p->conversion == PARM_AK_ADC);
                }
                else
                {
                if (p->reverse)
                {
                    val = p->MaxValue - val;
                }
                
                if (val <= p->MinValue)
                {
                    if (card->codec[p->codec].type == AKM4528 ||
                        card->codec[p->codec].type == AKM4524)
                    {
                        val = 0;
                    }
                }
                }
                akm4xxx_write(card, &(card->codec[p->codec]), p->reg, val);
                break;
            }
            
            p = p->Next;
        }
	}
    return kIOReturnSuccess;
}
    
IOReturn Envy24AudioDevice::outputMuteChangeHandler(IOService *target, IOAudioControl *muteControl, SInt32 oldValue, SInt32 newValue)
{
    IOReturn result = kIOReturnBadArgument;
    Envy24AudioDevice *audioDevice;
    
    audioDevice = (Envy24AudioDevice *)target;
    if (audioDevice) {
        result = audioDevice->outputMuteChanged(muteControl, oldValue, newValue);
    }

	return result;
}

IOReturn Envy24AudioDevice::outputMuteChanged(IOAudioControl *muteControl, SInt32 oldValue, SInt32 newValue)
{
    DBGPRINT("Envy24AudioDevice[%p]::outputMuteChanged(%p, %ld, %ld)\n", this, muteControl, oldValue, newValue);
    
	// Add output mute code here
	if (newValue) {

	} else {
	}

	return kIOReturnSuccess;
}

IOReturn Envy24AudioDevice::gainChangeHandler(IOService *target, IOAudioControl *gainControl, SInt32 oldValue, SInt32 newValue)
{
    IOReturn result = kIOReturnBadArgument;
    Envy24AudioDevice *audioDevice;
    
    audioDevice = (Envy24AudioDevice *)target;
    if (audioDevice) {
        result = audioDevice->gainChanged(gainControl, oldValue, newValue);
    }
    
    return result;
}

IOReturn Envy24AudioDevice::gainChanged(IOAudioControl *gainControl, SInt32 oldValue, SInt32 newValue)
{
    DBGPRINT("Envy24AudioDevice[%p]::gainChanged(%p, %ld, %ld)\n", this, gainControl, oldValue, newValue);
    
    if (gainControl) {
        DBGPRINT("\t-> Channel %ld\n", gainControl->getChannelID());
    }
    
    // Add hardware gain change code here 
    return kIOReturnSuccess;
}
    
IOReturn Envy24AudioDevice::inputMuteChangeHandler(IOService *target, IOAudioControl *muteControl, SInt32 oldValue, SInt32 newValue)
{
    IOReturn result = kIOReturnBadArgument;
    Envy24AudioDevice *audioDevice;
    
    audioDevice = (Envy24AudioDevice *)target;
    if (audioDevice) {
        result = audioDevice->inputMuteChanged(muteControl, oldValue, newValue);
    }
    
    return result;
}

IOReturn Envy24AudioDevice::inputMuteChanged(IOAudioControl *muteControl, SInt32 oldValue, SInt32 newValue)
{
    DBGPRINT("Envy24AudioDevice[%p]::inputMuteChanged(%p, %ld, %ld)\n", this, muteControl, oldValue, newValue);
    
    // Add input mute change code here
    return kIOReturnSuccess;
}


/*
 typedef enum _IOAudioDevicePowerState { 
 kIOAudioDeviceSleep = 0, // When sleeping 
 kIOAudioDeviceIdle = 1, // When no audio engines running 
 kIOAudioDeviceActive = 2 // audio engines running 
 } IOAudioDevicePowerState;  
 */

IOReturn Envy24AudioDevice::performPowerStateChange(IOAudioDevicePowerState oldPowerState, 
												    IOAudioDevicePowerState newPowerState, 
								 					UInt32 *microsecondsUntilComplete)
{
	IOLog("Envy24AudioDevice::performPowerStateChange!, old = %d, new = %d\n", oldPowerState, newPowerState);
	
	if (newPowerState == kIOAudioDeviceSleep) // go to sleep, power down and save settings
	{
		IOLog("Envy24AudioDevice::performPowerStateChange -> entering sleep\n");
	}
	else if (newPowerState != kIOAudioDeviceSleep &&
			 oldPowerState == kIOAudioDeviceSleep) // wake from sleep, power up and restore settings
	{
		IOLog("Envy24AudioDevice::performPowerStateChange -> waking up!\n");
		card_init(card);
	}
	
	return kIOReturnSuccess;
}


void Envy24AudioDevice::dumpRegisters()
{
    DBGPRINT("Envy24AudioDevice[%p]::dumpRegisters()\n", this);	
    int i;
	
	DBGPRINT("config 0x60 = %x\n", card->pci_dev->configRead8(0x60));
	DBGPRINT("config 0x61 = %x\n", card->pci_dev->configRead8(0x61));
	DBGPRINT("config 0x62 = %x\n", card->pci_dev->configRead8(0x62));
	DBGPRINT("config 0x63 = %x\n", card->pci_dev->configRead8(0x63));
	
	DBGPRINT("---\n");
	for (i = 0; i < 0x1E; i++)
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
