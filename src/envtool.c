/**
 * \file    envtool.c
 * \ingroup Envtool
 * \brief
 *   A tool to search and check various environment variables
 *   for correctness and check where a specific file is in corresponding
 *   environment variable.
 *
 *  \eg{1} `envtool --path notepad.exe` first checks the `%PATH%` env-var
 *          for consistency (reports missing directories in `%PATH%`) and
 *          prints all the locations of `notepad.exe`.
 *
 *  \eg{2} `envtool --inc afxwin.h` first checks the `%INCLUDE%` env-var
 *          for consistency (reports missing directories in `%INCLUDE%`) and
 *          prints all the locations of `afxwin.h`.
 *
 * By Gisle Vanem <gvanem@yahoo.no> August 2011 - 2018.
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
#include <psapi.h>

#if defined(__MINGW32__) || defined(_CYGWIN__)
#define INSIDE_ENVTOOL_C  /* Important for MinGW/CygWin with '_FORTIFY_SOURCE' only */
#endif

#include "getopt_long.h"
#include "Everything.h"
#include "Everything_IPC.h"
#include "Everything_ETP.h"
#include "auth.h"
#include "color.h"
#include "smartlist.h"
#include "regex.h"
#include "ignore.h"
#include "envtool.h"
#include "envtool_py.h"
#include "dirlist.h"
#include "sort.h"
#include "vcpkg.h"
#include "get_file_assoc.h"

extern BOOL find_vstudio_init (void);

/**
 * <!-- \includedoc README.md ->
 * \image html  envtool-help.png "List of modes" width=10cm
 * \image latex envtool-help.png "List of modes" width=10cm
 */

#ifdef __MINGW32__
  /**
   * Tell MinGW's CRT to turn off command line globbing by default.
   */
  int _CRT_glob = 0;

#ifndef __MINGW64_VERSION_MAJOR
  /**
   * MinGW-64's CRT seems to NOT glob the cmd-line by default.
   * Hence this doesn't change that behaviour.
   */
  int _dowildcard = 0;
#endif
#endif

char *program_name = NULL;   /**< For getopt_long.c */

/**
 * \def REG_APP_PATH
 * The Registry key under `HKEY_CURRENT_USER` or `HKEY_LOCAL_MACHINE`.
 */
#define REG_APP_PATH    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths"

/**
 * \def EVERYTHING_IPC_IS_DB_LOADED
 * \def EVERYTHING_IPC_IS_DB_BUSY
 *
 * IPC messages to check if EveryThing has loaded it's database or is busy indexing it's database.
 * These were added in Everything 1.4
 */
#ifndef EVERYTHING_IPC_IS_DB_LOADED
#define EVERYTHING_IPC_IS_DB_LOADED 401
#endif

#ifndef EVERYTHING_IPC_IS_DB_BUSY
#define EVERYTHING_IPC_IS_DB_BUSY   402
#endif

/**
 * \struct directory_array
 */
struct directory_array {
       char    *dir;         /**< FQDN of this entry */
       char    *cyg_dir;     /**< The Cygwin POSIX form of the above */
       int      exist;       /**< does it exist? */
       int      is_native;   /**< and is it a native dir; like %WinDir\sysnative */
       int      is_dir;      /**< and is it a dir; _S_ISDIR() */
       int      is_cwd;      /**< and is it equal to current_dir[] */
       int      exp_ok;      /**< expand_env_var() returned with no '%'? */
       int      num_dup;     /**< is duplicated elsewhere in %VAR%? */
       BOOL     check_empty; /**< check if it contains at least 1 file? */
       unsigned line;        /**< Debug: at what line was add_to_dir_array() called */
     };

/**
 * \struct registry_array
 */
struct registry_array {
       char   *fname;        /**< basename of this entry. I.e. the name of the enumerated key. */
       char   *real_fname;   /**< normally the same as above unless aliased. E.g. "winzip.exe -> "winzip32.exe" */
       char   *path;         /**< path of this entry */
       int     exist;        /**< does it exist? */
       time_t  mtime;        /**< file modification time */
       UINT64  fsize;        /**< file size */
       HKEY    key;
     };

static smartlist_t *dir_array, *reg_array;

struct prog_options opt;

char   sys_dir        [_MAX_PATH];  /* E.g. "c:\Windows\System32" */
char   sys_native_dir [_MAX_PATH];  /* E.g. "c:\Windows\sysnative". Not for WIN64 */
char   sys_wow64_dir  [_MAX_PATH];  /* E.g. "c:\Windows\SysWOW64". Not for WIN64 */

static UINT64   total_size = 0;
static DWORD    num_version_ok = 0;
static DWORD    num_verified = 0;
static DWORD    num_evry_dups = 0;
static DWORD    num_evry_ignored = 0;
static BOOL     have_sys_native_dir = FALSE;
static BOOL     have_sys_wow64_dir  = FALSE;

static char *who_am_I = (char*) "envtool";

static char *system_env_path = NULL;
static char *system_env_lib  = NULL;
static char *system_env_inc  = NULL;

static char *user_env_path   = NULL;
static char *user_env_lib    = NULL;
static char *user_env_inc    = NULL;
static char *report_header   = NULL;

static int   path_separator = ';';
static char  current_dir [_MAX_PATH];

static char *vcache_fname = NULL;
static BOOL  use_cache = FALSE;

static regex_t    re_hnd;         /* regex handle/state */
static regmatch_t re_matches[3];  /* regex sub-expressions */
static int        re_err;         /* last regex error-code */
static char       re_errbuf[300]; /* regex error-buffer */
static int        re_alloc;       /* the above 're_hnd' was allocated */

volatile int halt_flag;

/**
 * The list of prefixes for gnu C/C++ compilers.
 *
 * \eg{.} E.g. we try `gcc.exe` ... `avr-gcc.exe` to figure out the
 *        `%C_INCLUDE_PATH`, `%CPLUS_INCLUDE_PATH` and `%LIBRARY_PATH`.
 *        Unless one of the `<path>/<prefix>-gcc.exe` are in the
 *        `[Compiler]` ignore-list.
 *
 * \todo add more prefixes from `envtool.cfg` here?
 */
static const char *gnu_prefixes[] = {
                  "",
                  "x86_64-w64-mingw32-",
                  "i386-mingw32-",
                  "i686-w64-mingw32-",
                  "avr-"
                };

/** Get the bitness (32/64-bit) of the EveryThing program.
 */
static enum Bitness evry_bitness = bit_unknown;
static void get_evry_bitness (HWND wnd);

static void  usage (const char *fmt, ...) ATTR_PRINTF(1,2);
static int   do_check (void);
static int   do_tests (void);
static void  search_and_add_all_cc (BOOL print_info, BOOL print_lib_path);
static void  print_build_cflags (void);
static void  print_build_ldflags (void);
static int   get_pkg_config_info (char **exe_p, struct ver_info *ver);
static int   get_vcpkg_info (char **exe_p, struct ver_info *ver);
static int   get_cmake_info (char **exe_p, struct ver_info *ver);

/**
 * \todo Add support for *kpathsea*-like path searches (which some TeX programs uses). <br>
 *       E.g. if a `PATH` (or INCLUDE etc.) component contains `/foo/bar//`, the search will
 *            do a recursive search for all files (and dirs) under `/foo/bar/`.
 *
 *       Ref. http://tug.org/texinfohtml/kpathsea.html
 *
 * \todo In `report_file()`, test if a file (in `%PATH`, `%INCLUDE` or `%LIB`) is
 *       shadowed by an older file of the same name (ahead of the newer file).
 *       Warn if this is the case.
 *
 * \todo Add sort option:
 *   \li on date/time.
 *   \li on filename.
 *   \li on file-size.
 *
 * \todo Add `--locate` option (or in combination with `--evry` option?) to
 *       look into GNU's `locatedb` (`%LOCATE_PATH=/cygdrive/X/Cygwin32/locatedb`)
 *       information too.
 *
 * \todo Add a `--check` option for 64-bit Windows to check that all .DLLs in:<br>
 *           \li `"%SystemRoot%\System32"` are 64-bit and
 *           \li `"%SystemRoot%\SysWOW64"` are 32-bit. <br>
 *       \eg{.}
 *        \verbose
 *           pedump %SystemRoot%\SysWOW64\*.dll | grep 'Machine: '
 *           Machine:                      014C (i386)
 *           Machine:                      014C (i386)
 *           ....
 *        \endverbose
 *
 *       Also check their Wintrust signature status and version information.
 */

/**
 * Show some version details for the EveryThing program.
 * Called on 'FindWindow ("EVERYTHING_TASKBAR_NOTIFICATION")' success.
 */
static void show_evry_version (HWND wnd, const struct ver_info *ver)
{
  #define MAX_INDEXED  ('Z' - 'A' + 1)

  char buf [3*MAX_INDEXED+2], *p = buf, *bits = "";
  int  d, num;

  if (evry_bitness == bit_unknown)
     get_evry_bitness (wnd);

  if (evry_bitness == bit_32)
     bits = " (32-bit)";
  else if (evry_bitness == bit_64)
     bits = " (64-bit)";

  C_printf ("  Everything search engine ver. %u.%u.%u.%u%s (c)"
            " David Carpenter; ~6http://www.voidtools.com/~0\n",
            ver->val_1, ver->val_2, ver->val_3, ver->val_4, bits);

  *p = '\0';
  for (d = num = 0; d < MAX_INDEXED; d++)
  {
    if (SendMessage(wnd, WM_USER, EVERYTHING_IPC_IS_NTFS_DRIVE_INDEXED, d))
    {
      p += sprintf (p, "%c: ", d+'A');
      num++;
    }
  }

  if (num == 0)
     strcpy (buf, "<none> (busy indexing?)");
  C_printf ("  These drives are indexed: ~3%s~0\n", buf);
}

/**
 * The `SendMessage()` calls could hang if EveryThing is busy updating itself or
 * stuck for some reason.
 *
 * \todo This should be done in a thread.
 */
static BOOL get_evry_version (HWND wnd, struct ver_info *ver)
{
  LRESULT major    = SendMessage (wnd, WM_USER, EVERYTHING_IPC_GET_MAJOR_VERSION, 0);
  LRESULT minor    = SendMessage (wnd, WM_USER, EVERYTHING_IPC_GET_MINOR_VERSION, 0);
  LRESULT revision = SendMessage (wnd, WM_USER, EVERYTHING_IPC_GET_REVISION, 0);
  LRESULT build    = SendMessage (wnd, WM_USER, EVERYTHING_IPC_GET_BUILD_NUMBER, 0);

  ver->val_1 = (unsigned) major;
  ver->val_2 = (unsigned) minor;
  ver->val_3 = (unsigned) revision;
  ver->val_4 = (unsigned) build;
  return (ver->val_1 + ver->val_2 + ver->val_3 + ver->val_4) > 0;
}

/**
 * Get the bitness (32/64-bit) of the EveryThing program.
 *
 * \param[in] wnd  the Windows handle of the EveryThing program.
 */
static void get_evry_bitness (HWND wnd)
{
  DWORD        e_pid, e_tid;
  HANDLE       hnd;
  char         fname [_MAX_PATH] = "?";
  enum Bitness bits = bit_unknown;

  if (!wnd)
     return;

  /* Get the thread/process-ID of the EveryThing window.
   */
  e_tid = GetWindowThreadProcessId (wnd, &e_pid);

  DEBUGF (2, "e_pid: %lu, e_tid: %lu.\n", (unsigned long)e_pid, (unsigned long)e_tid);

  hnd = OpenProcess (PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, e_pid);
  if (!hnd)
     return;

  if (get_module_filename_ex(hnd, fname) && check_if_PE(fname, &bits))
     evry_bitness = bits;

  CloseHandle (hnd);
  DEBUGF (2, "fname: %s, evry_bitness: %d.\n", fname, evry_bitness);
}

/**
 * Show version information for various programs.
 */
static void show_ext_versions (void)
{
  static const char *found_fmt[] = { "  Cmake %u.%u.%u detected",
                                     "  Python %u.%u.%u detected",
                                     "  pkg-config %u.%u detected",
                                     "  vcpkg %u.%u.%u detected"
                                   };

  static const char *not_found_fmt[] = { "  Cmake ~5not~0 found.\n",
                                         "  Python ~5not~0 found.\n",
                                         "  pkg-config ~5not~0 found.\n",
                                         "  vcpkg ~5not~0 found.\n",
                                       };
  #define FOUND_SZ 100

  char            found [4][FOUND_SZ];
  int             pad_len, len [4] = { 0,0,0,0 };
  char           *py_exe         = NULL;
  char           *pkg_config_exe = NULL;
  char           *vcpkg_exe      = NULL;
  char           *cmake_exe      = NULL;
  struct ver_info py_ver, cmake_ver, pkg_config_ver, vcpkg_ver;

  memset (&found, '\0', sizeof(found));
  memset (&cmake_ver, '\0', sizeof(cmake_ver));
  memset (&py_ver, '\0', sizeof(py_ver));
  memset (&pkg_config_ver, '\0', sizeof(pkg_config_ver));
  memset (&vcpkg_ver, '\0', sizeof(vcpkg_ver));

  pad_len = sizeof("  pkg-config 9.99 detected");

  if (get_cmake_info(&cmake_exe, &cmake_ver))
  {
    len[0] = snprintf (found[0], FOUND_SZ, found_fmt[0], cmake_ver.val_1, cmake_ver.val_2, cmake_ver.val_3);
    if (len[0] > pad_len)
       pad_len = len[0];
  }

  if (py_get_info(&py_exe, NULL, &py_ver))
  {
    slashify2 (py_exe, py_exe, opt.show_unix_paths ? '/' : '\\');
    len[1] = snprintf (found[1], FOUND_SZ, found_fmt[1], py_ver.val_1, py_ver.val_2, py_ver.val_3);
    pad_len = len[1];
  }

  if (get_pkg_config_info(&pkg_config_exe, &pkg_config_ver))
  {
    len[2] = snprintf (found[2], FOUND_SZ, found_fmt[2], pkg_config_ver.val_1, pkg_config_ver.val_2);
    if (len[2] > pad_len)
       pad_len = len[2];
  }

  if (get_vcpkg_info(&vcpkg_exe, &vcpkg_ver))
  {
    len[3] = snprintf (found[3], FOUND_SZ, found_fmt[3], vcpkg_ver.val_1, vcpkg_ver.val_2, vcpkg_ver.val_3);
    if (len[3] > pad_len)
       pad_len = len[3];
  }

  if (cmake_exe)
       C_printf ("%-*s -> ~6%s~0\n", pad_len, found[0], cmake_exe);
  else C_printf (not_found_fmt[0]);

  if (py_exe)
       C_printf ("%-*s -> ~6%s~0\n", pad_len, found[1], py_exe);
  else C_printf (not_found_fmt[1]);

  if (pkg_config_exe)
       C_printf ("%-*s -> ~6%s~0\n", pad_len, found[2], pkg_config_exe);
  else C_printf (not_found_fmt[2]);

  if (vcpkg_exe)
  {
    unsigned num1, num2;

    vcpkg_get_list();
    num1 = vcpkg_get_num_CONTROLS();
    num2 = vcpkg_get_num_installed();
    C_printf ("%-*s -> ~6%s~0", pad_len, found[3], vcpkg_exe);
    if (num1 >= 1)
         C_printf (" (%u CONTROL nodes. %u packages installed).\n", num1, num2);
    else C_printf (" (%s).\n", vcpkg_last_error());
  }
  else
    C_printf (not_found_fmt[3]);

  FREE (cmake_exe);
  FREE (py_exe);
  FREE (pkg_config_exe);
  FREE (vcpkg_exe);
}

/**
 * Hook-functions for color.c functions.
 * Used to dump version-information to file-cache.
 */
static BOOL  saw_newline;
static FILE *fcache;

static void write_hook (const char *buf)
{
  size_t len = strlen (buf);

  if (len >= 1)
  {
    /* Put 'opt.cache_ver_level' only on lines after [\r\n] was seen
     */
    if (saw_newline)
         fprintf (fcache, "%d:%s", opt.cache_ver_level, buf);
    else fputs (buf, fcache);
    saw_newline = (buf[len-1] == '\r' || buf[len-1] == '\n');
  }
}

static void read_hook (smartlist_t *sl, const char *buf)
{
  int vlevel = (int)buf[0];

  if (isdigit(vlevel) && buf[1] == ':')
  {
    if (opt.do_version >= vlevel - '0')
       C_puts (buf+2);
  }
  else
    C_puts (buf);
  ARGSUSED (sl);
}

/**
 * \li Show some basic version information:    option `-V`.
 * \li Show more detailed version information: option `-VV`.
 */
static int show_version (void)
{
  HWND         wnd;
  BOOL         wow64 = is_wow64_active();
  smartlist_t *vcache = NULL;

  if (use_cache)
  {
    if (FILE_EXISTS(vcache_fname))
    {
      /* If file-cache exist, read from it and print it.
       */
      vcache = smartlist_read_file (vcache_fname, read_hook);
      goto quit;
    }

    /* Otherwise, create the cache's content
     */
    // opt.do_version = 3;
    fcache = fopen (vcache_fname, "w+t");
    if (fcache)
    {
      /* Initialised to TRUE so that the first line written gets
       * a 'opt.cache_ver_level' prefix.
       */
      saw_newline = TRUE;
      C_write_hook = write_hook;
    }
  }

  opt.cache_ver_level = 1;
  C_printf ("%s.\n  Version ~3%s ~1(%s, %s%s)~0 by %s.\n  Hosted at: ~6%s~0\n",
            who_am_I, VER_STRING, compiler_version(), WIN_VERSTR,
            wow64 ? ", ~1WOW64" : "", AUTHOR_STR, GITHUB_STR);

  wnd = FindWindow (EVERYTHING_IPC_WNDCLASS, 0);
  if (wnd)
  {
    struct ver_info evry_ver;

    if (get_evry_version(wnd,&evry_ver))
         show_evry_version (wnd, &evry_ver);
    else C_printf ("  Everything search engine not responding.\n");
  }
  else
    C_printf ("  Everything search engine not found.\n");

  C_puts ("  Checking Python programs...");
  C_flush();
  py_init();
  C_printf ("\r                             \r");

  show_ext_versions();

  if (opt.do_version >= 2)
  {
    opt.cache_ver_level = 2;

    C_printf ("  OS-version: %s (%s bits).\n", os_name(), os_bits());
    C_printf ("  User-name:  \"%s\", %slogged in as Admin.\n", get_user_name(), is_user_admin() ? "" : "not ");
    C_printf ("  ConEmu:     %sdetected.\n", opt.under_conemu ? "" : "not ");

    C_puts ("\n  Compile command and ~3CFLAGS~0:");
    print_build_cflags();

    C_puts ("\n  Link command and ~3LDFLAGS~0:");
    print_build_ldflags();

    C_printf ("\n  Compilers on ~3PATH~0:\n");
    search_and_add_all_cc (TRUE, opt.do_version >= 3);

    C_printf ("\n  Pythons on ~3PATH~0:");
    py_searchpaths();

    vcpkg_list_installed();
  }

quit:
  if (vcache)
     smartlist_free (vcache);
  if (fcache)
     fclose (fcache);
  fcache = NULL;
  C_write_hook = NULL;
  return (0);
}

/**
 * Printer for illegal program usage. <br>
 * Call `exit(-1)` when done.
 */
static void usage (const char *fmt, ...)
{
  va_list args;

  va_start (args, fmt);
  C_vprintf (fmt, args);
  va_end (args);
  exit (-1);
}

/**
 * Print some help for the `--32` and `--64` options.
 */
static void print_help_bits (int bits)
{
  C_printf ("    ~6--%d~0           report only %d-bit PE-files in ~6--path~0 or ~6--evry~0 modes.\n"
            "                   ~4not yet~0: report only %d-bit libraries in ~6--lib~0 mode.\n"
            "                            report only %d-bit packages in ~6--vcpkg~0 mode.\n",
            bits, bits, bits, bits);
}

/**
 * Print a help-page of all program options.
 */
static int show_help (void)
{
  #define PFX_GCC  "~4<prefix>~0-~6gcc~0"
  #define PFX_GPP  "~4<prefix>~0-~6g++~0"

  const char **py = py_get_variants();
  int   i;

  C_printf ("Environment check & search tool.\n\n"
            "Usage: %s ~6[options] <--mode> <file-spec>~0\n", who_am_I);

  C_puts ("  ~6<--mode>~0 can be at least one of these:\n"
          "    ~6--cmake~0        check and search in ~3%CMAKE_MODULE_PATH%~0 and it's built-in module-path.\n"
          "    ~6--evry[=~3host~0]  check and search in the ~6EveryThing database~0.     ~2[1]~0\n"
          "    ~6--inc~0          check and search in ~3%INCLUDE%~0.                   ~2[2]~0\n"
          "    ~6--lib~0          check and search in ~3%LIB%~0 and ~3%LIBRARY_PATH%~0.    ~2[2]~0\n"
          "    ~6--man~0          check and search in ~3%MANPATH%~0.\n"
          "    ~6--path~0         check and search in ~3%PATH%~0.\n"
          "    ~6--pkg~0          check and search in ~3%PKG_CONFIG_PATH%~0.\n"
          "    ~6--python~0[~3=X~0]   check and search in ~3%PYTHONPATH%~0 and ~3sys.path[]~0. ~2[3]~0\n"
          "    ~6--vcpkg~0        check and search for ~3VCPKG~0 packages.             ~2[4]~0\n"
          "    ~6--check~0        check for missing directories in ~6all~0 supported environment variables\n"
          "                   and missing files in ~3HKx\\Microsoft\\Windows\\CurrentVersion\\App Paths~0 keys.\n");

  C_puts ("  ~6[options]~0\n"
          "    ~6--no-gcc~0       don't spawn " PFX_GCC " prior to checking.      ~2[2]~0\n"
          "    ~6--no-g++~0       don't spawn " PFX_GPP " prior to checking.      ~2[2]~0\n"
          "    ~6--no-prefix~0    don't check any ~4<prefix>~0-ed ~6gcc/g++~0 programs.    ~2[2]~0\n"
          "    ~6--no-sys~0       don't scan ~3HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment~0.\n"
          "    ~6--no-usr~0       don't scan ~3HKCU\\Environment~0.\n"
          "    ~6--no-app~0       don't scan ~3HKCU\\" REG_APP_PATH "~0 and\n"
          "                              ~3HKLM\\" REG_APP_PATH "~0.\n"
          "    ~6--no-colour~0    don't print using colours.\n"
          "    ~6--no-ansi~0      don't print colours using ANSI sequences (effective for CygWin/ConEmu only).\n"
          "    ~6--no-borland~0   don't check for Borland in ~6--include~0 or ~6--lib~0 mode.\n"
          "    ~6--no-clang~0     don't check for Clang in ~6--include~0 or ~6--lib~0 mode.\n"
          "    ~6--no-watcom~0    don't check for Watcom in ~6--include~0 or ~6--lib~0 mode.\n"
          "    ~6--owner~0        shown owner of the file (shows all owners).\n"
          "    ~6--owner~0=~3spec~0   shown only files/directories matching owner ~3spec~0.\n"
          "    ~6--owner~0=~3!spec~0  shown only files/directories ~4not~0 matching owner ~3spec~0.\n"
          "    ~6--pe~0           print checksum, version-info and signing status for PE-files.\n");

  print_help_bits (32);
  print_help_bits (64);

  C_puts ("    ~6--signed~0       check for ~4all~0 digital signature with the ~6--pe~0 option.\n"
          "    ~6--signed=0~0     report only PE-files files that are ~4unsigned~0.\n"
          "    ~6--signed=1~0     report only PE-files files that are ~4signed~0.\n"
          "    ~6--no-cwd~0       don't add current directory to search-lists.\n"
          "    ~6-c~0             be case-sensitive.\n"
          "    ~6-d~0, ~6--debug~0    set debug level (~3-dd~0 sets ~3PYTHONVERBOSE=1~0 in ~6--python~0 mode).\n"
          "    ~6-D~0, ~6--dir~0      looks only for directories matching ~6<file-spec>~0.\n");

  C_printf ("    ~6-r~0, ~6--regex~0    enable Regular Expressions in all ~6<--mode>~0 searches.\n"
            "    ~6-s~0, ~6--size~0     show size of files or directories found.\n"
            "    ~6-S~3x~0, ~6--sort=~3y~0  sort files on ~3%s~0 / ", get_sort_methods_short());

  C_printf ("~3%s~0 (not yet effective).\n", get_sort_methods_long());

  C_puts ("    ~6-q~0, ~6--quiet~0    disable warnings.\n"
          "    ~6-t~0             do some internal tests. Use ~6--owner~0, ~6--py~0 or ~6--evry~0 for extra tests.\n"
          "    ~6-T~0             show file times in sortable decimal format. E.g. \"~620121107.180658~0\".\n"
          "    ~6-u~0             show all paths on Unix format: \"~2c:/ProgramFiles/~0\".\n"
          "    ~6-v~0             increase verbose level (currently only used in ~6--pe~0).\n"
          "    ~6-V~0             show program version information. ~6-VV~0 and ~6-VVV~0  prints more info.\n"
          "    ~6-h~0, ~6-?~0         show this help.\n\n");

  C_puts ("    ~6--evry~0 remote FTP options:\n"
          "      ~6-H~0, ~6--host~0    hostname/IPv4-address. Can be used multiple times.\n"
          "                    alternative syntax is ~6--evry=<host>~0.\n"
          "      ~6--nonblock-io~0 connects using non-blocking I/O.\n"
          "      ~6--buffered-io~0 use buffering to receive the data.\n");

  C_puts ("\n"
          "  ~2[1]~0 The ~6--evry~0 option requires that the Everything search engine is installed.\n"
          "      Ref. ~3http://www.voidtools.com/support/everything/~0\n"
          "      For remote FTP search(es) (~6--evry=[host-name|IP-address]~0), a user/password\n"
          "      should be specified in your ~6%APPDATA%/.netrc~0 or ~6%APPDATA%/.authinfo~0 files or\n"
          "      you can use the \"~6user:passwd@host_or_IP-address:~3port~0\" syntax.\n"
          "\n"
          "      To perform raw searches, append a modifier like:\n"
          "        envtool ~6--evry~0 ~3*.exe~0 rc:today                     - find today's changes of all ~3*.exe~0 files.\n"
          "        envtool ~6--evry~0 ~3*.mp3~0 title:Hello                  - find all ~3*.mp3~0 files with a title starting with Hello.\n"
          "        envtool ~6--evry~0 ~3Makefile.am~0 content:pod2man        - find all ~3Makefile.am~0 containing ~3pod2man~0.\n"
          "        envtool ~6--evry~0 ~3M*.mp3~0 artist:Madonna \"year:<2002\" - find all Madonna ~3M*.mp3~0 titles issued prior to 2002.\n"
          "\n"
          "      Ref: https://www.voidtools.com/support/everything/recent_changes/\n"
          "           https://www.voidtools.com/support/everything/searching/#functions\n"

          "\n"
          "  ~2[2]~0 Unless ~6--no-prefix~0 is used, the ~3%C_INCLUDE_PATH%~0, ~3%CPLUS_INCLUDE_PATH%~0 and\n"
          "      ~3%LIBRARY_PATH%~0 are also found by spawning " PFX_GCC " and " PFX_GPP ".\n"
          "      These ~4<prefix>~0-es are built-in: ");

  for (i = 1; i < DIM(gnu_prefixes); i++)
  {
    size_t len = strlen (gnu_prefixes[i]);
    C_printf ("~6%.*s~0%s", len-1, gnu_prefixes[i], i <= DIM(gnu_prefixes)-2 ? ", " : ".");
  }

  C_puts ("\n\n"
          "  ~2[3]~0 The ~6--python~0 option can be detailed further with ~3=X~0:\n");

  for (; *py; py++)
  {
    unsigned v = py_variant_value (*py, NULL);

    if (v == ALL_PYTHONS)
         C_printf ("      ~6%-6s~0 use all of the above Python programs.\n", *py);
    else C_printf ("      ~6%-6s~0 use a %s program only.\n", *py, py_variant_name(v));
  }
  C_puts ("             otherwise use only first Python found on PATH (i.e. the default).\n");

  C_printf ("\n  ~2[4]~0 This needs the ~6vcpkg.exe~0 program on ~3%PATH%~0 with a set of ~6ports~0 & ~6CONTROL~0 files.\n"
            "      ~3Ref: https://github.com/Microsoft/vcpkg.git~0\n");

  C_puts ("\n"
          "Notes:\n"
          "  ~6<file-spec>~0 accepts Posix ranges. E.g. \"[a-f]*.txt\".\n"
          "  ~6<file-spec>~0 matches both files and directories. If ~6-D~0/~6--dir~0 is used, only\n"
          "              matching directories are reported.\n"
          "  Quote argument if it contains a shell-character [~6^&%~0]."
          " E.g. use ~6--regex \"^foo%%\\.exe$\"~0\n"
          "  Commonly used options can be set in ~3%ENVTOOL_OPTIONS%~0.\n");
  return (0);
}

