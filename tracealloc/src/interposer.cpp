#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "handler.hpp"
#include "mappings.hpp"
#include "common.hpp"


extern "C" void * dlopen(const char * filename, int flags);
extern "C" int    dlclose(void * handle);

extern "C" void * malloc(size_t size);
extern "C" void * calloc(size_t count, size_t unit);
extern "C" void * realloc(void * ptr, size_t size);
extern "C" int    posix_memalign(void ** pptr, size_t alignment, size_t size);
extern "C" void * aligned_alloc(size_t alignment, size_t size);
extern "C" void * memalign(size_t alignment, size_t size);
extern "C" void * valloc(size_t size);
extern "C" void * pvalloc(size_t size);
extern "C" void   free(void * ptr);
extern "C" void   cfree(void * ptr);
extern "C" size_t malloc_usable_size(void * ptr);


static bool g_ready = false;
static thread_local bool t_nested = false;
static thread_local trac::Handler * t_handler = nullptr;

void __attribute__((constructor)) interposer_setup()
{
  struct timespec wnow, pnow;
  clock_gettime(CLOCK_MONOTONIC_RAW, &wnow);
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &pnow);
  printf("TRAC_BEG:%ld.%09ld:%ld.%09ld\n", wnow.tv_sec, wnow.tv_nsec, pnow.tv_sec, pnow.tv_nsec);
  g_ready = true;
}

void __attribute__((destructor)) interposer_teardown()
{
  struct timespec now;
  struct timespec wnow, pnow;
  clock_gettime(CLOCK_MONOTONIC_RAW, &wnow);
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &pnow);
  printf("TRAC_END:%ld.%09ld:%ld.%09ld\n", wnow.tv_sec, wnow.tv_nsec, pnow.tv_sec, pnow.tv_nsec);
  g_ready = false; // TODO-lw maybe after, as handler was initialized with g_ready = true?
  trac::Handler::end();
  trac::Mappings::end();
}

void * dlopen(const char * filename, int flags)
{
  void * res = trac::orig_dlopen(filename, flags);
  if (!t_nested) {
   t_nested = true;
   trac::Mappings::update();
   t_nested = false;
  }

  return res;
}
int    dlclose(void * handle)
{
  int res = trac::orig_dlclose(handle);
  if (!t_nested) {
   t_nested = true;
   trac::Mappings::update();
   t_nested = false;
  }

  return res;
}

void * malloc(size_t size)
{
  if (!g_ready || t_nested) {
    return trac::orig_malloc(size);
  } else {
    t_nested = true;
    if (!t_handler) {
      t_handler = trac::Handler::get();
    }
    void * res = t_handler->malloc(size);
    t_nested = false;
    return res;
  }
}

void * calloc(size_t count, size_t unit)
{
  if (!g_ready || t_nested) {
    return trac::orig_calloc(count, unit);
  } else {
    t_nested = true;
    if (!t_handler) {
      t_handler = trac::Handler::get();
    }
    void * res = t_handler->calloc(count, unit);
    t_nested = false;
    return res;
  }
}

int    posix_memalign(void ** pptr, size_t bound, size_t size)
{
  if (!g_ready || t_nested) {
    return trac::orig_posix_memalign(pptr, bound, size);
  } else {
    t_nested = true;
    if (!t_handler) {
      t_handler = trac::Handler::get();
    }
    int err = t_handler->memalign(pptr, bound, size);
    t_nested = false;
    return err;
  }
}
void * memalign(size_t bound, size_t size)
{
  void * res;
  if (!posix_memalign(&res, bound, size)) {
    return res;
  } else {
    return nullptr;
  }
}
void * aligned_alloc(size_t bound, size_t size)
{
  void * res;
  if (!posix_memalign(&res, bound, size)) {
    return res;
  } else {
    return nullptr;
  }
}
void * valloc(size_t size)
{
  size_t bound = sysconf(_SC_PAGESIZE);
  void * res;
  if (!posix_memalign(&res, bound, size)) {
    return res;
  } else {
    return nullptr;
  }
}
void * pvalloc(size_t size)
{
  size_t bound = sysconf(_SC_PAGESIZE);
  size = trac::align(size, bound);
  void * res;
  if (!posix_memalign(&res, bound, size)) {
    return res;
  } else {
    return nullptr;
  }
}

void * realloc(void * ptr, size_t size)
{
  if (!g_ready || t_nested) {
    return trac::orig_realloc(ptr, size);
  } else {
    t_nested = true;
    if (!t_handler) {
      t_handler = trac::Handler::get();
    }
    void * res = ptr;
    if (!ptr) {
      res = t_handler->malloc(size);
    } else if (!t_handler->realloc(&res, size)) {
      // ptr did not come from handlers, so transfer to new handler allocation
      // TODO-lw usage of __malloc_usable_size seems buggy
      size_t oldsize = trac::orig_malloc_usable_size(ptr);
      res = t_handler->malloc(size);
      if (oldsize < size) {
        size = oldsize;
      }
      memcpy(res, ptr, size);
      trac::orig_free(ptr);
    }
    t_nested = false;
    return res;
  }
}

void   free(void * ptr)
{
  if (!ptr || trac::check_fallback(ptr)) {
    return;
  }
  if (!g_ready || t_nested || !t_handler) {
    trac::orig_free(ptr);
  } else {
    t_nested = true;
    if (!t_handler->free(ptr)) {
      trac::orig_free(ptr);
    }
    t_nested = false;
  }
}

void   cfree(void * ptr)
{
  return free(ptr);
}

size_t malloc_usable_size(void * ptr)
{
  if (!ptr) {
    return 0;
  }
  if (!g_ready || t_nested || !t_handler) {
    return trac::orig_malloc_usable_size(ptr);
  } else {
    size_t res = 0;
    t_nested = true;
    if (!t_handler->getsize(ptr, &res)) {
      res = trac::orig_malloc_usable_size(ptr);
    }
    t_nested = false;
    return res;
  }
}

