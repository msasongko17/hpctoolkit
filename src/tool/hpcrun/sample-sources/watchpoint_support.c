//
//  WatchPointDriver.cpp
//
//
//  Created by Milind Chabbi on 2/21/17.
//
//
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <asm/unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <linux/kernel.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <strings.h>
#include <asm/prctl.h>
#include <sys/prctl.h>

#include "common.h"
#include <hpcrun/main.h>
#include <hpcrun/hpcrun_options.h>
#include <hpcrun/write_data.h>
#include <hpcrun/safe-sampling.h>
#include <hpcrun/hpcrun_stats.h>
#include <hpcrun/memory/mmap.h>

#include <hpcrun/cct/cct.h>
#include <hpcrun/metrics.h>
#include <hpcrun/sample_event.h>
#include <hpcrun/sample_sources_registered.h>
#include <hpcrun/thread_data.h>
#include <hpcrun/trace.h>
#include <hpcrun/env.h>

#include <lush/lush-backtrace.h>
#include <messages/messages.h>

#include <utilities/tokenize.h>
#include <utilities/arch/context-pc.h>

#include <unwind/common/unwind.h>

#include "watchpoint_support.h"
#include <unwind/x86-family/x86-misc.h>
#if ADAMANT_USED
#include <adm_init_fini.h>
#endif
#include "matrix.h"

#define MULTITHREAD_REUSE_HISTO 1

#ifdef MULTITHREAD_REUSE_HISTO
//#include "reuse.h"
#define REUSE_HISTO 1
#endif

//extern int init_adamant;

//#define MAX_WP_SLOTS (5)
#define IS_ALIGNED(address, alignment) (! ((size_t)(address) & (alignment-1)))
#define ADDRESSES_OVERLAP(addr1, len1, addr2, len2) (((addr1)+(len1) > (addr2)) && ((addr2)+(len2) > (addr1) ))
//#define CACHE_LINE_SIZE (64)
//#define ALT_STACK_SZ (4 * SIGSTKSZ)
#define ALT_STACK_SZ ((1L<<20) > 4 * SIGSTKSZ? (1L<<20): 4* SIGSTKSZ)

//#define TEST
#ifdef TEST
#define EMSG(...) fprintf(stderr, __VA_ARGS__)
#define hpcrun_abort() abort()
#define hpcrun_safe_exit() (1)
#define hpcrun_safe_enter() (1)
#define hpcrun_context_pc(context) (0)
#define get_previous_instruction(ip, pip) (0)
#define get_mem_access_length_and_type(a, b, c) (0)
#endif


#if defined(PERF_EVENT_IOC_UPDATE_BREAKPOINT)
#define FAST_BP_IOC_FLAG (PERF_EVENT_IOC_UPDATE_BREAKPOINT)
#elif defined(PERF_EVENT_IOC_MODIFY_ATTRIBUTES)
#define FAST_BP_IOC_FLAG (PERF_EVENT_IOC_MODIFY_ATTRIBUTES)
#else
#endif


#define CHECK(x) ({int err = (x); \
		if (err) { \
		EMSG("%s: Failed with %d on line %d of file %s\n", strerror(errno), err, __LINE__, __FILE__); \
		monitor_real_abort(); }\
		err;})


#define HANDLE_ERROR_IF_ANY(val, expected, errstr) {if (val != expected) {perror(errstr); abort();}}
#define SAMPLES_POST_FULL_RESET_VAL (1)

// reuse queue before

#define MAX 512

uint64_t L1Counter = 0;
uint64_t L3Counter = 0;
int tidQueue[MAX];
int front = 0;
int rear = -1;
int entryCount = 0;

int peekQueue() {
   return tidQueue[front];
}

bool isEmpty() {
   return entryCount == 0;
}

bool isFull() {
   return entryCount == MAX;
}

int queueSize() {
   return entryCount;
}  

void insertQueue(int tid) {

   if(!isFull()) {
	
      if(rear == MAX-1) {
         rear = -1;            
      }       

      tidQueue[++rear] = tid;
      entryCount++;
   }
}

int removeDataQueue() {
   int tid = tidQueue[front++];
	
   if(front == MAX) {
      front = 0;
   }
	
   entryCount--;
   return tid;  
}

// reuse queue after


WPConfig_t wpConfig;

extern int event_type;

extern uint64_t l3_profile_counter;

/*typedef struct globalReuseEntry{
  volatile uint64_t counter __attribute__((aligned(CACHE_LINE_SZ)));
  uint64_t time;
  int tid;
  bool active;
  char dummy[CACHE_LINE_SZ];
} globalReuseEntry_t;

typedef struct globalReuseTable{
  struct globalReuseEntry table[MAX_WP_SLOTS];
  //struct SharedData * hashTable;
} globalReuseTable_t;*/

//const WatchPointInfo_t dummyWPInfo = {.sample = {}, .startTime =0, .fileHandle= -1, .isActive= false, .mmapBuffer=0};
//const struct DUMMY_WATCHPOINT dummyWP[MAX_WP_SLOTS];

//ReuseMtHashTable_t reuseMtBulletinBoard = {.counter = 0};

globalReuseTable_t globalReuseWPs;

typedef enum WP_CLIENT_ID{
	WP_DEADSPY,
	WP_REDSPY,
	WP_LOADSPY,
	WP_REUSE,
	WP_MT_REUSE,
	WP_REUSE_MT,
	WP_TEMPORAL_REUSE,
	WP_SPATIAL_REUSE,
	WP_FALSE_SHARING,
	WP_COMDETECTIVE,
	WP_ALL_SHARING,
	WP_TRUE_SHARING,
	WP_IPC_FALSE_SHARING,
	WP_IPC_TRUE_SHARING,
	WP_IPC_ALL_SHARING,
	WP_MAX_CLIENTS }WP_CLIENT_ID;

// Data structure that is given by clients to set a WP
typedef struct ThreadData{
	int lbrDummyFD __attribute__((aligned(CACHE_LINE_SZ)));
	stack_t ss;
	void * fs_reg_val;
	void * gs_reg_val;
	uint64_t samplePostFull;
	uint64_t numWatchpointArmingAttempt[MAX_WP_SLOTS];
	pid_t os_tid;
	long numWatchpointTriggers;
	long numActiveWatchpointTriggers;
	long numWatchpointImpreciseIP;
	long numWatchpointImpreciseAddressArbitraryLength;
	long numWatchpointImpreciseAddress8ByteLength;
	long numSampleTriggeringWatchpoints;
	long numWatchpointDropped;
	long numInsaneIP;
	struct drand48_data randBuffer;
	WatchPointInfo_t watchPointArray[MAX_WP_SLOTS];
	WatchPointUpCall_t fptr;
	volatile uint64_t counter[MAX_WP_SLOTS];
	char dummy[CACHE_LINE_SZ];
} ThreadData_t;

typedef struct threadDataTableStruct{
	volatile uint64_t counter __attribute__((aligned(64)));
	struct ThreadData hashTable[503];
	//struct SharedData * hashTable;
} ThreadDataTable_t;

ThreadDataTable_t threadDataTable = {.counter = 0};

typedef struct FdData {
	int fd;
	int tid;
	pid_t os_tid;
} FdData_t;

typedef struct fdDataTableStruct{
	volatile uint64_t counter __attribute__((aligned(64)));
	struct FdData hashTable[503];
	//struct SharedData * hashTable;
} FdDataTable_t;

FdDataTable_t fdDataTable = {.counter = 0};

int fdDataInsert(int fd, pid_t os_tid, int tid) {
	int idx = fd % 503;
	//printf("fd: %d is inserted to index: %d\n", fd, idx);
	fdDataTable.hashTable[idx].fd = fd;
	fdDataTable.hashTable[idx].os_tid = os_tid;
	fdDataTable.hashTable[idx].tid = tid;
	return idx;
}

int global_thread_count;
int dynamic_global_thread_count;

int same_thread_wp_count;
int l1_wp_count;
int same_thread_l1_wp_count;

bool globalWPIsActive[MAX_WP_SLOTS];
int globalWPIsUsers[MAX_WP_SLOTS];
uint64_t numWatchpointArmingAttempt[MAX_WP_SLOTS];

/*
uint64_t reuseMtDataInsert(int tid, uint64_t timestamp, bool active_flag) {
	uint64_t idx = timestamp % 503;
	//printf("fd: %d is inserted to index: %d\n", fd, idx);
	reuseMtBulletinBoard.hashTable[idx].tid = tid;
	reuseMtBulletinBoard.hashTable[idx].time = timestamp;
	reuseMtBulletinBoard.hashTable[idx].active = active_flag;
	return idx;
}*/

FdData_t fdDataGet(int fd) {
	int idx = fd % 503;
	return fdDataTable.hashTable[idx];
}

static __thread ThreadData_t tData;
__thread uint64_t create_wp_count = 0;
__thread uint64_t arm_wp_count = 0;
__thread uint64_t sub_wp_count1 = 0;
__thread uint64_t sub_wp_count2 = 0;
__thread uint64_t sub_wp_count3 = 0;
__thread uint64_t overlap_count = 0;
__thread uint64_t none_available_count = 0;
__thread uint64_t wp_count = 0;
__thread uint64_t wp_count1 = 0;
__thread uint64_t wp_count2 = 0;
__thread uint64_t wp_active = 0;
__thread uint64_t wp_dropped = 0;
__thread uint64_t intra_wp_dropped_counter = 0;
__thread uint64_t inter_wp_dropped_counter = 0;
__thread uint64_t subscribe_dropped = 0;

#ifdef REUSE_HISTO
/*extern bool reuse_output_trace;
extern double reuse_bin_start;
extern double reuse_bin_ratio;
extern __thread uint64_t * thread_reuse_bin_list;
extern __thread double * thread_reuse_bin_pivot_list; // store the bin intervals
extern __thread int thread_reuse_bin_size;*/
extern void initialize_reuse_ds();
#endif

void threadDataTablePrettyPrints() {
	printf("List of threads in thread data table:\n");
	for(int i = 0; i < global_thread_count; i++) {
		printf("%d\n", threadDataTable.hashTable[i].os_tid);
	}
}

void fdDataTablePrettyPrints() {
	printf("List of threads in fd data table:\n");
	for(int i = 0; i < global_thread_count; i++) {
		printf("fd: %d, os_tid: %d\n", fdDataTable.hashTable[i].fd, fdDataTable.hashTable[i].os_tid);
	}
}

bool IsAltStackAddress(void *addr){
	if((addr >= tData.ss.ss_sp) && (addr < tData.ss.ss_sp + tData.ss.ss_size))
		return true;
	return false;
}

bool IsFSorGS(void * addr) {
	if (tData.fs_reg_val == (void *) -1) {
		syscall(SYS_arch_prctl, ARCH_GET_FS, &tData.fs_reg_val);
		syscall(SYS_arch_prctl, ARCH_GET_GS, &tData.gs_reg_val);
	}
	// 4096 smallest one page size
	if ( (tData.fs_reg_val <= addr) && (addr < tData.fs_reg_val + 4096))
		return true;
	if ( (tData.gs_reg_val  <= addr) && (addr < tData.gs_reg_val  + 4096))
		return true;
	return false;
}


/********* OS SUPPORT ****************/

// perf-util.h has it
//static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
//    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
//}

/*
   static pid_t gettid() {
   return syscall(__NR_gettid);
   }*/


static inline void EnableWatchpoint(int fd) {
	// Start the event
	CHECK(ioctl(fd, PERF_EVENT_IOC_ENABLE, 0));
}

static inline void DisableWatchpoint(WatchPointInfo_t *wpi) {
	// Stop the event
	//assert(wpi->fileHandle != -1);
	if(wpi->fileHandle != -1)
		CHECK(ioctl(wpi->fileHandle, PERF_EVENT_IOC_DISABLE, 0));
	wpi->isActive = false;
}


static void * MAPWPMBuffer(int fd){
	void * buf = mmap(0, 2 * wpConfig.pgsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED) {
		EMSG("Failed to mmap : %s\n", strerror(errno));
		//fprintf(stderr, "error: Failed to mmap : %s\n", strerror(errno));
		monitor_real_abort();
	}
	return buf;
}

static void UNMAPWPMBuffer(void * buf){
	CHECK(munmap(buf, 2 * wpConfig.pgsz));
}

static int OnWatchPoint(int signum, siginfo_t *info, void *context);

__attribute__((constructor))
	static void InitConfig(){
		//printf("InitConfig is called\n");
		global_thread_count = 0;
		dynamic_global_thread_count = 0;
		/*if(!init_adamant) {
		  init_adamant = 1;*/
		//adm_initialize();
		//}
		tData.fptr = NULL;

		volatile int dummyWP[MAX_WP_SLOTS];
		wpConfig.isLBREnabled = true;

		struct perf_event_attr peLBR = {
			.type                   = PERF_TYPE_BREAKPOINT,
			.size                   = sizeof(struct perf_event_attr),
			.bp_type                = HW_BREAKPOINT_W,
			.bp_len                 = HW_BREAKPOINT_LEN_1,
			.bp_addr                = (uintptr_t)&dummyWP[0],
			.sample_period          = 1,
			.precise_ip             = 0 /* arbitraty skid */,
			.sample_type            = 0,
			.exclude_user           = 0,
			.exclude_kernel         = 1,
			.exclude_hv             = 1,
			.disabled               = 0, /* enabled */
		};
		int fd =  perf_event_open(&peLBR, 0, -1, -1 /*group*/, 0);
		if (fd != -1) {
			wpConfig.isLBREnabled = true;
		} else {
			wpConfig.isLBREnabled = false;
		}
		CHECK(close(fd));


#if defined(FAST_BP_IOC_FLAG)
		wpConfig.isWPModifyEnabled = true;
#else
		wpConfig.isWPModifyEnabled = false;
#endif
		//wpConfig.signalDelivered = SIGTRAP;
		//wpConfig.signalDelivered = SIGIO;
		//wpConfig.signalDelivered = SIGUSR1;
		wpConfig.signalDelivered = SIGRTMIN + 3;

		// Setup the signal handler
		sigset_t block_mask;
		sigfillset(&block_mask);
		// Set a signal handler for SIGUSR1
		struct sigaction sa1 = {
			.sa_sigaction = OnWatchPoint,
			.sa_mask = block_mask,
			.sa_flags = SA_SIGINFO | SA_RESTART | SA_NODEFER | SA_ONSTACK
		};

		if(monitor_sigaction(wpConfig.signalDelivered, OnWatchPoint, 0 /*flags*/, &sa1) == -1) {
			fprintf(stderr, "error: Failed to set WHICH_SIG handler: %s\n", strerror(errno));
			monitor_real_abort();
		}





		wpConfig.pgsz = sysconf(_SC_PAGESIZE);

		// identify max WP supported by the architecture
		//fprintf(stderr, "watchpoints are created\n");
		volatile int wpHandles[MAX_WP_SLOTS];
		int i = 0;
		for(; i < MAX_WP_SLOTS; i++){
			struct perf_event_attr pe = {
				.type                   = PERF_TYPE_BREAKPOINT,
				.size                   = sizeof(struct perf_event_attr),
				.bp_type                = HW_BREAKPOINT_W,
				.bp_len                 = HW_BREAKPOINT_LEN_1,
				.bp_addr                = (uintptr_t)&dummyWP[i],
				.sample_period          = 1,
				.precise_ip             = 0 /* arbitraty skid */,
				.sample_type            = 0,
				.exclude_user           = 0,
				.exclude_kernel         = 1,
				.exclude_hv             = 1,
				.disabled               = 0, /* enabled */
			};
			wpHandles[i] =  perf_event_open(&pe, 0, -1, -1 /*group*/, 0);
			if (wpHandles[i] == -1) {
				break;
			}
		}

		if(i == 0) {
			fprintf(stderr, "error: Cannot create a single watch point\n");
			monitor_real_abort();
		}
		for (int j = 0 ; j < i; j ++) {
			CHECK(close(wpHandles[j]));
		}
		int custom_wp_size = atoi(getenv(WATCHPOINT_SIZE));
		if(custom_wp_size < i)
			wpConfig.maxWP = custom_wp_size;
		else
			wpConfig.maxWP = i;
		same_thread_wp_count = 1;
		//l1_wp_count = 1;
		int l1_wp_count_temp = atoi(getenv(L1_WATCHPOINT_SIZE));
		if(l1_wp_count_temp < wpConfig.maxWP)
                        l1_wp_count = l1_wp_count_temp;
                else
                        l1_wp_count = wpConfig.maxWP;

		int same_thread_l1_wp_count_temp = atoi(getenv(SAME_THREAD_L1_WATCHPOINT_SIZE));
                if(same_thread_l1_wp_count_temp < l1_wp_count)
                        same_thread_l1_wp_count = same_thread_l1_wp_count_temp;
                else
                        same_thread_l1_wp_count = l1_wp_count;

		fprintf(stderr, "wpConfig.maxWP is %d\n", wpConfig.maxWP);
		fprintf(stderr, "l1_wp_count is %d\n", l1_wp_count);
		fprintf(stderr, "same_thread_l1_wp_count is %d\n", same_thread_l1_wp_count);

		// Should we get the floating point type in an access?
		wpConfig.getFloatType = false;

		// Get the replacement scheme
		char * replacementScheme = getenv("HPCRUN_WP_REPLACEMENT_SCHEME");
		if(replacementScheme){
			if(0 == strcasecmp(replacementScheme, "AUTO")) {
				wpConfig.replacementPolicy = AUTO;
			} if (0 == strcasecmp(replacementScheme, "OLDEST")) {
				wpConfig.replacementPolicy = OLDEST;
			} if (0 == strcasecmp(replacementScheme, "NEWEST")) {
				wpConfig.replacementPolicy = NEWEST;
			} if (0 == strcasecmp(replacementScheme, "RDX")) {
                                wpConfig.replacementPolicy = RDX;
                        } else {
				// default;
				wpConfig.replacementPolicy = AUTO;
			}
		} else {
			// default;
			wpConfig.replacementPolicy = AUTO;
		}
		//fprintf(stderr, "InitConfig is called\n"); 
		// Should we fix IP off by one?
		char * fixIP = getenv("HPCRUN_WP_DONT_FIX_IP");
		if(fixIP){
			if(0 == strcasecmp(fixIP, "1")) {
				wpConfig.dontFixIP = true;
			} if (0 == strcasecmp(fixIP, "true")) {
				wpConfig.dontFixIP = true;
			} else {
				// default;
				wpConfig.dontFixIP = false;
			}
		} else {
			// default;
			wpConfig.dontFixIP = false;
		}

		// Should we get the address in a WP trigger?
		char * disassembleWPAddress = getenv("HPCRUN_WP_DONT_DISASSEMBLE_TRIGGER_ADDRESS");
		if(disassembleWPAddress){
			if(0 == strcasecmp(disassembleWPAddress, "1")) {
				wpConfig.dontDisassembleWPAddress = true;
			} if (0 == strcasecmp(disassembleWPAddress, "true")) {
				wpConfig.dontDisassembleWPAddress = true;
			} else {
				// default;
				wpConfig.dontDisassembleWPAddress = false;
			}
		} else {
			// default;
			wpConfig.dontDisassembleWPAddress = false;
		}

		for(int i = 0; i < 503; i++) {
			for(int j = 0; j < MAX_WP_SLOTS; j++)
				threadDataTable.hashTable[i].counter[j] = 0;
			threadDataTable.hashTable[i].os_tid = -1;
		}

	for(int i = 0; i < MAX_WP_SLOTS; i++) {
		globalWPIsActive[i] = false;
		globalWPIsUsers[i] = -1;
		numWatchpointArmingAttempt[i] = SAMPLES_POST_FULL_RESET_VAL;
		globalReuseWPs.table[i].time = -1;
		globalReuseWPs.table[i].active = false;
		globalReuseWPs.table[i].counter = 0;
		globalReuseWPs.table[i].inc = 0;
		globalReuseWPs.table[i].rd = 0;
	}	
	l3_profile_counter = 0;
	L1Counter = 0;
	L3Counter = 0;
}

