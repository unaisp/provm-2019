/*
 * Virtio Block Device
 *
 * Copyright IBM, Corp. 2007
 *
 * Authors:
 * Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qemu/iov.h"
#include "qemu/error-report.h"
#include "trace.h"
#include "hw/block/block.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"
#include "hw/virtio/virtio-vssd.h"
#include "dataplane/virtio-vssd.h"
#include "block/scsi.h"
#ifdef __linux__
# include <scsi/sg.h>
#endif
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"

#include "sysemu/balloon.h"
#include "hw/virtio/virtio-balloon.h"

#include "qemu/timer.h"
#include "../../../../../server.h"


#define BENCHMARKING 1

unsigned long int backend_resize_duration = 0;
uint32_t resize_request_id = 0; 


/* NEW FUNCTION START */

uint32_t bitmap_offset;
uint32_t bitmap_reading;

static uint64_t l2p(VirtIOVssd *vssd, uint64_t lsn)
{
	//return logical+SSD_BLOCK_SIZE;
	unsigned long int pbn = vssd->block_list[lsn / SSD_BLOCK_SIZE];

	if(pbn < 6 * SSD_BLOCK_SIZE || pbn <= 0)
	{
		printf("\n\n\nBUG \n. lsn: %lu  lbn: %lu  pbn: %lu \n\n\n", lsn, lsn/SSD_BLOCK_SIZE, pbn);
		exit(1);
	}

	return pbn + (lsn % SSD_BLOCK_SIZE);
}

/*static unsigned long long current_time(void)
  {
  unsigned int lo, hi;
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));                        
  return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );  
  }*/


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

static char* get_date(char *buf)
{
	time_t rawtime;
	struct tm * timeinfo;

	time ( &rawtime );
	timeinfo = localtime ( &rawtime );
	sprintf(buf, "%s", asctime(timeinfo));
	buf[strlen(buf)-1] = '\0';
	// strpcy(buf, asctime (timeinfo));

	// free(timeinfo);
	return buf;
}

static unsigned long long int min(unsigned long long int n1, unsigned long long int n2)
{
	if(n1 < n2)
		return n1;
	else
		return n2;
}

static unsigned long long int gb_to_sectors(int size_in_gb)
{
	return (unsigned long long int) size_in_gb * 2097152;

	//	1GB = 2097152 sectors
}

static unsigned long long int gb_to_vssd_blocks(int size_in_gb)
{
	return (unsigned long long int) (gb_to_sectors(size_in_gb) / SSD_BLOCK_SIZE);

	//	1GB = 2097152 sectors
}



