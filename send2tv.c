#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <getopt.h>

#include "send2tv.h"

int verbose = 0;
volatile int running = 1;
static volatile pid_t ytdlp_pid = 0;
static struct termios orig_termios;
static int term_raw = 0;
static char conf_host[256];
static char conf_audiodev[64];
static char conf_codec[32];
static char conf_mac[18];

/* Audio channel remapping presets.
 * map[output_slot] = input_channel_index, matching the ffmpeg channelmap
 * filter convention.  Output slot order: FL FR FC LFE BL BR. */
typedef struct {
	const char	*name;
	const char	*desc;
	int		 map[6];
} channelmap_preset_t;

static const channelmap_preset_t channelmap_presets[] = {
	{ "mpeg",  "MPEG default    — FL FR FC LFE BL BR  (no change)", {0,1,2,3,4,5} },
	{ "ac3",   "AC3/Dolby       — FL FC FR BL BR LFE",              {0,2,1,5,3,4} },
	{ "ac3r",  "AC3 rear-swapped— FL FC FR BR BL LFE",              {0,2,1,5,4,3} },
	{ "dts",   "DTS/some AAC    — FL FR BL BR FC LFE",              {0,1,4,5,2,3} },
	{ "dtsr",  "Some MKV        — FL FR BL BR LFE FC",              {0,1,5,4,2,3} },
	{ "aac6",  "MPEG-4 AAC ch6  — C  FL FR SL SR LFE",             {1,2,0,5,3,4} },
	{ "aac6b", "Some AAC        — C  FL FR LFE BL BR",              {1,2,0,3,4,5} },
	{ NULL, NULL, {0} }
};

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
	    "usage: send2tv --server [-h host] [-v] [--ctrl path] [--data path]\n"
	    "       send2tv [-tv] [-b kbps] [-c codec] [-h host] [--ctrl path] [--data path] file ...\n"
	    "       send2tv [-av] [-b kbps] [-c codec] [-h host] [--ctrl path] [--data path] -s\n"
	    "       send2tv [-v] -d\n"
	    "       send2tv [-v] -q -h host\n"
	    "       send2tv [-v] -w\n"
	    "       send2tv [-v] -h host --app\n"
	    "       send2tv [-v] -h host --app <name>\n"
	    "\n"
	    "  -h host      TV IP address or hostname\n"
	    "  -t           force transcoding\n"
	    "  -s           stream screen and system audio\n"
	    "  -a device    sndio audio device (default: snd/mon)\n"
	    "  -d           discover TVs on the network\n"
	    "  -q           query TV capabilities\n"
	    "  -c codec     transcode video codec: h264, hevc (default: auto)\n"
	    "  -p port      HTTP server port (default: auto)\n"
	    "  -b kbps      video bitrate in kbps (default: 2000)\n"
	    "  -w           send Wake-on-LAN packet to configured MAC\n"
	    "  --server     run as server (manages TV connection and HTTP server)\n"
	    "  --ctrl path  control socket path (default: /tmp/send2tv.ctrl)\n"
	    "  --data path  data socket path (default: /tmp/send2tv.data)\n"
	    "  --app        list installed apps on the TV\n"
	    "  --app <n>    launch app whose name contains <n> (case-insensitive)\n"
	    "  --channelmap list 5.1 channel remapping presets\n"
	    "  --channelmap <preset>  remap audio channels (forces transcode)\n"
	    "  --lang       list audio streams in file\n"
	    "  --lang <id>  select audio stream by index or language tag\n"
	    "  -v           verbose/debug output\n"
	    "\n"
	    "During playback:\n"
	    "  arrows    seek (left/right: 10s, up/down: 30s)\n"
	    "  e         jump to last minute / jump back\n"
	    "  q         next file\n"
	    "  Q         quit\n");
	exit(1);
}

static void
sighandler(int sig)
{
	(void)sig;
	term_restore();
	if (!running) {
		if (ytdlp_pid > 0)
			kill(ytdlp_pid, SIGKILL);
		_exit(1);
	}
	running = 0;
	if (ytdlp_pid > 0)
		kill(ytdlp_pid, SIGTERM);
}

/*
 * Connect to a Unix domain socket at path.
 * Returns the connected fd on success, -1 on failure.
 */
