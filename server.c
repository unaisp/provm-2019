#define _GNU_SOURCE

#include  <stdio.h>
#include  <stdlib.h>
#include  <string.h>
#include  <signal.h>
#include  <pthread.h>
#include  <math.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>


#include "server.h"

unsigned long int resize_start_time;
unsigned long int resize_end_time;
unsigned long int resize_round;
char resize_operation;
unsigned long int resize_size;
pthread_mutex_t resize_lock;


#define CM_RANDOM_ID 1234

// global variables
central_manager* cm_global;
pthread_t listener_thread_ID;
pthread_t heart_beat_thread_ID;
pthread_t log_printing_thread_ID;
pthread_t auto_policy_thread_id;
int swap_in_blocks(central_manager *cm, vm_info *vm);
void show_vm_list(central_manager* cm);


void BUG(int line)
{
	printf("SERVER-ERROR: Found a bug. Line number: %d. \n", line);
	exit(1);
}

unsigned long long int min(unsigned long long int n1, unsigned long long int n2)
{
	if(n1 < n2)
		return n1;
	else
		return n2;
}

unsigned long long int MIN3(unsigned long long int n1, unsigned long long int n2, unsigned long long int n3)
{
	return min(n1, min(n2, n3));
}

/************ TIME *******************************/
static unsigned long long current_time(void)
{
	static unsigned long long min = 0;
	struct timeval cur_time;
	unsigned long long cur;

	gettimeofday(&cur_time, NULL);
	cur = cur_time.tv_sec*1000000 + cur_time.tv_usec;

	// if(min == 0)
	//   min = cur;

	return cur-min;
}


/*************************** Swap **************************************************/
void init_block(block_t *sblock)
{
	sblock->vm_index = -1;
	sblock->lbn = INDEX_NULL;
	sblock->type = FREE;

	sblock->prev = INDEX_NULL;
	sblock->next = INDEX_NULL;
}

block_t * pbn_to_block(block_space_t *bs, unsigned long int index)
{
	if(index == INDEX_NULL)
		return NULL;

	if(index >= bs->size)
	{
		printf("SERVER-ERROR: Index is greather than block space size \n");
		printf("SERVER-ERROR: Block space size: %lu. Index: %lu \n", bs->size, index);
		BUG(__LINE__);
		return NULL;
	}

	return bs->begin + index;
}

unsigned long int block_to_pbn(block_space_t *bs, block_t *sblock)
{
	unsigned long int index;

	if(!sblock)
		return INDEX_NULL;

	if(!(sblock < bs->begin + bs->size))
		BUG(__LINE__);

	index = sblock - bs->begin;
	if(index == INDEX_NULL)
		BUG(__LINE__);

	return index;
}

void init_block_list(sblock_list *list)
{
	list->size = 0u;
	list->head = INDEX_NULL;
	list->tail = INDEX_NULL;
}

block_t *block_list_head(block_space_t *bs, sblock_list *list)
{
	return pbn_to_block(bs, list->head);
}

block_t *block_list_tail(block_space_t *bs, sblock_list *list)
{
	return pbn_to_block(bs, list->tail);
}

void block_list_add_head(block_space_t *bs, sblock_list *list, block_t *sblock)
{
	block_t *head = block_list_head(bs, list);

	sblock->next = list->head;
	sblock->prev = INDEX_NULL;

	if(head && head->type != sblock->type)
	{
		BUG(__LINE__);
		return;
	}

	if(list == &bs->list_free && sblock->type != FREE)
	{
		BUG(__LINE__);
		return;
	}

	if(list != &bs->list_free && sblock->type == FREE)
	{
		BUG(__LINE__);
		return;
	}

	if((list->size == 0 && head) || (list->size > 0 && !head))
	{
		BUG(__LINE__);
		return;
	}

	if (head)
		head->prev = list->head = block_to_pbn(bs, sblock);
	else
		list->head = list->tail = block_to_pbn(bs, sblock);

	list->size++;
}

void block_list_add_tail(block_space_t *bs, sblock_list *list, block_t *sblock)
{
	block_t *tail = block_list_tail(bs, list);

	sblock->next = INDEX_NULL; 
	sblock->prev = list->tail;

	if(tail && tail->type != tail->type)
	{
		BUG(__LINE__);
		return;
	}

	if(list == &bs->list_free && sblock->type != FREE)
	{
		BUG(__LINE__);
		return;
	}

	if(list != &bs->list_free && sblock->type == FREE)
	{
		BUG(__LINE__);
		return;
	}

	if((list->size == 0 && tail) || (list->size > 0 && !tail))
	{
		BUG(__LINE__);
		return;
	}

	if (tail)
		tail->next = list->tail = block_to_pbn(bs, sblock);
	else
		list->head = list->tail = block_to_pbn(bs, sblock);

	list->size++;
}

block_t *block_list_next(block_space_t *bs, block_t *sblock)
{
	return pbn_to_block(bs, sblock->next);
}

block_t *block_list_prev(block_space_t *bs, block_t *sblock)
{
	return pbn_to_block(bs, sblock->prev);
}

void block_list_del(block_space_t *bs, sblock_list *list, block_t *sblock)
{
	block_t *prev = block_list_prev(bs, sblock);
	block_t *next = block_list_next(bs, sblock);

	if(prev && sblock->type != prev->type)
	{
		printf("Block- vm-index: %d. type: %c. lbn: %lu \n ", sblock->vm_index, sblock->type, sblock->lbn);
		BUG(__LINE__);
	}

	if(next && sblock->type != next->type)
		BUG(__LINE__);

	if((prev || next) && (list->head == INDEX_NULL || list->tail == INDEX_NULL))
		BUG(__LINE__);

	if (prev)
		prev->next = sblock->next;
	else
		list->head = sblock->next;

	if (next)
		next->prev = sblock->prev;
	else
		list->tail = sblock->prev;

	list->size--; 
}

block_t *block_list_pop_tail(block_space_t *bs, sblock_list *list)
{
	block_t *sblock;

	sblock = block_list_tail(bs, list);

	if(!sblock && list->size > 0)
		BUG(__LINE__); 

	if(!sblock)
		return NULL;

	if(list == &bs->list_free && sblock->type != FREE)
		BUG(__LINE__);

	if(list != &bs->list_free && sblock->type == FREE)
		BUG(__LINE__);

	block_list_del(bs, list, sblock);

	return sblock;
}

void free_block(block_space_t *bs, block_t *sblock)
{
	sblock->vm_index = -1;
	sblock->lbn = INDEX_NULL;
	sblock->type = FREE;

	block_list_add_head(bs, &bs->list_free, sblock);
}

void init_block_space(block_space_t *bs, unsigned long size)
{
	bs->size = size;
	bs->begin = (block_t *)malloc(sizeof(block_t) * bs->size);
	if(!bs->begin)
	{
		printf("SERVER-ERROR: Unable to allocated memory for block swapce \n");
	}

	init_block_list(&bs->list_free);
	init_block_list(&bs->list_metadata);

	for(unsigned long int i = 0; i < bs->size; i++)
	{
		block_t *sblock = pbn_to_block(bs, i);
		init_block(sblock);
		block_list_add_head(bs, &bs->list_free, sblock);
	}
}
/***************************** Server ***********************************************/
int persist_metadata(central_manager *cm)
{
	long int ret;

	// Persisting cental manager
	lseek(cm->metadata_fd, 0, SEEK_SET);   /* seek to start of file */
	ret = write(cm->metadata_fd, cm, (sizeof(central_manager)));
	if(ret != sizeof(central_manager))
	{
		printf("SERVER-ERROR: Unable to persist central_manager states \n");
		return -1;
	}

	//	Persisting vm list
	lseek(cm->metadata_fd, cm->vm_list_start_offset, SEEK_SET);   /* seek to start of file */
	ret = write(cm->metadata_fd, cm->vm_list, sizeof(vm_info) * cm->max_vms);
	if(ret != sizeof(vm_info) * cm->max_vms)
	{
		printf("SERVER-ERROR: Unable to persist vm_list \n");
		return -1;
	}

	// Persisting ssd block space
	lseek(cm->metadata_fd, cm->block_list_start_offset, SEEK_SET);   /* seek to start of file */
	ret = write(cm->metadata_fd, cm->ssd_space.begin, sizeof(block_t) * cm->ssd_space.size);
	if(ret != sizeof(block_t) * cm->ssd_space.size)
	{
		printf("SERVER-ERROR: Unable to persist block list \n");
		return -1;
	}

	//	Persisting swap space
	lseek(cm->metadata_fd, cm->swap_space_start_offset, SEEK_SET);  
	ret = write(cm->metadata_fd, cm->swap_space.begin, sizeof(block_t) * cm->swap_space.size);
	if(ret != sizeof(block_t) * cm->swap_space.size)
	{
		printf("SERVER-ERROR: Unable to persist swap space \n");
		return -1;
	}

	// printf("SERVER: \t\tMetadata persisted\n");
}

int opening_server_first_time(central_manager *cm)
{

	int ret;

	printf("SERVER: \tOpening server for first time \n");	
	cm->key = CM_RANDOM_ID;
	cm->message = NULL;

	cm->vssd_block_size = 2048;		//	1MB 
	cm->ssd_space.size = 103399;		//41957 			//	215040;		//123879;		Number of blocks
	cm->max_vms = 13u;
	cm->swap_space.size = 60000;

read_value:
	// printf("SERVER: Enter vSSD block size : ");
	// scanf("%u", &cm->vssd_block_size);

	// printf("SERVER: Enter capacity of disk in blocks : ");
	// scanf("%llu", &cm->vssd_capacity);

	// printf("SERVER: Maximum number of vms : ");
	// scanf("%u", &cm->max_vms);

	if(cm->vssd_block_size <= 0 || cm->ssd_space.size < 0)
		goto read_value;

	printf("SERVER: \t\tBlock size: %u sectors. Capacity: %lu blocks. Max-no-vms: %u \n", cm->vssd_block_size, cm->ssd_space.size, cm->max_vms);

	//	Initializing block list
	printf("SERVER: \t\tInitializing block spaces \n");
	init_block_space(&cm->ssd_space, cm->ssd_space.size);
	init_block_space(&cm->swap_space, cm->swap_space.size);

	// Initializing vm_list
	printf("SERVER: \t\tInitializing vm_list \n");
	cm->vm_list = (vm_info *)malloc(sizeof(vm_info) * cm->max_vms );
	if(!cm->vm_list)
	{
		printf("SERVER-ERROR: Unable to allocate memory for vm_list \n");
		return -1;
	}
	for(int i=0; i<cm->max_vms; i++)
	{
		cm->vm_list[i].vm_index = i;
		cm->vm_list[i].vm_id = -1;			//	EMPTY
		cm->vm_list[i].capacity = 0;
		cm->vm_list[i].to_resize = 0;
		cm->vm_list[i].to_resize_failed = 0;
		cm->vm_list[i].max_persistent_blocks = 0;
		cm->vm_list[i].persist_full = false;

		init_block_list(&cm->vm_list[i].list_ssd_persistent);
		init_block_list(&cm->vm_list[i].list_ssd_non_persistent);
		init_block_list(&cm->vm_list[i].swapped_block_list);
	}

	int ratios[3] = {3, 2, 1};
	cm->lcm = 6;

	for(int i=0; i<3; i++)
	{
		struct category_t *category = &cm->category[i];
		category->category_id = i;
		category->vm_count = 0;
		category->ratio = ratios[i];

		category->max_size = 0;
		category->new_size = 0;
	}

	//	Reserving metadata blocks
	printf("SERVER: \t\tReserving metadata blocks. %lu  \n", sizeof(central_manager) / 512);

	cm->sectors_for_cm = sizeof(central_manager) / 512;
	cm->sectors_for_cm += (sizeof(central_manager) % 512 == 0) ? 0 : 1;

	cm->sectors_for_vm_list = (sizeof(vm_info)*cm->max_vms) / 512;
	cm->sectors_for_vm_list += ((sizeof(vm_info)*cm->max_vms) % 512 == 0) ? 0 : 1;

	cm->sectors_for_block_list = (sizeof(block_t)*cm->ssd_space.size)  / 512;
	cm->sectors_for_block_list += ((sizeof(block_t)*cm->ssd_space.size) % 512 == 0) ? 0 : 1;

	cm->sectors_for_swap_space = (sizeof(block_t) * cm->swap_space.size) / 512;
	cm->sectors_for_swap_space += ((sizeof(block_t) * cm->swap_space.size) % 512 == 0) ? 0 : 1;

	cm->vm_list_start_offset = cm->sectors_for_cm * 512;
	cm->block_list_start_offset = (cm->sectors_for_cm + cm->sectors_for_vm_list) * 512;
	cm->swap_space_start_offset = (cm->sectors_for_cm + cm->sectors_for_vm_list + cm->block_list_start_offset) * 512;

	cm->metadata_blocks = 1 + (cm->sectors_for_cm + cm->sectors_for_vm_list  + cm->sectors_for_block_list + cm->sectors_for_swap_space)/cm->vssd_block_size;

	printf("SERVER: \t\tSectors for cm: %llu, sectors for vm_list: %llu, sectors for block list: %llu, sectors for swap space: %llu \n",
			cm->sectors_for_cm, cm->sectors_for_vm_list, cm->sectors_for_block_list, cm->sectors_for_swap_space);
	printf("SERVER: \t\tMetadata blocks: %lu \n", cm->metadata_blocks);

	for(int i=0; i<cm->metadata_blocks+993; i++)
	{
		//	Need to use first few blocks in the SSD for storing metadata.
		//	list_pop_tail will give blocks from 0 to n
		block_t *block = block_list_pop_tail(&cm->ssd_space, &cm->ssd_space.list_free);
		// printf("Metadata block: %lu \n", block_to_pbn(&cm->ssd_space, block));

		block->vm_index = -1;
		block->type = SSD_METADATA;
		block->lbn = -1;

		block_list_add_head(&cm->ssd_space, &cm->ssd_space.list_metadata, block);		
	}

	return persist_metadata(cm);
}