static void get_access_to_shared_memory(struct SharedMemory *message)
{
	int ret;
	pthread_mutex_lock(&message->lock);
	while(message->msg_type != MSG_TYPE_BUF_FREE)
	{
		ret = pthread_mutex_trylock(&message->lock);
		if(ret == 0)
		{
			printf("\n\nACCESS: Releasing shared memory: It should not happen\n\n\n\n\n\n");
			exit(1);
		}

		// printf("ACCESS: \t\tWaiting for buffer \n");
		pthread_cond_wait(&message->vm_can_enter, &message->lock);
		// printf("ACCESS: \t\tWaking up");
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
}

static void wait_for_reply(struct SharedMemory *message, struct VirtIOVssd *vssd, int msg_type)
{
	int ret;
	while(!(message->destination == vssd->vm_id && 
				message->source == CM_LISTENER_ID && 
				message->msg_type == msg_type))
	{
		// printf("%llu: WAIT_FOR_REPLY: Dest=%d. Src=%d. MsgType=%d \n", current_time(), 
			// message->destination, message->source, message->msg_type);
		ret = pthread_mutex_trylock(&message->lock);
		if(ret == 0)
		{
			printf("\n\n\nWAIT_FOR_REPLY: BUG. Try lock succeeded \n\n\n");
			exit(1);
		}

		// printf("%llu: WAIT_FOR_REPLY: Going to sleep. Will release the lock \n", current_time());
		pthread_cond_wait(&message->vm_can_enter, &message->lock);
		// printf("%llu: WAIT_FOR_REPLY: I woke up. I have the lock\n", current_time());
	}
}

static void lock_and_wait_for_reply(struct SharedMemory *message, struct VirtIOVssd *vssd, int msg_type)
{
	pthread_mutex_lock(&message->lock);
	wait_for_reply(message, vssd, msg_type);
}

static void send_message(struct SharedMemory *message, struct VirtIOVssd *vssd, int msg_type)
{
	message->msg_type = msg_type;
	message->source = vssd->vm_id;
	message->destination = CM_LISTENER_ID; 

	pthread_cond_broadcast(&message->cm_can_enter);
	// printf("VSSD: \t\tInformed the manager \n");
}



static void virtio_vssd_handle_resize_callback(VirtIODevice *vdev, VirtQueue *vq)
{
	#if BENCHMARKING
	 	unsigned long int start_time = current_time();
	 #endif

	struct iovec *iov;
	struct SharedMemory *message;
	uint32_t in_num;
	uint64_t logical_block_num=0;
	unsigned long int i;
	VirtIOVssd *vssd;
	VirtIOVssdResizeInfo resize_info;
	VirtQueueElement *elem;
	int flag;

	printf("\nVSSD: virtio_vssd_handle_resize_callback   called\n");

	vssd = (VirtIOVssd *)vdev;
	message = (struct SharedMemory*)vssd->message;

	while((elem = virtqueue_pop(vq, sizeof(VirtQueueElement))) != NULL) 
	{
		iov = elem->in_sg;
		in_num = elem->in_num;
		iov_to_buf(iov, in_num, 0, &resize_info, sizeof(resize_info));

		if(resize_info.operation == SSD_BALLOON_INFLATION)
		{

			printf("VSSD: \tSSD_BALLOON_INFLATION \n");
			printf("VSSD: \t\tRequested blocks: %d. Number of blocks given by front-end: %d. Command=%ld \n", 
					resize_info.current_requested_blocks, resize_info.given_blocks, vssd->command);

			int total_send_to_cm = 0;

			do
			{
				// printf("VSSD: \t\tLoop \n");
				get_access_to_shared_memory(message);
				inflation_reply *reply = (inflation_reply *)message->msg_content;

				int j=0;
				for(i=total_send_to_cm; i<resize_info.given_blocks && j<max_transfer_inflation; i++, j++) 
				{
					logical_block_num = resize_info.block_list[i];
					unsigned long int pbn = vssd->block_list[logical_block_num] / SSD_BLOCK_SIZE;
					reply->block_list[j] = pbn;
					vssd->block_list[logical_block_num] = -1;
					// printf("%ld [%ld-%ld]\t", i, logical_block_num, pbn);
					printf("[%ld - %ld]\t", logical_block_num, pbn);
				}

				reply->vm_id = vssd->vm_id;
				reply->count = j;
				total_send_to_cm += j;
				if(total_send_to_cm < resize_info.given_blocks)
					reply->flag = FLAG_ASK_ME_AGAIN;
				else
					reply->flag = resize_info.flag;
				
				send_message(message, vssd, MSG_TYPE_INFLATION_REPLY);
				pthread_mutex_unlock(&message->lock);
			}
			while(total_send_to_cm < resize_info.given_blocks);

			// todo  Atomicatlly update the command variable
			vssd->current_capacity -= resize_info.given_blocks*SSD_BLOCK_SIZE;

			if(resize_info.flag == FLAG_DO_NOT_ASK_ME && vssd->command == -1*resize_info.current_requested_blocks)
				vssd->command = 0;
			else
				vssd->command += resize_info.given_blocks;

			printf("VSSD: \t\tBlocks transfered to CM: %d. Current-capacity: %lu. Command: %ld. Flag: %s \n",
					total_send_to_cm, vssd->current_capacity/SSD_BLOCK_SIZE, vssd->command,
					resize_info.flag == FLAG_DO_NOT_ASK_ME? "FLAG_DO_NOT_ASK_ME": "FLAG_ASK_ME_AGAIN");

			iov_from_buf(iov, in_num, 0, &resize_info, sizeof(resize_info));
			virtqueue_push(vq, elem, sizeof(resize_info));
			virtio_notify(vdev, vq);

			// printf("virtio_notify ........... after inflation ........................... \n");

		}
		else if(resize_info.operation == SSD_BALLOON_DEFLATION)
		{

			// int max_transfer  = (BUF_SIZE_IN_BYTES-(3*sizeof(int)))/sizeof(struct physical_logical_block);

			printf("VSSD: \t\tSSD_BALLOON_DEFLATION \n");
			printf("VSSD: \t\t\tFront-end-requested-size: %d. command: %ld \n", 
					resize_info.current_requested_blocks,
					vssd->command);

			get_access_to_shared_memory(message);

			DeflationReq *deflation_req = (DeflationReq *)message->msg_content;
			deflation_req->vm_id = vssd->vm_id;
			deflation_req->requested_size = min(resize_info.current_requested_blocks, SSD_BALLOON_UNIT);

			printf("VSSD: \t\t\tDeflation request \n");

			printf("VSSD: \t\t\t\tRequested size = %ld \n", deflation_req->requested_size);

			send_message(message, vssd, MSG_TYPE_DEFLATION_REQ);
			wait_for_reply(message, vssd, MSG_TYPE_DEFLATION_BLK_ALCN);

			DeflationBlockAllocation *delfation_alcn = (DeflationBlockAllocation *)message->msg_content;
			printf("VSSD: \t\t\tDeflation block allocation \n");
			printf("VSSD: \t\t\t\tNumber of blocks received from CM: %d \n", delfation_alcn->count);

			unsigned long j=0, lbn=0;
			for(lbn=0, j=0; lbn<vssd->capacity/SSD_BLOCK_SIZE && j<delfation_alcn->count;  lbn++)
			{
				if(vssd->block_list[lbn] == -1)
				{
					unsigned long int pbn = delfation_alcn->block_list[j].physical_block;
					vssd->block_list[lbn] = pbn * SSD_BLOCK_SIZE;
					resize_info.block_list[j] = lbn;
					delfation_alcn->block_list[j].logical_block = lbn;

					j++;

					// printf(" %ld[%ld-%ld]   ", j, lbn, pbn);
					printf(" [%ld - %ld]   ",lbn, pbn);
				}
			}
			flag = delfation_alcn->flag;
			resize_info.flag = delfation_alcn->flag;
			resize_info.given_blocks = j;

			// Send deflation block allocation reply
			send_message(message, vssd, MSG_TYPE_DEFLATION_BLK_ALCN_REPLY);
			pthread_mutex_unlock(&message->lock);

			vssd->current_capacity += resize_info.given_blocks*SSD_BLOCK_SIZE;
			if(flag == FLAG_DO_NOT_ASK_ME && vssd->command == resize_info.current_requested_blocks)
				vssd->command = 0;
			else
				vssd->command -= resize_info.given_blocks;

			printf("VSSD: \t\t\tFront-end-requested-size: %d. Given = %d. Command=%ld. Current-capacity: %lu \n", 
					resize_info.current_requested_blocks,
					resize_info.given_blocks,
					vssd->command,
					vssd->current_capacity/SSD_BLOCK_SIZE);

			iov_from_buf(iov, in_num, 0, &resize_info, sizeof(resize_info));
			virtqueue_push(vq, elem, sizeof(resize_info));
			virtio_notify(vdev, vq);
		}
		else if(resize_info.operation == SSD_DM_CACHE_STATUS)
		{
			// int array[100];
			printf("Message recevied from front end. \n");

			get_access_to_shared_memory(message);

			struct status_msg *status_msg = (struct status_msg *)message->msg_content;
			status_msg->vm_id = vssd->vm_id;
			status_msg->size = resize_info.block_list[0] / sizeof(long int);
			memcpy(status_msg->data, resize_info.block_list + 1, status_msg->size * sizeof(long int));
			
			// printf("Size of message: %d. %ld %ld %ld %ld \n", 
			// 	status_msg->size,
			// 	status_msg->data[0],
			// 	status_msg->data[1],
			// 	status_msg->data[2],
			// 	status_msg->data[3]);

			send_message(message, vssd, MSG_TYPE_STATUS);
			pthread_mutex_unlock(&message->lock);

			iov_from_buf(iov, in_num, 0, &resize_info, sizeof(resize_info));
			virtqueue_push(vq, elem, sizeof(resize_info));
			virtio_notify(vdev, vq);

			printf("Status message: done. \n");
		}
		else
		{
			printf("VSSD-ERROR: \n");
		}

		#if BENCHMARKING
		 	unsigned long int end_time = current_time();
		 	backend_resize_duration += end_time - start_time;
		 #endif

		// if(vssd->command != 0)
		// {
		// 	virtio_notify_config(vdev);			
		// 	printf("Command != 0. Notify front endfor %ld blocks \n", vssd->command);
		// }
	}

}


static void virtio_ssd_balloon_to_target(void *opaque, int64_t target)
{
	VirtIOVssd *vssd = VIRTIO_VSSD(opaque);
	VirtIODevice *vdev = VIRTIO_DEVICE(vssd);

	vssd->command += target; // We add to ensure that the direction is preserved.

	printf("VSSD: virtio_ssd_balloon_to_target \n");
	printf("VSSD: \tTarget size : %ld \n", target);

	virtio_notify_config(vdev);
}

static struct SharedMemory* get_shared_memory(void)
{
	int ShmID;
	key_t ShmKEY;
	struct SharedMemory *ptr;

	/*Allocating shared memory segment*/
	printf("VSSD: Setup shared memory \n");
	ShmKEY = ftok("/tmp", 's');     
	ShmID = shmget(ShmKEY, sizeof(struct SharedMemory), 0666); 
	if (ShmID < 0) {
		printf("VSSD-ERROR: shmget error \n");
		return NULL;
	}
	printf("VSSD: \tKey = [%x] \n", ShmID);  

	ptr = (struct SharedMemory *) shmat(ShmID, NULL, 0);
	if (!ptr)
	{
		printf("VSSD-ERROR: shmat failed. \n");
		return NULL;
	} 
	printf("VSSD: \tAttached the shared memory\n");
	printf("VSSD: \tVirtual Address of the shared memory is : %p \n", ptr);

	return ptr;
}


static void resize_fn(VirtIOVssd *vssd)
{
	#if BENCHMARKING
		static int resize_id = 0;
		resize_id++;

		if(resize_id != 1)
		{
			printf("LOG: Resize id:\t%d\t Time Taken:\t%lu\t\n", resize_id-1, backend_resize_duration);
			backend_resize_duration = 0;
		}
	 	unsigned long int start_time = current_time();
	#endif

	struct SharedMemory *message;
	struct resize_req *req;

	message = (struct SharedMemory *)vssd->message;
	req = (struct resize_req *)message->msg_content;

	// todo Update the command atomically 
	// atomic {
	printf("VSSD: Resize \n");
	printf("VSSD: \tResize size: %ld blocks. current-command: %ld \n", req->size, vssd->command);

	vssd->command += req->size;
	if(vssd->command == req->size)
	{
		#if BENCHMARKING
	 		unsigned long int end_time = current_time();
	 		backend_resize_duration += end_time - start_time;
	 		resize_request_id++;
		#endif

		printf("VSSD: \tNotifying front end for %ld blocks \n", vssd->command);
		virtio_notify_config(VIRTIO_DEVICE(vssd));
		printf("VSSD: \tNotified front end \n");
	}
	else
	{
		printf("VSSD: No need to notify the front-end. One operation is also going \n");
	}
	release_shared_memory(message, __LINE__);

	// atomic }
}

static void* backend_listner_thread(void *arg)
{
	VirtIOVssd *vssd = (VirtIOVssd *)arg;
	struct SharedMemory *message = (struct SharedMemory *)vssd->message; 

	int ret;

	
	while(1)
	{	
		// printf("%llu: LISTENER: Aquiring lock \n", current_time());
		pthread_mutex_lock(&message->lock);
		// printf("%llu: LISTENER: I got the lock \n", current_time());


		while(!(message->destination == vssd->vm_id 
					&& message->msg_type != MSG_TYPE_BUF_FREE 
					&& message->msg_type == MSG_TYPE_RESIZE_REQ))
		{
			ret = pthread_mutex_trylock(&message->lock);
			if(ret == 0)
			{
				printf("\n\nLISTNER: BUG: It should not happen\n\n\n\n\n\n");
				exit(1);
			}

			// printf("%llu: LISTENER: Message is not for me. I am going sleep. will release the lock \n", current_time());
			pthread_cond_wait(&message->vm_can_enter, &message->lock);
			// printf("%llu: LISTENER: I woke up, I have the lock \n", current_time());
			
			// printf("%llu: LISTNER: Dest=%d. Source=%d.  Type=%d \n", current_time(), 
				// message->destination, message->source, message->msg_type);
		}

		// printf("%llu: LISTENER: Message received. Type: %d \n", current_time(), message->msg_type);

		switch(message->msg_type)
		{
			case MSG_TYPE_RESIZE_REQ:
				resize_fn(vssd);
				break;
		}
	}

	return NULL;

}

static void setup_log_file(struct VirtIOVssd *vssd)
{
	char file_name[100];
	char temp_buf[100];

	sprintf(file_name,"/home/unaisp/ssd_sdb1/provm/benchmark/log/%s_%s.log",vssd->vm_name, get_date(temp_buf));
	printf("VSSD: Creating log file. name = [%s] \n", file_name);
	FILE *filep = fopen(file_name, "a");
	if(!filep)
	{
		printf("VSSD-ERROR: Unable to open file %s \n", file_name);
		exit(1);
	}
	vssd->log_file = filep;

	sprintf(file_name,"/home/unaisp/ssd_sdb1/provm/benchmark/log/%s_%s_summary.log",vssd->vm_name, get_date(temp_buf));
	printf("VSSD: Creating log summmary file. name = [%s] \n", file_name);
	vssd->summary_file = fopen(file_name, "a");
	if(!vssd->summary_file)
	{
		printf("VSSD-ERROR: Unable to open file %s \n", file_name);
		exit(1);
	}



}
static void write_to_log(struct VirtIOVssdReq *req)
{

	if(!VSSD_DEBUG_MODE)
		return;

	pthread_mutex_lock(&req->dev->lock);

	struct statistics *s = &req->stats;

	static unsigned long long base2 = 0;
	if(base2 == 0)
		base2 = s->request_pull_time;

	unsigned long long base = s->request_pull_time;
	static unsigned long long count = 0;

	fprintf(req->dev->log_file, 
			"%llu\t" "%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t" "%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",
			++count,
			s->qiov_size,
			s->qiov_iov_count,

			//  After merging multiple requests
			s->qiov_merge_count,
			s->qiov_mereg_size,
			s->qiov_merg_iov_count,

			//  After dividing merged requests
			s->sub_qiov_count,

			s->request_pull_time,              //  First event
			s->adding_to_mrb_time - base,
			s->callback_time - base,                  //  Time at which call back for last sub-qiov received
			s->request_push_time - base,              //  Final event

			s->qiov_merging_start_time - base,
			s->qiov_division_start_time - base,       //  Merging end time
			s->first_sub_qiov_send_time - base,
			s->last_sub_qiov_send_time - base,        //  Division end time
			s->first_sub_qiov_callback_time - base,
			s->last_sub_qiov_callback_time - base    //  Request call back time
				);

	static unsigned long long exp_count=0; 
	static unsigned long long last_request_pull_time = 0;
	static unsigned long long last_request_push_time = 0;
	static unsigned long long requests_count = 0;
	//	Number of requests per GB = requests_count/40
	static unsigned long long sum_qiov_size = 0;
	//	Qiov size per GB = sum_qiov_size / 40
	//	Average qiov size = sum_qiov_size / requests count
	static unsigned long long qiov_vectors_count = 0;
	//	Number of qiov vectors per GB = qiov_vectors_ount/40
	//	Average number of qiov-vectors per qiov = qiov_vectors_count / requests_count
	static unsigned long long merging_count = 0;
	static unsigned long long merged_qiov_count = 0;
	//	Average merege size = merged_qiov_count / merging_count
	static unsigned long long after_merging_qiov_count = 0;		// merging_count + (requests_count-merged_qiov_count)
	static unsigned long long boundary_crossing_qiov_count = 0;		//After merging
	static unsigned long long sub_qiov_count = 0;
	//	Average partition size = sub_qiov_count / boundary_crossing_qiov_count
	static unsigned long long pull_send_time_sum = 0;			//	Divide by request count to get average
	static unsigned long long send_callback_time_sum = 0;		//	Divide by sub_qiov_count to get average
	static unsigned long long callback_push_time_sum = 0;		//	Divide by request count to get average
	static unsigned long long pull_push_time_sum = 0;
	static unsigned long long merging_time_sum = 0;
	static unsigned long long merge_send_time = 0;				//	after_merging_qiov_count

	static unsigned long long first_request_time = 0 ;

	// printf("Count = %llu. Delay = %llu \n", requests_count, (s->request_pull_time - last_request_pull_time)/1000000);

	unsigned long long delay = (s->request_pull_time > last_request_pull_time ) ? (s->request_pull_time - last_request_pull_time)/1000000 : 0;
	// 	printf("Pull-time=%llu. last-rqst-pull-time=%llu. delay = %llu. %llu \n "
	// ,		s->request_pull_time-base2, last_request_pull_time-base2, (s->request_pull_time - last_request_pull_time)/1000000, delay);

	if(delay > 10)
	{
		double duration = (last_request_push_time-first_request_time) > 0 ? (double)((last_request_push_time-first_request_time))/1000000 : 1;

		last_request_pull_time = s->request_pull_time;

		if(requests_count == 0)
			requests_count = 1;

		// printf("K. [%llu]  [%llu]. %f \n", after_merging_qiov_count, boundary_crossing_qiov_count, duration );

		// fprintf(req->dev->summary_file,
		printf(
				"%llu\t%4.2f\t%f\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n\n",
				exp_count,
				(double)(sum_qiov_size/(1024*1024))/duration,
				duration,
				requests_count,
				requests_count/40,
				(unsigned long long)sum_qiov_size/(1024*1024),
				sum_qiov_size/(1024*1024*40),
				sum_qiov_size/requests_count,
				qiov_vectors_count,
				qiov_vectors_count/40,
				qiov_vectors_count/requests_count,
				merging_count,
				merged_qiov_count,
				(merged_qiov_count == 0) ? 0 : merging_count/merged_qiov_count,
				(merged_qiov_count == 0) ? 0 : requests_count - merged_qiov_count,
				after_merging_qiov_count,
				boundary_crossing_qiov_count,
				sub_qiov_count,
				(boundary_crossing_qiov_count == 0) ? 0 : sub_qiov_count/boundary_crossing_qiov_count,

				(after_merging_qiov_count == 0) ? 0 : pull_send_time_sum/after_merging_qiov_count,		//	avg pull-send time
				(after_merging_qiov_count == 0) ? 0 : send_callback_time_sum/after_merging_qiov_count,	//	avg send-callback time
				callback_push_time_sum/requests_count,				//	avg callback-pull push time 	CHECK
				pull_push_time_sum/requests_count,					//	avg pull-push time

				merging_time_sum,			//	merging time per request
				merge_send_time			//	merge-send time
					);

		/*fprintf(req->dev->summary_file,
		  "%llu\t%4.2f\t%f\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n\n",
		  exp_count,
		  (double)(sum_qiov_size/(1024*1024))/duration,
		  duration,
		  requests_count,
		  requests_count/40,
		  (unsigned long long)sum_qiov_size/(1024*1024),
		  sum_qiov_size/(1024*1024*40),
		  sum_qiov_size/requests_count,
		  qiov_vectors_count,
		  qiov_vectors_count/40,
		  qiov_vectors_count/requests_count,
		  merging_count,
		  merged_qiov_count,
		  merging_count/merged_qiov_count,
		  requests_count - merged_qiov_count,
		  after_merging_qiov_count,
		  boundary_crossing_qiov_count,
		  sub_qiov_count,
		  sub_qiov_count/boundary_crossing_qiov_count,

		  pull_send_time_sum/after_merging_qiov_count,
		  send_callback_time_sum/after_merging_qiov_count,
		  callback_push_time_sum/after_merging_qiov_count,
		  pull_push_time_sum/requests_count,
		  merging_time_sum/after_merging_qiov_count,
		  merge_send_time/after_merging_qiov_count,

		  send_callback_time_sum/after_merging_qiov_count,
		  merge_send_time/after_merging_qiov_count
		  );*/



		first_request_time = s->request_pull_time;

		exp_count++; 
		requests_count = 0;
		//	Number of requests per GB = requests_count/40
		sum_qiov_size = 0;
		//	Qiov size per GB = sum_qiov_size / 40
		//	Average qiov size = sum_qiov_size / requests count
		qiov_vectors_count = 0;
		//	Number of qiov vectors per GB = qiov_vectors_ount/40
		//	Average number of qiov-vectors per qiov = qiov_vectors_count / requests_count
		merging_count = 0;
		merged_qiov_count = 0;
		//	Average merege size = merged_qiov_count / merging_count
		after_merging_qiov_count = 0;		// merging_count + (requests_count-merged_qiov_count)
		boundary_crossing_qiov_count = 0;		//After merging
		sub_qiov_count = 0;
		//	Average partition size = sub_qiov_count / boundary_crossing_qiov_count
		pull_send_time_sum = 0;			//	Divide by request count to get average
		send_callback_time_sum = 0;		//	Divide by sub_qiov_count to get average
		callback_push_time_sum = 0;		//	Divide by request count to get average
		pull_push_time_sum = 0;
		merging_time_sum = 0;
		merge_send_time = 0;				//	after_merging_qiov_count
	}

	if(s->request_pull_time > last_request_pull_time) 	
		last_request_pull_time = s->request_pull_time;
	if(s->request_pull_time < first_request_time)
		first_request_time = s->request_pull_time;

	if(s->request_push_time > last_request_push_time)
		last_request_push_time = s->request_push_time;

	requests_count++;
	sum_qiov_size += s->qiov_size;
	qiov_vectors_count += s->qiov_iov_count;

	merging_count += (s->qiov_merge_count>1) ? 1 : 0;
	merged_qiov_count += (s->qiov_merge_count>1) ? s->qiov_merge_count : 0;

	// after_merging_qiov_count = merging_count + (requests_count-merged_qiov_count);
	after_merging_qiov_count += s->qiov_merge_count;

	boundary_crossing_qiov_count += (s->sub_qiov_count>1) ? 1 : 0;
	sub_qiov_count += (s->sub_qiov_count>1) ? s->sub_qiov_count : 0;

	pull_send_time_sum += (s->first_sub_qiov_send_time > 0) ? s->first_sub_qiov_send_time - s->request_pull_time : 0;
	send_callback_time_sum += (s->first_sub_qiov_send_time > 0) ? s->callback_time - s->first_sub_qiov_send_time : 0;
	callback_push_time_sum += s->request_push_time - s->callback_time ;
	pull_push_time_sum += s->request_push_time - s->request_pull_time;

	merging_time_sum += s->qiov_division_start_time - s->qiov_merging_start_time;
	merge_send_time += s->last_sub_qiov_send_time - s->qiov_division_start_time; 

	pthread_mutex_unlock(&req->dev->lock);

}

static int virtio_setup_vssd(struct VirtIOVssd *vssd)
{
	// Registering ballong handler
	int flag;
	printf("VSSD: Setup resize  and manager communcation \n");
	VirtIODevice *vdev = VIRTIO_DEVICE(vssd);
	vssd->ctrl_vq = virtio_add_queue(vdev, 1024, virtio_vssd_handle_resize_callback);

	qemu_add_ssd_balloon_handler(virtio_ssd_balloon_to_target, vssd);

	uint64_t capacity_in_blocks = vssd->capacity/SSD_BLOCK_SIZE;
	vssd->block_list = (uint64_t *)malloc(sizeof(uint64_t) * capacity_in_blocks);

	for(unsigned long int i=0; i < capacity_in_blocks; i++)
		vssd->block_list[i] = -1;

	vssd->vm_id = vssd_vm_id;				//	Read from global variables
	strcpy(vssd->vm_name, vssd_vm_name);		//	Read from global variables

	setup_log_file(vssd);

	// Communicate with manager.
	struct SharedMemory * message = get_shared_memory();
	if(!message)
		return -1;
	printf("VSSD: \tShared memory ready !!!\n");
	vssd->message = (void *)message;

	printf("VSSD: \tRegistration \n");
	get_access_to_shared_memory(message);
	printf("VSSD:\t\tGot the access to SharedMemory \n");

	printf("VSSD: \t\tSize: %d GB. Allocate: %d GB. Persist: %d GB \n", vssd_size, vssd_current_allocate, vssd_current_persist);
	printf("VSSD: \t\tSize: %llu sectors. Allocate: %llu sectors. Persist: %llu sectors \n", 
			gb_to_sectors(vssd_size), gb_to_sectors(vssd_current_allocate), gb_to_sectors(vssd_current_persist));


	struct vm_registration_req *req = (struct vm_registration_req *)message->msg_content;
	req->vm_id = vssd->vm_id;
	req->capacity = vssd->capacity / SSD_BLOCK_SIZE;		
	req->current_allocate = gb_to_vssd_blocks(vssd_current_allocate);		
	req->current_persist =  gb_to_vssd_blocks(vssd_current_persist);
	req->persist_full = vssd_persist_full;
	req->pid = getpid();
	printf("VSSD: \t\tSize: %lu blocks. Allocate: %lu blocks. Persist: %lu blocks \n", 
			req->capacity, req->current_allocate, req->current_persist);

	strcpy(req->vm_name, vssd->vm_name);
	printf("VSSD: \t\tRegistration request ready \n");

	send_message(message, vssd, MSG_TYPE_VM_REG);
	wait_for_reply(message, vssd, MSG_TYPE_VM_REG_REPLY);
	printf("VSSD: \tRegistration reply received \n");

	struct vm_registration_reply *reply = (struct vm_registration_reply *)message->msg_content;
	if(reply->error)
	{
		printf("VSSD-ERROR: Registration failed \n");
		printf("VSSD-ERROR: Exiting the virtual machine \n");

		release_shared_memory(message, __LINE__);
		exit(1);
	}

	printf("VSSD: \t\tOld-blocks: %lu. Old-persistent-blocks: %lu. New-reserved-blocks: %ld. New-reserved-persistent-blocks: %ld. \n",
			reply->old_blocks, reply->old_persistent_blocks, reply->reserved_blocks, reply->reserved_blocks_persistent);
	bool first_time_registration = reply->first_time_registration;


	release_shared_memory(message, __LINE__);

	if(first_time_registration)
	{
		int round = 0;
		unsigned long long lbn=0;
		do 
		{
			lock_and_wait_for_reply(message, vssd, MSG_TYPE_PHYSICAL_BLOCK_ALCN);
			struct phsical_block_allocation *alcn = (struct phsical_block_allocation *)&message->msg_content;

			for(unsigned long int j=0; j<alcn->count; j++, lbn++)		//	lbn < vssd capacity todo
			{
				vssd->block_list[lbn] = alcn->block_list[j].physical_block * SSD_BLOCK_SIZE;
				alcn->block_list[j].logical_block = lbn;

					// printf("lbn: %llu. pbn: %lu \n", lbn, vssd->block_list[lbn] / SSD_BLOCK_SIZE);
			}
			vssd->current_capacity += alcn->count * SSD_BLOCK_SIZE;
			flag = alcn->flag;

			printf("VSSD: \t\tRound: %d. Type: %s. Received: %d. Current size: %lu \n", 
					++round, 
					(alcn->allocation_type == 2) ? "NEW-PERSISTENT": "NEW-NON-PERSISTENT",
					alcn->count, 
					vssd->current_capacity/SSD_BLOCK_SIZE);

			//	Sending logical block numbers of given physical blocks
			// phsical_block_allocation_reply *alcn_reply = (phsical_block_allocation_reply *)alloc;

			send_message(message, vssd, MSG_TYPE_PHYSICAL_BLOCK_ALCN_REPLY);
			pthread_mutex_unlock(&message->lock);
		}
		while(flag == FLAG_ASK_ME_AGAIN);
	}
	else
	{
		unsigned long long lbn=0;

		// Receiving old persistent blocks
		int round = 0;

		do
		{
			lock_and_wait_for_reply(message, vssd, MSG_TYPE_PHYSICAL_BLOCK_ALCN);
			struct phsical_block_allocation *alcn = (struct phsical_block_allocation *)&message->msg_content;

			if(alcn->allocation_type == 1)
			{
				// Old blocks
				for(unsigned long int j=0; j<alcn->count; j++)		
				{
					vssd->block_list[alcn->block_list[j].logical_block] = alcn->block_list[j].physical_block * SSD_BLOCK_SIZE;
				}

				vssd->current_capacity += alcn->count * SSD_BLOCK_SIZE;
				flag = alcn->flag;

				printf("VSSD: \t\tRound: %d. Type: OLD. Received: %d. Current size: %lu \n", 
						++round, alcn->count, vssd->current_capacity/SSD_BLOCK_SIZE);

				release_shared_memory(message, __LINE__);
			}
			else if(alcn->allocation_type == 2 || alcn->allocation_type == 3)
			{
				//	New Persistent block or New non-persistent block

				lock_and_wait_for_reply(message, vssd, MSG_TYPE_PHYSICAL_BLOCK_ALCN);
				struct phsical_block_allocation *alcn = (struct phsical_block_allocation *)&message->msg_content;

				for(unsigned long int j=0; j<alcn->count && lbn<vssd->capacity/SSD_BLOCK_SIZE; lbn++)		//	lbn < vssd capacity todo
				{
					if(vssd->block_list[lbn] == -1)
					{
						vssd->block_list[lbn] = alcn->block_list[j].physical_block * SSD_BLOCK_SIZE;
						alcn->block_list[j].logical_block = lbn;

						j++;
					}
				}
				vssd->current_capacity += alcn->count * SSD_BLOCK_SIZE;
				flag = alcn->flag;

				printf("VSSD: \t\tRound: %d. Type: %s. Received: %d. Current size: %lu \n", 
						++round, 
						(alcn->allocation_type == 2) ? "NEW-PERSISTENT": "NEW-NON-PERSISTENT",
						alcn->count, 
						vssd->current_capacity/SSD_BLOCK_SIZE);

				//	Sending logical block numbers of given physical blocks
				// phsical_block_allocation_reply *alcn_reply = (phsical_block_allocation_reply *)alloc;

				send_message(message, vssd, MSG_TYPE_PHYSICAL_BLOCK_ALCN_REPLY);
				pthread_mutex_unlock(&message->lock);
			}
		}
		while(flag == FLAG_ASK_ME_AGAIN);

		//	Receiving new persistent/non-persistent blocks

	}






	/*for(unsigned long int i=0;  i < reply->given;  i++)
	  {
	// if(i<20)
	// 	printf("%ld[%ld]  ",i, reply->block_list[i]);
	vssd->block_list[i] = reply->block_list[i] * SSD_BLOCK_SIZE;
	}
	vssd->current_capacity = reply->given * SSD_BLOCK_SIZE;

	int flag = reply->flag;
	int round = 1;
	while(flag == FLAG_ASK_ME_AGAIN && vssd->current_capacity < vssd->capacity)
	{
	printf("VSSD: \tNeed more blocks. round:%d \n", ++round);
	struct allocation_req *alloc_req = (struct allocation_req *)message->msg_content;
	alloc_req->vm_id = vssd->vm_id;
	alloc_req->requested_size = (vssd->capacity-vssd->current_capacity) / SSD_BLOCK_SIZE;
	printf("VSSD: \t\tRequest size = %ld blocks \n", alloc_req->requested_size);

	send_message(message, vssd, MSG_TYPE_ALLOCATION_REQ);
	wait_for_reply(message, vssd, MSG_TYPE_ALLOCATION_REPLY);

	allocation_reply *alloc_reply = (allocation_reply *)message->msg_content;
	printf("VSSD: \tAllocation reply received \n");
	printf("VSSD: \t\tRequest:%ld given:%ld \n", alloc_reply->requested_size, alloc_reply->given);

	unsigned last_filled = vssd->current_capacity/SSD_BLOCK_SIZE;
	for(unsigned long int i=0;  i < alloc_reply->given;  i++)
	{
	// if(i<20)
	// 	printf("%ld[%ld] ", i+last_filled, alloc_reply->block_list[i]);
	vssd->block_list[i+last_filled] = alloc_reply->block_list[i] * SSD_BLOCK_SIZE;
	}
	vssd->current_capacity += alloc_reply->given * SSD_BLOCK_SIZE;

	flag = alloc_reply->flag;		
	}

	release_shared_memory(message);*/

	if(pthread_create(&vssd->listener_thread_ID, NULL, backend_listner_thread, vssd))
	{
		fprintf(stderr, "Error creating thread\n");
		exit(1);
	}

	printf("VSSD: \tRegistration completed \n");

	/*DEBUG START*/
	pthread_mutex_init(&vssd->lock, NULL);
	printf("VSSD: \tLock initialized \n");
	/*DEBUG END*/
	return 1;
}

/*NEW FUNCTIONS END*/



static void virtio_vssd_init_request(VirtIOVssd *s, VirtQueue *vq,
		VirtIOVssdReq *req)
{
	req->dev = s;
	req->vq = vq;
	req->qiov.size = 0;
	req->in_len = 0;
	req->next = NULL;
	req->mr_next = NULL;
}

static void virtio_vssd_free_request(VirtIOVssdReq *req)
{
	if (req) {
		g_free(req);
	}
}

static void virtio_vssd_req_complete(VirtIOVssdReq *req, unsigned char status)
{
	VirtIOVssd *s = req->dev;
	VirtIODevice *vdev = VIRTIO_DEVICE(s);

	trace_virtio_blk_req_complete(req, status);

	stb_p(&req->in->status, status);
	virtqueue_push(req->vq, &req->elem, req->in_len);

	/*Unais DEBUG start*/
	if(VSSD_DEBUG_MODE)
	{
		req->stats.request_push_time = current_time();
		write_to_log(req);

	}
	/*Unais DEBUG end*/

	if (s->dataplane_started && !s->dataplane_disabled) {
		virtio_vssd_data_plane_notify(s->dataplane, req->vq);
	} else {
		virtio_notify(vdev, req->vq);
	}
}

static int virtio_vssd_handle_rw_error(VirtIOVssdReq *req, int error,
		bool is_read)
{
	BlockErrorAction action = blk_get_error_action(req->dev->blk,
			is_read, error);
	VirtIOVssd *s = req->dev;

	if (action == BLOCK_ERROR_ACTION_STOP) {
		/* Break the link as the next request is going to be parsed from the
		 * ring again. Otherwise we may end up doing a double completion! */
		req->mr_next = NULL;
		req->next = s->rq;
		s->rq = req;
	} else if (action == BLOCK_ERROR_ACTION_REPORT) {
		virtio_vssd_req_complete(req, VIRTIO_VSSD_S_IOERR);
		block_acct_failed(blk_get_stats(s->blk), &req->acct);
		virtio_vssd_free_request(req);
	}

	blk_error_action(s->blk, action, is_read, error);
	return action != BLOCK_ERROR_ACTION_IGNORE;
}

struct new_struct
{
	VirtIOVssdReq *req;
	QEMUIOVector *qiov;		//delete it man
};

static void virtio_vssd_rw_complete(void *opaque, int ret)
{
	struct new_struct *temp = opaque;
	VirtIOVssdReq *next = temp->req;
	VirtIOVssd *s = next->dev;

	/*Unais*/

	int completed = 0;
	int sub_qiov_count = 0;
	pthread_spin_lock(&next->lock);

	/*Unais DEBUG start*/
	if(VSSD_DEBUG_MODE)
	{
		next->stats.last_sub_qiov_callback_time = current_time();
		if(next->stats.first_sub_qiov_callback_time == 0)
			next->stats.first_sub_qiov_callback_time = next->stats.last_sub_qiov_callback_time;

		next->stats.sub_qiov_count = next->sub_qiov_count;
	}
	/*Unais DEBUG end*/


	next->sub_qiov_finished++;
	if(next->sub_qiov_finished == next->sub_qiov_count)
		completed = 1;
	sub_qiov_count = next->sub_qiov_count;

	// printf("VSSD: virtio_vssd_rw_complete \n");
	// printf("VSSD: \tqiov_count=%d finished=%d. \n", next->sub_qiov_count, next->sub_qiov_finished);

	pthread_spin_unlock(&next->lock);

	if(sub_qiov_count > 1)
	{
		qemu_iovec_destroy(temp->qiov);
		g_free(temp->qiov);
	}
	g_free(temp);

	if(completed == 0)
		return;
	/*End*/

	aio_context_acquire(blk_get_aio_context(s->conf.conf.blk));
	while (next) 
	{

		/*Unais DEBUG start*/
		if(VSSD_DEBUG_MODE)
		{
			next->stats.callback_time = current_time();
		}
		/*Unais DEBUG end*/

		VirtIOVssdReq *req = next;
		next = req->mr_next;
		trace_virtio_blk_rw_complete(req, ret);

		// printf("VSSD: \treq->qiov.nalloc = %d \n", req->qiov.nalloc);

		if (req->qiov.nalloc != -1) {

			/* If nalloc is != 1 req->qiov is a local copy of the original
			 * external iovec. It was allocated in submit_merged_requests
			 * to be able to merge requests. */
			// printf("VSSD: \tDestrying requests \n");
			qemu_iovec_destroy(&req->qiov);
		}

		// printf("VSSD: \tret = %d \n", ret);
		if (ret) {
			int p = virtio_ldl_p(VIRTIO_DEVICE(req->dev), &req->out.type);
			bool is_read = !(p & VIRTIO_VSSD_T_OUT);
			/* Note that memory may be dirtied on read failure.  If the
			 * virtio request is not completed here, as is the case for
			 * BLOCK_ERROR_ACTION_STOP, the memory may not be copied
			 * correctly during live migration.  While this is ugly,
			 * it is acceptable because the device is free to write to
			 * the memory until the request is completed (which will
			 * happen on the other side of the migration).
			 */
			if (virtio_vssd_handle_rw_error(req, -ret, is_read)) {
				continue;
			}
		}

		virtio_vssd_req_complete(req, VIRTIO_VSSD_S_OK);
		block_acct_done(blk_get_stats(req->dev->blk), &req->acct);
		virtio_vssd_free_request(req);
	}
	aio_context_release(blk_get_aio_context(s->conf.conf.blk));
}

static void virtio_vssd_flush_complete(void *opaque, int ret)
{
	VirtIOVssdReq *req = opaque;
	VirtIOVssd *s = req->dev;

	aio_context_acquire(blk_get_aio_context(s->conf.conf.blk));
	if (ret) {
		if (virtio_vssd_handle_rw_error(req, -ret, 0)) {
			goto out;
		}
	}

	virtio_vssd_req_complete(req, VIRTIO_VSSD_S_OK);
	block_acct_done(blk_get_stats(req->dev->blk), &req->acct);
	virtio_vssd_free_request(req);

out:
	aio_context_release(blk_get_aio_context(s->conf.conf.blk));
}

#ifdef __linux__

typedef struct {
	VirtIOVssdReq *req;
	struct sg_io_hdr hdr;
} VirtIOBlockIoctlReq;

static void virtio_vssd_ioctl_complete(void *opaque, int status)
{
	VirtIOBlockIoctlReq *ioctl_req = opaque;
	VirtIOVssdReq *req = ioctl_req->req;
	VirtIOVssd *s = req->dev;
	VirtIODevice *vdev = VIRTIO_DEVICE(s);
	struct virtio_scsi_inhdr *scsi;
	struct sg_io_hdr *hdr;

	scsi = (void *)req->elem.in_sg[req->elem.in_num - 2].iov_base;

	if (status) {
		status = VIRTIO_VSSD_S_UNSUPP;
		virtio_stl_p(vdev, &scsi->errors, 255);
		goto out;
	}

	hdr = &ioctl_req->hdr;
	/*
	 * From SCSI-Generic-HOWTO: "Some lower level drivers (e.g. ide-scsi)
	 * clear the masked_status field [hence status gets cleared too, see
	 * block/scsi_ioctl.c] even when a CHECK_CONDITION or COMMAND_TERMINATED
	 * status has occurred.  However they do set DRIVER_SENSE in driver_status
	 * field. Also a (sb_len_wr > 0) indicates there is a sense buffer.
	 */
	if (hdr->status == 0 && hdr->sb_len_wr > 0) {
		hdr->status = CHECK_CONDITION;
	}

	virtio_stl_p(vdev, &scsi->errors,
			hdr->status | (hdr->msg_status << 8) |
			(hdr->host_status << 16) | (hdr->driver_status << 24));
	virtio_stl_p(vdev, &scsi->residual, hdr->resid);
	virtio_stl_p(vdev, &scsi->sense_len, hdr->sb_len_wr);
	virtio_stl_p(vdev, &scsi->data_len, hdr->dxfer_len);

out:
	aio_context_acquire(blk_get_aio_context(s->conf.conf.blk));
	virtio_vssd_req_complete(req, status);
	virtio_vssd_free_request(req);
	aio_context_release(blk_get_aio_context(s->conf.conf.blk));
	g_free(ioctl_req);
}

#endif

static VirtIOVssdReq *virtio_vssd_get_request(VirtIOVssd *s, VirtQueue *vq)
{
	VirtIOVssdReq *req = virtqueue_pop(vq, sizeof(VirtIOVssdReq));

	if (req) {
		virtio_vssd_init_request(s, vq, req);
	}
	return req;
}

static int virtio_vssd_handle_scsi_req(VirtIOVssdReq *req)
{
	int status = VIRTIO_VSSD_S_OK;
	struct virtio_scsi_inhdr *scsi = NULL;
	VirtIODevice *vdev = VIRTIO_DEVICE(req->dev);
	VirtQueueElement *elem = &req->elem;
	VirtIOVssd *blk = req->dev;

#ifdef __linux__
	int i;
	VirtIOBlockIoctlReq *ioctl_req;
	BlockAIOCB *acb;
#endif

	/*
	 * We require at least one output segment each for the virtio_vssd_outhdr
	 * and the SCSI command block.
	 *
	 * We also at least require the virtio_vssd_inhdr, the virtio_scsi_inhdr
	 * and the sense buffer pointer in the input segments.
	 */
	if (elem->out_num < 2 || elem->in_num < 3) {
		status = VIRTIO_VSSD_S_IOERR;
		goto fail;
	}

	/*
	 * The scsi inhdr is placed in the second-to-last input segment, just
	 * before the regular inhdr.
	 */
	scsi = (void *)elem->in_sg[elem->in_num - 2].iov_base;

	if (!blk->conf.scsi) {
		status = VIRTIO_VSSD_S_UNSUPP;
		goto fail;
	}

	/*
	 * No support for bidirection commands yet.
	 */
	if (elem->out_num > 2 && elem->in_num > 3) {
		status = VIRTIO_VSSD_S_UNSUPP;
		goto fail;
	}

#ifdef __linux__
	ioctl_req = g_new0(VirtIOBlockIoctlReq, 1);
	ioctl_req->req = req;
	ioctl_req->hdr.interface_id = 'S';
	ioctl_req->hdr.cmd_len = elem->out_sg[1].iov_len;
	ioctl_req->hdr.cmdp = elem->out_sg[1].iov_base;
	ioctl_req->hdr.dxfer_len = 0;

	if (elem->out_num > 2) {
		/*
		 * If there are more than the minimally required 2 output segments
		 * there is write payload starting from the third iovec.
		 */
		ioctl_req->hdr.dxfer_direction = SG_DXFER_TO_DEV;
		ioctl_req->hdr.iovec_count = elem->out_num - 2;

		for (i = 0; i < ioctl_req->hdr.iovec_count; i++) {
			ioctl_req->hdr.dxfer_len += elem->out_sg[i + 2].iov_len;
		}

		ioctl_req->hdr.dxferp = elem->out_sg + 2;

	} else if (elem->in_num > 3) {
		/*
		 * If we have more than 3 input segments the guest wants to actually
		 * read data.
		 */
		ioctl_req->hdr.dxfer_direction = SG_DXFER_FROM_DEV;
		ioctl_req->hdr.iovec_count = elem->in_num - 3;
		for (i = 0; i < ioctl_req->hdr.iovec_count; i++) {
			ioctl_req->hdr.dxfer_len += elem->in_sg[i].iov_len;
		}

		ioctl_req->hdr.dxferp = elem->in_sg;
	} else {
		/*
		 * Some SCSI commands don't actually transfer any data.
		 */
		ioctl_req->hdr.dxfer_direction = SG_DXFER_NONE;
	}

	ioctl_req->hdr.sbp = elem->in_sg[elem->in_num - 3].iov_base;
	ioctl_req->hdr.mx_sb_len = elem->in_sg[elem->in_num - 3].iov_len;

	acb = blk_aio_ioctl(blk->blk, SG_IO, &ioctl_req->hdr,
			virtio_vssd_ioctl_complete, ioctl_req);
	if (!acb) {
		g_free(ioctl_req);
		status = VIRTIO_VSSD_S_UNSUPP;
		goto fail;
	}
	return -EINPROGRESS;
#else
	abort();
#endif

fail:
	/* Just put anything nonzero so that the ioctl fails in the guest.  */
	if (scsi) {
		virtio_stl_p(vdev, &scsi->errors, 255);
	}
	return status;
}

static void virtio_vssd_handle_scsi(VirtIOVssdReq *req)
{
	int status;

	status = virtio_vssd_handle_scsi_req(req);
	if (status != -EINPROGRESS) {
		virtio_vssd_req_complete(req, status);
		virtio_vssd_free_request(req);
	}
}


static inline void submit_requests(BlockBackend *blk, MultiVssdReqBuffer *mrb,
		int start, int num_reqs, int niov)
{
	/*Unais DEBUG start*/
	if(VSSD_DEBUG_MODE)
	{
		mrb->reqs[start]->stats.qiov_merging_start_time = current_time();
	}
	/*Unais DEBUG end*/

	QEMUIOVector *qiov = &mrb->reqs[start]->qiov;
	int64_t sector_num = mrb->reqs[start]->sector_num;
	bool is_write = mrb->is_write;

	/*Unais DEBUG start*/
	if(VSSD_DEBUG_MODE)
	{
		mrb->reqs[start]->stats.qiov_size = qiov->size;
		mrb->reqs[start]->stats.qiov_iov_count = qiov->niov;

		// mrb->reqs[start]->stats.qiov_merge_count++;
		// 		mrb->reqs[start]->stats.qiov_mereg_size = qiov->size
		// 		mrb->reqs[start]->stats.qiov_merg_iov_count = qiov.niov;
	}
	/*Unais DEBUG end*/

	// printf("VSSD: submit_requests \n");
	// printf("VSSD: \tNumber of request = %d \n", num_reqs);
	if (num_reqs > 1) {
		int i;
		struct iovec *tmp_iov = qiov->iov;
		int tmp_niov = qiov->niov;

		/* mrb->reqs[start]->qiov was initialized from external so we can't
		 * modify it here. We need to initialize it locally and then add the
		 * external iovecs. */
		qemu_iovec_init(qiov, niov);

		for (i = 0; i < tmp_niov; i++) {
			qemu_iovec_add(qiov, tmp_iov[i].iov_base, tmp_iov[i].iov_len);
			// printf("VSSD: \tiov_base = %p, length = %ld \n", tmp_iov[i].iov_base, tmp_iov[i].iov_len);
		}

		for (i = start + 1; i < start + num_reqs; i++) {
			qemu_iovec_concat(qiov, &mrb->reqs[i]->qiov, 0,
					mrb->reqs[i]->qiov.size);
			mrb->reqs[i - 1]->mr_next = mrb->reqs[i];

			/*Unais DEBUG start*/
			if(VSSD_DEBUG_MODE)
			{
				mrb->reqs[i]->stats.qiov_size = mrb->reqs[i]->qiov.size;
				mrb->reqs[i]->stats.qiov_iov_count = mrb->reqs[i]->qiov.niov;

				// mrb->reqs[start]->stats.qiov_merge_count++;
				// mrb->reqs[start]->stats.qiov_mereg_size += mrb->reqs[i]->qiov.size;
				// mrb->reqs[start]->stats.qiov_merg_iov_count += mrb->reqs[i]->qiov.niov;
			}
			/*Unais DEBUG end*/
		}

		trace_virtio_blk_submit_multireq(mrb, start, num_reqs,
				sector_num << BDRV_SECTOR_BITS,
				qiov->size, is_write);
		block_acct_merge_done(blk_get_stats(blk),
				is_write ? BLOCK_ACCT_WRITE : BLOCK_ACCT_READ,
				num_reqs - 1);
	}


	/*Unais DEBUG start*/
	if(VSSD_DEBUG_MODE)
	{
		mrb->reqs[start]->stats.qiov_merge_count = num_reqs;
		mrb->reqs[start]->stats.qiov_mereg_size = qiov->size;
		mrb->reqs[start]->stats.qiov_merg_iov_count = qiov->niov;
	}
	/*Unais DEBUG end*/



	/*Unais Start */

	/*blk_aio_pwritev(blk, (sector_num) << BDRV_SECTOR_BITS, qiov, 0,
	  virtio_vssd_rw_complete, mrb->reqs[start]);*/

	/*printf("\nVSSD: \t%s: sector=%ld [%ld]. req = %p \n", 
	  is_write?"Write. ":"Read. ",
	  sector_num,
	  sector_num << BDRV_SECTOR_BITS,
	  mrb->reqs[start]);

	  printf("VSSD: \tQEMUIOVector. iov=%p, niov=%d, nalloc=%d, size=%ldB[%lds]  \n",
	  qiov->iov,
	  qiov->niov,
	  qiov->nalloc,
	  qiov->size,
	  qiov->size/512);

	  printf("VSSD: \tList of iovs \n");
	  for(int i=0; i<qiov->niov; i++)
	  printf("VSSD: \t\tiov[%d]. base=%p, len=%ldB[%lds] \n", i, qiov->iov[i].iov_base, qiov->iov[i].iov_len, qiov->iov[i].iov_len/512);

	  printf("VSSD: \tParition the qiov \n");*/


	/*Unais DEBUG start*/
	if(VSSD_DEBUG_MODE)
	{
		mrb->reqs[start]->stats.qiov_division_start_time = current_time();
	}
	/*Unais DEBUG end*/

	mrb->reqs[start]->sub_qiov_count = 1000;
	mrb->reqs[start]->sub_qiov_finished = 0;
	int rqst_count=0;
	int64_t total_processed = 0;
	int64_t total_send_to_device = 0;
	int64_t start_sector, end;
	QEMUIOVector *qiov_temp = NULL;

	//Initialize the lock
	pthread_spin_init(&mrb->reqs[start]->lock, 0);

	int multiple_qiov_required = 0;
	if((sector_num%SSD_BLOCK_SIZE) + qiov->size/512 > SSD_BLOCK_SIZE )
		multiple_qiov_required = 1;

	if(!multiple_qiov_required)
	{
		struct new_struct *tmp_parameter = (struct new_struct *)malloc(sizeof(struct new_struct));
		tmp_parameter->req = mrb->reqs[start];
		tmp_parameter->qiov = qiov;

		mrb->reqs[start]->sub_qiov_count = 1;		// Do not delete this qiov.
		mrb->reqs[start]->sub_qiov_finished = 0;

		qiov_temp = qiov;

		/*Unais DEBUG start*/
		if(VSSD_DEBUG_MODE)
		{
			mrb->reqs[start]->stats.last_sub_qiov_send_time = current_time();
			if(mrb->reqs[start]->stats.first_sub_qiov_send_time == 0)
				mrb->reqs[start]->stats.first_sub_qiov_send_time = mrb->reqs[start]->stats.last_sub_qiov_send_time;
		}
		/*Unais DEBUG end*/


		if (is_write) 
			blk_aio_pwritev(blk, l2p(mrb->reqs[start]->dev, sector_num) << BDRV_SECTOR_BITS, qiov_temp, 0,
					virtio_vssd_rw_complete, tmp_parameter);
		else
			blk_aio_preadv(blk, l2p(mrb->reqs[start]->dev, sector_num) << BDRV_SECTOR_BITS, qiov_temp, 0,
					virtio_vssd_rw_complete, tmp_parameter);

		return;
	}

	for(int i=0; i<qiov->niov; i++)
	{
		//Partitioning by blocks of 8 sectors
		int64_t current_iov_sectors = qiov->iov[i].iov_len/512;
		int64_t current_iov_processed = 0;
		int64_t current_iov_remaining = current_iov_sectors;


		while(current_iov_remaining > 0)
		{
			start_sector = sector_num + total_processed;

			if(start_sector % SSD_BLOCK_SIZE == 0)
			{
				// Starting is block aligned

				if(current_iov_remaining >= SSD_BLOCK_SIZE)
					end = start_sector + SSD_BLOCK_SIZE;
				else
					end = start_sector + current_iov_remaining;
			}
			else
			{
				//	Starting is not block aligned.
				int offset_in_block = start_sector % SSD_BLOCK_SIZE;
				if(offset_in_block + current_iov_remaining >= SSD_BLOCK_SIZE)
					end = start_sector + SSD_BLOCK_SIZE - offset_in_block;
				else
					end = start_sector + current_iov_remaining;
			}

			if(!qiov_temp)
			{
				// printf("VSSD: \t\tCreating new iovector = %d.  set nalloc=%d \n", rqst_count, qiov->niov);

				qiov_temp = (QEMUIOVector *)malloc(sizeof(QEMUIOVector));
				qemu_iovec_init(qiov_temp, qiov->niov);

				rqst_count++;
			}

			// printf("VSSD: \t\t\tiov = %d. start = %ld, end = %ld. \n",
			// 		i, start_sector, end);

			qemu_iovec_add(qiov_temp, qiov->iov[i].iov_base + current_iov_processed*512, (end-start_sector)*512);

			if(i == qiov->niov-1 && current_iov_remaining - (end-start_sector) == 0)
			{
				//printf("VSSD: \t\t   Last sub parition.  \n");
				pthread_spin_lock(&mrb->reqs[start]->lock);
				mrb->reqs[start]->sub_qiov_count = rqst_count;
				pthread_spin_unlock(&mrb->reqs[start]->lock);
			}

			if(end % SSD_BLOCK_SIZE  == 0)
			{

				// printf("VSSD: \t\t\tReached block boundary. last iov start=%ld. end=%ld.  \n ",
				// 		start_sector, end);
				int64_t temp_qiov_size = qiov_temp->size;

				struct new_struct *tmp_parameter=(struct new_struct *)malloc(sizeof(struct new_struct));
				tmp_parameter->req = mrb->reqs[start];
				tmp_parameter->qiov = qiov_temp;

				/*printf("VSSD: \t\t\tiovs in this QIOV. This QIOV size=%ldB[%lds] \n", qiov_temp->size,  qiov_temp->size/512);
				  for(int t=0; t<qiov_temp->niov; t++)
				  {
				  printf("VSSD: \t\t\t    iov[%d]. base=%p, len=%ldB[%lds] \n", 
				  t, qiov_temp->iov[t].iov_base, qiov_temp->iov[t].iov_len, qiov_temp->iov[t].iov_len/512);
				  }*/

				/*Unais DEBUG start*/
				if(VSSD_DEBUG_MODE)
				{
					mrb->reqs[start]->stats.last_sub_qiov_send_time = current_time();
					if(mrb->reqs[start]->stats.first_sub_qiov_send_time == 0)
						mrb->reqs[start]->stats.first_sub_qiov_send_time = mrb->reqs[start]->stats.last_sub_qiov_send_time;
				}
				/*Unais DEBUG end*/


				if (is_write) 
					blk_aio_pwritev(blk, l2p(mrb->reqs[start]->dev, sector_num + total_send_to_device) << BDRV_SECTOR_BITS, qiov_temp, 0,
							virtio_vssd_rw_complete, tmp_parameter);
				else
					blk_aio_preadv(blk, l2p(mrb->reqs[start]->dev, sector_num + total_send_to_device) << BDRV_SECTOR_BITS, qiov_temp, 0,
							virtio_vssd_rw_complete, tmp_parameter);

				total_send_to_device += temp_qiov_size;
				qiov_temp = NULL;

			}

			total_processed += end-start_sector;
			current_iov_processed += end-start_sector;
			current_iov_remaining -= end-start_sector;			
		}	 		

	}

	if(qiov_temp)
	{
		int64_t temp_qiov_size = qiov_temp->size;

		/*printf("VSSD: \t\t\tReached the end last iov start=%ld. end=%ld. \n ",
		  start_sector, end);

		  printf("VSSD: \t\t\tiovs in this QIOV. This QIOV size=%ldB[%lds] \n", qiov_temp->size,  qiov_temp->size/512);
		  for(int t=0; t<qiov_temp->niov; t++)
		  {
		  printf("VSSD: \t\t\t    iov[%d]. base=%p, len=%ldB[%lds] \n", 
		  t, qiov_temp->iov[t].iov_base, qiov_temp->iov[t].iov_len, qiov_temp->iov[t].iov_len/512);
		  }*/


		struct new_struct *tmp_parameter=(struct new_struct *)malloc(sizeof(struct new_struct));
		tmp_parameter->req = mrb->reqs[start];
		tmp_parameter->qiov = qiov_temp;

		/*printf("VSSD : \t\t\tSending reqeust. logical=%ld. physical=%ld. \n", 
		  sector_num + total_send_to_device, 
		  l2p(mrb->reqs[start]->dev, sector_num + total_send_to_device));*/

		/*Unais DEBUG start*/
		if(VSSD_DEBUG_MODE)
		{
			mrb->reqs[start]->stats.last_sub_qiov_send_time = current_time();
			if(mrb->reqs[start]->stats.first_sub_qiov_send_time == 0)
				mrb->reqs[start]->stats.first_sub_qiov_send_time = mrb->reqs[start]->stats.last_sub_qiov_send_time;
		}
		/*Unais DEBUG end*/

		if (is_write) 
			blk_aio_pwritev(blk, l2p(mrb->reqs[start]->dev, sector_num + total_send_to_device) << BDRV_SECTOR_BITS, 
					qiov_temp, 0, virtio_vssd_rw_complete, tmp_parameter);
		else
			blk_aio_preadv(blk, l2p(mrb->reqs[start]->dev, sector_num + total_send_to_device) << BDRV_SECTOR_BITS, qiov_temp, 0,
					virtio_vssd_rw_complete, tmp_parameter);

		total_send_to_device += temp_qiov_size;
		qiov_temp = NULL;
	}

	/*Unais End*/
}



static int multireq_compare(const void *a, const void *b)
{
	const VirtIOVssdReq *req1 = *(VirtIOVssdReq **)a,
	      *req2 = *(VirtIOVssdReq **)b;

	/*
	 * Note that we can't simply subtract sector_num1 from sector_num2
	 * here as that could overflow the return value.
	 */
	if (req1->sector_num > req2->sector_num) {
		return 1;
	} else if (req1->sector_num < req2->sector_num) {
		return -1;
	} else {
		return 0;
	}
}

static void virtio_vssd_submit_multireq(BlockBackend *blk, MultiVssdReqBuffer *mrb)
{

	// printf("virtio: virtio_vssd_submit_multireq called \n");

	int i = 0, start = 0, num_reqs = 0, niov = 0, nb_sectors = 0;
	uint32_t max_transfer;
	int64_t sector_num = 0;

	//("VSSD: virtio_vssd_submit_multireq \n");

	if (mrb->num_reqs == 1) {
		//("VSSD: \tOnly  1 request. Directly calls submit_requests \n");
		submit_requests(blk, mrb, 0, 1, -1);
		mrb->num_reqs = 0;
		return;
	}

	max_transfer = blk_get_max_transfer(mrb->reqs[0]->dev->blk);
	//printf("VSSD: \tmax_transfer = %d \n", max_transfer);

	qsort(mrb->reqs, mrb->num_reqs, sizeof(*mrb->reqs),
			&multireq_compare);

	//printf("VSSD: \tmrb->num_reqs = %d \n", mrb->num_reqs);
	for (i = 0; i < mrb->num_reqs; i++) {
		VirtIOVssdReq *req = mrb->reqs[i];
		if (num_reqs > 0) {
			/*
			 * NOTE: We cannot merge the requests in below situations:
			 * 1. requests are not sequential
			 * 2. merge would exceed maximum number of IOVs
			 * 3. merge would exceed maximum transfer length of backend device
			 */
			if (sector_num + nb_sectors != req->sector_num ||
					niov > blk_get_max_iov(blk) - req->qiov.niov ||
					req->qiov.size > max_transfer ||
					nb_sectors > (max_transfer - req->qiov.size) / BDRV_SECTOR_SIZE) 
			{
				submit_requests(blk, mrb, start, num_reqs, niov);
				num_reqs = 0;
			}
		}

		if (num_reqs == 0) 
		{
			sector_num = req->sector_num;
			nb_sectors = niov = 0;
			start = i;
		}

		nb_sectors += req->qiov.size / BDRV_SECTOR_SIZE;
		niov += req->qiov.niov;
		num_reqs++;
	}

	submit_requests(blk, mrb, start, num_reqs, niov);
	mrb->num_reqs = 0;
}

static void virtio_vssd_handle_flush(VirtIOVssdReq *req, MultiVssdReqBuffer *mrb)
{
	block_acct_start(blk_get_stats(req->dev->blk), &req->acct, 0,
			BLOCK_ACCT_FLUSH);

	/*
	 * Make sure all outstanding writes are posted to the backing device.
	 */
	if (mrb->is_write && mrb->num_reqs > 0) {
		virtio_vssd_submit_multireq(req->dev->blk, mrb);
	}
	blk_aio_flush(req->dev->blk, virtio_vssd_flush_complete, req);
}

/*static bool virtio_vssd_sect_range_ok(VirtIOVssd *dev,
  uint64_t sector, size_t size)
  {
  uint64_t nb_sectors = size >> BDRV_SECTOR_BITS;
  uint64_t total_sectors;

  if (nb_sectors > BDRV_REQUEST_MAX_SECTORS) {
  return false;
  }
  if (sector & dev->sector_mask) {
  return false;
  }
  if (size % dev->conf.conf.logical_block_size) {
  return false;
  }
  blk_get_geometry(dev->blk, &total_sectors);
  if (sector > total_sectors || nb_sectors > total_sectors - sector) {
  return false;
  }
  return true;
  }*/

static int virtio_vssd_handle_request(VirtIOVssdReq *req, MultiVssdReqBuffer *mrb)
{
	uint32_t type;
	struct iovec *in_iov = req->elem.in_sg;
	struct iovec *iov = req->elem.out_sg;
	unsigned in_num = req->elem.in_num;
	unsigned out_num = req->elem.out_num;
	VirtIOVssd *s = req->dev;
	VirtIODevice *vdev = VIRTIO_DEVICE(s);

	//clock_t start, end;
	//double cpu_time_used; 

	// printf("\nVSSD: Handle request \n");

	if (req->elem.out_num < 1 || req->elem.in_num < 1) {
		virtio_error(vdev, "virtio-blk missing headers");
		return -1;
	}

	if (unlikely(iov_to_buf(iov, out_num, 0, &req->out,
					sizeof(req->out)) != sizeof(req->out))) {
		virtio_error(vdev, "virtio-blk request outhdr too short");
		return -1;
	}

	iov_discard_front(&iov, &out_num, sizeof(req->out));

	if (in_iov[in_num - 1].iov_len < sizeof(struct virtio_vssd_inhdr)) {
		virtio_error(vdev, "virtio-blk request inhdr too short");
		return -1;
	}

	/* We always touch the last byte, so just see how big in_iov is.  */
	req->in_len = iov_size(in_iov, in_num);
	req->in = (void *)in_iov[in_num - 1].iov_base
		+ in_iov[in_num - 1].iov_len
		- sizeof(struct virtio_vssd_inhdr);
	iov_discard_back(in_iov, &in_num, sizeof(struct virtio_vssd_inhdr));

	type = virtio_ldl_p(VIRTIO_DEVICE(req->dev), &req->out.type);

	/* VIRTIO_VSSd_T_OUT defines the command direction. VIRTIO_VSSd_T_BARRIER
	 * is an optional flag. Although a guest should not send this flag if
	 * not negotiated we ignored it in the past. So keep ignoring it. */
	switch (type & ~(VIRTIO_VSSD_T_OUT | VIRTIO_VSSD_T_BARRIER)) {
		case VIRTIO_VSSD_T_IN:
			{
				bool is_write = type & VIRTIO_VSSD_T_OUT;
				req->sector_num = virtio_ldq_p(VIRTIO_DEVICE(req->dev), &req->out.sector);

				/*printf("VSSD: \tout.sector:%ld, logical-sector:%ld, physical-sector:%ld \n",
				  req->out.sector,
				  req->sector_num,
				  l2p(s, req->sector_num));*/

				if (is_write) 
				{
					//printf("VSSD: \tWrite request \n");
					//start = clock(); 

					qemu_iovec_init_external(&req->qiov, iov, out_num);
					trace_virtio_blk_handle_write(req, req->sector_num,
							req->qiov.size / BDRV_SECTOR_SIZE);

				} 
				else 
				{   
					//printf("VSSD: \tRead request \n");
					//start = clock();

					qemu_iovec_init_external(&req->qiov, in_iov, in_num);
					trace_virtio_blk_handle_read(req, req->sector_num,
							req->qiov.size / BDRV_SECTOR_SIZE);
				}

				//	ToDoUnais  Verify range of physical......
				/*if (!virtio_vssd_sect_range_ok(req->dev, req->sector_num, req->qiov.size)) 
				  {
				  virtio_vssd_req_complete(req, VIRTIO_VSSD_S_IOERR);
				  block_acct_invalid(blk_get_stats(req->dev->blk), is_write ? BLOCK_ACCT_WRITE : BLOCK_ACCT_READ);
				  virtio_vssd_free_request(req);
				  return 0;
				  }
				 */
				block_acct_start(blk_get_stats(req->dev->blk),
						&req->acct, req->qiov.size,
						is_write ? BLOCK_ACCT_WRITE : BLOCK_ACCT_READ);

				/* merge would exceed maximum number of requests or IO direction
				 * changes */
				//printf("virtio: num_reqs=%d, VIRTIO_VSSd_MAX_MERGE_REQS=32, is_write=%d, mrb->is_write=%d, req->dev->conf.request_merging=%d \n",
				//     mrb->num_reqs, is_write, mrb->is_write, req->dev->conf.request_merging);

				if (mrb->num_reqs > 0 && (	mrb->num_reqs == VIRTIO_VSSD_MAX_MERGE_REQS || 
							is_write != mrb->is_write || 
							!req->dev->conf.request_merging)) 
				{
					// printf("virtio: Calling submit_multireq from handle_request \n");
					virtio_vssd_submit_multireq(req->dev->blk, mrb);
				}

				// end = clock();
				// cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
				// printf("virtio: %s Completed......\n", is_write ? "Write" : "Read");  
				// printf("virtio: %s took %f seconds to execute--------------->>>> \n", is_write ? "Write" : "Read", cpu_time_used);

				assert(mrb->num_reqs < VIRTIO_VSSD_MAX_MERGE_REQS);
				mrb->reqs[mrb->num_reqs++] = req;
				mrb->is_write = is_write;


				/*Unais DEBUG start*/
				if(VSSD_DEBUG_MODE)
				{
					req->stats.adding_to_mrb_time = current_time();
				}
				/*Unais DEBUG end*/

				break;
			}
		case VIRTIO_VSSD_T_FLUSH:
			virtio_vssd_handle_flush(req, mrb);
			break;
		case VIRTIO_VSSD_T_SCSI_CMD:
			virtio_vssd_handle_scsi(req);
			break;
		case VIRTIO_VSSD_T_GET_ID:
			{
				VirtIOVssd *s = req->dev;

				/*
				 * NB: per existing s/n string convention the string is
				 * terminated by '\0' only when shorter than buffer.
				 */
				const char *serial = s->conf.serial ? s->conf.serial : "";
				size_t size = MIN(strlen(serial) + 1,
						MIN(iov_size(in_iov, in_num),
							VIRTIO_VSSD_ID_BYTES));
				iov_from_buf(in_iov, in_num, 0, serial, size);
				virtio_vssd_req_complete(req, VIRTIO_VSSD_S_OK);
				virtio_vssd_free_request(req);
				break;
			}
		default:
			virtio_vssd_req_complete(req, VIRTIO_VSSD_S_UNSUPP);
			virtio_vssd_free_request(req);
	}
	return 0;
}

bool virtio_vssd_handle_vq(VirtIOVssd *s, VirtQueue *vq)
{
	VirtIOVssdReq *req;
	MultiVssdReqBuffer mrb = {};
	bool progress = false;

	/*Unais Start */
	clock_t temp_time = current_time();
	/*Unais End*/

	aio_context_acquire(blk_get_aio_context(s->blk));
	blk_io_plug(s->blk);



	do 
	{
		virtio_queue_set_notification(vq, 0);

		while ((req = virtio_vssd_get_request(s, vq))) 
		{

			/*Unais start*/
			if(VSSD_DEBUG_MODE)
			{
				req->stats.request_pull_time = temp_time;
				req->stats.adding_to_mrb_time = 0;
				req->stats.callback_time = 0;                  //  Time at which call back for last sub-qiov received
				req->stats.request_push_time = 0;              //  Final event

				req->stats.qiov_merging_start_time = 0;
				req->stats.qiov_division_start_time = 0;       //  Merging end time
				req->stats.first_sub_qiov_send_time = 0;
				req->stats.last_sub_qiov_send_time = 0;        //  Division end time
				req->stats.first_sub_qiov_callback_time = 0;
				req->stats.last_sub_qiov_callback_time = 0;
			}
			/*Unais end*/

			progress = true;
			if (virtio_vssd_handle_request(req, &mrb)) 
			{
				virtqueue_detach_element(req->vq, &req->elem, 0);
				virtio_vssd_free_request(req);
				break;
			}
		}

		virtio_queue_set_notification(vq, 1);
	} while (!virtio_queue_empty(vq));


	if (mrb.num_reqs) 
	{
		// printf("virtio: Calling multi req from handle_vq \n");
		virtio_vssd_submit_multireq(s->blk, &mrb);
	}

	blk_io_unplug(s->blk);
	aio_context_release(blk_get_aio_context(s->blk));
	return progress;
}

static void virtio_vssd_handle_output_do(VirtIOVssd *s, VirtQueue *vq)
{
	virtio_vssd_handle_vq(s, vq);
}

static void virtio_vssd_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
	VirtIOVssd *s = (VirtIOVssd *)vdev;

	if (s->dataplane) {
		/* Some guests kick before setting VIRTIO_CONFIG_S_DRIVER_OK so start
		 * dataplane here instead of waiting for .set_status().
		 */
		virtio_device_start_ioeventfd(vdev);
		if (!s->dataplane_disabled) {
			return;
		}
	}
	virtio_vssd_handle_output_do(s, vq);
}

