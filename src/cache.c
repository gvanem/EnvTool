/**\file    cache.c
 * \ingroup Misc
 * \brief   Functions for caching information.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "envtool.h"
#include "color.h"
#include "cache.h"

// #define USE_ADD_AND_SORT

/** \def CACHE_HEADER
 * Cache-file header.
 */
#define CACHE_HEADER   "# Envtool cache written at"

/** \def CACHE_HEADER_VER
 * This should give a clue whether to trust old information.
 * If we read another version, clear all `cache.entries`.
 */
#define CACHE_HEADER_VER   "# ver. "
#define CACHE_VERSION_NUM  1

/** \def CACHE_MAX_KEY
 * The maximum length of a key.
 */
#define CACHE_MAX_KEY  100

/** \def CACHE_STATES
 * The number of states used by `cache_vgetf()`
 */
#define CACHE_STATES   8

/**
 * The fixed cache sections we handle here.
 */
static const struct search_list sections[] = {
                  { SECTION_FIRST,     "[First-sec]" },
                  { SECTION_CMAKE,     "[Cmake]"     },
                  { SECTION_COMPILER,  "[Compiler]"  },
                  { SECTION_ENV_DIR,   "[EnvDir]"    },
                  { SECTION_PKGCONFIG, "[Pkgconfig]" },
                  { SECTION_PYTHON,    "[Python]"    },
                  { SECTION_VCPKG,     "[VCPKG]"     },
                };

/**
 * \typedef cache_node
 *
 * Each cache-node have a section number and a key/value pair.
 */
typedef struct cache_node {
        CacheSections section;
        char          key [CACHE_MAX_KEY];
        char         *value;
      } cache_node;

/**
 * \typedef vgetf_state
 *
 * Each call to `cache_vgetf()` uses this state-information.
 */
typedef struct vgetf_state {
        char **vec   [12];   /**< 12 args should be enough */
        int    d_val [12];   /**< decimal values for a `%d` parameter are stored here */
        char  *s_val [12];   /**< string values for a `%s` parameter are stored here */
        char  *value;        /**< A string-value returned from `cache_vgetf()` */
      } vgetf_state;

/**
 * \typedef cache
 *
 * Collect all module globals in this structure.
 */
typedef struct CACHE {
        char        *filename;               /**< File-name to write `cache.entries` to in `cache_write()`. */
        char        *filename_prev;          /**< Copy current `cache.filename` to this before writing out the cache. */
        smartlist_t *entries;                /**< Actual cache content; smartlist of `struct cache_node`. */
        DWORD        hits, misses;           /**< Simple cache statistics. */
        DWORD        bsearches;
        DWORD        bsearches_per_key;
        DWORD        appended;               /**< Number of calls to `cache_append()`. */
        DWORD        inserted;               /**< Number of calls to `cache_insert()`. */
        DWORD        deleted;                /**< Number of calls to `cache_del()`. */
        DWORD        changed;                /**< Number of calls to `cache_putf()` with another value. */
        vgetf_state  state [CACHE_STATES];   /**< State values for `cache_vgetf()`. */
      } CACHE;

static CACHE cache;

static void cache_sort (void);
static void cache_free_node (void *_c);
static void cache_append (CacheSections section, const char *key, const char *value);
static void cache_parse (FILE *f);
static void cache_write (void);
static void cache_report (int num);

/**
 * Initialise everything:
 *  \li Setup the needed stuctures.
 *  \li Open and parse the `envtool.cache` file.
 *  \li Add each node to the `cache.entries` smartlist.
 */
void cache_init (void)
{
  FILE *f;
  int   n;

  if (cache.entries)    /* Already done this */
     return;

  if (!cache.filename)  /* cache_config() not called or key/value was missing */
     return;

  n = CACHE_STATES;
  if (n & (n-1))
     FATAL ("'CACHE_STATES' must be a power of 2.\n");

  cache.entries = smartlist_new();

  f = fopen (cache.filename, "rt");
  if (!f)
  {
    DEBUGF (1, "Failed to open %s; %s.\n", cache.filename, strerror(errno));
    return;
  }
  cache_parse (f);
  fclose (f);
}

