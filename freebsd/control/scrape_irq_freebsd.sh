# Scrape interrupt names from $ vmstat -ia |grep ":[^ ]" output 
# dsites 2022.07.03
#
# Output to tmp_irq.txt
#
# Expecting
#   irq1: atkbd0                           0          0
# Producing
#     {1, "atkbd0"},
#
# first grep picks out irq lines with an actual name
# first sed turns tabs to spaces
# second grep keeps only transformed defines
# third grep removes defines that have no number in the right place

date=$(date -Idate)   
echo "  // Scraped from vmstat -ia on $date" >tmp_irq.txt

vmstat -ia \
  |grep ": [^ ]" \
  |sed 's/\t/ /g' \
  |sed 's/^irq\([0-9]*\):[ ]* \([^ ]*\).*$/  {\1, \"\2\"},/' \
  |grep "  {" \
  |grep -v "{," >>tmp_irq.txt
echo "tmp_irq.txt written"
