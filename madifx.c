/*
 *   ALSA driver for RME Hammerfall DSP MADI FX audio interface(s)
 *
 *      Based on hdspm.c
 *      Copyright (c) 2012-2015 Adrian Knoth <aknoth@google.com>
 *      Copyright (c) 2003 Winfried Ritsch (IEM)
 *      code based on hdsp.c   Paul Davis
 *                             Marcus Andersson
 *                             Thomas Charbonnel
 *                             Florian Faber
 *                             Adrian Knoth
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/math64.h>
#include <linux/io.h>
#include <linux/version.h>

#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/info.h>
#include <sound/asoundef.h>
#include <sound/rawmidi.h>
#include <sound/hwdep.h>
#include <sound/initval.h>

#include "madifx.h"

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX; /* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR; /* ID for this card */
/* Enable this card */
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for RME MADIFX interface.");

module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for RME MADIFX interface.");

module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable/disable specific MADIFX soundcards.");


MODULE_AUTHOR
(
	"Adrian Knoth <adi@drcomp.erfurt.thur.de>"
);
MODULE_DESCRIPTION("RME MADIFX");
MODULE_LICENSE("GPL");

/* --- Write registers. ---
  These are defined as byte-offsets from the iobase value.  */

#define MADIFX_CONTROL_REG	(0*4)
#define MADIFX_IRQ_ACK		(3*4)
#define MADIFX_FREQ_REG		(1*4)
#define MADIFX_SETTINGS_REG	(2*4)
#define MADIFX_START_LEVEL	(6*4)
#define MADIFX_midi_out0_data   (8*4)
#define MADIFX_midi_out1_data   (9*4)
#define MADIFX_midi_out2_data	(10*4)
#define MADIFX_midi_out3_data	(11*4)
#define MADIFX_ENABLE_OUTPUT	(64*4)
#define MADIFX_ENABLE_INPUT	(96*4)
#define MADIFX_MIXER_LIST_VOL	(16384*4)
#define MADIFX_MIXER_LIST_CH	(20480*4)
#define MADIFX_WR_OUTPUT_GAIN	((24576+256)*4)

#define MADIFX_SAMPLE_FRAMES_PER_BUFFER		8192

#define MADIFX_PAGE_ADDRESS_LIST   (8192*4)

/* page table size in entries, multiply by 4 to get byte offset */
#define MADIFX_MAX_PAGE_TABLE_SIZE	4096
#define MADIFX_LPTI_HMFX    (MADIFX_MAX_PAGE_TABLE_SIZE/2+25*32768*8/4096)
#define MADIFX_LPTI_MFXT    (MADIFX_MAX_PAGE_TABLE_SIZE/2+26*32768*8/4096)

#define HDSPM_MADI_mixerBase    32768	/* 32768-65535 for 2x64x64 Fader */

#define HDSPM_MATRIX_MIXER_SIZE  8192	/* = 2*64*64 * 4 Byte => 32kB */

/* --- Read registers. ---
   These are defined as byte-offsets from the iobase value */

#define MADIFX_RD_STATUS	(0*4)
#define MADIFX_RD_INP_STATUS	(1*4)
#define MADIFX_RD_INP_FREQ	(2*4)
#define MADIFX_RD_PLL_FREQ	(3*4)
/* MADIFX_RD_VERSION is encoded as
 * card_type(7..0) & "0000" & build(19..0)
 */
#define MADIFX_RD_VERSION	(4*4)
#define MADIFX_RD_FLASH		(5*4)
#define MADIFX_RD_BARCODE0	(6*4)
#define MADIFX_RD_BARCODE1	(7*4)
#define MADIFX_RD_DSP_DATA	(8*4)
#define MADIFX_RD_DSP_STATUS	(9*4)
#define MADIFX_midi_in0_data	(12*4)
#define MADIFX_midi_in1_data	(13*4)
#define MADIFX_midi_in2_data	(14*4)
#define MADIFX_midi_in3_data	(15*4)
#define MADIFX_midi_out0_status	(16*4)
#define MADIFX_midi_out1_status	(17*4)
#define MADIFX_midi_out2_status	(18*4)
#define MADIFX_midi_out3_status	(19*4)
#define MADIFX_midi_in0_status	(20*4)
#define MADIFX_midi_in1_status	(21*4)
#define MADIFX_midi_in2_status	(22*4)
#define MADIFX_midi_in3_status	(23*4)

/* input status */

#define MADIFX_madi1_lock	0x0001
#define MADIFX_madi2_lock	0x0002
#define MADIFX_madi3_lock	0x0004
#define MADIFX_aes_lock		0x0008
#define MADIFX_word_lock	0x0010
#define MADIFX_madi1_sync	0x0020
#define MADIFX_madi2_sync	0x0040
#define MADIFX_madi3_sync	0x0080
#define MADIFX_word_sync	0x0100
#define MADIFX_aes_sync		0x0200
#define MADIFX_madi1_rx_64ch	0x0400
#define MADIFX_madi2_rx_64ch	0x0800
#define MADIFX_madi3_rx_64ch	0x1000
#define MADIFX_SelSyncRef0	0x2000
#define MADIFX_SelSyncRef1	0x4000
#define MADIFX_SelSyncRef2	0x8000
#define MADIFX_MADIInput0	0x10000
#define MADIFX_MADIInput1	0x20000
#define MADIFX_redundancy_rb	0x40000
#define MADIFX_mirror_out_rb	0x80000
#define MADIFX_sync_in_lock	0x100000
#define MADIFX_sync_in_sync	0x200000

/* control register bits */

#define MADIFX_START		0x00000001
#define MADIFX_freq0		0x00000002
#define MADIFX_freq1		0x00000004
#define MADIFX_freq2		0x00000008
#define MADIFX_freq3		0x00000010
#define MADIFX_BUF_SIZ_0	0x00000020
#define MADIFX_BUF_SIZ_1	0x00000040
#define MADIFX_BUF_SIZ_2	0x00000080
#define MADIFX_LAT_0		0x00000100
#define MADIFX_LAT_1		0x00000200
#define MADIFX_LAT_2		0x00000400
#define MADIFX_LAT_3		0x00000800
#define MADIFX_IE_AUDIO		0x00001000
#define MADIFX_IEN0		0x00002000
#define MADIFX_IEN1		0x00004000
#define MADIFX_IEN2		0x00008000
#define MADIFX_IEN3		0x00010000
#define MADIFX_float_format	0x00020000
#define MADIFX_CLR_TMS		0x00040000
#define MADIFX_Dolby		0x00080000

#define MADIFX_kFrequencyMask \
		(MADIFX_freq0 + MADIFX_freq1 + MADIFX_freq2 + MADIFX_freq3)
#define MADIFX_kBufferPositionMask	0xFFF0

enum {
	MADIFX_kFrequency32kHz	  = 0,
	MADIFX_kFrequency44_1kHz  = MADIFX_freq0,
	MADIFX_kFrequency48kHz    = MADIFX_freq1,
	MADIFX_kFrequency64kHz	  = MADIFX_freq2 + 0,
	MADIFX_kFrequency88_2kHz  = MADIFX_freq2 + MADIFX_freq0,
	MADIFX_kFrequency96kHz	  = MADIFX_freq2 + MADIFX_freq1,
	MADIFX_kFrequency128kHz	  = MADIFX_freq3 + MADIFX_freq2 + 0,
	MADIFX_kFrequency176_4kHz = MADIFX_freq3 + MADIFX_freq2 + MADIFX_freq0,
	MADIFX_kFrequency192kHz	  = MADIFX_freq3 + MADIFX_freq2 + MADIFX_freq1
};

/* settings register bits */

#define MADIFX_SyncRef0		0x00000001
#define MADIFX_SyncRef1		0x00000002
#define MADIFX_SyncRef2		0x00000004
#define MADIFX_PRO		0x00000008
#define MADIFX_DSP_EN		0x00000010
#define MADIFX_WCK_TERM		0x00000020
#define MADIFX_WCK48		0x00000040
#define MADIFX_madi1_tx_64ch	0x00000080
#define MADIFX_madi2_tx_64ch	0x00000100
#define MADIFX_madi3_tx_64ch	0x00000200
#define MADIFX_madi1_smux	0x00000400
#define MADIFX_madi2_smux	0x00000800
#define MADIFX_madi3_smux	0x00001000
#define MADIFX_redundancy_mode	0x00002000
#define MADIFX_mirror_madi_out	0x00004000

#define MADIFX_SyncRefMask (MADIFX_SyncRef0 | MADIFX_SyncRef1 | MADIFX_SyncRef2)

/* input freq register bits */

#define MADIFX_madi1_freq0	0x00001
#define MADIFX_madi1_freq1	0x00002
#define MADIFX_madi1_freq2	0x00004
#define MADIFX_madi1_freq3	0x00008
#define MADIFX_madi2_freq0	0x00010
#define MADIFX_madi2_freq1	0x00020
#define MADIFX_madi2_freq2	0x00040
#define MADIFX_madi2_freq3	0x00080
#define MADIFX_madi3_freq0	0x00100
#define MADIFX_madi3_freq1	0x00200
#define MADIFX_madi3_freq2	0x00400
#define MADIFX_madi3_freq3	0x00800
#define MADIFX_aes_freq0	0x01000
#define MADIFX_aes_freq1	0x02000
#define MADIFX_aes_freq2	0x04000
#define MADIFX_aes_freq3	0x08000
#define MADIFX_word_freq0	0x10000
#define MADIFX_word_freq1	0x20000
#define MADIFX_word_freq2	0x40000
#define MADIFX_word_freq3	0x80000
#define MADIFX_sync_in_freq0	0x100000
#define MADIFX_sync_in_freq1	0x200000
#define MADIFX_sync_in_freq2	0x400000
#define MADIFX_sync_in_freq3	0x800000

/* Index to DMA level buffer in uint32_t units */
#define MADIFX_RD_RMS_IN	(0*1)
#define MADIFX_RD_PEAK_IN	(512*1)
#define MADIFX_RD_RMS_PLAY	(1024*1)
#define MADIFX_RD_PEAK_PLAY	(1536*1)
#define MADIFX_RD_RMS_OUT	(2048*1)
#define MADIFX_RD_PEAK_OUT	(2560*1)
#define MADIFX_RD_RMS_IN_PRE	(3072*1)
#define MADIFX_RD_PEAK_IN_PRE	(3584*1)
#define MADIFX_RD_RMS_OUT_PRE	(4096*1)
#define MADIFX_RD_PEAK_OUT_PRE	(4608*1)

/* MADIFX MIDI Interrupt enable */
#define MADIFX_IEN0		0x00002000
#define MADIFX_IEN1		0x00004000
#define MADIFX_IEN2		0x00008000
#define MADIFX_IEN3		0x00010000

/* status register, MIDI IRQ Pending */
#define MADIFX_mIRQ0		0x10000000
#define MADIFX_mIRQ1		0x20000000
#define MADIFX_mIRQ2		0x40000000
#define MADIFX_mIRQ3		0x80000000

/* --- bit helper defines */
#define MADIFX_LatencyMask (MADIFX_LAT_0|MADIFX_LAT_1|MADIFX_LAT_2|MADIFX_LAT_3)

#define madifx_encode_latency(x)       (((x)<<8) & MADIFX_LatencyMask)
#define madifx_decode_latency(x)       ((((x) & MADIFX_LatencyMask)>>8))

/* speemode is enum 0,1,2 for ss/ds/qs, so (1<<speedmode) returns 1, 2, 4. */
#define madifx_speed_multiplier(x)	(1<<(x)->speedmode)


#define HDSPM_audioIRQPending    (1<<0)	/* IRQ is high and pending */



/* Mixer Values */
#define UNITY_GAIN          32768	/* = 65536/2 */
#define MINUS_INFINITY_GAIN 0

/* Number of channels for different Speed Modes */
#define MADIFX_SS_IN_CHANNELS       194
#define MADIFX_DS_IN_CHANNELS       98
#define MADIFX_QS_IN_CHANNELS       50

#define MADIFX_SS_OUT_CHANNELS       196
#define MADIFX_DS_OUT_CHANNELS       100
#define MADIFX_QS_OUT_CHANNELS       52


#define HDSPM_MADIFX_REV	213

/* speed factor modes */
#define HDSPM_SPEED_SINGLE 0
#define HDSPM_SPEED_DOUBLE 1
#define HDSPM_SPEED_QUAD   2

/* DMA buffers in byte; 8192 samples per channel, each 4 bytes wide */
#define NUM_INPUTS_S_MFXT	(64*3+4)
#define NUM_OUTPUTS_S_MFXT	(64*3+6)
#define INPUT_DMA_BUFFER_SIZE (NUM_INPUTS_S_MFXT*32768)
#define OUTPUT_DMA_BUFFER_SIZE (NUM_OUTPUTS_S_MFXT*32768)

/* Per-stream DMA page counts and sizes */
#define MADIFX_HW_PAGE_SIZE  4096
#define NUM_AES_PAGES        (32768*2/MADIFX_HW_PAGE_SIZE)   /* 16: 2ch */
#define NUM_DMA_CH_PAGES     (32768*8/MADIFX_HW_PAGE_SIZE)   /* 64: 8ch slot */
#define NUM_MADI_PAGES       (32768*192/MADIFX_HW_PAGE_SIZE) /* 1536: all MADI */

/* PCM device (group) types */
#define MADIFX_PCM_TYPE_AES    0
#define MADIFX_PCM_TYPE_MADI   1
#define MADIFX_PCM_TYPE_PHONES 2

/* 24 MADI streams (3 ports × 8 streams of 8ch each) + AES + Phones */
#define MADIFX_NUM_MADI_STREAMS 24
#define MADIFX_NUM_PCM_GROUPS   26

/* Output page-table base indices (within first 2048 entries) */
#define MADIFX_PT_OUT_AES        0
#define MADIFX_PT_OUT_PHONES     (1 * NUM_DMA_CH_PAGES)
#define MADIFX_PT_OUT_MADI(n)    ((2 + (n)) * NUM_DMA_CH_PAGES)

