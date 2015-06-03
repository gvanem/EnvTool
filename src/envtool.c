/*
 * EnvTool:
 *  a simple tool to search and check various environment variables
 *  for correctness and check where a specific file is in corresponding
 *  environment variable.
 *
 * E.g. 1: "envtool --path notepad.exe" first checks the %PATH% env-var
 *         for consistency (reports missing directories in %PATH%) and prints
 *         all the locations of "notepad.exe".
 *
 * E.g. 2: "envtool --inc afxwin.h" first checks the %INCLUDE% env-var
 *         for consistency (reports missing directories in %INCLUDE) and prints
 *         all the locations of "afxwin.h".
 *
 * By Gisle Vanem <gvanem@yahoo.no> August 2011.
 *
 * Functions fnmatch() and searchpath() taken from djgpp and modified:
 *   Copyright (C) 1995 DJ Delorie, see COPYING.DJ for details
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <windows.h>
#include <shlobj.h>

#define INSIDE_ENVOOL_C

#include "getopt_long.h"
#include "Everything.h"
#include "Everything_IPC.h"

#include "color.h"
#include "envtool.h"
#include "envtool_py.h"

/* \todo:
 *    Add support for 'kpathsea'-like search paths.
 *    E.g. if a PATH (or INCLUDE etc.) component contains "/foo/bar//", the search will
 *         do a recursive search for all files (and dirs) under "/foo/bar/".
 *    Ref. http://tug.org/texinfohtml/kpathsea.html
 */

#ifdef __MINGW32__
  /*
   * Tell MingW's CRT to turn off command line globbing by default.
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

/* For getopt_long.c.
 */
char *program_name = NULL;

#define REG_APP_PATH    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths"

#define KNOWN_DLL_PATH  "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\KnownDLLs"

/*
 * \todo: if looking for a DLL pattern, the above location should also
 * be checked. Ref:
 *   http://msdn.microsoft.com/en-us/library/windows/desktop/ms682586%28v=vs.85%29.aspx
 */

#define MAX_PATHS   500
#define MAX_ARGS    20

struct directory_array {
       char *dir;           /* FQDN of this entry */
       int   exist;         /* does it exist? */
       int   is_dir;        /* and is it a dir; _S_ISDIR() */
       int   is_cwd;        /* and is equal to g_current_dir[] */
       int   exp_ok;        /* ExpandEnvironmentStrings() returned with no '%'? */
       int   num_dup;       /* is duplicated elsewhere in %VAR? */
     };

struct registry_array {
       char   *fname;       /* basename of this entry. I.e. the name of the enumerated key. */
       char   *real_fname;  /* normally the same as above unless aliased. E.g. "winzip.exe -> "winzip32.exe" */
       char   *path;        /* path of this entry */
       int     exist;       /* does it exist? */
       time_t  mtime;       /* file modification time */
       __int64 fsize;       /* file size */
       HKEY    key;
     };

struct directory_array dir_array [MAX_PATHS];
struct registry_array  reg_array [MAX_PATHS];

struct prog_options opt;

static int   num_version_ok = 0;

static char *who_am_I = (char*) "envtool";

static char *system_env_path = NULL;
static char *system_env_lib  = NULL;
static char *system_env_inc  = NULL;

static char *user_env_path   = NULL;
static char *user_env_lib    = NULL;
static char *user_env_inc    = NULL;
static char *report_header   = NULL;

static char *new_argv [MAX_ARGS];  /* argv[0...] + contents of "%ENVTOOL_OPTIONS" allocated here */
static int   new_argc;             /* 1... to highest allocated cmd-line component */

static int   path_separator = ';';
static char  file_spec_buf [_MAX_PATH];
static char  g_current_dir [_MAX_PATH];

static volatile int halt_flag;

static void  usage (const char *fmt, ...) ATTR_PRINTF(1,2);
static void  do_tests (void);
static void  searchpath_gnu (void);
static void  searchpath_msvc (void);
static void  searchpath_watcom (void);
static void  print_build_cflags (void);
static void  print_build_ldflags (void);

/*
 * \todo: In 'report_file()', test if a file (in %PATH, %INCLUDE or %LIB) is
 *        shadowed by an older file of the same name (ahead of the newer file).
 *        Warn if this is the case.
 *
 * \todo: Add sort option: on date/time.
 *                           on filename.
 *
 * \todo: Add date/time format option.
 *           Normal: "07 Nov 2012 - 18:06:58".
 *           Option: "20121107.180658".  (like zipinfo does)
 */

/*
 * According to:
 *  http://msdn.microsoft.com/en-us/library/windows/desktop/ms683188(v=vs.85).aspx
 */
#define MAX_ENV_VAR 32767

static void show_evry_version (HWND wnd)
{
  LRESULT major    = SendMessage (wnd, WM_USER, EVERYTHING_IPC_GET_MAJOR_VERSION, 0);
  LRESULT minor    = SendMessage (wnd, WM_USER, EVERYTHING_IPC_GET_MINOR_VERSION, 0);
  LRESULT revision = SendMessage (wnd, WM_USER, EVERYTHING_IPC_GET_REVISION, 0);
  LRESULT build    = SendMessage (wnd, WM_USER, EVERYTHING_IPC_GET_BUILD_NUMBER, 0);
  int     d, indexed ['Z'-'A'+1];
  char    buf [3*DIM(indexed)+2], *p = buf;

  C_printf ("  Everything search engine ver. %ld.%ld.%ld.%ld (c) David Carpenter; %s\n",
            major, minor, revision, build, "http://www.voidtools.com/");

  for (d = 0; d < DIM(indexed); d++)
      indexed[d] = (int) SendMessage (wnd, WM_USER, EVERYTHING_IPC_IS_NTFS_DRIVE_INDEXED, d);

  *p = '\0';
  for (d = 0; d < DIM(indexed); d++)
      if (indexed[d])
         p += sprintf (p, "%c: ", d+'A');

  C_printf ("  These drives are indexed: ~3%s~0\n", buf);
}

static void show_version (void)
{
  const char *py_exe;
  HWND        wnd = FindWindow (EVERYTHING_IPC_WNDCLASS, 0);

  /* \todo: report all detected Python programs and their versions. */

  int py_ver_major, py_ver_minor, py_ver_micro;
  int py = get_python_info (&py_exe, NULL, &py_ver_major, &py_ver_minor, &py_ver_micro);

  C_printf ("%s.\n  Version ~3%s ~1(%s, %s)~0 by %s. %s~0\n",
          who_am_I, VER_STRING, BUILDER, WIN_VERSTR, AUTHOR_STR,
          is_wow64_active() ? "~1WOW64." : "");

  if (wnd)
       show_evry_version (wnd);
  else C_printf ("  Everything search engine not found\n");

  if (py)
       C_printf ("  Python %d.%d.%d detected -> ~6%s~0.\n", py_ver_major, py_ver_minor, py_ver_micro, py_exe);
  else C_printf ("  Python ~5not~0 found.\n");

  if (opt.do_version >= 2)
  {
    C_printf ("\n  Compilers on ~3PATH~0:\n");
    searchpath_gnu();
    searchpath_msvc();
    searchpath_watcom();

    C_puts ("\n  Compile command and ~3CFLAGS~0:");
    print_build_cflags();

    C_puts ("\n  Link command and ~3LDFLAGS~0:");
    print_build_ldflags();

    C_printf ("\n  Pythons on ~3PATH~0:\n");
    searchpath_pythons();
  }
  exit (0);
}

static void usage (const char *fmt, ...)
{
  va_list args;

  va_start (args, fmt);
  C_vprintf (fmt, args);
  va_end (args);
  exit (-1);
}

static void show_help (void)
{
  C_printf ("Environment check & search tool.\n"
            "%s.\n\n"
            "Usage: %s [-cdDhitTrsqpuV?] ~6<--mode>~0 ~6<file-spec>~0\n"
            "  ~6<--mode>~0 can be one of these:\n"
            "    ~6--path~0:         check and search in ~3%%PATH%%~0.\n"
            "    ~6--python~0[~3=X~0]:   check and search in ~3%%PYTHONPATH%%~0 and '~3sys.path[]~0'. ~2[1]~0.\n"
            "    ~6--inc~0:          check and search in ~3%%INCLUDE%%~0                      ~2[2]~0.\n"
            "    ~6--lib~0:          check and search in ~3%%LIB%%~0 and ~3%%LIBRARY_PATH%%~0.      ~2[3]~0.\n"
            "    ~6--evry~0:         check and search in the EveryThing database.\n"
            "\n"
            "  Other options:\n"
            "    ~6--no-gcc~0:       do not spawn '*gcc.exe' prior to checking          ~2[2,3]~0.\n"
            "    ~6--no-g++~0:       do not spawn '*g++.exe' prior to checking          ~2[2,3]~0.\n"
            "    ~6--no-sys~0:       do not scan '~3HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment~0'.\n"
            "    ~6--no-usr~0:       do not scan '~3HKCU\\Environment~0'.\n"
            "    ~6--no-app~0:       do not scan '~3HKCU\\" REG_APP_PATH "~0' and\n"
            "                                '~3HKLM\\" REG_APP_PATH "~0'.\n"
            "    ~6--pe-check~0:     print checksum and version-info for PE-files.\n"
            "    ~6-c~0:             don't add current directory to search-list.\n"
            "    ~6-C~0, ~6--color~0:    print using colours to Windows console.\n"
            "    ~6-d~0, ~6--debug~0:    set debug level (~3-dd~0 sets ~3PYTHONVERBOSE=1~0 in ~6--python~0 mode).\n"
            "    ~6-D~0, ~6--dir~0:      looks only for directories matching \"file-spec\".\n",
            AUTHOR_STR, who_am_I);

  C_printf ("    ~6-r~0, ~6--regex~0:    enable Regular Expressions in '~6--evry~0' searches.\n"
            "    ~6-s~0, ~6--size~0:     show size of file(s) found.\n"
            "    ~6-q~0, ~6--quiet~0:    disable warnings.\n"
            "    ~6-t~0:             do some internal tests.\n"
            "    ~6-T~0:             show file times in sortable decimal format.\n"
            "    ~6-u~0:             show all paths on Unix format: '~2c:/ProgramFiles/~0'.\n"
            "    ~6-v~0:             increase verbose level (currently only used in '~6--pe-check~0').\n"
            "    ~6-V~0:             show program version information. '~6-VV~0' prints more info.\n"
            "    ~6-h~0, ~6-?~0:         show this help.\n"
            "\n"
            "  ~2[1]~0 The '~6--python~0' option can be detailed further with: '~3=X~0'\n"
            "      '~6py2~0'    use a Python2 program only.\n"
            "      '~6py3~0'    use a Python3 program only.\n"
            "      '~6ipy2~0'   use a IronPython2 program only.\n"
            "      '~6ipy3~0'   use a IronPython3 program only.\n"
            "      '~6pypy~0'   use a PyPy program only.\n"
            "      '~6jython~0' use a Jython program only.\n"
            "      '~6all~0'    use all of the above Python programs.\n"
            "               otherwise use only first Python found on PATH (i.e. the default).\n"
            "\n"
            "  ~2[2]~0  Unless '~6--no-gcc~0' and/or '~6--no-g++~0' is used, the\n"
            "       ~3%%C_INCLUDE_PATH%%~0 and ~3%%CPLUS_INCLUDE_PATH%%~0 are also found by spawning '*gcc.exe' and '*g++.exe'.\n"
            "\n"
            "  ~2[3]~0  Unless '~6--no-gcc~0' and/or '~6--no-g++~0' is used, the\n"
            "       ~3%%LIBRARY_PATH%%~0 are also found by spawning '*gcc.exe' and '*g++.exe'.\n"
            "\n"
            "  The '~6--evry~0' option requires that the Everything filename search engine is installed.\n"
            "  Ref. ~3http://www.voidtools.com/support/everything/~0\n"
            "\n"
            "Notes:\n"
            "  'file-spec' accepts Posix ranges. E.g. '[a-f]*.txt'.\n"
            "  'file-spec' matches both files and directories. If '--dir' or '-D' is used, only\n"
            "   matching directories are reported.\n"
            "   Commonly used options can be put in ~3%%ENVTOOL_OPTIONS%%~0.\n");
  exit (0);
}


