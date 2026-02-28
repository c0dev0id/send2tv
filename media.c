#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include "send2tv.h"

/*
 * Custom AVIO write callback: writes encoded data to a pipe fd.
 */
static int
avio_write_pipe(void *opaque, uint8_t *buf, int buf_size)
{
	int	 fd = *(int *)opaque;
	int	 total = 0;
	ssize_t	 n;

	while (total < buf_size) {
		n = write(fd, buf + total, buf_size - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return AVERROR(errno);
		}
		total += n;
	}
	return total;
}

/*
 * Check if video codec is natively supported by Samsung TVs.
 */
static int
video_codec_ok(enum AVCodecID id)
{
	return id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC;
}

/*
 * Check if audio codec is natively supported by Samsung TVs.
 */
static int
audio_codec_ok(enum AVCodecID id)
{
	switch (id) {
	case AV_CODEC_ID_AAC:
	case AV_CODEC_ID_MP3:
	case AV_CODEC_ID_FLAC:
	case AV_CODEC_ID_PCM_S16LE:
	case AV_CODEC_ID_PCM_S16BE:
	case AV_CODEC_ID_AC3:
	case AV_CODEC_ID_EAC3:
	case AV_CODEC_ID_VORBIS:
		return 1;
	default:
		return 0;
	}
}

/*
 * Check if container format is supported by Samsung TVs.
 */
static int
container_ok(const char *name)
{
	if (name == NULL)
		return 0;
	return strstr(name, "mp4") != NULL ||
	    strstr(name, "mov") != NULL ||
	    strstr(name, "matroska") != NULL ||
	    strstr(name, "mpegts") != NULL ||
	    strstr(name, "avi") != NULL ||
	    strstr(name, "mp3") != NULL ||
	    strstr(name, "flac") != NULL ||
	    strstr(name, "ogg") != NULL ||
	    strstr(name, "wav") != NULL;
}

/*
 * Determine MIME type from format name.
 */
static void
set_mime_type(media_ctx_t *ctx, const char *fmt_name)
{
	if (strstr(fmt_name, "mp4") || strstr(fmt_name, "mov"))
		strlcpy(ctx->mime_type, "video/mp4",
		    sizeof(ctx->mime_type));
	else if (strstr(fmt_name, "matroska"))
		strlcpy(ctx->mime_type, "video/x-mkv",
		    sizeof(ctx->mime_type));
	else if (strstr(fmt_name, "mpegts"))
		strlcpy(ctx->mime_type, "video/mp2t",
		    sizeof(ctx->mime_type));
	else if (strstr(fmt_name, "avi"))
		strlcpy(ctx->mime_type, "video/avi",
		    sizeof(ctx->mime_type));
	else if (strstr(fmt_name, "mp3"))
		strlcpy(ctx->mime_type, "audio/mpeg",
		    sizeof(ctx->mime_type));
	else if (strstr(fmt_name, "flac"))
		strlcpy(ctx->mime_type, "audio/flac",
		    sizeof(ctx->mime_type));
	else if (strstr(fmt_name, "ogg"))
		strlcpy(ctx->mime_type, "audio/ogg",
		    sizeof(ctx->mime_type));
	else if (strstr(fmt_name, "wav"))
		strlcpy(ctx->mime_type, "audio/wav",
		    sizeof(ctx->mime_type));
	else
		strlcpy(ctx->mime_type, "video/mp2t",
		    sizeof(ctx->mime_type));
}

/*
 * Probe a media file to determine codecs and whether transcoding is needed.
 */
