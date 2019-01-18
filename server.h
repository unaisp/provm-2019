#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define BUF_SIZE_IN_BYTES 40960
// #define SSD_SIZE_IN_BLOCK 524288 //9000000

#define ENABLE_HEART_BEAT 1
#define ENABLE_LOG 1
#define PRINT_LOG_CONT 1
#define AUTO_POLICY 1

#define MAX_NO_VMS 10

#define MSG_TYPE_BUF_FREE 0
#define MSG_TYPE_VM_REG 12
#define MSG_TYPE_VM_REG_REPLY 15
#define MSG_TYPE_ALLOCATION_REQ 3
#define MSG_TYPE_ALLOCATION_REPLY 4
#define MSG_TYPE_RESIZE_REQ 5

#define MSG_TYPE_INFLATION_REPLY 6

#define MSG_TYPE_DEFLATION_REQ 7
#define MSG_TYPE_DEFLATION_BLK_ALCN 8
#define MSG_TYPE_DEFLATION_BLK_ALCN_REPLY 9

#define MSG_TYPE_PHYSICAL_BLOCK_ALCN 20
#define MSG_TYPE_PHYSICAL_BLOCK_ALCN_REPLY 21

#define MSG_TYPE_STATUS 23

#define CM_LISTENER_ID 0
#define TARGET_NONE -1

#define FLAG_DO_NOT_ASK_ME 1
#define FLAG_ASK_ME_AGAIN 2

#define BLOCK_FREE -1
#define BLOCK_RESERVED -2


#define SSD_BALLOON_INFLATION '-'
#define SSD_BALLOON_DEFLATION '+'

#define INDEX_NULL 4294967295u	// ULONG_MAX

#define VM_STATUS_ACTIVE 1
#define VM_STATUS_INACTIVE 2	// BLocks may or may not swapped
#define VM_STATUS_REG 3
#define VM_STATUS_REREG 4

#define SHARE_BASED_POLICY 1
#define HIERARCHICAL_BASED_POLICY 2
#define NORMAL_POLICY 3		//	Allocate request number blocks, if available.
							//	Otherwse allocate allocated blocks

struct SharedMemory
{
	pthread_mutex_t lock;
	pthread_mutexattr_t mutexAttr;

	pthread_condattr_t condAttr;
	pthread_cond_t cm_can_enter;
	pthread_cond_t vm_can_enter;

	int key;
	int source;
	int destination;
	int msg_type;

	int msg_content[BUF_SIZE_IN_BYTES / sizeof(int)];
};
typedef struct SharedMemory SharedMemory;

struct block_t
{
	int vm_index;
	int type;			// SSD_PERSISTENT, SSD_NON_PERSISTENT, SSD_FREE, SSD_METADATA, SWAP_ALLOCATED, SWAP_FREE
	unsigned long int lbn;

	unsigned long int prev;
	unsigned long int next;
};
typedef struct block_t block_t;

struct sblock_list
{
	unsigned long int size;		//	Number of blocks in the list;
	unsigned long int head;		//	First element in the list;
	unsigned long long int tail;		//	Last element in the list
};
typedef struct sblock_list sblock_list;

struct block_space_t
{
	block_t *begin;
	unsigned long int size;

	sblock_list list_free;
	sblock_list list_metadata;
};
typedef struct block_space_t block_space_t;

struct vm_info
{
	int vm_index;		//	Index to the pointer array
	int vm_id;
	char vm_name[25];
	unsigned long int capacity;
	int pid;
	// unsigned long int allocated_blocks;					//	Total blocks allocated
	// unsigned long int allocated_blocks_persistent;		//	Persistent blocks allocated. Persistent block is a subset of allocated blocks
	long int to_resize;						//	Number of blocks to allocate or deallocate
	long int to_resize_failed;						//	Number of blocks to allocate or deallocate
	unsigned long int max_persistent_blocks;		//	Reserved persistent blocks is a subset of reserved blocks
	bool persist_full;

	sblock_list list_ssd_persistent;
	sblock_list list_ssd_non_persistent;
	sblock_list swapped_block_list;

	unsigned long int new_size;
	int status;

	//	Status message
	int status_count;
	struct
	{	
		// Received from front end
		long int sectors;
		long int rqst;
		long int miss;
		long int resident;
		long int cache_size;

		//	Measured
		double bandwidth;
		double hit_ratio;
		double utilization;
	}status_values[20];

	struct 
	{
		double Bmeasured;		//	avg of measured bandwidth
		double Bobserved;		//	k1 * Bmeasured.		k1 = 1.25
		double Bmin;			//	Target/SLO Bandwidth
		double Bmax;			//	k2 * Bmin.			k2 = 2 
		long int Cresident;		//	Number of blocks used in cache
		// long int Cutlization;	//	Chitrio * Cresident
		long int CcurrentSize;	//	Number of blocks allocated to cache
		long int CchangeInSize;		//	+/- 
		bool is_slo_met_in_last_round;
		double Cutilization;
	}cwnd_status;

