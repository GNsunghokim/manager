#ifndef __VM_H__ 
#define __VM_H__

#include <sys/types.h>
#include <control/vmspec.h>
//#include "vfio.h"
#include "mp.h"
#include "vnic.h"

#define EVENT_VM_STARTED    	0x0200000000000001
#define EVENT_VM_PAUSED	    	0x0200000000000002
#define EVENT_VM_RESUMED	    0x0200000000000003
#define EVENT_VM_STOPPED	    0x0200000000000004

#define VM_MEMORY_SIZE_ALIGN	0x200000
#define VM_STORAGE_SIZE_ALIGN	0x200000

#define MAX_VM_COUNT            128
typedef struct {
	uint32_t	count;
	void**		blocks;	// gmalloc(array), bmalloc(content)
} Block;

typedef struct _VM {
	uint32_t	id;
	int		core_size;
	uint8_t		cores[MP_MAX_CORE_COUNT];
	Block		memory;
	Block		storage;
	int		used_size;
	int		nic_count;
	VNIC**		nics;	// gmalloc, ni_create
	//	VFIO*		fio;

	int		argc;
	char**		argv;	// gmalloc

	int		status;

	uint32_t	global_heap_idx;
} __attribute__ ((packed)) VM;

void vm_init();
void vm_init();

uint32_t vm_create(VMSpec* vm_spec);
bool vm_destroy(uint32_t vmid);
int vm_count();
bool vm_contains(uint32_t vmid);
int vm_list(uint32_t* vmids, int size);

typedef void(*VM_STATUS_CALLBACK)(bool, void*);
void vm_status_set(uint32_t vmid, int status, VM_STATUS_CALLBACK callback, void* context);
int vm_status_get(uint32_t vmid);
VM* vm_get(uint32_t vmid);

ssize_t vm_storage_read(uint32_t vmid, void** buf, size_t offset, size_t size);
ssize_t vm_storage_write(uint32_t vmid, void* buf, size_t offset, size_t size);
ssize_t vm_storage_clear(uint32_t vmid);
bool vm_storage_md5(uint32_t vmid, uint32_t size, uint32_t digest[4]);
ssize_t vm_stdio(uint32_t vmid, int thread_id, int fd, const char* str, size_t size);
typedef void(*VM_STDIO_CALLBACK)(uint32_t vmid, int thread_id, int fd, char* buffer, volatile size_t* head, volatile size_t* tail, size_t size);
void vm_stdio_handler(VM_STDIO_CALLBACK callback);

#endif /* __VM_H__ */
