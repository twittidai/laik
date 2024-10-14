#!/bin/sh
timeout() { perl -e 'alarm shift; exec @ARGV' "$@"; }
timeout 5 ./mpidynrun -n 3 -r 1 -H n1:1,n2:1,n3:1 ../../examples/vsum3 1 | LC_ALL='C' sort > test-vsum3-3-r1.out
cmp test-vsum3-3-r1.out "$(dirname -- "${0}")/test-vsum3-3-r1.expected"
