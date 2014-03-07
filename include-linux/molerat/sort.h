#ifndef MOLERAT_SORT_H
#define MOLERAT_SORT_H

static inline void xsort(void *base, size_t n, size_t sz,
			 int (*cmp)(const void *, const void *, void *),
			 void *arg)
{
	qsort_r(base, n, sz, cmp, arg);
}

#endif
