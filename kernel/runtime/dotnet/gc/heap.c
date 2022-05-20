#include "heap.h"

#include "gc.h"
#include "kernel.h"
#include "proc/cpu_local.h"

#include <proc/thread.h>
#include <util/string.h>
#include <util/defs.h>
#include <mem/mem.h>

#include <stdatomic.h>

//
// the object heap is used to allocate objects, it starts 1TB after the direct
// map start, and it contains multiple areas.
//
// The total size of the pool
//
// Each pool is 512GB large, and can be used to allocate a different size of object, the dirty bit
//  0 - 16 byte objects
//  1 - 32 byte objects
//  2 - 64 byte objects
//  3 - 128 byte objects
//  4 - 256 byte objects
//  5 - 512 byte objects
//  6 - 1kb byte objects
//  7 - 2kb byte objects
//  8 - 4kb byte objects
//  9 - 8kb objects
// 10 - 16kb objects
// 11 - 32kb objects
// 12 - 64kb objects
// 13 - 128kb objects
// 14 - 256kb objects
// 15 - 512kb objects
// 16 - 1MB objects
// 17 - 2MB objects
// 18 - 4MB objects
// 19 - 8MB objects
// 20 - 16MB objects
// 21 - 32MB objects
// 22 - 64MB objects
// 23 - 128MB objects
// 24 - 256MB objects
// 25 - 512MB objects
//
// per object size pool we are going to have N regions, where the N is (512 / CpuCount),
// each core is not going to lock by itself
//

/**
 * The amount of top-level pools we have
 */
#define POOL_COUNT 26

/**
 * The amount of subpools we have over each object size pool
 */
#define SUBPOOLS_COUNT 512

/**
 * How many subpools each lock protects
 */
#define SUBPOOLS_PER_LOCK (512 / get_cpu_count())

/**
 * Locks, cpu_count per top-level pool
 */
static spinlock_t* m_heap_locks;

/**
 * Locks for the top level
 */
static spinlock_t m_top_level_locks[POOL_COUNT];

err_t init_heap() {
    err_t err = NO_ERROR;

    // we can only have up to 512 cores so we have enough locks
    CHECK(get_cpu_count() < 512);

    // allocate all the locks
    m_heap_locks = malloc(POOL_COUNT * SUBPOOLS_PER_LOCK * sizeof(spinlock_t));
    CHECK(m_heap_locks != NULL);

    // setup the top levels
    int num = 16;
    for (int pml4i = PML4_INDEX(OBJECT_HEAP_START); pml4i < PML4_INDEX(OBJECT_HEAP_START) + POOL_COUNT; pml4i++) {
        // allocate it
        void* page = palloc(PAGE_SIZE);
        CHECK_ERROR(page != NULL, ERROR_OUT_OF_RESOURCES);

        // set it
        PAGE_TABLE_PML4[pml4i] = (page_entry_t){
            .present = 1,
            .writeable = 1,
            .frame = DIRECT_TO_PHYS(page) >> 12
            // TOOD: set everything is empty
        };

        // unmap it
        vmm_unmap_direct_page(DIRECT_TO_PHYS(page));
    }

cleanup:
    return err;
}

/**
 * Gives an approximate of the object size
 * according to the pool it is in
 */
static int calc_object_size(uintptr_t obj) {
    size_t poolidx = (obj - OBJECT_HEAP_START) / SIZE_512GB;
    return 2 << (4 + poolidx);
}

System_Object heap_find(uintptr_t ptr) {
    if (OBJECT_HEAP_START <= ptr && ptr < OBJECT_HEAP_END) {
        size_t size = calc_object_size(ptr);

        // make sure it is present
        if (!PAGE_TABLE_PML3[PML3_INDEX(ptr)].present) return NULL;
        if (!PAGE_TABLE_PML2[PML2_INDEX(ptr)].present) return NULL;

        // if it is less than 2MB then need to check the PML1 as well
        if (size < SIZE_2MB && !PAGE_TABLE_PML1[PML1_INDEX(ptr)].present) return NULL;

        // if in the range of the heap, align down to the object size
        return (System_Object)ALIGN_DOWN(ptr, size);
    }
    return NULL;
}

