# Scrape names from syscall.h 
# dsites 2022.07.03
#
# Optional $1 is syscall file name, default /usr/src/sys/sys/syscall.h
# Output to tmp_syscall.txt
#
# Expecting
#   #define	SYS_fstatat	552
# Producing
#     {1064, "fstatat"},
#
# first sed turns tabs to spaces
# grep keeps only transformed defines
# awk renumbers those above 511 by adding 512
# (syscall numbers are in two discontiguous ranges, 0-511 and 1024-1535)

fname=$1
if [ -z "$1" ] ; then
    fname="/usr/src/sys/sys/syscall.h"
fi

date=$(date -Idate)
echo "  // Scraped from $fname on $date" >tmp_syscall.txt
cat $fname \
  |sed 's/\t/ /g' \
  |sed 's/^.*SYS_\([^ ]*\)[ ]*\([0-9]*\)/  {\2, \"\1\"},/' \
  |grep "{" \
  |awk '{n = substr($1, 2, length($1)-2); n += 512*int(n/512); $1 = "  {" n ","; print $0}' >>tmp_syscall.txt
echo "tmp_syscall.txt written"
