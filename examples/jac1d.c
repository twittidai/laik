/* This file is part of the LAIK parallel container library.
 * Copyright (c) 2017 Josef Weidendorfer
 *
 * LAIK is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, version 3.
 *
 * LAIK is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * 1d Jacobi example.
 */

#include <laik.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

// fixed boundary values
double loValue = -5.0, hiValue = 10.0;

// to deliberately change block partitioning (if arg 3 provided)
double getTW(int rank, const void* userData)
{
    int v = * ((int*) userData);
    // switch non-equal weigthing on and off
    return 1.0 + (rank * (v & 1));
}

int main(int argc, char* argv[])
{
    Laik_Instance* inst = laik_init (&argc, &argv);
    Laik_Group* world = laik_world(inst);

    int ksize = 0;
    int maxiter = 0;
    int repart = 0; // enforce repartitioning after <repart> iterations

    if (argc > 1) ksize = atoi(argv[1]);
    if (argc > 2) maxiter = atoi(argv[2]);
    if (argc > 3) repart = atoi(argv[3]);

    if (ksize == 0) ksize = 10000; // 10 mio entries
    if (maxiter == 0) maxiter = 50;

    if (laik_myid(world) == 0) {
        printf("%d k cells (mem %.1f MB), running %d iterations with %d tasks\n",
               ksize, .016 * ksize, maxiter, laik_size(world));
        if (repart > 0)
            printf("  with repartitioning every %d iterations\n", repart);
    }
    uint64_t size = (uint64_t) ksize * 1000;


    double *baseR, *baseW, *sumPtr;
    uint64_t countR, countW, off;
    int64_t gx1, gx2;
    int64_t x1, x2;

    // for global sum, used for residuum: 1 double accessible by all
    Laik_Space* sp1 = laik_new_space_1d(inst, 1);
    Laik_Partitioning* pSum = laik_new_partitioning(laik_All, world, sp1, 0);
    Laik_Data* sumD = laik_new_data(sp1, laik_Double);
    laik_data_set_name(sumD, "sum");
    laik_switchto_partitioning(sumD, pSum, LAIK_DF_None, LAIK_RO_None);

    // two 1d arrays for jacobi, using same space
    Laik_Space* space = laik_new_space_1d(inst, size);
    Laik_Data* data1 = laik_new_data(space, laik_Double);
    Laik_Data* data2 = laik_new_data(space, laik_Double);

    // we use two types of partitioners algorithms:
    // - prWrite: cells to update (disjunctive partitioning)
    // - prRead : extends partitionings by haloes, to read neighbor values
    Laik_Partitioner *prWrite, *prRead;
    prWrite = laik_new_block_partitioner1();
    prRead = laik_new_cornerhalo_partitioner(1);

    Laik_Data *dWrite, *dRead; // set to data1/2, depending on iteration
    Laik_Partitioning *pWrite, *pRead;
    int iter = laik_phase(inst);
    if (iter == 0) {
        // initial process
        // run partitioners to get partitionings over 2d space and <world> group
        // data1/2 are then alternately accessed using pRead/pWrite
        pWrite = laik_new_partitioning(prWrite, world, space, 0);
        pRead  = laik_new_partitioning(prRead, world, space, pWrite);

        // start with writing (= initialization) data1
        dWrite = data1;
        dRead = data2;

        // distributed initialization
        laik_switchto_partitioning(dWrite, pWrite, LAIK_DF_None, LAIK_RO_None);
        laik_my_range_1d(pWrite, 0, &gx1, &gx2);

        // arbitrary non-zero values based on global indexes to detect bugs
        laik_get_map_1d(dWrite, 0, (void**) &baseW, &countW);
        for(uint64_t i = 0; i < countW; i++)
            baseW[i] = (double) ((gx1 + i) & 6);

        // set fixed boundary values
        if (laik_global2local_1d(dWrite, 0, &off)) {
            // if global index 0 is local, it must be at local index 0
            assert(off == 0);
            baseW[off] = loValue;
        }
        if (laik_global2local_1d(dWrite, size-1, &off)) {
            // if last global index is local, it must be at countW-1
            assert(off == countW - 1);
            baseW[off] = hiValue;
        }
        laik_log(2, "Init done\n");
    }
    else {
        // joining process

        // when joining for uneven iteration, data got written to data2 to be preserved
        if ((iter & 1) == 1) { dRead = data1; dWrite = data2; }
        else                 { dRead = data2; dWrite = data1; }

        Laik_Group* parent = laik_group_parent(world);
        // partitionings before joining: empty for own process
        Laik_Partitioning *pWriteOld, *pReadOld;
        pWriteOld = laik_new_partitioning(prWrite, parent, space, 0);
        pReadOld  = laik_new_partitioning(prRead, parent, space, pWriteOld);
        laik_set_initial_partitioning(dWrite, pWriteOld);
        laik_set_initial_partitioning(dRead, pReadOld);

        // calculate and switch to partitionings with new processes
        pWrite = laik_new_partitioning(prWrite, world, space, 0);
        pRead  = laik_new_partitioning(prRead, world, space, pWrite);
        laik_switchto_partitioning(dWrite, pWrite,
                                   LAIK_DF_Preserve, LAIK_RO_None);
        laik_switchto_partitioning(dRead, pRead,
                                   LAIK_DF_None, LAIK_RO_None);
        laik_free_partitioning(pWriteOld);
        laik_free_partitioning(pReadOld);

        laik_finish_world_resize(inst);
    }
    laik_partitioning_set_name(pWrite, "pWrite");
    laik_partitioning_set_name(pRead, "pRead");

    // for statistics (with LAIK_LOG=2)
    double t, t1 = laik_wtime(), t2 = t1;
    int first_iter = iter;
    int last_iter = iter;
    int res_iters = 0; // iterations done with residuum calculation

    for(; iter < maxiter; iter++) {
        laik_set_iteration(inst, iter + 1);

        // switch roles: in even iterations, switch data1 to read partitioning
        if ((iter & 1) == 0) { dRead = data1; dWrite = data2; }
        else                 { dRead = data2; dWrite = data1; }

        laik_switchto_partitioning(dRead,  pRead,  LAIK_DF_Preserve, LAIK_RO_None);
        laik_switchto_partitioning(dWrite, pWrite, LAIK_DF_None, LAIK_RO_None);
        laik_get_map_1d(dRead,  0, (void**) &baseR, &countR);
        laik_get_map_1d(dWrite, 0, (void**) &baseW, &countW);

        // local range for which to do 1d stencil, adjust at borders
        x1 = 0;
        x2 = countW;
        if (laik_global2local_1d(dWrite, 0, &off)) {
            // global index 0 is local
            assert(off == 0);
            baseW[off] = loValue;
            x1++;
        }
        else {
            // start at inner border: adjust baseR such that
            //  baseR[i] and baseW[i] correspond to same global index
            assert(laik_local2global_1d(dWrite, 0) ==
                   laik_local2global_1d(dRead, 0) + 1);
            baseR++;
        }
        if (laik_global2local_1d(dWrite, size-1, &off)) {
            // last global index is local
            assert(off == countW - 1);
            baseW[off] = hiValue;
            x2--;
        }

        // do jacobi

        // check for residuum every 10 iterations (3 Flops more per update)
        if ((iter % 10) == 0) {
            // using work load in all tasks
            double newValue, diff, res;
            res = 0.0;
            for(int64_t i = x1; i < x2; i++) {
                newValue = 0.5 * (baseR[i-1] + baseR[i+1]);
                diff = baseR[i] - newValue;
                res += diff * diff;
                baseW[i] = newValue;
            }
            res_iters++;

            // calculate global residuum
            laik_switchto_flow(sumD, LAIK_DF_None, LAIK_RO_None);
            laik_get_map_1d(sumD, 0, (void**) &sumPtr, 0);
            *sumPtr = res;
            laik_switchto_flow(sumD, LAIK_DF_Preserve, LAIK_RO_Sum);
            laik_get_map_1d(sumD, 0, (void**) &sumPtr, 0);
            res = *sumPtr;

            if (iter > 0) {
                t = laik_wtime();
                // current iteration already done
                int diter = (iter + 1) - last_iter;
                double dt = t - t2;
                double gUpdates = 0.000000001 * size; // per iteration
                laik_log(2, "For %d iters: %.3fs, %.3f GF/s, %.3f GB/s",
                         diter, dt,
                         // 2 Flops per update in reg iters, with res 5 (once)
                         gUpdates * (5 + 2 * (diter-1)) / dt,
                         // per update 16 bytes read + 8 byte written
                         gUpdates * diter * 24 / dt);
                last_iter = iter + 1;
                t2 = t;
            }

            if (laik_myid(laik_data_get_group(sumD)) == 0) {
                printf("Residuum after %2d iters: %f\n", iter+1, res);
            }

            if (res < .001) break;
        }
        else {
            for(int64_t i = x1; i < x2; i++) {
                baseW[i] = 0.5 * (baseR[i-1] + baseR[i+1]);
            }
        }

        // optionally, change partitioning slightly as test
        if ((repart > 0) && (iter > 0) && ((iter % repart) == 0)) {
            static int userData;
            userData = iter / repart;
            laik_set_task_weight(prWrite, getTW, (void*) &userData);

            // calculate new partitionings, switch to them, free old
            // only need to preserve data written into dWrite
            Laik_Partitioning *pWriteNew, *pReadNew;
            pWriteNew = laik_new_partitioning(prWrite, world, space, 0);
            pReadNew  = laik_new_partitioning(prRead, world, space, pWriteNew);
            laik_switchto_partitioning(dWrite, pWriteNew,
                                       LAIK_DF_Preserve, LAIK_RO_None);
            laik_switchto_partitioning(dRead, pReadNew,
                                       LAIK_DF_None, LAIK_RO_None);
            laik_free_partitioning(pWrite);
            laik_free_partitioning(pRead);
            pWrite = pWriteNew;
            pRead = pReadNew;
        }

        // allow external repartitioning
        if ((repart < 0) && (iter > 0) && ((iter % (-repart)) == 0)) {
            // allow resize of world and get new world
            Laik_Group* newworld = laik_allow_world_resize(inst, iter + 1);
            if (newworld != world) {
                laik_release_group(world);
                world = newworld;

                Laik_Partitioning* pSumNew;
                pSumNew = laik_new_partitioning(laik_All, world, sp1, 0);
                laik_switchto_partitioning(sumD, pSumNew, LAIK_DF_None, LAIK_RO_None);
                laik_free_partitioning(pSum);
                pSum = pSumNew;

                Laik_Partitioning *pWriteNew, *pReadNew;
                pWriteNew = laik_new_partitioning(prWrite, world, space, 0);
                pReadNew  = laik_new_partitioning(prRead, world, space, pWriteNew);
                laik_switchto_partitioning(dWrite, pWriteNew,
                                           LAIK_DF_Preserve, LAIK_RO_None);
                laik_switchto_partitioning(dRead, pReadNew,
                                           LAIK_DF_None, LAIK_RO_None);
                laik_free_partitioning(pWrite);
                laik_free_partitioning(pRead);
                pWrite = pWriteNew;
                pRead = pReadNew;

                laik_finish_world_resize(inst);

                // exit if we got removed from world
                if (laik_myid(world) < 0) {
                    laik_finalize(inst);
                    return 0;
                }
            }
        }
    }

    // statistics for all iterations and reductions
    // using work load in all tasks
    if (laik_log_shown(2)) {
        t = laik_wtime();
        int diter = iter - first_iter;
        double dt = t - t1;
        double gUpdates = 0.000000001 * size; // per iteration
        laik_log(2, "For %d iters: %.3fs, %.3f GF/s, %.3f GB/s",
                 diter, dt,
                 // 2 Flops per update in reg iters, with res 5
                 gUpdates * (5 * res_iters + 2 * (diter - res_iters)) / dt,
                 // per update 16 bytes read + 8 byte written
                 gUpdates * diter * 24 / dt);
    }

    // for check at end: sum up all just written values
    double sum = 0.0;
    laik_get_map_1d(dWrite, 0, (void**) &baseW, &countW);
    for(uint64_t i = 0; i < countW; i++) sum += baseW[i];

    // global reduction of local sum values
    laik_switchto_flow(sumD, LAIK_DF_None, LAIK_RO_None);
    laik_get_map_1d(sumD, 0, (void**) &sumPtr, 0);
    *sumPtr = sum;
    laik_switchto_flow(sumD, LAIK_DF_Preserve, LAIK_RO_Sum);
    laik_get_map_1d(sumD, 0, (void**) &sumPtr, 0);
    sum = *sumPtr;

    if (laik_myid(laik_data_get_group(sumD)) == 0) {
        printf("Global value sum after %d iterations: %f\n",
               iter, sum);
    }

    laik_finalize(inst);
    return 0;
}
