/*
 * Copyright (C) 2003-2011 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "decoder_api.h"
#include "audio_check.h"

#include <glib.h>

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/log.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ffmpeg"

static GLogLevelFlags
level_ffmpeg_to_glib(int level)
{
	if (level <= AV_LOG_FATAL)
		return G_LOG_LEVEL_CRITICAL;

	if (level <= AV_LOG_ERROR)
		return G_LOG_LEVEL_WARNING;

	if (level <= AV_LOG_INFO)
		return G_LOG_LEVEL_MESSAGE;

	return G_LOG_LEVEL_DEBUG;
}

static void
mpd_ffmpeg_log_callback(G_GNUC_UNUSED void *ptr, int level,
			const char *fmt, va_list vl)
{
	const AVClass * cls = NULL;

	if (ptr != NULL)
		cls = *(const AVClass *const*)ptr;

	if (cls != NULL) {
		char *domain = g_strconcat(G_LOG_DOMAIN, "/", cls->item_name(ptr), NULL);
		g_logv(domain, level_ffmpeg_to_glib(level), fmt, vl);
		g_free(domain);
	}
}


#ifndef AV_VERSION_INT
#define AV_VERSION_INT(a, b, c) (a<<16 | b<<8 | c)
#endif

struct mpd_ffmpeg_stream {
	struct decoder *decoder;
	struct input_stream *input;

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(52,101,0)
	AVIOContext *io;
#else
	ByteIOContext *io;
#endif
	unsigned char buffer[8192];
};

static int
mpd_ffmpeg_stream_read(void *opaque, uint8_t *buf, int size)
{
	struct mpd_ffmpeg_stream *stream = opaque;

	return decoder_read(stream->decoder, stream->input,
			    (void *)buf, size);
}

static int64_t
mpd_ffmpeg_stream_seek(void *opaque, int64_t pos, int whence)
{
	struct mpd_ffmpeg_stream *stream = opaque;

	if (whence == AVSEEK_SIZE)
		return stream->input->size;

	if (!input_stream_lock_seek(stream->input, pos, whence, NULL))
		return -1;

	return stream->input->offset;
}

static struct mpd_ffmpeg_stream *
mpd_ffmpeg_stream_open(struct decoder *decoder, struct input_stream *input)
{
	struct mpd_ffmpeg_stream *stream = g_new(struct mpd_ffmpeg_stream, 1);
	stream->decoder = decoder;
	stream->input = input;
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(52,101,0)
	stream->io = avio_alloc_context(stream->buffer, sizeof(stream->buffer),
					false, stream,
					mpd_ffmpeg_stream_read, NULL,
					input->seekable
					? mpd_ffmpeg_stream_seek : NULL);
#else
	stream->io = av_alloc_put_byte(stream->buffer, sizeof(stream->buffer),
				       false, stream,
				       mpd_ffmpeg_stream_read, NULL,
				       input->seekable
				       ? mpd_ffmpeg_stream_seek : NULL);
#endif
	if (stream->io == NULL) {
		g_free(stream);
		return NULL;
	}

	return stream;
}

/**
 * API compatibility wrapper for av_open_input_stream() and
 * avformat_open_input().
 */
static int
mpd_ffmpeg_open_input(AVFormatContext **ic_ptr,
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(52,101,0)
		      AVIOContext *pb,
#else
		      ByteIOContext *pb,
#endif
		      const char *filename,
		      AVInputFormat *fmt)
{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(53,1,3)
	AVFormatContext *context = avformat_alloc_context();
	if (context == NULL)
		return AVERROR(ENOMEM);

	context->pb = pb;
	*ic_ptr = context;
	return avformat_open_input(ic_ptr, filename, fmt, NULL);
#else
	return av_open_input_stream(ic_ptr, pb, filename, fmt, NULL);
#endif
}

static void
mpd_ffmpeg_stream_close(struct mpd_ffmpeg_stream *stream)
{
	av_free(stream->io);
	g_free(stream);
}

static bool
ffmpeg_init(G_GNUC_UNUSED const struct config_param *param)
{
	av_log_set_callback(mpd_ffmpeg_log_callback);

	av_register_all();
	return true;
}

