#include "mappings.hpp"

#include <unistd.h>


namespace trac
{

Mappings::Entry::Entry(size_t index, size_t base, size_t size, size_t offset)
: index {index}
, base {base}
, size {size}
, offset {offset}
{ }

Mappings * Mappings::s_instance = nullptr;
pthread_rwlock_t Mappings::s_lock;
pthread_once_t Mappings::s_lockInit = PTHREAD_ONCE_INIT;

void Mappings::initLock()
{
  pthread_rwlock_init(&s_lock, nullptr);
}

Mappings::Mappings()
: m_libs()
, m_entries()
, m_log(nullptr)
{
  char * logpath = getenv("TRAC_LOGPATH");
  if (logpath) {
    char logfilename[256];
    snprintf(logfilename, sizeof(logfilename), "%s/maps.log", logpath);
    m_log = fopen(logfilename, "w");
  }
  doUpdate();
}

Mappings::~Mappings()
{
  if (m_log) {
    fclose(m_log);
  }
}

void Mappings::doUpdate()
{
  m_entries.clear();
  dl_iterate_phdr(&updateCallback, this);
}

int Mappings::updateCallback(struct dl_phdr_info * info, size_t size, void * data)
{
  Mappings * obj = (Mappings *)data;
  size_t index = obj->getIndex(info->dlpi_name);
  // printf("[%ld] @%lx %s:\n", index, info->dlpi_addr, info->dlpi_name);
  for (size_t i = 0; i < info->dlpi_phnum; ++i) {
    // printf(" > %lx @%lx (%lx)\n", info->dlpi_phdr[i].p_vaddr, info->dlpi_phdr[i].p_offset, info->dlpi_phdr[i].p_memsz);
    if (info->dlpi_phdr[i].p_type == PT_LOAD) {
      obj->putMapping(index,
                      info->dlpi_addr + info->dlpi_phdr[i].p_vaddr,
                      info->dlpi_phdr[i].p_memsz,
                      info->dlpi_phdr[i].p_offset);
    }
  }
  return 0;
}

size_t Mappings::getIndex(const char * filename)
{
  auto it = m_libs.find(filename);
  if (it != m_libs.end()) {
    return it->second;
  } else {
    size_t idx = m_libs.size() + 1;
    m_libs.emplace(filename, idx);
    if (m_log) {
      fprintf(m_log, "%ld: %s\n", idx, filename);
    }
    return idx;
  }
}

void Mappings::putMapping(size_t index, uintptr_t base, size_t size, size_t offset)
{
  // printf("MAP %lx to %lx   [%ld @%lx]\n", base, base+size, index, offset);
  m_entries.emplace(base, Entry(index, base, size, offset));
}


void Mappings::doLookup(uintptr_t vaddr, Mappings::LibAddr & laddr) const
{
  auto it = m_entries.upper_bound(vaddr);
  if (it == m_entries.cbegin()) {
    laddr.index = 0;
    laddr.offset = vaddr;
  } else {
    --it;
    size_t rel = vaddr - it->second.base;
    if (rel < it->second.size) {
      laddr.index = it->second.index;
      laddr.offset = rel + it->second.offset;
    } else {
      laddr.index = 0;
      laddr.offset = vaddr;
    }
  }
}


//void Mappings::setup()
//{
//  pthread_rwlock_wrlock(&s_lock);
//  if (!s_instance) {
//    s_instance = new Mappings();
//  }
//  pthread_rwlock_unlock(&s_lock);
//}

void Mappings::end()
{
  auto tid = gettid();
  pthread_once(&s_lockInit, initLock);
  //printf("[%09d] end() - wrlock(%p);\n", tid, &s_lock);
  pthread_rwlock_wrlock(&s_lock);
  if (s_instance) {
    delete s_instance;
    s_instance = nullptr;
  }
  //printf("[%09d] end() - unlock(%p);\n", tid, &s_lock);
  pthread_rwlock_unlock(&s_lock);
}

void Mappings::update()
{
  auto tid = gettid();
  pthread_once(&s_lockInit, initLock);
  //printf("[%09d] update() - wrlock(%p);\n", tid, &s_lock);
  pthread_rwlock_wrlock(&s_lock);
  if (!s_instance) {
    s_instance = new Mappings();
  }
  s_instance->doUpdate();
  //printf("[%09d] update() - unlock(%p);\n", tid, &s_lock);
  pthread_rwlock_unlock(&s_lock);
}

void Mappings::lookup(uintptr_t vaddr, Mappings::LibAddr & laddr)
{
  auto tid = gettid();
  pthread_once(&s_lockInit, initLock);
  //printf("[%09d] lookup() - rdlock(%p);\n", tid, &s_lock);
  pthread_rwlock_rdlock(&s_lock);
  if (!s_instance) {
    //printf("[%09d] lookup() - unlock(%p);\n", tid, &s_lock);
    pthread_rwlock_unlock(&s_lock);
    //printf("[%09d] lookup() - wrlock(%p);\n", tid, &s_lock);
    pthread_rwlock_wrlock(&s_lock);
    if (!s_instance) {
      s_instance = new Mappings();
    }
  }
  s_instance->doLookup(vaddr, laddr);
  //printf("[%09d] lookup() - unlock(%p);\n", tid, &s_lock);
  pthread_rwlock_unlock(&s_lock);
}


// std::map<uintptr_t, Entry> * g_entries = nullptr;
//void setup()
//{
//  g_entries = new std::map<uintptr_t, Entry>();
//  FILE * mapsfile = fopen("/proc/self/maps", "r");
//  FILE * logfile = nullptr;
//  char * logpath = getenv("TRACEALLOC_PATH");
//  if (logpath) {
//    char logfilename[256];
//    snprintf(logfilename, sizeof(logfilename), "%s/maps.log", logpath);
//    logfile = fopen(logfilename, "w");
//  }
//  if (mapsfile) {
//    char buf[256];
//    char fbuf[256];
//    char * line;
//    while (line = fgets(buf, sizeof(buf), mapsfile)) {
//      size_t base, end, offset;
//      int fields = sscanf(line, "%lx-%lx %*[-rwxp] %lx %*[0123456789:] %*[0123456789] %512s", &base, &end, &offset, fbuf);
//      const char * file = (fields > 3)? fbuf : "[ANON]";
//      size_t index = g_entries->size() + 1;
//      g_entries->emplace(base, Entry(index, base, end - base, offset, file));
//      if (logfile) {
//        fprintf(logfile, "%ld,%lx,%lx,%lx,%s\n", index, base, end, offset, file);
//      }
//    }
//    if (logfile) {
//      fclose(logfile);
//    }
//    fclose(mapsfile);
//  }
//}


} // namespace trac
