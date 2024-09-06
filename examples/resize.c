/* This file is part of the LAIK parallel container library.
 * Copyright (c) 2021 Josef Weidendorfer
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
 * World resize example.
 *
 * Allow resizing after each iteration.
 */

#include "laik.h"
#include <stdio.h>
#include <unistd.h>

int main(int argc, char* argv[])
{
    Laik_Instance* inst = laik_init(&argc, &argv);
    volatile int i = 0;
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    printf("PID %d on %s ready for attach\n", getpid(), hostname);
    fflush(stdout);
    while (0 == i)
        sleep(5);
    int phase = laik_phase(inst);

    int max = 0;
    if (argc > 1) max = atoi(argv[1]);  
    if (max == 0) max = 10;

    Laik_Group *world = laik_world(inst);
    while(1) {
        printf("Epoch %d / Phase %d: Hello from process %d of %d\n",
               laik_epoch(inst), phase,
               laik_myid(world), laik_size(world));
        laik_release_group(world);
        if (phase >= max) break;

        sleep(1);
        phase++;

        // allow resize of world and get new world
        world = laik_allow_world_resize(inst, phase);

        if (laik_myid(world) < 0) break;
    }

    laik_finalize(inst);
    return 0;
}
