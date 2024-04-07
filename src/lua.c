/** \file    lua.c
 *  \ingroup Lua
 *
 *  \brief The `--lua` mode search functions.
 */
#include "envtool.h"
#include "color.h"
#include "cache.h"
#include "dirlist.h"
#include "ignore.h"
#include "lua.h"

/**
 * \struct lua_dir
 *
 * The path-elements from `LUA_PATH` or `LUA_CPATH`
 * is kept here. Add needed information like pattern.
 */
typedef struct lua_dir {
        char   path [_MAX_PATH];  /**< The `LUA_PATH` or `LUA_CPATH` path-element */
        char   pattern [20];      /**< The pattern for this path; `"?.lua"` or `"?.dll"` */
        bool   is_cwd;            /**< This directory == current_dir */
        bool   is_CPATH;          /**< This directory is from `LUA_CPATH` */
        bool   exist;             /**< Does it exist? */
      } lua_dir;

/**
 * The global data for 'lua.exe' (or `luajit.exe`) found on `PATH`.
 */
static char           *lua_exe;
static struct ver_info lua_ver;
static int    prefer_luajit = 0;
static int    check_mode    = 0;

/** A `smartlist_t` of 'lua_dir'
 */
static smartlist_t *lua_dirs = NULL;

typedef struct dirent2 dirent2;

/**
 * Check recursively if a LUA directory is empty;
 * no `*.lua` or `*.dll` files.
 *
 * \note
 *  It is quite normal that e.g. `"%LUA_PATH"` contain a directory with
 *  no .lua-files but at least 1 subdirectory with .lua-files.
 */
static unsigned lua_count_files (const char *path, const char *lua_spec)
{
  struct od2x_options dir_opt;
  DIR2               *dp;
  dirent2            *de;
  unsigned            num_ignored = 0;
  unsigned            num_files = 0;

  memset (&dir_opt, '\0', sizeof(dir_opt));
  dir_opt.pattern = "*.*";

  dp = opendir2x (path, &dir_opt);
  if (!dp)
     return (0);

  while ((de = readdir2(dp)) != NULL)
  {
    if ((de->d_attrib == INVALID_FILE_ATTRIBUTES) ||
        (de->d_attrib & FILE_ATTRIBUTE_DEVICE))
       continue;

    if (cfg_ignore_lookup("[Lua]", de->d_name) ||
        cfg_ignore_lookup("[Lua]", basename(de->d_name)))
    {
      TRACE (2, "ignoring entry: '%s'.\n", de->d_name);
      num_ignored++;
      continue;
    }

    if (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY)
    {
      TRACE (2, "checking sub-dir: '%s'.\n", de->d_name);
      num_files += lua_count_files (de->d_name, lua_spec);
    }
    else if (fnmatch(lua_spec, de->d_name, FNM_FLAG_NOCASE) == FNM_MATCH)
    {
      num_files++;
    }
  }
  closedir2 (dp);

  if (num_files > 0)
     TRACE (2, "Found %u LUA-files (%s) under '%s'. num_ignored: %u.\n",
            num_files, lua_spec, path, num_ignored);
  return (num_files);
}

/**
 * Parse one component from either `LUA_PATH` or `LUA_CPATH` and append
 * to `lua_dirs`.
 */
static void lua_append_dir (const directory_array *dir, bool for_LUA_CPATH)
{
  const char *env_var     = for_LUA_CPATH ? "LUA_CPATH" : "LUA_PATH";
  const char *lua_pattern = for_LUA_CPATH ? "?.dll"     : "?.lua";
  const char *lua_spec    = for_LUA_CPATH ? "*.dll"     : "*.lua";
  lua_dir    lua;
  DWORD      attr;
  char      *p;
  int        i, add_it, max;

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
    _strlcpy (lua.pattern, lua_spec, sizeof(lua.pattern));
  }
  else if (!check_mode)
  {
    WARN ("%s: path-element \"%s\" has no \"%s\" pattern\n",
          env_var, dir->dir, lua_pattern);
    return;
  }

  attr = GetFileAttributes (lua.path);
  lua.exist    = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
  lua.is_cwd   = dir->is_cwd;
  lua.is_CPATH = for_LUA_CPATH;

  max = smartlist_len (lua_dirs);
  add_it = 1;

  /* Check if we already have this path.
   */
  for (i = 0; i < max; i++)
  {
    const lua_dir *prev_dir = smartlist_get (lua_dirs, i);

    if (prev_dir->is_CPATH == for_LUA_CPATH &&
        str_equal(prev_dir->path, lua.path))
    {
      TRACE (2, "Already have dir '%s' at %d for '%s'\n",
             prev_dir->path, i, prev_dir->is_CPATH ? "LUA_CPATH" : "LUA_PATH");
      add_it = 0;    /* Nothing more to do here */
      break;
    }
  }

  if (!lua.is_cwd && lua_count_files(lua.path, lua_spec) == 0)
  {
    lua.exist = false;
    if (!check_mode)
    {
      WARN ("Directory '%s' has no '%s' files for '%s'\n", lua.path, lua_spec, env_var);
      add_it = 0;   /* No point adding empty LUA-dirs */
    }
  }

  if (add_it)
  {
    lua_dir *copy = MALLOC (sizeof(*copy));

    *copy = lua;
    smartlist_add (lua_dirs, copy);
  }
}

