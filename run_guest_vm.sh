vm_image="/home/unaisp/ssd_sdb1/provm/guest_image/ubuntu16.04-1.img"
bzImage="/home/unaisp/ssd_sdb1/provm/kernel_source/linux-4.9.35/arch/x86_64/boot/bzImage"

sudo qemu-system-x86_64 \
-m 2048 \
-smp 2 \
-enable-kvm \
-append "root=/dev/vda1 console=ttyS0 rw" \
-object memory-backend-file,id=mb1,size=1M,share,mem-path=/dev/shm/ivshmem \
-kernel $bzImage \
-drive if=virtio,file=$vm_image,cache=none \
-nographic \




# vm_image : Guest OS image
# bzImage  : Kernel bz Image
# -m       : Memory in MB
# -smp     : Number of CPUs to be allocated to VM

