VM=$1
SIZE=$2

sudo ~/ssd_sdb1/provm/qemu/qemu-2.9.0/build/x86_64-softmmu/qemu-system-x86_64 \
-show-cursor \
-m 2048 \
-smp 2 \
-enable-kvm \
-kernel ~/ssd_sdb1/provm/kernel_source/linux-4.9.35/arch/x86_64/boot/bzImage \
-append "root=/dev/vda1 console=ttyS0 rw" \
-drive if=virtio,file=/home/unaisp/ssd_sdb1/provm/guest_image/ubuntu16.04-$VM,cache=none \
-object memory-backend-file,id=mb1,size=1M,share,mem-path=/dev/shm/ivshmem \
-drive if=virtio,file=/dev/nbd3,format=raw,cache=none \
-drive if=virtio,file=/dev/sdc,format=raw,cache=none,vssd,vm-id=$VM,vm-name=Virtual-Machine-$VM,size=$SIZE,allocate=$SIZE,persist=NONE \
-nographic


#-drive if=virtio,file=/dev/sdc$((VM+4)),format=raw,cache=none \
#-drive if=virtio,file=/home/unaisp/Desktop/ssd_mount/20GB-disk-$((VM+4)).qcow2,cache=none \
#-drive if=virtio,file=/dev/sdc,format=raw,cache=none,vssd,vm-id=$VM,vm-name=Virtual-Machine-$VM,size=$SIZE,allocate=$SIZE,persist=NONE \
#-drive if=virtio,file=/home/unaisp/ssd_sdb1/provm/guest_image/8GB-disk.qcow2,cache=none 





