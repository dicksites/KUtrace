# Paste in new JavaScript template for a KUtrace HTML file
# $1 is foo.html, output is foo_2.html
# uses unmakeself, makeself, and ./unmake show_cpu.html in current directory
# dick sites 2022.06.28

fname=$1
cat $1 |./unmakeself |sed 's/\], /\],\n/g' |./makeself show_cpu.html > ${fname%.html}_2.html
echo "  ${fname%.html}_2.html written"

