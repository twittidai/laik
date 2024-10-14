#!/bin/sh
timeout() { perl -e 'alarm shift; exec @ARGV' "$@"; }
timeout 4 ./mpidynrun -n 3 -r 2 -d -H n1:1,n2:1,n3:1 ../../examples/resize 1 | LC_ALL='C' sort > test-resize-3-r2.out
cmp test-resize-3-r2.out "$(dirname -- "${0}")/test-resize-3-r2.expected"
