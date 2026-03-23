#ifdef __FreeBSD__
void *recallocarray(void *, size_t, size_t, size_t);
#endif

/* REG_STARTEND is not available in musl libc. This stub allows compilation but
 * regex searches will not honor start offsets, causing forward search to find
 * matches before the cursor position. */
#ifndef REG_STARTEND
#define REG_STARTEND 0
#endif
