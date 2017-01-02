#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <malloc.h>
#include <util/event.h>
#include <errno.h>
#include <util/map.h>
#include <util/ring.h>
#include <net/md5.h>
#include <timer.h>
#include <file.h>

#include <pn_assistant.h>

#include "icc.h"
#include "vm.h"
#include "apic.h"
#include "icc.h"
#include "gmalloc.h"
#include "stdio.h"
#include "shared.h"
#include "mmap.h"

static uint32_t	last_vmid = 1;
// FIXME: change to static
Map*	vms;

// Core status
typedef struct {
	int			status;		// VM_STATUS_XXX
	int			error_code;
	int			return_code;
	
	VM*			vm;
	
	char*			stdin;
	volatile size_t*	stdin_head;
	volatile size_t*	stdin_tail;
	size_t			stdin_size;
	
	char*			stdout;
	volatile size_t*	stdout_head;
	volatile size_t*	stdout_tail;
	size_t			stdout_size;
	
	char*			stderr;
	volatile size_t*	stderr_head;
	volatile size_t*	stderr_tail;
	size_t			stderr_size;
} Core;

static Core cores[MP_MAX_CORE_COUNT];

static VM_STDIO_CALLBACK stdio_callback;

static void icc_started(ICC_Message* msg) {
	Core* core = &cores[msg->apic_id];
	VM* vm = core->vm;

	if(msg->result == 0) {
		core->error_code = 0;

		core->stdin = (char*)((uint64_t)msg->data.started.stdin - PHYSICAL_OFFSET);
		core->stdin_head = (size_t*)((uint64_t)msg->data.started.stdin_head - PHYSICAL_OFFSET);
		core->stdin_tail = (size_t*)((uint64_t)msg->data.started.stdin_tail - PHYSICAL_OFFSET);
		core->stdin_size = msg->data.started.stdin_size;

		core->stdout = (char*)((uint64_t)msg->data.started.stdout - PHYSICAL_OFFSET);
		core->stdout_head = (size_t*)((uint64_t)msg->data.started.stdout_head - PHYSICAL_OFFSET);
		core->stdout_tail = (size_t*)((uint64_t)msg->data.started.stdout_tail - PHYSICAL_OFFSET);
		core->stdout_size = msg->data.started.stdout_size;

		core->stderr = (char*)((uint64_t)msg->data.started.stderr - PHYSICAL_OFFSET);
		core->stderr_head = (size_t*)((uint64_t)msg->data.started.stderr_head - PHYSICAL_OFFSET);
		core->stderr_tail = (size_t*)((uint64_t)msg->data.started.stderr_tail - PHYSICAL_OFFSET);
		core->stderr_size = msg->data.started.stderr_size;
		
		core->status = VM_STATUS_START;

		if(!vm->global_heap_idx)
			vm->global_heap_idx = msg->data.started.global_heap_idx;
		
		printf("Execution succeed on core[%d].\n", mp_apic_id_to_core_id(msg->apic_id));
		
	} else {
		core->error_code = msg->result;
		
		core->status = VM_STATUS_PAUSE;
		
		printf("Execution FAILED on core[%d]: Error code 0x%x.\n", mp_apic_id_to_core_id(msg->apic_id), msg->result);
		
	}

	icc_free(msg);

	int error_code = 0;
	for(int i = 0; i < vm->core_size; i++) {
		core = &cores[vm->cores[i]];
		
		if(core->status == VM_STATUS_START) {
			continue;
		}
		
		if(core->error_code != 0) {
			error_code = core->error_code;
			continue;
		}
		
		return;
	}
	
	vm->status = error_code == 0 ? VM_STATUS_START : VM_STATUS_STOP;
	
	event_trigger_fire(EVENT_VM_STARTED, vm, NULL, NULL);
	
	if(error_code != 0) {
		for(int i = 0; i < vm->core_size; i++) {
			if(cores[vm->cores[i]].status == VM_STATUS_START) {
				ICC_Message* msg2 = icc_alloc(ICC_TYPE_STOP);
				icc_send(msg2, vm->cores[i]);
			}
		}
	}
	
	if(error_code == 0) {
		printf("VM started on cores[");
		for(int i = 0; i < vm->core_size; i++) {
			printf("%d", mp_apic_id_to_core_id(vm->cores[i]));
			if(i + 1 < vm->core_size) {
				printf(", ");
			}
		}
		printf("]\n");
	} else {
		printf("VM started with error(s) on cores[");
		for(int i = 0; i < vm->core_size; i++) {
			printf("%d", mp_apic_id_to_core_id(vm->cores[i]));
			error_code = cores[vm->cores[i]].error_code;
			if(error_code != 0)
				printf("(0x%x)", error_code);
			
			if(i + 1 < vm->core_size) {
				printf(", ");
			}
		}
		printf("]\n");
	}
}

