#include <stdio.h>

#include "socket.h"
#include "http_server.h"

int main(int argc, char **argv)
{
	struct error err;
	struct socket_factory *sf = socket_factory();
	struct server_socket *ss;
	struct http_server *hs = NULL;
	int res = 0;

	error_init(&err);

	switch (argc) {
	case 2:
		ss = socket_factory_bound_server_socket(sf, NULL, argv[1],
							&err);
		break;

	case 3:
		ss = socket_factory_bound_server_socket(sf, argv[1], argv[2],
							&err);
		break;

	default:
		error_set(&err, ERROR_MISC,
			  "usage: %s [<host>] <service>", argv[0]);
	}

	if (!error_ok(&err))
		goto out;

	hs = http_server_create(ss, 1, &err);
	if (!hs)
		goto out;

	socket_factory_run(&err);
	http_server_destroy(hs);

 out:
	res = !error_ok(&err);
	if (res) {
		fprintf(stderr, "%s\n", error_message(&err));
		res = 1;
	}

	error_fini(&err);
	return res;
}
