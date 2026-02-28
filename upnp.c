#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

#include "send2tv.h"

#define SSDP_ADDR	"239.255.255.250"
#define SSDP_PORT	1900
#define SSDP_MX		3

/* Known Samsung DMR endpoints to try */
static const struct {
	int	 port;
	const char *path;
} dmr_endpoints[] = {
	{ 9197, "/dmr" },
	{ 7676, "/dmr" },
	{ 8001, "/dmr" },
	{ 9197, "/dmr/SamsungMRDesc.xml" },
	{ 7676, "/xml/device_description.xml" },
	{ 0, NULL }
};

/*
 * Send all bytes to a socket, handling partial writes.
 * Returns 0 on success, -1 on error.
 */
static int
http_send_all(int sock, const char *buf, int len)
{
	ssize_t	 n;

	while (len > 0) {
		n = send(sock, buf, len, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += n;
		len -= n;
	}
	return 0;
}

/*
 * Simple HTTP request over a TCP socket. Returns malloc'd response body.
 * Caller must free. Sets *resp_len to body length. Returns NULL on error.
 */
static char *
http_request(const char *host, int port, const char *method, const char *path,
    const char *extra_headers, const char *body, int *resp_len)
{
	struct sockaddr_in	 addr;
	struct hostent		*he;
	int			 sock, n, hdr_end;
	char			 req[SEND2TV_SOAP_BUF];
	char			*buf = NULL;
	int			 buf_sz = 0, buf_len = 0;
	char			*body_start;

	he = gethostbyname(host);
	if (he == NULL)
		return NULL;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return NULL;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	memcpy(&addr.sin_addr, he->h_addr, he->h_length);

	{
		int ret;

		/* set timeouts for connect */
		struct timeval tv;
		tv.tv_sec = 10;
		tv.tv_usec = 0;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
		if (ret < 0) {
			close(sock);
			return NULL;
		}
	}

	/* Build request */
	if (body != NULL)
		n = snprintf(req, sizeof(req),
		    "%s %s HTTP/1.1\r\n"
		    "Host: %s:%d\r\n"
		    "Content-Length: %zu\r\n"
		    "%s"
		    "Connection: close\r\n"
		    "\r\n"
		    "%s",
		    method, path, host, port, strlen(body),
		    extra_headers ? extra_headers : "",
		    body);
	else
		n = snprintf(req, sizeof(req),
		    "%s %s HTTP/1.1\r\n"
		    "Host: %s:%d\r\n"
		    "%s"
		    "Connection: close\r\n"
		    "\r\n",
		    method, path, host, port,
		    extra_headers ? extra_headers : "");

	/* Guard against snprintf truncation */
	if (n >= (int)sizeof(req)) {
		DPRINTF("http: request too large (%d > %zu)\n",
		    n, sizeof(req));
		close(sock);
		return NULL;
	}

	DPRINTF("http: %s %s:%d%s\n", method, host, port, path);

	if (http_send_all(sock, req, n) < 0) {
		close(sock);
		return NULL;
	}

	/* Read response */
	buf_sz = 4096;
	buf = malloc(buf_sz);
	if (buf == NULL) {
		close(sock);
		return NULL;
	}

	while ((n = recv(sock, buf + buf_len, buf_sz - buf_len - 1, 0)) > 0) {
		buf_len += n;
		if (buf_len >= buf_sz - 1) {
			buf_sz *= 2;
			buf = realloc(buf, buf_sz);
			if (buf == NULL) {
				close(sock);
				return NULL;
			}
		}
	}
	if (n < 0)
		DPRINTF("http: recv error: %s\n", strerror(errno));
	buf[buf_len] = '\0';
	close(sock);

	DPRINTF("http: got %d bytes from %s:%d\n", buf_len, host, port);

	/* Find body (after \r\n\r\n) */
	body_start = strstr(buf, "\r\n\r\n");
	if (body_start == NULL) {
		free(buf);
		return NULL;
	}
	body_start += 4;
	hdr_end = body_start - buf;

	/* Move body to start of buffer */
	*resp_len = buf_len - hdr_end;
	memmove(buf, body_start, *resp_len);
	buf[*resp_len] = '\0';

	return buf;
}

/*
 * Extract text between open_tag and close_tag from xml.
 * Returns 0 on success, -1 on failure.
 */
static int
xml_extract(const char *xml, const char *open_tag, const char *close_tag,
    char *out, size_t out_sz)
{
	const char	*start, *end;
	size_t		 len;

	start = strstr(xml, open_tag);
	if (start == NULL)
		return -1;
	start += strlen(open_tag);

	end = strstr(start, close_tag);
	if (end == NULL)
		return -1;

	len = end - start;
	if (len >= out_sz)
		len = out_sz - 1;

	memcpy(out, start, len);
	out[len] = '\0';
	return 0;
}

/*
 * SSDP discovery: find MediaRenderer devices on the network.
 * Prints discovered devices to stdout. Returns number found.
 */
int
upnp_discover(void)
{
	int			 sock, n, found = 0;
	struct sockaddr_in	 mcast_addr, from_addr;
	socklen_t		 from_len;
	struct pollfd		 pfd;
	char			 buf[4096];
	const char		*msearch =
	    "M-SEARCH * HTTP/1.1\r\n"
	    "HOST: 239.255.255.250:1900\r\n"
	    "MAN: \"ssdp:discover\"\r\n"
	    "MX: 3\r\n"
	    "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
	    "\r\n";

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("socket");
		return 0;
	}

	/* Allow reuse */
	n = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &n, sizeof(n));

	memset(&mcast_addr, 0, sizeof(mcast_addr));
	mcast_addr.sin_family = AF_INET;
	mcast_addr.sin_port = htons(SSDP_PORT);
	inet_pton(AF_INET, SSDP_ADDR, &mcast_addr.sin_addr);

	/* Send M-SEARCH */
	n = sendto(sock, msearch, strlen(msearch), 0,
	    (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));
	if (n < 0) {
		perror("sendto");
		close(sock);
		return 0;
	}

	DPRINTF("ssdp: sent M-SEARCH to %s:%d\n", SSDP_ADDR, SSDP_PORT);
	printf("Searching for TVs...\n");

	/* Collect responses for SSDP_MX + 1 seconds */
	pfd.fd = sock;
	pfd.events = POLLIN;

	while (poll(&pfd, 1, (SSDP_MX + 1) * 1000) > 0) {
		from_len = sizeof(from_addr);
		n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
		    (struct sockaddr *)&from_addr, &from_len);
		if (n <= 0)
			break;
		buf[n] = '\0';

		/* Extract LOCATION header */
		char *loc = strcasestr(buf, "LOCATION:");
		if (loc == NULL)
			continue;
		loc += 9;
		while (*loc == ' ')
			loc++;
		char *loc_end = strstr(loc, "\r\n");
		if (loc_end == NULL)
			continue;
		*loc_end = '\0';

		DPRINTF("ssdp: response LOCATION: %s\n", loc);

		/* Parse host and port from LOCATION URL */
		char loc_host[64] = {0};
		int loc_port = 80;
		char loc_path[256] = "/";
		if (sscanf(loc, "http://%63[^:/]:%d%255s",
		    loc_host, &loc_port, loc_path) < 1)
			if (sscanf(loc, "http://%63[^:/]%255s",
			    loc_host, loc_path) < 1)
				continue;

		/* Fetch device description */
		int resp_len;
		char *desc = http_request(loc_host, loc_port, "GET",
		    loc_path, NULL, NULL, &resp_len);
		if (desc == NULL)
			continue;

		char friendly[128] = "Unknown";
		char model[128] = "";
		xml_extract(desc, "<friendlyName>", "</friendlyName>",
		    friendly, sizeof(friendly));
		xml_extract(desc, "<modelName>", "</modelName>",
		    model, sizeof(model));
		free(desc);

		char ip_str[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &from_addr.sin_addr,
		    ip_str, sizeof(ip_str));

		DPRINTF("ssdp: %s model=%s\n", friendly, model);
		printf("  %-16s %s", ip_str, friendly);
		if (model[0] != '\0')
			printf(" (%s)", model);
		printf("\n");
		found++;
	}

	close(sock);

	if (found == 0)
		printf("No devices found.\n");

	return found;
}

