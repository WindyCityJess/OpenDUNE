/** @file src/audio/dsp_sdl.c SDL implementation of the DSP. */

#include <assert.h>
#include <stdio.h>
#include <SDL.h>
#include "types.h"
#include "../os/endian.h"
#include "../os/error.h"

#include "dsp.h"

#define MIX_OUTPUT_FORMAT	AUDIO_U8
#define MIX_OUTPUT_FREQUENCY  22050
#define MIX_OUTPUT_CHANNELS	2
#define MIX_BUFFER_SIZE 1024

static uint8 *s_buffer;
static uint32 s_bufferLen;
static uint8 s_status;
static uint8 *s_data;		// allocated buffer
static uint32 s_dataLen;	// size of the allocated buffer

static SDL_AudioSpec s_spec;

static void DSP_Callback(void *userdata, Uint8 *stream, int len)
{
	VARIABLE_NOT_USED(userdata);

	if (s_status == 0 || s_bufferLen == 0 || s_buffer == NULL) {
		/* no more sample to play : */
		memset(stream, s_spec.silence, len);	/* fill buffer with silence (MiSTer: the device format's, not 8-bit's) */
		SDL_PauseAudio(1);	/* stop playback */
		return;
	}

	if (len <= (int)s_bufferLen) {
		memcpy(stream, s_buffer, len);
		s_bufferLen -= len;
		s_buffer += len;
	} else {
		memcpy(stream, s_buffer, s_bufferLen);
		memset(stream + s_bufferLen, s_spec.silence, len - s_bufferLen);	/* fill buffer end with silence */
		s_bufferLen = 0;
		s_buffer = NULL;
		s_status = 0;
	}
}

void DSP_Stop(void)
{
	SDL_PauseAudio(1);

	s_bufferLen = 0;
	s_buffer = NULL;
	s_status = 0;
}

