/**
 * \file    check.c
 * \ingroup Check
 * \brief
 *   Functions for `envtool --check`
 */
#include "envtool.h"
#include "ignore.h"
#include "lua.h"
#include "get_file_assoc.h"
#include "dirlist.h"
#include "cache.h"
#include "color.h"
#include "cmake.h"
#include "check.h"

#define CHECK_FMT  "   [%2d]: ~6"

static void check_env_val (const char *env, const char *file_spec, int *num, char *status, size_t status_sz);
static void check_env_val_reg (const smartlist_t *list, const char *env_name);
static void check_app_paths (HKEY key);
static int  check_user_sys_env (void);
static int  check_extras (void);
static void compare_user_sys_env (const char *env_var, const char *sys_value, int indent1);
static void shadow_report (smartlist_t *dir_list, const char *file_spec);

/**
 * The handler for mode `"--check"`.
 * Check the Registry keys even if `opt.no_app_path` is set.
 *
 * \typedef environ_fspec
 * Check which file-spec in which environment variable.
 */
typedef struct environ_fspec {
        const char *fspec;
        const char *env;
      } environ_fspec;

static environ_fspec envs[] = {
            { "*.exe",   "PATH"               },
            { "*.lib",   "LIB"                },
            { "*.a",     "LIBRARY_PATH"       },
            { "*.h",     "INCLUDE"            },
            { "*.h",     "C_INCLUDE_PATH"     },
            { "*",       "CPLUS_INCLUDE_PATH" },
            { NULL,      "MANPATH"            },
            { NULL,      "PKG_CONFIG_PATH"    },
            { "*.py?",   "PYTHONPATH"         },
            { "*.lua",   "LUA_PATH"           },
            { "*.dll",   "LUA_CPATH"          },
            { "*.cmake", "CMAKE_MODULE_PATH"  },
            { "*.pm",    "PERLLIBDIR"         },
            { NULL,      "CLASSPATH"          },  /* No support for these. But do it anyway */
            { "*.go",    "GOPATH"             },
            { NULL,      "FOO"                }   /* Check that non-existing env-vars are also checked */
          };

int check_handler (void)
{
  struct ver_info cmake_ver;
  char  *cmake_exe;
  char  *sys_env_path = NULL;
  char  *sys_env_inc  = NULL;
  char  *sys_env_lib  = NULL;
  char   status [200 + _MAX_PATH];
  int    i, save, num;
  int    rc = 0, index = 0;

  /* Do not implicitly add current directory in these searches.
   */
  save = opt.no_cwd;
  opt.no_cwd = 1;

  for (i = 0; i < DIM(envs); i++)
  {
    const char *env  = envs[i].env;
    const char *spec = envs[i].fspec;
    int   indent = (int) (sizeof("CPLUS_INCLUDE_PATH") - strlen(env));

    C_printf ("Checking ~3%%%s%%~0:%*c", env, indent, ' ');
    if (opt.verbose)
       C_putc ('\n');

    if (!stricmp("LUA_PATH", env) || !stricmp("LUA_CPATH", env))
         lua_check_env (env, &num, status, sizeof(status));
    else check_env_val (env, spec, &num, status, sizeof(status));

    C_printf ("%2d~0 elements, %s\n", num, status);
    if (opt.verbose)
       C_putc ('\n');
  }

  check_app_paths (HKEY_CURRENT_USER);
  if (opt.verbose)
     C_putc ('\n');

  check_app_paths (HKEY_LOCAL_MACHINE);
  if (opt.verbose)
     C_putc ('\n');

  if (opt.verbose && cmake_get_info(&cmake_exe, &cmake_ver))
  {
    FREE (cmake_exe);
    C_printf ("Checking ~3HKEY_CURRENT_USER\\%s~0 keys:\n", KITWARE_REG_NAME);
    num = cmake_get_info_registry (NULL, &index, HKEY_CURRENT_USER);
    if (num == 0)
         C_printf ("      ~5Does not exists~0\n\n");
    else C_printf ("      ~2OK~0, %d elements\n\n", num);

    C_printf ("Checking ~3HKEY_LOCAL_MACHINE\\%s~0 keys:\n", KITWARE_REG_NAME);
    num = cmake_get_info_registry (NULL, &index, HKEY_LOCAL_MACHINE);
    if (num == 0)
         C_printf ("         ~5Does not exists~0\n\n");
    else C_printf ("         ~2OK~0, %d elements\n\n", num);
  }

  scan_reg_environment (HKEY_LOCAL_MACHINE,
                        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                        &sys_env_path, &sys_env_inc, &sys_env_lib);

  check_env_val_reg (split_env_var(NULL, sys_env_path), "PATH");
  check_env_val_reg (split_env_var(NULL, sys_env_inc), "INCLUDE");
  check_env_val_reg (split_env_var(NULL, sys_env_lib), "LIB");

  opt.no_cwd = save;

  /**
   * \todo
   * Iterate over these environment sources:
   * ```
   *   _environ[]
   *   HKU\.DEFAULT\Environment
   *   HKCU\Environment
   *   HKCU\Volatile Environment
   * ```
   *
   * If there is a mismatch in a value in the above list, print a warning.
   *
   * And check for missing directories in above 'HKx' keys with values that looks
   * like directories. Like 'Path', 'TEMP'
   */
  if (opt.verbose)
  {
    rc += check_user_sys_env();
    C_putc ('\n');
  }

  rc += check_extras();

  FREE (sys_env_path);
  FREE (sys_env_inc);
  FREE (sys_env_lib);

  return (rc);
}

