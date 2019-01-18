/*
 * Virtio SSD
 *
 * Author:
 *     Muhammed Unais P <unaisp@cse.iitb.ac.in>
 *
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/timer.h"
#include "qemu-common.h"

#include "hw/i386/pc.h"
#include "sysemu/kvm.h"
#include "exec/address-spaces.h"
#include "qapi/visitor.h"
#include "qapi-event.h"
#include "trace.h"

#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"


//#include "hw/virtio/virtio-pci.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-vssd.h"

static Property virtio_vssd_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_vssd_handle_request(VirtIODevice *vdev, VirtQueue *vq)
{

}

static void virtio_vssd_handle_resize(VirtIODevice *vdev, VirtQueue *vq)
{

}

static void virtio_vssd_get_config(VirtIODevice *vdev, uint8_t *config_data)
{

}

static void virtio_vssd_device_unrealize(DeviceState *dev, Error **errp)
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

static uint64_t virtio_vssd_get_features(VirtIODevice *vdev, uint64_t features, Error **errp)
{
    return features;
}

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