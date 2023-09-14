
LIBUNWINDFLAGS = -I/hpi/fs00/home/lukas.wenzel/build_ppc/include -L/hpi/fs00/home/lukas.wenzel/build_ppc/lib -Wl,-Bstatic -lunwind -Wl,-Bdynamic
all: tracealloc_dram.so tracealloc_hms.so tracealloc_dbg.so alloctest

alloctest: alloctest.c
	gcc -g -o alloctest alloctest.c

tracealloc_dram.so: tracealloc.c allocator.c allocator.h
	gcc -fpic -rdynamic -shared -lpthread $(LIBUNWINDFLAGS) -o tracealloc_dram.so tracealloc.c allocator.c

tracealloc_hms.so: tracealloc.c allocator.c allocator.h
	gcc -fpic -rdynamic -shared -lpthread $(LIBUNWINDFLAGS) -o tracealloc_hms.so -DUSE_HMS tracealloc.c allocator.c

tracealloc_dbg.so: tracealloc.c allocator.c allocator.h
	gcc -g -fpic -rdynamic -shared -lpthread $(LIBUNWINDFLAGS) -o tracealloc_dbg.so -DDEBUG tracealloc.c allocator.c

.PHONY: clean
clean:
	rm -f alloctest
	rm -f tracealloc_dram.so
	rm -f tracealloc_hms.so
	rm -f tracealloc_dbg.so