/* Input page-table base indices (within second 2048 entries) */
#define MADIFX_PT_IN_BASE        (MADIFX_MAX_PAGE_TABLE_SIZE / 2)
#define MADIFX_PT_IN_AES         MADIFX_PT_IN_BASE
#define MADIFX_PT_IN_MADI(n)     (MADIFX_PT_IN_BASE + NUM_DMA_CH_PAGES + (n) * NUM_DMA_CH_PAGES)

/* DMA buffer size per group per direction */
#define MADIFX_DMA_SIZE_2CH  (2 * 32768)
#define MADIFX_DMA_SIZE_8CH  (8 * 32768)

/* PCI Vendor id for HDSPe MADI FX (not found in linux headers) */
#define PCI_VENDOR_ID_RME_MADI_FX 0x1d18

/* names for speed modes */
static char *madifx_speed_names[] = { "single", "double", "quad" };

static const char *const texts_madifx_clock_source[] = {
	"Internal",
	"Word Clock", /* OSX driver has first AES, then WC, but real HW is
			 different */
	"AES In",
	"MADI 1 In",
	"MADI 2 In",
	"MADI 3 In",
	"Sync In"
};


static const char *const texts_freq[] = {
	"No Lock",
	"32 kHz",
	"44.1 kHz",
	"48 kHz",
	"64 kHz",
	"88.2 kHz",
	"96 kHz",
	"128 kHz",
	"176.4 kHz",
	"192 kHz"
};


/* Per-PCM-device state. Each group is one ALSA PCM device. */
struct madifx_pcm_group {
	struct mfx *mfx;
	int type;          /* MADIFX_PCM_TYPE_* */
	int madi_stream;   /* MADI only: 0..23 */
	int n_channels;    /* 2 for AES/Phones, 8 for MADI */
	struct snd_pcm *pcm;
	struct snd_pcm_substream *playback_substream;
	struct snd_pcm_substream *capture_substream;
	pid_t playback_pid;
	pid_t capture_pid;
};

struct madifx_midi {
	struct mfx *mfx;
	int id;
	struct snd_rawmidi *rmidi;
	struct snd_rawmidi_substream *input;
	struct snd_rawmidi_substream *output;
	char istimer;		/* timer in use */
	struct timer_list timer;
	spinlock_t lock;
	int pending;
	int dataIn;
	int statusIn;
	int dataOut;
	int statusOut;
	int ie;
	int irq;
};


struct mfx {
	spinlock_t lock;

	char *card_name;	     /* for procinfo */
	unsigned short firmware_rev;

	uint8_t io_type;

	int monitor_outs;	/* set up monitoring outs init flag */

	u32 control_register;	/* cached value */
	u32 control2_register;	/* cached value */
	u32 settings_register;

	struct madifx_midi midi[4];
	struct work_struct midi_work;

	size_t period_bytes;
	unsigned char ss_in_channels;
	unsigned char ds_in_channels;
	unsigned char qs_in_channels;
	unsigned char ss_out_channels;
	unsigned char ds_out_channels;
	unsigned char qs_out_channels;

	unsigned char max_channels_in;
	unsigned char max_channels_out;

	char **port_names_in;
	char **port_names_out;

	char **port_names_in_ss, **port_names_in_ds, **port_names_in_qs;
	char **port_names_out_ss, **port_names_out_ds, **port_names_out_qs;

	u32 *level_buffer;	/* suitably aligned address */

	/* running_count tracks total active streams across all groups */
	int running_count;

	int last_external_sample_rate;	/* samplerate mystic ... */
	int last_internal_sample_rate;
	int system_sample_rate;

	int dev;		/* Hardware vars... */
	int irq;
	unsigned long port;
	void __iomem *iobase;

	int irq_count;		/* for debug */
	int midiPorts;

	struct snd_card *card;	/* one card */
	struct snd_hwdep *hwdep;	/* and a hwdep for additional ioctl */
	struct pci_dev *pci;	/* and an pci info */

	/* PCM groups: one ALSA PCM device per 8-channel slice */
	struct madifx_pcm_group groups[MADIFX_NUM_PCM_GROUPS];

	/* Dummy DMA page: safe target for unused page-table entries */
	void *dummy_dma_area;
	dma_addr_t dummy_dma_addr;

	/* Mixer vars */
	/* full mixer accessible over mixer ioctl or hwdep-device */
	struct madifx_newmixer *newmixer;
	dma_addr_t *dmaPageTable;

	const char *const *texts_clocksource;
	int texts_clocksource_items;

	cycles_t last_interrupt;

	unsigned int serial;

	int speedmode;

#ifdef CONFIG_SND_MADIFX_BROKEN
	struct snd_dma_buffer dmaLevelBuffer;
	struct madifx_level_buffer peak_rms;
#endif

};


static const struct pci_device_id snd_madifx_ids[] = {
	{
	 .vendor = PCI_VENDOR_ID_XILINX,
	 .device = 0x3fc7,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	 .class = 0,
	 .class_mask = 0,
	 .driver_data = 0},
	 {
	 .vendor = PCI_VENDOR_ID_RME_MADI_FX,
	 .device = 0x3fc7,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	 .class = 0,
	 .class_mask = 0,
	 .driver_data = 0},
	{0,}
};

MODULE_DEVICE_TABLE(pci, snd_madifx_ids);

/* prototypes */
static int snd_madifx_create_alsa_devices(struct snd_card *card,
						   struct mfx *mfx);
static int snd_madifx_create_pcm(struct snd_card *card,
					  struct mfx *mfx);

static inline void snd_madifx_initialize_midi_flush(struct mfx *mfx);
static int madifx_external_freq_index(struct mfx *mfx,
				      enum madifx_syncsource port);
static int madifx_get_clock_select(struct mfx *mfx);
static int snd_madifx_set_defaults(struct mfx *mfx);
static int madifx_system_clock_mode(struct mfx *mfx);

static inline int HDSPM_bit2freq(int n)
{
	static const int bit2freq_tab[] = {
		0, 32000, 44100, 48000, 64000, 88200,
		96000, 128000, 176400, 192000 };
	if (n < 1 || n > 9)
		return 0;
	return bit2freq_tab[n];
}

/* Write/read to/from HDSPM with Adresses in Bytes
   not words but only 32Bit writes are allowed */

static inline void madifx_write(struct mfx *mfx, unsigned int reg,
			       unsigned int val)
{
	writel(val, mfx->iobase + reg);
}

static inline unsigned int madifx_read(struct mfx *mfx, unsigned int reg)
{
	return readl(mfx->iobase + reg);
}

/* enable DMA for specific channels, now available for DSP-MADI */
static inline void snd_madifx_enable_in(struct mfx *mfx, int i, int v)
{
	madifx_write(mfx, MADIFX_ENABLE_INPUT + (4 * i), v);
}

static inline void snd_madifx_enable_out(struct mfx *mfx, int i, int v)
{
	madifx_write(mfx, MADIFX_ENABLE_OUTPUT + (4 * i), v);
}

