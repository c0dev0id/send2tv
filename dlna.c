#include <stdio.h>
#include <string.h>

#include "send2tv.h"

/*
 * Build DLNA.ORG content features string for the fourth field of
 * protocolInfo (used in both DIDL-Lite metadata and HTTP headers).
 *
 * DLNA.ORG_PN:    profile name (omitted when dlna_profile is NULL/empty)
 * DLNA.ORG_OP:    "ab" where a=time-seek, b=byte-seek (each 0 or 1)
 * DLNA.ORG_CI:    conversion indicator (0=original, 1=transcoded)
 * DLNA.ORG_FLAGS: 32-hex-char primary+reserved flags
 */
void
build_dlna_features(char *buf, size_t buflen, const char *dlna_profile,
    int is_streaming)
{
	if (dlna_profile != NULL && dlna_profile[0] != '\0')
		snprintf(buf, buflen,
		    "DLNA.ORG_PN=%s;DLNA.ORG_OP=%s;DLNA.ORG_CI=%s;"
		    "DLNA.ORG_FLAGS="
		    "%s",
		    dlna_profile,
		    is_streaming ? "00" : "01",
		    is_streaming ? "1" : "0",
		    is_streaming ? "01700000000000000000000000000000"
		                 : "21700000000000000000000000000000");
	else
		snprintf(buf, buflen,
		    "DLNA.ORG_OP=%s;DLNA.ORG_CI=%s;"
		    "DLNA.ORG_FLAGS="
		    "%s",
		    is_streaming ? "00" : "01",
		    is_streaming ? "1" : "0",
		    is_streaming ? "01700000000000000000000000000000"
		                 : "21700000000000000000000000000000");
}
