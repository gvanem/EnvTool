/*
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
#include "dirlist.h"
#include "getopt_long.h"

#define USE_MAKE_ARG_SPEC

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
  de->d_namlen = len;

  strcpy (p, dir);
  p = strchr (p, '\0');
  if (!IS_SLASH(p[-1]))
     *p++ = '\\';
  strcpy (p, file);

  DEBUGF (3, "len: %u, de->d_name: '%s'\n", len, de->d_name);
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

#if defined(USE_MAKE_ARG_SPEC)
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
  {
//  DEBUGF (1, "(1) _arg: '%s', p: '%s'\n", _arg, p);
    _arg = p + 1;
//  DEBUGF (1, "(2) _arg: '%s', p: '%s'\n", _arg, p);
  }

//DEBUGF (2, "_arg: '%s', arg: '%s', _arg-arg: %d\n", _arg, arg, _arg - arg);

  if (_arg - arg > 0)
     _strlcpy (dir, arg, _arg - arg + 1);

  if (end - _arg > 0)
     _strlcpy (spec, _arg, end - _arg + 1);

quit:
  DEBUGF (2, "dir: '%s', spec: '%s'\n", dir, spec);
  return (1);
}
#endif

static struct od2x_options default_opts = { "*", OD2X_UNSORTED };

DIR2 *opendir2x (const char *dir_name, struct od2x_options *opts)
{
  struct dirent2 *de;
  WIN32_FIND_DATA ff;
  DIR2           *dirp;
  HANDLE          hnd;
  size_t          max_cnt  = 50;
  size_t          max_size = max_cnt * sizeof(struct dirent2);
  char            path [_MAX_PATH];
  char           *str;

#if !defined(USE_MAKE_ARG_SPEC)
  struct stat     st;
  int             len, unc, exist;
  char           *op, c;
  const char     *ip;
#endif

  if (!opts)
     opts = &default_opts;

#if !defined(USE_MAKE_ARG_SPEC)
  for (ip = dir_name, op = path;; op++, ip++)
  {
    *op = *ip;
    if (*ip == '\0')
       break;
  }

  len = ip - dir_name;
  if (len > 0)
  {
    unc = (IS_SLASH(path[0]) && IS_SLASH(path[1]));
    c = path [len-1];
    if (unc)
    {
      if (!IS_SLASH(c))
      {
        path[len++] = '\\';
        path[len] = '\0';
      }
    }
    else
    {
      if (IS_SLASH(c) && len > 1)
      {
        len--;
        path[len] = '\0';

        if (path[len-1] == ':')
        {
          path[len++] = '\\';
          path[len++] = '.';
          path[len] = '\0';
        }
      }
      else if (c == ':')
      {
        path[len++] = '.';
        path[len] = '\0';
      }
    }
  }
  else
  {
    unc = 0;
    path[0] = '.';
    path[1] = '\0';
    len = 1;
  }

  exist = (stat(path, &st) >= 0);

  if (!exist || (st.st_mode & S_IFMT) != S_IFDIR)
  {
    errno = !exist ? ENOENT : ENOTDIR;
    return (NULL);
  }

#else
  _strlcpy (path, dir_name, sizeof(path));
#endif

  dirp = CALLOC (1, sizeof(*dirp));
  if (!dirp)
  {
    errno = ENOMEM;
    return (NULL);
  }

  /* This array get REALLOC()'ed as needed below.
   */
  dirp->dd_contents = CALLOC (1, max_size);
  if (!dirp->dd_contents)
  {
    FREE (dirp);
    errno = ENOMEM;
    return (NULL);
  }

  DEBUGF (2, "CALLOC (%u) -> %p\n", max_size, dirp->dd_contents);

#if !defined(USE_MAKE_ARG_SPEC)
  c = path[len-1];
  if (c == '.')
  {
    if (len == 1)
       len--;
    else
    {
      c = path[len-2];
      if (c == '\\' || c == ':')
         len--;
      else
      {
        path[len] = '\\';
        len++;
      }
    }
  }
  else if (!unc && (len != 1 || !IS_SLASH(c)))
  {
    path[len] = '\\';
    len++;
  }

  strcpy (path + len, opts->pattern);