static void put_dirlist_to_cache (const char *env_var, smartlist_t *dirs)
{
  const directory_array *da;
  int   i, max = smartlist_len (dirs);

  for (i = 0; i < max; i++)
  {
    char result [_MAX_PATH+1];

    da = smartlist_get (dirs, i);

    _fix_path (da->dir, result);
    cache_putf (SECTION_ENV_DIR, "env_dir_%s_%d = %s", env_var, i, result);
  }
}

/**
 * The opposite of the above; get the directories for `env_var` from cache.
 * The `dir_array` smartlist is empty at this point.
 *
 * NB. Not used yet.
 */
_WUNUSED_FUNC_OFF()
static int get_dirlist_from_cache (const char *env_var)
{
  int i;

  for (i = 0;; i++)
  {
    char dir [_MAX_PATH];
    char format [50];

    snprintf (format, sizeof(format), "env_dir_%s_%d = %%s", env_var, i);
    if (cache_getf(SECTION_ENV_DIR, format, dir) != 1)
       break;

    dir_array_add (dir, str_equal(dir, current_dir));
  }
  TRACE (1, "Found %d cached 'env_dir_x'.\n", i);
  return (i);
}
_WUNUSED_FUNC_POP()

/**
 * Expand and check a single env-var for missing directories
 * and trailing/leading white space.
 *
 * \eg
 * ```
 *   set LIB=c:\foo1\lib ;c:\foo2\lib;  ^
 *           c:\foo3\lib;
 * ```
 *
 * would leave trailing white-space in `c:\foo1\lib ;` and `c:\foo2\lib;  ` <br>
 * and leading white-space in `        c:\foo3\lib`.
 *
 * \param[in]     env         the environment variable to check.
 * \param[in]     file_spec   the file-spec to check for shadowing files.
 * \param[out]    num         the number of elements in the '*env' value.
 * \param[in,out] status      the buffer to receive the state of the check.
 * \param[in]     status_sz   the size of the above buffer.
 *
 * Quit the below for-loop on the first error and store the error in `status`.
 * (unless in verbose-mode; `opt.verbose`).
 */
