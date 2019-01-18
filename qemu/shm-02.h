#define NOT_READY  	-1
#define FILLED     	5
#define TAKEN		10
#define READY    	2
#include <stddef.h>
#include <pthread.h>
#include <semaphore.h>
struct Memory {
	pthread_mutex_t lock;
	pthread_mutexattr_t mutexAttr;
	int id;
	int status;
	int msg_type;
	int size;
	int flag;
	int vm_done;
	int size_alloc;
	int size_dealloc;
	int ptr[1000000];
};

