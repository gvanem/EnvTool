/* Copyright (C) 1995-2001 DJ Delorie, see COPYING.DJ for details */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <fcntl.h>
#include <malloc.h>
#include <io.h>
#include <windows.h>

#include "envtool.h"
#include "getopt_long.h"
#include "color.h"
#include "win_glob.h"

#define PATHBUF_LEN  2000
#define EOS          '\0'

typedef struct Save {
        struct Save *prev;
        char        *entry;
      } Save;

static Save *save_list;
static int   save_count;
static int (*errfunc) (const char *path, int err);
static char *pathbuf, *pathbuf_end;
static char  slash;
static char  global_slash;
static DWORD recursion_level;
static DWORD num_ignored_errors;

static int glob2 (const char *pattern, char *epathbuf);
static int glob_add (const char *path, BOOL is_dir, unsigned line);
static int MS_CDECL str_compare (const void *va, const void *vb);

struct ffblk {
       HANDLE    ff_handle;
       DWORD     ff_attrib;         /* FILE_ATTRIBUTE_xx. Ref MSDN. */
       FILETIME  ff_time_create;
       FILETIME  ff_time_access;    /* always midnight local time */
       FILETIME  ff_time_write;
       DWORD64   ff_fsize;
       char      ff_name [_MAX_PATH];
     };

typedef struct glob_new_entry {
        struct ffblk ff;
        char  *real_target;
      } glob_new_entry;

typedef struct Save_new {
        struct Save_new *prev;
        glob_new_entry   entry;
      } Save_new;

static DWORD find_first (const char *file_spec, struct ffblk *ffblk);
static DWORD find_next (struct ffblk *ffblk);

#if defined(WIN_GLOB_TEST)
  static DWORD64 total_files, total_dirs, total_reparse_points;
  static DWORD64 total_size;
  static int     show_full_path;
#endif

/*
 * For fnmatch() used in the walker_func callback.
 */
static int glob_flags;

#if defined(WIN_GLOB_TEST)
  static const char *global_spec, *orig_spec, *dot_spec;
  static int         fn_flags;

  typedef int (*walker_func) (const char *path, const struct ffblk *ff);

static DWORD glob_new2 (const char *dir,
                        walker_func callback,
                        glob_new_t *res)
{
  struct ffblk ff;
  char         search_spec[MAX_PATH], path[MAX_PATH], *dir_end;
  int          done;
  DWORD        rc;

  if (!dir || !*dir || !callback)
  {
    rc = ERROR_BAD_ARGUMENTS;
    goto quit;
  }

  /* Construct the search spec for find_first().  Treat "d:" as "d:.".
   */
  strcpy (search_spec, dir);
  dir_end = strchr (search_spec,'\0') - 1;

  if (*dir_end == ':')
  {
    *++dir_end = '.';
    dir_end[1] = '\0';
  }
  if (!IS_SLASH(*dir_end))
  {
    *++dir_end = '\\';
    *++dir_end = '\0';
  }

  strcpy (dir_end, global_spec);

  /* Prepare the buffer where the full pathname of the found files
   * will be placed.
   */
  strcpy (path, dir);
  dir_end = strchr (path,'\0') - 1;

  if (*dir_end == ':')
  {
    *++dir_end = '.';
    dir_end[1] = '\0';
  }
  if (!IS_SLASH(*dir_end))
  {
    *++dir_end = '\\';
    *++dir_end = '\0';
  }
  else
    ++dir_end;

  TRACE (2, "search_spec: '%s', dir_end: '%s', path: '%s'.\n", search_spec, dir_end, path);

  done = find_first (search_spec, &ff);

  while (!done)
  {
    int result1;

    /* Skip '.' and '..' entries.
     */
    if ((ff.ff_attrib & FILE_ATTRIBUTE_DIRECTORY) &&
        (!strcmp(ff.ff_name, ".") || !strcmp(ff.ff_name, "..")))
    {
      done = find_next (&ff);
      continue;
    }

    /* Construct full pathname in path[].
     */
    strcpy (dir_end, ff.ff_name);

    /* Invoke callback() on this file.
     */
    result1 = (*callback) (path, &ff);
    if (result1)
       return (result1);

    /* If this is a directory, walk its siblings if in recursive-mode.
     * The 'path' on exit could be different that on enty if this path is a junction.
     */
    if ((glob_flags & GLOB_RECURSIVE) && (ff.ff_attrib & FILE_ATTRIBUTE_DIRECTORY))
    {
      DWORD result2;

      recursion_level++;
      result2 = glob_new2 (path, callback, res);
      if (result2)
         return (result2);
      recursion_level--;
    }
    done = find_next (&ff);
  }

  rc = GetLastError();

quit:
  TRACE (1, "GetLastError(): %s.\n", win_strerror(rc));

  /* Normal case: tree exhausted
   */
  if (rc == ERROR_NO_MORE_FILES)
     rc = 0;

  /* If we get "access denied" on a directory at level > 0, we ignore and continue
   */
  else if (rc == ERROR_ACCESS_DENIED /* && recursion_level > 0 */)
     rc = 0;

  return (rc);
}
#endif /* WIN_GLOB_TEST */


