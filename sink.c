#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "send2tv.h"

/* ---------------------------------------------------------------------- */
/* AVIO read callback: reads from Unix socket fd.                          */
/* ---------------------------------------------------------------------- */

typedef struct {
	int		 fd;
	volatile int	*running_ctx; /* points to media_ctx_t.running */
} avio_sock_t;

static int
avio_read_sock(void *opaque, uint8_t *buf, int buf_size)
{
	avio_sock_t	*s = opaque;
	struct pollfd	 pfd;
	ssize_t		 n;

	pfd.fd     = s->fd;
	pfd.events = POLLIN;

	while (*s->running_ctx && running) {
		int r = poll(&pfd, 1, 200);
		if (r == 0)
			continue;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return AVERROR(errno);
		}
		n = read(s->fd, buf, buf_size);
		if (n > 0)  return (int)n;
		if (n == 0) return AVERROR_EOF;
		if (errno == EINTR) continue;
		return AVERROR(errno);
	}
	return AVERROR(EINTR);
}

/* ---------------------------------------------------------------------- */
/* Generate a 64x36 black JPEG using libavcodec.                           */
/* ---------------------------------------------------------------------- */

static int
make_black_jpeg(uint8_t **out, size_t *outsz)
{
	const AVCodec	*codec;
	AVCodecContext	*cctx  = NULL;
	AVFrame		*frame = NULL;
	AVPacket	*pkt   = NULL;
	int		 ret   = -1;

	codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	if (codec == NULL) {
		fprintf(stderr, "sink: MJPEG encoder not available\n");
		return -1;
	}

	cctx = avcodec_alloc_context3(codec);
	if (cctx == NULL)
		return -1;

	cctx->width     = 64;
	cctx->height    = 36;
	cctx->time_base = (AVRational){1, 25};
	cctx->pix_fmt   = AV_PIX_FMT_YUVJ420P;  /* full-range JPEG */

	if (avcodec_open2(cctx, codec, NULL) < 0)
		goto done;

	frame = av_frame_alloc();
	if (frame == NULL)
		goto done;

	frame->width  = 64;
	frame->height = 36;
	frame->format = AV_PIX_FMT_YUVJ420P;
	frame->pts    = 0;

	if (av_frame_get_buffer(frame, 0) < 0 ||
	    av_frame_make_writable(frame) < 0)
		goto done;

	/* Black: Y=0, U=128, V=128 (full-range) */
	memset(frame->data[0], 0,   frame->linesize[0] * cctx->height);
	memset(frame->data[1], 128, frame->linesize[1] * (cctx->height / 2));
	memset(frame->data[2], 128, frame->linesize[2] * (cctx->height / 2));

	pkt = av_packet_alloc();
	if (pkt == NULL)
		goto done;

	if (avcodec_send_frame(cctx, frame) >= 0 &&
	    avcodec_receive_packet(cctx, pkt) >= 0) {
		*out = av_malloc(pkt->size);
		if (*out != NULL) {
			memcpy(*out, pkt->data, pkt->size);
			*outsz = pkt->size;
			ret    = 0;
		}
	}

done:
	av_packet_free(&pkt);
	av_frame_free(&frame);
	avcodec_free_context(&cctx);
	return ret;
}

/* ---------------------------------------------------------------------- */
/* Open an AVFormatContext from a connected socket fd.                     */
/* Sets ctx->ifmt_ctx and ctx->avio_in.                                    */
/* ---------------------------------------------------------------------- */

static int
open_from_sock(media_ctx_t *ctx, int client_fd, avio_sock_t *s)
{
	AVFormatContext	*fmt      = NULL;
	AVIOContext	*avio_ctx = NULL;
	uint8_t		*avio_buf = NULL;

	s->fd          = client_fd;
	s->running_ctx = (volatile int *)&ctx->running;

	avio_buf = av_malloc(SEND2TV_BUF_SIZE);
	if (avio_buf == NULL)
		return -1;

	avio_ctx = avio_alloc_context(avio_buf, SEND2TV_BUF_SIZE,
	    0, s, avio_read_sock, NULL, NULL);
	if (avio_ctx == NULL) {
		av_free(avio_buf);
		return -1;
	}

	fmt = avformat_alloc_context();
	if (fmt == NULL) {
		av_free(avio_ctx->buffer);
		avio_context_free(&avio_ctx);
		return -1;
	}

	fmt->pb    = avio_ctx;
	fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	fmt->interrupt_callback.callback = ffmpeg_interrupt_cb;
	fmt->interrupt_callback.opaque   = ctx;

	if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) {
		/* fmt freed by avformat_open_input on failure,
		 * but pb is custom — free it */
		av_free(avio_ctx->buffer);
		avio_context_free(&avio_ctx);
		fprintf(stderr, "sink: cannot open stream\n");
		return -1;
	}

	ctx->ifmt_ctx = fmt;
	ctx->avio_in  = avio_ctx;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Main sink loop.                                                          */
/* upnp must already be connected (upnp_find_transport done by caller).   */
/* ---------------------------------------------------------------------- */