/**
 * Add the `dir` to the `dir_array` smartlist.
 * `is_cwd` == 1 if `dir` == current working directory.
 *
 * \param[in] dir     the directory to add to the smartlist.
 * \param[in] is_cwd  TRUE if `dir` is the current working directory.
 * \param[in] line    at what line was `add_to_dir_array()` called.

 * Since this function could be called with a `dir` from `expand_env_var()`,
 * we check here if it returned with no `%`.
 */
void add_to_dir_array (const char *dir, int is_cwd, unsigned line)
{
  struct directory_array *d = CALLOC (1, sizeof(*d));
  struct stat st;
  int    max, i, exp_ok = (dir && *dir != '%');
  BOOL   exists = FALSE;
  BOOL   is_dir = FALSE;

  if (safe_stat(dir, &st, NULL) == 0)
     is_dir = exists = _S_ISDIR (st.st_mode);

  d->cyg_dir = NULL;
  d->dir     = STRDUP (dir);
  d->exp_ok  = exp_ok;
  d->exist   = exp_ok && exists;
  d->is_dir  = is_dir;
  d->is_cwd  = is_cwd;
  d->line    = line;

  /* Can we have >1 native dirs?
   *
   * Use 'stricmp()' since on MinGW-w64, a native directory becomes partially
   * uppercase for some strange reason.
   * E.g. "C:\WINDOWS\sysnative".
   */
  d->is_native = (stricmp(dir,sys_native_dir) == 0);

  /* Some issue with MinGW and OpenWatcom's 'stat()' on a sys_native directory.
   * Even though 'GetFileAttributes()' in 'safe_stat()' says it's a
   * directory, 'stat()' doesn't report it as such. So just fake it.
   */
#if defined(__MINGW32__) || defined(__WATCOMC__)
  if (d->is_native && !d->exist)
  {
    d->exist  = TRUE;
    d->is_dir = TRUE;
  }
#endif

#if (IS_WIN64)
  if (d->is_native && !d->exist)  /* No access to this directory from WIN64; ignore */
  {
    d->exist = d->is_dir = TRUE;
    DEBUGF (2, "Ignore native dir '%s'.\n", dir);
  }
#else
  if (d->is_native && !have_sys_native_dir)
     DEBUGF (2, "Native dir '%s' doesn't exist.\n", dir);
  else if (!d->exist)
     DEBUGF (2, "'%s' doesn't exist.\n", dir);
#endif

#if defined(__CYGWIN__)
  {
    char cyg_dir [_MAX_PATH];
    int  rc = cygwin_conv_path (CCP_WIN_A_TO_POSIX, d->dir, cyg_dir, sizeof(cyg_dir));

    if (rc == 0)
       d->cyg_dir = STRDUP (cyg_dir);
    DEBUGF (2, "cygwin_conv_path(): rc: %d, '%s'\n", rc, cyg_dir);
  }
#endif

  smartlist_add (dir_array, d);

  if (is_cwd || !exp_ok)
     return;

  max = smartlist_len (dir_array);
  for (i = 0; i < max-1; i++)
  {
    const struct directory_array *d2 = smartlist_get (dir_array, i);

    if (!str_equal(dir,d2->dir))
       d->num_dup++;
  }
}

static int dump_dir_array (const char *where, const char *note)
{
  int i, max;

  DEBUGF (2, "%s now%s:\n", where, note);

  max = smartlist_len (dir_array);
  for (i = 0; i < max; i++)
  {
    const struct directory_array *dir = smartlist_get (dir_array, i);

    DEBUGF (2, "  dir_array[%d]: exist:%d, num_dup:%d, %s  %s\n",
            (int)i, dir->exist, dir->num_dup, dir->dir, dir->cyg_dir ? dir->cyg_dir : "");
  }
  return (max);
}

/**
 * `smartlist_make_uniq()` helper.
 *
 * \param[in] _a  The first `dir_array` element to check.
 * \param[in] _b  The second `dir_array` element to check.
 *
 * No need to use `stricmp()` or `str_equal()` since we already checked for
 * duplicates when items where added. Use the `num_dup` count instead.
 */
static int dir_array_compare (const void **_a, const void **_b)
{
  struct directory_array *a = *(struct directory_array **) _a;
  struct directory_array *b = *(struct directory_array **) _b;

  if (b->num_dup > 0)  /* this will get removed */
  {
    a->num_dup = 0;
    return (0);
  }
  return (1);
}

/**
 * `smartlist_wipe()` helper.
 *
 * \param[in] _r  The item in the `reg_array` smartlist to free.
 */
static void reg_array_free (void *_r)
{
  struct registry_array *r = (struct registry_array*) _r;

  FREE (r->fname);
  FREE (r->real_fname);
  FREE (r->path);
  FREE (r);
}

/**
 * `smartlist_wipe()` and `smartlist_make_uniq()` helper.
 *
 * \param[in] _d  The item in the `reg_array` smartlist to free.
 */
static void dir_array_free (void *_d)
{
  struct directory_array *d = (struct directory_array*) _d;

  FREE (d->dir);
  FREE (d->cyg_dir);
  FREE (d);
}

/**
 * The GNU-C report of directories is a mess. Especially all the duplicates and
 * non-canonical names. CygWin is more messy than others. So just remove the
 * duplicates.
 *
 * Loop over the `dir_array` smartlist and remove all non-unique items.
 * Also used for Watcom's include-path.
 *
 * \param[in]  where  Where this function was used;
 *                    equals `"%NT_INCLUDE%"` for `do_check_watcom_includes()` or
 *                    `"library paths"` for `setup_gcc_library_path()`.
 */
static int make_unique_dir_array (const char *where)
{
  int old_len, new_len;

  old_len = dump_dir_array (where, ", non-unique");
  smartlist_make_uniq (dir_array, dir_array_compare, dir_array_free);
  new_len = dump_dir_array (where, ", unique");
  return (old_len - new_len);    /* This should always be 0 or positive */
}

/**
 * Add elements to the 'reg_array' smartlist:
 *  \param[in] key     the key the entry came from: HKEY_CURRENT_USER or HKEY_LOCAL_MACHINE.
 *  \param[in] fname   the result from 'RegEnumKeyEx()'; name of each key.
 *  \param[in] fqdn    the result from 'enum_sub_values()'. This value includes the full path.
 *
 * Note: `basename (fqdn)` may NOT be equal to `fname` (aliasing). That's the reason
 *       we store `real_fname` too.
 */
static void add_to_reg_array (HKEY key, const char *fname, const char *fqdn)
{
  struct registry_array *reg;
  struct stat  st;
  const  char *base;
  int    rc;

  base = basename (fqdn);
  if (base == fqdn)
  {
    DEBUGF (1, "fqdn (%s) contains no '\\' or '/'\n", fqdn);
    return;
  }

  reg = CALLOC (1, sizeof(*reg));
  smartlist_add (reg_array, reg);

  rc = safe_stat (fqdn, &st, NULL);
  reg->mtime      = st.st_mtime;
  reg->fsize      = st.st_size;
  reg->fname      = STRDUP (fname);
  reg->real_fname = STRDUP (base);
  reg->path       = dirname (fqdn);
  reg->exist      = (rc == 0) && FILE_EXISTS (fqdn);
  reg->key        = key;
}

/**
 * Sort the 'reg_array' on 'path' + 'real_fname'.
 */
static int reg_array_compare (const void **_a, const void **_b)
{
  const struct registry_array *a = *_a;
  const struct registry_array *b = *_b;
  char  fqdn_a [_MAX_PATH];
  char  fqdn_b [_MAX_PATH];
  char  slash = (opt.show_unix_paths ? '/' : '\\');

  if (!a->path || !a->real_fname || !b->path || !b->real_fname)
     return (0);

  snprintf (fqdn_a, sizeof(fqdn_a), "%s%c%s", slashify(a->path, slash), slash, a->real_fname);
  snprintf (fqdn_b, sizeof(fqdn_b), "%s%c%s", slashify(b->path, slash), slash, b->real_fname);
  return str_equal (fqdn_a, fqdn_b);
}

static void print_reg_array (const char *intro)
{
  const struct registry_array *reg;
  int   i, max, slash = (opt.show_unix_paths ? '/' : '\\');

  DEBUGF (3, intro);

  max = smartlist_len (reg_array);
  for (i = 0; i < max; i++)
  {
    reg = smartlist_get (reg_array, i);
    DEBUGF (3, "%2d: FQDN: %s%c%s.\n", i, reg->path, slash, reg->real_fname);
  }
}

static void sort_reg_array (void)
{
  print_reg_array ("before smartlist_sort():\n");
  smartlist_sort (reg_array, reg_array_compare);
  print_reg_array ("after smartlist_sort():\n");
}

static void free_reg_array (void)
{
  smartlist_wipe (reg_array, reg_array_free);
}

static void free_dir_array (void)
{
  smartlist_wipe (dir_array, dir_array_free);
}

/**
 * Parses an environment string and returns all components as an array of
 * 'struct directory_array' pointing into the global 'dir_array[]'.
 * This works since we handle only one env-var at a time. The 'dir_array[]'
 * gets cleared in 'free_dir_array()' first (in case it was used already).
 *
 * Add current working directory first if 'opt.no_cwd' is FALSE.
 *
 * Convert CygWin style paths to Windows paths: "/cygdrive/x/.." -> "x:/.."
 */
