#include <stdio.h>
#include <string.h>

#include <molerat/base.h>

#define DEBUG 0

struct status {
	int code;
	const char *message;
};

static struct status codes[] = {
	{ 100, "Continue" },
	{ 101, "Switching Protocols" },
	{ 102, "Processing" },
	{ 200, "OK" },
	{ 201, "Created" },
	{ 202, "Accepted" },
	{ 203, "Non-Authoritative Information" },
	{ 204, "No Content" },
	{ 205, "Reset Content" },
	{ 206, "Partial Content" },
	{ 207, "Multi-Status" },
	{ 208, "Already Reported" },
	{ 226, "IM Used" },
	{ 300, "Multiple Choices" },
	{ 301, "Moved Permanently" },
	{ 302, "Found" },
	{ 303, "See Other" },
	{ 304, "Not Modified" },
	{ 305, "Use Proxy" },
	{ 306, "Switch Proxy" },
	{ 307, "Temporary Redirect" },
	{ 308, "Permanent Redirect" },
	{ 400, "Bad Request" },
	{ 401, "Unauthorized" },
	{ 402, "Payment Required" },
	{ 403, "Forbidden" },
	{ 404, "Not Found" },
	{ 405, "Method Not Allowed" },
	{ 406, "Not Acceptable" },
	{ 407, "Proxy Authentication Required" },
	{ 408, "Request Timeout" },
	{ 409, "Conflict" },
	{ 410, "Gone" },
	{ 411, "Length Required" },
	{ 412, "Precondition Failed" },
	{ 413, "Request Entity Too Large" },
	{ 414, "Request-URI Too Long" },
	{ 415, "Unsupported Media Type" },
	{ 416, "Requested Range Not Satisfiable" },
	{ 417, "Expectation Failed" },
	{ 418, "I'm a teapot" },
	{ 419, "Authentication Timeout" },
	{ 422, "Unprocessable Entity" },
	{ 423, "Locked" },
	{ 424, "Failed Dependency" },
	{ 425, "Unordered Collection" },
	{ 426, "Upgrade Required" },
	{ 428, "Precondition Required" },
	{ 429, "Too Many Requests" },
	{ 431, "Request Header Fields Too Large" },
	{ 451, "Unavailable For Legal Reasons" },
	{ 500, "Internal Server Error" },
	{ 501, "Not Implemented" },
	{ 502, "Bad Gateway" },
	{ 503, "Service Unavailable" },
	{ 504, "Gateway Timeout" },
	{ 505, "HTTP Version Not Supported" },
	{ 506, "Variant Also Negotiates" },
	{ 507, "Insufficient Storage" },
	{ 508, "Loop Detected" },
	{ 509, "Bandwidth Limit Exceeded" },
	{ 510, "Not Extended" },
	{ 511, "Network Authentication Required" },
	{ 522, "Connection timed out" },
};

#define N_CODES (sizeof codes / sizeof(struct status))

int search(unsigned int table_size)
{
	unsigned int mult;
	unsigned int i, hash;
	unsigned int mask = table_size - 1;
	unsigned int *hits = xalloc(table_size * sizeof *hits);

	memset(hits, 0, table_size * sizeof *hits);

	for (mult = 1; mult < table_size << 10; mult += 2) {
		for (i = 0;; i++) {
			if (i == N_CODES)
				/* Success */
				goto out;

			hash = ((codes[i].code * mult) >> 10) & mask;
			if (hits[hash] == mult)
				/* Collision */
				break;

			hits[hash] = mult;
		}

		if (DEBUG)
			fprintf(stderr, "%d ", codes[i].code);
	}

	mult = -1;

 out:
	free(hits);

	if (DEBUG)
		fprintf(stderr, "\n");

	return mult;
}

void dump(unsigned int table_size, unsigned int mult)
{
	unsigned char *table = xalloc(table_size * sizeof *table);
	unsigned int i;
	unsigned int mask = table_size - 1;

	memset(table, 255, table_size * sizeof *table);

	for (i = 0; i < N_CODES; i++)
		table[((codes[i].code * mult) >> 10) & mask] = i;

	printf("#include <molerat/http_status.h>\n\n");

	printf("static unsigned char table[] = { ");
	for (i = 0; i < table_size; i++)
		printf("%d, ", (int)table[i]);
	printf("};\n\n");

	printf("static struct http_status statuses[] = {\n");
	for (i = 0; i < N_CODES; i++)
		printf("\t{ %d, %d, \"HTTP/1.1 %d %s\\r\\n\" },\n",
		       codes[i].code, (int)strlen(codes[i].message) + 15,
		       codes[i].code, codes[i].message);
	printf("};\n\n");

	printf("struct http_status *http_status_lookup(int code)\n"
	       "{\n"
	       "\tunsigned int n = table[((code * %d) >> 10) & %d];\n"
	       "\tif (n != 255 && statuses[n].code == code)\n"
	       "\t\treturn &statuses[n];\n"
	       "\telse\n"
	       "\t\treturn 0;\n"
	       "}\n",
	       mult, table_size - 1);
}

unsigned int power_of_2(unsigned int n)
{
	/* The algorithm naturally rounds down, we want to round up. */
	n--;

	for (;;) {
		unsigned int one_less_bit = n & (n - 1);
		if (!one_less_bit)
			return n << 1;

		n = one_less_bit;
	}
}

int main(void)
{
	unsigned int table_size;

	for (table_size = power_of_2(N_CODES);
	     table_size < 600 * 2;
	     table_size <<= 1) {
		int mult;

		if (DEBUG)
			fprintf(stderr, "Table size %u:\n", table_size);

		mult = search(table_size);
		if (mult >= 0) {
			dump(table_size, mult);
			return 0;
		}
	}

	fprintf(stderr, "Could not construct HTTP status code table.\n");
	return 1;
}
