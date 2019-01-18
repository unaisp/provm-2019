/*
 * Virtio SSD
 *
 * Author:
 *     Muhammed Unais P <unaisp@cse.iitb.ac.in>
 *
 */

#include "standard-headers/linux/virtio_ids.h"

#define TYPE_VIRTIO_VSSD "virtio-vssd"
#define VIRTIO_VSSD(obj) \
    OBJECT_CHECK(VirtIOVssd, (obj), TYPE_VIRTIO_VSSD)

typedef struct VirtIOVssd {
    VirtIODevice parent_obj;
    VirtQueue *vq;
    VirtQueue *ctrl_vq;
} VirtIOVssd;

struct virtio_vssd_config {
//    int32_t status;
    int32_t command;
    uint64_t capacity;
};



