#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/timer.h"
#include "qemu-common.h"
#include "hw/virtio/virtio.h"
#include "hw/i386/pc.h"
#include "sysemu/balloon.h"
#include "hw/virtio/virtio-balloon.h"
#include "sysemu/kvm.h"
#include "exec/address-spaces.h"
#include "qapi/visitor.h"
#include "qapi-event.h"
#include "trace.h"

#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"

#include "qemu-common.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-vssd.h"

static void virtio_vssd_free_request(VirtIOVssdReq *vssdReq)
{
    if (vssdReq) {
        g_free(vssdReq);
    }
}

static inline uint64_t virtio_vssd_map_offset(uint64_t sector) {
    // Some dummy fixed offset for now. Will need to have access to
    // shared state where the mappings for all VMs are maintained.
    //return (sector  SECTOR_OFFSET) << SECTOR_SHIFT;
    return (sector + SECTOR_OFFSET) << SECTOR_SHIFT;
}

/*
static uint32_t virtio_vssd_get_balloon_size(VirtIOVssd *vssd) {
    uint32_t i=0, total_sectors = 0;
    uint32_t *list = vssd->block_list;
    for(; i<vssd->capacity; i++) {
        if((list[i/32] & (1 << (i%32))) == (1 << (i%32))) {
            total_sectors++;
        }
    }
    return total_sectors;
}

// The guest has freed blocks that we maintain a list of here too.
static void virtio_vssd_free_blocks(struct VirtIOVssd *vssd, struct VirtIOVssdResizeInfo *resize_info) {
    uint32_t i=0, total_sectors = abs(resize_info->status);
    uint64_t sector_num;
    uint32_t *list = vssd->block_list;
    for(;i<abs(total_sectors);i++) {
        sector_num = resize_info->sector_list[i];
        list[sector_num/32] |= (1 << (sector_num % 32));
    }
    resize_info->ack = resize_info->status;
    //printf("%u\n", virtio_vssd_get_balloon_size(vssd));
}

// We tell the guest which blocks to map again.
static void virtio_vssd_map_blocks(struct VirtIOVssd *vssd, struct VirtIOVssdResizeInfo *resize_info) {
    uint32_t i=0, j=0, total_sectors = resize_info->status;
    uint32_t *list = vssd->block_list;
    for(;i<vssd->capacity && j<total_sectors; i++) {
        if((list[i/32] & (1 << (i%32))) == (1 << (i%32))) {
            list[i/32] &= ~(1 << (i%32));
            resize_info->sector_list[j++] = i;
        }
    }
    //printf("%u\n", virtio_vssd_get_balloon_size(vssd));
}
*/


/*
// We do not need these functions!! The IO is automatically block aligned perhaps also because of the fact
// that our constant block offset is 4096 bytes, which ensure block boundary alignment.
static int32_t virtio_vssd_read(int fd, QEMUIOVector *qiov)
{
    uint32_t i, size;
    char *buffer;
    int32_t error = 0;

    for(i=0; i<qiov->niov-1; i++) {
        size = qiov->iov[i].iov_len > SECTOR_SIZE ? (qiov->iov[i].iov_len / SECTOR_SIZE) * SECTOR_SIZE : SECTOR_SIZE;
        buffer = (char *)aligned_alloc(SECTOR_SIZE, size);
//        if((error = errno) < 0)
//            return error;
        if(buffer == NULL) {
            error = errno;
            goto out_error;
        }
        if(read(fd, buffer, qiov->iov[i].iov_len) < 0) {
            error = errno;
            goto out_error;
        }
        strncpy(qiov->iov[i].iov_base, buffer, qiov->iov[i].iov_len);
    }
out_error:
    return error;
}

static int32_t virtio_vssd_write(int fd, QEMUIOVector *qiov)
{
    uint32_t i, size;
    char *buffer;
    int32_t error = 0;

    for(i=0; i<qiov->niov; i++) {
        size = qiov->iov[i].iov_len > SECTOR_SIZE ? (qiov->iov[i].iov_len / SECTOR_SIZE) * SECTOR_SIZE : SECTOR_SIZE;
        buffer = (char *)aligned_alloc(SECTOR_SIZE, size);
//        memset(buffer, 0, size);
//        if((error = errno) < 0)
//            return error;
        if(buffer == NULL) {
            error = errno;
           goto out_error;
        }
        strncpy(buffer, qiov->iov[i].iov_base, qiov->iov[i].iov_len);
        if(write(fd, buffer, qiov->iov[i].iov_len) < 0) {
            error = errno;
            goto out_error;
        }
    }
out_error:
    return error;
}

*/

