#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#define UNW_LOCAL_ONLY
#include <libunwind.h>

#include "allocator.h"

void * malloc(size_t size);
void free(void *ptr);
void * realloc(void *ptr, size_t size);
void * calloc(size_t count, size_t unit);
void * memalign(size_t bound, size_t size);

static size_t get_stacktrace(uintptr_t * locations, size_t count, size_t offset)
{
  unw_cursor_t cursor;
  unw_context_t context;
  unw_word_t ip, sp, lastsp = 0;

  if (unw_getcontext(&context) < 0) {
    return 0;
  }
  if (unw_init_local(&cursor, &context) < 0) {
    return 0;
  }
  size_t pos = 0;
  for (size_t idx = 0; idx < count + offset; ++idx) {
    if (idx >= offset) {
      unw_get_reg(&cursor, UNW_REG_IP, &ip);
      unw_get_reg(&cursor, UNW_REG_SP, &sp);
      locations[pos++] = ip;
      unw_word_t off;
      char buf[256];
      //unw_get_proc_name(&cursor, buf, sizeof(buf), &off);
      //if (lastsp) {
      //  ptrdiff_t diff = lastsp - sp;
      //  printf("..0x%016lx (%+ld) -> %s() [0x%016lx + 0x%lx]\n", sp, diff, buf, ip - off, off);
      //} else {
      //  printf("..0x%016lx -> %s() [0x%016lx + 0x%lx]\n", sp, buf, ip - off, off);
      //}
      //lastsp = sp;
    }
    if (unw_step(&cursor) <= 0) {
      break;
    }
  }
  return pos;
}

static void log_access(void * free, void * alloc, size_t size)
{
  static FILE * log = NULL;
  uintptr_t locations[8];

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);


  if (!log) {
    char * logfile = getenv("ALLOC_LOG");
    if (logfile) {
      fprintf(stderr, "Logging to %s\n", logfile);
      log = fopen(logfile, "w");
    }
    if (!log) {
      log = stderr;
    }
    //fprintf(log, "malloc()   [%p]\n", &malloc);
    //fprintf(log, "free()     [%p]\n", &free);
    //fprintf(log, "realloc()  [%p]\n", &realloc);
    //fprintf(log, "calloc()   [%p]\n", &calloc);
    //fprintf(log, "memalign() [%p]\n", &memalign);
  }

  if (log) {
    size_t timestamp = ts.tv_nsec/1000000 + ts.tv_sec * 1000ul;
    fprintf(log, "%ld.%09ld,%016lx,%016lx,%016lx\n", ts.tv_sec, ts.tv_nsec, (size_t)free, (size_t)alloc, size);
    //size_t nlocs = get_stacktrace(locations, 4, 3);
    //for (size_t i = 0; i < nlocs; ++i) {
    //  fprintf(log, "> %016lx ", locations[i]);
    //}
    //fprintf(log, "\n");
  }
}

static char tiny_region[0x10000];
static char * tiny_cur = tiny_region;
static char * tiny_end = tiny_region + sizeof(tiny_region);

static void * tiny_alloc(size_t size, size_t bound)
{
  DEBUGMSG("!!! tiny_alloc(0x%lx, %ld)", size, bound);
  char * ptr = ALIGN_PTR(tiny_cur, bound);
  char * end = ptr + size;
  if (end <= tiny_end) {
    tiny_cur = end;
    DEBUGMSG(" = [%p], (0x%lx) remaining\n", ptr, (size_t)tiny_end - (size_t)tiny_cur);
    return ptr;
  } else {
    DEBUGMSG(" = NULL\n");
    return NULL;
  }
}

static int tiny_free(void * ptr)
{
  // return true if ptr is within tiny_region
  // no memory is actually freed
  if (((size_t)ptr >= (size_t)tiny_region) &&
      ((size_t)ptr <  (size_t)tiny_end)) {
    DEBUGMSG("!!! tiny_free([%p])\n", ptr);
    return 1;
  } else {
    return 0;
  }
}

static __thread int reenter = 0;
static pthread_once_t mutex_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mutex;

void mutex_init()
{
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
}

void * malloc(size_t size)
{
  pthread_once(&mutex_once, mutex_init);
  pthread_mutex_lock(&mutex);
  DEBUGMSG("malloc(0x%lx)\n", size);
  if (reenter) {
    return tiny_alloc(size, BASEALIGN);
  }
  reenter = 1;

  void * ptr = my_alloc(size, BASEALIGN);
  log_access(NULL, ptr, size);
  my_dump();

  reenter = 0;
  pthread_mutex_unlock(&mutex);
  return ptr;
}

void free(void *ptr)
{
  pthread_once(&mutex_once, mutex_init);
  pthread_mutex_lock(&mutex);
  DEBUGMSG("free([0x%p])\n", ptr);
  if (!ptr || tiny_free(ptr)) {
    return;
  }

  if (my_free(ptr)) {
    log_access(ptr, NULL, 0);
    my_dump();
  }

  pthread_mutex_unlock(&mutex);
}

void * realloc(void *ptr, size_t size)
{
  pthread_once(&mutex_once, mutex_init);
  pthread_mutex_lock(&mutex);
  DEBUGMSG("realloc([0x%p], 0x%lx)\n", ptr, size);
  if (reenter) {
    DEBUGMSG("!!! tiny_realloc([%p], 0x%lx) not supported\n", ptr, size);
    return NULL;
  }
  reenter = 1;

  void * nptr = my_realloc(ptr, size, BASEALIGN);
  log_access(ptr, nptr, size);
  my_dump();

  reenter = 0;
  pthread_mutex_unlock(&mutex);
  return nptr;
}

void * calloc(size_t count, size_t unit)
{
  pthread_once(&mutex_once, mutex_init);
  pthread_mutex_lock(&mutex);
  DEBUGMSG("calloc(0x%lx, 0x%lx)\n", count, unit);
  if (reenter) {
    return tiny_alloc(count * unit, BASEALIGN);
  }
  reenter = 1;

  void * ptr = my_alloc(count * unit, BASEALIGN);
  log_access(NULL, ptr, count * unit);
  my_dump();

  reenter = 0;
  pthread_mutex_unlock(&mutex);
  return ptr;
}

void * memalign(size_t bound, size_t size)
{
  pthread_once(&mutex_once, mutex_init);
  pthread_mutex_lock(&mutex);
  DEBUGMSG("memalign(%ld, 0x%lx)\n", bound, size);
  if (reenter) {
    return tiny_alloc(size, bound);
  }
  reenter = 1;

  void * ptr = my_alloc(size, bound);
  log_access(NULL, ptr, size);
  my_dump();

  reenter = 0;
  pthread_mutex_unlock(&mutex);
  return ptr;
}

