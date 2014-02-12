#include <stdio.h>
#include <string.h>

#include <molerat/base.h>
#include <molerat/socket.h>
#include <molerat/tasklet.h>
#include <molerat/application.h>
#include <molerat/echo_server.h>

#include "stream_utils.h"

struct tester {
	struct mutex mutex;
	struct socket *socket;

	struct tasklet write_tasklet;
	struct stream_pump *write_pump;
	struct error write_err;

	struct tasklet read_tasklet;
	struct growbuf read_buf;
	struct stream_pump *read_pump;
	struct error read_err;

	int stopped;
};

void tester_stop_1(struct tester *t, struct tasklet *tasklet)
{
	tasklet_stop(tasklet);
	if (++t->stopped == 2)
		application_stop();
}

void tester_write(void *v_t)
{
	struct tester *t = v_t;

	switch (stream_pump(t->write_pump, &t->write_tasklet, &t->write_err)) {
	case STREAM_END:
		socket_close_write(t->socket, &t->write_err);
		/* fall through */

	case STREAM_ERROR:
		tester_stop_1(t, &t->write_tasklet);
	}
}

void tester_read(void *v_t)
{
	struct tester *t = v_t;

	switch (stream_pump(t->read_pump, &t->read_tasklet, &t->read_err)) {
	case STREAM_END:
	case STREAM_ERROR:
		tester_stop_1(t, &t->read_tasklet);
	}
}

struct tester *tester_create(struct socket *s)
{
	struct tester *t = xalloc(sizeof *t);
	mutex_init(&t->mutex);
	t->socket = s;
	t->stopped = 0;

	tasklet_init(&t->write_tasklet, &t->mutex, t);
	t->write_pump
		= stream_pump_create(c_string_read_stream_create("Hello"),
				     socket_stream(s), 10);
	error_init(&t->write_err);

	tasklet_init(&t->read_tasklet, &t->mutex, t);
	growbuf_init(&t->read_buf, 10);
	t->read_pump = stream_pump_create(socket_stream(s),
				 growbuf_write_stream_create(&t->read_buf), 10);
	error_init(&t->read_err);

	mutex_lock(&t->mutex);
	tasklet_goto(&t->write_tasklet, tester_write);
	tasklet_goto(&t->read_tasklet, tester_read);
	mutex_unlock(&t->mutex);

	return t;
}

static void tester_destroy(struct tester *t)
{
	mutex_lock(&t->mutex);
	tasklet_fini(&t->write_tasklet);
	tasklet_fini(&t->read_tasklet);
	stream_pump_destroy_with_source(t->write_pump);
	stream_pump_destroy_with_dest(t->read_pump);
	growbuf_fini(&t->read_buf);
	socket_destroy(t->socket);
	error_fini(&t->write_err);
	error_fini(&t->read_err);
	mutex_unlock_fini(&t->mutex);
	free(t);
}

static void check_error(struct error *err)
{
	if (!error_ok(err))
		die("%s", error_message(err));
}

static void tester_check(struct tester *t)
{
	check_error(&t->write_err);
	check_error(&t->read_err);

	if (bytes_compare(c_string_bytes("Hello"),
			  growbuf_to_bytes(&t->read_buf)))
		die("Data returned from echo server did not match data sent");
}

static void test_echo_server(struct echo_server *es, struct socket *s)
{
	struct tester *t = tester_create(s);

	application_run();
	tester_check(t);
	tester_destroy(t);
	echo_server_destroy(es);
}

static void test_echo_direct(void)
{
	struct error err;
	struct socket_factory *sf = socket_factory();
	struct server_socket *ss;
	struct echo_server *es;
	struct sockaddr **sas;
	struct socket *s;

	error_init(&err);

	ss = socket_factory_unbound_server_socket(sf, &err);
	check_error(&err);

	es = echo_server_create(ss, 0);
	sas = echo_server_addresses(es, &err);
	check_error(&err);

	s = socket_factory_connect_addresses(sf, sas, &err);
	check_error(&err);

	test_echo_server(es, s);

	free_sockaddrs(sas);
	error_fini(&err);
}

static void test_echo_gai(const char *bind_host, const char *connect_host,
			  const char *service)
{
	struct error err;
	struct socket_factory *sf = socket_factory();
	struct server_socket *ss;
	struct echo_server *es;
	struct socket *s;

	error_init(&err);

	ss = socket_factory_bound_server_socket(sf, bind_host, service, &err);
	check_error(&err);

	es = echo_server_create(ss, 0);
	s = socket_factory_connect(sf, connect_host, service, &err);
	check_error(&err);

	test_echo_server(es, s);

	error_fini(&err);
}

static void test_connect_failure(void)
{
	struct error err;
	struct socket_factory *sf = socket_factory();
	struct socket *s;
	struct tester *t;

	error_init(&err);

	s = socket_factory_connect(sf, "127.0.0.1", "9997", &err);
	check_error(&err);

	t = tester_create(s);
	application_run();
	assert(!error_ok(&t->write_err));
	assert(strstr(error_message(&t->write_err), "Connection refused"));
	assert(!error_ok(&t->read_err));
	assert(strstr(error_message(&t->read_err), "Connection refused"));
	tester_destroy(t);
	error_fini(&err);
}

void test_gai_failure(void)
{
	struct error err;
	struct socket_factory *sf = socket_factory();
	struct socket *s;

	error_init(&err);

	s = socket_factory_connect(sf, "nosuch.invalid.", "6666", &err);
	assert(!s);
	assert(strstr(error_message(&err), "not known"));

	error_fini(&err);
}

int main(void)
{
	application_prepare_test();
	test_echo_direct();
	test_echo_gai("localhost", "localhost", "9998");
	test_echo_gai(NULL, "localhost", "9999");
	test_connect_failure();
	test_gai_failure();
	return 0;
}