int
sink_run(upnp_ctx_t *upnp, const char *sock_path, int port,
    int vcodec, int bitrate)
{
	httpd_ctx_t	 httpd;
	media_ctx_t	 media;
	uint8_t		*img_data = NULL;
	size_t		 img_size = 0;
	int		 srv_fd   = -1;
	int		 seg_id   = 0;
	int		 ret      = -1;
	struct sockaddr_un addr;
	char		 image_url[256];
	char		 media_url[256];

	memset(&httpd, 0, sizeof(httpd));
	memset(&media, 0, sizeof(media));
	media.pipe_rd = -1;
	media.pipe_wr = -1;
	media.mode    = MODE_SINK;

	/* Generate idle image */
	if (make_black_jpeg(&img_data, &img_size) < 0) {
		fprintf(stderr, "sink: cannot generate idle image\n");
		return -1;
	}

	httpd.image_data = img_data;
	httpd.image_size = img_size;

	/* Start HTTP server */
	if (httpd_start(&httpd, &media, port) < 0) {
		fprintf(stderr, "sink: cannot start HTTP server\n");
		goto done;
	}

	snprintf(image_url, sizeof(image_url),
	    "http://%s:%d/image", upnp->local_ip, httpd.port);

	/* Show idle image on TV */
	printf("Sink: idle image at %s\n", image_url);
	if (upnp_set_uri(upnp, image_url, "image/jpeg",
	    "Waiting", 0, "JPEG_LRG") < 0 ||
	    upnp_play(upnp) < 0) {
		fprintf(stderr, "sink: cannot show idle image on TV\n");
		goto done;
	}

	/* Create Unix domain socket */
	srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv_fd < 0) {
		perror("socket");
		goto done;
	}

	unlink(sock_path);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, sock_path, sizeof(addr.sun_path));

	if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		goto done;
	}
	if (listen(srv_fd, 1) < 0) {
		perror("listen");
		goto done;
	}

	printf("Sink: listening on %s\n", sock_path);
	printf("      cat video.mp4 | nc -U %s\n", sock_path);
	ret = 0;

	while (running) {
		struct pollfd	 pfd;
		avio_sock_t	 avio_sock;
		int		 client_fd;

		pfd.fd     = srv_fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 500) <= 0)
			continue;

		client_fd = accept(srv_fd, NULL, NULL);
		if (client_fd < 0)
			continue;

		printf("\nSink: client connected\n");

		/* Reset media context for this segment */
		memset(&media, 0, sizeof(media));
		media.pipe_rd = -1;
		media.pipe_wr = -1;
		media.mode    = MODE_SINK;
		media.running = 1;
		media.vcodec  = vcodec;
		media.bitrate = bitrate;

		if (open_from_sock(&media, client_fd, &avio_sock) < 0) {
			fprintf(stderr, "sink: cannot open stream from "
			    "client\n");
			close(client_fd);
			continue;
		}

		if (media_probe_avfmt(&media, media.ifmt_ctx, 0) < 0) {
			fprintf(stderr, "sink: cannot determine stream "
			    "format\n");
			media_close(&media);
			close(client_fd);
			memset(&media, 0, sizeof(media));
			media.pipe_rd = -1;
			media.pipe_wr = -1;
			media.mode    = MODE_SINK;
			continue;
		}

		if (media.needs_transcode) {
			printf("Sink: transcoding\n");
			if (media_open_transcode(&media) < 0) {
				fprintf(stderr, "sink: transcode setup "
				    "failed\n");
				media_close(&media);
				close(client_fd);
				memset(&media, 0, sizeof(media));
				media.pipe_rd = -1;
				media.pipe_wr = -1;
				media.mode    = MODE_SINK;
				continue;
			}
		} else {
			printf("Sink: remuxing to MPEG-TS\n");
			if (media_open_remux(&media) < 0) {
				fprintf(stderr, "sink: remux setup failed\n");
				media_close(&media);
				close(client_fd);
				memset(&media, 0, sizeof(media));
				media.pipe_rd = -1;
				media.pipe_wr = -1;
				media.mode    = MODE_SINK;
				continue;
			}
		}

		if (pthread_create(&media.thread, NULL,
		    media.needs_transcode ? media_transcode_thread
		                          : media_remux_thread,
		    &media) != 0) {
			fprintf(stderr, "sink: cannot start processing "
			    "thread\n");
			media_close(&media);
			close(client_fd);
			memset(&media, 0, sizeof(media));
			media.pipe_rd = -1;
			media.pipe_wr = -1;
			media.mode    = MODE_SINK;
			continue;
		}

		seg_id++;
		snprintf(media_url, sizeof(media_url),
		    "http://%s:%d/media?id=%d",
		    upnp->local_ip, httpd.port, seg_id);

		printf("Sink: segment %d — %s\n", seg_id, media_url);

		if (upnp_set_uri(upnp, media_url, media.mime_type,
		    "Sink", 1, media.dlna_profile) < 0 ||
		    upnp_play(upnp) < 0) {
			fprintf(stderr, "sink: cannot start playback on "
			    "TV\n");
			media.running = 0;
		}

		pthread_join(media.thread, NULL);
		media_close(&media);
		close(client_fd);

		/* Reset for next client */
		memset(&media, 0, sizeof(media));
		media.pipe_rd = -1;
		media.pipe_wr = -1;
		media.mode    = MODE_SINK;

		if (!running)
			break;

		printf("Sink: segment done, going idle\n");
		upnp_stop(upnp);
	}

done:
	if (srv_fd >= 0) {
		close(srv_fd);
		unlink(sock_path);
	}
	httpd_stop(&httpd);
	av_free(img_data);
	return ret;
}
