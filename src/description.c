/**\file    description.c
 * \ingroup Misc
 *
 * \brief   Reading and parsing of 4NT/TCC-style file descriptions.
 */
#include <stdio.h>
#include <stdlib.h>

#include "color.h"
#include "smartlist.h"
#include "envtool.h"
#include "description.h"

/**
 * \def MAX_DESCR
 *  The maximum length of a file/directory description line we accept.
 *  4NT/TCC only accepts 20 - 511 bytes (can be changed by
 *  the `4NT.INI` directive `DescriptionMax`).
 */
#define MAX_DESCR 1000

/**
 * \struct descr_node
 * Elements for a file/directory with a description
 */
struct descr_node {
       char file_dir [_MAX_PATH];    /**< the file or directory name */
       char file_descr [MAX_DESCR];  /**< the description of the above `file_name` (a file or a directory) */
       BOOL is_dir;                  /**< TRUE if `file_dir` is a directory */
     };

/**
 * \struct descr_dir
 * Elements for an already checked directory with or without a `DESCRIPT.ION` file.
 */
struct descr_dir {
       /**
        * The directory this `DESCRIPT.ION` file is in.
        */
       char dir [_MAX_PATH];

       /**
        * The list of file/directory descriptions in this `dir`.
        * If NULL, this `dir` has no `DESCRIPT.ION` file.
        */
       smartlist_t *descr;
     };

/**
 * All directories already checked.
 * A smartlist of `struct descr_dir`
 */
static smartlist_t *all_descr = NULL;

/**
 * A simple hit-counter for `file_descr_get()`.
 */
static DWORD cache_hits = 0;

/**
 * The default name of a description-file.
 */
static char descr_name [200] = "DESCRIPT.ION";

/**
 * The directory we're currently parsing for
 * a description-file.
 */
static const char *curr_dir;

/**
 * Initialise this module.
 */
void file_descr_init (void)
{
  const char *env, *line, *shell;

  if (all_descr)
     return;

  all_descr = smartlist_new();
  env = getenv ("COMSPEC");
  if (!env)
     return;

  shell = strlwr (basename(env));
  if (!strcmp(shell, "4nt.exe") || !strcmp(env, "tcc.exe"))
  {
    /* `%_DNAME` is an internal 4NT/TCC variable
     */
    popen_run (NULL, env, "/C echo %%_dname");
    line = popen_last_line();
    TRACE (2, "line: '%s'.\n", line);
    if (*line && strchr(line,'.'))
       _strlcpy (descr_name, line, sizeof(descr_name));
   }
}

/**
 * Report a summary for `all_descr`.
 */
static void all_descr_dump (void)
{
  const struct descr_dir  *dd;
  const struct descr_node *dn;
  int i, j, j_max, i_max = smartlist_len (all_descr);
  int save = C_setraw (1);

  C_printf ("file_descr_dump(): cache_hits: %lu\n"
            "  i  j  is_dir Directory / 'File':     Description\n"
            "  ------------------------------------------------------------------------------------\n",
            cache_hits);

  for (i = 0; i < i_max; i++)
  {
    dd = smartlist_get (all_descr, i);
    j_max = dd->descr ? smartlist_len (dd->descr) : 0;

    C_printf (" %2d           '%s':\n", i, dd->dir);
    for (j = 0; j < j_max; j++)
    {
      dn = smartlist_get (dd->descr, j);
      C_printf ("    %2d  %d     '%-20.20s':  %.80s\n",
                j, dn->is_dir, dn->file_dir, dn->file_descr);
    }
    C_putc ('\n');
  }
  C_setraw (save);
}

/**
 * 'smartlist_wipe()' helper.
 * Free an item in the 'all_descr' smartlist.
 */
static void all_descr_free (void *_d)
{
  struct descr_dir *dd = (struct descr_dir*) _d;

  smartlist_free_all (dd->descr);
  FREE (dd);
}

/**
 * Free memory allocated by this module.
 */
void file_descr_exit (void)
{
  if (!all_descr)
     return;

  if (opt.debug >= 2)
     all_descr_dump();

  smartlist_wipe (all_descr, all_descr_free);
  smartlist_free (all_descr);
  all_descr = NULL;
}

/**
 * Parser for a single `DESCRIPT.ION` file for a specific directory.
 * Add `struct descr_node` elements to this smartlist as we parse the file.
 */
