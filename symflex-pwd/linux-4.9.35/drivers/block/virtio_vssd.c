//#define DEBUG
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/module.h>
// #include <linux/spinlock.h>
#include <linux/virtio.h>
#include <linux/virtio_vssd.h>
#include <linux/scatterlist.h>
#include <linux/string_helpers.h>
#include <scsi/scsi_cmnd.h>
#include <linux/idr.h>
#include <linux/blk-mq.h>
#include <linux/numa.h>
#include <linux/time.h>
#include <linux/ioctl.h>

#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>

#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/Bitmap.h>

#define PART_BITS 4
#define VQ_NAME_LEN 16

#define SSD_BALLOON_UNIT 2046
#define SSD_BALLOON_INFLATION '-'
#define SSD_BALLOON_DEFLATION '+'
#define SSD_DM_CACHE_STATUS 's'
#define SSD_BLOCK_SIZE 2048  

#define BENCHMARKING 1
long long int front_resize_duration = 0; 
int resize_completed = 1;
struct virtio_vssd_resize_info *deflation_resize_info_pending;
struct virtio_vssd_resize_info *deflation_resize_info_pending_temp;

/*dm cache*/
// struct smq_policy *mq;
// struct policy_locker *locker;
unsigned long (*new_policy)(int count, int flag, int *buf, int round) = NULL;
EXPORT_SYMBOL(new_policy);
struct virtio_vssd *vssd_global;


#define FLAG_DO_NOT_ASK_ME 1
#define FLAG_ASK_ME_AGAIN 2

static int virtio_vssd_major = 0;

static DEFINE_IDA(vd_index_ida);

static struct workqueue_struct *virtVssd_wq;
static void virtVssd_config_changed(struct virtio_device *vdev);
static void send_resize_info_to_back_end(struct virtio_vssd *vssd, struct virtio_vssd_resize_info *resize_info);



struct virtio_vssd_vq 
{
	struct virtqueue *vq;
	spinlock_t lock;
	char name[VQ_NAME_LEN];
} ____cacheline_aligned_in_smp;

struct virtio_vssd 
{
	struct virtio_device *vdev;

	/* The disk structure for the kernel. */
	struct gendisk *disk;

	/* Block layer tags. */
	struct blk_mq_tag_set tag_set;

	/* Process context for config space updates */
	struct work_struct config_work;

	/* What host tells us, plus 2 for header & tailer. */
	unsigned int sg_elems;

	/* Ida index - used to track minor number allocations. */
	int index;

	/* num of vqs */
	int num_vqs;
	struct virtio_vssd_vq *vqs;

	//New varialbles
	__u64 capacity;			//only for book keeping.	in sectors
	// __u64 current_capacity;	//in sectors.
	// __u32 *block_list;		
	struct bitmap_t *allocated_bitmap;		// 0 : Physical block not allocated. 1 :  Phsyical block allocated
	struct bitmap_t *free_bitmap;			// 1 : Physical block allocated and logical blocks not allocated for any client
	//struct virtqueue *vq, *ctrl_vq;
	struct virtio_vssd_vq *ctrl_vq;
	spinlock_t q_lock/*, list_lock*/;


	unsigned int nvsdd_clients;
	struct vssd_client *vssd_clients;
};

struct virtio_vssd *vssd_global;

struct virtVssd_req 
{
	struct request *req;
	struct virtio_vssd_outhdr out_hdr;
	struct virtio_scsi_inhdr in_hdr;
	u8 status;
	u8 sense[SCSI_SENSE_BUFFERSIZE];
	struct scatterlist sg[];
};

struct virtio_vssd_resize_info 
{
	s32 status;
	s32 ack;
	u64 block_list[SSD_BALLOON_UNIT]; // Just less than a page length as there is a status field too here.

	//	New Variable
	s32 total_requested_blocks;
	s32 current_requested_blocks;
	s32 given_blocks;
	s32 remaining_blocks;
	char operation;			//		SSD_BALLOON_DEFLATION, SSD_BALLOON_INFLATION
	int flag;
};

struct resize_request_t
{
	int instance_id;
	unsigned long int request_size;
	unsigned long int given_size;
	char operation;         ////SSD_BALLOON_DEFLATION, SSD_BALLOON_INFLATION

	unsigned long int block_list[1];     //  bitmap
};
struct vssd_client
{
	struct vssd_client *next;

	int instance_id;
	int share;
	bool userspace_app;
	unsigned long long int *callback;
	unsigned long long int *arg;
	struct bitmap_t *allocated_bitmap;

	// bool resize_request_arrived;
	// bool list_of_free_blocks_ready;

	//temps 
	spinlock_t lock;
	bool message_ready;
	// bool block_list_ready;
	wait_queue_head_t driver_queue;
	wait_queue_head_t app_queue;

	struct resize_request_t *resize_req;
};

struct client_app_registration_t
{
	int instance_id;
	int total_instance;
};

struct client_app_reg_reply_t
{
	int instance_id;
	uint64_t vssd_block_size;
	uint64_t vssd_size;
};

struct client_block_allocation_t
{
	int  instance_id;
	uint64_t request_size;
	uint64_t given_size;

	struct bitmap_t bitmap;     // It should be at the end
};

/*	Unais New Functions start*/

#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define MIN3(a, b, c)	MIN(a, MIN(b, c))	
#define MAX3(a, b, c)	MAX(a, MAX(b, c))	
#define Sectors_to_blocks(a)	a / SSD_BLOCK_SIZE
#define Blocks_to_sectors(a)	a * SSD_BLOCK_SIZE

