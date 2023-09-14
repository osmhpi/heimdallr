#include "handler.hpp"

#include <alloca.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// #include <endian.h>
//#define UNW_LOCAL_ONLY
//#include <libunwind.h>
#include <execinfo.h>


namespace trac
{

memkind_t Handler::s_memkind = nullptr;
pthread_once_t Handler::s_memkindCreate = PTHREAD_ONCE_INIT;
pthread_once_t Handler::s_memkindDestroy = PTHREAD_ONCE_INIT;

std::vector<Handler *> Handler::s_handlers;

pthread_mutex_t Handler::s_createGuard = PTHREAD_MUTEX_INITIALIZER;

Handler * Handler::get()
{
  pthread_mutex_lock(&s_createGuard);
  Handler * handler = new Handler(s_handlers.size());
  s_handlers.push_back(handler);
  pthread_mutex_unlock(&s_createGuard);
  return handler;
}

void Handler::end()
{
  for (Handler * handler : s_handlers) {
    handler->onEnd();
  }
  s_handlers.clear();
  pthread_once(&s_memkindDestroy, destroyMemkind);
}

Handler * Handler::globalAllocLookup(uintptr_t base, Alloc & info, Handler * exclude)
{
  for (Handler * handler : s_handlers) {
    if (handler != exclude && handler->localAllocLookup(base, info)) {
      return handler;
    }
  }
  return nullptr;
}

void Handler::createMemkind()
{
  const char * pmemdir = getenv("TRAC_PMEMDIR");
  if (pmemdir) {
    const char * pmemsize = getenv("TRAC_PMEMSIZE");
    size_t size = strtoull(pmemsize, nullptr, 0);
    if (!size) {
      size = 1ULL << 32; // default to 4 GiB
    }
    int err = memkind_create_pmem(pmemdir, size, &s_memkind);
    if (err) {
      printf("PMEM memkind error: %d\n", err);
      s_memkind = nullptr;
    } else {
      printf("PMEM memkind: %p\n", s_memkind);
    }
  } else {
    s_memkind = nullptr;
  }
}

void Handler::destroyMemkind()
{
  if (s_memkind) {
    memkind_destroy_kind(s_memkind);
  }
}

memkind_t Handler::getMemkind()
{
  pthread_once(&s_memkindCreate, createMemkind);
  if (!s_memkind) {
    // printf("Using default memkind\n");
    return MEMKIND_DEFAULT;
  } else {
    // printf("Using memkind: %p\n", s_memkind);
    return s_memkind;
  }
}


Handler::Handler(size_t id)
: m_id(id)
, m_allocs()
, m_allocsGuard(PTHREAD_RWLOCK_INITIALIZER)
, m_log(nullptr)
, m_threshold(0)
, m_stacklevels(0)
, m_stackoffset(3)
, m_stackbuf(nullptr)
{
  char * logpath = getenv("TRAC_LOGPATH");
  if (logpath) {
    char logfilename[256];
    snprintf(logfilename, sizeof(logfilename), "%s/alloc_%ld_%d.log", logpath, id, gettid());
    m_log = fopen(logfilename, "w");
  }
  char * threshold = getenv("TRAC_THRESHOLD");
  if (threshold) {
    m_threshold = strtoul(threshold, nullptr, 0);
  }
  char * stacklevels = getenv("TRAC_STACKLEVELS");
  if (stacklevels) {
    m_stacklevels = strtoul(stacklevels, nullptr, 0);
  }
  if (m_stacklevels) {
    m_stackbuf = new Mappings::LibAddr[m_stacklevels];
  }
}

Handler::~Handler()
{
  if (m_stackbuf) {
    delete m_stackbuf;
    m_stackbuf = nullptr;
  }
}

void * Handler::malloc(size_t size)
{
  void * ptr;
  if (size < m_threshold) {
    ptr = orig_malloc(size);
    if (ptr) {
      Alloc info = {size, nullptr};
      this->allocInsert((uintptr_t)ptr, info);
    }
  } else {
    size_t stackcnt = 0;
    Mappings::LibAddr * stackbuf = stack(stackcnt);
    memkind_t kind = select(size, stackbuf, stackcnt);
    ptr = memkind_malloc(kind, size);
    if (ptr) {
      log(true, (uintptr_t)ptr, size, stackbuf, stackcnt);
      Alloc info = {size, kind};
      this->allocInsert((uintptr_t)ptr, info);
    }
  }
  return ptr;
}

void * Handler::calloc(size_t count, size_t unit)
{
  size_t size = count * unit;
  void * ptr;
  if (size < m_threshold) {
    ptr = orig_calloc(count, unit);
    if (ptr) {
      Alloc info = {size, nullptr};
      this->allocInsert((uintptr_t)ptr, info);
    }
  } else {
    size_t stackcnt = 0;
    Mappings::LibAddr * stackbuf = stack(stackcnt);
    memkind_t kind = select(size, stackbuf, stackcnt);
    ptr = memkind_calloc(kind, count, unit);
    if (ptr) {
      log(true, (uintptr_t)ptr, size, stackbuf, stackcnt);
      Alloc info = {size, kind};
      this->allocInsert((uintptr_t)ptr, info);
    }
  }
  return ptr;
}

int    Handler::memalign(void ** pptr, size_t bound, size_t size)
{
  int err;
  if (size < m_threshold) {
    err = orig_posix_memalign(pptr, bound, size);
    if (!err) {
      Alloc info = {size, nullptr};
      this->allocInsert((uintptr_t)(*pptr), info);
    }
  } else {
    size_t stackcnt = 0;
    Mappings::LibAddr * stackbuf = stack(stackcnt);
    memkind_t kind = select(size, stackbuf, stackcnt);
    err = memkind_posix_memalign(kind, pptr, bound, size);
    if (!err) {
      log(true, (uintptr_t)(*pptr), size, stackbuf, stackcnt);
      Alloc info = {size, kind};
      this->allocInsert((uintptr_t)(*pptr), info);
    }
  }
  return err;
}

bool Handler::realloc(void ** pptr, size_t size)
{
  void * oldptr = *pptr;
  Alloc oldinfo;
  Handler * home = allocLookup((uintptr_t)oldptr, oldinfo);
  if (!home) {
    return false;
  }
  void * newptr;
  if (size < m_threshold) {
    if (oldinfo.kind) {
      newptr = memkind_realloc(oldinfo.kind, oldptr, size);
    } else {
      newptr = orig_realloc(oldptr, size);
    }
    if (newptr) {
      if (oldinfo.size >= m_threshold) {
        size_t stackcnt = 0;
        Mappings::LibAddr * stackbuf = stack(stackcnt);
        log(false, (uintptr_t)oldptr, oldinfo.size, stackbuf, stackcnt);
      }
      Alloc newinfo = {size, oldinfo.kind};
      home->allocRemove((uintptr_t)oldptr);
      this->allocInsert((uintptr_t)newptr, newinfo);
    }
  } else {
    size_t stackcnt = 0;
    Mappings::LibAddr * stackbuf = stack(stackcnt);
    memkind_t newkind = select(size, stackbuf, stackcnt);
    if (oldinfo.kind == newkind) {
      newptr = memkind_realloc(oldinfo.kind, oldptr, size);
    } else {
      newptr = memkind_malloc(newkind, size);
      if (newptr) {
        memcpy(newptr, oldptr, (oldinfo.size < size)? oldinfo.size : size);
        if (oldinfo.kind) {
          memkind_free(oldinfo.kind, oldptr);
        } else {
          orig_free(oldptr);
        }
      }
    }
    if (newptr) {
      if (oldinfo.size >= m_threshold) {
        log(false, (uintptr_t)oldptr, oldinfo.size, stackbuf, stackcnt);
      }
      log(true, (uintptr_t)newptr, size, stackbuf, stackcnt);
      Alloc newinfo = {size, newkind};
      home->allocRemove((uintptr_t)oldptr);
      this->allocInsert((uintptr_t)newptr, newinfo);
    }
  }
  *pptr = newptr;
  return true;
}

bool   Handler::free(void * ptr)
{
  Alloc info;
  Handler * home = allocLookup((uintptr_t)ptr, info);
  if (!home) {
    return false;
  }

  if (info.kind) {
    memkind_free(info.kind, ptr);
  } else {
    orig_free(ptr);
  }
  if (info.size >= m_threshold) {
    size_t stackcnt = 0;
    Mappings::LibAddr * stackbuf = stack(stackcnt);
    log(false, (uintptr_t)ptr, info.size, stackbuf, stackcnt);
  }
  home->allocRemove((uintptr_t)ptr);
  return true;
}

bool   Handler::getsize(void * ptr, size_t * size)
{
  uintptr_t base = (uintptr_t)ptr;
  Alloc info;
  Handler * home = allocLookup(base, info);
  if (!home) {
    return false;
  }
  *size = info.size;
  return true;
}

void Handler::onEnd()
{
  // printf("!!%d:onEnd():WRlock(%p)\n", gettid(), &m_allocsGuard);
  pthread_rwlock_wrlock(&m_allocsGuard);
  for (auto & entry : m_allocs) {
    if (entry.second.size > m_threshold) {
      log(false, entry.first, entry.second.size);
    }
  }
  m_allocs.clear();
  pthread_rwlock_unlock(&m_allocsGuard);
  // printf("!!%d:onEnd():WRfree(%p)\n", gettid(), &m_allocsGuard);
  if (m_log) {
    fclose(m_log);
  }
}

Handler * Handler::allocLookup(uintptr_t base, Alloc & info)
{
  if (localAllocLookup(base, info)) {
    return this;
  }
  return globalAllocLookup(base, info, this);
}

memkind_t Handler::select(size_t size, Mappings::LibAddr * stackbuf, size_t stackcnt)
{
  memkind_t kind = getMemkind();
  // printf("Handler::select(%ld, ...) = %p\n", size, kind);
  return kind;
  // return getMemkind(); // uses a shared memkind across all threads
}

bool Handler::localAllocLookup(uintptr_t base, Alloc & info)
{
  bool success = false;
  // printf("!!%d:localAllocLookup():RDlock(%p)\n", gettid(), &m_allocsGuard);
  pthread_rwlock_rdlock(&m_allocsGuard);
  auto it = m_allocs.find(base);
  if (it != m_allocs.end()) {
    info = it->second;
    success = true;
  }
  pthread_rwlock_unlock(&m_allocsGuard);
  // printf("!!%d:localAllocLookup():RDfree(%p)\n", gettid(), &m_allocsGuard);
  return success;
}

void Handler::allocInsert(uintptr_t base, const Alloc & info)
{
  // printf("!!%d:allocInsert():WRlock(%p)\n", gettid(), &m_allocsGuard);
  pthread_rwlock_wrlock(&m_allocsGuard);
  m_allocs.emplace(base, info);
  pthread_rwlock_unlock(&m_allocsGuard);
  // printf("!!%d:allocInsert():WRfree(%p)\n", gettid(), &m_allocsGuard);
}

void Handler::allocRemove(uintptr_t base)
{
  // printf("!!%d:allocRemove():WRlock(%p)\n", gettid(), &m_allocsGuard);
  pthread_rwlock_wrlock(&m_allocsGuard);
  m_allocs.erase(base);
  pthread_rwlock_unlock(&m_allocsGuard);
  // printf("!!%d:allocRemove():WRfree(%p)\n", gettid(), &m_allocsGuard);
}


Mappings::LibAddr * Handler::stack(size_t & count)
{
  if (!m_stackbuf) {
    return nullptr;
  }
  size_t capacity = m_stacklevels + m_stackoffset;
  void ** buffer = (void **)alloca(capacity * sizeof(void *));
  size_t levels = backtrace(buffer, capacity);
  count = 0;
  for (size_t idx = m_stackoffset; idx < levels; ++idx) {
    Mappings::lookup((uintptr_t)buffer[idx], m_stackbuf[count++]);
  }
  return m_stackbuf;
}

// size_t Handler::stack(uintptr_t * buf, size_t count, size_t offset)
// {
//   unw_cursor_t cursor;
//   unw_context_t context;
//   unw_word_t ip = 0;

//   if (unw_getcontext(&context) < 0) {
//     return 0;
//   }
//   if (unw_init_local(&cursor, &context) < 0) {
//     return 0;
//   }
//   size_t pos = 0;
//   for (size_t idx = 0; idx < count + offset; ++idx) {
//     if (idx >= offset) {
//       unw_get_reg(&cursor, UNW_REG_IP, &ip);
//       buf[pos++] = ip;
//       //unw_word_t off;
//       //char buf[256];
//       //unw_get_proc_name(&cursor, buf, sizeof(buf), &off);
//       //printf("..%s + %lx\n", buf, off);
//     }
//     if (unw_step(&cursor) <= 0) {
//       break;
//     }
//   }
//   return pos;
// }

// void Handler::log(size_t free, size_t alloc, size_t size)
void Handler::log(bool alloc, uintptr_t base, size_t size, Mappings::LibAddr * sbuf, size_t snum)
{
  if (!m_log) {
    return;
  }
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC_RAW, &now);
  fprintf(m_log, "%c%ld.%09ld,%016lx,%016lx", alloc? '+' : '-', now.tv_sec, now.tv_nsec, base, size);
  if (sbuf) {
    for (size_t i = 0; i < snum; ++i) {
      fprintf(m_log, ",%ld+%lx", sbuf[i].index, sbuf[i].offset);
    }
  }
  fprintf(m_log, "\n");
}


} // namespace trac
