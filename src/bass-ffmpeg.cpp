// bass-ffmpeg.cpp - BASS add-on, powered by FFmpeg
//
// Plays (almost) anything FFmpeg can DECODE: all audio formats, plus the audio
// stream of any video file (video is skipped, audio-only playback). Everything
// (libavformat/libavcodec/libswresample/libavutil + external codec libs) is
// linked STATICALLY into this DLL - only bass.dll is required (the host).
//
// File I/O goes through BASS's BASSFILE routines, so network streaming,
// buffered downloads and user file systems all work automatically.
//
// Built for Windows XP+ (see .github/workflows/build.yml).

#define VERSION 0x02040000

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bass-addon.h"
const BASS_FUNCTIONS *bassfunc;

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#define PLUGIN_NAME "bass-ffmpeg"

// custom channel types (keep unique per add-on)
#define BASS_CTYPE_STREAM_FFMPEG		0x10101
#define BASS_CTYPE_STREAM_FFMPEG_VIDEO	0x10102

struct FFMPEGSTREAM {
	HSTREAM handle;
	BASSFILE file;
	AVFormatContext *fmt;
	AVIOContext *io;
	AVCodecContext *dec;
	SwrContext *swr;
	AVFrame *frame;
	AVPacket *pkt;
	int stream;          // audio stream index
	DWORD outres;        // BASS output sample size: 1=8bit 2=16bit 4=float
	DWORD freq, chans;   // decoded format (stream format)
	double duration;     // seconds (0 = unknown)
	QWORD bytelength;    // decoded length in output bytes (0 = unknown)
	BOOL seekable, sent_eof, eof;
	float *staging;      // interleaved float32 staging buffer
	DWORD stagecap;      // capacity in floats
	DWORD stagelen, stagepos; // floats valid / consumed

	static const ADDON_FUNCTIONS addonfuncs;
	static void WINAPI Free(void *inst);
	static QWORD WINAPI GetLength(void *inst, DWORD mode);
	static void WINAPI GetInfo(void *inst, BASS_CHANNELINFO *info);
	static BOOL WINAPI CanSetPosition(void *inst, QWORD pos, DWORD mode);
	static QWORD WINAPI SetPosition(void *inst, QWORD pos, DWORD mode);
	static DWORD CALLBACK StreamProc(HSTREAM handle, void *buffer, DWORD length, void *inst);
};

// ----------------------------------------------------------------- BASSFILE I/O

static int io_read(void *opaque, uint8_t *buf, int size)
{
	FFMPEGSTREAM *stream = (FFMPEGSTREAM*)opaque;
	DWORD n = bassfunc->file.Read(stream->file, buf, size);
	if (n) return (int)n;
	return AVERROR_EOF; // BASS's Read blocks until data on net files; 0 = EOF
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
	FFMPEGSTREAM *stream = (FFMPEGSTREAM*)opaque;
	if (whence == AVSEEK_SIZE) return (int64_t)bassfunc->file.GetPos(stream->file, BASS_FILEPOS_END);
	int64_t base = 0;
	if (whence == SEEK_CUR) base = (int64_t)bassfunc->file.GetPos(stream->file, BASS_FILEPOS_CURRENT);
	else if (whence == SEEK_END) base = (int64_t)bassfunc->file.GetPos(stream->file, BASS_FILEPOS_END);
	else if (whence != SEEK_SET) return -1;
	offset += base;
	if (offset < 0 || !bassfunc->file.Seek(stream->file, (QWORD)offset)) return -1;
	return offset;
}

// -------------------------------------------------------------------- FFmpeg

static void FreeFF(FFMPEGSTREAM *stream)
{
	if (stream->swr) swr_free(&stream->swr);
	if (stream->dec) avcodec_free_context(&stream->dec);
	if (stream->fmt) avformat_close_input(&stream->fmt); // CUSTOM_IO: ours stays
	if (stream->io) avio_context_free(&stream->io);
	if (stream->frame) av_frame_free(&stream->frame);
	if (stream->pkt) av_packet_free(&stream->pkt);
	free(stream->staging);
	stream->staging = 0;
}

static BOOL ensure_staging(FFMPEGSTREAM *stream, DWORD floats)
{
	if (floats <= stream->stagecap) return TRUE;
	float *p = (float*)realloc(stream->staging, floats * sizeof(float));
	if (!p) return FALSE;
	stream->staging = p;
	stream->stagecap = floats;
	return TRUE;
}

