#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "send2tv.h"

/*
 * Read a newline-terminated line from fd into buf (NUL-terminated, newline
 * stripped).  Returns the line length on success, 0 on EOF, -1 on error.
 * Does not handle partial lines longer than bufsz-1.
 */
static int
read_line(int fd, char *buf, size_t bufsz)
{
	size_t	 n = 0;
	char	 c;
	ssize_t	 r;

	while (n < bufsz - 1) {
		r = read(fd, &c, 1);
		if (r == 0)
			return 0;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (c == '\n')
			break;
		buf[n++] = c;
	}
	buf[n] = '\0';
	return (int)n;
}

/*
 * Create and bind a Unix domain socket at path, ready to accept.
 * Returns the listening fd on success, -1 on failure.
 */
static int
unix_listen(const char *path)
{
	struct sockaddr_un	 addr;
	int			 fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	unlink(path);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(fd);
		return -1;
	}
	if (listen(fd, 1) < 0) {
		perror("listen");
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * Server main loop.
 * upnp must already be connected (upnp_find_transport done by caller).
 * httpd must already be started (httpd_start done by caller).
 */
int
server_run(upnp_ctx_t *upnp, httpd_ctx_t *httpd,
    const char *ctrl_path, const char *data_path)
{
	media_ctx_t	 media;
	int		 ctrl_listen = -1, data_listen = -1;
	int		 ctrl_fd = -1, data_fd = -1;
	int		 seg_id = 0;
	int		 ret = -1;
	char		 url[256];

	memset(&media, 0, sizeof(media));
	media.pipe_rd = -1;
	media.pipe_wr = -1;
	media.ctrl_fd = -1;
	media.mode    = MODE_SINK;

	ctrl_listen = unix_listen(ctrl_path);
	if (ctrl_listen < 0) {
		fprintf(stderr, "server: cannot create control socket %s\n",
		    ctrl_path);
		goto done;
	}
	data_listen = unix_listen(data_path);
	if (data_listen < 0) {
		fprintf(stderr, "server: cannot create data socket %s\n",
		    data_path);
		goto done;
	}

	/* Start HTTP server, pointing it at our local media context */
	if (httpd_start(httpd, &media, 0) < 0) {
		fprintf(stderr, "server: cannot start HTTP server\n");
		goto done;
	}
	printf("Server: HTTP server on port %d\n", httpd->port);

	printf("Server: control socket %s\n", ctrl_path);
	printf("Server: data socket    %s\n", data_path);

	upnp_stop(upnp);
	ret = 0;

	while (running) {
		struct pollfd	 pfds[3];
		int		 nfds = 2;

		pfds[0].fd     = ctrl_listen;
		pfds[0].events = POLLIN;
		pfds[1].fd     = data_listen;
		pfds[1].events = POLLIN;
		if (ctrl_fd >= 0) {
			pfds[2].fd     = ctrl_fd;
			pfds[2].events = POLLIN;
			nfds = 3;
		}

		if (poll(pfds, nfds, 500) <= 0)
			continue;

		/* New control connection */
		if (pfds[0].revents & POLLIN) {
			int newfd = accept(ctrl_listen, NULL, NULL);
			if (newfd >= 0) {
				if (ctrl_fd >= 0) {
					upnp_stop(upnp);
					close(ctrl_fd);
				}
				ctrl_fd = newfd;
				printf("Server: client connected\n");
			}
		}

		/* New data connection */
		if (pfds[1].revents & POLLIN) {
			int newfd = accept(data_listen, NULL, NULL);
			if (newfd >= 0) {
				if (data_fd >= 0)
					close(data_fd);
				data_fd = newfd;
				DPRINTF("server: data connection accepted\n");
			}
		}

		/* Control command from connected client */
		if (ctrl_fd >= 0 && nfds == 3 &&
		    (pfds[2].revents & (POLLIN | POLLHUP))) {
			char	 line[256];
			char	 mime[64], dlna[64];
			int	 n;

			n = read_line(ctrl_fd, line, sizeof(line));

			if (n == 0 || n < 0) {
				/* Client disconnected */
				printf("Server: client disconnected, "
				    "going idle\n");
				upnp_stop(upnp);
				media.running = 0;
				if (media.pipe_rd >= 0) {
					close(media.pipe_rd);
					media.pipe_rd = -1;
				}
				if (data_fd >= 0) {
					close(data_fd);
					data_fd = -1;
				}
				close(ctrl_fd);
				ctrl_fd = -1;

			} else if (strncmp(line, "PLAY ", 5) == 0 &&
			    sscanf(line + 5, "%63s %63s", mime, dlna) == 2) {

				/* Switch to new segment */
				media.running = 0;
				if (media.pipe_rd >= 0 &&
				    media.pipe_rd != data_fd) {
					close(media.pipe_rd);
					media.pipe_rd = -1;
				}

				if (data_fd < 0) {
					fprintf(stderr,
					    "server: PLAY with no data "
					    "connection\n");
				} else {
					media.pipe_rd = data_fd;
					data_fd = -1;
					media.running = 1;
					media.mode = MODE_SINK;
					strlcpy(media.mime_type, mime,
					    sizeof(media.mime_type));
					strlcpy(media.dlna_profile, dlna,
					    sizeof(media.dlna_profile));

					seg_id++;
					snprintf(url, sizeof(url),
					    "http://%s:%d/media?id=%d",
					    upnp->local_ip, httpd->port,
					    seg_id);
					printf("Server: segment %d — %s\n",
					    seg_id, url);

					if (upnp_set_uri(upnp, url, mime,
					    "Client", 1, dlna) < 0 ||
					    upnp_play(upnp) < 0)
						fprintf(stderr,
						    "server: TV playback "
						    "failed\n");
				}

			} else if (strcmp(line, "STOP") == 0) {
				printf("Server: stop\n");
				upnp_stop(upnp);
			}
		}
	}

done:
	if (ctrl_listen >= 0) {
		close(ctrl_listen);
		unlink(ctrl_path);
	}
	if (data_listen >= 0) {
		close(data_listen);
		unlink(data_path);
	}
	if (ctrl_fd >= 0)
		close(ctrl_fd);
	if (data_fd >= 0)
		close(data_fd);
	if (media.pipe_rd >= 0)
		close(media.pipe_rd);
	httpd_stop(httpd);
	return ret;
}
