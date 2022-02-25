#pragma once

#include "types.h"
#include "util/except.h"

#include <stdbool.h>
#include <stdint.h>

typedef union object_set_entry {
    object_t* key;
    object_t* value;
} object_set_entry_t;

typedef object_set_entry_t* object_set_t;

typedef struct gc_local_data {
    bool trace_on;
    bool snoop;
    uint8_t alloc_color;
    object_t** buffer;
    object_set_t snooped;
} gc_local_data_t;

err_t init_gc();

object_t* gc_new(type_t* type, size_t count);

void gc_update(object_t* o, size_t offset, object_t* new);

/**
 * Trigger the collection in an async manner
 */
void gc_wake();

/**
 * Trigger the gc and wait for it to finish
 */
void gc_wait();
