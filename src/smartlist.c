/**\file    smartlist.c
 * \ingroup Misc
 * \brief
 *   Functions for dynamic arrays.
 */
#include "envtool.h"
#include "smartlist.h"

/**
 * \typedef struct smartlist_t
 *
 * From Tor's `src/lib/smartlist_core/smartlist.h`:
 *
 * A resizeable list of pointers, with associated helpful functionality.
 *
 * The members of this struct are exposed only so that macros and inlines can
 * use them; all access to smartlist internals should go through the functions
 * and macros defined here.
 */
typedef struct smartlist_t {
        /**
         * `list` (of anything) has enough capacity to store exactly `capacity`
         * elements before it needs to be resized. Only the first `num_used`
         * (`<= capacity`) elements point to valid data.
         */
        void **list;
        int    num_used;
        int    capacity;
      } smartlist_t;

/**
 * \def SMARTLIST_DEFAULT_CAPACITY
 *
 * All newly allocated smartlists have this capacity.
 * I.e. room for 16 elements in `smartlist_t::list[]`.
 */
#define SMARTLIST_DEFAULT_CAPACITY  16

/**
 * \def SMARTLIST_MAX_CAPACITY
 *
 * A smartlist can hold `INT_MAX` (2147483647) number of
 * elements in `smartlist_t::list[]`.
 */
#define SMARTLIST_MAX_CAPACITY  INT_MAX

#if defined(_CRTDBG_MAP_ALLOC) && !defined(USE_ASAN) /* Implies '_DEBUG' defined */
 /**
  * \def ASSERT_VAL
  *
  * In MSVC `_DEBUG` mode (`-MDd`), if `free()' was called on `sl`, the start-value
  * (or the whole block?) gets filled with `0xDDDDDDD`. <br>
  * Also the value `0xFDFDFDFD` is automatically placed before and after the heap block. <br>
  * Ref:
  *   http://www.highprogrammer.com/alan/windev/visualstudio.html
  *
  * Do not do this for ASAN (CFLAGS contains `-DUSE_ASAN -fsanitize=address`), since
  * that could trigger an exception when reading the DWORD value. I.e. this could be
  * 3 bytes into the "Heap right redzone".
  * Ref:
  *   https://stackoverflow.com/questions/60476634/addresssanitizer-what-do-these-terms-mean
  */
  #define ASSERT_VAL(ptr) do {                                        \
                            const DWORD *_val = (const DWORD*) (ptr); \
                            ASSERT (*_val != 0xDDDDDDDD);             \
                            ASSERT (*_val != 0xFDFDFDFD);             \
                          } while (0)
#else
  #define ASSERT_VAL(ptr) (void) 0
#endif

#undef smartlist_len
#undef smartlist_get
#undef smartlist_getu

/**
 * Return the number of items in `sl`.
 */
int smartlist_len (const smartlist_t *sl)
{
  ASSERT (sl);
  ASSERT_VAL (sl);
  return (sl->num_used);
}

/**
 * Return the `idx`-th element of `sl`.
 */
void *smartlist_get (const smartlist_t *sl, int idx)
{
  ASSERT (sl);
  ASSERT_VAL (sl);
  ASSERT (idx >= 0);
  ASSERT (sl->num_used > idx);
  return (sl->list[idx]);
}

/**
 * As `smartlist_get()` but return the unsigned value
 * at the `idx`-th element of `sl`.
 */
unsigned smartlist_getu (const smartlist_t *sl, int idx)
{
  ASSERT (sl);
  ASSERT_VAL (sl);
  ASSERT (idx >= 0);
  ASSERT (sl->num_used > idx);
  return (unsigned) (intptr_t) sl->list[idx];
}

/**
 * Set the `idx`-th element of `sl` to `val`.
 */
void smartlist_set (smartlist_t *sl, int idx, void *val)
{
  ASSERT (sl);
  ASSERT_VAL (sl);
  ASSERT (idx >= 0);
  ASSERT (sl->num_used > idx);
  sl->list[idx] = val;
}

