#include "common.hpp"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>

#include <atomic>


namespace trac
{

size_t align(size_t value, size_t bound)
{
  size_t mask = (1ul << (64 - __builtin_clzl(bound))) - 1;
  return (value + mask) & ~mask;
}

void * (*g_orig_dlopen)(const char * filename, int flags) = nullptr;
int    (*g_orig_dlclose)(void * handle) = nullptr;
void * (*g_orig_malloc)(size_t size) = nullptr;
void * (*g_orig_calloc)(size_t count, size_t unit) = nullptr;
int    (*g_orig_posix_memalign)(void ** pptr, size_t bound, size_t size) = nullptr;
void * (*g_orig_realloc)(void * ptr, size_t size) = nullptr;
void   (*g_orig_free)(void * ptr) = nullptr;
size_t (*g_orig_malloc_usable_size)(void * ptr) = nullptr;

std::atomic_bool g_haveOrig(false);

pthread_once_t g_initMutexOnce = PTHREAD_ONCE_INIT;
pthread_mutex_t g_initOrigMutex;
bool g_recurse = false;

void initMutex()
{
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g_initOrigMutex, &attr);
  pthread_mutexattr_destroy(&attr);
}

void initOrig() // must be called holding g_initOrigMutex
{
  g_recurse = true;
  g_orig_dlopen             = (void * (*)(const char *, int))       dlsym(RTLD_NEXT, "dlopen");
  g_orig_dlclose            = (int    (*)(void *))                  dlsym(RTLD_NEXT, "dlclose");
  g_orig_malloc             = (void * (*)(size_t))                  dlsym(RTLD_NEXT, "malloc");
  g_orig_calloc             = (void * (*)(size_t, size_t))          dlsym(RTLD_NEXT, "calloc");
  g_orig_posix_memalign     = (int    (*)(void **, size_t, size_t)) dlsym(RTLD_NEXT, "posix_memalign");
  g_orig_realloc            = (void * (*)(void *, size_t))          dlsym(RTLD_NEXT, "realloc");
  g_orig_free               = (void   (*)(void *))                  dlsym(RTLD_NEXT, "free");
  g_orig_malloc_usable_size = (size_t (*)(void *))                  dlsym(RTLD_NEXT, "malloc_usable_size");
  g_recurse = false;
  g_haveOrig = true;
}

char g_fallbackBuffer[1024*1024];
void * g_fallbackBeg = g_fallbackBuffer;
void * g_fallbackCur = g_fallbackBuffer;
void * g_fallbackEnd = g_fallbackBuffer + sizeof(g_fallbackBuffer);

void * fallback_alloc(size_t bound, size_t size)
{
  void * ptr = align(g_fallbackCur, bound);
  void * cur = (void *)((char *)ptr + size);
  if (cur <= g_fallbackEnd) {
    g_fallbackCur = cur;
    return ptr;
  } else {
    return nullptr;
  }
}

bool check_fallback(void * ptr)
{
  return (uintptr_t)g_fallbackBeg <= (uintptr_t)ptr &&
         (uintptr_t)ptr < (uintptr_t)g_fallbackEnd;
}

void * orig_dlopen(const char * filename, int flags)
{
  if (g_haveOrig) {
    return g_orig_dlopen(filename, flags);
  } else {
    pthread_once(&g_initMutexOnce, initMutex);
    pthread_mutex_lock(&g_initOrigMutex);
    if (g_recurse) {
      // we should never end up here, as initOrig() would not open any libraries
      return nullptr;
    } else if (!g_haveOrig) {
      initOrig();
    }
    pthread_mutex_unlock(&g_initOrigMutex);
    return g_orig_dlopen(filename, flags);
  }
}

int    orig_dlclose(void * handle)
{
  if (g_haveOrig) {
    return g_orig_dlclose(handle);
  } else {
    pthread_once(&g_initMutexOnce, initMutex);
    pthread_mutex_lock(&g_initOrigMutex);
    if (g_recurse) {
      // we should never end up here, as initOrig() would not open any libraries
      return -1;
    } else if (!g_haveOrig) {
      initOrig();
    }
    pthread_mutex_unlock(&g_initOrigMutex);
    return g_orig_dlclose(handle);
  }
}


