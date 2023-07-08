/**\file    dirlist.c
 * \ingroup Misc
 * \brief
 *   A public domain implementation of BSD directory routines
 *   getdirent(), opendir() etc.
 *
 * Written by Michael Rendell `({uunet,utai}michael@garfield)`,
 * August 1987
 *
 * Enhanced and ported to OS/2 by Kai Uwe Rommel; added scandir() prototype
 * December 1989, February 1990 <br>
 * Change of MAXPATHLEN for HPFS, October 1990 <br>
 *
 * Cleanup, other hackery, Summer '92, Brian Moran , brianmo@microsoft.com
 *
 * Changes for EnvTool:
 *  \li Added code for a main() test; `-DDIRLIST_TEST`.
 *  \li Added `make_dir_spec()` function.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "envtool.h"
#include "color.h"
#include "dirlist.h"
#include "getopt_long.h"
#include "get_file_assoc.h"

#define USE_get_actual_filename 0

/*
 * Local functions
 */
static char *getdirent2 (HANDLE *hnd, const char *spec, WIN32_FIND_DATA *ff);
static void  free_contents (DIR2 *dp);
static void  set_sort_funcs (enum od2x_sorting sort, QsortCmpFunc *qsort_func, ScandirCmpFunc *sd_cmp_func);
static void  print_tree_branch (const char *sz_buf, const char *directory);

static BOOL setdirent2 (struct dirent2 *de, const char *dir, const char *file)
{
  size_t len = strlen(file) + strlen(dir) + 2;
  char  *p   = MALLOC (len);

  if (!p)
     return (FALSE);

  de->d_name   = p;
  de->d_reclen = len;
  de->d_namlen = len - 2;
  de->d_link   = NULL;

  strcpy (p, dir);
  p = strchr (p, '\0');
  if (!IS_SLASH(p[-1]))
     *p++ = '\\';
  strcpy (p, file);

  TRACE (3, "len: %u, de->d_name: '%s'\n", (unsigned)len, de->d_name);
  return (TRUE);
}

static int sort_reverse = 0;
static int sort_exact = 0;

static int reverse_sort (int rc)
{
  if (rc == 0)
     return (0);
  if (sort_reverse)
       rc = rc < 0 ?  1 : -1;
  else rc = rc < 0 ? -1 : 1;
  return (rc);
}

/**
 * Prevent an ugly "disk not ready" dialogue from Windows before
 * we call `stat()` or `FindFirstFile()`.
 */
static BOOL safe_to_access (const char *file)
{
  if (_has_drive(file) && !chk_disk_ready((int)file[0]))
  {
    TRACE (2, "Disk %c: not safe to access.\n", (int)file[0]);
    return (FALSE);
  }
  return (TRUE);
}

/**
 * Split an `arg` into a `dir` part and a wildcard `spec` for use by
 * `opendir2()` and `scandir2()`.
 *
 * Both `dir` and `spec` are assumed to point to a buffer of at least
 * `_MAX_PATH` characters.
 *
 * If `arg` start with a `"\\UNC_name"`, do not use `stat()` as that could
 * hang the program for a long time if `UNC_name` resolves to an IP of
 * a host that is down.
 *
 * If `arg` is simply a valid directory-name, use that as `dir` and
 * set `arg == "*"`.
 */
int make_dir_spec (const char *arg, char *dir, char *spec)
{
  struct stat st;
  const char *p, *_arg;
  char *end, *a_copy;
  BOOL  unc, safe;

  /* First, remove any enclosing `"` given in `arg`.
   */
  a_copy = STRDUP (arg);
  str_unquote (a_copy);

  TRACE (3, "a_copy: '%s'\n", a_copy);

  unc  = (strncmp(a_copy, "\\\\", 2) == 0);
  safe = safe_to_access (a_copy);

  /* Check if `arg` is simply a directory.
   */
  if (!unc && safe && stat(a_copy, &st) >= 0 && (st.st_mode & S_IFMT) == S_IFDIR)
  {
    strcpy (dir, a_copy);
    strcpy (spec, "*");
    TRACE (2, "stat() okay:\n");
    goto quit;
  }

  if (unc)
       TRACE (2, "Not using stat() on an UNC name.\n");
  else strcpy (dir, ".");   /* set default values */

  strcpy (spec, "*");
  end = strchr (a_copy, '\0');
  _arg = a_copy;

  /* Step over drive/dir part
   */
  while ((p = strpbrk(_arg, ":/\\")) != NULL)
        _arg = p + 1;

  if (_arg - a_copy > 0)
  {
    _strlcpy (dir, a_copy, _arg - a_copy + 1);
    if (dir[1] == ':' && dir[2] == '\0') /* Turn a "x:" into a "x:." */
       strcat (dir+2, ".");
  }

  if (end - _arg > 0)
     _strlcpy (spec, _arg, end - _arg + 1);

quit:
  FREE (a_copy);
  TRACE (2, "dir: '%s', spec: '%s'\n", dir, spec);
  return (1);
}

