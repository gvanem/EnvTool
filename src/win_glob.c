/* Copyright (C) 2001 DJ Delorie, see COPYING.DJ for details */
/* Copyright (C) 1999 DJ Delorie, see COPYING.DJ for details */
/* Copyright (C) 1998 DJ Delorie, see COPYING.DJ for details */
/* Copyright (C) 1997 DJ Delorie, see COPYING.DJ for details */
/* Copyright (C) 1996 DJ Delorie, see COPYING.DJ for details */
/* Copyright (C) 1995 DJ Delorie, see COPYING.DJ for details */

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
#include "win_glob.h"

#define PATHBUF_LEN  2000
#define EOS          '\0'

struct ffblk {
       HANDLE    ff_handle;
       DWORD     ff_attrib;         /* Attributes, see constants above. */
       FILETIME  ff_time_create;
       FILETIME  ff_time_access;    /* always midnight local time */
       FILETIME  ff_time_write;
       uint64_t  ff_fsize;
       char      ff_name [_MAX_PATH];
     };

static int findfirst (const char *file_spec, struct ffblk *ffblk, int attrib)
{
  WIN32_FIND_DATA ff_data;
  HANDLE          handle = FindFirstFile (file_spec, &ff_data);

  if (handle == INVALID_HANDLE_VALUE)
  {
    DEBUGF (1, "\"%s\" not found.\n", file_spec);
    return (-1);
  }

  ffblk->ff_handle      = handle;
  ffblk->ff_attrib      = ff_data.dwFileAttributes;
  ffblk->ff_time_create = ff_data.ftCreationTime;
  ffblk->ff_time_access = ff_data.ftLastAccessTime;
  ffblk->ff_time_write  = ff_data.ftLastWriteTime;
  ffblk->ff_fsize       = ((uint64_t)ff_data.nFileSizeHigh << 32) + ff_data.nFileSizeLow;

  _strlcpy (ffblk->ff_name, ff_data.cFileName, sizeof(ffblk->ff_name));
  ARGSUSED (attrib);
  return (0);
}

static int findnext (struct ffblk *ffblk)
{
  WIN32_FIND_DATA ff_data;
  DWORD           rc = FindNextFile (ffblk->ff_handle, &ff_data);

  if (!rc)
  {
    FindClose (ffblk->ff_handle);
    ffblk->ff_handle = NULL;
  }
  else
  {
    ffblk->ff_attrib = ff_data.dwFileAttributes;
    _strlcpy (ffblk->ff_name, ff_data.cFileName, sizeof(ffblk->ff_name));
  }
  return (rc ? 0 : 1);
}

typedef struct Save {
        struct Save *prev;
        char        *entry;
      } Save;

static Save *save_list;
static int   save_count;
static int   flags;
static int (*errfunc)(const char *epath, int eerno);
static char *pathbuf, *pathbuf_end;
static int   wildcard_nesting;
static char  use_lfn = 1;
static char  slash;

static int glob2 (const char *pattern, char *epathbuf);
static int add (const char *path, unsigned line);
static int str_compare (const void *va, const void *vb);

/* `tolower' might depend on the locale.  We don't want to.
 */
static int msdos_tolower_fname (int c)
{
  return (c >= 'A' && c <= 'Z') ? c + 'a' - 'A' : c;
}

static int add (const char *path, unsigned line)
{
  Save *sp;

  for (sp = save_list; sp; sp = sp->prev)
    if (!stricmp(sp->entry, path))
      return (0);

  sp = CALLOC (sizeof(*sp), 1);
  if (!sp)
     return (1);

  sp->entry = STRDUP (path);
  if (!sp->entry)
  {
    FREE (sp);
    return (1);
  }
  DEBUGF (1, "add: `%s' (from line %u)\n", sp->entry, line);

  sp->prev = save_list;
  save_list = sp;
  save_count++;
  return (0);
}