void RedSpyWPConfigOverride(void *v){
	wpConfig.getFloatType = true;
}

void LoadSpyWPConfigOverride(void *v){
	wpConfig.getFloatType = true;
}


void FalseSharingWPConfigOverride(void *v){
	// replacement policy is OLDEST forced.
	wpConfig.replacementPolicy = OLDEST;
}

void ComDetectiveWPConfigOverride(void *v){
	// replacement policy is OLDEST forced.
	wpConfig.replacementPolicy = OLDEST;
}

void ReuseWPConfigOverride(void *v){
	// dont fix IP
	//wpConfig.dontFixIP = true;
	//wpConfig.dontDisassembleWPAddress = true;
	//wpConfig.isLBREnabled = false; //jqswang
	//fprintf(stderr, "ReuseWPConfigOverride is called\n");
	wpConfig.replacementPolicy = RDX;
	//wpConfig.replacementPolicy = OLDEST;
}

void TrueSharingWPConfigOverride(void *v){
	// replacement policy is OLDEST forced.
	wpConfig.replacementPolicy = OLDEST;
}

void AllSharingWPConfigOverride(void *v){
	// replacement policy is OLDEST forced.
	wpConfig.replacementPolicy = OLDEST;
}

void IPCFalseSharingWPConfigOverride(void *v){
	// replacement policy is OLDEST forced.
	wpConfig.replacementPolicy = OLDEST;
}

void IPCTrueSharingWPConfigOverride(void *v){
	// replacement policy is OLDEST forced.
	wpConfig.replacementPolicy = OLDEST;
}

void IPCAllSharingWPConfigOverride(void *v){
	// replacement policy is OLDEST forced.
	wpConfig.replacementPolicy = OLDEST;
}


void TemporalReuseWPConfigOverride(void *v){
	// dont fix IP
	wpConfig.dontFixIP = true;
	wpConfig.dontDisassembleWPAddress = true;
}

void SpatialReuseWPConfigOverride(void *v){
	// dont fix IP
	wpConfig.dontFixIP = true;
	wpConfig.dontDisassembleWPAddress = true;
}

static void CreateWatchPoint(WatchPointInfo_t * wpi, SampleData_t * sampleData, bool modify) {
	// Perf event settings
	create_wp_count++;
	struct perf_event_attr pe = {
		.type                   = PERF_TYPE_BREAKPOINT,
		.size                   = sizeof(struct perf_event_attr),
		//        .bp_type                = HW_BREAKPOINT_W,
		//        .bp_len                 = HW_BREAKPOINT_LEN_4,
		.sample_period          = 1,
		.precise_ip             = wpConfig.isLBREnabled? 2 /*precise_ip 0 skid*/ : 0 /* arbitraty skid */,
		.sample_type            = (PERF_SAMPLE_IP),
		.exclude_user           = 0,
		.exclude_kernel         = 1,
		.exclude_hv             = 1,
		.disabled               = 0, /* enabled */
	};

	switch (sampleData->wpLength) {
		case 1: pe.bp_len = HW_BREAKPOINT_LEN_1; break;
		case 2: pe.bp_len = HW_BREAKPOINT_LEN_2; break;
		case 4: pe.bp_len = HW_BREAKPOINT_LEN_4; break;
		case 8: pe.bp_len = HW_BREAKPOINT_LEN_8; break;
		default:
			EMSG("Unsupported .bp_len %d: %s\n", wpi->sample.wpLength,strerror(errno));
			//fprintf(stderr, "error: Unsupported .bp_len %d: %s\n", wpi->sample.wpLength,strerror(errno));
			monitor_real_abort();
	}
	pe.bp_addr = (uintptr_t)sampleData->va;

	switch (sampleData->type) {
		case WP_READ: pe.bp_type = HW_BREAKPOINT_R; break;
		case WP_WRITE: pe.bp_type = HW_BREAKPOINT_W; break;
		default: pe.bp_type = HW_BREAKPOINT_W | HW_BREAKPOINT_R; 
	}
	//fprintf(stderr, "pe.bp_len: %d, pe.bp_addr: %lx\n", pe.bp_len, pe.bp_addr);
#if defined(FAST_BP_IOC_FLAG)
	if(modify) {
		// modification
		assert(wpi->fileHandle != -1);
		assert(wpi->mmapBuffer != 0);
		//DisableWatchpoint(wpi);
		//fprintf(stderr, "watchpoint is created with FAST_BP_IOC_FLAG\n");
		//create_wp_count++;
		CHECK(ioctl(wpi->fileHandle, FAST_BP_IOC_FLAG, (unsigned long) (&pe)));
		//if(wpi->isActive == false) {
		//EnableWatchpoint(wpi->fileHandle);
		//}
	} else
#endif
	{
		//create_wp_count++;
		// fresh creation
		// Create the perf_event for this thread on all CPUs with no event group
		//fprintf(stderr, "watchpoint is created with perf_event_open\n");
		int perf_fd = perf_event_open(&pe, 0, -1, -1 /*group*/, 0);
		if (perf_fd == -1) {
			EMSG("Failed to open perf event file: %s\n",strerror(errno));
			//fprintf(stderr, "error: Failed to open perf event file: %s\n",strerror(errno));
			monitor_real_abort();
		}
		// Set the perf_event file to async mode
		CHECK(fcntl(perf_fd, F_SETFL, fcntl(perf_fd, F_GETFL, 0) | O_ASYNC));

		// Tell the file to send a signal when an event occurs
		CHECK(fcntl(perf_fd, F_SETSIG, wpConfig.signalDelivered));

		// Deliver the signal to this thread
		struct f_owner_ex fown_ex;
		fown_ex.type = F_OWNER_TID;
		fown_ex.pid  = syscall(__NR_gettid); //gettid();
		int ret = fcntl(perf_fd, F_SETOWN_EX, &fown_ex);
		if (ret == -1){
			EMSG("Failed to set the owner of the perf event file: %s\n", strerror(errno));
			//fprintf(stderr, "error: Failed to set the owner of the perf event file: %s\n", strerror(errno));
			return;
		}
		//fprintf(stderr, "this point is reached\n");

		//       CHECK(fcntl(perf_fd, F_SETOWN, gettid()));

		wpi->fileHandle = perf_fd;
		// insert to perf_fd - tid table here
		// mmap the file if lbr is enabled
		if(wpConfig.isLBREnabled) {
			wpi->mmapBuffer = MAPWPMBuffer(perf_fd);
		}	
	}

	wp_active++;
	wpi->isActive = true;
	wpi->va = (void *) pe.bp_addr;
	wpi->sample = *sampleData;
	wpi->startTime = rdtsc();
	wpi->bulletinBoardTimestamp = sampleData->bulletinBoardTimestamp;
	//fprintf(stderr, "this point is reached 2\n");
}

static void CreateWatchPointShared(WatchPointInfo_t * wpi, SampleData_t * sampleData, int tid, bool modify) {
	// Perf event settings
	create_wp_count++;
	struct perf_event_attr pe = {
		.type                   = PERF_TYPE_BREAKPOINT,
		.size                   = sizeof(struct perf_event_attr),
		//        .bp_type                = HW_BREAKPOINT_W,
		//        .bp_len                 = HW_BREAKPOINT_LEN_4,
		.sample_period          = 1,
		.precise_ip             = wpConfig.isLBREnabled? 2 /*precise_ip 0 skid*/ : 0 /* arbitraty skid */,
		.sample_type            = (PERF_SAMPLE_IP),
		.exclude_user           = 0,
		.exclude_kernel         = 1,
		.exclude_hv             = 1,
		.disabled               = 0, /* enabled */
	};

	switch (sampleData->wpLength) {
		case 1: pe.bp_len = HW_BREAKPOINT_LEN_1; break;
		case 2: pe.bp_len = HW_BREAKPOINT_LEN_2; break;
		case 4: pe.bp_len = HW_BREAKPOINT_LEN_4; break;
		case 8: pe.bp_len = HW_BREAKPOINT_LEN_8; break;
		default:
			EMSG("Unsupported .bp_len %d: %s\n", wpi->sample.wpLength,strerror(errno));
			fprintf(stderr, "error: Unsupported .bp_len %d: %s\n", wpi->sample.wpLength,strerror(errno));
			monitor_real_abort();
	}
	pe.bp_addr = (uintptr_t)sampleData->va;

	switch (sampleData->type) {
		case WP_READ: pe.bp_type = HW_BREAKPOINT_R; break;
		case WP_WRITE: pe.bp_type = HW_BREAKPOINT_W; break;
		default: pe.bp_type = HW_BREAKPOINT_W | HW_BREAKPOINT_R;
	}

#if defined(FAST_BP_IOC_FLAG)
	if(modify) {
		// modification
		assert(wpi->fileHandle != -1);
		assert(wpi->mmapBuffer != 0);
		//DisableWatchpoint(wpi);
		//fprintf(stderr, "watchpoint is created with FAST_BP_IOC_FLAG in thread %d, modify: %d\n", TD_GET(core_profile_trace_data.id), modify);
		//create_wp_count++;
		CHECK(ioctl(wpi->fileHandle, FAST_BP_IOC_FLAG, (unsigned long) (&pe)));
		//if(wpi->isActive == false) {
		//EnableWatchpoint(wpi->fileHandle);
		//}
	} else 
#endif
		if (threadDataTable.hashTable[tid].os_tid != -1) { 
			// check if os_tid is available here  
			//create_wp_count++;
			// fresh creation
			// Create the perf_event for this thread on all CPUs with no event group
			//fprintf(stderr, "watchpoint is created with perf_event_open\n");
			int perf_fd = perf_event_open(&pe, threadDataTable.hashTable[tid].os_tid, -1, -1, 0);
			if (perf_fd == -1) {
				EMSG("Failed to open perf event file: %s\n",strerror(errno));
				//fprintf(stderr, "error: Failed to open perf event file !!! os_tid: %d: %s in thread %d, modify: %d\n", threadDataTable.hashTable[tid].os_tid,strerror(errno), TD_GET(core_profile_trace_data.id), modify);
				//monitor_real_abort();
				return;
			}

			// Set the perf_event file to async mode
			CHECK(fcntl(perf_fd, F_SETFL, fcntl(perf_fd, F_GETFL, 0) | O_ASYNC));

			// Tell the file to send a signal when an event occurs
			CHECK(fcntl(perf_fd, F_SETSIG, wpConfig.signalDelivered));

			// Deliver the signal to this thread
			struct f_owner_ex fown_ex;
			fown_ex.type = F_OWNER_TID;
			fown_ex.pid  = threadDataTable.hashTable[tid].os_tid; //syscall(__NR_gettid); //gettid();
			int ret = fcntl(perf_fd, F_SETOWN_EX, &fown_ex);
			if (ret == -1){
				EMSG("Failed to set the owner of the perf event file: %s\n", strerror(errno));
				//fprintf(stderr, "error: Failed to set the owner of the perf event file: %s\n", strerror(errno));
				return;
			}


			//       CHECK(fcntl(perf_fd, F_SETOWN, gettid()));

			wpi->fileHandle = perf_fd;
			// insert to perf_fd - tid table here
			// mmap the file if lbr is enabled
			if(wpConfig.isLBREnabled) {
				wpi->mmapBuffer = MAPWPMBuffer(perf_fd);
			}

			//fprintf(stderr, "perf_event_open has been used successfully\n");
			int idx = fdDataInsert(perf_fd, threadDataTable.hashTable[tid].os_tid, tid);
			//fdDataTablePrettyPrints();
			//fprintf(stderr, "in fd table, fd: %d, os_tid: %d, tid: %d\n", fdDataTable.hashTable[idx].fd, fdDataTable.hashTable[idx].os_tid, tid);

			/*wp_active++;
			  wpi->isActive = true;
			  wpi->va = (void *) pe.bp_addr;
			  wpi->sample = *sampleData;
			  wpi->startTime = rdtsc();*/
		}
	wp_active++;
	wpi->isActive = true;
	wpi->va = (void *) pe.bp_addr;
	wpi->sample = *sampleData;
	wpi->startTime = rdtsc();
	//wpi->bulletinBoardTimestamp = sampleData->bulletinBoardTimestamp;
}

/* create a dummy PERF_TYPE_HARDWARE event that will never fire */
static void CreateDummyHardwareEvent(void) {
	// Perf event settings
	struct perf_event_attr pe = {
		.type                   = PERF_TYPE_HARDWARE,
		.size                   = sizeof(struct perf_event_attr),
		.config                 = PERF_COUNT_HW_CACHE_MISSES,
		.sample_period          = 0x7fffffffffffffff, /* some insanely large sample period */
		.precise_ip             = 2,
		.sample_type            = PERF_SAMPLE_BRANCH_STACK,
		.exclude_user           = 0,
		.exclude_kernel         = 1,
		.exclude_hv             = 1,
		.branch_sample_type     = PERF_SAMPLE_BRANCH_ANY,
	};

	// Create the perf_event for this thread on all CPUs with no event group
	int perf_fd = perf_event_open(&pe, 0, -1, -1, 0);
	if (perf_fd == -1) {
		EMSG("Failed to open perf event file: %s\n", strerror(errno));
		fprintf(stderr, "error: Failed to open perf event file: %s\n", strerror(errno));
		monitor_real_abort();
	}
	tData.lbrDummyFD = perf_fd;
}

static void CloseDummyHardwareEvent(int perf_fd){
	CHECK(close(perf_fd));
}


/*********** Client interfaces *******/

