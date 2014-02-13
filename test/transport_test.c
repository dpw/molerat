#include <molerat/tasklet.h>
#include <molerat/socket.h>
#include <molerat/socket_transport.h>
#include <molerat/application.h>

#include "stream_utils.h"

#define MESSAGES 100

struct receiver {
	struct mutex mutex;
	struct error err;
	int received;
	struct async_server *server;
};

struct incoming {
	struct receiver *receiver;
	struct mutex mutex;
	struct tasklet tasklet;
	struct growbuf buf;
	struct error err;
	struct stream_pump *pump;
};

static void read_message(void *v_i)
{
	struct incoming *i = v_i;
	struct receiver *r;
	ssize_t res;

	switch (res = stream_pump(i->pump, &i->tasklet, &i->err)) {
	case STREAM_END:
		break;

	case STREAM_ERROR:
		die("%s", error_message(&i->err));

	default:
		return;
	}

	if (bytes_compare(c_string_bytes("Hello"),
			  growbuf_to_bytes(&i->buf)))
		die("Message did not contain expected data");

	r = i->receiver;
	stream_pump_destroy_with_streams(i->pump);
	error_fini(&i->err);
	growbuf_fini(&i->buf);
	tasklet_fini(&i->tasklet);
	mutex_unlock_fini(&i->mutex);
	free(i);

	mutex_lock(&r->mutex);

	if (++r->received == MESSAGES)
		application_stop();

	mutex_unlock(&r->mutex);
}

static void handler(struct stream *input, void *v_r)
{
	struct incoming *i = xalloc(sizeof *i);

	i->receiver = v_r;
	mutex_init(&i->mutex);
	tasklet_init(&i->tasklet, &i->mutex, i);
	growbuf_init(&i->buf, 10);
	i->pump = stream_pump_create(input,
				     growbuf_write_stream_create(&i->buf), 10);
	error_init(&i->err);

	tasklet_later(&i->tasklet, read_message);
}

static void receiver_init(struct receiver *r, struct async_transport *t)
{
	mutex_init(&r->mutex);
	error_init(&r->err);
	r->received = 0;
	r->server = async_transport_serve(t, handler, r, &r->err);
	if (!r->server)
		die("%s", error_message(&r->err));
}

static void receiver_fini(struct receiver *r)
{
	async_server_destroy(r->server);
	error_fini(&r->err);
	mutex_fini(&r->mutex);
}

static struct address *receiver_address(struct receiver *r)
{
	struct address *a = async_server_address(r->server, &r->err);
	if (!a)
		die("%s", error_message(&r->err));

	return a;
}

struct sender {
	struct async_transport *transport;
	struct address *address;
	struct mutex mutex;
	struct tasklet tasklet;
	struct error err;
	struct stream_pump *pump;
};

static void send(void *v_s)
{
	struct sender *s = v_s;

	if (!s->pump) {
		struct stream *ss = async_transport_send(s->transport,
							 s->address, &s->err);
		assert(s);

		s->pump = stream_pump_create(
					  c_string_read_stream_create("Hello"),
					  ss, 10);
	}

	switch (stream_pump(s->pump, &s->tasklet, &s->err)) {
	case STREAM_ERROR:
		die("%s", error_message(&s->err));
		/* fall through */

	case STREAM_END:
		if (s->pump) {
			stream_pump_destroy_with_streams(s->pump);
			s->pump = NULL;
		}

		tasklet_stop(&s->tasklet);
	}
}

static void sender_init(struct sender *s, struct async_transport *t,
			struct address *addr)
{
	s->transport = t;
	s->address = addr;

	mutex_init(&s->mutex);
	tasklet_init(&s->tasklet, &s->mutex, s);
	error_init(&s->err);
	s->pump	= NULL;

	mutex_lock(&s->mutex);
	tasklet_goto(&s->tasklet, send);
	mutex_unlock(&s->mutex);
}

static void sender_fini(struct sender *s)
{
	mutex_lock(&s->mutex);

	if (s->pump)
		stream_pump_destroy_with_streams(s->pump);

	error_fini(&s->err);
	tasklet_fini(&s->tasklet);
	mutex_unlock_fini(&s->mutex);
	address_release(s->address);
}

static void test_socket_transport(void)
{
	struct async_transport *st = socket_transport_create(socket_factory());
	struct receiver receiver;
	struct sender senders[MESSAGES];
	size_t i;

	receiver_init(&receiver, st);
	for (i = 0; i < MESSAGES; i++)
		sender_init(&senders[i], st, receiver_address(&receiver));

	application_run();

	for (i = 0; i < MESSAGES; i++)
		sender_fini(&senders[i]);

	receiver_fini(&receiver);
	async_transport_destroy(st);
}

int main(void)
{
	application_prepare_test();
	test_socket_transport();
	return 0;
}