central_manager* initialize_cm(void)
{
	central_manager *cm;
	int ret;
	int fd;

	// Allocating memory for central manager
	printf("\nSERVER: Initialize central_manager \n");
	// printf("SERVER: \tSize of single block : %lu Bytes\n", sizeof(physical_block));
	// printf("SERVER: \tcentral_manager size : %f MB \n", (double)sizeof(central_manager)/(1024*1024));
	cm = (central_manager *)malloc(sizeof(central_manager));
	if(!cm)
	{
		printf("SERVER-ERROR: Unable to allocate memory for central_manager \n ");
		return NULL;
	}
	printf("SERVER: \tMemory allocated for cm. cm: %p \n", cm);

	//	Opening the ssd device
	printf("SERVER: \tOperning the device \n");
	fd = open("/dev/sdc", O_RDWR|O_SYNC);
	if(fd < 0)
	{
		printf("SERVER-ERROR: Unable to open the device '\\dev\\sdc' \n ");
		return NULL;
	}
	printf("SERVER: \t'\\dev\\sdc' opened !!!. fd: %d \n", fd);

	// Reading the metadata from ssd
	ret = read(fd, cm, sizeof(central_manager));
	printf("SERVER: \tret = %d \n", ret);
	if(ret != sizeof(central_manager))
	{
		printf("SERVER-ERROR: Unable to read from disk. ret: %d \n ", ret);
		return NULL;
	}

	//	Compare the metadata
	cm->metadata_fd = fd;
	if(cm->key == CM_RANDOM_ID && cm->vm_list_start_offset > 0 && cm->block_list_start_offset > 0)
	{
		printf("\nSERVER: \tNot first time \n");
		printf("SERVER: \t\tBlock size: %u sectors. SSD space size: %lu blocks. SWAP space size: %lu. Max. number of vms: %u \n",
				cm->vssd_block_size, cm->ssd_space.size, cm->swap_space.size, cm->max_vms);
		printf("SERVER: \t\tStart-offset of vm_list: %llu. start-offset of block_list: %llu \n", cm->vm_list_start_offset, cm->block_list_start_offset);

		// Loading VMs 
		cm->vm_list = (vm_info *)malloc(sizeof(vm_info) * cm->max_vms);
		if(!cm->vm_list)
		{
			printf("SERVER-ERROR: Unable to allocate memory for vm list \n ");
			return NULL;
		}
		lseek(cm->metadata_fd, cm->vm_list_start_offset, SEEK_SET);   /* seek to start of file */
		ret = read(cm->metadata_fd, cm->vm_list, sizeof(vm_info) * cm->max_vms);
		if(ret != sizeof(vm_info) * cm->max_vms)
		{
			printf("SERVER-ERROR: Unable to read vm_list from disk \n");
			return NULL;
		}

		// Loading SSD Block lists
		cm->ssd_space.begin = (block_t *)malloc(sizeof(block_t) * cm->ssd_space.size);
		if(!cm->ssd_space.begin)
		{
			printf("SERVER-ERROR: Unable to allocate memory for block list \n ");
			return NULL;
		}
		lseek(cm->metadata_fd, cm->block_list_start_offset, SEEK_SET);   /* seek to start of file */
		ret = read(cm->metadata_fd, cm->ssd_space.begin, sizeof(block_t) * cm->ssd_space.size);
		if(ret != sizeof(block_t) * cm->ssd_space.size)
		{
			printf("SERVER-ERROR: Unable to read block_list from disk \n");
			return NULL;
		}

		// Loading Swap Block lists
		cm->swap_space.begin = (block_t *)malloc(sizeof(block_t) * cm->swap_space.size);
		if(!cm->swap_space.begin)
		{
			printf("SERVER-ERROR: Unable to allocate memory for swap space \n ");
			return NULL;
		}
		lseek(cm->metadata_fd, cm->swap_space_start_offset, SEEK_SET);   
		ret = read(cm->metadata_fd, cm->swap_space.begin, sizeof(block_t) * cm->swap_space.size);
		if(ret != sizeof(block_t) * cm->swap_space.size)
		{
			printf("SERVER-ERROR: Unable to read swap spce from disk \n");
			return NULL;
		}

		cm->running_first_time = false;
	}
	else
	{
		printf("SERVER: \tOperning the server for first time \n");
		opening_server_first_time(cm);

		cm->running_first_time = true;
	}

	//	Opening the swap device
	printf("SERVER: \tOperning swap device \n");
	fd = open("./swap_disk.img", O_RDWR|O_SYNC);
	if(fd < 0)
	{
		printf("SERVER-ERROR: Unable to open the swap device './swap_disk.img'. \n ");
		return NULL;
	}
	printf("SERVER: \t Swap disk opened. fd: %d \n", fd);
	cm->swap_fd = fd;

	return cm;
}

/************************** Messaging system ***************************************/

static void get_access_to_shared_memory(struct SharedMemory *message)
{
	int ret;
	// printf("ACCESS: Waiting for the lock \n");
	pthread_mutex_lock(&message->lock);
	// printf("ACCESS: Got the lock \n");

	while(message->msg_type != MSG_TYPE_BUF_FREE)
	{
		ret = pthread_mutex_trylock(&message->lock);
		// printf("ACCESS: tylock ret: %d \n", ret);
		if(ret == 0)
		{
			printf("\n\tACCESS: BUG: Somebody unlocked it. Now thread is going to wait with out lock\n\n");
			exit(1);
		}

		// printf("ACCESS: Going to sleep, will release the lock: \n");
		pthread_cond_wait(&message->cm_can_enter, &message->lock);
		// printf("ACCESS: Woke up. I have the lock \n");
	}
}

static void release_shared_memory(struct SharedMemory *message, int line)
{
	// printf("%llu: RLS-SHM: Called by %d \n", current_time(), line);
	message->msg_type = MSG_TYPE_BUF_FREE;
	message->source = TARGET_NONE;
	message->destination = TARGET_NONE;

	pthread_cond_broadcast(&message->vm_can_enter);
	pthread_cond_broadcast(&message->cm_can_enter);

	pthread_mutex_unlock(&message->lock);

	// printf("%llu: RLS-SHM: Completed. %d \n", current_time(), line);

	// sleep(1);
}

static void send_message(struct SharedMemory *message, int destination, int msg_type)
{
	// printf("%llu: SND-MSG: dest:%d.  src:%d.  type:%d \n", 
	// 		current_time(), message->destination, message->source, message->msg_type);
	message->msg_type = msg_type;
	message->source = CM_LISTENER_ID;
	message->destination = destination; 

	pthread_cond_broadcast(&message->vm_can_enter);


	// printf("%llu: SND-MSG: dest:%d.  src:%d.  type:%d \n", 
	// 		current_time(), message->destination, message->source, message->msg_type);



	// printf("VSSD: \t\tInformed the virtual machine '%d' \n", destination);
}

static void wait_for_reply(struct SharedMemory *message, int source, int msg_type)
{
	int ret;

	while(!(message->destination == CM_LISTENER_ID && 
				message->source == source && 
				message->msg_type == msg_type))
	{

		ret = pthread_mutex_trylock(&message->lock);
		// printf("wait_for_reply: Trylock ret: %d \n", ret);
		if(ret == 0)
		{
			printf("\n\nwait_for_reply: BUG: Somebody unlocked it. Now thread is going to wait with out lock\n\n");
			exit(1);
		}

		// printf("VSSD: \t\tGoing to sleep \n");
		pthread_cond_wait(&message->cm_can_enter, &message->lock);
		// printf("VSSD: \tI woke up \n");
	}
}

	struct SharedMemory* 
initialize_shared_mem(central_manager* cm)
{
	int ShmID;
	int ret;
	key_t ShmKEY;
	struct SharedMemory *ptr;

	/*Allocating shared memory segment*/
	printf("SERVER: Setup shared memory \n");
	ShmKEY = ftok("/tmp", 's');     
	ShmID = shmget(ShmKEY, sizeof(struct SharedMemory), IPC_CREAT | 0666 ); 
	if (ShmID < 0) {
		printf("SERVER-ERROR: shmget error \n");
		return NULL;
	}
	printf("SERVER: \tKey = [%x] \n", ShmID);  

	ptr = (struct SharedMemory *) shmat(ShmID, NULL, 0);
	if (!ptr)
	{
		printf("SERVER-ERROR: shmat failed. \n");
		return NULL;
	} 
	printf("SERVER: \tAttached the shared memory\n");
	printf("SERVER: \tVirtual Address of the shared memory is : %p \n", ptr);

	if(cm->running_first_time || ptr->key != 987)
	{

		ptr->key = 987;

		/*Initializing shared murtex and condidional variables*/
		printf("SERVER: Initialize lock\n");
		pthread_mutexattr_init(&ptr->mutexAttr);
		pthread_mutexattr_setpshared(&ptr->mutexAttr, PTHREAD_PROCESS_SHARED);
		ret = pthread_mutex_init(&(ptr->lock), &(ptr->mutexAttr));
		if(ret)
		{
			printf("SERVER-ERROR: pthread_mutex_init failed\n");
			return NULL;
		}
		printf("SERVER: \tLock initialized \n");


		printf("SERVER: Initialize condidional variables \n");
		pthread_condattr_init(&ptr->condAttr);
		pthread_condattr_setpshared(&ptr->condAttr, PTHREAD_PROCESS_SHARED);

		ret = pthread_cond_init(&ptr->cm_can_enter, &ptr->condAttr);
		if(ret)
		{
			printf("SERVER-ERROR: pthread_cond_init of cm_can_enter failed\n");
			return NULL;
		} 

		ret = pthread_cond_init(&ptr->vm_can_enter, &ptr->condAttr);
		if(ret)
		{
			printf("SERVER-ERROR: pthread_cond_init of vm_can_enter failed\n");
			return NULL;
		} 
		printf("SERVER: \tConditional variables initialized \n");
	}
	else
	{
		release_shared_memory(ptr, __LINE__);
	}

	return ptr;
}

/************************ Utilities ***************************************/

void clearScreen()
{
	const char *CLEAR_SCREEN_ANSI = "\e[1;1H\e[2J";
	write(STDOUT_FILENO, CLEAR_SCREEN_ANSI, 12);
}

/************************* Handling errors ********************************/
void print_error_message(int error)
{
	switch(error)
	{
		case ERROR_NO_VM_SLOT:
			printf("SERVER-ERROR: No free slot in vm_list---------- \n");
			break;

		case ERROR_FIRST_TIME_REG_POLICY:
			printf("SERVER-ERROR: First registration policy error---------- \n");
			break;

		case ERROR_INVALID_VM:
			printf("SERVER-ERROR: ERROR_INVALID_VM \n");
			break;

		case ERROR_INVALID_PBN:
			printf("SERVER-ERROR: ERROR_INVALID_PBN \n");
			break;

		case ERROR_PBN_ALLOCATED_TO_SOME_ONE_ELSE:
			printf("SERVER-ERROR: ERROR_PBN_ALLOCATED_TO_SOME_ONE_ELSE \n");
			break;

		case ERROR_FREEING_NON_ALLOCATED_BLOCK:
			printf("SERVER-ERROR: ERROR_FREEING_NON_ALLOCATED_BLOCK \n");
			break;

		case ERROR_NON_PERISTENT_BLOCK_FOR_FULL_PERSIST_VM:
			printf("SERVER-ERROR: ERROR_NON_PERISTENT_BLOCK_FOR_FULL_PERSIST_VM \n");
			break;

		default:
			printf("SERVER-ERROR: Unknown error. Error code : %d---------- \n", error);
			break;

	}
}

void send_registration_error_message(SharedMemory* message, int destination_vm_id, int error)
{
	struct vm_registration_reply *reply = (struct vm_registration_reply *)&message->msg_content;
	reply->reserved_blocks = 0;
	reply->reserved_blocks_persistent = 0;
	reply->old_blocks = 0;
	reply->old_persistent_blocks = 0;
	reply->first_time_registration = true;
	reply->error = error;

	print_error_message(error);

	send_message(message, destination_vm_id, MSG_TYPE_VM_REG_REPLY);
	pthread_mutex_unlock(&message->lock);
}

/************************* Managing VMs ***********************************/
struct vm_info* find_vm(central_manager *cm, int vm_id)
{
	for(int i=0; i<cm->max_vms; i++)
	{
		if(cm->vm_list[i].vm_id == vm_id)
			return &cm->vm_list[i];
	}
	return NULL;
}