int
media_probe(media_ctx_t *ctx, const char *filepath)
{
	AVFormatContext		*fmt = NULL;
	const char		*fmt_name;
	int			 ret;
	int			 has_video = 0;
	enum AVCodecID		 vid_codec = AV_CODEC_ID_NONE;
	enum AVCodecID		 aud_codec = AV_CODEC_ID_NONE;

	ret = avformat_open_input(&fmt, filepath, NULL, NULL);
	if (ret < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", filepath,
		    av_err2str(ret));
		return -1;
	}

	ret = avformat_find_stream_info(fmt, NULL);
	if (ret < 0) {
		fprintf(stderr, "Cannot find stream info: %s\n",
		    av_err2str(ret));
		avformat_close_input(&fmt);
		return -1;
	}

	ctx->video_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO,
	    -1, -1, NULL, 0);
	ctx->audio_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO,
	    -1, -1, NULL, 0);

	if (ctx->video_idx >= 0) {
		vid_codec = fmt->streams[ctx->video_idx]->codecpar->codec_id;
		has_video = 1;
	}
	if (ctx->audio_idx >= 0)
		aud_codec = fmt->streams[ctx->audio_idx]->codecpar->codec_id;

	fmt_name = fmt->iformat->name;

	/* Determine if transcoding is needed */
	if (has_video) {
		ctx->needs_transcode = !(video_codec_ok(vid_codec) &&
		    (ctx->audio_idx < 0 || audio_codec_ok(aud_codec)) &&
		    container_ok(fmt_name));
	} else {
		/* Audio-only */
		ctx->needs_transcode = !(audio_codec_ok(aud_codec) &&
		    container_ok(fmt_name));
	}

	if (!ctx->needs_transcode)
		set_mime_type(ctx, fmt_name);
	else
		strlcpy(ctx->mime_type, "video/mp2t",
		    sizeof(ctx->mime_type));

	/* Keep format context open if we need to transcode */
	if (ctx->needs_transcode) {
		ctx->ifmt_ctx = fmt;
	} else {
		avformat_close_input(&fmt);
	}

	return 0;
}

/*
 * Try to initialize VAAPI hardware device context.
 * Returns 0 on success, -1 on failure (caller should fall back to software).
 */
static int
init_vaapi(media_ctx_t *ctx)
{
	int ret;

	ret = av_hwdevice_ctx_create(&ctx->hw_device_ctx,
	    AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0);
	if (ret < 0) {
		fprintf(stderr, "VAAPI init failed: %s, "
		    "falling back to software encoding\n",
		    av_err2str(ret));
		return -1;
	}

	return 0;
}

/*
 * Set up the video filter graph for VAAPI encoding:
 *   format=nv12,hwupload,scale_vaapi=format=nv12
 * For software fallback:
 *   format=yuv420p
 */
static int
init_video_filters(media_ctx_t *ctx, int width, int height,
    AVRational time_base, enum AVPixelFormat pix_fmt, int use_vaapi)
{
	char			 args[512];
	char			 filter_descr[256];
	const AVFilter		*buffersrc, *buffersink;
	AVFilterInOut		*inputs = NULL, *outputs = NULL;
	int			 ret;

	ctx->filter_graph = avfilter_graph_alloc();
	if (ctx->filter_graph == NULL)
		return -1;

	buffersrc = avfilter_get_by_name("buffer");
	buffersink = avfilter_get_by_name("buffersink");

	snprintf(args, sizeof(args),
	    "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d",
	    width, height, pix_fmt, time_base.num, time_base.den);

	ret = avfilter_graph_create_filter(&ctx->buffersrc_ctx, buffersrc,
	    "in", args, NULL, ctx->filter_graph);
	if (ret < 0)
		goto fail;

	ret = avfilter_graph_create_filter(&ctx->buffersink_ctx, buffersink,
	    "out", NULL, NULL, ctx->filter_graph);
	if (ret < 0)
		goto fail;

	if (use_vaapi) {
		/* Attach hw device to filter graph */
		unsigned int i;

		for (i = 0; i < ctx->filter_graph->nb_filters; i++) {
			ctx->filter_graph->filters[i]->hw_device_ctx =
			    av_buffer_ref(ctx->hw_device_ctx);
		}

		snprintf(filter_descr, sizeof(filter_descr),
		    "format=nv12,hwupload,scale_vaapi=format=nv12");
	} else {
		snprintf(filter_descr, sizeof(filter_descr),
		    "format=yuv420p");
	}

	inputs = avfilter_inout_alloc();
	outputs = avfilter_inout_alloc();
	if (inputs == NULL || outputs == NULL)
		goto fail;

	outputs->name = av_strdup("in");
	outputs->filter_ctx = ctx->buffersrc_ctx;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	inputs->name = av_strdup("out");
	inputs->filter_ctx = ctx->buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = NULL;

	ret = avfilter_graph_parse_ptr(ctx->filter_graph, filter_descr,
	    &inputs, &outputs, NULL);
	if (ret < 0)
		goto fail;

	/* Set hw device context on filters that need it */
	if (use_vaapi) {
		unsigned int i;
		for (i = 0; i < ctx->filter_graph->nb_filters; i++) {
			if (ctx->filter_graph->filters[i]->hw_device_ctx == NULL)
				ctx->filter_graph->filters[i]->hw_device_ctx =
				    av_buffer_ref(ctx->hw_device_ctx);
		}
	}

	ret = avfilter_graph_config(ctx->filter_graph, NULL);
	if (ret < 0)
		goto fail;

	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	return 0;

fail:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	avfilter_graph_free(&ctx->filter_graph);
	ctx->filter_graph = NULL;
	return -1;
}

