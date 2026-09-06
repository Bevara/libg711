/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / Sun-NeXT ".au" decoder filter.
 *
 *  The G.711/G.721/G.723 family has no container of its own, and a raw stream
 *  does not say which member of it you are holding: G.723 at 24 kbit/s packs 3
 *  bits per sample, G.721 4, G.723 at 40 kbit/s 5, and nothing in the bytes
 *  distinguishes them. The ".au" file - Sun's own format, the one that shipped
 *  alongside this code - does say, in a single header word, and it is what the
 *  reference material for these codecs comes in. So this filter reads the
 *  container rather than guessing at a bare bitstream:
 *
 *     ".snd", data offset, data size, encoding, sample rate, channels
 *
 *  and covers the five encodings that make up the row:
 *
 *     1  mu-law              G.711, 8 bits in, 14-bit magnitude out
 *     23 ADPCM 32 kbit/s     G.721,  4 bits per sample
 *     25 ADPCM 24 kbit/s     G.723,  3 bits per sample
 *     26 ADPCM 40 kbit/s     G.723,  5 bits per sample
 *     27 A-law               G.711, 8 bits in, 13-bit magnitude out
 *
 *  The two companding laws are expanded here, from their definition in the
 *  standard, the same way dec_g711.c does it. The three ADPCM ones go through
 *  Sun's public-domain reference implementation, which libsndfile carries in
 *  src/G72x with a block-oriented wrapper - that wrapper is what makes it
 *  usable without reimplementing the bit packing, which differs per rate.
 *
 *  Linear PCM encodings (2, 3, 4, 5) are deliberately not handled: rfpcm is
 *  the filter for those.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <g72x.h>

#define AU_MAGIC          GF_4CC('.', 's', 'n', 'd')
#define AU_ENC_MULAW      1
#define AU_ENC_ADPCM_G721 23
#define AU_ENC_ADPCM_G723_24 25
#define AU_ENC_ADPCM_G723_40 26
#define AU_ENC_ALAW       27

typedef struct
{
	GF_FilterPid *ipid, *opid;
} GF_AUDecCtx;

/* A-law, G.711 table 1: every other bit is stored inverted (the 0x55 toggle),
 * then the byte reads as sign, 3-bit exponent, 4-bit mantissa. */
static s16 au_alaw_to_s16(u8 a)
{
	int sign, exponent, mantissa, sample;

	a ^= 0x55;
	sign = a & 0x80;
	exponent = (a & 0x70) >> 4;
	mantissa = a & 0x0F;

	sample = (mantissa << 4) + 8; /* + 8 puts the value mid-interval */
	if (exponent != 0)
		sample = (sample + 256) << (exponent - 1);

	/* A-law inverts the polarity convention: the sign bit is 1 for POSITIVE
	 * values, the opposite of mu-law. Getting this backwards produces audio
	 * that is exactly the negation of the right answer - which sounds
	 * identical, and only a sample-level comparison catches it. */
	return (s16)(sign ? sample : -sample);
}

/* mu-law, G.711 table 2: the byte is stored inverted, then read as sign,
 * 3-bit exponent and 4-bit mantissa over a 33-unit bias. */
static s16 au_ulaw_to_s16(u8 u)
{
	int sign, exponent, mantissa, sample;

	u = ~u;
	sign = u & 0x80;
	exponent = (u & 0x70) >> 4;
	mantissa = u & 0x0F;

	sample = ((mantissa << 3) + 0x84) << exponent;
	sample -= 0x84;

	return (s16)(sign ? -sample : sample);
}

static GF_Err audec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_AUDecCtx *ctx = (GF_AUDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	/* The rate and channel count come from the header, in process(); what is
	 * fixed here is that the output is 16-bit linear. */
	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));

	return GF_OK;
}

