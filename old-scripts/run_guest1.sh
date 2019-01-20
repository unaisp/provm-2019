sudo ~/ssd_sdb1/provm/qemu/qemu-2.9.0/build/x86_64-softmmu/qemu-system-x86_64 \
-s \
-m 2048 \
-smp 2 \
-enable-kvm \
-kernel ~/ssd_sdb1/provm/kernel_source/linux-4.9.35/arch/x86_64/boot/bzImage \
-append "root=/dev/vda1 console=ttyS0 rw" \
-drive if=virtio,file=/home/unaisp/ssd_sdb1/provm/guest_image/ubuntu16.04-1,cache=none \
-object memory-backend-file,id=mb1,size=1M,share,mem-path=/dev/shm/ivshmem \
-drive if=virtio,file=/dev/sdc,format=raw,cache=none,vssd,vm-id=300,vm-name=Virtual-Machine-300,size=10,allocate=10,persist=NONE \
-drive if=virtio,file=/dev/nbd3,format=raw,cache=none \
-drive if=virtio,file=/home/unaisp/ssd_sdb1/provm/guest_image/8GB-disk.qcow2,cache=none 


#-nographic \

#-append "root=/dev/vda1 rw console=ttyS0" \
#-drive if=virtio,file=/home/unaisp/Desktop/sdc/image.qcow2,cache=none \


#-s \



#r -m 4096 -smp 4 -enable-kvm -kernel ~/ssd_sdb1/provm/kernel_source/linux-4.9.35/arch/x86_64/boot/bzImage -append "root=/dev/vda1 rw console=ttyS0" -drive if=virtio,file=/home/unaisp/ssd_sdb1/provm/guest_image/ubuntu16.04-1,cache=none -object memory-backend-file,id=mb1,size=1M,share,mem-path=/dev/shm/ivshmem -drive if=virtio,file=/dev/sdc,format=raw,cache=none,vssd,vm-id=101,vm-name=Virtual-Machine-1,size=200,allocate=200,persist=NONE -drive if=virtio,file=/dev/nbd3,format=raw,cache=none 



#-drive if=virtio,file=/home/unaisp/ssd_sdb1/provm/guest_image/shared_drive.qcow2,cache=none \





#-virtfs local,path=/home/unaisp/ssd_sdb1/provm/share,mount_tag=host0,security_model=passthrough,id=host0 \





#./buildroot/output.x86_64~/host/usr/bin/qemu-system-x86_64 -m 128M -monitor telnet::45454,server,nowait -netdev user,hostfwd=tcp::45455-:45455,id=net0 -smp 1 -virtfs local,path=9p,mount_tag=host0,security_model=mapped,id=host0  -M pc -append 'root=/dev/vda nopat nokaslr norandmaps printk.devkmsg=on printk.time=y' -device edu -device lkmc_pci_min -device virtio-net-pci,netdev=net0 -kernel ./buildroot/output.x86_64~/images/bzImage    -drive file='./buildroot/output.x86_64~/images/rootfs.ext2.qcow2,if=virtio,format=qcow2'


#-drive if=virtio,file=/dev/sdc,format=raw,cache=none,vssd,vm-id=105,vm-name=Virtual-Machine-2,size=10 \


#-drive if=virtio,file=/home/unaisp/Desktop/ssd_mount/test.img,cache=none \
#-drive if=virtio,file=/dev/sdc,format=raw,cache=none \
#-device virtio-vssd-pci,file=/dev/sdc,cache=none
#-drive if=virtio,file=/dev/sdc,cache=none
#-device virtio-vssd-pci 
#-net nic,model=virtio,vlan=0,macaddr=00:16:3e:00:01:01 
#-net tap,vlan=0,script=/root/ifup-br0,downscript=/root/ifdown-br0
#-netdev bridge,id=bridge,br=br0 -device virtio net-pci,netdev=bridge