vm_info* allocate_vm_info(central_manager *cm, int vm_id, int pid, char* vm_name, unsigned long int capacity, bool persist_full)
{
	vm_info* vm = NULL;

	for(int i=0; i<cm->max_vms; i++)
	{
		if(cm->vm_list[i].vm_id == -1)
		{
			vm = &cm->vm_list[i];
			break;
		}
	}

	if(!vm)
		return NULL;

	vm->vm_id = vm_id;
	strcpy(vm->vm_name, vm_name);
	vm->capacity = capacity;
	vm->pid = pid;

	vm->to_resize = 0;
	vm->to_resize_failed = 0;
	vm->max_persistent_blocks = 0;
	vm->persist_full = persist_full;

	return vm;
}

/************************* Managing blocks ********************************/
int allocate_multiple_blocks(central_manager* cm, int max, physical_logical_block* block_list, vm_info* vm, bool persistent)
{
	int j;
	unsigned long int pbn = 0;		// SSD block number

	for(j=0; j < max && cm->ssd_space.list_free.size > 0; j++)
	{
		block_t *block = block_list_pop_tail(&cm->ssd_space, &cm->ssd_space.list_free);
		if(!block)
			BUG(__LINE__);

		pbn = block_to_pbn(&cm->ssd_space, block);

		block->vm_index = vm->vm_index;
		block->type = persistent ? SSD_PERSISTENT : SSD_NON_PERSISTENT;
		block->lbn = INDEX_NULL;			// todo

		block_list[j].physical_block = pbn;
		block_list[j].logical_block = -1;		//todo need to work on it

		if(pbn >= 0 && pbn <= 5)
			BUG(__LINE__);

		block_list_add_head(&cm->ssd_space, persistent ? &vm->list_ssd_persistent : &vm->list_ssd_non_persistent, block);
	}

	vm->to_resize -= j;

	return j;
}

int map_physical_to_logical_blocks(central_manager* cm, physical_logical_block *block_list, vm_info *vm, int count)
{
	unsigned long int pbn, lbn, j=0;

	for(int j=0; j<count; j++)		//	lbn < vssd capacity todo
	{
		pbn = block_list[j].physical_block;
		lbn = block_list[j].logical_block;

		block_t *block = pbn_to_block(&cm->ssd_space, pbn);
		if(!block)
			BUG(__LINE__);

		if(block->vm_index != vm->vm_index)
			BUG(__LINE__);

		if(!(block->type == SSD_PERSISTENT || block->type == SSD_NON_PERSISTENT))
			BUG(__LINE__);

		if(block->lbn != INDEX_NULL)
			BUG(__LINE__);

		block->lbn = lbn;
	}

	return count;
}

int get_old_blocks(central_manager* cm, physical_logical_block* block_list, vm_info* vm, int max, unsigned long int *last_block)
{
	unsigned long int j = 0;
	block_t* block;
	unsigned long int pbn = 0;

	block = pbn_to_block(&cm->ssd_space, *last_block);

	if(!block || block->type == SSD_PERSISTENT)
	{	
		if(!block)
			block = block_list_head(&cm->ssd_space, &vm->list_ssd_persistent );
		else
			block = block_list_next(&cm->ssd_space, block);

		*last_block = block_to_pbn(&cm->ssd_space, block);

		for(; block && j < max; block = block_list_next(&cm->ssd_space, block), j++)
		{
			if(block->type != SSD_PERSISTENT)
				BUG(__LINE__);

			pbn = block_to_pbn(&cm->ssd_space, block);

			block_list[j].physical_block = pbn;
			block_list[j].logical_block = block->lbn;

			*last_block = pbn;

			if(pbn >= 0 && pbn <= 5)
				BUG(__LINE__);
		}

		if(j == max)
			return max; 
	}

	if(!block)
		block = block_list_head(&cm->ssd_space, &vm->list_ssd_non_persistent );
	else
		block = block_list_next(&cm->ssd_space, block);

	*last_block = block_to_pbn(&cm->ssd_space, block);

	for(; block && j < max; block = block_list_next(&cm->ssd_space, block), j++)
	{
		if(block->type != SSD_NON_PERSISTENT)
			BUG(__LINE__);

		pbn = block_to_pbn(&cm->ssd_space, block);

		block_list[j].physical_block = pbn;
		block_list[j].logical_block = block->lbn;

		*last_block = pbn;
	}

	return j;
}

void do_synch_resize(central_manager* cm, vm_info* vm, int size)
{
	char operation;
	struct SharedMemory *message;
	static int round = 0;

	message = cm->message;
	if(round == 0)
		pthread_mutex_init(&resize_lock, NULL);

	pthread_mutex_lock(&resize_lock);

	resize_start_time = current_time();
	resize_operation = size < 0 ? SSD_BALLOON_INFLATION : SSD_BALLOON_DEFLATION; 
	resize_round = ++round;
	resize_size = abs(size);

	size += vm->to_resize_failed;
	vm->to_resize_failed = 0;

	if(size == 0)
	{
		printf("SERVER: VM:%d  No need for resize \n", vm->vm_id);
		return;
	}

	vm->to_resize += size;
	operation = size < 0 ? SSD_BALLOON_INFLATION : SSD_BALLOON_DEFLATION;

	get_access_to_shared_memory(message);

	// printf("SERVER: \tGot access tot shared memory\n");

	if(ENABLE_LOG)
		if(operation == SSD_BALLOON_INFLATION)
			printf("LOG\tInflation \t %lld \t %d \t %lu %d\n", current_time(), 
					vm->vm_id, vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size, size);
		else
			printf("LOG\tDeflation \t %lld \t %d \t %lu %d\n", current_time(), 
					vm->vm_id, vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size, size);


	struct resize_req *req = (struct resize_req *)&message->msg_content;
	req->size = size;
	req->operation = operation;

	send_message(message, vm->vm_id, MSG_TYPE_RESIZE_REQ);
	pthread_mutex_unlock(&message->lock);

	if(operation == SSD_BALLOON_INFLATION)
		printf("SERVER: \tResize message send. SSD_BALLOON_INFLATION. size: %d \n", size);
	else
		printf("SERVER: \tResize message send. SSD_BALLOON_DEFLATION. size %d \n", size);

	pthread_mutex_lock(&resize_lock);
	pthread_mutex_unlock(&resize_lock);
	printf("do_synch_resize completed \n");
}


/************************** Policy ****************************************/

int apply_hierarchical_policy(central_manager* cm, vm_info* reg_vm)
{
	struct vm_info *vm;
	struct category_t *category;
	unsigned long int category_allocate;
	unsigned long int allocate_to_vm;

	printf("SERVER: \t\tApplying proportional registration policy \n");

	// Clearing category values
	for(int i = 0;  i < 3;  i++)
	{
		category = &cm->category[i];
		category->vm_count = 0;
		category->max_size = 0;
		category->new_size = 0;
		category->allocated_to_vms = 0;
	}

	// Recalculating category vm_count, max_size
	printf("SERVER: Recalculaing VM count and max size \n");
	for(int i=0;  i < MAX_NO_VMS;  i++)
	{
		vm = &cm->vm_list[i];
		vm->new_size = 0;

		if(vm->vm_id < 0)
			continue;

		if(vm->status == VM_STATUS_INACTIVE)
			continue;

		category = &cm->category[vm->vm_id % 3];
		category->vm_count ++;
		category->max_size += vm->capacity;
	}
	for(int i=0; i<3; i++)
	{
		printf("SERVER: Category: %d. Vm count: %d. max_size: %lu \n", i, cm->category[i].vm_count, cm->category[i].max_size);
	}

	//	Recalculating category new_size
	printf("SERVER: Recalculaing category new size \n");
	unsigned long int block_remaining = cm->ssd_space.size - cm->ssd_space.list_metadata.size;
	unsigned long int allocated = 0;
	printf("Block remaining: %lu \n", block_remaining);
	while(block_remaining > 0)
	{
		allocated = 0;

		for(int i=0;  i<3 && block_remaining>0; i++)
		{
			category = &cm->category[i];
			if(category->new_size < category->max_size)
			{
				category_allocate = MIN3(category->ratio, block_remaining, category->max_size - category->new_size);
				category->new_size += category_allocate;
				block_remaining -= category_allocate;
				allocated += category_allocate;
			}	
		}

		if(allocated == 0)
			break;
	}
	for(int i=0; i<3; i++)
	{
		printf("SERVER: Category: %d. Vm count: %d. max_size: %lu. New size: %lu \n", 
				i, cm->category[i].vm_count, cm->category[i].max_size, cm->category[i].new_size);
	}

	// scanf("%ld", &category_allocate);

	//	Find new size of VMs
	printf("SERVER: Recalculaing vm sizes\n");
	for(int i=0;  i<3; i++)
	{
		category = &cm->category[i];

		if(category->vm_count <= 0)
			continue;

		printf("SERVER: \tCategory: %d\n", i);
		while(category->allocated_to_vms < category->new_size)
		{
			for(int j=0;  j < MAX_NO_VMS && category->allocated_to_vms < category->new_size;  j++)
			{
				vm = &cm->vm_list[j];

				// VM slot is free
				if(vm->vm_id < 0)	
					continue;

				//	Already swapped. 	// 	Not alive
				if(vm->status == VM_STATUS_INACTIVE)
					continue;

				//	Already allocated requered blocks
				if(vm->new_size == vm->capacity)
					continue;

				// VM belongs to different category
				if(category->category_id  !=  vm->vm_id % 3)
					continue;

				allocate_to_vm = MIN3(1, category->new_size - category->allocated_to_vms, vm->capacity - vm->new_size);
				vm->new_size += allocate_to_vm;
				category->allocated_to_vms += allocate_to_vm;

				// printf("SERVER: \t\tAllocating %lu blocks to VM %d\n", allocate_to_vm, vm->vm_id);
			}
		}
	}

	printf("SERVER: Vm new sizes \n");
	for(int i=0;  i < MAX_NO_VMS;  i++)
	{
		vm = &cm->vm_list[i];
		if(vm->vm_id < 0)
			continue;

		printf("SERVER: VM ID: %d. Capacity: %lu. New Size: %lu \n", vm->vm_id, vm->capacity, vm->new_size);
	}

	// Inflation VMs whose allocated blocks > new size 
	for(int i=0;  i < MAX_NO_VMS;  i++)
	{
		vm = &cm->vm_list[i];

		// VM slot is free
		if(vm->vm_id < 0)	
			continue;

		//	VM is not acive 	
		// 	inactive OR reg OR rereg.
		if(vm->status != VM_STATUS_ACTIVE)
			continue;

		// VM No need for inflation
		if(vm->new_size >= vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size)
			continue;

		if(vm == reg_vm)
			BUG(__LINE__);

		printf("SERVER: Inflating VM %d. Size: %ld \n", vm->vm_id, (vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size - vm->new_size));
		do_synch_resize(cm, vm, -1 * (vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size - vm->new_size));
	}

	// Deflating VMs whose allocated blocks < new size 
	for(int i=0;  i < MAX_NO_VMS;  i++)
	{
		vm = &cm->vm_list[i];

		// VM slot is free
		if(vm->vm_id < 0)	
			continue;

		//	VM is not acive 	
		// 	inactive OR reg OR rereg.
		if(vm->status != VM_STATUS_ACTIVE)
			continue;

		// VM No need for deflation
		if(vm->new_size <= vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size)
			continue;

		if(vm == reg_vm)
			continue;

		printf("SERVER: Deflating VM %d. Size: %ld \n", i, vm->new_size - (vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size));
		do_synch_resize(cm, vm, vm->new_size - (vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size));
	}

	return 0;
}

