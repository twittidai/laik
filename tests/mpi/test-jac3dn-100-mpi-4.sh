#!/bin/sh
LAIK_BACKEND=mpi mpiexec -n 4 ../../examples/jac3d -s -n 100 > test-jac3dn-100-mpi-4.out
cmp test-jac3dn-100-mpi-4.out "$(dirname -- "${0}")/test-jac3dn-100.expected"