/*
 * Add the 'dir' to 'dir_array[]' at index 'i'.
 * 'is_cwd' == 1 if 'dir == cwd'.
 *
 * Since this function could be called with a 'dir' from ExpandEnvironmentStrings(),
 * we check here if it returned with no '%'.
 */
void add_to_dir_array (const char *dir, int i, int is_cwd)
{
  struct directory_array *d = dir_array + i;
  struct stat st;
  int    j, exp_ok = (*dir != '%');

  memset (&st, 0, sizeof(st));
  d->dir    = STRDUP (dir);
  d->exp_ok = exp_ok;
  d->exist  = exp_ok && (stat(dir, &st) == 0);
  d->is_dir = _S_ISDIR (st.st_mode);
  d->is_cwd = is_cwd;

  if (is_cwd || !exp_ok)
     return;

  for (j = 0; j < i; j++)
      if (!stricmp(dir,dir_array[j].dir))
         d->num_dup++;
}

/*
 * Add elements to 'reg_array[]':
 *  - '*idx':    the array-index to store to. Increment on successfull add.
 *  - 'top_key': the key the entry came from: HKEY_CURRENT_USER or HKEY_LOCAL_MACHINE.
 *  - 'fname':   the result from 'RegEnumKeyEx()'; name of each key.
 *  - 'fqdn':    the result from 'enum_sub_values()'. This value includes the full path.
 *
 * Note: 'basename (fqdn)' may NOT be equal to 'fname' (aliasing). That's the reason
 *       we store 'real_fname' too.
 */
static void add_to_reg_array (int *idx, HKEY key, const char *fname, const char *fqdn)
{
  struct registry_array *reg;
  struct stat  st;
  const  char *base;
  char   path [_MAX_PATH];
  size_t len;
  int    rc, i = *idx;

  reg = reg_array + i;

  assert (fname);
  assert (fqdn);
  assert (i >= 0);
  assert (i < DIM(reg_array));

  memset (&st, '\0', sizeof(st));
  base = basename (fqdn);
  if (base == fqdn)
  {
    DEBUGF (1, "fqdn (%s) contains no '\\' or '/'\n", fqdn);
    return;
  }

  len = base - fqdn;
  rc  = stat (fqdn, &st);
  reg->mtime      = st.st_mtime;
  reg->fsize      = st.st_size;
  reg->fname      = STRDUP (fname);
  reg->real_fname = STRDUP (base);
  reg->path       = STRDUP (_strlcpy(path,fqdn,len));
  reg->exist      = (rc == 0) && FILE_EXISTS (fqdn);
  reg->key        = key;
  *idx = ++i;
}

/*
 * `Sort the 'reg_array' on 'path' + 'real_fname'.
 */
#define DO_SORT  1

#if DO_SORT
  typedef int (*CmpFunc) (const void *, const void *);

  static int reg_array_compare (const struct registry_array *a,
                                const struct registry_array *b)
  {
    char fqdn_a [_MAX_PATH];
    char fqdn_b [_MAX_PATH];
    int  slash = (opt.show_unix_paths ? '/' : '\\');

    if (!a->path || !a->real_fname || !b->path || !b->real_fname)
       return (0);
    snprintf (fqdn_a, sizeof(fqdn_a), "%s%c%s", slashify(a->path, slash), slash, a->real_fname);
    snprintf (fqdn_b, sizeof(fqdn_b), "%s%c%s", slashify(b->path, slash), slash, b->real_fname);

    return stricmp (fqdn_a, fqdn_b);
  }
#endif

static void sort_reg_array (int num)
{
#if DO_SORT
  int i, slash = (opt.show_unix_paths ? '/' : '\\');

  DEBUGF (1, "before qsort():\n");
  for (i = 0; i < num; i++)
     DEBUGF (1, "%2d: FQDN: %s%c%s.\n", i, reg_array[i].path, slash, reg_array[i].real_fname);

  qsort (&reg_array, num, sizeof(reg_array[0]), (CmpFunc)reg_array_compare);

  DEBUGF (1, "after qsort():\n");
  for (i = 0; i < num; i++)
     DEBUGF (1, "%2d: FQDN: %s%c%s.\n", i, reg_array[i].path, slash, reg_array[i].real_fname);
#endif
}

/*
 * Parses an environment string and returns all components as an array of
 * 'struct directory_array'. Add current working directory first if
 * 'opt.add_cwd' is TRUE.
 *
 * Convert CygWin style paths to Windows paths: "/cygdrive/x/.." -> "x:/.."
 */
static struct directory_array *split_env_var (const char *env_name, const char *value)
{
  char *tok, *val = STRDUP (value);
  int   is_cwd, i;
  char  sep [2];

  sep[0] = path_separator;
  sep[1] = 0;

  tok = strtok (val, sep);
  memset (dir_array, 0, sizeof(dir_array));

  is_cwd = !strcmp(val,".") || !strcmp(val,".\\") || !strcmp(val,"./");

  DEBUGF (1, "'val': \"%s\". 'tok': \"%s\", is_cwd: %d\n", val, tok, is_cwd);

 /*
  * If 'val' doesn't start with ".\" or "./", we should possibly add that
  * first since the search along e.g. %LIB% will include the current
  * directory (cwd) in the search implicitly. This is not always the case for
  * all 'env' variables. E.g. Gnu-C's preprocessor doesn't include "." in
  * the %C_INCLUDE_PATH% by default.
  */
  i = 0;
  if (opt.add_cwd && !is_cwd)
     add_to_dir_array (g_current_dir, i++, 1);

  for ( ; i < DIM(dir_array)-1 && tok; i++)
  {
    /* Remove trailing '\\' or '/' from environment component
     * unless it's a "c:\".
     */
    char *p, *end = strchr (tok, '\0');

    if (end > tok+3 && (end[-1] == '\\' || end[-1] == '/'))
       end[-1] = '\0';

    if (!opt.quiet)
    {
      /* Check and warn when a component on form 'c:\dir with space' is found.
       * I.e. a path without quotes "c:\dir with space".
       */
      p = strchr (tok, ' ');
      if (p && (*tok != '"' || end[-1] != '"'))
         WARN ("%s: \"%s\" needs to be enclosed in quotes.\n", env_name, tok);
    }

    p = strchr (tok, '%');
    if (p)
      WARN ("%s: unexpanded component \"%s\".\n", env_name, tok);

    if (*tok == '"' && end[-1] == '"')   /* Remove quotes */
    {
      tok++;
      end[-1] = '\0';
    }

    /* _stati64(".") doesn't work. Hence turn "." into 'g_current_dir'.
     */
    is_cwd = !strcmp(tok,".") || !strcmp(tok,".\\") || !strcmp(tok,"./");
    if (is_cwd)
    {
      if (i > 0)
         WARN ("Having \"%s\" not first in \"%s\" is asking for trouble.\n",
               tok, env_name);
      tok = g_current_dir;
    }
    else if (opt.conv_cygdrive && strlen(tok) >= 12 && !strnicmp(tok,"/cygdrive/",10))
    {
      char buf [_MAX_PATH];

      snprintf (buf, sizeof(buf), "%c:/%s", tok[10], tok+12);
      DEBUGF (1, "CygPath conv: '%s' -> '%s'\n", tok, buf);
      tok = buf;
    }

    add_to_dir_array (tok, i, !stricmp(tok,g_current_dir));

    tok = strtok (NULL, sep);
  }

  if (i == DIM(dir_array)-1)
     WARN ("Too many paths (%d) in env-var \"%s\"\n", i, env_name);

  FREE (val);
  return (dir_array);
}

/*
 * Report time and name of 'file'. Also: if the match came from a
 * registry search, report which key had the match.
 * If the Python 'sys.path[]'
 */
static int found_in_hkey_current_user = 0;
static int found_in_hkey_current_user_env = 0;
static int found_in_hkey_local_machine = 0;
static int found_in_hkey_local_machine_sess_man = 0;
static int found_in_python_egg = 0;
static int found_in_default_env = 0;
static int found_in_everything_db = 0;

static void print_PE_info (BOOL is_PE, BOOL is_python_egg, BOOL chksum_ok,
                           const struct ver_info *ver)
{
  const char *filler = "      ";
  char       *ver_trace, *line;
  int         raw;

  if (is_python_egg)
  {
    C_printf ("\n%sCannot examine PYD-files inside Python EGGs.", filler);
    if (opt.verbose >= 1)
       C_putc ('\n');
    C_flush();
    return;
  }

  if (!is_PE)
  {
    C_printf ("\n%s~3Not~0 a PE-image.", filler);
    if (opt.verbose >= 1)
       C_putc ('\n');
    C_flush();
    return;
  }

  C_printf ("\n%sver ~6%u.%u.%u.%u~0, Chksum %s~0",
            filler, ver->val_1, ver->val_2, ver->val_3, ver->val_4,
            chksum_ok ? "~2OK" : "~5fail");

  ver_trace = get_version_info_buf();
  if (ver_trace)
  {
    raw = C_setraw (1);  /* In case version-info contains a "~" (SFN). */

    C_putc ('\n');
    for (line = strtok(ver_trace,"\n"); line; line = strtok(NULL,"\n"))
        C_printf ("%s%s\n", filler, line);
    C_setraw (raw);
    get_version_info_free();
    C_flush();
  }
}

static const char *fsize_str (unsigned __int64 size)
{
  const char *suffix = "";
  unsigned __int64 divisor;
  static char buf [10];

  if (size < 1024)
    divisor = 1, suffix = " B ";

  else if (size < 1024*1024)
    divisor = 1024, suffix = " kB";

  else if (size < 1024ULL*1024ULL*1024ULL)
    divisor = 1024*1024, suffix = " MB";

  else if (size < 1024ULL*1024ULL*1024ULL*1024ULL)
    divisor = 1024*1024*1024, suffix = " GB";

  else
    divisor = 1024ULL*1024ULL*1024ULL*1024ULL, suffix = " PB";

  size /= divisor;
  snprintf (buf, sizeof(buf), "%4" U64_FMT "%s", size, suffix);
  return (buf);
}