/**
 * Called from outside to clean up this module:
 *   \li Report cache statistics if `opt.debug >= 1`.
 *   \li Write the cache entries to file if the cached information has changed.
 *   \li Free all memory allocated here.
 */
void cache_exit (void)
{
  int i, num = 0;

  if (cache.entries && cache.filename)
  {
    cache_sort();
    cache_write();
  }

  for (i = 0; i < DIM(cache.state); i++)
      FREE (cache.state[i].value);
  FREE (cache.filename);
  FREE (cache.filename_prev);

  if (cache.entries)
  {
    num = smartlist_len (cache.entries);
    smartlist_wipe (cache.entries, cache_free_node);
    smartlist_free (cache.entries);
    cache.entries = NULL;
  }
  cache_report (num);
}

/**
 * The `envtool.cfg` handler called from `envtool_cfg_handler()` in envtool.c.
 * Calls `cache_init()` if `"cache.enable = 1"`.
 */
void cache_config (const char *key, const char *value)
{
  if (!stricmp(key,"filename"))
     cache.filename = getenv_expand2 (value);

  else if (!stricmp(key,"filename_prev"))
     cache.filename_prev = getenv_expand2 (value);

  else if (!stricmp(key,"enable"))
     opt.use_cache = atoi (value);
}

/**
 * Parse the `cache.filename` file and add the section/key/value entries to `cache.entries`.
 * Assume the entries are already sorted on `section` and `key`
 * (since that was done in `cache_write()` on last `cache_exit()`.)
 */
static void cache_parse (FILE *f)
{
  UINT curr_section = UINT_MAX;
  UINT cache_ver = 0;
  BOOL found_hdr = FALSE;
  BOOL found_ver = FALSE;

  while (1)
  {
    char buf[10000], *p, *key = NULL, *value = NULL;

    if (!fgets(buf, sizeof(buf)-1, f))   /* EOF */
       break;

    if (!found_hdr && !strnicmp(buf, CACHE_HEADER, sizeof(CACHE_HEADER)-1))
       found_hdr = TRUE;

    if (found_hdr && !found_ver && !strnicmp(buf, CACHE_HEADER_VER, sizeof(CACHE_HEADER_VER)-1))
    {
      found_ver = TRUE;
      p = buf + sizeof(CACHE_HEADER_VER) - 1;
      cache_ver = atoi (p);
      DEBUGF (1, "Current cache version: %u, got version: %u.\n", CACHE_VERSION_NUM, cache_ver);
    }

    if (buf[0] == '#')   /* A '# comment' line */
       continue;

    if (buf[0] == '[')   /* A '[section]' line */
    {
      UINT section;

      p = strchr (buf, ']');
      if (!p)
         continue;
      p[1] = '\0';
      section = list_lookup_value (buf, sections, DIM(sections));
      if (section == SECTION_FIRST || section >= SECTION_LAST)
      {
        DEBUGF (1, "No such section: '%s'.\n", buf);
        continue;
      }
      curr_section = section;
    }
    else   /* 'key = value' line */
    {
      p = strchr (buf, '=');
      if (p > buf+2)
      {
        *p = '\0';
        key   = str_rtrim (buf);
        value = str_ltrim (p+1);

        p = strchr (value, '\n');
        if (p)
           *p = '\0';
      }
    }

    if (found_hdr && key && value)
    {
      DEBUGF (3, "key: '%s', value: '%s', is_quoted: %d.\n", key, value, str_isquoted(value));
      cache_append (curr_section, key, value);
    }
  }
}

/**
 * `_MSC_VER <= 1800` (Visual Studio 2012 or older) does not have `log2()`.
 */
#if defined(_MSC_VER) && (_MSC_VER <= 1800)
  #define __M_LN2  0.69314718055994530942
  #define log2(x)  (log(x) / __M_LN2)
#endif

/**
 * Print out a small cache report (if `opt.debug >= 1`).
 */
