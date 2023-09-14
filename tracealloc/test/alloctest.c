#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>


void printSize(size_t size)
{
  static char * suffixes[] = { "  B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
  const size_t mask = 0x3ff;
  int index = 0;
  while (size && !(size & mask)) {
    size >>= 10;
    index += 1;
  }
  printf("% 5ld %s", size, suffixes[index]);
}

int main(int argc, char *argv[])
{
  //printf("alloc: startup\n");
  size_t positions = 16;
  size_t iterations = 16;

  if (argc > 1) {
    positions = strtoul(argv[1], NULL, 0);
    iterations = positions;
    if (!positions) {
      return -1;
    }
  }
  if (argc > 2) {
    iterations = strtoul(argv[2], NULL, 0);
    if (!iterations) {
      return -1;
    }
  }
  //printf("alloc: positions=%ld iterations=%ld\n", positions, iterations);

  void ** allocs = alloca(positions * sizeof(void *));
  //printf("alloc: allocs=[[0x%p]]\n", allocs);
  for (size_t i = 0; i < positions; ++i) {
    allocs[i] = NULL;
  }


  //printf("alloc: srand\n");
  srand(time(NULL));

  //printf("alloc: begin\n");
  for (int i = 0; i < iterations; ++i) {
    int pos = rand() % positions;
    //printf("alloc: pos=%d\n", pos);
    if (allocs[pos]) {
      printf("> Free(% 3d) @0x%p\n", pos, allocs[pos]);
      free(allocs[pos]);
      allocs[pos] = NULL;
    } else {
      int size_code = rand();
      size_t size = ((size_t)size_code & 0xf) << ((size_code >> 2) & 0x1c);
      size = (size < sizeof(size_t))? sizeof(size_t) : size;
      allocs[pos] = malloc(size);
      printf("> Alloc(% 3d, ", pos);
      printSize(size);
      printf(") @0x%p\n", allocs[pos]);
    }
  }
}