/*
 * Set up the output muxer writing MPEG-TS to a pipe.
 */
static int
init_output(media_ctx_t *ctx, int has_video, int has_audio)
{
	int		 pipefd[2];
	uint8_t		*avio_buf;
	AVIOContext	*avio;
	int		 ret;

	if (pipe(pipefd) < 0) {
		perror("pipe");
		return -1;
	}
	ctx->pipe_rd = pipefd[0];
	ctx->pipe_wr = pipefd[1];

	ret = avformat_alloc_output_context2(&ctx->ofmt_ctx, NULL,
	    "mpegts", NULL);
	if (ret < 0) {
		fprintf(stderr, "Cannot create output context: %s\n",
		    av_err2str(ret));
		return -1;
	}

	/* Custom AVIO writing to the pipe */
	avio_buf = av_malloc(SEND2TV_BUF_SIZE);
	if (avio_buf == NULL)
		return -1;

	avio = avio_alloc_context(avio_buf, SEND2TV_BUF_SIZE, 1,
	    &ctx->pipe_wr, NULL, avio_write_pipe, NULL);
	if (avio == NULL) {
		av_free(avio_buf);
		return -1;
	}

	ctx->ofmt_ctx->pb = avio;
	ctx->ofmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

	/* Add video output stream */
	if (has_video && ctx->video_enc != NULL) {
		AVStream *out_st = avformat_new_stream(ctx->ofmt_ctx, NULL);
		if (out_st == NULL)
			return -1;
		avcodec_parameters_from_context(out_st->codecpar,
		    ctx->video_enc);
		out_st->time_base = ctx->video_enc->time_base;
	}

	/* Add audio output stream */
	if (has_audio && ctx->audio_enc != NULL) {
		AVStream *out_st = avformat_new_stream(ctx->ofmt_ctx, NULL);
		if (out_st == NULL)
			return -1;
		avcodec_parameters_from_context(out_st->codecpar,
		    ctx->audio_enc);
		out_st->time_base = ctx->audio_enc->time_base;
	}

	return 0;
}

/*
 * Set up video encoder (VAAPI or software fallback).
 */
static int
init_video_encoder(media_ctx_t *ctx, int width, int height,
    AVRational time_base, AVRational framerate, int use_vaapi)
{
	const AVCodec	*codec;
	int		 ret;

	if (use_vaapi)
		codec = avcodec_find_encoder_by_name("h264_vaapi");
	else
		codec = avcodec_find_encoder_by_name("libx264");

	if (codec == NULL) {
		/* Try generic H.264 encoder */
		codec = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (codec == NULL) {
			fprintf(stderr, "No H.264 encoder found\n");
			return -1;
		}
	}

	ctx->video_enc = avcodec_alloc_context3(codec);
	if (ctx->video_enc == NULL)
		return -1;

	ctx->video_enc->width = width;
	ctx->video_enc->height = height;
	ctx->video_enc->time_base = time_base;
	ctx->video_enc->framerate = framerate;
	ctx->video_enc->gop_size = framerate.num > 0 ?
	    framerate.num / framerate.den : 30;
	ctx->video_enc->max_b_frames = 0;

	if (use_vaapi) {
		AVBufferRef *hw_frames_ref;
		AVHWFramesContext *hw_frames;

		ctx->video_enc->pix_fmt = AV_PIX_FMT_VAAPI;

		/* Create hw frames context */
		hw_frames_ref = av_hwframe_ctx_alloc(ctx->hw_device_ctx);
		if (hw_frames_ref == NULL)
			return -1;

		hw_frames = (AVHWFramesContext *)hw_frames_ref->data;
		hw_frames->format = AV_PIX_FMT_VAAPI;
		hw_frames->sw_format = AV_PIX_FMT_NV12;
		hw_frames->width = width;
		hw_frames->height = height;
		hw_frames->initial_pool_size = 20;

		ret = av_hwframe_ctx_init(hw_frames_ref);
		if (ret < 0) {
			av_buffer_unref(&hw_frames_ref);
			return -1;
		}

		ctx->video_enc->hw_frames_ctx =
		    av_buffer_ref(hw_frames_ref);
		av_buffer_unref(&hw_frames_ref);
	} else {
		ctx->video_enc->pix_fmt = AV_PIX_FMT_YUV420P;
		ctx->video_enc->bit_rate = 4000000;
		av_opt_set(ctx->video_enc->priv_data, "preset",
		    "ultrafast", 0);
		av_opt_set(ctx->video_enc->priv_data, "tune",
		    "zerolatency", 0);
		av_opt_set(ctx->video_enc->priv_data, "refs", "3", 0);
	}

	ret = avcodec_open2(ctx->video_enc, codec, NULL);
	if (ret < 0) {
		fprintf(stderr, "Cannot open video encoder: %s\n",
		    av_err2str(ret));
		return -1;
	}

	return 0;
}

