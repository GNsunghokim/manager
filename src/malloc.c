#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <tlsf.h>

#include <pn_assistant.h>

extern void* __malloc_pool;	// Defined in malloc.c from libcore

#define LOCAL_MALLOC_SIZE	0x1000000 /* 16 MB */
void malloc_init() {
	__malloc_pool = pn_assistant_get_pool();
	if(!__malloc_pool) {
		printf("Malloc pool init failed. Terminated...\n");
		//TODO fix
		exit(-1);
		return;
	}

	ssize_t pool_size = pn_assistant_get_pool_size();
	init_memory_pool(pool_size, __malloc_pool, 0);
	printf("%x %p\n", pool_size, __malloc_pool);

	return;
}

size_t malloc_total() {
	return 0;
}

size_t malloc_used() {
	return 0;
}

