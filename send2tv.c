#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "send2tv.h"

int verbose = 0;
static volatile int running = 1;

static void
usage(void)
{
	fprintf(stderr,
	    "usage: send2tv [-v] -h host -f file\n"
	    "       send2tv [-v] -h host -s\n"
	    "       send2tv [-v] -d\n"
	    "\n"
	    "  -h host   TV IP address or hostname\n"
	    "  -f file   media file to send\n"
	    "  -s        stream screen and system audio\n"
	    "  -d        discover TVs on the network\n"
	    "  -p port   HTTP server port (default: auto)\n"
	    "  -v        verbose/debug output\n");
	exit(1);
}

static void
sighandler(int sig)
{
	(void)sig;
	running = 0;
}

int
main(int argc, char *argv[])
{
	const char	*host = NULL;
	const char	*file = NULL;
	int		 screen = 0;
	int		 discover = 0;
	int		 port = 0;
	int		 ch;
	upnp_ctx_t	 upnp;
	httpd_ctx_t	 httpd;
	media_ctx_t	 media;
	char		 media_url[256];

	while ((ch = getopt(argc, argv, "h:f:sp:dv")) != -1) {
		switch (ch) {
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

	/* Initialize contexts */
	memset(&upnp, 0, sizeof(upnp));
	memset(&httpd, 0, sizeof(httpd));
	memset(&media, 0, sizeof(media));

	strlcpy(upnp.tv_ip, host, sizeof(upnp.tv_ip));
	media.running = 1;

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
		if (media_probe(&media, file) < 0) {
			fprintf(stderr, "Failed to probe %s\n", file);
			return 1;
		}
		if (media.needs_transcode) {
			printf("Transcoding required (format not natively "
			    "supported)\n");
			if (media_open_transcode(&media) < 0) {
				fprintf(stderr, "Failed to set up "
				    "transcoding\n");
				return 1;
			}
		} else {
			printf("Format supported, sending directly\n");
		}
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

	/* Find TV's AVTransport service */
	printf("Connecting to TV at %s...\n", upnp.tv_ip);
	if (upnp_find_transport(&upnp) < 0) {
		media.running = 0;
		httpd_stop(&httpd);
		media_close(&media);
		return 1;
	}
	printf("AVTransport: %s:%d%s\n", upnp.tv_ip,
	    upnp.tv_port, upnp.control_url);

	/* Build media URL */
	snprintf(media_url, sizeof(media_url),
	    "http://%s:%d/media", upnp.local_ip, httpd.port);

	/* Set URI and play */
	printf("Sending media URL to TV...\n");
	if (upnp_set_uri(&upnp, media_url, media.mime_type,
	    file ? file : "Screen") < 0) {
		fprintf(stderr, "SetAVTransportURI failed\n");
		media.running = 0;
		httpd_stop(&httpd);
		media_close(&media);
		return 1;
	}

	if (upnp_play(&upnp) < 0) {
		fprintf(stderr, "Play failed\n");
		media.running = 0;
		httpd_stop(&httpd);
		media_close(&media);
		return 1;
	}

	printf("Playing. Press Ctrl+C to stop.\n");

	/* Wait for signal */
	while (running && media.running)
		sleep(1);

	/* Stop playback */
	printf("\nStopping...\n");
	upnp_stop(&upnp);

	media.running = 0;
	if (media.needs_transcode || media.mode == MODE_SCREEN) {
		/* Close write end of pipe to unblock reader */
		if (media.pipe_wr >= 0)
			close(media.pipe_wr);
		pthread_join(media.thread, NULL);
	}

	httpd_stop(&httpd);
	media_close(&media);

	printf("Done.\n");
	return 0;
}
