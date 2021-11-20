/**\file    cache.c
 * \ingroup Misc
 * \brief   Functions for caching information.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

#include "envtool.h"
#include "color.h"
#include "cache.h"

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

/** \def CACHE_MAX_VALUE
 * The maximum length of a value.
 */
#define CACHE_MAX_VALUE  10000

/** \def CACHE_MAX_ARGS
 * The number of arguments supported in `cache_vgetf()`
 */
#define CACHE_MAX_ARGS  12

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
                  { SECTION_LUA,       "[Lua]"       },
                  { SECTION_PKGCONFIG, "[Pkgconfig]" },
                  { SECTION_PYTHON,    "[Python]"    },
                  { SECTION_VCPKG,     "[VCPKG]"     },
                  { SECTION_TEST,      "[Test]"      }
                };

/**
 * \typedef cache_node
 *
 * Each cache-node have a section number and a key/value pair.
 * If the value is a comma-string, use `_strtok_r()` to parse it.
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
        char **vec   [CACHE_MAX_ARGS];   /**< Where to store `"%d"` and `"%s"` values. */
        int    d_val [CACHE_MAX_ARGS];   /**< decimal values for a `%d` parameter are stored here */
        char  *s_val [CACHE_MAX_ARGS];   /**< string values for a `%s` parameter are stored here */
        char  *value;                    /**< A string-value returned from `cache_vgetf()` */
      } vgetf_state;

/**
 * \typedef cache
 *
 * Keep all module globals in this structure.
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
        BOOL         testing;                /**< For testing with `-DUSE_ASAN`. Do not write `cache.filename` at exit. */
        vgetf_state  state [CACHE_STATES];   /**< State values for `cache_vgetf()`. */
      } CACHE;

static CACHE cache;

static void cache_sort (void);
static void cache_free_node (void *_c);
static void cache_append (CacheSections section, const char *key, const char *value);
static BOOL cache_parse (FILE *f);
static void cache_write (void);
static void cache_report (int num);

/**
 * Initialise the cache-functions:
 *
 *  \li Setup the needed stuctures.
 *  \li Open and parse the `envtool.cache` file.
 *  \li Add each node to the `cache.entries` smartlist.
 */
void cache_init (void)
{
  FILE *f;
  int   n;

  if (cache.entries ||    /* Already done this */
      !cache.filename)    /* cache_config() not called or key/value was missing */
     return;

  n = CACHE_STATES;
  if (n & (n-1))
     FATAL ("'CACHE_STATES' must be a power of 2.\n");

  if (DIM(sections) != SECTION_LAST)
     FATAL ("'DIM(sections) == %d' too small. Should be: %d.\n", DIM(sections), SECTION_LAST);

  cache.entries = smartlist_new();

  f = fopen (cache.filename, "rt");
  if (!f)
  {
    TRACE (1, "Failed to open %s; %s.\n", cache.filename, strerror(errno));
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

  if (!cache.testing && cache.entries && cache.filename)
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
    cache_report (num);
    smartlist_wipe (cache.entries, cache_free_node);
    smartlist_free (cache.entries);
    cache.entries = NULL;
  }
}

/**
 * The `envtool.cfg` handler called from `envtool_cfg_handler()` in envtool.c.
 */
void cache_config (const char *key, const char *value)
{
  if (!stricmp(key, "filename"))
     cache.filename = getenv_expand2 (value);

  else if (!stricmp(key, "filename_prev"))
     cache.filename_prev = getenv_expand2 (value);

  else if (!stricmp(key, "enable"))
     opt.use_cache = atoi (value);
}

/**
 * Parse the `cache.filename` file and add the section/key/value entries to `cache.entries`.
 * Assume the entries are already sorted on `section` and `key`
 * (since that was done in `cache_write()` on last `cache_exit()`.)
 */