int apply_share_based_policy(central_manager* cm, vm_info* reg_vm)
{
	struct vm_info *vm;
	struct category_t *category;
	unsigned long int category_allocate;
	unsigned long int allocate_to_vm;

	printf("SERVER: \t\tApplying shared based policy \n");

	// 	Clearing values
	for(int j=0;  j < MAX_NO_VMS;  j++)
	{
		vm = &cm->vm_list[j];

		// VM slot is free
		// if(vm->vm_id < 0)	
		// continue;

		vm->new_size = 0;
	}

	//	Recalculating VN new_size
	unsigned long int block_remaining = cm->ssd_space.size - cm->ssd_space.list_metadata.size;
	unsigned long int allocated = 0;
	printf("Block remaining: %lu \n", block_remaining);
	while(block_remaining > 0)
	{
		allocated = 0;

		for(int j=0;  j < MAX_NO_VMS && block_remaining>0;  j++)
		{
			vm = &cm->vm_list[j];
			category = &cm->category[vm->vm_id%3];

			// VM slot is free
			if(vm->vm_id < 0)	
				continue;

			//	Already swapped. 	// 	Not alive
			if(vm->status == VM_STATUS_INACTIVE)
				continue;

			//	Already allocated requered blocks
			if(vm->new_size == vm->capacity)
				continue;

			allocate_to_vm = MIN3(category->ratio, block_remaining, vm->capacity - vm->new_size);

			vm->new_size += allocate_to_vm;
			block_remaining -= allocate_to_vm;
			allocated += allocate_to_vm;
			// printf("SERVER: \t\tAllocating %lu blocks to VM %d\n", allocate_to_vm, vm->vm_id);
		}

		if(allocated == 0)
			break;
	}


	// scanf("%ld", &category_allocate);

	printf("SERVER: Vm new sizes \n");
	for(int i=0;  i < MAX_NO_VMS;  i++)
	{
		vm = &cm->vm_list[i];
		if(vm->vm_id < 0)
			continue;

		printf("SERVER: VM ID: %d. Capacity: %lu. New Size: %lu \n", vm->vm_id, vm->capacity, vm->new_size);
	}

	// Inflation VMs whose allocated blocks > new size 
	for(int i=0;  i < MAX_NO_VMS;  i++)
	{
		vm = &cm->vm_list[i];

		// VM slot is free
		if(vm->vm_id < 0)	
			continue;

		//	VM is not acive 	
		// 	inactive OR reg OR rereg.
		if(vm->status != VM_STATUS_ACTIVE)
			continue;

		// VM No need for inflation
		if(vm->new_size >= vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size)
			continue;

		if(vm == reg_vm)
			BUG(__LINE__);

		printf("SERVER: Inflating VM %d. Size: %ld \n", vm->vm_id, (vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size - vm->new_size));
		do_synch_resize(cm, vm, -1 * (vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size - vm->new_size));
	}

	// Deflating VMs whose allocated blocks < new size 
	for(int i=0;  i < MAX_NO_VMS;  i++)
	{
		vm = &cm->vm_list[i];

		// VM slot is free
		if(vm->vm_id < 0)	
			continue;

		//	VM is not acive 	
		// 	inactive OR reg OR rereg.
		if(vm->status != VM_STATUS_ACTIVE)
			continue;

		// VM No need for deflation
		if(vm->new_size <= vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size)
			continue;

		if(vm == reg_vm)
			continue;

		printf("SERVER: Deflating VM %d. Size: %ld \n", i, vm->new_size - (vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size));
		do_synch_resize(cm, vm, vm->new_size - (vm->list_ssd_non_persistent.size + vm->list_ssd_persistent.size));
	}

	return 0;
}

int apply_normal_policy(central_manager* cm, vm_info* reg_vm)
{
	struct vm_info *vm;
	struct category_t *category;
	unsigned long int category_allocate;
	unsigned long int allocate_to_vm;

	printf("SERVER: \t\tApplying normal policy \n");

	if(cm->ssd_space.list_free.size < reg_vm->new_size)
		reg_vm->new_size = cm->ssd_space.list_free.size;

	return 0;
}

int apply_policy(central_manager* cm, vm_info* reg_vm)
{
	int current_policy = 0;

	// current_policy = HIERARCHICAL_BASED_POLICY;
	// current_policy = SHARE_BASED_POLICY;
	current_policy = NORMAL_POLICY;

	if(current_policy == SHARE_BASED_POLICY)
	{
		return apply_share_based_policy(cm, reg_vm);
	}
	else if(current_policy == HIERARCHICAL_BASED_POLICY)
	{
		apply_hierarchical_policy(cm, reg_vm);
	}
	else if(current_policy == NORMAL_POLICY)
	{
		apply_normal_policy(cm, reg_vm);
	}
}


int apply_first_registration_policy(central_manager* cm, vm_info* reg_vm, unsigned long int current_allocate, unsigned long int current_persisit)
{
	struct vm_info *vm;

	printf("SERVER: \t\tApplying first registration policy \n");

	unsigned long int category_size = 214038;		//68266;		
	unsigned long int category_allocated = 0;
	unsigned long int category_free = 0;
	unsigned long int category_vm_count = 0;
	unsigned long int blocks_required = 0;
	unsigned long int resize_per_vm = 0;
	unsigned long int per_vm_size = 0;
	unsigned long int total_resize = 0;


	for(int i=0; i< cm->max_vms; i++)
	{
		vm = &cm->vm_list[i];

		if(vm->vm_id < 0 || vm->vm_id == reg_vm->vm_id)
			continue;

		if(reg_vm->vm_id % 3 == vm->vm_id % 3 )
		{
			category_vm_count++;
			category_allocated += vm->list_ssd_persistent.size  + vm->list_ssd_non_persistent.size;
		}
	}

	category_free = category_size - category_allocated;

	printf("SERVER: Category size: %lu blocks. allocated: %lu blocks. free: %lu blocks \n",
			category_size, category_allocated, category_free);

	if(category_free > current_allocate)
	{
		reg_vm->to_resize = current_allocate;
		reg_vm->max_persistent_blocks = reg_vm->persist_full ? 0 : min(current_persisit, reg_vm->to_resize);

		printf("SERVER: \t\t\tBlocks reserved: %ld. Max persistent blocks: %ld \n", 
				reg_vm->to_resize, reg_vm->max_persistent_blocks);

		return 0;
	}

	per_vm_size = (category_size / (category_vm_count + 1));

	// blocks_required = (category_size / (category_vm_count + 1))  - category_free;
	// resize_per_vm = blocks_required / (category_vm_count + 1);
	// printf("Requries to evict %lu blocks from some VMs. per vm: %lu blocks \n", blocks_required, resize_per_vm);
	current_allocate = per_vm_size;
	printf("SERVER: New per vm size: %lu blocks. total blocks to evict: %lu \n", per_vm_size, current_allocate - category_free);


	release_shared_memory(cm->message, __LINE__);


	for(int i=0; i < cm->max_vms; i++)
	{
		vm = &cm->vm_list[i];

		if(vm->vm_id < 0 || vm->vm_id == reg_vm->vm_id)
			continue;

		if(reg_vm->vm_id % 3 == vm->vm_id % 3 )
		{
			resize_per_vm = (vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size) - per_vm_size;
			total_resize += resize_per_vm;
			printf("Evicting %lu blocks from vm: %d-%s \n", resize_per_vm, vm->vm_id, vm->vm_name);
			do_synch_resize(cm, vm, -1 * resize_per_vm);
		}
	}

	printf("SERVER: All resize completed. total blocks evicted: %lu \n", total_resize);
	reg_vm->to_resize = category_free + total_resize;
	reg_vm->max_persistent_blocks = reg_vm->persist_full ? 0 : min(current_persisit, reg_vm->to_resize);

	get_access_to_shared_memory(cm->message);

	return 0;
}

int apply_re_registration_policy(central_manager* cm, vm_info* vm, unsigned long int current_allocate, unsigned long int current_persisit)
{
	unsigned long int allocated_blocks = vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size;

	printf("SERVER: \t\tApplying re-registration policy \n");
	printf("SERVER: \t\t\tAllocate: %lu, Persisit: %lu \n", current_allocate, current_persisit);
	printf("SERVER: \t\t\tAlready allocated: %lu, persisited: %lu \n", allocated_blocks, vm->list_ssd_persistent.size);

	long int new_blocks_required = current_allocate - allocated_blocks;
	if(new_blocks_required > 0)
		vm->to_resize = new_blocks_required; 		//min(new_blocks_required, cm->free_blocks);
	else
		vm->to_resize = 0;

	vm->max_persistent_blocks = vm->persist_full ? 0 : current_persisit ;

	printf("SERVER: \t\t\tBlocks reserved: %ld. Max persistent blocks: %ld \n", 
			vm->to_resize, vm->max_persistent_blocks);

	vm->to_resize_failed = 0;

	return 0;
}

int stage;

int change_category_allocation_ration(central_manager *cm)
{
	/*int r0, r1, r2;
	int lcm;
	printf("Enter the new ratio (seperated by space): ");
	scanf("%d %d %d", &r0, &r1, &r2);
	printf("Enter lcm: ");
	scanf("%d", &lcm);
	
	printf("Ratio: c0: %d. c1: %d. c2: %d. \n", r0, r1, r2);
	printf("LCM: %d \n", lcm);
	cm->category[0].ratio = r0;
	cm->category[1].ratio = r1;
	cm->category[2].ratio = r2;
	cm->lcm = lcm;*/

	if(stage == 0)
	{
		cm->category[0].ratio = 4;
		cm->category[1].ratio = 2;
		cm->category[2].ratio = 3;

		cm->lcm = 12;

		stage ++;
	}
	else if(stage == 1)
	{
		cm->category[0].ratio = 3;
		cm->category[1].ratio = 2;
		cm->category[2].ratio = 1;

		cm->lcm = 6;
		stage ++;
	}

	// apply_hierarchical_policy(cm, NULL);
	// apply_policy(cm, NULL);

	return 1;
}


/*********************** Functions in Listner thread **********************/
int handle_first_registration(central_manager* cm, vm_registration_req *req)
{
	int error = 0;
	unsigned long int current_transfer;
	SharedMemory* message;
	vm_info* vm;
	unsigned long int current_persisit;

	message = cm->message;

	printf("SERVER: \tVM first time registration. \n");

	// Allocating entry in vm_list
	vm = allocate_vm_info(cm, req->vm_id, req->pid, req->vm_name, req->capacity, req->persist_full);
	if(!vm)
		return ERROR_NO_VM_SLOT;
	printf("SERVER: \t\tFree vm_slot: %d \n ", vm->vm_index);
	vm->status = VM_STATUS_REG;
	current_persisit = req->current_persist;

	vm->new_size = req->current_allocate;
	printf("Size: %lu. Current request: %lu \n", vm->capacity, vm->new_size);

	// Applying first time registartion policy
	//error = apply_first_registration_policy(cm, vm, req->current_allocate, req->current_persist);

	release_shared_memory(cm->message, __LINE__);
	// error = apply_hierarchical_policy(cm, vm);
	error = apply_policy(cm, vm);
	if(error)
		return error;
	get_access_to_shared_memory(cm->message);


	//	Allocating blocks for the new VM
	vm->to_resize = vm->new_size;
	vm->max_persistent_blocks = vm->persist_full ? 0 : min(current_persisit, vm->to_resize);

	// Creating registration reply message
	printf("SERVER: \t\tCreate registration reply message \n");
	struct vm_registration_reply *reply = (struct vm_registration_reply *)&message->msg_content;
	reply->reserved_blocks = vm->to_resize;
	reply->reserved_blocks_persistent = vm->max_persistent_blocks;
	reply->old_blocks = 0;
	reply->old_persistent_blocks = 0;
	reply->first_time_registration = true;
	reply->error = 0;

	//	Send registration reply
	send_message(message, vm->vm_id, MSG_TYPE_VM_REG_REPLY);
	pthread_mutex_unlock(&message->lock);

	if(ENABLE_LOG)
		printf("LOG\tFirst_reg\t%lld\t%d\t%lu\n", current_time(), vm->vm_id, vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size);

	unsigned long long int pbn = 0;		//	todo start from first free block
	int flag;

	printf("SERVER: \t\tTransfering blocks \n");
	int round = 0;
	do
	{
		get_access_to_shared_memory(message);
		struct phsical_block_allocation *alcn = (struct phsical_block_allocation *)&message->msg_content;

		alcn->allocation_type = (vm->persist_full || vm->max_persistent_blocks > vm->list_ssd_persistent.size) ? TYPE_NEW_PERSISTENT : TYPE_NEW_NON_PERSISTENT;
		current_transfer = (vm->persist_full || vm->max_persistent_blocks <= vm->list_ssd_persistent.size) ? vm->to_resize : vm->max_persistent_blocks - vm->list_ssd_persistent.size;
		current_transfer = min(current_transfer, max_transfer_alcn);

		alcn->count = allocate_multiple_blocks(cm, current_transfer, alcn->block_list, vm, alcn->allocation_type == TYPE_NEW_PERSISTENT);

		alcn->flag = FLAG_ASK_ME_AGAIN;
		if(vm->to_resize == 0 || alcn->count < current_transfer)
			alcn->flag = FLAG_DO_NOT_ASK_ME;
		flag = alcn->flag;

		printf("SERVER: \t\t\tRound: %d. Type: %s. Count: %d. vm-allocated: %lu. allloc-persisted: %lu  \n", 
				++round,
				(alcn->allocation_type == TYPE_OLD_BLOCK) ? "OLD" : ((alcn->allocation_type == TYPE_NEW_PERSISTENT)?"NEW-PERSIST":"NEW-NON-PERSIST"),
				alcn->count,
				vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size,
				vm->list_ssd_persistent.size);	

		send_message(message, vm->vm_id, MSG_TYPE_PHYSICAL_BLOCK_ALCN);

		wait_for_reply(message, vm->vm_id, MSG_TYPE_PHYSICAL_BLOCK_ALCN_REPLY);
		phsical_block_allocation_reply *alcn_reply = (phsical_block_allocation_reply *)&message->msg_content;

		map_physical_to_logical_blocks(cm, alcn_reply->block_list, vm, alcn_reply->count);
		if(ENABLE_LOG)
			printf("LOG\tFirst_reg\t%lld\t%d\t%lu\n", current_time(), vm->vm_id, vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size);
		release_shared_memory(message, __LINE__);


	}while(flag == FLAG_ASK_ME_AGAIN);

	vm->status = VM_STATUS_ACTIVE;

	vm->to_resize_failed = vm->to_resize;
	vm->to_resize = 0;

	return 0;
}

