#include <stdio.h>
#include <string.h>

#include "base.h"
#include "socket.h"
#include "tasklet.h"
#include "application.h"
#include "echo_server.h"

struct tester {
	struct mutex mutex;
	struct socket *socket;

	struct tasklet write_tasklet;
	char *write_buf;
	size_t write_len;
	size_t write_pos;
	struct error write_err;

	struct tasklet read_tasklet;
	char *read_buf;
	size_t read_pos;
	size_t read_capacity;
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
	ssize_t res;

	for (;;) {
		if (t->write_pos == t->write_len) {
			socket_partial_close(t->socket, 1, 0, &t->write_err);
			tester_stop_1(t, &t->write_tasklet);
			break;
		}

		res = socket_write(t->socket, t->write_buf + t->write_pos,
				   t->write_len - t->write_pos,
				   &t->write_tasklet,
				   &t->write_err);
		if (res < 0)
			break;

		t->write_pos += res;
	}

	if (!error_ok(&t->write_err))
		tester_stop_1(t, &t->write_tasklet);

	mutex_unlock(&t->mutex);
}

void tester_read(void *v_t)
{
	struct tester*t = v_t;
	ssize_t res;

	for (;;) {
		if (t->read_pos == t->read_capacity) {
			t->read_capacity = t->read_capacity * 2 + 100;
			t->read_buf = xrealloc(t->read_buf, t->read_capacity);
		}

		res = socket_read(t->socket, t->read_buf + t->read_pos,
				  t->read_capacity - t->read_pos,
				  &t->read_tasklet, &t->read_err);
		if (res <= 0) {
			if (res == 0)
				tester_stop_1(t, &t->read_tasklet);

			break;
		}

		t->read_pos += res;
	}

	if (!error_ok(&t->read_err))
		tester_stop_1(t, &t->read_tasklet);

	mutex_unlock(&t->mutex);
}

struct tester *tester_create(struct socket *s)
{
	struct tester *t = xalloc(sizeof *t);
	mutex_init(&t->mutex);
	t->socket = s;
	t->stopped = 0;

	tasklet_init(&t->write_tasklet, &t->mutex, t);
	t->write_buf = "Hello";
	t->write_len = 5;
	t->write_pos = 0;
	error_init(&t->write_err);

	tasklet_init(&t->read_tasklet, &t->mutex, t);
	t->read_buf = NULL;
	t->read_pos = 0;
	t->read_capacity = 0;
	error_init(&t->read_err);

	mutex_lock(&t->mutex);
	tasklet_now(&t->write_tasklet, tester_write);

	mutex_lock(&t->mutex);
	tasklet_now(&t->read_tasklet, tester_read);

	return t;
}

static void tester_destroy(struct tester *t)
{
	mutex_lock(&t->mutex);
	tasklet_fini(&t->write_tasklet);
	tasklet_fini(&t->read_tasklet);
	socket_destroy(t->socket);
	error_fini(&t->write_err);
	error_fini(&t->read_err);
	free(t->read_buf);
	mutex_unlock_fini(&t->mutex);
	free(t);
}

static void check_error(struct error *err)
{
	if (!error_ok(err)) {
		fprintf(stderr, "%s\n", error_message(err));
		abort();
	}
}

static void tester_check(struct tester *t)
{
	check_error(&t->write_err);
	check_error(&t->read_err);

	if (t->write_len != t->read_pos
	    || memcmp(t->write_buf, t->read_buf, t->write_len)) {
		fprintf(stderr, "Data returned from echo server"
			" did not match data sent\n");
		abort();
	}
}

static void test_echo_server(struct socket_factory *sf,
			     struct echo_server *es,
			     struct socket *s)
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

	es = echo_server_create(ss, 0, &err);
	check_error(&err);

	sas = echo_server_addresses(es, &err);
	check_error(&err);

	s = socket_factory_connect_address(sf, sas[0], &err);
	check_error(&err);

	test_echo_server(sf, es, s);

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

	es = echo_server_create(ss, 0, &err);
	check_error(&err);

	s = socket_factory_connect(sf, connect_host, service, &err);
	check_error(&err);

	test_echo_server(sf, es, s);

	error_fini(&err);
}

static void test_connect_failure(void)
{
	struct error err;
	struct socket_factory *sf = socket_factory();
	struct socket *s;
	struct tester *t;

	error_init(&err);

	s = socket_factory_connect(sf, "127.0.0.1", "9998", &err);
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
	test_echo_direct();
	test_echo_gai("localhost", "localhost", "9999");
	test_echo_gai(NULL, "localhost", "9999");
	test_connect_failure();
	test_gai_failure();
	return 0;
}
