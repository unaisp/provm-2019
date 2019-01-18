#now=$(date)
now=$1

git checkout qemu
git add qemu-2.9.0
git add gitpush-qemu.sh
git commit -m "Daily commit / qemu / ${now}"
git push symflex-all qemu


##
