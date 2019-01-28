now=$(date)
#date > last-push-time.txt

git add .
git commit -m "backup : ${now}"
git push provm-2019-synerg master
git push provm-2019-github master





#git checkout other
#git add server.c
#git add server.h
#git add gitpush.sh
#git commit -m "Daily commit / other / ${now}"
#git push symflex-all other

#


# pushing linux
#cd kernel_source
#./gitpush-linux.sh "$now"
#cd ../

#pushing qemu
#cd qemu
#./gitpush-qemu.sh "$now"


#