static void cache_report (int num)
{
  DEBUGF (1, "cache.entries:  %4lu, cache.hits:    %4lu, cache.misses:  %4lu.\n",
          (unsigned long)num, cache.hits, cache.misses);

  DEBUGF (1, "cache.inserted: %4lu, cache.deleted: %4lu, cache.changed: %4lu.\n",
          cache.inserted, cache.deleted, cache.changed);

  if (cache.bsearches)
  {
    double average = (double)cache.bsearches_per_key / (double)cache.bsearches;
    double maximum = log2 ((double)num) / log2 (2.0);
    DEBUGF (1, "On average, there were %.2f comparisions per key. Maximum comparision should be %.2f\n",
            average, maximum);
  }
}

/**
 * `smartlist_sort()` helper function.
 * Compare on `cache_node::section`, then on `cache_node::key`.
 */
static int compare_on_section_key1 (const void **_a, const void **_b)
{
  const cache_node *a = *_a;
  const cache_node *b = *_b;

  if (a->section != b->section)
     return (int)a->section - (int)b->section;
  return strcmp (a->key, b->key);
}

/**
 * Sort the cache on `section` and `key`.
 */
static void cache_sort (void)
{
  if (cache.entries)
     smartlist_sort (cache.entries, compare_on_section_key1);
}

/**
 * Free one cache-node.
 */
static void cache_free_node (void *_c)
{
  cache_node *c = (cache_node *) _c;

  FREE (c->value);
  FREE (c);
}

/**
 * Final step is to write the `cache.entries` to `cache.filename`.
 * Do it sorted on section, then on keys alpabetically.
 */
static void cache_write (void)
{
  const cache_node *c;
  FILETIME ft_now;
  FILE    *f;
  int      last_section = -1;
  int      i, max;

  if (!cache.entries || !cache.filename)
     return;

  if (cache.inserted + cache.deleted + cache.changed == 0)
  {
    DEBUGF (1, "No change.\n");
    return;
  }

  if (cache.filename_prev && FILE_EXISTS(cache.filename))
     CopyFile (cache.filename, cache.filename_prev, FALSE);

  f = fopen (cache.filename, "w+t");
  if (!f)
  {
    DEBUGF (1, "Failed to open %s; %s.\n", cache.filename, strerror(errno));
    return;
  }

  GetSystemTimeAsFileTime (&ft_now);
  fprintf (f, "#\n%s %s.\n", CACHE_HEADER, get_time_str_FILETIME(&ft_now));

  fprintf (f, "%s%d\n#", CACHE_HEADER_VER, CACHE_VERSION_NUM);

  max = smartlist_len (cache.entries);
  for (i = 0; i < max; i++)
  {
    c = smartlist_get (cache.entries, i);
    if (c->section != last_section)
       fprintf (f, "\n%s # = %d\n", sections[c->section].name, c->section);

    fprintf (f, "%s = %s\n", c->key, c->value);
    last_section = c->section;
  }
  fclose (f);
}

/**
 * `smartlist_bsearch_idx()` helper function.
 * Compare on `cache_node::section`, then on `cache_node::key`.
 */
static int compare_on_section_key2 (const void *_key, const void **member)
{
  const cache_node *c   = *member;
  const cache_node *key = (const cache_node *) _key;

  /**< \todo make this into a ring-buffer to generate some more valuable statistics.
   * E.g. a bucket of 20 last searches, then generate a std-deviation in `cache_report()`.
   */
  cache.bsearches_per_key++;

  if (key->section != c->section)
     return (int)key->section - (int)c->section;
  return strcmp (key->key, c->key);
}

/**
 * Do a binary search for `section` and `key` and return a `cache_node*` if found.
 * If `idx_p` is set and:
 *   if cache-node was found, set the index of the entry in `*idx_p`.
 *   if cache-node was not found, set the index of the first vacant (new) entry in `*idx_p`.
 */