static void DisArm(WatchPointInfo_t * wpi){

	//    assert(wpi->isActive);
	//fprintf(stderr, "watchpoint is disarmed\n");
	assert(wpi->fileHandle != -1);

	if(wpi->mmapBuffer)
		UNMAPWPMBuffer(wpi->mmapBuffer);
	wpi->mmapBuffer = 0;

	CHECK(close(wpi->fileHandle));
	wpi->fileHandle = -1;
	wpi->isActive = false;
}

static bool ArmWatchPoint(WatchPointInfo_t * wpi, SampleData_t * sampleData) {
	// if WP modification is suppoted use it
	//void * cacheLineBaseAddress = (void *) ((uint64_t)((size_t)sampleData->va) & (~(64-1)));
	arm_wp_count++;
	if(wpConfig.isWPModifyEnabled){
		// Does not matter whether it was active or not.
		// If it was not active, enable it.
		if(wpi->fileHandle != -1) {
			//fprintf(stderr, "before CreateWatchPoint\n");
			CreateWatchPoint(wpi, sampleData, true);
			return true;
		}
	}
	// disable the old WP if active
	if(wpi->isActive) {
		DisArm(wpi);
	}
	//fprintf(stderr, "before CreateWatchPoint\n");
	CreateWatchPoint(wpi, sampleData, false);
	return true;
}

static bool ArmWatchPointShared(WatchPointInfo_t * wpi, SampleData_t * sampleData, int tid) {
	//fprintf(stderr, "in ArmWatchPointShared\n");
	// if WP modification is suppoted use it
	//void * cacheLineBaseAddress = (void *) ((uint64_t)((size_t)sampleData->va) & (~(64-1)));
	arm_wp_count++;
	if(wpConfig.isWPModifyEnabled){
		// Does not matter whether it was active or not.
		// If it was not active, enable it.
		if((wpi->fileHandle != -1) && (sampleData->first_accessing_tid == wpi->sample.first_accessing_tid)) {
			//fprintf(stderr, "CreateWatchPointShared is entered with modify\n");
			CreateWatchPointShared(wpi, sampleData, tid, true);
			return true;
		}
	}
	// disable the old WP if active
	/*if(wpi->isActive) {
	  DisArm(wpi);
	  }*/

	if(wpi->fileHandle != -1) {
		DisArm(wpi);
	}
	//fprintf(stderr, "CreateWatchPointShared is entered with false modify\n");
	CreateWatchPointShared(wpi, sampleData, tid, false);
	return true;
}

// Per thread initialization

void WatchpointThreadInit(WatchPointUpCall_t func){
	global_thread_count++;
	dynamic_global_thread_count++;
	//fprintf(stderr, "in WatchpointThreadInit, WP_MT_REUSE\n");
	//printf("WatchpointThreadInit is called by thread with os id %d and id %d, thread count %d\n", gettid(), TD_GET(core_profile_trace_data.id), global_thread_count);
	int me = TD_GET(core_profile_trace_data.id);
	tData.ss.ss_sp = malloc(ALT_STACK_SZ);
	if (tData.ss.ss_sp == NULL){
		EMSG("Failed to malloc ALT_STACK_SZ");
		fprintf(stderr, "Failed to malloc ALT_STACK_SZ\n");
		monitor_real_abort();
	}
	tData.ss.ss_size = ALT_STACK_SZ;
	tData.ss.ss_flags = 0;
	if (sigaltstack(&tData.ss, NULL) == -1){
		EMSG("Failed sigaltstack");
		fprintf(stderr, "Failed sigaltstack\n");
		monitor_real_abort();
	}

	tData.lbrDummyFD = -1;
	tData.fptr = func;
	tData.fs_reg_val = (void*)-1;
	tData.gs_reg_val = (void*)-1;
	srand48_r(time(NULL), &tData.randBuffer);
	tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
	tData.numWatchpointTriggers = 0;
	tData.numWatchpointImpreciseIP = 0;
	tData.numWatchpointImpreciseAddressArbitraryLength = 0;
	tData.numWatchpointImpreciseAddress8ByteLength = 0;
	tData.numWatchpointDropped = 0;
	tData.numSampleTriggeringWatchpoints = 0;
	tData.numInsaneIP = 0;


	for (int i=0; i<wpConfig.maxWP; i++) {
		tData.watchPointArray[i].isActive = false;
		tData.watchPointArray[i].fileHandle = -1;
		tData.watchPointArray[i].startTime = 0;
		tData.numWatchpointArmingAttempt[i] = SAMPLES_POST_FULL_RESET_VAL;
	}

	//if LBR is supported create a dummy PERF_TYPE_HARDWARE for Linux workaround
	if(wpConfig.isLBREnabled) {
		CreateDummyHardwareEvent();
	}

	tData.os_tid = syscall(__NR_gettid); //gettid();

	for(int i = 0; i < MAX_WP_SLOTS; i++)
		tData.counter[i] = 0;

	//if((event_type == WP_REUSE_MT) || (event_type == WP_MT_REUSE))
#ifdef REUSE_HISTO
	threadDataTable.hashTable[me] = tData;
	insertQueue(me);
#endif

	#ifdef REUSE_HISTO
	{
		/*fprintf(stderr, "ok until this point\n");
		char * bin_scheme_str = getenv("HPCRUN_WP_REUSE_BIN_SCHEME");
		if (bin_scheme_str){
		} else { //default
            		if(reuse_bin_start == 0) {
				reuse_output_trace = false;
                		reuse_bin_start = 124;
                		//reuse_bin_start = 1000;
                		reuse_bin_ratio = 2;
                		fprintf(stderr, "default configuration is applied\n");
            		}
          	}

		if (reuse_output_trace == false){
			thread_reuse_bin_size = 20;
            		thread_reuse_bin_list = hpcrun_malloc(sizeof(uint64_t)*thread_reuse_bin_size);
            		memset(thread_reuse_bin_list, 0, sizeof(uint64_t)*thread_reuse_bin_size);
            		thread_reuse_bin_pivot_list = hpcrun_malloc(sizeof(double)*thread_reuse_bin_size);
            		thread_reuse_bin_pivot_list[0] = reuse_bin_start;

            		for(int i=1; i < thread_reuse_bin_size; i++){
                		thread_reuse_bin_pivot_list[i] = thread_reuse_bin_pivot_list[i-1] * reuse_bin_ratio;
                        	fprintf(stderr, "reuse_bin_pivot_list[%d]: %0.2lf\n", i, thread_reuse_bin_pivot_list[i]);
            		}
		}
		fprintf(stderr, "no problem until this point\n");*/
		if(event_type == WP_MT_REUSE || event_type == WP_REUSE_MT)
			initialize_reuse_ds();
	}
	#endif
}

void WatchpointThreadTerminate(){
	int me = TD_GET(core_profile_trace_data.id);
	dynamic_global_thread_count--;
	ThreadData_t threadData;
	if(event_type == WP_REUSE_MT) {

		for(int i = 0; i < wpConfig.maxWP; i++){
                        if(globalWPIsUsers[i] == me) {
                                globalWPIsActive[i] = false;
				globalWPIsUsers[i] = -1;
                        }
                }

		threadDataTable.hashTable[me].os_tid = -1;
		threadData = threadDataTable.hashTable[me];
		threadDataTable.hashTable[me].os_tid = -1;
        	for (int i = 0; i < wpConfig.maxWP; i++) {
                	if(threadDataTable.hashTable[me].watchPointArray[i].fileHandle != -1) {
                        	DisArm(&threadDataTable.hashTable[me].watchPointArray[i]);
                	}	
        	}

       		if(threadData.lbrDummyFD != -1) {
                	CloseDummyHardwareEvent(threadDataTable.hashTable[me].lbrDummyFD);
                	threadDataTable.hashTable[me].lbrDummyFD = -1;
        	}
        	threadDataTable.hashTable[me].fs_reg_val = (void*)-1;
        	threadDataTable.hashTable[me].gs_reg_val = (void*)-1;
	} else if (event_type == WP_MT_REUSE) {

		for(int i = l1_wp_count; i < wpConfig.maxWP; i++){
                	if(globalWPIsUsers[i] == me) {
                		globalWPIsActive[i] = false;
				globalWPIsUsers[i] = -1; 
                	}
        	}
		threadData = tData;
                for (int i = 0; i < wpConfig.maxWP; i++) {
                        if(tData.watchPointArray[i].fileHandle != -1) {
                                DisArm(&tData.watchPointArray[i]);
                        }
                }

                if(tData.lbrDummyFD != -1) {
                        CloseDummyHardwareEvent(tData.lbrDummyFD);
                        tData.lbrDummyFD = -1;
                }
                tData.fs_reg_val = (void*)-1;
                tData.gs_reg_val = (void*)-1;

	} else {
		threadData = tData;
		for (int i = 0; i < wpConfig.maxWP; i++) {
        		if(tData.watchPointArray[i].fileHandle != -1) {
            			DisArm(&tData.watchPointArray[i]);
        		}
    		}

    		if(tData.lbrDummyFD != -1) {
        		CloseDummyHardwareEvent(tData.lbrDummyFD);
        		tData.lbrDummyFD = -1;
    		}
    		tData.fs_reg_val = (void*)-1;
    		tData.gs_reg_val = (void*)-1;
	}	    
	

	//threadDataTablePrettyPrints();

	//fprintf(stderr, "threadData.numWatchpointTriggers: %ld\n", threadData.numWatchpointTriggers); 
	//fprintf(stderr, "threadData.numActiveWatchpointTriggers: %ld\n", threadData.numActiveWatchpointTriggers);
	hpcrun_stats_num_watchpoints_triggered_inc(threadData.numWatchpointTriggers);
	hpcrun_stats_num_watchpoints_imprecise_inc(threadData.numWatchpointImpreciseIP);
	hpcrun_stats_num_watchpoints_imprecise_address_inc(threadData.numWatchpointImpreciseAddressArbitraryLength);
	hpcrun_stats_num_watchpoints_imprecise_address_8_byte_inc(threadData.numWatchpointImpreciseAddress8ByteLength);
	hpcrun_stats_num_insane_ip_inc(threadData.numInsaneIP);
	hpcrun_stats_num_watchpoints_dropped_inc(threadData.numWatchpointDropped);
	hpcrun_stats_num_sample_triggering_watchpoints_inc(threadData.numSampleTriggeringWatchpoints);
#if 0
	tData.ss.ss_flags = SS_DISABLE;
	if (sigaltstack(&tData.ss, NULL) == -1){
		EMSG("Failed sigaltstack WatchpointThreadTerminate");
		// no need to abort , just leak the memory
		// monitor_real_abort();
	} else {
		if(tData.ss.ss_sp)
			free(tData.ss.ss_sp);
	}
#endif
}

void DisableWPforL3() {
	int me = TD_GET(core_profile_trace_data.id);
	for(int i = l1_wp_count; i < wpConfig.maxWP; i++){
		if(!globalWPIsActive[i] && threadDataTable.hashTable[me].watchPointArray[i].isActive) {
			DisableWatchpointWrapper(&threadDataTable.hashTable[me].watchPointArray[i]);
		}	
	}
}

bool GetVictimL3(int * location, uint64_t sampleTime) {
        int me = TD_GET(core_profile_trace_data.id);
        for(int i = l1_wp_count; i < wpConfig.maxWP; i++){
                if(globalWPIsActive[i] && (globalWPIsUsers[i] == me)) {
                        double probabilityToReplace =  1.0/((double)numWatchpointArmingAttempt[i]);
                        double randValue;
                        drand48_r(&tData.randBuffer, &randValue);
                        if((randValue <= probabilityToReplace) /*|| (probabilityToReplace < 0.1)*/) {
                                globalWPIsActive[i] = false;
                                globalWPIsUsers[i] = -1;
                                //fprintf(stderr, "a position in %d is opened by thread %d\n", i, me);
                                /*if(threadDataTable.hashTable[me].watchPointArray[i].isActive)
                                        DisableWatchpointWrapper(&threadDataTable.hashTable[me].watchPointArray[i]);*/
                        } else {
                                //fprintf(stderr, "global wp in %d is retained\n", i);
                        }
                        numWatchpointArmingAttempt[i]++;
                        return false;
                } else if (globalWPIsUsers[i] == me) {
                        globalWPIsUsers[i] = -1;
                        return false;
                }
        }

	uint64_t theCounter = L3Counter;
                if((theCounter & 1) == 0)
                        if(__sync_bool_compare_and_swap(&L3Counter, theCounter, theCounter+1)) {
                                for(int i = l1_wp_count; i < wpConfig.maxWP; i++){
                                        if(globalWPIsUsers[i] == -1)
                                                *location = i;
                                }
                                L3Counter++;
                        }

        /*uint64_t theCounter = queueCounter;
                if((theCounter & 1) == 0)
                        if(__sync_bool_compare_and_swap(&queueCounter, theCounter, theCounter+1)) {*/
                if(*location != -1) {
                        //fprintf(stderr, "open position in %d is taken by thread %d\n", *location, me);
                        globalWPIsActive[*location] = true;
                        globalWPIsUsers[*location] = me;
                        globalReuseWPs.table[*location].tid = me;
                        globalReuseWPs.table[*location].active = true;
                        globalReuseWPs.table[*location].time = sampleTime;
                        //queueCounter++;
                        return true;
                }
        /*queueCounter++;
        }*/
        return false;
}


bool GetVictimL1(int * location, uint64_t sampleTime) {
	int me = TD_GET(core_profile_trace_data.id);
	for(int i = same_thread_l1_wp_count; i < l1_wp_count; i++){
                if(globalWPIsActive[i] && (globalWPIsUsers[i] == me)) {
                        double probabilityToReplace =  1.0/((double)numWatchpointArmingAttempt[i]);
                    	double randValue;
                   	drand48_r(&tData.randBuffer, &randValue);
                   	if((randValue <= probabilityToReplace) /*|| (probabilityToReplace < 0.1)*/) {
                    		globalWPIsActive[i] = false;
				globalWPIsUsers[i] = -1;
				//fprintf(stderr, "a position in %d is opened by thread %d\n", i, me);
				/*if(threadDataTable.hashTable[me].watchPointArray[i].isActive)
					DisableWatchpointWrapper(&threadDataTable.hashTable[me].watchPointArray[i]);*/
                    	} /*else {
				fprintf(stderr, "global wp in %d is retained\n", i);
			}*/
			numWatchpointArmingAttempt[i]++;
			return false;
                } else if (globalWPIsUsers[i] == me) {
			globalWPIsUsers[i] = -1;
			return false;
		}
        }


	uint64_t theCounter = L1Counter;
                if((theCounter & 1) == 0)
                        if(__sync_bool_compare_and_swap(&L1Counter, theCounter, theCounter+1)) {
				for(int i = same_thread_l1_wp_count; i < l1_wp_count; i++){
					if(globalWPIsUsers[i] == -1)
						*location = i;
				}
				L1Counter++;
        		}

	/*uint64_t theCounter = queueCounter;
        	if((theCounter & 1) == 0)
                	if(__sync_bool_compare_and_swap(&queueCounter, theCounter, theCounter+1)) {*/
        	if(*location != -1) {
			//fprintf(stderr, "open position in %d is taken by thread %d\n", *location, me);
			globalWPIsActive[*location] = true;
			globalWPIsUsers[*location] = me;
			globalReuseWPs.table[*location].tid = me;
			globalReuseWPs.table[*location].active = true;
			globalReuseWPs.table[*location].time = sampleTime;
			//queueCounter++;
                        return true;
                }
	/*queueCounter++;
	}*/
        return false;
}

