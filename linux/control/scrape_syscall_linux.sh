# Scrape names from unistd_64.h (Linux)
# dsites 2022.07.31
#
# Optional $1 is syscall file name, default arch/x86/include/generated/uapi/asm/unistd_64.h
# Output to tmp_syscall_linux.txt
#
# Expecting
#   #define __NR_rt_sigaction 13
# Producing
#     {13, "rt_sigaction"},
#
# First sed turns tabs to spaces
# grep keeps only transformed defines
# awk renumbers those above 511 by adding 512
# (syscall numbers are in two discontiguous ranges, 0-511 and 1024-1535)

fname=$1
if [ -z "$1" ] ; then
    fname="arch/x86/include/generated/uapi/asm/unistd_64.h"
fi

date=$(date -Idate)
echo "  // Scraped from $fname on $date" >tmp_syscall_linux.txt
cat $fname \
  |sed 's/\t/ /g' \
  |sed 's/^.*NR_\([^ ]*\)[ ]*\([0-9]*\)/  {\2, \"\1\"},/' \
  |grep "{" \
  |awk '{n = substr($1, 2, length($1)-2); n += 512*int(n/512); $1 = "  {" n ","; print $0}' >>tmp_syscall_linux.txt
echo "tmp_syscall_linux.txt written"

