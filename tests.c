/*
 * Unit tests for send2tv.
 *
 * Includes source files directly to test static functions.
 * Build:  make tests
 * Run:    ./tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provide verbose flag needed by DPRINTF macro */
int verbose = 0;

/* Include source files to access static functions */
#include "dlna.c"
#include "media.c"
#include "upnp.c"

/* ------------------------------------------------------------------ */
/* Minimal test framework                                             */
/* ------------------------------------------------------------------ */

static int tests_run;
static int tests_passed;
static int tests_failed;
static int current_failed;

#define TEST(name)	static void test_##name(void)

#define RUN_TEST(name)	do {					\
	current_failed = 0;					\
	printf("  %-55s", #name);				\
	tests_run++;						\
	test_##name();						\
	if (current_failed) {					\
		tests_failed++;					\
	} else {						\
		tests_passed++;					\
		printf("pass\n");				\
	}							\
} while (0)

#define ASSERT(cond)	do {					\
	if (!(cond)) {						\
		printf("FAIL\n    %s:%d: %s\n",			\
		    __FILE__, __LINE__, #cond);			\
		current_failed = 1;				\
		return;						\
	}							\
} while (0)

#define ASSERT_INT_EQ(a, b)	do {				\
	int _a = (a), _b = (b);				\
	if (_a != _b) {						\
		printf("FAIL\n    %s:%d: got %d, want %d\n",	\
		    __FILE__, __LINE__, _a, _b);		\
		current_failed = 1;				\
		return;						\
	}							\
} while (0)

#define ASSERT_STR_EQ(a, b)	do {				\
	const char *_a = (a), *_b = (b);			\
	if (strcmp(_a, _b) != 0) {				\
		printf("FAIL\n    %s:%d: got \"%s\","		\
		    " want \"%s\"\n",				\
		    __FILE__, __LINE__, _a, _b);		\
		current_failed = 1;				\
		return;						\
	}							\
} while (0)

/* ------------------------------------------------------------------ */
/* Tests: video_container_ok                                          */
/* ------------------------------------------------------------------ */

TEST(video_h264_mp4)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_H264, "mp4"), 1);
}

TEST(video_h264_matroska)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_H264,
	    "matroska,webm"), 1);
}

TEST(video_h264_avi)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_H264, "avi"), 1);
}

TEST(video_h264_asf)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_H264, "asf"), 1);
}

TEST(video_h264_flv)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_H264, "flv"), 1);
}

TEST(video_h264_mpegts)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_H264, "mpegts"), 1);
}

TEST(video_h264_mov)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_H264, "mov"), 1);
}

TEST(video_h264_webm_rejected)
{
	/* H264 in plain webm is not supported */
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_H264, "webm"), 0);
}

TEST(video_h264_null)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_H264, NULL), 0);
}

TEST(video_hevc_mp4)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_HEVC, "mp4"), 1);
}

TEST(video_hevc_matroska)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_HEVC,
	    "matroska,webm"), 1);
}

TEST(video_hevc_mpegts)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_HEVC, "mpegts"), 1);
}

TEST(video_hevc_avi_rejected)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_HEVC, "avi"), 0);
}

TEST(video_vp8_webm)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_VP8, "webm"), 1);
}

TEST(video_vp8_matroska)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_VP8,
	    "matroska,webm"), 1);
}

TEST(video_vp8_mp4_rejected)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_VP8, "mp4"), 0);
}

TEST(video_vp9_webm)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_VP9, "webm"), 1);
}

TEST(video_av1_matroska)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_AV1,
	    "matroska,webm"), 1);
}

TEST(video_mpeg4_avi)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_MPEG4, "avi"), 1);
}

TEST(video_mpeg4_mp4)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_MPEG4, "mp4"), 1);
}

TEST(video_mpeg4_mpeg_rejected)
{
	/* MPEG4 in plain mpeg container is not listed */
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_MPEG4, "mpeg"), 0);
}

TEST(video_mpeg2_mpeg)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_MPEG2VIDEO,
	    "mpeg"), 1);
}

TEST(video_mpeg1_mpeg)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_MPEG1VIDEO,
	    "mpeg"), 1);
}

TEST(video_vc1_avi)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_VC1, "avi"), 1);
}

TEST(video_wmv3_asf)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_WMV3, "asf"), 1);
}

TEST(video_mjpeg_avi)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_MJPEG, "avi"), 1);
}

TEST(video_unknown_codec)
{
	ASSERT_INT_EQ(video_container_ok(AV_CODEC_ID_NONE, "mp4"), 0);
}