static void virtio_vssd_dma_restart_bh(void *opaque)
{
	VirtIOVssd *s = opaque;
	VirtIOVssdReq *req = s->rq;
	MultiVssdReqBuffer mrb = {};

	qemu_bh_delete(s->bh);
	s->bh = NULL;

	s->rq = NULL;

	aio_context_acquire(blk_get_aio_context(s->conf.conf.blk));
	while (req) {
		VirtIOVssdReq *next = req->next;
		if (virtio_vssd_handle_request(req, &mrb)) {
			/* Device is now broken and won't do any processing until it gets
			 * reset. Already queued requests will be lost: let's purge them.
			 */
			while (req) {
				next = req->next;
				virtqueue_detach_element(req->vq, &req->elem, 0);
				virtio_vssd_free_request(req);
				req = next;
			}
			break;
		}
		req = next;
	}

	if (mrb.num_reqs) {
		virtio_vssd_submit_multireq(s->blk, &mrb);
	}
	aio_context_release(blk_get_aio_context(s->conf.conf.blk));
}

static void virtio_vssd_dma_restart_cb(void *opaque, int running,
		RunState state)
{
	VirtIOVssd *s = opaque;

	if (!running) {
		return;
	}

	if (!s->bh) {
		s->bh = aio_bh_new(blk_get_aio_context(s->conf.conf.blk),
				virtio_vssd_dma_restart_bh, s);
		qemu_bh_schedule(s->bh);
	}
}