// Finds a victim slot to set a new WP
static VictimType GetVictimShared(int * location, ReplacementPolicy policy, int me, bool profile_l1){
	// same_thread_wp_count
	if(profile_l1) {

		if(event_type == WP_MT_REUSE) {
		for(int i = 0; i < l1_wp_count; i++){
                	if(!tData.watchPointArray[i].isActive) {
                        	*location = i;
                               	for(int j = 0; j < l1_wp_count; j++){
                                	if(tData.watchPointArray[j].isActive || (i == j)){
                                        	tData.numWatchpointArmingAttempt[j]++;
                                        }
                                }
                        	return EMPTY_SLOT;
                	}
        	}

		int indices[l1_wp_count - 1];
        	for (int i = 0; i < l1_wp_count; i++) {
                	indices[i] = i;
        	}

        	int wp_index = l1_wp_count;
		// until this point
        	while (wp_index) {
                	long int tmpVal;
                	lrand48_r(&tData.randBuffer, &tmpVal);
                	int index = tmpVal % wp_index;
                	wp_index--;
                	int swap = indices[index];
                	indices[index] = indices[wp_index];
                	indices[wp_index] = swap;
        	}
        	for(int i = 0; i < l1_wp_count; i++) {
                	int idx = indices[i];
                	double probabilityToReplace =  1.0/((double)tData.numWatchpointArmingAttempt[idx]);
                	double randValue;
                	drand48_r(&tData.randBuffer, &randValue);
                	if(randValue <= probabilityToReplace /* 1 */) {
                        	*location = idx;
                        	//fprintf(stderr, "arming watchpoint at i: %d and probability: %0.4lf\n", i, probabilityToReplace);
                        	for(int j = 0; j < l1_wp_count; j++){
                                	tData.numWatchpointArmingAttempt[j]++;
                        	}
                        	return NON_EMPTY_SLOT;
                	}
        	}
        	for(int i = 0; i < l1_wp_count; i++) {
                	tData.numWatchpointArmingAttempt[i]++;
        	}
		} else {

		for(int i = 0; i < same_thread_l1_wp_count; i++){
                        if(!tData.watchPointArray[i].isActive) {
                                *location = i;
                                for(int j = 0; j < same_thread_l1_wp_count; j++){
                                        if(tData.watchPointArray[j].isActive || (i == j)){
                                                tData.numWatchpointArmingAttempt[j]++;
                                        }
                                }
                                return EMPTY_SLOT;
                        }
                }

                int indices[same_thread_l1_wp_count - 1];
                for (int i = 0; i < same_thread_l1_wp_count; i++) {
                        indices[i] = i;
                }

                int wp_index = same_thread_l1_wp_count;
                // until this point
                while (wp_index) {
                        long int tmpVal;
                        lrand48_r(&tData.randBuffer, &tmpVal);
                        int index = tmpVal % wp_index;
                        wp_index--;
                        int swap = indices[index];
                        indices[index] = indices[wp_index];
                        indices[wp_index] = swap;
                }

		for(int i = 0; i < same_thread_l1_wp_count; i++) {
                        int idx = indices[i];
                        double probabilityToReplace =  1.0/((double)tData.numWatchpointArmingAttempt[idx]);
                        double randValue;
                        drand48_r(&tData.randBuffer, &randValue);
                        if(randValue <= probabilityToReplace /* 1 */) {
                                *location = idx;
                                //fprintf(stderr, "arming watchpoint at i: %d and probability: %0.4lf\n", i, probabilityToReplace);
                                for(int j = 0; j < same_thread_l1_wp_count; j++){
                                        tData.numWatchpointArmingAttempt[j]++;
                                }
                                return NON_EMPTY_SLOT;
                        }
                }
                for(int i = 0; i < same_thread_l1_wp_count; i++) {
                        tData.numWatchpointArmingAttempt[i]++;
                }

		}

	} else {
		for(int i = l1_wp_count; i < wpConfig.maxWP; i++){
			if(!threadDataTable.hashTable[me].watchPointArray[i].isActive) {
				*location = i;
				for(int j = l1_wp_count; j < wpConfig.maxWP; j++){
					if(threadDataTable.hashTable[me].watchPointArray[j].isActive || (i == j)){
						threadDataTable.hashTable[me].numWatchpointArmingAttempt[j]++;
					}
				}

				return EMPTY_SLOT;
			}
		}

		int indices[wpConfig.maxWP - l1_wp_count];
		for (int i = l1_wp_count; i < wpConfig.maxWP; i++) {
			indices[i - l1_wp_count] = i;
		}
		int wp_index = wpConfig.maxWP - l1_wp_count;
		while (wp_index) {
			long int tmpVal;
			lrand48_r(&threadDataTable.hashTable[me].randBuffer, &tmpVal);
			int index = tmpVal % wp_index;
			wp_index--;
			int swap = indices[index];
			indices[index] = indices[wp_index];
			indices[wp_index] = swap;
		}
		for(int i = l1_wp_count; i < wpConfig.maxWP; i++) {
			int idx = indices[i - l1_wp_count];
			double probabilityToReplace =  1.0/((double)threadDataTable.hashTable[me].numWatchpointArmingAttempt[idx]);
			double randValue;
			drand48_r(&threadDataTable.hashTable[me].randBuffer, &randValue);
			if(randValue <= probabilityToReplace /* 1 */) {
				*location = idx;
				//fprintf(stderr, "arming watchpoint at i: %d and probability: %0.4lf\n", i, probabilityToReplace);
				for(int j = l1_wp_count; j < wpConfig.maxWP; j++){
					threadDataTable.hashTable[me].numWatchpointArmingAttempt[j]++;
				}
				return NON_EMPTY_SLOT;
			}
		}
		for(int i = l1_wp_count; i < wpConfig.maxWP; i++) {
			threadDataTable.hashTable[me].numWatchpointArmingAttempt[i]++;
		}
	}
	return NONE_AVAILABLE;
}

// Finds a victim slot to set a new WP
static VictimType GetVictim(int * location, ReplacementPolicy policy){
	// If any WP slot is inactive, return it;
	/*if(policy == AUTO)
		fprintf(stderr, "policy is AUTO\n");*/
	for(int i = 0; i < wpConfig.maxWP; i++){
		if(!tData.watchPointArray[i].isActive) {
			*location = i;
			if(policy == RDX) {
				for(int j = 0; j < wpConfig.maxWP; j++){
					if(tData.watchPointArray[j].isActive || (i == j)){
						tData.numWatchpointArmingAttempt[j]++;
					}
				}
			}	
			return EMPTY_SLOT;
		}
	}
	switch (policy) {
		case AUTO:{
				  //fprintf(stderr, "replacement policy is AUTO\n");
				  // Equal probability for any data access


				  // Randomly pick a slot to victimize.
				  long int tmpVal;
				  lrand48_r(&tData.randBuffer, &tmpVal);
				  int rSlot = tmpVal % wpConfig.maxWP;
				  *location = rSlot;

				  // if it is the first sample after full, use wpConfig.maxWP/(wpConfig.maxWP+1) probability to replace.
				  // if it is the second sample after full, use wpConfig.maxWP/(wpConfig.maxWP+2) probability to replace.
				  // if it is the third sample after full, use wpConfig.maxWP/(wpConfig.maxWP+3) probability replace.

				  double probabilityToReplace =  wpConfig.maxWP/((double)wpConfig.maxWP+tData.samplePostFull);
				  double randValue;
				  drand48_r(&tData.randBuffer, &randValue);

				  // update tData.samplePostFull
				  //fprintf(stderr, "thread id: %d, tData.samplePostFull: %ld\n", TD_GET(core_profile_trace_data.id), tData.samplePostFull);
				  tData.samplePostFull++;
				  //fprintf(stderr, "thread id: %d, tData.samplePostFull: %ld\n", TD_GET(core_profile_trace_data.id), tData.samplePostFull);
				  //fprintf(stderr, "probabilityToReplace: %0.2lf\n", probabilityToReplace); 
				  if(/*randValue <= probabilityToReplace*/ 1) {
					  return NON_EMPTY_SLOT;
				  }
				  // this is an indication not to replace, but if the client chooses to force, they can
				  return NONE_AVAILABLE;
			  }
			  break;

		case NEWEST:{
				    // Always replace the newest
				    //fprintf(stderr, "replacement policy is NEWEST\n");
				    int64_t newestTime = 0;
				    for(int i = 0; i < wpConfig.maxWP; i++){
					    if(newestTime < tData.watchPointArray[i].startTime) {
						    *location = i;
						    newestTime = tData.watchPointArray[i].startTime;
					    }
				    }
				    return NON_EMPTY_SLOT;
			    }
			    break;

		case OLDEST:{
				    // Always replace the oldest
				    //fprintf(stderr, "replacement policy is OLDEST\n");
				    int64_t oldestTime = INT64_MAX;
				    for(int i = 0; i < wpConfig.maxWP; i++){
					    if(oldestTime > tData.watchPointArray[i].startTime) {
						    *location = i;
						    oldestTime = tData.watchPointArray[i].startTime;
					    }
				    }
				    return NON_EMPTY_SLOT;
			    }
			    break;

		case EMPTY_SLOT_ONLY:{
					     return NONE_AVAILABLE;
				     }
				     break;
		case RDX:{
				 // make a random sequence of watchpoints to visit 
				 // before
				//fprintf(stderr, "replacement policy is RDX\n");
				 int indices[wpConfig.maxWP];
				 for (int i = 0; i < wpConfig.maxWP; i++) {
					 indices[i] = i;
				 }
				 //fprintf(stderr, "in thread %d, before indices[0]: %d, indices[1]: %d, indices[2]: %d, indices[3]: %d\n", TD_GET(core_profile_trace_data.id), indices[0], indices[1], indices[2], indices[3]);
				 int wp_index = wpConfig.maxWP;
				 while (wp_index) {
					 long int tmpVal;
					 lrand48_r(&tData.randBuffer, &tmpVal);
					 int index = tmpVal % wp_index;
					 wp_index--;
					 int swap = indices[index];
					 indices[index] = indices[wp_index];
					 indices[wp_index] = swap;
				 }
				 //fprintf(stderr, "in thread %d, after indices[0]: %d, indices[1]: %d, indices[2]: %d, indices[3]: %d\n", TD_GET(core_profile_trace_data.id), indices[0], indices[1], indices[2], indices[3]);
				 // after
				 // visit each watchpoint according to the sequence
				 for(int i = 0; i < wpConfig.maxWP; i++) {
					 int idx = indices[i];
					 double probabilityToReplace =  1.0/((double)tData.numWatchpointArmingAttempt[idx]);
					 double randValue;
					 drand48_r(&tData.randBuffer, &randValue);
					 //fprintf(stderr, "i: %d, idx: %d, denominator: %ld, probability: %0.4lf\n", i, idx, tData.numWatchpointArmingAttempt[idx], probabilityToReplace);
					 if(randValue <= probabilityToReplace /* 1 */) {
						 *location = idx;
						 //fprintf(stderr, "arming watchpoint at i: %d and probability: %0.4lf\n", i, probabilityToReplace);
						 for(int j = 0; j < wpConfig.maxWP; j++){
							 tData.numWatchpointArmingAttempt[j]++;
						 }
						 return NON_EMPTY_SLOT;
					 }
				 }
				 for(int i = 0; i < wpConfig.maxWP; i++) {
					 tData.numWatchpointArmingAttempt[i]++;
				 }

				 return NONE_AVAILABLE;
			 }
			 break;
		default:
			 return NONE_AVAILABLE;
	}
	// No unarmed WP slot found.
}

static inline void
rmb(void) {
	asm volatile("lfence":::"memory");
}

static void ConsumeAllRingBufferData(void  *mbuf) {
	struct perf_event_mmap_page *hdr = (struct perf_event_mmap_page *)mbuf;
	unsigned long tail;
	size_t avail_sz;
	size_t pgmsk = wpConfig.pgsz - 1;
	/*
	 * data points to beginning of buffer payload
	 */
	void * data = ((void *)hdr) + wpConfig.pgsz;

	/*
	 * position of tail within the buffer payload
	 */
	tail = hdr->data_tail & pgmsk;

	/*
	 * size of what is available
	 *
	 * data_head, data_tail never wrap around
	 */
	avail_sz = hdr->data_head - hdr->data_tail;
	rmb();
#if 0
	if(avail_sz == 0 )
		EMSG("\n avail_sz = %d\n", avail_sz);
	else
		EMSG("\n EEavail_sz = %d\n", avail_sz);
#endif
	// reset tail to head
	hdr->data_tail = hdr->data_head;
}



static int ReadMampBuffer(void  *mbuf, void *buf, size_t sz) {
	//    fprintf(stderr, "in ReadMampBuffer\n");
	struct perf_event_mmap_page *hdr = (struct perf_event_mmap_page *)mbuf;
	//fprintf(stderr, "in ReadMampBuffer 6\n");
	void *data;
	unsigned long tail;
	size_t avail_sz, m, c;
	size_t pgmsk = wpConfig.pgsz - 1;
	/*
	 * data points to beginning of buffer payload
	 */
	data = ((void *)hdr) + wpConfig.pgsz;

	/*
	 * position of tail within the buffer payload
	 */
	//fprintf(stderr, "in ReadMampBuffer 7\n");
	tail = hdr->data_tail & pgmsk;

	/*
	 * size of what is available
	 *
	 * data_head, data_tail never wrap around
	 */
	//fprintf(stderr, "in ReadMampBuffer 5\n");
	avail_sz = hdr->data_head - hdr->data_tail;
	if (sz > avail_sz) {
		//printf("\n sz > avail_sz: sz = %lu, avail_sz = %lu\n", sz, avail_sz);
		rmb();
		return -1;
	}

	/* From perf_event_open() manpage */
	rmb();


	/*
	 * sz <= avail_sz, we can satisfy the request
	 */

	/*
	 * c = size till end of buffer
	 *
	 * buffer payload size is necessarily
	 * a power of two, so we can do:
	 */
	c = pgmsk + 1 -  tail;

	/*
	 * min with requested size
	 */
	m = c < sz ? c : sz;

	//fprintf(stderr, "in ReadMampBuffer 4\n"); 
	/* copy beginning */
	memcpy(buf, data + tail, m);

	/*
	 * copy wrapped around leftover
	 */
	//fprintf(stderr, "in ReadMampBuffer 3\n");
	if (sz > m)
		memcpy(buf + m, data, sz - m);
	//fprintf(stderr, "in ReadMampBuffer 2\n");
	hdr->data_tail += sz;

	return 0;
}


void
SkipBuffer(struct perf_event_mmap_page *hdr, size_t sz){
	if ((hdr->data_tail + sz) > hdr->data_head)
		sz = hdr->data_head - hdr->data_tail;
	rmb();
	hdr->data_tail += sz;
}

static inline bool IsPCSane(void * contextPC, void *possiblePC){
	if( (possiblePC==0) || ((possiblePC > contextPC) ||  (contextPC-possiblePC > 15))){
		return false;
	}
	return true;
}


double ProportionOfWatchpointAmongOthersSharingTheSameContext(WatchPointInfo_t *wpi){
#if 0
	int share = 0;
	for(int i = 0; i < wpConfig.maxWP; i++) {
		if(tData.watchPointArray[i].isActive && tData.watchPointArray[i].sample.node == wpi->sample.node) {
			share ++;
		}
	}
	assert(share > 0);
	return 1.0/share;
#else
	return 1.0;
#endif
}

static inline void *  GetPatchedIP(void *  contextIP) {
	void * patchedIP;
	void * excludeList[MAX_WP_SLOTS] = {0};
	int numExcludes = 0;
	for(int idx = 0; idx < wpConfig.maxWP; idx++){
		if(tData.watchPointArray[idx].isActive) {
			excludeList[numExcludes]=tData.watchPointArray[idx].va;
			numExcludes++;
		}
	}
	get_previous_instruction(contextIP, &patchedIP, excludeList, numExcludes);
	return patchedIP;
}

static inline void *  GetPatchedIPShared(void *  contextIP, int me) {
	//fprintf(stderr, "in GetPatchedIPShared\n");
	ThreadData_t threadData = threadDataTable.hashTable[me];
	void * patchedIP;
	void * excludeList[MAX_WP_SLOTS] = {0};
	int numExcludes = 0;
	for(int idx = 0; idx < wpConfig.maxWP; idx++){
		if(threadData.watchPointArray[idx].isActive) {
			excludeList[numExcludes]=threadData.watchPointArray[idx].va;
			numExcludes++;
		}
	}
	get_previous_instruction(contextIP, &patchedIP, excludeList, numExcludes);
	return patchedIP;
}

