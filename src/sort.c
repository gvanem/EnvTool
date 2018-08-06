/**
 * Handling of sort options `-S` and `"--sort"`.
 */
#include "envtool.h"
#include "sort.h"

static const struct search_list short_sort_methods[] = {
           { SORT_FILE_NAME,      "n" },
           { SORT_FILE_EXTENSION, "e"  },
           { SORT_FILE_DATETIME,  "d" },
           { SORT_FILE_DATETIME,  "t" },
           { SORT_FILE_SIZE,      "s" },
         };

static const struct search_list long_sort_methods[] = {
           { SORT_FILE_NAME,      "name" },
           { SORT_FILE_EXTENSION, "ext"  },
           { SORT_FILE_DATETIME,  "date" },
           { SORT_FILE_DATETIME,  "time" },
           { SORT_FILE_SIZE,      "size" },
         };

static const char *get_methods (const struct search_list *l, size_t num)
{
  static char methods [200];
  char   *p = methods;
  size_t  i, sz, left = sizeof(methods);

  *p = '\0';
  for (i = 0; i < num && left > 2; i++, l++)
  {
    sz = strlen (l->name);
    _strlcpy (p, l->name, left);
    p += sz;
    *p = ',';
    if (i == num - 1)
       break;
    p++;
    left -= sz + 1;
  }
  *p = '\0';
  return (methods);
}

const char *get_sort_methods_short (void)
{
  return get_methods (short_sort_methods, DIM(short_sort_methods));
}

const char *get_sort_methods_long (void)
{
  return get_methods (long_sort_methods, DIM(long_sort_methods));
}

/**
 * Called from the `getopt_long()` handlers (`set_short_option()` or `set_long_option()`
 * in envtool.c) to set `opt.sort_method` based on `short_opt` or `long_opt`.
 *
 * \param[in] short_opt  If `!NULL`, match this against `short_sort_methods[]`.
 * \param[in] long_opt   If `!NULL`, match this against `long_sort_methods[]`.
 *
 * \retval TRUE if a matching method was found.
 */
BOOL set_sort_method (const char *short_opt, const char *long_opt)
{
  SortMethod m = SORT_FILE_UNSORTED;

  ASSERT (short_opt || long_opt);

  if (short_opt)
  {
    DEBUGF (0, "got \"-S\" option: '%s'.\n", short_opt);
    m = list_lookup_value (short_opt, short_sort_methods, DIM(short_sort_methods));
    if (m == UINT_MAX)
       return (FALSE);
  }
  else
  {
    DEBUGF (0, "got \"--sort\" option: '%s'.\n", long_opt);
    m = list_lookup_value (long_opt, long_sort_methods, DIM(long_sort_methods));
    if (m == UINT_MAX)
       return (FALSE);
  }
  opt.sort_method = m;
  return (TRUE);
}