static int
unix_connect(const char *path)
{
	struct sockaddr_un	 addr;
	int			 fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * Send a command to the server's control socket.
 */
static void
ctrl_send(int ctrl_fd, const char *cmd)
{
	size_t len = strlen(cmd);
	write(ctrl_fd, cmd, len);
}

static void
load_config(const char **host, const char **audiodev, int *port,
    int *bitrate, int *transcode, const char **codec, const char **mac)
{
	FILE		*fp;
	const char	*home;
	char		 path[1024];
	char		 line[512];
	int		 lineno;

	home = getenv("HOME");
	if (home == NULL)
		return;

	snprintf(path, sizeof(path), "%s/.send2tv.conf", home);

	fp = fopen(path, "r");
	if (fp == NULL)
		return;

	lineno = 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		char	*key, *val, *p;
		size_t	 len;

		lineno++;

		/* strip trailing whitespace */
		len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' ||
		    line[len - 1] == '\r' ||
		    line[len - 1] == ' ' ||
		    line[len - 1] == '\t'))
			line[--len] = '\0';

		/* skip leading whitespace */
		key = line;
		while (*key == ' ' || *key == '\t')
			key++;

		/* skip empty lines and comments */
		if (*key == '\0' || *key == '#')
			continue;

		/* find '=' separator */
		p = strchr(key, '=');
		if (p == NULL) {
			fprintf(stderr, "%s:%d: missing '='\n",
			    path, lineno);
			continue;
		}

		*p = '\0';
		val = p + 1;

		/* strip trailing whitespace from key */
		p = key + strlen(key) - 1;
		while (p > key && (*p == ' ' || *p == '\t'))
			*p-- = '\0';

		/* strip leading whitespace from value */
		while (*val == ' ' || *val == '\t')
			val++;

		if (strcmp(key, "host") == 0) {
			strlcpy(conf_host, val, sizeof(conf_host));
			*host = conf_host;
		} else if (strcmp(key, "audiodev") == 0) {
			strlcpy(conf_audiodev, val,
			    sizeof(conf_audiodev));
			*audiodev = conf_audiodev;
		} else if (strcmp(key, "bitrate") == 0) {
			*bitrate = atoi(val);
			if (*bitrate <= 0) {
				fprintf(stderr,
				    "%s:%d: invalid bitrate\n",
				    path, lineno);
				*bitrate = 2000;
			}
		} else if (strcmp(key, "port") == 0) {
			*port = atoi(val);
		} else if (strcmp(key, "transcode") == 0) {
			if (strcmp(val, "yes") == 0)
				*transcode = 1;
			else if (strcmp(val, "no") == 0)
				*transcode = 0;
			else
				fprintf(stderr,
				    "%s:%d: transcode: "
				    "expected yes or no\n",
				    path, lineno);
		} else if (strcmp(key, "verbose") == 0) {
			if (strcmp(val, "yes") == 0)
				verbose = 1;
			else if (strcmp(val, "no") == 0)
				verbose = 0;
			else
				fprintf(stderr,
				    "%s:%d: verbose: "
				    "expected yes or no\n",
				    path, lineno);
		} else if (strcmp(key, "codec") == 0) {
			if (strcmp(val, "h264") == 0 ||
			    strcmp(val, "hevc") == 0 ||
			    strcmp(val, "auto") == 0) {
				strlcpy(conf_codec, val,
				    sizeof(conf_codec));
				*codec = conf_codec;
			} else
				fprintf(stderr,
				    "%s:%d: codec: "
				    "expected h264, hevc, or auto\n",
				    path, lineno);
		} else if (strcmp(key, "mac") == 0) {
			strlcpy(conf_mac, val, sizeof(conf_mac));
			*mac = conf_mac;
		} else {
			fprintf(stderr, "%s:%d: unknown key '%s'\n",
			    path, lineno, key);
		}
	}

	fclose(fp);
}

/*
 * Append mac=<addr> to ~/.send2tv.conf so future cold-start invocations
 * can send a Wake-on-LAN packet without manual configuration.
 */
static void
save_mac_to_config(const char *mac)
{
	const char	*home;
	char		 path[1024];
	FILE		*fp;

	home = getenv("HOME");
	if (home == NULL)
		return;

	snprintf(path, sizeof(path), "%s/.send2tv.conf", home);
	fp = fopen(path, "a");
	if (fp == NULL) {
		fprintf(stderr, "Cannot save MAC to %s: %s\n",
		    path, strerror(errno));
		return;
	}
	fprintf(fp, "mac=%s\n", mac);
	fclose(fp);
	printf("Saved MAC address %s to %s\n", mac, path);
}

/*
 * Write or replace the host= entry in ~/.send2tv.conf.
 * If the file doesn't exist it is created with just host=<new_host>.
 */
static void
update_config_host(const char *new_host)
{
	const char	*home;
	char		 path[1024], tmp[1040];
	FILE		*in, *out;
	char		 line[512];
	int		 found = 0;

	home = getenv("HOME");
	if (home == NULL)
		return;

	snprintf(path, sizeof(path), "%s/.send2tv.conf", home);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	in = fopen(path, "r");
	out = fopen(tmp, "w");
	if (out == NULL) {
		fprintf(stderr, "Cannot write config %s: %s\n",
		    tmp, strerror(errno));
		if (in != NULL)
			fclose(in);
		return;
	}

	if (in != NULL) {
		while (fgets(line, sizeof(line), in) != NULL) {
			if (strncmp(line, "host=", 5) == 0) {
				if (!found) {
					fprintf(out, "host=%s\n", new_host);
					found = 1;
				}
				/* drop extra host= lines */
			} else {
				fputs(line, out);
			}
		}
		fclose(in);
	}

	if (!found)
		fprintf(out, "host=%s\n", new_host);

	fclose(out);

	if (rename(tmp, path) < 0) {
		fprintf(stderr, "Cannot save config: %s\n", strerror(errno));
		return;
	}
	printf("Saved %s as default host in %s\n", new_host, path);
}