static int
ffmpeg_find_audio_stream(const AVFormatContext *format_context)
{
	for (unsigned i = 0; i < format_context->nb_streams; ++i)
		if (format_context->streams[i]->codec->codec_type ==
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(52, 64, 0)
		    AVMEDIA_TYPE_AUDIO)
#else
		    CODEC_TYPE_AUDIO)
#endif
			return i;

	return -1;
}

/**
 * On some platforms, libavcodec wants the output buffer aligned to 16
 * bytes (because it uses SSE/Altivec internally).  This function
 * returns the aligned version of the specified buffer, and corrects
 * the buffer size.
 */
static void *
align16(void *p, size_t *length_p)
{
	unsigned add = 16 - (size_t)p % 16;

	*length_p -= add;
	return (char *)p + add;
}

G_GNUC_CONST
static double
time_from_ffmpeg(int64_t t, const AVRational time_base)
{
	assert(t != (int64_t)AV_NOPTS_VALUE);

	return (double)av_rescale_q(t, time_base, (AVRational){1, 1024})
		/ (double)1024;
}

G_GNUC_CONST
static int64_t
time_to_ffmpeg(double t, const AVRational time_base)
{
	return av_rescale_q((int64_t)(t * 1024), (AVRational){1, 1024},
			    time_base);
}

static enum decoder_command
ffmpeg_send_packet(struct decoder *decoder, struct input_stream *is,
		   const AVPacket *packet,
		   AVCodecContext *codec_context,
		   const AVRational *time_base)
{
	if (packet->pts != (int64_t)AV_NOPTS_VALUE)
		decoder_timestamp(decoder,
				  time_from_ffmpeg(packet->pts, *time_base));

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(52,25,0)
	AVPacket packet2 = *packet;
#else
	const uint8_t *packet_data = packet->data;
	int packet_size = packet->size;
#endif

	uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2 + 16];
	size_t buffer_size = sizeof(audio_buf);
	int16_t *aligned_buffer = align16(audio_buf, &buffer_size);

	enum decoder_command cmd = DECODE_COMMAND_NONE;
	while (
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(52,25,0)
	       packet2.size > 0 &&
#else
	       packet_size > 0 &&
#endif
	       cmd == DECODE_COMMAND_NONE) {
		int audio_size = buffer_size;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(52,25,0)
		int len = avcodec_decode_audio3(codec_context,
						aligned_buffer, &audio_size,
						&packet2);
#else
		int len = avcodec_decode_audio2(codec_context,
						aligned_buffer, &audio_size,
						packet_data, packet_size);
#endif

		if (len < 0) {
			/* if error, we skip the frame */
			g_message("decoding failed\n");
			break;
		}

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(52,25,0)
		packet2.data += len;
		packet2.size -= len;
#else
		packet_data += len;
		packet_size -= len;
#endif

		if (audio_size <= 0)
			continue;

		cmd = decoder_data(decoder, is,
				   aligned_buffer, audio_size,
				   codec_context->bit_rate / 1000);
	}
	return cmd;
}

static enum sample_format
ffmpeg_sample_format(G_GNUC_UNUSED const AVCodecContext *codec_context)
{
	switch (codec_context->sample_fmt) {
	case SAMPLE_FMT_S16:
		return SAMPLE_FORMAT_S16;

	case SAMPLE_FMT_S32:
		return SAMPLE_FORMAT_S32;

	default:
		g_warning("Unsupported libavcodec SampleFormat value: %d",
			  codec_context->sample_fmt);
		return SAMPLE_FORMAT_UNDEFINED;
	}
}