static smartlist_t *split_env_var (const char *env_name, const char *value)
{
  char *tok, *val;
  int   is_cwd, max, i;
  char  sep [2];

  if (!value)
  {
    DEBUGF (1, "split_env_var(\"%s\", NULL)' called!\n", env_name);
    return (NULL);
  }

  val = STRDUP (value);  /* Freed before we return */
  free_dir_array();

  sep[0] = (char) path_separator;
  sep[1] = '\0';

  tok = strtok (val, sep);
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
  if (!opt.no_cwd && is_cwd)
     add_to_dir_array (current_dir, 1, __LINE__);

  max = INT_MAX;

  for ( ; i < max && tok; i++)
  {
    /* Remove trailing '\\', '/' or '\\"' from environment component
     * unless it's a simple "c:\".
     */
    char *p, *end = strchr (tok, '\0');

    if (end > tok+3)
    {
      if (end[-1] == '\\' || end[-1] == '/')
        end[-1] = '\0';
      else if (end[-2] == '\\' && end[-1] == '"')
        end[-2] = '\0';
    }

    if (!opt.quiet)
    {
      /* Check and warn when a component on form 'c:\dir with space' is found.
       * I.e. a path without quotes "c:\dir with space".
       */
      p = strchr (tok, ' ');
      if (opt.quotes_warn && p && (*tok != '"' || end[-1] != '"'))
         WARN ("%s: \"%s\" needs to be enclosed in quotes.\n", env_name, tok);

#if !defined(__CYGWIN__)
      /*
       * Check for missing drive-letter ('x:') in component.
       */
      if (!is_cwd && IS_SLASH(tok[0]) && str_equal_n(tok,"/cygdrive/",10))
         WARN ("%s: \"%s\" is missing a drive letter.\n", env_name, tok);
#endif

      /* Warn on 'x:' (a missing trailing slash)
       */
      if (strlen(tok) <= 3 && isalpha((int)tok[0]) && tok[1] == ':' && !IS_SLASH(tok[2]))
         WARN ("%s: Component \"%s\" should be \"%s%c\".\n", env_name, tok, tok, DIR_SEP);
    }

    p = strchr (tok, '%');
    if (p)
      WARN ("%s: unexpanded component \"%s\".\n", env_name, tok);

    if (*tok == '"' && end[-1] == '"')   /* Remove quotes */
    {
      tok++;
      end[-1] = '\0';
    }

    /* _stati64(".") doesn't work. Hence turn "." into 'current_dir'.
     */
    is_cwd = !strcmp(tok,".") || !strcmp(tok,".\\") || !strcmp(tok,"./");
    if (is_cwd)
    {
      if (i > 0 && !opt.under_conemu)
         WARN ("Having \"%s\" not first in \"%s\" is asking for trouble.\n",
               tok, env_name);
      tok = current_dir;
    }
    else if (opt.conv_cygdrive && strlen(tok) >= 12 && !str_equal_n(tok,"/cygdrive/",10))
    {
      char buf [_MAX_PATH];

      snprintf (buf, sizeof(buf), "%c:/%s", tok[10], tok+12);
      DEBUGF (1, "CygPath conv: '%s' -> '%s'\n", tok, buf);
      tok = buf;
    }

    add_to_dir_array (tok, !str_equal(tok,current_dir), __LINE__);
    tok = strtok (NULL, sep);
  }

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

/* Use this as an indication that the EveryThing database is not up-to-date with
 * the reality; files have been deleted after the database was last updated.
 * Unless we're running a 64-bit version of envtool and a file was found in
 * the 'sys_native_dir[]' and we 'have_sys_native_dir == 0'.
 */
static int found_everything_db_dirty = 0;

/*

 Improve dissection of .sys-files. E.g.:

envtool.exe --evry -d --pe pwdspio.sys
envtool.c(2218): file_spec: pwdspio.sys
envtool.c(1383): Everything_SetSearch ("regex:^pwdspio\.sys$").
envtool.c(1392): Everything_Query: No error
envtool.c(1402): Everything_GetNumResults() num: 3, err: No error
Matches from EveryThing:
      01 Jan 1970 - 00:00:00: c:\Windows\System32\pwdspio.sys   <<< access this via 'c:\Windows\Sysnative\pwdspio.sys'
      Not a PE-image.
      30 Sep 2013 - 17:26:48: f:\ProgramFiler\Disk\MiniTool-PartitionWizard\x64\x64\pwdspio.sysmisc.c(168): Opt magic: 0x020B, file_sum:
      0x73647770
misc.c(171): rc: 0, 0x0000F9D7, 0x0000F9D7
show_ver.c(587): Unable to access file "f:\ProgramFiler\Disk\MiniTool-PartitionWizard\x64\x64\pwdspio.sys":
  1813 Finner ikke den angitte ressurstypen i avbildningsfilen

      ver 0.0.0.0, Chksum OK
      19 Jun 2014 - 15:34:10: f:\ProgramFiler\Disk\MiniTool-PartitionWizard\x86\pwdspio.sysmisc.c(168): Opt magic: 0x010B, file_sum:
      0x00000000
misc.c(171): rc: 0, 0x0000328A, 0x0000328A
show_ver.c(587): Unable to access file "f:\ProgramFiler\Disk\MiniTool-PartitionWizard\x86\pwdspio.sys":
  1813 Finner ikke den angitte ressurstypen i avbildningsfilen

      ver 0.0.0.0, Chksum OK
3 matches found for "pwdspio.sys". 0 have PE-version info.

 */

/*
 * Print the Resource-version details after any 'wintrust_check()' results has
 * been printed. The details come from 'get_PE_file_brief()' and 'get_PE_version_info()'.
 * Which can be retrieved using 'get_PE_version_info_buf()'.
 */
static void print_PE_file_details (const char *filler)
{
  char *line, *ver_trace = get_PE_version_info_buf();
  int   save, i;

  if (!ver_trace)
     return;

  save = C_setraw (1);  /* In case version-info contains a "~" (SFN). */

  for (i = 0, line = strtok(ver_trace,"\n"); line; line = strtok(NULL,"\n"), i++)
  {
    const char *colon  = strchr (line, ':');
    size_t      indent = strlen (filler);

    if (colon)
    {
      if (colon && colon[1] == ' ')
      {
        char   ignore [200];
        size_t len = min (sizeof(ignore)-1, colon-line+1);

        _strlcpy (ignore, line, len);
        if (cfg_ignore_lookup("[PE-resources]",str_trim(ignore)))
           continue;
      }
      indent += colon - line + 1;
    }
    if (i == 0)
       C_putc ('\n');
    C_puts (filler);
    C_puts_long_line (line, indent+1);
  }
  C_setraw (save);
  get_PE_version_info_free();
}

/*
 * With the "--pe" (and "--32" or "--64") option, check if a 'file' is a PE-file.
 * If so, save the checksum, version-info, signing-status for later when
 * 'report_file()' is ready to print this info.
 */
static BOOL get_PE_file_brief (const char *file, const char *filler, HKEY key, char *dest, size_t dest_size)
{
  struct ver_info ver;
  enum Bitness    bits;
  const char     *bitness;
  BOOL            chksum_ok  = FALSE;
  BOOL            version_ok = FALSE;

  *dest = '\0';

  if (key == HKEY_INC_LIB_FILE || key == HKEY_MAN_FILE ||
      key == HKEY_EVERYTHING_ETP || key == HKEY_PKGCONFIG_FILE)
     return (FALSE);

  if (!check_if_PE(file,&bits))
     return (FALSE);

  if (opt.only_32bit && bits != bit_32)
     return (FALSE);

  if (opt.only_64bit && bits != bit_64)
     return (FALSE);

  memset (&ver, 0, sizeof(ver));
  chksum_ok  = verify_PE_checksum (file);
  version_ok = get_PE_version_info (file, &ver);
  if (version_ok)
     num_version_ok++;

  bitness = (bits == bit_32) ? "~232" :
            (bits == bit_64) ? "~364" : "~5?";

  /* Do not add a '\n' since 'wintrust_check()' is called right after this function.
   */
  snprintf (dest, dest_size, "\n%sver ~6%u.%u.%u.%u~0, %s~0-bit, Chksum %s~0",
            filler, ver.val_1, ver.val_2, ver.val_3, ver.val_4,
            bitness, chksum_ok ? "~2OK" : "~5fail");
  return (TRUE);
}

/**
 * With the "--pe" (and "--32" or "--64") option, and if 'file' is a PE-file
 * (verified in above function), do a check for any signatures.
 *
 * \todo If the PE has a SECURITY data-directory, try to extract it's raw data;
 *       const WIN_CERTIFICATE *cert = ..
 *       if (cert->wCertificateType == WIN_CERT_TYPE_PKCS_SIGNED_DATA ||
 *           cert->wCertificateType == WIN_CERT_TYPE_X509)
 *          use 'libssl_1.1.dll' and call 'PKCS7_verify()' on 'cert+1'
 *
 *       Refs:
 *         https://github.com/zed-0xff/pedump/blob/master/lib/pedump/security.rb
 *         http://pedump.me/1d82d1e52ca97759a6f90438e59e7dc7/#signature
 */
static BOOL get_wintrust_info (const char *file, char *dest, size_t dest_size)
{
  char  *p = dest;
  size_t left = dest_size;
  DWORD  rc = wintrust_check (file, TRUE, FALSE);

  *p = '\0';

  switch (rc)
  {
    case ERROR_SUCCESS:
         p    += snprintf (dest, left, " ~2(Verified");
         left -= p - dest;
         num_verified++;
         break;
    case TRUST_E_NOSIGNATURE:
    case TRUST_E_SUBJECT_FORM_UNKNOWN:
    case TRUST_E_PROVIDER_UNKNOWN:
         p    += snprintf (dest, left, " ~5(Not signed");
         left -= p - dest;
         break;
    case TRUST_E_SUBJECT_NOT_TRUSTED:
         p    += snprintf (dest, left, " ~5(Not trusted");
         left -= p - dest;
         break;
  }

  if (wintrust_signer_subject)
       snprintf (p, left, ", %s)~0.", wintrust_signer_subject);
  else snprintf (p, left, ")~0.");

  wintrust_cleanup();

  switch (opt.signed_status)
  {
    case SIGN_CHECK_NONE:
         return (FALSE);
    case SIGN_CHECK_ALL:
         return (TRUE);
    case SIGN_CHECK_SIGNED:
         if (rc == ERROR_SUCCESS)
            return (TRUE);
         return (FALSE);
    case SIGN_CHECK_UNSIGNED:
         if (rc != ERROR_SUCCESS)
            return (TRUE);
         return (FALSE);
  }
  return (FALSE);
}

UINT64 get_directory_size (const char *dir)
{
  struct dirent2 **namelist = NULL;
  int    i, n = scandir2 (dir, &namelist, NULL, NULL);
  UINT64 size = 0;

  for (i = 0; i < n; i++)
  {
    int   is_dir      = (namelist[i]->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
    int   is_junction = (namelist[i]->d_attrib & FILE_ATTRIBUTE_REPARSE_POINT);
    const char *link;

    if (is_junction)
    {
      link = namelist[i]->d_link ? namelist[i]->d_link : "?";
      DEBUGF (1, "Not recursing into junction \"%s\"\n", link);
      size += get_file_alloc_size (dir, (UINT64)-1);
    }
    else if (is_dir)
    {
      DEBUGF (1, "Recursing into \"%s\"\n", namelist[i]->d_name);
      size += get_file_alloc_size (namelist[i]->d_name, (UINT64)-1);
      size += get_directory_size (namelist[i]->d_name);
    }
    else
      size += get_file_alloc_size (namelist[i]->d_name, namelist[i]->d_fsize);
  }

  while (n--)
    FREE (namelist[n]);
  FREE (namelist);

  return (size);
}

/**
 * Return the indentation needed for the next 'she-bang' or 'man-file link'
 * to align up more nicely.
 * Not ideal since we don't know the length of all files we need to report.
 */
static int get_trailing_indent (const char *file)
{
  static int longest_file_so_far = 0;
  static int indent = 0;
  int    len = (int) strlen (file);

  if (longest_file_so_far == 0 || len > longest_file_so_far)
     longest_file_so_far = len;

  if (len <= longest_file_so_far)
     indent = 1 + longest_file_so_far - len;

  DEBUGF (2, "longest_file_so_far: %d, len: %d, indent: %d\n",
          longest_file_so_far, len, indent);
  return (indent);
}

/**
 * This is the main printer for a file/dir.
 * Prints any notes, time-stamp, size, file/dir name.
 * Also any she-bang statements, links for a gzipped man-page,
 * PE-information like resource version or trust information
 * and file-owner.
 *
 * \param[in] file         the file or directory to report.
 * \param[in] mtime        the modification time of the file or directory (-1 if unknown).
 * \param[in] fsize        the allocated size of the file or directory (-1 if unknown).
 * \param[in] is_dir       TRUE if the `file` is a directory.
 * \param[in] is_junction  TRUE if file (i.e. directory) was a reparse-point) not used yet.
 * \param[in] key          the (pseudo) key the search was a result of.
 */
int report_file (const char *file, time_t mtime, UINT64 fsize, BOOL is_dir, BOOL is_junction, HKEY key)
{
  const char *note   = NULL;
  const char *filler = "      ";
  char        size [40] = "?";
  int         save;
  BOOL        have_it = TRUE;
  BOOL        show_dir_size = TRUE;
  BOOL        show_pc_files_only = FALSE;
  BOOL        show_this_file = TRUE;
  BOOL        possible_PE_file = TRUE;

  FMT_buf     fmt_buf_time_size;
  FMT_buf     fmt_buf_file_info;
  FMT_buf     fmt_buf_owner_info;
  FMT_buf     fmt_buf_ver_info;
  FMT_buf     fmt_buf_trust_info;

  BUF_INIT (&fmt_buf_time_size, 100);
  BUF_INIT (&fmt_buf_file_info, 100 + _MAX_PATH);
  BUF_INIT (&fmt_buf_owner_info, 100);
  BUF_INIT (&fmt_buf_ver_info, 100);
  BUF_INIT (&fmt_buf_trust_info, 100);

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
    possible_PE_file = FALSE;
    note = " (5)  ";
  }
  else if (key == HKEY_EVERYTHING)
  {
#if (IS_WIN64)
    /*
     * If e.g. a 32-bit EveryThing program is finding matches is "%WinDir\\System32",
     * don't set 'found_everything_db_dirty=1' when we don't 'have_sys_native_dir'.
     */
    if (mtime == 0 &&
        (!have_sys_native_dir || !strnicmp(file,sys_native_dir,strlen(sys_native_dir))))
       have_it = FALSE;
#endif

    if (have_it && mtime == 0 && !(is_dir ^ opt.dir_mode))
    {
      found_everything_db_dirty = 1;
      note = " (6)  ";
    }
  }
  else if (key == HKEY_EVERYTHING_ETP)
  {
    show_dir_size = FALSE;
    possible_PE_file = FALSE;
  }
  else if (key == HKEY_PKGCONFIG_FILE)
  {
    show_pc_files_only = TRUE;
    possible_PE_file = FALSE;
  }
  else
  {
    found_in_default_env = 1;
  }

  if (is_dir)
     note = "<DIR> ";

  if ((!is_dir && opt.dir_mode) || !have_it)
  {
    show_this_file = FALSE;
    return (0);
  }

  if (show_pc_files_only)
  {
    const char *ext = get_file_ext (file);

    if (stricmp(ext,"pc"))
    {
      show_this_file = FALSE;
      return (0);
    }
  }

 /*
  * Recursively get the size of files under directory matching 'file'.
  * The ETP-server (key == HKEY_EVERYTHING_ETP) can not reliably report size
  * of directories.
  */
  if (opt.show_size && opt.dir_mode && show_dir_size)
  {
    if (is_dir)
       fsize = get_directory_size (file);
    snprintf (size, sizeof(size), " - %s", get_file_size_str(fsize));
    total_size += fsize;
  }
  else if (opt.show_size)
  {
    snprintf (size, sizeof(size), " - %s", get_file_size_str(fsize));
    if (fsize < (__int64)-1)
    {
      if (key == HKEY_EVERYTHING_ETP)
           total_size += fsize;
      else total_size += get_file_alloc_size (file, fsize);
    }
  }
  else
    size[0] = '\0';

  if (report_header)
     C_printf ("~3%s~0", report_header);

  report_header = NULL;

  if (possible_PE_file && opt.PE_check)
  {
    static DWORD num_version_ok_last = 0;

    show_this_file = get_PE_file_brief (file, filler, key, fmt_buf_ver_info.buffer_start, fmt_buf_ver_info.buffer_size);
    if (show_this_file && opt.signed_status != SIGN_CHECK_NONE)
    {
      show_this_file = get_wintrust_info (file, fmt_buf_trust_info.buffer_start, fmt_buf_trust_info.buffer_size);
      if (!show_this_file && num_version_ok_last < num_version_ok)
         num_version_ok--;  /* Fix this counter for the  'final_report()' */
    }
    num_version_ok_last = num_version_ok;
  }

  buf_printf (&fmt_buf_time_size, "~3%s~0%s%s: ",
              note ? note : filler, get_time_str(mtime), size);

  /* The remote 'file' from EveryThing is not something Windows knows
   * about. Hence no point in trying to get the DomainName + AccountName
   * for it.
   */
  if (opt.show_owner && key != HKEY_EVERYTHING_ETP)
  {
    char       *account_name;
    const char *found_owner = NULL;
    BOOL        inverse = FALSE;

    if (get_file_owner(file, NULL, &account_name))
    {
      int i, max = smartlist_len (opt.owners);

      /* Show only the file/directory if it matches (or not matches) one of the
       * owners in 'opt.owners'.
       * With 'opt.owners == "*"', match all.
       * With 'opt.owners == "!*"', match none.
       *
       * E.g. with:
       *   envtool --man --owner=Admin*  pkcs7*
       *   show only Man-pages matching "pkcs7*" and owners "Admin*":
       *
       *   envtool --man --owner=!Admin* pkcs7*
       *   show only Man-pages matching "pkcs7*" and owners not matching "Admin*":
       */
      if (max > 0)
         show_this_file = FALSE; /* Assume no, if there are >= 1 owner-patterns to check for */

      for (i = 0; i < max; i++)
      {
        const char *owner = smartlist_get (opt.owners, i);

        if (owner[0] == '!' && fnmatch(owner+1, account_name, FNM_FLAG_NOCASE) == FNM_NOMATCH)
        {
          inverse = TRUE;
          found_owner = owner + 1;
          show_this_file = TRUE;
          break;
        }
        else if (fnmatch(owner, account_name, FNM_FLAG_NOCASE) == FNM_MATCH)
        {
          found_owner = owner;
          show_this_file = TRUE;
          break;
        }
      }
    }

    if (found_owner)
    {
      DEBUGF (2, "account_name (%s) %smatches owner (%s).\n", account_name, inverse ? "does not " : "", found_owner);
      buf_printf (&fmt_buf_owner_info, "%-18s", str_shorten(account_name,18));
    }
    else
    {
      buf_printf (&fmt_buf_owner_info, "%-18s", "<None>");
      DEBUGF (2, "account_name (%s) did not match any wanted owner(s) for file '%s'.\n",
              account_name, basename(file));
    }
    FREE (account_name);
  }

  /* slashify2() will remove excessive '/' or '\\' anywhere in the name.
   * Add a trailing slash to directories.
   */
  buf_printf (&fmt_buf_file_info, "%s%c", file, is_dir ? DIR_SEP: '\0');
  slashify2 (fmt_buf_file_info.buffer_start, fmt_buf_file_info.buffer_start,
             opt.show_unix_paths ? '/' : '\\');

  if (!is_dir && key == HKEY_MAN_FILE)
  {
    const char *link = get_man_link (file);
    const char *ext  = get_file_ext (file);

    if (!link && !isdigit((int)*ext))
       link = get_gzip_link (file);
    if (link)
       buf_printf (&fmt_buf_file_info, "%*s(%s)", get_trailing_indent(file), " ", link);
  }
  else if (!is_dir)
  {
    const char *shebang = check_if_shebang (file);

    if (shebang)
       buf_printf (&fmt_buf_file_info, "%*s(%s)", get_trailing_indent(file), " ", shebang);
  }

  if (!show_this_file)
  {
    get_PE_version_info_free();
    return (0);
  }

  C_puts (fmt_buf_time_size.buffer_start);
  C_puts (fmt_buf_owner_info.buffer_start);

  /* In case 'report_file_info' contains a "~" (SFN), we switch to raw mode.
   */
  save = C_setraw (1);
  C_puts (fmt_buf_file_info.buffer_start);
  C_setraw (save);

  /* All this must be printed on the next line
   */
  if (opt.PE_check && fmt_buf_ver_info.buffer_start[0])
  {
    C_printf ("%-60s", fmt_buf_ver_info.buffer_start);
    C_puts (fmt_buf_trust_info.buffer_start);
    print_PE_file_details (filler);
  }

  C_putc ('\n');

  ARGSUSED (is_junction);
  return (1);
}

static void final_report (int found)
{
  BOOL do_warn = FALSE;
  char duplicates [50] = "";
  char ignored [50] = "";

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

   if (found_everything_db_dirty)
      C_puts ("~3 (6): EveryThing database is not up-to-date.~0\n");

  if (do_warn)
     C_printf ("\n"
               "  ~5The search found matches outside the default environment (PATH etc.).\n"
               "  Hence running an application from the Start-Button may result in different .EXE/.DLL\n"
               "  to be loaded than from the command-line. Revise the above registry-keys.\n\n~0");

  if (num_evry_dups)
     snprintf (duplicates, sizeof(duplicates), " (%lu duplicated)",
               (unsigned long)num_evry_dups);
  else if (ETP_num_evry_dups)
     snprintf (duplicates, sizeof(duplicates), " (%lu duplicated)",
               (unsigned long)ETP_num_evry_dups);

  if (num_evry_ignored)
     snprintf (ignored, sizeof(ignored), " (%lu ignored)",
               (unsigned long)num_evry_ignored);

  C_printf ("%s match%s found for \"%s\"%s%s.",
            dword_str((DWORD)found), (found == 0 || found > 1) ? "es" : "", opt.file_spec, duplicates, ignored);

  if (opt.show_size && total_size > 0)
     C_printf (" Totalling %s (%s bytes). ",
               str_trim((char*)get_file_size_str(total_size)), qword_str(total_size));

  if (opt.evry_host)
  {
    if (opt.debug >= 1 && ETP_total_rcv)
       C_printf ("\n%s bytes received from ETP-host(s).", dword_str(ETP_total_rcv));
  }
  else if (opt.PE_check)
  {
    C_printf (" %lu have PE-version info.", (unsigned long)num_version_ok);

    if (opt.signed_status != SIGN_CHECK_NONE)
       C_printf (" %lu %s verified.",
                 (unsigned long)num_verified, plural_str(num_verified,"is","are"));
  }
  C_putc ('\n');
}

/**
 * Check for suffix or trailing wildcards. If not found, add a
 * trailing `"*"`.
 *
 * If `opt.file_spec` starts with a subdir(s) part, return that in
 * `*sub_dir` with a trailing `DIR_SEP`. And return a `fspec`
 * without the sub-dir part.
 *
 * Not used in `"--evry"` search.
 */
static char *fix_filespec (char **sub_dir)
{
  static char fname  [_MAX_PATH];
  static char subdir [_MAX_PATH];
  char  *p, *fspec = _strlcpy (fname, opt.file_spec, sizeof(fname));
  char  *lbracket, *rbracket;

  /**
   * If we do e.g. `"envtool --inc openssl/ssl.h"`, we must preserve
   * the subdir part since `FindFirstFile()` doesn't give us this subdir
   * part in `ff_data.cFileName`. It just returns the matching file(s)
   * \b within that sub-directory.
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

 /**
  * Since FindFirstFile() doesn't work with POSIX ranges, replace
  * the range part in `fspec` with a `*`. This could leave a
  * `**` in `fspec`, but that doesn't hurt.
  *
  * \note We must still use `opt.file_spec` in `fnmatch()` for
  *       a POSIX range to work below.
  */
  lbracket = strchr (fspec, '[');
  rbracket = strchr (fspec, ']');

  if (lbracket && rbracket > lbracket)
  {
    *lbracket = '*';
    _strlcpy (lbracket+1, rbracket+1, strlen(rbracket));
  }

  DEBUGF (1, "fspec: %s, *sub_dir: %s\n", fspec, *sub_dir);

  if (*sub_dir && strpbrk(*sub_dir, "[]*?"))
  {
    /** \todo
     * Check for POSIX ranges in the sub-dir part as we do above
     * for the base `fspec`. The `FindFirstFile()` loop in
     * `process_dir()` should then have another outer \b "directory-loop".
     */
    WARN ("Ignoring wildcards in sub-dir part: '%s%s'.\n", *sub_dir, fspec);
  }
  return (fspec);
}

/**
 * Expand an environment variable for current user or for SYSTEM.
 *
 * \param in         The data to expand the environment variable(s) in.
 * \param out        The buffer that receives the expanded variable.
 * \param out_len    The size of `out` buffer.
 * \param for_system If TRUE, search the `SYSTEM` environment for the `in` variable.
 *
 * \note `in` and `out` can point to the same buffer.
 */
static void expand_env_var (const char *in, char *out, size_t out_len, BOOL for_system)
{
  char *rc;

  if (for_system)
        rc = getenv_expand_sys (in);
   else rc = getenv_expand (in);

  if (rc)
  {
    _strlcpy (out, rc, out_len);
    FREE (rc);
  }
  else
    _strlcpy (out, "<none>", out_len);
}

static BOOL enum_sub_values (HKEY top_key, const char *key_name, const char **ret)
{
  HKEY   key = NULL;
  u_long num;
  DWORD  rc;
  REGSAM acc = reg_read_access();
  const char *ext = strrchr (key_name, '.');

  *ret = NULL;
  rc = RegOpenKeyEx (top_key, key_name, 0, acc, &key);

  DEBUGF (1, "  RegOpenKeyEx (%s\\%s, %s):\n                  %s\n",
          reg_top_key_name(top_key), key_name, reg_access_name(acc), win_strerror(rc));

  if (rc != ERROR_SUCCESS)
  {
    WARN ("    Error opening registry key \"%s\\%s\", rc=%lu\n",
          reg_top_key_name(top_key), key_name, (u_long)rc);
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
       expand_env_var (data, data, sizeof(data), TRUE);

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
           DEBUGF (1, "    num: %lu, REG_LINK, value: \"%" WIDESTR_FMT "\", data: \"%" WIDESTR_FMT "\"\n",
                      num, (wchar_t*)value, (wchar_t*)data);
           break;

      case REG_DWORD_BIG_ENDIAN:
           val32 = reg_swap_long (*(DWORD*)&data[0]);
           /* fall through */

      case REG_DWORD:
           DEBUGF (1, "    num: %lu, %s, value: \"%s\", data: %lu\n",
                      num, reg_type_name(type), value[0] ? value : "(no value)", (u_long)val32);
           break;

      case REG_QWORD:
           DEBUGF (1, "    num: %lu, REG_QWORD, value: \"%s\", data: %" S64_FMT "\n",
                      num, value[0] ? value : "(no value)", val64);
           break;

      case REG_NONE:
           break;

      default:
           DEBUGF (1, "    num: %lu, unknown REG_type %lu\n", num, (u_long)type);
           break;
    }
  }
  if (key)
     RegCloseKey (key);
  return (*ret != NULL);
}

/**
 * Enumerate all keys under `top_key + REG_APP_PATH` and build up
 * the `reg_array` smartlist.
 *
 * Either under: <br>
 *   `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths` <br>
 * or <br>
 *   `HKEY_CURRENT_USER\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths` <br>
 *
 * The number of entries added is given by `smartlist_len (reg_array)`.
 */
static void build_reg_array_app_path (HKEY top_key)
{
  HKEY   key = NULL;
  int    num;
  REGSAM acc = reg_read_access();
  DWORD  rc  = RegOpenKeyEx (top_key, REG_APP_PATH, 0, acc, &key);

  DEBUGF (1, "  RegOpenKeyEx (%s\\%s, %s):\n                   %s\n",
          reg_top_key_name(top_key), REG_APP_PATH, reg_access_name(acc), win_strerror(rc));

  for (num = 0; rc == ERROR_SUCCESS; num++)
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
       add_to_reg_array (top_key, fname, fqdn);
  }
  if (key)
     RegCloseKey (key);
}

/**
 * Scan registry under: <br>
 *   `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment` <br>
 * and <br>
 *   `HKCU\Environment`
 *
 * and return any `PATH`, `LIB` and `INCLUDE` in them.
 *
 * There can only be one of each of these under each registry `sub_key`.
 * (otherwise the registry is truly messed up). Return first of each found.
 *
 * If one of these still contains a `"%value%"` after expand_env_var(),
 * this is checked later.
 */
static void scan_reg_environment (HKEY top_key, const char *sub_key,
                                  char **path, char **inc, char **lib)
{
  HKEY   key = NULL;
  REGSAM acc = reg_read_access();
  DWORD  num, rc = RegOpenKeyEx (top_key, sub_key, 0, acc, &key);

  DEBUGF (1, "RegOpenKeyEx (%s\\%s, %s):\n                 %s\n",
          reg_top_key_name(top_key), sub_key, reg_access_name(acc), win_strerror(rc));

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
       expand_env_var (value, value, sizeof(value), TRUE);

    if (!stricmp(name,"PATH"))
       *path = STRDUP (value);

    else if (!stricmp(name,"INCLUDE"))
       *inc = STRDUP (value);

    else if (!stricmp(name,"LIB"))
       *lib = STRDUP (value);

#if 0
    DEBUGF (1, "num %2lu, %s, %s=%.40s%s\n",
            (u_long)num, reg_type_name(type), name, value,
            strlen(value) > 40 ? "..." : "");
#else
    DEBUGF (1, "num %2lu, %s, %s=%s\n",
            (u_long)num, reg_type_name(type), name, value);
#endif
  }
  if (key)
     RegCloseKey (key);

  DEBUGF (1, "\n");
}

static int do_check_env2 (HKEY key, const char *env, const char *value)
{
  smartlist_t *list = split_env_var (env, value);
  int          found = 0;
  int          i, max = list ? smartlist_len (list) : 0;

  for (i = 0; i < max; i++)
  {
    const struct directory_array *arr = smartlist_get (list, i);

    found += process_dir (arr->dir, arr->num_dup, arr->exist, arr->check_empty,
                          arr->is_dir, arr->exp_ok, env, key, FALSE);
  }
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

/**
 * Report matches from Registry under
 *   "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths" or
 *   "HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths"
 */
static int report_registry (const char *reg_key)
{
  int i, found, max = smartlist_len (reg_array);

  for (i = found = 0; i < max; i++)
  {
    const struct registry_array *arr = smartlist_get (reg_array, i);
    char  fqfn [_MAX_PATH];
    int   match = FNM_NOMATCH;

    DEBUGF (1, "i=%2d: exist=%d, match=%d, key=%s, fname=%s, path=%s\n",
            i, arr->exist, match, reg_top_key_name(arr->key), arr->fname, arr->path);

    if (!arr->exist)
    {
      snprintf (fqfn, sizeof(fqfn), "%s%c%s", arr->path, DIR_SEP, arr->real_fname);
      if (!cfg_ignore_lookup("[Registry]",fqfn))
         WARN ("\"%s\\%s\" points to\n  '%s'. But this file does not exist.\n\n",
               reg_top_key_name(arr->key), reg_key, fqfn);
    }
    else
    {
      match = fnmatch (opt.file_spec, arr->fname, fnmatch_case(0));
      if (match == FNM_MATCH)
      {
        snprintf (fqfn, sizeof(fqfn), "%s%c%s", arr->path, DIR_SEP, arr->real_fname);
        if (report_file(fqfn, arr->mtime, arr->fsize, FALSE, FALSE, arr->key))
           found++;
      }
    }
  }
  free_reg_array();
  return (found);
}

static int do_check_registry (void)
{
  char reg[300];
  int  found = 0;

  snprintf (reg, sizeof(reg), "Matches in HKCU\\%s:\n", REG_APP_PATH);
  report_header = reg;
  DEBUGF (1, "%s\n", reg);
  build_reg_array_app_path (HKEY_CURRENT_USER);
  sort_reg_array();
  found += report_registry (REG_APP_PATH);

  snprintf (reg, sizeof(reg), "Matches in HKLM\\%s:\n", REG_APP_PATH);
  report_header = reg;
  DEBUGF (1, "%s\n", reg);
  build_reg_array_app_path (HKEY_LOCAL_MACHINE);
  sort_reg_array();
  found += report_registry (REG_APP_PATH);

  report_header = NULL;
  return (found);
}

/**
 * Check if directory is empty (no files or directories except
 * `"."` and `".."`).
 *
 * \note It is quite normal that e.g. `"%INCLUDE"` contain a directory with
 *       no .h-files but at least 1 subdirectory with .h-files.
 */
static BOOL dir_is_empty (const char *dir)
{
  HANDLE          handle;
  WIN32_FIND_DATA ff_data;
  char            path  [_MAX_PATH];
  int             num_entries = 0;

  snprintf (path, sizeof(path), "%s\\*", dir);
  handle = FindFirstFile (path, &ff_data);
  if (handle == INVALID_HANDLE_VALUE)
     return (FALSE);    /* We really can't tell */

  do
  {
    if (strcmp(ff_data.cFileName,".") && strcmp(ff_data.cFileName,".."))
       num_entries++;
  }
  while (num_entries == 0 && FindNextFile(handle, &ff_data));

  DEBUGF (3, "%s(): at least %d entries in '%s'.\n", __FUNCTION__, num_entries, dir);
  FindClose (handle);
  return (num_entries == 0);
}

/*
 * Try to match 'str' against the global regular expression in 'opt.file_spec'.
 */
static BOOL regex_match (const char *str)
{
  memset (&re_matches, '\0', sizeof(re_matches));
  re_err = regexec (&re_hnd, str, DIM(re_matches), re_matches, 0);
  DEBUGF (3, "regex() pattern '%s' against '%s'. re_err: %d\n", opt.file_spec, str, re_err);

  if (re_err == REG_NOMATCH)
     return (FALSE);

  if (re_err == REG_NOERROR)
     return (TRUE);

  regerror (re_err, &re_hnd, re_errbuf, sizeof(re_errbuf));
  DEBUGF (0, "Error while matching \"%s\": %d\n", str, re_err);
  return (FALSE);
}

/*
 * Process directory specified by 'path' and report any matches
 * to the global 'opt.file_spec'.
 */
int process_dir (const char *path, int num_dup, BOOL exist, BOOL check_empty,
                 BOOL is_dir, BOOL exp_ok, const char *prefix, HKEY key,
                 BOOL recursive)
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
#if 0     /* \todo */
    WARN ("%s: directory \"%s\" is duplicated at position %d. Skipping.\n", prefix, path, dup_pos);
#else
    WARN ("%s: directory \"%s\" is duplicated. Skipping.\n", prefix, path);
#endif
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

  if (check_empty && is_dir && dir_is_empty(path))
     WARN ("%s: directory \"%s\" is empty.\n", prefix, path);

  if (!fspec)
     fspec = (opt.use_regex ? "*" : fix_filespec(&subdir));

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
    BOOL   is_junction;
    BOOL   ignore = /* opt.use_regex && */
                    ((ff_data.cFileName[0] == '.' && ff_data.cFileName[1] == '\0') ||
                    !strcmp(ff_data.cFileName,".."));
    if (ignore)
       continue;

    len  = snprintf (fqfn, sizeof(fqfn), "%s%c", path, DIR_SEP);
    base = fqfn + len;
    snprintf (base, sizeof(fqfn)-len, "%s%s", subdir ? subdir : "", ff_data.cFileName);

    is_dir      = ((ff_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    is_junction = ((ff_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);

    if (opt.use_regex)
    {
      if (regex_match(fqfn) && safe_stat(fqfn, &st, NULL) == 0)
      {
        if (report_file(fqfn, st.st_mtime, st.st_size, is_dir, is_junction, key))
        {
          found++;
       // regex_print (&re_hnd, re_matches, fqfn);
        }
      }
      continue;
    }

    file  = slashify2 (fqfn, fqfn, DIR_SEP);
    match = fnmatch (opt.file_spec, base, fnmatch_case(0) | FNM_FLAG_NOESCAPE);

#if 0
    if (opt.man_mode)
    {
      DEBUGF (2, "opt.file_spec: \"%s\", base: \"%s\".\n", opt.file_spec, base);
      if (match == FNM_NOMATCH)
         continue;
    }

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
      if (!is_dir && !opt.dir_mode && !opt.man_mode &&
          !str_equal_n(base,opt.file_spec,strlen(base)))
         match = FNM_MATCH;
    }

    DEBUGF (1, "Testing \"%s\". is_dir: %d, is_junction: %d, %s\n",
            file, is_dir, is_junction, fnmatch_res(match));

    if (match == FNM_MATCH && safe_stat(file, &st, NULL) == 0)
    {
      if (report_file(file, st.st_mtime, st.st_size, is_dir, is_junction, key))
         found++;
    }
  }
  while (FindNextFile(handle, &ff_data));

  FindClose (handle);
  ARGSUSED (recursive);
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
    case EVERYTHING_ERROR_INVALIDREQUEST:
         return ("Invalid request data");
    case EVERYTHING_ERROR_INVALIDPARAMETER:
         return ("Bad parameter");
  }
  snprintf (buf, sizeof(buf), "Unknown error %lu", (u_long)err);
  return (buf);
}

static void check_sys_dir (const char *dir, const char *name, BOOL *have_it)
{
  DWORD attr = GetFileAttributes (dir);
  BOOL  is_dir = (attr != INVALID_FILE_ATTRIBUTES) &&
                 (attr & FILE_ATTRIBUTE_DIRECTORY);

  if (is_dir)
       DEBUGF (1, "%s: '%s' okay\n", name, dir);
  else DEBUGF (1, "%s: '%s', GetLastError(): %lu\n", name, dir, (unsigned long)GetLastError());

  if (have_it)
     *have_it = is_dir;
}

static void check_sys_dirs (void)
{
  check_sys_dir (sys_dir, "sys_dir", NULL);
#if (IS_WIN64 == 0)
  check_sys_dir (sys_native_dir, "sys_native_dir", &have_sys_native_dir);
  check_sys_dir (sys_wow64_dir, "sys_wow64_dir", &have_sys_wow64_dir);
#else
  ARGSUSED (have_sys_wow64_dir);
#endif
}

/*
 * Figure out if 'file' can have a shadow in '%WinDir%\sysnative'.
 * Makes no sense on Win64.
 */
static const char *get_sysnative_file (const char *file, struct stat *st)
{
#if (IS_WIN64 == 0)
  if (!str_equal_n(sys_dir,file,strlen(sys_dir)) && sys_native_dir[0])
  {
    static char shadow [_MAX_PATH];

    snprintf (shadow, sizeof(shadow), "%s\\%s", sys_native_dir, file+strlen(sys_dir)+1);
    safe_stat (shadow, st, NULL);
    return (shadow);
  }
#else
  ARGSUSED (st);
#endif
  return (file);
}

