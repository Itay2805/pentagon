/*
 * Code taken and modified from Go
 *
 * Copyright (c) 2009 The Go Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *    * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "semaphore.h"

#include <proc/scheduler.h>

#include <stdatomic.h>

void semaphore_queue(semaphore_t* semaphore, waiting_thread_t* wt, bool lifo) {
    waiting_thread_t* t = semaphore->waiters;
    if (lifo) {
        // Substitute wt in t's place in waiters.
        semaphore->waiters = wt;
        wt->ticket = t->ticket;
        wt->wait_link = t;
        wt->wait_tail = t->wait_tail;
        if (wt->wait_tail == NULL) {
            wt->wait_tail = t;
        }
        t->wait_tail = NULL;
    } else {
        // Add wt to the end of t's wait list
        if (t->wait_tail == NULL) {
            t->wait_link = wt;
        } else {
            t->wait_tail->wait_link = wt;
        }
        t->wait_tail = wt;
        t->wait_link = NULL;
    }
}

waiting_thread_t* semaphore_dequeue(semaphore_t* semaphore) {
    waiting_thread_t* wt = semaphore->waiters;
    if (wt->wait_link != NULL) {
        waiting_thread_t* t = wt->wait_link;
        // Substitute t, for wt in the root
        semaphore->waiters = t;
        t->ticket = wt->ticket;
        if (t->wait_link != NULL) {
            t->wait_tail = wt->wait_tail;
        } else {
            t->wait_tail = NULL;
        }
        wt->wait_link = NULL;
        wt->wait_tail = NULL;
    } else {
        semaphore->waiters = NULL;
    }
    wt->ticket = 0;
    return wt;
}


static bool semaphore_can_acquire(semaphore_t* semaphore) {
    while (true) {
        uint32_t v = atomic_load(&semaphore->value);
        if (v == 0) {
            return false;
        }
        if (atomic_compare_exchange_weak(&semaphore->value, &v, v - 1)) {
            return true;
        }
    }
}

void semaphore_acquire(semaphore_t* semaphore, bool lifo) {
    // Easy case
    if (semaphore_can_acquire(semaphore)) {
        return;
    }

    // Harder case:
    //	increment waiter count
    //	try semaphore_can_acquire one more time, return if succeeded
    //	enqueue itself as a waiter
    //	sleep
    //	(waiter descriptor is dequeued by signaler)
    waiting_thread_t* wt = acquire_waiting_thread();
    wt->thread = get_current_thread();

    while (true) {
        spinlock_lock(&semaphore->lock);

        // Add ourselves to nwait to disable "easy case" in semaphore_release
        atomic_fetch_add(&semaphore->nwait, 1);

        // Check semaphore_can_acquire to avoid missed wakeup
        if (semaphore_can_acquire(semaphore)) {
            atomic_fetch_sub(&semaphore->nwait, 1);
            spinlock_unlock(&semaphore->lock);
            break;
        }

        // Any semaphore_release after the semaphore_can_acquire knows we're waiting
        // (we set nwait above), so go to sleep.
        semaphore_queue(semaphore, wt, lifo);

        // park the thread, making sure to unlock our lock
        wt->thread->wait_lock = &semaphore->lock;
        scheduler_park();

        if (wt->ticket != 0 || semaphore_can_acquire(semaphore)) {
            break;
        }
    }

    release_waiting_thread(wt);
}

void semaphore_release(semaphore_t* semaphore, bool handoff) {
    atomic_fetch_add(&semaphore->value, 1);

    // Easy case: no waiters?
    // This check must happen after the add, to avoid a missed wakeup
    // (see loop in semaphore_acquire).
    if (atomic_load(&semaphore->nwait) == 0) {
        return;
    }

    // Harder case: search for a waiter and wake it.
    spinlock_lock(&semaphore->lock);
    if (atomic_load(&semaphore->nwait) == 0) {
        // The count is already consumed by another thread,
        // so no need to wake up another thread.
        spinlock_unlock(&semaphore->lock);
        return;
    }

    waiting_thread_t* wt = semaphore_dequeue(semaphore);
    atomic_fetch_sub(&semaphore->nwait, 1);
    spinlock_unlock(&semaphore->lock);

    if (handoff & semaphore_can_acquire(semaphore)) {
        wt->ticket = 1;
    }

    scheduler_ready_thread(wt->thread);

    if (wt->ticket == 1) {
        // Direct thread handoff
        // scheduler_ready_thread has added the waiter thread as run next in the
        // current cpu, we now call the scheduler so that we start running the
        // waiter thread immediately.
        // Note that the waiter inherits our time slice: this is desirable to avoid
        // having a highly contended semaphore hog the cpu indefinitely. scheduler_yield
        // is like scheduler_schedule, but it puts the current thread on the local run queue
        // instead of the global one. We only do this in the starving regime (handoff=true),
        // as in non-starving case it is possible for a different waiter to acquire the semaphore
        // while we are yielding/scheduling, and this would be wasteful. We wait instead to enter
        // starving regime, and then we do direct handoffs of ticket and cpu.
        scheduler_yield();
    }
}