int handle_re_registration(central_manager* cm, vm_registration_req *req, vm_info* vm)
{
	int error = 0;
	int flag;
	unsigned long int current_transfer;
	unsigned long int pbn = 0;
	unsigned long int old_blocks_to_trasfer;
	unsigned long int current_persisit;

	SharedMemory* message;
	unsigned long int last_block = INDEX_NULL;

	message = cm->message;

	printf("SERVER: \t VM re-registration \n");

	error = swap_in_blocks(cm, vm);
	if(error)
		return error;


	vm->status = VM_STATUS_REREG;
	current_persisit = req->current_persist;

	release_shared_memory(cm->message, __LINE__);
	// error = apply_hierarchical_policy(cm, vm);
	error = apply_policy(cm, vm);
	if(error)
		return error;
	get_access_to_shared_memory(cm->message);

	//	Allocating blocks for the new VM
	vm->to_resize = vm->new_size;
	vm->max_persistent_blocks = vm->persist_full ? 0 : min(current_persisit, vm->to_resize);

	// error = apply_re_registration_policy(cm, vm, req->current_allocate, req->current_persist);
	// if(error)
	// 	return error;

	old_blocks_to_trasfer = vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size;

	if(old_blocks_to_trasfer > vm->new_size)
	{
		printf("\n\nError \n");
	}

	printf("SERVER: \t\tCreate registration reply message \n");
	struct vm_registration_reply *reply = (struct vm_registration_reply *)&message->msg_content;
	reply->reserved_blocks = vm->to_resize;
	reply->reserved_blocks_persistent = vm->max_persistent_blocks;
	reply->old_blocks = old_blocks_to_trasfer;
	reply->old_persistent_blocks = vm->list_ssd_persistent.size;
	reply->first_time_registration = false;

	send_message(message, vm->vm_id, MSG_TYPE_VM_REG_REPLY);
	pthread_mutex_unlock(&message->lock);

	printf("SERVER: \t\tTransfering the physical blocks \n");
	int round = 0;
	do
	{
		// if(round == 5)
		// 	exit(1);

		printf("1 \n");

		get_access_to_shared_memory(message);
		struct phsical_block_allocation *alcn = (struct phsical_block_allocation *)&message->msg_content;

		if(old_blocks_to_trasfer > 0)
		{
			alcn->count = get_old_blocks(cm, alcn->block_list, vm, max_transfer_alcn, &last_block);
			old_blocks_to_trasfer -= alcn->count;

			alcn->allocation_type = TYPE_OLD_BLOCK;
			alcn->flag = FLAG_DO_NOT_ASK_ME;
			if(old_blocks_to_trasfer > 0 || vm->to_resize > 0)
				alcn->flag = FLAG_ASK_ME_AGAIN;
			flag = alcn->flag;

			printf("SERVER: \t\t\tRound: %d. Type: %s. Count: %d. vm-allocated: %lu. allloc-persisted: %lu  \n", 
					++round,
					(alcn->allocation_type == 1) ? "OLD" : ((alcn->allocation_type==2)?"NEW-PERSIST":"NEW-NON-PERSIST"),
					alcn->count,
					vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size,
					vm->list_ssd_persistent.size);	

			send_message(message, vm->vm_id, MSG_TYPE_PHYSICAL_BLOCK_ALCN);
			pthread_mutex_unlock(&message->lock);

			continue;
		}

		alcn->allocation_type = (vm->persist_full || vm->max_persistent_blocks > vm->list_ssd_persistent.size) ? TYPE_NEW_PERSISTENT : TYPE_NEW_NON_PERSISTENT;
		current_transfer = (vm->persist_full || vm->max_persistent_blocks <= vm->list_ssd_persistent.size) ? vm->to_resize : vm->max_persistent_blocks - vm->list_ssd_persistent.size;
		current_transfer = min(current_transfer, max_transfer_alcn);

		alcn->count = allocate_multiple_blocks(cm, current_transfer, alcn->block_list, vm,  alcn->allocation_type == TYPE_NEW_PERSISTENT);

		alcn->flag = FLAG_ASK_ME_AGAIN;
		if(vm->to_resize == 0 || alcn->count < current_transfer)
			alcn->flag = FLAG_DO_NOT_ASK_ME;
		flag = alcn->flag;

		printf("SERVER: \t\t\tRound: %d. Type: %s. Count: %d. vm-allocated: %lu. allloc-persisted: %lu. Flag: %d \n", 
				++round,
				(alcn->allocation_type == 1) ? "OLD" : ((alcn->allocation_type==2)?"NEW-PERSIST":"NEW-NON-PERSIST"),
				alcn->count,
				vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size,
				vm->list_ssd_non_persistent.size,
				alcn->flag);


		send_message(message, vm->vm_id, MSG_TYPE_PHYSICAL_BLOCK_ALCN);

		wait_for_reply(message, vm->vm_id, MSG_TYPE_PHYSICAL_BLOCK_ALCN_REPLY);
		phsical_block_allocation_reply *alcn_reply = (phsical_block_allocation_reply *)&message->msg_content;

		map_physical_to_logical_blocks(cm, alcn_reply->block_list, vm, alcn_reply->count);
		release_shared_memory(message, __LINE__);

	}while(flag == FLAG_ASK_ME_AGAIN);

	vm->to_resize_failed = vm->to_resize;
	vm->to_resize = 0;

	return 0;
}

void* handle_vm_registration(void* arg)
{
	int error = 0;
	int vm_id;
	central_manager *cm;
	SharedMemory* message;
	vm_info* vm;

	cm = cm_global;
	message = cm->message;
	printf("\nSERVER: VM registration request \n");
	struct vm_registration_req *req = (struct vm_registration_req *)&message->msg_content;

	printf("SERVER: \tVM id:%d. name:%s. size:%ld \n",
			req->vm_id, req->vm_name, req->capacity);

	vm_id = req->vm_id;

	vm = find_vm(cm, req->vm_id);
	if(vm)
		error = handle_re_registration(cm, req, vm);
	else
		error = handle_first_registration(cm, req);

	if(error)
		send_registration_error_message(message, vm_id, error);

	persist_metadata(cm);

}

void* handle_inflation_reply(void * arg)
{
	int error = 0;
	unsigned long pbn;
	central_manager* cm;
	inflation_reply *reply;
	SharedMemory *message;
	vm_info *vm;
	block_t *block;
	bool inflation_completed = false;

	cm = cm_global;
	message = cm->message;


	printf("SERVER: \tInflation reply received \n");
	reply = (inflation_reply *)message->msg_content;

	vm = find_vm(cm, reply->vm_id);
	if(!vm)
		BUG(__LINE__);

	printf("SERVER: \tVM id: %d \n", reply->vm_id);
	printf("SERVER: \t\tNumber of blocks given by backend: %u \n", reply->count);

	for(unsigned long int i=0; i<reply->count; i++)
	{
		// printf("%ld[%ld]    \n", i, reply->block_list[i]);
		pbn = reply->block_list[i];
		block_t *block = pbn_to_block(&cm->ssd_space, pbn);

		if(!(block->type == SSD_PERSISTENT || block->type == SSD_NON_PERSISTENT))
			BUG(__LINE__);

		if(block->vm_index != vm->vm_index)
			BUG(__LINE__);

		if(vm->persist_full && block->type != SSD_PERSISTENT)
			BUG(__LINE__);

		block_list_del(&cm->ssd_space, block->type == SSD_PERSISTENT ? &vm->list_ssd_persistent : &vm->list_ssd_non_persistent, block);
		free_block(&cm->ssd_space, block);
	}

	vm->to_resize += reply->count;
	inflation_completed = vm->to_resize == 0;

	if(reply->flag == FLAG_DO_NOT_ASK_ME || (vm->to_resize !=0 && vm->to_resize_failed != 0))
	{
		vm->to_resize_failed += vm->to_resize;
		vm->to_resize = 0;

		//	todo
		vm->to_resize_failed = 0;
		inflation_completed = 1;
	}


	if(ENABLE_LOG)
		printf("LOG\tInflation\t%lld\t%d\t%lu\n", current_time(), vm->vm_id, vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size);

	if(inflation_completed)
	{
		persist_metadata(cm);

		resize_end_time = current_time();

		if(ENABLE_LOG)
			printf("BENCH_MARK:\t%lu\t%lu\t%lu\t%lu\t%c\t%lu\n",
					resize_round, resize_start_time, resize_end_time, resize_end_time - resize_start_time,
					resize_operation, resize_size);

		pthread_mutex_unlock(&resize_lock);
		printf("resize lock released \n");

	}
	release_shared_memory(message, __LINE__);
	printf("Shared memory released \n");
}

void* handle_deflation(void *arg)
{
	int error = 0;
	int flag;
	unsigned long pbn;
	unsigned long int request_count;
	unsigned long int current_transfer;
	central_manager *cm;
	DeflationReq *deflation_req;
	SharedMemory *message;
	vm_info *vm;
	bool deflation_completed;

	cm = cm_global;
	message = cm->message;

	printf("SERVER: \tDeflation request received \n");
	deflation_req = (DeflationReq *)message->msg_content;

	vm = find_vm(cm, deflation_req->vm_id);
	if(!vm)
		BUG(__LINE__);

	request_count = deflation_req->requested_size;

	printf("SERVER: \tVM id: %d \n", vm->vm_id);
	printf("SERVER: \t\tNumber of blocks request by backend: %lu \n", request_count);

	DeflationBlockAllocation *deflation_alcn = (DeflationBlockAllocation *)message->msg_content;

	deflation_alcn->allocation_type = (vm->persist_full || vm->max_persistent_blocks > vm->list_ssd_persistent.size) ? TYPE_NEW_PERSISTENT : TYPE_NEW_NON_PERSISTENT;
	current_transfer = (vm->persist_full || vm->max_persistent_blocks <= vm->list_ssd_persistent.size) ? 
		vm->to_resize : vm->max_persistent_blocks - vm->list_ssd_persistent.size;
	current_transfer = min(current_transfer, max_transfer_alcn);
	current_transfer = min(current_transfer, request_count);

	printf("SERVER: \t\tAllocating %lu blocks for VM \n", current_transfer);

	deflation_alcn->count = allocate_multiple_blocks(cm, current_transfer, deflation_alcn->block_list, vm, 
			deflation_alcn->allocation_type == TYPE_NEW_PERSISTENT);

	if(vm->to_resize == 0 || deflation_alcn->count < current_transfer)
		flag = deflation_alcn->flag = FLAG_DO_NOT_ASK_ME;
	else
		flag = deflation_alcn->flag = FLAG_ASK_ME_AGAIN;

	printf("SERVER: \t\t\tType: %s. Count: %d. vm-allocated: %lu. allloc-persisted: %lu. Flag: %d \n", 
			(deflation_alcn->allocation_type == 1) ? "OLD" : ((deflation_alcn->allocation_type == TYPE_NEW_PERSISTENT)?"NEW-PERSIST":"NEW-NON-PERSIST"),
			deflation_alcn->count,
			vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size,
			vm->list_ssd_persistent.size,
			deflation_alcn->flag);

	send_message(message, vm->vm_id, MSG_TYPE_DEFLATION_BLK_ALCN);

	wait_for_reply(message, vm->vm_id, MSG_TYPE_DEFLATION_BLK_ALCN_REPLY);
	phsical_block_allocation_reply *deflation_alcn_reply = (phsical_block_allocation_reply *)&message->msg_content;

	map_physical_to_logical_blocks(cm, deflation_alcn_reply->block_list, vm, deflation_alcn_reply->count);

	if(flag == FLAG_DO_NOT_ASK_ME || (vm->to_resize !=0 && vm->to_resize_failed != 0))
	{
		vm->to_resize_failed += vm->to_resize;
		vm->to_resize = 0;
	}

	deflation_completed = vm->to_resize == 0;

	if(ENABLE_LOG)
		printf("LOG\tDeflation\t%lld\t%d\t%lu\n", current_time(), vm->vm_id, vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size);
	// release_shared_memory(message);

	if(deflation_completed)
	{
		persist_metadata(cm);

		resize_end_time = current_time();

		if(ENABLE_LOG)
			printf("BENCH_MARK:\t%lu\t%lu\t%lu\t%lu\t%c\t%lu\n",
					resize_round, resize_start_time, resize_end_time, resize_end_time - resize_start_time,
					resize_operation, resize_size);

		pthread_mutex_unlock(&resize_lock);
	}

	release_shared_memory(message, __LINE__);

}



/********************** Functions in Main thread **************************/

void print_logo()
{
	printf("\n\n");

	/*	printf("\t\t*****  *   *  *     *  *****  *      *****  *   *   		           \n");
		printf("\t\t*       * *   * * * *  *      *      *       * *      ---- v1.0 ----   \n");
		printf("\t\t*****    *    *  *  *  ***    *      ***      *       1st April 2018   \n");
		printf("\t\t    *   *     *     *  *      *      *       * *  		               \n");
		printf("\t\t*****  *      *     *  *      *****  *****  *   * 		               \n");*/





	printf("      /@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@    @@@@@@@@@  @@  *@@@@@@@@@@@@@@@@@@@@@@@@@@@@ \n");
	printf("     @@                                             @@         @@                           &@   \n");
	printf("     @*        @@        @@  @@  ,,@@.. .*$@@       @@         @@      .@*@@#.    (@.      @@    \n");
	printf("     @@         @@      (@   @@@@    @@,@    @@     @@         @@    @@&     @@     @@   $@*     \n");
	printf("      @@@        @@    .@    @@       @@      @@    @@@@@@@@@  @@   @@         @.    @@ @@       \n"); 
	printf("        #@@       @@   @,    @@       @@      @@    @@         @@  .@@@@@@@@@@@@@     /@@        \n"); 
	printf("          @@@      @* @#     @@       @@      @@    @@         @@   @@               @@ @@       \n");
	printf("           ,@)      @@@      @@       @@      @@    @@         @@    @@      @@     @@    @@     \n"); 
	printf("    @       @@      @@       @@       @@      @@    @@         @@     @@@@@&*      @@       @@   \n");
	printf("    &@@    @@      @@                                                             @@             \n");
	printf("     *@@@@@(      @@      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@*              \n");





}