/*
 * Set up AAC audio encoder.
 */
static int
init_audio_encoder(media_ctx_t *ctx, int sample_rate, int channels)
{
	const AVCodec		*codec;
	AVChannelLayout		 ch_layout;
	int			 ret;

	codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (codec == NULL) {
		fprintf(stderr, "No AAC encoder found\n");
		return -1;
	}

	ctx->audio_enc = avcodec_alloc_context3(codec);
	if (ctx->audio_enc == NULL)
		return -1;

	ctx->audio_enc->sample_rate = sample_rate;
	ctx->audio_enc->sample_fmt = codec->sample_fmts ?
	    codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
	ctx->audio_enc->bit_rate = 128000;
	ctx->audio_enc->time_base = (AVRational){1, sample_rate};

	av_channel_layout_default(&ch_layout, channels);
	av_channel_layout_copy(&ctx->audio_enc->ch_layout, &ch_layout);
	av_channel_layout_uninit(&ch_layout);

	ret = avcodec_open2(ctx->audio_enc, codec, NULL);
	if (ret < 0) {
		fprintf(stderr, "Cannot open audio encoder: %s\n",
		    av_err2str(ret));
		return -1;
	}

	return 0;
}

/*
 * Set up audio resampler.
 */
static int
init_audio_resampler(media_ctx_t *ctx, AVCodecContext *dec)
{
	int ret;

	ret = swr_alloc_set_opts2(&ctx->swr_ctx,
	    &ctx->audio_enc->ch_layout, ctx->audio_enc->sample_fmt,
	    ctx->audio_enc->sample_rate,
	    &dec->ch_layout, dec->sample_fmt, dec->sample_rate,
	    0, NULL);
	if (ret < 0)
		return -1;

	ret = swr_init(ctx->swr_ctx);
	if (ret < 0) {
		fprintf(stderr, "Cannot init resampler: %s\n",
		    av_err2str(ret));
		return -1;
	}

	return 0;
}

/*
 * Set up transcoding pipeline for a file.
 * Assumes media_probe() already opened ifmt_ctx and set needs_transcode.
 */
