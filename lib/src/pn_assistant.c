#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>

#include <control/vmspec.h>

#include <pn_assistant.h>

typedef struct _Manager_Data {
	uint64_t PHYSICAL_OFFSET;
	Shmmap* vms;
} __attribute__ ((packed)) Manager_Data;

typedef struct _PN_Assistant {
	Manager_Data manager_data;
	ssize_t pool_size;
	uint8_t padding[4096 - sizeof(Manager_Data) - sizeof(ssize_t) /*pool_size*/];
	uint8_t pool[0];
} __attribute__ ((packed)) PN_Assistant;

#define SHM_SIZE	0x1000000 //16Mbyte
#define SHM_NAME	"/pn_assistant"

#define SHM_MANAGER_OFFSET	0xff000000
static PN_Assistant* pn_assistant;
// API for Manager
bool pn_assistant_init() {
	int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR , 0666);
	if(fd < 0) {
		return false;
	}

	if(ftruncate(fd, SHM_SIZE) < 0) {
		return false;
	}
	
	pn_assistant = mmap((void*)SHM_MANAGER_OFFSET, SHM_SIZE,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if(pn_assistant == MAP_FAILED) {
		return false;
	}

	memset(pn_assistant, 0, SHM_SIZE);
	pn_assistant->pool_size = SHM_SIZE - sizeof(PN_Assistant);

	return true;
}

void* pn_assistant_get_pool() {
	return pn_assistant->pool;
}

ssize_t pn_assistant_get_pool_size() {
	return pn_assistant->pool_size;
}

bool pn_assistant_set_vms(Shmmap* vms) {
	if(!pn_assistant)
		return false;

	pn_assistant->manager_data.vms = vms;

	return true;
}

bool pn_assistant_set_physical_offset(uint64_t PHYSICAL_OFFSET) {
	if(!pn_assistant)
		return false;

	pn_assistant->manager_data.PHYSICAL_OFFSET = PHYSICAL_OFFSET;

	return true;
}

// API for Application
#define MANAGER_PHYSICAL_OFFSET		0xff00000000
#define MAPPING_AREA_SIZE		0x80000000	/* 2 GB */

bool pn_assistant_load() {		//O_RDONLY
	int fd = shm_open(SHM_NAME, O_RDONLY, 0666);
	if(fd < 0) {
		return false;
	}
	
	pn_assistant = mmap((void*)SHM_MANAGER_OFFSET, SHM_SIZE,
			PROT_READ, MAP_SHARED, fd, 0);

	if(pn_assistant == MAP_FAILED) {
		return false;
	}

  	fd = open("/dev/mem", O_RDONLY | O_SYNC);
  	if(fd < 0) {
		printf("mem open fail\n");
		return false;
  	}
  	void* mapping = mmap((void*)MANAGER_PHYSICAL_OFFSET + 0x100000,
  			MAPPING_AREA_SIZE, PROT_READ,
  			MAP_SHARED, fd, (off_t)pn_assistant->manager_data.PHYSICAL_OFFSET + 0x100000);
 
  	if(mapping == MAP_FAILED) {
 		printf("Mapping Fail\n");
  		close(fd);
		return false;
  	}
 
  	if(mapping != (void*)(MANAGER_PHYSICAL_OFFSET + 0x100000)) {
  		printf("Mapping Fail %p\n", mapping);
 		close(fd);
		return false;
  	}

	return true;
}

static void** get_memory_blocks(VM* vm) {
	return (void**)((uint64_t)MANAGER_PHYSICAL_OFFSET | (uint64_t)vm->memory.blocks);
}

static VM* pn_assistant_get_vm(int vmid) {
	if(!pn_assistant)
		return NULL;

	VM* vm = shmmap_get(pn_assistant->manager_data.vms, (void*)(uint64_t)vmid);
	if(!vm)
		return NULL;

	vm = (VM*)((uint64_t)vm | MANAGER_PHYSICAL_OFFSET);

	return vm;
}

//TODO First check this fucntion

typedef struct {
	uint8_t		gmalloc_lock;
	uint8_t		barrior_lock;
	uint32_t	barrior;
	void*		shared;
} SharedBlock;