static void virtio_vssd_reset(VirtIODevice *vdev)
{
	VirtIOVssd *s = VIRTIO_VSSD(vdev);
	AioContext *ctx;
	VirtIOVssdReq *req;

	ctx = blk_get_aio_context(s->blk);
	aio_context_acquire(ctx);
	blk_drain(s->blk);

	/* We drop queued requests after blk_drain() because blk_drain() itself can
	 * produce them. */
	while (s->rq) {
		req = s->rq;
		s->rq = req->next;
		virtqueue_detach_element(req->vq, &req->elem, 0);
		virtio_vssd_free_request(req);
	}

	aio_context_release(ctx);

	assert(!s->dataplane_started);
	blk_set_enable_write_cache(s->blk, s->original_wce);
}

static void int_to_binary(unsigned int ptr, char *buf)
{

	int i;

	for (i = 0; i < 32; i++) 
	{
		if( (ptr>>(32-i-1)) & 1) 
			buf[i] = '1';
		else
			buf[i] = '0';
	}
	buf[i] = '\0';
}


/* coalesce internal state, copy to pci i/o region 0
 */
static void virtio_vssd_update_config(VirtIODevice *vdev, uint8_t *config)
{
	VirtIOVssd *s = VIRTIO_VSSD(vdev);
	BlockConf *conf = &s->conf.conf;
	struct virtio_vssd_config blkcfg;
	uint64_t capacity;
	uint64_t lbn;
	int blk_size = conf->logical_block_size;

	char buf[50];

	// printf("VSSD: Virtio_vssd_update_config \n ");

	blk_get_geometry(s->blk, &capacity);
	memset(&blkcfg, 0, sizeof(blkcfg));

	/*UNias start*/

	virtio_stq_p(vdev, &blkcfg.capacity, s->capacity);
	virtio_stq_p(vdev, &blkcfg.current_capacity, s->current_capacity);

	//int sign = s->command < 0 ? -1 : 1;
	//blkcfg.command = sign*s->command > SSD_BALLOON_UNIT ? sign*SSD_BALLOON_UNIT : s->command;
	blkcfg.command = s->command;
	//s->command -= blkcfg.command;

	// printf("VSSD: virtio_vssd_update_config called.  \n");

	if(bitmap_reading)
	{
		uint64_t starting = bitmap_offset * 32;
		uint64_t ending = starting + 32;
		if(ending > s->capacity * SSD_BLOCK_SIZE)
			ending = s->capacity * SSD_BLOCK_SIZE;

		blkcfg.bitmap_value = 0XFFFFFFFF;
		for(lbn = starting; lbn < ending; lbn++ ) 
		{
			if(s->block_list[lbn] != -1)
				blkcfg.bitmap_value &= ~(1 << (lbn % 32));
			// if(lbn > 10000 && lbn < 11000)
			// printf(" [lbn:pbn %lu:%ld] \n", lbn, s->block_list[lbn]);
		}

		int_to_binary(blkcfg.bitmap_value, buf);

		// printf("VSSD: Reading bitmap. %lu-%lu: [%s] \n",
		// starting, ending-1, buf);

		bitmap_offset ++;
	}


	/*End*/

	virtio_stl_p(vdev, &blkcfg.seg_max, 128 - 2);
	virtio_stw_p(vdev, &blkcfg.geometry.cylinders, conf->cyls);
	virtio_stl_p(vdev, &blkcfg.blk_size, blk_size);
	virtio_stw_p(vdev, &blkcfg.min_io_size, conf->min_io_size / blk_size);
	virtio_stw_p(vdev, &blkcfg.opt_io_size, conf->opt_io_size / blk_size);

	blkcfg.current_request_id = resize_request_id;

	blkcfg.geometry.heads = conf->heads;

	// printf("VSSD: \tCapacity = %ld \n", blkcfg.capacity);
	/*
	 * We must ensure that the block device capacity is a multiple of
	 * the logical block size. If that is not the case, let's use
	 * sector_mask to adopt the geometry to have a correct picture.
	 * For those devices where the capacity is ok for the given geometry
	 * we don't touch the sector value of the geometry, since some devices
	 * (like s390 dasd) need a specific value. Here the capacity is already
	 * cyls*heads*secs*blk_size and the sector value is not block size
	 * divided by 512 - instead it is the amount of blk_size blocks
	 * per track (cylinder).
	 */
	if (blk_getlength(s->blk) /  conf->heads / conf->secs % blk_size) {
		blkcfg.geometry.sectors = conf->secs & ~s->sector_mask;
	} else {
		blkcfg.geometry.sectors = conf->secs;
	}
	blkcfg.size_max = 0;
	blkcfg.physical_block_exp = get_physical_block_exp(conf);
	blkcfg.alignment_offset = 0;
	blkcfg.wce = blk_enable_write_cache(s->blk);
	virtio_stw_p(vdev, &blkcfg.num_queues, s->conf.num_queues);
	memcpy(config, &blkcfg, sizeof(struct virtio_vssd_config));
}

