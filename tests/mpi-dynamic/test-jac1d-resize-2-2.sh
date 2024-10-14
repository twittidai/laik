#!/bin/sh
timeout() { perl -e 'alarm shift; exec @ARGV' "$@"; }
timeout 5 ./mpidynrun -n 2 -a 2 -H n1:1,n2:1 ../../examples/jac1d 100 50 -10 > test-jac1d-resize-2-2.out
cmp test-jac1d-resize-2-2.out "$(dirname -- "${0}")/test-jac1d-resize-2-2.expected"
