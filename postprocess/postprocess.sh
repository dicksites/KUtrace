#!/bin/bash

FILE=$1
if ([ -z "$1" ]);then
	echo "Usage: ./postprocess x.trace title [JSON]"
	exit 1
fi

if !([ -f ${FILE} ]); then
	echo "Please Input a valid .trace file as the first input"
	exit 1
fi	
if ([ -z "$2" ]);then
	echo "No title input! Using Untitled as Title"
	TITLE="Untitled"
else
	TITLE=$2
fi
if ([ -z "$3" ]);then
	echo "Will not generate JSON output! Pass in 'JSON' as third argument"
	JSON=false
else
	JSON=true
fi
if  !([ -f "./rawtoevent" ] && [ -f "./eventtospan" ] && [ -f "./makeself" ]);then
	make clean
	make all	
fi
export LC_ALL=C
if ! [ -d "HTML_output" ];then
	echo "Making HTML Output Dir"
	mkdir "HTML_output"
fi
if (! ([ -d "JSON_output" ]) && $JSON);then
	echo "Making JSON output directory"
	mkdir "JSON_output"
fi
if (! $JSON);then
	echo "Generating File"
	cat ${FILE} | ./rawtoevent | sort -n |./eventtospan "${TITLE}" | sort | ./makeself show_cpu_2019.html > "HTML_output/${TITLE}.html";
else
	./rawtoevent ${FILE} | sort -n |./eventtospan "${TITLE}" | sort > "JSON_output/${TITLE}.json"
	cat JSON_output/${TITLE}.json | ./makeself show_cpu_2019.html > "HTML_output/${TITLE}.html"
fi
