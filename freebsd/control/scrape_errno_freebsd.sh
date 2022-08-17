# Scrape names from errno.h 
# dsites 2022.07.03
#
# Optional $1 is errno file name, default /usr/src/sys/sys/errno.h
# Output to tmp_errno.txt
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
    fname="/usr/src/sys/sys/errno.h"
fi
 
date=$(date -Idate)   
echo "  // Scraped from $fname on $date" >tmp_errno.txt
cat $fname  \
  |sed 's/\t/ /g' \
  |sed 's/^#define[ ]*\([^ ]*\)[ ]*\([0-9]*\)/  {\2, \"\1\"},/' \
  |grep "  {" \
  |grep -v "{," >>tmp_errno.txt
echo "tmp_errno.txt written"

