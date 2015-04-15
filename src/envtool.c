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

/* The file-spec we're searching for.
 */
char *file_spec = NULL;

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
       char  *fname;        /* basename of this entry. I.e. the name of the enumerated key. */
       char  *real_fname;   /* normally the same as above unless aliased. E.g. "winzip.exe -> "winzip32.exe" */
       char  *path;         /* path of this entry */
       int    exist;        /* does it exist? */
       time_t mtime;        /* file modification time */
       HKEY   key;
     };

struct directory_array dir_array [MAX_PATHS];
struct registry_array  reg_array [MAX_PATHS];

int  show_unix_paths = 0;
int  decimal_timestamp = 0;
int  no_sys_env = 0;
int  no_usr_env = 0;
int  no_app_path = 0;
int  quiet = 0;
int  use_regex = 0;
int  debug = 0;
int  verbose = 0;
int  add_cwd = 1;
int  dir_mode = 0;
int  PE_check = 0;
int  do_test = 0;
int  do_help = 0;
int  no_gcc = 0;
int  no_gpp = 0;
int  do_evry = 0;
int  num_version_ok = 0;

static char *who_am_I = (char*) "envtool";

static char *system_env_path = NULL;
static char *system_env_lib  = NULL;
static char *system_env_inc  = NULL;

static char *user_env_path   = NULL;
static char *user_env_lib    = NULL;
static char *user_env_inc    = NULL;
static char *report_header   = NULL;

static char *new_argv [MAX_ARGS];  /* argv[1...] + contents of "%ENVTOOL_OPTIONS" allocated here */
static int   new_argc;             /* 1... to highest allocated cmd-line component */

static int   do_path     = 0;
static int   do_lib      = 0;
static int   do_include  = 0;
static int   do_python   = 0;
static int   conv_cygdrive  = 1;
static int   path_separator = ';';
static char  file_spec_buf [_MAX_PATH];
static char  g_current_dir [_MAX_PATH];

static volatile int halt_flag;

static void  usage (const char *format, ...) ATTR_PRINTF(1,2);
static void  do_tests (void);