static void virtio_vssd_set_config(VirtIODevice *vdev, const uint8_t *config)
{

	VirtIOVssd *s = VIRTIO_VSSD(vdev);
	struct virtio_vssd_config blkcfg;

	memcpy(&blkcfg, config, sizeof(blkcfg));
	bitmap_reading = blkcfg.bitmap_reading;
	bitmap_offset = blkcfg.bitmap_offset;

	// printf("VSSD: virtio_vssd_set_config. offset: %u. count: %u \n",
	// 	blkcfg.bitmap_offset, blkcfg.bitmap_count);

	aio_context_acquire(blk_get_aio_context(s->blk));
	blk_set_enable_write_cache(s->blk, blkcfg.wce != 0);
	aio_context_release(blk_get_aio_context(s->blk));
}

static uint64_t virtio_vssd_get_features(VirtIODevice *vdev, uint64_t features,
		Error **errp)
{
	VirtIOVssd *s = VIRTIO_VSSD(vdev);

	virtio_add_feature(&features, VIRTIO_VSSD_F_SEG_MAX);
	virtio_add_feature(&features, VIRTIO_VSSD_F_GEOMETRY);
	virtio_add_feature(&features, VIRTIO_VSSD_F_TOPOLOGY);
	virtio_add_feature(&features, VIRTIO_VSSD_F_BLK_SIZE);
	if (virtio_has_feature(features, VIRTIO_F_VERSION_1)) {
		if (s->conf.scsi) {
			error_setg(errp, "Please set scsi=off for virtio-blk devices in order to use virtio 1.0");
			return 0;
		}
	} else {
		virtio_clear_feature(&features, VIRTIO_F_ANY_LAYOUT);
		virtio_add_feature(&features, VIRTIO_VSSD_F_SCSI);
	}

	if (s->conf.config_wce) {
		virtio_add_feature(&features, VIRTIO_VSSD_F_CONFIG_WCE);
	}
	if (blk_enable_write_cache(s->blk)) {
		virtio_add_feature(&features, VIRTIO_VSSD_F_WCE);
	}
	if (blk_is_read_only(s->blk)) {
		virtio_add_feature(&features, VIRTIO_VSSD_F_RO);
	}
	if (s->conf.num_queues > 1) {
		virtio_add_feature(&features, VIRTIO_VSSD_F_MQ);
	}

	return features;
}