// Gather all useful data when a WP triggers
static bool CollectWatchPointTriggerInfo(WatchPointInfo_t  * wpi, WatchPointTrigger_t *wpt, void * context){
	//struct perf_event_mmap_page * b = wpi->mmapBuffer;
	struct perf_event_header hdr;
	//fprintf(stderr, "in CollectWatchPointTriggerInfo\n");
	if (ReadMampBuffer(wpi->mmapBuffer, &hdr, sizeof(struct perf_event_header)) < 0) {
		EMSG("Failed to ReadMampBuffer: %s\n", strerror(errno));
		monitor_real_abort();
	}
	//fprintf(stderr, "in CollectWatchPointTriggerInfo 1\n");
	switch(hdr.type) {
		case PERF_RECORD_SAMPLE:
			assert (hdr.type & PERF_SAMPLE_IP);
			void *  contextIP = hpcrun_context_pc(context);
			void *  preciseIP = (void *)-1;
			void *  patchedIP = (void *)-1;
			void *  reliableIP = (void *)-1;
			void *  addr = (void *)-1;
			if (hdr.type & PERF_SAMPLE_IP){
				if (ReadMampBuffer(wpi->mmapBuffer, &preciseIP, sizeof(uint64_t)) < 0) {
					EMSG("Failed to ReadMampBuffer: %s\n", strerror(errno));
					monitor_real_abort();
					//goto ErrExit;
				}

				if(! (hdr.misc & PERF_RECORD_MISC_EXACT_IP)){
					//EMSG("PERF_SAMPLE_IP imprecise\n");
					// change here
					tData.numWatchpointImpreciseIP ++;
					if(wpConfig.dontFixIP == false) {
						patchedIP = GetPatchedIP(contextIP);
						if(!IsPCSane(contextIP, patchedIP)) {
							//EMSG("get_previous_instruction  failed \n");
							tData.numInsaneIP ++;
							goto ErrExit;
						}
						reliableIP = patchedIP;
					} else {
						//fprintf(stderr, "dontFixIP\n");
						// Fake as requested by Xu for reuse clients
						reliableIP = contextIP-1;
					}
					//EMSG("PERF_SAMPLE_IP imprecise: %p patched to %p in WP handler\n", tmpIP, patchedIP);
				} else {
#if 0 // Precise PC can be far away in jump/call instructions.
					// Ensure the "precise" PC is within one instruction from context pc
					if(!IsPCSane(contextIP, preciseIP)) {
						tData.numInsaneIP ++;
						//EMSG("get_previous_instruction failed \n");
						goto ErrExit;
					}
#endif
					reliableIP = preciseIP;
					//if(! ((ip <= tmpIP) && (tmpIP-ip < 20))) ConsumeAllRingBufferData(wpi->mmapBuffer);
					//assert( (ip <= tmpIP) && (tmpIP-ip < 20));
				}
			} else {
				// Should happen only for wpConfig.isLBREnabled==false
				assert(wpConfig.isLBREnabled==false);
				// Fall back to old scheme of disassembling and capturing the info
				if(wpConfig.dontFixIP == false) {
					patchedIP = GetPatchedIP(contextIP);
					if(!IsPCSane(contextIP, patchedIP)) {
						tData.numInsaneIP ++;
						//EMSG("PERF_SAMPLE_IP imprecise: %p failed to patch in  WP handler, WP dropped\n", tmpIP);
						goto ErrExit;
					}
					reliableIP = patchedIP;
				}else {
					// Fake as requested by Xu for reuse clients
					reliableIP = contextIP-1;
				}
			}

			wpt->pc = reliableIP;

			if(wpConfig.dontDisassembleWPAddress == false){
				FloatType * floatType = wpConfig.getFloatType? &wpt->floatType : 0;
				if(false == get_mem_access_length_and_type_address(wpt->pc, (uint32_t*) &(wpt->accessLength), &(wpt->accessType), floatType, context, &addr)){
					//EMSG("WP triggered on a non Load/Store add = %p\n", wpt->pc);
					goto ErrExit;
				}
				if (wpt->accessLength == 0) {
					//EMSG("WP triggered 0 access length! at pc=%p\n", wpt->pc);
					goto ErrExit;
				}


				void * patchedAddr = (void *)-1;
				// Stack affecting addresses will be off by 8
				// Some instructions affect the address computing register: mov    (%rax),%eax
				// Hence, if the addresses do NOT overlap, merely use the Sample address!
				if(false == ADDRESSES_OVERLAP(addr, wpt->accessLength, wpi->va, wpi->sample.wpLength)) {
					if ((wpt->accessLength == sizeof(void *)) && (wpt->accessLength == wpi->sample.wpLength) &&  (((addr - wpi->va) == sizeof(void *)) || ((wpi->va - addr) == sizeof(void *))))
						tData.numWatchpointImpreciseAddress8ByteLength ++;
					else
						tData.numWatchpointImpreciseAddressArbitraryLength ++;


					tData.numWatchpointImpreciseAddressArbitraryLength ++;
					patchedAddr = wpi->va;
				} else {
					patchedAddr = addr;
				}
				wpt->va = patchedAddr;
			} else {
				//fprintf(stderr, "address is not disassembled\n");
				wpt->va = (void *)-1;
			}
			wpt->ctxt = context;
			// We must cleanup the mmap buffer if there is any data left
			ConsumeAllRingBufferData(wpi->mmapBuffer);
			return true;
		case PERF_RECORD_EXIT:
			EMSG("PERF_RECORD_EXIT sample type %d sz=%d\n", hdr.type, hdr.size);
			//SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
			goto ErrExit;
		case PERF_RECORD_LOST:
			EMSG("PERF_RECORD_LOST sample type %d sz=%d\n", hdr.type, hdr.size);
			//SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
			goto ErrExit;
		case PERF_RECORD_THROTTLE:
			EMSG("PERF_RECORD_THROTTLE sample type %d sz=%d\n", hdr.type, hdr.size);
			//SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
			goto ErrExit;
		case PERF_RECORD_UNTHROTTLE:
			EMSG("PERF_RECORD_UNTHROTTLE sample type %d sz=%d\n", hdr.type, hdr.size);
			//SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
			goto ErrExit;
		default:
			EMSG("unknown sample type %d sz=%d\n", hdr.type, hdr.size);
			//SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
			goto ErrExit;
	}

ErrExit:
	// We must cleanup the mmap buffer if there is any data left
	ConsumeAllRingBufferData(wpi->mmapBuffer);
	return false;
}

static bool CollectWatchPointTriggerInfoShared(WatchPointInfo_t  * wpi, WatchPointTrigger_t *wpt, void * context, int me){
	//struct perf_event_mmap_page * b = wpi->mmapBuffer;
	struct perf_event_header hdr;
	//fprintf(stderr, "in CollectWatchPointTriggerInfoShared in thread %d\n", me);
	if (ReadMampBuffer(wpi->mmapBuffer, &hdr, sizeof(struct perf_event_header)) < 0) {
		EMSG("Failed to ReadMampBuffer: %s\n", strerror(errno));
		//fprintf(stderr, "error: Failed to ReadMampBuffer: %s\n", strerror(errno));
		//monitor_real_abort();
		goto ErrExit2;
	}
	//fprintf(stderr, "in CollectWatchPointTriggerInfo 1\n");
	switch(hdr.type) {
		case PERF_RECORD_SAMPLE:
			assert (hdr.type & PERF_SAMPLE_IP);
			void *  contextIP = hpcrun_context_pc(context);
			void *  preciseIP = (void *)-1;
			void *  patchedIP = (void *)-1;
			void *  reliableIP = (void *)-1;
			void *  addr = (void *)-1;
			if (hdr.type & PERF_SAMPLE_IP){
				if (ReadMampBuffer(wpi->mmapBuffer, &preciseIP, sizeof(uint64_t)) < 0) {
					EMSG("Failed to ReadMampBuffer: %s\n", strerror(errno));
					//fprintf(stderr, "error: Failed to ReadMampBuffer: %s\n", strerror(errno));
					//monitor_real_abort();
					goto ErrExit2;
				}

				if(! (hdr.misc & PERF_RECORD_MISC_EXACT_IP)){
					//EMSG("PERF_SAMPLE_IP imprecise\n");
					threadDataTable.hashTable[me].numWatchpointImpreciseIP ++;
					if(wpConfig.dontFixIP == false) {
						patchedIP = GetPatchedIPShared(contextIP, me);
						if(!IsPCSane(contextIP, patchedIP)) {
							//EMSG("get_previous_instruction  failed \n");
							threadDataTable.hashTable[me].numInsaneIP ++;
							goto ErrExit;
						}
						reliableIP = patchedIP;
					} else {
						// Fake as requested by Xu for reuse clients
						reliableIP = contextIP-1;
					}
					//EMSG("PERF_SAMPLE_IP imprecise: %p patched to %p in WP handler\n", tmpIP, patchedIP);
				} else {
#if 0 // Precise PC can be far away in jump/call instructions.
					// Ensure the "precise" PC is within one instruction from context pc
					if(!IsPCSane(contextIP, preciseIP)) {
						tData.numInsaneIP ++;
						//EMSG("get_previous_instruction failed \n");
						goto ErrExit;
					}
#endif
					reliableIP = preciseIP;
					//if(! ((ip <= tmpIP) && (tmpIP-ip < 20))) ConsumeAllRingBufferData(wpi->mmapBuffer);
					//assert( (ip <= tmpIP) && (tmpIP-ip < 20));
				}
			} else {
				// Should happen only for wpConfig.isLBREnabled==false
				assert(wpConfig.isLBREnabled==false);
				// Fall back to old scheme of disassembling and capturing the info
				if(wpConfig.dontFixIP == false) {
					fprintf(stderr, "wpConfig.dontFixIP is false\n");
					patchedIP = GetPatchedIPShared(contextIP, me);
					if(!IsPCSane(contextIP, patchedIP)) {
						threadDataTable.hashTable[me].numInsaneIP ++;
						//EMSG("PERF_SAMPLE_IP imprecise: %p failed to patch in  WP handler, WP dropped\n", tmpIP);
						goto ErrExit;
					}
					reliableIP = patchedIP;
				}else {
					fprintf(stderr, "wpConfig.dontFixIP is true\n");
					// Fake as requested by Xu for reuse clients
					reliableIP = contextIP-1;
				}
			}

			wpt->pc = reliableIP;

			if(wpConfig.dontDisassembleWPAddress == false){
				//fprintf(stderr, "wpConfig.dontDisassembleWPAddress is false\n");
				FloatType * floatType = wpConfig.getFloatType? &wpt->floatType : 0;
				if(false == get_mem_access_length_and_type_address(wpt->pc, (uint32_t*) &(wpt->accessLength), &(wpt->accessType), floatType, context, &addr)){
					//EMSG("WP triggered on a non Load/Store add = %p\n", wpt->pc);
					goto ErrExit;
				}
				if (wpt->accessLength == 0) {
					//EMSG("WP triggered 0 access length! at pc=%p\n", wpt->pc);
					goto ErrExit;
				}


				void * patchedAddr = (void *)-1;
				// Stack affecting addresses will be off by 8
				// Some instructions affect the address computing register: mov    (%rax),%eax
				// Hence, if the addresses do NOT overlap, merely use the Sample address!
				if(false == ADDRESSES_OVERLAP(addr, wpt->accessLength, wpi->va, wpi->sample.wpLength)) {
					if ((wpt->accessLength == sizeof(void *)) && (wpt->accessLength == wpi->sample.wpLength) &&  (((addr - wpi->va) == sizeof(void *)) || ((wpi->va - addr) == sizeof(void *))))
						threadDataTable.hashTable[me].numWatchpointImpreciseAddress8ByteLength ++;
					else
						threadDataTable.hashTable[me].numWatchpointImpreciseAddressArbitraryLength ++;


					threadDataTable.hashTable[me].numWatchpointImpreciseAddressArbitraryLength ++;
					patchedAddr = wpi->va;
				} else {
					patchedAddr = addr;
				}
				wpt->va = patchedAddr;
			} else {
				wpt->va = (void *)-1;
			}
			wpt->ctxt = context;
			// We must cleanup the mmap buffer if there is any data left
			ConsumeAllRingBufferData(wpi->mmapBuffer);
			return true;
		case PERF_RECORD_EXIT:
			EMSG("PERF_RECORD_EXIT sample type %d sz=%d\n", hdr.type, hdr.size);
			//SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
			goto ErrExit;
		case PERF_RECORD_LOST:
			EMSG("PERF_RECORD_LOST sample type %d sz=%d\n", hdr.type, hdr.size);
			//SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
			goto ErrExit;
		case PERF_RECORD_THROTTLE:
			EMSG("PERF_RECORD_THROTTLE sample type %d sz=%d\n", hdr.type, hdr.size);
			//SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
			goto ErrExit;
		case PERF_RECORD_UNTHROTTLE:
			EMSG("PERF_RECORD_UNTHROTTLE sample type %d sz=%d\n", hdr.type, hdr.size);
			//SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
			goto ErrExit;
		default:
			EMSG("unknown sample type %d sz=%d\n", hdr.type, hdr.size);
			//SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
			goto ErrExit;
	}

ErrExit:
	// We must cleanup the mmap buffer if there is any data left
	ConsumeAllRingBufferData(wpi->mmapBuffer);
ErrExit2:
	return false;
}

void DisableWatchpointWrapper(WatchPointInfo_t *wpi){
	if(wpConfig.isWPModifyEnabled) {
		DisableWatchpoint(wpi);
	} else {
		DisArm(wpi);
	}
}