/*
 * Fetch the TV's device description and find the AVTransport control URL.
 * Populates ctx->tv_port and ctx->control_url.
 * Returns 0 on success, -1 on failure.
 */
int
upnp_find_transport(upnp_ctx_t *ctx)
{
	int	 i, resp_len;
	char	*desc = NULL;
	char	*avt_start;
	char	 ctrl_url[254];

	for (i = 0; dmr_endpoints[i].path != NULL; i++) {
		DPRINTF("upnp: trying %s:%d%s\n", ctx->tv_ip,
		    dmr_endpoints[i].port, dmr_endpoints[i].path);
		desc = http_request(ctx->tv_ip, dmr_endpoints[i].port,
		    "GET", dmr_endpoints[i].path, NULL, NULL, &resp_len);
		if (desc != NULL && resp_len > 0 &&
		    strstr(desc, "AVTransport") != NULL) {
			ctx->tv_port = dmr_endpoints[i].port;
			break;
		}
		free(desc);
		desc = NULL;
	}

	if (desc == NULL) {
		fprintf(stderr, "Cannot reach TV at %s. Is it turned on?\n",
		    ctx->tv_ip);
		return -1;
	}

	/*
	 * Find the AVTransport service block and extract controlURL.
	 * The XML has multiple <service> blocks; we need the one
	 * containing "AVTransport".
	 */
	avt_start = strstr(desc, "AVTransport");
	if (avt_start == NULL) {
		fprintf(stderr, "TV does not support AVTransport\n");
		free(desc);
		return -1;
	}

	if (xml_extract(avt_start, "<controlURL>", "</controlURL>",
	    ctrl_url, sizeof(ctrl_url)) < 0) {
		fprintf(stderr, "Cannot find AVTransport controlURL\n");
		free(desc);
		return -1;
	}

	/* Ensure it starts with / */
	if (ctrl_url[0] == '/')
		strlcpy(ctx->control_url, ctrl_url,
		    sizeof(ctx->control_url));
	else
		snprintf(ctx->control_url, sizeof(ctx->control_url),
		    "/%s", ctrl_url);

	DPRINTF("upnp: AVTransport at %s:%d%s\n", ctx->tv_ip,
	    ctx->tv_port, ctx->control_url);

	free(desc);
	return 0;
}

