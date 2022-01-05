#include "list.h"

#include <stddef.h>
#include <util/except.h>

void list_init(list_t *list) {
    list->prev = list;
    list->next = list;
}

void list_push(list_t* list, list_entry_t* entry) {
    list_t *prev = list->prev;
    entry->prev = prev;
    entry->next = list;
    prev->next = entry;
    list->prev = entry;
}

void list_remove(list_entry_t* entry) {
    list_t *prev = entry->prev;
    list_t *next = entry->next;
    prev->next = next;
    next->prev = prev;
}

list_entry_t* list_pop(list_t* list) {
    list_entry_t* back = list->prev;
    if (back == list) {
        return NULL;
    }
    list_remove(back);
    return back;
}