#define ASSERT(expr) \
	if(!(expr)) { \
		printk( "\n" __FILE__ ":%d: Assertion " #expr " failed!\n",__LINE__); \
		panic(#expr); \
	}

unsigned long long get_current_time(void)
{
	static unsigned long long min = 0;
	struct timeval cur_time;
	unsigned long long cur;

	do_gettimeofday(&cur_time);
	cur = cur_time.tv_sec*1000000 + cur_time.tv_usec;

	// if(min == 0)
	//   min = cur;

	return cur-min;
}



struct vssd_client *find_and_alloc_client(struct virtio_vssd *vssd, int instance_id, bool alloc)
{
	struct vssd_client *client;

	for(client = vssd->vssd_clients; client; client = client->next)
	{
		// printk(KERN_INFO"Instance: %d \n", client->instance_id);
		if(client->instance_id == instance_id)
		{
			// printk(KERN_INFO"Instance found \n");
			return client;
		}
	}

	if(!alloc)
		return NULL;

	client = (struct vssd_client *)kmalloc(sizeof(struct vssd_client), GFP_ATOMIC);
	client->instance_id = instance_id;
	client->resize_req = NULL;
	client->allocated_bitmap = NULL;

	client->message_ready = false;
	// client->block_list_ready = false;
	spin_lock_init(&client->lock);
	init_waitqueue_head(&client->driver_queue);
	init_waitqueue_head(&client->app_queue);

	client->next = vssd->vssd_clients;
	vssd->vssd_clients = client;
	vssd->nvsdd_clients++;

	return client;
}


/*********************************** DM cache ************************/
struct bitmap_t *
register_dm_cache(int instance_id)
{
	int i;
	struct vssd_client *client;
	struct virtio_vssd *vssd;

	vssd = vssd_global;

	client = find_and_alloc_client(vssd, instance_id, true);
	client->userspace_app = false;

	if(!client->allocated_bitmap)
	{
		printk(KERN_INFO "VSSD-A: \tFirst time client calling block allocation. \n");
		printk(KERN_INFO "VSSD-A: \tAllocating blocks for client");

		client->allocated_bitmap = Bitmap_create_and_initialize(vssd->free_bitmap->max_blocks, "dm-cache bitmap");
		ASSERT(client->allocated_bitmap != NULL);

		for(i = 0; i < vssd->free_bitmap->max_blocks; i++)
		{
			// Block is not free
			if(!Bitmap_is_set(vssd->free_bitmap, i)) 
				continue;

				// ith block is free
				// Bitmap_set(&alcn_reply->bitmap, i);
			Bitmap_set(client->allocated_bitmap, i);
			Bitmap_unset(vssd->free_bitmap, i);
		}
	}

	return client->allocated_bitmap;
	// bitmap = client->allocated_bitmap;

}
EXPORT_SYMBOL(register_dm_cache);


int send_message_to_backend(char *buf, int msg_size)
{
	struct virtio_vssd_resize_info *msg;
	struct virtio_vssd *vssd;

	vssd = vssd_global;

	msg = kmalloc(sizeof(*msg), GFP_ATOMIC);
	msg->operation = SSD_DM_CACHE_STATUS;
	msg->status = 0;			// todo
	msg->ack = 0;
	msg->current_requested_blocks = 0;
	msg->block_list[0] = msg_size;

	memcpy(msg->block_list + 1, buf, msg_size);

	send_resize_info_to_back_end(vssd, msg);	
	kfree(msg);	

	return 1;
}
EXPORT_SYMBOL(send_message_to_backend);
/********************************** Resize ***************************/
	static void 
send_resize_info_to_back_end(struct virtio_vssd *vssd, struct virtio_vssd_resize_info *resize_info)
{
	struct scatterlist *sglist[2], res_info;
	int out = 0, in = 0, error;
	int count = 10;

	sg_init_one(&res_info, resize_info, sizeof(*resize_info));
	sglist[in++] = &res_info;

	//	Inserting the resize_info into ctrl_queue and notifies the backend
	if(!vssd->ctrl_vq->vq)
	{
		printk(KERN_ALERT"VSSD-ERROR: vssd->ctrl_vq->vq  is null ?? \n");
		return ;
	}

	while(count > 0)
	{
		error = virtqueue_add_sgs(vssd->ctrl_vq->vq, sglist, out, in, resize_info, GFP_ATOMIC);
		if(error == 0)
			break;

		count--;
		mdelay(100);
	}

	if (error == 0)
	{
#if BENCHMARKING
		front_resize_duration += get_current_time();
#endif

		printk(KERN_INFO"VSSD: \tKicking to backend \n");
		virtqueue_kick(vssd->ctrl_vq->vq);
	}
	else
	{
		printk(KERN_ALERT "VSSD-ERROR: Failed to insert to control queue \n");
		printk(KERN_ALERT "VSSD-ERROR: Error code: %d  %d \n", error, -error);
	}
}

	static void 
send_inflation_reply_to_back_end(void)
{
	struct virtio_vssd *vssd;
	struct vssd_client *client;
	struct virtio_vssd_resize_info *resize_info;
	unsigned long int j;
	unsigned long int lbn;

	vssd = vssd_global;
	client = vssd->vssd_clients;

	resize_info = kmalloc(sizeof(*resize_info), GFP_ATOMIC);
	resize_info->operation = SSD_BALLOON_INFLATION;
	resize_info->status = client->resize_req->request_size;			// todo
	resize_info->ack = 0;
	resize_info->current_requested_blocks = client->resize_req->request_size;

	printk("VSSD: Sending list of free blocks to back end. \n");
	printk("VSSD: Blocks asked for app: %lu. given by app: %lu \n", 
			client->resize_req->request_size, client->resize_req->given_size);

	for(j = 0; j < client->resize_req->given_size; j++)
	{
		lbn = client->resize_req->block_list[j];
		// printk(KERN_INFO "VSSD: lbn: %lu \n", lbn);

		ASSERT(!Bitmap_is_set(vssd->free_bitmap, lbn));
		Bitmap_unset(vssd->allocated_bitmap, lbn);

		resize_info->block_list[j] = lbn;
	}

	resize_info->given_blocks = client->resize_req->given_size;
	resize_info->remaining_blocks = resize_info->current_requested_blocks - resize_info->given_blocks;

	printk("VSSD: Request size: %d. Given: %d \n", 
			resize_info->current_requested_blocks, resize_info->given_blocks);

	if(resize_info->given_blocks < resize_info->current_requested_blocks && resize_info->given_blocks < SSD_BALLOON_UNIT)
		resize_info->flag = FLAG_DO_NOT_ASK_ME;
	else
		resize_info->flag = FLAG_ASK_ME_AGAIN;


	kfree(client->resize_req);
	printk(KERN_INFO "VSSD: client resize req freed \n");
	client->resize_req = NULL;

	send_resize_info_to_back_end(vssd, resize_info);	
	kfree(resize_info);
}


	static void 
virtio_vssd_free_blocks(struct virtio_vssd *vssd,  unsigned long int count) // struct virtio_vssd_resize_info *resize_info) 
{
	int *buf;
	unsigned long int i,j;
	struct vssd_client *client;
	struct virtio_vssd_resize_info *resize_info;
	u64 total_blocks;

	static int round = -1;


	printk(KERN_INFO "VSSD: virtio_vssd_free_blocks \n");
	printk(KERN_INFO "VSSD: \tFree %lu blocks \n", count);

	total_blocks = MIN3(SSD_BALLOON_UNIT, vssd->allocated_bitmap->nset, count);

	printk(KERN_INFO "VSSD: \t Free blocks in free Bitmap: %lu. current_capacity: %lu \n",
			vssd->free_bitmap->nset, vssd->allocated_bitmap->nset);

	if(vssd->free_bitmap->nset > 0 || total_blocks == 0 )
	{
		printk(KERN_INFO "VSSD: \tFreeing %llu blocks from free list \n", total_blocks);
		resize_info = kmalloc(sizeof(*resize_info), GFP_ATOMIC);
		resize_info->operation = SSD_BALLOON_INFLATION;
		resize_info->status = count;			
		resize_info->ack = 0;
		resize_info->current_requested_blocks = count;

		for(i =0, j = 0; i < vssd->free_bitmap->max_blocks && j < total_blocks; i++)
		{
			if(Bitmap_is_set(vssd->free_bitmap, i))
			{
				Bitmap_unset(vssd->free_bitmap, i);
				Bitmap_unset(vssd->allocated_bitmap, i);

				resize_info->block_list[j++] = i;
			}
		}
		resize_info->given_blocks = j; 

		if(vssd->allocated_bitmap->nset > 0)
			resize_info->flag = FLAG_ASK_ME_AGAIN;
		else
			resize_info->flag = FLAG_DO_NOT_ASK_ME;		// todo

		send_resize_info_to_back_end(vssd, resize_info);	
		kfree(resize_info);

		return;
	}

	client = vssd->vssd_clients;
	ASSERT(client != NULL);

	if(client->userspace_app)
	{
		printk("VSSD: \tThread 0 trying to lock \n");
		spin_lock(&client->lock);
		printk("VSSD: \tThread 0  Lock OK !!\n");

		if(client->resize_req)
		{
			printk(KERN_ALERT "VSSD-ERROR: Previous resize operation not completed \n");
			return;
		}

		client->resize_req = (struct resize_request_t *)kmalloc(sizeof(struct resize_request_t) + total_blocks * sizeof(unsigned long int), GFP_ATOMIC);
		if(!client->resize_req)
			printk(KERN_ALERT "VSSD: Memory allocation failed for resize req \n");

		client->resize_req->instance_id = client->instance_id;
		client->resize_req->request_size = total_blocks;		// Request to the appplication
		client->resize_req->given_size = 0;
		client->resize_req->operation = SSD_BALLOON_INFLATION;

		client->message_ready = true;
		wake_up(&client->app_queue);

		spin_unlock(&client->lock);

		printk(KERN_INFO "VSSD: \t'virtio_vssd_free_blocks' completed \n");
	}
	else
	{
		/*ADDED BY PRAFULL....BEGIN ADD..*/
		//	printk(KERN_INFO "VSSD: \tFreeing %llu blocks from free list \n", total_blocks);
		resize_info = kmalloc(sizeof(*resize_info), GFP_ATOMIC);
		resize_info->operation = SSD_BALLOON_INFLATION;
		resize_info->status = count;			
		resize_info->ack = 0;
		resize_info->current_requested_blocks = count;
		buf = kzalloc(total_blocks*sizeof(int),GFP_KERNEL);

		// unsigned long excess = 0;
		unsigned long int ret = 0;
		if(new_policy != NULL)
		{

			if(count == 4096)
				round ++;

			printk(KERN_ALERT "VSSD: calling hook \n");
			ret = (*new_policy)(total_blocks, 1, buf, round); 
			// excess = (*new_policy)(total_blocks, 1, buf);      
		}       	

	//	spin_lock_irq(&vssd->ctrl_vq->lock);	
		for(i=0, j=0; i<total_blocks && i < SSD_BALLOON_UNIT; i++)
		{
			if(buf[i] == -1)
				break;

			ASSERT(!Bitmap_is_set(vssd->free_bitmap, buf[i]));
			Bitmap_unset(vssd->allocated_bitmap, buf[i]);
			resize_info->block_list[j++] = buf[i];
		}

		// Bitmap_print(vssd->allocated_bitmap);
		// Bitmap_print(vssd->allocated_bitmap);

	//	spin_unlock_irq(&vssd->ctrl_vq->lock);
		printk(KERN_ALERT"Invalidated Mappings..\n");

		resize_info->given_blocks = j; 

		if(vssd->allocated_bitmap->nset > 0 && j == total_blocks)
			resize_info->flag = FLAG_ASK_ME_AGAIN;
		else
			resize_info->flag = FLAG_DO_NOT_ASK_ME;		// todo

		send_resize_info_to_back_end(vssd, resize_info);	
		kfree(resize_info);


		/*ADDED BY PRAFULL....END ADD..*/	
	}
}

	static void 
virtio_vssd_resize_query(struct virtio_vssd *vssd, s32 status, s32 ack) 
{
	struct virtio_vssd_resize_info *resize_info;

	printk(KERN_INFO"VSSD: virtio_vssd_resize_query\n");

	if(status < 0) 
	{
		//	Inflation request.
		printk(KERN_INFO"VSSD: \tOperataion : Inflation. Status = %d \n", status);
		virtio_vssd_free_blocks(vssd, abs(status));
	}
	else
	{
		// Deflation
		resize_info = kmalloc(sizeof(*resize_info), GFP_ATOMIC);
		resize_info->status = status;
		resize_info->ack = ack;

		printk(KERN_INFO"VSSD: \tOperataion : Deflation. Status = %d \n", status);
		resize_info->operation = SSD_BALLOON_DEFLATION;
		resize_info->current_requested_blocks =  abs(status);
		resize_info->given_blocks = 0;


		if(vssd->vssd_clients && vssd->vssd_clients->userspace_app && vssd->vssd_clients->resize_req)
		{
			deflation_resize_info_pending = resize_info;
			printk("VSSD: \tPrevious request not completed. App thread will send resize info \n");
		}
		else
		{
			deflation_resize_info_pending = NULL;
			printk("VSSD: \tPrevious request completed. Idle thread sends resize info \n");
			send_resize_info_to_back_end(vssd, resize_info);
		}

	}
}

void ABC(struct resize_request_t *p)
{
	if(p != NULL)
	{
		mdelay(1000);
		ABC(p);
	}
}

	static void 
virtio_vssd_resize_callback(struct virtqueue *vq) 
{
	int *buf;
	uint64_t lbn;
	uint64_t j;
	unsigned int len;
	struct virtio_vssd *vssd = vq->vdev->priv;
	struct virtio_vssd_resize_info *resize_info;
	struct vssd_client *client;

	printk(KERN_INFO"VSSD-CALLBACK: virtio_vssd_resize_callback \n");

	do {
		virtqueue_disable_cb(vq);
		while ((resize_info = virtqueue_get_buf(vq, &len)) != NULL) 
		{
			if(resize_info->operation == SSD_BALLOON_DEFLATION)
			{
#if BENCHMARKING
				front_resize_duration -= get_current_time();
#endif

				// printk(KERN_INFO "VSSD-CALLBACK: \t Pid: %d. Tgid: %d \n", current->pid, current->tgid);
				printk(KERN_INFO "VSSD-CALLBACK: \tRequested : %d, given=%d \n", 
						resize_info->current_requested_blocks, resize_info->given_blocks);

				client = vssd->vssd_clients;

				if(!client)
				{
					for(j=0; j < resize_info->given_blocks; j++)
					{
						lbn = resize_info->block_list[j];

						Bitmap_set(vssd->allocated_bitmap, lbn);
						Bitmap_set(vssd->free_bitmap, lbn);
					}
				}
				else if(client->userspace_app)
				{
					printk(KERN_INFO "VSSD-CALLBACK: Its for fatcache \n");
					// ABC(client->resize_req);
					// while(client->resize_req)
					// {
					// 	mdelay(50);
					// 	mdelay(50);
					// }
					printk(KERN_INFO "VSSD-CALLBACK: Aquiring lock \n");

					spin_lock(&vssd->vssd_clients->lock);
					printk(KERN_INFO "VSSD-CALLBACK: Got the lock :) \n");

					if(client->resize_req)
						printk(KERN_ALERT "Client Resize req should be null here \n");

					client->resize_req = (struct resize_request_t *)kmalloc( sizeof(struct resize_request_t) + 
							resize_info->given_blocks * sizeof(unsigned long int), GFP_ATOMIC);

					if(!client->resize_req)
						printk(KERN_ALERT "VSSD-CALLBACK: Memory allocation failed for resize req \n");

					client->resize_req->instance_id = client->instance_id;
					client->resize_req->request_size = resize_info->given_blocks;
					client->resize_req->given_size = resize_info->given_blocks;
					client->resize_req->operation = SSD_BALLOON_DEFLATION;

					for(j=0; j < resize_info->given_blocks; j++)
					{
						lbn = resize_info->block_list[j];

						ASSERT(!Bitmap_is_set(vssd->free_bitmap, lbn));
						Bitmap_set(vssd->allocated_bitmap, lbn);

						client->resize_req->block_list[j] = lbn;
					}

					client->message_ready = true;
					spin_unlock(&client->lock);

					wake_up(&client->app_queue);
					printk(KERN_INFO "VSSD-CALLBACK: Message ready for app, and woke the app thead up \n");

				}
				else
				{
					buf = kzalloc(resize_info->given_blocks*sizeof(int),GFP_KERNEL);
					printk("VSSD: \tRequested : %d, given=%d \n", 
							resize_info->current_requested_blocks, resize_info->given_blocks);
					
					for(j=0; j<resize_info->given_blocks; j++)
					{
						lbn = resize_info->block_list[j];
						buf[j] = lbn;
						ASSERT(!Bitmap_is_set(vssd->free_bitmap, lbn));
						Bitmap_set(vssd->allocated_bitmap, lbn);
					}

					if(new_policy!=NULL)
					{
						printk(KERN_ALERT "VSSD: calling hook \n");
						(*new_policy)(resize_info->given_blocks, 0, buf, 0);      
					}
					printk(KERN_ALERT "VSSD: Dm-cache deflation completed \n");

					// Bitmap_print(vssd->allocated_bitmap);
					// Bitmap_print(vssd->allocated_bitmap);

					// kfree(resize_info);
				}

#if BENCHMARKING
				front_resize_duration += get_current_time();
#endif

				printk(KERN_INFO "VSSD-CALLBACK: Deflation callback completed\n");

				virtVssd_config_changed(vssd->vdev);

			}
			else if(resize_info->operation == SSD_BALLOON_INFLATION)
			{
				virtqueue_enable_cb(vq);
				printk(KERN_INFO "VSSD-CALLBACK: Inflation callback completed .\n");
				// kfree(resize_info);
				virtVssd_config_changed(vssd->vdev);
				return;
				// printk("Thank you \n");
				// return;
			}
			else if(resize_info->operation == SSD_DM_CACHE_STATUS)
			{
				virtqueue_enable_cb(vq);
				printk(KERN_INFO "VSSD-CALLBACK: Dm-cahe status callback completed .\n");
				// kfree(resize_info);
				virtVssd_config_changed(vssd->vdev);
				return;
				// printk("Thank you \n");
				// return;
			}

			kfree(resize_info);
		}

		if (unlikely(virtqueue_is_broken(vq))) {
			printk(KERN_ALERT "virtio_vssd: virtqueue is broken\n");
			break;
		}
	} while (!virtqueue_enable_cb(vq));

	j = 0;
}


/******************** Clients  - IOCTL *******************/

static int my_open(struct inode *i, struct file *f)
{
	printk(KERN_INFO"VSSD-A: /dev/query opened \n");
	if(i)
	{
		printk(KERN_INFO"VSSD-A: \tInode: %lu \n", i->i_ino);
	}

	return 0;
}

static int my_close(struct inode *i, struct file *f)
{
	return 0;
}

static long my_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	char *buf;
	unsigned long int i;
	struct virtio_vssd *vssd;
	struct client_app_registration_t reg;
	struct client_app_reg_reply_t reply; 
	struct client_block_allocation_t alcn;
	struct client_block_allocation_t *alcn_reply;
	struct vssd_client *client;
	struct resize_request_t resize_req;

	// #if BENCHMARKING
	// 	static int end_of_trn = 0;
	// #endif

	vssd = vssd_global;

	switch (cmd)
	{
		case 25:
			// Receving registration request
			if (copy_from_user(&reg, (struct clicxent_app_registration_t *)arg, sizeof(struct client_app_registration_t)))
				return -EACCES;
			printk(KERN_INFO "VSSD-A: Client app registration \n");
			printk(KERN_INFO "VSSD-A: \tApplication instance id: %d \n", reg.instance_id);
			printk(KERN_INFO "VSSD-A: \tTotal number of instances: %d \n", reg.total_instance);
			printk(KERN_INFO "VSSD-A: \t Pid: %d. Tgid: %d \n", current->pid, current->tgid);

			// Initiating new vssd client
			client = find_and_alloc_client(vssd, reg.instance_id, true);
			vssd->vssd_clients->userspace_app = true;

			client->resize_req = NULL;			
			client->message_ready = false;

			spin_lock_init(&client->lock);
			init_waitqueue_head(&client->driver_queue);
			init_waitqueue_head(&client->app_queue);

			// Sending registration reply
			reply.instance_id = client->instance_id;
			reply.vssd_block_size = SSD_BLOCK_SIZE;
			reply.vssd_size = Sectors_to_blocks(vssd->capacity);

			if (copy_to_user((struct client_app_reg_reply_t *)arg, &reply, sizeof(struct client_app_reg_reply_t)))
				return -EACCES;    
			break;

		case 26:
			// Block allocation
			if (copy_from_user(&alcn, (struct  client_block_allocation_t*)arg, sizeof(struct client_block_allocation_t)))
				return -EACCES;
			printk(KERN_INFO"VSSD-A: Block allocation request received \n");
			printk(KERN_INFO"VSSD-A: \tBlocks requested: %llu \n", alcn.request_size);

			client = find_and_alloc_client(vssd, alcn.instance_id, false);
			ASSERT(client != NULL);
			printk(KERN_INFO "VSSD-A: Client: %d \n", client->instance_id);

			if(!client->allocated_bitmap)
			{
				printk(KERN_INFO "VSSD-A: \tFirst time client calling block allocation. \n");
				printk(KERN_INFO "VSSD-A: \tAllocating blocks for client");

				client->allocated_bitmap = Bitmap_create_and_initialize(vssd->free_bitmap->max_blocks, "Client bitmap");
				ASSERT(client->allocated_bitmap != NULL);

				for(i = 0; i < vssd->free_bitmap->max_blocks; i++)
				{
					// Block is not free
					if(!Bitmap_is_set(vssd->free_bitmap, i)) 
						continue;

					// ith block is free
					// Bitmap_set(&alcn_reply->bitmap, i);
					Bitmap_set(client->allocated_bitmap, i);
					Bitmap_unset(vssd->free_bitmap, i);
				}
			}

			buf = (char *)kmalloc(sizeof(struct client_block_allocation_t) + 
					client->allocated_bitmap->size_in_bytes, GFP_ATOMIC);

			alcn_reply = (struct client_block_allocation_t *)buf;
			memcpy(alcn_reply, &alcn, sizeof(struct client_block_allocation_t));
			memcpy(&alcn_reply->bitmap, client->allocated_bitmap, sizeofBitmap(client->allocated_bitmap));
			alcn_reply->given_size = client->allocated_bitmap->nset;

			printk(KERN_INFO "VSSD-A: Given size: %llu. repyl bitmap.nset: %lu. free bitmap.nset: %lu\t", 
					alcn_reply->given_size, alcn_reply->bitmap.nset, vssd->free_bitmap->nset);

			if (copy_to_user((char *)arg, buf, 
						sizeof(struct client_block_allocation_t) + alcn_reply->bitmap.size_in_bytes))
				return -EACCES;   

			printk(KERN_INFO"VSSD-A: \tCompleted \n");
			kfree(buf);
			// Bitmap_print(vssd->free_bitmap);
			break;


		case 27:
			//	Listener

			// #if BENCHMARKING
			// 	if(end_of_trn == 1)
			// 		front_resize_duration += get_current_time();
			// 	end_of_trn = 0;
			// #endif

			printk(KERN_INFO "VSSD-A: Client resize waiting \n");
			// printk(KERN_INFO "VSSD-A: \tPid: %d. Tgid: %d \n", current->pid, current->tgid);
			// printk(KERN_INFO "VSSD-A: \tsizeof resize-req: %lu \n", sizeof(struct resize_request_t));
			if (copy_from_user(&resize_req, (struct  resize_request_t*)arg, sizeof(struct resize_request_t)))
				return -EACCES;
			client = find_and_alloc_client(vssd, resize_req.instance_id, false);

			spin_lock(&client->lock);

			while(!client->message_ready)
			{	
				printk(KERN_INFO "VSSD-A: Message not ready \n");
				spin_unlock(&client->lock);

				wait_event(client->app_queue, client->message_ready);
				spin_lock(&client->lock);
			}

			// #if BENCHMARKING
			// 	if(client->resize_req->operation == SSD_BALLOON_DEFLATION)
			// 	{
			// 		front_resize_duration -= get_current_time();
			// 		end_of_trn = 1;
			// 	}
			// #endif

			printk(KERN_INFO "VSSD-A: \tMessage ready \n");

			if (copy_to_user((struct resize_request_t*)arg, client->resize_req, sizeof(struct resize_request_t)))
			{
				printk(KERN_ALERT "VSSD-ERROR: copy_from_user failed. %d \n", __LINE__);
				return -EACCES;
			}

			printk(KERN_INFO "VSSD-A: \t27 returning \n ");

			// Returning to userspace with lock.
			// Lock will be released while handling Inflation or Deflation

			break;

		case 28:

			//	Inflation
			printk(KERN_INFO "VSSD-A: Inflation block list received \n");
			if (copy_from_user(&resize_req, (struct  resize_request_t*)arg, sizeof(struct resize_request_t)))
				return -EACCES;

			client = find_and_alloc_client(vssd, resize_req.instance_id, false);
			printk(KERN_INFO "VSSD-A: \tClient: %d. Given size: %lu \n", resize_req.instance_id, resize_req.given_size);

			if (copy_from_user(client->resize_req, (struct  resize_request_t*)arg, 
						sizeof(struct resize_request_t) + resize_req.request_size * sizeof(unsigned long int)))
			{
				printk(KERN_ALERT "VSSD-ERROR: copy_from_user failed \n");
				return -EACCES;
			}

			client->message_ready = false;
			spin_unlock(&client->lock);

			printk(KERN_INFO "VSSD-A: \tInfaltion reply from all clients have received \n");
			send_inflation_reply_to_back_end();
			printk(KERN_INFO "VSSD-A: \tInflation reply forwared to backend \n");

			break;

		case 29:
			//	Deflation
			printk(KERN_INFO "VSSD-A: Deflation. \n");
			if (copy_from_user(&resize_req, (struct  resize_request_t*)arg, sizeof(struct resize_request_t)))
				return -EACCES;

			client = find_and_alloc_client(vssd, resize_req.instance_id, false);

			if (copy_to_user((struct  resize_request_t*)arg, client->resize_req, 
						sizeof(struct resize_request_t) + client->resize_req->given_size * sizeof(unsigned long int)))
				return -EACCES;

			kfree(client->resize_req);
			client->resize_req = NULL;

			client->message_ready = false;

			deflation_resize_info_pending_temp = deflation_resize_info_pending;
			deflation_resize_info_pending = NULL;


			spin_unlock(&client->lock);

			if(deflation_resize_info_pending_temp)
			{
				printk(KERN_INFO "VSSD-A: \tForwarding next resize_info_pending to back end \n");
				send_resize_info_to_back_end(vssd, deflation_resize_info_pending_temp);
			}

			printk(KERN_INFO "VSSD-A: \tDeflation blocks are forwarded to app \n");


			// #if BENCHMARKING
			//  	end_time = get_current_time();
			//  	front_resize_duration += end_time - start_time;
			// #endif

			break;

		case 30:
			printk(KERN_INFO "VSSD-A: Application exit \n");
			if (copy_from_user(&resize_req, (struct  resize_request_t*)arg, sizeof(struct resize_request_t)))
				return -EACCES;
			client = find_and_alloc_client(vssd, resize_req.instance_id, false);

			spin_lock(&client->lock);
			client->resize_req = (struct resize_request_t *)kmalloc(sizeof(struct resize_request_t), GFP_ATOMIC);
			if(!client->resize_req)
				printk(KERN_ALERT "VSSD: Memory allocation failed for resize req \n");
			client->resize_req->operation = 'x';

			client->message_ready = true;

			spin_unlock(&client->lock);

			wake_up(&client->app_queue);


		default:
			return -EINVAL;


	}

	return 0;
}

static struct file_operations query_fops =
{
	.owner = THIS_MODULE,
	.open = my_open,
	.release = my_close,
	.unlocked_ioctl = my_ioctl
};

static dev_t dev;
static struct cdev c_dev;
static struct class *cl;
static int intialize_vssd_client(struct virtio_vssd *vssd)
{
	int ret;
	struct device *dev_ret;

	vssd->nvsdd_clients = 0;
	vssd->vssd_clients = NULL;

	if ((ret = alloc_chrdev_region(&dev, 0, 1, "query_ioctl")) < 0)
	{
		return ret;
	}

	cdev_init(&c_dev, &query_fops);
	if ((ret = cdev_add(&c_dev, dev, 1)) < 0)
	{
		return ret;
	}

	if (IS_ERR(cl = class_create(THIS_MODULE, "char")))
	{
		cdev_del(&c_dev);
		unregister_chrdev_region(dev, 1);
		return PTR_ERR(cl);
	}
	if (IS_ERR(dev_ret = device_create(cl, NULL, dev, NULL, "query")))
	{
		class_destroy(cl);
		cdev_del(&c_dev);
		unregister_chrdev_region(dev, 1);
		return PTR_ERR(dev_ret);
	}

	return 1;
}

/************************************************************/

static int virtio_vssd_setup_resize(struct virtio_vssd *vssd)
{
	uint64_t size_in_blocks;
	uint64_t offset;
	int i;
	uint32_t value;
	uint64_t current_capacity;

	virtio_cread(vssd->vdev, struct virtio_vssd_config, capacity, &vssd->capacity);
	virtio_cread(vssd->vdev, struct virtio_vssd_config, current_capacity, &current_capacity);

	size_in_blocks = vssd->capacity / SSD_BLOCK_SIZE;

	vssd->allocated_bitmap = Bitmap_create_and_initialize(size_in_blocks, "Allocated blocks");
	printk(KERN_INFO "Sizeof struct bitmap_t: %lu \n", sizeof(struct bitmap_t));

	printk(KERN_INFO"VSSD: virtio_vssd_setup_resize \n");
	printk(KERN_INFO"VSSD: \tCapacity: %lld blocks. Current capacity: %lld blocks \n", 
			size_in_blocks, Sectors_to_blocks(current_capacity));


	// Reading bitmap from back end
	printk("VSSD: \tReading bitmap from backend \n");
	virtio_cwrite32(vssd->vdev, 
			offsetof(struct virtio_vssd_config, bitmap_offset), 0);	
	virtio_cwrite32(vssd->vdev, 
			offsetof(struct virtio_vssd_config, bitmap_reading), 1);	//	Yes, front end reading the bitmap

	offset = 0;
	while(offset < size_in_blocks / 32)
	{
		virtio_cread(vssd->vdev, struct virtio_vssd_config, bitmap_value, &value);

		for(i = 0; i<32; i++)
		{
			if((value & (1 << (i%32))) == 0)			// Backend  0 = set
				Bitmap_set(vssd->allocated_bitmap, offset * 32 + i);
		}
		offset ++;
	}

	vssd->free_bitmap = Bitmap_duplicate(vssd->allocated_bitmap, "Free blocks");

	virtio_cwrite32(vssd->vdev, 
			offsetof(struct virtio_vssd_config, bitmap_offset), 0);
	virtio_cwrite32(vssd->vdev, 
			offsetof(struct virtio_vssd_config, bitmap_reading), 0);

	ASSERT(Sectors_to_blocks(current_capacity) == vssd->allocated_bitmap->nset); 

	// Bitmap_print(vssd->allocated_bitmap);
	// Bitmap_print(vssd->free_bitmap);

	// Intializing vssd clients
	vssd_global = vssd;
	intialize_vssd_client(vssd);

	return 1;
}


static bool virtio_vssd_request_valid(struct request *req, struct virtio_vssd *vssd) 
{
	struct bio *bio;
	struct bio_vec bvec;
	struct bvec_iter iter;
	u64 sector_num, lbn, start_block_num, end_block_num;

	// uint64_t bio_count=0;

	bio = req->bio;

	/*printk(KERN_INFO"VSSD: Verifying bio requests \n");*/
	for_each_bio(bio) 
	{
		/*printk(KERN_INFO"VSSD: \tbio=%d. No. of iovs=%d. Max-iovs-in-a-bio=%d \n",
		  bio_count++, bio->bi_vcnt, bio->bi_max_vecs);*/

		// uint64_t iov_count=0;
		bio_for_each_segment(bvec, bio, iter) 
		{
			//printk(KERN_ALERT "virtio_vssd: BIO Flags: %u\n", bio_flags(bio) & BIO_NULL_MAPPED);

			sector_num = iter.bi_sector;
			start_block_num = sector_num / SSD_BLOCK_SIZE;
			end_block_num = (sector_num + iter.bi_size / 512) / SSD_BLOCK_SIZE;
			if((sector_num + iter.bi_size / 512) % SSD_BLOCK_SIZE == 0)
				end_block_num--;

			// if(start_block_num > 5119)
			// 	 printk(KERN_ALERT "VSSD: \t\tiov=%llu.  sector_num=%lu. len=%uB[%us]. start=%llu. end=%llu \n", 
			// 	 	iov_count++, iter.bi_sector, iter.bi_size, iter.bi_size/512, start_block_num, end_block_num);

			ASSERT(end_block_num < Sectors_to_blocks(vssd->capacity));

			for(lbn = start_block_num; lbn <= end_block_num; lbn++)
			{
				// int valid=1;
				if(!Bitmap_is_set(vssd->allocated_bitmap, lbn)) {
					printk(KERN_ALERT, "\n\nVSSD-ERROR: lbn %llu is not valid \n\n\n",lbn);
					return false;
				}

				// if((list[block_num/32] & (1 << (block_num % 32))) == (1 << (block_num % 32))) 
				// {
				// 	//printk(KERN_ALERT "virtio_vssd: BIO Flags: %d\n", bio_flags(bio) | ~BIO_SEG_VALID);
				// 	//invalid_block = true;
				// 	//printk(KERN_ALERT "virtio_vssd: Sector number: %lu\n", iter.bi_sector);
				// 	//blk_start_request(req); // This is necessary, otherwise it BUGs on blk_queued_req(req)
				// 	//__blk_end_request_all(req, -EIO);
				// 	//bio->bi_error = -EIO;
				// 	// return false;
				// }

				/*printk(KERN_INFO"VSSD: \t\t\tBlock num %ld. valid=%d \n", block_num, valid);*/
			}
		}
	}
	return true;
}

/*New Functions ENd */

static inline int virtVssd_result(struct virtVssd_req *vbr)
{
	switch (vbr->status) {
		case VIRTIO_VSSD_S_OK:
			return 0;
		case VIRTIO_VSSD_S_UNSUPP:
			return -ENOTTY;
		default:
			return -EIO;
	}
}

static int __virtVssd_add_req(struct virtqueue *vq,
		struct virtVssd_req *vbr,
		struct scatterlist *data_sg,
		bool have_data)
{
	struct scatterlist hdr, status, cmd, sense, inhdr, *sgs[6];
	unsigned int num_out = 0, num_in = 0;
	__virtio32 type = vbr->out_hdr.type & ~cpu_to_virtio32(vq->vdev, VIRTIO_VSSD_T_OUT);

	sg_init_one(&hdr, &vbr->out_hdr, sizeof(vbr->out_hdr));
	sgs[num_out++] = &hdr;

	/*
	 * If this is a packet command we need a couple of additional headers.
	 * Behind the normal outhdr we put a segment with the scsi command
	 * block, and before the normal inhdr we put the sense data and the
	 * inhdr with additional status information.
	 */
	if (type == cpu_to_virtio32(vq->vdev, VIRTIO_VSSD_T_SCSI_CMD)) {
		sg_init_one(&cmd, vbr->req->cmd, vbr->req->cmd_len);
		sgs[num_out++] = &cmd;
	}

	if (have_data) {
		if (vbr->out_hdr.type & cpu_to_virtio32(vq->vdev, VIRTIO_VSSD_T_OUT))
			sgs[num_out++] = data_sg;
		else
			sgs[num_out + num_in++] = data_sg;
	}

	if (type == cpu_to_virtio32(vq->vdev, VIRTIO_VSSD_T_SCSI_CMD)) {
		memcpy(vbr->sense, vbr->req->sense, SCSI_SENSE_BUFFERSIZE);
		sg_init_one(&sense, vbr->sense, SCSI_SENSE_BUFFERSIZE);
		sgs[num_out + num_in++] = &sense;
		sg_init_one(&inhdr, &vbr->in_hdr, sizeof(vbr->in_hdr));
		sgs[num_out + num_in++] = &inhdr;
	}

	sg_init_one(&status, &vbr->status, sizeof(vbr->status));
	sgs[num_out + num_in++] = &status;

	return virtqueue_add_sgs(vq, sgs, num_out, num_in, vbr, GFP_ATOMIC);
}

static inline void virtVssd_request_done(struct request *req)
{
	struct virtVssd_req *vbr = blk_mq_rq_to_pdu(req);
	struct virtio_vssd *vblk = req->q->queuedata;
	int error = virtVssd_result(vbr);

	if (req->cmd_type == REQ_TYPE_BLOCK_PC) {
		req->resid_len = virtio32_to_cpu(vblk->vdev, vbr->in_hdr.residual);
		req->sense_len = virtio32_to_cpu(vblk->vdev, vbr->in_hdr.sense_len);
		req->errors = virtio32_to_cpu(vblk->vdev, vbr->in_hdr.errors);
	} else if (req->cmd_type == REQ_TYPE_DRV_PRIV) {
		req->errors = (error != 0);
	}

	blk_mq_end_request(req, error);
}

static void virtVssd_done(struct virtqueue *vq)
{
	struct virtio_vssd *vblk = vq->vdev->priv;
	bool req_done = false;
	int qid = vq->index;
	struct virtVssd_req *vbr;
	unsigned long flags;
	unsigned int len;

	spin_lock_irqsave(&vblk->vqs[qid].lock, flags);
	do {
		virtqueue_disable_cb(vq);
		while ((vbr = virtqueue_get_buf(vblk->vqs[qid].vq, &len)) != NULL) {

			//do_gettimeofday(&(vbr->times.cs));

			blk_mq_complete_request(vbr->req, vbr->req->errors);
			req_done = true;

			//do_gettimeofday(&(vbr->times.ce));
			/*printk(KERN_ALERT "virtio: Request=%ld us, Backend=%ld us, Completed=%ld us, Total=%ld us \n", 
			  interval(vbr->times.rs, vbr->times.re),
			  interval(vbr->times.re, vbr->times.cs),
			  interval(vbr->times.cs, vbr->times.ce),
			  interval(vbr->times.rs, vbr->times.ce));*/


		}
		if (unlikely(virtqueue_is_broken(vq)))
			break;
	} while (!virtqueue_enable_cb(vq));

	/* In case queue is stopped waiting for more buffers. */
	if (req_done)
		blk_mq_start_stopped_hw_queues(vblk->disk->queue, true);
	spin_unlock_irqrestore(&vblk->vqs[qid].lock, flags);
}

static int virtio_queue_rq(struct blk_mq_hw_ctx *hctx,
		const struct blk_mq_queue_data *bd)
{
	//struct timeval t0;
	//do_gettimeofday(&t0);

	struct virtio_vssd *vblk = hctx->queue->queuedata;
	struct request *req = bd->rq;
	struct virtVssd_req *vbr = blk_mq_rq_to_pdu(req);
	unsigned long flags;
	unsigned int num;
	int qid = hctx->queue_num;
	int err;
	bool notify = false;

	BUG_ON(req->nr_phys_segments + 2 > vblk->sg_elems);

	/*Unais start*/
	if(!virtio_vssd_request_valid(req, vblk))
	{ 
		return BLK_MQ_RQ_QUEUE_ERROR;
	}

	/*End*/

	vbr->req = req;
	if (req_op(req) == REQ_OP_FLUSH) 
	{
		vbr->out_hdr.type = cpu_to_virtio32(vblk->vdev, VIRTIO_VSSD_T_FLUSH);
		vbr->out_hdr.sector = 0;
		vbr->out_hdr.ioprio = cpu_to_virtio32(vblk->vdev, req_get_ioprio(vbr->req));
	} 
	else 
	{
		switch (req->cmd_type) 
		{
			case REQ_TYPE_FS:
				vbr->out_hdr.type = 0;
				vbr->out_hdr.sector = cpu_to_virtio64(vblk->vdev, blk_rq_pos(vbr->req));
				vbr->out_hdr.ioprio = cpu_to_virtio32(vblk->vdev, req_get_ioprio(vbr->req));
				break;
			case REQ_TYPE_BLOCK_PC:
				vbr->out_hdr.type = cpu_to_virtio32(vblk->vdev, VIRTIO_VSSD_T_SCSI_CMD);
				vbr->out_hdr.sector = 0;
				vbr->out_hdr.ioprio = cpu_to_virtio32(vblk->vdev, req_get_ioprio(vbr->req));
				break;
			case REQ_TYPE_DRV_PRIV:
				vbr->out_hdr.type = cpu_to_virtio32(vblk->vdev, VIRTIO_VSSD_T_GET_ID);
				vbr->out_hdr.sector = 0;
				vbr->out_hdr.ioprio = cpu_to_virtio32(vblk->vdev, req_get_ioprio(vbr->req));
				break;
			default:
				/* We don't put anything else in the queue. */
				BUG();
		}
	}

	blk_mq_start_request(req);

	num = blk_rq_map_sg(hctx->queue, vbr->req, vbr->sg);
	if (num) 
	{
		if (rq_data_dir(vbr->req) == WRITE)
			vbr->out_hdr.type |= cpu_to_virtio32(vblk->vdev, VIRTIO_VSSD_T_OUT);
		else
			vbr->out_hdr.type |= cpu_to_virtio32(vblk->vdev, VIRTIO_VSSD_T_IN);
	}

	spin_lock_irqsave(&vblk->vqs[qid].lock, flags);
	err = __virtVssd_add_req(vblk->vqs[qid].vq, vbr, vbr->sg, num);
	if (err) 
	{
		virtqueue_kick(vblk->vqs[qid].vq);
		blk_mq_stop_hw_queue(hctx);
		spin_unlock_irqrestore(&vblk->vqs[qid].lock, flags);
		/* Out of mem doesn't actually happen, since we fall back
		 * to direct descriptors */
		if (err == -ENOMEM || err == -ENOSPC)
			return BLK_MQ_RQ_QUEUE_BUSY;
		return BLK_MQ_RQ_QUEUE_ERROR;
	}

	if (bd->last && virtqueue_kick_prepare(vblk->vqs[qid].vq))
		notify = true;
	spin_unlock_irqrestore(&vblk->vqs[qid].lock, flags);

	if (notify)
		virtqueue_notify(vblk->vqs[qid].vq);

	//do_gettimeofday(&(vbr->times.re));


	return BLK_MQ_RQ_QUEUE_OK;
}

/* return id (s/n) string for *disk to *id_str
 */
static int virtVssd_get_id(struct gendisk *disk, char *id_str)
{
	struct virtio_vssd *vblk = disk->private_data;
	struct request_queue *q = vblk->disk->queue;
	struct request *req;
	int err;

	req = blk_get_request(q, READ, GFP_KERNEL);
	if (IS_ERR(req))
		return PTR_ERR(req);
	req->cmd_type = REQ_TYPE_DRV_PRIV;

	err = blk_rq_map_kern(q, req, id_str, VIRTIO_VSSD_ID_BYTES, GFP_KERNEL);
	if (err)
		goto out;

	err = blk_execute_rq(vblk->disk->queue, vblk->disk, req, false);
out:
	blk_put_request(req);
	return err;
}

static int virtVssd_ioctl(struct block_device *bdev, fmode_t mode,
		unsigned int cmd, unsigned long data)
{
	struct gendisk *disk = bdev->bd_disk;
	struct virtio_vssd *vssd = disk->private_data;

	/*
	 * Only allow the generic SCSI ioctls if the host can support it.
	 */
	if (!virtio_has_feature(vssd->vdev, VIRTIO_VSSD_F_SCSI))
		return -ENOTTY;

	return scsi_cmd_blk_ioctl(bdev, mode, cmd,
			(void __user *)data);
}

/* We provide getgeo only to please some old bootloader/partitioning tools */
static int virtVssd_getgeo(struct block_device *bd, struct hd_geometry *geo)
{
	struct virtio_vssd *vssd = bd->bd_disk->private_data;

	/* see if the host passed in geometry config */
	if (virtio_has_feature(vssd->vdev, VIRTIO_VSSD_F_GEOMETRY)) {
		virtio_cread(vssd->vdev, struct virtio_vssd_config,
				geometry.cylinders, &geo->cylinders);
		virtio_cread(vssd->vdev, struct virtio_vssd_config,
				geometry.heads, &geo->heads);
		virtio_cread(vssd->vdev, struct virtio_vssd_config,
				geometry.sectors, &geo->sectors);
	} else {
		/* some standard values, similar to sd */
		geo->heads = 1 << 6;
		geo->sectors = 1 << 5;
		geo->cylinders = get_capacity(bd->bd_disk) >> 11;
	}
	return 0;
}

static const struct block_device_operations virtVssd_fops = {
	.ioctl  = virtVssd_ioctl,
	.owner  = THIS_MODULE,
	.getgeo = virtVssd_getgeo,
};

static int index_to_minor(int index)
{
	return index << PART_BITS;
}

static int minor_to_index(int minor)
{
	return minor >> PART_BITS;
}

static ssize_t virtVssd_serial_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	int err;

	/* sysfs gives us a PAGE_SIZE buffer */
	BUILD_BUG_ON(PAGE_SIZE < VIRTIO_VSSD_ID_BYTES);

	buf[VIRTIO_VSSD_ID_BYTES] = '\0';
	err = virtVssd_get_id(disk, buf);
	if (!err)
		return strlen(buf);

	if (err == -EIO) /* Unsupported? Make it empty. */
		return 0;

	return err;
}

