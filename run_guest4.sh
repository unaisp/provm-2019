sudo ~/ssd_sdb1/provm/qemu/qemu-2.9.0/build/x86_64-softmmu/qemu-system-x86_64 \
-m 2048 \
-smp 2 \
-enable-kvm \
-kernel ~/ssd_sdb1/provm/kernel_source/linux-4.9.35/arch/x86_64/boot/bzImage \
-append "root=/dev/vda1 rw console=ttyS0" \
-drive if=virtio,file=/home/unaisp/ssd_sdb1/provm/guest_image/ubuntu16.04-4,cache=none \
-object memory-backend-file,id=mb1,size=1M,share,mem-path=/dev/shm/ivshmem \
-drive if=virtio,file=/dev/nbd3,format=raw,cache=none \
-drive if=virtio,file=/dev/sdc,format=raw,cache=none,vssd,vm-id=303,vm-name=Virtual-Machine-303,size=200,allocate=200,persist=NONE
#-drive if=virtio,file=/dev/nbd3,format=raw,cache=none \






