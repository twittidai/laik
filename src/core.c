/*
 * This file is part of the LAIK library.
 * Copyright (c) 2017, 2018 Josef Weidendorfer <Josef.Weidendorfer@gmx.de>
 *
 * LAIK is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, version 3 or later.
 *
 * LAIK is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include <laik-internal.h>
#include <laik-backend-mpi.h>
#include <laik-backend-single.h>
#include <laik-backend-tcp.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>


//program name
extern const char *__progname;

//-----------------------------------------------------------------------
// LAIK init/finalize
//
// see corresponding backend code for non-generic initialization of LAIK

// generic LAIK init function
Laik_Instance* laik_init (int* argc, char*** argv)
{
    const char* override = getenv("LAIK_BACKEND");
    Laik_Instance* inst = 0;

#ifdef USE_MPI
    if (inst == 0) {
        // default to MPI if available, or if explicitly wanted
        if ((override == 0) || (strcmp(override, "mpi") == 0)) {
            inst = laik_init_mpi(argc, argv);
        }
    }
#endif

    if (inst == 0) {
        // fall-back to "single" backend as default if MPI is not available, or
        // if "single" backend is explicitly requested
        if ((override == 0) || (strcmp(override, "single") == 0)) {
            (void) argc;
            (void) argv;
            inst = laik_init_single();
        }
    }

#ifdef USE_TCP
    if (inst == 0) {
        if ((override == 0) || (strcmp(override, "tcp") == 0)) {
            inst = laik_init_tcp(argc, argv);
        }
    }
#endif

    if (inst == 0) {
        // Error: unknown backend wanted
        assert(override != 0);

        // create dummy backend for laik_log to work
        laik_init_single();
        laik_log(LAIK_LL_Panic,
                 "Unknwown backend '%s' requested by LAIK_BACKEND", override);
        exit (1);
    }

    // wait for debugger to attach?
    char* rstr = getenv("LAIK_DEBUG_RANK");
    if (rstr) {
        int wrank = atoi(rstr);
        if ((wrank < 0) || (wrank == inst->myid)) {
            // as long as "wait" is 1, wait in loop for debugger
            volatile int wait = 1;
            while(wait) { usleep(10000); }
        }
    }

    return inst;
}


int laik_size(Laik_Group* g)
{
    return g->size;
}

int laik_myid(Laik_Group* g)
{
    return g->myid;
}

void laik_finalize(Laik_Instance* inst)
{
    laik_log(1, "finalizing...");
    if (inst->backend && inst->backend->finalize)
        (*inst->backend->finalize)(inst);

    if (inst->repart_ctrl){
        laik_ext_cleanup(inst);
    }

    if (laik_log_begin(2)) {
        laik_log_append("switch statistics (this task):\n");
        Laik_SwitchStat* ss = laik_newSwitchStat();
        for(int i=0; i<inst->data_count; i++) {
            Laik_Data* d = inst->data[i];
            laik_addSwitchStat(ss, d->stat);

            laik_log_append("  data '%s': ", d->name);
            laik_log_SwitchStat(d->stat);
        }
        if (inst->data_count > 1) {
            laik_log_append("  summary: ");
            laik_log_SwitchStat(ss);
        }
        free(ss);

        laik_log_flush(0);
    }

    laik_close_profiling_file(inst);
    laik_free_profiling(inst);
    free(inst->control);

    laik_log_cleanup(inst);
}

// return a backend-dependant string for the location of the calling task
char* laik_mylocation(Laik_Instance* inst)
{
    return inst->mylocation;
}

// allocate space for a new LAIK instance
Laik_Instance* laik_new_instance(const Laik_Backend* b,
                                 int size, int myid,
                                 char* location, void* data, void *gdata)
{
    Laik_Instance* instance;
    instance = malloc(sizeof(Laik_Instance));
    if (!instance) {
        laik_panic("Out of memory allocating Laik_Instance object");
        exit(1); // not actually needed, laik_panic never returns
    }

    instance->backend = b;
    instance->backend_data = data;
    instance->size = size;
    instance->myid = myid;
    instance->mylocation = strdup(location);
    instance->locationStore = 0;

    // for logging wall-clock time since LAIK initialization
    gettimeofday(&(instance->init_time), NULL);

    instance->firstSpaceForInstance = 0;

    instance->group_count = 0;
    instance->data_count = 0;
    instance->mapping_count = 0;

    laik_space_init();
    laik_data_init(); // initialize the data module

    instance->control = laik_program_control_init();
    instance->profiling = laik_init_profiling();

    instance->repart_ctrl = 0;

    // logging (TODO: multiple instances)
    laik_log_init(instance);

    if (laik_log_begin(2)) {
        laik_log_append_info();
        laik_log_flush(0);
    }

    // Create a group in this instance with same parameters as the instance.
    // Since it's the first group, this is what laik_world() will return.
    Laik_Group* first_group = laik_create_group (instance);
    first_group->size         = size;
    first_group->myid         = myid;
    first_group->backend_data = gdata;

    // Assign default location mappings
    for(int i = 0; i <first_group->size; i++) {
        first_group->toLocation[i] = i;
    }

    return instance;
}

// add/remove space to/from instance
void laik_addSpaceForInstance(Laik_Instance* inst, Laik_Space* s)
{
    assert(s->nextSpaceForInstance == 0);
    s->nextSpaceForInstance = inst->firstSpaceForInstance;
    inst->firstSpaceForInstance = s;
}

void laik_removeSpaceFromInstance(Laik_Instance* inst, Laik_Space* s)
{
    if (inst->firstSpaceForInstance == s) {
        inst->firstSpaceForInstance = s->nextSpaceForInstance;
    }
    else {
        // search for previous item
        Laik_Space* ss = inst->firstSpaceForInstance;
        while(ss->nextSpaceForInstance != s)
            ss = ss->nextSpaceForInstance;
        assert(ss != 0); // not found, should not happen
        ss->nextSpaceForInstance = s->nextSpaceForInstance;
    }
    s->nextSpaceForInstance = 0;
}

void laik_addDataForInstance(Laik_Instance* inst, Laik_Data* d)
{
    assert(inst->data_count < MAX_DATAS);
    inst->data[inst->data_count] = d;
    inst->data_count++;
}


// create a group to be used in this LAIK instance
Laik_Group* laik_create_group(Laik_Instance* i)
{
    assert(i->group_count < MAX_GROUPS);

    Laik_Group* g;

    g = malloc(sizeof(Laik_Group) + 3 * (i->size) * sizeof(int));
    if (!g) {
        laik_panic("Out of memory allocating Laik_Group object");
        exit(1); // not actually needed, laik_panic never returns
    }
    i->group[i->group_count] = g;

    g->inst = i;
    g->gid = i->group_count;
    g->size = 0; // yet invalid
    g->backend_data = 0;
    g->parent = 0;

    // space after struct
    g->toParent   = (int*) (((char*)g) + sizeof(Laik_Group));
    g->fromParent = g->toParent + i->size;
    g->toLocation = g->fromParent + i->size;

    i->group_count++;
    return g;
}

Laik_Instance* laik_inst(Laik_Group* g)
{
    return g->inst;
}

Laik_Group* laik_world(Laik_Instance* i)
{
    // world must have been added by backend
    assert(i->group_count > 0);

    Laik_Group* g = i->group[0];
    assert(g->gid == 0);
    assert(g->inst == i);
    assert(g->size == i->size);

    return g;
}

// create a clone of <g>, derived from <g>.
Laik_Group* laik_clone_group(Laik_Group* g)
{
    Laik_Group* g2 = laik_create_group(g->inst);
    g2->parent = g;
    g2->size = g->size;
    g2->myid = g->myid;

    for(int i=0; i < g->size; i++) {
        g2->toParent[i] = i;
        g2->fromParent[i] = i;
        g2->toLocation[i] = g->toLocation[i];
    }

    return g2;
}


// Shrinking (collective)
Laik_Group* laik_new_shrinked_group(Laik_Group* g, int len, int* list)
{
    Laik_Group* g2 = laik_clone_group(g);

    for(int i = 0; i < g->size; i++)
        g2->fromParent[i] = 0; // init

    for(int i = 0; i < len; i++) {
        assert((list[i] >= 0) && (list[i] < g->size));
        g2->fromParent[list[i]] = -1; // mark removed
    }
    int o = 0;
    for(int i = 0; i < g->size; i++) {
        if (g2->fromParent[i] < 0) continue;
        g2->fromParent[i] = o;
        g2->toParent[o] = i;
        g2->toLocation[o] = g->toLocation[i];
        o++;
    }
    g2->size = o;
    g2->myid = (g->myid < 0) ? -1 : g2->fromParent[g->myid];

    if (g->inst->backend->updateGroup)
        (g->inst->backend->updateGroup)(g2);

    if (laik_log_begin(1)) {
        laik_log_append("shrink group: "
                        "%d (size %d, myid %d) => %d (size %d, myid %d):",
                        g->gid, g->size, g->myid, g2->gid, g2->size, g2->myid);
        laik_log_append("\n  fromParent (to shrinked)  : ");
        laik_log_IntList(g->size, g2->fromParent);
        laik_log_append("\n  toParent   (from shrinked): ");
        laik_log_IntList(g2->size, g2->toParent);
        laik_log_append("\n  toLocation (in shrinked): ");
        laik_log_IntList(g2->size, g2->toLocation);
        laik_log_flush(0);
    }

    return g2;
}

int laik_group_locationid(Laik_Group *group, int id)
{
    assert(id >= 0 && id < group->size);
    return group->toLocation[id];
}

static char* locationkey(int loc) {
    static char key[10];
    snprintf(key, 10, "%i", loc);
    return key;
}

// synchronize location strings via KVS among processes in current world
void laik_sync_location(Laik_Instance *instance)
{
    if (instance->locationStore == NULL)
        instance->locationStore = laik_kvs_new("location", instance);

    Laik_Group* world = laik_world(instance);
    char* mylocation = laik_mylocation(instance);
    char* myKey = locationkey(laik_group_locationid(world, laik_myid(world)));

    laik_kvs_sets(instance->locationStore, myKey, mylocation);
    laik_kvs_sync(instance->locationStore);
}

// get location string identifier from process index in given group
char* laik_group_location(Laik_Group *group, int id)
{
    if (group->inst->locationStore == NULL)
        return NULL;

    char* myKey = locationkey(laik_group_locationid(group, id));
    return laik_kvs_get(group->inst->locationStore, myKey, NULL);
}


// Utilities

char* laik_get_guid(Laik_Instance* i){
    return i->guid;
}


