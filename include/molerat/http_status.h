#ifndef MOLERAT_HTTP_STATUS_H
#define MOLERAT_HTTP_STATUS_H

/* HTTP status message lookup */

struct http_status {
	int code;
	int message_len;
	const char *message;
};

struct http_status *http_status_lookup(int code);

#endif