static void icc_paused(ICC_Message* msg) {
	VM* vm = cores[msg->apic_id].vm;
	
	cores[msg->apic_id].status = VM_STATUS_PAUSE;
	
	printf("Execution paused on core[%d].\n", mp_apic_id_to_core_id(msg->apic_id));
	
	icc_free(msg);
	for(int i = 0; i < vm->core_size; i++) {
		if(cores[vm->cores[i]].status != VM_STATUS_PAUSE)
			return;
	}
	
	vm->status = VM_STATUS_PAUSE;
	
	event_trigger_fire(EVENT_VM_PAUSED, vm, NULL, NULL);
	
	printf("VM paused on cores[");
	for(int i = 0; i < vm->core_size; i++) {
		printf("%d", mp_apic_id_to_core_id(vm->cores[i]));
		if(i + 1 < vm->core_size) {
			printf(", ");
		}
	}
	printf("]\n");
}

static void icc_resumed(ICC_Message* msg) {
	if(msg->result == -1000) {	// VM is not strated yet
		ICC_Message* msg2 = icc_alloc(ICC_TYPE_RESUME);
		icc_send(msg2, msg->apic_id);
		icc_free(msg);
		return;
	}

	VM* vm = cores[msg->apic_id].vm;
	
	cores[msg->apic_id].status = VM_STATUS_START;
	
	printf("Execution resumed on core[%d].\n", mp_apic_id_to_core_id(msg->apic_id));
	
	icc_free(msg);
	
	for(int i = 0; i < vm->core_size; i++) {
		if(cores[vm->cores[i]].status != VM_STATUS_START)
			return;
	}
	
	vm->status = VM_STATUS_START;
	
	event_trigger_fire(EVENT_VM_RESUMED, vm, NULL, NULL);
	
	printf("VM resumed on cores[");
	for(int i = 0; i < vm->core_size; i++) {
		printf("%d", mp_apic_id_to_core_id(vm->cores[i]));
		if(i + 1 < vm->core_size) {
			printf(", ");
		}
	}
	printf("]\n");
}

static void icc_stopped(ICC_Message* msg) {
	if(msg->result == -1000) {	// VM is not strated yet
		ICC_Message* msg2 = icc_alloc(ICC_TYPE_STOP);
		icc_send(msg2, msg->apic_id);
		icc_free(msg);
		// Resend stop icc
		return;
	}

	VM* vm = cores[msg->apic_id].vm;
	
	cores[msg->apic_id].status = VM_STATUS_PAUSE;
	cores[msg->apic_id].error_code = msg->result;
	cores[msg->apic_id].return_code = msg->data.stopped.return_code;
	cores[msg->apic_id].stdin = NULL;
	cores[msg->apic_id].stdout = NULL;
	cores[msg->apic_id].stderr = NULL;
	
	printf("Execution completed on core[%d].\n", mp_apic_id_to_core_id(msg->apic_id));
	
	icc_free(msg);

	for(int i = 0; i < vm->core_size; i++) {
		if(cores[vm->cores[i]].status != VM_STATUS_PAUSE) {
			return;
		}
	}
	
	vm->status = VM_STATUS_STOP;
	
	event_trigger_fire(EVENT_VM_STOPPED, vm, NULL, NULL);
	
	printf("VM stopped on cores[");
	for(int i = 0; i < vm->core_size; i++) {
		printf("%d(%d/%d)", mp_apic_id_to_core_id(vm->cores[i]), cores[vm->cores[i]].error_code, cores[vm->cores[i]].return_code);
		if(i + 1 < vm->core_size) {
			printf(", ");
		}
	}
	printf("]\n");
}