/**
 * Allocate, initialise and return an empty smartlist.
 */
smartlist_t *smartlist_new (void)
{
  smartlist_t *sl = MALLOC (sizeof(*sl));

  if (!sl)
     return (NULL);

  sl->num_used = 0;
  sl->capacity = SMARTLIST_DEFAULT_CAPACITY;
  sl->list = CALLOC (sizeof(void*), sl->capacity);
  if (!sl->list)
     FREE (sl);
  return (sl);
}

/**
 * Deallocate a smartlist. Does not release storage associated with the
 * list's elements.
 */
void smartlist_free (smartlist_t *sl)
{
  if (sl)
  {
    ASSERT_VAL (sl->list);  /* detect a double smartlist_free() */
    ASSERT_VAL (sl);
    sl->num_used = 0;
    FREE (sl->list);
    FREE (sl);
  }
}

/**
 * Deallocate a smartlist and associated storage in the list's elements.
 *
 * All storage (`sl->list[]`) must have been allocated using `MALLOC()`,
 * `CALLOC()` or `STRDUP()`.
 */
void smartlist_free_all (smartlist_t *sl)
{
  if (sl)
  {
    int i, max = smartlist_len (sl);

    for (i = 0; i < max; i++)
    {
      size_t *p = (size_t*) sl->list[i];

      ASSERT_VAL (p);
      FREE (p);
    }
  }
  smartlist_free (sl);
}

/**
 * Make sure that `sl` can hold at least `num` entries.
 */
void **smartlist_ensure_capacity (smartlist_t *sl, size_t num)
{
  void **new_list = sl->list;

  ASSERT (num <= SMARTLIST_MAX_CAPACITY);

  if (num > (size_t)sl->capacity)
  {
    size_t higher = (size_t) sl->capacity;

    if (num > SMARTLIST_MAX_CAPACITY/2)
       higher = SMARTLIST_MAX_CAPACITY;
    else
    {
      while (num > higher)
        higher *= 2;
    }
    new_list = REALLOC (sl->list, sizeof(void*) * higher);
    if (!new_list)
       return (NULL);

    sl->list = new_list;
    memset (sl->list + sl->capacity, 0, sizeof(void*) * (higher - sl->capacity));
    sl->capacity = (int) higher;
  }
  return (sl->list);
}

/**
 * Append the pointer `element` (or `unsigned element`) to the end
 * of the `sl` list.
 */
#if !defined(_DEBUG)
void *smartlist_add (smartlist_t *sl, void *element)
{
  ASSERT (sl);
  if (smartlist_ensure_capacity(sl, 1 + (size_t)sl->num_used))
     sl->list [sl->num_used++] = element;
  return (element);
}

unsigned smartlist_addu (smartlist_t *sl, unsigned element)
{
  ASSERT (sl);
  if (smartlist_ensure_capacity(sl, 1 + (size_t)sl->num_used))
     sl->list [sl->num_used++] = (void*) (intptr_t) element;
  return (element);
}

/**
 * Append a malloced copy of `string` to `sl`.
 */
char *smartlist_add_strdup (struct smartlist_t *sl, const char *string)
{
  char *copy = STRDUP (string);

  smartlist_add (sl, copy);
  return (copy);
}
#endif  /* _DEBUG */

/**
 * Remove the `idx`-th element of `sl`: <br>
 *  - if `idx` is not the last element, swap the last element of `sl`
 *    into the `idx`-th space.
 */
void smartlist_del (smartlist_t *sl, int idx)
{
  ASSERT (sl);
  ASSERT_VAL (sl);
  ASSERT (idx >= 0);
  ASSERT (idx < sl->num_used);
  --sl->num_used;
  sl->list [idx] = sl->list [sl->num_used];
  sl->list [sl->num_used] = NULL;
}

/**
 * Remove the `idx`-th element of `sl`: <br>
 *  - if `idx` is not the last element, move all subsequent elements back one
 *    space. Return the old value of the `idx`-th element.
 */