static int OnWatchPoint(int signum, siginfo_t *info, void *context){
	linux_perf_events_pause();
	wp_count++;
	void* pc = hpcrun_context_pc(context);
	if (!hpcrun_safe_enter_async(pc)) {
		linux_perf_events_resume();
		return 0;
	}

	wp_count1++;

	if(event_type == WP_MT_REUSE) {


		tData.numWatchpointTriggers++;

		int location = -1;

		FdData_t fdData;
		int me;

		for(int i = 0 ; i < l1_wp_count; i++) {
                        if((tData.watchPointArray[i].isActive) && (info->si_fd == tData.watchPointArray[i].fileHandle)) {
                                location = i;
                                break;
                        }
                }

		uint64_t theCounter = - 1;
		if(location == -1) {

			if(l1_wp_count < wpConfig.maxWP) {
				//fprintf(stderr, "a candidate location for L3 profiling\n");
				fdData = fdDataGet(info->si_fd);
                		me = fdData.tid;

				for(int i = l1_wp_count; i < wpConfig.maxWP; i++) {
                                        //fprintf(stderr, "info->si_fd: %d, fd in table: %d\n", info->si_fd, threadDataTable.hashTable[me].watchPointArray[i].fileHandle);
                                        if(threadDataTable.hashTable[me].watchPointArray[i].isActive /*&& globalWPIsActive[i]*/ && (info->si_fd == threadDataTable.hashTable[me].watchPointArray[i].fileHandle)) {
					       	location = i;
						//theCounter = threadDataTable.hashTable[me].counter;
						//fprintf(stderr, "location is found in %d\n", location);
                                                break;
                                        }
				}	
			}
		}

		//fprintf(stderr, "in OnWatchpoint after location selection\n");
		if(location == -1) {
			if(l1_wp_count <= location)
                                fprintf(stderr, "in OnWatchpoint trap in thread %d handled by thread %d is rejected\n", me, TD_GET(core_profile_trace_data.id));
			EMSG("\n WP trigger did not match any known active WP\n");
			//monitor_real_abort();
			hpcrun_safe_exit();
			linux_perf_events_resume();
			//fprintf(stderr, "WP trigger did not match any known active WP\n");
			return 0;
		}

		if(location < l1_wp_count) {
		wp_count2++;

		WatchPointTrigger_t wpt;
		WPTriggerActionType retVal;

		WatchPointInfo_t *wpi = &tData.watchPointArray[location];

		//fprintf(stderr, "in OnWatchpoint before preWPAction\n");
		// Perform Pre watchpoint action
		switch (wpi->sample.preWPAction) {
			case DISABLE_WP:
				//fprintf(stderr, "in DISABLE_WP\n");
				//if(location < l1_wp_count)
				DisableWatchpointWrapper(wpi);
				break;
			default:
				//fprintf(stderr, "aborted here\n");
				assert(0 && "NYI");
				monitor_real_abort();
				break;
		}

		//fprintf(stderr, "in OnWatchpoint before CollectWatchPointTriggerInfo\n");
		if( false == CollectWatchPointTriggerInfo(wpi, &wpt, context)) {
			tData.numWatchpointDropped++;
			retVal = DISABLE_WP; // disable if unable to collect any info.
			wp_dropped++;
			//wp_dropped_counter++;
		} else {
			tData.numActiveWatchpointTriggers++;
			retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
			//wp_dropped_counter = 0;
		}

		switch (retVal) {
			case ALREADY_DISABLED: { // Already disabled, perhaps in pre-WP actio
						       assert(wpi->isActive == false);
						       tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
						       tData.numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
					       }
					       break;
			default: // Retain the state
					break;
		}
		} else {

			//fprintf(stderr, "this region has been entered\n");
			//uint64_t theCounter = threadDataTable.hashTable[me].counter;
			//do {
			uint64_t theCounter = threadDataTable.hashTable[me].counter[location];
                        if((theCounter & 1) == 0) {
                        if(__sync_bool_compare_and_swap(&threadDataTable.hashTable[me].counter[location], theCounter, theCounter+1)){
				//fprintf(stderr, "this region is entered\n");
				//fprintf(stderr, "WP in location %d due to trap in thread %d handled by thread %d is about to be handled\n", location, me, TD_GET(core_profile_trace_data.id));
				wp_count2++;

				WatchPointTrigger_t wpt;
				WPTriggerActionType retVal;

				WatchPointInfo_t *wpi = &threadDataTable.hashTable[me].watchPointArray[location];
				bool handle_flag =false;
				switch (wpi->sample.preWPAction) {
					case DISABLE_WP:
					//fprintf(stderr, "in DISABLE_WP\n");
					DisableWatchpointWrapper(wpi);
					if(location >= l1_wp_count)
						globalWPIsActive[location] = false;
					/*if(globalWPIsActive[location]) {
						globalWPIsActive[location] = false;
						handle_flag = true;
					}*/
					break;
				default:
					//fprintf(stderr, "aborted here\n");
					assert(0 && "NYI");
					threadDataTable.hashTable[me].counter[location]++;
					monitor_real_abort();
					break;
				}

				if( (false == CollectWatchPointTriggerInfoShared(wpi, &wpt, context, me)) /*|| !handle_flag*/) {
					tData.numWatchpointDropped++;
					retVal = DISABLE_WP; // disable if unable to collect any info.
					wp_dropped++;
					//wp_dropped_counter++;
				} else {
					tData.numActiveWatchpointTriggers++;
					//fprintf(stderr, "in OnWatchpoint before fptr in thread %d handled by thread %d\n", me, TD_GET(core_profile_trace_data.id));
					if(me == wpi->sample.first_accessing_tid) {
						do {
							uint64_t theCounter = globalReuseWPs.table[location].counter;
                                        if((theCounter & 1) == 0) {
                                        if(__sync_bool_compare_and_swap(&globalReuseWPs.table[location].counter, theCounter, theCounter+1)) {
                                        if((wpi->sample.sampleTime == globalReuseWPs.table[location].time) && (wpi->sample.first_accessing_tid == globalReuseWPs.table[location].tid)) {
                                                wpt.location = location;
						retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
                                                numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
                                        }
                                        globalReuseWPs.table[location].counter++;
					break;
                                        }
                                        }
						} while(1);
					} else {
					uint64_t theCounter = globalReuseWPs.table[location].counter;
					if((theCounter & 1) == 0) {
                        		if(__sync_bool_compare_and_swap(&globalReuseWPs.table[location].counter, theCounter, theCounter+1)) {
					if((wpi->sample.sampleTime == globalReuseWPs.table[location].time) && (wpi->sample.first_accessing_tid == globalReuseWPs.table[location].tid) && (globalReuseWPs.table[location].active == true)) {
						wpt.location = location;
						retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
						numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL; 
						globalReuseWPs.table[location].active = false;
					}
					globalReuseWPs.table[location].counter++;
					}
					}
					}
					retVal = ALREADY_DISABLED;
					//wp_dropped_counter = 0;
				}

				switch (retVal) {
					case ALREADY_DISABLED: { // Already disabled, perhaps in pre-WP action
						       assert(wpi->isActive == false);
						       tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;					       
							threadDataTable.hashTable[me].numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
					       }
					       break;
					default: // Retain the state
						break;
				}
				threadDataTable.hashTable[me].counter[location]++;
				//break;
			}
			}			

			//} while(1);
		}
	} else if(event_type == WP_REUSE_MT) {
		tData.numWatchpointTriggers++;

		int location = -1;

		FdData_t fdData = fdDataGet(info->si_fd);
		int me = fdData.tid;

	// before
	for(int i = 0 ; i < same_thread_l1_wp_count; i++) {
        	if((tData.watchPointArray[i].isActive) && (info->si_fd == tData.watchPointArray[i].fileHandle)) {
                	location = i;
                        break;
          	}
   	}

 	uint64_t theCounter = - 1;
  	if(location == -1) {
	// after

		for(int i = same_thread_l1_wp_count; i < wpConfig.maxWP; i++) {
                	if(threadDataTable.hashTable[me].watchPointArray[i].isActive && (info->si_fd == threadDataTable.hashTable[me].watchPointArray[i].fileHandle)) {
				location = i;
				//theCounter = threadDataTable.hashTable[me].counter;
				//fprintf(stderr, "location is found in %d\n", location);
                               	break;
			}
		}

	}

		if(location == -1) {
                        EMSG("\n WP trigger did not match any known active WP\n");
			//monitor_real_abort();
			hpcrun_safe_exit();
			linux_perf_events_resume();
			//fprintf(stderr, "WP trigger did not match any known active WP\n");
			return 0;
		}

		if(location < same_thread_l1_wp_count) {
                wp_count2++;

                WatchPointTrigger_t wpt;
                WPTriggerActionType retVal;

                WatchPointInfo_t *wpi = &tData.watchPointArray[location];

                //fprintf(stderr, "in OnWatchpoint before preWPAction\n");
                // Perform Pre watchpoint action 
                switch (wpi->sample.preWPAction) {
                        case DISABLE_WP:
                                //fprintf(stderr, "in DISABLE_WP\n");
                                //if(location < l1_wp_count)
                                DisableWatchpointWrapper(wpi);
                                break;
                        default:
                                //fprintf(stderr, "aborted here\n");
                                assert(0 && "NYI");
                                monitor_real_abort();
                                break;
                }

                //fprintf(stderr, "in OnWatchpoint before CollectWatchPointTriggerInfo\n");
                if( false == CollectWatchPointTriggerInfo(wpi, &wpt, context)) {
                        tData.numWatchpointDropped++;
                        retVal = DISABLE_WP; // disable if unable to collect any info.
                        wp_dropped++;
                        //wp_dropped_counter++;
                } else {
                        tData.numActiveWatchpointTriggers++;
			wpt.location = location;
                        retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
                        //wp_dropped_counter = 0;
                }

		switch (retVal) {
                        case ALREADY_DISABLED: { // Already disabled, perhaps in pre-WP actio
                                                       assert(wpi->isActive == false);
                                                       tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
                                                       tData.numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
                                               }
                                               break;
                        default: // Retain the state
                                        break;
                }
	
		} else {

			//fprintf(stderr, "this region has been entered\n");
			uint64_t theCounter = threadDataTable.hashTable[me].counter[location];
                        if((theCounter & 1) == 0) {
                        if(__sync_bool_compare_and_swap(&threadDataTable.hashTable[me].counter[location], theCounter, theCounter+1)){
				//fprintf(stderr, "this region is entered\n");
				//fprintf(stderr, "WP in location %d due to trap in thread %d handled by thread %d is about to be handled\n", location, me, TD_GET(core_profile_trace_data.id));
				wp_count2++;

				WatchPointTrigger_t wpt;
				WPTriggerActionType retVal;

				WatchPointInfo_t *wpi = &threadDataTable.hashTable[me].watchPointArray[location];
				bool handle_flag =false;
				switch (wpi->sample.preWPAction) {
					case DISABLE_WP:
					//fprintf(stderr, "in DISABLE_WP\n");
					DisableWatchpointWrapper(wpi);
					globalWPIsActive[location] = false;	
					break;
				default:
					//fprintf(stderr, "aborted here\n");
					assert(0 && "NYI");
					threadDataTable.hashTable[me].counter[location]++;
					monitor_real_abort();
					break;
				}

				if( (false == CollectWatchPointTriggerInfoShared(wpi, &wpt, context, me))) {
					tData.numWatchpointDropped++;
					retVal = DISABLE_WP; // disable if unable to collect any info.
					wp_dropped++;
				} else {
					tData.numActiveWatchpointTriggers++;
					//fprintf(stderr, "in OnWatchpoint before fptr in thread %d handled by thread %d\n", me, TD_GET(core_profile_trace_data.id));
					
					if(location < l1_wp_count) {
						uint64_t theCounter = globalReuseWPs.table[location].counter;
                                        	if((theCounter & 1) == 0) {
                                        	if(__sync_bool_compare_and_swap(&globalReuseWPs.table[location].counter, theCounter, theCounter+1)) {
                                        	if((wpi->sample.sampleTime == globalReuseWPs.table[location].time) && (wpi->sample.first_accessing_tid == globalReuseWPs.table[location].tid) && (globalReuseWPs.table[location].active == true)) {
                                                	retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
                                                	numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
                                                	globalReuseWPs.table[location].active = false;
                                        	}
                                        	globalReuseWPs.table[location].counter++;
                                        	}
                                        	}
					} else if(me == wpi->sample.first_accessing_tid) {
                                                do {
                                                        uint64_t theCounter = globalReuseWPs.table[location].counter;
                                        if((theCounter & 1) == 0) {
                                        if(__sync_bool_compare_and_swap(&globalReuseWPs.table[location].counter, theCounter, theCounter+1)) {
                                        if((wpi->sample.sampleTime == globalReuseWPs.table[location].time) && (wpi->sample.first_accessing_tid == globalReuseWPs.table[location].tid)) {
                                                wpt.location = location;
                                                retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
                                                numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
                                        }
                                        globalReuseWPs.table[location].counter++;
                                        break;
                                        }
                                        }
                                                } while(1);
                                        } else {
                                        uint64_t theCounter = globalReuseWPs.table[location].counter;
                                        if((theCounter & 1) == 0) {
                                        if(__sync_bool_compare_and_swap(&globalReuseWPs.table[location].counter, theCounter, theCounter+1)) {
                                        if((wpi->sample.sampleTime == globalReuseWPs.table[location].time) && (wpi->sample.first_accessing_tid == globalReuseWPs.table[location].tid) && (globalReuseWPs.table[location].active == true)) {
                                                wpt.location = location;
                                                retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
                                                numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
                                                globalReuseWPs.table[location].active = false;
                                        }
                                        globalReuseWPs.table[location].counter++;
                                        }
                                        }
                                        }
					/*uint64_t theCounter = globalReuseWPs.table[location].counter;
					if((theCounter & 1) == 0) {
                        		if(__sync_bool_compare_and_swap(&globalReuseWPs.table[location].counter, theCounter, theCounter+1)) {
					if((wpi->sample.sampleTime == globalReuseWPs.table[location].time) && (wpi->sample.first_accessing_tid == globalReuseWPs.table[location].tid) && (globalReuseWPs.table[location].active == true)) {
						retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
						numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL; 
						globalReuseWPs.table[location].active = false;
					}
					globalReuseWPs.table[location].counter++;
					}
					}*/
					retVal = ALREADY_DISABLED;
				}

				switch (retVal) {
					case ALREADY_DISABLED: { // Already disabled, perhaps in pre-WP action
						       assert(wpi->isActive == false);
						       tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;					       
							threadDataTable.hashTable[me].numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
					       }
					       break;
					default: // Retain the state
						break;
				}
				threadDataTable.hashTable[me].counter[location]++;
				//break;
			}
			}
		}
	} else {


		tData.numWatchpointTriggers++;

		int location = -1;

		for(int i = 0 ; i < wpConfig.maxWP; i++) {
			if((tData.watchPointArray[i].isActive) && (info->si_fd == tData.watchPointArray[i].fileHandle)) {
				location = i;
				break;
			}
		}

		//fprintf(stderr, "in OnWatchpoint after location selection\n");
		if(location == -1) {
			EMSG("\n WP trigger did not match any known active WP\n");
			//monitor_real_abort();
			hpcrun_safe_exit();
			linux_perf_events_resume();
			//fprintf(stderr, "WP trigger did not match any known active WP\n");
			return 0;
		}
		wp_count2++;

		WatchPointTrigger_t wpt;
		WPTriggerActionType retVal;
		// WP_REUSE_MT until here
		WatchPointInfo_t *wpi = &tData.watchPointArray[location];

		//fprintf(stderr, "in OnWatchpoint before preWPAction\n");
		// Perform Pre watchpoint action
		switch (wpi->sample.preWPAction) {
			case DISABLE_WP:
				//fprintf(stderr, "in DISABLE_WP\n");
				DisableWatchpointWrapper(wpi);
				break;
			case DISABLE_ALL_WP:
				for(int i = 0; i < wpConfig.maxWP; i++) {
					if(tData.watchPointArray[i].isActive){
						DisableWatchpointWrapper(&tData.watchPointArray[i]);
					}
				}
				break;
			default:
				//fprintf(stderr, "aborted here\n");
				assert(0 && "NYI");
				monitor_real_abort();
				break;
		}

		//fprintf(stderr, "in OnWatchpoint before CollectWatchPointTriggerInfo\n");
		if( false == CollectWatchPointTriggerInfo(wpi, &wpt, context)) {
			tData.numWatchpointDropped++;
			retVal = DISABLE_WP; // disable if unable to collect any info.
			wp_dropped++;
			//wp_dropped_counter++;
		} else {
			tData.numActiveWatchpointTriggers++;
			//fprintf(stderr, "in OnWatchpoint before fptr\n");
			retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
			//wp_dropped_counter = 0;
		}

		switch (retVal) {
			case DISABLE_WP: {
						 if(wpi->isActive){
							 DisableWatchpointWrapper(wpi);
						 }

						 tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
					 }
					 break;
			case DISABLE_ALL_WP: {
						     for(int i = 0; i < wpConfig.maxWP; i++) {
							     if(tData.watchPointArray[i].isActive){
								     DisableWatchpointWrapper(&tData.watchPointArray[i]);
							     }
						     }
						     tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
					     }
					     break;
			case ALREADY_DISABLED: { // Already disabled, perhaps in pre-WP action
						       assert(wpi->isActive == false);
						       tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
						       if (wpConfig.replacementPolicy == RDX) {
							       tData.numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
						       }
					       }
					       break;
			case RETAIN_WP: { // resurrect this wp
						if(!wpi->isActive){
							EnableWatchpoint(wpi->fileHandle);
							wpi->isActive = true;
						}
					}
					break;
			default: // Retain the state
					break;
		}
	}

	hpcrun_safe_exit();
	linux_perf_events_resume();
	return 0;
}