/* Returns 1 if at most one process has open streams across all groups. */
static int snd_madifx_use_is_exclusive(struct mfx *mfx)
{
	unsigned long flags;
	pid_t first_pid = -1;
	int i, ret = 1;

	spin_lock_irqsave(&mfx->lock, flags);
	for (i = 0; i < MADIFX_NUM_PCM_GROUPS; i++) {
		pid_t pp = mfx->groups[i].playback_pid;
		pid_t cp = mfx->groups[i].capture_pid;
		if (pp >= 0) {
			if (first_pid < 0)
				first_pid = pp;
			else if (first_pid != pp) {
				ret = 0;
				break;
			}
		}
		if (cp >= 0) {
			if (first_pid < 0)
				first_pid = cp;
			else if (first_pid != cp) {
				ret = 0;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&mfx->lock, flags);
	return ret;
}

/* Returns true if any group has an open stream. */
static bool madifx_any_stream_open(struct mfx *mfx)
{
	int i;
	for (i = 0; i < MADIFX_NUM_PCM_GROUPS; i++) {
		if (mfx->groups[i].capture_pid >= 0 ||
		    mfx->groups[i].playback_pid >= 0)
			return true;
	}
	return false;
}


/* return latency in samples per period */
static int madifx_get_latency(struct mfx *mfx)
{
	int n;

	n = madifx_decode_latency(mfx->control_register);

	return 1 << (n + 5);
}

/* Latency function */
static inline void madifx_compute_period_size(struct mfx *mfx)
{
	mfx->period_bytes = 4 * madifx_get_latency(mfx);
}


/* position of the hardware pointer in the buffer */
static snd_pcm_uframes_t madifx_hw_pointer(struct mfx *mfx)
{
	u32 position;

	position = madifx_read(mfx, MADIFX_RD_STATUS);

	position &= MADIFX_kBufferPositionMask;
	position >>= 4;
	position *= 4;
#if 0
	position -= 4; /* safety offset */
#endif
	position &= (MADIFX_SAMPLE_FRAMES_PER_BUFFER-1);

	return position;
}

static inline void madifx_start_audio(struct mfx *s)
{
	s->control_register |= (MADIFX_IE_AUDIO | MADIFX_START);
	madifx_write(s, MADIFX_CONTROL_REG, s->control_register);
}

static inline void madifx_stop_audio(struct mfx *s)
{
	s->control_register &= ~(MADIFX_START | MADIFX_IE_AUDIO);
	madifx_write(s, MADIFX_CONTROL_REG, s->control_register);
}

static void madifx_silence_group_playback(struct madifx_pcm_group *group)
{
	struct snd_pcm_substream *ps = group->playback_substream;

	if (ps && ps->runtime && ps->runtime->dma_area)
		memset(ps->runtime->dma_area, 0,
		       ps->runtime->dma_bytes);
}

static int madifx_set_interrupt_interval(struct mfx *s, unsigned int frames)
{
	int n;

	spin_lock_irq(&s->lock);

	/* FIXME: We have four bits, but we don't know the mapping to frames,
	 * yet.
	 * LAT_3 is 2048
	 * LAT_2 is 128
	 * LAT_1 is 32
	 * LAT_0 is 16
	 *
	 * 2^(n+4), encode n to 4 bits
	 */
	n = 0;
	frames >>= 6;
	while (frames) {
		n++;
		frames >>= 1;
	}

	s->control_register &= ~MADIFX_LatencyMask;
	s->control_register |= madifx_encode_latency(n);

	madifx_write(s, MADIFX_CONTROL_REG, s->control_register);

	madifx_compute_period_size(s);

	spin_unlock_irq(&s->lock);

	return 0;
}

static u64 madifx_calc_dds_value(struct mfx *mfx, u64 period)
{
	u64 freq_const;

	if (period == 0)
		return 0;

	switch (mfx->io_type) {
	case MADIFX:
		freq_const = 131072000000000ULL;
		break;
	default:
		snd_BUG();
		return 0;
	}

	return div_u64(freq_const, period);
}


static void madifx_set_dds_value(struct mfx *mfx, int rate)
{
	u64 n;

	if (rate >= 112000)
		rate /= 4;
	else if (rate >= 56000)
		rate /= 2;

	switch (mfx->io_type) {
	case MADIFX:
		n = 131072000000000ULL;  /* 125 MHz */
		break;
	default:
		snd_BUG();
		return;
	}

	n = div_u64(n, rate);
	/* n should be less than 2^32 for being written to FREQ register */
	snd_BUG_ON(n >> 32);
	madifx_write(mfx, MADIFX_FREQ_REG, (u32)n);
}

static int madifx_get_external_rate(struct mfx *mfx)
{
	int current_clock = madifx_get_clock_select(mfx);

	/* map to enum madifx_syncsource */
	switch (current_clock) {
	case 0:
	case 6:
		/* Master or Sync-In */
		break;
	case 1:
		current_clock = syncsource_wc;
		break;
	case 2:
		current_clock = syncsource_aes;
		break;
	case 3:
	case 4:
	case 5:
		/* MADI1 == 3, MADI2 == 4, MADI3 == 5 map to
		 * MADI1 == 0, MADI2 = 1, MADI3 == 2
		 */
		current_clock -= 3;
		break;
	default:
		dev_err(mfx->card->dev,
				"MADIFX: Unknown clock source\n");
		return 0;
	}



	return HDSPM_bit2freq(madifx_external_freq_index(mfx, current_clock));
}

/* dummy set rate lets see what happens */
static int madifx_set_rate(struct mfx *mfx, int rate, int called_internally)
{
	int current_rate;
	int rate_bits;
	int not_set = 0;
	int current_speed, target_speed;

	/* ASSUMPTION: mfx->lock is either set, or there is no need for
	   it (e.g. during module initialization).
	 */

	if (1 == madifx_system_clock_mode(mfx)) {

		/* SLAVE --- */
		if (called_internally) {

			/* request from ctl or card initialization
			   just make a warning an remember setting
			   for future master mode switching */

			dev_warn(mfx->card->dev,
				"MADIFX: Warning: device is not running as a clock master.\n");
			not_set = 1;
		} else {
			int external_freq = madifx_get_external_rate(mfx);



			if (rate != external_freq) {
				dev_warn(mfx->card->dev,
				    "MADIFX: Warning: Requested rate %d doesn't match external rate %d\n",
				    rate, external_freq);
				not_set = 1;
			}
		}
	}

	current_rate = mfx->system_sample_rate;

	/* Changing between Singe, Double and Quad speed is not
	   allowed if any substreams are open. This is because such a change
	   causes a shift in the location of the DMA buffers and a reduction
	   in the number of available buffers.

	   Note that a similar but essentially insoluble problem exists for
	   externally-driven rate changes. All we can do is to flag rate
	   changes in the read/write routines.
	 */

	if (current_rate <= 56000)
		current_speed = HDSPM_SPEED_SINGLE;
	else if (current_rate <= 96000)
		current_speed = HDSPM_SPEED_DOUBLE;
	else
		current_speed = HDSPM_SPEED_QUAD;

	if (rate <= 48000)
		target_speed = HDSPM_SPEED_SINGLE;
	else if (rate <= 112000)
		target_speed = HDSPM_SPEED_DOUBLE;
	else
		target_speed = HDSPM_SPEED_QUAD;

	switch (rate) {
	case 32000:
		rate_bits = MADIFX_kFrequency32kHz;
		break;
	case 44100:
		rate_bits = MADIFX_kFrequency44_1kHz;
		break;
	case 48000:
		rate_bits = MADIFX_kFrequency48kHz;
		break;
	case 64000:
		rate_bits = MADIFX_kFrequency64kHz;
		break;
	case 88200:
		rate_bits = MADIFX_kFrequency88_2kHz;
		break;
	case 96000:
		rate_bits = MADIFX_kFrequency96kHz;
		break;
	case 128000:
		rate_bits = MADIFX_kFrequency128kHz;
		break;
	case 176400:
		rate_bits = MADIFX_kFrequency176_4kHz;
		break;
	case 192000:
		rate_bits = MADIFX_kFrequency192kHz;
		break;
	default:
		return -EINVAL;
	}

	if (current_speed != target_speed && madifx_any_stream_open(mfx)) {
		dev_err
		    (mfx->card->dev,
		     "MADIFX: cannot change from %s speed to %s speed mode (streams active)\n",
		     madifx_speed_names[current_speed],
		     madifx_speed_names[target_speed]);
		return -EBUSY;
	}

	madifx_set_dds_value(mfx, rate);

	mfx->control_register &= ~MADIFX_kFrequencyMask;
	mfx->control_register |= rate_bits;
	madifx_write(mfx, MADIFX_CONTROL_REG, mfx->control_register);



	mfx->system_sample_rate = rate;

	if (rate <= 56000) {
		mfx->max_channels_in = mfx->ss_in_channels;
		mfx->max_channels_out = mfx->ss_out_channels;
		mfx->port_names_in = mfx->port_names_in_ss;
		mfx->port_names_out = mfx->port_names_out_ss;
		mfx->speedmode = ss;
	} else if (rate <= 112000) {
		mfx->max_channels_in = mfx->ds_in_channels;
		mfx->max_channels_out = mfx->ds_out_channels;
		mfx->port_names_in = mfx->port_names_in_ds;
		mfx->port_names_out = mfx->port_names_out_ds;
		mfx->speedmode = ds;
	} else {
		mfx->max_channels_in = mfx->qs_in_channels;
		mfx->max_channels_out = mfx->qs_out_channels;
		mfx->port_names_in = mfx->port_names_in_qs;
		mfx->port_names_out = mfx->port_names_out_qs;
		mfx->speedmode = qs;
	}

	if (not_set != 0)
		return -1;

	return 0;
}

/*----------------------------------------------------------------------------
   MIDI
  ----------------------------------------------------------------------------*/

static inline unsigned char snd_madifx_midi_read_byte(struct mfx *mfx,
						      int id)
{
	/* the hardware already does the relevant bit-mask with 0xff */
	return madifx_read(mfx, mfx->midi[id].dataIn);
}

static inline void snd_madifx_midi_write_byte(struct mfx *mfx, int id,
					      int val)
{
	/* the hardware already does the relevant bit-mask with 0xff */
	return madifx_write(mfx, mfx->midi[id].dataOut, val);
}

static inline int snd_madifx_midi_input_available(struct mfx *mfx, int id)
{
	return madifx_read(mfx, mfx->midi[id].statusIn) & 0xFF;
}

static inline int snd_madifx_midi_output_possible(struct mfx *mfx, int id)
{
	int fifo_bytes_used;

	fifo_bytes_used = madifx_read(mfx, mfx->midi[id].statusOut) & 0xFF;

	if (fifo_bytes_used < 128)
		return  128 - fifo_bytes_used;
	else
		return 0;
}

static void snd_madifx_flush_midi_input(struct mfx *mfx, int id)
{
	while (snd_madifx_midi_input_available(mfx, id))
		snd_madifx_midi_read_byte(mfx, id);
}

static int snd_madifx_midi_output_write(struct madifx_midi *hmidi)
{
	unsigned long flags;
	int n_pending;
	int to_write;
	int i;
	unsigned char buf[128];

	/* Output is not interrupt driven */

	spin_lock_irqsave(&hmidi->lock, flags);
	if (hmidi->output &&
	    !snd_rawmidi_transmit_empty(hmidi->output)) {
		n_pending = snd_madifx_midi_output_possible(hmidi->mfx,
							    hmidi->id);
		if (n_pending > 0) {
			if (n_pending > (int)sizeof(buf))
				n_pending = sizeof(buf);

			to_write = snd_rawmidi_transmit(hmidi->output, buf,
							 n_pending);
			if (to_write > 0) {
				for (i = 0; i < to_write; ++i)
					snd_madifx_midi_write_byte(hmidi->mfx,
								   hmidi->id,
								   buf[i]);
			}
		}
	}
	spin_unlock_irqrestore(&hmidi->lock, flags);
	return 0;
}

static int snd_madifx_midi_input_read(struct madifx_midi *hmidi)
{
	unsigned char buf[128]; /* this buffer is designed to match the MIDI
				 * input FIFO size
				 */
	unsigned long flags;
	int n_pending;
	int i;

	spin_lock_irqsave(&hmidi->lock, flags);
	n_pending = snd_madifx_midi_input_available(hmidi->mfx, hmidi->id);
	if (n_pending > 0) {
		if (hmidi->input) {
			if (n_pending > (int)sizeof(buf))
				n_pending = sizeof(buf);
			for (i = 0; i < n_pending; ++i)
				buf[i] = snd_madifx_midi_read_byte(hmidi->mfx,
								   hmidi->id);
			if (n_pending)
				snd_rawmidi_receive(hmidi->input, buf,
						     n_pending);
		} else {
			/* flush the MIDI input FIFO */
			while (n_pending--)
				snd_madifx_midi_read_byte(hmidi->mfx,
							  hmidi->id);
		}
	}
	hmidi->pending = 0;
	spin_unlock_irqrestore(&hmidi->lock, flags);

	spin_lock_irqsave(&hmidi->mfx->lock, flags);
	hmidi->mfx->control_register |= hmidi->ie;
	madifx_write(hmidi->mfx, MADIFX_CONTROL_REG,
		    hmidi->mfx->control_register);
	spin_unlock_irqrestore(&hmidi->mfx->lock, flags);

	return snd_madifx_midi_output_write(hmidi);
}

static void
snd_madifx_midi_input_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct mfx *mfx;
	struct madifx_midi *hmidi;
	unsigned long flags;

	hmidi = substream->rmidi->private_data;
	mfx = hmidi->mfx;

	spin_lock_irqsave(&mfx->lock, flags);
	if (up) {
		if (!(mfx->control_register & hmidi->ie)) {
			snd_madifx_flush_midi_input(mfx, hmidi->id);
			mfx->control_register |= hmidi->ie;
		}
	} else {
		mfx->control_register &= ~hmidi->ie;
	}

	madifx_write(mfx, MADIFX_CONTROL_REG, mfx->control_register);
	spin_unlock_irqrestore(&mfx->lock, flags);
}

static void snd_madifx_midi_output_timer(struct timer_list *t)
{
	struct madifx_midi *hmidi = from_timer(hmidi, t, timer);
	unsigned long flags;

	snd_madifx_midi_output_write(hmidi);
	spin_lock_irqsave(&hmidi->lock, flags);

	/* this does not bump hmidi->istimer, because the
	   kernel automatically removed the timer when it
	   expired, and we are now adding it back, thus
	   leaving istimer wherever it was set before.
	*/

	if (hmidi->istimer)
		mod_timer(&hmidi->timer, 1 + jiffies);

	spin_unlock_irqrestore(&hmidi->lock, flags);
}

static void
snd_madifx_midi_output_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct madifx_midi *hmidi;
	unsigned long flags;

	hmidi = substream->rmidi->private_data;
	spin_lock_irqsave(&hmidi->lock, flags);
	if (up) {
		if (!hmidi->istimer) {
			timer_setup(&hmidi->timer,
					snd_madifx_midi_output_timer, 0);
			mod_timer(&hmidi->timer, 1 + jiffies);
			hmidi->istimer++;
		}
	} else {
		if (hmidi->istimer && --hmidi->istimer <= 0)
			del_timer(&hmidi->timer);
	}
	spin_unlock_irqrestore(&hmidi->lock, flags);
	if (up)
		snd_madifx_midi_output_write(hmidi);
}

static int snd_madifx_midi_input_open(struct snd_rawmidi_substream *substream)
{
	struct madifx_midi *hmidi;

	hmidi = substream->rmidi->private_data;
	spin_lock_irq(&hmidi->lock);
	snd_madifx_flush_midi_input(hmidi->mfx, hmidi->id);
	hmidi->input = substream;
	spin_unlock_irq(&hmidi->lock);

	return 0;
}

static int snd_madifx_midi_output_open(struct snd_rawmidi_substream *substream)
{
	struct madifx_midi *hmidi;

	hmidi = substream->rmidi->private_data;
	spin_lock_irq(&hmidi->lock);
	hmidi->output = substream;
	spin_unlock_irq(&hmidi->lock);

	return 0;
}

static int snd_madifx_midi_input_close(struct snd_rawmidi_substream *substream)
{
	struct madifx_midi *hmidi;

	snd_madifx_midi_input_trigger(substream, 0);

	hmidi = substream->rmidi->private_data;
	spin_lock_irq(&hmidi->lock);
	hmidi->input = NULL;
	spin_unlock_irq(&hmidi->lock);

	return 0;
}

static int snd_madifx_midi_output_close(struct snd_rawmidi_substream *substream)
{
	struct madifx_midi *hmidi;

	snd_madifx_midi_output_trigger(substream, 0);

	hmidi = substream->rmidi->private_data;
	spin_lock_irq(&hmidi->lock);
	hmidi->output = NULL;
	spin_unlock_irq(&hmidi->lock);

	return 0;
}

static const struct snd_rawmidi_ops snd_madifx_midi_output = {
	.open =		snd_madifx_midi_output_open,
	.close =	snd_madifx_midi_output_close,
	.trigger =	snd_madifx_midi_output_trigger,
};

static const struct snd_rawmidi_ops snd_madifx_midi_input = {
	.open =		snd_madifx_midi_input_open,
	.close =	snd_madifx_midi_input_close,
	.trigger =	snd_madifx_midi_input_trigger,
};

static int snd_madifx_create_midi(struct snd_card *card,
					    struct mfx *mfx, int id)
{
	int err;
	char buf[64];

	mfx->midi[id].id = id;
	mfx->midi[id].mfx = mfx;
	spin_lock_init(&mfx->midi[id].lock);

	switch (id) {
	case 0:
		mfx->midi[0].dataIn = MADIFX_midi_in0_data;
		mfx->midi[0].statusIn = MADIFX_midi_in0_status;
		mfx->midi[0].dataOut = MADIFX_midi_out0_data;
		mfx->midi[0].statusOut = MADIFX_midi_out0_status;
		mfx->midi[0].ie = MADIFX_IEN0;
		mfx->midi[0].irq = MADIFX_mIRQ0;
		break;

	case 1:
		mfx->midi[1].dataIn = MADIFX_midi_in1_data;
		mfx->midi[1].statusIn = MADIFX_midi_in1_status;
		mfx->midi[1].dataOut = MADIFX_midi_out1_data;
		mfx->midi[1].statusOut = MADIFX_midi_out1_status;
		mfx->midi[1].ie = MADIFX_IEN1;
		mfx->midi[1].irq = MADIFX_mIRQ1;
		break;

	case 2:
		mfx->midi[2].dataIn = MADIFX_midi_in2_data;
		mfx->midi[2].statusIn = MADIFX_midi_in2_status;
		mfx->midi[2].dataOut = MADIFX_midi_out2_data;
		mfx->midi[2].statusOut = MADIFX_midi_out2_status;
		mfx->midi[2].ie = MADIFX_IEN2;
		mfx->midi[2].irq = MADIFX_mIRQ2;
		break;

	case 3:
		mfx->midi[3].dataIn = MADIFX_midi_in3_data;
		mfx->midi[3].statusIn = MADIFX_midi_in3_status;
		mfx->midi[3].dataOut = MADIFX_midi_out3_data;
		mfx->midi[3].statusOut = MADIFX_midi_out3_status;
		mfx->midi[3].ie = MADIFX_IEN3;
		mfx->midi[3].irq = MADIFX_mIRQ3;
		break;

	default:
		dev_err(mfx->card->dev,
				"MADIFX: Unknown MIDI port %i\n", id);
		return -EINVAL;

	}

	snprintf(buf, sizeof(buf), "%s MIDIoverMADI %d", card->shortname, id+1);

	err = snd_rawmidi_new(card, buf, id, 1, 1,
			&mfx->midi[id].rmidi);
	if (err < 0)
		return err;

	snprintf(mfx->midi[id].rmidi->name,
			sizeof(mfx->midi[id].rmidi->name),
			"%s MIDI %d",
			card->id, id+1);
	mfx->midi[id].rmidi->private_data = &mfx->midi[id];

	snd_rawmidi_set_ops(mfx->midi[id].rmidi,
			SNDRV_RAWMIDI_STREAM_OUTPUT,
			&snd_madifx_midi_output);
	snd_rawmidi_set_ops(mfx->midi[id].rmidi,
			SNDRV_RAWMIDI_STREAM_INPUT,
			&snd_madifx_midi_input);

	mfx->midi[id].rmidi->info_flags |=
		SNDRV_RAWMIDI_INFO_OUTPUT |
		SNDRV_RAWMIDI_INFO_INPUT |
		SNDRV_RAWMIDI_INFO_DUPLEX;

