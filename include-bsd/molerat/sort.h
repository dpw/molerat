#ifndef MOLERAT_SORT_H
#define MOLERAT_SORT_H

struct xsort_compare {
	int (*cmp)(const void *, const void *, void *);
	void *arg;
};

int xsort_thunk(void *arg, const void *a, const void *b);

static inline void xsort(void *base, size_t n, size_t sz,
			 int (*cmp)(const void *, const void *, void *),
			 void *arg)
{
	struct xsort_compare c = { cmp, arg };
	qsort_r(base, n, sz, &c, xsort_thunk);
}

#endif
