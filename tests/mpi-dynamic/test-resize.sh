#!/bin/sh
export LAIK_BACKEND=mpi_dyn
mpirun -n 1 -H n1:1,n2:1,n3:1,n4:1 /opt/hpc/build/laik/examples/resize 4 #| LC_ALL='C' sort > test-resize-2-s1-r1.out