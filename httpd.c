#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>

#include "send2tv.h"

/*
 * Send a complete buffer to a socket, handling partial writes.
 */
static int
send_all(int fd, const void *buf, size_t len)
{
	const char	*p = buf;
	ssize_t		 n;

	while (len > 0) {
		n = send(fd, p, len, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= n;
	}
	return 0;
}

/*
 * Send HTTP response headers.
 */
static void
send_headers(int fd, int status, const char *status_text,
    const char *content_type, off_t content_length,
    off_t range_start, off_t range_end, off_t total_size,
    int is_streaming, const char *dlna_profile)
{
	char	 hdrs[2048];
	char	 dlna_features[256];
	int	 n;

	if (dlna_profile != NULL && dlna_profile[0] != '\0')
		snprintf(dlna_features, sizeof(dlna_features),
		    "DLNA.ORG_PN=%s;DLNA.ORG_OP=%s;DLNA.ORG_CI=%s;"
		    "DLNA.ORG_FLAGS="
		    "01700000000000000000000000000000",
		    dlna_profile,
		    is_streaming ? "00" : "01",
		    is_streaming ? "1" : "0");
	else
		snprintf(dlna_features, sizeof(dlna_features),
		    "DLNA.ORG_OP=%s;DLNA.ORG_CI=%s;"
		    "DLNA.ORG_FLAGS="
		    "01700000000000000000000000000000",
		    is_streaming ? "00" : "01",
		    is_streaming ? "1" : "0");

	n = snprintf(hdrs, sizeof(hdrs),
	    "HTTP/1.1 %d %s\r\n"
	    "Content-Type: %s\r\n"
	    "transferMode.dlna.org: Streaming\r\n"
	    "contentFeatures.dlna.org: %s\r\n"
	    "Connection: close\r\n",
	    status, status_text, content_type, dlna_features);

	if (status == 206 && total_size > 0)
		n += snprintf(hdrs + n, sizeof(hdrs) - n,
		    "Content-Range: bytes %lld-%lld/%lld\r\n"
		    "Content-Length: %lld\r\n",
		    (long long)range_start, (long long)range_end,
		    (long long)total_size,
		    (long long)(range_end - range_start + 1));
	else if (content_length >= 0)
		n += snprintf(hdrs + n, sizeof(hdrs) - n,
		    "Content-Length: %lld\r\n",
		    (long long)content_length);

	strlcat(hdrs, "\r\n", sizeof(hdrs));
	send_all(fd, hdrs, strlen(hdrs));
}

/*
 * Serve a file directly (passthrough mode).
 */
static void
serve_file(int client_fd, media_ctx_t *media, int head_only,
    off_t range_start)
{
	struct stat	 st;
	int		 fd;
	char		 buf[SEND2TV_BUF_SIZE];
	ssize_t		 n;
	off_t		 total, end;

	DPRINTF("httpd: serving file %s (range=%lld)\n",
	    media->filepath, (long long)range_start);

	if (stat(media->filepath, &st) < 0) {
		send_headers(client_fd, 404, "Not Found",
		    "text/plain", 9, -1, -1, -1, 0, NULL);
		if (!head_only)
			send_all(client_fd, "Not Found", 9);
		return;
	}

	total = st.st_size;
	if (range_start < 0 || range_start >= total)
		range_start = 0;
	end = total - 1;

	if (range_start > 0)
		send_headers(client_fd, 206, "Partial Content",
		    media->mime_type, -1, range_start, end, total, 0,
		    media->dlna_profile);
	else
		send_headers(client_fd, 200, "OK",
		    media->mime_type, total, -1, -1, -1, 0,
		    media->dlna_profile);

	if (head_only)
		return;

	fd = open(media->filepath, O_RDONLY);
	if (fd < 0)
		return;

	if (range_start > 0)
		lseek(fd, range_start, SEEK_SET);

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (send_all(client_fd, buf, n) < 0)
			break;
	}

	close(fd);
}

/*
 * Serve from a pipe (transcoded or captured stream).
 */