static DEVICE_ATTR(serial, S_IRUGO, virtVssd_serial_show, NULL);

static void virtVssd_config_changed_work(struct work_struct *work)
{
	struct virtio_vssd *vblk = 	container_of(work, struct virtio_vssd, config_work);
	struct virtio_device *vdev = vblk->vdev;
	struct request_queue *q = vblk->disk->queue;
	char cap_str_2[10], cap_str_10[10];
	char *envp[] = { "RESIZE=1", NULL };
	u64 capacity;


	printk(KERN_INFO"\n\nVSSD: virtVssd_config_changed_work \n");
	// Host must always specify the capacity. 
	virtio_cread(vdev, struct virtio_vssd_config, capacity, &capacity);

	printk(KERN_INFO"VSSD: \t Capacity = %lld \n", capacity);

	// If capacity is too big, truncate with warning. 
	if ((sector_t)capacity != capacity) {
		dev_warn(&vdev->dev, "Capacity %llu too large: truncating\n",
				(unsigned long long)capacity);
		capacity = (sector_t)-1;
	}

	string_get_size(capacity, queue_logical_block_size(q),
			STRING_UNITS_2, cap_str_2, sizeof(cap_str_2));
	string_get_size(capacity, queue_logical_block_size(q),
			STRING_UNITS_10, cap_str_10, sizeof(cap_str_10));

	dev_notice(&vdev->dev,
			"new size: %llu %d-byte logical blocks (%s/%s)\n",
			(unsigned long long)capacity,
			queue_logical_block_size(q),
			cap_str_10, cap_str_2);

	set_capacity(vblk->disk, capacity);
	revalidate_disk(vblk->disk);
	kobject_uevent_env(&disk_to_dev(vblk->disk)->kobj, KOBJ_CHANGE, envp);
}