static bool vm_delete(VM* vm, int core) {
	bool is_destroy = true;
	
	if(core == -1) {
		for(int i = 0; i < vm->core_size; i++) {
			if(vm->cores[i]) {
				cores[vm->cores[i]].status = VM_STATUS_STOP;
			}
		}
	} else {
		for(int i = 0; i < vm->core_size; i++) {
			if(vm->cores[i] == core) {
				cores[vm->cores[i]].status = VM_STATUS_STOP;
			}
			
			if(!cores[vm->cores[i]].status == VM_STATUS_STOP)
				is_destroy = false;
		}
	}
	
	if(is_destroy) {
		if(vm->memory.blocks) {
			for(uint32_t i = 0; i < vm->memory.count; i++) {
				if(vm->memory.blocks[i]) {
					bfree(vm->memory.blocks[i]);
				}
			}
			
			gfree(vm->memory.blocks);
		}
		
		if(vm->storage.blocks) {
			for(uint32_t i = 0; i < vm->storage.count; i++) {
				if(vm->storage.blocks[i]) {
					bfree(vm->storage.blocks[i]);
				}
			}
			
			gfree(vm->storage.blocks);
		}
		
		if(vm->nics) {
			for(uint16_t i = 0; i < vm->nic_count; i++) {
				if(vm->nics[i])
					vnic_destroy(vm->nics[i]);
			}
			
			gfree(vm->nics);
		}
		
		if(vm->argv) {
			gfree(vm->argv);
		}
		
		gfree(vm);
	}
	
	return is_destroy;
}

static bool vm_loop(void* context) {
	// Standard I/O/E processing
	int get_thread_id(VM* vm, int core) {
		for(int i = 0; i < vm->core_size; i++) {
			if(vm->cores[i] == core)
				return i;
		}
		
		return -1;
	}
	
	for(int i = 1; i < MP_MAX_CORE_COUNT; i++) {
		Core* core = &cores[i];
		
		if(core->status != VM_STATUS_PAUSE && core->status != VM_STATUS_START) {
			continue;
		}
		int thread_id = -1;
	/*
	 *
	 *        if(core->stdout == NULL) {
	 *                printf("stdout NULL\n");
	 *                return true;
	 *        }
	 *        printf("Core stdout : %p (%p), head : %d (%p) tail : %d (%p) \n",
	 *                        *core->stdout, core->stdout,
	 *                        *core->stdout_head, core->stdout_head,
	 *                        *core->stdout_tail, core->stdout_tail);
	 */
		if(core->stdout != NULL && *core->stdout_head != *core->stdout_tail) {
			//printf("Something printed\n");
			thread_id = get_thread_id(core->vm, i);
			
			stdio_callback(core->vm->id, thread_id, 1, core->stdout, core->stdout_head, core->stdout_tail, core->stdout_size);
		}
		
		if(core->stderr != NULL && *core->stderr_head != *core->stderr_tail) {
			if(thread_id == -1)
				thread_id = get_thread_id(core->vm, i);
			
			stdio_callback(core->vm->id, thread_id, 2, core->stderr, core->stderr_head, core->stderr_tail, core->stderr_size);
		}
	}
	
	/*void stdio_dump(int coreno, int fd, char* buffer, volatile size_t* head, volatile size_t* tail, size_t size);*/
	
	/*for(int i = 1; i < MP_MAX_CORE_COUNT; i++) {*/
		/*if(cores[i].status == VM_STATUS_INVALID)*/
			/*continue;*/


		/*char* buffer = *(shared->__stdout); //(char*)MP_CORE(__stdout, i);*/
		/*volatile size_t* head = *(shared->__stdout_head); //(size_t*)MP_CORE(&__stdout_head, i);*/
		/*volatile size_t* tail = *(shared->__stdout_tail);//(size_t*)MP_CORE(&__stdout_tail, i);*/
		/*size_t size = *(shared->__stdout_size);// *(size_t*)MP_CORE(&__stdout_size, i);*/

		/*while(*head != *tail) {*/
			/*stdio_dump(mp_apic_id_to_core_id(i), 1, buffer, head, tail, size);*/
		/*}*/
		
		/*
		 *buffer = (char*)MP_CORE(__stderr, i);
		 *head = (size_t*)MP_CORE(&__stderr_head, i);
		 *tail = (size_t*)MP_CORE(&__stderr_tail, i);
		 *size = *(size_t*)MP_CORE(&__stderr_size, i);
		 *
		 *while(*head != *tail) {
		 *        stdio_dump(mp_apic_id_to_core_id(i), 2, buffer, head, tail, size);
		 *}
		 */
	/*}*/
	
	return true;
}