// open + analyse the file; FALSE = not a playable audio stream
static BOOL InitFF(FFMPEGSTREAM *stream)
{
	const int IOBUF = 64 * 1024;
	unsigned char *iobuf = (unsigned char*)av_malloc(IOBUF);
	stream->io = avio_alloc_context(iobuf, IOBUF, 0, stream, io_read, NULL, stream->seekable ? io_seek : NULL);
	if (!stream->io) { av_free(iobuf); return FALSE; }
	stream->io->seekable = stream->seekable ? AVIO_SEEKABLE_NORMAL : 0;

	stream->fmt = avformat_alloc_context();
	if (!stream->fmt) return FALSE;
	stream->fmt->pb = stream->io;
	stream->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	if (avformat_open_input(&stream->fmt, NULL, NULL, NULL) < 0) return FALSE;
	if (avformat_find_stream_info(stream->fmt, NULL) < 0) return FALSE;

	{ // pick the audio stream (video streams are ignored entirely)
		int s = av_find_best_stream(stream->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
		if (s < 0) return FALSE;
		stream->stream = s;
	}

	{ // open the audio decoder
		AVStream *st = stream->fmt->streams[stream->stream];
		const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
		if (!c) return FALSE;
		stream->dec = avcodec_alloc_context3(c);
		if (!stream->dec) return FALSE;
		if (avcodec_parameters_to_context(stream->dec, st->codecpar) < 0) return FALSE;
		if (avcodec_open2(stream->dec, c, NULL) < 0) return FALSE;
	}

	stream->freq = stream->dec->sample_rate;
	stream->chans = stream->dec->ch_layout.nb_channels;
	if (!stream->freq || !stream->chans) return FALSE;
	if (stream->chans > 8) { // BASS maxes out at 8 channels - downmix beyond that
		AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
		av_channel_layout_uninit(&stream->dec->ch_layout);
		av_channel_layout_copy(&stream->dec->ch_layout, &stereo);
		stream->chans = 2;
	}
	if (stream->dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_default(&stream->dec->ch_layout, stream->chans);

	// decode -> interleaved float32 at the source rate/layout; BASS resamples
	// to the device itself
	if (swr_alloc_set_opts2(&stream->swr, &stream->dec->ch_layout, AV_SAMPLE_FMT_FLT, stream->freq,
			&stream->dec->ch_layout, stream->dec->sample_fmt, stream->freq, 0, NULL) < 0) return FALSE;
	if (swr_init(stream->swr) < 0) return FALSE;

	stream->frame = av_frame_alloc();
	stream->pkt = av_packet_alloc();
	if (!stream->frame || !stream->pkt) return FALSE;

	{ // duration
		AVStream *st = stream->fmt->streams[stream->stream];
		if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
			stream->duration = st->duration * av_q2d(st->time_base);
		else if (stream->fmt->duration != AV_NOPTS_VALUE && stream->fmt->duration > 0)
			stream->duration = (double)stream->fmt->duration / AV_TIME_BASE;
	}
	return TRUE;
}

// ------------------------------------------------------------- addon methods

const ADDON_FUNCTIONS FFMPEGSTREAM::addonfuncs = {
	0,          // flags
	Free,
	GetLength,
	NULL,       // GetTags: let BASS read the common tags from the file
	NULL,       // GetFilePosition: let BASS handle it
	GetInfo,
	CanSetPosition,
	SetPosition,
	NULL,       // GetPosition: let BASS track position
	NULL,       // SetSync: let BASS handle syncs
	NULL,       // RemoveSync
	NULL,       // CanResume: let BASS decide
	NULL,       // SetFlags
	NULL,       // Attribute
	NULL        // AttributeEx
};

void WINAPI FFMPEGSTREAM::Free(void *inst)
{
	FFMPEGSTREAM *stream = (FFMPEGSTREAM*)inst;
	FreeFF(stream);
	free(stream);
}

// called by BASS_ChannelGetLength
QWORD WINAPI FFMPEGSTREAM::GetLength(void *inst, DWORD mode)
{
	FFMPEGSTREAM *stream = (FFMPEGSTREAM*)inst;
	if (mode != BASS_POS_BYTE) errorn(BASS_ERROR_NOTAVAIL);
	noerrorn(stream->bytelength);
}

// called by BASS_ChannelGetInfo
void WINAPI FFMPEGSTREAM::GetInfo(void *inst, BASS_CHANNELINFO *info)
{
	FFMPEGSTREAM *stream = (FFMPEGSTREAM*)inst;
	info->origres = stream->dec->bits_per_raw_sample ? stream->dec->bits_per_raw_sample : 16;
}

// called by BASS_ChannelSetPosition
BOOL WINAPI FFMPEGSTREAM::CanSetPosition(void *inst, QWORD pos, DWORD mode)
{
	FFMPEGSTREAM *stream = (FFMPEGSTREAM*)inst;
	if ((BYTE)mode != BASS_POS_BYTE) error(BASS_ERROR_NOTAVAIL);
	if (!stream->seekable) error(BASS_ERROR_POSITION);
	if (stream->bytelength && pos >= stream->bytelength) error(BASS_ERROR_POSITION);
	return true;
}

// called by BASS_ChannelSetPosition after CanSetPosition
QWORD WINAPI FFMPEGSTREAM::SetPosition(void *inst, QWORD pos, DWORD mode)
{
	FFMPEGSTREAM *stream = (FFMPEGSTREAM*)inst;
	if ((BYTE)mode != BASS_POS_BYTE) errorn(BASS_ERROR_NOTAVAIL);
	double t = (double)pos / ((double)stream->freq * stream->chans * stream->outres);
	if (av_seek_frame(stream->fmt, -1, (int64_t)(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0)
		errorn(BASS_ERROR_POSITION);
	avcodec_flush_buffers(stream->dec);
	swr_init(stream->swr); // drop any resampler delay
	stream->stagelen = stream->stagepos = 0;
	stream->eof = stream->sent_eof = FALSE;
	noerrorn(pos);
}

// decode data for BASS ("length" = bytes to fill, whole frames)
DWORD CALLBACK FFMPEGSTREAM::StreamProc(HSTREAM handle, void *vbuffer, DWORD length, void *inst)
{
	FFMPEGSTREAM *stream = (FFMPEGSTREAM*)inst;
	BYTE *buffer = (BYTE*)vbuffer;
	DWORD done = 0;
	DWORD framebytes = stream->chans * stream->outres;

	while (done < length) {
		if (stream->stagepos < stream->stagelen) { // staged samples available
			DWORD want = (length - done) / framebytes * stream->chans; // floats wanted
			DWORD avail = stream->stagelen - stream->stagepos;
			if (want > avail) want = avail;
			if (stream->outres == 4) // float output: pass through
				memcpy(buffer + done, stream->staging + stream->stagepos, want * sizeof(float));
			else // 8/16-bit output
				bassfunc->data.Float2Int(stream->staging + stream->stagepos, buffer + done, want, stream->outres);
			stream->stagepos += want;
			done += want * stream->outres;
			continue;
		}
		if (stream->eof) break;

		int ret = avcodec_receive_frame(stream->dec, stream->frame);
		if (ret == 0) { // got a frame -> resample to interleaved float
			int maxn = swr_get_out_samples(stream->swr, stream->frame->nb_samples);
			if (maxn < 1) maxn = stream->frame->nb_samples + 256;
			if (!ensure_staging(stream, (DWORD)maxn * stream->chans)) { stream->eof = TRUE; break; }
			uint8_t *dst = (uint8_t*)stream->staging;
			int got = swr_convert(stream->swr, &dst, maxn,
				(const uint8_t**)stream->frame->extended_data, stream->frame->nb_samples);
			if (got < 0) got = 0;
			stream->stagelen = (DWORD)got * stream->chans;
			stream->stagepos = 0;
			continue;
		}
		if (ret == AVERROR_EOF || (ret < 0 && ret != AVERROR(EAGAIN))) {
			stream->eof = TRUE; // drained or decode error
			break;
		}
		// decoder needs more input
		if (stream->sent_eof) { stream->eof = TRUE; break; }
		ret = av_read_frame(stream->fmt, stream->pkt);
		if (ret >= 0) {
			if (stream->pkt->stream_index != stream->stream) {
				av_packet_unref(stream->pkt); // video/subtitle packet - skip
				continue;
			}
			avcodec_send_packet(stream->dec, stream->pkt);
			av_packet_unref(stream->pkt);
			continue;
		}
		if (ret == AVERROR(EAGAIN)) continue;
		stream->sent_eof = TRUE;
		avcodec_send_packet(stream->dec, NULL); // end of input: drain the decoder
	}

	if (stream->eof && done < length) { // refine the actual decoded length
		stream->bytelength = bassfunc->GetPosition(handle, (QWORD)-1, BASS_POS_BYTE) + done;
		done |= BASS_STREAMPROC_END;
	}
	return done;
}

// ------------------------------------------------------------ stream creation

static HSTREAM WINAPI StreamCreateProc(BASSFILE file, DWORD flags)
{
	DWORD fileflags = bassfunc->file.GetFlags(file);

	FFMPEGSTREAM *stream = (FFMPEGSTREAM*)calloc(1, sizeof(FFMPEGSTREAM));
	if (!stream) error(BASS_ERROR_MEM);
	stream->file = file;
	stream->seekable = !(fileflags & BASSFILE_BLOCK);

	if (!InitFF(stream)) {
		FFMPEGSTREAM::Free(stream);
		error(BASS_ERROR_FILEFORM);
	}

	// restrict flags to valid ones, and create the BASS stream
	flags &= BASS_SAMPLE_FLOAT | BASS_SAMPLE_8BITS | BASS_SAMPLE_SOFTWARE | BASS_SAMPLE_LOOP | BASS_SAMPLE_3D | BASS_SAMPLE_FX
		| BASS_STREAM_DECODE | BASS_STREAM_AUTOFREE | 0x3f000000; // 0x3f000000 = all speaker flags
	flags |= fileflags & BASS_STREAM_BLOCK; // BLOCK flag disables seeking
	stream->outres = (flags & BASS_SAMPLE_FLOAT) ? 4 : (flags & BASS_SAMPLE_8BITS) ? 1 : 2;

	HSTREAM handle = bassfunc->CreateStream(stream->freq, stream->chans, flags,
		FFMPEGSTREAM::StreamProc, stream, &FFMPEGSTREAM::addonfuncs);
	if (!handle) {
		FFMPEGSTREAM::Free(stream);
		return 0; // CreateStream set the error code
	}
	stream->handle = handle;
	if (stream->duration > 0)
		stream->bytelength = (QWORD)(stream->duration * stream->freq * stream->chans * stream->outres);

	if (fileflags & BASSFILE_BUFFERED) {
		DWORD rate = stream->freq * stream->chans * stream->outres; // bytes/sec
		if (!bassfunc->file.StartThread(file, rate, 0)) { // start download thread
			DWORD err = BASS_ErrorGetCode();
			if (!err) err = BASS_ERROR_MEM;
			BASS_StreamFree(handle);
			error(err);
		}
	}
	bassfunc->file.SetStream(file, handle); // associate stream & file (after error checking)
	noerrorn(handle);
}

// ------------------------------------------------------------ plugin export

#ifdef __cplusplus
extern "C"
#endif
__declspec(dllexport) const void *WINAPI BASSplugin(DWORD face)
{
	static const BASS_PLUGINFORM pluginforms[] = {
		{ BASS_CTYPE_STREAM_FFMPEG, "FFmpeg audio",
			"*.mp3;*.mp2;*.mp1;*.mpa;*.m4a;*.m4b;*.m4r;*.aac;*.ac3;*.eac3;*.dts;*.dtshd;*.hra;*.mlp;*.truehd;*.thd;"
			"*.flac;*.ogg;*.oga;*.opus;*.spx;*.wma;*.wav;*.wave;*.w64;*.wv;*.ape;*.tta;*.tak;*.ofr;*.ofs;*.mpc;*.mp+;"
			"*.mpp;*.vqf;*.8svx;*.svx;*.amr;*.awb;*.gsm;*.gsm610;*.aa3;*.oma;*.at3;*.at9;*.ra;*.mka;*.caf;*.snd;*.au;"
			"*.aif;*.aiff;*.aifc;*.voc;*.iff;*.vox;*.ircam;*.amb;*.dsf;*.dff;*.xwb;*.aax;*.aa;*.ac4;*.aptx" },
		{ BASS_CTYPE_STREAM_FFMPEG_VIDEO, "FFmpeg video (audio only)",
			"*.mkv;*.mp4;*.avi;*.mov;*.qt;*.wmv;*.asf;*.flv;*.f4v;*.webm;*.ogv;*.ts;*.m2ts;*.mts;*.m2t;*.mpg;*.mpeg;"
			"*.mpe;*.mpv;*.m2v;*.vob;*.ifo;*.3gp;*.3g2;*.3gpp;*.3gp2;*.divx;*.xvid;*.rm;*.rmvb;*.rv;*.m4v;*.tp;*.trp;"
			"*.dvr-ms;*.wtv;*.mxf;*.nuv;*.nsv;*.nut;*.roq;*.swf;*.fla;*.fli;*.flc;*.bik;*.bk2;*.sm2;*.r3d;*.yop;*.mtv" },
	};
	static const BASS_PLUGININFO plugininfo = { VERSION, 2, pluginforms };

	switch (face) {
		case BASSPLUGIN_INFO:
			return (const void*)&plugininfo;

		case BASSPLUGIN_CREATE:
			return (const void*)StreamCreateProc;
	}
	return NULL;
}

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hDLL, DWORD reason, LPVOID reserved)
{
	switch (reason) {
		case DLL_PROCESS_ATTACH:
			DisableThreadLibraryCalls(hDLL);
			if (HIWORD(BASS_GetVersion()) != BASSVERSION || !GetBassFunc()) {
				MessageBox(0, "Incorrect BASS.DLL version (2.4 is required)", PLUGIN_NAME, MB_ICONERROR);
				return FALSE;
			}
			break;
	}
	return TRUE;
}
#endif