int report_file (const char *file, time_t mtime, __int64 fsize, BOOL is_dir, HKEY key)
{
  const char      *note   = NULL;
  const char      *filler = "      ";
  char             size [30] = "?";
  int              raw;

  if (key == HKEY_CURRENT_USER)
  {
    found_in_hkey_current_user = 1;
    note = " (1)  ";
  }
  else if (key == HKEY_LOCAL_MACHINE)
  {
    found_in_hkey_local_machine = 1;
    note = " (2)  ";
  }
  else if (key == HKEY_CURRENT_USER_ENV)
  {
    found_in_hkey_current_user_env = 1;
    note = " (3)  ";
  }
  else if (key == HKEY_LOCAL_MACHINE_SESSION_MAN)
  {
    found_in_hkey_local_machine_sess_man = 1;
    note = " (4)  ";
  }
  else if (key == HKEY_PYTHON_EGG)
  {
    found_in_python_egg = 1;
    note = " (5)  ";
  }
  else if (key == HKEY_EVERYTHING)
  {
    found_in_everything_db = 1;
    if (is_dir)
       note = "<DIR> ";
  }
  else
  {
    found_in_default_env = 1;
  }

  if (!is_dir && opt.dir_mode)
    return (0);

  if (opt.show_size && fsize > 0)
       snprintf (size, sizeof(size), " - %s", fsize_str((unsigned __int64)fsize));
  else size[0] = '\0';

  if (key != HKEY_PYTHON_EGG)
  {
    char buf [_MAX_PATH];

    _fixpath (file, buf);  /* Has '\\' slashes */
    file = buf;
    if (opt.show_unix_paths)
       file = slashify (file, '/');
  }

  if (report_header)
     C_printf ("~3%s~0", report_header);

  report_header = NULL;

  C_printf ("~3%s~0%s%s: ", note ? note : filler, get_time_str(mtime), size);

  /* In case 'file' contains a "~" (SFN), we switch to raw mode.
   */
  raw = C_setraw (1);
  C_puts (file);
  C_setraw (raw);

  /* Add a slash to end of a directory.
   */
  if (is_dir)
  {
    const char *end = strchr (file, '\0');

    if (end > file && end[-1] != '\\' && end[-1] != '/')
      C_putc (opt.show_unix_paths ? '/' : '\\');
  }
  else if (opt.PE_check)
  {
    struct ver_info ver;
    BOOL   is_PE      = FALSE;
    BOOL   is_py_egg  = (key == HKEY_PYTHON_EGG);
    BOOL   chksum_ok  = FALSE;
    BOOL   version_ok = FALSE;

    memset (&ver, 0, sizeof(ver));
    if (!is_py_egg && check_if_PE(file))
    {
      is_PE      = TRUE;
      chksum_ok  = verify_pe_checksum (file);
      version_ok = get_version_info (file, &ver);
      if (version_ok)
         num_version_ok++;
    }
    print_PE_info (is_PE, is_py_egg, chksum_ok, &ver);
  }

  C_putc ('\n');
  return (1);
}

static void final_report (int found)
{
  BOOL do_warn = FALSE;

  if ((found_in_hkey_current_user || found_in_hkey_current_user_env ||
       found_in_hkey_local_machine || found_in_hkey_local_machine_sess_man) &&
       found_in_default_env)
  {
    /* We should only warn if a match finds file(s) from different sources.
     */
    do_warn = opt.quiet ? FALSE : TRUE;
  }

  if (do_warn || found_in_python_egg)
     C_putc ('\n');

  if (found_in_hkey_current_user)
     C_printf ("~3 (1): found in \"HKEY_CURRENT_USER\\%s\".~0\n", REG_APP_PATH);

  if (found_in_hkey_local_machine)
     C_printf ("~3 (2): found in \"HKEY_LOCAL_MACHINE\\%s\".~0\n", REG_APP_PATH);

  if (found_in_hkey_current_user_env)
     C_printf ("~3 (3): found in \"HKEY_CURRENT_USER\\%s\".~0\n", "Environment");

  if (found_in_hkey_local_machine_sess_man)
     C_printf ("~3 (4): found in \"HKEY_LOCAL_MACHINE\\%s\".~0\n",
               "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");

  if (found_in_python_egg)
     C_puts ("~3 (5): found in a .zip/.egg in 'sys.path[]'.~0\n");

  if (do_warn)
    C_printf ("\n"
              "  ~5The search found matches outside the default environment (PATH etc.).\n"
              "  Hence running an application from the Start-Button may result in different .EXE/.DLL\n"
              "  to be loaded than from the command-line. Revise the above registry-keys.\n\n~0");

  C_printf ("%d match%s found for \"%s\".",
            found, (found == 0 || found > 1) ? "es" : "", opt.file_spec);

  if (opt.PE_check)
     C_printf (" %d have PE-version info.", num_version_ok);

  C_putc ('\n');
}

/*
 * Check for suffix or trailing wildcards. If not found, add a
 * trailing "*".
 *
 * If 'opt.file_spec' starts with a subdir(s) part, return that in
 * '*sub_dir' with a trailing DIR_SEP. And return a 'fspec'
 * without the sub-dir part.
 *
 * Not used in '--evry' search.
 */
static char *fix_filespec (char **sub_dir)
{
  static char fname  [_MAX_PATH];
  static char subdir [_MAX_PATH];
  char  *p, *fspec = _strlcpy (fname, opt.file_spec, sizeof(fname));
  char  *lbracket, *rbracket;

  /*
   * If we do e.g. "envtool --inc openssl/ssl.h", we must preserve
   * the subdir part since FindFirstFile() doesn't give us this subdir part
   * in 'ff_data.cFileName'. It just returns the matching file(s) *within*
   * that subdir.
   */
  *sub_dir = NULL;
  p = basename (fspec);
  if (p > fspec)
  {
    memcpy (&subdir, fspec, p-fspec);
    *sub_dir = subdir;
    fspec = p;
    DEBUGF (2, "fspec: '%s', *sub_dir: '%s'\n", fspec, *sub_dir);
  }

 /*
  * Since FindFirstFile() doesn't work with POSIX ranges, replace
  * the range part in 'fspec' with a '*'. This could leave a '**' in
  * 'fspec', but that doesn't hurt.
  *
  * Note: we still must use 'opt.file_spec' in 'fnmatch()' for a POSIX
  *       range to work below.
  */
  lbracket = strchr (fspec, '[');
  rbracket = strchr (fspec, ']');

  if (lbracket && rbracket > lbracket)
  {
    *lbracket = '*';
    _strlcpy (lbracket+1, rbracket+1, strlen(rbracket));
  }

  DEBUGF (1, "fspec: %s, *sub_dir: %s\n", fspec, *sub_dir);
  return (fspec);
}

const char *reg_type_name (DWORD type)
{
  return (type == REG_SZ               ? "REG_SZ" :
          type == REG_MULTI_SZ         ? "REG_MULTI_SZ" :
          type == REG_EXPAND_SZ        ? "REG_EXPAND_SZ" :
          type == REG_LINK             ? "REG_LINK" :
          type == REG_BINARY           ? "REG_BINARY" :
          type == REG_DWORD            ? "REG_DWORD" :
          type == REG_RESOURCE_LIST    ? "REG_RESOURCE_LIST" :
          type == REG_DWORD_BIG_ENDIAN ? "REG_DWORD_BIG_ENDIAN" :
          type == REG_QWORD            ? "REG_QWORD" : "?");
}

/*
 * Swap bytes in a 32-bit value.
 */
static DWORD swap_long (DWORD val)
{
  return ((val & 0x000000FFU) << 24) |
         ((val & 0x0000FF00U) <<  8) |
         ((val & 0x00FF0000U) >>  8) |
         ((val & 0xFF000000U) >> 24);
}

static const char *top_key_name (HKEY key)
{
  return (key == HKEY_LOCAL_MACHINE ? "HKEY_LOCAL_MACHINE" :
          key == HKEY_CURRENT_USER  ? "HKEY_CURRENT_USER" :
          "?");
}

static const char *access_name (REGSAM acc)
{
  #define ADD_VALUE(v)  { v, #v }

  static const struct search_list access[] = {
                                  ADD_VALUE (KEY_CREATE_LINK),
                                  ADD_VALUE (KEY_CREATE_SUB_KEY),
                                  ADD_VALUE (KEY_ENUMERATE_SUB_KEYS),
                                  ADD_VALUE (KEY_NOTIFY),
                                  ADD_VALUE (KEY_QUERY_VALUE),
                                  ADD_VALUE (KEY_SET_VALUE),
#if defined(KEY_WOW64_32KEY)
                                  ADD_VALUE (KEY_WOW64_32KEY),
#endif
#if defined(KEY_WOW64_64KEY)
                                  ADD_VALUE (KEY_WOW64_64KEY)
#endif
                                };

  acc &= ~STANDARD_RIGHTS_READ;  /* == STANDARD_RIGHTS_WRITE, STANDARD_RIGHTS_EXECUTE */

  if ((acc & KEY_ALL_ACCESS) == KEY_ALL_ACCESS)
     return ("KEY_ALL_ACCESS");
  return flags_decode (acc, access, DIM(access));
}

static REGSAM read_access (void)
{
  static BOOL init     = FALSE;
  static BOOL is_wow64 = FALSE;
  REGSAM access = KEY_READ;

#if defined(KEY_WOW64_32KEY) && defined(KEY_WOW64_64KEY)
#if (IS_WIN64)
  access |= KEY_WOW64_32KEY;
#else
  if (!init)
     is_wow64 = is_wow64_active();
  if (is_wow64)
     access |= KEY_WOW64_64KEY;
#endif
#endif  /* KEY_WOW32_64KEY && KEY_WOW64_64KEY */

  ARGSUSED (is_wow64);
  init = TRUE;
  return (access);
}