void vm_init() {
	//vms = map_create(4, map_uint64_hash, map_uint64_equals, NULL);
	vms = map_create(4, NULL, NULL, NULL);
	
	icc_register(ICC_TYPE_STARTED, icc_started);
	icc_register(ICC_TYPE_PAUSED, icc_paused);
	icc_register(ICC_TYPE_RESUMED, icc_resumed);
	icc_register(ICC_TYPE_STOPPED, icc_stopped);
	
	// Core 0 is occupied by RPC manager
	cores[0].status = VM_STATUS_START;

	uint8_t* core_map = mp_core_map();
	for(int i = 1; i < MP_MAX_CORE_COUNT; i++) {
		if(core_map[i] == MP_CORE_INVALID)
			cores[i].status = VM_STATUS_INVALID;	// Disable the core
	}

	event_idle_add(vm_loop, NULL);

	pn_assistant_set_vms(vms);
}

uint32_t vm_create(VMSpec* vm_spec) {
	VM* vm = gmalloc(sizeof(VM));
	memset(vm, 0, sizeof(VM));

	// Allocate args
	int argv_len = sizeof(char*) * vm_spec->argc;
	for(int i = 0; i < vm_spec->argc; i++) {
		argv_len += strlen(vm_spec->argv[i]) + 1;
	}
	vm->argv = gmalloc(argv_len);
	vm->argc = vm_spec->argc;
	char* args = (void*)vm->argv + sizeof(char*) * vm_spec->argc;
	for(int i = 0; i < vm_spec->argc; i++) {
		vm->argv[i] = args;
		int len = strlen(vm_spec->argv[i]) + 1;
		memcpy(args, vm_spec->argv[i], len);
		args += len;
	}
	
	// Allocate core
	vm->core_size = vm_spec->core_size;
	
	int j = 0;
	for(int i = 1; i < MP_MAX_CORE_COUNT; i++) {
		if(cores[i].status == VM_STATUS_STOP) {
			vm->cores[j++] = i;
			cores[i].status = VM_STATUS_PAUSE;
			cores[i].vm = vm;
			
			if(j >= vm->core_size)
				break;
		}
	}
	
	if(j < vm->core_size) {
		printf("Manager: Not enough core to allocate.\n");
		vm_delete(vm, -1);
		return 0;
	}
	
	// Allocate memory
	uint32_t memory_size = vm_spec->memory_size;
	memory_size = (memory_size + (VM_MEMORY_SIZE_ALIGN - 1)) & ~(VM_MEMORY_SIZE_ALIGN - 1);
	vm->memory.count = memory_size / VM_MEMORY_SIZE_ALIGN;
	vm->memory.blocks = gmalloc(vm->memory.count * sizeof(void*));
	memset(vm->memory.blocks, 0x0, vm->memory.count * sizeof(void*));
	for(uint32_t i = 0; i < vm->memory.count; i++) {
		vm->memory.blocks[i] = bmalloc();
		if(!vm->memory.blocks[i]) {
			printf("Manager: Not enough memory to allocate.\n");
			vm_delete(vm, -1);
			return 0;
		}
	}

	// Allocate storage
	uint32_t storage_size = vm_spec->storage_size;
	storage_size = (storage_size + (VM_STORAGE_SIZE_ALIGN - 1)) & ~(VM_STORAGE_SIZE_ALIGN - 1);
	vm->storage.count = storage_size / VM_STORAGE_SIZE_ALIGN;
	vm->storage.blocks = gmalloc(vm->storage.count * sizeof(void*));
	memset(vm->storage.blocks, 0x0, vm->storage.count * sizeof(void*));
	for(uint32_t i = 0; i < vm->storage.count; i++) {
		vm->storage.blocks[i] = bmalloc();
		if(i == 0)
			printf("Storage block : %p\n", vm->storage.blocks[i]);

		if(!vm->storage.blocks[i]) {
			printf("Manager: Not enough storage to allocate.\n");
			vm_delete(vm, -1);
			return 0;
		}
	}
	
	// Allocate vmid
	uint32_t vmid;
	while(true) {
		vmid = last_vmid++;

		if(vmid != 0 && !map_contains(vms, (void*)(uint64_t)vmid)) {
			vm->id = vmid;
			map_put(vms, (void*)(uint64_t)vmid, vm);
			break;
		}
	}

	// Allocate VNICs
	NICSpec* nics = vm_spec->nics;
	vm->nic_count = vm_spec->nic_count;
	vm->nics = gmalloc(sizeof(VNIC) * vm->nic_count);
	memset(vm->nics, 0x0, sizeof(VNIC) * vm->nic_count);
	
	for(int i = 0; i < vm->nic_count; i++) {
		uint64_t mac = nics[i].mac;
		if(mac == 0) {
			do {
				mac = timer_frequency() & 0x0fffffffffffL;
				mac |= 0x02L << 40;	// Locally administrered
			} while(vnic_contains(nics[i].dev, mac));
		} else if(vnic_contains(nics[i].dev, mac)) {
			printf("Manager: The mac address already in use: %012lx.\n", mac);
			map_remove(vms, (void*)(uint64_t)vmid);
			vm_delete(vm, -1);
			return 0;
		}
		
		uint64_t attrs[] = {
			NIC_MAC, mac,
			NIC_DEV, (uint64_t)nics[i].dev,
			NIC_INPUT_BUFFER_SIZE, nics[i].input_buffer_size,
			NIC_OUTPUT_BUFFER_SIZE, nics[i].output_buffer_size,
			NIC_INPUT_BANDWIDTH, nics[i].input_bandwidth,
			NIC_OUTPUT_BANDWIDTH, nics[i].output_bandwidth,
			NIC_PADDING_HEAD, nics[i].padding_head ? nics[i].padding_head : 32,
			NIC_PADDING_TAIL, nics[i].padding_tail ? nics[i].padding_tail : 32,
			NIC_POOL_SIZE, nics[i].pool_size,
			NIC_INPUT_ACCEPT_ALL, 1,
			NIC_OUTPUT_ACCEPT_ALL, 1,
			NIC_INPUT_FUNC, 0,
			NIC_NONE
		};
		
		vm->nics[i] = vnic_create(attrs);
		if(!vm->nics[i]) {
			printf("Manager: Not enough VNIC to allocate: errno=%d.\n", errno);
			map_remove(vms, (void*)(uint64_t)vmid);
			vm_delete(vm, -1);
			return 0;
		}
	}

	// Dump info
	printf("Manager: VM[%d] created(cores [", vmid);
	for(int i = 0; i < vm->core_size; i++) {
		printf("%d", mp_apic_id_to_core_id(vm->cores[i]));
		if(i + 1 < vm->core_size)
			printf(", ");
	}
	printf("], %dMBs memory, %dMBs storage, VNICs: %d\n",
		vm->memory.count * VM_MEMORY_SIZE_ALIGN / 0x100000,
		vm->storage.count * VM_STORAGE_SIZE_ALIGN / 0x100000, vm->nic_count);
	
	for(int i = 0; i < vm->nic_count; i++) {
		VNIC* nic = vm->nics[i];
		printf("\t");
		for(int j = 5; j >= 0; j--) {
			printf("%02lx", (nic->mac >> (j * 8)) & 0xff);
			if(j - 1 >= 0)
				printf(":");
			else
				printf(" ");
		}
		printf("%ldMbps/%ld, %ldMbps/%ld, %ldMBs\n", nic->input_bandwidth / 1000000, fifo_capacity(nic->nic->input_buffer) + 1, nic->output_bandwidth / 1000000, fifo_capacity(nic->nic->output_buffer) + 1, list_size(nic->pools) * 2); }
	
	printf("\targs(%d): ", vm->argc);
	for(int i = 0; i < vm->argc; i++) {
		char* ch = strchr(vm->argv[i], ' ');
		if(ch == NULL)
			printf("%s ", vm->argv[i]);
		else
			printf("\"%s\" ", vm->argv[i]);
	}
	printf("\n");

	return vmid;
}