/**
 * \todo If the result returns a file on a remote disk (X:) and the
 *       remote computer is down, EveryThing will return the
 *       the entry in it's database. But then the stat() below
 *       will fail after a long SMB timeout (SessTimeOut, default 60 sec).
 *
 *       Try to detect this if 'file[0:1]' is 'X:' prior to calling
 *       'stat()'. Use:
 *         GetFileAttributes(file) and test if GetLastError()
 *         returns ERROR_BAD_NETPATH.  ??
 *
 *  Or simply exclude the remote disk `X:` in the query. E.g.:
 *   ```
 *    C:\> envtool --evry foxitre*.exe
 *   ```
 *  should query for:
 *   ```
 *    ^[^X]:\\.*foxitre.*\.exe$
 *   ```
 *
 * But then we need a-priori knowledge that `X:` is remote. Like
 *  `C:\> net use` does:
 *  \code
 *   Status       Local     External                  Network
 *   -------------------------------------------------------------------------------
 *   Disconnected X:        \\DONALD\X-PARTITION      Microsoft Windows Network
 *   ^^
 *   \endcode
 *  where to get this state?
 */
static int report_evry_file (const char *file, time_t mtime, UINT64 fsize, BOOL *is_shadow)
{
  struct stat st;
  BOOL        is_dir = FALSE;
  const char *file2;
  DWORD       attr;

  memset (&st, '\0', sizeof(st));
  *is_shadow = FALSE;

  /* Do not use the slower 'safe_stat()' unless needed.
   * See below.
   */
  attr = GetFileAttributes (file);
  if (attr != INVALID_FILE_ATTRIBUTES)
     is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY);

  else if (attr == INVALID_FILE_ATTRIBUTES)
  {
    file2 = get_sysnative_file (file, &st);
    if (file2 != file)
    {
      DEBUGF (1, "shadow: '%s' -> '%s'\n", file, file2);
      *is_shadow = TRUE;
    }
    file = file2;
  }

  if (st.st_mtime == 0)
  {
    /* If EveryThing older than 1.4.1 was used, these are not set.
     */
    if (mtime == 0)
    {
      safe_stat (file, &st, NULL);
      mtime = st.st_mtime;
    }
    if (fsize == (__int64)-1)
    {
      safe_stat (file, &st, NULL);
      fsize = st.st_size;
    }
  }

  return report_file (file, mtime, is_dir ? (__int64)-1 : fsize,
                      is_dir, FALSE, HKEY_EVERYTHING);
}

/*
 * Check if EveryThing database is loaded and not busy indexing itself.
 */
static BOOL evry_IsDBLoaded (HWND wnd)
{
  int loaded = 0;
  int busy = 0;

  if (wnd)
  {
    loaded = (int) SendMessage (wnd, WM_USER, EVERYTHING_IPC_IS_DB_LOADED, 0);
    busy   = (int) SendMessage (wnd, WM_USER, EVERYTHING_IPC_IS_DB_BUSY, 0);
  }
  DEBUGF (1, "wnd: %p, loaded: %d, busy: %d.\n", wnd, loaded, busy);
  return (loaded && !busy);
}

static int do_check_evry (void)
{
  DWORD  i, err, num, request_flags, version = 0;
  char   query_buf [_MAX_PATH+8];
  char  *query = query_buf;
  char  *dir   = NULL;
  char  *base  = NULL;
  int    len, found = 0;
  HWND   wnd;
  struct ver_info evry_ver = { 0, 0, 0, 0 };

  wnd = FindWindow (EVERYTHING_IPC_WNDCLASS, 0);
  if (!wnd)
  {
    C_printf ("  Everything search engine not found.\n");
    return (0);
  }

  num_evry_dups = 0;

  if (evry_bitness == bit_unknown)
     get_evry_bitness (wnd);

  if (get_evry_version(wnd,&evry_ver))
     version = (evry_ver.val_1 << 16) + (evry_ver.val_2 << 8) + evry_ver.val_3;

  DEBUGF (1, "version %u.%u.%u, build: %u\n",
          evry_ver.val_1,
          evry_ver.val_2,
          evry_ver.val_3,
          evry_ver.val_4);

  if (opt.evry_raw)
     query = opt.file_spec;
  else
  {
    /* EveryThing seems not to support '\\'. Must split the 'opt.file_spec'
     * into a 'dir' and 'base' part.
     */
    if (strpbrk(opt.file_spec, "/\\"))
    {
      dir  = dirname (opt.file_spec);   /* Allocates memory */
      base = basename (opt.file_spec);
    }

    /* With option '-D' / '--dir', match only folders.
     */
    if (opt.dir_mode && !opt.use_regex)
    {
      snprintf (query_buf, sizeof(query_buf), "regex:^%s$ folder:", translate_shell_pattern(opt.file_spec));
      DEBUGF (2, "Simple directory mode: '%s'\n", query_buf);
    }
    else
    {
      if (opt.use_regex)
      {
        len = snprintf (query_buf, sizeof(query_buf), "regex:%s", opt.file_spec);
        if (opt.dir_mode)
           snprintf (query_buf+len, sizeof(query_buf)-len, " folder:");
      }
      else if (dir)
      {
        /* If user didn't use the '-r/--regex' option, we must convert
         * 'opt.file_spec into' a RegExp compatible format.
         * E.g. "ez_*.py" -> "^ez_.*\.py$"
         */
        len = snprintf (query_buf, sizeof(query_buf), "regex:%s\\\\%s", dir, base);
      }
      else
        len = snprintf (query_buf, sizeof(query_buf), "regex:^%s$", translate_shell_pattern(opt.file_spec));
    }

#if 0   /* \todo Query contents with option "--grep" */
    if (opt.evry_grep && len > 0)
       snprintf (query_buf+len, sizeof(query_buf)-len, "content: %s", opt.evry_grep);
#endif

    FREE (dir);
  }

  Everything_SetMatchCase (opt.case_sensitive);

  DEBUGF (1, "Everything_SetSearch (\"%s\").\n"
             "                 Everything_SetMatchCase (%d).\n",
             query, opt.case_sensitive);

  request_flags = Everything_GetRequestFlags();

  /* The request flags: EVERYTHING_REQUEST_SIZE and/or EVERYTHING_REQUEST_DATE_MODIFIED
   * needs v. 1.4.1 or later.
   * Ref:
   *   http://www.voidtools.com/support/everything/sdk/everything_setrequestflags/
   */
  if (version >= 0x010401)
  {
    request_flags |= EVERYTHING_REQUEST_SIZE | EVERYTHING_REQUEST_DATE_MODIFIED;
    Everything_SetRequestFlags (request_flags);
    request_flags = Everything_GetRequestFlags();  /* should be the same as set above */
  }

  Everything_SetSearchA (query);
  Everything_QueryA (TRUE);

  err = Everything_GetLastError();
  DEBUGF (1, "Everything_Query: %s\n", evry_strerror(err));

  if (halt_flag > 0)
     return (0);

  if (err == EVERYTHING_ERROR_IPC)
  {
    WARN ("Everything IPC service is not running.\n");
    return (0);
  }
  if (!evry_IsDBLoaded(wnd))
  {
    WARN ("Everything is busy loading it's database.\n");
    return (0);
  }

  num = Everything_GetNumResults();
  DEBUGF (1, "Everything_GetNumResults() num: %lu, err: %s\n",
          (u_long)num, evry_strerror(Everything_GetLastError()));

  if (num == 0)
  {
    if (opt.use_regex)
         WARN ("Nothing matched your regexp \"%s\".\n"
               "Are you sure it is correct? Try quoting it.\n",
               opt.file_spec);
    else WARN ("Nothing matched your search \"%s\".\n"
               "Are you sure all NTFS disks are indexed by EveryThing? Try adding folders manually.\n",
               opt.file_spec);
    return (0);
  }

  /* Sort results by path (ignore case).
   * This will fail if 'request_flags' has either 'EVERYTHING_REQUEST_SIZE'
   * or 'EVERYTHING_REQUEST_DATE_MODIFIED' since version 2 of the query protocol
   * is used.
   * Ref: The comment in Everything.c; "//TODO: sort list2"
   */
  Everything_SortResultsByPath();
  err = Everything_GetLastError();
  if (err != EVERYTHING_OK)
  {
    DEBUGF (2, "Everything_SortResultsByPath(), err: %s\n", evry_strerror(err));
    Everything_SetLastError (EVERYTHING_OK);
  }

  for (i = 0; i < num; i++)
  {
    static char prev [_MAX_PATH];
    char   file [_MAX_PATH];
    UINT64 fsize = (__int64)-1;  /* since a 0-byte file is valid */
    time_t mtime = 0;
    BOOL   is_shadow = FALSE;

    if (halt_flag > 0)
       break;

    len = Everything_GetResultFullPathName (i, file, sizeof(file));
    err = Everything_GetLastError();
    if (len == 0 || err != EVERYTHING_OK)
    {
      DEBUGF (2, "Everything_GetResultFullPathName(), err: %s\n",
              evry_strerror(err));
      len = 0;
      break;
    }

    if (cfg_ignore_lookup("[EveryThing]",file))
    {
      num_evry_ignored++;
      continue;
    }

    if (request_flags & EVERYTHING_REQUEST_DATE_MODIFIED)
    {
      FILETIME ft;

      if (Everything_GetResultDateModified(i,&ft))
      {
        mtime = FILETIME_to_time_t (&ft);
        DEBUGF (2, "%3lu: Everything_GetResultDateModified(), mtime: %.24s\n",
                i, mtime ? ctime(&mtime) : "<N/A>");
      }
      else
      {
        err = Everything_GetLastError();
        DEBUGF (2, "%3lu: Everything_GetResultDateModified(), err: %s\n",
                i, evry_strerror(err));
      }
    }
    if (request_flags & EVERYTHING_REQUEST_SIZE)
    {
      LARGE_INTEGER fs;

      if (Everything_GetResultSize(i,&fs))
      {
        fsize = ((UINT64)fs.u.HighPart << 32) + fs.u.LowPart;
        DEBUGF (2, "%3lu: Everything_GetResultSize(), %s\n",
                i, get_file_size_str(fsize));
      }
      else
      {
        err = Everything_GetLastError();
        DEBUGF (2, "%3lu: Everything_GetResultSize(), err: %s\n",
                i, evry_strerror(err));
      }
    }

    /* Clear any error so the next Everything_XX() won't
     * trigger an error.
     */
    Everything_SetLastError (EVERYTHING_OK);

    if (len > 0)
    {
      if (!opt.dir_mode && prev[0] && !strcmp(prev, file))
         num_evry_dups++;
      else if (report_evry_file(file, mtime, fsize, &is_shadow))
         found++;
      if (!is_shadow)
         _strlcpy (prev, file, sizeof(prev));
    }
  }
  return (found);
}

/*
 * The main work-horse of this program.
 */
static int do_check_env (const char *env_name, BOOL recursive)
{
  smartlist_t *list;
  int          i, max, found = 0;
  BOOL         check_empty = FALSE;
  char        *orig_e = getenv_expand (env_name);

  if (!orig_e)
  {
    DEBUGF (1, "Env-var %s not defined.\n", env_name);
    return (0);
  }

  if (!strcmp(env_name,"PATH") ||
      !strcmp(env_name,"LIB") ||
      !strcmp(env_name,"LIBRARY_PATH") ||
      !strcmp(env_name,"INCLUDE") ||
      !strcmp(env_name,"C_INCLUDE_PATH") ||
      !strcmp(env_name,"CPLUS_INCLUDE_PATH"))
    check_empty = TRUE;

  list = split_env_var (env_name, orig_e);
  max  = smartlist_len (list);
  for (i = 0; i < max; i++)
  {
    struct directory_array *arr = smartlist_get (list, i);

    if (check_empty && arr->exist)
       arr->check_empty = check_empty;
    found += process_dir (arr->dir, arr->num_dup, arr->exist, arr->check_empty,
                          arr->is_dir, arr->exp_ok, env_name, NULL,
                          recursive);
  }
  free_dir_array();
  FREE (orig_e);
  return (found);
}

/*
 * Check a single MANPAGE directory for man-page match(es).
 */
static int check_man_dir (const char *dir, const char *env_name)
{
  const char *_dir;

  if (!strcmp(dir,"\\"))
       _dir = current_dir;
  else _dir = dir;

  DEBUGF (1, "Looking for \"%s\" in \"%s\".\n", opt.file_spec, _dir);
  return process_dir (dir, 0, TRUE, TRUE, 1, TRUE, env_name, HKEY_MAN_FILE, FALSE);
}

/*
 * The MANPATH checking needs to be recursive (1 level); check all
 * 'man*' and 'cat*' directories under each directory in %MANPATH.
 *
 * If ".\\" (or "./") is in '%MANPATH', test for existence in 'current_dir'
 * first.
 */
static int do_check_manpath (void)
{
  struct directory_array *arr;
  smartlist_t *list;
  int    i, j, max, found = 0;
  char  *orig_e;
  char   report [300];
  BOOL   save1, save2, done_cwd = FALSE;
  static const char env_name[] = "MANPATH";

  /* \todo
   *   This should be all directories matching "man?[pn]" or "cat?[pn]".
   *   Make this array into a smartlist too?
   */
  static const char *sub_dirs[] = { "cat1", "cat2", "cat3", "cat4", "cat5",
                                    "cat6", "cat7", "cat8", "cat9",
                                    "man1", "man2", "man3", "man4", "man5",
                                    "man6", "man7", "man8", "man9", "mann"
                                  };

  /* Do not implicit add current directory in searches.
   * Unless '%MANPATH' contain a "./;" or a ".\;".
   */
  save1 = opt.no_cwd;
  opt.no_cwd = 1;

  orig_e = getenv_expand (env_name);
  list   = orig_e ? split_env_var (env_name, orig_e) : NULL;
  if (!list)
  {
    WARN ("Env-var %s not defined.\n", env_name);
    opt.no_cwd = save1;
    return (0);
  }

  snprintf (report, sizeof(report), "Matches in %%%s:\n", env_name);
  report_header = report;

  /* Man-files should have an extension. Hence do not report dotless files as a
   * match in process_dir().
   */
  save2 = opt.man_mode;
  opt.man_mode = 1;

  max = smartlist_len (list);
  for (i = 0; i < max; i++)
  {
    arr = smartlist_get (list, i);
    if (!arr->exist)
    {
      WARN ("%s: directory \"%s\" doesn't exist.\n", env_name, arr->dir);
      continue;
    }

    /* If '%MANPATH' contains a "./", check in current-dir.
     * If the path contains multiple "./", do this only once (with a warning).
     * Also if current-dir contains "man*" sub-directories, check them too in the
     * below for-loop.
     */
    if (arr->is_cwd)
    {
      if (done_cwd)
           WARN ("%s: Contains multiple \"%s\".\n", env_name, ".\\");
      else found += check_man_dir (".\\", env_name);
      done_cwd = TRUE;
    }

    for (j = 0; j < 1+DIM(sub_dirs); j++)
    {
      char dir [_MAX_PATH];

      if (j == 0)
           _strlcpy (dir, arr->dir, sizeof(dir));
      else snprintf (dir, sizeof(dir), "%s%c%s", arr->dir, DIR_SEP, sub_dirs[j-1]);
      if (FILE_EXISTS(dir))
         found += check_man_dir (dir, env_name);
    }
  }

  opt.no_cwd   = save1;
  opt.man_mode = save2;
  free_dir_array();
  FREE (orig_e);
  return (found);
}

/*
 * Find the version and location pkg-config.exe (on PATH).
 */
static int pkg_config_major, pkg_config_minor;

static int find_pkg_config_version_cb (char *buf, int index)
{
  ARGSUSED (index);
  if (sscanf (buf, "%d.%d", &pkg_config_major, &pkg_config_minor) == 2)
     return (1);
  return (0);
}

static int get_pkg_config_info (char **exe_p, struct ver_info *ver)
{
  char  exe_copy [_MAX_PATH];
  char *exe;

  *exe_p = NULL;
  pkg_config_major = pkg_config_minor = -1;
  exe = searchpath ("pkg-config.exe", "PATH");
  if (exe == NULL)
     return (0);

  slashify2 (exe_copy, exe, '\\');

  if (popen_runf(find_pkg_config_version_cb, "\"%s\" --version", exe_copy) > 0)
  {
    ver->val_1 = pkg_config_major;
    ver->val_2 = pkg_config_minor;
    *exe_p = STRDUP (exe);
    return (1);
  }
  return (0);
}

/*
 * Find the version and location vcpkg.exe (on PATH).
 */
static int vcpkg_major, vcpkg_minor, vcpkg_micro;

static int find_vcpkg_version_cb (char *buf, int index)
{
  ARGSUSED (index);
  if (sscanf(buf, "Vcpkg package management program version %d.%d.%d", &vcpkg_major, &vcpkg_minor, &vcpkg_micro) >= 2)
     return (1);
  return (0);
}

static int get_vcpkg_info (char **exe_p, struct ver_info *ver)
{
  char  exe_copy [_MAX_PATH];
  char *exe;

  *exe_p = NULL;
  vcpkg_major = vcpkg_minor = vcpkg_micro = -1;
  exe = searchpath ("vcpkg.exe", "PATH");
  if (exe == NULL)
     return (0);

  slashify2 (exe_copy, exe, '\\');

  if (popen_runf(find_vcpkg_version_cb, "\"%s\" version", exe_copy) > 0)
  {
    ver->val_1 = vcpkg_major;
    ver->val_2 = vcpkg_minor;
    ver->val_3 = vcpkg_micro;
    *exe_p = STRDUP (exe);
    return (1);
  }
  return (0);
}
/*
 * Search and check along %PKG_CONFIG_PATH% for a
 * matching '<filespec>.pc' file.
 */
static int do_check_pkg (void)
{
  smartlist_t *list;
  int          i, max, num, prev_num = 0, found = 0;
  BOOL         do_warn = FALSE;
  char        *orig_e;
  char         report [300];
  static const char env_name[] = "PKG_CONFIG_PATH";

  orig_e = getenv_expand (env_name);
  list = orig_e ? split_env_var (env_name, orig_e) : NULL;
  if (!list)
  {
    WARN ("Env-var %s not defined.\n", env_name);
    return (0);
  }

  snprintf (report, sizeof(report), "Matches in %%%s:\n", env_name);
  report_header = report;

  max = smartlist_len (list);
  for (i = 0; i < max; i++)
  {
    const struct directory_array *arr = smartlist_get (list, i);

    DEBUGF (2, "Checking in dir '%s'\n", arr->dir);
    num = process_dir (arr->dir, 0, arr->exist, TRUE, arr->is_dir, arr->exp_ok,
                       env_name, HKEY_PKGCONFIG_FILE, FALSE);

    if (arr->num_dup == 0 && prev_num > 0 && num > 0)
       do_warn = TRUE;
    if (prev_num == 0 && num > 0)
       prev_num = num;
    found += num;
  }

  free_dir_array();
  FREE (orig_e);

  if (do_warn && !opt.quiet)
  {
    WARN ("Note: ");
    C_printf ("~6There seems to be several '%s' files in different %%%s directories.\n"
              "      \"pkgconfig\" will only select the first.~0\n", opt.file_spec, env_name);
  }
  return (found);
}

/**
 * Search for VCPKG packages matching the 'opt.file_spec`.
 * Also show if a package matching 'opt.file_spec` is installed.
 */
static int do_check_vcpkg (void)
{
  unsigned num;

  vcpkg_get_list();
  num = vcpkg_get_num_CONTROLS();
  if (num >= 1)
     return (int) vcpkg_dump_control (opt.file_spec);

  if (!opt.quiet)
     C_printf ("%s.\n", vcpkg_last_error());
  return (0);
}

/*
 * Before checking the CMAKE_MODULE_PATH, we need to find the version and location
 * of cmake.exe (on PATH). Then assume it's built-in Module-path is relative to this.
 * E.g:
 *   cmake.exe     -> f:\MinGW32\bin\CMake\bin\cmake.exe.
 *   built-in path -> f:\MinGW32\bin\CMake\share\cmake-X.Y\Modules
 */
static int cmake_major, cmake_minor, cmake_micro;

static int find_cmake_version_cb (char *buf, int index)
{
  static char prefix[] = "cmake version ";

  if (!strncmp(buf,prefix,sizeof(prefix)-1) && strlen(buf) >= sizeof(prefix))
  {
    char *p = buf + sizeof(prefix) - 1;

    sscanf (p, "%d.%d.%d", &cmake_major, &cmake_minor, &cmake_micro);
    return (1);
  }
  ARGSUSED (index);
  return (0);
}

static int get_cmake_info (char **exe_p, struct ver_info *ver)
{
  char  exe_copy [_MAX_PATH];
  char *exe;

  *exe_p = NULL;
  cmake_major = cmake_minor = cmake_micro = -1;

  exe = searchpath ("cmake.exe", "PATH");
  if (exe == NULL)
     return (0);

  slashify2 (exe_copy, exe, '\\');

  if (popen_runf(find_cmake_version_cb, "\"%s\" -version", exe_copy) > 0)
  {
    ver->val_1 = cmake_major;
    ver->val_2 = cmake_minor;
    ver->val_3 = cmake_micro;
    ver->val_4 = 0;
    *exe_p = STRDUP (exe);
    return (1);
  }
  return (0);
}

static int do_check_cmake (void)
{
  const char *cmake_bin = searchpath ("cmake.exe", "PATH");
  const char *env_name = "CMAKE_MODULE_PATH";
  int         found = 0;
  BOOL        check_env = TRUE;
  char        report [_MAX_PATH+50];

  cmake_major = cmake_minor = cmake_micro = -1;

  if (!getenv(env_name))
  {
    WARN ("Env-var %s not defined.\n", env_name);
    check_env = FALSE;
  }

  if (cmake_bin)
  {
    char *cmake_root = dirname (cmake_bin);
    char  cmake_copy [_MAX_PATH];

    DEBUGF (3, "cmake -> '%s', cmake_root: '%s'\n", cmake_bin, cmake_root);
    slashify2 (cmake_copy, cmake_bin, '\\');

    if (popen_runf(find_cmake_version_cb, "\"%s\" -version", cmake_copy) > 0)
    {
      char dir [_MAX_PATH];

      snprintf (dir, sizeof(dir), "%s\\..\\share\\cmake-%d.%d\\Modules",
                cmake_root, cmake_major, cmake_minor);
      _fix_path (dir, dir);

      DEBUGF (1, "found Cmake version %d.%d.%d. Module-dir -> '%s'\n",
              cmake_major, cmake_minor, cmake_micro, dir);

      report_header = "Matches among built-in Cmake modules:\n";
      found = process_dir (dir, 0, TRUE, TRUE, 1, TRUE, env_name, NULL, FALSE);
    }
    else
      WARN ("Calling '%s' failed.\n", cmake_bin);

    FREE (cmake_root);
  }
  else
  {
    WARN ("cmake.exe not found on PATH.\n");
    if (check_env)
       WARN (" Checking %%%s anyway.\n", env_name);
  }

  if (check_env)
  {
    snprintf (report, sizeof(report), "Matches in %%%s:\n", env_name);
    report_header = report;
    found += do_check_env ("CMAKE_MODULE_PATH", TRUE);
  }
  report_header = NULL;
  return (found);
}

/**
 * Handling of GNU-compilers (`*gcc.exe`, `*g++.exe`), MSVC,
 * clang-cl, Borland and Watcom.
 *
 * \enum compiler_type
 */
typedef enum compiler_type {
             CC_UNKNOWN = 0, /**< Unknown compiler (not initialised value) */
             CC_GNU_GCC,     /**< Some sort of (prefixed) `*gcc.exe`. */
             CC_GNU_GPP,     /**< Some sort of (prefixed) `*g++.exe`. */
             CC_MSVC,        /**< A MSVC compiler */
             CC_CLANG_CL,    /**< A clang/clang-cl compiler */
             CC_BORLAND,     /**< A Borland / Embarcadero compiler */
             CC_WATCOM       /**< A Watcom/OpenWatcom compiler */
           } compiler_type;

/** \struct compiler_info
 */
typedef struct compiler_info {
        char          *short_name;  /**< the short name we're looking for on `%PATH` */
        char          *full_name;   /**< the full name if found `%PATH` */
        compiler_type  type;        /**< what type is it? */
        BOOL           ignore;      /**< shall we ignore it? */
        BOOL           no_prefix;   /**< shall we check gnu prefixed gcc/g++? */
      } compiler_info;

/**
 * The information for all added compilers is in this list;
 * an array of `compiler_info` created by `search_and_add_all_cc()`.
 */
static smartlist_t *all_cc = NULL;

static size_t longest_cc        = 0;
static BOOL   ignore_all_gcc    = FALSE;
static BOOL   ignore_all_gpp    = FALSE;
static BOOL   looks_like_cygwin = FALSE;
static BOOL   found_search_line = FALSE;
static char  *cygwin_root       = NULL;
static char   cygwin_fqfn [_MAX_PATH];
static char  *watcom_dir[4];

/**
 * Free the memory allocated by `search_and_add_all_cc()`.
 */
static void free_all_compilers (void)
{
  int i, max = all_cc ? smartlist_len (all_cc) : 0;

  for (i = 0; i < max; i++)
  {
    compiler_info *cc = smartlist_get (all_cc, i);

    FREE (cc->short_name);
    FREE (cc->full_name);
    FREE (cc);
  }
  smartlist_free (all_cc);
}

/**
 * Check if we shall ignore this compiler.
 *
 * \li if `cc->full_name` is non-NULL (i.e. found), check the ignore-list for that.
 * \li if `cc->full_name` is NULL, check the ignore-list for the `cc->short_name`.
 *
 * \eg{.} if the config-file contains a `"ignore = i386-mingw32-gcc.exe"`, and
 *        `"i386-mingw32-gcc.exe"` is not found, don't try to spawn it (since it
 *        will fail).
 *
 * \param[in] cc the the `compiler_info` to check.
 */