static void virtVssd_config_changed(struct virtio_device *vdev)
{
	// Unais Already existing code to handle config. 		ToDoUnaisp  virtVssd_config_changed_work is required on config_change ?
	/*struct virtio_vssd *vblk = vdev->priv;
	  queue_work(virtVssd_wq, &vblk->config_work);*/

	/* UNais Start*/

#if BENCHMARKING
	unsigned long long int start_time = get_current_time();
	static unsigned int previous_request_id = -1;
	unsigned int current_request_id;
#endif

	struct virtio_vssd *vssd = vdev->priv;
	__le32 command;

	virtio_cread(vdev, struct virtio_vssd_config, command, &command);

#if BENCHMARKING
	virtio_cread(vdev, struct virtio_vssd_config, current_request_id, &current_request_id);
	if(current_request_id != previous_request_id)
	{	
		printk(KERN_INFO "LOG: Resize id:\t%d\t Time Taken:\t%lld\tCommand:\t%d\n", previous_request_id, front_resize_duration, command);
		front_resize_duration = 0;

		previous_request_id = current_request_id;
	}
#endif

	if(command != 0)
	{
#if BENCHMARKING
		front_resize_duration -= start_time;
#endif


		printk(KERN_INFO "VSSD:  \n");
		printk(KERN_INFO "VSSD:  \n");
		printk(KERN_INFO "VSSD: virtVssd_config_changed \n");
		printk(KERN_INFO "VSSD: \tRequested size: %d\n", command);
		virtio_vssd_resize_query(vssd, __le32_to_cpu(command), -1); // ack = -1 for new requests.
	}

	/* End */
}


