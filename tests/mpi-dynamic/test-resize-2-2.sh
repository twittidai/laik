#!/bin/sh
timeout() { perl -e 'alarm shift; exec @ARGV' "$@"; }
timeout 2 ./mpidynrun -n 2 -a 2 -H n1:1,n2:1,n3:1,n4:1 ../../examples/resize 1 | LC_ALL='C' sort > test-resize-2-2.out
./mycmp test-resize-2-2.out "$(dirname -- "${0}")/test-resize-2-2.expected"