/*
 * XML-encode a string (escape <, >, &, ", ').
 * Returns malloc'd string. Caller frees.
 */
static char *
xml_encode(const char *s)
{
	size_t	 len = 0;
	const char *p;
	char	*out, *o;

	for (p = s; *p; p++) {
		switch (*p) {
		case '<':  len += 4; break;
		case '>':  len += 4; break;
		case '&':  len += 5; break;
		case '"':  len += 6; break;
		case '\'': len += 6; break;
		default:   len += 1; break;
		}
	}

	out = malloc(len + 1);
	if (out == NULL)
		return NULL;

	o = out;
	for (p = s; *p; p++) {
		switch (*p) {
		case '<':  memcpy(o, "&lt;", 4); o += 4; break;
		case '>':  memcpy(o, "&gt;", 4); o += 4; break;
		case '&':  memcpy(o, "&amp;", 5); o += 5; break;
		case '"':  memcpy(o, "&quot;", 6); o += 6; break;
		case '\'': memcpy(o, "&apos;", 6); o += 6; break;
		default:   *o++ = *p; break;
		}
	}
	*o = '\0';
	return out;
}

/*
 * Send a SOAP action to the TV's AVTransport service.
 * Retries up to 2 times on no-response (Samsung TVs sometimes close
 * the connection before replying, especially for SetAVTransportURI).
 * Returns 0 on success, -1 on failure.
 */