	return 0;
}


static void madifx_midi_work(struct work_struct *work)
{
	struct mfx *mfx = container_of(work, struct mfx, midi_work);
	int i = 0;

	while (i < mfx->midiPorts) {
		if (mfx->midi[i].pending)
			snd_madifx_midi_input_read(&mfx->midi[i]);

		i++;
	}
}


/*-----------------------------------------------------------------------------
  Status Interface
  ----------------------------------------------------------------------------*/

/* get the system sample rate which is set */


/*
 * Calculate the real sample rate from the
 * current DDS value.
 */
static int madifx_get_system_sample_rate(struct mfx *mfx)
{
	unsigned int period, rate;

	period = madifx_read(mfx, MADIFX_RD_PLL_FREQ);
	rate = madifx_calc_dds_value(mfx, period) *
		madifx_speed_multiplier(mfx);

	return rate;
}


#define HDSPM_SYSTEM_SAMPLE_RATE(xname, xindex) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = xname, \
	.index = xindex, \
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |\
		SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
	.info = snd_madifx_info_system_sample_rate, \
	.put = snd_madifx_put_system_sample_rate, \
	.get = snd_madifx_get_system_sample_rate \
}

static int snd_madifx_info_system_sample_rate(struct snd_kcontrol *kcontrol,
					      struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 27000;
	uinfo->value.integer.max = 207000;
	uinfo->value.integer.step = 1;
	return 0;
}


static int snd_madifx_get_system_sample_rate(struct snd_kcontrol *kcontrol,
					     struct snd_ctl_elem_value *
					     ucontrol)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = madifx_get_system_sample_rate(mfx);
	return 0;
}

static int snd_madifx_put_system_sample_rate(struct snd_kcontrol *kcontrol,
					     struct snd_ctl_elem_value *
					     ucontrol)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);

	madifx_set_dds_value(mfx, ucontrol->value.integer.value[0]);
	return 0;
}



static int madifx_external_freq_index(struct mfx *mfx,
				      enum madifx_syncsource port)
{
	int i = 0;
	int freq0_bit;
	int inp_freq;
	int lock_bit;
	int inp_status;

	inp_status = madifx_read(mfx, MADIFX_RD_INP_STATUS);
	inp_freq = madifx_read(mfx, MADIFX_RD_INP_FREQ);

	switch (port) {
	case syncsource_madi1:
		lock_bit = MADIFX_madi1_lock;
		freq0_bit = MADIFX_madi1_freq0;
		break;
	case syncsource_madi2:
		lock_bit = MADIFX_madi2_lock;
		freq0_bit = MADIFX_madi2_freq0;
		break;
	case syncsource_madi3:
		lock_bit = MADIFX_madi3_lock;
		freq0_bit = MADIFX_madi3_freq0;
		break;
	case syncsource_aes:
		lock_bit = MADIFX_aes_lock;
		freq0_bit = MADIFX_aes_freq0;
		break;
	case syncsource_wc:
		lock_bit = MADIFX_word_lock;
		freq0_bit = MADIFX_word_freq0;
		break;
	case syncsource_syncin:
		lock_bit = MADIFX_sync_in_lock;
		freq0_bit = MADIFX_sync_in_freq0;
		break;
	default:
		dev_err(mfx->card->dev,
				"MADIFX: Unknown external port ID %i\n", port);
		return 0;
	}

	if (!(inp_status & lock_bit)) {
		i = 0;
	} else {
		int freq_bits = inp_freq & (freq0_bit*15);

		if      (freq_bits == freq0_bit * 1)
			i = 1;
		else if (freq_bits == freq0_bit * 2)
			i = 2;
		else if (freq_bits == freq0_bit * 3)
			i = 3;
		else if (freq_bits == freq0_bit * 4)
			i = 4;
		else if (freq_bits == freq0_bit * 5)
			i = 5;
		else if (freq_bits == freq0_bit * 6)
			i = 6;
		else if (freq_bits == freq0_bit * 7)
			i = 7;
		else if (freq_bits == freq0_bit * 8)
			i = 8;
		else if (freq_bits == freq0_bit * 9)
			i = 9;
		else
			i = 0;
	}
	return i;
}


#define ENUMERATED_CTL_INFO(info, texts) \
	snd_ctl_enum_info(info, 1, ARRAY_SIZE(texts), texts)

#define HDSPM_AUTOSYNC_SAMPLE_RATE(xname, xindex) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = xname, \
	.private_value = xindex, \
	.access = SNDRV_CTL_ELEM_ACCESS_READ, \
	.info = snd_madifx_info_autosync_sample_rate, \
	.get = snd_madifx_get_autosync_sample_rate \
}


static int snd_madifx_info_autosync_sample_rate(struct snd_kcontrol *kcontrol,
					       struct snd_ctl_elem_info *uinfo)
{
	ENUMERATED_CTL_INFO(uinfo, texts_freq);

	return 0;
}


static int snd_madifx_get_autosync_sample_rate(struct snd_kcontrol *kcontrol,
					      struct snd_ctl_elem_value *
					      ucontrol)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);

	switch (mfx->io_type) {
	case MADIFX:
		ucontrol->value.enumerated.item[0] =
			madifx_external_freq_index(mfx,
						   kcontrol->private_value);
		break;

	default:
		break;
	}

	return 0;
}

#define MADIFX_MADI_CHANNELCOUNT(xname, xindex) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = xname, \
	.private_value = xindex, \
	.access = SNDRV_CTL_ELEM_ACCESS_READ, \
	.info = snd_madifx_info_channelcount, \
	.get = snd_madifx_get_channelcount \
}


static int snd_madifx_info_channelcount(struct snd_kcontrol *kcontrol,
					       struct snd_ctl_elem_info *uinfo)
{
	static const char *const texts[] = { "64", "56", "32", "28", "16",
		"14", "No lock" };

	ENUMERATED_CTL_INFO(uinfo, texts);
	return 0;
}

static int madifx_get_madichannelcount(struct mfx *mfx, int idx)
{
	int rate_index;
	int i = 0;
	int inp_status;
	int rx_64ch_bit = (MADIFX_madi1_rx_64ch << idx);

	inp_status = madifx_read(mfx, MADIFX_RD_INP_STATUS);


	/* Check for speed. If rate_index is zero, there's no lock */
	rate_index = madifx_external_freq_index(mfx, idx);
	if (0 == rate_index)
		i = 6;
	else {
		if (inp_status & rx_64ch_bit)
			i = 0;
		else
			i = 1;

		/* I know this is ugly */
		if (rate_index > 3)
			i += 2; /* Double speed */
		if (rate_index > 6)
			i += 2; /* Quad speed */
	}

	return i;

}


static int snd_madifx_get_channelcount(struct snd_kcontrol *kcontrol,
					      struct snd_ctl_elem_value *
					      ucontrol)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);
	int idx = kcontrol->private_value;


	ucontrol->value.enumerated.item[0] =
		madifx_get_madichannelcount(mfx, idx);

	return 0;
}



/*
 * Returns the system clock mode for the given card.
 * @returns 0 - master, 1 - slave
 */
static int madifx_system_clock_mode(struct mfx *mfx)
{
	u32 status;

	status = madifx_read(mfx, MADIFX_RD_INP_STATUS);
	if ((status & (MADIFX_SelSyncRef0 * 7)) == (MADIFX_SelSyncRef0 * 7))
		return 0;

	return 1;
}


#define HDSPM_INTERNAL_CLOCK(xname, xindex) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = xname, \
	.index = xindex, \
	.info = snd_madifx_info_clock_source, \
	.get = snd_madifx_get_clock_source, \
	.put = snd_madifx_put_clock_source \
}


static int madifx_clock_source(struct mfx *mfx)
{
	switch (mfx->system_sample_rate) {
	case 32000: return 0;
	case 44100: return 1;
	case 48000: return 2;
	case 64000: return 3;
	case 88200: return 4;
	case 96000: return 5;
	case 128000: return 6;
	case 176400: return 7;
	case 192000: return 8;
	}

	return -1;
}

static int madifx_set_clock_source(struct mfx *mfx, int mode)
{
	int rate;

	switch (mode) {
	case 0:
		rate = 32000; break;
	case 1:
		rate = 44100; break;
	case 2:
		rate = 48000; break;
	case 3:
		rate = 64000; break;
	case 4:
		rate = 88200; break;
	case 5:
		rate = 96000; break;
	case 6:
		rate = 128000; break;
	case 7:
		rate = 176400; break;
	case 8:
		rate = 192000; break;
	default:
		rate = 48000;
	}
	madifx_set_rate(mfx, rate, 1);
	return 0;
}

static int snd_madifx_info_clock_source(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, 9, texts_freq + 1);
}

static int snd_madifx_get_clock_source(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = madifx_clock_source(mfx);
	return 0;
}

static int snd_madifx_put_clock_source(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);
	int change;
	int val;

	if (!snd_madifx_use_is_exclusive(mfx))
		return -EBUSY;
	val = ucontrol->value.enumerated.item[0];
	if (val < 0)
		val = 0;
	if (val > 9)
		val = 9;
	spin_lock_irq(&mfx->lock);
	if (val != madifx_clock_source(mfx))
		change = (madifx_set_clock_source(mfx, val) == 0) ? 1 : 0;
	else
		change = 0;
	spin_unlock_irq(&mfx->lock);
	return change;
}


static int madifx_get_clock_select(struct mfx *mfx)
{
	switch (mfx->io_type) {
	case MADIFX:
		{
			int i;
			u32 status;

			status = madifx_read(mfx, MADIFX_RD_INP_STATUS);

			switch (status & (MADIFX_SelSyncRef0 * 7)) {
			case MADIFX_SelSyncRef0 * 0:
				i = 0; break;
			case MADIFX_SelSyncRef0 * 1:
				i = 1; break;
			case MADIFX_SelSyncRef0 * 2:
				i = 2; break;
			case MADIFX_SelSyncRef0 * 3:
				i = 3; break;
			case MADIFX_SelSyncRef0 * 4:
				i = 4; break;
			case MADIFX_SelSyncRef0 * 5:
				i = 5; break;
			case MADIFX_SelSyncRef0 * 6:
				i = 6; break;
			default:
						     /* We are master */
						     i = 0;
						     break;
			}
			return i;
		}
		break;
	default:
		break;
	}

	return -1;
}

static int madifx_set_clock_select(struct mfx *mfx, int val)
{
	mfx->settings_register &= ~MADIFX_SyncRefMask;
	mfx->settings_register |= MADIFX_SyncRef0 * val;
	madifx_write(mfx, MADIFX_SETTINGS_REG, mfx->settings_register);

	if (val > 0) {
		/* switched to slave mode */
		mfx->system_sample_rate = madifx_get_external_rate(mfx);
	}

	return 0;
}



#define MADIFX_CLOCK_SELECT(xname, xindex) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = xname, \
	.index = xindex, \
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |\
			SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
	.info = snd_madifx_info_clock_select, \
	.get = snd_madifx_get_clock_select, \
	.put = snd_madifx_put_clock_select \
}


static int snd_madifx_info_clock_select(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_info *uinfo)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);

	snd_ctl_enum_info(uinfo, 1, mfx->texts_clocksource_items,
			mfx->texts_clocksource);

	return 0;
}

static int snd_madifx_get_clock_select(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);
	int psf = madifx_get_clock_select(mfx);

	if (psf >= 0) {
		ucontrol->value.enumerated.item[0] = psf;
		return 0;
	}

	return -1;
}

static int snd_madifx_put_clock_select(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);
	int val, change = 0;

	if (!snd_madifx_use_is_exclusive(mfx))
		return -EBUSY;

	val = ucontrol->value.enumerated.item[0];

	if (val < 0)
		val = 0;
	else if (val >= mfx->texts_clocksource_items)
		val = mfx->texts_clocksource_items-1;

	spin_lock_irq(&mfx->lock);
	if (val != madifx_get_clock_select(mfx))
		change = (0 == madifx_set_clock_select(mfx, val)) ? 1 : 0;

	spin_unlock_irq(&mfx->lock);
	return change;
}



#define MADIFX_TOGGLE_SETTING(xname, xindex) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = xname, \
	.private_value = xindex, \
	.info = snd_madifx_info_toggle_setting, \
	.get = snd_madifx_get_toggle_setting, \
	.put = snd_madifx_put_toggle_setting \
}

static int madifx_read_toggle_setting(struct mfx *mfx, u32 reg)
{
	return (mfx->settings_register & (reg)) ? 1 : 0;
}

static int madifx_set_toggle_setting(struct mfx *mfx, u32 reg, int out)
{
	if (out)
		mfx->settings_register |= (reg);
	else
		mfx->settings_register &= ~(reg);
	madifx_write(mfx, MADIFX_SETTINGS_REG, mfx->settings_register);

	return 0;
}

#define snd_madifx_info_toggle_setting		snd_ctl_boolean_mono_info

static int snd_madifx_get_toggle_setting(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);

	spin_lock_irq(&mfx->lock);
	ucontrol->value.integer.value[0] = madifx_read_toggle_setting(mfx,
			kcontrol->private_value);
	spin_unlock_irq(&mfx->lock);
	return 0;
}

static int snd_madifx_put_toggle_setting(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);
	int change;
	u32 reg = kcontrol->private_value;
	unsigned int val;

	if (!snd_madifx_use_is_exclusive(mfx))
		return -EBUSY;
	val = ucontrol->value.integer.value[0] & 1;
	spin_lock_irq(&mfx->lock);
	change = (int) val != madifx_read_toggle_setting(mfx, reg);
	madifx_set_toggle_setting(mfx, reg, val);
	spin_unlock_irq(&mfx->lock);
	return change;
}



#define HDSPM_MIXER(xname, xindex) \
{	.iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
	.name = xname, \
	.index = xindex, \
	.device = 0, \
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | \
		SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
	.info = snd_madifx_info_mixer, \
	.get = snd_madifx_get_mixer, \
	.put = snd_madifx_put_mixer \
}

