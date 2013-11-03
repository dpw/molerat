#include <stdio.h>

#include "base.h"
#include "socket.h"
#include "tasklet.h"
#include "application.h"
#include "echo_server.h"

int main(int argc, char **argv)
{
	struct error err;
	struct socket_factory *sf = socket_factory();
	struct server_socket *ss;
	struct echo_server *es = NULL;
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

	application_prepare();
	es = echo_server_create(ss, 1, &err);
	if (!es)
		goto out;

	application_run();
	echo_server_destroy(es);

 out:
	res = !error_ok(&err);
	if (res) {
		fprintf(stderr, "%s\n", error_message(&err));
		res = 1;
	}

	error_fini(&err);
	return res;
}