/* Added by Bhavesh Singh. 2017.04.14. Begin add */
/* WARNING: Remove the #pragma GCC just below this function too if removing this */
#pragma GCC push_options
#pragma GCC optimize ("-O0")
/* Added by Bhavesh Singh. 2017.04.14. End add */

static int init_vq(struct virtio_vssd *vblk)
{
	int err;
	int i;
	vq_callback_t **callbacks;
	const char **names;
	struct virtqueue **vqs;
	unsigned short num_vqs;
	struct virtio_device *vdev = vblk->vdev;

	printk(KERN_INFO"VSSD: init_vq \n");

	err = virtio_cread_feature(vdev, VIRTIO_VSSD_F_MQ,
			struct virtio_vssd_config, num_queues,
			&num_vqs);

	if (err)
		num_vqs = 1;

	vblk->vqs = kmalloc_array(num_vqs, sizeof(*vblk->vqs), GFP_KERNEL);
	if (!vblk->vqs)
		return -ENOMEM;
	printk(KERN_INFO"VSSD: \tvblk->vqs = kmalloc_array done \n");

	names = kmalloc_array(num_vqs+1, sizeof(*names), GFP_KERNEL);
	callbacks = kmalloc_array(num_vqs+1, sizeof(*callbacks), GFP_KERNEL);
	vqs = kmalloc_array(num_vqs+1, sizeof(*vqs), GFP_KERNEL);
	if (!names || !callbacks || !vqs) {
		err = -ENOMEM;
		goto out;
	}

	printk(KERN_INFO"VSSD: \tMemory allocated for names, callbacks, and vqs \n");

	for (i = 0; i < num_vqs; i++) {
		callbacks[i] = virtVssd_done;
		snprintf(vblk->vqs[i].name, VQ_NAME_LEN, "req.%d", i);
		names[i] = vblk->vqs[i].name;
	}

	printk(KERN_INFO"VSSD: \tcallbacks, names filled. i = %d \n", i);

	/*Unais End Adding control queue*/
	vblk->ctrl_vq = kmalloc_array(num_vqs, sizeof(*vblk->vqs), GFP_KERNEL);

	callbacks[i] = virtio_vssd_resize_callback;
	strcpy(vblk->ctrl_vq->name, "resize_callback");;
	names[i] = vblk->ctrl_vq->name;
	/*Unais End*/

	printk(KERN_INFO"VSSD: \tresize vq done \n");

	/* Discover virtqueues and write information to configuration.  */
	err = vdev->config->find_vqs(vdev, num_vqs+1, vqs, callbacks, names);
	if (err)
		goto out;

	printk(KERN_INFO"VSSD: \tfind_vqs done \n");

	for (i = 0; i < num_vqs; i++) {
		spin_lock_init(&vblk->vqs[i].lock);
		vblk->vqs[i].vq = vqs[i];
	}
	vblk->num_vqs = num_vqs;

	printk(KERN_INFO"VSSD: \tQueues populated to vssd->vqs[].  i=%d \n", i);

	/*Unais End Adding control queue*/
	spin_lock_init(&vblk->ctrl_vq->lock);
	vblk->ctrl_vq->vq = vqs[i];
	/*Unais End*/

	printk(KERN_INFO"VSSD: \tQueues populated to vssd->ctrl_vq.  i=%d \n", i);

	printk(KERN_INFO"VSSD: \tinit_vq completed \n");

out:
	kfree(vqs);
	kfree(callbacks);
	kfree(names);
	if (err)
	{
		kfree(vblk->vqs);
		kfree(vblk->ctrl_vq);
	}
	return err;
}

