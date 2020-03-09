#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <byteswap.h>
#include <string.h> 
#include "json.h"

// to run this program standalong
// int wr64b(uint64_t addr, uint64_t offset, uint64_t data){return 0;}
// int wr32b(uint64_t addr, uint64_t offset, uint32_t data){return 0;}
// uint32_t rd32b(uint64_t addr, uint64_t offset){ return (uint32_t) offset;}
// uint64_t rd64b(uint64_t addr, uint64_t offset){ return (uint64_t) offset;}

//forward declaration
int wr32b(uint64_t addr, uint64_t offset, uint32_t data);
int wr64b(uint64_t addr, uint64_t offset, uint64_t data);
uint32_t rd32b(uint64_t addr, uint64_t offset);
uint64_t rd64b(uint64_t addr, uint64_t offset);


// #define FA_QFLEX_ROOT_DIR "./"
#define FA_QFLEX_ROOT_DIR "/dev/shm/qflex/"
#define PATH_MAX 4096
#define NUM_XREGS 32
#define errno (*__errno_location ())
#define ULONG_HEX_MAX   16
#define UINT_HEX_MAX    8
#define STR(x) #x
#define XSTR(s) STR(s)

typedef enum FA_QFlexCmds_t {
    // Commands SIM->QEMU
    DATA_LOAD   = 0,
    DATA_STORE  = 1,
    INST_FETCH  = 2,
    INST_UNDEF  = 3,
    INST_EXCP   = 4,
    // Commands QEMU->SIM
    SIM_START  = 5, // Load state from QEMU
    SIM_STOP   = 6, //
    // Commands QEMU<->SIM
    LOCK_WAIT   = 7,
    CHECK_N_STEP = 8,
    FA_QFLEXCMDS_NR
} FA_QFlexCmds_t;

typedef struct FA_QFlexCmd_t {
    int cmd;
    uint64_t addr;
} FA_QFlexCmd_t;



char* fa_qflex_read_file(const char* filename, size_t *size) {
    int ret, fr;
    char *buffer;
    size_t fsize;
    char filepath[PATH_MAX];

    // printf("READ FILE %s\n", filename);
    snprintf(filepath, PATH_MAX, FA_QFLEX_ROOT_DIR"/%s", filename);

    fr = open(filepath, O_RDONLY, 0666);
    assert(fr);

    lseek(fr, 0, SEEK_END);
    fsize = lseek(fr, 0, SEEK_CUR);
    lseek(fr, 0, SEEK_SET);
    buffer = malloc(fsize + 1);

    ret = pread(fr, buffer, fsize, 0);
    assert(ret);
    close(fr);
    *size = fsize;
    return buffer;
}


int fa_qflex_write_file(const char *filename, void* buffer, size_t size) {
    char filepath[PATH_MAX];
    int fd = -1;
    void *region;
    printf("Writing file : "FA_QFLEX_ROOT_DIR"/%s\n", filename);
    if (mkdir(FA_QFLEX_ROOT_DIR, 0777) && errno != EEXIST) {
        printf("'mkdir "FA_QFLEX_ROOT_DIR"' failed\n");
        return 1;
    }
    snprintf(filepath, PATH_MAX, FA_QFLEX_ROOT_DIR"/%s", filename);
    if((fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0666)) == -1) {
        printf("Program Page dest file: open failed\n"
                       "    filepath:%s\n", filepath);
        return 1;
    }
    if (lseek(fd, size-1, SEEK_SET) == -1) {
        close(fd);
        printf("Error calling lseek() to 'stretch' the file\n");
        return 1;
    }
    if (write(fd, "", 1) != 1) {
        close(fd);
        printf(
            "Error writing last byte of the file\n");
        return 1;
    }

    region = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(region == MAP_FAILED) {
        close(fd);
        printf("Error dest file: mmap failed");
        return 1;
    }

    memcpy(region, buffer, size);
    msync(region, size, MS_SYNC);
    munmap(region, size);

    close(fd);
    return 0;
}


