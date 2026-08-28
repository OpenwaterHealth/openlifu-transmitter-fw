#include "module_manager.h"
#include "utils.h"
#include <stdio.h>

// Internal storage for module information.
static ModuleInfo modules[MAX_MODULES];
static uint8_t totalModules = 1;
static bool _configured = false;
static uint8_t slave_address = 0;
static uint8_t module_ID = 0;
static DEVICE_ROLE my_device_role = ROLE_UNDEFINED;

static void ModuleManager_clear_storage() {
    totalModules = 1;
    module_ID = 0;
    slave_address = 0;
    // Clear all module entries.
    for (int i = 0; i < MAX_MODULES; i++) {
        modules[i].i2c_address = 0;
        modules[i].num_transmitters = 0;
        modules[i].mode = NODE_MODE_UNKNOWN;
        for (int j = 0; j < TX_PER_MODULE; j++) {
            modules[i].transmitters[j].cs_port = NULL;
            modules[i].transmitters[j].cs_pin = 0;
        }
    }
    _configured = false;
}

uint8_t ModuleManager_GetModuleIndex(uint8_t globalTxIndex) {
    return globalTxIndex / TX_PER_MODULE;
}

uint8_t ModuleManager_GetLocalTxIndex(uint8_t globalTxIndex) {
    return globalTxIndex % TX_PER_MODULE;
}

void ModuleManager_Init(void) {
	FW_DEBUG("ModuleManager_Init\r\n");
	ModuleManager_clear_storage();
}

void ModuleManager_DeInit(void) {
	FW_DEBUG("ModuleManager_DeInit\r\n");
	ModuleManager_clear_storage();
}

int ModuleManager_RegisterMaster(uint8_t i2c_address) {
	FW_DEBUG("ModuleManager_RegisterMaster\r\n");
    if (totalModules >= MAX_MODULES) {
        return -1;  // No space available.
    }
    // The master module is stored at index 0.
    modules[0].i2c_address = i2c_address;
    modules[0].num_transmitters = TX_PER_MODULE;
    modules[0].mode = NODE_MODE_APP;
    // You might want to initialize the transmitters here using your TX7332_Init() function if needed.
    totalModules = 1;
    return 0;
}

int ModuleManager_AddSlave(uint8_t i2c_address, uint8_t mode) {
	FW_DEBUG("ModuleManager_AddSlave 0x%02X mode %u\r\n", i2c_address, mode);
    if (totalModules >= MAX_MODULES) {
        return -1;  // Maximum number of modules reached.
    }
    modules[totalModules].i2c_address = i2c_address;
    modules[totalModules].num_transmitters = TX_PER_MODULE;
    modules[totalModules].mode = mode;
    // Initialization of each TX7332 on the slave may be done here or later.
    totalModules++;
    return totalModules - 1;  // Return the index of the newly added module.
}

uint8_t ModuleManager_GetModuleMode(uint8_t moduleIndex) {
    if (moduleIndex >= totalModules) {
        return NODE_MODE_UNKNOWN;
    }
    return modules[moduleIndex].mode;
}

ModuleInfo* ModuleManager_GetModule(uint8_t moduleIndex) {
    if (moduleIndex >= totalModules) {
        return NULL;
    }
    return &modules[moduleIndex];
}

ModuleInfo* ModuleManager_GetModule_byTxID(uint8_t tx_id) {
    if (tx_id >= totalModules*TX_PER_MODULE) {
        return NULL;
    }
    uint8_t module_idx = tx_id / 2;
    return &modules[module_idx];
}

TX7332* ModuleManager_GetTransmitter(uint8_t globalTxIndex) {
    uint8_t moduleIndex = ModuleManager_GetModuleIndex(globalTxIndex);
    uint8_t localTxIndex = ModuleManager_GetLocalTxIndex(globalTxIndex);
    if (moduleIndex >= totalModules) {
        return NULL;
    }
    ModuleInfo* mod = &modules[moduleIndex];
    if (localTxIndex >= mod->num_transmitters) {
        return NULL;
    }
    return &mod->transmitters[localTxIndex];
}

void set_module_ID(uint8_t id) {
    module_ID = id;
}

uint8_t get_module_ID() {
    return module_ID;
}

uint8_t get_slave_addres() {
    return slave_address;
}

void set_slave_address(uint8_t address) {
	slave_address = address;
}

DEVICE_ROLE get_device_role()
{
	return my_device_role;
}

void set_device_role(DEVICE_ROLE role)
{
	my_device_role = role;
}

bool get_configured(){
	if(my_device_role == ROLE_SLAVE && slave_address != 0 && _configured) return true;
	else if(my_device_role == ROLE_MASTER && _configured) return true;

	return false;
}

void set_configured(bool configured) {
	if(_configured == true && configured == false){
		ModuleManager_clear_storage();
	}else{
		_configured = configured;
	}
}

uint8_t get_tx_chip_count() {
	return totalModules * 2;
}

uint8_t get_module_count()
{
	return totalModules;
}

