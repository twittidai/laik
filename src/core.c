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
#include <laik-backend-mpi-dynamic.h>
#include <laik-backend-single.h>
#include <laik-backend-tcp.h>
#include <laik-backend-tcp2.h>

// for string.h to declare strdup
#define __STDC_WANT_LIB_EXT2__ 1

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
Laik_Instance* laik_init(int* argc, char*** argv)
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

#ifdef USE_MPI_DYN
    if (inst == 0) {
        // default to MPI if available, or if explicitly wanted
        if ((override == 0) || (strcmp(override, "mpi_dyn") == 0)) {
            inst = laik_init_mpi_dyn(argc, argv);
        }
    }
#endif

#ifdef USE_TCP2
    if (inst == 0) {
        if ((override == 0) || (strcmp(override, "tcp2") == 0)) {
            inst = laik_init_tcp2(argc, argv);
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
        laik_log(LAIK_LL_Error,
                 "Unknown backend '%s' requested by LAIK_BACKEND", override);
        laik_log(LAIK_LL_Panic,
                 "Supported backends: "
#ifdef USE_MPI
                 "mpi "
#endif
#ifdef USE_TCP2
                 "tcp2 "
#endif
#ifdef USE_TCP
                 "tcp "
#endif
                 "single");
        exit (1);
    }

    // wait for debugger to attach?
    char* rstr = getenv("LAIK_DEBUG_RANK");
    if (rstr) {
        int wrank = atoi(rstr);
        if ((wrank < 0) || (wrank == inst->mylocationid)) {
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

int laik_phase(Laik_Instance* i)
{
    return i->phase;
}

int laik_epoch(Laik_Instance* i)
{
    return i->epoch;
}


void laik_finalize(Laik_Instance* inst)
{
    laik_log(1, "finalizing...");

    // finish an eventual ongoing resize phase
    laik_finish_world_resize(inst);

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

//! get unique location ID for the calling process in given instance
int laik_mylocationid(Laik_Instance *i)
{
    return i->mylocationid;
}

// return a backend-dependant string for the location of the calling task
char* laik_mylocation(Laik_Instance* inst)
{
    return inst->mylocation;
}

// allocate space for a new LAIK instance
Laik_Instance* laik_new_instance(const Laik_Backend* b,
                                 int size, int myid, int epoch, int phase,
                                 char* location, void* data)
{
    Laik_Instance* instance;
    instance = malloc(sizeof(Laik_Instance));
    if (!instance) {
        laik_panic("Out of memory allocating Laik_Instance object");
        exit(1); // not actually needed, laik_panic never returns
    }

    instance->backend = b;
    instance->backend_data = data;
    instance->epoch = epoch;
    instance->phase = phase;
    instance->locations = size;    // initial number of locations
    instance->mylocationid = myid; // initially, myid is my locationid
    instance->mylocation = strdup(location);
    instance->world = 0;           // still invalid, gets set by backend

    instance->resizeRequests = 0;

    instance->locationStore = 0;
    instance->location = 0; // set at location sync

    instance->spaceStore = 0;

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
Laik_Group* laik_create_group(Laik_Instance* i, int maxsize)
{
    assert(i->group_count < MAX_GROUPS);

    Laik_Group* g;

    // with 3 arrays, 2 with number of processes as size, other with parent size
    g = malloc(sizeof(Laik_Group) + (5 * maxsize) * sizeof(int));
    if (!g) {
        laik_panic("Out of memory allocating Laik_Group object");
        exit(1); // not actually needed, laik_panic never returns
    }
    i->group[i->group_count] = g;

    g->inst = i;
    g->gid = i->group_count;
    g->size = 0; // yet invalid
    g->maxsize = maxsize;
    g->backend_data = 0;
    g->parent = 0;
    g->parent2 = 0;

    // space after struct
    g->locationid  = (int*) (((char*)g) + sizeof(Laik_Group));
    g->fromParent  = g->locationid + maxsize;
    g->toParent    = g->fromParent + maxsize;
    g->fromParent2 = g->toParent + maxsize;
    g->toParent2   = g->fromParent2 + maxsize;

    g->rc_app = 0;
    g->rc_others = 0;
    g->rc_ownprocess = 0;

    i->group_count++;
    return g;
}

Laik_Instance* laik_inst(Laik_Group* g)
{
    return g->inst;
}

Laik_Group* laik_world(Laik_Instance* i)
{
    Laik_Group* g = i->world;
    g->rc_app = 1;
    return g;
}

void laik_release_group(Laik_Group* g)
{
    g->rc_app = 0;
    // TODO: free if other RCs are zero
}

Laik_Group* laik_group_parent(Laik_Group* g)
{
    return g->parent;
}

void laik_set_world(Laik_Instance* i, Laik_Group* newworld)
{
    // TODO: check that removed processes do not appear in any
    //       active group of this instance

    if (i->world == newworld) return;

    assert(newworld->inst == i);
    newworld->rc_ownprocess++; // inc refcounter: used as world

    i->world = newworld;
    i->epoch++;
}

// create a clone of <g>, derived from <g>
// helper for laik_new_shrinked_group, does not call into backend to track new group
Laik_Group* laik_clone_group(Laik_Group* g)
{
    Laik_Group* g2 = laik_create_group(g->inst, g->size);
    g2->parent = g;
    g2->size = g->size;
    g2->myid = g->myid;

    for(int i=0; i < g->size; i++) {
        g2->toParent[i] = i;
        g2->fromParent[i] = i;
        g2->locationid[i] = g->locationid[i];
    }

    return g2;
}

// helpers for laik_new_union_group

struct lididx
{
    int lid;
    int idx;
};

static
int lididx_cmp(const void *p1, const void *p2)
{
    const struct lididx* li1 = (const struct lididx*) p1;
    const struct lididx* li2 = (const struct lididx*) p2;

    return li1->lid - li2->lid;
}

// create new group as union of 2 groups
Laik_Group* laik_new_union_group(Laik_Group* g1, Laik_Group* g2)
{
    assert(g1->inst == g2->inst);
    int sizesum = g1->size + g2->size;
    struct lididx* li_array = malloc(sizesum * sizeof(struct lididx));
    assert(li_array != 0);

    int off = 0;
    for(int i = 0; i < g1->size; i++) {
        li_array[off].idx = i;
        li_array[off].lid = g1->locationid[i];
        off++;
    }
    for(int i = 0; i < g2->size; i++) {
        li_array[off].idx = g1->size + i;
        li_array[off].lid = g2->locationid[i];
        off++;
    }
    assert(off == sizesum);
    qsort(li_array, sizesum, sizeof(struct lididx), lididx_cmp);

    int lids = 0, lastlid = -1;
    for(int i = 0; i < sizesum; i++) {
        if (lastlid == li_array[i].lid) continue;
        lastlid = li_array[i].lid;
        lids++;
    }

    if (lids == g1->size) {
        // g1 has same LIDs as g1 and g2, so g1 is union
        free(li_array);
        laik_log(1, "union group of %d (size %d, myid %d) + %d (size %d, myid %d): %d",
                 g1->gid, g1->size, g1->myid, g2->gid, g2->size, g2->myid, g1->gid);
        return g1;
    }
    if (lids == g2->size) {
        // g2 has same LIDs as g1 and g2, so g1 is union
        free(li_array);
        laik_log(1, "union group of %d (size %d, myid %d) + %d (size %d, myid %d): %d",
                 g1->gid, g1->size, g1->myid, g2->gid, g2->size, g2->myid, g2->gid);
        return g2;
    }

    Laik_Group* g = laik_create_group(g1->inst, lids);
    g->size = lids;
    g->myid = -1;
    g->parent = g1;
    g->parent2 = g2;
    int gi = -1;
    lastlid = -1;
    for(int i = 0; i < sizesum; i++) {
        if (lastlid != li_array[i].lid) {
            lastlid = li_array[i].lid;
            gi++;
            g->locationid[gi] = lastlid;
            if (g->inst->mylocationid == lastlid)
                g->myid = gi;
            g->toParent[gi] = -1;
            g->toParent2[gi] = -1;
        }
        if (li_array[i].idx < g1-> size) {
            g->fromParent[li_array[i].idx] = gi;
            g->toParent[gi] = li_array[i].idx;
        }
        else {
            g->fromParent2[li_array[i].idx - g1->size] = gi;
            g->toParent2[gi] = li_array[i].idx - g1->size;
        }
    }

    // allow the backend to track the new created group g
    if (g->inst->backend->updateGroup)
        (g->inst->backend->updateGroup)(g);

    if (laik_log_begin(1)) {
        laik_log_append("union group of %d (size %d, myid %d) + %d (size %d, myid %d)",
                        g1->gid, g1->size, g1->myid, g2->gid, g2->size, g2->myid);
        laik_log_append(" => %d (size %d, myid %d):",
                        g->gid, g->size, g->myid);
        laik_log_append("\n  fromParent1 (to union)  : ");
        laik_log_IntList(g1->size, g->fromParent);
        laik_log_append("\n  toParent1   (from union): ");
        laik_log_IntList(g->size, g->toParent);
        laik_log_append("\n  fromParent2 (to union)  : ");
        laik_log_IntList(g2->size, g->fromParent2);
        laik_log_append("\n  toParent2   (from union): ");
        laik_log_IntList(g->size, g->toParent2);
        laik_log_append("\n  toLocation (in union): ");
        laik_log_IntList(g->size, g->locationid);
        laik_log_flush(0);
    }

    free(li_array);
    return g;
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
        g2->locationid[o] = g->locationid[i];
        o++;
    }
    g2->size = o;
    g2->myid = (g->myid < 0) ? -1 : g2->fromParent[g->myid];

    // allow the backend to track the new created group g2
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
        laik_log_IntList(g2->size, g2->locationid);
        laik_log_flush(0);
    }

    return g2;
}

Laik_Group* laik_allow_world_resize(Laik_Instance* instance, int phase)
{
    instance->phase = phase;

    if (instance->backend->resize == 0) {
        // resize not supported by backend
        return instance->world;
    }

    // before starting a new resize, first finish a previous one
    laik_finish_world_resize(instance);

    // LAIK just got back control from an eventually long application
    // phase, so ask backend to progress (if it provides such a function).
    // this may result in queuing incoming join/remove requests
    if (instance->backend->make_progress) {
        (instance->backend->make_progress)();
    }

    // for now, we handle all resize requests directly
    // TODO: use an app-specific resize policy
    Laik_ResizeRequests* reqs = instance->resizeRequests;
    if (laik_log_begin(1)) {
        int jcount = 0, rcount = 0;
        if (reqs && (reqs->used > 0)) {
            for(int r = 0; r < reqs->used; r++) {
                if (reqs->req[r].is_join_req)
                    jcount++;
                else
                    rcount++;
            }
        }
        laik_log_flush("allow-world-resize: %d join + %d remove request(s)",
                       jcount, rcount);
    }

    Laik_Group* g = (instance->backend->resize)(reqs);
    if (reqs)
        reqs->used = 0;

    if (g) {
        laik_set_world(instance, g);
        return g;
    }

    return instance->world;
}

void laik_finish_world_resize(Laik_Instance* instance)
{
    Laik_Group* parent = instance->world->parent;
    if (parent == 0) return;

    if (instance->backend->finish_resize) {
        (instance->backend->finish_resize)();
    }

    laik_release_group(parent);
    instance->world->parent = 0;
}

Laik_ResizeRequests* laik_new_resize_reqs(int size)
{
    Laik_ResizeRequests* reqs;
    reqs = malloc(size * sizeof(Laik_ResizeRequest) + sizeof(Laik_ResizeRequests));
    reqs->size = size;
    reqs->used = 0;

    return reqs;
}

// usually called by a backend which got a join request
static
void laik_add_resize_req(Laik_Instance* instance, bool is_join_req, void* backend_data)
{
    if (instance->resizeRequests == 0) {
        // reserve space for 100 join requests
        instance->resizeRequests = laik_new_resize_reqs(100);
    }

    Laik_ResizeRequests* reqs = instance->resizeRequests;
    assert(reqs->used < reqs->size);
    reqs->req[reqs->used].is_join_req = is_join_req;
    reqs->req[reqs->used].backend_data = backend_data;
    reqs->used++;
}

void laik_add_join_req(Laik_Instance* instance, void* backend_data)
{
    laik_add_resize_req(instance, true, backend_data);
}

void laik_add_remove_req(Laik_Instance* instance, void* backend_data)
{
    laik_add_resize_req(instance, false, backend_data);
}


// locations in KVS

int laik_group_locationid(Laik_Group *group, int id)
{
    assert(id >= 0 && id < group->size);
    return group->locationid[id];
}

static char* locationkey(int loc)
{
    static char key[10];
    snprintf(key, 10, "%i", loc);
    return key;
}

static void update_location(Laik_KVStore* s, Laik_KVS_Entry* e)
{
    int lid = atoi(e->key);
    assert((lid >= 0) && (lid < s->inst->locations));
    s->inst->location[lid] = e->value;
    laik_log(1, "location for locID %d (key '%s') updated to '%s'",
             lid, e->key, e->value);
}

static void remove_location(Laik_KVStore* s, char* key)
{
    int lid = atoi(key);
    assert((lid >= 0) && (lid < s->inst->locations));
    laik_log(1, "location for locID %d (key '%s') removed (was '%s')",
             lid, key, s->inst->location[lid]);
    s->inst->location[lid] = 0;
}


// synchronize location strings via KVS among processes in current world
// TODO: only sync new locations after growth of instance
void laik_sync_location(Laik_Instance *instance)
{
    if (instance->locationStore == NULL) {
        instance->locationStore = laik_kvs_new("location", instance);
        instance->location = (char**) malloc((unsigned)instance->locations * sizeof(char*));
        assert(instance->location != 0);
        for(int i = 0; i < instance->locations; i++)
            instance->location[i] = 0;

        // register function to update direct access to location
        laik_kvs_reg_callbacks(instance->locationStore,
                               update_location, update_location, remove_location);
    }

    Laik_Group* world = laik_world(instance);
    char* mylocation = laik_mylocation(instance);
    int mylocationid = laik_group_locationid(world, laik_myid(world));
    // update location array directly with own location
    instance->location[mylocationid] = mylocation;

    char* myKey = locationkey(mylocationid);
    laik_kvs_sets(instance->locationStore, myKey, mylocation);
    laik_kvs_sync(instance->locationStore);
}

// get location string identifier from process index in given group
char* laik_group_location(Laik_Group *group, int id)
{
    if (group->inst->location == NULL)
        return NULL;

    int lid = laik_group_locationid(group, id);
    assert(lid >= 0 && lid < group->inst->locations);
    return group->inst->location[lid];
}


// Utilities

char* laik_get_guid(Laik_Instance* i){
    return i->guid;
}


