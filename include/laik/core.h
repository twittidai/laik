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

#ifndef LAIK_CORE_H
#define LAIK_CORE_H

#include <stdbool.h>  // for bool

// configuration for a LAIK instance (there may be multiple)
typedef struct _Laik_Instance Laik_Instance;

// LAIK error struct
typedef struct _Laik_Error Laik_Error;

// backend to use for a LAIK instance
typedef struct _Laik_Backend Laik_Backend;

// a task group over which a index space gets distributed
typedef struct _Laik_Group Laik_Group;

// forward decl (for laik_log_set_time)
struct timeval;

/*********************************************************************/
/* Core LAIK API: task groups and elasticity
 *********************************************************************/

/**
 * Generic LAIK initialization function for programs which do not require
 * a specific backend. Users can choose the backend by setting the
 * LAIK_BACKEND environment variable.
 *
 * In contrast, if a program wants to use LAIK together with a specific
 * communication library, it should call the backend initialization directly.
 *
 * For example, with MPI, this function will call MPI_Init. However, if
 * a program already called MPI_Init itself, it should use laik_mpi_init
 * to enable cooperative usage of MPI between the application and LAIK,
 * by passing an MPI communicator to specify the ranks which LAIK is allowed
 * to use.
 */
Laik_Instance* laik_init(int* argc, char*** argv);

//! shut down communication and free resources of this instance
void laik_finalize(Laik_Instance* inst);

//! get unique location ID for the calling process in given instance
int laik_mylocationid(Laik_Instance*);

//! return a backend-dependent string for the location of the calling process
char* laik_mylocation(Laik_Instance*);

//! create a group to be used in this LAIK instance
Laik_Group* laik_create_group(Laik_Instance*, int maxsize);

/**
 * Get group of all processes in this instance marked as active.
 *
 * After LAIK initialization, all processes known to the communication
 * backend are active. LAIK applications can mark processes inactive
 * by collectively calling laik_set_world giving a group not containing
 * these processes.
 *
 * This increments an internal reference counter for the group to keep
 * it valid. If you do not need the group any more, call laik_release_group().
 * Note: groups are immutable. On each change of world, a new group is created.
 */
Laik_Group* laik_world(Laik_Instance* i);

/**
 * Notify LAIK that the application will not use the given group any longer.
 * This allows to free group resources if no process and neither LAIK internally
 * needs the group anymore (all LAIK objects depending on the group are removed).
 */
void laik_release_group(Laik_Group* g);

/**
 * Return parent group of a given group
 */
Laik_Group* laik_group_parent(Laik_Group* g);

/**
 * Specify a new group of active processes for this instance
 *
 * This function must be called collectively by all active
 * processes in the instance.
 * Processes not in the new world will get deactivated and
 * can call laik_finalized or laik_wait for reactivation.
 * The new group may include newly joining processes
 * not yet part of this instance, still waiting in LAIK
 * initalization or reactivation. Create a group with such
 * waiting processes by calling laik_new_joining_group.
 */
void laik_set_world(Laik_Instance* i, Laik_Group* newworld);

//! Get instance the group is created in
Laik_Instance* laik_inst(Laik_Group*);

//! Return number of processes in a LAIK process group
int laik_size(Laik_Group*);

//! Return the id of the calling process in a LAIK process group
int laik_myid(Laik_Group*);

//! Return current program phase, important for new joining processes
int laik_phase(Laik_Instance*);

//! Return instance epoch, incremented at every world size change
int laik_epoch(Laik_Instance*);

//! Create a clone of process group
Laik_Group* laik_clone_group(Laik_Group* g);

// create new group as union of 2 groups
Laik_Group* laik_new_union_group(Laik_Group* g1, Laik_Group* g2);

//! Create group by removing <len> processes from <g> as given in <list>
Laik_Group* laik_new_shrinked_group(Laik_Group* g, int len, int* list);

/**
 * Allow LAIK to remove/add processes to current world.
 * Reasons for changes may be external requests to remove processes
 * or new processes wanting to join.
 *
 * On change, a new process group reflecting the changes is created,
 * the original world of the instance is set to be parent of the new
 * group (see laik_parent), the world (returned by laik_world) is
 * replaced by the new group, and the new group is returned.
 * The old world is still valid. Call laik_finish_world_resize() to
 * explicitly free the resources of the old world group.
 * Change of world triggers an increment of the epoch counter
 * (returned from laik_epoch).
 * 
 * If no change happens, the old world is returned.
 *
 * If the original world, when calling this function, still has a
 * parent set (from a previous resize), its resources first will be
 * freed.
 *
 * The given <phase> will be passed to new joining processes which can
 * request the phase via laik_phase() after LAIK initialzation.
 * This tells new processes where to start and join computation.
 *
 * This function needs to be called by all processes in the world group.
 */
Laik_Group* laik_allow_world_resize(Laik_Instance* instance, int phase);

/**
 * Notify LAIK that resize adaptation of world is done.
 * This is a promise that the old world (parent of current world) will
 * not be used any more, and all resources can be freed.
 *
 * Calling this function is optional after laik_allow_world_resize().
 * Another resize will free resources of a previous resize automatically.
 */
void laik_finish_world_resize(Laik_Instance*);


// get location ID from process ID in given group
int laik_group_locationid(Laik_Group *group, int id);

// get location string identifier from process ID in given group
// (returns 0 if location strings not synchronized)
char* laik_group_location(Laik_Group *group, int id);

// change the master to task <id>. Return if successful
bool laik_set_master(Laik_Group* g, int id);

//! get the ID if the master task in this group
int laik_get_master(Laik_Group* g);

//! return true if I am master
bool laik_is_master(Laik_Group* g);

