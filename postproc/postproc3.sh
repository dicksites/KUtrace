#!/bin/sh
#
# Postprocess a raw KUtrace file into a dynamic HTML file and open it in chrome
#
# arg 1 filename stem (no .trace), arg 2 "title", arg 3/4 spantotrim args
#

# Must sort by byte code, not local collating sequence
export LC_ALL=C

cat $1.trace  |./rawtoevent |sort -n |./eventtospan3 "$2" |sort >$1.json 
echo "  $1.json written"

# Default to no trim, else start/stop seconds
trim_arg='0'
if [ -n "$3" ]
then
delimit=' '
trim_arg=$3$delimit$4
fi

cat $1.json |./spantotrim $trim_arg |./makeself show_cpu.html >$1.html
echo "  $1.html written"

# Open up in Chrome
google-chrome $1.html &