static void compiler_check_ignore (compiler_info *cc)
{
  BOOL ignore = FALSE;

  /* "envtool --no-prefix .." given and this `cc->short_name` is
   * a prefixed `*-gcc.exe` or `*-g++.exe`.
   */
  if (cc->no_prefix)
     ignore = TRUE;

  /* "envtool --no-gcc .." given and this `cc->type == CC_GNU_GCC`.
   */
  else if (cc->type == CC_GNU_GCC && opt.no_gcc)
     ignore = TRUE;

  /* "envtool --no-g++ .." given and this `cc->type == CC_GNU_GPP`.
   */
  else if (cc->type == CC_GNU_GPP && opt.no_gpp)
     ignore = TRUE;

  /* "envtool --no-watcom .." given and this `cc->type == CC_WATCOM`.
   */
  else if (cc->type == CC_WATCOM && opt.no_watcom)
     ignore = TRUE;

  /* "envtool --no-borland .." given and this `cc->type == CC_BORLAND`.
   */
  else if (cc->type == CC_BORLAND && opt.no_borland)
     ignore = TRUE;

  /* "envtool --no-clang .." given and this `cc->type == CC_CLANG_CL`.
   */
  else if (cc->type == CC_CLANG_CL && opt.no_clang)
     ignore = TRUE;

  else if (cc->full_name)
    ignore = cfg_ignore_lookup ("[Compiler]", cc->full_name);

  /* Last chance to check ignore.
   */
  if (!ignore)
     ignore = cfg_ignore_lookup ("[Compiler]", cc->short_name);

  DEBUGF (1, "Checking %s (%s), ignore: %d.\n",
          cc->short_name, cc->full_name ? cc->full_name : "<not found>", ignore);

  cc->ignore = ignore;
}

/**
 * Having several gcc compilers installed makes it nearly impossible to
 * set `C_INCLUDE_PATH` to the desired compiler's include-dir. So Envtool
 * simply asks `*gcc.exe` for what it think is the include search-path.
 * Do that by spawning the `*gcc.exe` and parsing the include paths.
 *
 * Same goes for the `LIBRARY_PATH`.
 */
static void check_if_cygwin (const char *path)
{
  static const char cyg_usr[] = "/usr/";
  static const char cyg_drv[] = "/cygdrive/";

  if (looks_like_cygwin)
     return;

  if (!memcmp(path,&cyg_usr,sizeof(cyg_usr)-1) || !memcmp(path,&cyg_drv,sizeof(cyg_drv)-1))
  {
    looks_like_cygwin = TRUE;
//  opt.conv_cygdrive = 1;
    DEBUGF (2, "looks_like_cygwin = %d, cygwin_root: '%s'\n", looks_like_cygwin, cygwin_root);
  }
}

/**
 * In case the `gcc` is a CygWin gcc, we need to figure out the root-directory.
 * Since `gcc` reports `C_INCLUDE_PATH` like `"/usr/lib/gcc/i686-w64-mingw32/6.4.0/include"`,
 * we must prefix this as `"<cygwin_root>/usr/lib/gcc/i686-w64-mingw32/6.4.0/include"`.
 *
 * Otherwise `FILE_EXISTS()` wont work for non-Cygwin targets.
 * An alternative would be to parse the `"<cygwin_root>/etc/fstab"` file!
 */
static void setup_cygwin_root (const compiler_info *cc)
{
  looks_like_cygwin = FALSE;
  cygwin_root = NULL;
  cygwin_fqfn[0] = '\0';

  if (cc->full_name && !cc->ignore)
  {
    char *bin_dir;

    slashify2 (cygwin_fqfn, cc->full_name, '/');
    bin_dir = strstr (cygwin_fqfn, "/bin");
    if (bin_dir)
    {
      cygwin_root = STRDUP (cygwin_fqfn);
      *strstr (cygwin_root, "/bin") = '\0';
    }
  }
}

static int find_include_path_cb (char *buf, int index)
{
  static const char start[] = "#include <...> search starts here:";
  static const char end[]   = "End of search list.";
  const  char *p;

  if (!found_search_line && !memcmp(buf,&start,sizeof(start)-1))
  {
    found_search_line = TRUE;
    return (0);
  }

  if (found_search_line)
  {
    p = str_ltrim (buf);
    check_if_cygwin (p);

    if (!memcmp(buf,&end,sizeof(end)-1)) /* got: "End of search list.". No more paths excepted. */
    {
      found_search_line = FALSE;
      return (-1);
    }

#if defined(__CYGWIN__)
    if (looks_like_cygwin)
    {
      char result [_MAX_PATH];
      int  rc = cygwin_conv_path (CCP_POSIX_TO_WIN_A, p, result, sizeof(result));

      if (rc == 0)
      {
        DEBUGF (2, "CygWin path detected. Converting '%s' -> '%s'\n", p, result);
        p = _fix_drive (result);
      }
      /* otherwise add 'p' as-is */
    }
    else
#endif
    {
      char buf2 [_MAX_PATH];

#if !defined(__CYGWIN__)
      if (looks_like_cygwin && cygwin_root)
      {
        snprintf (buf2, sizeof(buf2), "%s%s", cygwin_root, str_trim(buf));
        p = buf2;
      }
      else
#endif
        p = _fix_path (str_trim(buf), buf2);
    }

    add_to_dir_array (p, !stricmp(current_dir,p), __LINE__);
    DEBUGF (3, "line: '%s'\n", p);
    return (1);
  }

  ARGSUSED (index);
  return (0);
}

static int find_library_path_cb (char *buf, int index)
{
  static const char prefix[] = "LIBRARY_PATH=";
  char   buf2 [_MAX_PATH];
  char   sep[2], *p, *tok, *rc, *end;
  int    i = 0;

  if (strncmp(buf,prefix,sizeof(prefix)-1) || strlen(buf) <= sizeof(prefix))
     return (0);

  p = buf + sizeof(prefix) - 1;

  check_if_cygwin (p);

  sep[0] = looks_like_cygwin ? ':' : ';';
  sep[1] = '\0';

  for (i = 0, tok = strtok(p,sep); tok; tok = strtok(NULL,sep), i++)
  {
#if defined(__CYGWIN__)
    if (looks_like_cygwin)
    {
      char result [_MAX_PATH];
      int  rc1 = cygwin_conv_path (CCP_POSIX_TO_WIN_A, tok, result, sizeof(result));

      if (rc1 == 0)
           rc = _fix_drive (result);
      else rc = tok;  /* otherwise add 'tok' as-is */
    }
    else
#endif
    {
#if !defined(__CYGWIN__)
      if (looks_like_cygwin && cygwin_root)
      {
        snprintf (buf2, sizeof(buf2), "%s%s", cygwin_root, tok);
        rc = _fix_path (buf2, buf2);
        end = rc ? strrchr(rc,'\\') : NULL;
        if (end)
           *end = '\0';
      }
      else
#endif
      {
        rc = _fix_path (tok, buf2);
        end = rc ? strrchr(rc,'\\') : NULL;
        if (end)
           *end = '\0';
      }
    }
    add_to_dir_array (rc, FALSE, __LINE__);
    DEBUGF (3, "tok %d: '%s'\n", i, rc);
  }
  ARGSUSED (index);
  return (i);
}

/*
 * Print a warning on last error from a gnu 'popen_runf()' callback.
 */
static void gnu_popen_warn (const char *gcc, int rc)
{
  const char *err = popen_last_line();

  if (*err != '\0')
     err = strstr (err, "error: ");

  WARN ("Calling %s returned %d", cygwin_fqfn[0] ? cygwin_fqfn : gcc, rc);
  if (err && !opt.quiet)
     C_printf (":\n  %s.\n", err);
}

/**
 * The include-directory for C++ headers is not reported in the
 * 'find_include_path_cb()' callback.
 * Insert a 'x/c++' to the list where a 'c++' subdirectory is found.
 */
static void gnu_add_gpp_path (void)
{
  struct directory_array *d;
  int    i, j, max = smartlist_len (dir_array);
  char   fqdn [_MAX_PATH];

  for (i = 0; i < max; i++)
  {
    d = smartlist_get (dir_array, i);
    snprintf (fqdn, sizeof(fqdn), "%s%c%s", d->dir, DIR_SEP, "c++");
    if (is_directory(fqdn))
    {
      /* This will be added at 'dir_array[max+1]'.
       */
      add_to_dir_array (fqdn, 0, __LINE__);

#if 0
      /* Insert the new 'c++' directory at the 'i'-th element.
       */
      j = smartlist_len (dir_array) - 1;
      d = smartlist_get (dir_array, j);
      smartlist_insert (dir_array, i, d);
#else
      ARGSUSED (j);
#endif
      break;
    }
  }
}

#if defined(__CYGWIN__)
  #define CLANG_DUMP_FMT "%s -v -dM -xc -c - < /dev/null 2>&1"
  #define GCC_DUMP_FMT   "%s %s -v -dM -xc -c - < /dev/null 2>&1"
#else
  #define CLANG_DUMP_FMT "%s -o NUL -v -dM -xc -c - < NUL 2>&1"
  #define GCC_DUMP_FMT   "%s %s -o NUL -v -dM -xc -c - < NUL 2>&1"
#endif             /* gcc ^, ^ '', '-m32' or '-m64' */

static int setup_gcc_includes (const compiler_info *cc)
{
  const char *gcc = cc->full_name;
  int   found;

  free_dir_array();

  /* We want the output of stderr only. But that seems impossible on CMD/4NT.
   * Hence redirect stderr + stdout into the same pipe for us to read.
   * Also assume that the '*gcc' is on PATH.
   */
  found_search_line = FALSE;

  setup_cygwin_root (cc);

  found = popen_runf (find_include_path_cb, GCC_DUMP_FMT, gcc, "");
  if (found > 0)
  {
    DEBUGF (1, "found %d include paths for %s.\n", found, gcc);
    if (cc->type == CC_GNU_GPP)
       gnu_add_gpp_path();
  }
  else
    gnu_popen_warn (gcc, found);
  return (found);
}

static int setup_gcc_library_path (const compiler_info *cc, BOOL warn)
{
  const char *m_cpu, *gcc = cc->full_name;
  int   found, duplicates;

  free_dir_array();

  /* Tell '*gcc.exe' to return 32 or 64-bot or both types of libs.
   * (assuming it supports the '-m32'/'-m64' switches.
   */
  if (opt.only_32bit)
       m_cpu = "-m32";
  else if (opt.only_64bit)
       m_cpu = "-m64";
  else m_cpu = "";

  /* We want the output of stderr only. But that seems impossible on CMD/4NT.
   * Hence redirect stderr + stdout into the same pipe for us to read.
   * Also assume that the '*gcc' is on PATH.
   */
  found_search_line = FALSE;

  setup_cygwin_root (cc);

  found = popen_runf (find_library_path_cb, GCC_DUMP_FMT, gcc, m_cpu);
  if (found <= 0)
  {
    if (warn)
       gnu_popen_warn (gcc, found);
    return (found);
  }

  DEBUGF (1, "found %d library paths for %s.\n", found, gcc);

#if defined(__CYGWIN__)
  /*
   * The Windows-API lib-dir isn't among the defaults. Just add it
   * at the end of list anyway. In case it was already reported, we'll
   * remove it below.
   */
  if (looks_like_cygwin)
  {
    char result [_MAX_PATH];
    int  rc = cygwin_conv_path (CCP_POSIX_TO_WIN_A, "/usr/lib/w32api", result, sizeof(result));

    if (rc == 0)
       add_to_dir_array (result, FALSE, __LINE__);
  }
#endif

  duplicates = make_unique_dir_array ("library paths");
  if (duplicates > 0)
     DEBUGF (1, "found %d duplicates in library paths for %s.\n", duplicates, gcc);
  return (found);
}

/**
 * Check library-paths found in setup_gcc_library_path(). <br>
 * Check include-paths found in setup_gcc_includes().
 */
static int process_gcc_dirs (const char *gcc, int *num_dirs)
{
  int i, found, max = smartlist_len (dir_array);

  for (i = found = 0; i < max; i++)
  {
    const struct directory_array *arr = smartlist_get (dir_array, i);

    DEBUGF (2, "dir: %s\n", arr->dir);
    found += process_dir (arr->dir, arr->num_dup, arr->exist, arr->check_empty,
                          arr->is_dir, arr->exp_ok, gcc, HKEY_INC_LIB_FILE, FALSE);
  }
  *num_dirs = max;
  free_dir_array();
  return (found);
}

/**
 * Add all supported GNU gcc/g++ compilers to the `all_cc` smartlist.
 * But only add the first `"*gcc.exe"` / `"*g++.exe"` found on `PATH`.
 *
 * The first pair added has no prefix (simply `"gcc.exe"` / `"g++.exe"`).
 * The others pairs use the prefixes in `gnu_prefixes[]`.
 */
static void add_gnu_compilers (void)
{
  char   short_name[30];
  size_t i;

  for (i = 0; i < DIM(gnu_prefixes); i++)
  {
    compiler_info *gcc = CALLOC (sizeof(*gcc), 1);
    compiler_info *gpp = CALLOC (sizeof(*gpp), 1);

    snprintf (short_name, sizeof(short_name)-1, "%sgcc.exe", gnu_prefixes[i]);
    gcc->short_name = STRDUP (short_name);
    gcc->no_prefix  = (i > 0 && opt.gcc_no_prefixed);

    snprintf (short_name, sizeof(short_name)-1, "%sg++.exe", gnu_prefixes[i]);
    gpp->short_name = STRDUP (short_name);
    gpp->no_prefix  = (i > 0 && opt.gcc_no_prefixed);

    gcc->type = CC_GNU_GCC;
    gpp->type = CC_GNU_GPP;

    gcc->full_name = searchpath (gcc->short_name, "PATH");
    if (gcc->full_name)
       gcc->full_name = STRDUP (gcc->full_name);

    gpp->full_name = searchpath (gpp->short_name, "PATH");
    if (gpp->full_name)
       gpp->full_name = STRDUP (gpp->full_name);

    smartlist_add (all_cc, gcc);
    smartlist_add (all_cc, gpp);
  }
}

/**
 * Simple; only add the first "cl.exe" found on PATH.
 * \todo
 *   do as with "envtool --path cl.exe" does and add all "cl.exe" found
 *   on PATH to the list.
 */
static void add_msvc_compilers (void)
{
  compiler_info *cl = CALLOC (sizeof(*cl), 1);
  const char    *full_name;

  cl->type       = CC_MSVC;
  cl->short_name = STRDUP ("cl.exe");
  full_name      = searchpath (cl->short_name, "PATH");
  if (full_name)
      cl->full_name = STRDUP (full_name);
  smartlist_add (all_cc, cl);
}

/**
 * Search and add supported clang compilers to the `all_cc` smartlist.
 */
static void add_clang_cl_compilers (void)
{
  static const char *clang[] = {
                    "clang.exe",
                    "clang-cl.exe"
                  };
  int i;

  for (i = 0; i < DIM(clang); i++)
  {
    compiler_info *cl = CALLOC (sizeof(*cl), 1);
    const char    *full_name;

    cl->type       = CC_CLANG_CL;
    cl->short_name = STRDUP (clang[i]);
    full_name      = searchpath (cl->short_name, "PATH");
    if (full_name)
        cl->full_name = STRDUP (full_name);
    smartlist_add (all_cc, cl);
  }
}

/**
 * Search and add supported Borland compilers to the `all_cc` smartlist.
 */
static void add_borland_compilers (void)
{
  static const char *borland[] = {
                    "bcc32.exe",
                    "bcc32c.exe"
                  };
  int i;

  for (i = 0; i < DIM(borland); i++)
  {
    compiler_info *bcc = CALLOC (sizeof(*bcc), 1);
    const char    *full_name;

    bcc->type       = CC_BORLAND;
    bcc->short_name = STRDUP (borland[i]);
    full_name       = searchpath (bcc->short_name, "PATH");
    if (full_name)
       bcc->full_name = STRDUP (full_name);
    smartlist_add (all_cc, bcc);
  }
}

/**
 * Search and add supported Watcom compilers to the `all_cc` smartlist.
 */
static void add_watcom_compilers (void)
{
  static const char *wcc[] = {
                    "wcc386.exe",
                    "wpp386.exe",
#if 0
                    /* x86 16-bit C/C++ compilers
                     */
                    "wcc.exe",
                    "wpp.exe",

                    /* MIPS / PowerPC, C compilers
                     */
                    "wccmps.exe",
                    "wccppc.exe",
#endif
                    /* Museum stuff; Alpha AXP, C/C++ compilers
                     */
                    "wccaxp.exe",
                    "wppaxp.exe"
                  };
  int i;

  for (i = 0; i < DIM(wcc); i++)
  {
    compiler_info *wc = CALLOC (sizeof(*wc), 1);
    const char    *full_name;

    wc->type       = CC_WATCOM;
    wc->short_name = STRDUP (wcc[i]);
    full_name      = searchpath (wc->short_name, "PATH");
    if (full_name)
       wc->full_name = STRDUP (full_name);
    smartlist_add (all_cc, wc);
  }
}

/**
 * This is used to find the longest `cc->short_name`. For aligning the 1st column
 * (e.g. `"cl.exe"`) to fit the compiler with the longest `cc->short_name`.
 * I.e. `"x86_64-w64-mingw32-gcc.exe"`.
 */
static size_t get_longest_short_name (void)
{
  int    i, max = smartlist_len (all_cc);
  size_t longest = 0;

  for (i = 0; i < max; i++)
  {
    const  compiler_info *cc = smartlist_get (all_cc, i);
    size_t len = strlen (cc->short_name);

    if (!cc->ignore && len > longest)
       longest = len;
  }
  return (longest);
}

/**
 * Print the internal `"*gcc"` or `"*g++"` `LIBRARY_PATH` returned from
 * `setup_gcc_library_path()`.
 * I.e. only the directories \b not in `%LIBRARY_PATH%`.
 *
 * If we have no `%LIBRARY_PATH%`, the `copy[]` will contain only internal
 * directories.
 */
static void print_gcc_internal_dirs (const char *env_name, const char *env_value)
{
  struct directory_array *arr;
  smartlist_t            *list;
  char                  **copy;
  char                    slash = (opt.show_unix_paths ? '/' : '\\');
  int                     i, j, max;
  static BOOL done_note = FALSE;

  max = smartlist_len (dir_array);
  if (max == 0)
     return;

  copy = alloca ((max+1) * sizeof(char*));
  for (i = 0; i < max; i++)
  {
    arr = smartlist_get (dir_array, i);
    copy[i] = STRDUP (arr->dir);
    slashify2 (copy[i], copy[i], slash);
  }
  copy[i] = NULL;
  DEBUGF (3, "Made a 'copy[]' of %d directories.\n", max);

  free_dir_array();

  list = split_env_var (env_name, env_value);
  max  = list ? smartlist_len (list) : 0;
  DEBUGF (3, "smartlist for '%s' have %d entries.\n", env_name, max);

  for (i = 0; copy[i]; i++)
  {
    BOOL  found = FALSE;
    const char *dir;

    for (j = 0; j < max; j++)
    {
      arr = smartlist_get (list, j);
      dir = slashify2 (arr->dir, arr->dir, slash);
      if (!stricmp(dir,copy[i]))
      {
        found = TRUE;
        break;
      }
    }
    if (!found)
    {
      C_printf ("%*s%s %s\n", (int)(longest_cc+8), "", copy[i], done_note ? "" : "~3(1)~0");
      done_note = TRUE;
    }
  }

  for (i = 0; copy[i]; i++)
      FREE (copy[i]);
  free_dir_array();
}

/**
 * Called during `envtool -VV` to print:
 * ```
 *  Compilers on PATH:
 *    gcc.exe                    -> f:\MingW32\TDM-gcc\bin\gcc.exe
 *    ...
 * ```
 *
 * `envtool -VVV (print_lib_path = TRUE)` will print the internal
 * `*gcc` or `*g++` library paths too.
 */
static void print_compiler_info (const compiler_info *cc, BOOL print_lib_path)
{
  BOOL   is_gnu;
  size_t len = strlen (cc->short_name);

  C_printf ("    %s%*s -> ", cc->short_name, (int)(longest_cc-len), "");
  if (cc->full_name)
       C_printf ("~6%s~0\n", cc->full_name);
  else C_printf ("~5Not found~0\n");

  if (!cc->full_name || cc->ignore || !print_lib_path)
     return;

  is_gnu = (cc->type == CC_GNU_GCC || cc->type == CC_GNU_GPP);
  if (is_gnu && setup_gcc_library_path(cc,FALSE) > 0)
  {
    char *env = getenv_expand ("LIBRARY_PATH");

    print_gcc_internal_dirs ("LIBRARY_PATH", env);
    FREE (env);
  }
  FREE (cygwin_root);
}

/**
 * Return TRUE if we shall ignore all gnu-type compilers.
 */
static BOOL ignore_all_gnus (compiler_type type)
{
  int i, num_gnu = 0, gnu_ignore = 0;
  int max = smartlist_len (all_cc);

  for (i = 0; i < max; i++)
  {
    const compiler_info *cc = smartlist_get (all_cc, i);

    if (cc->type != type)
       continue;

     num_gnu++;
     if (cc->ignore)
        gnu_ignore++;
  }
  return (gnu_ignore >= num_gnu);
}

/**
 * In `--lib` or `--inc` mode, search the `PATH` for all supported compilers.
 *
 * \param[in] print_info      If called from `show_version()`, print additional
 *                            information on each compiler (unless it is in the ignore-list).
 * \param[in] print_lib_path  If called from `show_version()` and `envtool -VVV` was used,
 *                            print the internal GCC library paths too.
 */
static void search_and_add_all_cc (BOOL print_info, BOOL print_lib_path)
{
  struct compiler_info *cc;
  BOOL   at_least_one_gnu = FALSE;
  int    i, max, ignored, save = opt.cache_ver_level;
  int    save_u;

  ASSERT (all_cc == NULL);
  all_cc = smartlist_new();

  if (print_info && print_lib_path)
     opt.cache_ver_level = 3;

  save_u = opt.show_unix_paths;
  if (!print_info)
     opt.show_unix_paths = 0;

  add_gnu_compilers();
  add_msvc_compilers();
  add_clang_cl_compilers();
  add_borland_compilers();
  add_watcom_compilers();

  opt.show_unix_paths = save_u;

  max = smartlist_len (all_cc);
  for (i = 0; i < max; i++)
      compiler_check_ignore (smartlist_get(all_cc, i));

  longest_cc = get_longest_short_name();

  ignore_all_gcc = ignore_all_gnus (CC_GNU_GCC);
  ignore_all_gpp = ignore_all_gnus (CC_GNU_GPP);

  DEBUGF (1, "ignore_all_gcc: %d, ignore_all_gpp: %d.\n", ignore_all_gcc, ignore_all_gpp);

  if (!print_info)
     return;

  /* Count the # of compilers that were ignored.
   * And print some info if it wasn't ignored.
   */
  for (i = ignored = 0; i < max; i++)
  {
    cc = smartlist_get (all_cc, i);
    if (cc->ignore)
         ignored++;
    else print_compiler_info (cc, print_lib_path);

    if (!at_least_one_gnu)
       at_least_one_gnu = (cc->type == CC_GNU_GCC || cc->type == CC_GNU_GPP);
  }

  if (print_lib_path && at_least_one_gnu)
     C_puts ("    ~3(1)~0: internal GCC library paths.\n");

  opt.cache_ver_level = save;

  if (ignored == 0)
     return;

  /* Show the ignored ones
   */
  C_puts ("\n    Ignored:\n");
  for (i = 0; i < max; i++)
  {
    cc = smartlist_get (all_cc, i);
    if (cc->ignore)
       C_printf ("      %s%s\n",
                 cc->full_name ? cc->full_name : cc->short_name,
                 cc->full_name == NULL ? "  ~5Not found~0" : "");
  }
}

/**
 * Common to both gcc/g++ checking of include-dirs.
 */
static int check_gnu_includes (compiler_type type, int *num_dirs)
{
  char  report [_MAX_PATH+50];
  int   i, max, found;
  const compiler_info *cc;
  const char          *env;

  *num_dirs = 0;
  max = smartlist_len (all_cc);

  for (i = found = 0; i < max; i++)
  {
    cc = smartlist_get (all_cc, i);
    if (cc->type == type && !cc->ignore && setup_gcc_includes(cc) > 0)
    {
      env = (cc->type == CC_GNU_GCC) ? "%C_INCLUDE_PATH%" : "%CPLUS_INCLUDE_PATH%";
      snprintf (report, sizeof(report), "Matches in %s %s path:\n", cc->full_name, env);
      report_header = report;
      found += process_gcc_dirs (cc->short_name, num_dirs);
    }
    FREE (cygwin_root);
  }
  return (found);
}

static int do_check_gcc_includes (void)
{
  int num_dirs;
  int found = check_gnu_includes (CC_GNU_GCC, &num_dirs);

  if (num_dirs == 0 && !ignore_all_gcc)  /* Impossible unless we ignore all '*gcc' */
     WARN ("No gcc.exe programs returned any include paths.\n");
  return (found);
}

static int do_check_gpp_includes (void)
{
  int num_dirs;
  int found = check_gnu_includes (CC_GNU_GPP, &num_dirs);

  if (num_dirs == 0 && !ignore_all_gpp)  /* Impossible unless we ignore all '*g++.exe' */
     WARN ("No g++.exe programs returned any include paths.\n");
  return (found);
}

static int do_check_gcc_library_paths (void)
{
  char  report [_MAX_PATH+50];
  int   found = 0;
  int   num_dirs = 0;
  int   i, max;
  BOOL  is_gnu;
  const compiler_info *cc;
  const char          *gcc;

  max = smartlist_len (all_cc);

  for (i = 0; i < max; i++)
  {
    cc     = smartlist_get (all_cc, i);
    is_gnu = (cc->type == CC_GNU_GCC || cc->type == CC_GNU_GPP);
    if (is_gnu && !cc->ignore && setup_gcc_library_path(cc,TRUE) > 0)
    {
      gcc = cc->short_name;
      snprintf (report, sizeof(report), "Matches in %s %%LIBRARY_PATH%% path:\n", cc->full_name);
      report_header = report;
      found += process_gcc_dirs (gcc, &num_dirs);
    }
    FREE (cygwin_root);
  }

  if (num_dirs == 0 && !ignore_all_gcc)  /* Impossible unless we ignore all '*gcc' */
     WARN ("No gcc.exe programs returned any LIBRARY_PATH paths!?.\n");

  return (found);
}