static void virtio_vssd_set_status(VirtIODevice *vdev, uint8_t status)
{
	VirtIOVssd *s = VIRTIO_VSSD(vdev);

	if (!(status & (VIRTIO_CONFIG_S_DRIVER | VIRTIO_CONFIG_S_DRIVER_OK))) {
		assert(!s->dataplane_started);
	}

	if (!(status & VIRTIO_CONFIG_S_DRIVER_OK)) {
		return;
	}

	/* A guest that supports VIRTIO_VSSd_F_CONFIG_WCE must be able to send
	 * cache flushes.  Thus, the "auto writethrough" behavior is never
	 * necessary for guests that support the VIRTIO_VSSd_F_CONFIG_WCE feature.
	 * Leaving it enabled would break the following sequence:
	 *
	 *     Guest started with "-drive cache=writethrough"
	 *     Guest sets status to 0
	 *     Guest sets DRIVER bit in status field
	 *     Guest reads host features (WCE=0, CONFIG_WCE=1)
	 *     Guest writes guest features (WCE=0, CONFIG_WCE=1)
	 *     Guest writes 1 to the WCE configuration field (writeback mode)
	 *     Guest sets DRIVER_OK bit in status field
	 *
	 * s->blk would erroneously be placed in writethrough mode.
	 */
	if (!virtio_vdev_has_feature(vdev, VIRTIO_VSSD_F_CONFIG_WCE)) {
		aio_context_acquire(blk_get_aio_context(s->blk));
		blk_set_enable_write_cache(s->blk,
				virtio_vdev_has_feature(vdev,
					VIRTIO_VSSD_F_WCE));
		aio_context_release(blk_get_aio_context(s->blk));
	}
}