void smartlist_del_keeporder (smartlist_t *sl, int idx)
{
  ASSERT (sl);
  ASSERT_VAL (sl);
  ASSERT (idx >= 0);
  ASSERT (idx < sl->num_used);
  --sl->num_used;
  if (idx < sl->num_used)
  {
    void  *src = sl->list + idx + 1;
    void  *dst = sl->list + idx;
    size_t sz = (sl->num_used - idx) * sizeof(void*);

    memmove (dst, src, sz);
  }
  sl->list [sl->num_used] = NULL;
}

/**
 * Remove all elements from the list `sl`.
 */
void smartlist_clear (smartlist_t *sl)
{
  ASSERT (sl);
  memset (sl->list, 0, sizeof(void*) * sl->num_used);
  sl->num_used = 0;
}

/**
 * Like `smartlist_clear()`, but call a `free_fn` for all items first.
 */
void smartlist_wipe (smartlist_t *sl, void (*free_fn)(void *a))
{
  int i;

  ASSERT (sl);
  for (i = 0; i < sl->num_used; i++)
     (*free_fn) (sl->list[i]);
  smartlist_clear (sl);
}

/**
 * Given a sorted smartlist `sl` and the comparison function (`compare`)
 * used to sort it, return number of duplicate members.
 */
int smartlist_duplicates (smartlist_t *sl, smartlist_sort_func compare)
{
  int i, dups = 0;

  for (i = 1; i < sl->num_used; i++)
  {
    if ((*compare)((const void**)&sl->list[i-1],
                   (const void**)&sl->list[i]) == 0)
      dups++;
  }
  return (dups);
}

/**
 * Given a sorted smartlist `sl` and the comparison function (`compare`)
 * used to sort it, remove all duplicate members.<br>
 * If `free_fn` is provided, calls `free_fn` on each duplicate. <br>
 * Otherwise, just removes them. <br>
 * Preserves the list order.
 */
int smartlist_make_uniq (smartlist_t *sl, smartlist_sort_func compare, smartlist_free_func free_fn)
{
  int i, dups = 0;

  for (i = 1; i < sl->num_used; i++)
  {
    if ((*compare)((const void**)&sl->list[i-1],
                   (const void**)&sl->list[i]) == 0)
    {
      if (free_fn)
        (*free_fn) (sl->list[i]);
      smartlist_del_keeporder (sl, i--);
      dups++;
    }
  }
  return (dups);
}

/**
 * Open a file and return the parsed lines as a smartlist. <br>
 * Lines starting with `#` or `;` are assumed to be comment lines
 * and ignored in the returned list.
 */
smartlist_t *smartlist_read_file (smartlist_parse_func parse, const char *file_fmt, ...)
{
  smartlist_t *sl;
  char         file [_MAX_PATH];
  FILE        *f;
  va_list      args;

  va_start (args, file_fmt);
  vsnprintf (file, sizeof(file), file_fmt, args);
  va_end (args);

  f = fopen (file, "r");
  if (!f)
     return (NULL);

  sl = smartlist_new();

  while (sl)
  {
    char buf[5000], *p;

    if (!fgets(buf, sizeof(buf)-1, f))   /* EOF */
       break;

    p = str_ltrim (buf);
    if (*p != '#' && *p != ';')
       (*parse) (sl, buf);
  }
  fclose (f);
  return (sl);
}

/**
 * Dump a smartlist of text-lines to a file. <br>
 * Lines are assumed to not contain newlines at the end.
 * Not used.
 */
int smartlist_write_file (smartlist_t *sl, const char *file_fmt, ...)
{
  FILE   *f;
  int     i, max;
  char    file [_MAX_PATH];
  va_list args;

  ASSERT (sl);
  ASSERT_VAL (sl);

  va_start (args, file_fmt);
  vsnprintf (file, sizeof(file), file_fmt, args);
  va_end (args);

  f = fopen (file, "w+t");
  if (!f)
     return (0);

  max = smartlist_len (sl);
  for (i = 0; i < max; i++)
  {
    fputs ((const char*)smartlist_get(sl, i), f);
    fputc ('\n', f);
  }
  fclose (f);
  return (1);
}