/*
 * \todo: Check if the same dir is listed multiple times in $PATH, $INC or $LIB.
 *        Warn if this is the case.
 *
 * \todo: Check if a file (in $PATH, $INC or $LIB) is shadowed by
 *        an newer file of the same name. Warn if this is the case.
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

  printf ("  Everything search engine ver. %ld.%ld.%ld.%ld (c) David Carpenter.\n",
          major, minor, revision, build);

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
  HWND wnd = FindWindow (EVERYTHING_IPC_WNDCLASS, 0);
  const char *py_exe;
  int         py_ver_major, py_ver_minor, py_ver_micro;
  int         py = get_python_version (&py_exe, &py_ver_major, &py_ver_minor, &py_ver_micro);

  C_printf ("%s.\n  Version ~3%s ~1(%s, %s)~0 by %s. %s~0\n",
          who_am_I, VER_STRING, BUILDER, WIN_VERSTR, AUTHOR_STR,
          is_wow64_active() ? "~1WOW64." : "");

  if (wnd)
       show_evry_version (wnd);
  else printf ("  Everything search engine not found\n");

  if (py)
       printf ("  Python %d.%d.%d detected (%s).\n", py_ver_major, py_ver_minor, py_ver_micro, py_exe);
  else printf ("  Python not found.\n");
  exit (0);
}

static void usage (const char *format, ...)
{
  if (format)
  {
    va_list args;
    va_start (args, format);
    C_vprintf (format, args);
    va_end (args);
    exit (-1);
  }

  C_printf ("Environment check & search tool.\n"
            "%s.\n\n"
            "Usage: %s [-cdDhitTqpuV?] <--mode> <file-spec>\n"
            "  --path:         check and search in %%PATH.\n"
            "  --python:       check and search in %%PYTHONPATH and 'sys.path[]'.\n"
            "  --lib:          check and search in %%LIB and %%LIBRARY_PATH.\n"
            "  --inc:          check and search in %%INCLUDE and paths returned by gcc/g++.\n"
            "  --evry:         check and search in the EveryThing database.\n"
            "  --no-gcc        do not run 'gcc' to check include paths (check %%C_INCLUDE_PATH instead).\n"
            "  --no-g++        do not run 'g++' to check include paths (check %%CPLUS_INCLUDE_PATH instead).\n"
            "  --no-sys        do not scan 'HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment'.\n"
            "  --no-usr        do not scan 'HKCU\\Environment'.\n"
            "  --no-app        do not scan 'HKCU\\" REG_APP_PATH "' and\n"
            "                              'HKLM\\" REG_APP_PATH "'.\n"
            "  --pe-check:     print checksum and version-info for PE-files.\n"
            "  -c:             don't add current directory to search-list.\n"
            "  -C, --color:    print using colours to Windows console.\n"
            "  -d, --debug:    set debug level (-dd==%%PYTHONVERBOSE=1).\n"
            "  -D, --dir:      looks only for directories matching \"file-spec\".\n"
            "  -h, -?:         show this help.\n", AUTHOR_STR, who_am_I);

  C_printf ("  -r, --regex:    enable \"regex\" in '--evry' searches.\n"
            "  -q, --quiet:    disable warnings.\n"
            "  -t:             do some internal tests.\n"
            "  -T:             show file times in sortable decimal format.\n"
            "  -u:             show all paths on Unix format: 'c:/ProgramFiles/'.\n"
            "  -v:             increase verbose level (currently only used '--pe-check').\n"
            "  -V:             show program version information.\n"
            "\n"
            "  Commonly used options can be put in %%ENVTOOL_OPTIONS. Those\n"
            "  are parsed before the command-line.\n"
            "\n"
            "  The '--evry' option requires that the Everything program is installed.\n"
            "  Ref. http://www.voidtools.com/support/everything/\n"
            "\n"
            "  'file-spec' accepts Posix ranges. E.g. \"[a-f]*.txt\".\n"
            "  'file-spec' matches both files and directories. If \"--dir\" is used, only\n"
            "   those directories are reported.\n");
  exit (-1);
}

static void show_help (void)
{
  usage (NULL);
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
 *  - 'i':       the array-index to store to.
 *  - 'top_key': the key the entry came from: HKEY_CURRENT_USER or HKEY_LOCAL_MACHINE.
 *  - 'fname':   the result from 'RegEnumKeyEx()'; name of each key.
 *  - 'fqdn':    the result from 'enum_sub_values()'. This value includes the full path.
 *
 * Note: 'basename (fqdn)' may NOT be equal to 'fname' (aliasing). That's the reason
 *       we store 'real_fname' too.
 */
static void add_to_reg_array (int i, HKEY key, const char *fname, const char *fqdn)
{
  struct registry_array *reg = reg_array + i;
  struct stat st;
  const  char *base;
  char   path [_MAX_PATH];
  size_t len;
  int    rc;

  assert (fname);
  assert (fqdn);
  assert (i >= 0);
  assert (i < DIM(reg_array));

  memset (&st, '\0', sizeof(st));
  base = basename (fqdn);
  if (base == fqdn)
  {
    DEBUGF (1, "fqdn (%s) has no '\\' or '/'\n", fqdn);
    return;
  }

  len = base - fqdn;
  rc  = stat (fqdn, &st);
  reg->mtime      = st.st_mtime;
  reg->fname      = STRDUP (fname);
  reg->real_fname = STRDUP (base);
  reg->path       = STRDUP (_strlcpy(path,fqdn,len));
  reg->exist      = (rc == 0) && FILE_EXISTS (fqdn);
  reg->key        = key;
}

/*
 * `Sort the 'reg_array' on 'path' + 'real_fname'.
 */
#define DO_SORT  0

#if DO_SORT
  typedef int (*CmpFunc) (const void *, const void *);

  static int reg_array_compare (const struct registry_array *a,
                                const struct registry_array *b)
  {
    char fqdn_a [_MAX_PATH];
    char fqdn_b [_MAX_PATH];
    int  slash = (show_unix_paths ? '/' : '\\');

    snprintf (fqdn_a, sizeof(fqdn_a), "%s%c%s", slashify(a->path, slash), slash, a->real_fname);
    snprintf (fqdn_b, sizeof(fqdn_b), "%s%c%s", slashify(b->path, slash), slash, b->real_fname);

    return stricmp (fqdn_a, fqdn_b);
  }