bool vm_destroy(uint32_t vmid) {
	VM* vm = map_get(vms, (void*)(uint64_t)vmid);
	if(!vm) {
		printf("Manager: Vm not found\n");
		return false;
	}
	for(int i = 0; i < vm->core_size; i++) {
		if(cores[vm->cores[i]].status == VM_STATUS_START) {
			return false;
		}
	}
	
	map_remove(vms, (void*)(uint64_t)vmid);
	
	printf("Manager: Delete vm[%d] on cores [", vmid);
	for(int i = 0; i < vm->core_size; i++) {
		printf("%d", mp_apic_id_to_core_id(vm->cores[i]));
		if(i + 1 < vm->core_size)
			printf(", ");
	}
	printf("]\n");
	
	vm_delete(vm, -1);
	
	return true;
}

bool vm_contains(uint32_t vmid) {
	return map_contains(vms, (void*)(uint64_t)vmid);
}

int vm_count() {
	return map_size(vms);
}

int vm_list(uint32_t* vmids, int size) {
	int i = 0;
	
	MapIterator iter;
	map_iterator_init(&iter, vms);
	while(i < size && map_iterator_has_next(&iter)) {
		MapEntry* entry = map_iterator_next(&iter);
		vmids[i++] = (uint32_t)(uint64_t)entry->key;
	}
	
	return i;
}

