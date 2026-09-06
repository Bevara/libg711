/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / G.711 decoder filter, covering both companding
 *  laws of ITU-T G.711: A-law (used on European telephony) and mu-law (North
 *  America and Japan).
 *
 *  A link in a chain, like the Speex filter: something upstream emits a
 *  GF_CODECID_ALAW or GF_CODECID_MULAW pid and this filter expands the
 *  companded bytes. That upstream is rfg711, shipped alongside in this module -
 *  the same pairing libaif uses for rfaiff and aiffdec. rfpcm was taught the
 *  two G.711 WAVE format tags as well, so its output works here too.
 *
 *  No third-party library: G.711 is a pair of piecewise-logarithmic mappings
 *  from 8 bits to 14 (A-law) or 13 (mu-law) bits, small enough that the
 *  expansion tables are built here at startup from the definition in the
 *  standard rather than pulled in from elsewhere. That also keeps the two
 *  tables exact, which a hand-copied table would not guarantee.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;
	u32 codecid, sample_rate, nb_chan;
	s16 table[256];
} GF_G711DecCtx;

/* A-law, as defined in G.711 table 1: the byte is stored with every other bit
 * inverted (the 0x55 toggle), then read as sign, 3-bit exponent and 4-bit
 * mantissa. The result is a 13-bit magnitude, shifted up to fill 16 bits. */
static s16 g711_alaw_to_s16(u8 a)
{
	int sign, exponent, mantissa, sample;

	a ^= 0x55;
	sign = a & 0x80;
	exponent = (a & 0x70) >> 4;
	mantissa = a & 0x0F;

	sample = (mantissa << 4) + 8; /* + 8 puts the value mid-interval */
	if (exponent != 0)
		sample = (sample + 256) << (exponent - 1);

	return (s16)(sign ? -sample : sample);
}

/* mu-law, G.711 table 2: the byte is stored inverted, then read as sign,
 * 3-bit exponent and 4-bit mantissa over a 33-unit bias. */
static s16 g711_ulaw_to_s16(u8 u)
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

static void g711dec_build_table(GF_G711DecCtx *ctx)
{
	u32 i;
	for (i = 0; i < 256; i++)
	{
		ctx->table[i] = (ctx->codecid == GF_CODECID_ALAW)
		                    ? g711_alaw_to_s16((u8)i)
		                    : g711_ulaw_to_s16((u8)i);
	}
}

static GF_Err g711dec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *p;
	GF_G711DecCtx *ctx = gf_filter_get_udta(filter);

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

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_CODECID);
	if (!p || ((p->value.uint != GF_CODECID_ALAW) && (p->value.uint != GF_CODECID_MULAW)))
		return GF_NOT_SUPPORTED;
	ctx->codecid = p->value.uint;
	g711dec_build_table(ctx);

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_SAMPLE_RATE);
	ctx->sample_rate = (p && p->value.uint) ? p->value.uint : 8000;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_NUM_CHANNELS);
	ctx->nb_chan = (p && p->value.uint) ? p->value.uint : 1;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(ctx->sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(ctx->nb_chan));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT,
	                           &PROP_LONGUINT((ctx->nb_chan == 2)
	                                              ? (GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT)
	                                              : GF_AUDIO_CH_FRONT_CENTER));
	/* one companded byte in, two bytes out, so the bitrate doubles */
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_BITRATE,
	                           &PROP_UINT(ctx->sample_rate * ctx->nb_chan * 16));

	return GF_OK;
}

static GF_Err g711dec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size, i;
	GF_G711DecCtx *ctx = gf_filter_get_udta(filter);

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
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data || !size)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OK;
	}

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, size * 2, &output);
	if (!dst_pck)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	{
		s16 *out = (s16 *)output;
		for (i = 0; i < size; i++)
			out[i] = ctx->table[data[i]];
	}

	gf_filter_pck_merge_properties(pck, dst_pck);
	gf_filter_pck_set_cts(dst_pck, gf_filter_pck_get_cts(pck));
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_drop_packet(ctx->ipid);
	return GF_OK;
}

static void g711dec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability G711DecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_ALAW),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_MULAW),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister G711DecoderRegister = {
	.name = "g711dec",
	GF_FS_SET_DESCRIPTION("G.711 A-law and mu-law decoder")
		GF_FS_SET_HELP("This filter decodes ITU-T G.711 audio, both the A-law and the mu-law companding laws, to raw 16-bit PCM. It takes a GF_CODECID_ALAW or GF_CODECID_MULAW pid, which rfg711 produces from a WAVE file whose format tag is 6 or 7.")
			.private_size = sizeof(GF_G711DecCtx),
	SETCAPS(G711DecCaps),
	.configure_pid = g711dec_configure_pid,
	.process = g711dec_process,
	.finalize = g711dec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE g711dec_register(GF_FilterSession *session)
{
	return &G711DecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_g711dec(void) {
    gf_filter_auto_register("g711dec", g711dec_register);
}
