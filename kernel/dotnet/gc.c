#include "gc.h"

#include "types.h"
#include "util/stb_ds.h"
#include "sync/conditional.h"
#include "heap.h"
#include "loader.h"

#include <threading/scheduler.h>
#include <threading/thread.h>
#include <stdatomic.h>
#include <stdnoreturn.h>

/**
 * Get the gc local data as fs relative pointer, this should allow the compiler
 * to nicely optimize this access, while still allowing us to access it from the
 * tcb which is stored in the thread struct
 */
#define GCL ((gc_local_data_t __seg_fs*)offsetof(thread_control_block_t, gc_local_data))

static int m_color_black = 0;
static int m_color_white = 1;

static System_Object* read_field(System_Object* o, size_t offset) {
    return *(System_Object**)((uintptr_t)o + offset);
}

static void write_field(System_Object* o, size_t offset, System_Object* new) {
    *(System_Object**)((uintptr_t)o + offset) = new;
}

/**
 * Singly linked list of all the allocated objects
 */
static System_Object* m_all_objects = NULL;

void* gc_new(System_Type* type, size_t size) {
    scheduler_preempt_disable();

    System_Object* o = heap_alloc(size);
    o->next = NULL;
    o->color = GCL->alloc_color;
    o->Type = type;

    // add to the all objects list atomically
    o->next = atomic_load_explicit(&m_all_objects, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&m_all_objects, &o->next, o, memory_order_relaxed, memory_order_relaxed));

    scheduler_preempt_enable();

    return o;
}

void gc_update(System_Object* o, size_t offset, System_Object* new) {
    scheduler_preempt_disable();

    if (GCL->trace_on && o->color == m_color_white) {
        if (o->log_pointer) { // object not dirty
            int temp_pos = arrlen(GCL->buffer);

            // TODO: managed pointers
//            size_t* managed_pointer_offsets = o->Type->managed_pointer_offsets;
//            arrsetcap(GCL->buffer, arrlen(GCL->buffer) + arrlen(managed_pointer_offsets));
//            for (int i = 0; i < arrlen(managed_pointer_offsets); i++) {
//                GCL->buffer[++temp_pos] = read_field(o, managed_pointer_offsets[i]);
//            }

            // is it still not dirty?
            if (o->log_pointer == NULL) {
                // committing values in buffer
                arrsetlen(GCL->buffer, temp_pos);

                // set dirty
                o->log_pointer = &GCL->buffer[temp_pos];
            }
        }
    }

    write_field(o, offset, new);

    if (GCL->snoop && new != NULL) {
        stbds_hmput(GCL->snooped, new, new);
    }

    scheduler_preempt_enable();
}

/**
 * This is set once before collection because currently new
 * threads will be created with a badly initialized tcb when
 * the collector is running, the real fix is to just have it
 * so the gc can set the default values for the tcb, but I am
 * too lazy to do it, so for now simply lock the amount of
 * threads we are going to scan, new threads will simply create
 * their objects as black, so it won't matter for us
 */
static int m_thread_count;

static void initiate_collection_cycle() {
    // first handshake
    lock_all_threads();
    m_thread_count = arrlen(g_all_threads);
    for (int i = 0; i < m_thread_count; i++) {
        thread_t* thread = g_all_threads[i];
        if (thread == get_current_thread()) continue;

        suspend_state_t state = scheduler_suspend_thread(thread);
        thread->tcb->gc_local_data.snoop = true;
        scheduler_resume_thread(state);
    }
    unlock_all_threads();

    // give a small room for new threads to be created...

    // second handshake
    lock_all_threads();
    for (int i = 0; i < m_thread_count; i++) {
        thread_t* thread = g_all_threads[i];
        if (thread == get_current_thread()) continue;

        suspend_state_t state = scheduler_suspend_thread(thread);
        thread->tcb->gc_local_data.trace_on = true;
        scheduler_resume_thread(state);
    }
    unlock_all_threads();
}

static object_set_t m_roots = NULL;

static void get_roots() {
    m_color_black = 1 - m_color_black;
    m_color_white = 1 - m_color_white;

    // third handshake
    lock_all_threads();
    for (int i = 0; i < m_thread_count; i++) {
        thread_t* thread = g_all_threads[i];
        if (thread == get_current_thread()) continue;

        suspend_state_t state = scheduler_suspend_thread(thread);
        gc_local_data_t* gcl = &thread->tcb->tcb->gc_local_data;

        gcl->alloc_color = m_color_black;
        gcl->snoop = false;

        // TODO: copy thread local state...

        scheduler_resume_thread(state);
    }
    unlock_all_threads();

    // give a small room for new threads to be created...

    lock_all_threads();
    for (int i = 0; i < m_thread_count; i++) {
        thread_t* thread = g_all_threads[i];
        if (thread == get_current_thread()) continue;

        // copy and clear snooped objects set
        for (int j = 0; j < hmlen(thread->tcb->gc_local_data.snooped); j++) {
            System_Object* o = thread->tcb->gc_local_data.snooped[j].key;
            hmput(m_roots, o, o);
        }
        hmfree(thread->tcb->gc_local_data.snooped);
    }
    unlock_all_threads();

    // add all the globals
    // TODO: dotnet globals

    // runtime globals
    hmput(m_roots, (System_Object*)g_corelib, (System_Object*)g_corelib);
}

static System_Object** m_mark_stack = NULL;