void * orig_malloc(size_t size)
{
  if (g_haveOrig) {
    return g_orig_malloc(size);
  } else {
    pthread_once(&g_initMutexOnce, initMutex);
    pthread_mutex_lock(&g_initOrigMutex);
    if (g_recurse) {
      return fallback_alloc(16, size);
    } else if (!g_haveOrig) {
      initOrig();
    }
    pthread_mutex_unlock(&g_initOrigMutex);
    return g_orig_malloc(size);
  }
}

void * orig_calloc(size_t count, size_t unit)
{
  if (g_haveOrig) {
    return g_orig_calloc(count, unit);
  } else {
    pthread_once(&g_initMutexOnce, initMutex);
    pthread_mutex_lock(&g_initOrigMutex);
    if (g_recurse) {
      return fallback_alloc(16, count * unit);
    } else if (!g_haveOrig) {
      initOrig();
    }
    pthread_mutex_unlock(&g_initOrigMutex);
    return g_orig_calloc(count, unit);
  }
}

int    orig_posix_memalign (void ** pptr, size_t bound, size_t size)
{
  if (g_haveOrig) {
    return g_orig_posix_memalign(pptr, bound, size);
  } else {
    pthread_once(&g_initMutexOnce, initMutex);
    pthread_mutex_lock(&g_initOrigMutex);
    if (g_recurse) {
      void * res = fallback_alloc(bound, size);
      if (res) {
        *pptr = res;
        return 0;
      } else {
        return ENOMEM;
      }
    } else if (!g_haveOrig) {
      initOrig();
    }
    pthread_mutex_unlock(&g_initOrigMutex);
    return g_orig_posix_memalign(pptr, bound, size);
  }
}

void * orig_realloc(void * ptr, size_t size)
{
  if (g_fallbackBeg <= ptr && ptr < g_fallbackEnd) {
    // can not realloc fallback-allocated pointer!
    // TODO-lw consider implementing with memcpy (always to fallback or if possible to orig_malloc??)
    return nullptr;
  }

  if (g_haveOrig) {
    return g_orig_realloc(ptr, size);
  } else {
    pthread_once(&g_initMutexOnce, initMutex);
    pthread_mutex_lock(&g_initOrigMutex);
    if (g_recurse) {
      if (!ptr) {
        return fallback_alloc(16, size);
      } else {
        // dlsym (only anticipated source of recursion) should not have a non-fallback-allocated ptr anyway,
        //   so this case would already be covered by check at start of function
        return nullptr;
      }
    } else if (!g_haveOrig) {
      initOrig();
    }
    pthread_mutex_unlock(&g_initOrigMutex);
    return g_orig_realloc(ptr, size);
  }
}

void   orig_free(void * ptr)
{
  if (!ptr || (g_fallbackBeg <= ptr && ptr < g_fallbackEnd)) {
    return;
  }

  if (g_haveOrig) {
    g_orig_free(ptr);
    return;
  } else {
    pthread_once(&g_initMutexOnce, initMutex);
    pthread_mutex_lock(&g_initOrigMutex);
    if (g_recurse) {
      // dlsym (only anticipated source of recursion) should not have a non-fallback-allocated ptr anyway,
      //   so this case would already be covered by check at start of function
      return;
    } else if (!g_haveOrig) {
      initOrig();
    }
    pthread_mutex_unlock(&g_initOrigMutex);
    g_orig_free(ptr);
    return;
  }
}

size_t orig_malloc_usable_size(void * ptr)
{
  if (!ptr || (g_fallbackBeg <= ptr && ptr < g_fallbackEnd)) {
    // There is no way of knowing the size of a fallback allocation
    // TODO-lw consier fancier first fit allocator from `old/allocator.c` though probably not worth it
    return 0;
  }

  if (g_haveOrig) {
    return g_orig_malloc_usable_size(ptr);
  } else {
    pthread_once(&g_initMutexOnce, initMutex);
    pthread_mutex_lock(&g_initOrigMutex);
    if (g_recurse) {
      // dlsym (only anticipated source of recursion) should not have a non-fallback-allocated ptr anyway,
      //   so this case would already be covered by check at start of function
      return 0;
    } else if (!g_haveOrig) {
      initOrig();
    }
    pthread_mutex_unlock(&g_initOrigMutex);
    return g_orig_malloc_usable_size(ptr);
  }
}


} // namespace trac
