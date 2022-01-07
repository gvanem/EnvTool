/** \file    searchpath.c
 *  \ingroup Misc
 *  \brief
 *    Find a file along an envirnment variable. (Usually the `%PATH%`).
 */

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
 * Return the last position in `env_var` for last successfull
 * `searchpath()` call.
 */
int searchpath_pos (void)
{
  return (last_pos);
}

/**
 * Search `%env_var` for the first `file` (not a `file_spec`).
 *
 * If successful, store the full pathname in static buffer and return a
 * pointer to it. If not successful, return `NULL`.
 *
 * This is what the Borland `searchpath()` library function does.
 *
 * \note
 * if `env_var` is just a directory name, the `file` is just tested for
 * presence in that directory name.
 *   \eg
 *   \code
 *     searchpath ("SWAPFILE.SYS", "c:\\")
 *   \endcode
 * would simply return `C:\\SWAPFILE.SYS`.
 */
static char *_searchpath (const char *file, const char *env_var, char *found)
{
  char       *env, *path, *_path, *end, *tok;
  const char *env_dir = NULL;
  char       *_file = alloca (strlen(file)+1);

  last_pos = -1;

  if (!file || !*file)
  {
    TRACE (1, "given a bogus 'file': '%s'\n", file);
    errno = EINVAL;
    return (NULL);
  }

  if (!strncmp(file, "\\\\.\\", 4))
  {
    TRACE (1, "Not handling UNC-names: '%s'\n", file);
    errno = EINVAL;
    return (NULL);
  }

  if (!env_var || !*env_var)
  {
    TRACE (1, "given a bogus 'env_var'\n");
    errno = EINVAL;
    return (NULL);
  }

  init_misc();

  strcpy (_file, file);
  str_unquote (_file);     /* remove quotes around a long filename */

  found[0] = '\0';

  env = getenv_expand (env_var);
  if (!env)
  {
    path = MALLOC (strlen(env_var) + 3);   /* Room for `.;env_var` */
    if (is_directory(env_var))             /* Given env_var is "c:\\" */
       env_dir = env_var;
  }
  else
    path = MALLOC (strlen(env) + 3);       /* Room for `.;%env_var%` */

  if (!path)
  {
    TRACE (1, "calloc() failed");
    FREE (env);
    errno = ENOMEM;
    return (NULL);
  }

  if ((env && !(env[0] == '.' && env[1] == ';')) || !env)
  {
    path[0] = '.';
    path[1] = ';';
    path[2] = '\0';
    _path = path + 2;
  }
  else
    _path = path;

  if (env)
     strcpy (_path, env);
  else if (env_dir)
     strcpy (_path, env_dir);

  TRACE (2, "Looking for _file: '%s' in path: '%s'\n", _file, path);

  tok = _strtok_r (path, ";", &end);
  while (tok)
  {
    last_pos++;

    snprintf (found, _MAX_PATH, "%s\\%s", str_unquote(tok), _file);
    if (FILE_EXISTS(found))
       goto was_found;

    tok = _strtok_r (NULL, ";", &end);
  }

  FREE (env);
  FREE (path);

  last_pos = -1;
  errno = ENOENT;
  return (NULL);

was_found:
  FREE (env);
  FREE (path);
  return _fix_path (found, found);
}

/**
 * The public interface to this module.
 * \param[in] file    the file to search for in an environment variable.
 * \param[in] env_var the name of the environment variable (e.g. `PATH`).
 *
 * \note If `file` is found, this function returns a static buffer.
 */
char *searchpath (const char *file, const char *env_var)
{
  static char found [_MAX_PATH];
  char       *s = _searchpath (file, env_var, found);

  if (s)
     return slashify2 (found, found, opt.show_unix_paths ? '/' : '\\');
  return (NULL);
}

/*
 * Not used any more.
 */
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

  if (strchr("+,; =[]", c))
    return (0);                          /* special non-DOS characters */
  return (1);                            /* all chars OK */
}