static void check_env_val (const char *env, const char *file_spec, int *num, char *status, size_t status_sz)
{
  smartlist_t           *list = NULL;
  int                    i, errors, ignored = 0, max = 0;
  char                  *value;
  const directory_array *arr;

  status[0] = '\0';
  *num = 0;

  value = getenv_expand (env);
  if (value)
  {
    list = split_env_var (env, value);
    *num = max = smartlist_len (list);
  }

  for (i = errors = 0; i < max; i++)
  {
    bool  is_cygdrive = false;
    char  fbuf [_MAX_PATH];
    const char *start, *end;

    arr = smartlist_get (list, i);
    start = arr->dir;
    end   = arr->dir + strlen(arr->dir) - 1;

    /* step over leading white-space
     */
    while (*start == ' ' || *start == '\t')
       start++;

    /* find trailing white-space
     */
    while (*end == ' ' || *end == '\t')
       end--;

    if (str_equal_n("/cygdrive/", arr->dir, 10))
    {
      _strlcpy (fbuf, arr->dir, sizeof(fbuf));
      is_cygdrive = true;
    }
    else
      slashify2 (fbuf, arr->dir, opt.show_unix_paths ? '/' : '\\');

    if (!stricmp("PATH", env))
       ignored = cfg_ignore_lookup ("[Path]", arr->dir);

    if (!opt.file_mode && !is_cygdrive && !isalpha(arr->dir[0]))
    {
      snprintf (status, status_sz, "~5Missing drive~0: ~3\"%s\"~0", fbuf);
      errors++;
    }
    else if (start > arr->dir)
    {
      snprintf (status, status_sz, "~5Leading white-space~0: ~3\"%s\"~0", fbuf);
      errors++;
    }
    else if (end < arr->dir + strlen(arr->dir) - 1)
    {
      snprintf (status, status_sz, "~5Trailing white-space~0: ~3\"%s\"~0", fbuf);
      errors++;
    }
    else if (!arr->exist)
    {
      if (opt.file_mode)
           snprintf (status, status_sz, "~5Missing file~0: ~3\"%s\"~0", fbuf);
      else snprintf (status, status_sz, "~5Missing dir~0: ~3\"%s\"~0", fbuf);
      errors++;
    }
    else if (!arr->is_cwd && dir_is_empty(fbuf))
    {
      snprintf (status, status_sz, "~5Empty dir~0: ~3\"%s\"~0", fbuf);
      errors++;
    }

    if (opt.verbose)
       C_printf (CHECK_FMT "%s~0\n", i, fbuf);
    else
    {
      if (errors)
         break;
    }
  }

  if (max == 0)
     _strlcpy (status, "~5Does not exists~0", status_sz);
  else if (!status[0])
    _strlcpy (status, "~2OK~0", status_sz);

  if (ignored)
  {
    status    += strlen (status);
    status_sz -= strlen (status);
    _strlcpy (status, " (ignored)", status_sz);
  }

  FREE (value);

  if (list)
  {
    put_dirlist_to_cache (env, list);
    if (opt.verbose && file_spec)
       shadow_report (list, file_spec);
  }

  dir_array_free();
  path_separator = ';';
}

/**
 * Do a check on an environment varable from: <br>
 * `HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Environment`.
 *
 * \param[in] list     a `smartlist_t` of the environment value components.
 * \param[in] env_name The name of the environment variable. E.g. `PATH`.
 */
static void check_env_val_reg (const smartlist_t *list, const char *env_name)
{
  int   i, errors = 0, max = 0;
  int   indent = sizeof("Checking");
  const directory_array *arr;

  C_printf ("Checking ~3%s\\%s / ~6%s~0:\n", reg_top_key_name(HKEY_LOCAL_MACHINE),
            "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", env_name);

  if (list)
     max = smartlist_len (list);

  for (i = 0; i < max; i++)
  {
    char  fbuf [_MAX_PATH];
    char  link [_MAX_PATH];
    DWORD attr;

    arr = smartlist_get (list, i);
    slashify2 (fbuf, arr->dir, opt.show_unix_paths ? '/' : '\\');

    if (opt.verbose)
    {
      C_printf (CHECK_FMT, i);
      print_raw (fbuf, "~6", NULL);

      attr = GetFileAttributes (arr->dir);
      if ((attr != INVALID_FILE_ATTRIBUTES) &&
          (attr & FILE_ATTRIBUTE_REPARSE_POINT) &&
          get_disk_type(arr->dir[0]) != DRIVE_REMOTE &&
          get_reparse_point (arr->dir, link, sizeof(link)))
      {
        char link2 [_MAX_PATH+100];

        C_puts ("\n      -> ~4");
        C_setraw (1);
        C_puts (slashify2(link2, link, opt.show_unix_paths ? '/' : '\\'));
        C_setraw (0);
      }
      C_puts ("~0\n");
    }

    if (!arr->exist)
    {
      C_printf ("%*c~5Missing dir~0:", indent, ' ');
      print_raw (fbuf, " ~3", "~0\n");
      errors++;
    }
    else if (arr->num_dup)
    {
      C_printf ("%*c~5Duplicated~0:", indent, ' ');
      print_raw (fbuf, " ~3", "~0\n");
      errors++;
    }
    else if (!arr->is_cwd && dir_is_empty(fbuf))
    {
      C_printf ("%*c~5Empty dir~0:", indent, ' ');
      print_raw (fbuf, " ~3", "~0\n");
      errors++;
    }
  }

  C_printf ("%*c", indent, ' ');

  if (max == 0)
     C_puts ("~5Empty~0, ");
  else if (errors == 0)
     C_puts ("~2OK~0, ");

  C_printf ("~6%2d~0 elements\n\n", max);
  dir_array_free();
}

/**
 * Check a single Registry-key for missing files and directories.
 * \param[in] key `HKEY_CURRENT_USER` or `HKEY_LOCAL_MACHINE`.
 *
 * The key to check will be:
 *   `key` + `\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths`
 *
 * Print results here since there can be so many missing files/directories.
 */
