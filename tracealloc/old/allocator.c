#include "allocator.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>


#define PTRADD(ptr, bytes) ((void *)(ptr) + (bytes))
#define PTRSUB(ptr, bytes) ((void *)(ptr) - (bytes))
// extract size residing at chunk beginning
#define SIZE(chunk) (* ((size_t *) chunk))
// extract link pointer residing one (size_t)-cell after chunk beginning
#define LINK(chunk) (* (void **) PTRADD(chunk, sizeof(size_t)))
// extract chunk pointer residing one (void *)-cell before data pointer
#define CHUNK(ptr)  (* (void **) PTRSUB(ptr, sizeof(void *)))
// determine first usable address in chunk
#define DATA(chunk) (PTRADD(chunk, OVERHEAD))


size_t align(size_t value, size_t bound)
{
  size_t mask = (1ul << (64 - __builtin_clzl(bound))) - 1;
  return (value + mask) & ~mask;
}

static void ** my_regionlist()
{
  static void * regions = NULL;
  return &regions;
}

static void ** my_freelist()
{
  static void * free = NULL;
  return &free;
}

static void * my_region(size_t * size)
{
  DEBUGMSG("=== my_region([%p]->%ld)\n", size, *size);
  // block-size according to "/sys/devices/platform/ibm,persistent-memory/of_node/ibm,pmemory\@44100001/ibm\,block-size"
  static const size_t Unit = 0x4000000;

  size_t req_size = (*size + OVERHEAD + Unit - 1) & ~(Unit - 1);
  void * region;

#ifdef USE_HMS
  static size_t offset = 0;
  static int fd = -1;
  if (fd < 0) {
    if ((fd = open("/dev/dax0.0", O_RDWR)) < 0) {
      perror("Opening hms memory device");
      return NULL;
    }
  }
  if ((region = mmap(NULL, req_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset)) == MAP_FAILED) {
    perror("Could not get new hms region");
    return NULL;
  }
  offset += req_size;
#else
  if ((region = mmap(NULL, req_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) == MAP_FAILED) {
    perror("Could not get new dram region");
    return NULL;
  }
#endif

  void ** regionlist = my_regionlist();
  SIZE(region) = req_size;
  LINK(region) = *regionlist;
  *regionlist = region;

  *size = req_size - OVERHEAD;
  return region + OVERHEAD;
}

static int my_check(void * ptr)
{
  DEBUGMSG("=== my_check([%p])\n", ptr);

  void ** cur = my_regionlist();
  while (*cur) {
    size_t size = SIZE(*cur);
    if ((size_t)ptr >= ((size_t)cur + OVERHEAD) &&
        (size_t)ptr <  ((size_t)cur + size)) {
      return 1;
    }
    cur = &LINK(*cur);
  }
  return 0;
}

static void * my_take(void ** free, size_t size, size_t bound)
// only considers the chunk pointed to by *free, returns a new allocation and links in any remaining block
{
  DEBUGMSG("=== my_take([%p], %ld, %ld)\n", free, size, bound);

  void * chunk;
  if (!free || !(chunk = *free)) {
    return NULL;
  }

  size_t capacity = SIZE(chunk);
  if (capacity < (size + OVERHEAD + bound - 1)) {
    return NULL;
  }

  void * link = LINK(chunk);
  void * ptr = ALIGN_PTR(DATA(chunk), bound);
  CHUNK(ptr) = chunk;

  size_t overhead = (size_t)ptr - (size_t)chunk;
  void * rem = ALIGN_PTR(chunk + size + overhead, BASEALIGN);
  size_t chunk_size = (size_t)rem - (size_t)chunk;
  ptrdiff_t rem_size = capacity - chunk_size;

  if (rem_size < THRESHOLD) {
    // append remaining space to allocation
    SIZE(chunk) = capacity;
    // unlink chunk
    *free = link;
  } else {
    // only allocate required space to allocation
    SIZE(chunk) = chunk_size;
    // link remaining chunk
    SIZE(rem) = rem_size;
    LINK(rem) = link;
    *free = rem;
  }

  return ptr;
}

static void * my_grow(void ** free, size_t size, size_t bound)
{
  DEBUGMSG("=== my_grow([[%p]], %ld, %ld)\n", free, size, bound);

  size_t req_size = size + OVERHEAD + bound - 1;
  void * region = my_region(&req_size);

  if (!region) {
    return NULL;
  }

  SIZE(region) = req_size;
  LINK(region) = *free;
  *free = region;

  return my_take(free, size, bound);
}

void * my_alloc(size_t size, size_t bound)
{
  DEBUGMSG("=== my_alloc(0x%lx, %ld)\n", size, bound);

  void ** cur = my_freelist();
  while (*cur) {
    void * ptr = my_take(cur, size, bound);
    if (ptr) {
      return ptr;
    }
    cur = &LINK(*cur);
  }
  return my_grow(cur, size, bound);
}

int my_free(void * ptr)
{
  DEBUGMSG("=== my_free([0x%p])\n", ptr);

  if (!my_check(ptr)) {
    return 0;
  }
  void * chunk = CHUNK(ptr);

  void ** freelist = my_freelist();
  void ** cur = freelist;
  while (*cur) {
    if (((size_t)chunk + SIZE(chunk)) == (size_t)(*cur)) {
      // chunk connects before *cur, extend chunk with *cur and unlink *cur
      SIZE(chunk) = SIZE(chunk) + SIZE(*cur);
      *cur = LINK(*cur);

      // restart loop
      cur = freelist;
    } else if (((size_t)(*cur) + SIZE(*cur)) == (size_t)chunk) {
      // chunk connects after *cur, extend *cur with chunk, set chunk to *cur and unlink
      SIZE(*cur) = SIZE(*cur) + SIZE(chunk);
      chunk = *cur;
      *cur = LINK(*cur);

      // restart loop
      cur = freelist;
    } else {
      cur = &LINK(*cur);
    }
  }
  // link (possibly expanded) chunk at the beginning
  LINK(chunk) = *freelist;
  *freelist = chunk;

  return 1;
}

void * my_realloc(void * ptr, size_t size, size_t bound)
{
  DEBUGMSG("=== my_realloc([0x%p], 0x%lx, %ld)\n", ptr, size, bound);
  void * new = my_alloc(size, bound);
  if (!new) {
    return NULL;
  }

  if (my_check(ptr)) {
    void * chunk = CHUNK(ptr);
    size_t chunk_size = SIZE(chunk);
    size_t ptr_size = chunk_size - ((size_t)ptr - (size_t)chunk);
    size_t move_size = (ptr_size < size)? ptr_size : size;
    memmove(new, ptr, move_size);

    my_free(ptr);
  }

  return new;
}

void * my_dump()
{
  void ** cur = my_freelist();
  DEBUGMSG("=== my_dump() [[%p]]\n", cur);
  while (*cur) {
    DEBUGMSG("====> [%p] (0x%lx) -> [%p]\n", *cur, SIZE(*cur), LINK(*cur));
    cur = &LINK(*cur);
  }
  DEBUGMSG("\n");
}

