#!/bin/sh
timeout() { perl -e 'alarm shift; exec @ARGV' "$@"; }
timeout 4 ./mpidynrun -n 2 -a 1 -r 1 -H n1:1,n2:1 ../../examples/resize 1 | LC_ALL='C' sort > test-resize-2-s1-r1.out
cmp test-resize-2-s1-r1.out "$(dirname -- "${0}")/test-resize-2-s1-r1.expected"
