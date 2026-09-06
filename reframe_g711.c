/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / G.711 reframer: it reads a WAVE file whose
 *  format tag says G.711 (6 for A-law, 7 for mu-law) and emits the companded
 *  bytes on a GF_CODECID_ALAW or GF_CODECID_MULAW pid, which dec_g711.c then
 *  expands to 16-bit samples.
 *
 *  Same shape as libaif, which pairs rfaiff with aiffdec in one module. The
 *  general-purpose WAVE reframer, rfpcm, was taught the two G.711 format tags
 *  as well, so g711dec will happily take its output too - but rfpcm's WAVE
 *  path does not currently connect in this build (a plain 16-bit PCM file
 *  fails the same way, and its test in test-player is commented out), which is
 *  why this reframer exists rather than relying on it.
 *
 *  The parsing here is deliberately narrow: anything that is not 8-bit G.711
 *  is refused, so every other WAVE flavour is left to rfpcm.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool header_done;
} GF_ReframeG711Ctx;

static GF_Err rfg711_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_ReframeG711Ctx *ctx = (GF_ReframeG711Ctx *)gf_filter_get_udta(filter);

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
	ctx->header_done = GF_FALSE;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	/* GPAC resolves the graph from the output properties before any data
	 * flows, so plausible values go out now and process() corrects them once
	 * the fmt chunk has been read. A-law is the arbitrary choice of the two;
	 * whichever the file turns out to be is set below. */
	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_ALAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(8000));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(8000));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(1));

	return GF_OK;
}

/* Walks a RIFF/WAVE header far enough to answer three questions: is this
 * G.711, at what rate and how many channels, and where the sample data starts.
 * Returns 0 for anything it does not handle. */
static u32 rfg711_parse_wav(const u8 *data, u32 size, u32 *codecid, u32 *sr, u32 *ch, u32 *data_size)
{
	u32 pos = 12;
	Bool have_fmt = GF_FALSE;

	if ((size < 44) || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4))
		return 0;

	while (pos + 8 <= size)
	{
		u32 csize = data[pos + 4] | (data[pos + 5] << 8) | (data[pos + 6] << 16) | ((u32)data[pos + 7] << 24);
		const u8 *body = data + pos + 8;

		if (!memcmp(data + pos, "fmt ", 4) && (csize >= 16) && (pos + 8 + 16 <= size))
		{
			u32 tag = body[0] | (body[1] << 8);
			u32 bps = body[14] | (body[15] << 8);
			if (tag == 6)
				*codecid = GF_CODECID_ALAW;
			else if (tag == 7)
				*codecid = GF_CODECID_MULAW;
			else
				return 0;
			if (bps != 8)
				return 0;
			*ch = body[2] | (body[3] << 8);
			*sr = body[4] | (body[5] << 8) | (body[6] << 16) | ((u32)body[7] << 24);
			if (!*ch || !*sr)
				return 0;
			have_fmt = GF_TRUE;
		}
		else if (!memcmp(data + pos, "data", 4))
		{
			u32 start = pos + 8;
			if (!have_fmt)
				return 0;
			/* a truncated file still plays, up to what is actually there */
			*data_size = (csize <= size - start) ? csize : (size - start);
			return start;
		}
		/* RIFF chunks are word-aligned */
		pos += 8 + csize + (csize & 1);
	}
	return 0;
}

static GF_Err rfg711_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size, off, codecid = 0, sr = 0, ch = 0, dsize = 0;
	GF_ReframeG711Ctx *ctx = (GF_ReframeG711Ctx *)gf_filter_get_udta(filter);

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

	off = rfg711_parse_wav(data, size, &codecid, &sr, &ch, &dsize);
	if (!off)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("[RFG711] Not an 8-bit G.711 WAVE file\n"));
		return GF_NOT_SUPPORTED;
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(codecid));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(sr));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(sr));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(ch));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT,
	                           &PROP_LONGUINT((ch == 2)
	                                              ? (GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT)
	                                              : GF_AUDIO_CH_FRONT_CENTER));

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, dsize, &output);
	if (!dst_pck)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	memcpy(output, data + off, dsize);
	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_drop_packet(ctx->ipid);
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static const GF_FilterCapability ReframeG711Caps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "wav"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/wav|audio/x-wav"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_ALAW),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_MULAW),
};

GF_FilterRegister ReframeG711Register = {
	.name = "rfg711",
	GF_FS_SET_DESCRIPTION("G.711 WAVE reframer")
		GF_FS_SET_HELP("This filter reads a WAVE file whose format tag is 6 (A-law) or 7 (mu-law) and emits the companded samples for g711dec to decode.")
			.private_size = sizeof(GF_ReframeG711Ctx),
	SETCAPS(ReframeG711Caps),
	.configure_pid = rfg711_configure_pid,
	.process = rfg711_process,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE rfg711_register(GF_FilterSession *session)
{
	return &ReframeG711Register;
}

#include "filter_register.h"
__attribute__((constructor))
void register_rfg711(void) {
    gf_filter_auto_register("rfg711", rfg711_register);
}
