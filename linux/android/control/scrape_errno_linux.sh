# Scrape names from errno-base.h and errno.h
# dsites 2022.07.31
#
# Optional $1 is errno file name, default include/uapi/asm-generic/errno-base.h
# Optional $2 is errno file name, default include/uapi/asm-generic/errno.h
# Output to tmp_errno_linux.txt
#
# Expecting
#   #define	EPERM		1		/* Operation not permitted */
# Producing
#     {1, "EPERM"},  /* Operation not permitted */
#
# first sed turns tabs to spaces
# first grep keeps only transformed defines
# second grep removes defines that have no number in the right place

fname=$1
if [ -z "$1" ] ; then
    fname="include/uapi/asm-generic/errno-base.h"
fi
 
date=$(date -Idate)   
echo "  // Scraped from $fname on $date" >tmp_errno_linux.txt
cat $fname  \
  |sed 's/\t/ /g' \
  |sed 's/^#define[ ]*\([^ ]*\)[ ]*\([0-9]*\)/  {\2, \"\1\"},/' \
  |grep "  {" \
  |grep -v "{," >>tmp_errno_linux.txt

fname=$2
if [ -z "$2" ] ; then
    fname="include/uapi/asm-generic/errno.h"
fi
 
echo "  // Scraped from $fname on $date" >>tmp_errno_linux.txt
cat $fname  \
  |sed 's/\t/ /g' \
  |sed 's/^#define[ ]*\([^ ]*\)[ ]*\([0-9]*\)/  {\2, \"\1\"},/' \
  |grep "  {" \
  |grep -v "{," >>tmp_errno_linux.txt

echo "tmp_errno_linux.txt written"