/**
 * \todo Open a Registry key and return the wanted records as a smartlist.
 * \param[in] reg_fmt  The top-key and subkey is specified as a var-arg string.
 *
 * E.g.
 * ```
 *  void parse_cmake_packages (smartlist_t *sl, const char *key, const char *value)
 *  {
 *    registry_array *ra = ...  // compare key/value and process as needed
 *    smartlist_add (sl, ra);
 *  }
 *
 *  smartlist_t *sl = smartlist_read_registry (parse_cmake_packages,
 *                                             "HKLM\\Software\\Kitware\\CMake\\%s",
 *                                             "Packages");
 *  registry_array *ra = sl ? smartlist_get (sl, 0) : NULL;
 *  while (ra) {
 *    // process 'sl'
 *  }
 *  smartlist_wipe (sl, free_cmake_package);
 * ```
 */
smartlist_t *smartlist_read_registry (smartlist_parse_reg_func parse, const char *reg_fmt, ...)
{
  ARGSUSED (parse);
  ARGSUSED (reg_fmt);
  return (NULL);
}

/**
 * Append each element from `sl2` to the end of `sl1`.
 */
size_t smartlist_append (smartlist_t *sl1, const smartlist_t *sl2)
{
  size_t new_size;

  ASSERT (sl1);
  ASSERT_VAL (sl1);

  ASSERT (sl2);
  ASSERT_VAL (sl2);

  if (sl2->num_used == 0) /* `sl2` is empty */
     return (0);

  new_size = (size_t)sl1->num_used + (size_t)sl2->num_used;
  ASSERT (new_size >= (size_t)sl1->num_used);    /* check for folding overflow. */

  if (!smartlist_ensure_capacity(sl1, new_size))
     return (0);

  memcpy (sl1->list + sl1->num_used, sl2->list, sl2->num_used * sizeof(void*));
  sl1->num_used = (int) new_size;
  return (size_t) sl1->num_used;
}

/**
 * Insert the value `val` as the new `idx`-th element of `sl`,
 * moving all items previously at `idx` or later forward one space.
 */
void smartlist_insert (smartlist_t *sl, int idx, void *val)
{
  ASSERT (sl);
  ASSERT (idx >= 0);
  ASSERT (idx <= sl->num_used);

  if (idx == sl->num_used)
     smartlist_add (sl, val);
  else
  {
    if (!smartlist_ensure_capacity (sl, ((size_t)sl->num_used)+1))
       return;

    /* Move other elements away
     */
    if (idx < sl->num_used)
       memmove (sl->list + idx + 1, sl->list + idx, sizeof(void*) * (sl->num_used - idx));
    sl->num_used++;
    sl->list [idx] = val;
  }
}

/**
 * Exchange the elements at indices `idx1` and `idx2` of the
 * smartlist `sl`.
 */
void smartlist_swap (smartlist_t *sl, int idx1, int idx2)
{
  if (idx1 != idx2)
  {
    void *elt = smartlist_get (sl, idx1);

    smartlist_set (sl, idx1, smartlist_get(sl, idx2));
    smartlist_set (sl, idx2, elt);
  }
}

/**
 * If `element` is the same pointer as an element of `sl`,
 * return that element's index.
 * Otherwise, return -1.
 */
int smartlist_pos (const smartlist_t *sl, const void *element)
{
  int i;

  ASSERT (sl);

  for (i = 0; i < sl->num_used; i++)
      if (sl->list[i] == element)
         return (i);
  return (-1);
}

/**
 *\typedef int (*UserCmpFunc) (const void *, const void *);
 *
 * The `__cdecl` or `__fastcall` type of user's compare function.<br>
 * Since `qsort()` needs a `__cdecl` compare function, we sort via
 * a function of this type.
 */
typedef int (*UserCmpFunc) (const void *, const void *);

/** The actual pointer to the user's compare function
 */
static UserCmpFunc user_compare;