/*
 * Run SSDP discovery, present a numbered list, prompt the user to
 * pick a device, and optionally save it as the default in the config.
 * Copies the chosen IP into out_host (size out_sz).
 * Returns 0 on success, -1 if nothing was chosen.
 */
static int
discover_and_select(char *out_host, size_t out_sz)
{
	upnp_device_t	 devices[UPNP_MAX_DEVICES];
	char		 line[64];
	int		 count, sel, i;

	count = upnp_discover(devices, UPNP_MAX_DEVICES);
	if (count <= 0) {
		fprintf(stderr, "No devices found.\n");
		return -1;
	}

	printf("\nFound %d device(s):\n", count);
	for (i = 0; i < count; i++)
		printf("  %d) %-16s %s\n", i + 1,
		    devices[i].ip, devices[i].name);

	printf("\nSelect device (1-%d): ", count);
	fflush(stdout);
	if (fgets(line, sizeof(line), stdin) == NULL)
		return -1;

	sel = atoi(line);
	if (sel < 1 || sel > count) {
		fprintf(stderr, "Invalid selection.\n");
		return -1;
	}

	strlcpy(out_host, devices[sel - 1].ip, out_sz);

	printf("Save %s as default? [y/N] ", out_host);
	fflush(stdout);
	if (fgets(line, sizeof(line), stdin) != NULL &&
	    (line[0] == 'y' || line[0] == 'Y'))
		update_config_host(out_host);

	return 0;
}

/*
 * Connect to the TV's AVTransport service.
 * If the initial attempt fails and a MAC address is configured,
 * send a Wake-on-LAN packet and retry for up to 60 seconds.
 * Returns 0 on success, -1 on failure.
 */
static int
connect_to_tv(upnp_ctx_t *upnp)
{
	int	 t;

	if (upnp_find_transport(upnp) == 0) {
		if (upnp->tv_mac[0] == '\0' &&
		    upnp_get_mac(upnp) == 0)
			save_mac_to_config(upnp->tv_mac);
		return 0;
	}

	if (upnp->tv_mac[0] == '\0')
		return -1;

	printf("TV not reachable, sending Wake-on-LAN to %s...\n",
	    upnp->tv_mac);
	if (upnp_wake(upnp) < 0)
		return -1;

	for (t = 5; t <= 60 && running; t += 5) {
		printf("Waiting for TV... (%d/60s)\n", t);
		sleep(5);
		if (upnp_find_transport(upnp) == 0)
			return 0;
	}

	fprintf(stderr, "TV did not respond after Wake-on-LAN\n");
	return -1;
}

/*
 * Resolve a web URL to a direct stream URL using yt-dlp.
 * Writes the direct URL into out_url and the video title into out_title.
 * Returns 0 on success, -1 on failure.
 */
static int
ytdlp_resolve(const char *url, char *out_url, size_t url_sz,
    char *out_title, size_t title_sz)
{
	const char *args[] = {
		"yt-dlp", "--no-playlist",
		"-S", "vcodec:h264,proto,ext:mp4,vcodec:hevc",
		"-f", "best[height<=1080]/best",
		"--print", "%(title)s\n%(url)s",
		"--", url, NULL
	};
	int	 pipefd[2];
	pid_t	 pid;
	char	 buf[4096];
	ssize_t	 n;
	size_t	 total = 0;
	int	 status;
	char	*nl, *url_start;
	size_t	 len;

	if (pipe(pipefd) < 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		int null;
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);
		null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, STDERR_FILENO);
		execvp("yt-dlp", (char *const *)args);
		_exit(1);
	}
	close(pipefd[1]);
	ytdlp_pid = pid;

	while (total < sizeof(buf) - 1) {
		n = read(pipefd[0], buf + total, sizeof(buf) - 1 - total);
		if (n < 0) {
			if (errno == EINTR) {
				if (!running)
					break;
				continue;
			}
			break;
		}
		if (n == 0)
			break;
		total += n;
	}
	buf[total] = '\0';
	close(pipefd[0]);

	if (!running) {
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
		ytdlp_pid = 0;
		return -1;
	}

	waitpid(pid, &status, 0);
	ytdlp_pid = 0;

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;

	/* Output is: title\nurl\n */
	nl = strchr(buf, '\n');
	if (nl == NULL)
		return -1;
	*nl = '\0';

	/* Strip trailing \r from title */
	len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\r')
		buf[len - 1] = '\0';
	strlcpy(out_title, buf, title_sz);

	/* Strip trailing whitespace from URL */
	url_start = nl + 1;
	len = strlen(url_start);
	while (len > 0 && (url_start[len - 1] == '\n' ||
	    url_start[len - 1] == '\r' || url_start[len - 1] == ' '))
		url_start[--len] = '\0';

	if (url_start[0] == '\0')
		return -1;

	strlcpy(out_url, url_start, url_sz);
	return 0;
}