static BOOL enum_sub_values (HKEY top_key, const char *key_name, const char **ret)
{
  HKEY   key = NULL;
  DWORD  num, rc;
  REGSAM acc = read_access();
  const char *ext = strrchr (key_name, '.');

  *ret = NULL;
  rc = RegOpenKeyEx (top_key, key_name, 0, acc, &key);

  DEBUGF (1, "  RegOpenKeyEx (%s\\%s, %s):\n                  %s\n",
          top_key_name(top_key), key_name, access_name(acc), win_strerror(rc));

  if (rc != ERROR_SUCCESS)
  {
    WARN ("    Error opening registry key \"%s\\%s\", rc=%lu\n",
          top_key_name(top_key), key_name, rc);
    return (FALSE);
  }

  for (num = 0; rc == ERROR_SUCCESS; num++)
  {
    char   value [512] = "\0";
    char   data [512]  = "\0";
    DWORD  value_size  = sizeof(value);
    DWORD  data_size   = sizeof(data);
    DWORD  type        = REG_NONE;
    DWORD  val32;
    LONG64 val64;

    rc = RegEnumValue (key, num, value, &value_size, NULL, &type,
                       (LPBYTE)&data, &data_size);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    val32 = *(DWORD*) &data[0];
    val64 = *(LONG64*) &data[0];

    if (type == REG_EXPAND_SZ && strchr(data,'%'))
    {
      char  exp_buf [MAX_ENV_VAR] = "<none>";
      DWORD ret = ExpandEnvironmentStrings (data, exp_buf, sizeof(exp_buf));

      DEBUGF (1, "    ExpandEnvironmentStrings(): ret: %lu, exp_buf: \"%s\"\n", ret, exp_buf);

      if (ret > 0)
         _strlcpy (data, exp_buf, sizeof(data));
    }

    switch (type)
    {
      case REG_SZ:
      case REG_EXPAND_SZ:
      case REG_MULTI_SZ:
           DEBUGF (1, "    num: %lu, %s, value: \"%s\", data: \"%s\"\n",
                      num, reg_type_name(type),
                      value[0] ? value : "(no value)",
                      data[0]  ? data  : "(no data)");
           if (!*ret && data[0])
           {
             static char ret_data [_MAX_PATH];
             const char *dot = strrchr (data, '.');

             /* Found 1st data-value with extension we're looking for. Return it.
              */
             if (dot && !stricmp(dot,ext))
                *ret = _strlcpy (ret_data, data, sizeof(ret_data));
           }
           break;

      case REG_LINK:
           DEBUGF (1, "    num: %lu, REG_LINK, value: \"%S\", data: \"%S\"\n",
                      num, (wchar_t*)value, (wchar_t*)data);
           break;

      case REG_DWORD_BIG_ENDIAN:
           val32 = swap_long (*(DWORD*)&data[0]);
           /* fall through */

      case REG_DWORD:
           DEBUGF (1, "    num: %lu, %s, value: \"%s\", data: %lu\n",
                      num, reg_type_name(type), value[0] ? value : "(no value)", val32);
           break;

      case REG_QWORD:
           DEBUGF (1, "    num: %lu, REG_QWORD, value: \"%s\", data: %" S64_FMT "\n",
                      num, value[0] ? value : "(no value)", val64);
           break;

      case REG_NONE:
           break;

      default:
           DEBUGF (1, "    num: %lu, unknown REG_type %lu\n", num, type);
           break;
    }
  }
  if (key)
     RegCloseKey (key);
  return (*ret != NULL);
}

/*
 * Enumerate all keys under 'top_key + REG_APP_PATH' and build up
 * 'reg_array [idx..MAX_PATHS]'.
 *
 * Either under:
 *   "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths"
 * or
 *   "HKEY_CURRENT_USER\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths"
 *
 * Return number of entries added.
 */
static int build_reg_array_app_path (HKEY top_key)
{
  HKEY   key = NULL;
  int    num, idx = 0;
  REGSAM acc = read_access();
  DWORD  rc  = RegOpenKeyEx (top_key, REG_APP_PATH, 0, acc, &key);

  DEBUGF (1, "  RegOpenKeyEx (%s\\%s, %s):\n                   %s\n",
          top_key_name(top_key), REG_APP_PATH, access_name(acc), win_strerror(rc));

  for (num = idx = 0; rc == ERROR_SUCCESS; num++)
  {
    char  sub_key [512];
    char  fname [512];
    const char *fqdn;
    DWORD size = sizeof(fname);

    rc = RegEnumKeyEx (key, num, fname, &size, NULL, NULL, NULL, NULL);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    DEBUGF (1, "  RegEnumKeyEx(): num %d: %s\n", num, fname);

    snprintf (sub_key, sizeof(sub_key), "%s\\%s", REG_APP_PATH, fname);

    if (enum_sub_values(top_key,sub_key,&fqdn))
       add_to_reg_array (&idx, top_key, fname, fqdn);

    if (idx == DIM(reg_array)-1)
       break;
  }

  if (key)
     RegCloseKey (key);
  return (idx);
}


/*
 * Scan registry under:
 *   HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment
 * and
 *   HKCU\Environment
 *
 * and return any 'PATH', 'LIB' and 'INCLUDE' in them.
 *
 * There can only be one of each of these under each registry 'sub_key'.
 * (otherwise the registry is truly messed up). Return first of each found.
 *
 * If one of these still contains a "%value%" after ExpandEnvironmentStrings(),
 * this is checked later.
 */
static void scan_reg_environment (HKEY top_key, const char *sub_key,
                                  char **path, char **inc, char **lib)
{
  HKEY   key = NULL;
  REGSAM acc = read_access();
  DWORD  num, rc = RegOpenKeyEx (top_key, sub_key, 0, acc, &key);

  DEBUGF (1, "RegOpenKeyEx (%s\\%s, %s):\n                 %s\n",
          top_key_name(top_key), sub_key, access_name(acc), win_strerror(rc));

  for (num = 0; rc == ERROR_SUCCESS; num++)
  {
    char  name  [100]         = "<none>";
    char  value [MAX_ENV_VAR] = "<none>";
    DWORD nsize = sizeof(name);
    DWORD vsize = sizeof(value);
    DWORD type;

    rc = RegEnumValue (key, num, name, &nsize, NULL, &type, (LPBYTE)&value, &vsize);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    if (type == REG_EXPAND_SZ && strchr(value,'%'))
    {
      char  exp_buf [MAX_ENV_VAR];
      DWORD ret = ExpandEnvironmentStrings (value, exp_buf, sizeof(exp_buf));

      if (ret > 0)
        _strlcpy (value, exp_buf, sizeof(value));
    }

    if (!strcmp(name,"PATH"))
       *path = STRDUP (value);

    else if (!strcmp(name,"INCLUDE"))
       *inc = STRDUP (value);

    else if (!strcmp(name,"LIB"))
       *lib = STRDUP (value);

#if 0
    DEBUGF (1, "num %2lu, %s, %s=%.40s%s\n", num, reg_type_name(type), name, value,
               strlen(value) > 40 ? "..." : "");
#else
    DEBUGF (1, "num %2lu, %s, %s=%s\n", num, reg_type_name(type), name, value);
#endif
  }
  if (key)
     RegCloseKey (key);

  DEBUGF (1, "\n");
}

static struct directory_array *arr0;

static void free_dir_array (void)
{
  struct directory_array *arr;

  for (arr = arr0; arr && arr->dir; arr++)
      FREE (arr->dir);
}

static int do_check_env2 (HKEY key, const char *env, const char *value)
{
  struct directory_array *arr;
  int    found = 0;

  for (arr = arr0 = split_env_var(env,value); arr->dir; arr++)
      found += process_dir (arr->dir, arr->num_dup, arr->exist,
                            arr->is_dir, arr->exp_ok, env, key);
  free_dir_array();
  return (found);
}


static int scan_system_env (void)
{
  int found = 0;

  report_header = "Matches in HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment:\n";

  scan_reg_environment (HKEY_LOCAL_MACHINE,
                        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                        &system_env_path, &system_env_inc, &system_env_lib);

  if (opt.do_path && system_env_path)
     found += do_check_env2 (HKEY_LOCAL_MACHINE_SESSION_MAN, "System PATH", system_env_path);

  if (opt.do_include && system_env_inc)
     found += do_check_env2 (HKEY_LOCAL_MACHINE_SESSION_MAN, "System INCLUDE", system_env_inc);

  if (opt.do_lib && system_env_lib)
     found += do_check_env2 (HKEY_LOCAL_MACHINE_SESSION_MAN, "System LIB", system_env_lib);

  return (found);
}

static int scan_user_env (void)
{
  int found = 0;

  report_header = "Matches in HKCU\\Environment:\n";

  scan_reg_environment (HKEY_CURRENT_USER, "Environment",
                        &user_env_path, &user_env_inc, &user_env_lib);

  if (opt.do_path && user_env_path)
     found += do_check_env2 (HKEY_CURRENT_USER_ENV, "User PATH", user_env_path);

  if (opt.do_include && user_env_inc)
     found += do_check_env2 (HKEY_CURRENT_USER_ENV, "User INCLUDE", user_env_inc);

  if (opt.do_lib && user_env_lib)
     found += do_check_env2 (HKEY_CURRENT_USER_ENV, "User LIB", user_env_lib);

  return (found);
}

/************************************************************************************************/

static int report_registry (const char *reg_key, int num)
{
  struct registry_array *arr;
  int    i, found;

  for (i = found = 0, arr = reg_array; i < num; i++, arr++)
  {
    char fqdn [_MAX_PATH];
    int  match = FNM_NOMATCH;

    snprintf (fqdn, sizeof(fqdn), "%s%c%s", arr->path, DIR_SEP, arr->real_fname);

    if (!arr->exist)
       WARN ("\"%s\\%s\" points to\n  \"%s\". But this file does not exist.\n\n",
             top_key_name(arr->key), reg_key, fqdn);
    else
    {
      match = fnmatch (opt.file_spec, arr->fname, FNM_FLAG_NOCASE);
      if (match == FNM_MATCH)
      {
        if (report_file(fqdn, arr->mtime, arr->fsize, FALSE, arr->key))
           found++;
      }
    }
    DEBUGF (1, "i=%2d: exist=%d, match=%d, key=%s, fname=%s, path=%s\n",
               i, arr->exist, match, top_key_name(arr->key), arr->fname, arr->path);
  }

  for (arr = reg_array; arr->fname; arr++)
  {
    FREE (arr->fname);
    FREE (arr->real_fname);
    FREE (arr->path);
  }
  return (found);
}

static int do_check_registry (void)
{
  char reg[300];
  int  num, found = 0;

  snprintf (reg, sizeof(reg), "Matches in HKCU\\%s:\n", REG_APP_PATH);
  report_header = reg;
  DEBUGF (1, "%s\n", reg);
  num = build_reg_array_app_path (HKEY_CURRENT_USER);
  sort_reg_array (num);
  found += report_registry (REG_APP_PATH, num);

  snprintf (reg, sizeof(reg), "Matches in HKLM\\%s:\n", REG_APP_PATH);
  report_header = reg;
  DEBUGF (1, "%s\n", reg);
  num = build_reg_array_app_path (HKEY_LOCAL_MACHINE);
  sort_reg_array (num);
  found += report_registry (REG_APP_PATH, num);

  return (found);
}

/*
 * Process directory specified by 'path' and report any matches
 * to the global 'opt.file_spec'.
 */