static int
soap_action(upnp_ctx_t *ctx, const char *action, const char *body_xml)
{
	char	 headers[512];
	char	 envelope[SEND2TV_SOAP_BUF];
	char	*resp;
	int	 resp_len;
	int	 attempts;

	snprintf(headers, sizeof(headers),
	    "Content-Type: text/xml; charset=\"utf-8\"\r\n"
	    "SOAPAction: \"urn:schemas-upnp-org:service:AVTransport:1#%s\"\r\n",
	    action);

	snprintf(envelope, sizeof(envelope),
	    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
	    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
	    " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
	    "  <s:Body>\r\n"
	    "    %s\r\n"
	    "  </s:Body>\r\n"
	    "</s:Envelope>",
	    body_xml);

	DPRINTF("soap: %s -> %s:%d%s\n", action, ctx->tv_ip,
	    ctx->tv_port, ctx->control_url);

	resp = NULL;
	for (attempts = 0; attempts < 3; attempts++) {
		if (attempts > 0) {
			DPRINTF("soap: %s retry %d\n", action, attempts);
			sleep(1);
		}
		resp = http_request(ctx->tv_ip, ctx->tv_port, "POST",
		    ctx->control_url, headers, envelope, &resp_len);
		if (resp != NULL)
			break;
	}

	if (resp == NULL) {
		fprintf(stderr, "SOAP %s failed: no response\n", action);
		return -1;
	}

	/* Check for SOAP fault */
	if (strstr(resp, "Fault") != NULL) {
		fprintf(stderr, "SOAP %s fault\n", action);
		DPRINTF("soap: response: %.*s\n", resp_len, resp);
		free(resp);
		return -1;
	}

	free(resp);
	return 0;
}

/*
 * Send a SOAP action and return the response body.
 * Caller must free the returned buffer.
 * Returns NULL on error or SOAP fault.
 */
static char *
soap_action_resp(upnp_ctx_t *ctx, const char *action, const char *body_xml)
{
	char	 headers[512];
	char	 envelope[SEND2TV_SOAP_BUF];
	char	*resp;
	int	 resp_len;

	snprintf(headers, sizeof(headers),
	    "Content-Type: text/xml; charset=\"utf-8\"\r\n"
	    "SOAPAction: \"urn:schemas-upnp-org:service:AVTransport:1#%s\"\r\n",
	    action);

	snprintf(envelope, sizeof(envelope),
	    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
	    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
	    " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
	    "  <s:Body>\r\n"
	    "    %s\r\n"
	    "  </s:Body>\r\n"
	    "</s:Envelope>",
	    body_xml);

	DPRINTF("soap: %s -> %s:%d%s\n", action, ctx->tv_ip,
	    ctx->tv_port, ctx->control_url);

	resp = http_request(ctx->tv_ip, ctx->tv_port, "POST",
	    ctx->control_url, headers, envelope, &resp_len);
	if (resp == NULL) {
		fprintf(stderr, "SOAP %s failed: no response\n", action);
		return NULL;
	}

	if (strstr(resp, "Fault") != NULL) {
		fprintf(stderr, "SOAP %s fault\n", action);
		DPRINTF("soap: response: %.*s\n", resp_len, resp);
		free(resp);
		return NULL;
	}

	return resp;
}

/*
 * SetAVTransportURI with DIDL-Lite metadata.
 */
int
upnp_set_uri(upnp_ctx_t *ctx, const char *uri, const char *mime,
    const char *title, int is_streaming, const char *dlna_profile)
{
	char	 didl[2048];
	char	*didl_encoded;
	char	 body[4096];
	char	 dlna_features[256];
	char	*title_xml, *uri_xml;
	int	 ret;

	title_xml = xml_encode(title);
	uri_xml = xml_encode(uri);
	if (title_xml == NULL || uri_xml == NULL) {
		free(title_xml);
		free(uri_xml);
		return -1;
	}

	build_dlna_features(dlna_features, sizeof(dlna_features),
	    dlna_profile, is_streaming);

	snprintf(didl, sizeof(didl),
	    "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\""
	    " xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\""
	    " xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
	    "<item id=\"0\" parentID=\"0\" restricted=\"0\">"
	    "<dc:title>%s</dc:title>"
	    "<upnp:class>object.item.videoItem</upnp:class>"
	    "<res protocolInfo=\"http-get:*:%s:"
	    "%s\">%s</res>"
	    "</item>"
	    "</DIDL-Lite>",
	    title_xml, mime, dlna_features, uri_xml);

	didl_encoded = xml_encode(didl);
	if (didl_encoded == NULL) {
		free(title_xml);
		free(uri_xml);
		return -1;
	}

	snprintf(body, sizeof(body),
	    "<u:SetAVTransportURI"
	    " xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
	    "<InstanceID>0</InstanceID>"
	    "<CurrentURI>%s</CurrentURI>"
	    "<CurrentURIMetaData>%s</CurrentURIMetaData>"
	    "</u:SetAVTransportURI>",
	    uri_xml, didl_encoded);

	free(title_xml);
	free(uri_xml);
	free(didl_encoded);

	ret = soap_action(ctx, "SetAVTransportURI", body);
	return ret;
}