static AVInputFormat *
ffmpeg_probe(struct decoder *decoder, struct input_stream *is)
{
	enum {
		BUFFER_SIZE = 16384,
		PADDING = 16,
	};

	unsigned char *buffer = g_malloc(BUFFER_SIZE);
	size_t nbytes = decoder_read(decoder, is, buffer, BUFFER_SIZE);
	if (nbytes <= PADDING ||
	    !input_stream_lock_seek(is, 0, SEEK_SET, NULL)) {
		g_free(buffer);
		return NULL;
	}

	/* some ffmpeg parsers (e.g. ac3_parser.c) read a few bytes
	   beyond the declared buffer limit, which makes valgrind
	   angry; this workaround removes some padding from the buffer
	   size */
	nbytes -= PADDING;

	AVProbeData avpd = {
		.buf = buffer,
		.buf_size = nbytes,
		.filename = is->uri,
	};

	AVInputFormat *format = av_probe_input_format(&avpd, true);
	g_free(buffer);

	return format;
}

static void
ffmpeg_decode(struct decoder *decoder, struct input_stream *input)
{
	AVInputFormat *input_format = ffmpeg_probe(decoder, input);
	if (input_format == NULL)
		return;

	g_debug("detected input format '%s' (%s)",
		input_format->name, input_format->long_name);

	struct mpd_ffmpeg_stream *stream =
		mpd_ffmpeg_stream_open(decoder, input);
	if (stream == NULL) {
		g_warning("Failed to open stream");
		return;
	}

	//ffmpeg works with ours "fileops" helper
	AVFormatContext *format_context = NULL;
	if (mpd_ffmpeg_open_input(&format_context, stream->io, input->uri,
				  input_format) != 0) {
		g_warning("Open failed\n");
		mpd_ffmpeg_stream_close(stream);
		return;
	}

	if (av_find_stream_info(format_context)<0) {
		g_warning("Couldn't find stream info\n");
		av_close_input_stream(format_context);
		mpd_ffmpeg_stream_close(stream);
		return;
	}

	int audio_stream = ffmpeg_find_audio_stream(format_context);
	if (audio_stream == -1) {
		g_warning("No audio stream inside\n");
		av_close_input_stream(format_context);
		mpd_ffmpeg_stream_close(stream);
		return;
	}

	AVStream *av_stream = format_context->streams[audio_stream];

	AVCodecContext *codec_context = av_stream->codec;
	if (codec_context->codec_name[0] != 0)
		g_debug("codec '%s'", codec_context->codec_name);

	AVCodec *codec = avcodec_find_decoder(codec_context->codec_id);

	if (!codec) {
		g_warning("Unsupported audio codec\n");
		av_close_input_stream(format_context);
		mpd_ffmpeg_stream_close(stream);
		return;
	}

	if (avcodec_open(codec_context, codec)<0) {
		g_warning("Could not open codec\n");
		av_close_input_stream(format_context);
		mpd_ffmpeg_stream_close(stream);
		return;
	}

	GError *error = NULL;
	struct audio_format audio_format;
	if (!audio_format_init_checked(&audio_format,
				       codec_context->sample_rate,
				       ffmpeg_sample_format(codec_context),
				       codec_context->channels, &error)) {
		g_warning("%s", error->message);
		g_error_free(error);
		avcodec_close(codec_context);
		av_close_input_stream(format_context);
		mpd_ffmpeg_stream_close(stream);
		return;
	}

	int total_time = format_context->duration != (int64_t)AV_NOPTS_VALUE
		? format_context->duration / AV_TIME_BASE
		: 0;

	decoder_initialized(decoder, &audio_format,
			    input->seekable, total_time);

	enum decoder_command cmd;
	do {
		AVPacket packet;
		if (av_read_frame(format_context, &packet) < 0)
			/* end of file */
			break;

		if (packet.stream_index == audio_stream)
			cmd = ffmpeg_send_packet(decoder, input,
						 &packet, codec_context,
						 &av_stream->time_base);
		else
			cmd = decoder_get_command(decoder);

		av_free_packet(&packet);

		if (cmd == DECODE_COMMAND_SEEK) {
			int64_t where =
				time_to_ffmpeg(decoder_seek_where(decoder),
					       av_stream->time_base);

			if (av_seek_frame(format_context, audio_stream, where,
					  AV_TIME_BASE) < 0)
				decoder_seek_error(decoder);
			else {
				avcodec_flush_buffers(codec_context);
				decoder_command_finished(decoder);
			}
		}
	} while (cmd != DECODE_COMMAND_STOP);

	avcodec_close(codec_context);
	av_close_input_stream(format_context);
	mpd_ffmpeg_stream_close(stream);
}