/*
 * Check along clang directories
 */
static int process_clang_dirs (const char *cc, int *num_dirs)
{
  int i, found, max = smartlist_len (dir_array);

  for (i = found = 0; i < max; i++)
  {
    const struct directory_array *arr = smartlist_get (dir_array, i);

    DEBUGF (2, "dir: %s\n", arr->dir);
    found += process_dir (arr->dir, arr->num_dup, arr->exist, arr->check_empty,
                          arr->is_dir, arr->exp_ok, cc, HKEY_INC_LIB_FILE, FALSE);
  }
  *num_dirs = max;
  free_dir_array();
  return (found);
}

static void clang_popen_warn (const compiler_info *cc, int rc)
{
  const char *err = popen_last_line();

  if (*err != '\0')
     err = strstr (err, "error: ");

  WARN ("Calling %s returned %d", cc->full_name, rc);
  if (err && !opt.quiet)
     C_printf (":\n  %s.\n", err);
}

static int setup_clang_includes (const compiler_info *cc)
{
  int found;

  free_dir_array();

  /* We want the output of stderr only. But that seems impossible on CMD/4NT.
   * Hence redirect stderr + stdout into the same pipe for us to read.
   * Also assume that the '*gcc' is on PATH.
   */
  found_search_line = FALSE;

  found = popen_runf (find_include_path_cb, CLANG_DUMP_FMT, cc->full_name);
  if (found > 0)
       DEBUGF (1, "found %d include paths for %s.\n", found, cc->full_name);
  else clang_popen_warn (cc, found);
  return (found);
}

static int find_clang_library_path_cb (char *buf, int index)
{
  static const char prefix[] = "libraries: =";
  char  *p, *tok;
  int    i = 0;

  if (strncmp(buf,prefix,sizeof(prefix)-1) || strlen(buf) <= sizeof(prefix))
     return (0);

  p = buf + sizeof(prefix) - 1;

  for (i = 0, tok = strtok(p,";"); tok; tok = strtok(NULL,";"), i++)
  {
    char buf1 [_MAX_PATH];
    char buf2 [_MAX_PATH];

    _strlcpy (buf1, tok, sizeof(buf1));
    _strlcpy (buf2, buf1, sizeof(buf2));

#ifdef HAVE_STRCAT_S
    strcat_s (buf1, sizeof(buf1), "\\lib\\windows");
    strcat_s (buf2, sizeof(buf2), "\\..\\..");
#else
    strcat (buf1, "\\lib\\windows");
    strcat (buf2, "\\..\\..");
#endif

    _fix_path (buf1, buf1);
    _fix_path (buf2, buf2);

    DEBUGF (2, "buf1: '%s'\n", buf1);
    add_to_dir_array (buf1, FALSE, __LINE__);

    DEBUGF (2, "buf2: '%s'\n", buf2);
    add_to_dir_array (buf2, FALSE, __LINE__);
  }
  ARGSUSED (index);
  return (2*i);
}

static int setup_clang_library_path (const compiler_info *cc)
{
  int found;

  free_dir_array();

  /* We want the output of stderr only. But that seems impossible on CMD/4NT.
   * Hence redirect stderr + stdout into the same pipe for us to read.
   * Also assume that the '*gcc' is on PATH.
   */
  found_search_line = FALSE;

  found = popen_runf (find_clang_library_path_cb, "%s -print-search-dirs", cc->full_name);
  if (found > 0)
       DEBUGF (1, "found %d library paths for %s.\n", found, cc->full_name);
  else clang_popen_warn (cc, found);
  return (found);
}

/**
 * Checking of clang include-dirs.
 */
static int do_check_clang_includes (void)
{
  char report [_MAX_PATH+50];
  int  i, found, num_dirs = 0;
  int  max = smartlist_len (all_cc);

  for (i = found = 0; i < max; i++)
  {
    const compiler_info *cc = smartlist_get (all_cc, i);

    if (cc->type == CC_CLANG_CL && !cc->ignore &&
        !stricmp("clang.exe",cc->short_name)   &&   /* Do it for clang.exe only */
        setup_clang_includes(cc) > 0)
    {
      snprintf (report, sizeof(report), "Matches in %s %%INCLUDE%% path:\n", cc->full_name);
      report_header = report;
      found += process_clang_dirs (cc->short_name, &num_dirs);
    }
    FREE (cygwin_root);
  }

  if (num_dirs == 0)
     WARN ("No clang.exe programs returned any include paths.\n");
  return (found);
}

/**
 * Checking of special clang library-dirs.
 */
static int do_check_clang_library_paths (void)
{
  char report [_MAX_PATH+50];
  int  i, found, num_dirs = 0;
  int  max = smartlist_len (all_cc);

  for (i = found = 0; i < max; i++)
  {
    const compiler_info *cc = smartlist_get (all_cc, i);

    if (cc->type == CC_CLANG_CL && !cc->ignore &&
        !stricmp("clang.exe",cc->short_name)   &&   /* Do it for clang.exe only */
        setup_clang_library_path(cc) > 0)
    {
      snprintf (report, sizeof(report), "Matches in %s %%LIB%% path:\n", cc->full_name);
      report_header = report;
      found += process_clang_dirs (cc->short_name, &num_dirs);
    }
    FREE (cygwin_root);
  }

  if (num_dirs == 0)
     WARN ("\nNo clang.exe programs returned any library paths.\n");
  return (found);
}

/**
 * Common stuff for Watcom checking.
 */
static int setup_watcom_dirs (const char *dir0, const char *dir1, const char *dir2)
{
  const compiler_info *cc;
  int   i, found, ignored, max;
  BOOL  dir2_found;

  max = smartlist_len (all_cc);
  for (i = found = ignored = 0; i < max; i++)
  {
    cc = smartlist_get (all_cc, i);
    if (!cc->full_name || cc->type != CC_WATCOM)
       continue;

    found++;
    if (cc->ignore)
       ignored++;
  }

  if (found == 0)
  {
    DEBUGF (1, "No Watcom compilers found.\n");
    return (0);
  }

  if (ignored >= found)
  {
    DEBUGF (1, "All Watcom compilers were ignored.\n");
    return (0);
  }

  if (!getenv("WATCOM"))
  {
    DEBUGF (1, "%%WATCOM%% not defined.\n");
    return (0);
  }

  if (!opt.no_cwd)
     add_to_dir_array (current_dir, 1, __LINE__);

  watcom_dir[0] = getenv_expand (dir0);
  watcom_dir[1] = getenv_expand (dir1);
  watcom_dir[2] = getenv_expand (dir2);

  /* This directory exist only on newer Watcom distos.
   * Like "%WATCOM%\\lh" for Linux headers.
   */
  dir2_found = is_directory (watcom_dir[2]);

  add_to_dir_array (watcom_dir[0], 0, __LINE__);
  add_to_dir_array (watcom_dir[1], 0, __LINE__);
  if (dir2_found)
     add_to_dir_array (watcom_dir[2], 0, __LINE__);

  return (1);
}

static void free_watcom_dirs (void)
{
  FREE (watcom_dir[0]);
  FREE (watcom_dir[1]);
  FREE (watcom_dir[2]);
  FREE (watcom_dir[3]);
}

/**
 * Check in Watcom's include-directories:
 * \code
 *   %WATCOM%\h
 *   %WATCOM%\nt
 *   %WATCOM%\lh    (Linux headers in recent OpenWatcom)
 * \endcode
 *
 * And full path given by `%NT_INCLUDE%`.
 *
 * \note We do not spawn `"wcc*.exe"` to ask for it's internal include-directory
 *   (as we do for `"gcc*"`). Simply search along the above directories.
 *   Does not searches `"%INCLUDE%"`.
 */
static int do_check_watcom_includes (void)
{
  int i, max, save, found = 0;

  watcom_dir[3] = getenv_expand ("%NT_INCLUDE%");

  if (!watcom_dir[3])
       DEBUGF (1, "Env-var %s not defined.\n", "%NT_INCLUDE%");
  else split_env_var ("%NT_INCLUDE%", watcom_dir[3]);

  /* This will append to what was inserted in `dir_array` above.
   * Do not add `".\\"` again (set `opt.no_cwd` to 1).
   */
  save = opt.no_cwd;
  opt.no_cwd = 1;
  if (!setup_watcom_dirs("%WATCOM%\\h", "%WATCOM%\\h\\nt", "%WATCOM%\\lh"))
     goto quit;

  /* The above adding of `"%NT_INCLUDE"` will probably create duplicate
   * entries. Remove them.
   */
  make_unique_dir_array ("%NT_INCLUDE%");

  report_header = "Matches in %NT_INCLUDE:\n";

  max = smartlist_len (dir_array);
  for (i = 0; i < max; i++)
  {
    struct directory_array *arr = smartlist_get (dir_array, i);

    found += process_dir (arr->dir, arr->num_dup, arr->exist, TRUE,
                          arr->is_dir, arr->exp_ok, "WATCOM", HKEY_INC_LIB_FILE, FALSE);
  }

quit:
  opt.no_cwd = save;
  free_watcom_dirs();
  free_dir_array();
  return (found);
}

/**
 * Check in Watcom's library-directories:
 * \code
 *   %WATCOM%\lib386
 *   %WATCOM%\lib386\nt
 *   %WATCOM%\lib386\linux    (Linux libs in recent OpenWatcom)
 * \endcode
 */
static int do_check_watcom_library_paths (void)
{
  int i, max, found;

  if (!setup_watcom_dirs("%WATCOM%\\lib386", "%WATCOM%\\lib386\\nt", "%WATCOM%\\lib386\\linux"))
     return (0);

  report_header = "Matches in %WATCOM libraries:\n";

  max = smartlist_len (dir_array);
  for (i = found = 0; i < max; i++)
  {
    struct directory_array *arr = smartlist_get (dir_array, i);

    found += process_dir (arr->dir, arr->num_dup, arr->exist, TRUE,
                          arr->is_dir, arr->exp_ok, "WATCOM", HKEY_INC_LIB_FILE, FALSE);
  }
  dump_dir_array ("Watcom libs", "");
  free_watcom_dirs();
  free_dir_array();
  return (found);
}

/**
 * Common stuff for Borland checking.
 */
static char *bcc_root = NULL;

typedef void (*bcc_parser_func) (smartlist_t *sl, char *line);

/**
 * Check in Borland's include-directories which are given by the format in
 * `<bcc_root>\bcc32c.cfg`:
 * ```
 *  -isystem @\..\include\dinkumware64
 *  -isystem @\..\include\windows\crtl
 * ```
 *
 * Or the older format in `<bcc_root>\bcc32.cfg`:
 * ```
 *  -I<inc_path1>;<inc_path2>...
 * ```
 */
static void bcc32_cfg_parse_inc (smartlist_t *sl, char *line)
{
  const char *isystem = "-isystem @\\..\\";

  line = strip_nl (str_ltrim(line));
  DEBUGF (2, "line: %s.\n", line);

  if (!strnicmp(line,isystem,strlen(isystem)))
  {
    char dir [MAX_PATH];

    snprintf (dir, sizeof(dir), "%s\\%s", bcc_root, line + strlen(isystem));
    DEBUGF (2, "dir: %s.\n", dir);
    add_to_dir_array (dir, FALSE, __LINE__);
  }
  else if (!strncmp(line,"-I",2))
  {
    split_env_var ("Borland INC", str_ltrim(line+2));
  }
  ARGSUSED (sl);
}

/**
 * Check in Borland's library-directories which are given by the format in
 * `<bcc_root>\bcc32c.cfg`:
 * ```
 *   -L@\..\lib\win32c\debug
 *   -L@\..\lib\win32c\release
 * ```
 *
 * Or the older format in `<bcc_root>\bcc32.cfg`:
 * ```
 *  -L<lib_path1>;<lib_path2>...
 * ```
 */
static void bcc32_cfg_parse_lib (smartlist_t *sl, char *line)
{
  const char *Ldir = "-L@\\..\\";

  line = strip_nl (str_ltrim(line));
  DEBUGF (2, "line: %s.\n", line);

  if (!strnicmp(line,Ldir,strlen(Ldir)))
  {
    char dir [MAX_PATH];

    snprintf (dir, sizeof(dir), "%s\\%s", bcc_root, line + strlen(Ldir));
    DEBUGF (2, "dir: %s.\n", dir);
    add_to_dir_array (dir, FALSE, __LINE__);
  }
  else if (!strncmp(line,"-L",2))
  {
    split_env_var ("Borland LIB", str_ltrim(line+2));
  }
  ARGSUSED (sl);
}

/**
 * Setup Borland directories for either a INC or LIB search.
 */
static BOOL setup_borland_dirs (const compiler_info *cc, bcc_parser_func parser)
{
  smartlist_t *dir_list;
  char        *bin_dir;
  char         bcc_cfg [_MAX_PATH];

  bcc_root = strlwr (STRDUP(cc->full_name));
  bin_dir =  strrchr (bcc_root, '\\');
  *bin_dir = '\0';
  bin_dir =  strrchr (bcc_root, '\\');
  *bin_dir = '\0';

  DEBUGF (2, "bcc_root: %s, short_name: %s\n", bcc_root, cc->short_name);

  /* Get the 'bcc*.cfg' filename:
   * <bcc_root>\bccX.exe -> <bcc_root>\bccX.cfg
   */
  snprintf (bcc_cfg, sizeof(bcc_cfg), "%s\\bin\\%.*s.cfg",
            bcc_root, strrchr(cc->short_name,'.') - cc->short_name, cc->short_name);

  DEBUGF (2, "bcc_cfg: %s.\n", bcc_cfg);

  dir_list = smartlist_read_file (bcc_cfg, (smartlist_parse_func)parser);
  if (!dir_list)
  {
    FREE (bcc_root);
    return (FALSE);
  }
  smartlist_free (dir_list);  /* No need for this */
  return (TRUE);
}

static int do_check_borland_inc_lib (const char *inc_lib, const char *matches, bcc_parser_func parser)
{
  int   ignored, bcc_found, found = 0;
  int   i, j, max_i, max_j;
  const compiler_info *cc;

  max_i = smartlist_len (all_cc);

  for (i = bcc_found = ignored = 0; i < max_i; i++)
  {
    cc = smartlist_get (all_cc, i);
    if (!cc->full_name || cc->type != CC_BORLAND)
       continue;

    bcc_found++;
    if (cc->ignore)
       ignored++;

    if (!cc->ignore && setup_borland_dirs(cc, parser))
    {
      char report [_MAX_PATH+50];

      snprintf (report, sizeof(report), matches, cc->full_name);
      report_header = report;

      max_j = smartlist_len (dir_array);
      for (j = 0; j < max_j; j++)
      {
        struct directory_array *arr = smartlist_get (dir_array, j);

        DEBUGF (1, "arr->dir: %s.\n", arr->dir);

        found += process_dir (arr->dir, arr->num_dup, arr->exist, TRUE,
                              arr->is_dir, arr->exp_ok, inc_lib,
                              HKEY_INC_LIB_FILE, FALSE);
      }
      free_dir_array();
      FREE (bcc_root);
    }
  }

  if (bcc_found == 0)
  {
    DEBUGF (1, "No Borland compilers found.\n");
    return (0);
  }
  if (ignored >= bcc_found)
  {
    DEBUGF (1, "All Borland compilers were ignored.\n");
    return (0);
  }
  return (found);
}

/**
 * Check all `bcc*.exe` found on `PATH` for an `INC` search.
 */
static int do_check_borland_includes (void)
{
  return do_check_borland_inc_lib ("Borland INC", "Matches in %s headers:\n",
                                   bcc32_cfg_parse_inc);
}

/**
 * Check all `bcc*.exe` found on `PATH` for a `LIB` search.
 */
static int do_check_borland_library_paths (void)
{
  return do_check_borland_inc_lib ("Borland LIB", "Matches in %s libraries:\n",
                                   bcc32_cfg_parse_lib);
}

/**
 * getopt_long() processing.
 */
static const struct option long_options[] = {
           { "help",        no_argument,       NULL, 'h' },
           { "help",        no_argument,       NULL, '?' },  /* 1 */
           { "version",     no_argument,       NULL, 'V' },
           { "inc",         no_argument,       NULL, 0 },    /* 3 */
           { "path",        no_argument,       NULL, 0 },
           { "lib",         no_argument,       NULL, 0 },    /* 5 */
           { "python",      optional_argument, NULL, 0 },
           { "dir",         no_argument,       NULL, 'D' },  /* 7 */
           { "debug",       optional_argument, NULL, 'd' },
           { "no-sys",      no_argument,       NULL, 0 },    /* 9 */
           { "no-usr",      no_argument,       NULL, 0 },
           { "no-app",      no_argument,       NULL, 0 },    /* 11 */
           { "test",        no_argument,       NULL, 't' },
           { "quiet",       no_argument,       NULL, 'q' },  /* 13 */
           { "no-gcc",      no_argument,       NULL, 0 },
           { "no-g++",      no_argument,       NULL, 0 },    /* 15 */
           { "verbose",     no_argument,       NULL, 'v' },
           { "pe",          no_argument,       NULL, 0 },    /* 17 */
           { "no-colour",   no_argument,       NULL, 0 },
           { "no-color",    no_argument,       NULL, 0 },    /* 19 */
           { "evry",        optional_argument, NULL, 0 },
           { "regex",       no_argument,       NULL, 0 },    /* 21 */
           { "size",        no_argument,       NULL, 0 },
           { "man",         no_argument,       NULL, 0 },    /* 23 */
           { "cmake",       no_argument,       NULL, 0 },
           { "pkg",         no_argument,       NULL, 0 },    /* 25 */
           { "32",          no_argument,       NULL, 0 },
           { "64",          no_argument,       NULL, 0 },    /* 27 */
           { "no-prefix",   no_argument,       NULL, 0 },
           { "no-ansi",     no_argument,       NULL, 0 },    /* 29 */
           { "host",        required_argument, NULL, 0 },
           { "buffered-io", no_argument,       NULL, 0 },    /* 31 */
           { "nonblock-io", no_argument,       NULL, 0 },
           { "no-watcom",   no_argument,       NULL, 0 },    /* 33 */
           { "no-borland",  no_argument,       NULL, 0 },
           { "no-clang",    no_argument,       NULL, 0 },    /* 35 */
           { "owner",       optional_argument, NULL, 0 },
           { "check",       no_argument,       NULL, 0 },    /* 37 */
           { "signed",      optional_argument, NULL, 0 },
           { "no-cwd",      no_argument,       NULL, 0 },    /* 39 */
           { "sort",        required_argument, NULL, 0 },
           { "vcpkg",       no_argument,       NULL, 0 },    /* 41 */
           { NULL,          no_argument,       NULL, 0 }
         };

static int *values_tab[] = {
            NULL,
            NULL,                     /* 1 */
            NULL,
            &opt.do_include,          /* 3 */
            &opt.do_path,
            &opt.do_lib,              /* 5 */
            &opt.do_python,
            &opt.dir_mode,            /* 7 */
            NULL,
            &opt.no_sys_env,          /* 9 */
            &opt.no_usr_env,
            &opt.no_app_path,         /* 11 */
            NULL,
            NULL,                     /* 13 */
            &opt.no_gcc,
            &opt.no_gpp,              /* 15 */
            &opt.verbose,
            &opt.PE_check,            /* 17 */
            &opt.no_colours,
            &opt.no_colours,          /* 19 */
            &opt.do_evry,
            &opt.use_regex,           /* 21 */
            &opt.show_size,
            &opt.do_man,              /* 23 */
            &opt.do_cmake,
            &opt.do_pkg,              /* 25 */
            &opt.only_32bit,
            &opt.only_64bit,          /* 27 */
            &opt.gcc_no_prefixed,
            &opt.no_ansi,             /* 29 */
            (int*)&opt.evry_host,
            &opt.use_buffered_io,     /* 31 */
            &opt.use_nonblock_io,
            &opt.no_watcom,           /* 33 */
            &opt.no_borland,
            &opt.no_clang,            /* 35 */
            &opt.show_owner,
            &opt.do_check,            /* 37 */
            (int*)&opt.signed_status,
            &opt.no_cwd,              /* 39 */
            (int*)&opt.sort_method,
            &opt.do_vcpkg             /* 41 */
          };

/**
 * `getopt_long()` handler for option `--python=<short_name>`.
 *
 * Accept only a Python `short_name` which is compiled in.
 *
 * Ref. the `all_py_programs[]` array in envtool_py.c.
 */
static void set_python_variant (const char *arg)
{
  const char **py = py_get_variants();
  unsigned     v  = UNKNOWN_PYTHON;
  int          i;

  DEBUGF (2, "optarg: '%s'\n", arg);
  ASSERT (arg);

  for (i = 0; py[i]; i++)
     if (!stricmp(arg,py[i]))
     {
       v = py_variant_value (arg, NULL);
       break;
    }

  if (v == UNKNOWN_PYTHON)
  {
    char buf[100], *p = buf;
    int  left = sizeof(buf)-1;

    for (i = 0; py[i] && left > 4; i++)
    {
      p += snprintf (p, left, "\"%s\", ", py[i]);
      left = (int) (sizeof(buf) - (p - buf));
    }
    if (p > buf+2)
       p[-2] = '\0';
    usage ("Illegal '--python' option: '%s'.\n"
           "Use one of these: %s.\n", arg, buf);
  }

  /* Found a valid match
   */
  py_which = (enum python_variants) v;
}

/**
 * `getopt_long()` handler for option `-H arg` or `--host arg` used by remote ETP queries.
 */
static void set_evry_options (const char *arg)
{
  if (arg)
  {
    if (!opt.evry_host)
       opt.evry_host = smartlist_new();
    smartlist_add (opt.evry_host, STRDUP(arg));
  }
}

/**
 * `getopt_long()` handler for option `--signed`.
 */
static void set_signed_options (const char *arg)
{
  static const struct search_list sign_status[] = {
                    { SIGN_CHECK_ALL,      "SIGN_CHECK_ALL"      },
                    { SIGN_CHECK_UNSIGNED, "SIGN_CHECK_UNSIGNED" },
                    { SIGN_CHECK_SIGNED,   "SIGN_CHECK_SIGNED"   },
                  };

  opt.signed_status = SIGN_CHECK_ALL;

  if (arg)
  {
    if (*arg == '1' || !stricmp(arg,"on") || !stricmp(arg,"yes"))
       opt.signed_status = SIGN_CHECK_SIGNED;

    else if (*arg == '0' || !stricmp(arg,"off") || !stricmp(arg,"no"))
       opt.signed_status = SIGN_CHECK_UNSIGNED;
  }

  DEBUGF (2, "got long option \"--signed %s\" -> opt.signed_status: %s\n",
          arg, list_lookup_name(opt.signed_status,sign_status, DIM(sign_status)));
}

/**
 * `getopt_long()` handler for option `--owner`.
 */
static void set_owner_options (const char *arg)
{
  opt.show_owner = 1;

  if (!opt.owners)
     opt.owners = smartlist_new();

  if (arg)
     smartlist_add (opt.owners, STRDUP(arg));
  else
  if (smartlist_len(opt.owners) == 0)
     smartlist_add (opt.owners, STRDUP("*"));
}

/**
 * The handler for short options called from `getopt_long()`.
 *
 * \param[in] o    an alphabetical letters given in `c->short_opt` and `getopt_parse(c)`.
 * \param[in] arg  the optional argument for the option.
 */
static void set_short_option (int o, const char *arg)
{
  DEBUGF (2, "got short option '%c' (%d).\n", o, o);

  switch (o)
  {
    case 'h':
         opt.help = 1;
         break;
    case 'H':
         set_evry_options (arg);
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
         opt.case_sensitive = 1;
         break;
    case 'k':
         opt.keep_temp = 1;
         break;
    case 'r':
         opt.use_regex = 1;
         break;
    case 's':
         opt.show_size = 1;
         break;
    case 'S':
         if (!set_sort_method(arg, NULL))
            usage ("Illegal \"-S\" method '%s'. Use one of: %s\n",
                   arg, get_sort_methods_short());
         break;
    case 'T':
         opt.decimal_timestamp = 1;
         break;
    case 't':
         opt.do_tests++;
         break;
    case 'u':
         opt.show_unix_paths = 1;
         break;
    case 'q':
         opt.quiet = 1;
         break;
    case '?':      /* '?' == BADCH || BADARG */
         usage ("  Use %s \"-h\" / \"--help\" for options\n", who_am_I);
         break;
    default:
         usage ("Illegal option: '%c'\n", optopt);
         break;
  }
}

/**
 * The handler for long options called from `getopt_long()`.
 *
 * \param[in] o    the index into `long_options[]`.
 * \param[in] arg  the optional argument for the option.
 */
static void set_long_option (int o, const char *arg)
{
  int new_value, *val_ptr;

  ASSERT (values_tab[o]);
  ASSERT (long_options[o].name);

  ASSERT (o >= 0);
  ASSERT (o < DIM(values_tab));

  DEBUGF (2, "got long option \"--%s\" with argument \"%s\".\n",
          long_options[o].name, arg);

  if (!strcmp("evry",long_options[o].name))
  {
    set_evry_options (arg);
    opt.do_evry = 1;
    return;
  }

  if (!strcmp("signed",long_options[o].name))
  {
    set_signed_options (arg);
    return;
  }

  if (!strcmp("sort",long_options[o].name))
  {
    if (!set_sort_method(NULL, arg))
       usage ("Illegal \"-S\" method '%s'. Use one of: %s\n",
              arg, get_sort_methods_long());
    return;
  }

  if (!strcmp("owner",long_options[o].name))
  {
    set_owner_options (arg);
    return;
  }

  if (arg)
  {
    if (!strcmp("python",long_options[o].name))
    {
      opt.do_python++;
      set_python_variant (arg);
    }

    else if (!strcmp("debug",long_options[o].name))
      opt.debug = atoi (arg);

    else if (!strcmp("host",long_options[o].name))
      set_evry_options (arg);
  }
  else
  {
    val_ptr = values_tab [o];
    new_value = *val_ptr + 1;

    DEBUGF (2, "got long option \"--%s\". Setting value %d -> %d. o: %d.\n",
            long_options[o].name, *val_ptr, new_value, o);

    *val_ptr = new_value;
  }
}