#else
  snprintf (path, sizeof(path), "%s\\%s", dir_name, opts->pattern);
#endif


  DEBUGF (2, "path: %s\n", path);

  de  = dirp->dd_contents;
  str = getdirent2 (&hnd, path, &ff);
  if (!str)
  {
    FREE (dirp->dd_contents);
    return (dirp);
  }

  do
  {
    de = dirp->dd_contents + dirp->dd_num;
    if (!setdirent2(de, dir_name, str))
    {
      free_contents (dirp);
      FREE (dirp->dd_contents);
      FREE (dirp);
      errno = ENOMEM;
      return (NULL);
    }

    DEBUGF (3, "adding to de: %p, dirp->dd_num: %u\n", de, dirp->dd_num);

    de->d_attrib      = ff.dwFileAttributes;
    de->d_time_create = ff.ftCreationTime;
    de->d_time_access = ff.ftLastAccessTime;
    de->d_time_write =  ff.ftLastWriteTime;
    de->d_fsize      =  ((DWORD64)ff.nFileSizeHigh << 32) + ff.nFileSizeLow;

    str = getdirent2 (&hnd, NULL, &ff);
    dirp->dd_num++;

    if (str && dirp->dd_num == max_cnt)
    {
      struct dirent2 *more;

      max_size *= 5;
      max_cnt  *= 5;

      more = REALLOC (dirp->dd_contents, max_size);
      DEBUGF (2, "Limit reached. REALLOC (%u) -> %p\n", max_size, more);

      if (!more)
      {
        FREE (dirp);
        errno = ENOMEM;
        return (NULL);
      }
      dirp->dd_contents = more;
    }
  }
  while (str);

  dirp->dd_loc = 0;
  sort_contents (dirp, opts->sort);
  return (dirp);
}

static int compare_names (const struct dirent2 *a, const struct dirent2 *b)
{
  DEBUGF (2, "a->d_name: %s, b->d_name: %s\n", a->d_name, b->d_name);
  return stricmp (a->d_name, b->d_name);
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
  return (rc);
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
  return (rc);
}

/*
 * Sort the 'dirp->dd_contents[]' array using specified sorting method.
 */
typedef int (*QsortCmpFunc) (const void *, const void *);

