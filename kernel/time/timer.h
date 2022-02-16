#pragma once

#include <util/except.h>

/**
 * Initialize the timer
 */
err_t init_timer();

/**
 * Gets the TSC frequency
 */
uint64_t get_tsc_freq();

/**
 * Get a timer in microseconds, it has no defined start date, but will
 * always grow upward in microseconds
 */
uint64_t microtime();
