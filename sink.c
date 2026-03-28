#include <stdio.h>
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
	int		 srv_fd   = -1;
	int		 seg_id   = 0;
	int		 ret      = -1;
	struct sockaddr_un addr;
	char		 media_url[256];

	memset(&httpd, 0, sizeof(httpd));
	memset(&media, 0, sizeof(media));
	media.pipe_rd = -1;
	media.pipe_wr = -1;
	media.mode    = MODE_SINK;

	/* Start HTTP server */
	if (httpd_start(&httpd, &media, port) < 0) {
		fprintf(stderr, "sink: cannot start HTTP server\n");
		goto done;
	}

	/* Ensure TV is in a clean stopped state while we wait for input */
	upnp_stop(upnp);

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
	return ret;
}