static void check_app_paths (HKEY key)
{
  int i, errors, max, indent = sizeof("Checking");
  smartlist_t *reg;

  C_printf ("Checking ~3%s\\%s~0:\n", reg_top_key_name(key), REG_APP_PATH);

  build_reg_array_app_path (key);
  reg = reg_array_head();
  max  = smartlist_len (reg);
  for (i = errors = 0; i < max; i++)
  {
    const registry_array *arr = smartlist_get (reg, i);
    char  fqfn [_MAX_PATH+2];
    char  fbuf [_MAX_PATH];

    slashify2 (fbuf, arr->path, opt.show_unix_paths ? '/' : '\\');

    if (opt.verbose)
    {
      char  fbuf2 [_MAX_PATH];
      char *fname;

      C_printf (CHECK_FMT, i);
      snprintf (fbuf2, sizeof(fbuf2), "%s\\%s", arr->path, arr->fname);
      fname = fbuf2;

      if (get_actual_filename(&fname, false))
      {
        print_raw (fname, NULL, NULL);
        FREE (fname);
      }
      else
      {
        fname = opt.show_unix_paths ? slashify (fname,'/') : fname;
        print_raw (fname, NULL, NULL);
      }
      C_puts ("~0\n");
    }

    if (!is_directory(fbuf) && !cfg_ignore_lookup("[Registry]", fbuf))
    {
      C_printf ("%*c~5Missing dir~0: ~3%s\"~0\n", indent, ' ', fbuf);
      errors++;
      continue;
    }

    snprintf (fqfn, sizeof(fqfn), "%s\\%s", fbuf, arr->fname);

    if (!arr->exist && !cfg_ignore_lookup("[Registry]",fqfn))
    {
      slashify2 (fbuf, fqfn, opt.show_unix_paths ? '/' : '\\');
      C_printf ("%*c~5Missing file~0: ~3%s~0\n", indent, ' ', fbuf);
      errors++;
    }
  }

  C_printf ("%*c", indent, ' ');
  if (max == 0)
     C_puts ("~5Does not exists~0,");  /* Impossible */
  else if (errors == 0)
     C_puts ("~2OK~0, ");
  else
     C_puts ("~5Error~0, ");

  C_printf ("~6%2d~0 elements\n", max);
  reg_array_free();
}

/**
 * Compare all environment values for SYSTEM and check if there
 * is a difference in the corresponding USER variable.
 */
static int check_user_sys_env (void)
{
  smartlist_t *sys_list;
  int          i, max;
  size_t       len, longest_env = 0;
  char        *sys_env, *sys_val;

  C_puts ("\nComparing ~6SYSTEM~0 and ~2USER~0 environments:\n");

  if (!getenv_system(&sys_list))
  {
    C_printf ("CreateEnvironmentBlock() failed: %s.\n", win_strerror(GetLastError()));
    return (0);
  }

  TRACE (1, "C_screen_width(): %d\n", (int)C_screen_width());
  max = smartlist_len (sys_list);
  for (i = 0; i < max; i++)
  {
    sys_env = smartlist_get (sys_list, i);
    sys_val = strchr (sys_env, '=');
    *sys_val++ = '\0';
    len = strlen (sys_env);
    if (len > longest_env)
       longest_env = len;
  }

  for (i = 0; i < max; i++)
  {
    sys_env = smartlist_get (sys_list, i);
    sys_val = strchr (sys_env, '\0') + 1;
    compare_user_sys_env (sys_env, sys_val, (int)(2 + longest_env));
  }
  smartlist_free_all (sys_list);
  return (0);
}

#if 0
/**
 * \todo
 * The handler for mode `"--check-dups"`.
 * Requires a TCC shell
 *
 * This `dedupe` command searches recursively for symlink duplicated files.
 * And printing the SHA256 signature of each file.
 */
static void do_check_dups (const char *dir)
{
  char *env = getenv ("COMSPEC");

  if (!env)
     return;

  env = strlwr (basename(env));
  if (strcmp(env, "tcc.exe"))
  {
    WARN ("dedupe needs a 'TCC' shell\n");
    return;
  }
  popen_run2 (dedupe_cb, "dedupe", "/SV * %s 2> NUL", dir);
}
#endif

/**
 * Print a single environment value from SYSTEM and USER limited to the screen width.
 * Thus printing a long string as "C:\Program Files (x86)\Mi...tudio 14.0\Common7\Tools"
 */
