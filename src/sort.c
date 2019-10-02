/**
 * \file    sort.c
 * \ingroup Misc
 * \brief
 *   Handling of command-line options `-S` and `--sort`.
 */
#include "envtool.h"
#include "sort.h"

#define ADD_VALUE(v)  { v, #v }

static const struct search_list method_names[] = {
                    ADD_VALUE (SORT_FILE_NAME),
                    ADD_VALUE (SORT_FILE_EXTENSION),
                    ADD_VALUE (SORT_FILE_DATETIME),
                    ADD_VALUE (SORT_FILE_SIZE),
                    ADD_VALUE (SORT_PE_VERSION)
                  };

static const struct search_list short_methods[] = {
           { SORT_FILE_NAME,      "n" },
           { SORT_FILE_EXTENSION, "e"  },
           { SORT_FILE_DATETIME,  "t" },
           { SORT_FILE_SIZE,      "s" },
           { SORT_PE_VERSION,     "v" }
         };

static const struct search_list long_methods[] = {
           { SORT_FILE_NAME,      "name"    },
           { SORT_FILE_EXTENSION, "ext"     },
           { SORT_FILE_DATETIME,  "time"    },
           { SORT_FILE_SIZE,      "size"    },
           { SORT_PE_VERSION,     "version" }
         };

/**
 * Internal: ensure the local `struct search_list` are large enough
 * and of the same sized (use since a `_STATIC_ASSERT()` did not work).
 */
static void check_sizes (void)
{
  if (DIM(method_names) != DIM(short_methods))
     FATAL ("'method_names[]' and 'short_methods[]' must have the same number of elements.\n");

  if (DIM(method_names) != DIM(long_methods))
     FATAL ("'method_names[]' and 'long_methods[]' must have the same number of elements.\n");

  if (DIM(opt.sort_methods) < DIM(long_methods))
     FATAL ("'DIM(opt.sort_methods[])' must be >= %d\n.", DIM(long_methods));
}

/**
 * Internal: build a comma separated list of sort methods.
 *
 * \retval the length of `buf` that was filled.
 */
static size_t get_methods (char *buf, size_t left, const struct search_list *l, size_t num)
{
  size_t i, sz, sz_total = 0;

  for (i = 0; i < num && left > 2; i++, l++)
  {
    sz = strlen (l->name);
    _strlcpy (buf, l->name, left);
    sz_total += sz;
    buf  += sz;
    left -= sz;
    if (i < num-1)
    {
      *buf++ = ',';
      sz_total++;
    }
  }
  *buf = '\0';
  return (sz_total);
}

/**
 * Return a comma separated list of the accepted short and long sort methods.
 *
 * \retval currently `"n,e,d,t,s,v,name,ext,date,time,size,version"`.
 */
const char *get_sort_methods (void)
{
  static char methods [200];
  size_t sz;

  check_sizes();
  sz = get_methods (methods, sizeof(methods), short_methods, DIM(short_methods));
  methods [sz++] = ',';
  get_methods (methods + sz, sizeof(methods) - sz, long_methods, DIM(long_methods));
  return (methods);
}

/**
 * Called from the `getopt_long()` handlers (`set_short_option()` or `set_long_option()`
 * in envtool.c) to set `opt.sort_methods[]` based on `opts`.
 *
 * \param[in]     opts     match each `enum SortMethod`bit-value in this against
 *                         `short_methods[]` and `long_methods[]`.
 *
 * \param[in,out] err_opt  a char-pointer which can be set to the illegal sort method.
 *
 * \retval TRUE if a matching method(s) was found.
 */
BOOL set_sort_method (const char *opts, char **err_opt)
{
  static char err_buf[20];
  BOOL        rc;
  char       *opts2, *end, *tok;
  int         i, num = 0;

  *err_opt = NULL;
  check_sizes();
  ASSERT (opts);

  DEBUGF (1, "got sort opts: '%s'.\n", opts);
  opts2 = alloca (strlen(opts)+1);
  strcpy (opts2, opts);

  rc = TRUE;    /* Assume 'opts2' is okay */
  tok = _strtok_r (opts2, ", ", &end);
  while (tok && num < DIM(opt.sort_methods)-1)
  {
    unsigned m1 = list_lookup_value (tok, short_methods, DIM(short_methods));
    unsigned m2 = list_lookup_value (tok, long_methods, DIM(long_methods));

    if (m1 < UINT_MAX)
       opt.sort_methods [num++] = m1;
    else if (m2 < UINT_MAX)
       opt.sort_methods [num++] = m2;
    else
    {
      *err_opt = _strlcpy (err_buf, tok, sizeof(err_buf));
      rc = FALSE;
      break;
    }
    tok = _strtok_r (NULL, ", ", &end);
  }
  for (i = 0; opt.sort_methods[i]; i++)
      DEBUGF (1, "opt.sort_methods[%d]: '%s'.\n",
              i, list_lookup_name(opt.sort_methods[i], method_names, DIM(method_names)));
  DEBUGF (1, "opt.sort_methods[%d]: 0.\n", i);
  return (rc);
}