int glob_new (const char *_dir, int _flags,
              int (*_callback)(const char *path),
              glob_new_t *_pglob)
{
  ARGSUSED (_dir);
  ARGSUSED (_flags);
  ARGSUSED (_callback);
  ARGSUSED (_pglob);
  return (0);  /* to-do !! */
}

void globfree_new (glob_new_t *_pglob)
{
  struct glob_new_entry *e;
  size_t i;

  if (!_pglob->gl_pathv)
     return;

  for (i = 0, e = _pglob->gl_pathv; i < _pglob->gl_pathc; i++, e++)
  {
    FREE (e->real_target);
  }
  FREE (_pglob->gl_pathv);
}


/*
 * Use FindFirstFile() or FindFirstFileEx().
 *
 * A list of "File Management Functions" here:
 *   https://msdn.microsoft.com/en-us/library/aa364232(VS.85).aspx
 *
 * Note on the possibility of infinite recursion using FindFirstFile():
 *   https://social.msdn.microsoft.com/Forums/windowsdesktop/en-US/05d14368-25dd-41c8-bdba-5590bf762a68/inconsistent-file-system-enumeration-with-findfirstfile-finenextfile?forum=windowscompatibility
 *
 * Notes on FIND_FIRST_EX_LARGE_FETCH:
 *   http://blogs.msdn.com/b/oldnewthing/archive/2013/10/24/10459773.aspx
 */
static DWORD find_first (const char *file_spec, struct ffblk *ffblk)
{
  WIN32_FIND_DATA ff_data;
  HANDLE          handle;
  DWORD           rc = 0;

  ff_data.dwFileAttributes = 0;

  handle = (glob_flags & GLOB_USE_EX) ?
             FindFirstFileEx (file_spec, FindExInfoStandard, &ff_data,
                              FindExSearchNameMatch, 0, FIND_FIRST_EX_LARGE_FETCH) :
             FindFirstFile (file_spec, &ff_data);


  TRACE (3, "find_first (\"%s\") -> %" ADDR_FMT "\n", file_spec, ADDR_CAST(handle));

  if (handle == INVALID_HANDLE_VALUE)
  {
    rc = GetLastError();
    TRACE (1, "recursion_level: %lu, GetLastError(): %s.\n",
           (unsigned long)recursion_level, win_strerror(rc));

    /*
     * We get "access denied" for a directory-name which is a Junction.
     * But we use get_reparse_point() to get the target for this junction.
     */
    if (rc == ERROR_ACCESS_DENIED)
    {
      num_ignored_errors++;
    }
#if 0
    else if (recursion_level > 1)
    {
      num_ignored_errors++;
      rc = 0;
    }
#endif
    return (rc);
  }

  ffblk->ff_handle      = handle;
  ffblk->ff_attrib      = ff_data.dwFileAttributes;
  ffblk->ff_time_create = ff_data.ftCreationTime;
  ffblk->ff_time_access = ff_data.ftLastAccessTime;
  ffblk->ff_time_write  = ff_data.ftLastWriteTime;
  ffblk->ff_fsize       = ((DWORD64)ff_data.nFileSizeHigh << 32) + ff_data.nFileSizeLow;

  _strlcpy (ffblk->ff_name, ff_data.cFileName, sizeof(ffblk->ff_name));

  if (errfunc)
    (*errfunc) (ffblk->ff_name, rc);

  return (rc);
}