typedef struct {
	VM_STATUS_CALLBACK	callback;
	void*			context;
	int			status;
} CallbackInfo;

static bool status_changed(uint64_t event_type, void* event, void* context) {
	VM* vm = event;
	CallbackInfo* info = context;
	
	bool result = vm->status == info->status || (vm->status == VM_STATUS_START && info->status == VM_STATUS_RESUME);
	info->callback(result, info->context);
	
	char* op = "";
	switch(info->status) {
		case VM_STATUS_START: op = "start"; break;
		case VM_STATUS_PAUSE: op = "pause"; break;
		case VM_STATUS_RESUME: op = "resume"; break;
		case VM_STATUS_STOP: op = "stop"; break;
	}
	
	printf("Manager: VM[%d] %s %s on cores [", vm->id, op, result ? "succeed" : "failed");
	for(int i = 0; i < vm->core_size; i++) {
		printf("%d", mp_apic_id_to_core_id(vm->cores[i]));
		if(i + 1 < vm->core_size)
			printf(", ");
	}
	printf("]\n");
	
	free(info);
	
	return false;
}

void vm_status_set(uint32_t vmid, int status, VM_STATUS_CALLBACK callback, void* context) {
	VM* vm = map_get(vms, (void*)(uint64_t)vmid);
	if(!vm) {
		callback(false, context);
		return;
	}
	
	int icc_type = 0;
	uint64_t event_type = 0;
	switch(status) {
		case VM_STATUS_START:
			printf("VM (%p) Status start\n", vm);
			if(vm->status != VM_STATUS_STOP) {
				callback(false, context);
				return;
			}
			event_type = EVENT_VM_STARTED;
			icc_type = ICC_TYPE_START;
			break;
		case VM_STATUS_PAUSE:
			if(vm->status != VM_STATUS_START) {
				callback(false, context);
				return;
			}
			event_type = EVENT_VM_PAUSED;
			icc_type = ICC_TYPE_PAUSE;
			break;
		case VM_STATUS_RESUME:
			if(vm->status != VM_STATUS_PAUSE) {
				callback(false, context);
				return;
			}
			event_type = EVENT_VM_RESUMED;
			icc_type = ICC_TYPE_RESUME;
			break;
		case VM_STATUS_STOP:
			printf("VM (%p) Status stop\n", vm);
			if(vm->status != VM_STATUS_PAUSE && vm->status != VM_STATUS_START) {
				callback(false, context);
				return;
			}
			event_type = EVENT_VM_STOPPED;
			icc_type = ICC_TYPE_STOP;
			break;
	}
	
	// Lazy clean up
	if(status == VM_STATUS_START) {
		for(size_t i = 0; i < vm->memory.count; i++) {
			memset(vm->memory.blocks[i], 0x0, 0x200000);
		}
	}

	CallbackInfo* info = malloc(sizeof(CallbackInfo));
	info->callback = callback;
	info->context = context;
	info->status = status;
	
	event_trigger_add(event_type, status_changed, info);
	
	for(int i = 0; i < vm->core_size; i++) {
		if(status == VM_STATUS_PAUSE) {
			apic_write64(APIC_REG_ICR, ((uint64_t)vm->cores[i] << 56) |
						APIC_DSH_NONE |
						APIC_TM_EDGE |
						APIC_LV_DEASSERT |
						APIC_DM_PHYSICAL |
						APIC_DMODE_FIXED |
						49);
		} else {
			cores[vm->cores[i]].error_code = 0;
			ICC_Message* msg = icc_alloc(icc_type);
			if(status == VM_STATUS_START) {
				msg->data.start.vm = vm;
			}
			icc_send(msg, vm->cores[i]);
		}
	}
}