/**
 * Sort the members of `sl` into an order defined by
 * the ordering function `compare`, which
 *
 *  - returns less than 0 if `a` precedes `b`.
 *  - greater than 0 if `b` precedes `a`.
 *  - and 0 if `a` equals `b`.
 *
 * Do it via `__cdecl local_compare()` since the caller's `compare` may
 * be `__fastcall`.
 *
 * Only important for MSVC.
 */
static int MS_CDECL local_compare (const void *a, const void *b)
{
  return (*user_compare) (a, b);
}

void smartlist_sort (smartlist_t *sl, smartlist_sort_func compare)
{
  if (sl->num_used > 0)
  {
#if defined(USE_UBSAN)
   /*
    * Avoid this issue with USE_UBSAN:
    *  runtime error: call to function compare_on_section_key1 through pointer to incorrect function type
    *  'int (*)(const void *, const void *)'
    */
    qsort (sl->list, sl->num_used, sizeof(void*), (_CoreCrtNonSecureSearchSortCompareFunction) compare);
#else
    user_compare = (UserCmpFunc) compare;
    qsort (sl->list, sl->num_used, sizeof(void*), local_compare);
#endif
    user_compare = NULL;
  }
}

/**
 * Assuming the members of `sl` are in order, return the index of the
 * member that matches `key`.
 *
 * If no member matches, return the index of the first member greater than `key`,
 * or `smartlist_len(sl)` if no member is greater than `key`. <br>
 * Set `found_out` to `true` on a match, to `false` otherwise.
 *
 * Ordering and matching are defined by a `compare` function that:
 *  \li - returns 0 on a match.
 *  \li - less than 0 if `key` is less than the member.
 *  \li - and greater than 0 if `key` is greater than the member.
 */
int smartlist_bsearch_idx (const smartlist_t *sl, const void *key,
                           smartlist_compare_func compare, bool *found_out)
{
  int hi, lo, cmp, mid, len, diff;

  ASSERT (sl);
  ASSERT (compare);
  ASSERT (found_out);

  len = smartlist_len (sl);

  /* Check for the trivial case of a zero-length list
   */
  if (len == 0)
  {
    *found_out = false;
    return (0);
  }

  /* Okay, we have a real search to do
   */
  lo = 0;
  hi = len - 1;

  /* These invariants are always true:
   *
   * For all i such that 0 <= i < lo, sl[i] < key
   * For all i such that hi < i <= len, sl[i] > key
   */

  while (lo <= hi)
  {
    diff = hi - lo;

    /* We want mid = (lo + hi) / 2, but that could lead to overflow, so
     * instead diff = hi - lo (non-negative because of loop condition), and
     * then hi = lo + diff, mid = (lo + lo + diff) / 2 = lo + (diff / 2).
     */
    mid = lo + (diff / 2);
    cmp = (*compare) (key, (const void**) &sl->list[mid]);
    if (cmp == 0)
    {
      /* sl[mid] == key; we found it
       */
      *found_out = true;
      return (mid);
    }
    if (cmp > 0)
    {
      /* key > sl[mid] and an index i such that sl[i] == key must
       * have i > mid if it exists.
       */

      /* Since lo <= mid <= hi, hi can only decrease on each iteration (by
       * being set to mid - 1) and hi is initially len - 1, mid < len should
       * always hold, and this is not symmetric with the left end of list
       * mid > 0 test below.  A key greater than the right end of the list
       * should eventually lead to lo == hi == mid == len - 1, and then
       * we set lo to len below and fall out to the same exit we hit for
       * a key in the middle of the list but not matching.  Thus, we just
       * ASSERT for consistency here rather than handle a mid == len case.
       */
      ASSERT (mid < len);

      /* Move lo to the element immediately after sl[mid]
       */
      lo = mid + 1;
    }
    else
    {
      /* This should always be true in this case
       */
      ASSERT (cmp < 0);

      /* key < sl[mid] and an index i such that sl[i] == key must
       * have i < mid if it exists.
       */

      if (mid > 0)
      {
        /* Normal case, move hi to the element immediately before sl[mid]
         */
        hi = mid - 1;
      }
      else
      {
        /* These should always be true in this case
         */
        ASSERT (mid == lo);
        ASSERT (mid == 0);

        /* We were at the beginning of the list and concluded that every
         * element e compares e > key.
         */
        *found_out = false;
        return (0);
      }
    }
  }

  /* lo > hi; we have no element matching key but we have elements falling
   * on both sides of it.  The lo index points to the first element > key.
   */
  ASSERT (lo == hi + 1);  /* All other cases should have been handled */
  ASSERT (lo >= 0);
  ASSERT (lo <= len);
  ASSERT (hi >= 0);
  ASSERT (hi <= len);

  if (lo < len)
  {
    cmp = (*compare) (key, (const void**) &sl->list[lo]);
    ASSERT (cmp < 0);
  }
  else
  {
    cmp = (*compare) (key, (const void**) &sl->list[len-1]);
    ASSERT (cmp > 0);
  }

  *found_out = false;
  return (lo);
}