System_Object heap_alloc(size_t size) {
    // check if we support this allocation
    if (size > SIZE_512MB) {
        return NULL;
    }

    // TODO: track wasted space cause why not

    // get the aligned size by finding the next power of two
    size_t aligned_size = 1ull << (64 - __builtin_clzll(size - 1));

    // now get the pool index from the size by using log2
    int pool_idx = (64 - __builtin_clzll(aligned_size - 1)) - 4;

    // the last taken lock, for easier lock management
    spinlock_t* last_lock_taken = NULL;

    // the allocated object, can be null if oom
    System_Object allocated = NULL;

    // some constants for the searching
    pml_index_t pml4i = pool_idx + PML4_INDEX(OBJECT_HEAP_START);

    // go over each 1GB region in the pool, making sure that each of the pools
    // actually exists
    for (int subpool_idx = 0; subpool_idx < SUBPOOLS_COUNT; subpool_idx++) {
        pml_index_t pml3i = (pml4i << 9) + subpool_idx;

        // we got to the first pool in the subpool region, lock the region
        if ((subpool_idx % SUBPOOLS_PER_LOCK) == 0) {
            // unlock the last lock taken
            if (last_lock_taken != NULL) {
                spinlock_unlock(last_lock_taken);
            }

            // now take a new lock
            last_lock_taken = &m_heap_locks[pool_idx * POOL_COUNT + subpool_idx / SUBPOOLS_PER_LOCK];

            if (!spinlock_try_lock(last_lock_taken)) {
                // we didn't lock it
                last_lock_taken = NULL;

                // region is locked, skip it entirely, it is fine to not try
                // again later because there are as many regions as running
                // cpus and this code can't be preemptible
                subpool_idx += SUBPOOLS_PER_LOCK - 1;
                continue;
            }
        }

        if (!PAGE_TABLE_PML3[pml3i].present) {
            // allocate this
            if (!vmm_setup_level(PAGE_TABLE_PML3, PAGE_TABLE_PML2, pml3i)) {
                WARN("heap: out of memory trying to setup subpool");

                // failed to allocate this region, try another one
                continue;
            }
        }

        if (aligned_size >= SIZE_2MB) {
            // the objects are larger than 2MB, so we can skip at the object size and simply check
            // the PML2 per object to check if it is present
            for (uintptr_t ptr = PML3_BASE(pml3i); ptr < PML3_BASE(pml3i) + SIZE_1GB; ptr += aligned_size) {
                pml_index_t pml2i = PML2_INDEX(ptr);

                if (!PAGE_TABLE_PML2[PML2_INDEX(ptr)].present) {
                    // allocate the whole object

                    bool allocated_it = true;
                    for (int i = 0; i < aligned_size / SIZE_2MB; i++) {
                        void* page = palloc(SIZE_2MB);
                        if (page == NULL) {
                            WARN("heap: out of memory allocating %S object (with 2MB pages)", aligned_size);

                            // free all the pages that we tried to allocate
                            while (i--) {
                                uintptr_t phys = PAGE_TABLE_PML2[pml2i + i].frame;
                                void* direct = PHYS_TO_DIRECT(phys);
                                vmm_map(phys, direct, SIZE_2MB / PAGE_SIZE, MAP_WRITE);
                                pfree(direct);
                            }

                            allocated_it = false;
                            break;
                        }

                        // setup the page table entry
                        PAGE_TABLE_PML2[pml2i + i] = (page_entry_t){
                            .huge_page = 1,
                            .writeable = 1,
                            .present = 1,
                            .frame = DIRECT_TO_PHYS(page) >> 12
                            // TODO: set that we have all the entries
                        };

                        // unmap the physical page
                        for (int j = 0; j < SIZE_2MB / PAGE_SIZE; j++) {
                            vmm_unmap_direct_page(DIRECT_TO_PHYS(page + j * PAGE_SIZE));
                        }
                    }

                    if (!allocated_it) {
                        continue;
                    }
                }

                // return the object if it is empty
                System_Object object = (System_Object) ptr;
                if (object->color == COLOR_BLUE) {
                    allocated = object;
                    goto exit;
                }
            }
        } else {
            // the objects are smaller than 2MB, meaning each PML2 has multiple objects,
            // so iterate it
            for (pml_index_t pml2i = pml3i << 9; pml2i < (pml3i << 9) + 512; pml2i++) {
                if (!PAGE_TABLE_PML2[pml2i].present) {
                    if (!vmm_setup_level(PAGE_TABLE_PML2, PAGE_TABLE_PML1, pml2i)) {
                        WARN("heap: out of memory trying to setup PML2 for 4KB pools");
                        continue;
                    }
                }

                if (aligned_size >= SIZE_4KB) {
                    // the objects are larger than 4KB, so we can skip at the object size
                    // and simply check the PML1 per object to check if it is present
                    for (uintptr_t ptr = PML1_BASE(pml2i); ptr < PML1_BASE(pml2i) + SIZE_2MB; ptr += aligned_size) {
                        pml_index_t pml1i = PML1_INDEX(ptr);

                        if (!PAGE_TABLE_PML1[pml1i].present) {
                            // allocate the whole object

                            bool allocated_it = true;
                            for (pml_index_t i = 0; i < aligned_size / PAGE_SIZE; i++) {
                                void* page = palloc(SIZE_4KB);
                                if (page == NULL) {
                                    WARN("heap: out of memory allocating %S object (with 4KB pages)", aligned_size);

                                    // free all the pages that we tried to allocate
                                    while (i--) {
                                        uintptr_t phys = PAGE_TABLE_PML1[pml1i + i].frame;
                                        void* direct = PHYS_TO_DIRECT(phys);
                                        vmm_map(phys, direct, 1, MAP_WRITE);
                                        pfree(direct);
                                    }

                                    allocated_it = false;
                                    break;
                                }

                                // setup the page table entry
                                PAGE_TABLE_PML1[pml1i + i] = (page_entry_t){
                                    .writeable = 1,
                                    .present = 1,
                                    .frame = DIRECT_TO_PHYS(page) >> 12
                                    // TODO: set that we have all the entries
                                };

                                // unmap the physical page
                                vmm_unmap_direct_page(DIRECT_TO_PHYS(page));
                            }

                            if (!allocated_it) {
                                continue;
                            }
                        }

                        // TODO: check if we have enough entries

                        // return the object if it is empty
                        System_Object object = (System_Object) ptr;
                        if (object->color == COLOR_BLUE) {

                            // TODO: decrement entries and update the rest of the levels

                            allocated = object;
                            goto exit;
                        }
                    }
                } else {
                    // the objects are smaller than 4KB, meaning each PML1 has multiple
                    // objects, so iterate it
                    for (pml_index_t pml1i = pml2i << 9; pml1i < (pml2i << 9) + 512; pml1i++) {
                        if (!PAGE_TABLE_PML1[pml1i].present) {
                            void* page = palloc(SIZE_4KB);
                            if (page == NULL) {
                                WARN("heap: out of memory allocating 4kb card for %S object", aligned_size);
                                continue;
                            }

                            // setup the page table entry
                            PAGE_TABLE_PML1[pml1i] = (page_entry_t){
                                .present = 1,
                                .writeable = 1,
                                .frame = DIRECT_TO_PHYS(page) >> 12
                                // TODO: set that we have all the entries
                            };

                            // unmap the physical page
                            vmm_unmap_direct_page(DIRECT_TO_PHYS(page));
                        }

                        // TODO: check if we have enough entries

                        // just iterate all of them
                        for (uintptr_t ptr = PML1_BASE(pml1i); ptr < PML1_BASE(pml1i) + SIZE_4KB; ptr += aligned_size) {
                            System_Object object = (System_Object) ptr;
                            if (object->color == COLOR_BLUE) {

                                // TODO: decrement entries and update the rest of the levels

                                allocated = object;
                                goto exit;
                            }
                        }
                    }
                }
            }
        }
    }

exit:
    // if we still have a taken lock then free it now
    if (last_lock_taken != NULL) {
        spinlock_unlock(last_lock_taken);
    }

    // return the newly allocated object (or null)
    return allocated;
}