int
media_open_transcode(media_ctx_t *ctx)
{
	const AVCodec	*dec;
	AVStream	*in_st;
	int		 ret, use_vaapi;
	int		 width, height;
	AVRational	 tb, fr;
	enum AVPixelFormat pix_fmt;
	int		 has_audio;

	avdevice_register_all();

	/* Open video decoder */
	if (ctx->video_idx >= 0) {
		in_st = ctx->ifmt_ctx->streams[ctx->video_idx];
		dec = avcodec_find_decoder(in_st->codecpar->codec_id);
		if (dec == NULL) {
			fprintf(stderr, "No decoder for video\n");
			return -1;
		}
		ctx->video_dec = avcodec_alloc_context3(dec);
		avcodec_parameters_to_context(ctx->video_dec,
		    in_st->codecpar);
		ret = avcodec_open2(ctx->video_dec, dec, NULL);
		if (ret < 0) {
			fprintf(stderr, "Cannot open video decoder: %s\n",
			    av_err2str(ret));
			return -1;
		}
		width = ctx->video_dec->width;
		height = ctx->video_dec->height;
		pix_fmt = ctx->video_dec->pix_fmt;
		tb = in_st->time_base;
		fr = av_guess_frame_rate(ctx->ifmt_ctx, in_st, NULL);
		if (fr.num == 0) {
			fr = (AVRational){30, 1};
		}
	} else {
		width = 0;
		height = 0;
		pix_fmt = AV_PIX_FMT_NONE;
		tb = (AVRational){1, 48000};
		fr = (AVRational){0, 1};
	}

	/* Open audio decoder */
	has_audio = 0;
	if (ctx->audio_idx >= 0) {
		in_st = ctx->ifmt_ctx->streams[ctx->audio_idx];
		dec = avcodec_find_decoder(in_st->codecpar->codec_id);
		if (dec != NULL) {
			ctx->audio_dec = avcodec_alloc_context3(dec);
			avcodec_parameters_to_context(ctx->audio_dec,
			    in_st->codecpar);
			ret = avcodec_open2(ctx->audio_dec, dec, NULL);
			if (ret >= 0)
				has_audio = 1;
		}
	}

	/* Init VAAPI */
	use_vaapi = 0;
	if (ctx->video_idx >= 0 && init_vaapi(ctx) == 0)
		use_vaapi = 1;

	/* Init video encoder */
	if (ctx->video_idx >= 0) {
		if (init_video_encoder(ctx, width, height,
		    (AVRational){1, fr.num / fr.den}, fr, use_vaapi) < 0) {
			if (use_vaapi) {
				/* Retry with software */
				fprintf(stderr, "VAAPI encoder failed, "
				    "trying software\n");
				av_buffer_unref(&ctx->hw_device_ctx);
				use_vaapi = 0;
				if (init_video_encoder(ctx, width, height,
				    (AVRational){1, fr.num / fr.den}, fr,
				    0) < 0)
					return -1;
			} else {
				return -1;
			}
		}

		/* Init video filter graph */
		if (init_video_filters(ctx, width, height, tb,
		    pix_fmt, use_vaapi) < 0) {
			if (use_vaapi) {
				fprintf(stderr, "VAAPI filters failed, "
				    "trying software\n");
				avcodec_free_context(&ctx->video_enc);
				av_buffer_unref(&ctx->hw_device_ctx);
				use_vaapi = 0;
				if (init_video_encoder(ctx, width, height,
				    (AVRational){1, fr.num / fr.den}, fr,
				    0) < 0)
					return -1;
				if (init_video_filters(ctx, width, height,
				    tb, pix_fmt, 0) < 0)
					return -1;
			} else {
				return -1;
			}
		}
	}

	/* Init audio encoder */
	if (has_audio) {
		if (init_audio_encoder(ctx,
		    ctx->audio_dec->sample_rate,
		    ctx->audio_dec->ch_layout.nb_channels) < 0)
			return -1;
		if (init_audio_resampler(ctx, ctx->audio_dec) < 0)
			return -1;
	}

	/* Init output muxer */
	if (init_output(ctx, ctx->video_idx >= 0, has_audio) < 0)
		return -1;

	strlcpy(ctx->mime_type, "video/mp2t", sizeof(ctx->mime_type));

	return 0;
}

/*
 * Set up screen + sndio audio capture.
 */