static void virtio_vssd_save_device(VirtIODevice *vdev, QEMUFile *f)
{
	VirtIOVssd *s = VIRTIO_VSSD(vdev);
	VirtIOVssdReq *req = s->rq;

	while (req) {
		qemu_put_sbyte(f, 1);

		if (s->conf.num_queues > 1) {
			qemu_put_be32(f, virtio_get_queue_index(req->vq));
		}

		qemu_put_virtqueue_element(f, &req->elem);
		req = req->next;
	}
	qemu_put_sbyte(f, 0);
}

static int virtio_vssd_load_device(VirtIODevice *vdev, QEMUFile *f,
		int version_id)
{
	VirtIOVssd *s = VIRTIO_VSSD(vdev);

	while (qemu_get_sbyte(f)) {
		unsigned nvqs = s->conf.num_queues;
		unsigned vq_idx = 0;
		VirtIOVssdReq *req;

		if (nvqs > 1) {
			vq_idx = qemu_get_be32(f);

			if (vq_idx >= nvqs) {
				error_report("Invalid virtqueue index in request list: %#x",
						vq_idx);
				return -EINVAL;
			}
		}

		req = qemu_get_virtqueue_element(vdev, f, sizeof(VirtIOVssdReq));
		virtio_vssd_init_request(s, virtio_get_queue(vdev, vq_idx), req);
		req->next = s->rq;
		s->rq = req;
	}

	return 0;
}

