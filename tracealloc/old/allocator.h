
#define BASEALIGN 16
#define OVERHEAD (2 * sizeof(void *))
#define THRESHOLD (2 * BASEALIGN + OVERHEAD)


extern size_t align(size_t value, size_t bound);

extern void * my_alloc(size_t size, size_t bound);
extern int my_free(void * ptr);
extern void * my_realloc(void * ptr, size_t size, size_t bound);
extern void * my_dump();

