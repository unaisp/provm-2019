/*
 * Virtio Block Device
 *
 * Copyright IBM, Corp. 2007
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_VIRTIO_VSSD_H
#define QEMU_VIRTIO_VSSD_H

#define SSD_BALLOON_UNIT 2046
#define SSD_BALLOON_INFLATION '-'
#define SSD_BALLOON_DEFLATION '+'
#define SSD_DM_CACHE_STATUS 's'
#define SSD_BLOCK_SIZE 2048    // 1 block = 2048 sectors.  ToDo: Find best block size. 
#define VSSD_DEBUG_MODE 0


#include "hw/virtio/virtio.h"
#include "hw/block/block.h"
#include "sysemu/iothread.h"
#include "sysemu/block-backend.h"
#include "standard-headers/linux/virtio_vssd.h"

#define TYPE_VIRTIO_VSSD "virtio-vssd-device"
#define VIRTIO_VSSD(obj) \
        OBJECT_CHECK(VirtIOVssd, (obj), TYPE_VIRTIO_VSSD)

/* This is the last element of the write scatter-gather list */
struct virtio_vssd_inhdr
{
    unsigned char status;
};

/*
 * This comes first in the read scatter-gather list.
 * For legacy virtio, if VIRTIO_F_ANY_LAYOUT is not negotiated,
 * this is the first element of the read scatter-gather list.
 */
struct virtio_vssd_outhdr {
    /* VIRTIO_VSSD_T* */
    __virtio32 type;
    /* io priority. */
    __virtio32 ioprio;
    /* Sector (ie. 512 byte offset) */
    __virtio64 sector;
};


struct VirtIOVssdConf
{
    BlockConf conf;
    IOThread *iothread;
    char *serial;
    uint32_t scsi;
    uint32_t config_wce;
    uint32_t request_merging;
    uint16_t num_queues;
};

struct VirtIOVssdDataPlane;


typedef struct VirtIOVssd 
{
    VirtIODevice parent_obj;
    BlockBackend *blk;
    void *rq;
    QEMUBH *bh;
    VirtIOVssdConf conf;
    unsigned short sector_mask;
    bool original_wce;
    VMChangeStateEntry *change;
    bool dataplane_disabled;
    bool dataplane_started;
    struct VirtIOVssdDataPlane *dataplane;

    // new variables
    uint64_t capacity;  //  Capacity of front-end device(vssd) in sectors;
    uint64_t current_capacity;  // in sectors
    uint32_t *bitmap;   //  Allocate memory on realize
                        //  Set / unset on realize
                        //  copy the list conetent to virtio_vssd_config on update_config.
                        //  Do I need to copy?  YES (:
                        //  can virtio_vssd_config->bitmap point to vssd->bitmap ??  NO 
    VirtQueue *ctrl_vq;
    VirtQueueElement *ctrl_vq_elem;
    int64_t command;
    uint64_t *block_list;

    //new variables to communicate with cm
    int vm_id;
    char vm_name[25];
    void *message;
    pthread_t listener_thread_ID;
    FILE *log_file;
    FILE *summary_file;

    //Variables for debugging
    pthread_mutex_t lock;


} VirtIOVssd;

struct statistics
{
    //  Related to one VirtioVSSD-request
    unsigned long int qiov_size;
    unsigned long int qiov_iov_count;

    //  After merging multiple requests
    unsigned long int qiov_merge_count;
    unsigned long int qiov_mereg_size;          //  Number of QIOVs merged 
    unsigned long int qiov_merg_iov_count;

    //  After dividing merged requests
    unsigned long int sub_qiov_count;   // 1 = Not crossing the boundary

    //  Time frame
    unsigned long long request_pull_time;              //  First event
    unsigned long long adding_to_mrb_time;
    unsigned long long callback_time;                  //  Time at which call back for last sub-qiov received
    unsigned long long request_push_time;              //  Final event

    unsigned long long qiov_merging_start_time;
    unsigned long long qiov_division_start_time;       //  Merging end time
    unsigned long long first_sub_qiov_send_time;
    unsigned long long last_sub_qiov_send_time;        //  Division end time
    unsigned long long first_sub_qiov_callback_time;
    unsigned long long last_sub_qiov_callback_time;    //  Request call back time
};

struct VirtIOVssdReq;
typedef struct VirtIOVssdReq 
{
    VirtQueueElement elem;
    int64_t sector_num;
    VirtIOVssd *dev;
    VirtQueue *vq;
    struct virtio_vssd_inhdr *in;
    struct virtio_vssd_outhdr out;
    QEMUIOVector qiov;
    size_t in_len;
    struct VirtIOVssdReq *next;
    struct VirtIOVssdReq *mr_next;
    BlockAcctCookie acct;

    //New variables
    int sub_qiov_count;
    int sub_qiov_finished;
    pthread_spinlock_t lock;       //  Lock is used to protect the above two variables
    struct statistics stats;

} VirtIOVssdReq;

#define VIRTIO_VSSD_MAX_MERGE_REQS 32

typedef struct MultiVssdReqBuffer 
{
    VirtIOVssdReq *reqs[VIRTIO_VSSD_MAX_MERGE_REQS];
    unsigned int num_reqs;
    bool is_write;
} MultiVssdReqBuffer;

typedef struct VirtIOVssdResizeInfo 
{
    int32_t status;
    int32_t ack;
    int64_t block_list[SSD_BALLOON_UNIT];

    int32_t total_requested_blocks;
    int32_t current_requested_blocks;
    int32_t given_blocks;
    int32_t remaining_blocks;
    char operation;      //      SSD_BALLOON_DELATION, SSD_BALLOON_INFLATION
    int flag;

} VirtIOVssdResizeInfo;


bool virtio_vssd_handle_vq(VirtIOVssd *s, VirtQueue *vq);

#endif