static DWORD find_next (struct ffblk *ffblk)
{
  WIN32_FIND_DATA ff_data;
  DWORD           rc = 0;

  ff_data.dwFileAttributes = 0;

  if (!FindNextFile(ffblk->ff_handle, &ff_data))
  {
    rc = GetLastError();
    TRACE (3, "find_next() -> %s\n", win_strerror(rc));
    FindClose (ffblk->ff_handle);
    ffblk->ff_handle = NULL;
  }
  else
  {
    ffblk->ff_attrib      = ff_data.dwFileAttributes;
    ffblk->ff_time_create = ff_data.ftCreationTime;
    ffblk->ff_time_access = ff_data.ftLastAccessTime;
    ffblk->ff_time_write  = ff_data.ftLastWriteTime;
    ffblk->ff_fsize       = ((DWORD64)ff_data.nFileSizeHigh << 32) + ff_data.nFileSizeLow;

    _strlcpy (ffblk->ff_name, ff_data.cFileName, sizeof(ffblk->ff_name));
  }

  if (errfunc)
    (*errfunc) (ffblk->ff_name, rc);

  return (rc);
}

/* 'tolower' might depend on the locale.  We don't want to.
 */
int msdos_tolower_fname (int c)
{
  return (c >= 'A' && c <= 'Z') ? c + 'a' - 'A' : c;
}

static int glob_add (const char *path, BOOL is_dir, unsigned line)
{
  Save *sp;

  for (sp = save_list; sp; sp = sp->prev)
      if (!stricmp(sp->entry, path))
         return (1);

  sp = CALLOC (sizeof(*sp), 1);
  if (!sp)
     return (0);

  /* use get_reparse_point() to get the true location.
   */
  if (is_dir)
  {
    char result [_MAX_PATH] = "??";
    BOOL rc = get_reparse_point (path, result, sizeof(result));

    if (!rc)
         TRACE (1, "get_reparse_point(): %s\n", last_reparse_err);
    else path = result;
  }

  sp->entry = STRDUP (path);
  if (!sp->entry)
  {
    FREE (sp);
    return (0);
  }
  TRACE (2, "add: '%s' (from line %u)\n", sp->entry, line);

  sp->prev = save_list;
  save_list = sp;
  save_count++;
  return (1);
}

static int glob_dirs (const char *rest, char *epathbuf,
                      int first)  /* rest is ptr to null or ptr after slash, bp after slash */
{
  struct ffblk ff;
  int    done;

  TRACE (2, "glob_dirs[%lu]: rest='%s' %c epathbuf='%s' %c pathbuf='%s'\n",
         (unsigned long)recursion_level, rest, *rest, epathbuf, *epathbuf, pathbuf);

  if (first)
  {
    if (*rest)
    {
      if (glob2(rest, epathbuf) == GLOB_NOSPACE)
         return (GLOB_NOSPACE);
    }
    else
    {
      char sl = epathbuf[-1];

      *epathbuf = '\0';
      TRACE (2, "end, checking '%s'\n", pathbuf);
      if (epathbuf == pathbuf)
      {
        epathbuf[0] = '.';
        epathbuf[1] = '\0';
      }
      else
        epathbuf[-1] = '\0';

      if (FILE_EXISTS(pathbuf))
         if (!glob_add(pathbuf,FALSE,__LINE__))
            return (GLOB_NOSPACE);
      epathbuf[-1] = sl;
    }
  }

  strcpy (epathbuf, "*.*");
  done = find_first (pathbuf, &ff);

  while (!done)
  {
    if ((ff.ff_attrib & FILE_ATTRIBUTE_DIRECTORY) &&
        (strcmp(ff.ff_name, ".") && strcmp(ff.ff_name, "..")))
    {
      char *tp;

      TRACE (1, "found '%s' '%s'\n", pathbuf, ff.ff_name);

      strcpy (epathbuf, ff.ff_name);
      tp = strchr (epathbuf, '\0');
      *tp++ = slash;
      *tp = '\0';

      recursion_level++;
      if (*rest)
      {
        if (glob2(rest, tp) == GLOB_NOSPACE)
           return (GLOB_NOSPACE);
      }
      else
      {
        if (!(glob_flags & GLOB_MARK))
           tp[-1] = '\0';
        if (!glob_add(pathbuf,TRUE,__LINE__))
           return (GLOB_NOSPACE);
        tp[-1] = slash;
      }

      *tp = '\0';
      if (glob_dirs(rest, tp, 0) == GLOB_NOSPACE)
         return (GLOB_NOSPACE);

      recursion_level--;
    }
    done = find_next (&ff);
  }
  return (0);
}