DIR2 *opendir2x (const char *dir_name, const struct od2x_options *opts)
{
  struct dirent2 *de;
  WIN32_FIND_DATA ff;
  DIR2           *dirp;
  HANDLE          hnd;
  size_t          max_cnt  = 100;
  size_t          max_size = max_cnt * sizeof(*de);
  char            path [_MAX_PATH];
  char           *file;

  sort_exact = sort_reverse = 0;

  _strlcpy (path, dir_name, sizeof(path));

  dirp = CALLOC (1, sizeof(*dirp));
  if (!dirp)
     goto enomem;

  /* This array get REALLOC()'ed as needed below.
   */
  dirp->dd_contents = CALLOC (1, max_size);
  if (!dirp->dd_contents)
     goto enomem;

  TRACE (3, "CALLOC (%u) -> %p\n", (unsigned)max_size, dirp->dd_contents);

 /*
  * If we're called from `scandir2()`, we have no pattern; we match all files.
  * If we're called from `opendir2x()`, maybe use "*" as pattern if `opts->recursive == 1`?
  * And filter later on.
  */
  if (IS_SLASH(dir_name[0]) && !dir_name[1])
       snprintf (path, sizeof(path), "\\%s", opts ? opts->pattern : "*");
  else snprintf (path, sizeof(path), "%s\\%s", dir_name, opts ? opts->pattern : "*");

  TRACE (3, "path: %s\n", path);

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
      goto enomem;
    }

    TRACE (3, "adding to de: %p, dirp->dd_num: %u\n", de, (unsigned)dirp->dd_num);

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
      TRACE (3, "Limit reached. REALLOC (%u) -> %p\n", (unsigned)max_size, more);

      if (!more)
      {
        free_contents (dirp);
        goto enomem;
      }
      dirp->dd_contents = more;
    }
  }
  while (file);

  dirp->dd_loc = 0;

  if (opts)
  {
    QsortCmpFunc sorter;

    set_sort_funcs (opts->sort, &sorter, NULL);
    if (sorter)
       qsort (dirp->dd_contents, dirp->dd_num, sizeof(struct dirent2), sorter);
  }

  return (dirp);

enomem:
  FREE (dirp);
  errno = ENOMEM;
  return (NULL);
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

  TRACE (3, "dirp->dd_contents: %p, dirp->dd_loc: %u, dirp->dd_num: %u\n",
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
  return (long) (dp->dd_loc);
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
    {
      FREE (de->d_name);
      FREE (de->d_link);
    }
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
    if (!safe_to_access(spec))
         *hnd = INVALID_HANDLE_VALUE;
    else *hnd = FindFirstFile (spec, ff);

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
    if (!strcmp(rc, ".") || !strcmp(rc, ".."))
       rc = getdirent2 (hnd, NULL, ff);
  }

  TRACE (3, "spec: %s, *hnd: %p, rc: %s, err: %lu\n",
         spec ? spec : "<N/A>", *hnd, rc, okay ? 0 : GetLastError());
  return (rc);
}

/**
 * Implementation of scandir2() which uses the above opendir2()
 * and readdir2() implementations.
 *
 * \param[in]      dirname     a plain directory name; no wild-card part.
 * \param[in,out]  namelist_p  unallocated array of pointers to `dirent2` structures.
 * \param[in]      sd_select   pointer to function to specify which files to include in `namelist[]`.
 * \param[in]      dcomp       pointer to sorting function to `qsort()`, e.g. `compare_alphasort()`.
 *
 * \retval `number-1` of files added to `*namelist_p[]`.
 *         (highest index allocated in this array).
 *         I.e. if it returns 0, there are no files in `dir_name`.
 *
 * \retval -1 on error. Inspect `errno` for cause.
 */