typedef struct ffmpeg_tag_map {
	enum tag_type type;
	const char *name;
} ffmpeg_tag_map;

static const ffmpeg_tag_map ffmpeg_tag_maps[] = {
	{ TAG_TITLE,             "title" },
#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(50<<8))
	{ TAG_ARTIST,            "artist" },
	{ TAG_DATE,              "date" },
#else
	{ TAG_ARTIST,            "author" },
	{ TAG_DATE,              "year" },
#endif
	{ TAG_ALBUM,             "album" },
	{ TAG_COMMENT,           "comment" },
	{ TAG_GENRE,             "genre" },
	{ TAG_TRACK,             "track" },
	{ TAG_ARTIST_SORT,       "author-sort" },
	{ TAG_ALBUM_ARTIST,      "album_artist" },
	{ TAG_ALBUM_ARTIST_SORT, "album_artist-sort" },
	{ TAG_COMPOSER,          "composer" },
	{ TAG_PERFORMER,         "performer" },
	{ TAG_DISC,              "disc" },
};

static bool
ffmpeg_copy_metadata(struct tag *tag,
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(53,1,0)
		     AVDictionary *m,
#else
		     AVMetadata *m,
#endif
		     const ffmpeg_tag_map tag_map)
{
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(53,1,0)
	AVDictionaryEntry *mt = NULL;

	while ((mt = av_dict_get(m, tag_map.name, mt, 0)) != NULL)
		tag_add_item(tag, tag_map.type, mt->value);
#else
	AVMetadataTag *mt = NULL;

	while ((mt = av_metadata_get(m, tag_map.name, mt, 0)) != NULL)
		tag_add_item(tag, tag_map.type, mt->value);
#endif

	return mt != NULL;
}

//no tag reading in ffmpeg, check if playable
static struct tag *
ffmpeg_stream_tag(struct input_stream *is)
{
	AVInputFormat *input_format = ffmpeg_probe(NULL, is);
	if (input_format == NULL)
		return NULL;

	struct mpd_ffmpeg_stream *stream = mpd_ffmpeg_stream_open(NULL, is);
	if (stream == NULL)
		return NULL;

	AVFormatContext *f = NULL;
	if (mpd_ffmpeg_open_input(&f, stream->io, is->uri,
				  input_format) != 0) {
		mpd_ffmpeg_stream_close(stream);
		return NULL;
	}

	if (av_find_stream_info(f) < 0) {
		av_close_input_stream(f);
		mpd_ffmpeg_stream_close(stream);
		return NULL;
	}

	struct tag *tag = tag_new();

	tag->time = f->duration != (int64_t)AV_NOPTS_VALUE
		? f->duration / AV_TIME_BASE
		: 0;

#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(52,101,0)
	av_metadata_conv(f, NULL, f->iformat->metadata_conv);
#endif

	for (unsigned i = 0; i < sizeof(ffmpeg_tag_maps)/sizeof(ffmpeg_tag_map); i++) {
		int idx = ffmpeg_find_audio_stream(f);
		ffmpeg_copy_metadata(tag, f->metadata, ffmpeg_tag_maps[i]);
		if (idx >= 0)
			ffmpeg_copy_metadata(tag, f->streams[idx]->metadata, ffmpeg_tag_maps[i]);
	}

	av_close_input_stream(f);
	mpd_ffmpeg_stream_close(stream);

	return tag;
}

/**
 * A list of extensions found for the formats supported by ffmpeg.
 * This list is current as of 02-23-09; To find out if there are more
 * supported formats, check the ffmpeg changelog since this date for
 * more formats.
 */