static int glob_dirs (const char *rest, char *epathbuf,
                      int first)  /* rest is ptr to null or ptr after slash, bp after slash */
{
  struct ffblk ff;
  int    done;

  DEBUGF (1, "glob_dirs[%d]: rest=`%s' %c epathbuf=`%s' %c pathbuf=`%s'\n",
             wildcard_nesting, rest, *rest, epathbuf, *epathbuf, pathbuf);

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
      DEBUGF (1, "end, checking `%s'\n", pathbuf);
      if (epathbuf == pathbuf)
      {
        epathbuf[0] = '.';
        epathbuf[1] = '\0';
      }
      else
        epathbuf[-1] = '\0';

      if (FILE_EXISTS(pathbuf))
         if (add(pathbuf,__LINE__))
            return (GLOB_NOSPACE);
      epathbuf[-1] = sl;
    }
  }

  strcpy (epathbuf, "*.*");
  done = findfirst (pathbuf, &ff, FILE_ATTRIBUTE_DIRECTORY);

  while (!done)
  {
    if ((ff.ff_attrib & FILE_ATTRIBUTE_DIRECTORY) &&
        (strcmp(ff.ff_name, ".") && strcmp(ff.ff_name, "..")))
    {
      char *tp;

      DEBUGF (1, "found `%s' `%s'\n", pathbuf, ff.ff_name);

      strcpy (epathbuf, ff.ff_name);
      tp = strchr (epathbuf, '\0');
      *tp++ = slash;
      *tp = '\0';

      wildcard_nesting++;
      if (*rest)
      {
        if (glob2(rest, tp) == GLOB_NOSPACE)
           return (GLOB_NOSPACE);
      }
      else
      {
        if (!(flags & GLOB_MARK))
           tp[-1] = '\0';
        if (add(pathbuf,__LINE__))
           return (GLOB_NOSPACE);
        tp[-1] = slash;
      }
      *tp = '\0';
      if (glob_dirs(rest, tp, 0) == GLOB_NOSPACE)
         return (GLOB_NOSPACE);
      wildcard_nesting--;
    }
    done = findnext(&ff);
  }
  return (0);
}

