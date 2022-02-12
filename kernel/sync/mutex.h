#pragma once

#include "semaphore.h"

#include <stdint.h>

typedef struct mutex {
    int32_t state;
    semaphore_t semaphore;
} mutex_t;

void mutex_lock(mutex_t* mutex);

bool mutex_try_lock(mutex_t* mutex);

void mutex_unlock(mutex_t* mutex);