/* Added by Bhavesh Singh. 2017.04.14. Begin add */
/* WARNING: Remove the #pragma GCC just above this function too if removing this */
#pragma GCC pop_options
/* Added by Bhavesh Singh. 2017.04.14. End add */

/*
 * Legacy naming scheme used for virtio devices.  We are stuck with it for
 * virtio blk but don't ever use it for any new driver.
 */
static int virtVssd_name_format(char *prefix, int index, char *buf, int buflen)
{
	const int base = 'z' - 'a' + 1;
	char *begin = buf + strlen(prefix);
	char *end = buf + buflen;
	char *p;
	int unit;

	p = end - 1;
	*p = '\0';
	unit = base;
	do {
		if (p == begin)
			return -EINVAL;
		*--p = 'a' + (index % unit);
		index = (index / unit) - 1;
	} while (index >= 0);

	memmove(begin, p, end - p);
	memcpy(buf, prefix, strlen(prefix));

	return 0;
}

static int virtVssd_get_cache_mode(struct virtio_device *vdev)
{
	u8 writeback;
	int err;

	err = virtio_cread_feature(vdev, VIRTIO_VSSD_F_CONFIG_WCE,
			struct virtio_vssd_config, wce,
			&writeback);

	/*
	 * If WCE is not configurable and flush is not available,
	 * assume no writeback cache is in use.
	 */
	if (err)
		writeback = virtio_has_feature(vdev, VIRTIO_VSSD_F_FLUSH);

	return writeback;
}

