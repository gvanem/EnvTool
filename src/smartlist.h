/** \file smartlist.h
 *  \ingroup Misc
 */
#ifndef _SMARTLIST_H
#define _SMARTLIST_H

#define SMARTLIST_FOREACH(sl)  for (i = 0; i < smartlist_len(sl); i++)
#define SMARTLIST_EMPTY(sl)    (smartlist_len(sl) == 0)

typedef struct smartlist_t smartlist_t;  /* Opaque struct; defined in smartlist.c */

typedef int  (*smartlist_sort_func) (const void **a, const void **b);
typedef int  (*smartlist_compare_func) (const void *key, const void **member);
typedef void (*smartlist_parse_func) (smartlist_t *sl, const char *line);
typedef void (*smartlist_free_func) (void *a);

smartlist_t *smartlist_new  (void);
int          smartlist_len  (const smartlist_t *sl);
void        *smartlist_get  (const smartlist_t *sl, int idx);
unsigned     smartlist_getu (const smartlist_t *sl, int idx);
void         smartlist_set  (smartlist_t *sl, int idx, void *val);

void     smartlist_free (smartlist_t *sl);
void     smartlist_free_all (smartlist_t *sl);
void   **smartlist_ensure_capacity (smartlist_t *sl, size_t num);
void    *smartlist_add (smartlist_t *sl, void *element);
unsigned smartlist_addu (smartlist_t *sl, unsigned element);
char    *smartlist_add_strdup (smartlist_t *sl, const char *str);
void     smartlist_del (smartlist_t *sl, int idx);
void     smartlist_del_keeporder (smartlist_t *sl, int idx);
size_t   smartlist_append (smartlist_t *sl1, const smartlist_t *sl2);
void     smartlist_insert (smartlist_t *sl, int idx, void *val);
void     smartlist_swap (smartlist_t *sl, int idx1, int idx2);
int      smartlist_pos (const smartlist_t *sl, const void *element);

void     smartlist_clear (smartlist_t *sl);
void     smartlist_wipe (smartlist_t *sl, smartlist_free_func free_fn);

int      smartlist_duplicates (smartlist_t *sl, smartlist_sort_func compare);
int      smartlist_make_uniq (smartlist_t *sl, smartlist_sort_func compare, smartlist_free_func free_fn);

void     smartlist_sort (smartlist_t *sl, smartlist_sort_func compare);

int      smartlist_bsearch_idx (const smartlist_t *sl, const void *key,
                                smartlist_compare_func compare, int *found_out);

void    *smartlist_bsearch (const smartlist_t *sl, const void *key,
                            smartlist_compare_func compare);

smartlist_t *smartlist_read_file (smartlist_parse_func parse, const char *file_fmt, ...);
int          smartlist_write_file (smartlist_t *sl, const char *file_fmt, ...);

smartlist_t *smartlist_split_str (const char *val, const char *sep);
char        *smartlist_join_str (smartlist_t *sl, const char *sep);

#ifdef _DEBUG
  /*
   * Some helpful bug-hunter versions and macros.
   */
  int      smartlist_len_dbg        (const smartlist_t *sl, const char *sl_name, const char *file, unsigned line);
  void    *smartlist_get_dbg        (const smartlist_t *sl, int idx, const char *sl_name, const char *file, unsigned line);
  unsigned smartlist_getu_dbg       (const smartlist_t *sl, int idx, const char *sl_name, const char *file, unsigned line);
  void    *smartlist_add_dbg        (smartlist_t *sl, void *element, const char *sl_name, const char *file, unsigned line);
  unsigned smartlist_addu_dbg       (smartlist_t *sl, unsigned element, const char *sl_name, const char *file, unsigned line);
  char    *smartlist_add_strdup_dbg (smartlist_t *sl, const char *string, const char *sl_name, const char *file, unsigned line);

  #define smartlist_len(sl)             smartlist_len_dbg (sl, #sl, __FILE(), __LINE__)
  #define smartlist_get(sl, idx)        smartlist_get_dbg (sl, idx, #sl, __FILE(), __LINE__)
  #define smartlist_getu(sl, idx)       smartlist_getu_dbg (sl, idx, #sl, __FILE(), __LINE__)
  #define smartlist_add(sl, elem)       smartlist_add_dbg (sl, elem, #sl, __FILE(), __LINE__)
  #define smartlist_addu(sl, elem)      smartlist_addu_dbg (sl, elem, #sl, __FILE(), __LINE__)
  #define smartlist_add_strdup(sl, str) smartlist_add_strdup_dbg (sl, str, #sl, __FILE(), __LINE__)
#endif

#endif
