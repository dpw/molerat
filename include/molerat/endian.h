#ifndef MOLERAT_ENDIAN_H
#define MOLERAT_ENDIAN_h

/* This assume the Linux <endian.h>.  */

#include <endian.h>

#define ENDIAN_TYPE(prefix, size, e)                                  \
typedef struct { uint##size##_t v; } prefix##size##_##e##_t;          \
                                                                      \
static inline prefix##size##_t prefix##size##_from_##e(prefix##size##_##e##_t a) \
{                                                                     \
	return e##size##toh(a.v);                                     \
}                                                                     \
                                                                      \
static inline prefix##size##_##e##_t prefix##size##_to_##e(prefix##size##_t a) \
{                                                                     \
	prefix##size##_##e##_t r;                                     \
	r.v = hto##e##size(a);                                        \
	return r;                                                     \
}

ENDIAN_TYPE(uint,16,le)
ENDIAN_TYPE(uint,16,be)
ENDIAN_TYPE(int,16,le)
ENDIAN_TYPE(int,16,be)

ENDIAN_TYPE(uint,32,le)
ENDIAN_TYPE(uint,32,be)
ENDIAN_TYPE(int,32,le)
ENDIAN_TYPE(int,32,be)

ENDIAN_TYPE(uint,64,le)
ENDIAN_TYPE(uint,64,be)
ENDIAN_TYPE(int,64,le)
ENDIAN_TYPE(int,64,be)

#endif