static void sort_contents (DIR2 *dirp, enum od2x_sorting sort)
{
  switch (sort)
  {
    case OD2X_FILES_FIRST:
         DEBUGF (1, "Using compare_files_first().\n");
         qsort (dirp->dd_contents, dirp->dd_num, sizeof(struct dirent2),
                (QsortCmpFunc)compare_files_first);
         break;
    case OD2X_DIRECTORIES_FIRST:
         DEBUGF (1, "Using compare_dirs_first().\n");
         qsort (dirp->dd_contents, dirp->dd_num, sizeof(struct dirent2),
                (QsortCmpFunc)compare_dirs_first);
         break;
    case OD2X_ON_NAME:
         DEBUGF (1, "Using compare_names().\n");
         qsort (dirp->dd_contents, dirp->dd_num, sizeof(struct dirent2),
                (QsortCmpFunc)compare_names);
         break;
    default:
         DEBUGF (1, "Not sorting.\n");
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
          dirp->dd_contents, dirp->dd_loc, dirp->dd_num);

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
  size_t i;

  for (i = 0; i < dp->dd_num; i++)
  {
    struct dirent2 *de = dp->dd_contents + i;

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

/* Alphabetic order comparison routine
 */
int alphasort2 (const void **a, const void **b)
{
  const struct dirent2 *_a = *(const struct dirent2 **) a;
  const struct dirent2 *_b = *(const struct dirent2 **) b;

  DEBUGF (2, "_a->d_name: %s, _b->d_name: %s\n", _a->d_name, _b->d_name);
  return stricmp (_a->d_name, _b->d_name);
}

static int sd_compare_files_first (const void **a, const void **b)
{
  return compare_files_first (*a, *b);
}

static int sd_compare_dirs_first (const void **a, const void **b)
{
  return compare_dirs_first (*a, *b);
}

/*
 * Arguments:
 *   dir_name:   a plain directory name; no wild-card part.
 *   namelist_p: unallocated array of pointers to dirent2 strctures.
 *   sd_select:  pointer to function to specify which files to include in namelist[].
 *   dcomp:      pointer to sorting function to qsort, e.g. alphasort2().
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
              int (*dcomp) (const void **, const void **))
{
  struct dirent2 **namelist;
  DIR2  *dirptr;
  int    tdirsize = sizeof(struct dirent2) + _MAX_PATH;
  int    num = 0;
  int    si = 0;
  size_t max_cnt  = 30;
  size_t max_size = max_cnt * sizeof(struct dirent2);

  dirptr = opendir2 (dirname);
  if (!dirptr)
  {
    DEBUGF (1, "opendir2 (\"%s\"): failed\n", dirname);
    return (-1);
  }

  namelist = CALLOC (1, max_size);
  if (!namelist)
  {
    DEBUGF (1, "CALLOC() of %u bytes failed.\n", max_size);
    errno = ENOMEM;
    return (-1);
  }

  while (1)
  {
    struct dirent2 *de = readdir2 (dirptr);

    if (!de)
       break;

    DEBUGF (2, "readdir2(): %s.\n", de->d_name);

#if 0
    if (!sd_select)
         si = _sd_select (de);
    else si = (*sd_select) (de);
#else
    /* The "." and ".." entries are already filtered out in opendir2().
     * The caller can filter out more if needed in a 'sd_select' function.
     * E.g. use fnmatch() to search for a narrow range of files.
     */
    if (sd_select)
         si = (*sd_select) (de);
    else si = 1;
#endif

    if (si)
    {
      char *p;

      namelist [num] = MALLOC (tdirsize);
      if (!namelist[num])
      {
        DEBUGF (1, "MALLOC() of %u bytes failed.\n", tdirsize);
        errno = ENOMEM;
        break;
      }

      memcpy (namelist[num], de, sizeof(struct dirent2));
      p =  (char*) namelist[num] + sizeof(struct dirent2);
      strncpy (p, de->d_name, _MAX_PATH);
      namelist[num]->d_name = p;

      num++;
      if (num == max_cnt)
      {
        max_size *= 5;
        max_cnt  *= 5;
        namelist = REALLOC (namelist, max_size);
        if (!namelist)
        {
          DEBUGF (1, "REALLOC() of %u bytes failed.\n", max_size);
          errno = ENOMEM;
          break;
        }
      }
    }
  }

  if (dcomp)
     qsort (namelist, num, sizeof(struct dirent2*), (QsortCmpFunc)dcomp);

  closedir2 (dirptr);

  *namelist_p = namelist;
  return (num);
}

#if defined(DIRLIST_TEST)

#ifdef __MINGW32__
  /*
   * Tell MinGW's CRT to turn off command line globbing by default.
   */
  int _CRT_glob = 0;

 #ifndef __MINGW64_VERSION_MAJOR
    /*
     * MinGW-64's CRT seems to NOT glob the cmd-line by default.
     * Hence this doesn't change that behaviour.
     */
    int _dowildcard = 0;
  #endif
#endif

DWORD  recursion_level = 0;
struct prog_options opt;
char  *program_name = "dirent";

void usage (void)
{
  printf ("Usage: dirlist [-dqrs] <dir\\spec*>\n"
          "       -d:  debug-level.\n"
          "       -s:  sort the listing.\n"
          "       -r:  be recursive.\n"
          "       -S:  use scandir(). Otherwise use readdir2().\n");
  exit (-1);
}