FA_QFlexCmd_t* fa_qflex_loadfile_json2cmd(const char* filename) {
    char *json;
    size_t size;
    FA_QFlexCmd_t* cmd = malloc(sizeof(FA_QFlexCmd_t));
    json = fa_qflex_read_file(filename, &size);
    json_value_s* root = json_parse(json, size);
    json_object_s* objects = root->payload;
    json_object_element_s* curr = objects->start;
    do {
        if(!strcmp(curr->name->string, "addr")) {
            assert(curr->value->type == json_type_string);
            json_string_s* number = curr->value->payload;
            cmd->addr = strtol(number->string, NULL, 16);
        } else if(!strcmp(curr->name->string, "cmd")) {
            assert(curr->value->type == json_type_string);
            json_number_s* number = curr->value->payload;
            cmd->cmd = strtol(number->number, NULL, 10);
        }
        curr = curr->next;
    } while(curr);
    free(json);
    return cmd;
}

void fa_qflex_writefile_cmd2json(const char* filename, FA_QFlexCmd_t in_cmd) {
    size_t size;
    char *json;
    json_value_s root;
    json_object_s objects;
    objects.length = 0;
    root.type = json_type_object;
    root.payload = &objects;
    json_object_element_s* head;


    //* cmd packing
    json_string_s cmd;

    char cmd_num[3];
    snprintf(cmd_num, 3,"%0"XSTR(2)"d", in_cmd.cmd);
    cmd.string = cmd_num;
    cmd.string_size = 2;

    json_value_s cmd_val = {.payload = &cmd, .type = json_type_string};
    json_string_s cmd_name = {.string = "cmd", .string_size = strlen("cmd")};
    json_object_element_s cmd_obj = {.value = &cmd_val, .name = &cmd_name, .next = NULL};
    objects.start = &cmd_obj;
    head = &cmd_obj;
    objects.length++;
    // */

    //* addr flags packing
    json_string_s addr;

    char addr_num[ULONG_HEX_MAX + 1];
    snprintf(addr_num, ULONG_HEX_MAX+1, "%0"XSTR(ULONG_HEX_MAX)"lx", in_cmd.addr);
    addr.string = addr_num;
    addr.string_size = ULONG_HEX_MAX;

    json_value_s addr_val = {.payload = &addr, .type = json_type_string};
    json_string_s addr_name = {.string = "addr", .string_size = strlen("addr")};
    json_object_element_s addr_obj = {.value = &addr_val, .name = &addr_name, .next = NULL};
    head->next = &addr_obj;
    head = &addr_obj;
    objects.length++;
    // */

    json = json_write_minified(&root, &size);
    fa_qflex_write_file(filename, json, size-1);
    free(json);
}

int writePage(uint64_t addr){
	size_t size;
    printf("Write programe page to FPGA \n");
	char* filename = "PROGRAM_PAGE";
	uint32_t* page = (uint32_t *)fa_qflex_read_file(filename, &size);
	int i = 0;
	for (; i < size/(sizeof(uint32_t)); ++i)
	{
        // printf("[%016x]\n", __bswap_32 (*(page + i)));
		wr32b(addr, i, __bswap_32 (*(page + i)));
	}
return 0;
}

