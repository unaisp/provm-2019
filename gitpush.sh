now=$(date)

git checkout other
git add server.c
git add server.h
git add gitpush.sh
git commit -m "Daily commit / other / ${now}"
git push symflex-all other

#


# pushing linux
cd kernel_source
./gitpush-linux.sh "$now"
cd ../

#pushing qemu
cd qemu
./gitpush-qemu.sh "$now"


#