/**
 * Parse the command-line.
 */
static void parse_cmdline (void)
{
  command_line *c = &opt.cmd_line;

  c->env_opt       = "ENVTOOL_OPTIONS";
  c->short_opt     = "+chH:vVdDkrsS:tTuq";
  c->long_opt      = long_options;
  c->set_short_opt = set_short_option;
  c->set_long_opt  = set_long_option;
  getopt_parse (c);

  if (c->argc0 > 0 && c->argc - c->argc0 >= 1)
     opt.file_spec = STRDUP (c->argv[c->argc0]);

  if ((c->argc0 > 0) && (c->argc - c->argc0 >= 2))
     opt.evry_raw = TRUE;

  if (opt.evry_raw)
  {
    FREE (opt.file_spec);
    opt.file_spec = _strjoin (c->argv + c->argc0, " ");
  }

  DEBUGF (2, "c->argc0:      %d\n", c->argc0);
  DEBUGF (2, "opt.file_spec: %s'\n", opt.file_spec);
  DEBUGF (2, "opt.evry_raw:  %d\n", opt.evry_raw);
}

/**
 * Evaluate if some combinations of options doesn't make sense.
 */
static int eval_options (void)
{
  if (!(opt.do_lib || opt.do_include) && opt.only_32bit && opt.only_64bit)
  {
    WARN ("Specifying both '--32' and '--64' doesn't make sense.\n");
    return (0);
  }

  if (!opt.PE_check && (opt.only_32bit || opt.only_64bit))
     opt.PE_check = TRUE;

  C_no_ansi = opt.no_ansi;

  /* Use ANSI-sequences under ConEmu or if "%COLOUR_TRACE >= 2".
   * Nullifies the "--no-ansi" option.
   */
  if (opt.under_conemu || C_trace_level() >= 2)
     C_use_ansi_colours = 1;

  if (opt.no_colours)
     C_use_colours = C_use_ansi_colours = 0;

  return (1);
}

/**
 * The only `atexit()` function where all cleanup is done.
 */
static void MS_CDECL cleanup (void)
{
  /* If we're called from the SIGINT thread, don't do any Python stuff.
   * That will probably crash in Py_Finalize().
   */
  if (halt_flag == 0)
     py_exit();

  free_dir_array();

  FREE (who_am_I);

  FREE (system_env_path);
  FREE (system_env_lib);
  FREE (system_env_inc);

  FREE (user_env_path);
  FREE (user_env_lib);
  FREE (user_env_inc);
  FREE (vcache_fname);
  FREE (opt.file_spec);

  free_all_compilers();

  if (re_alloc)
     regfree (&re_hnd);

  smartlist_free (dir_array);
  smartlist_free (reg_array);

  smartlist_free_all (opt.evry_host);
  smartlist_free_all (opt.owners);

  getopt_free (&opt.cmd_line);

  cfg_ignore_exit();

  netrc_exit();
  authinfo_exit();
  envtool_cfg_exit();
  vcpkg_free();

  exit_misc();

  if (halt_flag == 0 && opt.debug > 0)
     mem_report();

  if (halt_flag > 0)
     C_puts ("~5Quitting.\n~0");

  C_exit();
  crtdbug_exit();
}

/**
 * This signal-handler gets called in another thread.
 *
 * \param[in] sig the signal to handle.
 */
static void MS_CDECL halt (int sig)
{
  BOOL quick_exit = FALSE;

  halt_flag++;
  CRTDBG_CHECK_OFF();

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

#ifdef SIGTRAP
  if (sig == SIGTRAP)
  {
    C_puts ("\n~5Got SIGTRAP.~0\n");
    quick_exit = TRUE;
  }
#endif

  if (sig == SIGILL)
  {
    C_puts ("\n~5Illegal instruction.~0\n");
    quick_exit = TRUE;
  }

  if (quick_exit)
  {
    /* Get out as fast as possible */
    C_exit();
    ExitProcess (GetCurrentProcessId());
  }
}

/**
 * The main initialiser.
 */
static void init_all (const char **argv)
{
  char buf [_MAX_PATH];

  atexit (cleanup);
  crtdbug_init();

  tzset();
  memset (&opt, 0, sizeof(opt));
  opt.under_conemu = C_conemu_detected();

  if (GetModuleFileName(NULL, buf, sizeof(buf)))
       who_am_I = STRDUP (buf);
  else who_am_I = STRDUP (argv[0]);

  program_name = who_am_I;

  C_use_colours = 1;  /* Turned off by "--no-colour" */

  dir_array = smartlist_new();
  reg_array = smartlist_new();

#ifdef __CYGWIN__
  opt.conv_cygdrive = 1;
#endif

  vcache_fname = getenv_expand ("%TEMP%\\envtool.cache");

  current_dir[0] = '.';
  current_dir[1] = DIR_SEP;
  current_dir[2] = '\0';
  getcwd (current_dir, sizeof(current_dir));

  sys_dir[0] = sys_native_dir[0] = '\0';
  if (GetSystemDirectory(sys_dir,sizeof(sys_dir)))
  {
    const char *rslash = strrchr (sys_dir,'\\');

    if (rslash > sys_dir)
    {
      snprintf (sys_native_dir, sizeof(sys_native_dir), "%.*s\\sysnative",
                (int)(rslash - sys_dir), sys_dir);
      snprintf (sys_wow64_dir, sizeof(sys_wow64_dir), "%.*s\\SysWOW64",
                (int)(rslash - sys_dir), sys_dir);
    }
  }
}

/**
 * Our main entry point.
 *  \li Initialise program.
 *  \li Parse the command line.
 *  \li Evaluate given options for conflicts.
 *  \li Open and parse `"%APPDATA%\\envtool.cfg"`.
 *  \li Check if `%WINDIR%\\sysnative` and/or `%WINDIR%\\SysWOW64` exists.
 *  \li Install signal-handlers for `SIGINT` and `SIGILL`.
 *  \li Call the appropriate functions based on command-line options.
 *  \li Finally call `final_report()` to report findings.
 */
int MS_CDECL main (int argc, const char **argv)
{
  int found = 0;

  init_all (argv);

  parse_cmdline();
  if (!eval_options())
     return (1);

  cfg_ignore_init ("%APPDATA%\\envtool.cfg");
  check_sys_dirs();

  /* Sometimes the IPC connection to the EveryThing Database will hang.
   * Clean up if user presses ^C.
   * SIGILL handler is needed for test_libssp().
   */
  signal (SIGINT, halt);
  signal (SIGILL, halt);

  if (opt.help)
     return show_help();

  if (opt.do_version)
     return show_version();

  if (opt.do_python)
     py_init();

  if (opt.do_check)
     return do_check();

  if (opt.do_tests)
     return do_tests();

  if (opt.do_evry && !opt.do_path)
     opt.no_sys_env = opt.no_usr_env = opt.no_app_path = 1;

  if (opt.do_lib || opt.do_include)
     search_and_add_all_cc (FALSE, FALSE);

  if (!(opt.do_path || opt.do_lib || opt.do_include))
     opt.no_sys_env = opt.no_usr_env = 1;

  if (!opt.do_path && !opt.do_include && !opt.do_lib && !opt.do_python &&
      !opt.do_evry && !opt.do_cmake   && !opt.do_man && !opt.do_pkg && !opt.do_vcpkg)
     usage ("Use at least one of; \"--evry\", \"--cmake\", \"--inc\", \"--lib\", "
            "\"--man\", \"--path\", \"--pkg\", \"--vcpkg\" and/or \"--python\".\n");

  if (!opt.file_spec)
     usage ("You must give a ~1filespec~0 to search for.\n");

  if (!opt.evry_raw && !opt.dir_mode)
  {
    if (!opt.use_regex)
    {
      char *end, *dot, *fspec;

      if (strchr(opt.file_spec,'~') > opt.file_spec)
      {
        fspec = opt.file_spec;
        opt.file_spec = _fix_path (fspec, NULL);
        FREE (fspec);
      }

      end = strrchr (opt.file_spec, '\0');
      dot = strrchr (opt.file_spec, '.');
      if (!dot && !opt.do_vcpkg)
      {
        if (opt.do_pkg && end > opt.file_spec && end[-1] != '*')
           opt.file_spec = _stracat (opt.file_spec, ".pc*");

        else if (opt.do_vcpkg && end > opt.file_spec && end[-1] != '*')
           opt.file_spec = _stracat (opt.file_spec, "*");

        else if (end > opt.file_spec && end[-1] != '*' && end[-1] != '$')
           opt.file_spec = _stracat (opt.file_spec, ".*");
      }
    }
    else
    {
      re_err = regcomp (&re_hnd, opt.file_spec, opt.case_sensitive ? 0 : REG_ICASE);
      re_alloc = TRUE;
      if (re_err)
      {
        regerror (re_err, &re_hnd, re_errbuf, sizeof(re_errbuf));
        WARN ("Invalid regular expression \"%s\": %s\n", opt.file_spec, re_errbuf);
      }
    }
  }

  DEBUGF (1, "opt.file_spec: '%s'\n", opt.file_spec);

  if (!opt.no_sys_env)
     found += scan_system_env();

  if (!opt.no_usr_env)
     found += scan_user_env();

  if (opt.do_path)
  {
    if (!opt.no_app_path)
       found += do_check_registry();

    report_header = "Matches in %PATH:\n";
    found += do_check_env ("PATH", FALSE);
  }

  if (opt.do_lib)
  {
    report_header = "Matches in %LIB:\n";
    found += do_check_env ("LIB", FALSE);

    if (!opt.no_watcom)
       found += do_check_watcom_library_paths();

    if (!opt.no_borland)
       found += do_check_borland_library_paths();

    if (!(opt.no_gcc && opt.no_gpp))   /* Ignore all 'gcc.exe' and g++.exe' */
       found += do_check_gcc_library_paths();

    if (!opt.no_clang)
       found += do_check_clang_library_paths();
  }

  if (opt.do_include)
  {
    report_header = "Matches in %INCLUDE:\n";
    found += do_check_env ("INCLUDE", FALSE);

    if (!opt.no_watcom)
       found += do_check_watcom_includes();

    if (!opt.no_borland)
      found += do_check_borland_includes();

    if (!opt.no_gcc)
       found += do_check_gcc_includes();

    if (!opt.no_gpp)
       found += do_check_gpp_includes();

    if (!opt.no_clang)
       found += do_check_clang_includes();
  }

  if (opt.do_cmake)
     found += do_check_cmake();

  if (opt.do_man)
     found += do_check_manpath();

  if (opt.do_pkg)
     found += do_check_pkg();

  if (opt.do_vcpkg)
     found += do_check_vcpkg();

  if (opt.do_python)
  {
    char  report [_MAX_PATH+50];
    char *py_exe;

    py_get_info (&py_exe, NULL, NULL);
    snprintf (report, sizeof(report), "Matches in \"%s\" sys.path[]:\n", py_exe);
    report_header = report;
    found += py_search();
    FREE (py_exe);
  }

  /* Mode "--evry" specified.
   */
  if (opt.do_evry)
  {
    int i, max = 0;

    if (opt.evry_host)
       max = smartlist_len (opt.evry_host);

    /* Mode "--evry:host" specified at least once.
     * Connect and query all hosts.
     */
    for (i = 0; i < max; i++)
    {
      const char *host = smartlist_get (opt.evry_host, i);
      char  buf [200];

      snprintf (buf, sizeof(buf), "Matches from %s:\n", host);
      report_header = buf;
      found += do_check_evry_ept (host);
    }
    if (max  == 0)
    {
      report_header = "Matches from EveryThing:\n";
      found += do_check_evry();
    }
  }

  ARGSUSED (argc);

  final_report (found);
  return (found ? 0 : 1);
}

/**
 * Test non-Cygwin env-var splitting in split_env_var().
 */
static void test_split_env (const char *env)
{
  smartlist_t *list;
  char        *value;
  int          i, max;

  C_printf ("~3%s():~0 ", __FUNCTION__);
  C_printf (" 'split_env_var (\"%s\",\"%%%s\")':\n", env, env);

  value = getenv_expand (env);
  list  = split_env_var (env, value);
  max   = list ? smartlist_len (list) : 0;
  for (i = 0; i < max; i++)
  {
    const struct directory_array *arr = smartlist_get (list, i);
    char  buf [_MAX_PATH];
    char *dir = arr->dir;

    if (arr->exist && arr->is_dir)
       dir = _fix_path (dir, buf);

    if (opt.show_unix_paths)
       dir = slashify2 (dir, dir, '/');

    C_printf ("  arr[%2d]: %-65s", i, dir);

    if (arr->cyg_dir)
       C_printf ("\n%*s%s", 11, "", arr->cyg_dir);

    if (arr->num_dup > 0)
       C_puts ("  ~3**duplicated**~0");
    if (arr->is_native && !have_sys_native_dir)
       C_puts ("  ~5**native dir not existing**~0");
    else if (!arr->exist)
       C_puts ("  ~5**not existing**~0");
    else if (!arr->is_dir)
       C_puts ("  ~5**not a dir**~0");

    C_putc ('\n');
  }
  free_dir_array();
  FREE (value);
  C_printf ("  ~3%d elements~0\n\n", i);
}


#if defined(__CYGWIN__)

#pragma GCC diagnostic ignored  "-Wstack-protector"

/**
 * Test Cygwin specific env-var splitting in split_env_var().
 */
static void test_split_env_cygwin (const char *env)
{
  smartlist_t *list;
  char        *value, *cyg_value;
  int          i, max, rc, needed, save = opt.conv_cygdrive;

  free_dir_array();

  C_printf ("~3%s():~0 ", __FUNCTION__);
  C_printf (" testing 'split_env_var (\"%s\",\"%%%s\")':\n", env, env);

  value = getenv_expand (env);
  if (!value)
  {
    i = 0;
    goto fail;  /* Should not happen */
  }

  needed = cygwin_conv_path_list (CCP_WIN_A_TO_POSIX, value, NULL, 0);
  cyg_value = alloca (needed+1);
  DEBUGF (2, "cygwin_conv_path_list(): needed %d\n", needed);

  rc = cygwin_conv_path_list (CCP_WIN_A_TO_POSIX, value, cyg_value, needed+1);
  DEBUGF (2, "cygwin_conv_path_list(): rc: %d, '%s'\n", rc, cyg_value);

  path_separator = ':';
  opt.conv_cygdrive = 0;
  list = split_env_var (env, cyg_value);

  max = list ? smartlist_len (list) : 0;

  for (i = 0; i < max; i++)
  {
    const struct directory_array *arr = smartlist_get (list, i);
    char *dir = arr->dir;

    if (arr->exist && arr->is_dir)
       dir = cygwin_create_path (CCP_WIN_A_TO_POSIX, dir);

    C_printf ("  arr[%d]: %s", i, dir);

    if (arr->num_dup > 0)
       C_puts ("  ~4**duplicated**~0");
    if (!arr->exist)
       C_puts ("  ~5**not existing**~0");
    if (!arr->is_dir)
       C_puts ("  ~4**not a dir**~0");
    C_putc ('\n');

    if (dir != arr->dir)
       free (dir);
  }
  free_dir_array();
  FREE (value);

  path_separator = ';';
fail:
  opt.conv_cygdrive = save;
  C_printf ("~0  %d elements\n\n", i);
}

/**
 * Test the POSIX to Windows Path functions.
 */
void test_posix_to_win_cygwin (void)
{
  int i, rc, raw, save;
  static const char *cyg_paths[] = {
                    "/usr/bin",
                    "/usr/lib",
                    "/etc/profile.d",
                    "~/",
                    "/cygdrive/c"
                  };

  C_printf ("~3%s():~0\n", __FUNCTION__);

  path_separator = ':';
  save = opt.conv_cygdrive;
  opt.conv_cygdrive = 0;

  for (i = 0; i < DIM(cyg_paths); i++)
  {
    const char *file, *dir = cyg_paths[i];
    char  result [_MAX_PATH];

    rc = cygwin_conv_path (CCP_POSIX_TO_WIN_A, dir, result, sizeof(result));
    DEBUGF (2, "cygwin_conv_path(CCP_POSIX_TO_WIN_A): rc: %d, '%s'\n", rc, result);

    raw = C_setraw (1);  /* In case result contains a "~". */

    file = slashify2 (result, result, opt.show_unix_paths ? '/' : '\\');
    C_printf ("    %-20s -> %s\n", cyg_paths[i], file);
    C_setraw (raw);
  }
  C_putc ('\n');
  path_separator = ';';
  opt.conv_cygdrive = save;
}
#endif  /* __CYGWIN__ */

/**\struct test_table1
 * The structure used in test_searchpath().
 */
struct test_table1 {
       const char *file;   /**< the file to test in searchpath() */
       const char *env;    /**< the environment variable to use */
     };

static const struct test_table1 tab1[] = {
                  { "kernel32.dll",      "PATH" },
                  { "notepad.exe",       "PATH" },

                  /* Relative file-name test:
                   *   'c:\Windows\system32\Resources\Themes\aero.theme' is present in Win-8.1+
                   *   and 'c:\Windows\system32' should always be on PATH.
                   */
                  { "..\\Resources\\Themes\\aero.theme", "PATH" },

                  { "./envtool.c",       "FOO-BAR" },       /* CWD should always be at pos 0 regardless of env-var. */
                  { "msvcrt.lib",        "LIB" },
                  { "libgcc.a",          "LIBRARY_PATH" },  /* MinGW-w64 doesn't seem to have libgcc.a */
                  { "libgmon.a",         "LIBRARY_PATH" },
                  { "stdio.h",           "INCLUDE" },
                  { "../os.py",          "PYTHONPATH" },

                  /* test if searchpath() works for Short File Names
                   * (%WinDir\systems32\PresentationHost.exe).
                   * SFN seems not to be available on Win-7+.
                   * "PRESEN~~1.EXE" = "PRESEN~1.EXE" since C_printf() is used.
                   */
                  { "PRESEN~~1.EXE",      "PATH" },

                  /* test if searchpath() works with "%WinDir%\sysnative" on Win-7+.
                   */
#if (IS_WIN64)
                  { "NDIS.SYS",          "%WinDir%\\system32\\drivers" },
#else
                  { "NDIS.SYS",          "%WinDir%\\sysnative\\drivers" },
#endif
                  { "SWAPFILE.SYS",      "c:\\" },  /* test if searchpath() finds hidden files. */
                  { "\\\\localhost\\$C", "PATH" },  /* Does it work on a share too? */
                  { "\\\\.\\C:",         "PATH" },  /* Or as a device name? */
                  { "CLOCK$",            "PATH" },  /* Does it handle device names? */
                  { "PRN",               "PATH" }
                };

/**
 * Tests for searchpath().
 */
static void test_searchpath (void)
{
  const struct test_table1 *t;
  size_t len, i = 0;
  int    is_env, pad;

  C_printf ("~3%s():~0\n", __FUNCTION__);
  C_printf ("  ~6What %s Where                      Result~0\n", str_repeat(' ',28));

  for (t = tab1; i < DIM(tab1); t++, i++)
  {
    const char *env   = t->env;
    const char *found = searchpath (t->file, env);

    is_env = (strchr(env,'\\') == NULL);
    len = C_printf ("  %s:", t->file);
    pad = max (0, 35-(int)len);
    C_printf ("%*s %s%s", pad, "", is_env ? "%" : "", env);
    pad = max (0, 26-(int)strlen(env)-is_env);
    C_printf ("%*s -> %s, pos: %d\n", pad, "",
              found ? found : strerror(errno), searchpath_pos());
  }
  C_putc ('\n');
}

struct test_table2 {
       int         expect;
       const char *pattern;
       const char *fname;
       int         flags;
     };

static const struct test_table2 tab2[] = {
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

/**
 * Tests for fnmatch().
 *
 * `test_table::expect` does not work with `opt.case_sensitive`.
 * I.e. `envtool --test -C`.
 */
static void test_fnmatch (void)
{
  const struct test_table2 *t;
  size_t len1, len2;
  int    rc, flags, i = 0;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  for (t = tab2; i < DIM(tab2); t++, i++)
  {
    flags = fnmatch_case (t->flags);
    rc    = fnmatch (t->pattern, t->fname, flags);
    len1  = strlen (t->pattern);
    len2  = strlen (t->fname);

    C_puts (rc == t->expect ? "~2  OK  ~0" : "~5  FAIL~0");

    C_printf (" fnmatch (\"%s\", %*s \"%s\", %*s 0x%02X): %s\n",
              t->pattern, (int)(15-len1), "", t->fname, (int)(15-len2), "",
              flags, fnmatch_res(rc));
  }
  C_putc ('\n');
}

/**
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
  char  fbuf [_MAX_PATH];
  int   i;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(files1); i++)
  {
    f  = files1 [i];
    rc = slashify2 (fbuf, f, '/');
    C_printf ("  (\"%s\",'/') %*s -> %s\n", f, (int)(39-strlen(f)), "", rc);
  }
  for (i = 0; i < DIM(files2); i++)
  {
    f  = files2 [i];
    rc = slashify2 (fbuf, f, '\\');
    C_printf ("  (\"%s\",'\\\\') %*s -> %s\n", f, (int)(38-strlen(f)), "", rc);
  }
  C_putc ('\n');
}

/**
 * Tests for _fix_path().
 * Canonize the horrendous pathnames reported from `gcc -v`.
 *
 * It doesn't matter if these paths or files exists or not. `_fix_path()`
 * (i.e. `GetFullPathName()`) should canonizes these regardless.
 */
static void test_fix_path (void)
{
  static const char *files[] = {
    "f:\\CygWin64\\bin\\../lib/gcc/x86_64-w64-mingw32/6.4.0/include",                            /* exists here */
    "f:\\CygWin64\\bin\\../lib/gcc/x86_64-w64-mingw32/6.4.0/include\\ssp\\ssp.h",                /* exists here */
    "f:\\CygWin64\\bin\\../lib/gcc/i686-w64-mingw32/6.4.0/../../../../i686-w64-mingw32/include", /* exists here */
    "/usr/lib/gcc/x86_64-pc-cygwin/6.4.0/../../../../include/w32api"                             /* CygWin64 output, exists here */
  };
  const char *f;
  char *rc1;
  int   i, rc2;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(files); i++)
  {
    char buf [_MAX_PATH], *cyg_result = "";
    BOOL is_dir;

    f = files [i];
    rc1 = _fix_path (f, buf);    /* has only '\\' slashes */
    rc2 = FILE_EXISTS (buf);
    is_dir = is_directory (rc1);

    if (opt.show_unix_paths)
       rc1 = slashify2 (buf, buf, '/');

#if defined(__CYGWIN__)
    {
      extern char *canonicalize_file_name (const char *);
      char *realname = canonicalize_file_name (f);

      if (realname)
      {
        cyg_result = ", ~2cyg-exists: 1";
        rc1 = _strlcpy (buf, realname, sizeof(buf));
        free (realname);
      }
      else
        cyg_result = ", ~5cyg-exists: 0";
    }
#endif

    C_printf ("  _fix_path (\"%s\")\n", f);

    if (!rc2)
         C_printf ("   ~5exists 0, is_dir %d%s~0", is_dir, cyg_result);
    else C_printf ("   ~2exists 1, is_dir %d%s~0", is_dir, cyg_result);

    C_printf (" -> %s\n", rc1);
  }
  C_putc ('\n');
}

/*
 * https://msdn.microsoft.com/en-us/library/windows/desktop/bb762181%28v=vs.85%29.aspx
 */
