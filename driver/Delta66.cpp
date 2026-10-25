#include "misc.h"
#include "DriverData.h"
#include "regs.h"
#include "ak_codec.h"
#include "Delta66.h"
#include <IOKit/IOLib.h>

// Board support for the M-Audio Delta 66 and Delta 66 rev E.
// Ported from ALSA sound/pci/ice1712/delta.c; see PORTING.md for the pin map.


static bool Delta66_IsRevE(struct CardData *card)
{
	return card->SubType == MAUDIO_DELTA66E;
}


// ALSA akm_delta44_priv / akm_delta66e_priv, with the chip select that
// delta_ak4524_lock() / delta66e_ak4524_lock() pick for each chip.
static void Delta66_SetupCodec(struct CardData *card, int chip)
{
	struct akm_codec *codec = &card->codec[chip];

	codec->caddr = 2;
	codec->cif = 0;

	if (Delta66_IsRevE(card))
	{
		codec->datamask = DELTA66E_DOUT;
		codec->clockmask = DELTA66E_CCLK;
		codec->csmask = (chip == 0) ? DELTA66E_CS_CHIP_A : DELTA66E_CS_CHIP_B;
	}
	else
	{
		codec->datamask = DELTA66_CODEC_DATA;
		codec->clockmask = DELTA66_CODEC_CLOCK;
		codec->csmask = (chip == 0) ? DELTA66_CODEC_CHIP_A : DELTA66_CODEC_CHIP_B;
	}

	codec->csaddr = codec->csmask;
	codec->csnone = 0;
	codec->addflags = 0;
	codec->totalmask = 0;
	codec->type = AKM4524;
	codec->newflag = 1;
}


// ALSA snd_ice1712_save_gpio_status(): keep GPIO direction and write mask.
void Delta66_ak4524_lock(struct CardData *card, int chip)
{
	card->SavedDir = ReadCCI(card, CCI_GPIO_DIR);
	card->SavedMask = ReadCCI(card, CCI_GPIO_MASK);
}


// ALSA snd_ice1712_restore_gpio_status(): put direction and write mask back.
// GPIO data is left alone, so DFS and chip selects keep their state.
void Delta66_ak4524_unlock(struct CardData *card, int chip)
{
	WriteCCI(card, CCI_GPIO_DIR, (unsigned char) card->SavedDir);
	WriteCCI(card, CCI_GPIO_MASK, (unsigned char) card->SavedMask);
}


static void Delta66_codec_write(struct CardData *card, int chip, unsigned char addr, unsigned char data)
{
	Delta66_ak4524_lock(card, chip);
	akm4xxx_write(card, &card->codec[chip], addr, data);
	Delta66_ak4524_unlock(card, chip);
}


// ALSA ak4524_reset(): hold the ADC/DAC in reset, or release them and
// restore the level registers 4-7.
static void Delta66_ak4524_reset(struct CardData *card, int state)
{
	for (int chip = 0; chip < 2; chip++)
	{
		Delta66_codec_write(card, chip, 0x01, state ? 0x00 : 0x03);
		if (state)
			continue;

		for (unsigned char reg = 0x04; reg < 0x08; reg++)
			Delta66_codec_write(card, chip, reg, card->codec[chip].regs[reg]);
	}
}


// ALSA delta_ak4524_set_rate_val(): DFS must be set above 48 kHz. The codecs
// are reset around the change, and only when DFS actually changes.
void Delta66_set_rate_val(struct CardData *card, unsigned int rate)
{
	unsigned char tmp, tmp2;

	if (rate == 0) // no hint, S/PDIF input is master
		return;

	tmp = ReadCCI(card, CCI_GPIO_DATA);
	tmp2 = tmp & ~DELTA66_DFS;
	if (rate > 48000)
		tmp2 |= DELTA66_DFS;
	if (tmp == tmp2)
		return;

	Delta66_ak4524_reset(card, 1);
	tmp = ReadCCI(card, CCI_GPIO_DATA) & ~DELTA66_DFS;
	if (rate > 48000)
		tmp |= DELTA66_DFS;
	WriteCCI(card, CCI_GPIO_DATA, tmp);
	Delta66_ak4524_reset(card, 0);
}


int Delta66_Init(struct CardData *card)
{
	unsigned char tmp;

	card->akm_type = AKM4524;
	card->Specific.NumChannels = 4;
	card->Specific.HasSPDIF = false; // digital I/O is not enabled in this pass

	/* ALSA snd_ice1712_delta_init(): initialize the SPI clock to high.
	   On a classic Delta 66 this pin is the S/PDIF input status (an input). */
	tmp = ReadCCI(card, CCI_GPIO_DATA);
	tmp |= DELTA66E_CCLK;

	/* rev E: the CS8427 shares DOUT/CCLK with the codecs. It is not set up
	   in this pass, so keep it deselected while the codecs are written. */
	if (Delta66_IsRevE(card))
		tmp |= DELTA66E_CS_CS8427;

	WriteCCI(card, CCI_GPIO_DATA, tmp);
	MicroDelay(5);

	for (int chip = 0; chip < 2; chip++)
	{
		Delta66_SetupCodec(card, chip);

		Delta66_ak4524_lock(card, chip);
		Init_akm4xxx(card, &card->codec[chip]);
		Delta66_ak4524_unlock(card, chip);
	}

	// The engine starts at 44.1 kHz; clear DFS if the EEPROM state left it set.
	Delta66_set_rate_val(card, 44100);

	IOLog("Envy24: Delta 66%s initialized, GPIO dir %02x mask %02x data %02x\n",
	      Delta66_IsRevE(card) ? " rev E" : "",
	      ReadCCI(card, CCI_GPIO_DIR), ReadCCI(card, CCI_GPIO_MASK), ReadCCI(card, CCI_GPIO_DATA));

	return 0;
}