/**
 * Assuming the members of `sl` are in order, return a pointer to the
 * member that matches `key`. Ordering and matching are defined by a
 * `compare` function that:
 *
 *  - returns 0 on a match.
 *  - less than 0 if `key` is less than member.
 *  - and greater than 0 if `key` is greater than member.
 */
void *smartlist_bsearch (const smartlist_t *sl, const void *key,
                         smartlist_compare_func compare)
{
  bool found;
  int  idx = smartlist_bsearch_idx (sl, key, compare, &found);

  if (!found)
     return (NULL);
  return smartlist_get(sl, idx);
}

/**
 * Takes a `char *` string separated by a character in `sep` and returns
 * a new `smartlist_t *` containing a list of `char *`.
 *
 * \param[in] str   the string to split.
 * \param[in] sep   the string-separator to use for `strtok_r()`.
 *
 * Examples:
 * \code
 *  smartlist_t *sl = smartlist_split_str ("a", ",");
 * \endcode
 *
 * will return a `sl` with 1 element: \n
 *  \li `smartlist_get (sl, 0)` -> `"a"`.
 *
 * \code
 *  smartlist_t *sl = smartlist_split_str ("a,b, c", ", ");
 * \endcode
 *
 * will return a `sl` with 3 elements: \n
 *  \li `smartlist_get (sl, 0)` -> `"a"`.
 *  \li `smartlist_get (sl, 1)` -> `"b"`.
 *  \li `smartlist_get (sl, 2)` -> `"c"`. The space in `" c"` is ignored since sep == `", "`.
 */
smartlist_t *smartlist_split_str (const char *str, const char *sep)
{
  smartlist_t *sl = smartlist_new();
  char        *s, *p, *tok_end;

  if (!sl)
     return (NULL);

#ifdef USE_strdupa
  s = strdupa (str);
#else
  s = STRDUP (str);
#endif

  if (!s)
  {
    smartlist_free (sl);
    return (NULL);
  }

  str_unquote (s);
  for (p = _strtok_r(s, sep, &tok_end); p;
       p = _strtok_r(NULL, sep, &tok_end))
     smartlist_add_strdup (sl, p);

#ifndef USE_strdupa
  FREE (s);
#endif
  return (sl);
}

/**
 * Takes a smartlist `sl` containing a list of `char *` and
 * join these into a malloced string.
 *
 * \param[in] sl   the smartlist to join strings from.
 * \param[in] sep  the optional separator appended to each string; except the last.
 *
 * Example:
 * \code
 *   smartlist_t *sl = smartlist_new();
 *   smartlist_add (sl, "hello");
 *   smartlist_add (sl, "world");
 *   res = smartlist_join_str (sl, " "); // res becomes "hello world"
 *   ...
 *   FREE (res);
 * \endcode
 *
 * `sep` can be NULL to join strings tightly.
 * Example:
 * \code
 *   smartlist_t *sl = smartlist_new();
 *   smartlist_add (sl, "hello ");
 *   smartlist_add (sl, "world!");
 *   res = smartlist_join_str (sl, MULL); // res becomes "hello world!"
 *   ...
 *   FREE (res);
 * \endcode
 */
