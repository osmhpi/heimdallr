#pragma once

#include <map>
#include <vector>
#include <utility>

#include <stdio.h>
#include <pthread.h>

#include <memkind.h>

#include "common.hpp"
#include "mappings.hpp"


namespace trac
{

class Handler
{
public:
  struct Alloc
  {
    size_t size;
    memkind_t kind;
  };

private:
  static memkind_t s_memkind;
  static pthread_once_t s_memkindCreate;
  static pthread_once_t s_memkindDestroy;

  static std::vector<Handler *> s_handlers;
  static pthread_mutex_t s_createGuard;

  size_t m_id;
  std::map<uintptr_t, Alloc> m_allocs;
  pthread_rwlock_t m_allocsGuard;
  FILE * m_log;
  size_t m_threshold;
  size_t m_stacklevels;
  size_t m_stackoffset;
  Mappings::LibAddr * m_stackbuf;

  Handler(size_t id);

  static void createMemkind();
  static void destroyMemkind();
  static memkind_t getMemkind();

public:
  static Handler * get();
  static void end();
  static Handler * globalAllocLookup(uintptr_t base, Alloc & info, Handler * exclude = nullptr);

  ~Handler();

  void * malloc(size_t size);
  void * calloc(size_t count, size_t unit);
  int    memalign(void ** pptr, size_t bound, size_t size);
  bool   realloc(void ** ptr, size_t size);
  bool   free(void * ptr);
  bool   getsize(void * ptr, size_t * size);

  void onEnd();

private:
  memkind_t select(size_t size, Mappings::LibAddr * stackbuf, size_t stacknum);

  Handler * allocLookup(uintptr_t base, Alloc & info);
  bool localAllocLookup(uintptr_t base, Alloc & info);
  void allocInsert(uintptr_t base, const Alloc & info);
  void allocRemove(uintptr_t base);

  Mappings::LibAddr * stack(size_t & count);
  void log(bool alloc, uintptr_t base, size_t size, Mappings::LibAddr * sbuf = nullptr, size_t snum = 0);
};

} // namespace trac
