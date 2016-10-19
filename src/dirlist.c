/*
 *  A public domain implementation of BSD directory routines for
 *  MS-DOS.  Written by Michael Rendell ({uunet,utai}michael@garfield),
 *  August 1987
 *
 *  Enhanced and ported to OS/2 by Kai Uwe Rommel; added scandir() prototype
 *  December 1989, February 1990
 *  Change of MAXPATHLEN for HPFS, October 1990
 *
 *  Cleanup, other hackery, Summer '92, Brian Moran , brianmo@microsoft.com
 *
 *  Changes for EnvTool:
 *    Added code for a main() test; 'DDIRLIST_TEST'.
 *    Added 'make_dir_spec()' function.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "envtool.h"
#include "color.h"
#include "dirlist.h"
#include "getopt_long.h"

typedef int (*QsortCmpFunc) (const void *, const void *);

/*
 * Local functions
 */
static char *getdirent2 (HANDLE *hnd, const char *spec, WIN32_FIND_DATA *ff);
static void  sort_contents (DIR2 *dp, enum od2x_sorting sort);
static void  free_contents (DIR2 *dp);

static BOOL setdirent2 (struct dirent2 *de, const char *dir, const char *file)
{
  size_t len = strlen(file) + strlen(dir) + 2;
  char  *p   = MALLOC (len);

  if (!p)
     return (FALSE);

  de->d_name   = p;
  de->d_reclen = len;
  de->d_namlen = len - 2;

  strcpy (p, dir);
  p = strchr (p, '\0');
  if (!IS_SLASH(p[-1]))
     *p++ = '\\';
  strcpy (p, file);

  DEBUGF (3, "len: %u, de->d_name: '%s'\n", (unsigned)len, de->d_name);
  return (TRUE);
}

static int _sd_select (const struct dirent2 *de)
{
  int rc = 1;

  if (!strcmp(de->d_name,".") || !strcmp(de->d_name,".."))
     rc = 0;
  DEBUGF (2, "rc: %d, de->d_name: %s\n", rc, de->d_name);
  return (rc);
}

/*
 * Split an 'arg' into a 'dir' part and a wildcard 'spec' for use by
 * opendir() and scandir().
 * Both 'dir' and 'spec' are assumed to point to a buffer of at least
 * _MAX_PATH characters.
 *
 * If 'arg' start with a "\\UNC_name", do not use stat() as that could
 * hang the program for a long time if 'UNC_name' resolves to an IP of
 * a host that is down.
 *
 * If 'arg' is simply a valid directory-name, use that as 'dir' and
 * set 'arg == "*"'.
 */
static int make_dir_spec (const char *arg, char *dir, char *spec)
{
  struct stat st;
  const char *p, *end, *_arg;
  BOOL  unc = (strncmp(arg,"\\\\",2) == 0);

  if (!unc && stat(arg, &st) >= 0 && (st.st_mode & S_IFMT) == S_IFDIR)
  {
    strcpy (dir, arg);
    strcpy (spec, "*");
    DEBUGF (2, "stat() okay:\n");
    goto quit;
  }

  if (unc)
       DEBUGF (2, "Not using stat() on an UNC name.\n");
  else strcpy (dir, ".");   /* set default values */

  strcpy (spec, "*");
  end = strchr (arg, '\0');
  _arg = arg;

  /* Step over drive/dir part
   */
  while ((p = strpbrk(_arg, ":/\\")) != NULL)
        _arg = p + 1;

  if (_arg - arg > 0)
     _strlcpy (dir, arg, _arg - arg + 1);

  if (end - _arg > 0)
     _strlcpy (spec, _arg, end - _arg + 1);

quit:
  DEBUGF (2, "dir: '%s', spec: '%s'\n", dir, spec);
  return (1);
}

