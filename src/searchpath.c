/** \file    searchpath.c
 *  \ingroup Misc
 *  \brief
 *    Find a file along an envirnment variable. (Usually the \c \%PATH\%).
 */

/* Copyright (C) 2001 DJ Delorie, see COPYING.DJ for details */
/* Copyright (C) 1999 DJ Delorie, see COPYING.DJ for details */
/* Copyright (C) 1996 DJ Delorie, see COPYING.DJ for details */
/* Copyright (C) 1995 DJ Delorie, see COPYING.DJ for details */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <fcntl.h>

#include "envtool.h"

static int last_pos = -1;

/**
 * Return the last position in 'env_var' for last successfull
 * 'searchpath()' call.
 */
int searchpath_pos (void)
{
  return (last_pos);
}

/**
 * Search \c %env_var for the first 'file' (not a \c file_spec).
 * If successful, store the full pathname in static buffer and return a
 * pointer to it. If not sucessful, return NULL.
 * This is what the Borland \c searchpath() library function does.
 *
 * \note: if \c env_var is just a dirname, the \c file is just tested for
 *        presence in current directory and that dirname.
 *        \eg
 *        \code{c}
 *          searchpath ("SWAPFILE.SYS", "c:\\")
 *        \endcode would simply return \c \"C:\\SWAPFILE.SYS\".
 */
static char *searchpath_internal (const char *file, const char *env_var, char *found)
{
  char   *p, *path, *test_dir;
  size_t  alloc;
  int     save_debug = opt.debug;

  last_pos = -1;

  if (!file || !*file)
  {
    DEBUGF (1, "given a bogus 'file': '%s'\n", file);
    errno = EINVAL;
    return (NULL);
  }
  if (!strncmp(file,"\\\\.\\",4))
  {
    DEBUGF (1, "Not handling UNC-names: '%s'\n", file);
    errno = EINVAL;
    return (NULL);
  }

  if (!env_var || !*env_var)
  {
    DEBUGF (1, "given a bogus 'env_var'\n");
    errno = EINVAL;
    return (NULL);
  }

  init_misc();

  found[0] = '\0';
  opt.debug = 0;
  p = getenv_expand (env_var);
  opt.debug = save_debug;

  if (!p)
       alloc = 2;              /* Room for '.' */
  else alloc = strlen(p) + 3;  /* Room for '.;%env_var' */

  path = CALLOC (alloc, 1);
  if (!path)
  {
    DEBUGF (1, "calloc() failed");
    FREE (p);
    errno = ENOMEM;
    return (NULL);
  }

  /* Prepend `.' to the $(env_var), so current directory
   * is always searched first.
   */
  path[0] = '.';

  /* If the env-var has a value, search along ".;%env_val".
   */
  if (p)
  {
    char *s, *name_start = 0;
    int   preserve_case = 1;

    path[1] = ';';
    strcpy (path + 2, p);

    /* switch FOO\BAR to foo/bar, downcase where appropriate.
     */
    for (s = path + 2, name_start = s; *name_start; s++)
    {
      char lname [FILENAME_MAX];

      if (s == name_start)
         continue;

      if (*s == ':')
         name_start = s + 1;
      else if (!preserve_case && (*s == '/' || *s == ';' || *s == '\0'))
      {
        memcpy (lname, name_start+1, s-name_start-1);
        lname [s-name_start-1] = '\0';
        if (_is_DOS83(lname))
        {
          name_start++;
          while (name_start < s)
          {
            if (*name_start >= 'A' && *name_start <= 'Z')
              *name_start += 'a' - 'A';
            name_start++;
          }
        }
        else
          name_start = s;
      }
      else if (*s == '\0')
        break;
    }
  }

  /* If the file name includes slashes or the drive letter, maybe they
   * already have a good name.
   */
  if (strpbrk(file, "/\\:") != 0 && FILE_EXISTS(file))
  {
    if (IS_SLASH(file[0]) || file[1] == ':' ||
        ((file[0] == '.' && IS_SLASH(file[1])) ||
         (file[1] == '.' && IS_SLASH(file[2]))) )
    {
      /* Either absolute file name or it begins with a "./".
       */
      strcpy (found, file);
    }
    else
    {
      /* Relative file name: add ".\\".
       */
      strcpy (found, ".\\");
      strcat (found, file);
    }
    FREE (p);
    FREE (path);
    last_pos = 0;

    if (strstr(found,".."))
       _fix_path (found, found);
    return (found);
  }

  test_dir = path;
  last_pos = 0;     /* Assume it will be found in 1st position */

  do
  {
    char *dp = strchr (test_dir, ';');

    if (!dp)
       dp = test_dir + strlen (test_dir);

    if (dp == test_dir)
       strcpy (found, file);
    else
    {
      strncpy (found, test_dir, dp - test_dir);
      found [dp-test_dir] = '\\';
      strcpy (found + (dp - test_dir) + 1, file);
    }

    if (FILE_EXISTS(found))
    {
      FREE (p);
      FREE (path);
      if (strstr(found,".."))
         _fix_path (found, found);
      return (found);
    }

    if (*dp == 0)   /* We have reached the end of "%env_val" */
       break;
    test_dir = dp + 1;
    last_pos++;
  }
  while (*test_dir);

  FREE (p);
  FREE (path);

  /* FIXME: perhaps now that we failed to find it, we should try the
   * basename alone, like BC does?  But let somebody complain about
   * this first... ;-)
   */
  last_pos = -1;
  errno = ENOENT;
  return (NULL);
}

/**
 * \note If 'file' is found, this function returns a static buffer
 *       in 'slashify()'.
 */
char *searchpath (const char *file, const char *env_var)
{
  char found [_MAX_PATH];
  char *s = searchpath_internal (file, env_var, found);

  if (s)
     return slashify (s, opt.show_unix_paths ? '/' : '\\');
  return (NULL);
}

int _is_DOS83 (const char *fname)
{
  const char *s = fname;
  const char *e;
  char  c, period_seen = 0;

  if (*s == '.')
  {
    if (s[1] == 0)
       return (1);                       /* "." is valid */
    if (s[1] == '.' && s[2] == 0)
       return (1);                       /* ".." is valid */
    return (0);                          /* starting period invalid */
  }

  e = s + 8;                             /* end */

  while ((c = *s++) != 0)
    if (c == '.')
    {
      if (period_seen)
         return (0);                     /* multiple periods invalid */
      period_seen = 1;
      e = s + 3;                         /* already one past period */
    }
    else if (s > e)
      return (0);                        /* name component too long */

  if (c >= 'a' && c <= 'z')
    return (0);                          /* lower case character */

  if (c == '+' || c == ',' ||
      c == ';' || c == ' ' ||
      c == '=' || c == '[' || c == ']')
    return (0);                          /* special non-DOS characters */
  return (1);                            /* all chars OK */
}

