#pragma once

#include <stdint.h>
#include <stddef.h>


// extern "C" void*  __libc_malloc(size_t);
// extern "C" void   __libc_free(void*);
// extern "C" void*  __libc_calloc(size_t, size_t);
// extern "C" void*  __libc_realloc(void*, size_t);
// extern "C" int    __posix_memalign (void **, size_t, size_t);
// extern "C" void*  __libc_memalign(size_t, size_t);
// extern "C" void*  __libc_valloc(size_t);
// extern "C" void*  __libc_pvalloc(size_t);
// extern "C" size_t __malloc_usable_size (void *);


namespace trac
{

uintptr_t align(uintptr_t value, size_t bound);

inline void * align(void * ptr, size_t bound)
{
  return (void *)align((uintptr_t)ptr, bound);
}

void * orig_dlopen(const char * filename, int flags);
int    orig_dlclose(void * handle);

void * orig_malloc(size_t size);
void * orig_calloc(size_t count, size_t unit);
int    orig_posix_memalign (void ** pptr, size_t bound, size_t size);
void * orig_realloc(void * ptr, size_t size);
void   orig_free(void * ptr);
size_t orig_malloc_usable_size(void * ptr);

bool check_fallback(void * ptr);

} // namespace trac



