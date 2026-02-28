#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>

#include "send2tv.h"

int verbose = 0;
static volatile int running = 1;
static struct termios orig_termios;
static int term_raw = 0;

static void
term_restore(void)
{
	if (term_raw) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
		term_raw = 0;
	}
}

static int
term_raw_mode(void)
{
	struct termios raw;

	if (!isatty(STDIN_FILENO))
		return -1;

	if (tcgetattr(STDIN_FILENO, &orig_termios) < 0)
		return -1;

	raw = orig_termios;
	raw.c_lflag &= ~(ECHO | ICANON | ISIG);
	raw.c_iflag &= ~(IXON | ICRNL);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
		return -1;

	term_raw = 1;
	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: send2tv [-tv] -h host -f file\n"
	    "       send2tv [-av] -h host -s\n"
	    "       send2tv [-v] -d\n"
	    "\n"
	    "  -h host   TV IP address or hostname\n"
	    "  -f file   media file to send\n"
	    "  -t        force transcoding\n"
	    "  -s        stream screen and system audio\n"
	    "  -a device sndio audio device (default: snd/default.mon)\n"
	    "  -d        discover TVs on the network\n"
	    "  -p port   HTTP server port (default: auto)\n"
	    "  -v        verbose/debug output\n"
	    "\n"
	    "During playback:\n"
	    "  arrows    seek (left/right: 10s, up/down: 30s)\n"
	    "  q         quit\n");
	exit(1);
}

static void
sighandler(int sig)
{
	(void)sig;
	term_restore();
	if (!running)
		_exit(1);
	running = 0;
}

