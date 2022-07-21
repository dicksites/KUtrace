#!/bin/sh
#
# Put in newer version of show_cpu.html template (in current directory)
# usage: remake foo.html produces foo_2.html
#
# The sed step turns one extremely long line into one with carriage returns after
# every closing bracket.
#

fname=$1
echo ${fname%.html}_2.html
cat $1 |./unmakeself |sed 's/\], /\],\n/g' |./makeself show_cpu.html > ${fname%.html}_2.html