char *smartlist_join_str (smartlist_t *sl, const char *sep)
{
  char  *ret, *p, *q;
  size_t len, sep_len = sep ? strlen (sep) : 0;
  int    i, max;

  if (!sl || smartlist_len(sl) == 0)
     return (NULL);

  max = smartlist_len (sl);
  len = 0;
  for (i = 0; i < max; i++)
      len += strlen ((const char*)smartlist_get(sl, i)) + sep_len;

  ret = p = MALLOC (len+1);
  if (!p)
     return (NULL);

  for (i = 0; i < max; i++)
  {
    q = smartlist_get (sl, i);
    len = strlen (q);
    memcpy (p, q, len);
    p += len;
    if (i < max-1 && sep)
    {
      strcpy (p, sep);
      p += sep_len;
    }
  }
  *p = '\0';
  return (ret);
}

#ifdef _DEBUG
/*
 * Some helpful versions of 'smartlist_len()' and 'smartlist_get()' for
 * '_DEBUG' (and '_CRTDBG_MAP_ALLOC'). These give a clue as to where these
 * were wrongly used.
 */
int smartlist_len_dbg (const smartlist_t *sl, const char *sl_name, const char *file, unsigned line)
{
  if (!sl)
     FATAL ("Illegal use of 'smartlist_len (%s)' from %s(%u).\n", sl_name, file, line);
  ASSERT_VAL (sl);
  return (sl->num_used);
}

void *smartlist_get_dbg (const smartlist_t *sl, int idx, const char *sl_name, const char *file, unsigned line)
{
  if (!sl)
     FATAL ("Illegal use of 'smartlist_get (%s, %d)' from %s(%u).\n", sl_name, idx, file, line);
  ASSERT_VAL (sl);
  return (sl->list[idx]);
}

unsigned smartlist_getu_dbg (const smartlist_t *sl, int idx, const char *sl_name, const char *file, unsigned line)
{
  if (!sl)
     FATAL ("Illegal use of 'smartlist_getu (%s, %d)' from %s(%u).\n", sl_name, idx, file, line);
  ASSERT_VAL (sl);
  return (unsigned) (intptr_t) sl->list[idx];
}

void *smartlist_add_dbg (smartlist_t *sl, void *element, const char *sl_name, const char *file, unsigned line)
{
  if (!sl)
     FATAL ("Illegal use of 'smartlist_add (%s, 0x%p)' from %s(%u).\n", sl_name, element, file, line);
  ASSERT_VAL (sl);
  if (smartlist_ensure_capacity (sl, 1 + (size_t)sl->num_used))
     sl->list [sl->num_used++] = element;
  return (element);
}

unsigned smartlist_addu_dbg (smartlist_t *sl, unsigned element, const char *sl_name, const char *file, unsigned line)
{
  if (!sl)
     FATAL ("Illegal use of 'smartlist_addu (%s, %u)' from %s(%u).\n", sl_name, element, file, line);
  ASSERT_VAL (sl);
  if (smartlist_ensure_capacity (sl, 1 + (size_t)sl->num_used))
     sl->list [sl->num_used++] = (void*) (intptr_t) element;
  return (element);
}

char *smartlist_add_strdup_dbg (smartlist_t *sl, const char *string, const char *sl_name, const char *file, unsigned line)
{
  char *copy;

  if (!sl)
     FATAL ("Illegal use of 'smartlist_add_strdup (%s, \"%.10s\")' from %s(%u).\n", sl_name, string, file, line);
  ASSERT_VAL (sl);

  copy = STRDUP (string);
  if (!copy)
     FATAL ("`strdup()` failed in 'smartlist_add_strdup (%s, \"%.10s\")' from %s(%u).\n", sl_name, string, file, line);
  smartlist_add (sl, copy);
  return (copy);
}
#endif /* _DEBUG */