	//	todo.	need a lock here to proctect the updates.
};
typedef struct vm_info vm_info;

struct category_t
{
	int category_id;
	int vm_count;
	unsigned int ratio;
	unsigned long int max_size;
	unsigned long int new_size;
	unsigned long int allocated_to_vms;
};

struct central_manager
{
	// union
	// {
	int key;
	unsigned long long int vm_list_start_offset;
	unsigned long long int block_list_start_offset;
	unsigned long long int swap_space_start_offset;
	
	unsigned long long int sectors_for_cm;
	unsigned long long int sectors_for_vm_list;
	unsigned long long int sectors_for_block_list;
	unsigned long long int sectors_for_swap_space;
	unsigned long int metadata_blocks;
	// }metadata;

	unsigned int vssd_block_size;				//	Size of vSSD block in sectors
	// unsigned long long int vssd_capacity;		//	Capacity in vSSD blocks
	// unsigned long long int allocated;			//	Allocated blocks
	// unsigned long long int free_blocks;			//	Number of free blocks
	// unsigned long long int reserved_blocks_to_allocate;		//	Number of blocks reserved for allocation. After the operation blocks will be added to allocated
	//unsigned long long int blocks_to_free;		//	Number of blocks to free.
	//	vssd_capacity = allocated+free_blocks+reserved_blocks_to_allocate;
	unsigned int max_vms;

	bool running_first_time;

	SharedMemory *message;
	vm_info* vm_list;

	int metadata_fd;
	int swap_fd;

	//	Swap Device
	block_space_t swap_space;
	block_space_t ssd_space;

	struct category_t category[3];
	int lcm;
};
typedef struct central_manager central_manager;


struct vm_registration_req
{
	int vm_id;
	char vm_name[25];
	int pid;
	unsigned long int capacity;
	unsigned long int current_allocate;
	unsigned long int current_persist;
	bool persist_full;
};

struct physical_logical_block
{
	unsigned long long int physical_block;
	unsigned long long int logical_block;
};
typedef struct physical_logical_block physical_logical_block;

struct vm_registration_reply
{
	unsigned long int reserved_blocks;
	unsigned long int reserved_blocks_persistent;
	unsigned long int old_blocks;
	unsigned long int old_persistent_blocks;
	bool first_time_registration;
	int error;
	// int flag;
	// unsigned long int block_list[(BUF_SIZE_IN_BYTES-(2*sizeof(unsigned long int)+sizeof(int)))/sizeof(struct physical_logical_block)];
};




struct phsical_block_allocation
{
	int flag;
	int count;
	int allocation_type; 		//	1 Old block, 
								//	2 New persistent block
								//	3 New non-persistent block
	physical_logical_block block_list[(BUF_SIZE_IN_BYTES-(3*sizeof(int)))/sizeof(struct physical_logical_block)];
};
unsigned long int max_transfer_alcn = (BUF_SIZE_IN_BYTES - 3*sizeof(int))/sizeof(struct physical_logical_block);

struct allocation_req
{
	int vm_id;
	unsigned long int requested_size;	//	requested size in blocks;
};

struct resize_req
{
	unsigned long int size;
	char operation;
};

struct inflation_reply
{
	int vm_id;
    int count;
    int flag;
    unsigned long int block_list[(BUF_SIZE_IN_BYTES - 3*sizeof(int))/sizeof(unsigned long int)];
};
typedef struct inflation_reply inflation_reply;
unsigned long int max_transfer_inflation = (BUF_SIZE_IN_BYTES - 3*sizeof(int))/sizeof(unsigned long int);

typedef struct phsical_block_allocation phsical_block_allocation_reply ;
typedef struct vm_registration_req vm_registration_req;
typedef struct vm_registration_reply allocation_reply;
typedef struct allocation_req DeflationReq;
typedef struct phsical_block_allocation DeflationBlockAllocation;
typedef struct phsical_block_allocation DeflationBlockAlcnReply;

struct status_msg
{
	int vm_id;
	int size;
	long int data[100];
};




#define ERROR_NO_VM_SLOT 1
#define ERROR_FIRST_TIME_REG_POLICY 2
#define ERROR_INVALID_VM 3
#define ERROR_INVALID_PBN 10
#define ERROR_PBN_ALLOCATED_TO_SOME_ONE_ELSE 11
#define ERROR_FREEING_NON_ALLOCATED_BLOCK 12
#define ERROR_NON_PERISTENT_BLOCK_FOR_FULL_PERSIST_VM 13
#define ERROR_SWAPPING_NON_PERSISTENT_BLOCK 14
#define ERROR_NO_FREE_BLOCK_IN_SSD_FOR_SWAP_IN 15

#define TYPE_OLD_BLOCK 1
#define TYPE_NEW_PERSISTENT 2
#define TYPE_NEW_NON_PERSISTENT 3

#define SSD_PERSISTENT 1
#define SSD_NON_PERSISTENT 2
#define SSD_METADATA 4
#define SWAP_ALLOCATED 5
#define FREE 6
