# Scrape interrupt names from arch/x86/include/asm/irq_vectors.h 
# dsites 2022.07.31
#
# Optional $1 is irq file name, default arch/x86/include/asm/irq_vectors.h
# Output to tmp_irq_linux.txt
#
# Expecting
#   #define NMI_VECTOR			0x02
# Producing
#     {0x02, "NMI_VECTOR"},
#
# First grep picks out irq lines with an actual name and number
# first sed turns tabs to spaces
# second sed picks out name and number
# third sed removes _VECTOR from names
# tr lowercases names

fname=$1
if [ -z "$1" ] ; then
    fname="arch/x86/include/asm/irq_vectors.h "
fi

date=$(date -Idate)   
echo "  // Scraped from $fname on $date" >tmp_irq_linux.txt

cat $fname \
  |grep "#define" |grep "0x" \
  |sed 's/\t/ /g' \
  |sed 's/^[^ ]*[ ]*\([^ ]*\)[ ]*\([^ ]*\).*$/  {\2, \"\1\"},/' \
  |sed 's/_VECTOR//' \
  |tr [A-Z] [a-z] \
  >>tmp_irq_linux.txt
echo "tmp_irq_linux.txt written"