static void virtio_vssd_resize(void *opaque)
{
	VirtIODevice *vdev = VIRTIO_DEVICE(opaque);

	virtio_notify_config(vdev);
}

static const BlockDevOps virtio_block_ops = {
	.resize_cb = virtio_vssd_resize,
};

static void virtio_vssd_device_realize(DeviceState *dev, Error **errp)
{
	printf("\n\n\nVSSD: virtio_vssd_device_realize function called \n");   

	// extern int test_vm_id;
	// extern char test_vm_name;
	// printf("id = %d, name = %s \n", test_vm_id, test_vm_name);

	//pritf("vm-id  = %d \n", test_vm_id);
	//exit(1);

	VirtIODevice *vdev = VIRTIO_DEVICE(dev);
	VirtIOVssd *s = VIRTIO_VSSD(dev);
	VirtIOVssdConf *conf = &s->conf;
	Error *err = NULL;
	int ret;
	unsigned i;

	if (!conf->conf.blk) {
		error_setg(errp, "drive property not set");
		return;
	}
	if (!blk_is_inserted(conf->conf.blk)) {
		error_setg(errp, "Device needs media, but drive is empty");
		return;
	}
	if (!conf->num_queues) {
		error_setg(errp, "num-queues property must be larger than 0");
		return;
	}

	blkconf_serial(&conf->conf, &conf->serial);
	blkconf_apply_backend_options(&conf->conf,
			blk_is_read_only(conf->conf.blk), true,
			&err);
	if (err) {
		error_propagate(errp, err);
		return;
	}


	s->original_wce = blk_enable_write_cache(conf->conf.blk);
	blkconf_geometry(&conf->conf, NULL, 65535, 255, 255, &err);
	if (err) {
		error_propagate(errp, err);
		return;
	}
	blkconf_blocksizes(&conf->conf);
	printf("VSSD: \tLogical block size=%d, Physicalblock size=%d \n",
			conf->conf.logical_block_size, conf->conf.physical_block_size);


	virtio_init(vdev, "virtio-vssd", VIRTIO_ID_VSSD,
			sizeof(struct virtio_vssd_config));


	/*ToDoUnais  Read vssd size as command line argumenet to qemu*/  
	// int capacity_in_gb = 10;
	s->capacity = gb_to_sectors (vssd_size); 
	// s->current_capacity = s->capacity;
	s->current_capacity = 0;

	printf("VSSD: \tvssd capacity = %ld \n", s->capacity);

	s->blk = conf->conf.blk;
	s->rq = NULL;
	s->sector_mask = (s->conf.conf.logical_block_size / BDRV_SECTOR_SIZE) - 1;

	printf("VSSD: \tAdding %d queues to virtio \n", conf->num_queues);
	for (i = 0; i < conf->num_queues; i++) {
		virtio_add_queue(vdev, 128, virtio_vssd_handle_output);
	}

	/*UNAIS START*/
	ret = virtio_setup_vssd(s);
	if(ret < 0)
		exit(1);
	/*END*/


	virtio_vssd_data_plane_create(vdev, conf, &s->dataplane, &err);
	if (err != NULL) {
		error_propagate(errp, err);
		virtio_cleanup(vdev);
		return;
	}


	printf("VSSD: \tData plane created \n");

	s->change = qemu_add_vm_change_state_handler(virtio_vssd_dma_restart_cb, s);
	blk_set_dev_ops(s->blk, &virtio_block_ops, s);
	blk_set_guest_block_size(s->blk, s->conf.conf.logical_block_size);

	blk_iostatus_enable(s->blk);


	printf("VSSD: \tRealize completed \n");
}

static void virtio_vssd_device_unrealize(DeviceState *dev, Error **errp)
{
	VirtIODevice *vdev = VIRTIO_DEVICE(dev);
	VirtIOVssd *s = VIRTIO_VSSD(dev);

	virtio_vssd_data_plane_destroy(s->dataplane);
	s->dataplane = NULL;
	qemu_del_vm_change_state_handler(s->change);
	blockdev_mark_auto_del(s->blk);
	virtio_cleanup(vdev);
}

static void virtio_vssd_instance_init(Object *obj)
{
	VirtIOVssd *s = VIRTIO_VSSD(obj);

	object_property_add_link(obj, "iothread", TYPE_IOTHREAD,
			(Object **)&s->conf.iothread,
			qdev_prop_allow_set_link_before_realize,
			OBJ_PROP_LINK_UNREF_ON_RELEASE, NULL);
	device_add_bootindex_property(obj, &s->conf.conf.bootindex,
			"bootindex", "/disk@0,0",
			DEVICE(obj), NULL);
}

static const VMStateDescription vmstate_virtio_vssd = {
	.name = "virtio-vssd",
	.minimum_version_id = 2,
	.version_id = 2,
	.fields = (VMStateField[]) {
		VMSTATE_VIRTIO_DEVICE,
		VMSTATE_END_OF_LIST()
	},
};

static Property virtio_vssd_properties[] = {
	DEFINE_BLOCK_PROPERTIES(VirtIOVssd, conf.conf),
	DEFINE_BLOCK_ERROR_PROPERTIES(VirtIOVssd, conf.conf),
	DEFINE_BLOCK_CHS_PROPERTIES(VirtIOVssd, conf.conf),
	DEFINE_PROP_STRING("serial", VirtIOVssd, conf.serial),
	DEFINE_PROP_BIT("config-wce", VirtIOVssd, conf.config_wce, 0, true),
#ifdef __linux__
	DEFINE_PROP_BIT("scsi", VirtIOVssd, conf.scsi, 0, false),
#endif
	DEFINE_PROP_BIT("request-merging", VirtIOVssd, conf.request_merging, 0,
			true),
	DEFINE_PROP_UINT16("num-queues", VirtIOVssd, conf.num_queues, 1),
	DEFINE_PROP_END_OF_LIST(),
};

static void virtio_vssd_class_init(ObjectClass *klass, void *data)
{
	printf("virtio: 'virtio_vssd_class_init' function called \n");   

	DeviceClass *dc = DEVICE_CLASS(klass);
	VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

	dc->props = virtio_vssd_properties;
	dc->vmsd = &vmstate_virtio_vssd;
	set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
	vdc->realize = virtio_vssd_device_realize;
	vdc->unrealize = virtio_vssd_device_unrealize;
	vdc->get_config = virtio_vssd_update_config;
	vdc->set_config = virtio_vssd_set_config;
	vdc->get_features = virtio_vssd_get_features;
	vdc->set_status = virtio_vssd_set_status;
	vdc->reset = virtio_vssd_reset;
	vdc->save = virtio_vssd_save_device;
	vdc->load = virtio_vssd_load_device;
	vdc->start_ioeventfd = virtio_vssd_data_plane_start;
	vdc->stop_ioeventfd = virtio_vssd_data_plane_stop;
}

static const TypeInfo virtio_vssd_info = {
	.name = TYPE_VIRTIO_VSSD,
	.parent = TYPE_VIRTIO_DEVICE,
	.instance_size = sizeof(VirtIOVssd),
	.instance_init = virtio_vssd_instance_init,
	.class_init = virtio_vssd_class_init,
};



static void virtio_register_types(void)
{
	printf("Registration \n");   
	type_register_static(&virtio_vssd_info);
	printf("\tvirtio-vssd device registered. \n");
}

type_init(virtio_register_types)