static int glob2 (const char *pattern, char *epathbuf)  /* both point *after* the slash */
{
  struct ffblk ff;
  const char *pp, *pslash;
  char *bp, *my_pattern;
  int   done, attr;

  if (strcmp(pattern, "...") == 0)
     return glob_dirs (pattern+3, epathbuf, 1);

  if (strncmp(pattern, "...", 3) == 0 && (pattern[3] == '\\' || pattern[3] == '/'))
  {
    slash = pattern[3];
    return glob_dirs (pattern+4, epathbuf, 1);
  }

  *epathbuf = '\0';

  /* copy as many non-wildcard segments as possible */
  pp = pattern;
  bp = epathbuf;
  pslash = bp - 1;

  while (bp < pathbuf_end)
  {
    if (*pp == ':' || *pp == '\\' || *pp == '/')
    {
      pslash = bp;
      if (strcmp(pp+1, "...") == 0 ||
          (strncmp(pp+1, "...", 3) == 0 && (pp[4] == '/' || pp[4] == '\\')))
      {
        if (*pp != ':')
           slash = *pp;
        DEBUGF (1, "glob2: dots at `%s'\n", pp);
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

  DEBUGF (1, "glob2: pp: `%s'\n", pp);

  if (*pp == 0) /* end of pattern? */
  {
    if (FILE_EXISTS(pathbuf))
    {
      if (flags & GLOB_MARK)
      {
        struct ffblk _ff;

        attr = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM |
               FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ARCHIVE;

        findfirst (pathbuf, &_ff, attr);
        if (_ff.ff_attrib & FILE_ATTRIBUTE_DIRECTORY)
        {
          char *_pathbuf = pathbuf + strlen(pathbuf);
          *_pathbuf++ = '/';
          *_pathbuf = 0;
        }
      }
      if (add(pathbuf,__LINE__))
         return (GLOB_NOSPACE);
    }
    return (0);
  }

  DEBUGF (1, "glob2(): initial segment is `%s', wildcard_nesting: %d\n",
             pathbuf, wildcard_nesting);

  if (wildcard_nesting)
  {
    char s = bp[-1];

    bp[-1] = 0;
    if (!FILE_EXISTS(pathbuf))
       return (0);
    bp[-1] = s;
  }

  for (pslash = pp; *pslash && *pslash != '\\' && *pslash != '/';  pslash++)
      ;

  if (*pslash)
     slash = *pslash;

  my_pattern = alloca (pslash - pp + 1);
  _strlcpy (my_pattern, pp, pslash - pp + 1);

  DEBUGF (1, "glob2: `%s' `%s'\n", pathbuf, my_pattern);

  if (strcmp(my_pattern, "...") == 0)
  {
    if (glob_dirs(*pslash ? pslash+1 : pslash, bp, 1) == GLOB_NOSPACE)
       return (GLOB_NOSPACE);
    return (0);
  }

  strcpy (bp, "*.*");

  attr = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM |
         FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ARCHIVE;
  done = findfirst (pathbuf, &ff, attr);

  while (!done)
  {
    if ((ff.ff_attrib & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (strcmp(ff.ff_name, ".") && strcmp(ff.ff_name, "..")))
    {
      if (fnmatch(my_pattern, ff.ff_name,
                  FNM_FLAG_NOESCAPE | FNM_FLAG_PATHNAME |
                  FNM_FLAG_NOCASE) == FNM_MATCH)
      {
        strcpy (bp, ff.ff_name);
        if (*pslash)
        {
          char *tp = bp + strlen(bp);

          *tp++ = *pslash;
          *tp = 0;
          DEBUGF (1, "nest: `%s' `%s'\n", pslash+1, pathbuf);
          wildcard_nesting++;
          if (glob2(pslash+1, tp) == GLOB_NOSPACE)
             return (GLOB_NOSPACE);
          wildcard_nesting--;
        }
        else
        {
          DEBUGF (1, "ffmatch: `%s' matching `%s', add `%s'\n",
                     ff.ff_name, my_pattern, pathbuf);
          if ((ff.ff_attrib & FILE_ATTRIBUTE_DIRECTORY) && (flags & GLOB_MARK))
          {
            size_t len = strlen (bp);
            bp [len+1] = '\0';
            bp [len] = slash;
          }
          if (add(pathbuf,__LINE__))
             return (GLOB_NOSPACE);
        }
      }
    }
    done = findnext (&ff);
  }
  return (0);
}

static int str_compare (const void *va, const void *vb)
{
  return stricmp (*(const char * const *)va, *(const char * const *)vb);
}

int glob (const char *_pattern, int _flags,
          int (*_errfunc)(const char *_epath, int _eerrno),
          glob_t *_pglob)
{
  char path_buffer [PATHBUF_LEN + 1];
  int  l_ofs, l_ptr;

  pathbuf = path_buffer + 1;
  pathbuf_end = path_buffer + PATHBUF_LEN;
  flags = _flags;
  errfunc = _errfunc;
  wildcard_nesting = 0;
  save_count = 0;
  save_list = 0;
  use_lfn = 1;
  slash = '/';

  if (!(_flags & GLOB_APPEND))
  {
    _pglob->gl_pathc = 0;
    _pglob->gl_pathv = NULL;
    if (!(flags & GLOB_DOOFFS))
       _pglob->gl_offs = 0;
  }

  if (glob2(_pattern, pathbuf) == GLOB_NOSPACE)
     return (GLOB_NOSPACE);

  if (save_count == 0)
  {
    if (flags & GLOB_NOCHECK)
    {
      if (add(_pattern,__LINE__))
         return (GLOB_NOSPACE);
    }
    else
      return (GLOB_NOMATCH);
  }

  if (flags & GLOB_DOOFFS)
       l_ofs = _pglob->gl_offs;
  else l_ofs = 0;

  if (flags & GLOB_APPEND)
  {
    _pglob->gl_pathv = REALLOC (_pglob->gl_pathv, (l_ofs + _pglob->gl_pathc + save_count + 1) * sizeof(char *));
    if (!_pglob->gl_pathv)
       return (GLOB_NOSPACE);
    l_ptr = l_ofs + _pglob->gl_pathc;
  }
  else
  {
    _pglob->gl_pathv = CALLOC (l_ofs + save_count + 1, sizeof(char *));
    if (!_pglob->gl_pathv)
       return (GLOB_NOSPACE);
    l_ptr = l_ofs;
  }

  l_ptr += save_count;
  _pglob->gl_pathv[l_ptr] = NULL;

  while (save_list)
  {
    Save *s = save_list;

    l_ptr --;
    _pglob->gl_pathv[l_ptr] = save_list->entry;
    save_list = save_list->prev;
    FREE (s);
  }

  if (!(flags & GLOB_NOSORT))
     qsort (_pglob->gl_pathv + l_ptr, save_count, sizeof(char *), str_compare);

  _pglob->gl_pathc = l_ptr + save_count;

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

#if defined(TEST)

int debug = 0;
int show_unix_paths = 0;
int color = 0;

/* Tell MingW's CRT to turn off command line globbing by default.
 */
int _CRT_glob = 0;

/* MinGW-64's CRT seems to NOT glob the cmd-line by default.
 * Hence this doesn't change that behaviour.
 */
int _dowildcard = 0;

char *getenv_expand (const char *var)
{
  char *e = getenv (var);

  if (e)
     return STRDUP (e);
  return (NULL);
}

void usage (void)
{
  printf ("Usage: win_glob [-d] <file_spec>\n");
  exit (-1);
}

int main (int argc, char **argv)
{
  glob_t res;
  size_t cnt;
  int    rc;
  char  **p;

  if (argc < 2)
     usage();

  if (!strcmp(argv[1],"-d"))
    debug++, argv++;

  rc = glob (argv[1], GLOB_NOSORT | GLOB_MARK | GLOB_NOCHECK, NULL, &res);
  if (rc != 0)
    printf ("glob() failed: %d\n", rc);
  else
    for (p = res.gl_pathv, cnt = 1; cnt <= res.gl_pathc; p++, cnt++)
    {
      char fp[_MAX_PATH];
      printf ("%2d: %s -> %s\n", cnt, *p, _fixpath(*p,fp));
    }

  if (debug > 0)
  {
    printf ("Before globfree()\n");
    mem_report();
  }

  globfree (&res);

  if (debug > 0)
  {
    printf ("After globfree()\n");
    mem_report();
  }

  return (0);
}
#endif