/*
static int OnWatchPoint(int signum, siginfo_t *info, void *context){

	linux_perf_events_pause();
	wp_count++;
	void* pc = hpcrun_context_pc(context);
	if (!hpcrun_safe_enter_async(pc)) {
		linux_perf_events_resume();
		return 0;
	}
	wp_count1++;

	if(event_type != WP_REUSE_MT) {


		tData.numWatchpointTriggers++;

		int location = -1;

		for(int i = 0 ; i < wpConfig.maxWP; i++) {
			if((tData.watchPointArray[i].isActive) && (info->si_fd == tData.watchPointArray[i].fileHandle)) {
				location = i;
				break;
			}
		}

		if(location == -1) {
			EMSG("\n WP trigger did not match any known active WP\n");
			//monitor_real_abort();
			hpcrun_safe_exit();
			linux_perf_events_resume();
			//fprintf("\n WP trigger did not match any known active WP\n");
			return 0;
		}
		wp_count2++;

		WatchPointTrigger_t wpt;
		WPTriggerActionType retVal;
		// WP_REUSE_MT until here
		WatchPointInfo_t *wpi = &tData.watchPointArray[location];

		// Perform Pre watchpoint action
		switch (wpi->sample.preWPAction) {
			case DISABLE_WP:
				//fprintf(stderr, "in DISABLE_WP\n");
				DisableWatchpointWrapper(wpi);
				break;
			case DISABLE_ALL_WP:
				for(int i = 0; i < wpConfig.maxWP; i++) {
					if(tData.watchPointArray[i].isActive){
						DisableWatchpointWrapper(&tData.watchPointArray[i]);
					}
				}
				break;
			default:
				assert(0 && "NYI");
				monitor_real_abort();
				break;
		}

		if( false == CollectWatchPointTriggerInfo(wpi, &wpt, context)) {
			tData.numWatchpointDropped++;
			retVal = DISABLE_WP; // disable if unable to collect any info.
			wp_dropped++;
		} else {
			tData.numActiveWatchpointTriggers++;
			retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
		}

		switch (retVal) {
			case DISABLE_WP: {
						 if(wpi->isActive){
							 DisableWatchpointWrapper(wpi);
						 }

						 tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
					 }
					 break;
			case DISABLE_ALL_WP: {
						     for(int i = 0; i < wpConfig.maxWP; i++) {
							     if(tData.watchPointArray[i].isActive){
								     DisableWatchpointWrapper(&tData.watchPointArray[i]);
							     }
						     }
						     tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
					     }
					     break;
			case ALREADY_DISABLED: { // Already disabled, perhaps in pre-WP action
						       assert(wpi->isActive == false);
						       tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
						       if (wpConfig.replacementPolicy == RDX) {
							       tData.numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
						       }
					       }
					       break;
			case RETAIN_WP: { // resurrect this wp
						if(!wpi->isActive){
							EnableWatchpoint(wpi->fileHandle);
							wpi->isActive = true;
						}
					}
					break;
			default: // Retain the state
					break;
		}
	} else {

		int location = -1;
		FdData_t fdData = fdDataGet(info->si_fd);

		int me = fdData.tid;

		// pause counters of the other thread here
		//linux_perf_events_other_thread_pause(me);

		int loop_counter = 0;
		int threshold = 50;
		do {
			uint64_t theCounter = threadDataTable.hashTable[me].counter;
			if(theCounter & 1) {
				if(TD_GET(core_profile_trace_data.id) != me)
					break;
				loop_counter++;
				if(loop_counter > threshold) {
					//fprintf(stderr, "watchpoint handling is discarded\n");
					break;
				}
				continue;
			}
			if(__sync_bool_compare_and_swap(&threadDataTable.hashTable[me].counter, theCounter, theCounter+1)){
				//fprintf(stderr, "watchpoint handling is entered\n");

				for(int i = 0 ; i < wpConfig.maxWP; i++) {
					//fprintf(stderr, "info->si_fd: %d, fd in table: %d\n", info->si_fd, threadDataTable.hashTable[fdData.tid].watchPointArray[i].fileHandle);
					if((threadDataTable.hashTable[fdData.tid].watchPointArray[i].isActive) && (info->si_fd == threadDataTable.hashTable[fdData.tid].watchPointArray[i].fileHandle)) {
						//fprintf(stderr, "location is found in WP_REUSE_MT\n");
						location = i;
						break;
					}
				}
				if(location == -1) {
					EMSG("\n WP trigger did not match any known active WP\n");
					//monitor_real_abort();
					threadDataTable.hashTable[me].counter++;
					//linux_perf_events_other_thread_resume(me);
					hpcrun_safe_exit();
					linux_perf_events_resume();
					//fprintf("\n WP trigger did not match any known active WP\n");
					return 0;
				}
				wp_count2++;

				WatchPointTrigger_t wpt;
				WPTriggerActionType retVal;

				WatchPointInfo_t *wpi = &threadDataTable.hashTable[fdData.tid].watchPointArray[location];

				switch (wpi->sample.preWPAction) {
					case DISABLE_WP:
						//fprintf(stderr, "in DISABLE_WP\n");
						DisableWatchpointWrapper(wpi);
						break;
					case DISABLE_ALL_WP:
						for(int i = 0; i < wpConfig.maxWP; i++) {
							if(threadDataTable.hashTable[fdData.tid].watchPointArray[i].isActive){
								DisableWatchpointWrapper(&threadDataTable.hashTable[fdData.tid].watchPointArray[i]);
							}
						}
						break;
					default:
						threadDataTable.hashTable[me].counter++;
						assert(0 && "NYI");
						monitor_real_abort();
						break;
				}

				if( false == CollectWatchPointTriggerInfoShared(wpi, &wpt, context, fdData.tid)) {
					//fprintf(stderr, "numWatchpointDropped in WP_REUSE_MT\n");
					threadDataTable.hashTable[fdData.tid].numWatchpointDropped++;
					retVal = DISABLE_WP; // disable if unable to collect any info.
					wp_dropped++;
				} else {
					//fprintf(stderr, "in OnWatchpoint at that point 1!!!!\n");
					//fprintf(stderr, "numActiveWatchpointTriggers is incremented in WP_REUSE_MT\n");
					wpi->trap_origin_tid = fdData.tid;
					threadDataTable.hashTable[fdData.tid].numActiveWatchpointTriggers++;
					retVal = threadDataTable.hashTable[fdData.tid].fptr(wpi, 0, wpt.accessLength,  &wpt);
				}

				switch (retVal) {
					case DISABLE_WP: {
								 if(wpi->isActive){
									 DisableWatchpointWrapper(wpi);
								 }
								 threadDataTable.hashTable[fdData.tid].samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
								 //tData.numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
								 fprintf(stderr, "tData.samplePostFull is reset in DISABLE_WP in thread %d\n", TD_GET(core_profile_trace_data.id));
							 }
							 break;
					case DISABLE_ALL_WP: {
								     for(int i = 0; i < wpConfig.maxWP; i++) {
									     if(threadDataTable.hashTable[fdData.tid].watchPointArray[i].isActive){
										     DisableWatchpointWrapper(&threadDataTable.hashTable[fdData.tid].watchPointArray[i]);
									     }
								     }
								     //reset to tData.samplePostFull to SAMPLES_POST_FULL_RESET_VAL
								     threadDataTable.hashTable[fdData.tid].samplePostFull = SAMPLES_POST_FULL_RESET_VAL;

							     }
							     break;
					case ALREADY_DISABLED: { // Already disabled, perhaps in pre-WP action
								       assert(wpi->isActive == false);
								       threadDataTable.hashTable[fdData.tid].samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
								       if (wpConfig.replacementPolicy == RDX) {
									       threadDataTable.hashTable[fdData.tid].numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
									       //fprintf(stderr, "watchpoint %d is reset due to trap\n", location);
								       }
								       //fprintf(stderr, "tData.samplePostFull is reset in ALREADY_DISABLED in thread %d\n", TD_GET(core_profile_trace_data.id));
							       }
							       break;
					case RETAIN_WP: { // resurrect this wp
								if(!wpi->isActive){
									EnableWatchpoint(wpi->fileHandle);
									wpi->isActive = true;
								}
							}
							break;
					default: // Retain the state
							break;
				}

				threadDataTable.hashTable[me].counter++;
				break;
			}
		} while(1);

		//linux_perf_events_other_thread_resume(me);
	}
	//    hpcrun_all_sources_start();
	//linux_perf_events_resume();
	hpcrun_safe_exit();
	linux_perf_events_resume();
	return 0;
}*/


/*
   static int OnWatchPoint(int signum, siginfo_t *info, void *context){
   linux_perf_events_pause();
   wp_count++;
   void* pc = hpcrun_context_pc(context);
   if (!hpcrun_safe_enter_async(pc)) {
   linux_perf_events_resume();
   return 0;
   }

   wp_count1++;

   FdData_t fdData = fdDataGet(info->si_fd);
   if(event_type == WP_REUSE_MT) {
   threadDataTable.hashTable[fdData.tid].numWatchpointTriggers++;
   }
   else {
   tData.numWatchpointTriggers++;
   }
   int location = -1;
   if(event_type == WP_REUSE_MT) {
   for(int i = 0 ; i < wpConfig.maxWP; i++) {
//fprintf(stderr, "info->si_fd: %d, fd in table: %d\n", info->si_fd, threadDataTable.hashTable[fdData.tid].watchPointArray[i].fileHandle);
if((threadDataTable.hashTable[fdData.tid].watchPointArray[i].isActive) && (info->si_fd == threadDataTable.hashTable[fdData.tid].watchPointArray[i].fileHandle)) {
fprintf(stderr, "location is found in WP_REUSE_MT\n");
location = i;
break;
}
}
} else {
for(int i = 0 ; i < wpConfig.maxWP; i++) {
if((tData.watchPointArray[i].isActive) && (info->si_fd == tData.watchPointArray[i].fileHandle)) {
location = i;
break;
}
}
}    
//fprintf(stderr, "in OnWatchpoint at this point\n");
// Ensure it is an active WP
if(location == -1) {
//fprintf(stderr, "location is not found\n");
// before
//for(int i = 0 ; i < wpConfig.maxWP; i++) {
//if((tData.watchPointArray[i].isActive) && (info->si_fd == tData.watchPointArray[i].fileHandle)) {
//location = i;
//break;
//fprintf(stderr, "tData.watchPointArray[%d].isActive = %d and info->si_fd = %d and tData.watchPointArray[%d].fileHandle = %d monitored address: %lx\n", i, tData.watchPointArray[i].isActive, info->si_fd, i, tData.watchPointArray[i].fileHandle, (long) tData.watchPointArray[i].va);
//}
//}
// after
EMSG("\n WP trigger did not match any known active WP\n");
//monitor_real_abort();
hpcrun_safe_exit();
linux_perf_events_resume();
//fprintf("\n WP trigger did not match any known active WP\n");
return 0;
}
wp_count2++;

WatchPointTrigger_t wpt;
WPTriggerActionType retVal;
// WP_REUSE_MT until here
WatchPointInfo_t *wpi;
if(event_type == WP_REUSE_MT) {
//fprintf(stderr, "numWatchpointTriggers is incremented in WP_REUSE_MT\n");
wpi = &threadDataTable.hashTable[fdData.tid].watchPointArray[location];
} else {
wpi = &tData.watchPointArray[location];
}
// Perform Pre watchpoint action
switch (wpi->sample.preWPAction) {
case DISABLE_WP:
//fprintf(stderr, "in DISABLE_WP\n");
DisableWatchpointWrapper(wpi);
break;
case DISABLE_ALL_WP:
if(event_type == WP_REUSE_MT) {
	for(int i = 0; i < wpConfig.maxWP; i++) {
		if(threadDataTable.hashTable[fdData.tid].watchPointArray[i].isActive){
			DisableWatchpointWrapper(&threadDataTable.hashTable[fdData.tid].watchPointArray[i]);
		}
	}
} else {
	for(int i = 0; i < wpConfig.maxWP; i++) {
		if(tData.watchPointArray[i].isActive){
			DisableWatchpointWrapper(&tData.watchPointArray[i]);
		}
	}
}
break;
default:
assert(0 && "NYI");
monitor_real_abort();
break;
}

//fprintf(stderr, "in OnWatchpoint at that point\n"); 
bool info_retrieved;
if(event_type == WP_REUSE_MT) {
	info_retrieved = CollectWatchPointTriggerInfoShared(wpi, &wpt, context, fdData.tid);
} else {
	info_retrieved = CollectWatchPointTriggerInfo(wpi, &wpt, context);
}
if( false == info_retrieved) {
	//fprintf(stderr, "in OnWatchpoint at that point 3!!!!\n");
	if(event_type == WP_REUSE_MT) {
		fprintf(stderr, "numWatchpointDropped in WP_REUSE_MT\n");
		threadDataTable.hashTable[fdData.tid].numWatchpointDropped++;
	}
	else {
		tData.numWatchpointDropped++;
	}
	retVal = DISABLE_WP; // disable if unable to collect any info.
	wp_dropped++;
} else {
	//fprintf(stderr, "in OnWatchpoint at that point 1!!!!\n");
	if(event_type == WP_REUSE_MT) {
		fprintf(stderr, "numActiveWatchpointTriggers is incremented in WP_REUSE_MT\n");
		wpi->trap_origin_tid = fdData.tid;
		threadDataTable.hashTable[fdData.tid].numActiveWatchpointTriggers++;
		retVal = threadDataTable.hashTable[fdData.tid].fptr(wpi, 0, wpt.accessLength,  &wpt);
	}
	else {
		tData.numActiveWatchpointTriggers++;
		retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
	}
	//retVal = tData.fptr(wpi, 0, wpt.accessLength,  &wpt);
	//fprintf(stderr, "in OnWatchpoint at that point 2!!!!\n");
}
//fprintf(stderr, "in OnWatchpoint at that point !!!\n");

// Let the client take action.
switch (retVal) {
	case DISABLE_WP: {
				 if(wpi->isActive){
					 DisableWatchpointWrapper(wpi);
				 }
				 //reset to tData.samplePostFull
				 if(event_type == WP_REUSE_MT) {
					 threadDataTable.hashTable[fdData.tid].samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
				 } else {
					 tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
				 }
				 //tData.numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
				 fprintf(stderr, "tData.samplePostFull is reset in DISABLE_WP in thread %d\n", TD_GET(core_profile_trace_data.id));
			 }
			 break;
	case DISABLE_ALL_WP: {
				     if(event_type == WP_REUSE_MT) {
					     for(int i = 0; i < wpConfig.maxWP; i++) {
						     if(threadDataTable.hashTable[fdData.tid].watchPointArray[i].isActive){
							     DisableWatchpointWrapper(&threadDataTable.hashTable[fdData.tid].watchPointArray[i]);
						     }
					     }
					     //reset to tData.samplePostFull to SAMPLES_POST_FULL_RESET_VAL
					     threadDataTable.hashTable[fdData.tid].samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
				     } else {	    
					     for(int i = 0; i < wpConfig.maxWP; i++) {
						     if(tData.watchPointArray[i].isActive){
							     DisableWatchpointWrapper(&tData.watchPointArray[i]);
						     }
					     }
					     //reset to tData.samplePostFull to SAMPLES_POST_FULL_RESET_VAL
					     tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
				     }
				     //tData.numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
				     //fprintf(stderr, "tData.samplePostFull is reset in DISABLE_ALL_WP in thread %d\n", TD_GET(core_profile_trace_data.id));
			     }
			     break;
	case ALREADY_DISABLED: { // Already disabled, perhaps in pre-WP action
				       assert(wpi->isActive == false);
				       if(event_type == WP_REUSE_MT) {
					       threadDataTable.hashTable[fdData.tid].samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
					       if (wpConfig.replacementPolicy == RDX) {
						       threadDataTable.hashTable[fdData.tid].numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
						       //fprintf(stderr, "watchpoint %d is reset due to trap\n", location);
					       }
				       } else {
					       tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
					       if (wpConfig.replacementPolicy == RDX) {
						       tData.numWatchpointArmingAttempt[location] = SAMPLES_POST_FULL_RESET_VAL;
						       //fprintf(stderr, "watchpoint %d is reset due to trap\n", location);
					       }
				       }
				       //fprintf(stderr, "tData.samplePostFull is reset in ALREADY_DISABLED in thread %d\n", TD_GET(core_profile_trace_data.id));
			       }
			       break;
	case RETAIN_WP: { // resurrect this wp
				if(!wpi->isActive){
					EnableWatchpoint(wpi->fileHandle);
					wpi->isActive = true;
				}
			}
			break;
	default: // Retain the state
			break;
}
//    hpcrun_all_sources_start();
//linux_perf_events_resume();
hpcrun_safe_exit();
linux_perf_events_resume();
return 0;
}*/

static bool ValidateWPData(SampleData_t * sampleData){
	// Check alignment
#if defined(__x86_64__) || defined(__amd64__) || defined(__x86_64) || defined(__amd64)
	switch (sampleData->wpLength) {
		case 0: EMSG("\nValidateWPData: 0 length WP never allowed"); fprintf(stderr, "\nerror: ValidateWPData: 0 length WP never allowed\n"); monitor_real_abort();
		case 1:
		case 2:
		case 4:
		case 8:
			if(IS_ALIGNED(sampleData->va, sampleData->wpLength))
				return true; // unaligned
			else
				return false;
			break;

		default:
			EMSG("Unsuppported WP length %d", sampleData->wpLength);
			fprintf(stderr, "error: Unsuppported WP length %d\n", sampleData->wpLength);
			monitor_real_abort();
			return false; // unsupported alignment
	}
#else
#error "unknown architecture"
#endif
}

static bool IsOveralppedShared(SampleData_t * sampleData, int me, bool profile_l1){
	// Is a WP with the same/overlapping address active?
	//ThreadData_t threadData = threadDataTable.hashTable[me];
	if(profile_l1) {
	// for WP_MT_REUSE
	if (event_type == WP_MT_REUSE) {
		for (int i = 0;  i < l1_wp_count; i++) {
                	if(tData.watchPointArray[i].isActive){
                        	if(ADDRESSES_OVERLAP(tData.watchPointArray[i].sample.va, tData.watchPointArray[i].sample.wpLength, sampleData->va, sampleData->wpLength)){

                                	//fprintf(stderr, "address %lx and address %lx overlap\n", tData.watchPointArray[i].sample.va, sampleData->va);
                                	overlap_count++;
                                	return true;
                        	}
                	}
        	}	
	} else {
		for (int i = 0;  i < same_thread_l1_wp_count; i++) {
                        if(tData.watchPointArray[i].isActive){
                                if(ADDRESSES_OVERLAP(tData.watchPointArray[i].sample.va, tData.watchPointArray[i].sample.wpLength, sampleData->va, sampleData->wpLength)){

                                        //fprintf(stderr, "address %lx and address %lx overlap\n", tData.watchPointArray[i].sample.va, sampleData->va);
                                        overlap_count++;
                                        return true;
                                }
                        }
                }	
	}
	} else {
		for (int i = l1_wp_count;  i < wpConfig.maxWP; i++) {
			if(threadDataTable.hashTable[me].watchPointArray[i].isActive){
				if(ADDRESSES_OVERLAP(threadDataTable.hashTable[me].watchPointArray[i].sample.va, threadDataTable.hashTable[me].watchPointArray[i].sample.wpLength, sampleData->va, sampleData->wpLength) && (threadDataTable.hashTable[me].watchPointArray[i].sample.first_accessing_tid == sampleData->first_accessing_tid)){

					//fprintf(stderr, "address %lx and address %lx overlap\n", tData.watchPointArray[i].sample.va, sampleData->va);
					overlap_count++;    
					return true;
				}
			}
		}
	}
	return false;
}

static bool IsOveralpped(SampleData_t * sampleData){
	// Is a WP with the same/overlapping address active?
	for (int i = 0;  i < wpConfig.maxWP; i++) {
		if(tData.watchPointArray[i].isActive){
			if(ADDRESSES_OVERLAP(tData.watchPointArray[i].sample.va, tData.watchPointArray[i].sample.wpLength, sampleData->va, sampleData->wpLength)){

				//fprintf(stderr, "address %lx and address %lx overlap\n", tData.watchPointArray[i].sample.va, sampleData->va);
				overlap_count++;
				return true;
			}
		}
	}
	return false;
}

void CaptureValue(SampleData_t * sampleData, WatchPointInfo_t * wpi){
	void * valLoc = & (wpi->value[0]);
	switch(sampleData->wpLength) {
		default: // force 1 length
		case 1: *((uint8_t*)valLoc) = *(uint8_t*)(sampleData->va); break;
		case 2: *((uint16_t*)valLoc) = *(uint16_t*)(sampleData->va); break;
		case 4: *((uint32_t*)valLoc) = *(uint32_t*)(sampleData->va); break;
		case 8: *((uint64_t*)valLoc) = *(uint64_t*)(sampleData->va); break;
	}
}


