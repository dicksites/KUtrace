#!/bin/bash
# arg 1 filename stem (no .trace), arg 2 "title", arg 3/4 spantrim args

# Must sort by pure byte values, not local collating sequence
export LC_ALL=C

# Strip trailing .trace if it is there
var1=${1%.trace}

cat $var1.trace  |./rawtoevent |sort -n |./eventtospan3 "$2" |sort >$var1.json 
echo "  $var1.json written"

trim_arg='0'
if [ -n "$3" ]
then
delimit=' '
trim_arg=$3$delimit$4
fi

cat $var1.json |./spantotrim $trim_arg |./makeself show_cpu.html >$var1.html
echo "  $var1.html written"

google-chrome $var1.html &