void DSP_Uninit(void)
{
	if (SDL_WasInit(SDL_INIT_AUDIO) == 0) return;

	DSP_Stop();
	SDL_CloseAudio();

	free(s_data); s_data = NULL;
	s_dataLen = 0;

	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool DSP_Init(void)
{
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;

	s_spec.freq     = MIX_OUTPUT_FREQUENCY;
	s_spec.format   = MIX_OUTPUT_FORMAT;
	s_spec.channels = MIX_OUTPUT_CHANNELS;
	s_spec.samples  = MIX_BUFFER_SIZE;
	s_spec.callback = DSP_Callback;

	s_bufferLen = 0;
	s_buffer = NULL;
	s_status = 0;
	s_data = NULL;
	s_dataLen = 0;

	if (SDL_OpenAudio(&s_spec, &s_spec) != 0) return false;

	return (SDL_GetAudioStatus() != 0);
}

/**
 * In Dune2, the frequency of the VOC files are all over the place. SDL really
 *  dislikes it when we close/open the audio driver a lot. So, we convert all
 *  audio to one frequency, which resolves all issues.
 */

/* MiSTer (patch_opendune_perf.py): the device is 16-bit stereo (the hybrid SDL, 44100): the VOC's 8-bit mono
 * samples linearly interpolated to it, a 2 ms ramp in and out (the first and last sample of a VOC are not always
 * at 0x80: a step there is a click). SDL_ConvertAudio's windowed-sinc resampler took tens of ms a voice on this
 * ARM, inside the game loop. The result swaps into s_data; the other buffer keeps the next VOC's input. */
static uint8 *s_data2 = NULL;
static uint32 s_dataLen2 = 0;

static bool DSP_ConvertAudio_Mister(uint32 freq)
{
	uint32 inLen = s_bufferLen, outFrames, i, step, pos, ramp;
	const uint8 *in;
	int16 *out;
	uint8 *tmp; uint32 tmpLen;

	if (s_spec.format != AUDIO_S16LSB || s_spec.channels != 2 || s_spec.freq == 0 || freq == 0 || inLen < 2) return false;
	step = (uint32)(((unsigned long long)freq << 16) / s_spec.freq);          /* input samples an output frame, 16.16 */
	if (step == 0) return false;
	outFrames = (uint32)(((unsigned long long)(inLen - 1) << 16) / step);
	if (outFrames == 0) return false;
	if (s_dataLen2 < outFrames * 4) {
		s_data2 = realloc(s_data2, outFrames * 4);
		s_dataLen2 = outFrames * 4;
	}
	in = s_data; out = (int16 *)s_data2;
	ramp = s_spec.freq / 500;
	if (ramp * 2 > outFrames) ramp = outFrames / 2;
	for (i = 0, pos = 0; i < outFrames; i++, pos += step) {
		uint32 idx = pos >> 16, frac = pos & 0xffff;
		int32 a = (int32)in[idx] - 128, b = (int32)in[idx + 1] - 128;
		int32 v = (a * (int32)(0x10000 - frac) + b * (int32)frac) >> 8;    /* -32768 .. 32512 */
		if (i < ramp) v = v * (int32)i / (int32)ramp;
		else if (outFrames - i <= ramp) v = v * (int32)(outFrames - i) / (int32)ramp;
		*out++ = (int16)v; *out++ = (int16)v;
	}
	tmp = s_data; s_data = s_data2; s_data2 = tmp;
	tmpLen = s_dataLen; s_dataLen = s_dataLen2; s_dataLen2 = tmpLen;
	s_bufferLen = outFrames * 4;
	return true;
}

static void DSP_ConvertAudio(uint32 freq)
{
	SDL_AudioCVT cvt;
	int r;

	if (DSP_ConvertAudio_Mister(freq)) return;

	r = SDL_BuildAudioCVT(&cvt, AUDIO_U8, 1, freq, s_spec.format, s_spec.channels, s_spec.freq);
	if (r == 0) {
		// no conversion is needed
		return;
	} else if (r < 0) {
		Error("SDL_BuildAudioCVT(%p, 0x%04x, 1, %d, 0x%04x, %u, %d): %s\n",
		      &cvt, AUDIO_U8, freq,  s_spec.format, s_spec.channels, s_spec.freq, SDL_GetError());
		return;
	}

	// Setup for conversion.
	cvt.len = s_bufferLen; // original audio data in bytes
	if ((int)s_dataLen < cvt.len * cvt.len_mult) {
		s_data = realloc(s_data, cvt.len * cvt.len_mult);
		s_dataLen = cvt.len * cvt.len_mult;
	}
	cvt.buf = s_data;

	r = SDL_ConvertAudio(&cvt);
	if (r < 0) {
		Error("SDL_ConvertAudio(%p): %s\n", &cvt, SDL_GetError());
		return;
	}

	Debug("SDL_ConvertAudio(): rate_incr=%f len=%d len_cvt=%d len_mult=%d len_ratio=%f\n",
	      cvt.rate_incr, cvt.len, cvt.len_cvt, cvt.len_mult, cvt.len_ratio);

	s_bufferLen = cvt.len_cvt;
}

void DSP_Play(const uint8 *data)
{
	DSP_Stop();

	data += READ_LE_UINT16(data + 20);

	if (*data != 1) return;

	s_bufferLen = (READ_LE_UINT32(data) >> 8) - 2;

	if (s_dataLen < s_bufferLen) {
		s_data = realloc(s_data, s_bufferLen);
		s_dataLen = s_bufferLen;
	}

	memcpy(s_data, data + 6, s_bufferLen);
	{
		Uint32 t0 = SDL_GetTicks(), dt;
		DSP_ConvertAudio(1000000 / (256 - data[4]));
		dt = SDL_GetTicks() - t0;
		if (dt > 4) fprintf(stderr, "mister: DSP_Play: %u ms converting %u bytes\n", (unsigned)dt, (unsigned)s_bufferLen);
	}

	s_buffer = s_data;
	s_status = 2;
	SDL_PauseAudio(0);
}

uint8 DSP_GetStatus(void)
{
	return (SDL_GetAudioStatus() != 0) ? s_status : 0;
}