#define HDSPM_SYNC_CHECK(xname, xindex) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = xname, \
	.private_value = xindex, \
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
	.info = snd_madifx_info_sync_check, \
	.get = snd_madifx_get_sync_check \
}


static int snd_madifx_info_sync_check(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_info *uinfo)
{
	static const char *const texts[] = { "No Lock", "Lock", "Sync", "N/A" };

	ENUMERATED_CTL_INFO(uinfo, texts);
	return 0;
}


static int madifx_sync_check(struct mfx *mfx, enum madifx_syncsource idx)
{
	u32 status, lockmask, syncmask;
	int lock, sync;

	status = madifx_read(mfx, MADIFX_RD_INP_STATUS);

	switch (idx) {
	case syncsource_syncin:
		/* Sync-In */
		lockmask = MADIFX_sync_in_lock;
		syncmask = MADIFX_sync_in_sync;
		break;
	case syncsource_aes:
		/* AES */
		lockmask = MADIFX_aes_lock;
		syncmask = MADIFX_aes_sync;
		break;
	case syncsource_wc:
		/* WC */
		lockmask = MADIFX_word_lock;
		syncmask = MADIFX_word_sync;
		break;
	default:
		lockmask = (MADIFX_madi1_lock << idx);
		syncmask = (MADIFX_madi1_sync << idx);
		break;
	}

	lock = (status & lockmask) ? 1 : 0;
	sync = (status & syncmask) ? 1 : 0;

	if (lock && sync)
		return 2;
	else if (lock)
		return 1;
	return 0;
}




static int snd_madifx_get_sync_check(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct mfx *mfx = snd_kcontrol_chip(kcontrol);
	int val = -1;

	switch (mfx->io_type) {
	case MADIFX:
		val = madifx_sync_check(mfx, kcontrol->private_value);
		break;
	}

	if (-1 == val)
		val = 3;

	ucontrol->value.enumerated.item[0] = val;
	return 0;
}



static struct snd_kcontrol_new snd_madifx_controls_madi[] = {
	HDSPM_SYSTEM_SAMPLE_RATE("System Sample Rate", 0),
	HDSPM_INTERNAL_CLOCK("Internal Clock", 0),
	HDSPM_SYNC_CHECK("MADI 1 SyncCheck", syncsource_madi1),
	HDSPM_SYNC_CHECK("MADI 2 SyncCheck", syncsource_madi2),
	HDSPM_SYNC_CHECK("MADI 3 SyncCheck", syncsource_madi3),
	HDSPM_SYNC_CHECK("WC SyncCheck", syncsource_wc),
	HDSPM_SYNC_CHECK("AES SyncCheck", syncsource_aes),
	HDSPM_SYNC_CHECK("Sync-In SyncCheck", syncsource_syncin),
	HDSPM_AUTOSYNC_SAMPLE_RATE("MADI 1 Frequency", syncsource_madi1),
	HDSPM_AUTOSYNC_SAMPLE_RATE("MADI 2 Frequency", syncsource_madi2),
	HDSPM_AUTOSYNC_SAMPLE_RATE("MADI 3 Frequency", syncsource_madi3),
	HDSPM_AUTOSYNC_SAMPLE_RATE("WC Frequency", syncsource_wc),
	HDSPM_AUTOSYNC_SAMPLE_RATE("AES Frequency", syncsource_aes),
	HDSPM_AUTOSYNC_SAMPLE_RATE("Sync-In Frequency", syncsource_syncin),
	MADIFX_MADI_CHANNELCOUNT("MADI 1 RX #ch", 0),
	MADIFX_MADI_CHANNELCOUNT("MADI 2 RX #ch", 1),
	MADIFX_MADI_CHANNELCOUNT("MADI 3 RX #ch", 2),
	MADIFX_TOGGLE_SETTING("MADI 1 TX 64ch mode", MADIFX_madi1_tx_64ch),
	MADIFX_TOGGLE_SETTING("MADI 2 TX 64ch mode", MADIFX_madi2_tx_64ch),
	MADIFX_TOGGLE_SETTING("MADI 3 TX 64ch mode", MADIFX_madi3_tx_64ch),
	MADIFX_TOGGLE_SETTING("MADI 1 SMUX mode", MADIFX_madi1_smux),
	MADIFX_TOGGLE_SETTING("MADI 2 SMUX mode", MADIFX_madi2_smux),
	MADIFX_TOGGLE_SETTING("MADI 3 SMUX mode", MADIFX_madi3_smux),
	MADIFX_TOGGLE_SETTING("WC Term", MADIFX_WCK_TERM),
	MADIFX_TOGGLE_SETTING("WC single speed", MADIFX_WCK48),
	MADIFX_TOGGLE_SETTING("AES professional", MADIFX_PRO),
	MADIFX_TOGGLE_SETTING("Redundancy mode", MADIFX_redundancy_mode),
	MADIFX_TOGGLE_SETTING("Mirror MADI out", MADIFX_mirror_madi_out),
	MADIFX_CLOCK_SELECT("Clock Selection", 0)
};


static int snd_madifx_create_controls(struct snd_card *card,
					struct mfx *mfx)
{
	unsigned int idx, limit;
	int err;
	struct snd_kcontrol_new *list = NULL;

	switch (mfx->io_type) {
	case MADIFX:
		list = snd_madifx_controls_madi;
		limit = ARRAY_SIZE(snd_madifx_controls_madi);
		break;
	}

	if (list) {
		for (idx = 0; idx < limit; idx++) {
			err = snd_ctl_add(card,
					snd_ctl_new1(&list[idx], mfx));
			if (err < 0)
				return err;
		}
	}

	/* FIXME: If there'll ever be an ALSA mixer for MADIFX, you want to
	 * create the corresponding controls here.
	 */
	return 0;
}

/*------------------------------------------------------------
   /proc interface
 ------------------------------------------------------------*/

static void
snd_madifx_proc_read_madifx(struct snd_info_entry *entry,
			 struct snd_info_buffer *buffer)
{
	struct mfx *mfx = entry->private_data;
	u32 status, inp_status, control, freq;

	char *system_clock_mode;
	int x;

	unsigned int period;
	u64 freq_const = 0;
	u32 rate;

	status = madifx_read(mfx, MADIFX_RD_STATUS);
	inp_status = madifx_read(mfx, MADIFX_RD_INP_STATUS);
	control = mfx->control_register;
	freq = madifx_read(mfx, MADIFX_RD_INP_FREQ);

#if 0
	snd_iprintf(buffer, "%s (Card #%d) Rev.%x Status2first3bits: %x\n",
			mfx->card_name, mfx->card->number + 1,
			mfx->firmware_rev,
			(status2 & HDSPM_version0) |
			(status2 & HDSPM_version1) | (status2 &
				HDSPM_version2));
#endif
	snd_iprintf(buffer, "HW Serial: 0x%x\n", madifx_read(mfx,
				MADIFX_RD_VERSION));

	snd_iprintf(buffer, "IRQ: %d Registers bus: 0x%lx VM: 0x%lx\n",
			mfx->irq, mfx->port, (unsigned long)mfx->iobase);

	snd_iprintf(buffer, "--- System ---\n");

	snd_iprintf(buffer,
		"IRQ Pending: Audio=%d, MIDI0=%d, MIDI1=%d, MIDI2=%d MIDI3=%d IRQcount=%d\n",
		status & HDSPM_audioIRQPending,
		(status & MADIFX_mIRQ0) ? 1 : 0,
		(status & MADIFX_mIRQ1) ? 1 : 0,
		(status & MADIFX_mIRQ2) ? 1 : 0,
		(status & MADIFX_mIRQ3) ? 1 : 0,
		mfx->irq_count);

	snd_iprintf(buffer,
		"MIDI FIFO: Out0=0x%x, Out1=0x%x, Out2=0x%x, Out3=0x%x\n",
		madifx_read(mfx, MADIFX_midi_out0_status) & 0xFF,
		madifx_read(mfx, MADIFX_midi_out1_status) & 0xFF,
		madifx_read(mfx, MADIFX_midi_out2_status) & 0xFF,
		madifx_read(mfx, MADIFX_midi_out3_status) & 0xFF);
	snd_iprintf(buffer,
		"MIDI FIFO: in0=0x%x, in1=0x%x, in2=0x%x, in3=0x%x\n",
		madifx_read(mfx, MADIFX_midi_in0_status) & 0xFF,
		madifx_read(mfx, MADIFX_midi_in1_status) & 0xFF,
		madifx_read(mfx, MADIFX_midi_in2_status) & 0xFF,
		madifx_read(mfx, MADIFX_midi_in3_status) & 0xFF);
	snd_iprintf(buffer,
		"Register:\ncontrol=0x%x, settings=0x%x, status=0x%x, input=0x%x inp_freq=0x%x\n",
		mfx->control_register, mfx->settings_register,
		status, inp_status, freq);

		switch (mfx->io_type) {
		case MADIFX:
			freq_const = 131072000000000ULL;
			break;
		}

		period = madifx_read(mfx, MADIFX_RD_PLL_FREQ);
		snd_iprintf(buffer, "    period: %u\n", period);


		/* rate = freq_const/period; */
		rate = div_u64(freq_const, period);

		rate *= madifx_speed_multiplier(mfx);


		snd_iprintf(buffer, "  Frequency: %u Hz\n",
				(unsigned int) rate);


	snd_iprintf(buffer, "--- Settings ---\n");

	x = madifx_get_latency(mfx);

	snd_iprintf(buffer,
		"Size (Latency): %d samples\n", x);

	if (1 == madifx_system_clock_mode(mfx))
		system_clock_mode = "Slave";
	else
		system_clock_mode = "Master";
	snd_iprintf(buffer, "AutoSync Reference: %s\n", system_clock_mode);

	snd_iprintf(buffer, "Selected clock source: %s\n",
		mfx->texts_clocksource[madifx_get_clock_select(mfx)]);

	snd_iprintf(buffer, "System Clock Frequency: %d\n",
			mfx->system_sample_rate);

	snd_iprintf(buffer, "\n");
}


#ifdef CONFIG_SND_DEBUG
static void
snd_madifx_proc_read_debug(struct snd_info_entry *entry,
			  struct snd_info_buffer *buffer)
{
	struct mfx *mfx = entry->private_data;

	int j, i;

	for (i = 0; i < 256 /* 1024*64 */; i += j) {
		snd_iprintf(buffer, "0x%08X: ", i);
		for (j = 0; j < 16; j += 4)
			snd_iprintf(buffer, "%08X ", madifx_read(mfx, i + j));
		snd_iprintf(buffer, "\n");
	}
}
#endif


#if 0
/* FIXME: Portnames not implemented, yet. mfx->port_names_in and _out are
 * set to NULL, so don't derefence them for the time being.
 */
static void snd_madifx_proc_ports_in(struct snd_info_entry *entry,
			  struct snd_info_buffer *buffer)
{
	struct mfx *mfx = entry->private_data;
	int i;

	snd_iprintf(buffer, "# generated by mfx\n");

	for (i = 0; i < mfx->max_channels_in; i++)
		snd_iprintf(buffer, "%d=%s\n", i+1, mfx->port_names_in[i]);
}

static void snd_madifx_proc_ports_out(struct snd_info_entry *entry,
			  struct snd_info_buffer *buffer)
{
	struct mfx *mfx = entry->private_data;
	int i;

	snd_iprintf(buffer, "# generated by mfx\n");

	for (i = 0; i < mfx->max_channels_out; i++)
		snd_iprintf(buffer, "%d=%s\n", i+1, mfx->port_names_out[i]);
}
#endif


static void snd_madifx_proc_init(struct mfx *mfx)
{
	struct snd_info_entry *entry;

	if (!snd_card_proc_new(mfx->card, "madifx", &entry)) {
		switch (mfx->io_type) {
		case MADIFX:
			snd_info_set_text_ops(entry, mfx,
					snd_madifx_proc_read_madifx);
			break;
		}
	}

#if 0
	/* FIXME: port names still missing for MADIFX */
	if (!snd_card_proc_new(mfx->card, "ports.in", &entry))
		snd_info_set_text_ops(entry, mfx, snd_madifx_proc_ports_in);

	if (!snd_card_proc_new(mfx->card, "ports.out", &entry))
		snd_info_set_text_ops(entry, mfx, snd_madifx_proc_ports_out);
#endif

#ifdef CONFIG_SND_DEBUG
	/* debug file to read all mfx registers */
	if (!snd_card_proc_new(mfx->card, "debug", &entry))
		snd_info_set_text_ops(entry, mfx,
				snd_madifx_proc_read_debug);
#endif
}

/*------------------------------------------------------------
   mfx intitialize
 ------------------------------------------------------------*/

static int snd_madifx_set_defaults(struct mfx *mfx)
{
	/* ASSUMPTION: mfx->lock is either held, or there is no need to
	   hold it (e.g. during module initialization).
	   */

	/* set defaults:       */

	mfx->settings_register = 0;

	switch (mfx->io_type) {
	case MADIFX:
		/* OSX: LAT_3+BUF_SIZ_1+BUF_SIZ_2+freq1; */
		mfx->control_register = MADIFX_LAT_3 + MADIFX_BUF_SIZ_1 +
			MADIFX_BUF_SIZ_2 + MADIFX_freq1;
		/* PRO+madi1_tx_64ch+madi2_tx_64ch+madi3_tx_64ch; */
		mfx->settings_register = 0x8 + 0x80 + 0x100 + 0x200;
		break;
	}

	madifx_write(mfx, MADIFX_CONTROL_REG, mfx->control_register);

	madifx_compute_period_size(mfx);

	madifx_write(mfx, MADIFX_SETTINGS_REG, mfx->settings_register);

	/* set a default rate so that the channel map is set up. */
	madifx_set_rate(mfx, 48000, 1);

	return 0;
}


/*------------------------------------------------------------
   interrupt
 ------------------------------------------------------------*/