// Return true if own process is managed by LAIK instance
// a process may become unmanaged by shrinking the world group.
bool laik_is_managed(Laik_Instance* i);



// Utilities

// These are used for generate a mapping between e.g. MPI Tasks LAIK 
// instances
char* laik_get_guid(Laik_Instance* i);

// profiling

// start profiling for given instance
void laik_enable_profiling(Laik_Instance* i);
void laik_reset_profiling(Laik_Instance* i);
void laik_enable_profiling_file(Laik_Instance* i, const char* filename);
void laik_close_profiling_file(Laik_Instance* i);
double laik_get_total_time(void);
double laik_get_backend_time(void);
void laik_writeout_profile(void);
void laik_profile_printf(const char* msg, ...);
void laik_profile_user_start(Laik_Instance *i);
void laik_profile_user_stop(Laik_Instance* i);



/*********************************************************************/
/* Core LAIK API: Logging
 *********************************************************************/

//! Log levels control whether a log message should be shown to user.
// Default is to only show Error/Panic messages.
// Set environment variable LAIK_LOG to minimum level (integer) to see.
typedef enum _Laik_LogLevel {
    LAIK_LL_None = 0,
    LAIK_LL_Debug,
    LAIK_LL_Info,
    LAIK_LL_Warning, // prefix with "Warning"
    LAIK_LL_Error,   // prefix with "Error"
    LAIK_LL_Panic    // prefix with "Panic" and immediately exit
} Laik_LogLevel;

// initialize logging for instance <i>
void laik_log_init(Laik_Instance* i);

// for early-init before instance is known
void laik_log_init_loc(char* mylocation);

// cleanup logging of instance <i>
void laik_log_cleanup(Laik_Instance* i);

// reset start time for log output
void laik_log_set_time(struct timeval* t);

// increment logging counter used in logging prefix
void laik_log_inc(void);

// log a message, similar to printf
void laik_log(int level, const char* msg, ...) __attribute__ ((format (printf, 2, 3)));

// panic: terminate application
void laik_panic(const char* msg);

// check for log level: return true if given log level will be shown
// use this to guard possibly complex calculations for debug output
bool laik_log_shown(int level);

// to overwrite environment variable LAIK_LOG
void laik_set_loglevel(int level);

// buffered log API: same as laik_log, but allows to build message in steps

// begin a new log message with log level <l>
// returns true if message will be shown; can be used instead of laik_logshown
bool laik_log_begin(int level);
// append to a log message started before with laik_log_begin
void laik_log_append(const char* msg, ...);
// finalize the log message build with laik_log_begin/append and print it
void laik_log_flush(const char* msg, ...);


/*********************************************************************/
/* KV Store
 *********************************************************************/

// - with explicit request for synchronization
// - multiple KVS possible with own synchronization
// - entries may be shadows
//
// current restrictions:
// - flat
// - sync only among all processes of current world

// a flat KV store
typedef struct _Laik_KVStore Laik_KVStore;

// an entry in a KV Store
typedef struct _Laik_KVS_Entry Laik_KVS_Entry;

// functions optionally called on KVS changes (see laik_kvs_reg_callbacks)
typedef void (*laik_kvs_created_func)(Laik_KVStore*, Laik_KVS_Entry*);
typedef void (*laik_kvs_changed_func)(Laik_KVStore*, Laik_KVS_Entry*);
typedef void (*laik_kvs_removed_func)(Laik_KVStore*, char*);

// create KVS among processes of current world in instance
Laik_KVStore* laik_kvs_new(const char *name, Laik_Instance* inst);

// free KVS resources
void laik_kvs_free(Laik_KVStore* kvs);

// set a binary data blob as value for key (deep copy, overwrites if key exists)
Laik_KVS_Entry* laik_kvs_set(Laik_KVStore* kvs, char* key, unsigned int size, char* data);

// set a null-terminated string as value for key
Laik_KVS_Entry* laik_kvs_sets(Laik_KVStore* kvs, char* key, char* str);

// remove all entries
void laik_kvs_clean(Laik_KVStore* kvs);

// remove entry for key
bool laik_kvs_remove(Laik_KVStore* kvs, char* key);

// synchronize KV store
void laik_kvs_sync(Laik_KVStore* kvs);

// get data and size via *psize (warning: may get invalid on updates)
char* laik_kvs_get(Laik_KVStore* kvs, char* key, unsigned int *psize);

// get entry for a name
Laik_KVS_Entry* laik_kvs_entry(Laik_KVStore* kvs, char* key);

// get current number of entries
unsigned int laik_kvs_count(Laik_KVStore* kvs);

// get specific entry (warning: order may change an updates)
Laik_KVS_Entry* laik_kvs_getn(Laik_KVStore* kvs, unsigned int n);

// get key of an entry
char* laik_kvs_key(Laik_KVS_Entry* e);

// get size of entry data
unsigned int laik_kvs_size(Laik_KVS_Entry* e);

// get data and size via *psize (warning: may get invalid on updates)
char* laik_kvs_data(Laik_KVS_Entry* e, unsigned int *psize);

// deep copy of entry data, at most <size> bytes, returns bytes copied
unsigned int laik_kvs_copy(Laik_KVS_Entry* e, char* mem, unsigned int size);

// sort KVS entries for faster access (done after sync)
void laik_kvs_sort(Laik_KVStore* kvs);

// register functions to be called when entries are created/changed/removed in sync
void laik_kvs_reg_callbacks(Laik_KVStore* kvs,
                            laik_kvs_created_func fc,
                            laik_kvs_changed_func fu,
                            laik_kvs_removed_func fr);

#endif // LAIK_CORE_H
