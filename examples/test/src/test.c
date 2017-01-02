#include <stdio.h>
#include <stdlib.h>

#include <util/map.h>
#include <util/list.h>

#include <pn_assistant.h>

#include <../../src/vm.h>

int main(int argc, char** argv) {
	printf("This Application is PacketNgin assistant application for PacketNgin Network Application\n");
	if(argc == 1) {
		printf("Wrrong arguments\n");
		printf("./test [vmid]\n");
		return -1;
	}
	int vmid = atol(argv[1]);
	if(vmid == 0) { 
		printf("Wrrong arguments\n");
		printf("./test [vmid]\n");
		return -1;
	}

	if(!pn_assistant_load()) {
		printf("Can't Load PacketNgin Assistant\n");
		return -1;
	}
	printf("PacketNgin Assistant Loaded\n");
	
	pn_assistant_dump_vm(vmid);
	char* get_status(int vm_status){
		switch(vm_status) {
			case VM_STATUS_STOP:
				return "STOP";
			case VM_STATUS_PAUSE:
				return "PAUSE";
			case VM_STATUS_START:
				return "START";
			case VM_STATUS_RESUME:
				return "RESUME";
			default:
				return "INVALID";
		}
	}
	printf("VM Status:\t[%s]\n\n", get_status(pn_assistant_get_vm_status(vmid)));

	if(!pn_assistant_mapping_global_heap(vmid)) {
		printf("Fail Mapping Global Heap\n");
		return -2;
	}
	printf("Success Mapping Global Heap\n");
	void* gmalloc_pool = pn_assistant_get_gmalloc_pool(vmid);
	printf("Gmalloc Pool Address: %p\n", gmalloc_pool);
	void* shared = pn_assistant_get_shared(vmid);
	printf("Shared Address: %p\n", shared);

	return 0;
}