/* ------------------------------------------------------------------ */
/* Tests: audio_codec_ok                                              */
/* ------------------------------------------------------------------ */

TEST(audio_aac)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_AAC), 1);
}

TEST(audio_mp3)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_MP3), 1);
}

TEST(audio_mp2)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_MP2), 1);
}

TEST(audio_flac)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_FLAC), 1);
}

TEST(audio_ac3)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_AC3), 1);
}

TEST(audio_eac3)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_EAC3), 1);
}

TEST(audio_vorbis)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_VORBIS), 1);
}

TEST(audio_opus)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_OPUS), 1);
}

TEST(audio_wmav1)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_WMAV1), 1);
}

TEST(audio_wmav2)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_WMAV2), 1);
}

TEST(audio_pcm_s16le)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_PCM_S16LE), 1);
}

TEST(audio_pcm_alaw)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_PCM_ALAW), 1);
}

TEST(audio_pcm_mulaw)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_PCM_MULAW), 1);
}

TEST(audio_adpcm_ima_wav)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_ADPCM_IMA_WAV), 1);
}

TEST(audio_adpcm_ms)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_ADPCM_MS), 1);
}

TEST(audio_dts_rejected)
{
	/* DTS is NOT supported on Samsung 2024 TVs */
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_DTS), 0);
}

TEST(audio_unknown)
{
	ASSERT_INT_EQ(audio_codec_ok(AV_CODEC_ID_NONE), 0);
}

/* ------------------------------------------------------------------ */
/* Tests: container_ok                                                */
/* ------------------------------------------------------------------ */

TEST(container_mp4)
{
	ASSERT_INT_EQ(container_ok("mp4"), 1);
}

TEST(container_matroska)
{
	ASSERT_INT_EQ(container_ok("matroska,webm"), 1);
}

TEST(container_mpegts)
{
	ASSERT_INT_EQ(container_ok("mpegts"), 1);
}

TEST(container_avi)
{
	ASSERT_INT_EQ(container_ok("avi"), 1);
}

TEST(container_asf)
{
	ASSERT_INT_EQ(container_ok("asf"), 1);
}

TEST(container_flv)
{
	ASSERT_INT_EQ(container_ok("flv"), 1);
}

TEST(container_mp3)
{
	ASSERT_INT_EQ(container_ok("mp3"), 1);
}

TEST(container_flac)
{
	ASSERT_INT_EQ(container_ok("flac"), 1);
}

TEST(container_ogg)
{
	ASSERT_INT_EQ(container_ok("ogg"), 1);
}

TEST(container_wav)
{
	ASSERT_INT_EQ(container_ok("wav"), 1);
}

TEST(container_null)
{
	ASSERT_INT_EQ(container_ok(NULL), 0);
}

TEST(container_unknown)
{
	ASSERT_INT_EQ(container_ok("unknown"), 0);
}

/* ------------------------------------------------------------------ */
/* Tests: set_mime_type                                               */
/* ------------------------------------------------------------------ */

TEST(mime_mp4)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "mp4", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.mime_type, "video/mp4");
}

TEST(mime_mov)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "mov,mp4,m4a,3gp", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.mime_type, "video/mp4");
}

TEST(mime_mkv_h264)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "matroska,webm", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.mime_type, "video/x-mkv");
}

TEST(mime_mkv_vp8)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "matroska,webm", AV_CODEC_ID_VP8);
	ASSERT_STR_EQ(m.mime_type, "video/webm");
}

TEST(mime_mkv_vp9)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "matroska,webm", AV_CODEC_ID_VP9);
	ASSERT_STR_EQ(m.mime_type, "video/webm");
}

TEST(mime_mkv_av1)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "matroska,webm", AV_CODEC_ID_AV1);
	ASSERT_STR_EQ(m.mime_type, "video/webm");
}

TEST(mime_mpegts)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "mpegts", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.mime_type, "video/mp2t");
}

TEST(mime_mpeg)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "mpeg", AV_CODEC_ID_MPEG2VIDEO);
	ASSERT_STR_EQ(m.mime_type, "video/mpeg");
}

TEST(mime_avi)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "avi", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.mime_type, "video/avi");
}

TEST(mime_asf)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "asf", AV_CODEC_ID_WMV3);
	ASSERT_STR_EQ(m.mime_type, "video/x-ms-wmv");
}

TEST(mime_flv)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "flv", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.mime_type, "video/x-flv");
}

TEST(mime_audio_mp3)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "mp3", AV_CODEC_ID_NONE);
	ASSERT_STR_EQ(m.mime_type, "audio/mpeg");
}