static irqreturn_t snd_madifx_interrupt(int irq, void *dev_id)
{
	struct mfx *mfx = (struct mfx *) dev_id;
	unsigned int status;
	int i, audio, midi, schedule = 0;
	/* cycles_t now; */

	status = madifx_read(mfx, MADIFX_RD_STATUS);

	audio = status & HDSPM_audioIRQPending;
	midi = status & (MADIFX_mIRQ0 | MADIFX_mIRQ1 |
			MADIFX_mIRQ2 | MADIFX_mIRQ3);

	/* now = get_cycles(); */
	/*
	 *   LAT_2..LAT_0 period  counter (win)  counter (mac)
	 *          6       4096   ~256053425     ~514672358
	 *          5       2048   ~128024983     ~257373821
	 *          4       1024    ~64023706     ~128718089
	 *          3        512    ~32005945      ~64385999
	 *          2        256    ~16003039      ~32260176
	 *          1        128     ~7998738      ~16194507
	 *          0         64     ~3998231       ~8191558
	 */
	/*
	   snd_printk(KERN_INFO "snd_madifx_interrupt %llu @ %llx\n",
	   now-mfx->last_interrupt, status & 0xFFC0);
	   mfx->last_interrupt = now;
	*/

	if (!audio && !midi)
		return IRQ_NONE;

	madifx_write(mfx, MADIFX_IRQ_ACK, 0);
	mfx->irq_count++;


	if (audio) {
		int gi;
		for (gi = 0; gi < MADIFX_NUM_PCM_GROUPS; gi++) {
			if (mfx->groups[gi].capture_substream)
				snd_pcm_period_elapsed(mfx->groups[gi].capture_substream);
			if (mfx->groups[gi].playback_substream)
				snd_pcm_period_elapsed(mfx->groups[gi].playback_substream);
		}
	}

	if (midi) {
		i = 0;
		while (i < mfx->midiPorts) {
			if ((madifx_read(mfx,
				mfx->midi[i].statusIn) & 0xff) &&
					(status & mfx->midi[i].irq)) {
				/* we disable interrupts for this input until
				 * processing is done
				 */
				mfx->control_register &= ~mfx->midi[i].ie;
				madifx_write(mfx, MADIFX_CONTROL_REG,
						mfx->control_register);
				mfx->midi[i].pending = 1;
				schedule = 1;
			}

			i++;
		}

		if (schedule)
			queue_work(system_highpri_wq, &mfx->midi_work);
	}

	return IRQ_HANDLED;
}

/*------------------------------------------------------------
   pcm interface
  ------------------------------------------------------------*/


static snd_pcm_uframes_t snd_madifx_hw_pointer(struct snd_pcm_substream
					      *substream)
{
	struct madifx_pcm_group *group = snd_pcm_substream_chip(substream);

	return madifx_hw_pointer(group->mfx);
}


static int snd_madifx_reset(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct madifx_pcm_group *group = snd_pcm_substream_chip(substream);
	struct mfx *mfx = group->mfx;
	struct snd_pcm_substream *other;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		other = group->capture_substream;
	else
		other = group->playback_substream;

	if (mfx->running_count > 0)
		runtime->status->hw_ptr = madifx_hw_pointer(mfx);
	else
		runtime->status->hw_ptr = 0;
	if (other) {
		struct snd_pcm_substream *s;
		struct snd_pcm_runtime *oruntime = other->runtime;

		snd_pcm_group_for_each_entry(s, substream) {
			if (s == other) {
				oruntime->status->hw_ptr =
					runtime->status->hw_ptr;
				break;
			}
		}
	}
	return 0;
}

/* --- Per-group page-table and DMA helpers -------------------------------- */

static int group_pt_out_base(const struct madifx_pcm_group *g)
{
	switch (g->type) {
	case MADIFX_PCM_TYPE_AES:    return MADIFX_PT_OUT_AES;
	case MADIFX_PCM_TYPE_PHONES: return MADIFX_PT_OUT_PHONES;
	case MADIFX_PCM_TYPE_MADI:   return MADIFX_PT_OUT_MADI(g->madi_stream);
	}
	return -1;
}

static int group_pt_in_base(const struct madifx_pcm_group *g)
{
	switch (g->type) {
	case MADIFX_PCM_TYPE_AES:  return MADIFX_PT_IN_AES;
	case MADIFX_PCM_TYPE_MADI: return MADIFX_PT_IN_MADI(g->madi_stream);
	default: return -1; /* Phones has no capture */
	}
}

static int group_pt_pages(const struct madifx_pcm_group *g)
{
	return (g->n_channels == 8) ? NUM_DMA_CH_PAGES : NUM_AES_PAGES;
}

static size_t group_dma_size(const struct madifx_pcm_group *g)
{
	return (g->n_channels == 8) ? MADIFX_DMA_SIZE_8CH : MADIFX_DMA_SIZE_2CH;
}

/* -------------------------------------------------------------------------- */

static int snd_madifx_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct madifx_pcm_group *group = snd_pcm_substream_chip(substream);
	struct mfx *mfx = group->mfx;
	int err;
	int i;
	pid_t this_pid, other_pid;
	int pt_base, n_pages;
	size_t wanted;

	spin_lock_irq(&mfx->lock);
	if (substream->pstr->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		this_pid  = group->playback_pid;
		other_pid = group->capture_pid;
	} else {
		this_pid  = group->capture_pid;
		other_pid = group->playback_pid;
	}

	if (other_pid > 0 && this_pid != other_pid) {
		if (params_rate(params) != mfx->system_sample_rate) {
			spin_unlock_irq(&mfx->lock);
			_snd_pcm_hw_param_setempty(params, SNDRV_PCM_HW_PARAM_RATE);
			return -EBUSY;
		}
		if (params_period_size(params) != mfx->period_bytes / 4) {
			spin_unlock_irq(&mfx->lock);
			_snd_pcm_hw_param_setempty(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
			return -EBUSY;
		}
	}
	spin_unlock_irq(&mfx->lock);

	spin_lock_irq(&mfx->lock);
	err = madifx_set_rate(mfx, params_rate(params), 0);
	if (err < 0) {
		dev_info(mfx->card->dev, "err on madifx_set_rate: %d\n", err);
		spin_unlock_irq(&mfx->lock);
		_snd_pcm_hw_param_setempty(params, SNDRV_PCM_HW_PARAM_RATE);
		return err;
	}
	spin_unlock_irq(&mfx->lock);

	err = madifx_set_interrupt_interval(mfx, params_period_size(params));
	if (err < 0) {
		dev_info(mfx->card->dev,
			"err on madifx_set_interrupt_interval: %d\n", err);
		_snd_pcm_hw_param_setempty(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
		return err;
	}

	/* Allocate per-group DMA buffer */
	wanted = group_dma_size(group);
	err = snd_pcm_lib_malloc_pages(substream, wanted);
	if (err < 0) {
		dev_info(mfx->card->dev,
		    "err on snd_pcm_lib_malloc_pages: %d\n", err);
		return err;
	}

	/* Write only this group's page-table entries to hardware */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		pt_base = group_pt_out_base(group);
		n_pages  = group_pt_pages(group);
		for (i = 0; i < n_pages; i++) {
			mfx->dmaPageTable[pt_base + i] =
				snd_pcm_sgbuf_get_addr(substream,
						       i * MADIFX_HW_PAGE_SIZE);
			madifx_write(mfx,
				     MADIFX_PAGE_ADDRESS_LIST + 4 * (pt_base + i),
				     mfx->dmaPageTable[pt_base + i]);
		}
		for (i = 0; i < 32; ++i)
			snd_madifx_enable_out(mfx, i, 1);

		pr_debug("Playback DMA for group %d at %p\n",
			 group->madi_stream, substream->runtime->dma_area);
	} else {
		pt_base = group_pt_in_base(group);
		n_pages  = group_pt_pages(group);
		for (i = 0; i < n_pages; i++) {
			mfx->dmaPageTable[pt_base + i] =
				snd_pcm_sgbuf_get_addr(substream,
						       i * MADIFX_HW_PAGE_SIZE);
			madifx_write(mfx,
				     MADIFX_PAGE_ADDRESS_LIST + 4 * (pt_base + i),
				     mfx->dmaPageTable[pt_base + i]);
		}
		for (i = 0; i < 32; ++i)
			snd_madifx_enable_in(mfx, i, 1);

		pr_debug("Capture DMA for group %d at %p\n",
			 group->madi_stream, substream->runtime->dma_area);
	}

	/* Switch to native float format if requested */
	if (SNDRV_PCM_FORMAT_FLOAT_LE == params_format(params)) {
		if (!(mfx->control_register & MADIFX_float_format))
			dev_info(mfx->card->dev,
				"mfx: Switching to native 32bit LE float format.\n");

		mfx->control_register |= MADIFX_float_format;
	} else if (SNDRV_PCM_FORMAT_S32_LE == params_format(params)) {
		if (mfx->control_register & MADIFX_float_format)
			dev_info(mfx->card->dev,
				"mfx: Switching to native 32bit LE integer format.\n");

		mfx->control_register &= ~MADIFX_float_format;
	}
	madifx_write(mfx, MADIFX_CONTROL_REG, mfx->control_register);

	return 0;
}

static int snd_madifx_hw_free(struct snd_pcm_substream *substream)
{
	struct madifx_pcm_group *group = snd_pcm_substream_chip(substream);
	struct mfx *mfx = group->mfx;
	int i, pt_base, n_pages;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		pt_base = group_pt_out_base(group);
		n_pages  = group_pt_pages(group);

		/* Restore this group's page-table entries to the dummy page */
		for (i = 0; i < n_pages; i++) {
			mfx->dmaPageTable[pt_base + i] = mfx->dummy_dma_addr;
			madifx_write(mfx,
				     MADIFX_PAGE_ADDRESS_LIST + 4 * (pt_base + i),
				     mfx->dummy_dma_addr);
		}
		for (i = 0; i < 32; ++i)
			snd_madifx_enable_out(mfx, i, 0);
	} else {
		pt_base = group_pt_in_base(group);
		n_pages  = group_pt_pages(group);

		for (i = 0; i < n_pages; i++) {
			mfx->dmaPageTable[pt_base + i] = mfx->dummy_dma_addr;
			madifx_write(mfx,
				     MADIFX_PAGE_ADDRESS_LIST + 4 * (pt_base + i),
				     mfx->dummy_dma_addr);
		}
		for (i = 0; i < 32; ++i)
			snd_madifx_enable_in(mfx, i, 0);
	}

	snd_pcm_lib_free_pages(substream);
	return 0;
}


/*
 * Per-group channel_info: each group's DMA buffer starts at offset 0.
 * The hardware-level interleaving within each stream is:
 *   AES/Phones: 2 channels, 64-bit frames  (step=64, first=32*ch)
 *   MADI:       8 channels, 256-bit frames (step=256, first=32*ch)
 */
static int snd_madifx_channel_info(struct snd_pcm_substream *substream,
		struct snd_pcm_channel_info *info)
{
	struct madifx_pcm_group *group = snd_pcm_substream_chip(substream);
	unsigned int channel = info->channel;

	if (channel >= (unsigned int)group->n_channels)
		return -EINVAL;

	info->offset = 0;
	info->first  = 32 * channel;
	info->step   = (group->type == MADIFX_PCM_TYPE_MADI) ? 256 : 64;

	return 0;
}


static int snd_madifx_ioctl(struct snd_pcm_substream *substream,
		unsigned int cmd, void *arg)
{
	switch (cmd) {
	case SNDRV_PCM_IOCTL1_RESET:
		return snd_madifx_reset(substream);
	case SNDRV_PCM_IOCTL1_CHANNEL_INFO:
		return snd_madifx_channel_info(substream,
					       (struct snd_pcm_channel_info *)arg);
	default:
		break;
	}
	return snd_pcm_lib_ioctl(substream, cmd, arg);
}

static int snd_madifx_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct madifx_pcm_group *group = snd_pcm_substream_chip(substream);
	struct mfx *mfx = group->mfx;
	struct snd_pcm_substream *other;
	bool start;
	bool sync_other = false;
	int prev_count;

	spin_lock(&mfx->lock);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		start = true;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		start = false;
		break;
	default:
		snd_BUG();
		spin_unlock(&mfx->lock);
		return -EINVAL;
	}

	other = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		? group->capture_substream
		: group->playback_substream;

	if (other) {
		struct snd_pcm_substream *s;

		snd_pcm_group_for_each_entry(s, substream) {
			if (s == other) {
				snd_pcm_trigger_done(s, substream);
				sync_other = true;
				break;
			}
		}
		if (!sync_other) {
			/* Other stream is not in our sync group */
			if (!start && substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
				madifx_silence_group_playback(group);
			if (start && substream->stream == SNDRV_PCM_STREAM_CAPTURE)
				madifx_silence_group_playback(group);
		}
	} else {
		if (!start && substream->stream == SNDRV_PCM_STREAM_CAPTURE)
			madifx_silence_group_playback(group);
	}

	snd_pcm_trigger_done(substream, substream);

	prev_count = mfx->running_count;
	if (start) {
		mfx->running_count++;
		if (sync_other)
			mfx->running_count++;
	} else {
		mfx->running_count--;
		if (sync_other)
			mfx->running_count--;
	}

	if (prev_count == 0 && mfx->running_count > 0)
		madifx_start_audio(mfx);
	else if (prev_count > 0 && mfx->running_count == 0)
		madifx_stop_audio(mfx);

	spin_unlock(&mfx->lock);
	return 0;
}

static int snd_madifx_prepare(struct snd_pcm_substream *substream)
{
	return 0;
}