int
main(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "app",        optional_argument, NULL, 'A' },
		{ "channelmap", optional_argument, NULL, 'M' },
		{ "lang",       optional_argument, NULL, 'L' },
		{ "server",     no_argument,       NULL, 'S' },
		{ "ctrl",       required_argument, NULL, 'C' },
		{ "data",       required_argument, NULL, 'D' },
		{ NULL,         0,                 NULL,  0  }
	};
	const char	*host = NULL;
	const char	*audiodev = "snd/mon";
	const char	*codec = "auto";
	const char	*mac = NULL;
	const char	*app_name = NULL;
	const char	*channelmap_arg = NULL;
	const char	*lang_arg = NULL;
	int		 channelmap_mode = 0;
	int		 lang_mode = 0;
	int		 screen = 0;
	int		 transcode = 0;
	int		 discover = 0;
	int		 query = 0;
	int		 wol_only = 0;
	int		 app_mode = 0;
	int		 server_mode = 0;
	const char	*ctrl_path = "/tmp/send2tv.ctrl";
	const char	*data_path = "/tmp/send2tv.data";
	int		 port = 0;
	int		 bitrate = 2000;
	int		 vcodec = VCODEC_H264;
	int		 ch;
	int		 fileidx;
	upnp_ctx_t	 upnp;
	httpd_ctx_t	 httpd;
	media_ctx_t	 media;
	int		 ctrl_fd = -1;
	int		 data_fd = -1;

	load_config(&host, &audiodev, &port, &bitrate, &transcode, &codec, &mac);

	while ((ch = getopt_long(argc, argv, "a:b:c:h:sp:dqvtw",
	    longopts, NULL)) != -1) {
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
		case 'c':
			codec = optarg;
			if (strcmp(codec, "h264") != 0 &&
			    strcmp(codec, "hevc") != 0 &&
			    strcmp(codec, "auto") != 0) {
				fprintf(stderr, "Invalid codec: %s "
				    "(use h264, hevc, or auto)\n", codec);
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
		case 'q':
			query = 1;
			break;
		case 't':
			transcode = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'w':
			wol_only = 1;
			break;
		case 'A':
			app_mode = 1;
			if (optarg != NULL)
				app_name = optarg;
			else if (optind < argc && argv[optind][0] != '-')
				app_name = argv[optind++];
			break;
		case 'M':
			channelmap_mode = 1;
			if (optarg != NULL)
				channelmap_arg = optarg;
			else if (optind < argc && argv[optind][0] != '-')
				channelmap_arg = argv[optind++];
			/* else NULL → list mode */
			break;
		case 'L':
			lang_mode = 1;
			if (optarg != NULL)
				lang_arg = optarg;
			else if (optind < argc && argv[optind][0] != '-')
				lang_arg = argv[optind++];
			/* else NULL → list mode */
			break;
		case 'S':
			server_mode = 1;
			break;
		case 'C':
			ctrl_path = optarg;
			break;
		case 'D':
			data_path = optarg;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	/* Wake-on-LAN only */
	if (wol_only) {
		if (mac == NULL) {
			fprintf(stderr, "No MAC address configured "
			    "(add mac= to ~/.send2tv.conf)\n");
			return 1;
		}
		memset(&upnp, 0, sizeof(upnp));
		strlcpy(upnp.tv_mac, mac, sizeof(upnp.tv_mac));
		return upnp_wake(&upnp) < 0 ? 1 : 0;
	}

	/* App list / launch mode */
	if (app_mode) {
		app_entry_t	 apps[SAMSUNG_MAX_APPS];
		int		 i, count;

		if (host == NULL) {
			fprintf(stderr,
			    "--app requires a host (-h or config)\n");
			return 1;
		}
		memset(&upnp, 0, sizeof(upnp));
		strlcpy(upnp.tv_ip, host, sizeof(upnp.tv_ip));

		count = upnp_list_apps(&upnp, apps, SAMSUNG_MAX_APPS);
		if (count < 0) {
			fprintf(stderr,
			    "Failed to fetch app list from TV\n");
			return 1;
		}
		if (count == 0) {
			fprintf(stderr, "No apps returned by TV\n");
			return 1;
		}

		if (app_name == NULL) {
			/* List mode */
			printf("Installed apps (%d):\n", count);
			for (i = 0; i < count; i++)
				printf("  %-32s %s\n",
				    apps[i].name, apps[i].app_id);
			return 0;
		}

		/* Launch mode: case-insensitive substring match */
		for (i = 0; i < count; i++) {
			char	 hay[128], needle[128];
			char	*h, *n;

			strlcpy(hay, apps[i].name, sizeof(hay));
			strlcpy(needle, app_name, sizeof(needle));
			for (h = hay; *h; h++)
				*h = tolower((unsigned char)*h);
			for (n = needle; *n; n++)
				*n = tolower((unsigned char)*n);

			if (strstr(hay, needle) != NULL) {
				printf("Launching %s...\n", apps[i].name);
				return upnp_launch_app(&upnp,
				    apps[i].app_id) < 0 ? 1 : 0;
			}
		}

		fprintf(stderr, "No app matching '%s' found. "
		    "Run --app without a name to list all apps.\n",
		    app_name);
		return 1;
	}

	/* Channel map: list presets or validate the chosen one */
	if (channelmap_mode) {
		const channelmap_preset_t	*p;

		if (channelmap_arg == NULL) {
			/* List mode */
			printf("Available channelmap presets:\n\n");
			printf("  %-8s  %s\n", "PRESET", "SOURCE CHANNEL ORDER");
			printf("  %-8s  %s\n", "------",
			    "--------------------");
			for (p = channelmap_presets; p->name != NULL; p++)
				printf("  %-8s  %s\n", p->name, p->desc);
			printf("\nUsage: send2tv --channelmap <preset> file\n");
			return 0;
		}

		for (p = channelmap_presets; p->name != NULL; p++) {
			if (strcmp(p->name, channelmap_arg) == 0)
				break;
		}
		if (p->name == NULL) {
			fprintf(stderr,
			    "Unknown channelmap preset '%s'. "
			    "Run --channelmap without a name to list.\n",
			    channelmap_arg);
			return 1;
		}
		memcpy(media.channelmap, p->map, sizeof(media.channelmap));
		media.has_channelmap = 1;
		transcode = 1; /* channelmap requires transcoding */
		printf("Channel map: %s\n", p->desc);
	}

	/* Lang list mode: show audio streams and exit */
	if (lang_mode && lang_arg == NULL) {
		int i;

		if (argc == 0) {
			fprintf(stderr,
			    "--lang (list mode) requires a file argument\n");
			return 1;
		}
		for (i = 0; i < argc; i++)
			media_list_audio_streams(argv[i]);
		return 0;
	}

	/* Discovery mode: interactive select, optionally overwrite config */
	if (discover) {
		if (discover_and_select(conf_host, sizeof(conf_host)) == 0)
			host = conf_host;
		return (host != NULL) ? 0 : 1;
	}

	/* Query mode: show TV capabilities and exit */
	if (query) {
		if (host == NULL) {
			fprintf(stderr, "-q requires -h host\n");
			usage();
		}
		memset(&upnp, 0, sizeof(upnp));
		strlcpy(upnp.tv_ip, host, sizeof(upnp.tv_ip));
		if (upnp_find_transport(&upnp) < 0)
			return 1;
		return upnp_query_capabilities(&upnp, 1, NULL, 0) < 0;
	}

	/* Server mode: persistent daemon managing TV connection and HTTP server */
	if (server_mode) {
		if (host == NULL) {
			if (discover_and_select(conf_host,
			    sizeof(conf_host)) < 0)
				return 1;
			host = conf_host;
		}

		/* Set up signal handlers */
		signal(SIGINT, sighandler);
		signal(SIGTERM, sighandler);
		signal(SIGPIPE, SIG_IGN);

		memset(&upnp, 0, sizeof(upnp));
		memset(&httpd, 0, sizeof(httpd));
		strlcpy(upnp.tv_ip, host, sizeof(upnp.tv_ip));
		if (mac != NULL)
			strlcpy(upnp.tv_mac, mac, sizeof(upnp.tv_mac));

		if (upnp_get_local_ip(&upnp) < 0) {
			fprintf(stderr, "Cannot determine local IP\n");
			return 1;
		}
		printf("Local IP: %s\n", upnp.local_ip);

		printf("Connecting to TV at %s...\n", upnp.tv_ip);
		if (connect_to_tv(&upnp) < 0)
			return 1;
		printf("AVTransport: %s:%d%s\n", upnp.tv_ip,
		    upnp.tv_port, upnp.control_url);

		server_run(&upnp, &httpd, ctrl_path, data_path);
		return 0;
	}

	/* Validate arguments */
	if (argc == 0 && !screen)
		usage();
	if (argc > 0 && screen)
		usage();

	/* Resolve transcode video codec */
	if (strcmp(codec, "hevc") == 0)
		vcodec = VCODEC_HEVC;
	else if (strcmp(codec, "h264") == 0)
		vcodec = VCODEC_H264;
	/* else "auto": try to query TV, default to h264 */

	DPRINTF("host=%s, files=%d, screen=%d, codec=%s\n",
	    host ? host : "(null)", argc, screen, codec);

	/* Set up signal handlers */
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);
	atexit(term_restore);

	/* Connect to server */
	ctrl_fd = unix_connect(ctrl_path);
	if (ctrl_fd < 0) {
		fprintf(stderr,
		    "Cannot connect to server ctrl socket %s\n"
		    "(is 'send2tv --server' running?)\n", ctrl_path);
		return 1;
	}

	/* Initialize upnp for position queries (host optional) */
	memset(&upnp, 0, sizeof(upnp));
	if (host != NULL) {
		strlcpy(upnp.tv_ip, host, sizeof(upnp.tv_ip));
		if (mac != NULL)
			strlcpy(upnp.tv_mac, mac, sizeof(upnp.tv_mac));
	}

	memset(&media, 0, sizeof(media));
	media.pipe_rd = -1;
	media.pipe_wr = -1;
	media.ctrl_fd = -1;
	media.bitrate = bitrate;
	media.vcodec = vcodec;
	media.sndio_device = audiodev;

	/*
	 * Screen mode: single-pass, no file loop.
	 */
	if (screen) {
		media.mode = MODE_SCREEN;
		media.running = 1;

		if (!running) {
			close(ctrl_fd);
			return 1;
		}

		/* Connect data socket and set as pipe_wr before open */
		data_fd = unix_connect(data_path);
		if (data_fd < 0) {
			fprintf(stderr, "Cannot connect to server data "
			    "socket %s\n", data_path);
			close(ctrl_fd);
			return 1;
		}
		media.pipe_wr = data_fd;
		media.ctrl_fd = ctrl_fd;

		printf("Setting up screen capture...\n");
		if (media_open_screen(&media) < 0) {
			fprintf(stderr, "Failed to set up screen capture\n");
			close(ctrl_fd);
			media.pipe_wr = -1;
			close(data_fd);
			return 1;
		}

		if (!running) {
			media_close(&media);
			close(ctrl_fd);
			return 1;
		}

		if (pthread_create(&media.thread, NULL,
		    media_capture_thread, &media) != 0) {
			fprintf(stderr, "Failed to start capture\n");
			media_close(&media);
			close(ctrl_fd);
			return 1;
		}

		if (!running)
			goto screen_shutdown;

		{
			char play_cmd[256];
			snprintf(play_cmd, sizeof(play_cmd),
			    "PLAY %s %s\n",
			    media.mime_type, media.dlna_profile);
			ctrl_send(ctrl_fd, play_cmd);
		}

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
		ctrl_send(ctrl_fd, "STOP\n");

		media.running = 0;
		pthread_join(media.thread, NULL);

		media_close(&media);
		close(ctrl_fd);

		printf("Done.\n");
		return 0;
	}

	/*
	 * File mode: per-file loop, sending data to server.
	 * Optionally connect to TV directly for position queries.
	 */

	/* If host is configured, connect for position queries */
	if (host != NULL && upnp.tv_ip[0] != '\0') {
		if (upnp_find_transport(&upnp) < 0) {
			fprintf(stderr, "Warning: cannot connect to TV "
			    "for position queries (seek may be imprecise)\n");
			/* Non-fatal: seeking without position info
			 * is still possible with relative deltas */
			memset(&upnp, 0, sizeof(upnp));
		}
	}

	/* Per-file loop */
	for (fileidx = 0; fileidx < argc && running; fileidx++) {
		const char *file = argv[fileidx];
		const char *title;
		char ytdlp_url[2048];
		char ytdlp_title[256];
		int force_tc = transcode;

		/* Resolve web URLs via yt-dlp */
		ytdlp_title[0] = '\0';
		if (strncmp(file, "http://", 7) == 0 ||
		    strncmp(file, "https://", 8) == 0) {
			printf("\n[%d/%d] Resolving via yt-dlp: %s\n",
			    fileidx + 1, argc, file);
			if (ytdlp_resolve(file, ytdlp_url, sizeof(ytdlp_url),
			    ytdlp_title, sizeof(ytdlp_title)) < 0) {
				if (running)
					fprintf(stderr,
					    "yt-dlp failed to resolve "
					    "%s, skipping\n", file);
				continue;
			}
			DPRINTF("yt-dlp title='%s' url='%s'\n",
			    ytdlp_title, ytdlp_url);
			file = ytdlp_url;
			force_tc = 1;
		} else {
			printf("\n[%d/%d] %s\n", fileidx + 1, argc, file);
		}

		/* Re-initialize media context for this file */
		memset(&media, 0, sizeof(media));
		media.mode = MODE_FILE;
		media.filepath = file;
		media.running = 1;
		media.pipe_rd = -1;
		media.pipe_wr = -1;
		media.ctrl_fd = -1;
		media.bitrate = bitrate;
		media.vcodec = vcodec;
		if (lang_mode && lang_arg != NULL)
			media.audio_selector = lang_arg;
		/* channelmap is cleared by memset; restore for each file */
		if (channelmap_mode && channelmap_arg != NULL) {
			const channelmap_preset_t *p;

			for (p = channelmap_presets; p->name != NULL; p++) {
				if (strcmp(p->name, channelmap_arg) == 0) {
					memcpy(media.channelmap, p->map,
					    sizeof(media.channelmap));
					media.has_channelmap = 1;
					break;
				}
			}
		}

		/* Probe */
		if (media_probe(&media, file, force_tc) < 0) {
			if (running)
				fprintf(stderr,
				    "Failed to probe %s, skipping\n",
				    file);
			continue;
		}

		/* Connect data socket; set as pipe_wr before opening pipeline */
		data_fd = unix_connect(data_path);
		if (data_fd < 0) {
			fprintf(stderr, "Cannot connect to server data "
			    "socket %s, skipping\n", data_path);
			media_close(&media);
			continue;
		}
		media.pipe_wr = data_fd;
		media.ctrl_fd = ctrl_fd;

		if (media.needs_transcode) {
			printf("Transcoding %s\n", transcode ?
			    "forced by -t flag" :
			    "required (format not natively supported)");
			if (media_open_transcode(&media) < 0) {
				fprintf(stderr, "Failed to set up "
				    "transcoding, skipping\n");
				media.pipe_wr = -1;
				close(data_fd);
				data_fd = -1;
				media_close(&media);
				continue;
			}
			if (pthread_create(&media.thread, NULL,
			    media_transcode_thread, &media) != 0) {
				fprintf(stderr, "Failed to start "
				    "transcoding, skipping\n");
				media.pipe_wr = -1;
				close(data_fd);
				data_fd = -1;
				media_close(&media);
				continue;
			}
		} else {
			printf("Format supported, remuxing to MPEG-TS\n");
			if (media_open_remux(&media) < 0) {
				fprintf(stderr, "Failed to set up "
				    "remux, skipping\n");
				media.pipe_wr = -1;
				close(data_fd);
				data_fd = -1;
				media_close(&media);
				continue;
			}
			if (pthread_create(&media.thread, NULL,
			    media_remux_thread, &media) != 0) {
				fprintf(stderr, "Failed to start "
				    "remux, skipping\n");
				media.pipe_wr = -1;
				close(data_fd);
				data_fd = -1;
				media_close(&media);
				continue;
			}
		}
		/* data_fd is now owned by media.pipe_wr / thread */
		data_fd = -1;

		if (!running) {
			media_close(&media);
			break;
		}

		/* Derive title from yt-dlp metadata or filename */
		if (ytdlp_title[0] != '\0')
			title = ytdlp_title;
		else {
			title = strrchr(file, '/');
			title = (title != NULL) ? title + 1 : file;
		}
		(void)title; /* title used for display only in client mode */

		/* Tell server to start playback */
		{
			char play_cmd[256];
			snprintf(play_cmd, sizeof(play_cmd),
			    "PLAY %s %s\n",
			    media.mime_type, media.dlna_profile);
			printf("Sending PLAY to server...\n");
			ctrl_send(ctrl_fd, play_cmd);
		}

		if (!running)
			goto next_file;

		/* Enter raw terminal mode for key input */
		if (term_raw_mode() == 0)
			printf("Playing. Keys: arrows=seek, "
			    "e=end/back, q=next, Q=quit\n");
		else
			printf("Playing. Press Ctrl+C to stop.\n");

		/* Event loop: poll stdin for keypresses */
		{
			struct pollfd	 pfd;
			unsigned char	 buf[8];
			ssize_t		 n;
			int		 delta;
			int		 end_mode = 0;
			int		 saved_pos = 0;
			int		 seek_delta = 0;
			int		 seek_pending = 0;
			struct timespec	 seek_ts;

			pfd.fd = STDIN_FILENO;
			pfd.events = POLLIN;

			while (running && media.running) {
				int timeout = 500;

				/* Fire debounced seek if 500ms have elapsed */
				if (seek_pending) {
					struct timespec	 now;
					long		 elapsed_ms;
					int		 pos = 0, target;

					clock_gettime(CLOCK_MONOTONIC, &now);
					elapsed_ms =
					    (now.tv_sec  - seek_ts.tv_sec)
					    * 1000 +
					    (now.tv_nsec - seek_ts.tv_nsec)
					    / 1000000;
					if (elapsed_ms >= 500) {
						seek_pending = 0;
						if (upnp.control_url[0] != '\0')
							upnp_get_position(&upnp,
							    &pos);
						target = media.start_sec + pos
						    + seek_delta;
						seek_delta = 0;
						if (target < 0)
							target = 0;
						if (media.duration_sec > 0 &&
						    target >
						    media.duration_sec - 5)
							target =
							    media.duration_sec
							    - 5;

						DPRINTF("seek: restart at "
						    "%ds\n", target);
						media.running = 0;
						pthread_join(media.thread,
						    NULL);

						data_fd = unix_connect(
						    data_path);
						if (data_fd < 0) {
							fprintf(stderr,
							    "Seek: data connect"
							    " failed\n");
							running = 0;
							break;
						}
						media_close_transcode_state(
						    &media);
						media.pipe_wr = data_fd;
						media.ctrl_fd = ctrl_fd;
						media.running = 1;
						media.start_sec = target;
						av_seek_frame(media.ifmt_ctx,
						    -1,
						    (int64_t)target *
						    AV_TIME_BASE,
						    AVSEEK_FLAG_BACKWARD);
						if (media.video_dec)
							avcodec_flush_buffers(
							    media.video_dec);
						if (media.audio_dec)
							avcodec_flush_buffers(
							    media.audio_dec);
						if (media.needs_transcode) {
							if (media_open_transcode(
							    &media) < 0 ||
							    pthread_create(
							    &media.thread, NULL,
							    media_transcode_thread,
							    &media) != 0) {
								fprintf(stderr,
								    "Seek "
								    "failed\n");
								close(data_fd);
								data_fd = -1;
								running = 0;
								break;
							}
						} else {
							if (media_open_remux(
							    &media) < 0 ||
							    pthread_create(
							    &media.thread, NULL,
							    media_remux_thread,
							    &media) != 0) {
								fprintf(stderr,
								    "Seek "
								    "failed\n");
								close(data_fd);
								data_fd = -1;
								running = 0;
								break;
							}
						}
						data_fd = -1;
						{
							char play_cmd[256];
							snprintf(play_cmd,
							    sizeof(play_cmd),
							    "PLAY %s %s\n",
							    media.mime_type,
							    media.dlna_profile);
							ctrl_send(ctrl_fd,
							    play_cmd);
						}
						continue;
					} else {
						timeout = (int)(500 -
						    elapsed_ms);
					}
				}

				if (poll(&pfd, 1, timeout) <= 0)
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

				/* e: jump to last minute / jump back */
				if (buf[0] == 'e' &&
				    media.duration_sec > 0) {
					int epos = 0, etarget;

					if (!end_mode) {
						if (upnp.control_url[0] != '\0')
							upnp_get_position(&upnp,
							    &epos);
						etarget = media.duration_sec
						    - 60;
						if (etarget < 0)
							etarget = 0;
						saved_pos = media.start_sec
						    + epos;
						end_mode = 1;
					} else {
						etarget = saved_pos;
						end_mode = 0;
					}

					DPRINTF("seek: restart at %ds\n",
					    etarget);
					media.running = 0;
					pthread_join(media.thread, NULL);

					data_fd = unix_connect(data_path);
					if (data_fd < 0) {
						fprintf(stderr,
						    "Seek: data connect "
						    "failed\n");
						running = 0;
						break;
					}
					media_close_transcode_state(&media);
					media.pipe_wr = data_fd;
					media.ctrl_fd = ctrl_fd;
					media.running = 1;
					media.start_sec = etarget;
					av_seek_frame(media.ifmt_ctx, -1,
					    (int64_t)etarget * AV_TIME_BASE,
					    AVSEEK_FLAG_BACKWARD);
					if (media.video_dec)
						avcodec_flush_buffers(
						    media.video_dec);
					if (media.audio_dec)
						avcodec_flush_buffers(
						    media.audio_dec);
					if (media.needs_transcode) {
						if (media_open_transcode(
						    &media) < 0 ||
						    pthread_create(
						    &media.thread, NULL,
						    media_transcode_thread,
						    &media) != 0) {
							fprintf(stderr,
							    "Seek failed\n");
							close(data_fd);
							data_fd = -1;
							running = 0;
							break;
						}
					} else {
						if (media_open_remux(
						    &media) < 0 ||
						    pthread_create(
						    &media.thread, NULL,
						    media_remux_thread,
						    &media) != 0) {
							fprintf(stderr,
							    "Seek failed\n");
							close(data_fd);
							data_fd = -1;
							running = 0;
							break;
						}
					}
					data_fd = -1;
					{
						char play_cmd[256];
						snprintf(play_cmd,
						    sizeof(play_cmd),
						    "PLAY %s %s\n",
						    media.mime_type,
						    media.dlna_profile);
						ctrl_send(ctrl_fd, play_cmd);
					}
					continue;
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

				seek_delta += delta;
				seek_pending = 1;
				clock_gettime(CLOCK_MONOTONIC, &seek_ts);
			}
		}

		term_restore();

	next_file:
		printf("\nStopping...\n");
		ctrl_send(ctrl_fd, "STOP\n");

		media.running = 0;
		pthread_join(media.thread, NULL);

		media_close(&media);
	}

	close(ctrl_fd);

	printf("Done.\n");
	return 0;
}
