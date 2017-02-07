#ifndef _SMARTLIST_H
#define _SMARTLIST_H

#include <stdlib.h>

/*
 * From Tor's src/common/container.h:
 *
 * A resizeable list of pointers, with associated helpful functionality.
 *
 * The members of this struct are exposed only so that macros and inlines can
 * use them; all access to smartlist internals should go through the functions
 * and macros defined here.
 */
typedef struct smartlist_t {
        /*
         * 'list' (of anything) has enough capacity to store exactly 'capacity'
         * elements before it needs to be resized. Only the first 'num_used'
         * (<= capacity) elements point to valid data.
         */
        void **list;
        int    num_used;
        int    capacity;
      } smartlist_t;

int          smartlist_len (const smartlist_t *sl);
void        *smartlist_get (const smartlist_t *sl, int idx);
smartlist_t *smartlist_new (void);
smartlist_t *smartlist_init (smartlist_t *sl);

void  smartlist_free (smartlist_t *sl);
void  smartlist_free_all (smartlist_t *sl);
void  smartlist_ensure_capacity (smartlist_t *sl, size_t num);
void  smartlist_add (smartlist_t *sl, void *element);

typedef int (__cdecl *smartlist_compare_func) (const void *key, const void **member);

void  smartlist_sort (smartlist_t *sl, smartlist_compare_func compare);

int   smartlist_bsearch_idx (const smartlist_t *sl, const void *key,
                             smartlist_compare_func compare, int *found_out);

void *smartlist_bsearch (const smartlist_t *sl, const void *key,
                         smartlist_compare_func compare);

#endif
