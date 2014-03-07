#include <molerat/base.h>
#include <molerat/sort.h>

int xsort_thunk(void *v_c, const void *a, const void *b)
{
	struct xsort_compare *c = v_c;
	return c->cmp(a, b, c->arg);
}