static void descr_parse (smartlist_t *sl, const char *buf)
{
  char  file [_MAX_PATH]  = "?";
  char  descr [MAX_DESCR] = "?";
  const char *p = buf;
  int   i;

  if (*p == '"')
  {
    p++;
    for (i = 0; i < sizeof(file)-2 && *p && *p != '"'; p++)
        file[i++] = *p;
    p++;
  }
  else
  {
    for (i = 0 ; i < sizeof(file)-1 && *p && !isspace((int)*p); p++)
        file[i++] = *p;
  }
  file[i] = '\0';

  while (*p && isspace((int)*p))
        p++;

  for (i = 0 ; i < sizeof(descr)-1; p++)
  {
    if (*p == '\0' || *p == '\r' || *p == '\n')
       break;
    descr[i++] = *p;
  }
  descr[i] = '\0';

  /* Do not add an entry for a `DESCRIPT.ION` file.
   */
  if (file[0] != '?' && descr[0] != '?' && stricmp(file, descr_name))
  {
    struct descr_node *dn = CALLOC (1, sizeof(*dn));
    char   fqfn_name [_MAX_PATH+2];

    _strlcpy (dn->file_dir, file, sizeof(dn->file_dir));
    _strlcpy (dn->file_descr, descr, sizeof(dn->file_descr));
    snprintf (fqfn_name, sizeof(fqfn_name), "%s\\%s", curr_dir, file);
    dn->is_dir = is_directory (fqfn_name);
    smartlist_add (sl, dn);
  }
  TRACE (2, "file: '%s', descr: '%s'.\n", file, descr);
}

/**
 * Lookup a description for a `file_dir` in `smartlist sl` (i.e. an `all_descr::descr` element).
 * This list is for a single directory.
 *
 * \retval !NULL The file/dir description found.
 * \retval NULL  The file/dir have no description.
 */
static const char *lookup_file_descr (const smartlist_t *sl, const char *file_dir)
{
  int i, max = smartlist_len (sl);

  TRACE (2, "Looking for file_dir: '%s'.\n", file_dir);

  for (i = 0; i < max; i++)
  {
    const struct descr_node *dn = smartlist_get (sl, i);

    TRACE (2, "i: %d, file_dir: %s.\n", i, dn->file_dir);
    if (!stricmp(file_dir, dn->file_dir))
       return (dn->file_descr);
  }
  return (NULL);
}

#if defined(__GNUC__) && (__GNUC__ >= 7)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

/**
 * Allocate a new smartlist `struct descr_dir` element for this directory.
 *
 * The `smartlist_read_file()` builds up another smartlist for the `struct descr_node`
 * entries in this directory.
 *
 * We add this `dir` to the `all_descr` smartlist (a cache) to avoid calling
 * `smartlist_read_file()` every time.
 *
 * \retval !NULL The file/dir description found.
 * \retval NULL  The file/dir have no description.
 */
static const char *all_descr_new (const char *dir, const char *file_dir)
{
  struct descr_dir *dd;

  curr_dir = dir;
  dd = CALLOC (1, sizeof(*dd));
  dd->descr = smartlist_read_file (descr_parse, "%s\\%s", dir, descr_name);
  _strlcpy (dd->dir, dir, sizeof(dd->dir));

  if (dd->descr)
       TRACE (2, "Parser found %d descriptions for '%s'.\n", smartlist_len(dd->descr), dir);
  else TRACE (2, "Parser found no descriptions for files in '%s\\'.\n", dir);

  smartlist_add (all_descr, dd);

  /* Is it found now?
   */
  if (dd->descr)
     return lookup_file_descr (dd->descr, file_dir);
  return (NULL);
}

/**
 * Lookup a file/directory description for a `file_dir` in the directory `dir`.
 *
 * if `dd->descr == NULL`, it means the `dir` was already
 * tried and we found no `descr_name` in it. Hence no point to look further.
 *
 * \retval !NULL The file/dir description was found.
 * \retval NULL  The file/dir have no description.
 */
static const char *all_descr_lookup (const char *dir, const char *file_dir, BOOL *empty)
{
  const struct descr_dir *dd;
  int   i, max = smartlist_len (all_descr);

  *empty = FALSE;
  TRACE (2, "all_descr_lookup(): max: %d, looking for dir: '%s'\n", max, dir);

  for (i = 0; i < max; i++)
  {
    dd = smartlist_get (all_descr, i);
    TRACE (2, "  i=%d: empty: %d, dir: '%s'\n", i, dd->descr ? 0 : 1, dd->dir);

    if (!stricmp(dir, dd->dir))
    {
      if (dd->descr)
         return lookup_file_descr (dd->descr, file_dir);
      *empty = TRUE;
    }
  }
  return (NULL);
}