static void virtio_vssd_handle_request(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVssd *vssd = (VirtIOVssd *)vdev;
    VirtIOVssdReq *vssdReq;
    struct iovec *iov, *in_iov;
    uint32_t in_num, out_num, type;
    bool is_write;
    uint64_t offset;
    //printf("virtio_vssd_backend: virtqueue kick received!\n");
    while((vssdReq = virtqueue_pop(vq, sizeof(VirtIOVssdReq))) != NULL) {
        //printf("virtio_vssd_backend: virtqueue element popped!\n");
        vssdReq->vssd = vssd;
        vssdReq->vq = vq;
        vssdReq->error = 0;
        iov = vssdReq->elem.out_sg;
        in_iov = vssdReq->elem.in_sg;
        in_num = vssdReq->elem.in_num;
        out_num = vssdReq->elem.out_num;
        //printf("virtio_vssd_backend: Out: %u\tIn:%u\n", out_num, in_num);
        iov_to_buf(iov, out_num, 0, &vssdReq->hdr, sizeof(vssdReq->hdr));
        //printf("virtio_vssd_backend: hdr.type: %u\thdr.sector_num: %lu\n", vssdReq->hdr.type, vssdReq->hdr.sector_num);
        iov_discard_front(&iov, &out_num, sizeof(vssdReq->hdr));
        type = virtio_ldl_p(VIRTIO_DEVICE(vssdReq->vssd), &vssdReq->hdr.type);
        //printf("virtio_vssd_backend: Type: %u\n", type);
        is_write = type & VIRTIO_VSSD_WRITE;
        offset = virtio_vssd_map_offset(virtio_ldq_p(VIRTIO_DEVICE(vssdReq->vssd), &vssdReq->hdr.sector_num));
        if(lseek(vssd->fd, offset, SEEK_SET) < 0) // Go to the byte offset
            vssdReq->error = errno;
        //vssdReq->error = errno < 0 ? errno : 0;
        if(vssdReq->error < 0)
            goto push_output;

        if(is_write) {
            qemu_iovec_init_external(&vssdReq->qiov, iov, out_num);
            //printf("virtio_vssd_backend: Number of io vectors: %d\n", vssdReq->qiov.niov);
//            lseek(vssd->fd, offset, SEEK_SET); // Go to the byte offset
//            vssdReq->error = errno < 0 ? errno : 0;
//            if(vssdReq->error < 0)
//                goto push_output;
            if(writev(vssd->fd, vssdReq->qiov.iov, vssdReq->qiov.niov) < 0)
                vssdReq->error = errno;
            //vssdReq->error = virtio_vssd_write(vssd->fd, &vssdReq->qiov);
            //vssdReq->error = error < 0 ? error : 0;
            //for(i = 0; i < vssdReq->qiov.niov; i++) {
            //    snprintf(abc, 4096, "%s", (char*)vssdReq->qiov.iov[i].iov_base);
            //    abc[4095] = '\0';
            //    printf("virtio_vssd_backend: Virtio write operation: %s\n", abc);
            //}
        } else {
            qemu_iovec_init_external(&vssdReq->qiov, in_iov, in_num); // The last one is the status word which we do not want to map.
            //printf("virtio_vssd_backend: Number of io vectors: %d\n", vssdReq->qiov.niov);
//            lseek(vssd->fd, offset, SEEK_SET); // Go to the byte offset
//            vssdReq->error = errno < 0 ? errno : 0;
//            if(vssdReq->error < 0)
//                goto push_output;
            if(readv(vssd->fd, vssdReq->qiov.iov, vssdReq->qiov.niov - 1) < 0)
                vssdReq->error = errno;
            //int32_t error = virtio_vssd_read(vssd->fd, &vssdReq->qiov);
            //vssdReq->error = error < 0 ? error : 0;
            // The following two lines are just for testing whether errors are propagated upwards.
            //vssdReq->error = -ENOMEM;
            //goto set_error;
            //for(i = 0; i < vssdReq->qiov.niov; i++) {
            //    snprintf((char*)vssdReq->qiov.iov[i].iov_base, vssdReq->qiov.iov[i].iov_len, "Singh");
            //    //(char*)vssdReq->qiov.iov[i].iov_base[4095] = '\0';
            //    printf("virtio_vssd_backend: Virtio read operation: %s\n", (char*)vssdReq->qiov.iov[i].iov_base);
            //}
        }
        //printf("virtio_vssd_backend: Length of second in iov: %lu\n", in_iov[in_num - 1].iov_len);
        //printf("virtio_vssd_backend: Direction: %d\tOut: %u\tIn:%u\tSector: %lu\tSize: %lu\n", is_write, out_num, in_num, sector_num, vssdReq->qiov.size);
        //virtqueue_detach_element(vq, &vssdReq->elem, vssdReq->qiov.size);
push_output:
        *((int*)vssdReq->qiov.iov[vssdReq->qiov.niov - 1].iov_base) = vssdReq->error > 0 ? -vssdReq->error : 0;
        virtqueue_push(vq, &vssdReq->elem, vssdReq->qiov.size);
        virtio_notify(vdev, vq);
        virtio_vssd_free_request(vssdReq);
        //virtio_blk_handle_vq(s, vq);
        //virtqueue_push(vq, &vssdReq->elem, 32);
        //virtio_queue_notify(vdev, 0);
    }
}

static uint64_t virtio_vssd_get_features(VirtIODevice *vdev, uint64_t features, Error **errp)
{
    return features;
}

static void virtio_vssd_get_config(VirtIODevice *vdev, uint8_t *config_data)
{

}

static void virtio_vssd_device_unrealize(DeviceState *dev, Error **errp)
{
	
}

static void virtio_vssd_handle_resize(VirtIODevice *vdev, VirtQueue *vq)
{

}

static void virtio_vssd_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVssd *vssd = VIRTIO_VSSD(dev);
    

    virtio_init(vdev, "virtio-vssd", VIRTIO_ID_VSSD, sizeof(struct virtio_vssd_config));

    // TODO: What is a good virtqueue size for a block device? We set it to 128.
    vssd->vq = virtio_add_queue(vdev, 128, virtio_vssd_handle_request);
    vssd->ctrl_vq = virtio_add_queue(vdev, 128, virtio_vssd_handle_resize);
}

static Property virtio_vssd_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_vssd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->props = virtio_vssd_properties;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = virtio_vssd_device_realize;
    vdc->unrealize = virtio_vssd_device_unrealize;
    vdc->get_config = virtio_vssd_get_config;
    vdc->get_features = virtio_vssd_get_features;
}

static const TypeInfo virtio_vssd_info = {
    .name = TYPE_VIRTIO_VSSD,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOVssd),
    .class_init = virtio_vssd_class_init,
};
