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
 *  The maximum length of a file-description line we accept.
 *  4NT/TCC only accepts 20 - 511 bytes (can be changed by
 *  the `4NT.INI` directive `DescriptionMax`).
 */
#define MAX_DESCR 2000

/**
 * \struct descr_node
 * Elements for a file with a description
 */
struct descr_node {
       char *file_descr;    /**< the description of the below `fname` */
       char *file_name;     /**< the filename of the above description matches */
     };

/**
 * \struct descr_dir
 * Elements for a directory with a description-file.
 */
struct descr_dir {
       char        *dir;    /**< the directory this `descript.ion` file is in */
       smartlist_t *descr;  /**< the list of file-descriptions in this `dir` */
     };

/**
 * A smartlist of `struct descr_dir`
 */
static smartlist_t *all_descr = NULL;

/**
 * The default name of a description-file.
 */
static char descr_name [200] = "DESCRIPT.ION";

/**
 * 'smartlist_wipe()' helper.
 * Free an item in the 'all_descr' smartlist.
 */
static void all_descr_free (void *_d)
{
  struct descr_dir  *d = (struct descr_dir*) _d;
  struct descr_node *n;
  size_t i, max;

  if (d->descr)
  {
    max = smartlist_len (d->descr);
    for (i = 0; i < max; i++)
    {
      n = smartlist_get (d->descr, i);
      FREE (n->file_name);
      FREE (n->file_descr);
    }
    FREE (d->descr);
  }
  FREE (d->dir);
  FREE (d);
}

/**
 * Initialise this module.
 */
void file_descr_init (void)
{
  if (!all_descr)
  {
    const char *env = getenv ("COMSPEC");

    all_descr = smartlist_new();
    if (env)
    {
      const char *l, *shell = strlwr (basename(env));

      if (!strcmp(shell,"4nt.exe") || !strcmp(env,"tcc.exe"))
      {
        popen_runf (NULL, "%s /C echo %%_dname", shell);
        l = popen_last_line();
        DEBUGF (1, "l: '%s'.\n", l);
        if (*l && strchr(l,'.'))
           _strlcpy (descr_name, l, sizeof(descr_name));
       }
    }
  }
}

/**
 * Report a summary for `all_descr`.
 */
static void all_descr_dump (void)
{
  size_t i, max = smartlist_len (all_descr);

  debug_printf ("file_descr_dump():\n"
                "  Idx  #   directory\n"
                "  -----------------------------------------------------------------\n");
  for (i = 0; i < max; i++)
  {
    const struct descr_dir *d = smartlist_get (all_descr, i);

    if (d->descr)
         debug_printf ("  %2d  %3d  %s\n", i, smartlist_len(d->descr), d->dir);
    else debug_printf ("  %2d  N/A  %s\n", i, d->dir);
  }
}

/**
 * Free memory allocated by this module.
 */
void file_descr_exit (void)
{
  if (all_descr)
  {
    if (opt.debug >= 2)
       all_descr_dump();

    smartlist_wipe (all_descr, all_descr_free);
    smartlist_free (all_descr);
    all_descr = NULL;
  }
}

/**
 * Parser for a single `DESCRIPT.ION` file for a specific directory.
 * Add `struct descr_node` elements to this smartlist as we parse the file.
 */
static void descr_parse (smartlist_t *sl, const char *buf)
{
  char  file [_MAX_PATH+1]  = "?";
  char  descr [MAX_DESCR+1] = "?";
  const char *p = buf;
  int   i;

  for (i = 0; i < sizeof(file)-1 && *p && !isspace(*p); p++)
  {
    if (*p != '"')
       file[i++] = *p;
  }
  file[i] = '\0';

  while (*p && isspace(*p))
        p++;

  for (i = 0 ; i < sizeof(descr)-1 && *p; p++)
  {
    if (*p != '\r' && *p != '\n')
       descr[i++] = *p;
  }
  descr[i] = '\0';

  /* Do not add an entry for a `descript.ion` file.
   */
  if (file[0] != '?' && descr[0] != '?' && stricmp(file,descr_name))
  {
    struct descr_node *d = CALLOC (1, sizeof(*d));

    d->file_name = STRDUP (file);
    d->file_descr = STRDUP (descr);
    smartlist_add (sl, d);
  }
  DEBUGF (1, "file: '%s', descr: '%s'.\n", file, descr);
}

