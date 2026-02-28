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
	    "usage: send2tv [-tv] [-b kbps] -h host file ...\n"
	    "       send2tv [-av] [-b kbps] -h host -s\n"
	    "       send2tv [-v] -d\n"
	    "\n"
	    "  -h host   TV IP address or hostname\n"
	    "  -t        force transcoding\n"
	    "  -s        stream screen and system audio\n"
	    "  -a device sndio audio device (default: snd/default.mon)\n"
	    "  -d        discover TVs on the network\n"
	    "  -p port   HTTP server port (default: auto)\n"
	    "  -b kbps   video bitrate in kbps (default: 2000)\n"
	    "  -v        verbose/debug output\n"
	    "\n"
	    "During playback:\n"
	    "  arrows    seek (left/right: 10s, up/down: 30s)\n"
	    "  q         next file\n"
	    "  Q         quit\n");
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
	const char	*audiodev = "snd/default.mon";
	int		 screen = 0;
	int		 transcode = 0;
	int		 discover = 0;
	int		 port = 0;
	int		 bitrate = 2000;
	int		 ch;
	int		 fileidx;
	upnp_ctx_t	 upnp;
	httpd_ctx_t	 httpd;
	media_ctx_t	 media;
	char		 media_url[256];

	while ((ch = getopt(argc, argv, "a:b:h:sp:dvt")) != -1) {
		switch (ch) {
		case 'a':
			audiodev = optarg;
			break;
		case 'b':
			bitrate = atoi(optarg);
			if (bitrate <= 0) {
				fprintf(stderr, "Invalid bitrate: %s\n",
				    optarg);
				usage();
			}
			break;
		case 'h':
			host = optarg;
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
	argc -= optind;
	argv += optind;

	/* Discovery mode */
	if (discover) {
		upnp_discover();
		return 0;
	}

	/* Validate arguments */
	if (host == NULL)
		usage();
	if (argc == 0 && !screen)
		usage();
	if (argc > 0 && screen)
		usage();

	DPRINTF("host=%s, files=%d, screen=%d, port=%d\n",
	    host ? host : "(null)", argc, screen, port);

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
	media.pipe_rd = -1;
	media.pipe_wr = -1;
	media.bitrate = bitrate;
	media.sndio_device = audiodev;

	/*
	 * Screen mode: single-pass, no file loop.
	 */
	if (screen) {
		media.mode = MODE_SCREEN;
		media.running = 1;
		printf("Setting up screen capture...\n");
		if (media_open_screen(&media) < 0) {
			fprintf(stderr, "Failed to set up screen capture\n");
			return 1;
		}

		if (!running) {
			media_close(&media);
			return 1;
		}

		if (upnp_get_local_ip(&upnp) < 0) {
			fprintf(stderr, "Cannot determine local IP\n");
			media_close(&media);
			return 1;
		}
		printf("Local IP: %s\n", upnp.local_ip);

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

		if (pthread_create(&media.thread, NULL,
		    media_capture_thread, &media) != 0) {
			fprintf(stderr, "Failed to start capture\n");
			httpd_stop(&httpd);
			media_close(&media);
			return 1;
		}

		if (!running)
			goto screen_shutdown;

		printf("Connecting to TV at %s...\n", upnp.tv_ip);
		if (upnp_find_transport(&upnp) < 0)
			goto screen_shutdown;
		printf("AVTransport: %s:%d%s\n", upnp.tv_ip,
		    upnp.tv_port, upnp.control_url);

		if (!running)
			goto screen_shutdown;

		snprintf(media_url, sizeof(media_url),
		    "http://%s:%d/media", upnp.local_ip, httpd.port);

		if (upnp_set_uri(&upnp, media_url, media.mime_type,
		    "Screen", 1, media.dlna_profile) < 0)
			goto screen_shutdown;

		if (!running)
			goto screen_shutdown;

		if (upnp_play(&upnp) < 0)
			goto screen_shutdown;

		if (term_raw_mode() == 0)
			printf("Playing. Keys: q=quit\n");
		else
			printf("Playing. Press Ctrl+C to stop.\n");

		{
			struct pollfd	 pfd;
			unsigned char	 buf[8];
			ssize_t		 n;

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
			}
		}

		term_restore();

	screen_shutdown:
		printf("\nStopping...\n");
		upnp_stop(&upnp);

		media.running = 0;
		pthread_join(media.thread, NULL);

		httpd_stop(&httpd);
		media_close(&media);

		printf("Done.\n");
		return 0;
	}

	/*
	 * File mode: one-time infrastructure setup, then per-file loop.
	 */

	/* Determine our local IP */
	if (upnp_get_local_ip(&upnp) < 0) {
		fprintf(stderr, "Cannot determine local IP\n");
		return 1;
	}
	printf("Local IP: %s\n", upnp.local_ip);

	/* Start HTTP server (persists across all files) */
	if (httpd_start(&httpd, &media, port) < 0) {
		fprintf(stderr, "Failed to start HTTP server\n");
		return 1;
	}
	printf("HTTP server on port %d\n", httpd.port);

	if (!running) {
		httpd_stop(&httpd);
		return 1;
	}

	/* Find TV's AVTransport service (once) */
	printf("Connecting to TV at %s...\n", upnp.tv_ip);
	if (upnp_find_transport(&upnp) < 0) {
		httpd_stop(&httpd);
		return 1;
	}
	printf("AVTransport: %s:%d%s\n", upnp.tv_ip,
	    upnp.tv_port, upnp.control_url);

	/* Per-file loop */
	for (fileidx = 0; fileidx < argc && running; fileidx++) {
		const char *file = argv[fileidx];
		const char *title;

		/* Re-initialize media context for this file */
		memset(&media, 0, sizeof(media));
		media.mode = MODE_FILE;
		media.filepath = file;
		media.running = 1;
		media.pipe_rd = -1;
		media.pipe_wr = -1;
		media.bitrate = bitrate;

		printf("\n[%d/%d] %s\n", fileidx + 1, argc, file);

		/* Probe */
		if (media_probe(&media, file, transcode) < 0) {
			fprintf(stderr, "Failed to probe %s, skipping\n",
			    file);
			continue;
		}

		if (media.needs_transcode) {
			printf("Transcoding %s\n", transcode ?
			    "forced by -t flag" :
			    "required (format not natively supported)");
			if (media_open_transcode(&media) < 0) {
				fprintf(stderr, "Failed to set up "
				    "transcoding, skipping\n");
				media_close(&media);
				continue;
			}
		} else {
			printf("Format supported, sending directly\n");
		}

		if (!running) {
			media_close(&media);
			break;
		}

		/* Start transcode thread if needed */
		if (media.needs_transcode) {
			if (pthread_create(&media.thread, NULL,
			    media_transcode_thread, &media) != 0) {
				fprintf(stderr, "Failed to start "
				    "transcoding, skipping\n");
				media_close(&media);
				continue;
			}
		}

		/* Build media URL */
		snprintf(media_url, sizeof(media_url),
		    "http://%s:%d/media", upnp.local_ip, httpd.port);

		/* Derive title from filename */
		title = strrchr(file, '/');
		title = (title != NULL) ? title + 1 : file;

		/* Set URI and play */
		printf("Sending media URL to TV...\n");
		if (upnp_set_uri(&upnp, media_url, media.mime_type,
		    title, media.needs_transcode,
		    media.dlna_profile) < 0)
			goto next_file;

		if (!running)
			goto next_file;

		if (upnp_play(&upnp) < 0)
			goto next_file;

		/* Enter raw terminal mode for key input */
		if (term_raw_mode() == 0)
			printf("Playing. Keys: arrows=seek, "
			    "q=next, Q=quit\n");
		else
			printf("Playing. Press Ctrl+C to stop.\n");

		/* Event loop: poll stdin for keypresses */
		{
			struct pollfd	 pfd;
			unsigned char	 buf[8];
			ssize_t		 n;
			int		 delta;

			pfd.fd = STDIN_FILENO;
			pfd.events = POLLIN;

			while (running && media.running) {
				if (poll(&pfd, 1, 500) <= 0)
					continue;

				n = read(STDIN_FILENO, buf,
				    sizeof(buf));
				if (n <= 0)
					continue;

				/* q: skip to next file */
				if (buf[0] == 'q') {
					break;
				}

				/* Q / Ctrl+C: quit everything */
				if (buf[0] == 'Q' ||
				    buf[0] == 0x03) {
					running = 0;
					break;
				}

				/* Arrow keys: ESC [ A/B/C/D */
				if (n < 3 || buf[0] != 0x1b ||
				    buf[1] != '[')
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
					upnp_seek_relative(&upnp,
					    delta);
				} else {
					/* Transcoded: restart from
					 * new position */
					int pos, target;

					if (upnp_get_position(&upnp,
					    &pos) < 0)
						continue;

					target = media.start_sec +
					    pos + delta;
					if (target < 0)
						target = 0;

					DPRINTF("seek: restart "
					    "transcode at %ds\n",
					    target);

					upnp_stop(&upnp);
					media.running = 0;
					pthread_join(media.thread,
					    NULL);

					if (media_restart_transcode(
					    &media, target) < 0) {
						fprintf(stderr,
						    "Seek failed\n");
						running = 0;
						break;
					}

					if (pthread_create(
					    &media.thread, NULL,
					    media_transcode_thread,
					    &media) != 0) {
						fprintf(stderr,
						    "Failed to "
						    "restart "
						    "transcoding\n");
						running = 0;
						break;
					}

					if (upnp_set_uri(&upnp,
					    media_url,
					    media.mime_type,
					    title, 1,
					    media.dlna_profile)
					    < 0 ||
					    upnp_play(&upnp)
					    < 0) {
						running = 0;
						break;
					}
				}
			}
		}

		term_restore();

	next_file:
		printf("\nStopping...\n");
		upnp_stop(&upnp);

		media.running = 0;
		if (media.needs_transcode)
			pthread_join(media.thread, NULL);

		media_close(&media);
	}

	httpd_stop(&httpd);

	printf("Done.\n");
	return 0;
}