void heap_iterate_dirty_objects(object_callback_t callback) {
    spinlock_t* last_lock_taken = NULL;

    // iterate over all the top-level pools, each having 512 sub-pools
    for (int pool_idx = 0; pool_idx < POOL_COUNT; pool_idx++) {
        pml_index_t pml4i = pool_idx + PML4_INDEX(OBJECT_HEAP_START);
        size_t pool_object_size = 2 << (4 + pool_idx);

        // go over each 1GB region in the pool, making sure that each of the pools
        // actually exists
        for (int subpool_idx = 0; subpool_idx < SUBPOOLS_COUNT; subpool_idx++) {
            pml_index_t pml3i = pml4i << 9;

            // we got to the first pool in the subpool region, lock the region
            if ((subpool_idx % SUBPOOLS_PER_LOCK) == 0) {
                // unlock the last lock taken
                if (last_lock_taken != NULL) {
                    spinlock_unlock(last_lock_taken);
                }

                // now take a new lock
                last_lock_taken = &m_heap_locks[pool_idx * POOL_COUNT + subpool_idx / SUBPOOLS_PER_LOCK];
                spinlock_lock(last_lock_taken);
            }

            // if it is not present skip
            if (!PAGE_TABLE_PML3[pml3i].present) continue;

            // iterate all the 2mb ranges in the pool
            for (pml_index_t pml2i = pml3i << 9; pml2i < (pml3i << 9) + 512; pml2i++) {
                if (!PAGE_TABLE_PML2[pml2i].present) continue;

                if (pool_object_size >= SIZE_2MB) {
                    // for 2MB+ objects we only have up to PML2, so we can start doing
                    // the iteration here

                    // if not dirty continue
                    if (!PAGE_TABLE_PML2[pml2i].dirty) {
                        continue;
                    }

                    // iterate the objects in the card if we have a callback
                    if (callback != NULL) {
                        // get the object base and iterate it until the pool end
                        uintptr_t object_base = ALIGN_DOWN(pml2i << 12, pool_object_size);
                        for (uintptr_t obj = object_base; obj < (pml2i + 1) << 12; obj += pool_object_size) {
                            System_Object object = (System_Object)obj;
                            callback(object);
                        }
                    }

                    // clear the dirty bit, must be done after we touch
                    // all the objects
                    PAGE_TABLE_PML1[pml2i].dirty = 0;
                } else {
                    // iterate all the PML1s of the 2MB pages
                    for (pml_index_t pml1i = pml2i << 9; pml1i < (pml2i << 9) + 512; pml1i++) {
                        if (!PAGE_TABLE_PML1[pml1i].present) continue;

                        // if not dirty continue
                        if (!PAGE_TABLE_PML1[pml1i].dirty) {
                            continue;
                        }

                        // iterate the objects in the card if we have a callback
                        if (callback != NULL) {
                            // get the object base and iterate it until the pool end
                            uintptr_t object_base = ALIGN_DOWN(pml1i << 12, pool_object_size);
                            for (uintptr_t obj = object_base; obj < (pml1i + 1) << 12; obj += pool_object_size) {
                                System_Object object = (System_Object)obj;
                                callback(object);
                            }
                        }

                        // clear the dirty bit, must be done after we touch
                        // all the objects
                        PAGE_TABLE_PML1[pml1i].dirty = 0;
                    }
                }
            }
        }
    }

    // if we still have a taken lock then free it now
    if (last_lock_taken != NULL) {
        spinlock_unlock(last_lock_taken);
    }
}