static int glob2 (const char *pattern, char *epathbuf)  /* both point *after* the slash */
{
  struct ffblk ff;
  const char  *pp, *pslash;
  char        *bp, *my_pattern;
  int          done;

  if (!strcmp(pattern, "..."))
     return glob_dirs (pattern+3, epathbuf, 1);

  if (!strncmp(pattern, "...", 3) && IS_SLASH(pattern[3]))
  {
    slash = pattern[3];
    return glob_dirs (pattern+4, epathbuf, 1);
  }

  *epathbuf = '\0';

  /* copy as many non-wildcard segments as possible
   */
  pp = pattern;
  bp = epathbuf;
  pslash = bp - 1;

  while (bp < pathbuf_end)
  {
    if (*pp == ':' || IS_SLASH(*pp))
    {
      pslash = bp;
      if (!strcmp(pp+1, "...") ||
          (!strncmp(pp+1, "...", 3) && IS_SLASH(pp[4])))
      {
        if (*pp != ':')
           slash = *pp;
        TRACE (2, "glob2: dots at '%s'\n", pp);
        *bp++ = *pp++;
        break;
      }
    }
    else if (*pp == '*' || *pp == '?' || *pp == '[')
    {
      if (pslash > pathbuf)
         _strlcpy (epathbuf, pattern, pslash - pathbuf);
      pp = pattern + (pslash - epathbuf) + 1;
      bp = epathbuf + (pslash - epathbuf) + 1;
      break;
    }
    else if (*pp == 0)
      break;

    *bp++ = *pp++;
  }
  *bp = 0;

  /* A pattern this big won't match any file.  */
  if (bp >= pathbuf_end && *pp)
     return (0);

  TRACE (2, "glob2: pp: '%s'\n", pp);

  if (*pp == 0) /* end of pattern? */
  {
    if (FILE_EXISTS(pathbuf))
    {
      BOOL is_dir = FALSE;

      if (glob_flags & GLOB_MARK)
      {
        struct ffblk _ff;

        find_first (pathbuf, &_ff);
        if (_ff.ff_attrib & FILE_ATTRIBUTE_DIRECTORY)
        {
          char *_pathbuf = pathbuf + strlen (pathbuf);

          *_pathbuf++ = global_slash;
          *_pathbuf = '\0';
          is_dir = TRUE;
        }
      }
      if (!glob_add(pathbuf,is_dir,__LINE__))
         return (GLOB_NOSPACE);
    }
    return (0);
  }

  TRACE (2, "glob2(): initial segment is '%s', recursion_level: %lu\n",
         pathbuf, (unsigned long)recursion_level);

  if (recursion_level)
  {
    char s = bp[-1];

    bp[-1] = '\0';
    if (!FILE_EXISTS(pathbuf))
       return (0);
    bp[-1] = s;
  }

  for (pslash = pp; *pslash && !IS_SLASH(*pslash); pslash++)
      ;

  if (*pslash)
     slash = *pslash;

  my_pattern = alloca (pslash - pp + 1);
  _strlcpy (my_pattern, pp, pslash - pp + 1);

  TRACE (1, "glob2(): pathbuf: '%s', my_pattern: '%s'\n", pathbuf, my_pattern);

  if (!strcmp(my_pattern, "..."))
  {
    if (glob_dirs(*pslash ? pslash+1 : pslash, bp, 1) == GLOB_NOSPACE)
       return (GLOB_NOSPACE);
    return (0);
  }

  strcpy (bp, "*.*");
  done = find_first (pathbuf, &ff);

  while (!done)
  {
    if ((ff.ff_attrib & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (strcmp(ff.ff_name, ".") && strcmp(ff.ff_name, "..")))
    {
      if (fnmatch(my_pattern, ff.ff_name,
                  fnmatch_case(FNM_FLAG_NOESCAPE | FNM_FLAG_PATHNAME)) == FNM_MATCH)
      {
        strcpy (bp, ff.ff_name);
        if (*pslash)
        {
          char *tp = bp + strlen(bp);

          *tp++ = *pslash;
          *tp = 0;
          TRACE (2, "nest: '%s' '%s'\n", pslash+1, pathbuf);
          recursion_level++;
          if (glob2(pslash+1, tp) == GLOB_NOSPACE)
             return (GLOB_NOSPACE);
          recursion_level--;
        }
        else
        {
          BOOL is_dir;

          TRACE (1, "ffmatch: '%s' matching '%s', add '%s'\n",
                 ff.ff_name, my_pattern, pathbuf);

          is_dir = ff.ff_attrib & FILE_ATTRIBUTE_DIRECTORY;
          if (is_dir && (glob_flags & GLOB_MARK))
          {
            size_t len = strlen (bp);

            bp [len+1] = '\0';
            bp [len] = slash;
          }
          if (!glob_add(pathbuf,is_dir,__LINE__))
             return (GLOB_NOSPACE);
        }
      }
    }
    done = find_next (&ff);
  }
  return (0);
}

static int MS_CDECL str_compare (const void *a, const void *b)
{
  return stricmp (*(const char *const*)a, *(const char *const*)b);
}

static const char *glob_flags_str (unsigned flag)
{
  #define ADD_VALUE(v)  { v, #v }

  static const struct search_list flags[] = {
                      ADD_VALUE (GLOB_MARK),
                      ADD_VALUE (GLOB_NOSORT)
                   };
  return flags_decode (flag, flags, DIM(flags));
}

int glob (const char *_pattern, int flags,
          int (*_errfunc)(const char *path, int err),
          glob_t *_pglob)
{
  char path_buffer [PATHBUF_LEN + 1];
  int  idx;

  pathbuf         = path_buffer + 1;
  pathbuf_end     = path_buffer + PATHBUF_LEN;
  glob_flags      = flags;
  errfunc         = _errfunc;
  recursion_level = 0;
  save_count      = 0;
  save_list       = NULL;
  slash           = global_slash;

  path_buffer[1] = '\0';

  TRACE (1, "glob_flags: %s\n", glob_flags_str(glob_flags));

  memset (_pglob, 0, sizeof(*_pglob));

  if (glob2(_pattern, pathbuf) == GLOB_NOSPACE)
     return (GLOB_NOSPACE);

  if (save_count == 0)
     return (GLOB_NOMATCH);

  _pglob->gl_pathv = CALLOC (save_count+1, sizeof(char *));
  if (!_pglob->gl_pathv)
     return (GLOB_NOSPACE);

  idx = save_count;
  _pglob->gl_pathv[idx] = NULL;

  while (save_list)
  {
    Save *s = save_list;

    idx--;
    _pglob->gl_pathv[idx] = save_list->entry;
    save_list = save_list->prev;
    FREE (s);
  }

  if (!(glob_flags & GLOB_NOSORT))
     qsort (_pglob->gl_pathv, save_count, sizeof(char*), str_compare);

  _pglob->gl_pathc = save_count;
  return (0);
}

void globfree (glob_t *_pglob)
{
  size_t i;

  if (!_pglob->gl_pathv)
     return;

  for (i = 0; i < _pglob->gl_pathc; i++)
      FREE (_pglob->gl_pathv[i]);
  FREE (_pglob->gl_pathv);
}

#if defined(WIN_GLOB_TEST)

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

struct prog_options opt;
char  *program_name = "win_glob";

void usage (void)
{
  printf ("Usage: win_glob [-dCfgrux] <file_spec>\n"
          "       -d:  debug-level.\n"
          "       -C:  case-sensitive file-matching.\n"
          "       -f:  use _fix_path() to show full paths.\n"
          "       -g:  use glob().\n"
          "       -r:  be recursive\n"
          "       -u:  make glob() return Unix slashes.\n"
          "       -x:  use FindFirstFileEx().\n");
  exit (-1);
}

static int callback (const char *epath, int error)
{
//printf ("callback: rc: %d, '%s'\n", error, epath);
  ARGSUSED (epath);
  ARGSUSED (error);
  return (0);
}

#ifndef FILE_ATTRIBUTE_INTEGRITY_STREAM
#define FILE_ATTRIBUTE_INTEGRITY_STREAM  0x00008000
#endif

#ifndef FILE_ATTRIBUTE_NO_SCRUB_DATA
#define FILE_ATTRIBUTE_NO_SCRUB_DATA     0x00020000
#endif

static int ft_callback (const char *path, const struct ffblk *ff)
{
  char  attr_str[14]    = "_____________";
  char *base            = basename (path);
  BOOL  is_dir          = (ff->ff_attrib & FILE_ATTRIBUTE_DIRECTORY);
  BOOL  is_junction     = (ff->ff_attrib & FILE_ATTRIBUTE_REPARSE_POINT);
  BOOL  no_ext          = !is_dir && !is_junction && (strchr(base,'.') == NULL);
  const char *spec      = NULL;
  const char *orig_path = "";

  if (no_ext && dot_spec)
     spec = dot_spec;

  else if (orig_spec)
     spec = orig_spec;

  else if (global_spec)
      spec = global_spec;

  if (spec && fnmatch(spec, base, fn_flags) == FNM_NOMATCH)
  {
    TRACE (2, "fnmatch (\"%s\", \"%s\") failed.\n", spec, base);
    return (0);
  }

  if (ff->ff_attrib & FILE_ATTRIBUTE_READONLY)
     attr_str[0] = 'R';

  if (ff->ff_attrib & FILE_ATTRIBUTE_HIDDEN)
     attr_str[1] = 'H';

  if (ff->ff_attrib & FILE_ATTRIBUTE_SYSTEM)
     attr_str[2] = 'S';

  if (ff->ff_attrib & FILE_ATTRIBUTE_COMPRESSED)
     attr_str[3] = 'C';

  if (ff->ff_attrib & FILE_ATTRIBUTE_ARCHIVE)
     attr_str[4] = 'A';

  if (ff->ff_attrib & FILE_ATTRIBUTE_TEMPORARY)
     attr_str[5] = 'T';

  if (is_dir)
     attr_str[6] = 'D';

  else if (ff->ff_attrib & FILE_ATTRIBUTE_DEVICE)
     attr_str[6] = '!';

  if (ff->ff_attrib & FILE_ATTRIBUTE_ENCRYPTED)
     attr_str[7] = 'E';

  if (ff->ff_attrib & FILE_ATTRIBUTE_INTEGRITY_STREAM)
     attr_str[8] = 'I';

  if (ff->ff_attrib & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)
     attr_str[9] = 'N';

  if (ff->ff_attrib & FILE_ATTRIBUTE_NO_SCRUB_DATA)
     attr_str[10] = 'n';

  if (ff->ff_attrib & FILE_ATTRIBUTE_VIRTUAL)
     attr_str[11] = 'V';

  if (is_junction)
  {
    char result [_MAX_PATH] = "??";
    char orig   [_MAX_PATH+5];
    BOOL rc = get_reparse_point (path, result, sizeof(result));

    if (rc)
    {
      snprintf (orig, sizeof(orig), " [%s\\]", path);
      orig_path = orig;
      path      = _fix_drive (result);
      total_reparse_points++;
    }
    attr_str[6] = 'J';
  }

  if (is_dir || is_junction)
  {
    total_dirs++;
    printf ("%2lu, %14s: %s %s\\%s\n", (unsigned long)recursion_level, "<N/A>", attr_str, path, orig_path);
  }
  else
  {
    /* to-do: figure out the allocation unit to show the total allocated size
     */
    total_size += ff->ff_fsize;
    total_files++;
    printf ("%2lu, %14s: %s %s\n", (unsigned long)recursion_level, qword_str(ff->ff_fsize), attr_str, path);
  }
  return (0);
}

static void do_glob (const char *spec)
{
  glob_t res;
  size_t cnt;
  int    rc;
  char **p;

  rc = glob (spec, glob_flags, callback, &res);
  if (rc != 0)
     printf ("glob() failed: %d\n", rc);
  else
    for (p = res.gl_pathv, cnt = 1; cnt <= res.gl_pathc; p++, cnt++)
    {
      char fp[_MAX_PATH];
      printf ("%2d: %s\n", (int)cnt, show_full_path ? _fix_path(*p,fp) : *p);
    }

  fflush (stdout);
  if (opt.debug >= 2)
  {
    printf ("Before globfree()\n");
    mem_report();
  }

  globfree (&res);

  if (opt.debug >= 2)
  {
    printf ("After globfree()\n");
    mem_report();
  }
}

static void do_glob_new (const char *spec)
{
  DWORD rc;
  char *dir, *p, buf[_MAX_PATH];
  glob_new_t res;

  res.gl_pathv = NULL;

  /* Must use fnmatch() for such wildcards.
   */
  if ((glob_flags & GLOB_RECURSIVE) || strpbrk(spec,"[]"))
  {
    global_spec = "*";
    orig_spec   = basename (spec);
    if (*orig_spec == '\0')
       orig_spec = "*";
  }
  else
  {
    orig_spec   = NULL;
    global_spec = basename (spec);
    if (*global_spec == '\0')
       global_spec = "*";
  }

  dir = _fix_drive (dirname(spec));

  /* I failed to make fnmatch() handle files with no extension if 'spec' ends
   * in a '.' Hence, use this hack with a 'dot_spec' global.
   */
  p = strchr (spec,'\0') - 1;
  if (*p == '.')
  {
    dot_spec = strcpy (buf, basename(spec));
    p = strchr (dot_spec, '\0');
    p[-1] = '\0';
  }
  else
    dot_spec = NULL;

  total_files = total_dirs = total_reparse_points = total_size = 0;
  recursion_level = num_ignored_errors = 0;

  TRACE (1, "dir: '%s', global_spec: '%s', orig_spec: '%s', dot_spec: '%s'\n",
         dir, global_spec, orig_spec, dot_spec);

  printf ("Depth         Size  Attr          Path\n"
          "-------------------------------------------"
          "-------------------------------------------\n");

  rc = glob_new2 (dir, ft_callback, &res);
  FREE (dir);

  if (rc)
     printf ("\nGetLastError(): %s\n", win_strerror(rc));
  else
  {
    const char *size_str = str_trim ((char*)get_file_size_str(total_size));

    printf ("\nglob_new: %lu, total_files: %s, ",
            (unsigned long)rc, qword_str(total_files));

    printf ("total_dirs: %s, total_size: %s ",
            qword_str(total_dirs), size_str);

    printf ("(%s), total_reparse_points: %" U64_FMT "\n",
            qword_str(total_size), total_reparse_points);
  }
  printf ("recursion_level: %lu, num_ignored_errors: %lu\n",
          (unsigned long)recursion_level, (unsigned long)num_ignored_errors);

  fflush (stdout);
  if (opt.debug >= 2)
  {
    printf ("Before globfree_new()\n");
    mem_report();
  }

  globfree_new (&res);

  if (opt.debug >= 2)
  {
    printf ("After globfree_new()\n");
    mem_report();
  }
}

/*
 * Syntax for a recursive glob() is e.g. "...\\*.c". Will search
 * for .c-files in current dir and in directories below it.
 */
int MS_CDECL main (int argc, char **argv)
{
  int ch, use_glob = 0;

  glob_flags = GLOB_NOSORT | GLOB_MARK;

  show_full_path = 0;
  global_slash = '\\';
  C_init();

  while ((ch = getopt(argc, argv, "dCfgruxh?")) != EOF)
     switch (ch)
     {
       case 'd':
            opt.debug++;
            break;
       case 'C':
            opt.case_sensitive = 1;
            break;
       case 'f':
            show_full_path = 1;
            break;
       case 'g':
            use_glob = 1;
            break;
       case 'r':
            glob_flags |= GLOB_RECURSIVE;
            break;
       case 'u':
            opt.show_unix_paths = 1;
            global_slash = opt.show_unix_paths = '/';
            break;
       case 'x':
            glob_flags |= GLOB_USE_EX;
            break;
       case '?':
       case 'h':
       default:
            usage();
     }

  argc -= optind;
  argv += optind;
  fn_flags = fnmatch_case (FNM_FLAG_NOESCAPE | FNM_FLAG_PATHNAME);

  if (argc-- < 1 || *argv == NULL)
     usage();

  use_glob ? do_glob(*argv) : do_glob_new(*argv);
  return (0);
}
#endif  /* WIN_GLOB_TEST */