int
media_open_screen(media_ctx_t *ctx)
{
	const AVInputFormat	*x11grab_fmt, *sndio_fmt;
	AVDictionary		*opts = NULL;
	const AVCodec		*dec;
	AVStream		*st;
	int			 ret, use_vaapi;
	int			 width, height;
	AVRational		 fr = {30, 1};

	avdevice_register_all();

	/* Open X11 screen capture */
	x11grab_fmt = av_find_input_format("x11grab");
	if (x11grab_fmt == NULL) {
		fprintf(stderr, "x11grab not available\n");
		return -1;
	}

	av_dict_set(&opts, "framerate", "30", 0);
	av_dict_set(&opts, "draw_mouse", "1", 0);

	ret = avformat_open_input(&ctx->ifmt_ctx, ":0.0", x11grab_fmt,
	    &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		fprintf(stderr, "Cannot open X11 display: %s\n",
		    av_err2str(ret));
		return -1;
	}

	ret = avformat_find_stream_info(ctx->ifmt_ctx, NULL);
	if (ret < 0) {
		fprintf(stderr, "Cannot get x11grab stream info: %s\n",
		    av_err2str(ret));
		return -1;
	}

	ctx->video_idx = 0;
	st = ctx->ifmt_ctx->streams[0];
	width = st->codecpar->width;
	height = st->codecpar->height;

	/* Open x11grab decoder */
	dec = avcodec_find_decoder(st->codecpar->codec_id);
	if (dec == NULL) {
		fprintf(stderr, "No rawvideo decoder\n");
		return -1;
	}
	ctx->video_dec = avcodec_alloc_context3(dec);
	avcodec_parameters_to_context(ctx->video_dec, st->codecpar);
	ret = avcodec_open2(ctx->video_dec, dec, NULL);
	if (ret < 0) {
		fprintf(stderr, "Cannot open rawvideo decoder: %s\n",
		    av_err2str(ret));
		return -1;
	}

	/* Open sndio audio capture (monitor device) */
	sndio_fmt = av_find_input_format("sndio");
	if (sndio_fmt != NULL) {
		opts = NULL;
		av_dict_set(&opts, "sample_rate", "48000", 0);
		av_dict_set(&opts, "channels", "2", 0);
		ret = avformat_open_input(&ctx->sndio_ctx, "snd/0.mon",
		    sndio_fmt, &opts);
		av_dict_free(&opts);
		if (ret < 0) {
			fprintf(stderr, "Cannot open sndio monitor: %s "
			    "(continuing without audio)\n",
			    av_err2str(ret));
			ctx->sndio_ctx = NULL;
		} else {
			avformat_find_stream_info(ctx->sndio_ctx, NULL);
			ctx->sndio_audio_idx = 0;
			st = ctx->sndio_ctx->streams[0];
			dec = avcodec_find_decoder(st->codecpar->codec_id);
			if (dec != NULL) {
				ctx->sndio_dec =
				    avcodec_alloc_context3(dec);
				avcodec_parameters_to_context(
				    ctx->sndio_dec, st->codecpar);
				avcodec_open2(ctx->sndio_dec, dec, NULL);
			}
		}
	} else {
		fprintf(stderr, "sndio input not available "
		    "(continuing without audio)\n");
	}

	/* Init VAAPI */
	use_vaapi = 0;
	if (init_vaapi(ctx) == 0)
		use_vaapi = 1;

	/* Init video encoder */
	if (init_video_encoder(ctx, width, height,
	    (AVRational){1, 30}, fr, use_vaapi) < 0) {
		if (use_vaapi) {
			fprintf(stderr, "VAAPI encoder failed, "
			    "trying software\n");
			av_buffer_unref(&ctx->hw_device_ctx);
			use_vaapi = 0;
			if (init_video_encoder(ctx, width, height,
			    (AVRational){1, 30}, fr, 0) < 0)
				return -1;
		} else {
			return -1;
		}
	}

	/* Init video filter graph */
	if (init_video_filters(ctx, width, height,
	    ctx->ifmt_ctx->streams[0]->time_base,
	    ctx->video_dec->pix_fmt, use_vaapi) < 0) {
		if (use_vaapi) {
			fprintf(stderr, "VAAPI filters failed, "
			    "trying software\n");
			avcodec_free_context(&ctx->video_enc);
			av_buffer_unref(&ctx->hw_device_ctx);
			use_vaapi = 0;
			if (init_video_encoder(ctx, width, height,
			    (AVRational){1, 30}, fr, 0) < 0)
				return -1;
			if (init_video_filters(ctx, width, height,
			    ctx->ifmt_ctx->streams[0]->time_base,
			    ctx->video_dec->pix_fmt, 0) < 0)
				return -1;
		} else {
			return -1;
		}
	}

	/* Init audio encoder if sndio is available */
	int has_audio = 0;
	if (ctx->sndio_dec != NULL) {
		if (init_audio_encoder(ctx, 48000, 2) == 0) {
			if (init_audio_resampler(ctx, ctx->sndio_dec) == 0)
				has_audio = 1;
		}
	}

	/* Init output */
	if (init_output(ctx, 1, has_audio) < 0)
		return -1;

	ctx->needs_transcode = 1;
	strlcpy(ctx->mime_type, "video/mp2t", sizeof(ctx->mime_type));

	return 0;
}

/*
 * Encode and write a filtered video frame.
 */
static int
encode_video_frame(media_ctx_t *ctx, AVFrame *frame, int64_t *vid_pts,
    int out_stream_idx)
{
	AVPacket	*pkt;
	int		 ret;

	if (frame != NULL)
		frame->pts = (*vid_pts)++;

	ret = avcodec_send_frame(ctx->video_enc, frame);
	if (ret < 0)
		return ret;

	pkt = av_packet_alloc();
	while (avcodec_receive_packet(ctx->video_enc, pkt) == 0) {
		av_packet_rescale_ts(pkt, ctx->video_enc->time_base,
		    ctx->ofmt_ctx->streams[out_stream_idx]->time_base);
		pkt->stream_index = out_stream_idx;
		av_interleaved_write_frame(ctx->ofmt_ctx, pkt);
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);

	return 0;
}

/*
 * Encode and write an audio frame.
 */