DIR2 *opendir2x (const char *dir_name, struct od2x_options *opts)
{
  struct dirent2 *de;
  WIN32_FIND_DATA ff;
  DIR2           *dirp;
  HANDLE          hnd;
  size_t          max_cnt  = 50;
  size_t          max_size = max_cnt * sizeof(struct dirent2);
  char            path [_MAX_PATH];
  char           *file;

  _strlcpy (path, dir_name, sizeof(path));

  dirp = CALLOC (1, sizeof(*dirp));
  if (!dirp)
     goto enomem;

  /* This array get REALLOC()'ed as needed below.
   */
  dirp->dd_contents = CALLOC (1, max_size);
  if (!dirp->dd_contents)
     goto enomem;

  DEBUGF (2, "CALLOC (%u) -> %p\n", (unsigned)max_size, dirp->dd_contents);

  snprintf (path, sizeof(path), "%s\\%s", dir_name, opts ? opts->pattern : "*");

  DEBUGF (2, "path: %s\n", path);

  file = getdirent2 (&hnd, path, &ff);
  if (!file)
  {
    FREE (dirp->dd_contents);
    return (dirp);
  }

  do
  {
    de = dirp->dd_contents + dirp->dd_num;
    if (!setdirent2(de, dir_name, file))
    {
      free_contents (dirp);
      FREE (dirp->dd_contents);
      goto enomem;
    }

    DEBUGF (3, "adding to de: %p, dirp->dd_num: %u\n", de, (unsigned)dirp->dd_num);

    de->d_attrib      = ff.dwFileAttributes;
    de->d_time_create = ff.ftCreationTime;
    de->d_time_access = ff.ftLastAccessTime;
    de->d_time_write =  ff.ftLastWriteTime;
    de->d_fsize      =  ((DWORD64)ff.nFileSizeHigh << 32) + ff.nFileSizeLow;

    file = getdirent2 (&hnd, NULL, &ff);
    dirp->dd_num++;

    if (file && dirp->dd_num == max_cnt)
    {
      struct dirent2 *more;

      max_size *= 5;
      max_cnt  *= 5;

      more = REALLOC (dirp->dd_contents, max_size);
      DEBUGF (2, "Limit reached. REALLOC (%u) -> %p\n", (unsigned)max_size, more);

      if (!more)
      {
        free_contents (dirp);
        FREE (dirp->dd_contents);
        goto enomem;
      }
      dirp->dd_contents = more;
    }
  }
  while (file);

  dirp->dd_loc = 0;
  sort_contents (dirp, opts ? opts->sort : OD2X_UNSORTED);
  return (dirp);

enomem:
  FREE (dirp);
  errno = ENOMEM;
  return (NULL);
}

static int sort_reverse = 0;

static int reverse_sort (int rc)
{
  if (sort_reverse)
     return (rc > 1 ? -rc : rc);
  return (rc);
}

/*
 * Alphabetic order comparison routine
 */
static int compare_alphasort (const struct dirent2 *a, const struct dirent2 *b)
{
  DEBUGF (2, "a->d_name: %s, b->d_name: %s\n", a->d_name, b->d_name);
  return reverse_sort (stricmp (a->d_name, b->d_name));
}

