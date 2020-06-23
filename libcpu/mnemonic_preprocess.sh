#!/bin/sh

arch="$1"
defs="$2"
out="$3"

m4 "-D${arch}" -DDISASSEMBLER "$defs" \
	| sed "1,/^%%/d;/^#/d;/^[[:space:]]*$/d;s/[^:]*:\([^[:space:]]*\).*/MNE(\\1)/;s/{[^}]*}//g;/INVALID/d" \
	| sort -u \
> "$out"