void benchmark_resize(central_manager* cm)
{
	int id, size;
	unsigned long int i;
	struct vm_info *vm;
	int operation;
	int flag;
	int total_requested;
	unsigned long int total_given;
	SharedMemory *message;

	message = cm->message;

	// printf("SERVER: Resize \n"); 
	// printf("SERVER: VM ID: ");
	// scanf("%d",&id);
	// printf("SERVER: Size: ");
	// scanf("%d",&size);
	id = 1;

	vm = find_vm(cm, id);
	if(!vm)
	{
		printf("SERVER-ERROR: Invalid vm id \n");
		return;
	}
	printf("SERVER: \tValid vm \n");

	// for(int i=100; i<=200; i++)
	// {
	// 	show_vm_list(cm);
	// 	printf("operation: INFLATION. size: %d blocks = %d GB  \n", i*1024, i);
	// 	sleep(5);
	// 	do_synch_resize(cm, vm, -1*(i*1024));

	// 	show_vm_list(cm);
	// 	printf("operation: DEFLATION. size: %d blocks = %d GB  \n", i*1024, i);
	// 	sleep(5);
	// 	do_synch_resize(cm, vm, i*1024);
	// }

	for(int i=0; i<2500; i++)
	{
		show_vm_list(cm);

		int random = rand();
		unsigned long int current_capacity = (vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size);

		if(random % 2 == 0)
		{
			// Inflation
			if(current_capacity == 0)
			{
				i--;
				continue;
			}

			unsigned long int max_inflation_size = current_capacity;
			size = (random % max_inflation_size) + 1;
			size *= -1;

		}
		else
		{
			// deflation
			unsigned long int max_deflation_size = vm->capacity - current_capacity;
			if(max_deflation_size == 0)
			{
				i--;
				continue;
			}

			size = (random % max_deflation_size) + 1;	
		}

		printf("Resize : %d. Random: %d \n", size, random);
		sleep(1);

		do_synch_resize(cm, vm, size);
	}

	/*for(int i=0; i<10000; i++)
	  {
	  printf("Getting lock \n");
	  pthread_mutex_lock(&resize_lock);
	  printf("Got the lock \n");
	  show_vm_list(cm);
	  int random = rand();
	  unsigned long int current_capacity = (vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size);
	  if(random % 2 == 0)
	  {
	// Inflation
	if(current_capacity == 0)
	{
	i--;
	pthread_mutex_unlock(&resize_lock);
	continue;
	}
	unsigned long int max_inflation_size = current_capacity;
	size = (random % max_inflation_size) + 1;
	size *= -1;
	}
	else
	{
	// deflation
	unsigned long int max_deflation_size = vm->capacity - current_capacity;
	if(max_deflation_size == 0)
	{
	i--;
	pthread_mutex_unlock(&resize_lock);
	continue;
	}
	size = (random % max_deflation_size) + 1;	
	}
	printf("Resize : %d. Random: %d \n", size, random);
	sleep(5);
	resize_start_time = current_time();
	resize_round = i;
	resize_operation = size < 0 ? SSD_BALLOON_INFLATION : SSD_BALLOON_DEFLATION; 
	resize_size = abs(size);
	size += vm->to_resize_failed;
	vm->to_resize_failed = 0;
	if(size == 0)
	{
	printf("SERVER: No need for resize \n");
	return;
	}
	vm->to_resize += size;
	operation = size<0 ? SSD_BALLOON_INFLATION : SSD_BALLOON_DEFLATION;
	get_access_to_shared_memory(message);
	printf("SERVER: \tGot access tot shared memory\n");
	struct resize_req *req = (struct resize_req *)&message->msg_content;
	req->size = size;
	req->operation = operation;
	send_message(message, vm->vm_id, MSG_TYPE_RESIZE_REQ);
	pthread_mutex_unlock(&message->lock);
	if(operation == SSD_BALLOON_INFLATION)
		printf("SERVER: \tResize message send. SSD_BALLOON_INFLATION. size: %d \n", size);
	else
		printf("SERVER: \tResize message send. SSD_BALLOON_DEFLATION. size %d \n", size);
	// sleep(15);
}*/
}

void resize_fn(central_manager* cm)
{
	int id, size;
	unsigned long int i;
	struct vm_info *vm;
	int operation;
	int flag;
	int total_requested;
	unsigned long int total_given;
	SharedMemory *message;

	message = cm->message;

	printf("SERVER: Resize \n"); 
	printf("SERVER: VM ID: ");
	scanf("%d",&id);
	printf("SERVER: Size: ");
	scanf("%d",&size);

	vm = find_vm(cm, id);
	if(!vm)
	{
		printf("SERVER-ERROR: Invalid vm id \n");
		return;
	}
	printf("SERVER: \tValid vm \n");

	size += vm->to_resize_failed;
	vm->to_resize_failed = 0;

	if(size == 0)
	{
		printf("SERVER: No need for resize \n");
		return;
	}

	vm->to_resize += size;

	operation = size<0 ? SSD_BALLOON_INFLATION : SSD_BALLOON_DEFLATION;
	get_access_to_shared_memory(message);

	printf("SERVER: \tGot access tot shared memory\n");

	struct resize_req *req = (struct resize_req *)&message->msg_content;
	req->size = size;
	req->operation = operation;

	send_message(message, vm->vm_id, MSG_TYPE_RESIZE_REQ);
	pthread_mutex_unlock(&message->lock);

	if(operation == SSD_BALLOON_INFLATION)
		printf("SERVER: \tResize message send. SSD_BALLOON_INFLATION. size: %d \n", size);
	else
		printf("SERVER: \tResize message send. SSD_BALLOON_DEFLATION. size %d \n", size);
}

void show_vm_list(central_manager* cm)
{
	char buf[25];

	unsigned long int total_size = 0;
	unsigned long int total_persistent = 0;
	unsigned long int total_non_persistent = 0;
	unsigned long int total_swapped = 0;
	long int total_resize = 0;
	long int total_resize_failed = 0;

	clearScreen();

	printf("\n\n");

	// print_logo();


	printf("\n\n                                              Status \n");

	printf("---------------------------------------------------------------------------------------------------- \n");
	printf("    %20s %5s %7s %11s %25s %5s %8s %8s \n",
			"", "VM", "vSSD", "Maximum", "Allocated blocks", "",  "", "+/-");
	printf("   %21s %5s %7s %11s %11s %11s %7s %8s %8s\n",
			"VM name",  "ID","size", "Persistent", "persistent", "non-persist", "swapped", "+/-", "failed");
	printf("---------------------------------------------------------------------------------------------------- \n");
	for(int i=0; i<cm->max_vms; i++)
	{
		vm_info *vm = &cm->vm_list[i];
		sprintf(buf, "%lu", vm->max_persistent_blocks);

		printf("%2d %21s %5d %7lu %11s %11lu %11lu %7lu %8ld %8ld \n",
				i, 
				vm->vm_name,
				// vm->pid, 
				vm->vm_id, 
				vm->capacity, 
				(vm->vm_id == -1) ? "-" : (vm->persist_full ? "FULL" :(vm->max_persistent_blocks == 0 ? "NONE" : buf)),
				vm->list_ssd_persistent.size,
				vm->list_ssd_non_persistent.size, 
				vm->swapped_block_list.size,
				vm->to_resize, 
				vm->to_resize_failed);

		total_size += vm->capacity;
		total_persistent += vm->list_ssd_persistent.size;
		total_non_persistent += vm->list_ssd_non_persistent.size;
		total_swapped += vm->swapped_block_list.size;
		total_resize += vm->to_resize;
		total_resize_failed += vm->to_resize_failed;
	}
	printf("---------------------------------------------------------------------------------------------------- \n");
	printf("%2s  %20s %5s %7lu %11s %11lu %11lu %7lu %8ld %8ld \n",
			"", 
			"Total", 
			"", 
			total_size, 
			"",
			total_persistent,
			total_non_persistent,
			total_swapped,
			total_resize,
			total_resize_failed);
	printf("---------------------------------------------------------------------------------------------------- \n");


	printf("\n\n\t%14s%10s%20s%15s \n", "","SSD [/dev/sdc]", "", "Swap Disk[swap_disk.img]");
	printf("\t------------------------------------------------------------------------------- \n");

	printf("\t|%25s: %10lu |%25s: %10lu |\n","SSD capacity", cm->ssd_space.size, "Swap device capacity", cm->swap_space.size);
	printf("\t|%25s  %10s |%25s  %10s |\n","", "","","");
	printf("\t|%25s: %10lu |%25s  %10s |\n","Metdata blocks", cm->ssd_space.list_metadata.size, "","");
	printf("\t|%25s: %10lu |%25s: %10lu |\n",
			"Blocks allocated for VM", cm->ssd_space.size - (cm->ssd_space.list_free.size + cm->ssd_space.list_metadata.size), 
			"Swap blocks used", cm->swap_space.size - cm->swap_space.list_free.size);
	printf("\t|%25s: %10lu |%25s: %10lu |\n","Free blocks", cm->ssd_space.list_free.size, "Free swap blocks", cm->swap_space.list_free.size);

	// printf("\tNumber of blocks to free: %llu \n", cm->blocks_to_free);
	printf("\t------------------------------------------------------------------------------- \n\n\n");

}

void recover_non_persistent_blocks(central_manager* cm, vm_info *vm_p)
{
	int vm_id;
	int error = 0;
	unsigned  long int pbn, lbn;
	unsigned  long int blocks_freed = 0;
	vm_info* vm;

	if(vm_p == NULL)
	{

		printf("SERVER: Enter VM id : ");
		scanf("%d", &vm_id);

		vm = find_vm(cm, vm_id);
		if(!vm)
		{
			error = ERROR_INVALID_VM;
			goto handle_error;
		}
	}
	else
	{
		vm = vm_p;
	}

	printf("SERVER: Recovering non-persistent blocks from VM %d. Non-persistent blocks: %lu \n", vm->vm_id, vm->list_ssd_non_persistent.size);

	block_t *block = block_list_pop_tail(&cm->ssd_space, &vm->list_ssd_non_persistent);
	for( ;block ; block = block_list_pop_tail(&cm->ssd_space, &vm->list_ssd_non_persistent))
	{
		// pbn = block_to_pbn(&cm->ssd_space, block);
		// lbn = block->lbn;

		if(block->vm_index != vm->vm_index)
			BUG(__LINE__);

		if(block->type != SSD_NON_PERSISTENT)
			BUG(__LINE__);

		free_block(&cm->ssd_space, block);
		blocks_freed++;

		if(blocks_freed % 2048 == 0 && ENABLE_LOG)
			printf("LOG\tRecovery\t%lld\t%d\t%lu\n", current_time(), vm->vm_id, vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size);
	}

	if(ENABLE_LOG)
		printf("LOG\tRecovery\t%lld\t%d\t%lu\n", current_time(), vm->vm_id, vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size);

	printf("SERVER: %lu blocks recovered from VM %d \n", blocks_freed, vm->vm_id);
	// persist_metadata(cm);
	return;

handle_error:
	print_error_message(error);

}

int migrate_block(unsigned long int source_pbn, int source_fd, unsigned long int destination_pbn, int destination_fd, unsigned long int block_size, char *buf)
{
	int ret = 0;

	lseek(source_fd, source_pbn * block_size * 512, SEEK_SET);
	ret = read(source_fd, buf, block_size * 512);
	if(ret != block_size * 512)
	{
		printf("SERVER: Reading from the source failed. ret = %d \n", ret);
		return -1;
	}

	lseek(destination_fd, destination_pbn * block_size * 512, SEEK_SET);
	ret = write(destination_fd, buf, block_size * 512);
	if(ret != block_size * 512)
	{
		printf("SERVER: Writing to swap device failed. ret = %d \n", ret);
		return -1;
	}

	return 1;
}