static void do_dirent2 (const char *dir, const char *spec, int sort, int recursive)
{
  struct od2x_options opts;
  DIR2  *dp;

  opts.pattern   = spec;
  opts.sort      = sort;
  opts.recursive = recursive;
  dp = opendir2x (dir, &opts);

  DEBUGF (1, "dir: '%s', spec: '%s', dp: %p\n", dir, spec, dp);
  if (recursive)
     DEBUGF (1, "do_dirent2() does not yet support recursive mode.\n");

  if (dp)
  {
    struct dirent2 *de;
    int    is_dir, i = 0;

    while ((de = readdir2(dp)) != NULL)
    {
      is_dir = (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
      printf ("%3d (%lu): %s %-60s\n", i++, recursion_level, is_dir ? "<DIR>" : "     ", de->d_name);
#if 1
      if (recursive && is_dir)
      {
        recursion_level++;
        do_dirent2 (de->d_name, spec, sort, recursive);
        recursion_level--;
      }
#endif
    }

#if 0
    rewinddir2 (dp);
    printf ("After rewinddir2(dp):\n");
    while ((de = readdir2(dp)) != NULL)
    {
      is_dir = (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
      printf ("%2d: %s %-60s\n", telldir2(dp), is_dir ? "<DIR>" : "     ", de->d_name);
    }

#endif
    closedir2 (dp);
  }
}

/*
 * scandir2() does not yet support a 'spec'.
 */
static void do_scandir2 (const char *dir, const char *spec, int sort, int recursive)
{
  struct dirent2 **namelist;
  int    i, n;

  switch (sort)
  {
    case OD2X_ON_NAME:
         n = scandir2 (dir, &namelist, NULL, alphasort2);
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
      const char *end;
      const char *file   = namelist[i]->d_name;
      int         is_dir = (namelist[i]->d_attrib & FILE_ATTRIBUTE_DIRECTORY);

      end = strchr (file, '\0');
#if 1
      if (spec && fnmatch(spec,basename(file),FNM_FLAG_NOCASE|FNM_FLAG_PATHNAME) == FNM_MATCH)
#endif
        printf ("%3d (%lu): %s %-60s\n", i, recursion_level, is_dir ? "<DIR>" : "     ", file);

      if (!memcmp("\\.",end-2,2) || !memcmp("\\..",end-3,3))
      {
        DEBUGF (0, "Not recursing into \".\" or \"..\"\n");
        continue;
      }

      if (recursive && is_dir)
      {
        recursion_level++;
        do_scandir2 (file, spec, sort, recursive);
        recursion_level--;
      }
    }

    DEBUGF (1, "(recursion_level: %lu). freeing %d items and *namelist.\n",
            recursion_level, n);

    while (n--)
       FREE (namelist[n]);
    FREE (*namelist);
  }
}

/*
 */
int main (int argc, char **argv)
{
  int ch, do_sort = 0, do_scandir = 0, be_recursive = 0, test_make_dir_spec = 0;

#if defined(USE_MAKE_ARG_SPEC)
  char  dir_buf  [_MAX_PATH];
  char  spec_buf [_MAX_PATH];
#else
  char *dir = NULL, *spec = "*";
#endif

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

  if (argc-- < 1 || *argv == NULL)
     usage();

#if defined(USE_MAKE_ARG_SPEC)
  if (test_make_dir_spec)
  {
    opt.debug = 2;
    make_dir_spec (*argv, dir_buf, spec_buf);
    return (0);
  }
#endif

  if (do_scandir)
  {
#if !defined(USE_MAKE_ARG_SPEC)
    do_scandir2 (*argv, spec, do_sort, be_recursive);
#else
    make_dir_spec (*argv, dir_buf, spec_buf);
    do_scandir2 (dir_buf, spec_buf, do_sort, be_recursive);
#endif
  }
  else
  {
#if !defined(USE_MAKE_ARG_SPEC)
    dir  = dirname (*argv);
    spec = basename (*argv);
    do_dirent2 (dir, spec, do_sort, be_recursive);
#else
    make_dir_spec (*argv, dir_buf, spec_buf);
    do_dirent2 (dir_buf, spec_buf, do_sort, be_recursive);
#endif
  }

  fflush (stdout);

#if !defined(USE_MAKE_ARG_SPEC)
  FREE (dir);
#endif

  if (opt.debug)
     mem_report();
  return (0);
}
#endif  /* DIRLIST_TEST */