int
upnp_play(upnp_ctx_t *ctx)
{
	return soap_action(ctx, "Play",
	    "<u:Play"
	    " xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
	    "<InstanceID>0</InstanceID>"
	    "<Speed>1</Speed>"
	    "</u:Play>");
}

int
upnp_stop(upnp_ctx_t *ctx)
{
	return soap_action(ctx, "Stop",
	    "<u:Stop"
	    " xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
	    "<InstanceID>0</InstanceID>"
	    "</u:Stop>");
}

/*
 * Get the current playback position from the TV.
 * Writes the position in seconds to *pos_sec.
 * Returns 0 on success, -1 on failure.
 */
int
upnp_get_position(upnp_ctx_t *ctx, int *pos_sec)
{
	char	*resp;
	char	 reltime[32];
	int	 h, m, s;

	resp = soap_action_resp(ctx, "GetPositionInfo",
	    "<u:GetPositionInfo"
	    " xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
	    "<InstanceID>0</InstanceID>"
	    "</u:GetPositionInfo>");
	if (resp == NULL)
		return -1;

	if (xml_extract(resp, "<RelTime>", "</RelTime>",
	    reltime, sizeof(reltime)) < 0) {
		free(resp);
		return -1;
	}
	free(resp);

	if (sscanf(reltime, "%d:%d:%d", &h, &m, &s) != 3)
		return -1;

	*pos_sec = h * 3600 + m * 60 + s;
	return 0;
}

/*
 * Seek to an absolute position (in seconds from start).
 * Returns 0 on success, -1 on failure.
 */
int
upnp_seek(upnp_ctx_t *ctx, int target_sec)
{
	char	 body[512];
	int	 h, m, s;

	if (target_sec < 0)
		target_sec = 0;

	h = target_sec / 3600;
	m = (target_sec % 3600) / 60;
	s = target_sec % 60;

	snprintf(body, sizeof(body),
	    "<u:Seek"
	    " xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
	    "<InstanceID>0</InstanceID>"
	    "<Unit>REL_TIME</Unit>"
	    "<Target>%d:%02d:%02d</Target>"
	    "</u:Seek>",
	    h, m, s);

	return soap_action(ctx, "Seek", body);
}

/*
 * Seek relative to current position by delta_sec seconds.
 * Negative values seek backward. Clamps to 0 on the low end.
 * Returns 0 on success, -1 on failure.
 */
int
upnp_seek_relative(upnp_ctx_t *ctx, int delta_sec)
{
	int	 pos;

	if (upnp_get_position(ctx, &pos) < 0)
		return -1;

	DPRINTF("seek: position=%d, delta=%d\n", pos, delta_sec);

	pos += delta_sec;
	if (pos < 0)
		pos = 0;

	return upnp_seek(ctx, pos);
}

/*
 * Determine the local IP address that can reach the TV.
 * Uses the UDP connect+getsockname trick.
 */
int
upnp_get_local_ip(upnp_ctx_t *ctx)
{
	int			 sock;
	struct sockaddr_in	 dest, local;
	socklen_t		 len;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("socket");
		return -1;
	}

	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(9197);
	inet_pton(AF_INET, ctx->tv_ip, &dest.sin_addr);

	if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
		perror("connect");
		close(sock);
		return -1;
	}

	len = sizeof(local);
	if (getsockname(sock, (struct sockaddr *)&local, &len) < 0) {
		perror("getsockname");
		close(sock);
		return -1;
	}

	inet_ntop(AF_INET, &local.sin_addr,
	    ctx->local_ip, sizeof(ctx->local_ip));
	close(sock);
	return 0;
}