void swap_out_persistent_blocks(central_manager* cm, vm_info *vm_p)
{
	int vm_id;
	int error = 0;
	int ret;
	vm_info *vm;
	unsigned long int pbn=0;
	unsigned long int blocks_swapped = 0;
	block_t *ssd_block;
	block_t *swap_block;
	char* buf;

	if(vm_p == NULL)
	{
		printf("SERVER: Enter VM id : ");
		scanf("%d", &vm_id);

		vm = find_vm(cm, vm_id);
		if(!vm)
		{
			error = ERROR_INVALID_VM;
			goto handle_error;
		}
	}
	else
	{
		vm = vm_p;
	}

	if(vm->list_ssd_non_persistent.size > 0 )
	{
		printf("SERVER: Swapping failed \n");
		printf("SERVER: Please recover non-persistent blocks before swapping the persistent blocks \n");
		return;
	}

	buf = malloc(sizeof(char) * cm->vssd_block_size * 512);
	printf("SERVER: \tSwap buffe size: %lu \n", sizeof(char) * cm->vssd_block_size * 512);


	while(vm->list_ssd_persistent.size > 0 && cm->swap_space.list_free.size > 0)
	{
		ssd_block = block_list_pop_tail(&cm->ssd_space, &vm->list_ssd_persistent);
		if(!ssd_block)
			BUG(__LINE__);

		if(ssd_block->vm_index != vm->vm_index)
			BUG(__LINE__);

		if(ssd_block->type != SSD_PERSISTENT)
			BUG(__LINE__);

		swap_block = block_list_pop_tail(&cm->swap_space, &cm->swap_space.list_free);
		if(!swap_block)
			BUG(__LINE__);

		unsigned long int ssd_pbn = block_to_pbn(&cm->ssd_space, ssd_block);
		unsigned long int swap_pbn = block_to_pbn(&cm->swap_space, swap_block);

		migrate_block(ssd_pbn, cm->metadata_fd, swap_pbn, cm->swap_fd, cm->vssd_block_size, buf);

		swap_block->vm_index = ssd_block->vm_index;
		swap_block->lbn = ssd_block->lbn;
		swap_block->type = SWAP_ALLOCATED;

		if(vm->swapped_block_list.size % 100 == 0)
			printf(" Swap out: %lu. lbn: %lu. ssd-pbn: %lu. swap-pbn: %lu \n", vm->swapped_block_list.size, ssd_block->lbn, ssd_pbn, swap_pbn);

		free_block(&cm->ssd_space, ssd_block);
		block_list_add_head(&cm->swap_space, &vm->swapped_block_list, swap_block);

		blocks_swapped ++;

		if(blocks_swapped % 3000 == 0 && ENABLE_LOG)
			printf("LOG\tSwap-out\t%lld\t%d\t%lu\n", current_time(), vm->vm_id, vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size);
	}

	free(buf);

	if(ENABLE_LOG)
		printf("LOG\tSwap-out\t%lld\t%d\t%lu\n", current_time(), vm->vm_id, vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size);

	printf("SERVER: Blocks swapped: %lu \n", blocks_swapped);
	persist_metadata(cm);
	return;

handle_error:
	print_error_message(error);
}

int swap_in_blocks(central_manager *cm, vm_info *vm)
{
	int ret;
	char *buf;
	unsigned long int pbn = 0;
	unsigned long int s_pbn = 0;
	unsigned long int blocks_swapped = 0;
	block_t *ssd_block;
	block_t *swap_block;

	if(vm->swapped_block_list.size == 0)
		return 0;

	printf("SERVER: Swapping in \n");
	printf("SERVER: \tNumber of blocks to swap in: %lu. Free space in SSD: %lu \n", 
			vm->swapped_block_list.size, cm->ssd_space.list_free.size);

	if(vm->swapped_block_list.size > cm->ssd_space.list_free.size)
		return ERROR_NO_FREE_BLOCK_IN_SSD_FOR_SWAP_IN;

	printf("\n STARTED \n");


	buf = malloc(sizeof(char) * cm->vssd_block_size * 512);

	while(vm->swapped_block_list.size > 0)
	{
		swap_block = block_list_pop_tail(&cm->swap_space, &vm->swapped_block_list);
		if(!swap_block)
			BUG(__LINE__);

		if(swap_block->vm_index != vm->vm_index)
			BUG(__LINE__);

		if(swap_block->type != SWAP_ALLOCATED)
			BUG(__LINE__);

		ssd_block = block_list_pop_tail(&cm->ssd_space, &cm->ssd_space.list_free);
		if(!ssd_block)
			BUG(__LINE__);

		unsigned long int swap_pbn = block_to_pbn(&cm->swap_space, swap_block);
		unsigned long int ssd_pbn = block_to_pbn(&cm->ssd_space, ssd_block);

		migrate_block(swap_pbn, cm->swap_fd, ssd_pbn, cm->metadata_fd, cm->vssd_block_size, buf);

		ssd_block->vm_index = swap_block->vm_index;
		ssd_block->lbn = swap_block->lbn;
		ssd_block->type = SSD_PERSISTENT;

		if(vm->swapped_block_list.size % 100 == 0)
			printf(" Swap in: %lu. lbn: %lu. ssd-pbn: %lu. swap-pbn: %lu \n", vm->swapped_block_list.size, ssd_block->lbn, ssd_pbn, swap_pbn);

		free_block(&cm->swap_space, swap_block);
		block_list_add_head(&cm->ssd_space, &vm->list_ssd_persistent, ssd_block);

		blocks_swapped ++;
	}

	free(buf);

	if(vm->swapped_block_list.size != 0)
		BUG(__LINE__);

	return 0;
}

int recover_dead_vms(central_manager *cm)
{
	SharedMemory *message;
	vm_info *vm;
	unsigned long int active_vm_count = 0;
	unsigned long int recovery_vm_count = 0;

	message = cm->message;

	for(int i=0; i< cm->max_vms; i++)
	{
		vm = &cm->vm_list[i];

		//	Empty vm slot
		if(vm->vm_id < 0)
			continue;

		//	Already recovered.		
		if(vm->swapped_block_list.size > 0 || (vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size) == 0)
			continue;

		// Verify vm alive
		if(kill(vm->pid, 0) == 0)
		{
			// VM is active
			active_vm_count ++;
		}
		else
		{	
			// Recover VM
			recover_non_persistent_blocks(cm, vm);
			swap_out_persistent_blocks(cm, vm);
			recovery_vm_count++;

			vm->status = VM_STATUS_INACTIVE;
		}
	}

	printf("SERVER: Total %lu vms recovered\n", recovery_vm_count);
	return recovery_vm_count;
}

/*************************** Status **************************************/
void process_status_message(central_manager *cm, struct SharedMemory *message)
{
	struct status_msg *status = (struct status_msg *)message->msg_content;
	vm_info *vm = vm = find_vm(cm, status->vm_id);

	if(vm->status_count < 20)
	{
		vm->status_values[vm->status_count].sectors =  status->data[0];
		vm->status_values[vm->status_count].rqst =  status->data[1];
		vm->status_values[vm->status_count].miss =  status->data[2];
		vm->status_values[vm->status_count].resident =  status->data[3];
		vm->status_values[vm->status_count].cache_size =  status->data[4];


		vm->status_values[vm->status_count].bandwidth 
			= ((double)vm->status_values[vm->status_count].sectors) / (2048 * 30);
		vm->status_values[vm->status_count].hit_ratio 
			= 1 - ((double)vm->status_values[vm->status_count].miss / (double)vm->status_values[vm->status_count].rqst);
		// vm->status_values[vm->status_count].utilization 
		// 	= (vm->status_values[vm->status_count].hit_ratio  * (double)(vm->status_values[vm->status_count].cache_size));

		printf("STATUS: %llu VM: %d. [%d] SC: %ld. BW: %2.2f. RQ: %ld. MS: %ld. RS: %ld. CS: %ld. HR: %f. \n",
				current_time(),
				vm->vm_id,
				vm->status_count,		
				vm->status_values[vm->status_count].sectors,
				vm->status_values[vm->status_count].bandwidth,
				vm->status_values[vm->status_count].rqst,
				vm->status_values[vm->status_count].miss,
				vm->status_values[vm->status_count].resident,
				vm->status_values[vm->status_count].cache_size,
				vm->status_values[vm->status_count].hit_ratio);
		// vm->status_values[vm->status_count].utilization );

		if(vm->status_count < 19)
			vm->status_count ++;		
	}

	release_shared_memory(message, __LINE__);
}

/*************************** 2 threads ***********************************/
void* heart_beat_thread(void *arg)
{
	central_manager *cm;
	unsigned long int dead_vms;

	cm = cm_global;

	while(1)
	{
		sleep(60);
		dead_vms = recover_dead_vms(cm);

		if(dead_vms > 0)
		{
			apply_policy(cm, NULL);
			// apply_hierarchical_policy(cm, NULL);
		}
		persist_metadata(cm);
	}
}


void* log_printing_thread(void *arg)
{
	central_manager *cm;
	vm_info *vm;

	cm = cm_global;

	while(1)
	{
		sleep(10);
		for(int i=0; i< cm->max_vms; i++)
		{
			vm = &cm->vm_list[i];

			//	Empty vm slot
			if(vm->vm_id < 0)
				continue;

			//	Already recovered.		
			if(vm->status != VM_STATUS_ACTIVE)
				continue;

			printf("\nLOG\tACTIVE\t%lld\t%d\t%lu\n", current_time(), 
					vm->vm_id, vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size);
		}
	}
}

void* auto_absolute_policy_thread(void *arg)
{
	central_manager *cm;
	vm_info *vm;

	double bandwidth;
	double resident;

	double slo_bandwidth = 30;
	double delta = 0.1;

	double K1 = 0.5;			//	5
	double K2 = 0.5;			//	5
	double K3 = 0;
	double K4 = 0;

	cm = cm_global;

	while(1)
	{
		sleep(5 * 60);
		printf("Auto policy thread wakes up \n");

		for(int i=0; i<MAX_NO_VMS; i++)
		{
			vm = &cm->vm_list[i];
			vm->cwnd_status.CchangeInSize = 0;

			//	Empty vm slot or recovered
			if(vm->vm_id < 0 || vm->status != VM_STATUS_ACTIVE || vm->status_count == 0) 
				continue;


			bandwidth = 0;
			resident = 0;
			for(int j=0; j<vm->status_count; j++)
			{
				bandwidth += vm->status_values[j].bandwidth;
				resident += vm->status_values[j].resident;
			}

			bandwidth /= vm->status_count;
			resident /= vm->status_count;
			vm->cwnd_status.is_slo_met_in_last_round = false; 
			if(vm->status_values[vm->status_count-1].resident == vm->cwnd_status.CcurrentSize)
				if(vm->status_values[vm->status_count-1].bandwidth >= vm->cwnd_status.Bmin)
					vm->cwnd_status.is_slo_met_in_last_round = true;
			vm->status_count = 0;

			vm->cwnd_status.Bmeasured = bandwidth;
			vm->cwnd_status.Bobserved = bandwidth;		//* 1

			vm->cwnd_status.Bmin = slo_bandwidth * (1 - delta); 
			vm->cwnd_status.Bmax = slo_bandwidth * (1 + delta);

			// vm->cwnd_status.Bmin = slo_bandwidth - 2; 
			// vm->cwnd_status.Bmax = slo_bandwidth + 2;

			vm->cwnd_status.Cresident = resident;
			vm->cwnd_status.CcurrentSize = vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size;

			double Cutilization = ((double)vm->cwnd_status.Cresident) / ((double)vm->cwnd_status.CcurrentSize);

			printf("AUTO\t %llu VM: %d OB: %3.2f MBps  AvgRS: %5.2f   UTL: %0.2f   Min: %f   Bax: %f \n",
					current_time(), vm->vm_id, bandwidth, resident, 
					Cutilization,
					vm->cwnd_status.Bmin, vm->cwnd_status.Bmax);

			if(vm->cwnd_status.Bobserved >= vm->cwnd_status.Bmin && vm->cwnd_status.Bobserved <= vm->cwnd_status.Bmax)
			{
				vm->cwnd_status.CchangeInSize = 0; 
				printf("AUTO: SLO met. No changes required \n");
			}
			else if(vm->cwnd_status.Bobserved < vm->cwnd_status.Bmin)
			{
				//	SLO not met
				printf("AUTO: SLO not met. \n");
				if(Cutilization == 1.0 || (Cutilization > 0.9 && !vm->cwnd_status.is_slo_met_in_last_round))
				{
					//	Change = min{max inc, (Bmin-Bobserved) * k3}
					vm->cwnd_status.CchangeInSize = min(10 * 1024, (vm->cwnd_status.Bmin - vm->cwnd_status.Bobserved) *  K1 * 1024);
					printf("AUTO: \t Resize: %lu blocks \n", vm->cwnd_status.CchangeInSize);
				}
				else
				{
					printf("AUTO: \tLess than Tutilization. No changes required  \n");
				}
			}
			else
			{	
				// SLO exceedinglt met
				printf("AUTO: SLO exceedingly met. \n");

				//	Change = min{max dec, (Bobserved - Bmax) * k3}
				vm->cwnd_status.CchangeInSize = -1 * min(5 * 1024, (vm->cwnd_status.Bobserved - vm->cwnd_status.Bmax) *  K2 * 1024);
				printf("AUTO: \t Resize: %ld blocks \n", vm->cwnd_status.CchangeInSize);
			}
		}		

		for(int i=0; i<MAX_NO_VMS; i++)
		{
			vm = &cm->vm_list[i];

			if(vm->vm_id < 0 || vm->status != VM_STATUS_ACTIVE) 
				continue;

			if(vm->cwnd_status.CchangeInSize == 0)
				continue;

			do_synch_resize(cm, vm, vm->cwnd_status.CchangeInSize);
		}
	}
}