static void print_env_val (const char *val, size_t indent)
{
  size_t c_width = C_screen_width();

  C_setraw (1);
  if (c_width == UINT_MAX)
       C_puts (val);
  else print_long_line2 (val, indent-1, ';');
  C_setraw (0);
  C_puts ("~0\n");
}

/**
 * Compare an environment value from SYSTEM and USER.
 * Ignore differences in case and trailing slashes ('\\' or '/') or ';'.
 * Also ignore difference in values like:
 *   \li `0` vs `false` or
 *   \li `1` vs `true`.
 *
 * \todo if an env-var looks like 1 or more directories and one of it's values
 *       is a junction, check it's target before comparing each directory.
 */
static void compare_user_sys_env (const char *env_var, const char *sys_value, int indent1)
{
  const char *sys_end;
  const char *user_value = getenv (env_var);
  char       *sys_val    = getenv_expand_sys (sys_value);
  int         i;
  bool        equal = false;
  size_t      sys_len, indent2;

  if (sys_val)
     sys_value = sys_val;

  sys_len = strlen (sys_value);
  sys_end = sys_value + sys_len - 1;
  if (IS_SLASH(*sys_end))
  {
    sys_len--;
    sys_end--;
  }
  if (*sys_end == ';')
     sys_len--;

#if 0
  if (sys_value && user_value && strchr(sys_value, ';') && strchr(user_value, ';'))
  {
    user_env = split_env_var (user_value);
    sys_env  = split_env_var (sys_value);
  }
#endif

  if (sys_value && user_value)
  {
    const char *equals[] = { "false", "0", "true", "1" };

    for (i = 0; i < DIM(equals) && !equal; i++)
    {
      if (!stricmp(sys_value, equals[i]) || !stricmp(user_value, equals[i]))
         equal = true;
    }
    if (!equal)
       equal = (strnicmp(user_value, sys_value, sys_len) == 0);
  }

  C_printf ("  ~3%-*s~0", indent1, env_var);
  indent2 = indent1 + sizeof("SYSTEM = ") + 2;

  if (!user_value)
  {
    C_puts ("~6SYSTEM = ");
    print_env_val (sys_value, indent2);
    C_printf ("  %*s~2USER   = <None>~0\n", indent1, "");
  }
  else if (!equal)
  {
    C_printf ("Mismatch:\n  %*s~6SYSTEM = ", indent1, "");
    print_env_val (sys_value, indent2);
    C_printf ("  %*s~2USER   = ", indent1, "");
    print_env_val (user_value, indent2);
  }
  else
  {
    C_puts ("Match: ~6");
    print_env_val (sys_value, indent2);
  }
  FREE (sys_val);
}

/*
 * Check 2 files with the same `basename()`.
 *
 * If `this_de->d_name` is older than `prev_de->d_name` (by a configurable amount of time),
 * then it's considered a shadow of `prev_de->d_name`.
 *
 * Ignore if the file or (it's basename) is listed as `ignore` in the `[Shadow]`
 * section of `~/envtool.cfg`.
 */
static bool is_shadow_candidate (const struct dirent2 *this_de,
                                 const struct dirent2 *prev_de,
                                 FILETIME *newest, FILETIME *oldest)
{
  const char *this_base = basename (this_de->d_name);
  const char *prev_base = basename (prev_de->d_name);
  ULONGLONG   this_ft, prev_ft, diff;

  if (stricmp(this_base, prev_base))
     return (false);

  if (cfg_ignore_lookup("[Shadow]", this_base) ||
      cfg_ignore_lookup("[Shadow]", this_de->d_name) ||
      cfg_ignore_lookup("[Shadow]", prev_de->d_name))
  {
    TRACE (2, "Ignoring file '%s' and '%s'.\n", this_de->d_name, prev_de->d_name);
    return (false);
  }

  this_ft = (((ULONGLONG)this_de->d_time_write.dwHighDateTime) << 32) + this_de->d_time_write.dwLowDateTime;
  prev_ft = (((ULONGLONG)prev_de->d_time_write.dwHighDateTime) << 32) + prev_de->d_time_write.dwLowDateTime;
  diff = prev_ft - this_ft;

  if (this_ft && this_ft < prev_ft && diff > opt.shadow_dtime)
  {
    TRACE (1, "Write-time of '%s' shadows '%s'.\n", this_de->d_name, prev_de->d_name);
    *newest = prev_de->d_time_write;
    *oldest = this_de->d_time_write;
    return (true);
  }

#if 0
  this_ft = (((ULONGLONG)this_de->d_time_create.dwHighDateTime) << 32) + this_de->d_time_create.dwLowDateTime;
  prev_ft = (((ULONGLONG)prev_de->d_time_create.dwHighDateTime) << 32) + prev_de->d_time_create.dwLowDateTime;
  diff = prev_ft - this_ft;

  if (this_ft && this_ft < prev_ft && diff > opt.shadow_dtime)
  {
    TRACE (1, "Creation-time of '%s' shadows '%s'.\n", this_de->d_name, prev_de->d_name);
    *newest = prev_de->d_time_create;
    *oldest = this_de->d_time_create;
    return (true);
  }
#endif

  return (false);
}

