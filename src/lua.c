/** \file    lua.c
 *  \ingroup Lua
 *
 *  \brief The `--lua` mode search functions.
 */
#include "envtool.h"
#include "color.h"
#include "dirlist.h"
#include "lua.h"

/**
 * \struct lua_dir
 *
 * The path-elements from `LUA_PATH` or `LUA_CPATH`
 * is kept here. Add needed information like pattern.
 */
struct lua_dir {
       char  path [_MAX_PATH];  /**< The `LUA_PATH` or `LUA_CPATH` path-element */
       char  pattern [20];      /**< The pattern for this path; `"?.lua"` or `"?.dll"` */
       BOOL  is_cwd;            /**< This directory == current_dir */
       BOOL  is_CPATH;          /**< This directory is from `LUA_CPATH` */
     };

/** A `smartlist_t` of 'struct lua_dir'
 */
static smartlist_t *lua_dirs = NULL;

static BOOL lua_append_dir (const struct directory_array *dir, BOOL for_LUA_CPATH)
{
  const char     *env_var = for_LUA_CPATH ? "LUA_CPATH" : "LUA_PATH";
  struct lua_dir  lua;
  struct lua_dir *copy;
  char  *p;
  int    i, max;

  memset (&lua, '\0', sizeof(lua));
  _strlcpy (lua.path, dir->dir, sizeof(lua.path));

  p = strrchr (lua.path, '?');
  if (p && p > lua.path)
  {
    lua.pattern[0] = '*';
    _strlcpy (lua.pattern+1, p+1, sizeof(lua.pattern)-1);
    p[-1] = '\0';
  }
  else if (p && p == lua.path)
  {
    lua.pattern[0] = '*';
    _strlcpy (lua.pattern+1, p+1, sizeof(lua.pattern)-1);
    _strlcpy (lua.path, current_dir, sizeof(lua.path));
  }
  else if (dir->is_cwd)
  {
    _strlcpy (lua.pattern, for_LUA_CPATH ? "*.dll" : "*.lua", sizeof(lua.pattern));
  }
  else
  {
    WARN ("%s: path-element \"%s\" has no \"%s\" pattern\n",
          env_var, dir->dir, for_LUA_CPATH ? "?.dll" : "?.lua");
    return (FALSE);
  }

  lua.is_cwd   = dir->is_cwd;
  lua.is_CPATH = for_LUA_CPATH;

  max = smartlist_len (lua_dirs);

  /* Check if we already have this path.
   */
  for (i = 0; i < max; i++)
  {
    struct lua_dir *prev_dir = smartlist_get (lua_dirs, i);

    if (prev_dir->is_CPATH == for_LUA_CPATH && str_equal(prev_dir->path, lua.path))
    {
      TRACE (2, "Already have dir '%s' at %d for '%s'\n",
             prev_dir->path, i, prev_dir->is_CPATH ? "LUA_CPATH" : "LUA_PATH");
      return (FALSE); /* Nothing more to do here */
    }
  }
  copy = MALLOC (sizeof(*copy));
  *copy = lua;
  smartlist_add (lua_dirs, copy);
  return (TRUE);
}

/**
 * Handle one Lua env-var and append it's components to `lua_dirs`.
 */
static void lua_handle_var (const  char *env_var, BOOL for_LUA_CPATH)
{
  const char  *value = getenv (env_var);
  const struct directory_array *dir;
  smartlist_t *dirs;
  int          i, max;

  if (!value)
  {
    WARN ("%s not defined in the environment.\n", env_var);
    return;
  }

  opt.lua_mode = TRUE;
  dirs = split_env_var (env_var, value);
  max  = dirs ? smartlist_len (dirs) : 0;
  for (i = 0; i < max; i++)
  {
    dir = smartlist_get (dirs, i);
    lua_append_dir (dir, for_LUA_CPATH);
  }

  dir_array_free();
  opt.lua_mode = FALSE;
}

/**
 * Dump the `lua_dirs` smartlist array.
 * Called if `opt.debug >= 1`
 */
static void lua_dump_dirs (void)
{
  int i, max = smartlist_len (lua_dirs);

  C_printf ("Num  env-var    CWD  pattern  path\n%s\n", str_repeat('=', 90));
  for (i = 0; i < max; i++)
  {
    const struct lua_dir *dir = smartlist_get (lua_dirs, i);

    C_printf ("%2d:  %-9s  %d    %-5.5s    %s\n",
              i, dir->is_CPATH ? "LUA_CPATH" : "LUA_PATH",
              dir->is_cwd, dir->pattern, dir->path);
  }
}

/**
 * Initialise this module. Only once.
 */