static cache_node *cache_bsearch (CacheSections section, const char *key, int *idx_p)
{
  cache_node c, *ret;
  int   found, idx;

  /**< \todo make this into a ring-buffer to generate some more valuable statistics
   */
  cache.bsearches++;
  c.section = section;
  _strlcpy (c.key, key, sizeof(c.key));

  idx = smartlist_bsearch_idx (cache.entries, &c, compare_on_section_key2, &found);
  ret = found ? smartlist_get (cache.entries, idx) : NULL;

  found ? cache.hits++ : cache.misses++;
  if (idx_p)
     *idx_p = idx;
  return (ret);
}

/**
 * Delete the node with the entry `section` and `key`.
 */
void cache_del (CacheSections section, const char *key)
{
  cache_node *c;
  int   idx;

  if (!cache.entries || smartlist_len(cache.entries) == 0)
     return;

  if ((int)section < 0 || section >= SECTION_LAST)
  {
    DEBUGF (1, "No such section: %d.\n", section);
    return;
  }

  c = cache_bsearch (section, key, &idx);
  if (!c)
  {
    DEBUGF (2, "entry with key: '%s' in section '%s' not found.\n", key, sections[section].name);
    return;
  }

  DEBUGF (2, "deleting entry with key: '%s' in section '%s'.\n", key, sections[section].name);
  cache_free_node (c);
  smartlist_del_keeporder (cache.entries, idx);
  cache.deleted++;
}

/**
 * A var-arg version of `cache_del()`.
 */
void cache_delf (CacheSections section, const char *fmt, ...)
{
  char    key [CACHE_MAX_KEY];
  va_list args;

  va_start (args, fmt);
  if (vsnprintf(key, sizeof(key), fmt, args) < 0)
     FATAL ("'key' too small.\n");
  cache_del (section, key);
  va_end (args);
}

/**
 * Check the `section` and `key` values and allocate a new cache-node.
 */
static cache_node *cache_new_node (CacheSections section, const char *key, const char *value)
{
  cache_node *c;

  if (section <= SECTION_FIRST || section >= SECTION_LAST)
  {
    DEBUGF (1, "No such section: %d.\n", section);
    return (NULL);
  }
  if (strlen(key) >= CACHE_MAX_KEY-1)
  {
    DEBUGF (1, "'key' too large. Max %d bytes.\n", CACHE_MAX_KEY-1);
    return (NULL);
  }
  c = MALLOC (sizeof(*c));
  c->section = section;
  c->value   = STRDUP (value);
  _strlcpy (c->key, key, sizeof(c->key));
  return (c);
}

/**
 * Append the triplet `section`, `key` and `value` to the end of `cache.entries` smartlist.
 * Called from `cache_parse()` where the file-entries are assumed to already be sorted
 * on `section` and `key`.
 */
static void cache_append (CacheSections section, const char *key, const char *value)
{
  cache_node *c = cache_new_node (section, key, value);

  if (c)
  {
    smartlist_add (cache.entries, c);
    cache.appended++;
    DEBUGF (3, "Appending key: '%s', value: '%s'.\n", c->key, c->value);
  }
}

/**
 * Insert the triplet `section`, `key` and `value` to the `cache.entries` smartlist at index `idx`.
 */
static void cache_insert (CacheSections section, const char *key, const char *value, int idx)
{
  cache_node *c = cache_new_node (section, key, value);

  if (c)
  {
    int old_idx = idx;

#if defined(USE_ADD_AND_SORT)
    smartlist_add (cache.entries, c);
    cache_sort();
    idx = smartlist_pos (cache.entries, c); /* this should be the idx of the last element */
#else
    smartlist_insert (cache.entries, idx, c);
#endif

    cache.inserted++;
    DEBUGF (2, "Inserting key: '%s', value: '%s', section: '%s' at idx: %d/%d.\n",
            c->key, c->value, sections[section].name, idx, old_idx);
  }
}

/**
 * Add or replace an entry to the `cache.entries` smartlist.
 */
