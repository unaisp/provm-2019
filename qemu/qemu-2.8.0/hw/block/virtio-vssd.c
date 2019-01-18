/*
 * Virtio VSSD Support
 *
 * Copyright Indian Institute of Technology Bombay. 2017
 * Copyright Bhavesh Singh <bhavesh@cse.iitb.ac.in>
 *
 * This work is licensed under the terms of GNU GPL, version 3.
 * See the COPYING file in the top-level directory.
 */

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
    return (sector + SECTOR_OFFSET) << SECTOR_SHIFT;
}

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

static void virtio_vssd_handle_resize(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVssdResizeInfo resize_info;
    VirtIOVssd *vssd = (VirtIOVssd *)vdev;
    VirtQueueElement *elem;
    struct iovec *iov;
    uint32_t in_num;
    int sign;

    //static int x = 0;

    //printf("virtio_vssd_backend: Control virtqueue kick received!\n");

//    vssd->ctrl_vq_elem = virtqueue_pop(vq, sizeof(VirtQueueElement));

//    iov = vssd->ctrl_vq_elem->in_sg;
//    in_num = vssd->ctrl_vq_elem->in_num;
//    iov_to_buf(iov, in_num, 0, &resize_info, sizeof(resize_info));

//    printf("virtio_vssd_backend: Status: %d, Ack %d", resize_info.status, resize_info.ack);
    while((elem = virtqueue_pop(vq, sizeof(VirtQueueElement))) != NULL) {
        iov = elem->in_sg;
        in_num = elem->in_num;
        iov_to_buf(iov, in_num, 0, &resize_info, sizeof(resize_info));
        sign = resize_info.status < 0 ? -1 : 1;
        if(resize_info.status < 0) {
            // We set the ack to the number of sectors we were able to recover from the list, while still maintaining the sign for direction.
//            if(resize_info.status != -1) {
//                resize_info.ack = resize_info.status + 1; // All but one sectors unmapped successfully!
//                resize_info.sector_list[0] = 1298; // Sector 1298 was not unmapped successfully. We put it back in the sector list.
//            } else
//                resize_info.ack = resize_info.status;
            virtio_vssd_free_blocks(vssd, &resize_info);
            vssd->command -= sign*(resize_info.status - resize_info.ack);            
        } else if(resize_info.status > 0) {
            if(resize_info.ack != -1) // This means that this is an ack from the guest
                vssd->command -= sign*(resize_info.status - resize_info.ack);
            else if(resize_info.ack == -1) // This means this is a new command
                resize_info.ack = 0; // We don't want the guest logic to go awry
            if(abs(resize_info.status-resize_info.ack) != 0) {
                // We need to set the sectors for the given number.
                virtio_vssd_map_blocks(vssd, &resize_info);
            } else {
                // We end the processing of this resize request here.
                resize_info.status = 0;
                resize_info.ack = 0;
            }
        }

//        QEMUIOVector qiov;
//        qemu_iovec_init_external(&qiov, iov, elem->out_num);

//        printf("virtio_vssd_backend: Status: %d, First Sector: %llu\n", resize_info.status, resize_info.sector_list[0]);
//        if(resize_info.status == 0) {
            // We need to figure out what to do! Its a status query from the guest.

            // Case 1: We ask for sectors from the guest
//            if(x==0) {
//                resize_info.status = -8;
//                x = 1;
//            }
            // Case 2: We give back sectors to guest
//            if(x==0) {
//                resize_info.status = 5;
//                x = 1;
//                int i;
//                for(i=0;i<abs(resize_info.status); i++) {
//                    resize_info.sector_list[i] = (i+12)*5;
//                }
//            }
//        }
//        else if(resize_info.status < 0) {
//            // We have got a reply from the guest alongwith the desired number of sectors
//            printf("virtio_vssd_backend: Sector numbers obtained from guest: %llu", resize_info.sector_list[1298]);
//            //int i;
//            //for(i=0;i<abs(resize_info.status); i++) {
//            //    printf(" %llu", resize_info.sector_list[i]);
//            //}
//            printf("\n");
//            resize_info.status = 0; // We set the status to zero now or to another negative number if we want more.
//        }

        iov_from_buf(iov, in_num, 0, &resize_info, sizeof(resize_info));
        virtqueue_push(vq, elem, sizeof(resize_info));
        virtio_notify(vdev, vq);
    }
//    virtqueue_push(vssd->ctrl_vq, vssd->ctrl_vq_elem, sizeof(resize_info));
//    virtio_notify(vdev, vq);
    if(vssd->command != 0) {
        virtio_notify_config(vdev);
    } else if(vssd->command == 0 && resize_info.status == resize_info.ack) {
        /* Added by Bhavesh Singh. 2017.06.16. Begin add */
        // The command has completed successfully
        //printf("virtio_vssd_backend: Ballooning finished: %lu %ld\n", (clock() - vssd->clock), CLOCKS_PER_SEC);
        struct timeval curr;
        gettimeofday(&curr, 0);
        uint64_t duration = (curr.tv_sec - vssd->time.tv_sec) * 1e6 + curr.tv_usec - vssd->time.tv_usec;

        uint32_t total_sectors = virtio_vssd_get_balloon_size(vssd);

        printf("virtio_vssd_backend: Ballooning finished: %lu: %u\n", duration, total_sectors);
        /* Added by Bhavesh Singh. 2017.06.16. End add */
    }
}