static void
serve_pipe(int client_fd, media_ctx_t *media, int head_only)
{
	char		 buf[SEND2TV_BUF_SIZE];
	ssize_t		 n;

	DPRINTF("httpd: serving from pipe, mime=%s\n", media->mime_type);

	send_headers(client_fd, 200, "OK",
	    media->mime_type, -1, -1, -1, -1, 1,
	    media->dlna_profile);

	if (head_only)
		return;

	while (media->running) {
		n = read(media->pipe_rd, buf, sizeof(buf));
		if (n <= 0)
			break;
		if (send_all(client_fd, buf, n) < 0)
			break;
	}
}

/*
 * Handle one HTTP request.
 */
static void
handle_request(int client_fd, media_ctx_t *media)
{
	char	 req[4096];
	ssize_t	 n;
	int	 head_only = 0;
	off_t	 range_start = -1;
	char	*line, *p;

	n = recv(client_fd, req, sizeof(req) - 1, 0);
	if (n <= 0)
		return;
	req[n] = '\0';

	DPRINTF("httpd: request %.*s\n",
	    (int)(strchr(req, '\r') ? strchr(req, '\r') - req : n), req);

	/* Parse method */
	if (strncmp(req, "HEAD ", 5) == 0)
		head_only = 1;
	else if (strncmp(req, "GET ", 4) != 0) {
		send_headers(client_fd, 405, "Method Not Allowed",
		    "text/plain", 0, -1, -1, -1, 0, NULL);
		return;
	}

	/* Check path is /media */
	p = strchr(req, ' ');
	if (p != NULL)
		p++;
	if (p == NULL || strncmp(p, "/media", 6) != 0) {
		send_headers(client_fd, 404, "Not Found",
		    "text/plain", 9, -1, -1, -1, 0, NULL);
		if (!head_only)
			send_all(client_fd, "Not Found", 9);
		return;
	}

	/* Parse Range header */
	line = strcasestr(req, "Range:");
	if (line != NULL) {
		p = strstr(line, "bytes=");
		if (p != NULL) {
			p += 6;
			range_start = strtoll(p, NULL, 10);
		}
	}

	if (media->needs_transcode || media->mode == MODE_SCREEN)
		serve_pipe(client_fd, media, head_only);
	else
		serve_file(client_fd, media, head_only, range_start);
}

/*
 * HTTP server thread.
 */
static void *
httpd_thread(void *arg)
{
	httpd_ctx_t		*ctx = arg;
	struct pollfd		 pfd;
	int			 client_fd;
	struct sockaddr_in	 client_addr;
	socklen_t		 client_len;

	pfd.fd = ctx->listen_fd;
	pfd.events = POLLIN;

	while (ctx->running) {
		if (poll(&pfd, 1, 1000) <= 0)
			continue;

		client_len = sizeof(client_addr);
		client_fd = accept(ctx->listen_fd,
		    (struct sockaddr *)&client_addr, &client_len);
		if (client_fd < 0)
			continue;

		handle_request(client_fd, ctx->media);
		close(client_fd);
	}

	return NULL;
}

/*
 * Start the HTTP server.
 */
int
httpd_start(httpd_ctx_t *ctx, media_ctx_t *media, int port)
{
	struct sockaddr_in	 addr;
	socklen_t		 addr_len;
	int			 opt = 1;

	ctx->media = media;
	ctx->running = 1;

	ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (ctx->listen_fd < 0) {
		perror("socket");
		return -1;
	}

	setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR,
	    &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	if (bind(ctx->listen_fd, (struct sockaddr *)&addr,
	    sizeof(addr)) < 0) {
		perror("bind");
		close(ctx->listen_fd);
		return -1;
	}

	if (listen(ctx->listen_fd, 5) < 0) {
		perror("listen");
		close(ctx->listen_fd);
		return -1;
	}

	/* Get actual port (in case ephemeral was used) */
	addr_len = sizeof(addr);
	getsockname(ctx->listen_fd, (struct sockaddr *)&addr, &addr_len);
	ctx->port = ntohs(addr.sin_port);

	DPRINTF("httpd: listening on port %d\n", ctx->port);

	if (pthread_create(&ctx->thread, NULL, httpd_thread, ctx) != 0) {
		perror("pthread_create");
		close(ctx->listen_fd);
		return -1;
	}

	return 0;
}

void
httpd_stop(httpd_ctx_t *ctx)
{
	ctx->running = 0;
	close(ctx->listen_fd);
	pthread_join(ctx->thread, NULL);
}