int process_dir (const char *path, int num_dup, BOOL exist,
                 BOOL is_dir, BOOL exp_ok, const char *prefix, HKEY key)
{
  HANDLE          handle;
  WIN32_FIND_DATA ff_data;
  char            fqfn  [_MAX_PATH];  /* Fully qualified file-name */
  int             found = 0;

  /* We need to set these only once; 'opt.file_spec' is constant throughout the program.
   */
  static char *fspec  = NULL;
  static char *subdir = NULL;  /* Looking for a 'opt.file_spec' with a sub-dir part in it. */

  if (num_dup > 0)
  {
    WARN ("%s: directory \"%s\" is duplicated. Skipping.\n", prefix, path);
    return (0);
  }

  if (!exp_ok)
  {
    WARN ("%s: directory \"%s\" has an unexpanded value.\n", prefix, path);
    return (0);
  }

  if (!exist)
  {
    WARN ("%s: directory \"%s\" doesn't exist.\n", prefix, path);
    return (0);
  }

  if (!is_dir)
     WARN ("%s: directory \"%s\" isn't a directory.\n", prefix, path);

  if (!opt.file_spec)
  {
    DEBUGF (1, "\n");
    return (0);
  }

  if (!fspec)
     fspec = fix_filespec (&subdir);

  snprintf (fqfn, sizeof(fqfn), "%s%c%s%s", path, DIR_SEP, subdir ? subdir : "", fspec);
  handle = FindFirstFile (fqfn, &ff_data);
  if (handle == INVALID_HANDLE_VALUE)
  {
    DEBUGF (1, "\"%s\" not found.\n", fqfn);
    return (0);
  }

  do
  {
    struct stat   st;
    char  *base, *file;
    int    match, len;

    if (!strcmp(ff_data.cFileName,".."))
       continue;

    len  = snprintf (fqfn, sizeof(fqfn), "%s%c", path, DIR_SEP);
    base = fqfn + len;
    len += snprintf (base, sizeof(fqfn)-len, "%s%s",
                     subdir ? subdir : "", ff_data.cFileName);

    is_dir = ((ff_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    file = slashify (fqfn, DIR_SEP);

    match = fnmatch (opt.file_spec, base, FNM_FLAG_NOCASE | FNM_FLAG_NOESCAPE);

#if 0
    if (match == FNM_NOMATCH && strchr(opt.file_spec,'~'))
    {
      /* The case where 'opt.file_spec' is a SFN, fnmatch() doesn't work.
       * What to do?
       */
    }
    else
#endif

    if (match == FNM_NOMATCH)
    {
      /* The case where 'base' is a dotless file, fnmatch() doesn't work.
       * I.e. if 'opt.file_spec' == "ratio.*" and base == "ratio", we qualify
       *      this as a match.
       */
      if (!is_dir && !opt.dir_mode && !strnicmp(base,opt.file_spec,strlen(base)))
        match = FNM_MATCH;
    }

    DEBUGF (1, "Testing \"%s\". is_dir: %d, %s\n", file, is_dir, fnmatch_res(match));

    if (match == FNM_MATCH && stat(file, &st) == 0)
    {
      if (report_file(file, st.st_mtime, st.st_size, is_dir, key))
         found++;
    }
  }
  while (FindNextFile(handle, &ff_data));

  FindClose (handle);
  return (found);
}

static const char *evry_strerror (DWORD err)
{
  static char buf[30];

  switch (err)
  {
    case EVERYTHING_OK:
         return ("No error");
    case EVERYTHING_ERROR_MEMORY:
         return ("Memory error");
    case EVERYTHING_ERROR_IPC:
         return ("IPC error");
    case EVERYTHING_ERROR_REGISTERCLASSEX:
         return ("Error in RegisterClassEx()");
    case EVERYTHING_ERROR_CREATEWINDOW:
         return ("Error in CreateWindow()");
    case EVERYTHING_ERROR_CREATETHREAD:
         return ("Error in CreateThread()");
    case EVERYTHING_ERROR_INVALIDINDEX:
         return ("Invalid index given");
    case EVERYTHING_ERROR_INVALIDCALL:
         return ("Invalid call");
  }
  snprintf (buf, sizeof(buf), "Unknown error %lu", err);
  return (buf);
}

static int do_check_evry (void)
{
  DWORD err, num, len, i;
  char  query [_MAX_PATH+8];
  char *dir   = NULL;
  char *base  = NULL;
  int   found = 0;

  /* EveryThing seems to need '\\' only. Must split the 'opt.file_spec'
   * into a 'dir' and 'base' part.
   */
  if (strpbrk(opt.file_spec, "/\\"))
  {
    dir  = dirname (opt.file_spec);   /* Allocates memory */
    base = basename (opt.file_spec);
  }

  /* If user didn't use the '-r/--regex' option, we must convert
   * 'opt.file_spec into' a RegExp compatible format.
   * E.g. "ez_*.py" -> "^ez_.*\.py$"
   */
  if (opt.use_regex)
       snprintf (query, sizeof(query), "regex:%s", opt.file_spec);
  else if (dir)
       snprintf (query, sizeof(query), "regex:%s\\\\%s", dir, base);
  else snprintf (query, sizeof(query), "regex:^%s$", translate_shell_pattern(opt.file_spec));

  DEBUGF (1, "Everything_SetSearch (\"%s\").\n", query);

  Everything_SetSearchA (query);
  Everything_SetMatchCase (0);       /* Ignore case of matches */
  Everything_QueryA (TRUE);

  FREE (dir);

  err = Everything_GetLastError();
  DEBUGF (1, "Everything_Query: %s\n", evry_strerror(err));

  if (err == EVERYTHING_ERROR_IPC)
  {
    WARN ("Everything IPC service is not running.\n");
    return (0);
  }

  num = Everything_GetNumResults();
  DEBUGF (1, "Everything_GetNumResults() num: %lu, err: %s\n",
          num, evry_strerror(Everything_GetLastError()));

  if (num == 0)
  {
    const char *fmt = opt.use_regex ?
                        "Nothing matched your regexp \"%s\".\n"
                        "Are you sure it is correct? Try quoting it.\n" :
                        "Nothing matched your search \"%s\".\n"
                        "Are you sure all NTFS disks are indexed by EveryThing? Try adding folders manually.\n";
    char buf [5000];

    snprintf (buf, sizeof(buf), fmt, opt.file_spec);
    WARN ("%s", buf);
    return (0);
  }

  /* Sort results by path (ignore case).
   */
  Everything_SortResultsByPath();

  for (i = 0; i < num; i++)
  {
    struct stat st;
    char   file [_MAX_PATH];
    time_t mtime = 0;
    __int64 fsize = 0;
    int    is_dir = 0;

    len = Everything_GetResultFullPathName (i, file, sizeof(file));
    err = Everything_GetLastError();
    if (len == 0 || err != EVERYTHING_OK)
    {
      DEBUGF (1, "Everything_GetResultFullPathName(), err: %s\n",
              evry_strerror(err));
      break;
    }

    if (stat(file, &st) == 0)
    {
      mtime  = st.st_mtime;
      fsize  = st.st_size;
      is_dir = _S_ISDIR (st.st_mode);
    }
    if (report_file(file, mtime, fsize, is_dir, HKEY_EVERYTHING))
       found++;
  }
  return (found);
}

/*
 * The main work-horse of this program.
 */
static int do_check_env (const char *env_name)
{
  struct directory_array *arr;
  int    found = 0;
  char  *orig_e;

  orig_e = getenv_expand (env_name);
  arr0   = orig_e ? split_env_var(env_name,orig_e) : NULL;
  if (!arr0)
  {
    DEBUGF (1, "Env-var %s not defined.\n", env_name);
    return (0);
  }

  for (arr = arr0; arr->dir; arr++)
      found += process_dir (arr->dir, arr->num_dup, arr->exist,
                            arr->is_dir, arr->exp_ok, env_name, NULL);
  free_dir_array();
  FREE (orig_e);
  return (found);
}

/*
 * Having several gcc compilers installed makes it nearly impossible to
 * set C_INCLUDE_PATH to the desired compiler's include-dir. So EnvTool
 * simply asks *gcc.exe for what it think is the include search-path.
 * Do that by spawning the *gcc.exe and parsing the include paths.
 */
static BOOL found_search_line = FALSE;
static int  found_index = 0;

static int find_include_path_cb (char *buf, int index)
{
  static char start[] = "#include <...> search starts here:";
  static char end[]   = "End of search list.";

  if (found_index >= DIM(dir_array))
  {
    WARN ("'dir_array[]' too small. Max %d\n", DIM(dir_array));
    return (-1);
  }

  if (!found_search_line && !memcmp(buf,&start,sizeof(start)-1))
  {
    found_search_line = TRUE;
    return (0);
  }

  if (found_search_line)
  {
    if (memcmp(buf,&end,sizeof(end)-1))  /* Not reached "End of search list" */
    {
      char  buf2 [_MAX_PATH];
      const char *p = _fixpath (str_trim(buf), buf2);

      add_to_dir_array (p, found_index++, !stricmp(g_current_dir,p));
      DEBUGF (2, "line: '%s'\n", p);
      return (1);
    }
    found_search_line = FALSE; /* got: "End of search list.". No more paths excepted. */
    return (-1);
  }
  ARGSUSED (index);
  return (0);
}

static int find_library_path_cb (char *buf, int index)
{
  static char prefix[] = "LIBRARY_PATH=";
  char   buf2 [_MAX_PATH];
  char   sep[2], *p, *tok, *rc;
  int    i;

  sep[0] = path_separator;
  sep[1] = 0;

  if (!strncmp(buf,prefix,sizeof(prefix)-1) && strlen(buf) > sizeof(prefix))
  {
    p = buf + sizeof(prefix) - 1;
    for (i = 0, tok = strtok(p,sep); tok; tok = strtok(NULL,sep), i++)
    {
      rc = _fixpath (tok, buf2);
      DEBUGF (2, "tok %d: '%s'\n", i, rc);
    }
    return (1);
  }
  ARGSUSED (index);
  return (0);
}

static int setup_gcc_includes (const char *gcc)
{
  char cmd [1000];
  int  found;

  /* We want the output of stderr only. But that seems impossible on CMD/4NT.
   * Hence redirect stderr + stdout into the same pipe for us to read.
   * Also assume that the '*gcc' is on PATH.
   */
  snprintf (cmd, sizeof(cmd), "%s -v -dM -c nul.c 2>&1", gcc);
  found_index = 0;
  found_search_line = FALSE;
  found = popen_run (cmd, find_include_path_cb);
  if (found > 0)
       DEBUGF (1, "found %d include paths for %s.\n", found, gcc);
  else WARN ("Calling %s failed.\n", gcc);
  return (found);
}

static int setup_gcc_library_path (const char *gcc)
{
  char cmd [1000];
  int  found;

  /* We want the output of stderr only. But that seems impossible on CMD/4NT.
   * Hence redirect stderr + stdout into the same pipe for us to read.
   * Also assume that the '*gcc' is on PATH.
   */
  snprintf (cmd, sizeof(cmd), "%s -v -dM -c nul.c 2>&1", gcc);
  found_index = 0;
  found_search_line = FALSE;
  found = popen_run (cmd, find_library_path_cb);
  if (found > 0)
       DEBUGF (1, "found %d library paths for %s.\n", found, gcc);
  else WARN ("Calling %s failed.\n", gcc);
  return (found);
}

/*
 * Check include-paths found above.
 */
static int process_gcc_includes (const char *gcc)
{
  struct directory_array *arr;
  int    found = 0;

  for (arr = dir_array; arr->dir; arr++)
      found += process_dir (arr->dir, arr->num_dup, arr->exist,
                            arr->is_dir, arr->exp_ok, gcc, NULL);

  for (arr = dir_array; arr->dir; arr++)
      FREE (arr->dir);
  return (found);
}


static const char *gcc[] = { "gcc.exe",
#if CHECK_PREFIXED_GCC
                             "x86_64-w64-mingw32-gcc.exe",
                             "i386-mingw32-gcc.exe",
                             "i686-w64-mingw32-gcc.exe"
#endif
                             /* Add more gcc programs here?
                              *
                              * Maybe we should use 'searchpath("*gcc.exe", "PATH")'
                              * to find all 'gcc.exe' programs?
                              */
                            };

static const char *gpp[] = { "g++.exe",
#if CHECK_PREFIXED_GCC
                             "x86_64-w64-mingw32-g++.exe",
                             "i386-mingw32-g++.exe",
                             "i686-w64-mingw32-g++.exe"
#endif
                            };

static const char *cl[] = { "cl.exe"
                          };

static const char *wcc[] = { "wcc386.exe",
                             "wpp386.exe",
                             "wccaxp.exe",
                             "wppaxp.exe"
                          };

static size_t longest_cc = 0;

static void get_longest (const char **cc, size_t num)
{
  size_t i, len;

  for (i = 0; i < num; i++)
  {
    len = strlen (cc[i]);
    if (len > longest_cc)
       longest_cc = len;
  }
}

static void searchpath_compilers (const char **cc, size_t num)
{
  const char *found;
  size_t i, len;

  for (i = 0; i < num; i++)
  {
    found = searchpath (cc[i], "PATH");
    len = strlen (cc[i]);
    C_printf ("    %s: %*s -> ~%c%s~0\n",
              cc[i], longest_cc-len, "",
              found ? '6' : '5', found ? found : "Not found");
  }
}

static void searchpath_gnu (void)
{
  get_longest (gcc, DIM(gcc));
  get_longest (gpp, DIM(gpp));
  searchpath_compilers (gcc, DIM(gcc));
  searchpath_compilers (gpp, DIM(gpp));
}

static void searchpath_msvc (void)
{
  get_longest (cl, DIM(cl));
  searchpath_compilers (cl, DIM(cl));
}

static void searchpath_watcom (void)
{
  get_longest (wcc, DIM(wcc));
  searchpath_compilers (wcc, DIM(wcc));
}

static int do_check_gcc_includes (void)
{
  char report [_MAX_PATH+50];
  int  i, found = 0;

  for (i = 0; i < DIM(gcc); i++)
      if (setup_gcc_includes(gcc[i]) > 0)
      {
        snprintf (report, sizeof(report), "Matches in %s %%C_INCLUDE_PATH%% path:\n", gcc[i]);
        report_header = report;
        found += process_gcc_includes (gcc[i]);
      }

  if (found == 0)  /* Impossible? */
     WARN ("No gcc.exe programs returned any include paths.\n");

  return (found);
}

static int do_check_gpp_includes (void)
{
  char report [_MAX_PATH+50];
  int  i, found = 0;

  for (i = 0; i < DIM(gpp); i++)
      if (setup_gcc_includes(gpp[i]) > 0)
      {
        snprintf (report, sizeof(report), "Matches in %s %%CPLUS_INCLUDE_PATH%% path:\n", gpp[i]);
        report_header = report;
        found += process_gcc_includes (gpp[i]);
      }

  if (found == 0)  /* Impossible? */
     WARN ("No g++.exe programs returned any include paths.\n");

  return (found);
}

static int do_check_gcc_library_paths (void)
{
  int found = 0;

  if (setup_gcc_library_path("gcc.exe") > 0)
  {
#if 0
    char report [200];
    snprintf (report, sizeof(report), "Matches in gcc %%LIBRARY_PATH%% path:\n");
    report_header = report;
    found += process_gcc_includes (gcc[i]);
#endif
  }
  if (found == 0)  /* Impossible? */
     WARN ("No gcc.exe programs returned any LIBRARY_PATH paths!?.\n");

  return (found);
}


/*
 * getopt_long() processing.
 */
static const struct option long_options[] = {
           { "help",    no_argument,       NULL, 'h' },
           { "help",    no_argument,       NULL, '?' },  /* 1 */
           { "version", no_argument,       NULL, 'V' },
           { "inc",     no_argument,       NULL, 0 },    /* 3 */
           { "path",    no_argument,       NULL, 0 },
           { "lib",     no_argument,       NULL, 0 },    /* 5 */
           { "python",  optional_argument, NULL, 0 },
           { "dir",     no_argument,       NULL, 'D' },  /* 7 */
           { "debug",   optional_argument, NULL, 'd' },
           { "no-sys",  no_argument,       NULL, 0 },    /* 9 */
           { "no-usr",  no_argument,       NULL, 0 },
           { "no-app",  no_argument,       NULL, 0 },    /* 11 */
           { "test",    no_argument,       NULL, 't' },
           { "quiet",   no_argument,       NULL, 'q' },  /* 13 */
           { "no-gcc",  no_argument,       NULL, 0 },
           { "no-g++",  no_argument,       NULL, 0 },    /* 15 */
           { "verbose", no_argument,       NULL, 'v' },
           { "pe-check",no_argument,       NULL, 0 },    /* 17 */
           { "color",   no_argument,       NULL, 'C' },
           { "evry",    no_argument,       NULL, 0 },    /* 19 */
           { "regex",   no_argument,       NULL, 0 },
           { "size",    no_argument,       NULL, 0 },    /* 21 */
           { NULL,      no_argument,       NULL, 0 }
         };

static int *values_tab[] = {
            NULL,
            NULL,                /* 1 */
            NULL,
            &opt.do_include,     /* 3 */
            &opt.do_path,
            &opt.do_lib,         /* 5 */
            &opt.do_python,
            &opt.dir_mode,       /* 7 */
            NULL,
            &opt.no_sys_env,     /* 9 */
            &opt.no_usr_env,
            &opt.no_app_path,    /* 11 */
            NULL,
            NULL,                /* 13 */
            &opt.no_gcc,
            &opt.no_gpp,         /* 15 */
            &opt.verbose,
            &opt.PE_check,       /* 17 */
            &use_colours,
            &opt.do_evry,        /* 19 */
            &opt.use_regex,
            &opt.show_size       /* 21 */
          };

static void set_python_variant (const char *o)
{
  DEBUGF (2, "optarg: '%s'\n", o);

  if (!o)
     which_python = DEFAULT_PYTHON;

  else if (!strcmp("py2",o))
     which_python = PY2_PYTHON;

  else if (!strcmp("py3",o))
     which_python = PY3_PYTHON;

  else if (!strcmp("ipy",o))
     which_python = IRON2_PYTHON;

  else if (!strcmp("ipy2",o))
     which_python = IRON2_PYTHON;

  else if (!strcmp("ipy3",o))
     which_python = IRON3_PYTHON;

  else if (!strcmp("pypy",o))
     which_python = PYPY_PYTHON;

  else if (!strcmp("jython",o))
     which_python = JYTHON_PYTHON;

  else if (!strcmp("all",o))
     which_python = ALL_PYTHONS;

  else
     usage ("Illegal '--python' option: '%s'\n", o);
}

static void set_short_option (int c)
{
  DEBUGF (2, "got short option '%c' (%d).\n", c, c);

  switch (c)
  {
    case 'h':
         opt.help = 1;
         break;
    case 'V':
         opt.do_version++;
         break;
    case 'v':
         opt.verbose++;
         break;
    case 'd':
         opt.debug++;
         break;
    case 'D':
         opt.dir_mode = 1;
         break;
    case 'c':
         opt.add_cwd = 0;
         break;
    case 'C':
         use_colours = 1;
         break;
    case 'r':
         opt.use_regex = 1;
         break;
    case 's':
         opt.show_size = 1;
         break;
    case 'T':
         opt.decimal_timestamp = 1;
         break;
    case 't':
         opt.do_test = 1;
         break;
    case 'u':
         opt.show_unix_paths = 1;
         break;
    case 'q':
         opt.quiet = 1;
         break;
    case '?':      /* '?' == BADCH || BADARG */
         usage ("  Use \"--help\" for options\n");
         break;
    default:
         usage ("Illegal option: '%c'\n", optopt);
         break;
  }
}

static void set_long_option (int o)
{
  int *val;

  ASSERT (values_tab[o]);
  ASSERT (long_options[o].name);
  DEBUGF (2, "got long option \"--%s\".\n", long_options[o].name);

  if (!strcmp("python",long_options[o].name))
     set_python_variant (optarg);

  else if (optarg && !strcmp("debug",long_options[o].name))
     opt.debug = atoi (optarg);

  val = values_tab [o];
#if 0
  *val ^= 1;
#else
  *val = 1;
#endif
}

static char *parse_args (int argc, char *const *argv)
{
  char  buf [_MAX_PATH];
  char *env = getenv_expand ("ENVTOOL_OPTIONS");
  char *ext;

  if (GetModuleFileName(NULL, buf, sizeof(buf)))
       who_am_I = STRDUP (buf);
  else who_am_I = STRDUP (argv[0]);

  program_name = who_am_I;

  ext = (char*) get_file_ext (who_am_I);
  strlwr (ext);

  if (env)
  {
    char *s = strtok (env, "\t ");
    int   i, j;

    memset (new_argv, '\0', sizeof(new_argv));
    new_argv[0] = STRDUP (argv[0]);
    for (i = 1; s && i < DIM(new_argv)-1; i++)
    {
      new_argv[i] = STRDUP (s);
      s = strtok (NULL, "\t ");
    }
    new_argc = i;

    for (j = new_argc, i = 1; i < argc && j < DIM(new_argv)-1; i++, j++, new_argc++)
       new_argv [j] = STRDUP (argv[i]);  /* allocate original into new_argv[] */

    if (new_argc == DIM(new_argv)-1)
       WARN ("Too many arguments (%d) in %%ENVTOOL_OPTIONS%%.\n", i);
    argc = new_argc;
    argv = new_argv;

    DEBUGF (3, "argc: %d\n", argc);
    for (i = 0; i < argc; i++)
        DEBUGF (3, "argv[%d]: \"%s\"\n", i, argv[i]);
    FREE (env);
  }

  while (1)
  {
    int opt_index = 0, c = getopt_long (argc, argv, "cChvVdDrstTuq", long_options, &opt_index);

    if (c == 0)
       set_long_option (opt_index);
    else if (c > 0)
       set_short_option (c);
    else if (c == -1)
       break;
  }

  init_python();

  if (opt.do_version > 0)
     show_version();

  if (argc < 2 || opt.help)
     show_help();

  if (argv[optind])
      return _strlcpy (file_spec_buf, argv[optind], sizeof(file_spec_buf));
   return (NULL);
}

#if defined(_MSC_VER) && defined(_DEBUG)
static _CrtMemState last_state;

static void crtdbug_init (void)
{
  _HFILE file  = _CRTDBG_FILE_STDERR;
  int    mode  = _CRTDBG_MODE_FILE;
  int    flags = _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_DELAY_FREE_MEM_DF;

  _CrtSetReportFile (_CRT_ASSERT, file);
  _CrtSetReportMode (_CRT_ASSERT, mode);
  _CrtSetReportFile (_CRT_ERROR, file);
  _CrtSetReportMode (_CRT_ERROR, mode);
  _CrtSetReportFile (_CRT_WARN, file);
  _CrtSetReportMode (_CRT_WARN, mode);
  _CrtSetDbgFlag (flags | _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG));
  _CrtMemCheckpoint (&last_state);
}

void crtdbg_exit (void)
{
  _CrtCheckMemory();
  if (opt.debug)
       _CrtMemDumpStatistics (&last_state);
  else _CrtMemDumpAllObjectsSince (&last_state);
  _CrtDumpMemoryLeaks();
}
#endif

static void cleanup (void)
{
  int i;

  /* If we're called from the ^C thread, don't do any Python stuff.
   * That will crash in Py_Finalize().
   */
  if (halt_flag == 0)
     exit_python();

  free_dir_array();
  FREE (who_am_I);

  FREE (system_env_path);
  FREE (system_env_lib);
  FREE (system_env_inc);

  FREE (user_env_path);
  FREE (user_env_lib);
  FREE (user_env_inc);

  for (i = 0; i < new_argc && i < DIM(new_argv)-1; i++)
      FREE (new_argv[i]);

  if (halt_flag == 0 && opt.debug > 0)
     mem_report();

#if defined(_MSC_VER) && defined(_DEBUG)
  crtdbg_exit();
#endif
}

/*
 * This signal-handler gets called in another thread.
 */
static void halt (int sig)
{
  extern HANDLE Everything_hthread;

  halt_flag++;

  if (opt.do_evry)
  {
    if (Everything_hthread && Everything_hthread != INVALID_HANDLE_VALUE)
    {
      TerminateThread (Everything_hthread, 1);
      CloseHandle (Everything_hthread);
    }
    Everything_hthread = INVALID_HANDLE_VALUE;
    Everything_Reset();
  }

  if (sig == SIGILL)
       C_puts ("\n~5Illegal instruction.~0\n");
  else C_puts ("~5Quitting.\n~0");

  cleanup();
  ExitProcess (GetCurrentProcessId());
}

static void init (void)
{
  atexit (cleanup);

#if defined(_MSC_VER) && defined(_DEBUG)
  crtdbug_init();
#endif

  tzset();
  memset (&opt, 0, sizeof(opt));
  opt.add_cwd = 1;

  g_current_dir[0] = '.';
  g_current_dir[1] = DIR_SEP;
  g_current_dir[2] = '\0';
  getcwd (g_current_dir, sizeof(g_current_dir));
}

int main (int argc, char **argv)
{
  int   found = 0;
  char *end, *ext;

  init();

  opt.file_spec = parse_args (argc, argv);

  /* Sometimes the IPC connection to the EveryThing Database will hang.
   * Clean up if user presses ^C.
   */
  signal (SIGINT, halt);

  if (opt.do_test)
  {
    do_tests();
    return (0);
  }

  if (opt.do_evry && !opt.do_path)
     opt.no_sys_env = opt.no_usr_env = opt.no_app_path = 1;

  if (!opt.do_path && !opt.do_include && !opt.do_lib && !opt.do_python && !opt.do_evry)
     usage ("Use at least one of; \"--inc\", \"--lib\", \"--evry\", \"--python\" and/or \"--path\".\n");

  if (!opt.file_spec)
     usage ("You must give a ~1filespec~0 to search for.\n");

  if (strchr(opt.file_spec,'~') > opt.file_spec)
     opt.file_spec = _fixpath (opt.file_spec, file_spec_buf);

  end = strchr (opt.file_spec, '\0');
  ext = (char*) get_file_ext (opt.file_spec);

  if (!opt.use_regex && end[-1] != '*' && end[-1] != '$' && *ext == '\0')
  {
    end[0] = '.';
    end[1] = '*';
    end[2] = '\0';
  }

  DEBUGF (1, "file_spec: %s\n", opt.file_spec);

  /* Scan registry values unless only '--python' is specified.
   */
  if (!opt.do_python && !(opt.do_path && opt.do_lib && opt.do_include))
  {
    if (!opt.no_sys_env)
       found += scan_system_env();

    if (!opt.no_usr_env)
       found += scan_user_env();
  }

  if (opt.do_path)
  {
    if (!opt.no_app_path)
       found += do_check_registry();

    report_header = "Matches in %PATH:\n";
    found += do_check_env ("PATH");
  }

  if (opt.do_lib)
  {
    report_header = "Matches in %LIB:\n";
    found += do_check_env ("LIB");
    if (!opt.no_gcc && !opt.no_gpp)
    {
      report_header = "Matches in %LIBRARY_PATH:\n";
      found += do_check_gcc_library_paths();
      found += do_check_env ("LIBRARY_PATH");
    }
  }

  if (opt.do_include)
  {
    report_header = "Matches in %INCLUDE:\n";
    found += do_check_env ("INCLUDE");

    if (!opt.no_gcc)
       found += do_check_gcc_includes();

    if (!opt.no_gpp)
       found += do_check_gpp_includes();
  }

  if (opt.do_python)
  {
    char        report [_MAX_PATH+50];
    const char *py_exe = NULL;

    get_python_info (&py_exe, NULL, NULL, NULL, NULL);
    snprintf (report, sizeof(report), "Matches in \"%s\" sys.path[]:\n", py_exe);
    report_header = report;
    found += do_check_python();
  }

  if (opt.do_evry)
  {
    report_header = "Matches from EveryThing:\n";
    found += do_check_evry();
  }

  final_report (found);
  return (0);
}

/*
 * Returns the expanded version of an environment variable.
 * Stolen from curl. But I wrote the Win32 part of it...
 *
 * E.g. If "INCLUDE=c:\VC\include;%C_INCLUDE_PATH%" and
 * "C_INCLUDE_PATH=c:\MingW\include", the expansion returns
 * "c:\VC\include;c:\MingW\include".
 *
 * Note: Windows (cmd only?) requires a trailing '%' in
 *       "%C_INCLUDE_PATH".
 */
char *getenv_expand (const char *variable)
{
  const char *orig_var = variable;
  char *rc, *env = NULL;
  char  buf1 [MAX_ENV_VAR], buf2 [MAX_ENV_VAR];
  DWORD ret;

  /* Don't use getenv(); it doesn't find variable added after program was
   * started. Don't accept truncated results (i.e. rc >= sizeof(buf1)).
   */
  ret = GetEnvironmentVariable (variable, buf1, sizeof(buf1));
  if (ret > 0 && ret < sizeof(buf1))
  {
    env = buf1;
    variable = buf1;
  }
  if (strchr(variable,'%'))
  {
    /* buf2 == variable if not expanded.
     */
    ret = ExpandEnvironmentStrings (variable, buf2, sizeof(buf2));
    if (ret > 0 && ret < sizeof(buf2) &&
        !strchr(buf2,'%'))    /* no variables still un-expanded */
      env = buf2;
  }

  rc = (env && env[0]) ? STRDUP(env) : NULL;
  DEBUGF (1, "env: '%s', expanded: '%s'\n", orig_var, rc);
  return (rc);
}

/*
 * Some test functions.
 */
void test_split_env (const char *env)
{
  struct directory_array *arr, *arr0;
  char  *value;
  int    i;

  C_printf ("\n~3%s():~0 ", __FUNCTION__);
  C_printf (" 'split_env_var (\"%s\",\"%%%s\")':\n", env, env);

  value = getenv_expand (env);
  arr0  = split_env_var (env, value);

  for (arr = arr0, i = 0; arr->dir; i++, arr++)
  {
    char *dir = arr->dir;
    char  buf [_MAX_PATH];

    if (arr->exist && arr->is_dir)
       dir = _fixpath (dir, buf);

    if (opt.show_unix_paths)
       dir = slashify (dir, '/');

    C_printf ("  arr[%2d]: %s", i, dir);

    if (arr->num_dup > 0)
       C_puts ("  ~3**duplicated**~0");
    if (!arr->exist)
       C_puts ("  ~5**not existing**~0");
    if (!arr->is_dir)
       C_puts ("  **not a dir**");
    C_putc ('\n');
  }
  C_printf ("  ~3%d elements~0\n", i);

  for (arr = arr0; arr->dir; arr++)
      FREE (arr->dir);
  FREE (value);
}


#ifdef __CYGWIN__

void test_split_env_cygwin (const char *env)
{
  struct directory_array *arr, *arr0;
  char  *value, *cyg_value;
  int    i, rc, needed, save = opt.conv_cygdrive;

  C_printf ("\n~3%s():~0 ", __FUNCTION__);
  C_printf (" testing 'split_env_var (\"%s\",\"%%%s\")':\n", env, env);

  value  = getenv_expand (env);
  needed = cygwin_conv_path_list (CCP_WIN_A_TO_POSIX, value, NULL, 0);
  cyg_value = alloca (needed+1);
  DEBUGF (2, "cygwin_conv_path_list(): needed %d\n", needed);

  rc = cygwin_conv_path_list (CCP_WIN_A_TO_POSIX, value, cyg_value, needed+1);
  DEBUGF (2, "cygwin_conv_path_list(): rc: %d, '%s'\n", rc, cyg_value);

  path_separator = ':';
  opt.conv_cygdrive = 0;
  arr0 = split_env_var (env, cyg_value);

  for (arr = arr0, i = 0; arr->dir; i++, arr++)
  {
    char *dir = arr->dir;

    if (arr->exist && arr->is_dir)
       dir = cygwin_create_path (CCP_WIN_A_TO_POSIX, dir);

    C_printf ("  arr[%d]: %s", i, dir);

    if (arr->num_dup > 0)
       C_puts ("  ~4**duplicated**~0");
    if (!arr->exist)
       C_puts ("  ~0**not existing**~0");
    if (!arr->is_dir)
       C_puts ("  ~4**not a dir**~0");
    C_putc ('\n');

    if (dir != arr->dir)
       free (dir);
  }
  C_printf ("~0  %d elements\n", i);

  for (arr = arr0; arr->dir; arr++)
      FREE (arr->dir);
  FREE (value);

  path_separator = ';';
  opt.conv_cygdrive = save;
}
#endif

/*
 * Tests for searchpath().
 */
struct test_table1 {
       const char *file;
       const char *env;
     };

static const struct test_table1 tab1[] = {
                  { "kernel32.dll",      "PATH" },
                  { "notepad.exe",       "PATH" },
                  { "./envtool.c",       "FOO-BAR" },      /* CWD should always be at pos 0 regarless of env-var. */
                  { "msvcrt.lib",        "LIB" },
                  { "libgc.a",           "LIBRARY_PATH" },  /* TDM-MingW doesn't have this */
                  { "libgmon.a",         "LIBRARY_PATH" },
                  { "stdio.h",           "INCLUDE" },
                  { "os.py",             "PYTHONPATH" },

                  /* test if _fixpath() works for SFN. (%WinDir\systems32\PresentationHost.exe).
                   * SFN seems not to be available on Win-7+.
                   * "PRESEN~~1.EXE" = "PRESEN~1.EXE" since C_printf() is used.
                   */
                  { "PRESEN~~1.EXE",      "PATH" },

                  /* test if _fixpath() works with "%WinDir%\sysnative" on Win-7+
                   */
                  { "NDIS.SYS",          "%WinDir%\\sysnative\\drivers" },

                  { "c:\\NTLDR",         "c:\\" },  /* test if searchpath() finds hidden files. (Win-XP) */
                  { "c:\\BOOTMGR",       "c:\\" },  /* test if searchpath() finds hidden files. (Win-8+) */
                  { "c:\\BOOTMGR",       "" },      /* test if searchpath() handles non-env-vars too. */
                  { "\\\\localhost\\$C", "PATH" },  /* Does it work on a share too? */
                  { "CLOCK$",            "PATH" },  /* Does it handle device names? */
                  { "PRN",               "PATH" }
                };

static void test_searchpath (void)
{
  const struct test_table1 *t;
  size_t len, i = 0;

  C_printf ("\n~3%s():~0\n", __FUNCTION__);

  for (t = tab1; i < DIM(tab1); t++, i++)
  {
    const char *found = searchpath (t->file, t->env);
    char  buf [_MAX_PATH];

    len = strlen (t->file);
    if (strstr(t->file,"~~"))
       len--;
#if 0
    if (found)
       found = _fixpath (found, buf);
#endif
    C_printf ("  %s:%*s -> %s, pos: %d\n",
              t->file, 15-len, "", found ? found : strerror(errno), searchpath_pos());
  }
}

struct test_table2 {
       int         expect;
       const char *pattern;
       const char *fname;
       int         flags;
     };

static struct test_table2 tab2[] = {
         /* 0 */  { FNM_MATCH,   "bar*",         "barney.txt",     0 },
         /* 1 */  { FNM_MATCH,   "Bar*",         "barney.txt",     0 },
         /* 2 */  { FNM_MATCH,   "foo/Bar*",     "foo/barney.txt", 0 },
         /* 3 */  { FNM_MATCH,   "foo/bar*",     "foo/barney.txt", FNM_FLAG_PATHNAME },
         /* 4 */  { FNM_MATCH,   "foo\\bar*",    "foo/barney.txt", FNM_FLAG_PATHNAME },
         /* 5 */  { FNM_MATCH,   "foo\\*",       "foo\\barney",    FNM_FLAG_NOESCAPE| FNM_FLAG_PATHNAME },
         /* 6 */  { FNM_MATCH,   "foo\\*",       "foo\\barney",    0 },
         /* 7 */  { FNM_NOMATCH, "mil[!k]-bar*", "milk-bar",       0 },
         /* 8 */  { FNM_MATCH,   "mil[!k]-bar*", "milf-bar",       0 },
         /* 9 */  { FNM_MATCH,   "mil[!k]-bar?", "milf-barn",      0 },
                };

/*
 * Tests for fnmatch().
 */
static void test_fnmatch (void)
{
  struct test_table2 *t;
  size_t len1, len2;
  int    rc, i = 0;

  C_printf ("\n~3%s():~0\n", __FUNCTION__);

  for (t = tab2; i < DIM(tab2); t++, i++)
  {
    t->flags |= FNM_FLAG_NOCASE;
    rc   = fnmatch (t->pattern, t->fname, t->flags);
    len1 = strlen (t->pattern);
    len2 = strlen (t->fname);

    C_puts (rc == t->expect ? "~2  OK  ~0" : "~5  FAIL~0");

    C_printf (" fnmatch (\"%s\", %*s \"%s\", %*s 0x%02X): %s\n",
              t->pattern, 15-len1, "", t->fname, 15-len2, "",
              t->flags, fnmatch_res(rc));
  }
}

/*
 * Tests for slashify().
 */
static void test_slashify (void)
{
  const char *files1[] = {
              "c:\\bat\\foo.bat",
              "c:\\\\foo\\\\bar\\",
              "c:\\//Windows\\system32\\drivers\\etc\\hosts",
            };
  const char *files2[] = {
              "c:/bat/foo.bat",
              "c:///foo//bar//",
              "c:\\/Windows/system32/drivers/etc\\hosts"
            };
  const char *f, *rc;
  int   i;

  C_printf ("\n~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(files1); i++)
  {
    f = files1 [i];
    rc = slashify (f, '/');
    C_printf ("  (\"%s\",'/') %*s -> %s\n", f, (int)(39-strlen(f)), "", rc);
  }
  for (i = 0; i < DIM(files2); i++)
  {
    f = files2 [i];
    rc = slashify (f, '\\');
    C_printf ("  (\"%s\",'\\\\') %*s -> %s\n", f, (int)(38-strlen(f)), "", rc);
  }
}

/*
 * Tests for _fixpath().
 * Canonize the horrendous pathnames reported from "gcc -v".
 * It doesn't matter if these paths or files exists or not. _fixpath()
 * (i.e. GetFullPathName()) should canonizes these regardless.
 */
static void test_fixpath (void)
{
  const char *files[] = {
    "f:\\mingw32\\bin\\../lib/gcc/x86_64-w64-mingw32/4.8.1/include",                              /* exists here */
    "f:\\mingw32\\bin\\../lib/gcc/x86_64-w64-mingw32/4.8.1/include\\ssp\\ssp.h",                  /* exists here */
    "f:\\mingw32\\bin\\../lib/gcc/i686-w64-mingw32/4.8.1/../../../../i686-w64-mingw32/include",   /* exists here */
    "c:\\mingw32\\bin\\../lib/gcc/i686-w64-mingw32/4.8.1/../../../../i686-w64-mingw32/include",   /* doesn't exist here */
  };
  const char *f;
  char *rc1;
  int   i, rc2, rc3;

  C_printf ("\n~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(files); i++)
  {
    struct stat st;
    char   buf [_MAX_PATH];
    BOOL   is_dir;

    f = files [i];
    rc1 = _fixpath (f, buf);
    rc2 = FILE_EXISTS (buf);
    rc3 = (stat(rc1, &st) == 0);
    is_dir = (rc3 && _S_ISDIR(st.st_mode));

    if (opt.show_unix_paths)
       rc1 = slashify (buf, '/');

    C_printf ("  _fixpath (\"%s\")\n     -> \"%s\" ", f, rc1);
    if (!rc2)
         C_printf ("~5exists 0, is_dir %d~0\n\n", is_dir);
    else C_printf ("exists 1, is_dir %d~0\n\n", is_dir);
  }
}

/*
 * https://msdn.microsoft.com/en-us/library/windows/desktop/bb762181%28v=vs.85%29.aspx
 */
static void test_SHGetFolderPath (void)
{
#if 0
  char dirr [MAX_PATH];
  int rc;

  rc = SHGetFolderPath (NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, dir);
  if (rc != S_OK) ;
#endif
}

/*
 * When run as:
    gdb -args envtool.exe -t

  the output is something like:

  test_libssp():
  10:    0000: 00 00 00 00 00 00 00 00-00 00                   ..........
  10:    0000: 48 65 6C 6C 6F 20 77 6F-72 6C                   Hello worl
  *** stack smashing detected ***:  terminated

  Program received signal SIGILL, Illegal instruction.
  0x68ac12d4 in ?? () from f:\MingW32\bin\libssp-0.dll
  (gdb) bt
  #0  0x68ac12d4 in ?? () from f:\MingW32\bin\libssp-0.dll
  #1  0x68ac132e in libssp-0!__stack_chk_fail () from f:\MingW32\bin\libssp-0.dll
  #2  0x0040724f in _fu117____stack_chk_guard () at envtool.c:2518
  #3  0x004072eb in _fu118____stack_chk_guard () at envtool.c:2552
  #4  0x0040653d in _fu100____stack_chk_guard () at envtool.c:2081
  (gdb)
 */
static void test_libssp (void)
{
#if defined(_FORTIFY_SOURCE) && (_FORTIFY_SOURCE > 0)
   static const char buf1[] = "Hello world.\n\n";
   char buf2 [sizeof(buf1)-2] = { 0 };

   C_printf ("\n~3%s():~0\n", __FUNCTION__);

   hex_dump (&buf1, sizeof(buf1));
   memcpy (buf2, buf1, sizeof(buf1));

#if 0
   C_printf (buf2);   /* vulnerable data */
   C_flush();
#endif

   hex_dump (&buf2, sizeof(buf2));
#endif
}

static void do_tests (void)
{
  if (opt.do_python)
  {
    if (/* test_python_pipe() && */ !halt_flag)
    {
      test_pythons();
      test_python_funcs();
    }
    return;
  }

  test_split_env ("PATH");
  test_split_env ("MANPATH");

#if 1
  test_split_env ("LIB");
  test_split_env ("INCLUDE");

  putenv ("FOO=c:\\");
  test_split_env ("FOO");
#endif

#ifdef __CYGWIN__
  test_split_env_cygwin ("PATH");
#endif

  test_searchpath();
  test_fnmatch();
  test_slashify();
  test_fixpath();
  test_SHGetFolderPath();

  signal (SIGILL, halt);
  test_libssp();
}

/*
 * Function that prints the line argument while limiting it
 * to at most 'MAX_CHARS_PER_LINE'. An appropriate number
 * of spaces are added on subsequent lines.
 *
 * Stolen from Wget (main.c) and simplified.
 */
#define MAX_CHARS_PER_LINE 80
#define TABULATION         4

static void format_and_print_line (const char *line)
{
  char *token, *line_dup = strdup (line);
  int   remaining_chars = 0;

  /* We break on spaces.
   */
  token = strtok (line_dup, " ");
  while (token)
  {
    /* If a token is much larger than the maximum
     * line length, we print the token on the next line.
     */
    if (remaining_chars <= (int)strlen(token))
    {
      C_printf ("\n%*c", TABULATION, ' ');
      remaining_chars = MAX_CHARS_PER_LINE - TABULATION;
    }
    C_printf ("%s ", token);
    remaining_chars -= strlen (token) + 1;  /* account for " " */
    token = strtok (NULL, " ");
  }
  C_putc ('\n');
  free (line_dup);
}

#if defined(__MINGW32__)
  #define CFLAGS   "cflags_MingW.h"
  #define LDFLAGS  "ldflags_MingW.h"

#elif defined(_MSC_VER)
  #define CFLAGS   "cflags_MSVC.h"
  #define LDFLAGS  "ldflags_MSVC.h"

#elif defined(__CYGWIN__)
  #define CFLAGS   "cflags_CygWin.h"
  #define LDFLAGS  "ldflags_CygWin.h"

#elif defined(__WATCOMC__)
  #define CFLAGS   "cflags_Watcom.h"
  #define LDFLAGS  "ldflags_Watcom.h"
#endif

static void print_build_cflags (void)
{
#if defined(CFLAGS)
  #include CFLAGS
  format_and_print_line (cflags);
#else
  format_and_print_line ("Unknown");
#endif
}

static void print_build_ldflags (void)
{
#if defined(LDFLAGS)
  #include LDFLAGS
  format_and_print_line (ldflags);
#else
  format_and_print_line ("Unknown");
#endif
}