static void trace(System_Object* o) {
    if (o->color == m_color_white) {
        if (o->log_pointer == NULL) {
            // if not dirty

            // getting a replica
            // TODO: managed pointers
//            size_t count = arrlen(o->Type->managed_pointer_offsets);
//            System_Object* temp[count];
//            for (int i = 0; i < count; i++) {
//                temp[i] = read_field(o, o->Type->managed_pointer_offsets[i]);
//            }

            size_t count = 0;
            System_Object* temp[count];

            if (o->log_pointer == NULL) {
                for (int i = 0; i < count; i++) {
                    arrpush(m_mark_stack, temp[i]);
                }
            }
        } else {
            // object is dirty
//            for (int i = 0; i < arrlen(o->Type->managed_pointer_offsets); i++) {
//                int ni = arrlen(o->Type->managed_pointer_offsets) - i - 1;
//                arrpush(m_mark_stack, o->log_pointer[ni]);
//            }
        }

        o->color = m_color_black;
    }
}

static void trace_heap() {
    for (int i = 0; i < hmlen(m_roots); i++) {
        System_Object* o = m_roots[i].key;
        arrpush(m_mark_stack, o);
    }

    while (arrlen(m_mark_stack) != 0) {
        System_Object* o = arrpop(m_mark_stack);
        trace(o);
    }
}

static void sweep() {
    // fourth handshake
    lock_all_threads();
    for (int i = 0; i < m_thread_count; i++) {
        thread_t* thread = g_all_threads[i];
        if (thread == get_current_thread()) continue;
        suspend_state_t state = scheduler_suspend_thread(thread);
        thread->tcb->gc_local_data.trace_on = false;
        scheduler_resume_thread(state);
    }
    unlock_all_threads();

    // special handling for the swept object
    System_Object* last = NULL;
    System_Object* swept = atomic_load_explicit(&m_all_objects, memory_order_relaxed);
    while (swept != NULL) {
        // save the next for when we need it
        System_Object* next = swept->next;

        if (swept->color == m_color_white) {
            // remove from the queue
            if (last == NULL) {
                // removing the first object is a bit special
                // TODO: maybe we can do this just with exchange :think:
                System_Object* first_now = swept;
                if (!atomic_compare_exchange_weak_explicit(&m_all_objects, &first_now, swept->next, memory_order_relaxed, memory_order_relaxed)) {
                    // the compare exchange failed, this means that the swept is no longer
                    // the first item and the object that we have in our hand is the new first
                    // object, starting from it go to the next until we find the current item,
                    // setting the last along the way
                    do {
                        last = first_now;
                        first_now = first_now->next;
                    } while (first_now != swept);

                    // we found it, remove
                    last->next = swept->next;
                } else {
                    // the last is kept as NULL because it is now
                }
            } else {
                // easy case, we know the last, just remove it
                last->next = swept->next;
            }

            // TODO: we technically need to queue this for finalization and
            //       only then destroy it...
            swept->color = COLOR_BLUE;
            TRACE("Freed %p", swept);
            heap_free(swept);
        } else {
            // the last object is this one since it is still alive
            last = swept;
        }

        // set the swept item to be the next item
        swept = next;
    }
}

static void prepare_next_collection() {
    hmfree(m_roots);
    lock_all_threads();
    for (int i = 0; i < m_thread_count; i++) {
        thread_t* thread = g_all_threads[i];
        if (thread == get_current_thread()) continue;

        // Clear all log pointers
        System_Object** buffer = thread->tcb->gc_local_data.buffer;
        for (int j = 0; j < arrlen(buffer); j++) {
            buffer[j]->log_pointer = NULL;
        }

        // clear objects buffer
        arrfree(thread->tcb->gc_local_data.buffer);
    }
    unlock_all_threads();
}

static void gc_collection_cycle() {
    initiate_collection_cycle();
    get_roots();
    trace_heap();
    sweep();
    prepare_next_collection();

    // TODO: heap_flush every so often
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Conductor, allows mutators to trigger the gc
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Is the gc currently running
 */
static atomic_bool m_gc_running = true;

/**
 * Mutex for controlling the running state
 */
static mutex_t m_gc_mutex;

/**
 * Conditional variable for waking the garbage collector
 */
static conditional_t m_gc_wake;

/**
 * Conditional variable for waiting for the gc to be finished on the cycle
 */
static conditional_t m_gc_done;

/**
 * Allows the gc to wait until the next request for a collection
 */
static void gc_conductor_next() {
    m_gc_running = false;
    conditional_broadcast(&m_gc_done);
    do {
        conditional_wait(&m_gc_wake, &m_gc_mutex);
    } while (!m_gc_running);
}

/**
 * Wakeup the garbage collector
 */
static void gc_conductor_wake() {
    if (m_gc_running) {
        // gc is already running or someone
        // already requested it to run
        return;
    }

    m_gc_running = true;
    conditional_signal(&m_gc_wake);
}

/**
 * Wait for the garbage collector
 */
static void gc_conductor_wait() {
    do {
        conditional_wait(&m_gc_done, &m_gc_mutex);
    } while (m_gc_running);
}

void gc_wake() {
    gc_conductor_wake();
}

void gc_wait() {
    mutex_lock(&m_gc_mutex);
    gc_conductor_wake();
    gc_conductor_wait();
    mutex_unlock(&m_gc_mutex);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// GC Thread, actually does the garbage collection
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

noreturn static void gc_thread(void* ctx) {
    TRACE("gc: GC thread started");

    while (true) {
        TRACE("gc: Going to sleep");
        mutex_lock(&m_gc_mutex);
        gc_conductor_next();
        mutex_unlock(&m_gc_mutex);
        TRACE("gc: Starting collection");

        gc_collection_cycle();
    }
}

err_t init_gc() {
    err_t err = NO_ERROR;

    thread_t* thread = create_thread(gc_thread, NULL, "kernel/gc");
    CHECK(thread != NULL);
    scheduler_ready_thread(thread);

    mutex_lock(&m_gc_mutex);
    gc_conductor_wait();
    mutex_unlock(&m_gc_mutex);

cleanup:
    return err;
}