int scandir2 (const char       *dirname,
              struct dirent2 ***namelist_p,
              ScandirSelectFunc sd_select,
              ScandirCmpFunc    dcomp)
{
  struct dirent2 **namelist;
  DIR2  *dirptr = NULL;
  int    tdirsize = sizeof(struct dirent2) + _MAX_PATH;
  int    num = 0;
  size_t max_cnt  = 100;
  size_t max_size = max_cnt * sizeof(struct dirent2);

  dirptr = opendir2 (dirname);    /* This will match anything and not call qsort() */
  if (!dirptr)
  {
    TRACE (1, "opendir2 (\"%s\"): failed\n", dirname);
    return (-1);
  }

  namelist = CALLOC (1, max_size);
  if (!namelist)
  {
    TRACE (1, "CALLOC() of %u bytes failed.\n", (unsigned)max_size);
    goto enomem;
  }

  while (1)
  {
    struct dirent2 *de = readdir2 (dirptr);
    char  *p;
    int    si;

    if (!de)
       break;

    TRACE (2, "readdir2(): %s.\n", de->d_name);

    /* The "." and ".." entries are already filtered out in `getdirent2()`.
     * The caller can filter out more if needed in a `sd_select` function.
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
      TRACE (1, "MALLOC() of %u bytes failed.\n", tdirsize);
      goto enomem;
    }

    memcpy (namelist[num], de, sizeof(struct dirent2));
    p = (char*) namelist[num] + sizeof(struct dirent2);
    _strlcpy (p, de->d_name, _MAX_PATH);
    namelist [num]->d_name = p;

    if (++num == max_cnt)
    {
      max_size *= 5;
      max_cnt  *= 5;
      namelist = REALLOC (namelist, max_size);
      if (!namelist)
      {
        TRACE (1, "REALLOC() of %u bytes failed.\n", (unsigned)max_size);
        goto enomem;
      }
    }
  }

  if (dcomp)
       qsort (namelist, num, sizeof(struct dirent2*), (QsortCmpFunc)dcomp);
  else sort_reverse = 0;

  closedir2 (dirptr);

  *namelist_p = namelist;
  return (num);

enomem:
  if (dirptr)
     closedir2 (dirptr);
  errno = ENOMEM;
  return (-1);
}

/**
 * Alphabetic order comparison routine.
 * Does not differensiate between files and directories.
 */
static int MS_CDECL compare_alphasort (const struct dirent2 *a, const struct dirent2 *b)
{
  const char *base_a = basename (a->d_name);
  const char *base_b = basename (b->d_name);
  int         rc;

  if (sort_exact)
       rc = strcmp (base_a, base_b);
  else rc = stricmp (base_a, base_b);
  rc = reverse_sort (rc);

  TRACE (3, "base_a: %s, base_b: %s, rc: %d\n", base_a, base_b, rc);
  return (rc);
}

static int MS_CDECL compare_dirs_first (const struct dirent2 *a, const struct dirent2 *b)
{
  BOOL a_dir = (a->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
  BOOL b_dir = (b->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
  int  rc;

  if (!a_dir && !b_dir)
       rc = compare_alphasort (a, b);
  else if (a_dir && !b_dir)
       rc = reverse_sort (-1);
  else if (!a_dir && b_dir)
       rc = reverse_sort (1);
  else rc = compare_alphasort (a, b);

  TRACE (3, "a->d_name: %-15.15s, b->d_name: %-15.15s, a_dir: %d, b_dir: %d, rc: %d\n",
         basename(a->d_name), basename(b->d_name), a_dir, b_dir, rc);
  return (rc);
}

static int MS_CDECL compare_files_first (const struct dirent2 *a, const struct dirent2 *b)
{
  BOOL a_dir = (a->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
  BOOL b_dir = (b->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
  int  rc;

  if (!a_dir && !b_dir)
       rc = compare_alphasort (a, b);
  else if (a_dir && !b_dir)
       rc = reverse_sort (1);
  else if (!a_dir && b_dir)
       rc = reverse_sort (-1);
  else rc = compare_alphasort (a, b);

  TRACE (3, "a->d_name: %-15.15s, b->d_name: %-15.15s, a_dir: %d, b_dir: %d, rc: %d\n",
         basename(a->d_name), basename(b->d_name), a_dir, b_dir, rc);
  return (rc);
}

int MS_CDECL sd_compare_alphasort (const void **a, const void **b)
{
  return compare_alphasort (*a, *b);
}

int MS_CDECL sd_compare_files_first (const void **a, const void **b)
{
  return compare_files_first (*a, *b);
}

int MS_CDECL sd_compare_dirs_first (const void **a, const void **b)
{
  return compare_dirs_first (*a, *b);
}

/**
 * Return the sorting function based on `sort`.
 */
static void set_sort_funcs (enum od2x_sorting sort, QsortCmpFunc *qsort_func, ScandirCmpFunc *sd_cmp_func)
{
  enum od2x_sorting s = (sort & ~(OD2X_SORT_REVERSE | OD2X_SORT_EXACT));

  sort_reverse = (sort & OD2X_SORT_REVERSE) ? 1 : 0;
  sort_exact   = (sort & OD2X_SORT_EXACT)   ? 1 : 0;

  switch (s)
  {
    case OD2X_FILES_FIRST:
         TRACE (3, "Using compare_files_first(), sort_reverse: %d\n", sort_reverse);
         if (qsort_func)
            *qsort_func = (QsortCmpFunc) compare_files_first;
         if (sd_cmp_func)
            *sd_cmp_func = sd_compare_files_first;
         break;

    case OD2X_DIRECTORIES_FIRST:
         TRACE (3, "Using compare_dirs_first(), sort_reverse: %d\n", sort_reverse);
         if (qsort_func)
            *qsort_func = (QsortCmpFunc) compare_dirs_first;
         if (sd_cmp_func)
            *sd_cmp_func = sd_compare_dirs_first;
         break;

    case OD2X_ON_NAME:
         TRACE (3, "Using compare_alphasort(), sort_reverse: %d\n", sort_reverse);
         if (qsort_func)
            *qsort_func = (QsortCmpFunc) compare_alphasort;
         if (sd_cmp_func)
            *sd_cmp_func = sd_compare_alphasort;
         break;

    default:
         TRACE (3, "Not sorting.\n");
         if (qsort_func)
            *qsort_func = NULL;
         if (sd_cmp_func)
            *sd_cmp_func = NULL;
         break;
  }
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

static DWORD  recursion_level = 0;
static DWORD  num_directories = 0;
static DWORD  num_junctions = 0;
static DWORD  num_junctions_err = 0;
static DWORD  num_files = 0;
static UINT64 total_size = 0;
static UINT64 total_size_alloc = 0;
static UINT64 total_size_compr = 0;
static BOOL   follow_junctions = TRUE;
static BOOL   use_scandir = FALSE;
static BOOL   disk_usage_mode  = FALSE;
static BOOL   disk_usage_tree  = FALSE;
static int    disk_usage_print = 'b';
static char   drive_root [4];


void usage (void)
{
  if (disk_usage_mode)
       puts ("Usage: du [-bkmHRtu] [dir\\spec*]\n"
             "        -b:  show bytes count (default).\n"
             "        -k:  show KiloBytes count.\n"
             "        -m:  show MegaBytes count.\n"
             "        -H:  show sizes in human readable format (e.g. 103 KB, 23 MB).\n"
             "        -R:  do not be recursive.\n"
             "        -t:  show time of the last modification of any file in the directory (not yet).\n"
             "        -T:  show directories as a tree (not yet).\n"
             "        -u:  show directories on Unix form.");

  else puts ("Usage: dirlist [-cdourzSs<type>] [dir\\spec*]\n"
             "       -c:      case-sensitive.\n"
             "       -d:      debug-level.\n"
             "       -o:      show file-owner.\n"
             "       -u:      show files on Unix form.\n"
             "       -r:      be recursive.\n"
             "       -S:      use scandir2(). Otherwise use readdir2().\n"
             "       -s type: sort the listing on \"names\", \"files\", \"dirs\". Optionally with \",reverse\".\n"
             "       -z:      show file-sizes.");
  exit (-1);
}

/**
 * Cygwin:    Convert `path` from Windows to Posix form.
 * Otherwise: Replace all `\\` with a `/`.
 */
static const char *make_unixy_path (const char *path)
{
  static char name [_MAX_PATH];

#if defined(__CYGWIN__)
  if (cygwin_conv_path(CCP_WIN_A_TO_POSIX, path, name, sizeof(name)) == 0)
     return (name);
  return (path);

#else
  char *p, *end;

  _strlcpy (name, path, sizeof(name));
  end = strchr (name, '\0');
  for (p = name; p < end; p++)
  {
    if (IS_SLASH(*p))
       *p = '/';
  }
  *p = '\0';
  return (name);
#endif
}

static int print_it (const char  *file,
                     DWORD64      fsize,
                     const char  *prefix,
                     const struct od2x_options *opts,
                     BOOL         show_size,
                     BOOL         show_owner)
{
  const char *f;
  int         slash;

  if (opts && opts->unixy_paths)
  {
    f = make_unixy_path (file);
    slash = '/';
  }
  else
  {
    f = file;
    slash = '\\';
  }

  if (prefix)
     C_puts (prefix);

  if (show_size)
  {
    C_printf ("%-10s", get_file_size_str(fsize));
  }
  else if (show_owner)
  {
    char *account_name = NULL;

    if (get_file_owner(f, NULL, &account_name) || account_name)
         C_printf ("%-16s ", account_name);
    else C_printf ("%-16s ", "<Unknown>");
    FREE (account_name);
  }
  else if (opt.show_size)
  {
    C_puts ("          ");
  }
  else if (opt.show_owner)
  {
    C_puts ("                 ");
  }

  C_setraw (1);
  C_puts (f);
  C_setraw (0);
  return (slash);
}

static void print_dirent2 (const struct dirent2 *de, int idx, const struct od2x_options *opts)
{
  int is_dir      = (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
  int is_junction = (de->d_attrib & FILE_ATTRIBUTE_REPARSE_POINT);
  int slash;

  C_printf ("~1%5d ~0(%lu): ", idx, (unsigned long)recursion_level);
  if (recursion_level < 10)
     C_putc (' ');
  C_printf ("~4%-7s~6", is_junction ? "<LINK>" : is_dir ? "<DIR>" : "");

  /* Junctions need special handling.
   */
  if (is_junction)
  {
    static char prefix[] = " \n              -> ~3";

    prefix[0] = (char) print_it (de->d_name, 0ULL, NULL, opts, FALSE, FALSE);
    slash = print_it (de->d_link ? de->d_link : "??", 0ULL, prefix, opts, FALSE, FALSE);
    if (de->d_link)
       C_putc (slash);
  }
  else
  {
    slash = print_it (de->d_name, de->d_fsize, NULL, opts, opt.show_size && !is_dir, opt.show_owner);
    if (is_dir)
       C_putc (slash);
  }

  C_puts ("~0\n");

  if (!is_dir && !is_junction)
  {
    UINT64 compr_size;

    num_files++;
    total_size += de->d_fsize;
    total_size_alloc += get_file_alloc_size (de->d_name, de->d_fsize);
    if (get_file_compr_size(de->d_name, &compr_size))
         total_size_compr += compr_size;
    else total_size_compr += de->d_fsize;
  }
  else
  {
    if (is_dir)
    {
      num_directories++;
      total_size_alloc += get_file_alloc_size (de->d_name, (UINT64)-1);
    }
    if (is_junction)
    {
      num_junctions++;
      if (!de->d_link)
         num_junctions_err++;
    }
  }
}

static void final_report (void)
{
  #define ADD_VALUE(v)  { v, #v }

  static const struct search_list fs_flags[] = {
               ADD_VALUE (FILE_FILE_COMPRESSION),
               ADD_VALUE (FILE_NAMED_STREAMS),
               ADD_VALUE (FILE_PERSISTENT_ACLS),
               ADD_VALUE (FILE_READ_ONLY_VOLUME),
               ADD_VALUE (FILE_SEQUENTIAL_WRITE_ONCE),
               ADD_VALUE (FILE_SUPPORTS_ENCRYPTION),
               ADD_VALUE (FILE_SUPPORTS_EXTENDED_ATTRIBUTES),
               ADD_VALUE (FILE_SUPPORTS_HARD_LINKS),
               ADD_VALUE (FILE_SUPPORTS_OBJECT_IDS),
               ADD_VALUE (FILE_SUPPORTS_OPEN_BY_FILE_ID),
               ADD_VALUE (FILE_SUPPORTS_REPARSE_POINTS),
               ADD_VALUE (FILE_SUPPORTS_SPARSE_FILES),
               ADD_VALUE (FILE_SUPPORTS_TRANSACTIONS),
               ADD_VALUE (FILE_SUPPORTS_USN_JOURNAL),
               ADD_VALUE (FILE_UNICODE_ON_DISK),
               ADD_VALUE (FILE_VOLUME_IS_COMPRESSED),
               ADD_VALUE (FILE_VOLUME_QUOTAS)
             };
  char  fs_name [20];
  char  volume [_MAX_PATH];
  DWORD volume_sn = 0;
  DWORD max_component_length = 0;
  DWORD fs_flag = 0;

  C_printf ("  Num files:        %lu\n", (unsigned long)num_files);
  C_printf ("  Num directories:  %lu\n", (unsigned long)num_directories);
  C_printf ("  Num junctions:    %lu (errors: %lu)\n", (unsigned long)num_junctions, (unsigned long)num_junctions_err);
  C_printf ("  total-size:       %s bytes", str_qword(total_size));
  C_printf (" (allocated: %s,", str_qword(total_size_alloc));

  if (total_size_alloc && total_size_compr > 0 && total_size_compr < total_size_alloc)
  {
    double percentage = 100.0 - 100.0 * ((double)total_size_compr / (double)total_size_alloc);
    C_printf (" compressed: %s, %.02f%%)\n", str_qword(total_size_compr), percentage);
  }
  else
    C_printf (" no compressed files)\n");

  C_puts ("  Volume info: ");

  if (!GetVolumeInformation (drive_root, volume, sizeof(volume), &volume_sn, &max_component_length,
                             &fs_flag, fs_name, sizeof(fs_name)))
     C_printf ("GetVolumeInformation() failed: %s\n", win_strerror(GetLastError()));
  else
  {
    size_t i;

    C_printf ("'%s', volume_sn: %lu, fs_name: '%s', max_component_length: %lu\n",
              volume, volume_sn, fs_name, max_component_length);

    C_puts ("  fs_flags:\n");
    for (i = 0; i < DIM(fs_flags); i++)
    {
      if (fs_flag & fs_flags[i].value)
           C_printf ("    + %s\n", fs_flags[i].name);
      else C_printf ("    - %s\n", fs_flags[i].name);
    }
  }
}

static char *_get_actual_filename (char *link)
{
#if USE_get_actual_filename
  char *New = link;

  if (get_actual_filename(&New, FALSE))
     link = New;
  else
#endif
     link = STRDUP (link);
  return _fix_drive (link);
}

/**
 * Recursive handler for `scandir2()`.
 */
void do_scandir2 (const char *dir, const struct od2x_options *opts)
{
  struct dirent2 **namelist;
  int              i, n;
  ScandirCmpFunc   sorter;

  set_sort_funcs (opts->sort, NULL, &sorter);
  n = scandir2 (dir, &namelist, NULL, sorter);

  TRACE (1, "scandir2 (\"%s\"), pattern: '%s': n: %d, sort_reverse: %d.\n", dir, opts->pattern, n, sort_reverse);

  if (n < 0)
     TRACE (0, "(recursion_level: %lu). Error in scandir2 (\"%s\"): %s\n",
            (unsigned long)recursion_level, dir, strerror(errno));
  else
  {
    for (i = 0; i < n; i++)
    {
      struct dirent2 *de = namelist[i];
      int    is_dir      = (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
      int    is_junction = (de->d_attrib & FILE_ATTRIBUTE_REPARSE_POINT);

      if (is_junction && follow_junctions && get_disk_type(de->d_name[0]) != DRIVE_REMOTE)
      {
        char result [_MAX_PATH] = "??";
        BOOL rc = get_reparse_point (de->d_name, result, sizeof(result));

        if (rc)
           namelist[i]->d_link = _get_actual_filename (result);
      }

      if (fnmatch(opts->pattern, basename(de->d_name), fnmatch_case(FNM_FLAG_PATHNAME)) == FNM_MATCH)
         print_dirent2 (de, i, opts);

      if (opts->recursive && (is_dir || is_junction))
      {
        recursion_level++;
        do_scandir2 (de->d_name, opts);
        recursion_level--;
      }
    }

    TRACE (2, "(recursion_level: %lu). freeing %d items and *namelist.\n",
           (unsigned long)recursion_level, n);

    while (n--)
    {
      FREE (namelist[n]->d_link);
      FREE (namelist[n]);
    }
    FREE (namelist);
  }
}

/**
 * `opts->recursive` may not work if `spec != "*"`.
 * \todo use `opendir2()` and do the filtering here (use `fnmatch()`?).
 */
static void do_dirent2 (const char *dir, const struct od2x_options *opts)
{
  struct dirent2 *de;
  int             i = 0;
  DIR2           *dp = opendir2x (dir, opts);

  TRACE (1, "dir: '%s', pattern: '%s', dp: %p\n", dir, opts->pattern, dp);

  if (!dp)
     return;

  while ((de = readdir2(dp)) != NULL)
  {
    int is_dir      = (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
    int is_junction = (de->d_attrib & FILE_ATTRIBUTE_REPARSE_POINT);

    if (is_junction && follow_junctions && get_disk_type(de->d_name[0]) != DRIVE_REMOTE)
    {
      char result [_MAX_PATH] = "??";
      BOOL rc = get_reparse_point (de->d_name, result, sizeof(result));

      if (rc)
         de->d_link = _get_actual_filename (result);
    }

    print_dirent2 (de, i++, opts);

    if (opts->recursive && (is_dir || is_junction))
    {
      recursion_level++;
      do_dirent2 (de->d_name, opts);
      recursion_level--;
    }
  }
  closedir2 (dp);
}

/*
 * Do similar to what a POSIX 'du' program does:
 *  f:\Cygwin64\bin\du.exe -bc foo*
 *  11968   foo
 *  12194   foo bar
 *  24162   total
 */
static UINT64 do_disk_usage (const char *dir, const struct od2x_options *opts)
{
  struct dirent2 **namelist;
  char             sz_buf [20];
  int              i, n;
  UINT64           sum = 0ULL;
  static UINT64    highest_sum = 0ULL;
  static int       width = 8;

  n = scandir2 (dir, &namelist, NULL, NULL);
  TRACE (1, "recursion_level: %lu, dir: '%s', n: %d\n",
         (unsigned long)recursion_level, dir, n);

  if (n < 0)
     return (0);

  for (i = 0; i < n; i++)
  {
    const struct dirent2 *de = namelist[i];
    int   is_dir      = (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
    int   is_junction = (de->d_attrib & FILE_ATTRIBUTE_REPARSE_POINT);

    if (is_junction)
       continue;

    if (is_dir && opts->recursive)
    {
      if (recursion_level > 0 || fnmatch(opts->pattern, basename(de->d_name), FNM_FLAG_PATHNAME) == FNM_MATCH)
      {
        recursion_level++;
        sum += do_disk_usage (de->d_name, opts);
        recursion_level--;
      }
    }
    else
    {
      sum += de->d_fsize;
    }
  }

  if (sum > highest_sum)
     highest_sum = sum;

  if ((double)highest_sum > 1E8)
     width = count_digit (highest_sum);

  switch (disk_usage_print)
  {
    case 'k':
          snprintf (sz_buf, sizeof(sz_buf), "%-*llu", width, sum/1024);
          break;
    case 'm':
         snprintf (sz_buf, sizeof(sz_buf), "%-*llu", width, sum/1024/1024);
         break;
    case 'H':
         snprintf (sz_buf, sizeof(sz_buf), "%-8s", get_file_size_str(sum));
         break;
    default:
         snprintf (sz_buf, sizeof(sz_buf), "%-*llu", width, sum);
         break;
  }

  C_setraw (1);

  if (disk_usage_tree)
       print_tree_branch (sz_buf, opts->unixy_paths ? make_unixy_path(dir) : dir);

  else if (!opts->recursive)
       C_printf ("Total for '%s': %s\n", dir, str_ltrim(sz_buf));

  else C_printf ("%s %s\n", sz_buf, opts->unixy_paths ? make_unixy_path(dir) : dir);

  C_setraw (0);

  while (n--)
  {
    FREE (namelist[n]->d_link);
    FREE (namelist[n]);
  }
  FREE (namelist);
  return (sum);
}

static void print_tree_branch (const char *sz_buf, const char *directory)
{
  BOOL at_top = (recursion_level == 0);

  C_printf ("%s, l:%2lu ", sz_buf, recursion_level);

  if (at_top)
     C_printf ("%s\n", directory);
  else
     C_printf ("|__ %s\n", directory);
}

static enum od2x_sorting get_sorting (const char *s_type)
{
  enum od2x_sorting sort = OD2X_UNSORTED;
  const char *s_supported = "\"names\", \"files\", \"dirs\"";

  if (!strnicmp(s_type, "names", 5))
       sort = OD2X_ON_NAME;
  else if (!strnicmp(s_type, "files", 5))
       sort = OD2X_FILES_FIRST;
  else if (!strnicmp(s_type, "dirs", 4))
       sort = OD2X_DIRECTORIES_FIRST;
  else FATAL ("Illegal sorting type '%s'.\n"
              "These are supported: %s. Optionally with \",reverse\".\n", s_type, s_supported);

  if (strstr(s_type, ",reverse"))
     sort |= OD2X_SORT_REVERSE;
  return (sort);
}

static BOOL WINAPI halt (DWORD event)
{
  if (event != CTRL_C_EVENT)
     return (FALSE);

  C_puts ("~0\n");
  FAST_EXIT();
  return (TRUE);
}

static void do_getopt (int argc, char **argv, const char *options, struct od2x_options *opts)
{
  int ch;

  while ((ch = getopt(argc, argv, options)) != EOF)
     switch (ch)
     {
       case 'c':
            opts->sort |= OD2X_SORT_EXACT;
            break;
       case 'd':
            opt.debug++;
            break;
       case 'j':
            follow_junctions = FALSE;
            break;
       case 'u':
            opts->unixy_paths++;
            break;
       case 'r':
            opts->recursive = 1;
            break;
       case 'R':
            opts->recursive = 0;
            break;
       case 'S':
            use_scandir = TRUE;
            break;
       case 's':
            opts->sort |= get_sorting (optarg);
            break;
       case 'o':
            opt.show_owner++;
            break;
       case 'T':
            disk_usage_tree = TRUE;
            break;
       case 'z':
            opt.show_size++;
            break;
       case 'b':
       case 'm':
       case 'k':
       case 'H':
            disk_usage_print = ch;
            break;
       case '?':
       case 'h':
       default:
            usage();
     }
}

int MS_CDECL main (int argc, char **argv)
{
  char dir_buf  [_MAX_PATH];
  char spec_buf [_MAX_PATH];
  char root [_MAX_PATH];

  struct od2x_options opts;

  crtdbug_init();
  memset (&opts, '\0', sizeof(opts));
  memset (&opt, '\0', sizeof(opt));

  if (argc >= 2 && !strcmp(argv[0], "--disk-usage"))  /* called from 'du.exe' */
  {
    disk_usage_mode = TRUE;
    opts.recursive  = 1;  /* option '-R' reverts this */
    argc--;
    argv++;
    do_getopt (argc, argv, "ubdmkHRtTh?", &opts);
  }
  else
    do_getopt (argc, argv, "cdjors:Suzh?", &opts);

  if (!argv[optind])
  {
    argc    = 1;
    argv[0] = ".";
  }
  else
  {
    argc -= optind;
    argv += optind;
  }

  C_use_colours = 1;
  C_init();

  if (argc-- < 1 || *argv == NULL)
     usage();

  if (!disk_usage_mode && opts.sort == OD2X_SORT_EXACT)
     WARN ("Option '-c' with no sort option '-s xx' is meaningless.\n");

  SetConsoleCtrlHandler (halt, TRUE);

  make_dir_spec (*argv, dir_buf, spec_buf);
  opts.pattern = spec_buf;

  _fix_path (dir_buf, root);
  _strlcpy (drive_root, root, 4);

  if (disk_usage_mode)
  {
    do_disk_usage (dir_buf, &opts);
    crtdbug_exit();
  }
  else
  {
    if (use_scandir)
         do_scandir2 (dir_buf, &opts);
    else do_dirent2 (dir_buf, &opts);

    final_report();
    crtdbug_exit();
    mem_report();
  }
  return (0);
}
#endif  /* DIRLIST_TEST */