TEST(mime_audio_flac)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "flac", AV_CODEC_ID_NONE);
	ASSERT_STR_EQ(m.mime_type, "audio/flac");
}

TEST(mime_audio_ogg)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "ogg", AV_CODEC_ID_NONE);
	ASSERT_STR_EQ(m.mime_type, "audio/ogg");
}

TEST(mime_audio_wav)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "wav", AV_CODEC_ID_NONE);
	ASSERT_STR_EQ(m.mime_type, "audio/wav");
}

TEST(mime_unknown_defaults_to_mp2t)
{
	media_ctx_t m = {0};

	set_mime_type(&m, "something_unknown", AV_CODEC_ID_NONE);
	ASSERT_STR_EQ(m.mime_type, "video/mp2t");
}

/* ------------------------------------------------------------------ */
/* Tests: set_dlna_profile                                            */
/* ------------------------------------------------------------------ */

TEST(dlna_h264_mp4)
{
	media_ctx_t m = {0};

	set_dlna_profile(&m, "mp4", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.dlna_profile, "AVC_MP4_MP_SD_AAC");
}

TEST(dlna_h264_mov)
{
	media_ctx_t m = {0};

	set_dlna_profile(&m, "mov,mp4", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.dlna_profile, "AVC_MP4_MP_SD_AAC");
}

TEST(dlna_h264_3gp)
{
	media_ctx_t m = {0};

	set_dlna_profile(&m, "3gp", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.dlna_profile, "AVC_MP4_MP_SD_AAC");
}

TEST(dlna_h264_matroska)
{
	media_ctx_t m = {0};

	set_dlna_profile(&m, "matroska,webm", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.dlna_profile, "AVC_MKV_MP_HD_AAC");
}

TEST(dlna_h264_mpegts)
{
	media_ctx_t m = {0};

	set_dlna_profile(&m, "mpegts", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.dlna_profile, "AVC_TS_MP_SD_AAC_MULT5");
}

TEST(dlna_h264_avi)
{
	media_ctx_t m = {0};

	set_dlna_profile(&m, "avi", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.dlna_profile, "AVC_MP4_MP_SD_AAC");
}

TEST(dlna_hevc_mp4)
{
	media_ctx_t m = {0};

	set_dlna_profile(&m, "mp4", AV_CODEC_ID_HEVC);
	ASSERT_STR_EQ(m.dlna_profile, "HEVC_MP4_MP_L51_AAC");
}

TEST(dlna_hevc_matroska_empty)
{
	media_ctx_t m = {0};

	set_dlna_profile(&m, "matroska,webm", AV_CODEC_ID_HEVC);
	ASSERT_STR_EQ(m.dlna_profile, "");
}

TEST(dlna_mpeg4)
{
	media_ctx_t m = {0};

	set_dlna_profile(&m, "mp4", AV_CODEC_ID_MPEG4);
	ASSERT_STR_EQ(m.dlna_profile, "MPEG4_P2_MP4_SP_AAC");
}

TEST(dlna_vp8_empty)
{
	media_ctx_t m = {0};

	set_dlna_profile(&m, "webm", AV_CODEC_ID_VP8);
	ASSERT_STR_EQ(m.dlna_profile, "");
}