int writeState(uint64_t addr){
    printf("Write State to FPGA...\n");
	size_t size;
	char* filename = "QEMU_STATE";
	char* buffer = fa_qflex_read_file(filename, &size);
    json_value_s* root = json_parse(buffer, size);
    json_object_s* objects = root->payload;
    json_object_element_s* curr;
    curr = objects->start;
    uint64_t xregs [NUM_XREGS];
    uint64_t pc=0;
    uint32_t nzcv=0;
    do {
        if(!strcmp(curr->name->string, "xregs")) {
            assert(curr->value->type == json_type_array);
            json_array_s* xregs_json = curr->value->payload;
            json_array_element_s* curr_reg = xregs_json->start;
            json_string_s* reg_val;
            for(int i = 0; i < NUM_XREGS; i++) {
                assert(curr_reg->value->type == json_type_string);
                reg_val = curr_reg->value->payload;
                xregs[i] = strtol(reg_val->string, NULL, 16);
                curr_reg = curr_reg->next;
            }
        } else if(!strcmp(curr->name->string, "pc")) {
            assert(curr->value->type == json_type_string);
            json_string_s* pc_json = curr->value->payload;
            pc = strtol(pc_json->string, NULL, 16);
        } else if(!strcmp(curr->name->string, "nzcv")) {
                assert(curr->value->type == json_type_string);
            json_string_s* json_nzcv = curr->value->payload;
            nzcv = strtol(json_nzcv->string, NULL, 16);
        }
        curr = curr->next;
    } while(curr);
    free(root);

    int i =0;
    for (; i < NUM_XREGS; ++i)
    {
        // printf("[%016lx]\n", xregs[i] );
    	wr32b(addr, i*2, xregs[i]>>32);
    	wr32b(addr, i*2 + 1, xregs[i]&0xffffffff);
    }
	wr32b(addr, 64 , pc>>32);
	wr32b(addr, 65, pc&0xffffffff);
	wr32b(addr, 66, nzcv);
return 0;
}

char* writeStateBack(uint64_t addr){
    printf("Read state from FPGA....\n");
    uint64_t xregs_v [NUM_XREGS];
    uint64_t pc_v=0;
    uint32_t nzcv_v=0;
    int i =0;


    for (; i < NUM_XREGS; ++i) {
    	xregs_v[i] = rd64b(addr, i);
    }
	pc_v = rd64b(addr, i++);
	nzcv_v = rd32b(addr, 66);


    // CPUARMState *env = cpu->env_ptr;
    char *json;
    json_value_s root;
    json_object_s objects;
    objects.length = 0;
    root.type = json_type_object;
    root.payload = &objects;
    json_object_element_s* head;


    //* XREGS packing {
    json_array_element_s xregs_aobjs[NUM_XREGS];
    json_value_s xregs_vals[NUM_XREGS];
    json_string_s xregs[NUM_XREGS];
    char xregs_nums[NUM_XREGS][ULONG_HEX_MAX];
    for(int i = 0; i < NUM_XREGS; i++) {
        snprintf(xregs_nums[i], ULONG_HEX_MAX+1, "%0"XSTR(ULONG_HEX_MAX)"lx", xregs_v[i]);
        xregs[i].string = xregs_nums[i];
        xregs[i].string_size = ULONG_HEX_MAX;
        xregs_vals[i].payload = &xregs[i];
        xregs_vals[i].type = json_type_string;
        xregs_aobjs[i].value = &xregs_vals[i];
        json_array_element_s* next = (i == NUM_XREGS-1) ? NULL : &xregs_aobjs[i+1];
        xregs_aobjs[i].next = next;
    }

    json_array_s xregs_array = {.start = &xregs_aobjs[0], .length = NUM_XREGS };
    json_value_s xregs_val = {.payload = &xregs_array, .type = json_type_array};
    json_string_s xregs_name = {.string = "xregs", .string_size = strlen("xregs")};
    json_object_element_s xregs_obj = {.value = &xregs_val, .name = &xregs_name, .next = NULL};
    objects.start = &xregs_obj;
    head = &xregs_obj;
    objects.length++;
    // */ }

    //* PC packing {
    json_string_s pc;

    char pc_num[ULONG_HEX_MAX + 1];
    snprintf(pc_num, ULONG_HEX_MAX+1,"%0"XSTR(ULONG_HEX_MAX)"lx", pc_v);
    pc.string = pc_num;
    pc.string_size = ULONG_HEX_MAX;

    json_value_s pc_val = {.payload = &pc, .type = json_type_string};
    json_string_s pc_name = {.string = "pc", .string_size = strlen("pc")};
    json_object_element_s pc_obj = {.value = &pc_val, .name = &pc_name, .next = NULL};
    head->next = &pc_obj;
    head = &pc_obj;
    objects.length++;
    // */ }

    //* NZCV flags packing {
    json_string_s nzcv;

    char nzcv_num[UINT_HEX_MAX + 1];
    uint32_t nzcv_flags = nzcv_v;
            // ((env->CF) ? 1 << ARCH_PSTATE_CF_MASK : 0) |
            // ((env->VF & (1<<31)) ? 1 << ARCH_PSTATE_VF_MASK : 0) |
            // ((env->NF & (1<<31)) ? 1 << ARCH_PSTATE_NF_MASK : 0) |
            // (!(env->ZF) ? 1 << ARCH_PSTATE_ZF_MASK : 0);
    snprintf(nzcv_num, UINT_HEX_MAX+1, "%0"XSTR(UINT_HEX_MAX)"x", nzcv_flags);
    nzcv.string = nzcv_num;
    nzcv.string_size = UINT_HEX_MAX;

    json_value_s nzcv_val = {.payload = &nzcv, .type = json_type_string};
    json_string_s nzcv_name = {.string = "nzcv", .string_size = strlen("nzcv")};
    json_object_element_s nzcv_obj = {.value = &nzcv_val, .name = &nzcv_name, .next = NULL};
    head->next = &nzcv_obj;
    head = &nzcv_obj;
    objects.length++;
    // */ }

    size_t size;
    json = json_write_minified(&root, &size);

    // //timing
    // struct timeval tval_before, tval_after, tval_result;
    // gettimeofday(&tval_before, NULL);

    char *buffer = json;
    fa_qflex_write_file("SIM_STATE", buffer, size - 1); // Get rid of last char:'\0'
    free(buffer);

    // // timing
    // gettimeofday(&tval_after, NULL);
    // timersub(&tval_after, &tval_before, &tval_result);
    // printf("helper *** Time elapsed: %ld sec %ld us\n", (long int)tval_result.tv_sec, (long int)tval_result.tv_usec);

    return 0;
}