static BOOL cache_parse (FILE *f)
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
      TRACE (1, "Current cache version: %u, got version: %u.\n", CACHE_VERSION_NUM, cache_ver);
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
        TRACE (1, "No such section: '%s'.\n", buf);
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
      TRACE (3, "key: '%s', value: '%s', is_quoted: %d.\n", key, value, str_isquoted(value));
      cache_append (curr_section, key, value);
    }
  }
  return (cache.appended > 0);
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
  TRACE (1, "cache.entries:  %5lu, cache.hits:    %5lu, cache.misses:  %5lu.\n",
         (unsigned long)num, cache.hits, cache.misses);

  TRACE (1, "cache.inserted: %5lu, cache.deleted: %5lu, cache.changed: %5lu.\n",
         cache.inserted, cache.deleted, cache.changed);

  if (cache.bsearches)
  {
    double average = (double)cache.bsearches_per_key / (double)cache.bsearches;
    double maximum = log2 ((double)num) / log2 (2.0);
    TRACE (1, "On average, there were %.2f comparisions per key. Maximum comparision should be %.2f\n",
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
 * Write the `cache.entries` to `cache.filename`. <br>
 * Do it sorted on section, then on keys alpabetically.
 */
static void cache_write (void)
{
  const cache_node *c;
  FILETIME ft_now;
  FILE    *f;
  int      last_section = -1;
  int      i, max;

  if (cache.inserted + cache.deleted + cache.changed == 0)
  {
    TRACE (1, "No change.\n");
    return;
  }

  /* Make a backup of current cache.filename.
   */
  if (cache.filename_prev && FILE_EXISTS(cache.filename))
     CopyFile (cache.filename, cache.filename_prev, FALSE);

  f = fopen (cache.filename, "w+t");
  if (!f)
  {
    TRACE (1, "Failed to open %s; %s.\n", cache.filename, strerror(errno));
    return;
  }

  GetSystemTimeAsFileTime (&ft_now);
  fprintf (f, "#\n%s %s.\n", CACHE_HEADER, get_time_str_FILETIME(&ft_now));

  fprintf (f, "%s%d\n#", CACHE_HEADER_VER, CACHE_VERSION_NUM);

  max = smartlist_len (cache.entries);
  for (i = 0; i < max; i++)
  {
    c = smartlist_get (cache.entries, i);
    if (c->section == SECTION_TEST)
       continue;

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
  const cache_node *key = (const cache_node *) _key;
  const cache_node *node = *member;

  /**< \todo make this into a ring-buffer to generate some more valuable statistics.
   * E.g. a bucket of 20 last searches, then generate a std-deviation in `cache_report()`.
   */
  cache.bsearches_per_key++;

  if (key->section != node->section)
     return (int)key->section - (int)node->section;
  return strcmp (key->key, node->key);
}

/**
 * Do a binary search for `section` and `key` and return a `cache_node*` if found.
 * If `idx_p` is set and:
 *   if cache-node was found, set the index of the entry in `*idx_p`.
 *   if cache-node was not found, set the index of the first vacant (new) entry in `*idx_p`.
 */
static cache_node *cache_bsearch (CacheSections section, const char *_key, int *idx_p)
{
  cache_node c, *ret;
  char *key = strdupa (_key);
  int   found, idx = 0;

  if (idx_p)
     *idx_p = idx;

  if (!cache.entries || smartlist_len(cache.entries) == 0)
  {
    TRACE (1, "No cache.entries.\n");
    return (NULL);
  }

  /**< \todo make this into a ring-buffer to generate some more valuable statistics
   */
  cache.bsearches++;
  c.section = section;
  _strlcpy (c.key, str_trim(key), sizeof(c.key));

  idx = smartlist_bsearch_idx (cache.entries, &c, compare_on_section_key2, &found);
  if (found)
  {
    ret = smartlist_get (cache.entries, idx);
    cache.hits++;
  }
  else
  {
    ret = NULL;
    cache.misses++;
  }
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

  if ((int)section < SECTION_FIRST || section >= SECTION_LAST)
  {
    TRACE (1, "No such section: %d.\n", section);
    return;
  }

  c = cache_bsearch (section, key, &idx);
  if (!c)
  {
    TRACE (2, "entry with key: '%s' in section '%s' was not found.\n", key, sections[section].name);
    return;
  }

  TRACE (2, "deleting entry with key: '%s' in section '%s'.\n", key, sections[section].name);
  cache_free_node (c);
  smartlist_del_keeporder (cache.entries, idx);
  cache.deleted++;
}

/**
 * A var-arg version of `cache_del()`.
 */
void cache_delf (CacheSections section, _Printf_format_string_ const char *fmt, ...)
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
     FATAL ("Illegal section: %d.\n", section);

  if (strlen(key) >= CACHE_MAX_KEY-1)
     FATAL ("'key' too large. Max %d bytes.\n", CACHE_MAX_KEY-1);

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
    TRACE (3, "Appending key: '%s', value: '%s'.\n", c->key, c->value);
  }
}

/**
 * Insert the triplet `section`, `key` and `value` to the `cache.entries` smartlist at index `idx`.
 */
static void cache_insert (CacheSections section, const char *key, const char *value, int idx)
{
  cache_node *c;

  if (!cache.entries)
     return;

  c = cache_new_node (section, key, value);
  if (c)
  {
    smartlist_insert (cache.entries, idx, c);
    cache.inserted++;
    TRACE (3, "Inserting key: '%s', value: '%s', section: '%s' at idx: %d.\n",
           c->key, c->value, sections[section].name, idx);
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

  if (c->value == value ||        /* Seldom the case */
      !strcmp(value, c->value))   /* This more is common */
     return;

  /* Replace a node with a smaller or larger `value`.
   */
  cache.changed++;
  TRACE (1, "key: '%s', current value: '%s', new value: '%s'.\n", c->key, c->value, value);

  if (strlen(value) <= strlen(c->value))
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
void cache_putf (CacheSections section, _Printf_format_string_ const char *fmt, ...)
{
  char    key_val [CACHE_MAX_KEY + CACHE_MAX_VALUE + 4];  /* Max length of 'key = value\0' */
  char   *p;
  int     len;
  va_list args;

  if (!cache.entries)
     return;

  if (!strstr(fmt, " = "))
     FATAL ("'fmt' must be on 'key = value' form.\n");

  va_start (args, fmt);
  len = vsnprintf (key_val, sizeof(key_val), fmt, args);
  if (len >= sizeof(key_val))
     FATAL ("'key_val' too small. Need %d bytes.\n", len+1);

  if (len < 0)
     FATAL ("Encoding error for 'key_value'.\n");

  /* Split "key = value" -> "key", "value"
   */
  p = strchr (key_val, '=');
  p[-1] = '\0';
  cache_put (section, key_val, p +2);
  va_end (args);
}

/**
 * Lookup a `key` in `section` and return the value if found.
 * Returns `NULL` if `key` was not found in `section`.
 */
const char *cache_get (CacheSections section, const char *key)
{
  const cache_node *c = cache_bsearch (section, key, NULL);

  return (c ? c->value : NULL);
}

#define STATE_NORMAL 1  /** parse an unquoted string. */
#define STATE_QUOTED 2  /** parse a "quoted" string (with >=1 ',') as one value. */
#define STATE_ESCAPE 3  /** parse a ESCaped "\" string in 'STATE_QUOTED' */

static int get_next_value (char **start, char *end)
{
  char *c = *start;
  int   state = STATE_NORMAL;
  int   len = 0;
  char  buf [CACHE_MAX_VALUE+1] = { '\0' };
  char *p = buf;

  while (*c)
  {
    if (state == STATE_NORMAL)
    {
      if (*c == ',')                    /* right ',' in this value */
      {
        len++;
        break;
      }
      else if (*c == '"' && c == end-1) /* stray left '"' at EOL */
      {
        break;
      }
      else if (*c == '"')               /* left '"' in this value */
      {
        *c++ = '\0';
        *start = c;
        state = STATE_QUOTED;
      }
      else
      {
        *p++ = *c;
        len++;
        c++;
      }
    }
    else if (state == STATE_QUOTED)
    {
      if (*c == '"' && c == end-1)  /* stray right '"' at EOL */
      {
        len += 2;
        break;
      }
      else if (*c == '"')           /* normal right '"' in this value */
      {
        len++;
        *c++ = '\0';
        state = STATE_NORMAL;
      }
      else if (*c == '\\')          /* left ESC char */
      {
        *p++ = *c;
        len++;
        c++;
        state = STATE_ESCAPE;
      }
      else
      {
        *p++ = *c;
        len++;
        c++;
      }
    }
    else if (state == STATE_ESCAPE)
    {
      if (*c == '\\')       /* right ESC char */
      {
        *p++ = *c;
        len++;
        c++;
      }
      else if (*c == '"')   /* right ESCaped '"' */
      {
        *p++ = *c;
        len++;
        c++;
        state = STATE_QUOTED;
      }
      else
      {
        *p++ = *c;
        c++;
        len++;
      }
    }
    else
      FATAL ("Illegal state: %d.\n", state);
  }

  *c = '\0';
  *p = '\0';

  TRACE (3, "len: %d, *start: '%.*s', buf: '%s'.\n", len, len, *start, buf);

#if 0
  strcpy (*start, buf);
#endif

  return (len);
}

/**
 * Similar to `vsscanf()` but arguments for `%s` MUST be `char **`.
 *
 * A `"\"%s\""` format is legal in `cache_putf()`, but here only
 * `"%d"` and `"%s"` formats are allowed.
 *
 * It is called with a state; `state->vec[]`, `state->s_val[]` and `state->d_val[]` arrays
 * are used in round-robin (`idx = [0 ... CACHE_STATES-1]`) to be able to call this
 * function like:
 *  \code
 *  char *str_1, *str_2, ... *str_N;
 *
 *  if (cache_getf(SECTION_CMAKE, "key_1 = %s", &str_1) == 1 &&
 *      cache_getf(SECTION_CMAKE, "key_2 = %s", &str_2) == 1) &&
 *      ...
 *      cache_getf(SECTION_CMAKE, "key_N = %s", &str_N) == 1)
 *  {
 *    ...
 *  }
 * \endcode
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
  char      **pp, *key, *fmt_copy, *tok_end, *values, *v, *v_end;
  const char *this_fmt, *value;
  int         rc, i, i_max;

  TRACE_NL (3);

  if (!strstr(fmt, " = "))
     FATAL ("'fmt' must be on \"key = %%d,%%s...\" form. Not: '%s'.\n", fmt);

  values = strchr (fmt, '=') + 2;
  fmt_copy = strdupa (values);
  this_fmt = _strtok_r (fmt_copy, ",", &tok_end);

  for (i = 0, pp = va_arg(args, char**);
       this_fmt && i < DIM(state->vec);
       pp = va_arg(args, char**), this_fmt = _strtok_r(NULL, ",", &tok_end), i++)
  {
    state->vec[i]   = pp; /* the address to store a "%d" or "%s" value into */
    state->d_val[i] = 0;
    state->s_val[i] = NULL;
    TRACE (4, "vec[%d]: 0x%p, this_fmt: '%s'.\n", i, state->vec[i], this_fmt);
    if (*tok_end == '\0')
    {
      state->vec [++i] = NULL;
      break;
    }
  }

  key = strdupa (fmt);
  values = strchr (key, '=');
  values[-1] = '\0';                 /* terminate 'key' */
  values++;

  value = cache_get (section, key);  /* Get the value for the key */
  if (!value)
  {
    TRACE (2, "No value for key: '%s' (end of list?).\n", key);
    return (0);
  }

  TRACE (3, "value: '%s'.\n", value);

  state->value = STRDUP (value);

  i = 0;
  i_max = DIM(state->s_val);

  v     = state->value;
  v_end = strchr (v, '\0');
  while ((rc = get_next_value(&v, v_end)) > 0 && i < i_max)
  {
    TRACE (3, "i: %d, v: '%s'.\n", i, v);
    state->s_val [i++] = v;
    v += rc;
    if (v >= v_end)
       break;
  }

  i = 0;
  fmt_copy = strdupa (values + 1);
  for (v = _strtok_r(fmt_copy, ",", &tok_end); v;
       v = _strtok_r(NULL, ",", &tok_end))
  {
    if (i >= i_max)
       FATAL ("too many fields (%d) in 'fmt: \"%s\"'.\n", i+1, fmt);

    if (!strcmp(v, "%d"))
    {
      state->d_val[i] = strtoul (state->s_val[i], &v_end, 10);

      if (v_end == state->s_val[i])
         TRACE (2, "EINVAL; s_val[%d]: '%s'.\n", i, state->s_val[i]);
      else
      {
        TRACE (3, "d_val[%d]: %d.\n", i, state->d_val[i]);
        *(int*) state->vec[i] = state->d_val[i];
      }
      i++;
    }
    else if (!strcmp(v, "%s"))
    {
      TRACE (3, "s_val[%d]: '%s'.\n", i, state->s_val[i]);
      *state->vec[i] = state->s_val[i];
      i++;
    }
    else
      FATAL ("Unsupported format '%s'.\n", v);
  }

  rc = i;
  return (rc);
}

/*
 * Increment the state-index and free it's oldest value.
 */
static int cache_next_idx_state (int rc, int idx)
{
  int new_idx, oldest_idx;

  new_idx = idx + 1;
  new_idx &= (CACHE_STATES - 1);
  oldest_idx = (new_idx + 1) & (CACHE_STATES - 1);

  /* Ensure the LRU distance is 1 or 'number of states - 1'.
   */
  assert ((oldest_idx - new_idx == 1) || (new_idx - oldest_idx) == CACHE_STATES - 1);

  TRACE (3, "rc: %d, new_idx: %d, oldest_idx: %d, oldest_val: '%.10s'...\n",
         rc, new_idx, oldest_idx, cache.state[oldest_idx].value);
  FREE (cache.state[oldest_idx].value);     /* Free the oldest value */
  return (new_idx);
}

/*
 * The public interface for `cache_vgetf()`.
 */
int cache_getf (CacheSections section, const char *fmt, ...)
{
  static  int idx = 0;
  int     rc;
  va_list args;

  if (!cache.entries || smartlist_len(cache.entries) == 0)
     return (0);

  va_start (args, fmt);
  rc = cache_vgetf (section, fmt, args, &cache.state[idx]);
  va_end (args);

  idx = cache_next_idx_state (rc, idx);
  return (rc);
}

/*
 * An alternative using `vsscanf()`. NOT FINISHED and probably not needed.
 */
int cache_getf2 (CacheSections section, const char *fmt, ...)
{
  char       *p, *key, *fmt_copy, *fmt_values;
  const char *value;
  va_list     args;
  int         rc;

  if (!cache.entries)
     return (0);

  if (!strstr(fmt, " = "))
     FATAL ("'fmt' must be on \"key = %%d,%%s...\" form. Not: '%s'.\n", fmt);

  key = fmt_copy = strdupa (fmt);
  p = strchr (fmt_copy, '=');
  p[-1] = '\0';
  fmt_values = p + 3;

  value = cache_get (section, key);
  if (!value)
     return (0);

  va_start (args, fmt);
  rc = vsscanf (value, fmt_values, args);  // need a custom 'vsscanf()' here
  va_end (args);
  return (rc);
}

/**
 * Dump cached nodes in `section == SECTION_TEST`.
 */
static void cache_test_dump (void)
{
  const cache_node *c;
  int   i, max = cache.entries ? smartlist_len (cache.entries) : 0;

  debug_printf ("%s():\n  section: %s\n", __FUNCTION__, sections[SECTION_TEST].name);

  for (i = 0; i < max; i++)
  {
    c = smartlist_get (cache.entries, i);
    if (c->section == SECTION_TEST)
       debug_printf ("  %-30s -> %s.\n", c->key, c->value);
  }
}

struct test_table {
       int         rc;          /* the expected return-value from 'cache_getf()' */
       const char *key;         /* the key for this test */
       const char *putf_fmt;    /* the format for 'cache_putf()' */
       const char *getf_fmt;    /* the format for 'cache_getf()' */

       /* The key/value put in 'cache_put()'
        */
       char put_value [500];

       /* The value we expect in 'cache_getf()' with a max length of 'key = value\0'
        */
       char getf_value [CACHE_MAX_KEY + CACHE_MAX_VALUE + 4];

       /* The optional arguments for each 'put_value'
        */
       char *args [CACHE_MAX_ARGS+1];
     };

static struct test_table tests[] = {
#if 0
    { 10, "pythons_on_path0",
          "python.exe,c:\\Python36\\python.exe,c:\\Python36\\python36.dll,1,1,0,1,32,(3.6),%%APPDATA%%\\Roaming\\Python\\Python36\\site-packages",
          "%s,%s,%s,%d,%d,%d,%d,%d,%s,%s"
    },
    {  4, "python_path0_0",
          "1,1,0,c:\\Python36\\Scripts",
          "%d,%d,%d,%s"
    },
#endif

    { 4, "test_0", "-,-,-,-",                 /* no quoted values */
         "%s,%s,%s,%s", "", "-,-,-,-"
    },
    { 4, "test_1", "\"-\",-,-,-",             /* quoted value at pos 0 */
         "%s,%s,%s,%s", "", "-,-,-,-"
    },

    { 4, "test_2", "-,\"-\",-,-",             /* quoted value at pos 1 */
         "%s,%s,%s,%s", "", "-,-,-,-"
    },

    { 4, "test_3", "-,-,-,\"-\"",             /* quoted value at last pos */
         "%s,%s,%s,%s", "", "-,-,-,-"
    },

    { 4, "test_4", "-,-,-\",-",               /* extra quote after value 2 */
         "%s,%s,%s,%s", "", "-,-,-,-"
    },

    { 4, "test_5", "-,-,-,-\"",               /* extra quote after value 3 */
         "%s,%s,%s,%s", "", "-,-,-,-"
    },

    { 2, "test_6", "-,\"abc \\\"def\\\" \"",  /* Escaped quotes in quoted string */
         "%s,%s", "", "-,-"
    },

#if 1
    { 4, "test_5",
         "-,-,-,\"a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q\"",  /* Far more ',' than CACHE_MAX_ARGS */
         "%s,%s,%s,%s"
    },

    /* Test a "quoted description, with commas".
     */
    { 6, "port_node_0",
         "gts,0,1,0.7.6,https://github.com/finetjul/gts,\"A library, intended to provide a set of useful functions to deal with 3D surfaces...\"",
         "%s,%d,%d,%s,%s,%s", "",
         "gts,0,1,0.7.6,https://github.com/finetjul/gts,\"A library, intended to provide a set of useful functions to deal with 3D surfaces...\""
    },

    /* Test for fields with a tilde character '~'.
     */
    { 6, "port_node_1",
         "libsvm,0,1,3.25,https://www.csie.ntu.edu.tw/~cjlin/libsvm/,\"A library for Support Vector Machines.\"",
         "%s,%d,%d,%s,%s,%s", "",
         "libsvm,0,1,3.25,https://www.csie.ntu.edu.tw/~cjlin/libsvm/,\"A library for Support Vector Machines.\"",
    }
#endif
  };

static void cache_test_init (void)
{
  struct test_table *t;
  int                i;

  cache.testing = TRUE;

  t = tests + 0;
  for (i = 0; i < DIM(tests); i++, t++)
  {
    memset (&t->args, '\0', sizeof(t->args));

    snprintf (t->put_value, sizeof(t->put_value), t->putf_fmt,
              t->args[0],  t->args[1], t->args[2], t->args[3], t->args[4],  t->args[5],
              t->args[6],  t->args[7], t->args[8], t->args[9], t->args[10], t->args[11],
              t->args[12]);    /* To test overflow of 'CACHE_MAX_ARGS == 12': */

    cache_put (SECTION_TEST, t->key, t->put_value);
    if (!t->getf_value[0])
       snprintf (t->getf_value, sizeof(t->getf_value), "%s = %s", t->key, t->put_value);
  }

  if (opt.debug >= 2)
  {
    t = tests + 0;
    for (i = 0; i < DIM(tests); i++, t++)
        debug_printf ("  rc: %d, getf_value: '%.50s' ...\n", t->rc, t->getf_value);
    C_putc ('\n');
  }
}

static int cache_test_getf (void)
{
  const struct test_table *t = tests + 0;
  int         i, num_ok = 0;
  char       *args [CACHE_MAX_ARGS+1];

  TRACE (2, "%s():\n", __FUNCTION__);
  for (i = 0; i < DIM(tests); i++, t++)
  {
    char   key_value  [CACHE_MAX_KEY + CACHE_MAX_VALUE + 4];
    char   getf_value [CACHE_MAX_KEY + CACHE_MAX_VALUE];
    char  *buf = getf_value;
    int    len, rc, j;
    size_t left = sizeof(getf_value);
    BOOL   equal = FALSE;

    snprintf (key_value, sizeof(key_value), "%s = %s", t->key, t->getf_fmt);

    memset (&args, '\0', sizeof(args));
    rc = cache_getf (SECTION_TEST, key_value,
                     &args[0], &args[1], &args[2],  &args[3],
                     &args[4], &args[5], &args[6],  &args[7],
                     &args[8], &args[9], &args[10], &args[11]);

    getf_value[0] = '\0';
    for (j = 0; j < rc; j++)
    {
      len = snprintf (buf, left, "%s", args[j]);  // this is wrong
      left -= len;
      buf  += len;
      if (j < rc - 1)
      {
        *buf++ = ',';
        *buf++ = '\0';
        left -= 2;
      }
    }

    if (t->getf_value[0])
       equal = (strcmp (t->getf_value, getf_value) == 0);

    if (rc == t->rc && equal)
       num_ok++;

    debug_printf ("  key_value: '%s'...\n", key_value);

    debug_printf ("  rc: %d, t->rc: %d, equal: %d, t->getf_value: '%s', getf_value: '%s'\n",
                  rc, t->rc, equal, t->getf_value, getf_value);
  }

  if (num_ok == i)
       C_printf ("  All tests ran ~2OKAY~0.\n\n");
  else C_printf ("  %d tests ~5FAILED~0.\n\n", i - num_ok);
  return (num_ok);
}

/**
 * A simple test for all of the above.
 *
 * Should only be called from 'tests.c' if 'USE_ASAN' is defined.
 * Otherwise let that be 'FATAL()'
 */
int cache_test (void)
{
#if !defined(USE_ASAN)
  FATAL ("'cache_test()' needs '-DUSE_ASAN' to be called.\n");
#endif

  C_puts ("~3cache_test():~0\n");
  cache_test_init();
  opt.debug = 3;

  cache_test_getf();   /* Now, read them back */
  cache_test_dump();   /* and dump the entries in SECTION_TEST */

#if 0
  /*
   * Test overflow of 'CACHE_MAX_ARGS == 12':
   *
   * - First create a cache-node with 13 elements (which is legal).
   * - Then read 12 elements back.
   * - Then read all 13 elements back which is illegal.
   */
  if (opt.verbose >= 1)
  {
    const char *fmt_12 = "legal_key_val = %s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s";
    const char *fmt_13 = "illegal_key_val = %s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s";
    char       *str [CACHE_MAX_ARGS+1];
    int         rc;

    cache_putf (SECTION_TEST, fmt_13, "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12");

    memset (&str, '\0', sizeof(str));
    rc = cache_getf (SECTION_TEST, fmt_12,
                     &str[0], &str[1], &str[2],  &str[3],
                     &str[4], &str[5], &str[6],  &str[7],
                     &str[8], &str[9], &str[10], &str[11]);

    TRACE (0, "rc: %d: %s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s.\n", rc,
           str[0], str[1], str[2],  str[3],
           str[4], str[5], str[6],  str[7],
           str[8], str[9], str[10], str[11]);

    memset (&str, '\0', sizeof(str));
    rc = cache_getf (SECTION_TEST, fmt_13,
                     &str[0], &str[1], &str[2],  &str[3],
                     &str[4], &str[5], &str[6],  &str[7],
                     &str[8], &str[9], &str[10], &str[11],
                     &str[12]);

    TRACE (0, "rc: %d: %s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s.\n", rc,
           str[0], str[1], str[2],  str[3],
           str[4], str[5], str[6],  str[7],
           str[8], str[9], str[10], str[11],
           str[12]);
  }
#endif

  exit (0);
}
