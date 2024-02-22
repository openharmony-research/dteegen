#!/bin/bash

if [ $# -ne 1 ]; then
	echo "Usage: $0 <target>"
	exit 1
fi

CODEGEN=./build/dteegen

$CODEGEN convert $1

if [ -d $1.generated ]; then
	mv $1.generated/build generated
	rm -rf $1.generated
fi

mv generated $1.generated