int vm_status_get(uint32_t vmid) {
	VM* vm = map_get(vms, (void*)(uint64_t)vmid);
	if(!vm)
		return -1;
	
	return vm->status;
}

VM* vm_get(uint32_t vmid) {
	VM* vm = map_get(vms, (void*)(uint64_t)vmid);
	if(!vm)
		return NULL;
	
	return vm;
}

ssize_t vm_storage_read(uint32_t vmid, void** buf, size_t offset, size_t size) {
	VM* vm = map_get(vms, (void*)(uint64_t)vmid);
	if(!vm)
		return -1;

	if(offset > vm->storage.count * VM_STORAGE_SIZE_ALIGN) {
		*buf = NULL;
		return 0;
	}

	int index = offset / VM_STORAGE_SIZE_ALIGN;
	offset %= VM_STORAGE_SIZE_ALIGN;
	*buf = vm->storage.blocks[index] + offset;
	
	if(offset + size > VM_STORAGE_SIZE_ALIGN)
		return VM_STORAGE_SIZE_ALIGN - offset;
	else
		return size;
}

ssize_t vm_storage_write(uint32_t vmid, void* buf, size_t offset, size_t size) {
	VM* vm = map_get(vms, (void*)(uint64_t)vmid);
	if(!vm)
		return -1;

	if(!size)
		return 0;
	
	if((uint64_t)offset + size > (uint64_t)vm->storage.count * VM_STORAGE_SIZE_ALIGN)
		return -1;
	
	uint32_t index = offset / VM_STORAGE_SIZE_ALIGN;
	if(index >= vm->storage.count)
		return -1;

	size_t _size = size;
	offset %= VM_STORAGE_SIZE_ALIGN;
	for(; index < vm->storage.count; index++) {
		if(offset + _size > VM_STORAGE_SIZE_ALIGN) {
			size_t write_size = VM_STORAGE_SIZE_ALIGN - offset;
			memcpy(vm->storage.blocks[index] + offset, buf, write_size);
			_size -= write_size;
			buf += write_size;
		} else {
			memcpy(vm->storage.blocks[index] + offset, buf, _size);
			_size = 0;
		}

		if(_size == 0)
			break;
		offset = 0;
	}

	if(_size != 0)
		return -1;
	
	return size;
}

ssize_t vm_storage_clear(uint32_t vmid) {
	VM* vm = map_get(vms, (void*)(uint64_t)vmid);
	if(!vm)
		return -1;
	
	ssize_t size = 0;
	for(uint32_t i = 0; i < vm->storage.count; i++) {
		memset(vm->storage.blocks[i], 0x0, VM_STORAGE_SIZE_ALIGN);
		size += VM_STORAGE_SIZE_ALIGN;
	}
	
	return size;
}

bool vm_storage_md5(uint32_t vmid, uint32_t size, uint32_t digest[4]) {
	VM* vm = map_get(vms, (void*)(uint64_t)vmid);
	if(!vm)
		return false;
	
	uint32_t block_count = size / VM_MEMORY_SIZE_ALIGN;
	if(vm->storage.count < block_count)
		return false;

	md5_blocks(vm->storage.blocks, vm->storage.count, VM_STORAGE_SIZE_ALIGN, size, digest);
	
	return true;
}

ssize_t vm_stdio(uint32_t vmid, int thread_id, int fd, const char* str, size_t size) {
	VM* vm = map_get(vms, (void*)(uint64_t)vmid);
	if(!vm)
		return -1;
	
	if(thread_id < 0 || thread_id >= vm->core_size)
		return -1;
	
	Core* core = &cores[vm->cores[thread_id]];
	if(core->status != VM_STATUS_PAUSE && core->status != VM_STATUS_START)
		return -1;
	
	switch(fd) {
		case 0:
			if(core->stdin)
				return ring_write(core->stdin, *core->stdin_head, core->stdin_tail, core->stdin_size, str, size);
			else
				return -1;
		case 1:
			if(core->stdout)
				return ring_write(core->stdout, *core->stdout_head, core->stdout_tail, core->stdout_size, str, size);
			else
				return -1;
		case 2:
			if(core->stderr)
				return ring_write(core->stderr, *core->stderr_head, core->stderr_tail, core->stderr_size, str, size);
			return -1;
		default:
			return -1;
	}
}

void vm_stdio_handler(VM_STDIO_CALLBACK callback) {
	stdio_callback = callback;
}