/**
 * Handle one Lua env-var and append it's components to `lua_dirs`.
 */
static void lua_handle_var (const char *env_var, bool for_LUA_CPATH)
{
  const char  *value = getenv (env_var);
  smartlist_t *dirs;
  int          i, max;

  if (!value)
  {
    WARN ("%s not defined in the environment.\n", env_var);
    return;
  }

  opt.lua_mode = true;
  dirs = split_env_var (env_var, value);
  max  = dirs ? smartlist_len (dirs) : 0;
  for (i = 0; i < max; i++)
  {
    const directory_array *d = smartlist_get (dirs, i);

    if (!cfg_ignore_lookup("[Lua]", d->dir) &&
        !cfg_ignore_lookup("[Lua]", basename(d->dir)))
       lua_append_dir (d, for_LUA_CPATH);
  }
  dir_array_free();
  opt.lua_mode = false;
}

/**
 * Dump the `lua_dirs` smartlist array.
 * Called if `opt.debug >= 1`
 */
static void lua_dump_dirs (void)
{
  int i, max = smartlist_len (lua_dirs);

  C_printf ("\n%s():\n  Num  exist  env-var    CWD  pattern  path\n%s\n",
            __FUNCTION__, str_repeat('=', 90));
  for (i = 0; i < max; i++)
  {
    const lua_dir *dir = smartlist_get (lua_dirs, i);

    C_printf ("  %2d:  %d      %-9s  %d    %-5.5s    %s\n",
              i, dir->exist, dir->is_CPATH ? "LUA_CPATH" : "LUA_PATH",
              dir->is_cwd, dir->pattern, dir->path);
  }
}

/**
 * Initialise this module. Only once.
 */
