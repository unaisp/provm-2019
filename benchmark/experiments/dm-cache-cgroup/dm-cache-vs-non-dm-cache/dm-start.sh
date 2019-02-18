dd if=/dev/zero of=/dev/vdc bs=1G count=1 oflag=direct
dmsetup create my_cache --table '0 209715200 cache /dev/vdc /dev/vssda /dev/vdb 2048 1 writeback default 0'
