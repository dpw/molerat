#ifndef MOLERAT_ENDIAN_H
#define MOLERAT_ENDIAN_h

/* Endianness conversions for OS X, which doesn't have the Linux/BSD
   <endian.h>.  */

#include <libkern/OSByteOrder.h>

#define ENDIAN_TYPE(prefix, size, e, bl)                              \
typedef struct { uint##size##_t v; } prefix##size##_##e##_t;          \
                                                                      \
static inline prefix##size##_t prefix##size##_from_##e(prefix##size##_##e##_t a) \
{                                                                     \
	return OSSwap##bl##ToHostInt##size(a.v);                      \
}                                                                     \
                                                                      \
static inline prefix##size##_##e##_t prefix##size##_to_##e(prefix##size##_t a) \
{                                                                     \
	prefix##size##_##e##_t r;                                     \
	r.v = OSSwapHostTo##bl##Int##size(a);                         \
	return r;                                                     \
}

ENDIAN_TYPE(uint,16,le,Little)
ENDIAN_TYPE(uint,16,be,Big)
ENDIAN_TYPE(int,16,le,Little)
ENDIAN_TYPE(int,16,be,Big)

ENDIAN_TYPE(uint,32,le,Little)
ENDIAN_TYPE(uint,32,be,Big)
ENDIAN_TYPE(int,32,le,Little)
ENDIAN_TYPE(int,32,be,Big)

ENDIAN_TYPE(uint,64,le,Little)
ENDIAN_TYPE(uint,64,be,Big)
ENDIAN_TYPE(int,64,le,Little)
ENDIAN_TYPE(int,64,be,Big)

#undef ENDIAN_TYPE


#endif