/**
 * Lookup a description for a `file` in `smartlist sl`.
 * This list is for a single directory.
 */
static const char *lookup_descr (smartlist_t *sl, const char *file)
{
  size_t i, max = sl ? smartlist_len (sl) : 0;

  for (i = 0; i < max; i++)
  {
    const struct descr_node *d = smartlist_get (sl, i);

    if (!stricmp(file,d->file_name))
       return (d->file_descr);
  }
  return (NULL);
}

/**
 * Allocate a new smartlist node for this directory.
 * The `smartlist_read_file()` builds up another smartlist for the entries in this directory.
 *
 * \note We add this `dir` to the `all_descr` smartlist to avoid trying to call `smartlist_read_file()`
 *       every time.
 */
static const char *all_descr_new (const char *dir, const char *file)
{
  struct descr_dir *d;
  char fname [_MAX_PATH];

  snprintf (fname, sizeof(fname), "%s\\%s", dir, descr_name);

  d = CALLOC (1, sizeof(*d));
  d->dir   = STRDUP (dir);
  d->descr = smartlist_read_file (fname, descr_parse);
  if (d->descr)
       DEBUGF (1, "Parser found %d descriptions for %s.\n", smartlist_len(d->descr), fname);
  else DEBUGF (1, "Parser found no descriptions for files in '%s\\'.\n", dir);

  smartlist_add (all_descr, d);

  /* Is it found now?
   */
  return lookup_descr (d->descr, file);
}

/**
 * Lookup a file-description for a `file` in the directory `dir`.
 *
 * \note if the `d->descr` list-length is empty, it means the `dir` was already
 *       tried and we found no `descr_name` in it. Thus we consider the file-description
 *       to be empty (`""`).
 */
static const char *all_descr_lookup (const char *dir, const char *file)
{
  struct descr_dir *d;
  size_t i, max = all_descr ? smartlist_len (all_descr) : 0;

  DEBUGF (1, "all_descr_lookup(): max=%d: '%s\\%s'\n", max, dir, file);

  for (i = 0; i < max; i++)
  {
    DEBUGF (1, "all_descr_lookup(): i=%d: '%s\\%s'\n", i, dir, file);
    d = smartlist_get (all_descr, i);
    if (!stricmp(dir,d->dir))
    {
      if (d->descr && smartlist_len(d->descr) == 0)
         return ("");
      return lookup_descr (d->descr, file);
    }
  }
  return (NULL);
}

const char *file_descr_get (const char *file)
{
  const char *descr = NULL;
  const char *fname = basename (file);
  char        dir [_MAX_PATH];

  _strlcpy (dir, file, fname-file);
  descr = all_descr_lookup (dir, fname);
  if (!descr)
     descr = all_descr_new (dir, fname);
  return (descr);
}

#if defined(DESCRIPTION_TEST)
/*
 * Match what a 4NT command does:
 * dir /mk /z f:\util\Far-FileManager\addons\SetUp\Executor.4NT.farconfig
 *
 * It gives:
 *   Executor.4N?      825  11.05.18   9.00 ExcludeCmds (TI#54) for 4nt.exe
 */
struct prog_options opt;

int main (int argc, char **argv)
{
  static const char *files[] = {
                    "f:/util/Far-FileManager/addons/SetUp/Executor.4NT.farconfig",
                    "e:/DJGPP/contrib/SEEJPG/TESTOUTT.JPG"
                  };
  const char *descr, *f;
  int   i;

  if (argc == 2 && !strcmp(argv[1],"-d"))
     opt.debug = 2;

  crtdbug_init();
  file_descr_init();

  for (i = 0; i < DIM(files); i++)
  {
    f = files[i];
    descr = file_descr_get (f);
    printf ("%s -> descr: '%s'\n", f, descr);
  }

  /* Again to test the cached information.
   */
  descr = file_descr_get (files[1]);
  printf ("%s -> descr: '%s'\n", f, descr);

  file_descr_exit();
  crtdbug_exit();
  mem_report();
  return (0);
}
#endif