#endif

static void sort_reg_array (int num)
{
#if DO_SORT
  int i, slash = (show_unix_paths ? '/' : '\\');

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
 * 'add_cwd' is TRUE.
 *
 * Convert CygWin style paths to Windows paths: "/cygdrive/x/.." -> "x:/.."
 */
static struct directory_array *split_env_var (const char *env_name, const char *value)
{
  char *tok, *val = STRDUP (value);
  int   is_cwd, i;
  char  sep [2];

  sep [0] = path_separator;
  sep [1] = 0;

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
  if (add_cwd && !is_cwd)
     add_to_dir_array (g_current_dir, i++, 1);

  for ( ; i < DIM(dir_array)-1 && tok; i++)
  {
    /* Remove trailing '\\' or '/' from environment component
     * unless it's a "c:\".
     */
    char *p, *end = strchr (tok, '\0');

    if (end > tok+3 && (end[-1] == '\\' || end[-1] == '/'))
       end[-1] = '\0';

    if (!quiet)
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

    /* stat(".") doesn't work. Hence turn "." into 'g_current_dir'.
     */
    is_cwd = !strcmp(tok,".") || !strcmp(tok,".\\") || !strcmp(tok,"./");
    if (is_cwd)
    {
      if (i > 0)
         WARN ("Having \"%s\" not first in \"%s\" is asking for trouble.\n",
               tok, env_name);
      tok = g_current_dir;
    }
    else if (conv_cygdrive && strlen(tok) >= 12 && !strnicmp(tok,"/cygdrive/",10))
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

void report_file (const char *file, time_t mtime, BOOL is_dir, HKEY key)
{
  const struct tm *tm = localtime (&mtime);
  const char      *note = NULL;
  const char      *filler = "      ";
  char             time [30] = "?";
  int              raw;
  struct ver_info ver;
  BOOL   chksum_ok = FALSE;
  BOOL   version_ok = FALSE;

  memset (&ver, 0, sizeof(ver));

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

  if (!is_dir && dir_mode)
     return;

  /* Add a slash to end of a directory.
   */
  if (is_dir)
  {
    char *end = (char*)file + strlen (file);

    if (end > file && end[-1] != '\\' && end[-1] != '/')
    {
      *end++ = show_unix_paths ? '/' : '\\';
      *end = '\0';
    }
  }
  else if (PE_check && check_if_pe(file))
  {
    chksum_ok  = verify_pe_checksum (file);
    version_ok = show_version_info (file, &ver);
    if (version_ok)
       num_version_ok++;
  }

  if (decimal_timestamp)
       strftime (time, sizeof(time), "%Y%m%d.%H%M%S", tm);
  else strftime (time, sizeof(time), "%d %b %Y - %H:%M:%S", tm);

  if (key != HKEY_PYTHON_EGG)
  {
    char buf [_MAX_PATH];
    _fixpath (file, buf);  /* Has '\\' slashes */
    file = buf;
    if (show_unix_paths)
       file = slashify (file, '/');
  }

  if (report_header)
     C_printf ("~3%s~0", report_header);
  report_header = NULL;

  C_printf ("~3%s~0%s: ", note ? note : filler, time);

  raw = C_setraw (1);  /* In case 'file' contains a "~" (SFN), switch to raw mode */
  C_puts (file);
  C_setraw (raw);

  if (PE_check)
    C_printf ("\n~5%sver %u.%u.%u.%u~0, Chksum %s",
              filler, ver.val_1, ver.val_2, ver.val_3, ver.val_4,
              chksum_ok ? "OK" : "fail");
  C_putc ('\n');
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
    do_warn = quiet ? FALSE : TRUE;
  }

#if 1
  if (do_warn || found_in_python_egg)
     C_putc ('\n');
#endif

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
              "  ~5The search found matches outside the default environment (%%PATH etc.).\n"
              "  Hence running an application from the Start-Button may result in different .EXE/.DLL\n"
              "  to be loaded than from the command-line. Revise the above registry-keys.\n\n~0");

  C_printf ("%d match%s found for \"%s\".",
            found, (found == 0 || found > 1) ? "es" : "", file_spec);

  if (PE_check)
     C_printf (" %d have PE-version info.", num_version_ok);

  C_putc ('\n');
}

/*
 * Check for suffix or trailing wildcards. If not found, add a
 * trailing "*".
 *
 * If 'file_spec' starts with a subdir(s) part, return that in
 * '*sub_dir' with a trailing DIR_SEP. And return a 'fspec'
 * without the sub-dir part.
 */
static char *fix_filespec (char **sub_dir)
{
  static char fname [_MAX_PATH];
  static char subdir [_MAX_PATH];
  char  *p, *fspec = _strlcpy (fname, file_spec, sizeof(fname));
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
    *sub_dir = _strlcpy (subdir, fspec, p-fspec+1);
    fspec = p;
  }

 /*
  * Since FindFirstFile() doesn't work with POSIX ranges, replace
  * the range part in 'fspec' with a '*'. This could leave a '**' in
  * 'fspec', but that doesn't hurt.
  * Note: we still must use 'file_spec' in 'fnmatch()' for a POSIX
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
                                  ADD_VALUE (KEY_WOW64_32KEY),
                                  ADD_VALUE (KEY_WOW64_64KEY)
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

#if IS_WIN64
  access |= KEY_WOW64_32KEY;
#else
  if (!init)
     is_wow64 = is_wow64_active();
  if (is_wow64)
     access |= KEY_WOW64_64KEY;
#endif

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

  DEBUGF (1, "  RegOpenKeyEx (%s\\%s, %s):\n\t\t  %s\n",
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
           DEBUGF (1, "    num: %lu, REG_QWORD, value: \"%s\", data: " S64_FMT "\n",
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
static int build_registry_array (HKEY top_key, const char *app_path)
{
  HKEY   key = NULL;
  int    num, idx = 0;
  REGSAM acc = read_access();
  DWORD  rc  = RegOpenKeyEx (top_key, app_path, 0, acc, &key);

  DEBUGF (1, "  RegOpenKeyEx (%s\\%s, %s):\n                   %s\n",
          top_key_name(top_key), app_path, access_name(acc), win_strerror(rc));

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

    snprintf (sub_key, sizeof(sub_key), "%s\\%s", app_path, fname);

    if (enum_sub_values(top_key,sub_key,&fqdn))
       add_to_reg_array (idx++, top_key, fname, fqdn);

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

  DEBUGF (1, "RegOpenKeyEx (%s\\%s, %s):\n\t\t %s\n",
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

  if (do_path && system_env_path)
     found += do_check_env2 (HKEY_LOCAL_MACHINE_SESSION_MAN, "System PATH", system_env_path);

  if (do_include && system_env_inc)
     found += do_check_env2 (HKEY_LOCAL_MACHINE_SESSION_MAN, "System INCLUDE", system_env_inc);

  if (do_lib && system_env_lib)
     found += do_check_env2 (HKEY_LOCAL_MACHINE_SESSION_MAN, "System LIB", system_env_lib);

  return (found);
}

static int scan_user_env (void)
{
  int found = 0;

  report_header = "Matches in HKCU\\Environment:\n";

  scan_reg_environment (HKEY_CURRENT_USER, "Environment",
                        &user_env_path, &user_env_inc, &user_env_lib);

  if (do_path && user_env_path)
     found += do_check_env2 (HKEY_CURRENT_USER_ENV, "User PATH", user_env_path);

  if (do_include && user_env_inc)
     found += do_check_env2 (HKEY_CURRENT_USER_ENV, "User INCLUDE", user_env_inc);

  if (do_lib && user_env_lib)
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
      match = fnmatch (file_spec, arr->fname, FNM_FLAG_NOCASE);
      if (match == FNM_MATCH)
      {
        report_file (fqdn, arr->mtime, FALSE, arr->key);
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
  num = build_registry_array (HKEY_CURRENT_USER, REG_APP_PATH);
  sort_reg_array (num);
  found += report_registry (REG_APP_PATH, num);

  snprintf (reg, sizeof(reg), "Matches in HKLM\\%s:\n", REG_APP_PATH);
  report_header = reg;
  DEBUGF (1, "%s\n", reg);
  num = build_registry_array (HKEY_LOCAL_MACHINE, REG_APP_PATH);
  sort_reg_array (num);
  found += report_registry (REG_APP_PATH, num);

  return (found);
}

/*
 * Process directory specified by 'path' and report any matches
 * to the global 'file_spec'.
 */
int process_dir (const char *path, int num_dup, BOOL exist,
                 BOOL is_dir, BOOL exp_ok, const char *prefix, HKEY key)
{
  HANDLE          handle;
  WIN32_FIND_DATA ff_data;
  char            fqfn  [_MAX_PATH]; /* Fully qualified file-name */
  int             found = 0;

  /* We need to set these only once; 'file_spec' is constant throughout the program.
   */
  static char *fspec  = NULL;
  static char *subdir = NULL;  /* Looking for a 'file_spec' with a sub-dir part in it. */

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

  if (!file_spec)
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
    struct stat st;
    char  *base;
    int    match, len;

    if (!strcmp(ff_data.cFileName,".."))
       continue;

    len  = snprintf (fqfn, sizeof(fqfn), "%s%c", path, DIR_SEP);
    base = fqfn + len;
    len += snprintf (base, sizeof(fqfn)-len, "%s%s",
                     subdir ? subdir : "", ff_data.cFileName);

    is_dir = ((ff_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);

    match = fnmatch (file_spec, base, FNM_FLAG_NOCASE | FNM_FLAG_NOESCAPE);

#if 0
    if (match == FNM_NOMATCH && strchr(file_spec,'~'))
    {
      /* The case where 'file_spec' is a SFN, fnmatch() doesn't work.
       * What to do?
       */
    }
    else
#endif

    if (match == FNM_NOMATCH)
    {
      /* The case where 'base' is a dotless file, fnmatch() doesn't work.
       * I.e. if file_spec == 'ratio.*' and base == 'ratio', we qualify this
       *      as a match.
       */
      if (!is_dir && !dir_mode && !strnicmp(base,file_spec,strlen(base)))
        match = FNM_MATCH;
    }

    DEBUGF (1, "Testing \"%s\". is_dir: %d, %s\n",
               fqfn, is_dir, fnmatch_res(match));

    if (match == FNM_MATCH && stat(fqfn, &st) == 0)
    {
      report_file (fqfn, st.st_mtime, is_dir, key);
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
  char  query [_MAX_PATH];

  /* If user didn't use the '-r/--regex' option, we must convert
   * file_spec into a RegExp compatible format.
   * E.g. "ez_*.py" -> "^ez_.*\.py$"
   */
  if (use_regex)
       snprintf (query, sizeof(query), "regex:%s", file_spec);
  else snprintf (query, sizeof(query), "regex:%s", translate_shell_pattern(file_spec));

  DEBUGF (1, "Everything_SetSearch (\"%s\").\n", query);

  Everything_SetSearch (query);
  Everything_SetMatchCase (0);       /* Ignore case of matches */
  Everything_Query (TRUE);

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

  if (num == 0 && use_regex)
  {
    WARN ("Nothing matched your regexp \"%s\". Are you sure it is correct? Try quoting it.\n",
          file_spec);
    return (0);
  }

  for (i = 0; i < num; i++)
  {
    struct stat st;
    char   file [_MAX_PATH];
    time_t mtime = 0;
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
      mtime = st.st_mtime;
      is_dir = _S_ISDIR (st.st_mode);
    }
    report_file (file, mtime, is_dir, HKEY_EVERYTHING);
  }
  return (num);
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
    DEBUGF (1, "Env-var %%%s not defined.\n", env_name);
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
 * Do that by spawning it and parsing the paths.
 */
static BOOL found_search_line = FALSE;
static int  found_index = 0;

static int find_include_path (char *buf, int index)
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
    if (memcmp(buf,&end,sizeof(end)-1))
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
  found = popen_run (cmd, find_include_path);
  if (found > 0)
       DEBUGF (1, "found %d include paths for %s.\n", found, gcc);
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

static int do_check_gcc_includes (void)
{
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
  char report [200];
  int  i, found = 0;

  for (i = 0; i < DIM(gcc); i++)
      if (setup_gcc_includes(gcc[i]) > 0)
      {
        snprintf (report, sizeof(report), "Matches in %s %%C_INCLUDE_PATH path:\n", gcc[i]);
        report_header = report;
        found += process_gcc_includes (gcc[i]);
      }

  if (found == 0)  /* Impossible? */
     WARN ("No gcc.exe programs returned any include paths.\n");

  return (found);
}

static int do_check_gpp_includes (void)
{
  static const char *gpp[] = { "g++.exe",
#if CHECK_PREFIXED_GCC
                               "x86_64-w64-mingw32-g++.exe",
                               "i386-mingw32-g++.exe",
                               "i686-w64-mingw32-g++.exe"
#endif
                              };
  char report [200];
  int i, found = 0;

  for (i = 0; i < DIM(gpp); i++)
      if (setup_gcc_includes(gpp[i]) > 0)
      {
        snprintf (report, sizeof(report), "Matches in %s %%CPLUS_INCLUDE_PATH path:\n", gpp[i]);
        report_header = report;
        found += process_gcc_includes (gpp[i]);
      }

  if (found == 0)  /* Impossible? */
     WARN ("No g++.exe programs returned any include paths.\n");

  return (found);
}


/*
 * getopt_long() processing.
 */
static const struct option long_options[] = {
           { "help",    no_argument, NULL, 'h' },
           { "help",    no_argument, NULL, '?' },  /* 1 */
           { "version", no_argument, NULL, 'V' },
           { "inc",     no_argument, NULL, 0 },    /* 3 */
           { "path",    no_argument, NULL, 0 },
           { "lib",     no_argument, NULL, 0 },    /* 5 */
           { "python",  no_argument, NULL, 0 },
           { "dir",     no_argument, NULL, 'D' },  /* 7 */
           { "debug",   no_argument, NULL, 'd' },
           { "no-sys",  no_argument, NULL, 0 },    /* 9 */
           { "no-usr",  no_argument, NULL, 0 },
           { "no-app",  no_argument, NULL, 0 },    /* 11 */
           { "test",    no_argument, NULL, 't' },
           { "quiet",   no_argument, NULL, 'q' },  /* 13 */
           { "no-gcc",  no_argument, NULL, 0 },
           { "no-g++",  no_argument, NULL, 0 },    /* 15 */
           { "verbose", no_argument, NULL, 'v' },
           { "pe-check",no_argument, NULL, 0 },    /* 17 */
           { "color",   no_argument, NULL, 'C' },
           { "evry",    no_argument, NULL, 0 },    /* 19 */
           { "regex",   no_argument, NULL, 0 },
           { NULL,      no_argument, NULL, 0 }     /* 19 */
         };

static int *values_tab[] = {
            NULL,
            NULL,            /* 1 */
            NULL,
            &do_include,     /* 3 */
            &do_path,
            &do_lib,         /* 5 */
            &do_python,
            &dir_mode,       /* 7 */
            NULL,
            &no_sys_env,     /* 9 */
            &no_usr_env,
            &no_app_path,    /* 11 */
            NULL,
            NULL,            /* 13 */
            &no_gcc,
            &no_gpp,         /* 15 */
            &verbose,
            &PE_check,       /* 17 */
            &use_colours,
            &do_evry,        /* 19 */
            &use_regex
          };

static void set_long_option (int opt)
{
  int *val;

  ASSERT (values_tab[opt]);
  ASSERT (long_options[opt].name);
  DEBUGF (2, "got long option \"--%s\".\n", long_options[opt].name);
  val = values_tab [opt];
  *val ^= 1;
}

static char *parse_args (int argc, char *const *argv)
{
  char  buf [_MAX_PATH];
  char *env = getenv_expand ("ENVTOOL_OPTIONS");
  char *ext;

#if 0
  if (!program_name)
  {
    const char *cp = strrchr (argv[0], '/');

    if (cp)
         program_name = cp + 1;
    else program_name = argv[0];
  }
#endif

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
    new_argv[0] = argv[0];
    new_argc = 1;
    for (i = 1; s && i < DIM(new_argv)-1; i++, new_argc++)
    {
      new_argv[i] = STRDUP (s);
      s = strtok (NULL, "\t ");
    }

    for (j = i, i = 1; (i < argc) && (j < DIM(new_argv)-1); i++, j++)
        new_argv [j] = argv [i];  /* copy original into new_argv[] */

    if (j == DIM(new_argv)-1)
       WARN ("Too many arguments (%d) in %%ENVTOOL_OPTIONS.\n", i);
    argc = j;
    argv = new_argv;

    DEBUGF (3, "argc: %d\n", argc);
    for (i = 0; i < argc; i++)
        DEBUGF (3, "argv[%d]: \"%s\"\n", i, argv[i]);
    FREE (env);
  }

  while (1)
  {
    int c, opt_index = 0;

    c = getopt_long (argc, argv, "cChvVdDrstTuq", long_options, &opt_index);

    if (c == 0)
         DEBUGF (2, "c: <NUL>\n");
    else DEBUGF (2, "c: '%c' (%d)\n", c, c);

    if (c == -1)
       break;

    switch (c)
    {
      case 0:
           set_long_option (opt_index);
           break;
      case 'h':
           do_help = 1;
           break;
      case 'V':
           show_version();
           break;
      case 'v':
           verbose++;
           break;
      case 'd':
           debug++;
           break;
      case 'D':
           dir_mode = 1;
           break;
      case 'c':
           add_cwd = 0;
           break;
      case 'C':
           use_colours = 1;
           break;
      case 'r':
           use_regex = 1;
           break;
      case 'T':
           decimal_timestamp = 1;
           break;
      case 't':
           do_test = 1;
           break;
      case 'u':
           show_unix_paths = 1;
           break;
      case 'q':
           quiet = 1;
           break;
      case '?':      /* '?' == BADCH || BADARG */
           usage ("  Use \"--help\" for options\n");
           break;
      default:
           usage ("Illegal option: '%c'\n", optopt);
           break;
    }
  }
  if (argc < 2 || do_help)
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
  _CrtMemDumpAllObjectsSince (&last_state);
  if (debug)
    _CrtMemDumpStatistics (&last_state);
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
     exit_python_embedding();

  free_dir_array();
  FREE (who_am_I);

  FREE (system_env_path);
  FREE (system_env_lib);
  FREE (system_env_inc);

  FREE (user_env_path);
  FREE (user_env_lib);
  FREE (user_env_inc);

  for (i = 1; i < new_argc; i++)
     FREE (new_argv[i]);

  if (halt_flag == 0 && debug > 0)
     mem_report();

#if defined(_MSC_VER) && defined(_DEBUG)
  crtdbg_exit();
#endif
}

/*
 * This ^C handler gets called in another thread.
 */
static void halt (int sig)
{
  extern VOID EVERYTHINGAPI Everything_Reset (VOID);
  extern HANDLE Everything_hthread;

  (void) sig;
  halt_flag++;

  if (do_evry)
  {
    if (Everything_hthread)
       TerminateThread (Everything_hthread, 1);
    Everything_Reset();
  }
  C_puts ("~5Quitting.\n~0");
  cleanup();
  ExitProcess (GetCurrentProcessId());
}


int main (int argc, char **argv)
{
  int   found = 0;
  char *end, *ext;

#if defined(_MSC_VER) && defined(_DEBUG)
  crtdbug_init();
#endif

  atexit (cleanup);
  file_spec = parse_args (argc, argv);

  g_current_dir[0] = '.';
  g_current_dir[1] = DIR_SEP;
  g_current_dir[2] = '\0';
  getcwd (g_current_dir, sizeof(g_current_dir));

  /* Sometimes the IPC connection to the EveryThing Database will hang.
   * Clean up if user presses ^C.
   */
  signal (SIGINT, halt);

  if (do_test && do_python)
  {
    if (/* test_python_pipe() && */ !halt_flag)
       test_python_funcs();
    return (0);
  }

  if (do_test)
  {
    do_tests();
    return (0);
  }

  if (do_evry && !do_path)
     no_sys_env = no_usr_env = no_app_path = 1;

  if (!do_path && !do_include && !do_lib && !do_python && !do_evry)
     usage ("Use at least one of; \"--inc\", \"--lib\", \"--evry\", \"--python\" and/or \"--path\".\n");

  if (!file_spec)
     usage ("You must give me a ~1filespec~0 to search for.\n");

  if (strchr(file_spec,'~') > file_spec)
     file_spec = _fixpath (file_spec, file_spec_buf);

  end = strchr (file_spec, '\0');
  ext = (char*) get_file_ext (file_spec);

  if (!use_regex && end[-1] != '*' && end[-1] != '$' && *ext == '\0')
  {
    end[0] = '.';
    end[1] = '*';
    end[2] = '\0';
  }

  DEBUGF (1, "file_spec: %s\n", file_spec);

  /* Scan registry values unless only '--python' is specified.
   */
  if (!do_python && !(do_path && do_lib && do_include))
  {
    if (!no_sys_env)
       found += scan_system_env();

    if (!no_usr_env)
       found += scan_user_env();
  }

  if (do_path)
  {
    if (!no_app_path)
       found += do_check_registry();

    report_header = "Matches in %PATH:\n";
    found += do_check_env ("PATH");
  }

  if (do_lib)
  {
    report_header = "Matches in %LIB:\n";
    found += do_check_env ("LIB");
    if (!no_gcc && !no_gpp)
    {
      report_header = "Matches in %LIBRARY_PATH:\n";
      found += do_check_env ("LIBRARY_PATH");
    }
  }

  if (do_include)
  {
    report_header = "Matches in %INCLUDE:\n";
    found += do_check_env ("INCLUDE");

    if (!no_gcc)
       found += do_check_gcc_includes();
    else
    {
      report_header = "Matches in %C_INCLUDE_PATH:\n";
      found += do_check_env ("C_INCLUDE_PATH");
    }

    if (!no_gpp)
       found += do_check_gpp_includes();
    else
    {
      report_header = "Matches in %CPLUS_INCLUDE_PATH:\n";
      found += do_check_env ("CPLUS_INCLUDE_PATH");
    }
  }

  if (do_python)
  {
    report_header = "Matches in Python's sys.path[]:\n";
    found += do_check_python();
  }

  if (do_evry)
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
 * E.g. If "$INCLUDE" is "c:\VC\include;%C_INCLUDE_PATH%" and
 * "$C_INCLUDE_PATH=c:\MingW\include", the expansion returns
 * "c:\VC\include;c:\MingW\include". Note that Windows (cmd only?)
 * requires a trailing '%' in "$C_INCLUDE_PATH".
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
  int    i, rc, needed;

  C_printf ("\n~3%s():~0 ", __FUNCTION__);
  C_printf (" testing 'split_env_var (\"%s\",\"%%%s\")':\n", env, env);

  value = getenv_expand (env);
  needed = cygwin_conv_path_list (CCP_WIN_A_TO_POSIX, value, NULL, 0);
  cyg_value = alloca (needed+1);
  DEBUGF (2, "cygwin_conv_path_list(): needed %d\n", needed);

  rc = cygwin_conv_path_list (CCP_WIN_A_TO_POSIX, value, cyg_value, needed+1);
  DEBUGF (2, "cygwin_conv_path_list(): rc: %d, '%s'\n", rc, cyg_value);

  path_separator = ':';
  conv_cygdrive = 0;
  arr0  = split_env_var (env, cyg_value);

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
  conv_cygdrive = 1;
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

                  { "c:\\NTLDR",         "c:\\" },  /* test if searchpath() finds hidden files. */
               // { "c:\\NTLDR",         "" },      /* test if searchpath() handles non-env-vars too. */
                  { "\\\\localhost\\$C", "PATH" }   /* Does it work on a share too? */
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
    if (found)
       found = _fixpath (found, buf);
    C_printf ("  %s:%*s -> %s\n",
              t->file, 15-len, "", found ? found : strerror(errno));
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
  char *rc;
  int   i, rc2, rc3;

  C_printf ("\n~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(files); i++)
  {
    struct stat st;
    BOOL   is_dir;

    f = files [i];
    rc = _fixpath (f, NULL);
    ASSERT (rc);
    rc2 = FILE_EXISTS (rc);
    rc3 = (stat (rc, &st) == 0);
    is_dir = (rc3 && _S_ISDIR (st.st_mode));

    C_printf ("  _fixpath (\"%s\")\n     -> \"%s\" ", f, rc);
    if (!rc2)
         C_printf ("~5exists 0, is_dir %d~0\n\n", is_dir);
    else C_printf ("exists 1, is_dir %d~0\n\n", is_dir);
    FREE (rc);
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

static void do_tests (void)
{
  test_split_env ("PATH");
#if 0
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
  if (debug > 0)
     mem_report();
}