static struct snd_pcm_hardware snd_madifx_playback_subinfo = {
	.info = (SNDRV_PCM_INFO_MMAP |
		 SNDRV_PCM_INFO_MMAP_VALID |
		 SNDRV_PCM_INFO_COMPLEX |
		 SNDRV_PCM_INFO_SYNC_START),
	.formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_FLOAT_LE,
	.rates = (SNDRV_PCM_RATE_32000 |
		  SNDRV_PCM_RATE_44100 |
		  SNDRV_PCM_RATE_48000 |
		  SNDRV_PCM_RATE_64000 |
		  SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
		  SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000),
	.rate_min = 32000,
	.rate_max = 192000,
	.channels_min = 1,
	.channels_max = 196,
	.buffer_bytes_max = OUTPUT_DMA_BUFFER_SIZE,
	.period_bytes_min = (32 * 4),
	.period_bytes_max = OUTPUT_DMA_BUFFER_SIZE,
	.periods_min = 2,
	.periods_max = 1024,
	.fifo_size = 0
};

static struct snd_pcm_hardware snd_madifx_capture_subinfo = {
	.info = (SNDRV_PCM_INFO_MMAP |
		 SNDRV_PCM_INFO_MMAP_VALID |
		 SNDRV_PCM_INFO_COMPLEX |
		 SNDRV_PCM_INFO_SYNC_START),
	.formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_FLOAT_LE,
	.rates = (SNDRV_PCM_RATE_32000 |
		  SNDRV_PCM_RATE_44100 |
		  SNDRV_PCM_RATE_48000 |
		  SNDRV_PCM_RATE_64000 |
		  SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
		  SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000),
	.rate_min = 32000,
	.rate_max = 192000,
	.channels_min = 1,
	.channels_max = 194,
	.buffer_bytes_max = INPUT_DMA_BUFFER_SIZE,
	.period_bytes_min = (32 * 4),
	.period_bytes_max = INPUT_DMA_BUFFER_SIZE,
	.periods_min = 2,
	.periods_max = 1024,
	.fifo_size = 0
};


static int snd_madifx_playback_open(struct snd_pcm_substream *substream)
{
	struct madifx_pcm_group *group = snd_pcm_substream_chip(substream);
	struct mfx *mfx = group->mfx;
	struct snd_pcm_runtime *runtime = substream->runtime;

	spin_lock_irq(&mfx->lock);
	snd_pcm_set_sync(substream);

	runtime->hw = snd_madifx_playback_subinfo;
	runtime->hw.channels_min      = group->n_channels;
	runtime->hw.channels_max      = group->n_channels;
	runtime->hw.buffer_bytes_max  = group_dma_size(group);
	runtime->hw.period_bytes_max  = group_dma_size(group);

	if (!group->capture_substream)
		madifx_stop_audio(mfx);

	group->playback_pid       = current->pid;
	group->playback_substream = substream;

	spin_unlock_irq(&mfx->lock);

	snd_pcm_hw_constraint_msbits(runtime, 0, 32, 24);
	snd_pcm_hw_constraint_pow2(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				     32, 4096);
	snd_pcm_hw_constraint_single(runtime, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
				     8192);
	snd_pcm_hw_constraint_single(runtime, SNDRV_PCM_HW_PARAM_CHANNELS,
				     group->n_channels);
	return 0;
}

static int snd_madifx_playback_release(struct snd_pcm_substream *substream)
{
	struct madifx_pcm_group *group = snd_pcm_substream_chip(substream);
	struct mfx *mfx = group->mfx;

	spin_lock_irq(&mfx->lock);
	group->playback_pid       = -1;
	group->playback_substream = NULL;
	spin_unlock_irq(&mfx->lock);
	return 0;
}


static int snd_madifx_capture_open(struct snd_pcm_substream *substream)
{
	struct madifx_pcm_group *group = snd_pcm_substream_chip(substream);
	struct mfx *mfx = group->mfx;
	struct snd_pcm_runtime *runtime = substream->runtime;

	spin_lock_irq(&mfx->lock);
	snd_pcm_set_sync(substream);

	runtime->hw = snd_madifx_capture_subinfo;
	runtime->hw.channels_min      = group->n_channels;
	runtime->hw.channels_max      = group->n_channels;
	runtime->hw.buffer_bytes_max  = group_dma_size(group);
	runtime->hw.period_bytes_max  = group_dma_size(group);

	if (!group->playback_substream)
		madifx_stop_audio(mfx);

	group->capture_pid       = current->pid;
	group->capture_substream = substream;

	spin_unlock_irq(&mfx->lock);

	snd_pcm_hw_constraint_msbits(runtime, 0, 32, 24);
	snd_pcm_hw_constraint_pow2(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				     32, 4096);
	snd_pcm_hw_constraint_single(runtime, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
				     8192);
	snd_pcm_hw_constraint_single(runtime, SNDRV_PCM_HW_PARAM_CHANNELS,
				     group->n_channels);
	return 0;
}

static int snd_madifx_capture_release(struct snd_pcm_substream *substream)
{
	struct madifx_pcm_group *group = snd_pcm_substream_chip(substream);
	struct mfx *mfx = group->mfx;

	spin_lock_irq(&mfx->lock);
	group->capture_pid       = -1;
	group->capture_substream = NULL;
	spin_unlock_irq(&mfx->lock);
	return 0;
}

static int snd_madifx_hwdep_dummy_op(struct snd_hwdep *hw, struct file *file)
{
	/* we have nothing to initialize but the call is required */
	return 0;
}

static int snd_madifx_hwdep_ioctl(struct snd_hwdep *hw, struct file *file,
		unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct mfx *mfx = hw->private_data;
	struct madifx_config info;
	struct madifx_status status;
#ifdef CONFIG_SND_MADIFX_BROKEN
	struct madifx_level_buffer *levels;
	struct madifx_mixer_ioctl mixer;
	unsigned long int s;
#endif /* CONFIG_SND_MADIFX_BROKEN */
	int i = 0;

	switch (cmd) {

#ifdef CONFIG_SND_MADIFX_BROKEN
	int row;

	case SNDRV_MADIFX_IOCTL_GET_LEVEL:

		levels = &(mfx->peak_rms);
		for (row = 1; row <= 5 ; row++) {
			int rms_index, peak_index;
			u32 *target_rms, *target_peak;

			switch (row) {
			case 1:
				rms_index = MADIFX_RD_RMS_IN;
				peak_index = MADIFX_RD_PEAK_IN;
				target_rms = levels->rms_in;
				target_peak = levels->peak_in;
				break;
			case 2:
				rms_index = MADIFX_RD_RMS_PLAY;
				peak_index = MADIFX_RD_PEAK_PLAY;
				target_rms = levels->rms_play;
				target_peak = levels->peak_play;
				break;
			case 3:
				rms_index = MADIFX_RD_RMS_OUT;
				peak_index = MADIFX_RD_PEAK_OUT;
				target_rms = levels->rms_out;
				target_peak = levels->peak_out;
				break;
			case 4:
				rms_index = MADIFX_RD_RMS_IN_PRE;
				peak_index = MADIFX_RD_PEAK_IN_PRE;
				target_rms = levels->rms_in_pre;
				target_peak = levels->peak_in_pre;
				break;
			default:
				rms_index = MADIFX_RD_RMS_OUT_PRE;
				peak_index = MADIFX_RD_PEAK_OUT_PRE;
				target_rms = levels->rms_out_pre;
				target_peak = levels->peak_out_pre;
				break;
			}

			for (i = 0; i < 2 * 256; i++)
				*(target_rms + i) =
					mfx->level_buffer[rms_index + i];

			for (i = 0; i < 256; i++)
				*(target_peak + i) =
					mfx->level_buffer[peak_index + i];
		}

		levels->speed = mfx->speedmode;

		s = copy_to_user(argp, levels,
				 sizeof(*levels);
		if (0 != s) {
			/* snd_printk(KERN_ERR "copy_to_user(.., .., %lu): %lu
			 [Levels]\n", sizeof(struct madifx_peak_rms), s);
			 */
			return -EFAULT;
		}

		madifx_write(mfx, MADIFX_START_LEVEL, 0);

		break;
#endif /* CONFIG_SND_MADIFX_BROKEN */


	case SNDRV_MADIFX_IOCTL_GET_CONFIG:

		memset(&info, 0, sizeof(info));
		spin_lock_irq(&mfx->lock);

		for (i = 0; i < ARRAY_SIZE(info.madi_tx_64); i++) {
			info.madi_tx_64[i] = madifx_read_toggle_setting(mfx,
					(MADIFX_madi1_tx_64ch << i));

			info.madi_smux[i] = madifx_read_toggle_setting(mfx,
					(MADIFX_madi1_smux << i));
		}

		info.wcterm = madifx_read_toggle_setting(mfx,
				MADIFX_WCK_TERM);

		info.wck48 = madifx_read_toggle_setting(mfx, MADIFX_WCK48);

		info.aespro = madifx_read_toggle_setting(mfx, MADIFX_PRO);

		info.redundancy_mode = madifx_read_toggle_setting(mfx,
				MADIFX_redundancy_mode);

		info.mirror_madi_out = madifx_read_toggle_setting(mfx,
				MADIFX_mirror_madi_out);


		spin_unlock_irq(&mfx->lock);
		if (copy_to_user(argp, &info, sizeof(info)))
			return -EFAULT;
		break;

	case SNDRV_MADIFX_IOCTL_GET_STATUS:
		memset(&status, 0, sizeof(status));

		status.card_type = mfx->io_type;

		status.clock_selection = madifx_get_clock_select(mfx);

		status.system_sample_rate =
			madifx_get_system_sample_rate(mfx);


		for (i = 0; i < ARRAY_SIZE(status.sync_check); i++) {
			status.sync_check[i] = madifx_sync_check(mfx, i);
			status.external_sample_rates[i] =
				HDSPM_bit2freq(
					madifx_external_freq_index(mfx, i));
		}

		for (i = 0; i < ARRAY_SIZE(status.madi_channelcount); i++) {
			status.madi_channelcount[i] =
				madifx_get_madichannelcount(mfx, i);
		}


		if (copy_to_user(argp, &status, sizeof(status)))
			return -EFAULT;


		break;

#ifdef CONFIG_SND_MADIFX_BROKEN
	case SNDRV_MADIFX_IOCTL_GET_MIXER:
		if (copy_from_user(&mixer, argp, sizeof(mixer)))
			return -EFAULT;
		if (copy_to_user((void __user *)mixer.mixer, mfx->newmixer,
					sizeof(*mixer.mixer)))
			return -EFAULT;
		break;
#endif /* CONFIG_SND_MADIFX_BROKEN */

	default:
		return -EINVAL;
	}
	return 0;
}

static const struct snd_pcm_ops snd_madifx_playback_ops = {
	.open = snd_madifx_playback_open,
	.close = snd_madifx_playback_release,
	.ioctl = snd_madifx_ioctl,
	.hw_params = snd_madifx_hw_params,
	.hw_free = snd_madifx_hw_free,
	.prepare = snd_madifx_prepare,
	.trigger = snd_madifx_trigger,
	.pointer = snd_madifx_hw_pointer,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 6, 0)
	.page = snd_pcm_sgbuf_ops_page,
#endif
};

static const struct snd_pcm_ops snd_madifx_capture_ops = {
	.open = snd_madifx_capture_open,
	.close = snd_madifx_capture_release,
	.ioctl = snd_madifx_ioctl,
	.hw_params = snd_madifx_hw_params,
	.hw_free = snd_madifx_hw_free,
	.prepare = snd_madifx_prepare,
	.trigger = snd_madifx_trigger,
	.pointer = snd_madifx_hw_pointer,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 6, 0)
	.page = snd_pcm_sgbuf_ops_page,
#endif
};

static int snd_madifx_create_hwdep(struct snd_card *card,
					    struct mfx *mfx)
{
	struct snd_hwdep *hw;
	int err;

	err = snd_hwdep_new(card, "MADIFX hwdep", 0, &hw);
	if (err < 0)
		return err;

	mfx->hwdep = hw;
	hw->private_data = mfx;
	strcpy(hw->name, "MADIFX hwdep interface");

	hw->ops.open = snd_madifx_hwdep_dummy_op;
	hw->ops.ioctl = snd_madifx_hwdep_ioctl;
	hw->ops.ioctl_compat = snd_madifx_hwdep_ioctl;
	hw->ops.release = snd_madifx_hwdep_dummy_op;

	return 0;
}


/*------------------------------------------------------------
   memory interface
 ------------------------------------------------------------*/

/*
 * Preallocate DMA pages for one PCM group.
 * Called once per group from snd_madifx_create_pcm_group().
 */
static int snd_madifx_preallocate_group(struct madifx_pcm_group *group)
{
/*
 * https://htrd.su/en/blog/2018/06/12/why_may_linux_driver_developer_hate_distros_like_centos/
 * Thank you for this blog post, saved my day!
 */
#ifdef RHEL_RELEASE_CODE
#define RH_NEW_AUDIO_STACK (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(7,5))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 0) && !defined(RH_NEW_AUDIO_STACK)
	int err;
#endif
	struct mfx *mfx = group->mfx;
	size_t wanted = group_dma_size(group);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 0) && !defined(RH_NEW_AUDIO_STACK)
	err =
#endif
	     snd_pcm_lib_preallocate_pages_for_all(group->pcm,
						   SNDRV_DMA_TYPE_DEV_SG,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
						   snd_dma_pci_data(mfx->pci),
#else
						   &mfx->pci->dev,
#endif
						   wanted, wanted);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 0) && !defined(RH_NEW_AUDIO_STACK)
	if (err < 0) {
		pr_debug("Could not preallocate %zd Bytes\n", wanted);
		return err;
	}
#endif

	pr_debug("Preallocated %zd Bytes for group type=%d stream=%d\n",
		 wanted, group->type, group->madi_stream);
	return 0;
}

/* ------------- ALSA Devices ---------------------------- */

/*
 * Create one PCM device for a single channel group.
 * Group 0 is AES (2ch), groups 1..24 are MADI streams (8ch each),
 * group 25 is Phones (2ch, playback only).
 */
