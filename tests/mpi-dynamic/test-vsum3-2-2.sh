#!/bin/sh
timeout() { perl -e 'alarm shift; exec @ARGV' "$@"; }
timeout 5 ./mpidynrun -n 2 -a 2 -H n1:1,n2:1,n3:1,n4:1 ../../examples/vsum3 1 | LC_ALL='C' sort > test-vsum3-2-2.out
cmp test-vsum3-2-2.out "$(dirname -- "${0}")/test-vsum3-2-2.expected"