/**
 * Traverse dir-lists of 2 directories and add a "shadow warning" to `shadow_list`
 * if an older file is found in `prev_dir->dir` (which is ahead of `this_dir->dir`
 * in the path for this env-var).
 *
 * \eg. with a `PATH=f:\\ProgramFiler\\Python31;f:\\CygWin32\\bin` and these files:
 * ```
 *   f:\ProgramFiler\Python31\python3.1.exe   24.06.2024  12:38   (9 months older)
 *   f:\CygWin32\bin\python3.1.exe            24.03.2025  18:32
 * ```
 *
 * then the oldest `python3.1.exe` shadows the newest `python3.1.exe`.
 *
 * But the "time-slack" is controlled by `opt.shadow_dtime` (in seconds).
 * E.g. if opt.shadow_dtime == 23668200  (== 9 months), the shadow-state is ignored.
 */
static void check_shadow_files (smartlist_t *this_de_list,
                                smartlist_t *prev_de_list,
                                smartlist_t *shadow_list)
{
  const struct dirent2 *this_de;
  const struct dirent2 *prev_de;
  int   i, max_i = smartlist_len (this_de_list);
  int   j, max_j = smartlist_len (prev_de_list);

  for (i = 0; i < max_i; i++)
  {
    this_de = smartlist_get (this_de_list, i);

    for (j = 0; j < max_j; j++)
    {
      FILETIME newest, oldest;

      prev_de = smartlist_get (prev_de_list, j);
      if (is_shadow_candidate(this_de, prev_de, &newest, &oldest))
      {
        struct shadow_entry *se = MALLOC (sizeof(*se));

        se->shadowing_file      = this_de->d_name;
        se->shadowed_file       = prev_de->d_name;
        se->shadowed_FILE_TIME  = newest;
        se->shadowing_FILE_TIME = oldest;
        smartlist_add (shadow_list, se);
      }
    }
  }
}

/**
 * For all directories (in `dir_list`), build lists of files matching `file_spec`
 * and do a shadow check of files in all directories after the `arr_i->dir`.
 * This is to show possibly newer files that should be used instead.
 *
 * \param[in] dir_list   List of directories from the expansion of e.g. `%PATH%`.
 * \param[in] file_spec  The file-spec to check for shadows.
 *                       E.g. `"*.exe"` if we look for shadows in `%PATH%` and
 *                            `"*.h"` if we look for shadows in `%INCLUDE%`.
 */
static void shadow_report (smartlist_t *dir_list, const char *file_spec)
{
  directory_array *arr_i, *arr_j;
  smartlist_t     *shadows;
  int              i, j, max;

  shadows = smartlist_new();
  max = smartlist_len (dir_list);

  for (i = 0; i < max; i++)
  {
    arr_i = smartlist_get (dir_list, i);
    if (arr_i->exist && !arr_i->is_native)
       arr_i->dirent2 = get_matching_files (arr_i->dir, file_spec);
  }

  /* For all directories in env-var, do a shadow check of files in
   * all directories after the 'arr_i->dir'
   */
  for (i = 0; i < max; i++)
  {
    arr_i = smartlist_get (dir_list, i);
    for (j = max-1; j > i; j--)
    {
      arr_j = smartlist_get (dir_list, j);
      TRACE (1, "i/j: %2d/%2d: %-50.50s / %-50.50s\n", i, j, arr_i->dir, arr_j->dir);

      if (arr_i->dirent2 && arr_j->dirent2)
         check_shadow_files (arr_i->dirent2, arr_j->dirent2, shadows);
    }
  }

  max = smartlist_len (shadows);
  if (max > 0)
  {
    const struct shadow_entry *se;
    size_t len, longest = 0;
    char   slash = opt.show_unix_paths ? '/' : '\\';

    /* First find the longest shadow line
     */
    for (i = 0; i < max; i++)
    {
      se = smartlist_get (shadows, i);
      len = strlen (se->shadowing_file);
      if (len > longest)
         longest = len;
      len = strlen (se->shadowed_file);
      if (len > longest)
         longest = len;
    }
    if (max > 1)
         C_printf ("     ~5%d shadows:~0\n", max);
    else C_printf ("     ~5%d shadow:~0\n", max);
    for (i = 0; i < max; i++)
    {
      const char *t1, *t2;

      se = smartlist_get (shadows, i);
      t1 = get_time_str_FILETIME (&se->shadowed_FILE_TIME);
      t2 = get_time_str_FILETIME (&se->shadowing_FILE_TIME);

      C_printf ("     ~6shadowed:~0 %-*s  ~6%s~0\n", (int)longest, slashify(se->shadowed_file, slash), t1);
      C_printf ("               %-*s  ~6%s~0\n", (int)longest, slashify(se->shadowing_file, slash), t2);
    }

    /* We're done; free the shadow-list
     */
#if defined(_CRTDBG_MAP_ALLOC)
    smartlist_wipe (shadows, free);
#else
    _WFUNC_CAST_OFF()
    smartlist_wipe (shadows, (smartlist_free_func)free_at);
    _WFUNC_CAST_POP()
#endif
  }
  smartlist_free (shadows);
}