bool SubscribeWatchpoint(SampleData_t * sampleData, OverwritePolicy overwritePolicy, bool captureValue){
	sub_wp_count1++;
	//fprintf(stderr, "in SubscribeWatchpoint\n");
	if(ValidateWPData(sampleData) == false) {
		return false;
	}
	//fprintf(stderr, "in SubscribeWatchpoint after ValidateWPData\n");
	//sub_wp_count2++;
	if(IsOveralpped(sampleData)){
		//fprintf(stderr, "subscribing is dropped because of overlapping\n");
		return false; // drop the sample if it overlaps an existing address
	}
	//fprintf(stderr, "in SubscribeWatchpoint after IsOveralpped\n");
	sub_wp_count2++;

	// No overlap, look for a victim slot
	int victimLocation = -1;
	// Find a slot to install WP
	//linux_perf_events_events_of_thread(TD_GET(core_profile_trace_data.id));
	VictimType r = GetVictim(&victimLocation, wpConfig.replacementPolicy);
	//fprintf(stderr, "in SubscribeWatchpoint after GetVictim\n");
	sub_wp_count3++;
	if(r != NONE_AVAILABLE) {
		// VV IMP: Capture value before arming the WP.
		if(captureValue) {
			CaptureValue(sampleData, &tData.watchPointArray[victimLocation]);
		}
		// I know the error case that we have captured the value but ArmWatchPoint fails.
		// I am not handling that corner case because ArmWatchPoint() will fail with a monitor_real_abort().
		//printf("and this region\n");
		//printf("arming watchpoints\n");
		//fprintf(stderr, "watchpoint is armed\n");
		if(ArmWatchPoint(&tData.watchPointArray[victimLocation], sampleData) == false){
			//LOG to hpcrun log
			EMSG("ArmWatchPoint failed for address %p", sampleData->va);
			return false;
		}
		//fprintf(stderr, "subscribing succeeds\n");
		return true;
	}
	none_available_count++;
	return false;
}

/*
bool SubscribeWatchpointShared(SampleData_t * sampleData, OverwritePolicy overwritePolicy, bool captureValue, int me){
	//fprintf(stderr, "in SubscribeWatchpointShared, wp of thread %d is armed by thread %d\n", me, TD_GET(core_profile_trace_data.id));
	sub_wp_count1++;
	if(ValidateWPData(sampleData) == false) {
		return false;
	}
	//sub_wp_count2++;
	int tid = TD_GET(core_profile_trace_data.id);
	if(threadDataTable.hashTable[me].os_tid != -1) {
		//fprintf(stderr, "in SubscribeWatchpointShared, wp of thread %d is armed by thread %d\n", me, TD_GET(core_profile_trace_data.id));
		//if(event_type != WP_REUSE_MT) {
		if(IsOveralppedShared(sampleData, me)){
			return false; // drop the sample if it overlaps an existing address
		}
		//}
		sub_wp_count2++;
		// until here
		// No overlap, look for a victim slot
		//if((threadDataTable.hashTable[me].counter & 1) == 0)
		//printf("before accessing shared data\n");
		//printf("after accessing shared data\n");
		int victimLocation = -1;
		// Find a slot to install WP
		//VictimType r = GetVictimShared(&victimLocation, wpConfig.replacementPolicy, me);
		//fprintf(stderr, "before loop\n");
		ThreadData_t threadData = threadDataTable.hashTable[me];

		//fprintf(stderr, "in SubscribeWatchpointShared\n");

		int loop_counter = 0;
		int threshold = 50;
                do {
                        uint64_t theCounter = threadDataTable.hashTable[me].counter;
                        if(theCounter & 1) {
				if(TD_GET(core_profile_trace_data.id) != me)
					break;
				loop_counter++;
                                if(loop_counter > threshold) {
                                        if(TD_GET(core_profile_trace_data.id) == me)
                                                subscribe_dropped++;
                                        //fprintf(stderr, "arming is discarded\n");
                                        break;
                                }
                                continue;
                        }
                        //uint64_t theCounter = threadDataTable.hashTable[me].counter;
                        if(__sync_bool_compare_and_swap(&threadDataTable.hashTable[me].counter, theCounter, theCounter+1)){

				//VictimType r = GetVictim(&victimLocation, wpConfig.replacementPolicy);
				VictimType r = GetVictimShared(&victimLocation, wpConfig.replacementPolicy, me);
				sub_wp_count3++;
				if(r != NONE_AVAILABLE) {
					// VV IMP: Capture value before arming the WP.
					if(captureValue) {
						CaptureValue(sampleData, &threadDataTable.hashTable[me].watchPointArray[victimLocation]);
					}
					// I know the error case that we have captured the value but ArmWatchPoint fails.
					// I am not handling that corner case because ArmWatchPoint() will fail with a monitor_real_abort().
					//printf("and this region\n");
					//printf("arming watchpoints\n");
					//fprintf(stderr, "watchpoint is about to be armed in ArmWatchPointShared\n");
					//if(TD_GET(core_profile_trace_data.id) != 3) {
					if(ArmWatchPointShared(&threadDataTable.hashTable[me].watchPointArray[victimLocation] , sampleData, me) == false){
						//LOG to hpcrun log
						EMSG("ArmWatchPoint failed for address %p", sampleData->va);
						threadDataTable.hashTable[me].counter++;
						return false;
					}
					//fprintf(stderr, "watchpoint of thread %d is successfully armed by thread %d\n", me, TD_GET(core_profile_trace_data.id));
					reuseMtDataInsert(sampleData->first_accessing_tid, sampleData->sampleTime, true);
					threadDataTable.hashTable[me].counter++;
					return true;
					//}
				}
				none_available_count++;
				threadDataTable.hashTable[me].counter++; // makes the counter even
				break;
		}
		  } while(1);
		
		//fprintf(stderr, "after loop %d\n", loop_counter);
	}
	//fprintf(stderr, "after loop %d\n", loop_counter);
	return false;
}*/


bool SubscribeWatchpointShared(SampleData_t * sampleData, OverwritePolicy overwritePolicy, bool captureValue, int me, bool profile_l1, int location){
	sub_wp_count1++;
	if(ValidateWPData(sampleData) == false) {
		return false;
	}
	//sub_wp_count2++;
	//int tid = TD_GET(core_profile_trace_data.id);

	if(profile_l1) {

		if (event_type == WP_MT_REUSE) {
	//fprintf(stderr, "arming in the same thread\n");	
	if(IsOveralppedShared(sampleData, me, profile_l1)){
		//fprintf(stderr, "subscribing is dropped because of overlapping\n");
		return false; // drop the sample if it overlaps an existing address
	}
	sub_wp_count2++;

	// No overlap, look for a victim slot
	int victimLocation = -1;
	// Find a slot to install WP
	//linux_perf_events_events_of_thread(TD_GET(core_profile_trace_data.id));
	//until this point ******
	VictimType r = GetVictimShared(&victimLocation, wpConfig.replacementPolicy, me, profile_l1);
	sub_wp_count3++;
	if(r != NONE_AVAILABLE) {
		// VV IMP: Capture value before arming the WP.
		if(captureValue) {
			CaptureValue(sampleData, &tData.watchPointArray[victimLocation]);
                }
		// I know the error case that we have captured the value but ArmWatchPoint fails.
		// I am not handling that corner case because ArmWatchPoint() will fail with a monitor_real_abort().
		//printf("and this region\n");
		//printf("arming watchpoints\n");
		//fprintf(stderr, "watchpoint is armed\n");
		if(ArmWatchPoint(&tData.watchPointArray[victimLocation], sampleData) == false){
                	//LOG to hpcrun log
                	EMSG("ArmWatchPoint failed for address %p", sampleData->va);
                        return false;
		}
                //reuseMtDataInsert(sampleData->first_accessing_tid, sampleData->sampleTime, true);
                return true;	
	}
		} else if (event_type == WP_REUSE_MT) {

	if(location < same_thread_l1_wp_count ) {
		// until this point
		if(IsOveralppedShared(sampleData, me, profile_l1)){
                //fprintf(stderr, "subscribing is dropped because of overlapping\n");
                	return false; // drop the sample if it overlaps an existing address
        	}
        sub_wp_count2++;

        // No overlap, look for a victim slot
        int victimLocation = -1;
        // Find a slot to install WP
        //linux_perf_events_events_of_thread(TD_GET(core_profile_trace_data.id));
        //until this point ******
        VictimType r = GetVictimShared(&victimLocation, wpConfig.replacementPolicy, me, profile_l1);
        sub_wp_count3++;
        if(r != NONE_AVAILABLE) {
                // VV IMP: Capture value before arming the WP.
                if(captureValue) {
                        CaptureValue(sampleData, &tData.watchPointArray[victimLocation]);
                }
                // I know the error case that we have captured the value but ArmWatchPoint fails.
                // I am not handling that corner case because ArmWatchPoint() will fail with a monitor_real_abort().
                //printf("and this region\n");
                //printf("arming watchpoints\n");
                //fprintf(stderr, "watchpoint is armed\n");
                if(ArmWatchPoint(&tData.watchPointArray[victimLocation], sampleData) == false){
                        //LOG to hpcrun log
                        EMSG("ArmWatchPoint failed for address %p", sampleData->va);
                        return false;
                }
                //reuseMtDataInsert(sampleData->first_accessing_tid, sampleData->sampleTime, true);
                return true;
        }
	} else {
			if(threadDataTable.hashTable[me].os_tid != -1) {
                //fprintf(stderr, "arming another thread to profile L3 1\n");
                uint64_t theCounter = threadDataTable.hashTable[me].counter[location];

                if((theCounter & 1) == 0) {
                        if(__sync_bool_compare_and_swap(&threadDataTable.hashTable[me].counter[location], theCounter, theCounter+1)){
                sub_wp_count2++;

                sub_wp_count3++;
                if(captureValue) {
                        CaptureValue(sampleData, &threadDataTable.hashTable[me].watchPointArray[location]);
                }
                //fprintf(stderr, "arming another thread %d to profile L3 by thread %d\n", me, TD_GET(core_profile_trace_data.id));
                if(ArmWatchPointShared(&threadDataTable.hashTable[me].watchPointArray[location] , sampleData, me) == false){
                        //LOG to hpcrun log
                        EMSG("ArmWatchPoint failed for address %p", sampleData->va);
                        threadDataTable.hashTable[me].counter[location]++;
                        return false;
                }
                threadDataTable.hashTable[me].counter[location]++;
                return true;
                        }
                }
        }

	}
		}
	} else {
	//fprintf(stderr, "arming another thread to profile L3 0\n");
	if(threadDataTable.hashTable[me].os_tid != -1) {
		//fprintf(stderr, "arming another thread to profile L3 1\n");
        	uint64_t theCounter = threadDataTable.hashTable[me].counter[location]; 

		if((theCounter & 1) == 0) {
			if(__sync_bool_compare_and_swap(&threadDataTable.hashTable[me].counter[location], theCounter, theCounter+1)){
        	sub_wp_count2++;

		sub_wp_count3++;
		if(captureValue) {
			CaptureValue(sampleData, &threadDataTable.hashTable[me].watchPointArray[location]);
		}
		//fprintf(stderr, "arming another thread %d to profile L3 by thread %d\n", me, TD_GET(core_profile_trace_data.id));
		if(ArmWatchPointShared(&threadDataTable.hashTable[me].watchPointArray[location] , sampleData, me) == false){
			//LOG to hpcrun log
			EMSG("ArmWatchPoint failed for address %p", sampleData->va);
			threadDataTable.hashTable[me].counter[location]++;
			return false;
		}
		threadDataTable.hashTable[me].counter[location]++;
		return true;
			}
		}
	}
	}
	// after
	return false;
}

bool SubscribeWatchpointWithStoreTime(SampleData_t * sampleData, OverwritePolicy overwritePolicy, bool captureValue, uint64_t curTime){
	if(ValidateWPData(sampleData) == false) {
		return false;
	}
	if(IsOveralpped(sampleData)){
		return false; // drop the sample if it overlaps an existing address
	}

	// No overlap, look for a victim slot
	int victimLocation = -1;
	// Find a slot to install WP
	VictimType r = GetVictim(&victimLocation, wpConfig.replacementPolicy);

	if(r != NONE_AVAILABLE) {
		// VV IMP: Capture value before arming the WP.
		if(captureValue) {
			CaptureValue(sampleData, &tData.watchPointArray[victimLocation]);
		}
		// I know the error case that we have captured the value but ArmWatchPoint fails.
		// I am not handling that corner case because ArmWatchPoint() will fail with a monitor_real_abort().
		//printf("and this region\n");
		//printf("arming watchpoints\n");
		if((curTime - tData.watchPointArray[victimLocation].sample.bulletinBoardTimestamp) > tData.watchPointArray[victimLocation].sample.expirationPeriod) {
			//printf("watchpoints are armed on address %lx, length: %d\n", sampleData->va, sampleData->accessLength);
			if(ArmWatchPoint(&tData.watchPointArray[victimLocation], sampleData) == false){
				//LOG to hpcrun log
				EMSG("ArmWatchPoint failed for address %p", sampleData->va); 
				return false;
			}
		} /*else {
		    printf("watchpoints are not armed because they are still new\n");
		    }*/
		return true;
	}
	return false;
}

bool SubscribeWatchpointWithTime(SampleData_t * sampleData, OverwritePolicy overwritePolicy, bool captureValue, uint64_t curTime, uint64_t lastTime){
	if(ValidateWPData(sampleData) == false) {
		return false;
	}
	if(IsOveralpped(sampleData)){
		return false; // drop the sample if it overlaps an existing address
	}

	// No overlap, look for a victim slot
	int victimLocation = -1;
	// Find a slot to install WP
	VictimType r = GetVictim(&victimLocation, wpConfig.replacementPolicy);

	if(r != NONE_AVAILABLE) {
		// VV IMP: Capture value before arming the WP.
		if(captureValue) {
			CaptureValue(sampleData, &tData.watchPointArray[victimLocation]);
		}
		// I know the error case that we have captured the value but ArmWatchPoint fails.
		// I am not handling that corner case because ArmWatchPoint() will fail with a monitor_real_abort().
		//printf("and this region\n");
		//printf("arming watchpoints\n");
		if((sampleData->bulletinBoardTimestamp - tData.watchPointArray[victimLocation].bulletinBoardTimestamp) > (curTime - lastTime)) {
			//printf("watchpoints are armed on address %lx, length: %d\n", sampleData->va, sampleData->accessLength);
			if(ArmWatchPoint(&tData.watchPointArray[victimLocation], sampleData) == false){
				//LOG to hpcrun log
				EMSG("ArmWatchPoint failed for address %p", sampleData->va);
				return false;
			}
		} /*else {
		    printf("watchpoints are not armed because they are still new\n");
		    }*/
		return true;
	}
	return false;
}

#ifdef TEST
#include<omp.h>


__thread volatile int cnt;
WPUpCallTRetType Test1UpCall(WatchPointInfo_t * wp, WatchPointTrigger_t * wt) {
	printf("\n Test1UpCall %p\n", wt->va);
	if(wpConfig.isLBREnabled)
		assert(wp->sample.va == wt->va);

	cnt ++;
	return DISABLE;
}

void TestBasic(){
	tData.fptr = Test1UpCall;

	sigset_t block_mask;
	sigemptyset (&block_mask);
	// Set a signal handler for SIGUSR1
	struct sigaction sa1 = {
		.sa_sigaction = OnWatchPoint,
		//        .sa_mask = block_mask,
		.sa_flags = SA_SIGINFO | SA_RESTART | SA_NODEFER
	};

	if(sigaction(wpConfig.signalDelivered, &sa1, NULL) == -1) {
		fprintf(stderr, "Failed to set WHICH_SIG handler: %s\n", strerror(errno));
		monitor_real_abort();
	}


	WatchpointThreadInit();
	int N = 10000;
	volatile int dummyWPLocation[10000];
	cnt = 0;

	for(int i = 0 ; i < N; i++) {
		SampleData_t s = {.va = &dummyWPLocation[i], .wpLength = sizeof(int), .type = WP_WRITE};
		SubscribeWatchpoint(&s, AUTO);
	}
	for(int i = 0 ; i < N; i++) {
		dummyWPLocation[i]++;
	}
	printf("\n cnt = %d\n", cnt);
	assert(cnt == wpConfig.maxWP);
	WatchpointThreadTerminate();
}

int main() {
	printf("\n Test 1: single threaded");
	while(1) {
#pragma omp parallel
		{
			TestBasic();
		}
	}
	return 0;
}
#endif