void pn_assistant_dump_vm(int vmid) {
	VM* vm = pn_assistant_get_vm(vmid);
	if(!vm)
		return;

	printf("\n**** RTVM Information ****\n");
 	printf("VMID:\t\t%d\n", vm->id);
 	printf("Core Size: %d\n", vm->core_size);
	printf("Cores:\t\t");
	for(int i = 0; i < vm->core_size; i++)
		printf("[%d]", vm->cores[i]);
	printf("\n");
}

int pn_assistant_get_vm_status(int vmid) {
	VM* vm = pn_assistant_get_vm(vmid);
	if(!vm)
		return VM_STATUS_INVALID;

	return vm->status;
}

bool pn_assistant_mapping_global_heap(int vmid) {	//mapping memory pool
	VM* vm = pn_assistant_get_vm(vmid);
	if(!vm)
		return false;

 	void** memory = get_memory_blocks(vm);
  	int fd = open("/dev/mem", O_RDWR | O_SYNC);
  	if(fd < 0) {
		printf("mem open fail\n");
		return false;
  	}

 	for(uint32_t i = 0; i < vm->memory.count; i++) {
		if(((uint64_t)(memory[i] + pn_assistant->manager_data.PHYSICAL_OFFSET) >> 21) < vm->global_heap_idx)
			continue;

		// TODO fix here
		// Code, Data, Stack size fixed 0x200000
		// Calculate offset
		void* mapping = mmap((void*)0xe00000 + 0x200000 * (i - 3),
				0x200000, PROT_READ | PROT_WRITE,
				MAP_SHARED, fd, (off_t)(memory[i] + pn_assistant->manager_data.PHYSICAL_OFFSET));
		printf("Virtual memory map: %dMB -> %dMB Global Heap\n", (0xe00000 + 0x200000 * (i - 3)) / 0x100000, ((off_t)(memory[i] + pn_assistant->manager_data.PHYSICAL_OFFSET)) / 0x100000);

		if(mapping == MAP_FAILED) {
			printf("Mapping Fail\n");
			//TODO munmap
			close(fd);
			return false;
		}

		if(mapping != (void*)0xe00000 + 0x200000 * (i - 3)) {
			printf("Mapping Fail\n");
			close(fd);
			return false;
		}
 	}

	close(fd);
	return true;
}

void* pn_assistant_get_gmalloc_pool(int vmid) {
	VM* vm = pn_assistant_get_vm(vmid);
	if(!vm)
		return NULL;

 	void** memory = get_memory_blocks(vm);
 	for(uint32_t i = 0; i < vm->memory.count; i++) {
		if(((uint64_t)(memory[i] + pn_assistant->manager_data.PHYSICAL_OFFSET) >> 21) < vm->global_heap_idx)
			continue;

		// TODO fix here
		// Code, Data, Stack size fixed 0x200000
		// Calculate offset
		return (void*)0xe00000 + 0x200000 * (i - 3) + sizeof(SharedBlock);
 	}

	return NULL;
}

void* pn_assistant_get_shared(int vmid) {
	VM* vm = pn_assistant_get_vm(vmid);
	if(!vm)
		return NULL;

 	void** memory = get_memory_blocks(vm);
 	for(uint32_t i = 0; i < vm->memory.count; i++) {
		if(((uint64_t)(memory[i] + pn_assistant->manager_data.PHYSICAL_OFFSET) >> 21) < vm->global_heap_idx)
			continue;

		// TODO fix here
		// Code, Data, Stack size fixed 0x200000
		// Calculate offset
		SharedBlock* shared_block = (void*)0xe00000 + 0x200000 * (i - 3);
		return shared_block->shared;
 	}

	return NULL;
}

int pn_assistant_get_vm_id(char* name) {
	if(!pn_assistant)
		return -1;

// 	Map_iterator_init(&iter, pn_assistant->vms);
// 	while(map_iterator_has_next(&iter)) {
// 		VM* vm = map_iterator_next(&iter);
// 		if(!strcmp(name, vm->name))
// 			return vm->id;
// 	}
	return -1;
}