static int
encode_audio_frame(media_ctx_t *ctx, AVFrame *frame, int out_stream_idx)
{
	AVPacket	*pkt;
	int		 ret;

	ret = avcodec_send_frame(ctx->audio_enc, frame);
	if (ret < 0)
		return ret;

	pkt = av_packet_alloc();
	while (avcodec_receive_packet(ctx->audio_enc, pkt) == 0) {
		av_packet_rescale_ts(pkt, ctx->audio_enc->time_base,
		    ctx->ofmt_ctx->streams[out_stream_idx]->time_base);
		pkt->stream_index = out_stream_idx;
		av_interleaved_write_frame(ctx->ofmt_ctx, pkt);
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);

	return 0;
}

/*
 * Process video: decode, filter, encode.
 */
static int
process_video_packet(media_ctx_t *ctx, AVPacket *pkt, int64_t *vid_pts,
    int out_stream_idx)
{
	AVFrame		*frame, *filt_frame;
	int		 ret;

	frame = av_frame_alloc();
	filt_frame = av_frame_alloc();

	ret = avcodec_send_packet(ctx->video_dec, pkt);
	if (ret < 0)
		goto done;

	while (avcodec_receive_frame(ctx->video_dec, frame) == 0) {
		/* Push through filter graph */
		ret = av_buffersrc_add_frame_flags(ctx->buffersrc_ctx,
		    frame, AV_BUFFERSRC_FLAG_KEEP_REF);
		if (ret < 0)
			goto done;

		while (av_buffersink_get_frame(ctx->buffersink_ctx,
		    filt_frame) >= 0) {
			encode_video_frame(ctx, filt_frame, vid_pts,
			    out_stream_idx);
			av_frame_unref(filt_frame);
		}
		av_frame_unref(frame);
	}

done:
	av_frame_free(&frame);
	av_frame_free(&filt_frame);
	return 0;
}

/*
 * Process audio: decode, resample, encode.
 */
static int
process_audio_packet(media_ctx_t *ctx, AVPacket *pkt,
    AVCodecContext *dec, int out_stream_idx, int64_t *audio_pts)
{
	AVFrame		*frame, *out_frame;
	int		 ret;
	int		 out_samples;

	frame = av_frame_alloc();

	ret = avcodec_send_packet(dec, pkt);
	if (ret < 0)
		goto done;

	while (avcodec_receive_frame(dec, frame) == 0) {
		out_samples = swr_get_out_samples(ctx->swr_ctx,
		    frame->nb_samples);
		if (out_samples <= 0) {
			av_frame_unref(frame);
			continue;
		}

		out_frame = av_frame_alloc();
		out_frame->nb_samples = ctx->audio_enc->frame_size ?
		    ctx->audio_enc->frame_size : out_samples;
		out_frame->format = ctx->audio_enc->sample_fmt;
		av_channel_layout_copy(&out_frame->ch_layout,
		    &ctx->audio_enc->ch_layout);
		out_frame->sample_rate = ctx->audio_enc->sample_rate;
		av_frame_get_buffer(out_frame, 0);

		out_samples = swr_convert(ctx->swr_ctx,
		    out_frame->data, out_frame->nb_samples,
		    (const uint8_t **)frame->data, frame->nb_samples);

		if (out_samples > 0) {
			out_frame->nb_samples = out_samples;
			out_frame->pts = *audio_pts;
			*audio_pts += out_samples;
			encode_audio_frame(ctx, out_frame, out_stream_idx);
		}

		av_frame_free(&out_frame);
		av_frame_unref(frame);
	}

done:
	av_frame_free(&frame);
	return 0;
}

/*
 * Transcoding thread: reads from input, transcodes, writes to pipe.
 */
void *
media_transcode_thread(void *arg)
{
	media_ctx_t	*ctx = arg;
	AVPacket	*pkt;
	int		 ret;
	int64_t		 vid_pts = 0;
	int64_t		 audio_pts = 0;
	int		 audio_out_idx;

	ret = avformat_write_header(ctx->ofmt_ctx, NULL);
	if (ret < 0) {
		fprintf(stderr, "Cannot write header: %s\n",
		    av_err2str(ret));
		close(ctx->pipe_wr);
		ctx->pipe_wr = -1;
		return NULL;
	}

	/* Audio output stream index (video is 0 if present, audio is 1) */
	audio_out_idx = (ctx->video_idx >= 0) ? 1 : 0;

	pkt = av_packet_alloc();
	while (ctx->running && av_read_frame(ctx->ifmt_ctx, pkt) >= 0) {
		if (pkt->stream_index == ctx->video_idx) {
			process_video_packet(ctx, pkt, &vid_pts, 0);
		} else if (pkt->stream_index == ctx->audio_idx &&
		    ctx->audio_dec != NULL) {
			process_audio_packet(ctx, pkt, ctx->audio_dec,
			    audio_out_idx, &audio_pts);
		}
		av_packet_unref(pkt);
	}

	/* Flush encoders */
	if (ctx->video_enc != NULL)
		encode_video_frame(ctx, NULL, &vid_pts, 0);
	if (ctx->audio_enc != NULL)
		encode_audio_frame(ctx, NULL, audio_out_idx);

	av_write_trailer(ctx->ofmt_ctx);
	av_packet_free(&pkt);

	close(ctx->pipe_wr);
	ctx->pipe_wr = -1;

	return NULL;
}