static void test_SHGetFolderPath (void)
{
  #undef  ADD_VALUE
  #define ADD_VALUE(v)  { v, #v }

  #ifndef CSIDL_PROGRAM_FILESX86
  #define CSIDL_PROGRAM_FILESX86  0x002a
  #endif

  static const struct search_list sh_folders[] = {
                      ADD_VALUE (CSIDL_ADMINTOOLS),
                      ADD_VALUE (CSIDL_ALTSTARTUP),
                      ADD_VALUE (CSIDL_APPDATA),      /* Use this as HOME-dir ("~/") */
                      ADD_VALUE (CSIDL_BITBUCKET),    /* Recycle Bin */
                      ADD_VALUE (CSIDL_COMMON_ALTSTARTUP),
                      ADD_VALUE (CSIDL_COMMON_FAVORITES),
                      ADD_VALUE (CSIDL_COMMON_STARTMENU),
                      ADD_VALUE (CSIDL_COMMON_PROGRAMS),
                      ADD_VALUE (CSIDL_COMMON_STARTUP),
                      ADD_VALUE (CSIDL_COMMON_DESKTOPDIRECTORY),
                      ADD_VALUE (CSIDL_COOKIES),
                      ADD_VALUE (CSIDL_DESKTOP),
                      ADD_VALUE (CSIDL_LOCAL_APPDATA),
                      ADD_VALUE (CSIDL_NETWORK),
                      ADD_VALUE (CSIDL_NETHOOD),
                      ADD_VALUE (CSIDL_PERSONAL),
                      ADD_VALUE (CSIDL_PROFILE),
                      ADD_VALUE (CSIDL_PROGRAM_FILES),
                      ADD_VALUE (CSIDL_PROGRAM_FILESX86),
                      ADD_VALUE (CSIDL_PROGRAM_FILES_COMMON),
                      ADD_VALUE (CSIDL_PROGRAM_FILES_COMMONX86),
                      ADD_VALUE (CSIDL_STARTUP),
                      ADD_VALUE (CSIDL_SYSTEM),
                      ADD_VALUE (CSIDL_SYSTEMX86),
                      ADD_VALUE (CSIDL_TEMPLATES),
                      ADD_VALUE (CSIDL_WINDOWS)
                    };

#if 0
#define CSIDL_INTERNET                  0x0001        // Internet Explorer (icon on desktop)
#define CSIDL_PROGRAMS                  0x0002        // Start Menu\Programs
#define CSIDL_CONTROLS                  0x0003        // My Computer\Control Panel
#define CSIDL_PRINTERS                  0x0004        // My Computer\Printers
#define CSIDL_PERSONAL                  0x0005        // My Documents
#define CSIDL_FAVORITES                 0x0006        // <user name>\Favorites
#define CSIDL_STARTUP                   0x0007        // Start Menu\Programs\Startup
#define CSIDL_RECENT                    0x0008        // <user name>\Recent
#define CSIDL_SENDTO                    0x0009        // <user name>\SendTo
#define CSIDL_BITBUCKET                 0x000a        // <desktop>\Recycle Bin
#define CSIDL_STARTMENU                 0x000b        // <user name>\Start Menu
#define CSIDL_MYMUSIC                   0x000d        // "My Music" folder
#define CSIDL_MYVIDEO                   0x000e        // "My Videos" folder
#define CSIDL_DESKTOPDIRECTORY          0x0010        // <user name>\Desktop
#define CSIDL_DRIVES                    0x0011        // My Computer
#define CSIDL_NETWORK                   0x0012        // Network Neighborhood (My Network Places)
#define CSIDL_NETHOOD                   0x0013        // <user name>\nethood
#define CSIDL_FONTS                     0x0014        // windows\fonts
#define CSIDL_TEMPLATES                 0x0015
#define CSIDL_COMMON_STARTMENU          0x0016        // All Users\Start Menu
#define CSIDL_COMMON_PROGRAMS           0X0017        // All Users\Start Menu\Programs
#define CSIDL_COMMON_STARTUP            0x0018        // All Users\Startup
#define CSIDL_COMMON_DESKTOPDIRECTORY   0x0019        // All Users\Desktop
#define CSIDL_APPDATA                   0x001a        // <user name>\Application Data
#define CSIDL_PRINTHOOD                 0x001b        // <user name>\PrintHood
#define CSIDL_LOCAL_APPDATA             0x001c        // <user name>\Local Settings\Applicaiton Data (non roaming)
#define CSIDL_ALTSTARTUP                0x001d        // non localized startup
#define CSIDL_COMMON_ALTSTARTUP         0x001e        // non localized common startup
#define CSIDL_COMMON_FAVORITES          0x001f
#define CSIDL_INTERNET_CACHE            0x0020
#define CSIDL_COOKIES                   0x0021
#define CSIDL_HISTORY                   0x0022
#define CSIDL_COMMON_APPDATA            0x0023        // All Users\Application Data
#define CSIDL_WINDOWS                   0x0024        // GetWindowsDirectory()
#define CSIDL_SYSTEM                    0x0025        // GetSystemDirectory()
#define CSIDL_PROGRAM_FILES             0x0026        // C:\Program Files
#define CSIDL_MYPICTURES                0x0027        // C:\Program Files\My Pictures
#define CSIDL_PROFILE                   0x0028        // USERPROFILE
#define CSIDL_SYSTEMX86                 0x0029        // x86 system directory on RISC
#define CSIDL_PROGRAM_FILESX86          0x002a        // x86 C:\Program Files on RISC
#define CSIDL_PROGRAM_FILES_COMMON      0x002b        // C:\Program Files\Common
#define CSIDL_PROGRAM_FILES_COMMONX86   0x002c        // x86 Program Files\Common on RISC
#define CSIDL_COMMON_TEMPLATES          0x002d        // All Users\Templates
#define CSIDL_COMMON_DOCUMENTS          0x002e        // All Users\Documents
#define CSIDL_COMMON_ADMINTOOLS         0x002f        // All Users\Start Menu\Programs\Administrative Tools
#define CSIDL_ADMINTOOLS                0x0030        // <user name>\Start Menu\Programs\Administrative Tools
#define CSIDL_CONNECTIONS               0x0031        // Network and Dial-up Connections
#define CSIDL_COMMON_MUSIC              0x0035        // All Users\My Music
#define CSIDL_COMMON_PICTURES           0x0036        // All Users\My Pictures
#define CSIDL_COMMON_VIDEO              0x0037        // All Users\My Video
#define CSIDL_RESOURCES                 0x0038        // Resource Direcotry
#define CSIDL_RESOURCES_LOCALIZED       0x0039        // Localized Resource Direcotry
#define CSIDL_COMMON_OEM_LINKS          0x003a        // Links to All Users OEM specific apps
#define CSIDL_CDBURN_AREA               0x003b        // USERPROFILE\Local Settings\Application Data\Microsoft\CD Burning
#define CSIDL_COMPUTERSNEARME           0x003d        // Computers Near Me (computered from Workgroup membership)

#endif

  int i;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(sh_folders); i++)
  {
    char                      buf [_MAX_PATH];
    const struct search_list *folder   = sh_folders + i;
    const char               *flag_str = opt.verbose ? "SHGFP_TYPE_CURRENT" : "SHGFP_TYPE_DEFAULT";
    DWORD                     flag     = opt.verbose ?  SHGFP_TYPE_CURRENT  :  SHGFP_TYPE_DEFAULT;
    HRESULT                   rc       = SHGetFolderPath (NULL, folder->value, NULL, flag, buf);

    if (rc == S_OK)
         slashify2 (buf, buf, opt.show_unix_paths ? '/' : '\\');
    else snprintf (buf, sizeof(buf), "~5Failed: %s", win_strerror(rc));

    C_printf ("  ~3SHGetFolderPath~0 (~6%s~0, ~6%s~0):\n    ~2%s~0\n",
              folder->name, flag_str, buf);
  }
  C_putc ('\n');
}

/*
 * Test Windows' Reparse Points (Junctions and directory symlinks).
 *
 * Also make a test similar to the 'dir /AL' command (Attribute Reparse Points):
 *  cmd.exe /c dir \ /s /AL
 */
static void test_ReparsePoints (void)
{
  static const char *points[] = {
                    "c:\\Users\\All Users",
                    "c:\\Documents and Settings",
                    "c:\\Documents and Settings\\",
                    "c:\\ProgramData",
                    "c:\\Program Files",
                    "c:\\Program Files (x86)",
                  };
  int i;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(points); i++)
  {
    const char *p = points[i];
    char  result [_MAX_PATH];
    char  st_result [100] = "";
    BOOL  rc = get_reparse_point (p, result, TRUE);

#if defined(__CYGWIN__)
    {
      struct stat st;

      if (lstat(p, &st) == 0)
         sprintf (st_result, ", link: %s.", S_ISLNK(st.st_mode) ? "Yes" : "No");
    }
#endif

    C_printf ("  %d: \"%s\" %*s->", i, p, (int)(26-strlen(p)), "");

    if (!rc)
         C_printf (" ~5%s~0%s\n", last_reparse_err, st_result);
    else C_printf (" \"%s\"%s\n", slashify2(result, result, opt.show_unix_paths ? '/' : '\\'), st_result);
  }
  C_putc ('\n');
}

/*
 * Test the parsing of '%APPDATA%/.netrc' and '%APPDATA%/.authinfo'.
 */
static void test_auth (void)
{
  const char *appdata = getenv ("APPDATA");
  int   rc1, rc2, rc3;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  rc1 = netrc_lookup (NULL, NULL, NULL);
  rc2 = authinfo_lookup (NULL, NULL, NULL, NULL);
  rc3 = envtool_cfg_lookup (NULL, NULL, NULL, NULL);

  C_printf ("  Parsing ~6%s\\.netrc~0      ", appdata);
  if (rc1 == 0)
       C_puts ("~5failed.~0\n");
  else C_puts ("~3okay.~0\n");

  C_printf ("  Parsing ~6%s\\.authinfo~0   ", appdata);
  if (rc2 == 0)
       C_puts ("~5failed.~0\n");
  else C_puts ("~3okay.~0\n");

  C_printf ("  Parsing ~6%s\\envtool.cfg~0 ", appdata);
  if (rc3 == 0)
       C_puts ("~5failed.~0\n");
  else C_puts ("~3okay.~0\n");
}

/*
 * Test PE-file WinTrust crypto signature verification.
 * Optionally calling 'get_file_owner()' for each file if
 * option "--owner" was used on command-line.
 */
static void test_PE_wintrust (void)
{
  static const char *files[] = {
              "%s\\kernel32.dll",
              "%s\\drivers\\tcpip.sys",
              "c:\\bootmgr",
              "notepad.exe",
              "cl.exe",
              "some-file-never-found.exe",
              "%s\\drivers\\",        /* test "--owner" on a directory */
              "c:\\$Recycle.Bin\\"
            };
  int i;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(files); i++)
  {
    char  *file = (char*) files[i];
    char  *account_name;
    char  *domain_name;
    char   path [_MAX_PATH];
    char  *is_sys = strchr (file, '%');
    size_t len;
    DWORD  wintrust_rc;

    if (is_sys)
    {
      if (have_sys_native_dir)
           snprintf (path, sizeof(path), "%s\\%s", sys_native_dir, is_sys+3);
      else snprintf (path, sizeof(path), "%s\\%s", sys_dir, is_sys+3);
      file = path;
    }
    else if (file[1] != ':')
    {
      file = searchpath (file, "PATH");
      if (!file)
         file = _strlcpy (path, files[i], sizeof(path));
    }

    wintrust_rc = wintrust_check (file, FALSE, FALSE);

    len = strlen (file);
    if (len > 50)
         C_printf ("  %d: ...%-47.47s ->", i, file+len-47);
    else C_printf ("  %d: %-50.50s ->", i, file);

    C_printf (" ~2%-10s~0", wintrust_check_result(wintrust_rc));

    if (opt.show_owner)
    {
      if (wintrust_rc == ERROR_FILE_NOT_FOUND)
         C_printf ("  ~5<Not found>~0");
      else if (get_file_owner(file, &domain_name, &account_name))
      {
        C_printf ("  ~4%s\\%s~0", domain_name, account_name);
        FREE (domain_name);
        FREE (account_name);
      }
      else
        C_printf ("  ~5<Unknown>~0");
    }
    C_putc ('\n');
  }
  C_putc ('\n');

  wintrust_dump_pkcs7_cert (NULL);   /* does nothing at the moment */
}

static void test_disk_ready (void)
{
  static int drives[] = { 'A', 'C', 'X', 'Y' };
  int    i, d;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(drives); i++)
  {
    d = drives[i];
    C_printf ("  disk_ready('%c') -> ...", d);
    C_flush();
    C_printf (" %2d\n", disk_ready(d));
  }
  C_putc ('\n');
}

/*
 * Code for MinGW/Cygwin only:
 *
 * When run as:
 *   gdb -args envtool.exe -t
 *
 * the output is something like:
 *
 * test_libssp():
 * 10:    0000: 00 00 00 00 00 00 00 00-00 00                   ..........
 * 10:    0000: 48 65 6C 6C 6F 20 77 6F-72 6C                   Hello worl
 * *** stack smashing detected ***:  terminated
 *
 * Program received signal SIGILL, Illegal instruction.
 * 0x68ac12d4 in ?? () from f:\MinGW32\bin\libssp-0.dll
 * (gdb) bt
 * #0  0x68ac12d4 in ?? () from f:\MinGW32\bin\libssp-0.dll
 * #1  0x68ac132e in libssp-0!__stack_chk_fail () from f:\MinGW32\bin\libssp-0.dll
 * #2  0x0040724f in _fu117____stack_chk_guard () at envtool.c:2518
 * #3  0x004072eb in _fu118____stack_chk_guard () at envtool.c:2552
 * #4  0x0040653d in _fu100____stack_chk_guard () at envtool.c:2081
 * (gdb)
 */
static void test_libssp (void)
{
#if defined(_FORTIFY_SOURCE) && (_FORTIFY_SOURCE > 0)
  static const char buf1[] = "Hello world.\n\n";
  char buf2 [sizeof(buf1)-2] = { 0 };

  C_printf ("~3%s():~0\n", __FUNCTION__);

  hex_dump (&buf1, sizeof(buf1));
  memcpy (buf2, buf1, sizeof(buf1));  /* write beyond buf2[] */

#if 0
  C_printf (buf2);   /* vulnerable data */
  C_flush();
#endif

  hex_dump (&buf2, sizeof(buf2));
  C_putc ('\n');
#endif /* (_FORTIFY_SOURCE > 0) */
}

/*
 * This should run when user-name is "APPVYR-WIN\appveyor".
 * Check if it finds 'cmake.exe' on it's %PATH% which should contain
 * "c:\Program Files (x86)\CMake\bin".
 */
static void test_AppVeyor (void)
{
  const char *cmake = searchpath ("cmake.exe", "PATH");
  int   rc;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  if (!cmake)
     C_printf ("cmake.exe not on %%PATH.\n");
  else
  {
    rc = popen_runf (NULL, "\"%s\" -version > " DEV_NULL, cmake);
    C_printf ("popen_run() reported %d: %s\n", rc, rc == 0 ? cmake : "cmake not found");
  }
}

/*
 * A simple test for ETP searches
 */
static void test_ETP_host (void)
{
  int i, max;

  if (!opt.file_spec)
     opt.file_spec = STRDUP ("*");

  max = smartlist_len (opt.evry_host);
  for (i = 0; i < max; i++)
  {
    const char *host = smartlist_get (opt.evry_host, i);

    C_printf ("~3%s():~0 host %s:\n", __FUNCTION__, host);
    do_check_evry_ept (host);
  }
}

/*
 * A simple test for Python functions
 */
static int test_python_funcs (void)
{
  const command_line *c;
  char *str = NULL;

  if (halt_flag)
     return (1);

  py_test();

  c = &opt.cmd_line;
  if (c->argc0 > 0)
     str = py_execfile ((const char**)(c->argv + c->argc0), FALSE);

  FREE (str);
  return (0);
}

void regex_print (const regex_t *re, const regmatch_t *rm, const char *str)
{
  size_t i, j;

  C_puts ("sub-expr: ");
  for (i = 0; i < re->re_nsub; i++, rm++)
  {
    for (j = 0; j < strlen(str); j++)
    {
      if (j >= (size_t)rm->rm_so && j <= (size_t)rm->rm_eo)
           C_printf ("~5%c", str[j]);
      else C_printf ("~0%c", str[j]);
    }
  }
  if (i == 0)
       C_puts ("None\n");
  else C_puts ("~0\n");
}

/**
 * Expand and check a single env-var for missing directories
 * and trailing/leading white space.
 *
 * \eg{.}
 * ```
 *   set LIB=c:\foo1\lib ;c:\foo2\lib;  ^
 *           c:\foo3\lib;
 * ```
 *
 * would leave trailing white-space in `c:\foo1\lib ;` and `c:\foo2\lib;  ` <br>
 * and leading white-space in `        c:\foo3\lib`.
 *
 * \param[in]     env      the environment variable to check.
 * \param[out]    num      the number of elements in the '*env' value.
 * \param[in,out] status   the buffer to receive the state of the check.
 * \param[in]     status_sz the size of the above buffer.
 *
 * Quit the below for-loop on the first error and store the error in `status`.
 * (unless in verbose-mode; `opt.verbose`).
 */
static void check_env_val (const char *env, int *num, char *status, size_t status_sz)
{
  smartlist_t                  *list = NULL;
  int                           i, errors, max = 0;
  int                           save  = opt.conv_cygdrive;
  char                         *value = getenv_expand (env);
  const struct directory_array *arr;

  status[0] = '\0';
  *num = 0;

  if (value)
  {
#ifdef __CYGWIN__
    int   needed = cygwin_conv_path_list (CCP_WIN_A_TO_POSIX, value, NULL, 0);
    char *cyg_value = alloca (needed+1);

    cygwin_conv_path_list (CCP_WIN_A_TO_POSIX, value, cyg_value, needed+1);
    opt.conv_cygdrive = 0;
    path_separator = ':';
    FREE (value);
    value = cyg_value;
#endif

    list = split_env_var (env, value);
    *num = max = smartlist_len (list);
  }

  for (i = errors = 0; i < max; i++)
  {
    char fbuf [_MAX_PATH];
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

    slashify2 (fbuf, arr->dir, opt.show_unix_paths ? '/' : '\\');

    if (start > arr->dir)
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
      snprintf (status, status_sz, "~5Missing dir~0: ~3\"%s\"~0", fbuf);
      errors++;
    }
    else if (!arr->is_cwd && dir_is_empty(fbuf))
    {
      snprintf (status, status_sz, "~5Empty dir~0: ~3\"%s\"~0", fbuf);
      errors++;
    }

    if (opt.verbose)
    {
      C_printf ("     [%2d]: ~6%s", i, fbuf);
      C_puts ("~0\n");
    }
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

#ifndef __CYGWIN__
  FREE (value);
#endif

  free_dir_array();
  opt.conv_cygdrive = save;
  path_separator = ';';
}

/**
 * Do a simple check of an environment varable from <br>
 * `HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Environment`.
 */
static void check_env_val_reg (const smartlist_t *list, const char *env_name)
{
  int   i, raw, errors = 0, max = 0;
  int   indent = sizeof("Checking");
  const struct directory_array *arr;

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
      C_printf ("   [%2d]: ~6", i);
      raw = C_setraw (1);     /* In case 'fbuf' contains a "~". */
      C_puts (fbuf);

      attr = GetFileAttributes (arr->dir);
      if ((attr != INVALID_FILE_ATTRIBUTES) &&
          (attr & FILE_ATTRIBUTE_REPARSE_POINT) &&
          get_disk_type(arr->dir[0]) != DRIVE_REMOTE &&
          get_reparse_point (arr->dir, link, TRUE))
        C_printf ("\n      -> %s", _fix_drive(link));

      C_setraw (raw);
      C_puts ("~0\n");
    }

    if (!arr->exist)
    {
      C_printf ("%*c~5Missing dir~0: ~3%s~0\n", indent, ' ', fbuf);
      errors++;
    }
    else if (!arr->is_cwd && dir_is_empty(fbuf))
    {
      C_printf ("%*c~5Empty dir~0: ~3\"%s\"~0", indent, ' ', fbuf);
      errors++;
    }
  }

  C_printf ("%*c", indent, ' ');

  if (max == 0)
     C_puts ("~5Empty~0, ");
  else if (errors == 0)
     C_puts ("~2OK~0, ");

  C_printf ("~6%2d~0 elements\n", max);
  free_dir_array();
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
  int i, errors, max, raw, indent = sizeof("Checking");

  C_printf ("Checking ~3%s\\%s~0:\n", reg_top_key_name(key), REG_APP_PATH);

  build_reg_array_app_path (key);
  sort_reg_array();

  max  = smartlist_len (reg_array);
  for (i = errors = 0; i < max; i++)
  {
    const struct registry_array *arr = smartlist_get (reg_array, i);
    char  fqfn [_MAX_PATH];
    char  fbuf [_MAX_PATH];

    slashify2 (fbuf, arr->path, opt.show_unix_paths ? '/' : '\\');

    if (opt.verbose)
    {
      char  fbuf2 [_MAX_PATH];
      char *fname;

      C_printf ("   [%2d]: ~6", i);
      raw = C_setraw (1);     /* In case 'fqfn' contains a "~". */

      snprintf (fbuf2, sizeof(fbuf2), "%s\\%s", arr->path, arr->fname);
      fname = fbuf2;

      if (get_actual_filename(&fname, FALSE))
      {
        C_printf ("%s", fname);
        FREE (fname);
      }
      else
        C_printf ("%s", fname);

      C_setraw (raw);
      C_puts ("~0\n");
    }

    if (!is_directory(fbuf) && !cfg_ignore_lookup("[Registry]",fbuf))
    {
      C_printf ("%*c~5Missing dir~0: ~3%s~0\n", indent, ' ', fbuf);
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
     C_puts ("~2OK~0,             ");
  else
     C_puts ("~5Error~0,          ");

  C_printf ("~6%2d~0 elements\n", max);
  free_reg_array();
}

/**
 * The handler for mode `"--check"`.
 * Check the Registry keys even if `opt.no_app_path` is set.
 */
static int do_check (void)
{
  static const char *envs[] = {
                    "PATH",
                    "LIB",
                    "LIBRARY_PATH",
                    "INCLUDE",
                    "C_INCLUDE_PATH",
                    "CPLUS_INCLUDE_PATH",
                    "MANPATH",
                    "PKG_CONFIG_PATH",
                    "PYTHONPATH",
                    "CMAKE_MODULE_PATH",
                    "CLASSPATH",  /* No support for these. But do it anyway. */
                    "GOPATH",
                    "FOO",        /* Check that non-existing env-vars are also checked */
                    NULL
                   };
  const char *env;
  char       *sys_env_path = NULL;
  char       *sys_env_inc  = NULL;
  char       *sys_env_lib  = NULL;
  char        status [100+_MAX_PATH];
  int         i, save;
  int         num, indent;

  /* Do not implicit add current directory in these searches.
   */
  save = opt.no_cwd;
  opt.no_cwd = 1;

  for (i = 0, env = envs[0]; i < DIM(envs) && env; env = envs[++i])
  {
    indent = (int) (sizeof("CPLUS_INCLUDE_PATH") - strlen(env));

    C_printf ("Checking ~3%%%s%%~0:%*c", env, indent, ' ');
    if (opt.verbose)
       C_putc ('\n');

    check_env_val (env, &num, status, sizeof(status));
    C_printf ("%2d~0 elements, %s\n", num, status);
    if (opt.verbose)
       C_putc ('\n');
  }

  C_putc ('\n');
  check_app_paths (HKEY_CURRENT_USER);
  if (opt.verbose)
     C_putc ('\n');

  check_app_paths (HKEY_LOCAL_MACHINE);
  if (opt.verbose)
     C_putc ('\n');

  opt.no_cwd = save;

  scan_reg_environment (HKEY_LOCAL_MACHINE,
                        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                        &sys_env_path, &sys_env_inc, &sys_env_lib);

  check_env_val_reg (split_env_var(NULL, sys_env_path), "PATH");
  check_env_val_reg (split_env_var(NULL, sys_env_inc), "INCLUDE");
  check_env_val_reg (split_env_var(NULL, sys_env_lib), "LIB");

  /**\todo
   *
   * Iterate over these environment sources:
   * ```
   *   _environ[]
   *   HKU\.DEFAULT\Environment
   *   HKLM\System\CurrentControlSet\Control\Session Manager\Environment
   *   HKCU\Environment
   *   HKCU\Volatile Environment
   * ```
   *
   * If there is a mismatch in a value in the above list, print a warning.
   *
   * And check for missing directories in above 'HKx' keys with values that looks
   * like directories. Like 'Path', 'TEMP'
   */

  FREE (sys_env_path);
  FREE (sys_env_inc);
  FREE (sys_env_lib);
  return (0);
}

#ifdef NOT_USED
int test_str_shorten (void)
{
  const char *res, *str = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  int   i;

  C_printf ("~3%s():~0\n", __FUNCTION__);
  for (i = 2; i < strlen(str); i++)
  {
    res = str_shorten (str, i);
    C_printf ("%2d: %-26s %s~0\n", i, res, i == strlen(res) ? "~2OK" : "~5Error");
  }
  return (0);
}
#endif

/*
 * The handler for option "-t" / "--test".
 */
static int do_tests (void)
{
  int save;

#if 0
  {
    opt.debug = 1;
    str_endswith ("foo-bar-fuz", "-fuz");
    str_endswith ("c:\\Program Filer (x86)\\Python\\include", "\\include");
    str_endswith ("c:\\Program Filer (x86)\\Python\\lib", "\\include");
    return (0);
  }
#endif

  if (opt.do_evry && opt.evry_host)
  {
    test_ETP_host();
    return (0);
  }

  if (opt.do_python)
     return test_python_funcs();

#ifdef NOT_USED
  return test_str_shorten();
#endif

  test_split_env ("PATH");
  test_split_env ("MANPATH");

#ifdef __CYGWIN__
  test_split_env_cygwin ("PATH");
  test_posix_to_win_cygwin();
#endif

#ifdef __WATCOMC__
  test_split_env ("NT_INCLUDE");
#else
  test_split_env ("LIB");
  test_split_env ("INCLUDE");
#endif

  save = opt.no_cwd;
  opt.no_cwd = 0;
#ifdef __CYGWIN__
  setenv ("FOO", "/cygdrive/c", 1);
#else
  putenv ("FOO=c:\\");
#endif

  test_split_env ("FOO");
  opt.no_cwd = save;

  test_searchpath();
  test_fnmatch();
  test_PE_wintrust();
  test_slashify();
  test_fix_path();
  test_disk_ready();
  test_SHGetFolderPath();
  test_ReparsePoints();

  if (!stricmp(get_user_name(),"APPVYR-WIN\\appveyor"))
     test_AppVeyor();

  test_auth();

  test_libssp();

#if defined(_MSC_VER) && !defined(_DEBUG)
  find_vstudio_init();
#endif

  return (0);
}

#if !defined(__DOXYGEN__)
#if defined(__MINGW32__)
  #define CFLAGS   "cflags_MinGW.h"
  #define LDFLAGS  "ldflags_MinGW.h"

#elif defined(__POCC__)
  #define CFLAGS   "cflags_PellesC.h"
  #define LDFLAGS  "ldflags_PellesC.h"

#elif defined(__clang__)
  #define CFLAGS   "cflags_clang.h"
  #define LDFLAGS  "ldflags_clang.h"

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
#endif  /* !__DOXYGEN__ */

static void print_build_cflags (void)
{
#if defined(CFLAGS) && !defined(DOING_MAKE_DEPEND)
  #include CFLAGS
  C_puts ("\n    ");
  C_puts_long_line (cflags, 4);
#else
  C_puts (" Unknown\n");
#endif
}

static void print_build_ldflags (void)
{
#if defined(LDFLAGS) && !defined(DOING_MAKE_DEPEND)
  #include LDFLAGS
  C_puts ("\n    ");
  C_puts_long_line (ldflags, 4);
#else
  C_puts (" Unknown\n");
#endif
}