void cache_put (CacheSections section, const char *key, const char *value)
{
  int         idx;
  cache_node *c = cache_bsearch (section, key, &idx);

  if (!c)
  {
    /* Not found, insert it at the `idx` which is the first member greater than `key`.
     * This would hopefully keep the `cache.entries` still sorted.
     */
    cache_insert (section, key, value, idx);
    return;
  }

  if (c->value == value ||       /* Seldom the case */
      !strcmp(value,c->value))   /* This more is common */
     return;

  /* Replace a node with a smaller or larger `value`.
   */
  cache.changed++;
  DEBUGF (2, "key: '%s', current value: '%s', new value: '%s'.\n", c->key, c->value, value);

  if (strlen(c->value) >= strlen(value))
     strcpy (c->value, value);
  else
  {
    FREE (c->value);
    c->value = STRDUP (value);
  }
}

/**
 * A var-arg version of `cache_put()`.
 */
void cache_putf (CacheSections section, const char *fmt, ...)
{
  char    key_value [CACHE_MAX_KEY+10000];
  char   *p, *key, *value;
  va_list args;

  if (!cache.entries)
     return;

  if (!strchr(fmt, '='))
     FATAL ("'fmt' must contain a '='.\n");

  va_start (args, fmt);
  if (vsnprintf(key_value, sizeof(key_value), fmt, args) < 0)
     FATAL ("'key_value' too small.\n");

  /* Split "key = value" -> "key", "value"
   */
  p = strchr (key_value, '=');
  *p    = '\0';
  key   = str_rtrim (key_value);
  value = str_ltrim (p+1);
  cache_put (section, key, value);
  va_end (args);
}

/**
 * Lookup a `key` and return the value if found.
 * Sets `*value == NULL` if `key` was not found in `section`.
 */
static int cache_get (CacheSections section, const char *key, const char **value)
{
  const cache_node *c = cache_bsearch (section, key, NULL);

  if (c)
  {
    *value = c->value;
    return (1);
  }
  *value = NULL;
  return (0);
}

/*
 * Similar to `vsscanf()` but arguments for `%s` MUST be `char **`.
 * Only `%d` and `%s`  arguments are allowed here.
 *
 * It is called with a state; `state->vec[]`, `state->s_val[]` and `state->d_val[]` arrays
 * are used in round-robin (`idx = [0 ... CACHE_STATES-1]`) to be able to call this
 * function like:
 *
 *  char *str_1, *str_2, ... *str_N;
 *
 *  if (cache_getf(SECTION_CMAKE, "key_1 = %s", &str_1) == 1 &&
 *      cache_getf(SECTION_CMAKE, "key_2 = %s", &str_2) == 1) &&
 *      ...
 *      cache_getf(SECTION_CMAKE, "key_N = %s", &str_N) == 1)
 *  {
 *    ...
 *  }
 *
 * This way the returned `*str_1` ... `*str_N` should point to separate `s_val[idx][0]` slots.
 * The least recently used slot is freed at the end of `cache_getf()`. Ref. `oldest_idx`.
 * The other slots will be freed in `cache_exit()`.
 *
 * Using these state slots should be safe only if number of such calls to `cache_getf()`
 * are less than `CACHE_STATES` (`str_N <= str_{CACHE_STATES-1}`).
 */
