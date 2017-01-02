#ifndef __PN_ASSISTANT_H__
#define __PN_ASSISTANT_H__

#include <util/map.h>
#include "../../src/vm.h"

// API for Manager
bool pn_assistant_init();	//O_RDWR
void* pn_assistant_get_pool();
ssize_t pn_assistant_get_pool_size();
bool pn_assistant_set_vms(Map* vms);
bool pn_assistant_set_physical_offset(uint64_t PHYSICAL_OFFSET);

// API for Application
bool pn_assistant_load();		//O_RDONLY

int pn_assistant_get_vm_id(char* name);
void pn_assistant_dump_vm(int vmid);
int pn_assistant_get_vm_status(int vmid);
bool pn_assistant_mapping_global_heap(int vmid);

void* pn_assistant_get_gmalloc_pool(int vmid);
void* pn_assistant_get_shared(int vmid);

#endif /*__PN_ASSISTANT_H__*/