int writeLockWaitSIM(){
    FA_QFlexCmd_t* cmd = malloc(sizeof(FA_QFlexCmd_t));
    cmd->cmd=LOCK_WAIT;
    fa_qflex_writefile_cmd2json("SIM_CMD", *cmd);
    return 0;
}

void waitForStart(){
    printf("Waiting for START command from QEMU...\n");
	FA_QFlexCmd_t* cmd;
    do{
        cmd = fa_qflex_loadfile_json2cmd("SIM_CMD");
        // usleep(1); //TODO: change it
        sleep(1);
    } while (cmd->cmd != SIM_START);
    writeLockWaitSIM();
}

int writeUndefined(){
    printf("Sending UNDEFINED command to QEMU...\n");
    FA_QFlexCmd_t* cmd = malloc(sizeof(FA_QFlexCmd_t));
    cmd->cmd=INST_UNDEF;
    fa_qflex_writefile_cmd2json("QEMU_CMD", *cmd);
    return 0;
}



// int main(int argc, char const *argv[])
// {
//     waitForStart();
//     writeStateBack(0);
//     writeUndefined();
    



//     // printf("%s",writeStateBack(0));

// 	// FA_QFlexCmd_t* cmd = fa_qflex_loadfile_json2cmd("SIM_CMD");
// 	// switch(cmd->cmd){
// 	// 	case SIM_START:
// 	// 		printf("sim start\n");
// 	// 		writePage(10);
// 	// 	break;
// 	// 	case SIM_STOP:
// 	// 		printf("sim stop\n");
// 	// 	break;
// 	// 	default:
// 	// 		printf("unknown command\n");
// 	// }


//     // FA_QFlexCmd_t* cmd = malloc(sizeof(FA_QFlexCmd_t));
//     // cmd->cmd=INST_UNDEF;
//     // fa_qflex_writefile_cmd2json("SIM_CMD", *cmd);

// 	return 0;
// }