static int cache_vgetf (CacheSections section, const char *fmt, va_list args, vgetf_state *state)
{
  char       *fmt_copy, *tok_end, *p, *v;
  const char *value;
  int         rc, i;

  for (i = 0; i < DIM(state->vec); i++)
  {
    state->vec[i]   = va_arg (args, char **);
    state->d_val[i] = 0;
    DEBUGF (2, "vec[%d]: 0x%p\n", i, state->vec[i]);
  }

  fmt_copy = STRDUP (fmt);
  i = 0;
  for (v = _strtok_r(fmt_copy, ",", &tok_end); v;
       v = _strtok_r(NULL, ",", &tok_end), i++)
     *state->vec[i] = NULL;

  FREE (fmt_copy);
  v = fmt_copy = STRDUP (fmt);

  p  = strchr (fmt_copy, ' ');
  *p = '\0';
  DEBUGF (2, "fmt_copy: '%s'.\n", fmt_copy);
  value = NULL;
  if (cache_get(section, fmt_copy, &value) == 0)
  {
    DEBUGF (2, "No value for fmt_copy: '%s'.\n", fmt_copy);
    FREE (fmt_copy);
    return (0);
  }

  p = str_ltrim (p+2);
  fmt_copy = STRDUP (p);
  DEBUGF (2, "fmt_copy: '%s', value: '%s'.\n", fmt_copy, value);
  FREE (v);

  state->value = STRDUP (value);

  i = 0;
  for (v = _strtok_r(state->value, ",", &tok_end); v;
       v = _strtok_r(NULL, ",", &tok_end), i++)
  {
    if (*tok_end == '"')   /* handle a quoted string */
    {
      p = strchr (tok_end+1, '"');
      if (!p)
         FATAL ("%s(): value (%s) is missing the right '\"' in 'fmt: '%s'" , __FUNCTION__, value, fmt);

      state->s_val [i++] = v;
      DEBUGF (2, "i: %d, v: '%s'\n", i-1, v);
      v = tok_end;
      tok_end = p + 1;
    }
    state->s_val[i] = v;
    DEBUGF (2, "i: %d, v: '%s'\n", i, v);
  }

  i = 0;
  for (v = _strtok_r(fmt_copy, ",", &tok_end); v;
       v = _strtok_r(NULL, ",", &tok_end))
  {
    if (!strcmp(v, "%d"))
    {
      char *end_d;

      state->d_val[i] = strtoul (state->s_val[i], &end_d, 10);
      if (end_d == state->s_val[i])
         DEBUGF (2, "EINVAL; s_val[%d]: '%s'\n", i, state->s_val[i]);
      else
      {
        DEBUGF (2, "s_val[%d]: '%s' (%d).\n", i, state->s_val[i], state->d_val[i]);
        *state->vec[i] = (char*) state->d_val[i];
      }
      i++;
    }
    else if (!strcmp(v, "%s"))
    {
      DEBUGF (2, "s_val[%d]: '%s'\n", i, state->s_val[i]);
      *state->vec[i] = state->s_val[i];
      i++;
    }
  }

  rc = i;
  FREE (fmt_copy);
  return (rc);
}

/*
 * The public interface for `cache_vgetf()`.
 */
int cache_getf (CacheSections section, const char *fmt, ...)
{
  static  int idx = 0;
  int     rc, oldest_idx;
  va_list args;

  if (!cache.entries || smartlist_len(cache.entries) == 0)
     return (0);

  va_start (args, fmt);
  rc = cache_vgetf (section, fmt, args, &cache.state[idx]);
  va_end (args);

  idx++;
  idx &= (CACHE_STATES-1);
  oldest_idx = (idx+1) & (CACHE_STATES-1);

  /* Ensure the LRU distance is 1 or 'number of states - 1'.
   */
  ASSERT ((oldest_idx - idx == 1) || (idx - oldest_idx) == CACHE_STATES-1);

  DEBUGF (2, "rc: %d, new-idx: %d, oldest_idx: %d\n", rc, idx, oldest_idx);
  FREE (cache.state[oldest_idx].value);     /* Free the oldest value */
  return (rc);
}

/*
 * An alternative using `vsscanf()`. NOT FINISHED and probably not needed.
 */
int cache_getf2 (CacheSections section, const char *fmt, ...)
{
  char       *p, *fmt_copy, *fmt_values;
  const char *value;
  va_list     args;
  int         rc;

  if (!cache.entries)
     return (0);

  fmt_copy = STRDUP (fmt);
  p  = strchr (fmt_copy, ' ');
  *p = '\0';
  fmt_values = str_ltrim (p+2);
  if (cache_get(section, fmt_copy, &value) == 0)
  {
    FREE (fmt_copy);
    return (0);
  }

  va_start (args, fmt);
  rc = vsscanf (value, fmt_values, args);
  FREE (fmt_copy);
  va_end (args);
  return (rc);
}

/**
 * Dump cached nodes in all sections.
 */