int
main(int argc, char *argv[])
{
	const char	*host = NULL;
	const char	*file = NULL;
	const char	*audiodev = "snd/default.mon";
	int		 screen = 0;
	int		 transcode = 0;
	int		 discover = 0;
	int		 port = 0;
	int		 ch;
	upnp_ctx_t	 upnp;
	httpd_ctx_t	 httpd;
	media_ctx_t	 media;
	char		 media_url[256];

	while ((ch = getopt(argc, argv, "a:h:f:sp:dvt")) != -1) {
		switch (ch) {
		case 'a':
			audiodev = optarg;
			break;
		case 'h':
			host = optarg;
			break;
		case 'f':
			file = optarg;
			break;
		case 's':
			screen = 1;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'd':
			discover = 1;
			break;
		case 't':
			transcode = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
		}
	}

	/* Discovery mode */
	if (discover) {
		upnp_discover();
		return 0;
	}

	/* Validate arguments */
	if (host == NULL)
		usage();
	if (file == NULL && !screen)
		usage();
	if (file != NULL && screen)
		usage();

	DPRINTF("host=%s, file=%s, screen=%d, port=%d\n",
	    host ? host : "(null)", file ? file : "(null)", screen, port);

	/* Set up signal handlers */
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);
	atexit(term_restore);

	/* Initialize contexts */
	memset(&upnp, 0, sizeof(upnp));
	memset(&httpd, 0, sizeof(httpd));
	memset(&media, 0, sizeof(media));

	strlcpy(upnp.tv_ip, host, sizeof(upnp.tv_ip));
	media.running = 1;
	media.pipe_rd = -1;
	media.pipe_wr = -1;
	media.sndio_device = audiodev;

	if (screen) {
		media.mode = MODE_SCREEN;
		printf("Setting up screen capture...\n");
		if (media_open_screen(&media) < 0) {
			fprintf(stderr, "Failed to set up screen capture\n");
			return 1;
		}
	} else {
		media.mode = MODE_FILE;
		media.filepath = file;
		printf("Probing %s...\n", file);
		if (media_probe(&media, file, transcode) < 0) {
			fprintf(stderr, "Failed to probe %s\n", file);
			return 1;
		}
		if (media.needs_transcode) {
			printf("Transcoding %s\n", transcode ?
			    "forced by -t flag" :
			    "required (format not natively supported)");
			if (media_open_transcode(&media) < 0) {
				fprintf(stderr, "Failed to set up "
				    "transcoding\n");
				return 1;
			}
		} else {
			printf("Format supported, sending directly\n");
		}
	}

	if (!running) {
		media_close(&media);
		return 1;
	}

	/* Determine our local IP */
	if (upnp_get_local_ip(&upnp) < 0) {
		fprintf(stderr, "Cannot determine local IP\n");
		media_close(&media);
		return 1;
	}
	printf("Local IP: %s\n", upnp.local_ip);

	/* Start HTTP server */
	if (httpd_start(&httpd, &media, port) < 0) {
		fprintf(stderr, "Failed to start HTTP server\n");
		media_close(&media);
		return 1;
	}
	printf("HTTP server on port %d\n", httpd.port);

	if (!running) {
		httpd_stop(&httpd);
		media_close(&media);
		return 1;
	}

	/* Start transcoding/capture thread if needed */
	if (media.needs_transcode || media.mode == MODE_SCREEN) {
		if (media.mode == MODE_SCREEN) {
			if (pthread_create(&media.thread, NULL,
			    media_capture_thread, &media) != 0) {
				fprintf(stderr, "Failed to start capture\n");
				httpd_stop(&httpd);
				media_close(&media);
				return 1;
			}
		} else {
			if (pthread_create(&media.thread, NULL,
			    media_transcode_thread, &media) != 0) {
				fprintf(stderr, "Failed to start "
				    "transcoding\n");
				httpd_stop(&httpd);
				media_close(&media);
				return 1;
			}
		}
	}

	/*
	 * From here on, use goto shutdown for cleanup since
	 * threads may be running.
	 */
	if (!running)
		goto shutdown;

	/* Find TV's AVTransport service */
	printf("Connecting to TV at %s...\n", upnp.tv_ip);
	if (upnp_find_transport(&upnp) < 0)
		goto shutdown;
	printf("AVTransport: %s:%d%s\n", upnp.tv_ip,
	    upnp.tv_port, upnp.control_url);

	if (!running)
		goto shutdown;

	/* Build media URL */
	snprintf(media_url, sizeof(media_url),
	    "http://%s:%d/media", upnp.local_ip, httpd.port);

	/* Set URI and play */
	printf("Sending media URL to TV...\n");
	{
		const char *t;

		if (file != NULL) {
			t = strrchr(file, '/');
			t = (t != NULL) ? t + 1 : file;
		} else {
			t = "Screen";
		}

		if (upnp_set_uri(&upnp, media_url, media.mime_type,
		    t,
		    media.mode == MODE_SCREEN || media.needs_transcode,
		    media.dlna_profile) < 0)
			goto shutdown;
	}

	if (!running)
		goto shutdown;

	if (upnp_play(&upnp) < 0)
		goto shutdown;

	/* Enter raw terminal mode for key input */
	if (term_raw_mode() == 0)
		printf("Playing. Keys: arrows=seek, q=quit\n");
	else
		printf("Playing. Press Ctrl+C to stop.\n");

	/* Event loop: poll stdin for keypresses */
	{
		struct pollfd	 pfd;
		unsigned char	 buf[8];
		ssize_t		 n;
		int		 can_seek;
		int		 delta;
		const char	*title;

		can_seek = (media.mode == MODE_FILE);

		if (file != NULL) {
			title = strrchr(file, '/');
			title = (title != NULL) ? title + 1 : file;
		} else {
			title = "Screen";
		}

		pfd.fd = STDIN_FILENO;
		pfd.events = POLLIN;

		while (running && media.running) {
			if (poll(&pfd, 1, 500) <= 0)
				continue;

			n = read(STDIN_FILENO, buf, sizeof(buf));
			if (n <= 0)
				continue;

			if (buf[0] == 'q' || buf[0] == 'Q' ||
			    buf[0] == 0x03) {
				running = 0;
				break;
			}

			/* Arrow keys: ESC [ A/B/C/D */
			if (!can_seek || n < 3 ||
			    buf[0] != 0x1b || buf[1] != '[')
				continue;

			delta = 0;
			switch (buf[2]) {
			case 'C': delta =  10; break;
			case 'D': delta = -10; break;
			case 'A': delta =  30; break;
			case 'B': delta = -30; break;
			}
			if (delta == 0)
				continue;

			if (!media.needs_transcode) {
				/* Direct file: TV handles seek */
				upnp_seek_relative(&upnp, delta);
			} else {
				/* Transcoded: restart from new pos */
				int pos, target;

				if (upnp_get_position(&upnp, &pos) < 0)
					continue;

				target = media.start_sec + pos + delta;
				if (target < 0)
					target = 0;

				DPRINTF("seek: restart transcode "
				    "at %ds\n", target);

				upnp_stop(&upnp);
				media.running = 0;
				pthread_join(media.thread, NULL);

				if (media_restart_transcode(&media,
				    target) < 0) {
					fprintf(stderr,
					    "Seek failed\n");
					running = 0;
					break;
				}

				if (pthread_create(&media.thread,
				    NULL, media_transcode_thread,
				    &media) != 0) {
					fprintf(stderr,
					    "Failed to restart "
					    "transcoding\n");
					running = 0;
					break;
				}

				if (upnp_set_uri(&upnp, media_url,
				    media.mime_type, title, 1,
				    media.dlna_profile) < 0 ||
				    upnp_play(&upnp) < 0) {
					running = 0;
					break;
				}
			}
		}
	}

	term_restore();

shutdown:
	printf("\nStopping...\n");
	upnp_stop(&upnp);

	media.running = 0;
	if (media.needs_transcode || media.mode == MODE_SCREEN)
		pthread_join(media.thread, NULL);

	httpd_stop(&httpd);
	media_close(&media);

	printf("Done.\n");
	return 0;
}