/**
 * \typedef extra_check
 */
typedef struct extra_check {
        bool  in_environment;
        bool  is_quoted;
        HKEY  reg_key;
        char  what [500];
      } extra_check;

typedef struct key_map {
        const char *name;
        HKEY        key;
      } key_map;

static const key_map keymap[] = {
     { "HKCR", HKEY_CLASSES_ROOT },
     { "HKCU", HKEY_CURRENT_USER },
     { "HKLM", HKEY_LOCAL_MACHINE },
     { "HKU",  HKEY_USERS },
     { "HKPD", HKEY_PERFORMANCE_DATA },
     { "HKDD", HKEY_DYN_DATA },
     { "HKCC", HKEY_CURRENT_CONFIG },
     { "HKEY_USERS", HKEY_USERS },

   #if 0  /* Do we need these? */
     { "HKEY_DYN_DATA",      HKEY_DYN_DATA },
     { "HKEY_CLASSES_ROOT",  HKEY_CLASSES_ROOT },
     { "HKEY_CURRENT_USER",  HKEY_CURRENT_USER },
     { "HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE },
     { "HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG }
   #endif
   };

/**
 * A dynamic array of extra_check.
 */
static smartlist_t *extra_checks = NULL;
static size_t       longest_env_var = 0;
static size_t       longest_key_name = 0;

/**
 * Call from `cleanup()` to free the `extra_checks` list.
 */
void check_exit (void)
{
  if (extra_checks)
     smartlist_free_all (extra_checks);
  extra_checks = NULL;
}

/**
 * Translate a top-key string like "HKLM" to a predefined key.
 */
static bool key_name_to_top_key (const char *top_key, HKEY *reg_key)
{
  int i;

  *reg_key = NULL;

  for (i = 0; i < DIM(keymap); i++)
  {
    if (!stricmp(top_key, keymap[i].name))
    {
      *reg_key = keymap[i].key;
      return (true);
    }
  }
  return (false);
}

/**
 * Like `reg_top_key_name()`, but returns the short name.
 */
static const char *top_key_name_short (HKEY top_key)
{
  int i;

  for (i = 0; i < DIM(keymap); i++)
  {
    if (top_key == keymap[i].key)
       return (keymap[i].name);
  }
  return ("?");
}

/**
 * The config-callback for key / values in the `[Check]` section.
 */
bool check_cfg_handler (const char *section, const char *key, const char *value)
{
  extra_check *check;
  size_t len;

  ASSERT (!strcmp(section, "[Check]"));

  if (!extra_checks)
     extra_checks = smartlist_new();

  if (!stricmp(key, "env"))
  {
    char *p;

    check = CALLOC (sizeof(*check), 1);
    check->in_environment = true;

    if (value[0] == '%')
         _strlcpy (check->what, value+1, sizeof(check->what));
    else _strlcpy (check->what, value, sizeof(check->what));
    p = strrchr (check->what, '%');
    if (p)
       *p = '\0';

    len = strlen (check->what);
    if (len > longest_env_var)
       longest_env_var = len;

    smartlist_add (extra_checks, check);

    TRACE (0, "Added check for env-var: '%s'\n", check->what);
    return (true);
  }

  if (!stricmp(key, "registry"))
  {
    char  top_key  [sizeof(check->what)];
    char  key_name [sizeof(check->what)];
    char *val, *p;
    HKEY  key_val;
    bool  is_quoted = false;

    if (!strncmp(value, "\\\\", 2))
    {
#if 0
      WARN ("%s(%u):\nA Remote key '%s' is unsupported.\n",
            cfg_get_file(opt.cfg_file),  cfg_get_line(opt.cfg_file), value);
      return (true);   /* Avoid "Unhandled setting" warning */
#else
      return (false);
#endif
    }

    p = strchr (value, '\\');
    if (!p || p[1] == '\0')
       return (false);

    val = STRDUP (value);
    if (str_unquote(val) != val)
       is_quoted = true;
    if (*val == '\0')
    {
      WARN ("Empty quoted value: '%s'.\n", value);
      FREE (val);
      return (false);
    }

    p = strchr (val, '\\');
    *p = '\0';
    _strlcpy (top_key, val, sizeof(top_key));
    _strlcpy (key_name, p + 1, sizeof(key_name));
    FREE (val);

    if (!key_name_to_top_key(top_key, &key_val))
    {
      TRACE (0, "Illegal top-key: '%s'\n", top_key);
      return (false);
    }

    check = CALLOC (sizeof(*check), 1);
    _strlcpy (check->what, key_name, sizeof(check->what));
    check->reg_key   = key_val;
    check->is_quoted = is_quoted;

    len = strlen (top_key_name_short(check->reg_key)) + 1 + strlen (check->what);
    if (len > longest_key_name)
       longest_key_name = len;

    TRACE (0, "Added check for reg-key: '%s\\%s'\n", top_key_name_short(check->reg_key), check->what);
    smartlist_add (extra_checks, check);
    return (true);
  }
  return (false);
}

/**
 * Iterate over Registry `top_key\\key_name` and count all keys for `key_name`.
 * Similar to e.g.:
 *   reg.exe query HKLM\SYSTEM\CurrentControlSet\Services\SharedAccess\Defaults\FirewallPolicy /s
 */
static int get_reg_vals (HKEY top_key, const char *key_name)
{
  HKEY  key = NULL;
  int   num = 0;
  DWORD rc = RegOpenKeyEx (top_key, key_name, 0, KEY_READ, &key);

  if (rc != ERROR_SUCCESS)
      TRACE (1, "RegOpenKeyEx (\"%s\\%s\"): rc: %lu / %s\n",
             top_key_name_short(top_key), key_name, rc, win_strerror(GetLastError()));
  else num++;

  while (rc == ERROR_SUCCESS)
  {
    char  value [1000] = { "?" };
    DWORD size = sizeof(value);

    rc = RegEnumKeyEx (key, num - 1, value, &size, NULL, NULL, NULL, NULL);
    TRACE (1, "RegEnumKeyEx(%d): rc: %lu, '%s'\n", num - 1, rc, value);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;
    num++;
  }
  if (key)
     RegCloseKey (key);
  return (num);
}

static int check_reg_var (const extra_check *check)
{
  const char *indent = "\n    ";
  int         num = 0;
  size_t      len;

  C_puts ("  Checking ");
  len = (size_t) C_printf ("~3%s\\%s~0:", top_key_name_short(check->reg_key), check->what) - 1;
  num = get_reg_vals (check->reg_key, check->what);

  if (!opt.verbose)
     indent = str_repeat (' ', longest_key_name - len + 1);

  if (num == 0)
       C_printf ("%s~5Does not exists~0\n", indent);
  else C_printf ("%s~2OK~0, %d elements\n", indent, num);

  return (0);
}

static int check_env_var (const extra_check *check)
{
  const char *env = check->what;
  char   status [200 + _MAX_PATH];
  size_t indent = longest_env_var - strlen(env) + 1;
  int    num;

  C_printf ("  Checking ~3%%%s%%~0:%*c", env, (int)indent, ' ');
  if (opt.verbose)
     C_putc ('\n');

  check_env_val (env, NULL, &num, status, sizeof(status));
  C_printf ("    %s, %d~0 elements\n", status, num);
  return (0);
}

static int check_extras (void)
{
  int i, max = extra_checks ? smartlist_len (extra_checks) : 0;
  int save1 = opt.no_cwd;
  int save2 = opt.file_mode;
  int rc = 0;

  opt.no_cwd = opt.file_mode = 1;
  C_printf ("Doing %d ~3extra_checks:~0, longest_key_name: %zu\n", max, longest_key_name);

  for (i = 0; i < max; i++)
  {
    const extra_check *check = smartlist_get (extra_checks, i);

    if (check->in_environment)
         rc += check_env_var (check);
    else rc += check_reg_var (check);
  }
  opt.no_cwd    = save1;
  opt.file_mode = save2;
  check_exit();
  return (rc);
}

