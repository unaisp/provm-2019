/*
 * Virtio VSSD Support
 *
 * Copyright Indian Institute of Technology Bombay. 2017
 * Copyright Bhavesh Singh <bhavesh@cse.iitb.ac.in>
 *
 * This work is licensed under the terms of GNU GPL, version 3.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_VIRTIO_VSSD_H
#define QEMU_VIRTIO_VSSD_H

#include "standard-headers/linux/virtio_ids.h"

#define TYPE_VIRTIO_VSSD "virtio-vssd"
#define VIRTIO_VSSD(obj) \
    OBJECT_CHECK(VirtIOVssd, (obj), TYPE_VIRTIO_VSSD)
#define VIRTIO_VSSD_GET_PARENT_CLASS(obj) \
    OBJECT_GET_PARENT_CLASS(obj, TYPE_VIRTIO_VSSD)

#define SSD_BALLOON_UNIT 4096//2046

#define VIRTIO_VSSD_READ 0
#define VIRTIO_VSSD_WRITE 1
#define SSD_NAME_LEN 256
#define SECTOR_OFFSET 4096
#define SECTOR_SHIFT 9
#define SECTOR_SIZE 512

struct virtio_vssd_config {
//    int32_t status;
    int32_t command;
    uint64_t capacity;
};

typedef struct VirtIOVssd {
    VirtIODevice parent_obj;
    VirtQueue *vq, *ctrl_vq;
    VirtQueueElement *ctrl_vq_elem;
//    struct virtio_vssd_config conf;

    int64_t command; // the outstanding number of sectors yet to be processed
    uint64_t capacity; // The capacity of the disk (# of 512-byte sectors)
    uint32_t *block_list; // bitmap of valid guest sector numbers; use this to choose sectors to free

    char backingdevice[SSD_NAME_LEN];
    int fd;

    /* Added by Bhavesh Singh. 2017.06.16. Begin add */
    struct timeval time;
    /* Added by Bhavesh Singh. 2017.06.16. End add */
} VirtIOVssd;

typedef struct VirtIOVssdHdr {
    uint32_t type;
    uint64_t sector_num;
} VirtIOVssdHdr;

typedef struct VirtIOVssdReq {
    VirtQueueElement elem;
    VirtIOVssdHdr hdr;
    VirtIOVssd *vssd;
    VirtQueue *vq;
    QEMUIOVector qiov;
    int32_t error;
} VirtIOVssdReq;

typedef struct VirtIOVssdResizeInfo {
    int32_t status;
    int32_t ack;
    uint64_t sector_list[SSD_BALLOON_UNIT];
} VirtIOVssdResizeInfo;

#endif
