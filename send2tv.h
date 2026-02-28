#ifndef SEND2TV_H
#define SEND2TV_H

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/defs.h>		/* AV_PROFILE_* */
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>

#define SEND2TV_BUF_SIZE	65536
#define SEND2TV_SOAP_BUF	8192
#define SEND2TV_DEFAULT_PORT	0	/* ephemeral */

extern int verbose;
#define DPRINTF(fmt, ...) \
	do { if (verbose) fprintf(stderr, "debug: " fmt, ##__VA_ARGS__); } while (0)

/* Media mode */
enum {
	MODE_FILE,
	MODE_SCREEN
};

/* Media context */
typedef struct {
	int		 mode;		/* MODE_FILE or MODE_SCREEN */
	const char	*filepath;	/* NULL in screen mode */
	int		 needs_transcode;
	char		 mime_type[64];
	char		 dlna_profile[64]; /* DLNA.ORG_PN value */

	/* pipe for transcoded/captured output */
	int		 pipe_rd;
	int		 pipe_wr;

	/* input */
	AVFormatContext	*ifmt_ctx;
	int		 video_idx;
	int		 audio_idx;
	AVCodecContext	*video_dec;
	AVCodecContext	*audio_dec;

	/* output (transcoding/capture) */
	AVFormatContext	*ofmt_ctx;
	AVCodecContext	*video_enc;
	AVCodecContext	*audio_enc;

	/* VAAPI */
	AVBufferRef	*hw_device_ctx;

	/* filters (for hwupload + scale_vaapi) */
	AVFilterGraph	*filter_graph;
	AVFilterContext	*buffersrc_ctx;
	AVFilterContext	*buffersink_ctx;

	/* audio resampling */
	struct SwrContext *swr_ctx;
	AVAudioFifo	*audio_fifo;

	/* screen capture: second input for sndio audio */
	AVFormatContext	*sndio_ctx;
	int		 sndio_audio_idx;
	AVCodecContext	*sndio_dec;
	const char	*sndio_device;	/* sndio monitor device name */

	volatile int	 running;
	pthread_t	 thread;
	int		 start_sec;	/* transcode start position */
} media_ctx_t;

/* UPnP context */
typedef struct {
	char		 tv_ip[64];
	int		 tv_port;
	char		 control_url[256];
	char		 local_ip[64];
	int		 local_http_port;
} upnp_ctx_t;

/* HTTP server context */
typedef struct {
	int		 listen_fd;
	int		 port;
	media_ctx_t	*media;
	volatile int	 running;
	pthread_t	 thread;
} httpd_ctx_t;

/* upnp.c */
int	 upnp_discover(void);
int	 upnp_find_transport(upnp_ctx_t *ctx);
int	 upnp_set_uri(upnp_ctx_t *ctx, const char *uri, const char *mime,
	    const char *title, int is_streaming, const char *dlna_profile);
int	 upnp_play(upnp_ctx_t *ctx);
int	 upnp_stop(upnp_ctx_t *ctx);
int	 upnp_get_local_ip(upnp_ctx_t *ctx);
int	 upnp_get_position(upnp_ctx_t *ctx, int *pos_sec);
int	 upnp_seek(upnp_ctx_t *ctx, int target_sec);
int	 upnp_seek_relative(upnp_ctx_t *ctx, int delta_sec);

/* httpd.c */
int	 httpd_start(httpd_ctx_t *ctx, media_ctx_t *media, int port);
void	 httpd_stop(httpd_ctx_t *ctx);

/* media.c */
int	 media_probe(media_ctx_t *ctx, const char *filepath, int force_transcode);
int	 media_open_transcode(media_ctx_t *ctx);
int	 media_restart_transcode(media_ctx_t *ctx, int start_sec);
int	 media_open_screen(media_ctx_t *ctx);
void	*media_transcode_thread(void *arg);
void	*media_capture_thread(void *arg);
void	 media_close(media_ctx_t *ctx);

#endif /* SEND2TV_H */