/**
 * Lookup a file/dir description for a `file_dir` in the directory `dir`.
 *
 * Use already cached information or create a new node in the `all_descr` list.
 *
 * Should handle a relative path since `fix_path()` is used first:
 * \code
 *   file_descr_get ("../envtool.cfg") ->
 *    dir   -> CWD of parent-dir
 *    fname -> envtool.cfg
 * \endcode
 *
 * \retval !NULL The file/dir description was found.
 * \retval NULL  The file/dir have no description.
 */
const char *file_descr_get (const char *file_dir)
{
  const char *descr;
  char       *fname, *dir, _file[_MAX_PATH];
  BOOL        empty;

  /* `file_descr_init()` was not called.
   * Or this function was called after `file_descr_exit()`
   */
  if (!all_descr)
     return (NULL);

  _fix_path (file_dir, _file);
  dir = _file;
  fname = basename (_file);
  fname [-1] = '\0';    /* terminate 'dir' */

  if (opt.debug > 0)
     C_putc ('\n');
  TRACE (2, "file_dir: '%s', fname: '%s'\n", file_dir, fname);

  descr = all_descr_lookup (dir, fname, &empty);
  if (empty)
     return (NULL);

  if (descr)
       cache_hits++;
  else descr = all_descr_new (dir, fname);

  return (descr);
}

#if defined(DESCRIPTION_TEST) || defined(__DOXYGEN__)
struct prog_options opt;

static void init (int argc, char **argv)
{
  if (argc == 2)
  {
    if (!strcmp(argv[1], "-d"))
      opt.debug = 2;
    else if (!strcmp(argv[1], "-dd"))
      opt.debug = 3;
  }

  C_init();
  crtdbug_init();
  file_descr_init();
}

/**
 * Create a `DESCRIPT.ION` file such that a `dir /mk /z ..\envtool.cfg` in 4NT/TCC would print:
 * ```
 *   envtool.cfg      2426  28.08.19  10.38 EnvTool config-file
 * ```
 *
 * Create a long description for `..\envtool.exe` to compare our output with the output of 4NT/TCC:
 * ```
 *   envtool.exe    708096   3.10.19  12.18 EnvTool program.  Just some long lines of text to test the parser. Lorem ipsum dolor sit amet, consectetur adipiscing elit->
 * ```
 */
static void create_descr_file (void)
{
  char  fbuf [210];
  FILE *fil;

  snprintf (fbuf, sizeof(fbuf), "../%s", descr_name);
  fil = fopen (fbuf, "w+t");
  fputs ("\"envtool.exe\" EnvTool program. Just some long lines of text to test the parser. "
         "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Cras non nulla ac "
         "nibh venenatis ullamcorper. In ut dui lorem. Mauris molestie dolor quis erat "
         "interdum, vitae dignissim sapien cursus. Vestibulum pulvinar neque nec fringilla "
         "viverra. Nam feugiat condimentum nibh, sed cursus risus tempor eget. Vestibulum "
         "porttitor augue ut tellus vestibulum porta id nec erat. Proin pulvinar justo ut "
         "orci pharetra, ut rhoncus lorem tincidunt.\n"
         "envtool.cfg EnvTool config-file\n"
         "src EnvTool source directory\n", fil);
  fclose (fil);
}

int main (int argc, char **argv)
{
  static const char *files[] = {
                    "envtool.exe",      /* calls 'all_descr_new()' */
                    "envtool.exe",      /* Should be a negative cache-hit */
                    "../envtool.cfg",   /* calls 'all_descr_new()' */
                    "../envtool.exe",   /* Should be a positive cache-hit */
                    "../envtool.exe",   /* Should be a positive cache-hit */
                    "../src"            /* Test directory description. Should be a positive cache-hit */
                  };
  int i;

  init (argc, argv);
  create_descr_file();

  for (i = 0; i < DIM(files); i++)
  {
    const char *f = files[i];
    const char *descr = file_descr_get (f);

    printf ("%s -> descr: %s\n\n", f, descr);
  }

  if (cache_hits == 3)
       printf ("Cached logic seems to work.\n");
  else printf ("Cached logic failed!? cache_hits: %lu.\n", cache_hits);

  file_descr_exit();
  mem_report();
  C_exit();
  crtdbug_exit();
  return (0);
}
#endif /* DESCRIPTION_TEST || __DOXYGEN__ */