static int snd_madifx_create_pcm_group(struct snd_card *card,
					struct madifx_pcm_group *group,
					int device_index)
{
	struct snd_pcm *pcm;
	int n_play, n_capt;
	char name[32];
	int err;

	n_play = 1;
	n_capt = (group->type == MADIFX_PCM_TYPE_PHONES) ? 0 : 1;

	switch (group->type) {
	case MADIFX_PCM_TYPE_AES:
		snprintf(name, sizeof(name), "RME MADI FX AES");
		break;
	case MADIFX_PCM_TYPE_PHONES:
		snprintf(name, sizeof(name), "RME MADI FX Phones");
		break;
	case MADIFX_PCM_TYPE_MADI: {
		int port   = group->madi_stream / 8 + 1;  /* 1,2,3 */
		int stream = group->madi_stream % 8;       /* 0..7  */
		snprintf(name, sizeof(name), "RME MADI FX MADI%d Ch%d-%d",
			 port, stream * 8 + 1, stream * 8 + 8);
		break;
	}
	}

	err = snd_pcm_new(card, name, device_index, n_play, n_capt, &pcm);
	if (err < 0)
		return err;

	group->pcm    = pcm;
	pcm->private_data = group;   /* chip pointer is the group, not mfx */
	strscpy(pcm->name, name, sizeof(pcm->name));

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
			&snd_madifx_playback_ops);
	if (n_capt)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,
				&snd_madifx_capture_ops);

	pcm->info_flags = SNDRV_PCM_INFO_JOINT_DUPLEX;

	return snd_madifx_preallocate_group(group);
}

static int snd_madifx_create_pcm(struct snd_card *card, struct mfx *mfx)
{
	int i, err, device_index = 0;

	/* Group 0: AES (2ch, in+out) */
	mfx->groups[0].mfx        = mfx;
	mfx->groups[0].type       = MADIFX_PCM_TYPE_AES;
	mfx->groups[0].madi_stream = 0;
	mfx->groups[0].n_channels  = 2;
	err = snd_madifx_create_pcm_group(card, &mfx->groups[0], device_index++);
	if (err < 0)
		return err;

	/* Groups 1..24: MADI streams 0..23 (8ch each, in+out) */
	for (i = 0; i < MADIFX_NUM_MADI_STREAMS; i++) {
		struct madifx_pcm_group *g = &mfx->groups[1 + i];
		g->mfx        = mfx;
		g->type       = MADIFX_PCM_TYPE_MADI;
		g->madi_stream = i;
		g->n_channels  = 8;
		err = snd_madifx_create_pcm_group(card, g, device_index++);
		if (err < 0)
			return err;
	}

	/* Group 25: Phones (2ch, playback only) */
	mfx->groups[25].mfx        = mfx;
	mfx->groups[25].type       = MADIFX_PCM_TYPE_PHONES;
	mfx->groups[25].madi_stream = 0;
	mfx->groups[25].n_channels  = 2;
	err = snd_madifx_create_pcm_group(card, &mfx->groups[25], device_index++);
	if (err < 0)
		return err;

	return 0;
}

static inline void snd_madifx_initialize_midi_flush(struct mfx *mfx)
{
	int i;

	for (i = 0; i < mfx->midiPorts; i++)
		snd_madifx_flush_midi_input(mfx, i);
}

static int snd_madifx_create_alsa_devices(struct snd_card *card,
						   struct mfx *mfx)
{
	int err, i;

	pr_debug("Create card...\n");
	err = snd_madifx_create_pcm(card, mfx);
	if (err < 0)
		return err;

	i = 0;
	while (i < mfx->midiPorts) {
		err = snd_madifx_create_midi(card, mfx, i);
		if (err < 0)
			return err;
		i++;
	}

	err = snd_madifx_create_controls(card, mfx);
	if (err < 0)
		return err;

	err = snd_madifx_create_hwdep(card, mfx);
	if (err < 0)
		return err;

	pr_debug("proc init...\n");
	snd_madifx_proc_init(mfx);

	{
		int gi;
		mfx->system_sample_rate = -1;
		mfx->last_external_sample_rate = -1;
		mfx->last_internal_sample_rate = -1;
		mfx->running_count = 0;
		for (gi = 0; gi < MADIFX_NUM_PCM_GROUPS; gi++) {
			mfx->groups[gi].playback_pid       = -1;
			mfx->groups[gi].capture_pid        = -1;
			mfx->groups[gi].playback_substream = NULL;
			mfx->groups[gi].capture_substream  = NULL;
		}
	}

	pr_debug("Set defaults...\n");
	err = snd_madifx_set_defaults(mfx);
	if (err < 0)
		return err;

	pr_debug("Update mixer controls...\n");
#if 0
	/* FIXME: MADI FX disable, old mixer is broken */
	madifx_update_simple_mixer_controls(mfx);
#endif

	pr_debug("Initializeing complete ???\n");

	err = snd_card_register(card);
	if (err < 0) {
		dev_err(mfx->card->dev,
			    "MADIFX: error registering card\n");
		return err;
	}

	pr_debug("... yes now\n");

	return 0;
}

static int snd_madifx_create(struct snd_card *card,
		struct mfx *mfx)
{

	struct pci_dev *pci = mfx->pci;
	int err;
	int i;
	unsigned long io_extent;

	mfx->irq = -1;
	mfx->card = card;

	spin_lock_init(&mfx->lock);
	INIT_WORK(&mfx->midi_work, madifx_midi_work);

	pci_read_config_word(mfx->pci,
			PCI_CLASS_REVISION, &mfx->firmware_rev);

	strcpy(card->mixername, "Xilinx FPGA");
	strcpy(card->driver, "MADIFX");

	switch (mfx->firmware_rev) {
	case HDSPM_MADIFX_REV:
		mfx->io_type = MADIFX;
		mfx->card_name = "RME MADI FX";
		mfx->midiPorts = 4;
		break;
	default:
		dev_err(mfx->card->dev,
			"MADIFX: unknown firmware revision %x\n",
			mfx->firmware_rev);
		return -ENODEV;
	}

	err = pcim_enable_device(pci);
	if (err < 0)
		return err;

	pci_set_master(mfx->pci);

	err = pcim_iomap_regions(pci, 1 << 0, "mfx");
	if (err < 0)
		return err;

	mfx->port = pci_resource_start(pci, 0);
	io_extent = pci_resource_len(pci, 0);

	mfx->iobase = pcim_iomap_table(pci)[0];
	pr_debug("remapped region (0x%lx) 0x%lx-0x%lx\n",
			(unsigned long)mfx->iobase, mfx->port,
			mfx->port + io_extent - 1);

	if (devm_request_irq(&pci->dev, pci->irq, snd_madifx_interrupt,
			IRQF_SHARED, KBUILD_MODNAME, mfx)) {
		dev_err(mfx->card->dev,
			"MADIFX: unable to use IRQ %d\n", pci->irq);
		return -EBUSY;
	}

	pr_debug("use IRQ %d\n", pci->irq);

	mfx->irq = pci->irq;

	pr_debug("kmalloc Mixer memory of %zd Bytes\n",
			sizeof(*mfx->newmixer));

	mfx->newmixer = devm_kzalloc(&pci->dev, sizeof(*mfx->newmixer), GFP_KERNEL);
	if (!mfx->newmixer)
		return -ENOMEM;

	/* Allocate the dmaPageTable CPU mirror */
	mfx->dmaPageTable = devm_kzalloc(&pci->dev,
			sizeof(dma_addr_t) * MADIFX_MAX_PAGE_TABLE_SIZE,
			GFP_KERNEL);
	if (!mfx->dmaPageTable) {
		dev_err(mfx->card->dev,
			"MADIFX: unable to allocate dmaPageTable\n");
		return -ENOMEM;
	}

	/*
	 * Allocate a small coherent "dummy" page. All page-table entries are
	 * initialised to point here so the hardware always reads from a valid
	 * DMA address, even for channels that have no active stream.
	 */
	mfx->dummy_dma_area = dmam_alloc_coherent(&pci->dev,
						  MADIFX_HW_PAGE_SIZE,
						  &mfx->dummy_dma_addr,
						  GFP_KERNEL);
	if (!mfx->dummy_dma_area) {
		dev_err(mfx->card->dev,
			"MADIFX: unable to allocate dummy DMA page\n");
		return -ENOMEM;
	}
	memset(mfx->dummy_dma_area, 0, MADIFX_HW_PAGE_SIZE);

	/* Pre-fill the entire hardware page table with the dummy address */
	for (i = 0; i < MADIFX_MAX_PAGE_TABLE_SIZE; i++) {
		mfx->dmaPageTable[i] = mfx->dummy_dma_addr;
		madifx_write(mfx, MADIFX_PAGE_ADDRESS_LIST + 4 * i,
			     mfx->dummy_dma_addr);
	}

	/* This initialises the mixer to a static 1:1 routing.
	 *
	 * The new mixer is a list of tuples (C, g), where C represents
	 * a point in the matrix (x,y) encoded into a single integer and
	 * g the corresponding gain value.
	 *
	 * For a given channel number i,
	 *    256 + i encodes the input channel
	 * and
	 *    i << 9 encodes the output channel.
	 *
	 * C is then encoded as (256 + x) | (y << 9)
	 */

	for (i = 0; i < MADIFX_NUM_OUTPUT_GAINS; i++)
		mfx->newmixer->output_gain[i] = 0x9000;

	for (i = 0; i < MADIFX_LIST_LENGTH; i++) {
		mfx->newmixer->listCh[i] = 0;
		mfx->newmixer->listVol[i] = 0;
	}

	for (i = 0; i < 196; i++) {
		mfx->newmixer->listCh[i] = (256 + i) | (i << 9);
		mfx->newmixer->listVol[i] = 32768+(32768>>3);
	}

	/* Of course, the data has to be written to the device before
	 * something can happen.
	 */
	for (i = 0; i < MADIFX_LIST_LENGTH; i++) {
		madifx_write(mfx, MADIFX_MIXER_LIST_CH + (4 * i),
			     mfx->newmixer->listCh[i]);
		madifx_write(mfx, MADIFX_MIXER_LIST_VOL + (4 * i),
			     mfx->newmixer->listVol[i]);
	}

	for (i = 0; i < MADIFX_NUM_OUTPUT_GAINS; i++) {
		madifx_write(mfx, MADIFX_WR_OUTPUT_GAIN + (4 * i),
			     mfx->newmixer->output_gain[i]);
	}


	mfx->port_names_in = NULL;
	mfx->port_names_out = NULL;

	switch (mfx->io_type) {
	case MADIFX:
		mfx->ss_in_channels = MADIFX_SS_IN_CHANNELS;
		mfx->ds_in_channels = MADIFX_DS_IN_CHANNELS;
		mfx->qs_in_channels = MADIFX_QS_IN_CHANNELS;
		mfx->ss_out_channels = MADIFX_SS_OUT_CHANNELS;
		mfx->ds_out_channels = MADIFX_DS_OUT_CHANNELS;
		mfx->qs_out_channels = MADIFX_QS_OUT_CHANNELS;
		/* FIXME: portnames and stuff missing */
		break;
	}


	/* texts */
	switch (mfx->io_type) {
	/* Keep the switch if MFXT will be different */
	case MADIFX:
		mfx->texts_clocksource = texts_madifx_clock_source;
		mfx->texts_clocksource_items =
			ARRAY_SIZE(texts_madifx_clock_source);
		break;
	}

	snprintf(card->id, sizeof(card->id), "MADIFX");
	snd_card_set_id(card, card->id);

	pr_debug("create alsa devices.\n");
	err = snd_madifx_create_alsa_devices(card, mfx);
	if (err < 0)
		return err;

	snd_madifx_initialize_midi_flush(mfx);

	return 0;
}


static void snd_madifx_card_free(struct snd_card *card)
{
	struct mfx *mfx = card->private_data;

	if (mfx->port) {
		cancel_work_sync(&mfx->midi_work);

		/* stop th audio, and cancel all interrupts */
		mfx->control_register &=
		    ~(MADIFX_START | MADIFX_IE_AUDIO |
		      MADIFX_IEN0 | MADIFX_IEN1 |
		      MADIFX_IEN2 | MADIFX_IEN3);
		madifx_write(mfx, MADIFX_CONTROL_REG,
			    mfx->control_register);
		madifx_write(mfx, MADIFX_START_LEVEL, 0);
	}

#ifdef CONFIG_SND_MADIFX_BROKEN
	snd_dma_free_pages(&(mfx->dmaLevelBuffer));
#endif
}


static int snd_madifx_probe(struct pci_dev *pci,
				     const struct pci_device_id *pci_id)
{
	static int dev;
	struct mfx *mfx;
	struct snd_card *card;
	int err;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}

	err = snd_devm_card_new(&pci->dev, index[dev], id[dev],
			THIS_MODULE, sizeof(*mfx), &card);
	if (err < 0)
		return err;

	mfx = card->private_data;
	card->private_free = snd_madifx_card_free;
	mfx->dev = dev;
	mfx->pci = pci;

	err = snd_madifx_create(card, mfx);
	if (err < 0) {
		goto error;
	}

	snprintf(card->shortname, sizeof(card->shortname),
			"%s_%x",
			mfx->card_name,
			mfx->serial);
	snprintf(card->longname, sizeof(card->shortname),
			"%s S/N 0x%x at 0x%lx, irq %d",
			mfx->card_name,
			mfx->serial,
			mfx->port, mfx->irq);

	err = snd_card_register(card);
	if (err < 0) {
		goto error;
	}

	pci_set_drvdata(pci, card);

	madifx_write(mfx, MADIFX_START_LEVEL, 1);

	dev++;
	return 0;

error:
	snd_card_free(card);
	return err;
}

static struct pci_driver madifx_driver = {
	.name = KBUILD_MODNAME,
	.id_table = snd_madifx_ids,
	.probe = snd_madifx_probe,
};

module_pci_driver(madifx_driver);
