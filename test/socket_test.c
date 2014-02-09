#include <stdio.h>
#include <string.h>

#include <molerat/base.h>
#include <molerat/socket.h>
#include <molerat/tasklet.h>
#include <molerat/application.h>
#include <molerat/echo_server.h>

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
			socket_close_write(t->socket, &t->write_err);
			break;
		}

		switch (res = socket_write(t->socket,
					   t->write_buf + t->write_pos,
					   t->write_len - t->write_pos,
					   &t->write_tasklet, &t->write_err)) {
		case STREAM_WAITING:
			goto waiting;

		case STREAM_ERROR:
			goto error;
		}

		t->write_pos += res;
	}

 error:
	tester_stop_1(t, &t->write_tasklet);
 waiting:
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

		switch (res = socket_read(t->socket, t->read_buf + t->read_pos,
					  t->read_capacity - t->read_pos,
					  &t->read_tasklet, &t->read_err)) {
		case STREAM_WAITING:
			goto waiting;

		case STREAM_END:
		case STREAM_ERROR:
			goto done;
		}

		t->read_pos += res;
	}

 done:
	tester_stop_1(t, &t->read_tasklet);
 waiting:
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
	tasklet_goto(&t->write_tasklet, tester_write);

	mutex_lock(&t->mutex);
	tasklet_goto(&t->read_tasklet, tester_read);

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
	if (!error_ok(err))
		die("%s", error_message(err));
}

static void tester_check(struct tester *t)
{
	check_error(&t->write_err);
	check_error(&t->read_err);

	if (t->write_len != t->read_pos
	    || memcmp(t->write_buf, t->read_buf, t->write_len))
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

	s = socket_factory_connect_address(sf, sas[0], &err);
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