void cache_dump (void)
{
  int i, max = cache.entries ? smartlist_len (cache.entries) : 0;
  int num_sections = 0, last_section = -1;
  int max_sections = DIM(sections) - 2;  /* except SECTION_FIRST and SECTION_LAST */

  for (i = 0; i < max; i++)
  {
    const cache_node *c = smartlist_get (cache.entries, i);

    if (c->section != last_section)
    {
      DEBUGF (2, "section: %s\n", sections[c->section].name);
      num_sections++;
    }

    last_section = c->section;
    DEBUGF (2, "'%-15s' -> '%-15s'.\n", c->key, c->value);
  }
  if (num_sections != max_sections)
     DEBUGF (2, "Founded cached data for only %d section(s).\n"
                "Run 'envtool -VVV' to refresh the cache.\n",
             num_sections);
}

/**
 * A simple test for all of the above.
 */
void cache_test (void)
{
  int   rc0, rc1, rc2, rc3, rc4;
  int   d_val00, d_val01, d_val02;
  int   d_val10, d_val11, d_val12;
  int   d_val20, d_val21, d_val22;
  int   d_val30, d_val31, d_val32;
  char *s_val00 = NULL, *s_val10 = NULL, *s_val20 = NULL, *s_val30 = NULL;

  cache_node *c;

  if (!cache.entries || smartlist_len(cache.entries) == 0)
  {
    DEBUGF (0, "No cache.entries.\n");
    return;
  }

  c = cache_bsearch (SECTION_VCPKG, "ports_node_0", NULL);

  DEBUGF (0, "c->value: '%s', comparisions: %lu\n", c ? c->value : "<None>", cache.bsearches_per_key);

  rc0 = cache_getf (SECTION_PYTHON, "python_path0_0 = %d,%d,%d,%s", &d_val00, &d_val01, &d_val02, &s_val00);
  rc1 = cache_getf (SECTION_PYTHON, "python_path0_1 = %d,%d,%d,%s", &d_val10, &d_val11, &d_val12, &s_val10);
  rc2 = cache_getf (SECTION_PYTHON, "python_path0_2 = %d,%d,%d,%s", &d_val20, &d_val21, &d_val22, &s_val20);
  rc3 = cache_getf (SECTION_PYTHON, "python_path0_3 = %d,%d,%d,%s", &d_val30, &d_val31, &d_val32, &s_val30);

  DEBUGF (0, "rc0: %d, d_val00: %d, d_val01: %d, d_val02: %d, s_val00: '%s'.\n\n", rc0, d_val00, d_val01, d_val02, s_val00);
  DEBUGF (0, "rc1: %d, d_val10: %d, d_val11: %d, d_val12: %d, s_val10: '%s'.\n\n", rc1, d_val10, d_val11, d_val12, s_val10);
  DEBUGF (0, "rc2: %d, d_val20: %d, d_val21: %d, d_val22: %d, s_val20: '%s'.\n\n", rc2, d_val20, d_val21, d_val22, s_val20);
  DEBUGF (0, "rc3: %d, d_val30: %d, d_val31: %d, d_val32: %d, s_val30: '%s'.\n\n", rc3, d_val30, d_val31, d_val32, s_val30);

  /*
   * Add a test for parsing stuff like this (from VCPKG):
   *  ports_node_499 = gts,0.7.6-1,https://github.com/finetjul/gts,1,"A Library intended to provide a set of useful functions to deal with 3D surfaces meshed with interconnected triangles"
   *
   * with a "quoted description, with commas".
   */
  rc4 = cache_getf (SECTION_VCPKG, "ports_node_499 = %s,%s,%s,%d,%s",
                    &s_val00, &s_val10, &s_val20, &d_val00, &s_val30);
  if (rc4 == 5)
  {
    DEBUGF (0, "rc4: %d, name: %s, ver: %s, homepage: %s, have_CONTROL: %d\n"
            "              description: ",
            rc4, s_val00, s_val10, s_val20, d_val00);
    print_long_line (str_unquote(s_val30), 27);
  }
  else
    DEBUGF (0, "'ports_node_499' not found or not 5 arguments in that key.\n");

  if (opt.debug >= 2)
     cache_dump();
}