static const char *const ffmpeg_suffixes[] = {
	"16sv", "3g2", "3gp", "4xm", "8svx", "aa3", "aac", "ac3", "afc", "aif",
	"aifc", "aiff", "al", "alaw", "amr", "anim", "apc", "ape", "asf",
	"atrac", "au", "aud", "avi", "avm2", "avs", "bap", "bfi", "c93", "cak",
	"cin", "cmv", "cpk", "daud", "dct", "divx", "dts", "dv", "dvd", "dxa",
	"eac3", "film", "flac", "flc", "fli", "fll", "flx", "flv", "g726",
	"gsm", "gxf", "iss", "m1v", "m2v", "m2t", "m2ts",
	"m4a", "m4b", "m4v",
	"mad",
	"mj2", "mjpeg", "mjpg", "mka", "mkv", "mlp", "mm", "mmf", "mov", "mp+",
	"mp1", "mp2", "mp3", "mp4", "mpc", "mpeg", "mpg", "mpga", "mpp", "mpu",
	"mve", "mvi", "mxf", "nc", "nsv", "nut", "nuv", "oga", "ogm", "ogv",
	"ogx", "oma", "ogg", "omg", "psp", "pva", "qcp", "qt", "r3d", "ra",
	"ram", "rl2", "rm", "rmvb", "roq", "rpl", "rvc", "shn", "smk", "snd",
	"sol", "son", "spx", "str", "swf", "tgi", "tgq", "tgv", "thp", "ts",
	"tsp", "tta", "xa", "xvid", "uv", "uv2", "vb", "vid", "vob", "voc",
	"vp6", "vmd", "wav", "wma", "wmv", "wsaud", "wsvga", "wv", "wve",
	NULL
};

static const char *const ffmpeg_mime_types[] = {
	"application/m4a",
	"application/mp4",
	"application/octet-stream",
	"application/ogg",
	"application/x-ms-wmz",
	"application/x-ms-wmd",
	"application/x-ogg",
	"application/x-shockwave-flash",
	"application/x-shorten",
	"audio/8svx",
	"audio/16sv",
	"audio/aac",
	"audio/ac3",
	"audio/aiff"
	"audio/amr",
	"audio/basic",
	"audio/flac",
	"audio/m4a",
	"audio/mp4",
	"audio/mpeg",
	"audio/musepack",
	"audio/ogg",
	"audio/qcelp",
	"audio/vorbis",
	"audio/vorbis+ogg",
	"audio/x-8svx",
	"audio/x-16sv",
	"audio/x-aac",
	"audio/x-ac3",
	"audio/x-aiff"
	"audio/x-alaw",
	"audio/x-au",
	"audio/x-dca",
	"audio/x-eac3",
	"audio/x-flac",
	"audio/x-gsm",
	"audio/x-mace",
	"audio/x-matroska",
	"audio/x-monkeys-audio",
	"audio/x-mpeg",
	"audio/x-ms-wma",
	"audio/x-ms-wax",
	"audio/x-musepack",
	"audio/x-ogg",
	"audio/x-vorbis",
	"audio/x-vorbis+ogg",
	"audio/x-pn-realaudio",
	"audio/x-pn-multirate-realaudio",
	"audio/x-speex",
	"audio/x-tta"
	"audio/x-voc",
	"audio/x-wav",
	"audio/x-wma",
	"audio/x-wv",
	"video/anim",
	"video/quicktime",
	"video/msvideo",
	"video/ogg",
	"video/theora",
	"video/x-dv",
	"video/x-flv",
	"video/x-matroska",
	"video/x-mjpeg",
	"video/x-mpeg",
	"video/x-ms-asf",
	"video/x-msvideo",
	"video/x-ms-wmv",
	"video/x-ms-wvx",
	"video/x-ms-wm",
	"video/x-ms-wmx",
	"video/x-nut",
	"video/x-pva",
	"video/x-theora",
	"video/x-vid",
	"video/x-wmv",
	"video/x-xvid",

	/* special value for the "ffmpeg" input plugin: all streams by
	   the "ffmpeg" input plugin shall be decoded by this
	   plugin */
	"audio/x-mpd-ffmpeg",

	NULL
};

const struct decoder_plugin ffmpeg_decoder_plugin = {
	.name = "ffmpeg",
	.init = ffmpeg_init,
	.stream_decode = ffmpeg_decode,
	.stream_tag = ffmpeg_stream_tag,
	.suffixes = ffmpeg_suffixes,
	.mime_types = ffmpeg_mime_types
};
