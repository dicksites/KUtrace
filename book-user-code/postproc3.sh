#!/bin/bash
# arg 1 filename stem (no .trace), arg 2 "title", arg 3/4 spantrim args

export LC_ALL=C

cat $1.trace  |./rawtoevent |sort -n |./eventtospan3 "$2" |sort >$1.json 
echo "  $1.json written"

trim_arg='0'
if [ -n "$3" ]
then
delimit=' '
trim_arg=$3$delimit$4
fi

cat $1.json |./spantotrim $trim_arg |./makeself show_cpu.html >$1.html
echo "  $1.html written"

google-chrome $1.html &
