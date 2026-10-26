#ifndef DELTA66_H
#define DELTA66_H

int Delta66_Init(struct CardData *card);
void Delta66_ak4524_lock(struct CardData *card, int chip);
void Delta66_ak4524_unlock(struct CardData *card, int chip);
void Delta66_set_rate_val(struct CardData *card, unsigned int rate);
void CreateParmsForDelta66(struct CardData *card);


// GPIO bits, from ALSA sound/pci/ice1712/delta.h

// shared by all Delta cards
#define DELTA66_DFS             0x01 // fast/slow sample rate mode, 1 above 48 kHz

// Delta 66 (CS8403/CS8414 S/PDIF)
#define DELTA66_SPDIF_IN_STAT   0x02 // input, 0 = valid S/PDIF signal
#define DELTA66_SPDIF_OUT_CLOCK 0x04 // CS8403 status clock
#define DELTA66_SPDIF_OUT_DATA  0x08 // CS8403 status data
#define DELTA66_CODEC_DATA      0x10 // AK4524 serial data
#define DELTA66_CODEC_CLOCK     0x20 // AK4524 serial clock
#define DELTA66_CODEC_CHIP_A    0x40
#define DELTA66_CODEC_CHIP_B    0x80

// Delta 66 rev E (CS8427 over SPI)
#define DELTA66E_CCLK           0x02 // SPI clock
#define DELTA66E_DIN            0x04 // SPI data input
#define DELTA66E_DOUT           0x08 // SPI data output
#define DELTA66E_CS_CS8427      0x10 // low = CS8427 selected
#define DELTA66E_CS_CHIP_A      0x20 // AK4524 #0
#define DELTA66E_CS_CHIP_B      0x40 // AK4524 #1

#endif