static int compare_files_first (const struct dirent2 *a, const struct dirent2 *b)
{
  BOOL a_dir = !!(a->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
  BOOL b_dir = !!(b->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
  int  rc;

  if (!(a_dir ^ b_dir))
       rc = stricmp (a->d_name, b->d_name);
  else rc = (int)a_dir - (int)b_dir;

  DEBUGF (2, "a->d_name: %-15.15s, b->d_name: %-15.15s, a_dir: %d, b_dir: %d, rc: %d\n",
          basename(a->d_name), basename(b->d_name), a_dir, b_dir, rc);
  return reverse_sort (rc);
}

static int compare_dirs_first (const struct dirent2 *a, const struct dirent2 *b)
{
  BOOL a_dir = !!(a->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
  BOOL b_dir = !!(b->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
  int  rc;

#if 1
  if (~~(a_dir ^ b_dir))
       rc = stricmp (a->d_name, b->d_name);
  else rc = (int)a_dir - (int)b_dir;
#else
  if (a_dir == b_dir)
       rc = stricmp (a->d_name, b->d_name);
  else rc = stricmp (a->d_name, b->d_name);
#endif

  DEBUGF (2, "a->d_name: %-15.15s, b->d_name: %-15.15s, a_dir: %d, b_dir: %d, rc: %d\n",
          basename(a->d_name), basename(b->d_name), a_dir, b_dir, rc);
  return reverse_sort (rc);
}

int sd_compare_alphasort (const void **a, const void **b)
{
  return compare_alphasort (*a, *b);
}

int sd_compare_files_first (const void **a, const void **b)
{
  return compare_files_first (*a, *b);
}

int sd_compare_dirs_first (const void **a, const void **b)
{
  return compare_dirs_first (*a, *b);
}

/*
 * Sort the 'dirp->dd_contents[]' array using specified sorting method.
 * Only called from 'opendir2x()'. 'scandir2()' call different sorting-functions.
 */
static void sort_contents (DIR2 *dirp, enum od2x_sorting sort)
{
  sort_reverse = 0;

  switch (sort)
  {
    case OD2X_FILES_FIRST:
         DEBUGF (2, "Using compare_files_first().\n");
         qsort (dirp->dd_contents, dirp->dd_num, sizeof(struct dirent2),
                (QsortCmpFunc)compare_files_first);
         break;

    case OD2X_DIRECTORIES_FIRST:
         DEBUGF (2, "Using compare_dirs_first().\n");
         qsort (dirp->dd_contents, dirp->dd_num, sizeof(struct dirent2),
                (QsortCmpFunc)compare_dirs_first);
         break;

    case OD2X_ON_NAME:
         DEBUGF (2, "Using compare_alphasort().\n");
         qsort (dirp->dd_contents, dirp->dd_num, sizeof(struct dirent2),
                (QsortCmpFunc)compare_alphasort);
         break;

    default:
         DEBUGF (2, "Not sorting.\n");
         break;
  }
}

DIR2 *opendir2 (const char *dir_name)
{
  return opendir2x (dir_name, NULL);
}

void closedir2 (DIR2 *dirp)
{
  free_contents (dirp);
  FREE (dirp);
}

struct dirent2 *readdir2 (DIR2 *dirp)
{
  struct dirent2 *de;

  DEBUGF (2, "dirp->dd_contents: %p, dirp->dd_loc: %u, dirp->dd_num: %u\n",
          dirp->dd_contents, (unsigned)dirp->dd_loc, (unsigned)dirp->dd_num);

  if (!dirp->dd_contents || dirp->dd_loc >= dirp->dd_num)
     return (NULL);

  de = dirp->dd_contents + dirp->dd_loc;
  de->d_ino = (ino_t) dirp->dd_loc;        /* fake the inode */
  dirp->dd_loc++;
  return (de);
}

void seekdir2 (DIR2 *dp, long ofs)
{
  if (ofs > (long)dp->dd_num)
     ofs = (long) dp->dd_num;

  if (ofs < 0)
     ofs = 0;

  dp->dd_loc = (unsigned) ofs;
}

long telldir2 (DIR2 *dp)
{
  return (dp->dd_loc);
}

void rewinddir2 (DIR2 *dp)
{
  seekdir2 (dp, 0L);
}

static void free_contents (DIR2 *dp)
{
  struct dirent2 *de = dp->dd_contents;
  size_t i;

  for (i = 0; i < dp->dd_num; i++, de++)
  {
    if (de)
       FREE (de->d_name);
  }
  FREE (dp->dd_contents);
}

static char *getdirent2 (HANDLE *hnd, const char *spec, WIN32_FIND_DATA *ff)
{
  BOOL  okay = FALSE;
  char *rc   = NULL;

  memset (ff, '\0', sizeof(*ff));

  if (spec)     /* get first entry */
  {
    *hnd = FindFirstFile (spec, ff);
    if (*hnd != INVALID_HANDLE_VALUE)
       okay = TRUE;
  }
  else          /* get next entry */
    okay = FindNextFile (*hnd, ff);

  if (okay)
     rc = ff->cFileName;
  else
  {
    FindClose (*hnd);
    *hnd = INVALID_HANDLE_VALUE;
  }

  if (okay)
  {
    struct dirent2 de;

    de.d_name = rc;
    if (!_sd_select(&de))
       rc = getdirent2 (hnd, NULL, ff);
  }

  DEBUGF (3, "spec: %s, hnd: %p, rc: %s\n", spec, hnd, rc);
  return (rc);
}

/*
 * Implementation of scandir2() which uses the above opendir2() +
 * readdir2() implementation.
 */

/*
 * Arguments:
 *   dir_name:   a plain directory name; no wild-card part.
 *   namelist_p: unallocated array of pointers to dirent2 strctures.
 *   sd_select:  pointer to function to specify which files to include in namelist[].
 *   dcomp:      pointer to sorting function to qsort, e.g. compare_alphasort().
 *
 * Returns 'number-1' of files added to '*namelist_p[]'.
 *   (highest index allocated in this array).
 *   I.e. if it returns 0, there are no files in 'dir_name'.
 *
 * Returns -1 on error. Inspect 'errno' for cause.
 */
int scandir2 (const char *dirname,
              struct dirent2 ***namelist_p,
              int (*sd_select) (const struct dirent2 *),
              int (*dcomp) (const void **a, const void **b))
{
  struct dirent2 **namelist;
  DIR2  *dirptr = NULL;
  int    tdirsize = sizeof(struct dirent2) + _MAX_PATH;
  int    num = 0;
  size_t max_cnt  = 30;
  size_t max_size = max_cnt * sizeof(struct dirent2);

  dirptr = opendir2 (dirname); /* This will not call qsort() */
  if (!dirptr)
  {
    DEBUGF (1, "opendir2 (\"%s\"): failed\n", dirname);
    return (-1);
  }

  namelist = CALLOC (1, max_size);
  if (!namelist)
  {
    DEBUGF (1, "CALLOC() of %u bytes failed.\n", (unsigned)max_size);
    goto enomem;
  }

  while (1)
  {
    struct dirent2 *de = readdir2 (dirptr);
    char  *p;
    int    si;

    if (!de)
       break;

    DEBUGF (2, "readdir2(): %s.\n", de->d_name);

    /*
     * The "." and ".." entries are already filtered out in 'getdirent2()'.
     * The caller can filter out more if needed in a 'sd_select' function.
     * E.g. use fnmatch() to search for a narrow range of files.
     */
    if (sd_select)
         si = (*sd_select) (de);
    else si = 1;

    if (!si)
       continue;

    namelist [num] = MALLOC (tdirsize);
    if (!namelist[num])
    {
      DEBUGF (1, "MALLOC() of %u bytes failed.\n", tdirsize);
      goto enomem;
    }

    memcpy (namelist[num], de, sizeof(struct dirent2));
    p = (char*) namelist[num] + sizeof(struct dirent2);
    strncpy (p, de->d_name, _MAX_PATH);
    namelist [num++]->d_name = p;

    if (num == max_cnt)
    {
      max_size *= 5;
      max_cnt  *= 5;
      namelist = REALLOC (namelist, max_size);
      if (!namelist)
      {
        DEBUGF (1, "REALLOC() of %u bytes failed.\n", (unsigned)max_size);
        goto enomem;
      }
    }
  }

  sort_reverse = 0;
  if (dcomp)
     qsort (namelist, num, sizeof(struct dirent2*), (QsortCmpFunc)dcomp);

  closedir2 (dirptr);

  *namelist_p = namelist;
  return (num);

enomem:
  if (dirptr)
     closedir2 (dirptr);
  errno = ENOMEM;
  return (-1);
}


#if defined(DIRLIST_TEST)

#if defined(__MINGW32__)
  /*
   * Tell MinGW's CRT to turn off command line globbing by default.
   */
  int _CRT_glob = 0;

  #if !defined(__MINGW64_VERSION_MAJOR)
    /*
     * MinGW-64's CRT seems to NOT glob the cmd-line by default.
     * Hence this doesn't change that behaviour.
     */
    int _dowildcard = 0;
  #endif
#endif

struct prog_options opt;
char  *program_name = "dirlist";

static DWORD recursion_level = 0;
static DWORD num_directories = 0;
static DWORD num_junctions = 0;
static DWORD num_files = 0;

void usage (void)
{
  printf ("Usage: dirlist [-dqrs] <dir\\spec*>\n"
          "       -d:  debug-level.\n"
          "       -s:  sort the listing.\n"
          "       -r:  be recursive.\n"
          "       -S:  use scandir(). Otherwise use readdir2().\n");
  exit (-1);
}

static void print_de (const struct dirent2 *de, int idx)
{
  int is_dir      = (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
  int is_junction = (de->d_attrib & FILE_ATTRIBUTE_REPARSE_POINT);

  if (opt.debug > 0)
     C_printf ("~1%3d ~0(%lu): ", idx, recursion_level);
  C_printf ("~4%-7s~6", is_dir ? "<DIR>" : is_junction ? "<LINK>" : "");

  C_setraw (1);
  C_puts (de->d_name);
  C_setraw (0);
  C_puts ("~0\n");

  if (!is_dir && !is_junction)
     num_files++;
  else
  {
    if (is_dir)
       num_directories++;
    if (is_junction)
       num_junctions++;
  }
}

static void final_report (void)
{
  C_printf ("# files:       %lu (%lu files and dirs)\n", num_files, num_files+num_directories);
  C_printf ("# directories: %lu\n", num_directories);
  C_printf ("# junctions:   %lu\n", num_junctions);
}

/*
 * Call 'scandir2()' with correct sorting handler.
 */
void do_scandir2 (const char *dir, const char *spec, int sort, int recursive)
{
  struct dirent2 **namelist;
  int    i, n;

  switch (sort)
  {
    case OD2X_ON_NAME:
         n = scandir2 (dir, &namelist, NULL, sd_compare_alphasort);
         break;
    case OD2X_FILES_FIRST:
         n = scandir2 (dir, &namelist, NULL, sd_compare_files_first);
         break;
    case OD2X_DIRECTORIES_FIRST:
         n = scandir2 (dir, &namelist, NULL, sd_compare_dirs_first);
         break;
    default:
         n = scandir2 (dir, &namelist, NULL, NULL);
         break;
  }

  DEBUGF (1, "scandir (\"%s\"), spec: '%s': n: %d.\n", dir, spec, n);

  if (n < 0)
     DEBUGF (0, "(recursion_level: %lu). Error in scandir (\"%s\"): %s\n",
             recursion_level, dir, strerror(errno));
  else
  {
    for (i = 0; i < n; i++)
    {
      struct dirent2 *de = namelist[i];
      int    is_dir      = (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
      int    is_junction = (de->d_attrib & FILE_ATTRIBUTE_REPARSE_POINT);

      if (fnmatch(spec,basename(de->d_name),FNM_FLAG_NOCASE|FNM_FLAG_PATHNAME) == FNM_MATCH)
         print_de (de, i);

      if (recursive && (is_dir || is_junction))
      {
        recursion_level++;
        do_scandir2 (de->d_name, spec, sort, recursive);
        recursion_level--;
      }
    }

    DEBUGF (1, "(recursion_level: %lu). freeing %d items and *namelist.\n",
            recursion_level, n);

    while (n--)
       FREE (namelist[n]);
    FREE (namelist);
  }
}

/*
 * 'recursive' may not work if 'spec != "*"'.
 * \todo: use 'opendir2()' and do the filtering here (use 'fnmatch()'?).
 */
static void do_dirent2 (const char *dir, const char *spec, int sort, int recursive)
{
  struct od2x_options opts;
  struct dirent2     *de;
  DIR2  *dp;
  int    i = 0;

  opts.pattern   = spec;
  opts.sort      = sort;
  opts.recursive = recursive;
  dp = opendir2x (dir, &opts);

  DEBUGF (1, "dir: '%s', spec: '%s', dp: %p\n", dir, spec, dp);

  if (!dp)
     return;

  while ((de = readdir2(dp)) != NULL)
  {
    int is_dir      = (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
    int is_junction = (de->d_attrib & FILE_ATTRIBUTE_REPARSE_POINT);

#if 0
    if (is_junction)
    {
      char result [_MAX_PATH] = "??";
      BOOL rc = get_reparse_point (path, result, TRUE);

      if (rc)
      {
        FREE (de->d_name);
        de->d_name = STRDUP (result);
      //total_reparse_points++;
      }
    }
#endif

    print_de (de, i++);

    if (recursive && (is_dir || is_junction))
    {
      recursion_level++;
      do_dirent2 (de->d_name, spec, sort, recursive);
      recursion_level--;
    }
  }

#if 0
  rewinddir2 (dp);
  printf ("After rewinddir2(dp):\n");
  while ((de = readdir2(dp)) != NULL)
     print_de (de, telldir2(dp));
#endif

  closedir2 (dp);
}

/*
 */
int main (int argc, char **argv)
{
  int  ch, do_sort = 0, do_scandir = 0, be_recursive = 0, test_make_dir_spec = 0;
  char dir_buf  [_MAX_PATH];
  char spec_buf [_MAX_PATH];

  while ((ch = getopt(argc, argv, "mdrsSh?")) != EOF)
     switch (ch)
     {
       case 'd':
            opt.debug++;
            break;
       case 'r':
            be_recursive = 1;
            break;
       case 'S':
            do_scandir = 1;
            break;
       case 's':
            do_sort++;
            break;
       case 'm':
            test_make_dir_spec++;
            break;
       case '?':
       case 'h':
       default:
            usage();
     }

  argc -= optind;
  argv += optind;
  C_use_colours = 1;

  if (argc-- < 1 || *argv == NULL)
     usage();

  if (test_make_dir_spec)
  {
    opt.debug = 2;
    make_dir_spec (*argv, dir_buf, spec_buf);
    return (0);
  }

  if (do_scandir)
  {
    make_dir_spec (*argv, dir_buf, spec_buf);
    do_scandir2 (dir_buf, spec_buf, do_sort, be_recursive);
    final_report();
  }
  else
  {
    make_dir_spec (*argv, dir_buf, spec_buf);
    do_dirent2 (dir_buf, spec_buf, do_sort, be_recursive);
    final_report();
  }

  fflush (stdout);

  if (opt.debug)
     mem_report();
  return (0);
}
#endif  /* DIRLIST_TEST */