static void virtVssd_update_cache_mode(struct virtio_device *vdev)
{
	u8 writeback = virtVssd_get_cache_mode(vdev);
	struct virtio_vssd *vblk = vdev->priv;

	blk_queue_write_cache(vblk->disk->queue, writeback, false);
	revalidate_disk(vblk->disk);
}

static const char *const virtVssd_cache_types[] = {
	"write through", "write back"
};

	static ssize_t
virtVssd_cache_type_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct virtio_vssd *vblk = disk->private_data;
	struct virtio_device *vdev = vblk->vdev;
	int i;

	BUG_ON(!virtio_has_feature(vblk->vdev, VIRTIO_VSSD_F_CONFIG_WCE));
	for (i = ARRAY_SIZE(virtVssd_cache_types); --i >= 0; )
		if (sysfs_streq(buf, virtVssd_cache_types[i]))
			break;

	if (i < 0)
		return -EINVAL;

	virtio_cwrite8(vdev, offsetof(struct virtio_vssd_config, wce), i);
	virtVssd_update_cache_mode(vdev);
	return count;
}

	static ssize_t
virtVssd_cache_type_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct virtio_vssd *vblk = disk->private_data;
	u8 writeback = virtVssd_get_cache_mode(vblk->vdev);

	BUG_ON(writeback >= ARRAY_SIZE(virtVssd_cache_types));
	return snprintf(buf, 40, "%s\n", virtVssd_cache_types[writeback]);
}

static const struct device_attribute dev_attr_cache_type_ro =
__ATTR(cache_type, S_IRUGO,
		virtVssd_cache_type_show, NULL);
static const struct device_attribute dev_attr_cache_type_rw =
__ATTR(cache_type, S_IRUGO|S_IWUSR,
		virtVssd_cache_type_show, virtVssd_cache_type_store);

static int virtVssd_init_request(void *data, struct request *rq,
		unsigned int hctx_idx, unsigned int request_idx,
		unsigned int numa_node)
{
	struct virtio_vssd *vblk = data;
	struct virtVssd_req *vbr = blk_mq_rq_to_pdu(rq);

	sg_init_table(vbr->sg, vblk->sg_elems);
	return 0;
}

static struct blk_mq_ops virtio_mq_ops = {
	.queue_rq	= virtio_queue_rq,
	.complete	= virtVssd_request_done,
	.init_request	= virtVssd_init_request,
};

static unsigned int virtVssd_queue_depth;
module_param_named(queue_depth, virtVssd_queue_depth, uint, 0444);



static int virtVssd_probe(struct virtio_device *vdev)
{

	struct virtio_vssd *vblk;
	struct request_queue *q;
	int err, index;

	u64 cap;
	u32 v, blk_size, sg_elems, opt_io_size;
	u16 min_io_size;
	u8 physical_block_exp, alignment_offset;

	printk(KERN_INFO"VSSD: virtVssd_probe \n");

	if (!vdev->config->get) 
	{
		printk(KERN_ALERT"VSSD-ERROR: vdev->config->get is null \n");
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
				__func__);
		return -EINVAL;
	}

	err = ida_simple_get(&vd_index_ida, 0, minor_to_index(1 << MINORBITS), GFP_KERNEL);
	if (err < 0)
	{
		printk(KERN_ALERT"VSSD-ERROR: ida_simple_get \n");
		goto out;
	}
	index = err;

	/* We need to know how many segments before we allocate. */
	err = virtio_cread_feature(vdev, VIRTIO_VSSD_F_SEG_MAX,
			struct virtio_vssd_config, seg_max,
			&sg_elems);

	/* We need at least one SG element, whatever they say. */
	if (err || !sg_elems)
		sg_elems = 1;

	/* We need an extra sg elements at head and tail. */
	sg_elems += 2;
	vdev->priv = vblk = kmalloc(sizeof(*vblk), GFP_KERNEL);
	if (!vblk) {
		err = -ENOMEM;
		printk(KERN_ALERT"VSSD-ERROR: vdev->priv = vblk = kmalloc \n");
		goto out_free_index;
	}

	vblk->vdev = vdev;
	vblk->sg_elems = sg_elems;

	INIT_WORK(&vblk->config_work, virtVssd_config_changed_work);
	printk(KERN_INFO"VSSD: \tINIT_WORK done \n");

	err = init_vq(vblk);
	if (err)
	{
		printk(KERN_ALERT"VSSD-ERROR: init_vq returns error \n");
		goto out_free_vblk;
	}
	printk(KERN_INFO"VSSD: \tinit_vq done done \n");

	/* FIXME: How many partitions?  How long is a piece of string? */
	vblk->disk = alloc_disk(1 << PART_BITS);
	if (!vblk->disk) {
		err = -ENOMEM;
		printk(KERN_ALERT"VSSD-ERROR: vblk->disk = alloc_disk failed \n");
		goto out_free_vq;
	}

	/* Default queue sizing is to fill the ring. */
	if (!virtVssd_queue_depth) {
		virtVssd_queue_depth = vblk->vqs[0].vq->num_free;
		/* ... but without indirect descs, we use 2 descs per req */
		if (!virtio_has_feature(vdev, VIRTIO_RING_F_INDIRECT_DESC))
			virtVssd_queue_depth /= 2;
	}

	memset(&vblk->tag_set, 0, sizeof(vblk->tag_set));
	vblk->tag_set.ops = &virtio_mq_ops;
	vblk->tag_set.queue_depth = virtVssd_queue_depth;
	vblk->tag_set.numa_node = NUMA_NO_NODE;
	vblk->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
	vblk->tag_set.cmd_size =
		sizeof(struct virtVssd_req) +
		sizeof(struct scatterlist) * sg_elems;
	vblk->tag_set.driver_data = vblk;
	vblk->tag_set.nr_hw_queues = vblk->num_vqs;

	err = blk_mq_alloc_tag_set(&vblk->tag_set);
	if (err)
	{
		printk(KERN_ALERT"VSSD-ERROR: 	err = blk_mq_alloc_tag_set(&vblk->tag_set \n");
		goto out_put_disk;
	}

	q = vblk->disk->queue = blk_mq_init_queue(&vblk->tag_set);
	if (IS_ERR(q)) {
		printk(KERN_ALERT"VSSD-ERROR: blk_mq_init_queue failed \n");
		err = -ENOMEM;
		goto out_free_tags;
	}

	q->queuedata = vblk;

	virtVssd_name_format("vssd", index, vblk->disk->disk_name, DISK_NAME_LEN);

	printk(KERN_INFO"VSSD: \tname format specified \n");
	vblk->disk->major = virtio_vssd_major;
	vblk->disk->first_minor = index_to_minor(index);
	vblk->disk->private_data = vblk;
	vblk->disk->fops = &virtVssd_fops;
	vblk->disk->flags |= GENHD_FL_EXT_DEVT;
	vblk->index = index;

	/* configure queue flush support */
	virtVssd_update_cache_mode(vdev);

	/* If disk is read-only in the host, the guest should obey */
	if (virtio_has_feature(vdev, VIRTIO_VSSD_F_RO))
		set_disk_ro(vblk->disk, 1);

	/* Host must always specify the capacity. */
	virtio_cread(vdev, struct virtio_vssd_config, capacity, &cap);

	/* If capacity is too big, truncate with warning. */
	if ((sector_t)cap != cap) {
		dev_warn(&vdev->dev, "Capacity %llu too large: truncating\n",
				(unsigned long long)cap);
		cap = (sector_t)-1;
	}
	set_capacity(vblk->disk, cap);
	printk(KERN_INFO"VSSD: \tSetting capacity to %lld \n", cap);

	/* We can handle whatever the host told us to handle. */
	blk_queue_max_segments(q, vblk->sg_elems-2);

	/* No need to bounce any requests */
	blk_queue_bounce_limit(q, BLK_BOUNCE_ANY);

	/* No real sector limit. */
	blk_queue_max_hw_sectors(q, -1U);

	/* Host can optionally specify maximum segment size and number of
	 * segments. */
	err = virtio_cread_feature(vdev, VIRTIO_VSSD_F_SIZE_MAX,
			struct virtio_vssd_config, size_max, &v);
	if (!err)
		blk_queue_max_segment_size(q, v);
	else
		blk_queue_max_segment_size(q, -1U);

	/* Host can optionally specify the block size of the device */
	err = virtio_cread_feature(vdev, VIRTIO_VSSD_F_BLK_SIZE,
			struct virtio_vssd_config, blk_size,
			&blk_size);
	if (!err)
		blk_queue_logical_block_size(q, blk_size);
	else
		blk_size = queue_logical_block_size(q);

	/* Use topology information if available */
	err = virtio_cread_feature(vdev, VIRTIO_VSSD_F_TOPOLOGY,
			struct virtio_vssd_config, physical_block_exp,
			&physical_block_exp);
	if (!err && physical_block_exp)
		blk_queue_physical_block_size(q,
				blk_size * (1 << physical_block_exp));

	err = virtio_cread_feature(vdev, VIRTIO_VSSD_F_TOPOLOGY,
			struct virtio_vssd_config, alignment_offset,
			&alignment_offset);
	if (!err && alignment_offset)
		blk_queue_alignment_offset(q, blk_size * alignment_offset);

	err = virtio_cread_feature(vdev, VIRTIO_VSSD_F_TOPOLOGY,
			struct virtio_vssd_config, min_io_size,
			&min_io_size);
	if (!err && min_io_size)
		blk_queue_io_min(q, blk_size * min_io_size);

	err = virtio_cread_feature(vdev, VIRTIO_VSSD_F_TOPOLOGY,
			struct virtio_vssd_config, opt_io_size,
			&opt_io_size);
	if (!err && opt_io_size)
		blk_queue_io_opt(q, blk_size * opt_io_size);


	/*REIZE unaisp start*/
	/*uint64_t list;
	  virtio_cread(vdev, struct virtio_vssd_config,
	  list, &list);

	  printk("VSSD: \tlist = %x.  \n", list);*/
	//printk("VSSD: \tlist = %x. Value = %lld \n", list, *((uint64_t *)list));

	virtio_vssd_setup_resize(vblk);
	/*end*/




	virtio_device_ready(vdev);
	printk(KERN_ALERT"VSSD: \tvirtio drtive ready called \n");

	device_add_disk(&vdev->dev, vblk->disk);
	err = device_create_file(disk_to_dev(vblk->disk), &dev_attr_serial);
	if (err)
		goto out_del_disk;

	if (virtio_has_feature(vdev, VIRTIO_VSSD_F_CONFIG_WCE))
		err = device_create_file(disk_to_dev(vblk->disk),
				&dev_attr_cache_type_rw);
	else
		err = device_create_file(disk_to_dev(vblk->disk),
				&dev_attr_cache_type_ro);
	if (err)
		goto out_del_disk;
	return 0;