void heap_iterate_objects(object_callback_t callback) {
    spinlock_t* last_lock_taken = NULL;

    // iterate over all the top-level pools, each having 512 sub-pools
    for (int pool_idx = 0; pool_idx < POOL_COUNT; pool_idx++) {
        pml_index_t pml4i = pool_idx + PML4_INDEX(OBJECT_HEAP_START);
        size_t pool_object_size = 2 << (4 + pool_idx);

        // go over each 1GB region in the pool, making sure that each of the pools
        // actually exists
        for (int subpool_idx = 0; subpool_idx < SUBPOOLS_COUNT; subpool_idx++) {
            pml_index_t pml3i = pml4i << 9;

            // we got to the first pool in the subpool region, lock the region
            if ((subpool_idx % SUBPOOLS_PER_LOCK) == 0) {
                // unlock the last lock taken
                if (last_lock_taken != NULL) {
                    spinlock_unlock(last_lock_taken);
                }

                // now take a new lock
                last_lock_taken = &m_heap_locks[pool_idx * POOL_COUNT + subpool_idx / SUBPOOLS_PER_LOCK];
                spinlock_lock(last_lock_taken);
            }

            if (!PAGE_TABLE_PML3[pml3i].present) continue;

            if (pool_object_size >= SIZE_2MB) {
                // the objects are larger than 2MB, so we can skip at the object size and simply check
                // the PML2 per object to check if it is present
                for (uintptr_t ptr = PML3_BASE(pml3i); ptr < PML3_BASE(pml3i) + SIZE_1GB; ptr += pool_object_size) {
                    if (!PAGE_TABLE_PML2[PML2_INDEX(ptr)].present) continue;
                    callback((System_Object)ptr);
                }
            } else {
                // the objects are smaller than 2MB, meaning each PML2 has multiple objects,
                // so iterate it
                for (pml_index_t pml2i = pml3i << 9; pml2i < (pml3i << 9) + 512; pml2i++) {
                    if (!PAGE_TABLE_PML2[pml2i].present) continue;

                    if (pool_object_size >= SIZE_4KB) {
                        // the objects are larger than 4KB, so we can skip at the object size
                        // and simply check the PML1 per object to check if it is present
                        for (uintptr_t ptr = PML2_BASE(pml2i); ptr < PML2_BASE(pml2i) + SIZE_2MB; ptr += pool_object_size) {
                            if (!PAGE_TABLE_PML1[PML1_INDEX(ptr)].present) continue;
                            callback((System_Object)ptr);
                        }
                    } else {
                        // the objects are smaller than 4KB, meaning each PML1 has multiple
                        // objects, so iterate it
                        for (pml_index_t pml1i = pml2i << 9; pml1i < (pml2i << 9) + 512; pml1i++) {
                            if (!PAGE_TABLE_PML1[pml1i].present) continue;

                            // just iterate all of them
                            for (uintptr_t ptr = PML2_BASE(pml1i); ptr < PML2_BASE(pml1i) + SIZE_4KB; ptr += pool_object_size) {
                                callback((System_Object)ptr);
                            }
                        }
                    }
                }
            }
        }
    }

    // if we still have a taken lock then free it now
    if (last_lock_taken != NULL) {
        spinlock_unlock(last_lock_taken);
    }
}
