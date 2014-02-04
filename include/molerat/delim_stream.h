#ifndef MOLERAT_DELIM_STREAM_H
#define MOLERAT_DELIM_STREAM_H

#include <molerat/stream.h>

/* Delimited streams.  I.e,, a mechanism to provide multiple
   sub-streams within a single underlying stream.  For example, the
   underlying stream might be a socket.  The sub-streams might contain
   request messages. */

struct delim_write;

struct delim_write *delim_write_create(struct stream *underlying);
void delim_write_destroy(struct delim_write *dw);
struct stream *delim_write_next(struct delim_write *dw);

struct delim_read;

struct delim_read *delim_read_create(struct stream *underlying);
void delim_read_destroy(struct delim_read *dr);
struct stream *delim_read_next(struct delim_read *dr);

#endif