void lua_init (void)
{
  struct ver_info ver;
  char           *exe = NULL;

  if (lua_dirs)
     return;

  lua_get_info (&exe, &ver);
  FREE (exe);

  lua_dirs = smartlist_new();

  lua_handle_var ("LUA_PATH", false);
  lua_handle_var ("LUA_CPATH", true);

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
 * Called from `do_check()` to check `%LUA_PATH` and `%LUA_CPATH`
 * for missing directories.
 */
void lua_check_env (const char *env, int *num, char *status, size_t status_sz)
{
  int  i, max, errors, save;
  bool check_CPATH = true;

  ASSERT (!strcmp(env, "LUA_PATH") || !strcmp(env, "LUA_CPATH"));

  if (!strcmp(env, "LUA_PATH"))
     check_CPATH = false;

  if (!lua_dirs)
     lua_dirs = smartlist_new();

  status[0] = '\0';
  *num = 0;

  save = opt.quiet;
  opt.quiet  = 1;
  check_mode = 1;
  lua_handle_var (env, check_CPATH);
  check_mode = 0;
  opt.quiet  = save;

  if (opt.debug >= 1)
     lua_dump_dirs();

  *num = max = smartlist_len (lua_dirs);
  for (i = errors = 0; i < max; i++)
  {
    const lua_dir *dir = smartlist_get (lua_dirs, i);
    char fbuf [_MAX_PATH];

    if (str_equal_n("/cygdrive/", dir->path, 10))
         _strlcpy (fbuf, dir->path, sizeof(fbuf));
    else slashify2 (fbuf, dir->path, opt.show_unix_paths ? '/' : '\\');

    TRACE (2, "is_CPATH: %d, dir: '%s', exist: %d\n", dir->is_CPATH, fbuf, dir->exist);

    if (dir->is_CPATH != check_CPATH)
       continue;

    /* No Lua files (?.lua/?.dll) in current directory is normal.
     */
    if (!dir->exist && stricmp(fbuf, current_dir))
    {
      snprintf (status, status_sz, "~5Missing dir~0: ~3\"%s\"~0", fbuf);
      errors++;
    }

    if (opt.verbose)
    {
      C_printf ("     [%2d]: ~6%s", i, fbuf);
      C_puts ("~0\n");
    }
    else
    if (errors)
       break;
  }

  if (max == 0)
     _strlcpy (status, "~5Does not exists~0", status_sz);
  else if (!status[0])
    _strlcpy (status, "~2OK~0", status_sz);
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
 *  But that's the only way that `GetProcAddress()` will work.
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
static int lua_search_internal (const char *search_spec, bool is_CPATH)
{
  char env_name [30];
  int  i, i_max, found = 0;
  int  j, j_max;

  _strlcpy (env_name, is_CPATH ? "LUA_CPATH" : "LUA_PATH", sizeof(env_name));
  report_header_set ("Matches in %%%s:\n", env_name);

  i_max = smartlist_len (lua_dirs);
  for (i = 0; i < i_max; i++)
  {
    lua_dir     *dir = smartlist_get (lua_dirs, i);
    smartlist_t *dirlist;

    if (dir->is_CPATH != is_CPATH)
       continue;

    dirlist = get_matching_files (dir->path, dir->pattern);
    j_max   = dirlist ? smartlist_len (dirlist) : 0;

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
        r.file    = de->d_name;
        r.fsize   = de->d_fsize;
        r.mtime   = FILETIME_to_time_t (&de->d_time_write);
        r.key     = dir->is_CPATH ? HKEY_LUA_DLL : HKEY_LUA_FILE;
        r.content = opt.grep.content;
        if (report_file(&r))
        {
          found++;
          if (opt.PE_check && dir->is_CPATH)
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
  return lua_search_internal (search_spec, false) +
         lua_search_internal (search_spec, true);
}

/**
 * Config-file handler for kewords in the `[Lua]` section
 */
bool lua_cfg_handler (const char *section, const char *key, const char *value)
{
  if (!stricmp(key, "luajit.enable"))
  {
    prefer_luajit = atoi (value);
    return (true);
  }
  if (!stricmp(key, "ignore"))
     return cfg_ignore_handler (section, key, value);
  return (false);
}

const char *lua_get_exe (void)
{
  return (prefer_luajit ? "luajit.exe" : "lua.exe");
}

static int lua_version_cb (char *buf, int index)
{
  struct ver_info ver = { 0,0,0,0 };
  const char *fmt = prefer_luajit ? "LuaJIT %d.%d.%d" : "Lua %d.%d.%d";
  int   rc = 0;

  if (sscanf(buf, fmt, &ver.val_1, &ver.val_2, &ver.val_3) >= 2)
  {
    memcpy (&lua_ver, &ver, sizeof(lua_ver));
    rc = 1;
  }
  TRACE (2, "%s() returned %d, index: %d.\n", __FUNCTION__, rc, index);
  return (rc);
}

/**
 * Find the location and version for `lua.exe` (or `luajit.exe`) on `PATH`.
 */
bool lua_get_info (char **exe, struct ver_info *ver)
{
  static char exe_copy [_MAX_PATH];
  int    rc, _prefer_luajit = 0;

  *exe = NULL;
  *ver = lua_ver;

  /* We have already done this
   */
  if (lua_exe && VALID_VER(lua_ver))
  {
    *exe = STRDUP (lua_exe);
    return (true);
  }

  if (cache_getf(SECTION_LUA, "luajit.enable = %d", &_prefer_luajit) == 0 ||
      prefer_luajit != _prefer_luajit)
  {
    cache_del (SECTION_LUA, "lua_exe");
    cache_del (SECTION_LUA, "lua_version");
  }
  else
  {
    cache_getf (SECTION_LUA, "lua_exe = %s", &lua_exe);
    cache_getf (SECTION_LUA, "lua_version = %d,%d,%d", &lua_ver.val_1, &lua_ver.val_2, &lua_ver.val_3);
  }

  TRACE (2, "lua_exe: %s, ver: %d.%d.%d. _prefer_luajit: %d\n",
         lua_exe, lua_ver.val_1, lua_ver.val_2, lua_ver.val_3, _prefer_luajit);

  if (lua_exe && !FILE_EXISTS(lua_exe))
  {
    cache_del (SECTION_LUA, "lua_exe");
    cache_del (SECTION_LUA, "lua_version");
    memset (&lua_ver, '\0', sizeof(lua_ver));
    lua_exe = NULL;
    return lua_get_info (exe, ver);
  }

  if (!lua_exe)
     lua_exe = searchpath (lua_get_exe(), "PATH");

  if (!lua_exe)
     return (false);

  lua_exe = slashify2 (exe_copy, lua_exe, '\\');
  *exe = STRDUP (lua_exe);

  cache_putf (SECTION_LUA, "lua_exe = %s", lua_exe);
  cache_putf (SECTION_LUA, "luajit.enable = %d", prefer_luajit);

  if (!VALID_VER(lua_ver))
  {
    DWORD err_mode = SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    SetEnvironmentVariable ("LUA_TRACE", NULL);
    SetEnvironmentVariable ("LUAJIT_TRACE", NULL);

    rc = popen_run2 (lua_version_cb, lua_exe, "-v");
    TRACE (2, "popen_run2(): rc: %d.\n", rc);
    SetErrorMode (err_mode);

    if (rc > 0)
       cache_putf (SECTION_LUA, "lua_version = %d,%d,%d", lua_ver.val_1, lua_ver.val_2, lua_ver.val_3);

    /* The hopeless case were 'lua_exe' was found but failed to run a
     * simple "lua.exe -v". Hence just don't do this again.
     */
    if (rc <= 0)
    {
      cache_del (SECTION_LUA, "lua_exe");
      cache_del (SECTION_LUA, "lua_version");
      memset (&lua_ver, '\0', sizeof(lua_ver));
      *exe = lua_exe = NULL;
    }
  }

  *ver = lua_ver;
  TRACE (2, "%s: ver: %d.%d.%d.\n", lua_get_exe(), ver->val_1, ver->val_2, ver->val_3);

  return (lua_exe && VALID_VER(lua_ver));
}

