#pragma once

#include <string>
#include <map>

#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>


namespace trac
{

class Mappings
{
public:

  struct LibAddr
  {
    size_t index;
    uintptr_t offset;
  };

  struct Entry
  {
    Entry(size_t index, size_t base, size_t size, size_t offset);

    size_t index;
    size_t base;
    size_t size;
    size_t offset;
  };

private:
  static Mappings * s_instance;
  static pthread_rwlock_t s_lock;
  static pthread_once_t s_lockInit;

  static void initLock();

  std::map<std::string, size_t> m_libs;
  std::map<uintptr_t, Entry> m_entries;
  FILE * m_log;

  Mappings();
  ~Mappings();

  void doUpdate();
  static int updateCallback(struct dl_phdr_info * info, size_t size, void * data);
  size_t getIndex(const char * filename);
  void putMapping(size_t index, uintptr_t base, size_t size, size_t offset);

  void doLookup(uintptr_t vaddr, LibAddr & laddr) const;

public:
  static void end();
  static void update();
  static void lookup(uintptr_t vaddr, LibAddr & laddr);
};

} // namespace trac