void lua_init (void)
{
  if (lua_dirs)
     return;

  lua_dirs = smartlist_new();

  lua_handle_var ("LUA_PATH", FALSE);
  lua_handle_var ("LUA_CPATH", TRUE);

  if (opt.debug >= 1)
     lua_dump_dirs();
}

/**
 * Called from `cleanup()` to free memory allocated here.
 */
void lua_exit (void)
{
  smartlist_free_all (lua_dirs);
  lua_dirs = NULL;
}

/**
 * Check and print the name of the needed init-function exported from the `dll_file`.
 *
 * E.g. if `dll_file == ssl.dll`, check that's it is exporting one of these:
 *  \li `luaopen_ssl()`    - for plain old Lua 5.x
 *  \li `LuaJIT_BC_ssl()`  - for LuaJIT
 *
 * \note
 *  MSDN warns against using the `DONT_RESOLVE_DLL_REFERENCES` flag.
 *  But that the only way that `GetProcAddress()` will work.
 */
static void lua_print_exports (const char *dll_file, const char *filler)
{
  char    base [50], *p;
  char    symbol1 [60];
  char    symbol2 [60];
  int     len_diff;
  HANDLE  dll_hnd;
  FARPROC func1, func2;

  p = strrchr (dll_file, '\0');
  if (p[-4] != '.')
     return;

  set_error_mode (0);
  dll_hnd = LoadLibraryEx (dll_file, NULL, DONT_RESOLVE_DLL_REFERENCES);
  set_error_mode (1);

  if (!dll_hnd)
  {
    TRACE (1, "Failed to load %s; %s\n", dll_file, win_strerror(GetLastError()));
    return;
  }

  _strlcpy (base, basename(dll_file), sizeof(base));
  p = strrchr (base, '.');
  *p = '\0';

  snprintf (symbol1, sizeof(symbol1), "luaopen_%s", base);
  func1 = GetProcAddress (dll_hnd, symbol1);
  TRACE (1, "dll_file: %s, symbol1: %s -> 0x%p\n", dll_file, symbol1, func1);

  snprintf (symbol2, sizeof(symbol2), "LuaJIT_BC_%s", base);
  func2 = GetProcAddress (dll_hnd, symbol2);
  TRACE (1, "dll_file: %s, symbol2: %s -> 0x%p\n", dll_file, symbol2, func2);

  FreeLibrary (dll_hnd);

  if (func1 || func2)
  {
    len_diff = (int) (strlen(symbol2) - strlen(symbol1));
    C_printf ("%sexports: %s: %*s%s~0\n"
              "%s         %s: %s~0\n",
              filler, symbol1, len_diff, "", func1 ? "~2Yes" : "~5No",
              filler, symbol2,               func2 ? "~2Yes" : "~5No");
  }
}

/**
 * Search and check along `lua_dirs` for a match to `<filespec>`.
 * For each directory, match a `?.lua` or `?.dll` against the `<filespec>`.
 */
static int lua_search_internal (const char *search_spec, BOOL is_CPATH)
{
  char env_name [30];
  int  i, i_max, found = 0;
  int  j, j_max;

  _strlcpy (env_name, is_CPATH ? "LUA_CPATH" : "LUA_PATH", sizeof(env_name));
  report_header_set ("Matches in %%%s:\n", env_name);

  i_max = smartlist_len (lua_dirs);
  for (i = 0; i < i_max; i++)
  {
    struct lua_dir *dir = smartlist_get (lua_dirs, i);
    smartlist_t    *dirlist;

    if (dir->is_CPATH != is_CPATH)
       continue;

    dirlist = get_matching_files (dir->path, dir->pattern);
    j_max = dirlist ? smartlist_len (dirlist) : 0;

    for (j = 0; j < j_max; j++)
    {
      struct dirent2 *de = smartlist_get (dirlist, j);
      int    match = fnmatch (search_spec, basename(de->d_name), fnmatch_case(0));

      TRACE (2, "%s: Testing '%s' against '%s'; match: %s\n",
             env_name, de->d_name, search_spec, fnmatch_res(match));

      if (match == FNM_MATCH)
      {
        struct report r;

        memset (&r, '\0', sizeof(r));
        r.file  = de->d_name;
        r.fsize = de->d_fsize;
        r.mtime = FILETIME_to_time_t (&de->d_time_write);
        r.key   = dir->is_CPATH ? HKEY_LUA_DLL : HKEY_LUA_FILE;
        if (report_file(&r))
        {
          found++;
          if (opt.PE_check && opt.verbose >= 0 && dir->is_CPATH)
             lua_print_exports (de->d_name, r.filler);
       }
      }
      FREE (de->d_name);
      FREE (de);
    }
    smartlist_free (dirlist);
  }
  return (found);
}

int lua_search (const char *search_spec)
{
  return lua_search_internal (search_spec, FALSE) +
         lua_search_internal (search_spec, TRUE);
}
