#now=$(date)
now=$1

git checkout linux
git add linux-4.9.35
git add gitpush-linux.sh
git commit -m "Daily commit / linux / ${now}"
git push symflex-all linux
#
#