// We do not need these functions!! The IO is automatically block aligned perhaps also because of the fact
// that our constant block offset is 4096 bytes, which ensure block boundary alignment.
/*
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

static void virtio_vssd_get_config(VirtIODevice *vdev, uint8_t *config_data)
{
    VirtIOVssd *vssd = VIRTIO_VSSD(vdev);
    struct virtio_vssd_config config;
    int sign = vssd->command < 0 ? -1 : 1;
    config.command = sign*vssd->command > SSD_BALLOON_UNIT ? sign*SSD_BALLOON_UNIT : vssd->command;

    vssd->command -= config.command;

    //vssd->command -= sign*SSD_BALLOON_UNIT;
    //if(sign*vssd->command < 0)
    //    vssd->command = 0;

    config.capacity = vssd->capacity;
    memcpy(config_data, &config, sizeof(struct virtio_vssd_config));
}

static void virtio_ssd_balloon_to_target(void *opaque, int64_t target)
{
    VirtIOVssd *vssd = VIRTIO_VSSD(opaque);
    VirtIODevice *vdev = VIRTIO_DEVICE(vssd);

    vssd->command += target; // We add to ensure that the direction is preserved.

    /* Added by Bhavesh Singh. 2017.06.16. Begin add */
    //vssd->clock = clock();
    gettimeofday(&vssd->time, 0);
    /* Added by Bhavesh Singh. 2017.06.16. End add */

    virtio_notify_config(vdev);

//    VirtIOVssdResizeInfo resize_info;
//    VirtIOVssd *vssd = VIRTIO_VSSD(opaque);
//    VirtIODevice *vdev = VIRTIO_DEVICE(vssd);
//    struct iovec *iov;
//    uint32_t in_num;

//    printf("virtio_vssd_backend: Resize called.\n");

//    if(vssd->ctrl_vq_elem == NULL)
//        return;
//    iov = vssd->ctrl_vq_elem->in_sg;
//    in_num = vssd->ctrl_vq_elem->in_num;
//    iov_to_buf(iov, in_num, 0, &resize_info, sizeof(resize_info));
//    resize_info.status = target;
//    iov_from_buf(iov, in_num, 0, &resize_info, sizeof(resize_info));
//    virtqueue_push(vssd->ctrl_vq, vssd->ctrl_vq_elem, sizeof(resize_info));
//    virtio_notify(vdev, vssd->ctrl_vq);
}

static uint64_t virtio_vssd_get_features(VirtIODevice *vdev, uint64_t features, Error **errp)
{
    return features;
}

static void virtio_vssd_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVssd *vssd = VIRTIO_VSSD(dev);

    strncpy(vssd->backingdevice, "/dev/sdc1", 10);

    // FIXME: See if O_DIRECT works!
    vssd->fd = open(vssd->backingdevice, O_RDWR | O_DIRECT/* | O_SYNC*/);
    if(vssd->fd < 0) {
        error_setg(errp, "Unable to initialize backing SSD state");
        return;
    }
    vssd->ctrl_vq_elem = NULL;

    //vssd->capacity = 67108864; // =2^26 (# of 512 byte sectors; therefore 2^35 bytes or 32GB)
    //vssd->capacity = 4194304; // =2^22 (# of 512 byte sectors; therefore 2^31 bytes or 2GB)
    vssd->capacity = 2097152; // =2^21 (# of 512 byte sectors; therefore 2^30 bytes or 1GB)
    vssd->block_list = calloc(vssd->capacity/32, sizeof(uint32_t)); // We divide by 32 as our array is of unsigned 32 bit integers.

    virtio_init(vdev, "virtio-vssd", VIRTIO_ID_VSSD, sizeof(struct virtio_vssd_config));

    // TODO: What is a good virtqueue size for a block device? We set it to 128.
    vssd->vq = virtio_add_queue(vdev, 128, virtio_vssd_handle_request);
    vssd->ctrl_vq = virtio_add_queue(vdev, 128, virtio_vssd_handle_resize);

    qemu_add_ssd_balloon_handler(virtio_ssd_balloon_to_target, vssd);
}

static void virtio_vssd_device_unrealize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVssd *vssd = VIRTIO_VSSD(dev);
    qemu_remove_ssd_balloon_handler(vssd);
    virtio_del_queue(vdev, 0);
    virtio_cleanup(vdev);
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

static void virtio_register_types(void)
{
    type_register_static(&virtio_vssd_info);
}

type_init(virtio_register_types)
