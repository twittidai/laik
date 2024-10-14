#!/bin/sh
timeout() { perl -e 'alarm shift; exec @ARGV' "$@"; }
timeout 5 ./mpidynrun -n 4 -r 2 -H n1:1,n2:1,n3:1,n4:1 ../../examples/jac1d 100 50 -10 > test-jac1d-resize-4-r2.out
cmp test-jac1d-resize-4-r2.out "$(dirname -- "${0}")/test-jac1d-resize-4-r2.expected"