/*
 * Screen + audio capture thread.
 */
void *
media_capture_thread(void *arg)
{
	media_ctx_t	*ctx = arg;
	AVPacket	*vid_pkt, *aud_pkt;
	int		 ret;
	int64_t		 vid_pts = 0;
	int64_t		 audio_pts = 0;
	int		 audio_out_idx = 1;

	ret = avformat_write_header(ctx->ofmt_ctx, NULL);
	if (ret < 0) {
		fprintf(stderr, "Cannot write header: %s\n",
		    av_err2str(ret));
		close(ctx->pipe_wr);
		ctx->pipe_wr = -1;
		return NULL;
	}

	vid_pkt = av_packet_alloc();
	aud_pkt = av_packet_alloc();

	while (ctx->running) {
		/* Read video frame from x11grab */
		ret = av_read_frame(ctx->ifmt_ctx, vid_pkt);
		if (ret < 0)
			break;

		process_video_packet(ctx, vid_pkt, &vid_pts, 0);
		av_packet_unref(vid_pkt);

		/* Read available audio from sndio (non-blocking-ish) */
		if (ctx->sndio_ctx != NULL && ctx->sndio_dec != NULL) {
			while (av_read_frame(ctx->sndio_ctx,
			    aud_pkt) >= 0) {
				process_audio_packet(ctx, aud_pkt,
				    ctx->sndio_dec, audio_out_idx,
				    &audio_pts);
				av_packet_unref(aud_pkt);

				/*
				 * Only process a few audio packets per
				 * video frame to avoid falling behind.
				 */
				break;
			}
		}
	}

	/* Flush */
	if (ctx->video_enc != NULL)
		encode_video_frame(ctx, NULL, &vid_pts, 0);
	if (ctx->audio_enc != NULL)
		encode_audio_frame(ctx, NULL, audio_out_idx);

	av_write_trailer(ctx->ofmt_ctx);

	av_packet_free(&vid_pkt);
	av_packet_free(&aud_pkt);

	close(ctx->pipe_wr);
	ctx->pipe_wr = -1;

	return NULL;
}

void
media_close(media_ctx_t *ctx)
{
	if (ctx->swr_ctx != NULL)
		swr_free(&ctx->swr_ctx);
	if (ctx->filter_graph != NULL)
		avfilter_graph_free(&ctx->filter_graph);
	if (ctx->video_enc != NULL)
		avcodec_free_context(&ctx->video_enc);
	if (ctx->audio_enc != NULL)
		avcodec_free_context(&ctx->audio_enc);
	if (ctx->video_dec != NULL)
		avcodec_free_context(&ctx->video_dec);
	if (ctx->audio_dec != NULL)
		avcodec_free_context(&ctx->audio_dec);
	if (ctx->sndio_dec != NULL)
		avcodec_free_context(&ctx->sndio_dec);
	if (ctx->ifmt_ctx != NULL)
		avformat_close_input(&ctx->ifmt_ctx);
	if (ctx->sndio_ctx != NULL)
		avformat_close_input(&ctx->sndio_ctx);
	if (ctx->ofmt_ctx != NULL) {
		if (ctx->ofmt_ctx->pb != NULL) {
			av_free(ctx->ofmt_ctx->pb->buffer);
			avio_context_free(&ctx->ofmt_ctx->pb);
		}
		avformat_free_context(ctx->ofmt_ctx);
	}
	if (ctx->hw_device_ctx != NULL)
		av_buffer_unref(&ctx->hw_device_ctx);
	if (ctx->pipe_rd >= 0)
		close(ctx->pipe_rd);
	if (ctx->pipe_wr >= 0)
		close(ctx->pipe_wr);
}