static u32 au_rd32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static GF_Err audec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	const u8 *data;
	u8 *output;
	u32 size, offset, data_size, encoding, rate, channels, nb_samples;
	int bits_per_sample = 0;
	GF_AUDecCtx *ctx = (GF_AUDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = gf_filter_pck_get_data(pck, &size);
	if (!data || (size < 24))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AUDec] File too short to hold a header\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	if (au_rd32(data) != AU_MAGIC)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AUDec] Not a Sun/NeXT .au file (no \".snd\" magic)\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	offset = au_rd32(data + 4);
	data_size = au_rd32(data + 8);
	encoding = au_rd32(data + 12);
	rate = au_rd32(data + 16);
	channels = au_rd32(data + 20);

	if ((offset < 24) || (offset > size))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AUDec] Header says the data starts at %u, past the end of a %u byte file\n", offset, size));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	/* 0xFFFFFFFF means "unknown", and writers that stream do use it. */
	if (!data_size || (data_size == 0xFFFFFFFF) || (offset + data_size > size))
		data_size = size - offset;

	if (!rate) rate = 8000;
	if (!channels) channels = 1;
	if (channels != 1)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AUDec] %u channels: these codecs are telephony ones and this filter only handles mono\n", channels));
		return GF_NOT_SUPPORTED;
	}

	switch (encoding)
	{
	case AU_ENC_MULAW:
	case AU_ENC_ALAW:
		break;
	case AU_ENC_ADPCM_G721:
		bits_per_sample = 4;
		break;
	case AU_ENC_ADPCM_G723_24:
		bits_per_sample = 3;
		break;
	case AU_ENC_ADPCM_G723_40:
		bits_per_sample = 5;
		break;
	default:
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AUDec] Encoding %u is not one this filter covers (1 mu-law, 23 G.721, 25 G.723-24, 26 G.723-40, 27 A-law; linear PCM is rfpcm's job)\n", encoding));
		return GF_NOT_SUPPORTED;
	}

	if (bits_per_sample)
	{
		/* ADPCM: Sun's reference implementation, driven a block at a time.
		 * A block is 480 bits whatever the rate, so the byte and sample
		 * counts differ per codec and g72x_reader_init reports both. */
		int blocksize = 0, samples_per_block = 0;
		u32 nb_blocks, i;
		struct g72x_state *state = g72x_reader_init(bits_per_sample, &blocksize, &samples_per_block);

		if (!state || (blocksize <= 0) || (samples_per_block <= 0))
		{
			if (state) free(state);
			gf_filter_pid_drop_packet(ctx->ipid);
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AUDec] Could not start the G.72x decoder\n"));
			return GF_NOT_SUPPORTED;
		}

		nb_blocks = data_size / (u32)blocksize;
		if (!nb_blocks)
		{
			free(state);
			gf_filter_pid_drop_packet(ctx->ipid);
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AUDec] No complete block of %d bytes in %u bytes of data\n", blocksize, data_size));
			return GF_NON_COMPLIANT_BITSTREAM;
		}
		nb_samples = nb_blocks * (u32)samples_per_block;

		dst_pck = gf_filter_pck_new_alloc(ctx->opid, nb_samples * 2, &output);
		if (!dst_pck)
		{
			free(state);
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_OUT_OF_MEM;
		}
		for (i = 0; i < nb_blocks; i++)
		{
			g72x_decode_block(state, data + offset + (size_t)i * blocksize,
			                  (short *)(output + (size_t)i * samples_per_block * 2));
		}
		free(state);
	}
	else
	{
		u32 i;
		s16 *out;

		nb_samples = data_size;
		dst_pck = gf_filter_pck_new_alloc(ctx->opid, nb_samples * 2, &output);
		if (!dst_pck)
		{
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_OUT_OF_MEM;
		}
		out = (s16 *)output;
		if (encoding == AU_ENC_ALAW)
			for (i = 0; i < nb_samples; i++) out[i] = au_alaw_to_s16(data[offset + i]);
		else
			for (i = 0; i < nb_samples; i++) out[i] = au_ulaw_to_s16(data[offset + i]);
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(channels));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_LONGUINT(GF_AUDIO_CH_FRONT_CENTER));

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_duration(dst_pck, nb_samples);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_drop_packet(ctx->ipid);
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void audec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability AUDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "au|snd"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/basic|audio/x-au"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister AUDecoderRegister = {
	.name = "audec",
	GF_FS_SET_DESCRIPTION("Sun/NeXT .au decoder: G.711 mu-law and A-law, G.721 and G.723 ADPCM")
		GF_FS_SET_HELP("This filter decodes Sun/NeXT .au files carrying G.711 mu-law (encoding 1) or A-law (27), or G.721 32 kbit/s (23) and G.723 24 or 40 kbit/s (25, 26) ADPCM, to raw 16-bit PCM. Linear PCM .au files are rfpcm's job.")
			.private_size = sizeof(GF_AUDecCtx),
	SETCAPS(AUDecCaps),
	.configure_pid = audec_configure_pid,
	.process = audec_process,
	.finalize = audec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE audec_register(GF_FilterSession *session)
{
	return &AUDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_audec(void) {
    gf_filter_auto_register("audec", audec_register);
}