TEST(dlna_h264_unknown_fmt_empty)
{
	media_ctx_t m = {0};

	set_dlna_profile(&m, "unknown", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(m.dlna_profile, "");
}

/* ------------------------------------------------------------------ */
/* Tests: build_dlna_features (DLNA spec compliance)                  */
/* ------------------------------------------------------------------ */

/*
 * DLNA spec: file mode (non-streaming) must have OP=01 (byte-seek
 * supported), CI=0 (not transcoded), and the PN tag present.
 */
TEST(dlna_features_file_with_profile)
{
	char buf[256];

	build_dlna_features(buf, sizeof(buf), "AVC_MP4_MP_SD_AAC", 0);
	ASSERT(strstr(buf, "DLNA.ORG_PN=AVC_MP4_MP_SD_AAC") != NULL);
	ASSERT(strstr(buf, "DLNA.ORG_OP=01") != NULL);
	ASSERT(strstr(buf, "DLNA.ORG_CI=0") != NULL);
	ASSERT(strstr(buf, "DLNA.ORG_FLAGS=") != NULL);
}

/*
 * DLNA spec: streaming mode must have OP=00 (no seek), CI=1
 * (transcoded), and the PN tag present.
 */
TEST(dlna_features_streaming_with_profile)
{
	char buf[256];

	build_dlna_features(buf, sizeof(buf), "AVC_TS_HP_HD_AAC_MULT5", 1);
	ASSERT(strstr(buf, "DLNA.ORG_PN=AVC_TS_HP_HD_AAC_MULT5") != NULL);
	ASSERT(strstr(buf, "DLNA.ORG_OP=00") != NULL);
	ASSERT(strstr(buf, "DLNA.ORG_CI=1") != NULL);
}

/*
 * When dlna_profile is NULL, DLNA.ORG_PN must be omitted entirely.
 */
TEST(dlna_features_null_profile)
{
	char buf[256];

	build_dlna_features(buf, sizeof(buf), NULL, 0);
	ASSERT(strstr(buf, "DLNA.ORG_PN") == NULL);
	ASSERT(strstr(buf, "DLNA.ORG_OP=01") != NULL);
	ASSERT(strstr(buf, "DLNA.ORG_CI=0") != NULL);
}

/*
 * When dlna_profile is empty string, DLNA.ORG_PN must be omitted.
 */
TEST(dlna_features_empty_profile)
{
	char buf[256];

	build_dlna_features(buf, sizeof(buf), "", 0);
	ASSERT(strstr(buf, "DLNA.ORG_PN") == NULL);
	ASSERT(strstr(buf, "DLNA.ORG_OP=01") != NULL);
}

/*
 * DLNA spec: DLNA.ORG_OP must be exactly two characters "ab" where
 * a=time-seek support and b=byte-seek support, each 0 or 1.
 * File mode: a=0 b=1 -> "01".  Streaming: a=0 b=0 -> "00".
 */
TEST(dlna_features_op_format_file)
{
	char buf[256];
	char *p;

	build_dlna_features(buf, sizeof(buf), "AVC_MP4_MP_SD_AAC", 0);
	p = strstr(buf, "DLNA.ORG_OP=");
	ASSERT(p != NULL);
	p += strlen("DLNA.ORG_OP=");
	/* Exactly "01" followed by a separator */
	ASSERT(p[0] == '0');
	ASSERT(p[1] == '1');
	ASSERT(p[2] == ';');
}

TEST(dlna_features_op_format_streaming)
{
	char buf[256];
	char *p;

	build_dlna_features(buf, sizeof(buf), "AVC_TS_HP_HD_AAC_MULT5", 1);
	p = strstr(buf, "DLNA.ORG_OP=");
	ASSERT(p != NULL);
	p += strlen("DLNA.ORG_OP=");
	ASSERT(p[0] == '0');
	ASSERT(p[1] == '0');
	ASSERT(p[2] == ';');
}

/*
 * DLNA spec: DLNA.ORG_CI must be "0" for original content and "1"
 * for transcoded content.
 */
TEST(dlna_features_ci_original)
{
	char buf[256];
	char *p;

	build_dlna_features(buf, sizeof(buf), "AVC_MP4_MP_SD_AAC", 0);
	p = strstr(buf, "DLNA.ORG_CI=");
	ASSERT(p != NULL);
	p += strlen("DLNA.ORG_CI=");
	ASSERT(p[0] == '0');
	ASSERT(p[1] == ';');
}

TEST(dlna_features_ci_transcoded)
{
	char buf[256];
	char *p;

	build_dlna_features(buf, sizeof(buf), "AVC_TS_HP_HD_AAC_MULT5", 1);
	p = strstr(buf, "DLNA.ORG_CI=");
	ASSERT(p != NULL);
	p += strlen("DLNA.ORG_CI=");
	ASSERT(p[0] == '1');
	ASSERT(p[1] == ';');
}

/*
 * DLNA spec: DLNA.ORG_FLAGS must be exactly 32 hexadecimal characters.
 * Our flags are "01700000000000000000000000000000" which sets:
 *   bit 24: DLNA_ORG_FLAG_SENDER_PACED
 *   bit 22: DLNA_ORG_FLAG_CONNECTION_STALLING
 *   bit 21: DLNA_ORG_FLAG_DLNA_V15
 *   bit 20: DLNA_ORG_FLAG_STREAMING_TRANSFER
 */
TEST(dlna_features_flags_length)
{
	char buf[256];
	char *p;
	int i;

	build_dlna_features(buf, sizeof(buf), "AVC_MP4_MP_SD_AAC", 0);
	p = strstr(buf, "DLNA.ORG_FLAGS=");
	ASSERT(p != NULL);
	p += strlen("DLNA.ORG_FLAGS=");
	/* Must be exactly 32 hex characters */
	for (i = 0; i < 32; i++) {
		ASSERT((p[i] >= '0' && p[i] <= '9') ||
		    (p[i] >= 'a' && p[i] <= 'f') ||
		    (p[i] >= 'A' && p[i] <= 'F'));
	}
	/* Must end after 32 chars (nul or no more content) */
	ASSERT(p[32] == '\0' || p[32] == ';');
}

TEST(dlna_features_flags_value)
{
	char buf[256];
	char *p;

	build_dlna_features(buf, sizeof(buf), "AVC_MP4_MP_SD_AAC", 0);
	p = strstr(buf, "DLNA.ORG_FLAGS=");
	ASSERT(p != NULL);
	p += strlen("DLNA.ORG_FLAGS=");
	ASSERT(strncmp(p, "01700000000000000000000000000000", 32) == 0);
}

/*
 * DLNA spec: the fourth field of protocolInfo in DIDL-Lite must
 * match what's in the contentFeatures.dlna.org HTTP header.
 * Verify build_dlna_features produces consistent output for both.
 */
TEST(dlna_features_consistency)
{
	char buf1[256], buf2[256];

	build_dlna_features(buf1, sizeof(buf1), "AVC_TS_HP_HD_AAC_MULT5", 1);
	build_dlna_features(buf2, sizeof(buf2), "AVC_TS_HP_HD_AAC_MULT5", 1);
	ASSERT_STR_EQ(buf1, buf2);
}

/*
 * DLNA spec: semicolons separate each field in the features string.
 * Verify proper structure with profile present.
 */
TEST(dlna_features_field_order)
{
	char buf[256];
	char *pn, *op, *ci, *fl;

	build_dlna_features(buf, sizeof(buf), "AVC_MP4_MP_SD_AAC", 0);
	pn = strstr(buf, "DLNA.ORG_PN=");
	op = strstr(buf, "DLNA.ORG_OP=");
	ci = strstr(buf, "DLNA.ORG_CI=");
	fl = strstr(buf, "DLNA.ORG_FLAGS=");
	ASSERT(pn != NULL);
	ASSERT(op != NULL);
	ASSERT(ci != NULL);
	ASSERT(fl != NULL);
	/* PN comes first, then OP, then CI, then FLAGS */
	ASSERT(pn < op);
	ASSERT(op < ci);
	ASSERT(ci < fl);
}

/*
 * Without profile: field order should be OP, CI, FLAGS.
 */
TEST(dlna_features_field_order_no_profile)
{
	char buf[256];
	char *op, *ci, *fl;

	build_dlna_features(buf, sizeof(buf), NULL, 1);
	op = strstr(buf, "DLNA.ORG_OP=");
	ci = strstr(buf, "DLNA.ORG_CI=");
	fl = strstr(buf, "DLNA.ORG_FLAGS=");
	ASSERT(op != NULL);
	ASSERT(ci != NULL);
	ASSERT(fl != NULL);
	ASSERT(op < ci);
	ASSERT(ci < fl);
}

/*
 * Transcode path: the hardcoded profile must be AVC_TS_HP_HD_AAC_MULT5
 * to match the H.264 High Profile encoder output.
 */
TEST(dlna_transcode_profile_matches_encoder)
{
	media_ctx_t m = {0};

	m.needs_transcode = 1;
	strlcpy(m.mime_type, "video/mp2t", sizeof(m.mime_type));
	strlcpy(m.dlna_profile, "AVC_TS_HP_HD_AAC_MULT5",
	    sizeof(m.dlna_profile));
	/* HP = High Profile, matches AV_PROFILE_H264_HIGH in encoder */
	ASSERT(strstr(m.dlna_profile, "HP") != NULL);
	/* TS = MPEG-TS, matches the mpegts muxer */
	ASSERT(strstr(m.dlna_profile, "TS") != NULL);
	/* AAC audio */
	ASSERT(strstr(m.dlna_profile, "AAC") != NULL);
}

/*
 * HEVC transcode profile must be HEVC_TS_HD_NA.
 */
TEST(dlna_transcode_profile_hevc)
{
	media_ctx_t m = {0};

	m.needs_transcode = 1;
	m.vcodec = VCODEC_HEVC;
	strlcpy(m.mime_type, "video/mp2t", sizeof(m.mime_type));
	strlcpy(m.dlna_profile, "HEVC_TS_HD_NA",
	    sizeof(m.dlna_profile));
	ASSERT(strstr(m.dlna_profile, "HEVC") != NULL);
	ASSERT(strstr(m.dlna_profile, "TS") != NULL);
}

/*
 * Transcode MIME type must be video/mp2t (MPEG transport stream).
 */
TEST(dlna_transcode_mime_type)
{
	media_ctx_t m = {0};

	strlcpy(m.mime_type, "video/mp2t", sizeof(m.mime_type));
	ASSERT_STR_EQ(m.mime_type, "video/mp2t");
}

/*
 * Screen capture path: same profile and MIME as transcode.
 */
TEST(dlna_screen_capture_profile)
{
	media_ctx_t m = {0};

	m.needs_transcode = 1;
	strlcpy(m.mime_type, "video/mp2t", sizeof(m.mime_type));
	strlcpy(m.dlna_profile, "AVC_TS_HP_HD_AAC_MULT5",
	    sizeof(m.dlna_profile));
	ASSERT_STR_EQ(m.dlna_profile, "AVC_TS_HP_HD_AAC_MULT5");
	ASSERT_STR_EQ(m.mime_type, "video/mp2t");
}

/*
 * DLNA protocolInfo fourth field: verify full features string can be
 * used as-is in protocolInfo="http-get:*:<mime>:<features>".
 * The features string must not contain colons (which delimit
 * protocolInfo fields).
 */
TEST(dlna_features_no_colons)
{
	char buf[256];

	build_dlna_features(buf, sizeof(buf), "AVC_TS_HP_HD_AAC_MULT5", 1);
	ASSERT(strchr(buf, ':') == NULL);
}

TEST(dlna_features_no_colons_no_profile)
{
	char buf[256];

	build_dlna_features(buf, sizeof(buf), NULL, 0);
	ASSERT(strchr(buf, ':') == NULL);
}

/*
 * HEVC streaming features string.
 */
TEST(dlna_features_hevc_streaming)
{
	char buf[256];

	build_dlna_features(buf, sizeof(buf), "HEVC_TS_HD_NA", 1);
	ASSERT(strstr(buf, "DLNA.ORG_PN=HEVC_TS_HD_NA") != NULL);
	ASSERT(strstr(buf, "DLNA.ORG_OP=00") != NULL);
	ASSERT(strstr(buf, "DLNA.ORG_CI=1") != NULL);
}

/*
 * DLNA spec: the direct-play MPEG-TS profile should be Main Profile SD
 * (for passthrough files), while the transcode profile should be High
 * Profile HD (since the encoder uses AV_PROFILE_H264_HIGH).
 */
TEST(dlna_mpegts_direct_vs_transcode)
{
	media_ctx_t direct = {0};

	/* Direct play of an MPEG-TS file */
	set_dlna_profile(&direct, "mpegts", AV_CODEC_ID_H264);
	ASSERT_STR_EQ(direct.dlna_profile, "AVC_TS_MP_SD_AAC_MULT5");

	/* Transcode uses AVC_TS_HP_HD_AAC_MULT5 (set by media_probe) */
	ASSERT(strcmp(direct.dlna_profile, "AVC_TS_HP_HD_AAC_MULT5") != 0);
}

/* ------------------------------------------------------------------ */
/* Tests: vcodec enum                                                 */
/* ------------------------------------------------------------------ */

TEST(vcodec_default_is_h264)
{
	media_ctx_t m = {0};

	ASSERT_INT_EQ(m.vcodec, VCODEC_H264);
}

TEST(vcodec_hevc_value)
{
	ASSERT_INT_EQ(VCODEC_HEVC, 1);
}

/* ------------------------------------------------------------------ */
/* Tests: xml_extract                                                 */
/* ------------------------------------------------------------------ */

TEST(xml_extract_basic)
{
	char out[64];
	int ret;

	ret = xml_extract("<root><name>hello</name></root>",
	    "<name>", "</name>", out, sizeof(out));
	ASSERT_INT_EQ(ret, 0);
	ASSERT_STR_EQ(out, "hello");
}

TEST(xml_extract_nested)
{
	char out[128];
	int ret;

	ret = xml_extract(
	    "<service>"
	    "<serviceType>AVTransport</serviceType>"
	    "<controlURL>/ctrl</controlURL>"
	    "</service>",
	    "<controlURL>", "</controlURL>", out, sizeof(out));
	ASSERT_INT_EQ(ret, 0);
	ASSERT_STR_EQ(out, "/ctrl");
}

TEST(xml_extract_open_not_found)
{
	char out[64];
	int ret;

	ret = xml_extract("<root>data</root>",
	    "<missing>", "</missing>", out, sizeof(out));
	ASSERT_INT_EQ(ret, -1);
}

TEST(xml_extract_close_not_found)
{
	char out[64];
	int ret;

	ret = xml_extract("<root><name>data",
	    "<name>", "</name>", out, sizeof(out));
	ASSERT_INT_EQ(ret, -1);
}

TEST(xml_extract_empty_content)
{
	char out[64];
	int ret;

	ret = xml_extract("<tag></tag>",
	    "<tag>", "</tag>", out, sizeof(out));
	ASSERT_INT_EQ(ret, 0);
	ASSERT_STR_EQ(out, "");
}

TEST(xml_extract_truncation)
{
	char out[6]; /* only 5 chars + nul */
	int ret;

	ret = xml_extract("<t>abcdefghij</t>",
	    "<t>", "</t>", out, sizeof(out));
	ASSERT_INT_EQ(ret, 0);
	ASSERT_STR_EQ(out, "abcde");
}

TEST(xml_extract_first_match)
{
	char out[64];
	int ret;

	ret = xml_extract("<a>first</a><a>second</a>",
	    "<a>", "</a>", out, sizeof(out));
	ASSERT_INT_EQ(ret, 0);
	ASSERT_STR_EQ(out, "first");
}

/* ------------------------------------------------------------------ */
/* Tests: xml_encode                                                  */
/* ------------------------------------------------------------------ */

TEST(xml_encode_plain)
{
	char *r = xml_encode("hello world");

	ASSERT(r != NULL);
	ASSERT_STR_EQ(r, "hello world");
	free(r);
}

TEST(xml_encode_lt_gt)
{
	char *r = xml_encode("<tag>");

	ASSERT(r != NULL);
	ASSERT_STR_EQ(r, "&lt;tag&gt;");
	free(r);
}

TEST(xml_encode_amp)
{
	char *r = xml_encode("a&b");

	ASSERT(r != NULL);
	ASSERT_STR_EQ(r, "a&amp;b");
	free(r);
}

TEST(xml_encode_quotes)
{
	char *r = xml_encode("say \"hello\" & 'bye'");

	ASSERT(r != NULL);
	ASSERT_STR_EQ(r, "say &quot;hello&quot; &amp; &apos;bye&apos;");
	free(r);
}

TEST(xml_encode_empty)
{
	char *r = xml_encode("");

	ASSERT(r != NULL);
	ASSERT_STR_EQ(r, "");
	free(r);
}

TEST(xml_encode_all_special)
{
	char *r = xml_encode("<>&\"'");

	ASSERT(r != NULL);
	ASSERT_STR_EQ(r, "&lt;&gt;&amp;&quot;&apos;");
	free(r);
}

/* ------------------------------------------------------------------ */
/* Main: run all tests                                                */
/* ------------------------------------------------------------------ */

int
main(void)
{
	printf("send2tv unit tests\n\n");

	printf("video_container_ok:\n");
	RUN_TEST(video_h264_mp4);
	RUN_TEST(video_h264_matroska);
	RUN_TEST(video_h264_avi);
	RUN_TEST(video_h264_asf);
	RUN_TEST(video_h264_flv);
	RUN_TEST(video_h264_mpegts);
	RUN_TEST(video_h264_mov);
	RUN_TEST(video_h264_webm_rejected);
	RUN_TEST(video_h264_null);
	RUN_TEST(video_hevc_mp4);
	RUN_TEST(video_hevc_matroska);
	RUN_TEST(video_hevc_mpegts);
	RUN_TEST(video_hevc_avi_rejected);
	RUN_TEST(video_vp8_webm);
	RUN_TEST(video_vp8_matroska);
	RUN_TEST(video_vp8_mp4_rejected);
	RUN_TEST(video_vp9_webm);
	RUN_TEST(video_av1_matroska);
	RUN_TEST(video_mpeg4_avi);
	RUN_TEST(video_mpeg4_mp4);
	RUN_TEST(video_mpeg4_mpeg_rejected);
	RUN_TEST(video_mpeg2_mpeg);
	RUN_TEST(video_mpeg1_mpeg);
	RUN_TEST(video_vc1_avi);
	RUN_TEST(video_wmv3_asf);
	RUN_TEST(video_mjpeg_avi);
	RUN_TEST(video_unknown_codec);

	printf("\naudio_codec_ok:\n");
	RUN_TEST(audio_aac);
	RUN_TEST(audio_mp3);
	RUN_TEST(audio_mp2);
	RUN_TEST(audio_flac);
	RUN_TEST(audio_ac3);
	RUN_TEST(audio_eac3);
	RUN_TEST(audio_vorbis);
	RUN_TEST(audio_opus);
	RUN_TEST(audio_wmav1);
	RUN_TEST(audio_wmav2);
	RUN_TEST(audio_pcm_s16le);
	RUN_TEST(audio_pcm_alaw);
	RUN_TEST(audio_pcm_mulaw);
	RUN_TEST(audio_adpcm_ima_wav);
	RUN_TEST(audio_adpcm_ms);
	RUN_TEST(audio_dts_rejected);
	RUN_TEST(audio_unknown);

	printf("\ncontainer_ok:\n");
	RUN_TEST(container_mp4);
	RUN_TEST(container_matroska);
	RUN_TEST(container_mpegts);
	RUN_TEST(container_avi);
	RUN_TEST(container_asf);
	RUN_TEST(container_flv);
	RUN_TEST(container_mp3);
	RUN_TEST(container_flac);
	RUN_TEST(container_ogg);
	RUN_TEST(container_wav);
	RUN_TEST(container_null);
	RUN_TEST(container_unknown);

	printf("\nset_mime_type:\n");
	RUN_TEST(mime_mp4);
	RUN_TEST(mime_mov);
	RUN_TEST(mime_mkv_h264);
	RUN_TEST(mime_mkv_vp8);
	RUN_TEST(mime_mkv_vp9);
	RUN_TEST(mime_mkv_av1);
	RUN_TEST(mime_mpegts);
	RUN_TEST(mime_mpeg);
	RUN_TEST(mime_avi);
	RUN_TEST(mime_asf);
	RUN_TEST(mime_flv);
	RUN_TEST(mime_audio_mp3);
	RUN_TEST(mime_audio_flac);
	RUN_TEST(mime_audio_ogg);
	RUN_TEST(mime_audio_wav);
	RUN_TEST(mime_unknown_defaults_to_mp2t);

	printf("\nset_dlna_profile:\n");
	RUN_TEST(dlna_h264_mp4);
	RUN_TEST(dlna_h264_mov);
	RUN_TEST(dlna_h264_3gp);
	RUN_TEST(dlna_h264_matroska);
	RUN_TEST(dlna_h264_mpegts);
	RUN_TEST(dlna_h264_avi);
	RUN_TEST(dlna_hevc_mp4);
	RUN_TEST(dlna_hevc_matroska_empty);
	RUN_TEST(dlna_mpeg4);
	RUN_TEST(dlna_vp8_empty);
	RUN_TEST(dlna_h264_unknown_fmt_empty);

	printf("\nbuild_dlna_features:\n");
	RUN_TEST(dlna_features_file_with_profile);
	RUN_TEST(dlna_features_streaming_with_profile);
	RUN_TEST(dlna_features_null_profile);
	RUN_TEST(dlna_features_empty_profile);
	RUN_TEST(dlna_features_op_format_file);
	RUN_TEST(dlna_features_op_format_streaming);
	RUN_TEST(dlna_features_ci_original);
	RUN_TEST(dlna_features_ci_transcoded);
	RUN_TEST(dlna_features_flags_length);
	RUN_TEST(dlna_features_flags_value);
	RUN_TEST(dlna_features_consistency);
	RUN_TEST(dlna_features_field_order);
	RUN_TEST(dlna_features_field_order_no_profile);
	RUN_TEST(dlna_transcode_profile_matches_encoder);
	RUN_TEST(dlna_transcode_profile_hevc);
	RUN_TEST(dlna_transcode_mime_type);
	RUN_TEST(dlna_screen_capture_profile);
	RUN_TEST(dlna_features_no_colons);
	RUN_TEST(dlna_features_no_colons_no_profile);
	RUN_TEST(dlna_features_hevc_streaming);
	RUN_TEST(dlna_mpegts_direct_vs_transcode);

	printf("\nvcodec:\n");
	RUN_TEST(vcodec_default_is_h264);
	RUN_TEST(vcodec_hevc_value);

	printf("\nxml_extract:\n");
	RUN_TEST(xml_extract_basic);
	RUN_TEST(xml_extract_nested);
	RUN_TEST(xml_extract_open_not_found);
	RUN_TEST(xml_extract_close_not_found);
	RUN_TEST(xml_extract_empty_content);
	RUN_TEST(xml_extract_truncation);
	RUN_TEST(xml_extract_first_match);

	printf("\nxml_encode:\n");
	RUN_TEST(xml_encode_plain);
	RUN_TEST(xml_encode_lt_gt);
	RUN_TEST(xml_encode_amp);
	RUN_TEST(xml_encode_quotes);
	RUN_TEST(xml_encode_empty);
	RUN_TEST(xml_encode_all_special);

	printf("\n%d/%d passed", tests_passed, tests_run);
	if (tests_failed > 0)
		printf(", %d FAILED", tests_failed);
	printf("\n");

	return tests_failed > 0 ? 1 : 0;
}