out_del_disk:
	del_gendisk(vblk->disk);
	blk_cleanup_queue(vblk->disk->queue);
out_free_tags:
	blk_mq_free_tag_set(&vblk->tag_set);
out_put_disk:
	put_disk(vblk->disk);
out_free_vq:
	vdev->config->del_vqs(vdev);
out_free_vblk:
	kfree(vblk);
out_free_index:
	ida_simple_remove(&vd_index_ida, index);
out:
	return err;
}

static void virtVssd_remove(struct virtio_device *vdev)
{
	struct virtio_vssd *vblk = vdev->priv;
	int index = vblk->index;
	int refc;

	/* Make sure no work handler is accessing the device. */
	flush_work(&vblk->config_work);

	del_gendisk(vblk->disk);
	blk_cleanup_queue(vblk->disk->queue);

	blk_mq_free_tag_set(&vblk->tag_set);

	/* Stop all the virtqueues. */
	vdev->config->reset(vdev);

	refc = atomic_read(&disk_to_dev(vblk->disk)->kobj.kref.refcount);
	put_disk(vblk->disk);
	vdev->config->del_vqs(vdev);
	kfree(vblk->vqs);
	kfree(vblk);

	/* Only free device id if we don't have any users */
	if (refc == 1)
		ida_simple_remove(&vd_index_ida, index);
}

#ifdef CONFIG_PM_SLEEP
static int virtVssd_freeze(struct virtio_device *vdev)
{
	struct virtio_vssd *vblk = vdev->priv;

	/* Ensure we don't receive any more interrupts */
	vdev->config->reset(vdev);

	/* Make sure no work handler is accessing the device. */
	flush_work(&vblk->config_work);

	blk_mq_stop_hw_queues(vblk->disk->queue);

	vdev->config->del_vqs(vdev);
	return 0;
}

static int virtVssd_restore(struct virtio_device *vdev)
{
	struct virtio_vssd *vblk = vdev->priv;
	int ret;

	ret = init_vq(vdev->priv);
	if (ret)
		return ret;

	virtio_device_ready(vdev);

	blk_mq_start_stopped_hw_queues(vblk->disk->queue, true);
	return 0;
}
#endif

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_VSSD, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features_legacy[] = {
	VIRTIO_VSSD_F_SEG_MAX, VIRTIO_VSSD_F_SIZE_MAX, VIRTIO_VSSD_F_GEOMETRY,
	VIRTIO_VSSD_F_RO, VIRTIO_VSSD_F_BLK_SIZE, VIRTIO_VSSD_F_SCSI,
	VIRTIO_VSSD_F_FLUSH, VIRTIO_VSSD_F_TOPOLOGY, VIRTIO_VSSD_F_CONFIG_WCE,
	VIRTIO_VSSD_F_MQ,
}
;
static unsigned int features[] = {
	VIRTIO_VSSD_F_SEG_MAX, VIRTIO_VSSD_F_SIZE_MAX, VIRTIO_VSSD_F_GEOMETRY,
	VIRTIO_VSSD_F_RO, VIRTIO_VSSD_F_BLK_SIZE,
	VIRTIO_VSSD_F_FLUSH, VIRTIO_VSSD_F_TOPOLOGY, VIRTIO_VSSD_F_CONFIG_WCE,
	VIRTIO_VSSD_F_MQ,
};

static struct virtio_driver virtio_vssd_driver = {
	.feature_table			= features,
	.feature_table_size		= ARRAY_SIZE(features),
	.feature_table_legacy		= features_legacy,
	.feature_table_size_legacy	= ARRAY_SIZE(features_legacy),
	.driver.name			= KBUILD_MODNAME,
	.driver.owner			= THIS_MODULE,
	.id_table			= id_table,
	.probe				= virtVssd_probe,
	.remove				= virtVssd_remove,
	.config_changed			= virtVssd_config_changed,
#ifdef CONFIG_PM_SLEEP
	.freeze				= virtVssd_freeze,
	.restore			= virtVssd_restore,
#endif
};

static int __init init(void)
{
	int error;

	printk(KERN_INFO"VSSD: Initialization \n");

	virtVssd_wq = alloc_workqueue("virt_vssd", 0, 0);
	if (!virtVssd_wq)
		return -ENOMEM;

	virtio_vssd_major = register_blkdev(virtio_vssd_major, "virt_vssd");
	if (virtio_vssd_major < 0) {
		error = virtio_vssd_major;
		goto out_destroy_workqueue;
	}

	printk(KERN_INFO"VSSD: \tBlockdev registered. Major Number = %d \n", virtio_vssd_major);

	error = register_virtio_driver(&virtio_vssd_driver);
	if (error)
		goto out_unregister_blkdev;

	printk(KERN_INFO"VSSD: \tvirtio_vssd driver registered \n");

	printk(KERN_INFO"VSSD: \tVIRTIO_ID_VSSD = %d \n", VIRTIO_ID_VSSD);


	return 0;

out_unregister_blkdev:
	unregister_blkdev(virtio_vssd_major, "virt_vssd");
out_destroy_workqueue:
	destroy_workqueue(virtVssd_wq);
	return error;
}

static void __exit fini(void)
{
	printk(KERN_INFO"VSSD: Exit \n");

	unregister_virtio_driver(&virtio_vssd_driver);
	unregister_blkdev(virtio_vssd_major, "virt_vssd");
	destroy_workqueue(virtVssd_wq);
}
module_init(init);
module_exit(fini);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio vssd driver");
MODULE_LICENSE("GPL");