void* auto_fair_policy_thread(void *arg)
{
	central_manager *cm;
	vm_info *vm;
	vm_info *max_bw_vm, *min_bw_vm;
	struct category_t* category;

	double bandwidth;
	double resident;

	double slo_bandwidth = 30;
	double delta = 0.1;

	double K1 = 0.5;			//	5
	double K2 = 0.5;			//	5
	double K3 = 0;
	double K4 = 0;

	cm = cm_global;

	sleep(1 * 60);

	while(1)
	{
		printf("---------------------------------- \n");
		sleep(3 * 60);
		printf("Auto policy thread wakes up \n");
		int found_less_utilized_vm = false;

		max_bw_vm = min_bw_vm = NULL;

		for(int i=0; i<MAX_NO_VMS; i++)
		{
			vm = &cm->vm_list[i];
			vm->cwnd_status.CchangeInSize = 0;

			//	Empty vm slot or recovered
			if(vm->vm_id < 0 || vm->status != VM_STATUS_ACTIVE || vm->status_count == 0) 
				continue;

			bandwidth = 0;
			resident = 0;
			for(int j=0; j<vm->status_count; j++)
			{
				bandwidth += vm->status_values[j].bandwidth;
				resident += vm->status_values[j].resident;
			}

			bandwidth /= vm->status_count;
			resident /= vm->status_count;
			// vm->cwnd_status.is_slo_met_in_last_round = false; 
			// if(vm->status_values[vm->status_count-1].resident == vm->cwnd_status.CcurrentSize)
				// if(vm->status_values[vm->status_count-1].bandwidth >= vm->cwnd_status.Bmin)
					// vm->cwnd_status.is_slo_met_in_last_round = true;
			vm->status_count = 0;

			vm->cwnd_status.Bmeasured = bandwidth;

			category = &cm->category[vm->vm_id % 3];
			vm->cwnd_status.Bobserved = bandwidth * (cm->lcm /category->ratio);		

			vm->cwnd_status.Bmin = slo_bandwidth * (1 - delta); 
			vm->cwnd_status.Bmax = slo_bandwidth * (1 + delta);

			// vm->cwnd_status.Bmin = slo_bandwidth - 2; 
			// vm->cwnd_status.Bmax = slo_bandwidth + 2;

			vm->cwnd_status.Cresident = resident;
			vm->cwnd_status.CcurrentSize = vm->list_ssd_persistent.size + vm->list_ssd_non_persistent.size;
			vm->cwnd_status.Cutilization = ((double)vm->cwnd_status.Cresident) / ((double)vm->cwnd_status.CcurrentSize);

			if(vm->cwnd_status.Cutilization < 1)
				found_less_utilized_vm = true;

			if(vm->cwnd_status.Cutilization == 1)
			{
				if(!max_bw_vm || vm->cwnd_status.Bobserved > max_bw_vm->cwnd_status.Bobserved)
					max_bw_vm = vm;
				
				if(!min_bw_vm || vm->cwnd_status.Bobserved < min_bw_vm->cwnd_status.Bobserved)
					min_bw_vm = vm;
			}

			printf("AUTO\t %llu VM: %d  MSB: %3.2f  OB: %3.2f MBps  AvgRS: %5.2f   UTL: %0.2f   CZE: %lu  Min: %f   Bax: %f \n",
					current_time(), vm->vm_id, 
					vm->cwnd_status.Bmeasured,
					vm->cwnd_status.Bobserved,
					resident, 
					vm->cwnd_status.Cutilization, 
					vm->cwnd_status.CcurrentSize,
					vm->cwnd_status.Bmin, vm->cwnd_status.Bmax);
		}

		if(found_less_utilized_vm)
		{
			printf("Less utilized VMs exists \n");
			continue;
		}

		double fraction = 0;

		//	Verify all VMs has fair bandwidth 
		if(cm->ssd_space.list_free.size == 0)
		{	
			if(min_bw_vm)
			{

				if(min_bw_vm == max_bw_vm)
					continue;
				fraction = (max_bw_vm->cwnd_status.Bobserved - min_bw_vm->cwnd_status.Bobserved) / min_bw_vm->cwnd_status.Bobserved;

				printf("AUTO: Min: %f  Max: %f  Diff: %f  \n", 
					min_bw_vm->cwnd_status.Bobserved, max_bw_vm->cwnd_status.Bobserved, fraction);
				if(fraction <= 0.1)
				{
					continue;
				}
			}
		}
				
		//	Finding average bandwidth of all VMs having 100% utilization
		double avg_bandwidth = 0;
		int count = 0;;
		for(int i=0; i<MAX_NO_VMS; i++)
		{
			vm = &cm->vm_list[i];

			if(vm->vm_id < 0 || vm->status != VM_STATUS_ACTIVE) 
				continue;

			if(vm->cwnd_status.Cutilization != 1)
				continue;

			avg_bandwidth += vm->cwnd_status.Bobserved;
			count ++;
		}	
		if(count == 0)
			avg_bandwidth = 0;
		else
			avg_bandwidth /= (double)count;
		printf("Average bandwidth: %f \n", avg_bandwidth);
		

		//	For VMs with Bobserved is greater than avg_bandwidth, find change in size and inflate them
		if(cm->ssd_space.list_free.size == 0)
		{
			for(int i=0; i<MAX_NO_VMS; i++)
			{
				vm = &cm->vm_list[i];

				if(vm->vm_id < 0 || vm->status != VM_STATUS_ACTIVE) 
					continue;

				if(vm->cwnd_status.Cutilization != 1  || vm->cwnd_status.Bobserved <= avg_bandwidth)
					continue;

				//	If the difference betweem 0.1 and 0.2, victimize only the max-bandwidth vm.
				if(fraction > 0.1 && fraction < 0.2 && max_bw_vm && vm != max_bw_vm)
					continue;

				unsigned long int new_size = avg_bandwidth * (((double)vm->cwnd_status.CcurrentSize) / vm->cwnd_status.Bobserved);

				vm->cwnd_status.CchangeInSize =  vm->cwnd_status.CcurrentSize - new_size;
				category = &cm->category[vm->vm_id % 3];
				vm->cwnd_status.CchangeInSize /= cm->lcm / category->ratio;

				vm->cwnd_status.CchangeInSize = -1 * min(vm->cwnd_status.CchangeInSize, 3 * 1024);

				do_synch_resize(cm, vm, vm->cwnd_status.CchangeInSize);
			}	
		}

		//	For VMs with Bobserved is less than avg, find new size and deflate
		unsigned long int total_free_blocks = cm->ssd_space.list_free.size;
		count = 0;
		for(int i=0; i<MAX_NO_VMS; i++)
		{
			vm = &cm->vm_list[i];
			if(vm->vm_id < 0 || vm->status != VM_STATUS_ACTIVE) 
				continue;
			if(vm->cwnd_status.Cutilization != 1  || vm->cwnd_status.Bobserved >= avg_bandwidth)
				continue;

			count ++;
		}
		unsigned long int per_vm_change = total_free_blocks / count;
		unsigned long int balance_blocks = total_free_blocks % count;

		for(int i=0; i<MAX_NO_VMS; i++)
		{
			vm = &cm->vm_list[i];
			if(vm->vm_id < 0 || vm->status != VM_STATUS_ACTIVE) 
				continue;
			if(vm->cwnd_status.Cutilization != 1  || vm->cwnd_status.Bobserved >= avg_bandwidth)
				continue;

			vm->cwnd_status.CchangeInSize = per_vm_change;
			if(balance_blocks > 0)
			{
				vm->cwnd_status.CchangeInSize += 1;
				balance_blocks --;
			}

			do_synch_resize(cm, vm, vm->cwnd_status.CchangeInSize);
		}
	}
}

void* listner_thread(void *arg)
{
	pthread_t tid;
	int temp_sleep=0;
	SharedMemory* message;
	central_manager* cm;
	int ret;

	cm = cm_global;
	message = cm->message;

	printf("SERVER: Initalize the message buffer \n");
	message->msg_type = MSG_TYPE_BUF_FREE;
	message->source = TARGET_NONE;
	message->destination = TARGET_NONE;
	printf("SERVER: Server ready \n");

	while(1)
	{	

		// printf("%llu: LISTENER: Waiting for the lock.\n", current_time());
		pthread_mutex_lock(&message->lock);
		// printf("%llu: LISTENER: Aquired lock.\n", current_time());

		while(!(message->destination == CM_LISTENER_ID && message->msg_type!=MSG_TYPE_BUF_FREE) || temp_sleep)
		{
			ret = pthread_mutex_trylock(&message->lock);
			//printf("LISTENER: tylock ret: %d \n", ret);
			if(ret == 0)
			{
				// printf("\n\nLISTENER: BUG: Somebody unlocked it. Now thread is going to wait with out lock\n\n");
				exit(1);
			}

			// printf("%llu: LISTENER: Going to sleep. will release the lock \n", current_time());
			pthread_cond_wait(&message->cm_can_enter, &message->lock);
			// printf("%llu: LISTENER: woke up. I have the lock \n", current_time());
			temp_sleep = 0;

			// printf("%llu: LISTENER: Dest=%d. Src=%d. Type: %d \n", current_time(),
			// 		message->destination, message->source, message->msg_type);
		}

		// printf("%llu: LISTENER: I have received a message. Type: %d \n", current_time(), message->msg_type);

		switch(message->msg_type)
		{
			case MSG_TYPE_VM_REG:
				message->destination = TARGET_NONE;
				pthread_create(&tid, NULL,handle_vm_registration,NULL);
				// handle_vm_registration(cm);
				break;

			case MSG_TYPE_DEFLATION_REQ:
				message->destination = TARGET_NONE;
				pthread_create(&tid, NULL,handle_deflation,NULL);
				// handle_deflation(cm);
				break;

			case MSG_TYPE_INFLATION_REPLY:
				message->destination = TARGET_NONE;
				pthread_create(&tid, NULL,handle_inflation_reply,NULL);
				// handle_inflation_reply(cm);
				break;

			case MSG_TYPE_STATUS:
				//	No seperate threads for processing status messages
				message->destination = TARGET_NONE;
				process_status_message(cm, message);
				break;

			default:
				temp_sleep=1;
				pthread_mutex_unlock(&message->lock);
				break;
		}
	}
}

void main()
{
	int ret;
	int option;
	struct central_manager *cm;

	clearScreen();
	print_logo();

	cm = initialize_cm();
	if(!cm)
	{
		printf("SERVER: Unable to initialize central_manager \n");
		exit(1);
	}
	cm_global = cm;

	cm->message =  initialize_shared_mem(cm);
	if(!cm->message)
		return;

	printf("Test area \n\n");


	// // ret = pthread_mutex_lock(&cm->message->lock);
	// // printf("Mutext lock: %d \n", ret);

	// ret = pthread_mutex_trylock(&cm->message->lock);
	// printf("Try: %d \n", ret);

	// ret = pthread_mutex_trylock(&cm->message->lock);
	// printf("Try: %d \n", ret);

	// printf("while 1 \n");
	// while(1);


	// pthread_mutex_lock(&(cm->message->lock));
	// printf("SERVER: \tLock ok. ! \n");

	if(pthread_create(&listener_thread_ID, NULL,listner_thread,NULL))
	{
		fprintf(stderr, "Error creating listner thread\n");
		exit(1);
	}

	// if(ENABLE_HEART_BEAT)
	// {
	// 	if(pthread_create(&heart_beat_thread_ID, NULL, heart_beat_thread,NULL))
	// 	{
	// 		fprintf(stderr, "Error creating heart-beat thread\n");
	// 		exit(1);
	// 	}
	// }

	// if(PRINT_LOG_CONT)
	// {
	// 	if(pthread_create(&log_printing_thread_ID, NULL,log_printing_thread,NULL))
	// 	{
	// 		fprintf(stderr, "Error log printing thread\n");
	// 		exit(1);
	// 	}
	// }

	// if(AUTO_POLICY)
	// {
	// 	if(pthread_create(&auto_policy_thread_id, NULL, auto_fair_policy_thread, NULL))
	// 	{
	// 		fprintf(stderr, "Error auto policy thread\n");
	// 		exit(1);
	// 	}
	// }


	printf("SERVER: PID: %d \n", getpid());

	while(1)
	{
		printf("\nSERVER: \n------------------------------------------------\n");
		printf("[1] - Resize \n");
		printf("[2] - Characterisze resize operation \n" );
		printf("[3] - Change category allocation ratio\n");
		// printf("[4] - Get Hit ratio\n");
		printf("[5] - Show status\n");
		printf("[6] - Remove non-persistent blocks (Heart beat)\n");
		printf("[7] - Swap out persistent blocks \n");
		printf("[0] - Exit\n" );

		printf("\nEnter option : \n");

		scanf("%d", &option);

		clearScreen();
		switch(option)
		{
			case 1:
				resize_fn(cm);
				break;

			case 2:
				benchmark_resize(cm);
				break;

			case 3:
				change_category_allocation_ration(cm); 
				break;

			case 4:
				// get_hit_ratio();
				break;

			case 5:
				show_vm_list(cm);
				break;

			case 6:
				recover_non_persistent_blocks(cm, NULL);
				break;

			case 7:
				swap_out_persistent_blocks(cm, NULL);
				break;

			case 0:
				break;
		}

		if(option==0)
			break;
	}
}
