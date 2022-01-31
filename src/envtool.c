/**
 * \file    envtool.c
 * \ingroup Envtool
 * \brief
 *   A tool to search and check various environment variables
 *   for correctness and check where a specific file is in corresponding
 *   environment variable.
 *
 *  \Eg{1} `envtool --path notepad.exe` first checks the `%PATH%` env-var
 *          for consistency (reports missing directories in `%PATH%`) and
 *          prints all the locations of `notepad.exe`.
 *
 *  \Eg{2} `envtool --inc afxwin.h` first checks the `%INCLUDE%` env-var
 *          for consistency (reports missing directories in `%INCLUDE%`) and
 *          prints all the locations of `afxwin.h`.
 *
 * By Gisle Vanem <gvanem@yahoo.no> 2011 - 2020.
 *
 * Functions fnmatch() was taken from djgpp and modified:
 *   Copyright (C) 1995 DJ Delorie, see COPYING.DJ for details
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <locale.h>

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
#include "cache.h"
#include "dirlist.h"
#include "sort.h"
#include "vcpkg.h"
#include "lua.h"
#include "report.h"
#include "pkg-config.h"
#include "get_file_assoc.h"
#include "cfg_file.h"
#include "cmake.h"
#include "tests.h"
#include "description.h"
#include "compiler.h"

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

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

char *program_name = NULL;   /**< For getopt_long.c */

BOOL have_sys_native_dir = FALSE;
BOOL have_sys_wow64_dir  = FALSE;

/**
 * All program options are kept here.
 */
struct prog_options opt;

/**
 * A list of 'struct directory_array'
 */
static smartlist_t *dir_array;

/**
 * A list of 'struct registry_array'
 */
static smartlist_t *reg_array;

/**
 * Values of system directories.
 */
char   sys_dir        [_MAX_PATH];  /**< E.g. `"c:\Windows\System32"` */
char   sys_native_dir [_MAX_PATH];  /**< E.g. `"c:\Windows\sysnative"`. Not for WIN64 */
char   sys_wow64_dir  [_MAX_PATH];  /**< E.g. `"c:\Windows\SysWOW64"`. Not for WIN64 */
char   current_dir    [_MAX_PATH];
int    path_separator = ';';

static char *who_am_I = (char*) "envtool";
static char *system_env_path = NULL;
static char *system_env_lib  = NULL;
static char *system_env_inc  = NULL;

static char *user_env_path   = NULL;
static char *user_env_lib    = NULL;
static char *user_env_inc    = NULL;

static regex_t    re_hnd;         /* regex handle/state */
static regmatch_t re_matches[3];  /* regex sub-expressions */
static int        re_err;         /* last regex error-code */
static char       re_errbuf[300]; /* regex error-buffer */
static int        re_alloc;       /* the above `re_hnd` was allocated */

volatile int halt_flag;

/** Get the bitness (32/64-bit) of the EveryThing program.
 */
static enum Bitness evry_bitness = bit_unknown;
static int  evry_service = -1;
static void get_evry_bitness (HWND wnd);

static void  usage (const char *fmt, ...) ATTR_PRINTF(1,2);
static int   do_check (void);

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
 *   + on date/time.
 *   + on filename.
 *   + on file-size.
 *
 * \todo Add a `--check` option for 64-bit Windows to check that all .DLLs in:<br>
 *           + `"%SystemRoot%\System32"` are 64-bit and
 *           + `"%SystemRoot%\SysWOW64"` are 32-bit. <br>
 *       \eg \code
 *         pedump %SystemRoot%\SysWOW64\*.dll | grep 'Machine: '
 *         Machine:                      014C (i386)
 *         Machine:                      014C (i386)
 *         ....
 *        \endcode
 *
 *       Also check their Wintrust signature status and version information.
 */

/**
 * Show some version details for the EveryThing program.
 * Called on `FindWindow (EVERYTHING_IPC_WNDCLASS)` success.
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

  if (evry_service == -1)
  {
    evry_service = SendMessage (wnd, EVERYTHING_WM_IPC, EVERYTHING_IPC_IS_SERVICE, 0);
    TRACE (1, "evry_service: %d.\n", evry_service);
  }

  C_printf ("  Everything search engine ver. %u.%u.%u.%u%s (c)"
            " David Carpenter; ~6https://www.voidtools.com/~0\n",
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

  TRACE (2, "e_pid: %lu, e_tid: %lu.\n", (unsigned long)e_pid, (unsigned long)e_tid);

  hnd = OpenProcess (PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, e_pid);
  if (!hnd)
     return;

  if (get_module_filename_ex(hnd, fname) && check_if_PE(fname, &bits))
     evry_bitness = bits;

  CloseHandle (hnd);
  TRACE (2, "fname: %s, evry_bitness: %d.\n", fname, evry_bitness);
}

/**
 * Show version information for various external programs:
 *  + `Cmake.exe`
 *  + `Python*.exe`
 *  + `pkg-config.exe`
 *  + `vcpkg.exe`
 *  + `lua.exe`
 */
static ver_data *build_ver_data (void)
{
  static ver_data data [VER_MAX];
  char slash = (opt.show_unix_paths ? '/' : '\\');

  memset (&data, '\0', sizeof(data));

  data [VER_CMAKE]     .slash = slash;
  data [VER_PYTHON]    .slash = slash;
  data [VER_PKG_CONFIG].slash = slash;
  data [VER_VCPKG]     .slash = slash;
  data [VER_LUA]       .slash = slash;

  data [VER_CMAKE]     .found_fmt  = "  Cmake %u.%u.%u detected";
  data [VER_PYTHON]    .found_fmt  = "  Python %u.%u.%u detected";
  data [VER_PKG_CONFIG].found_fmt  = "  pkg-config %u.%u detected";
  data [VER_VCPKG]     .found_fmt  = "  vcpkg %u.%u.%u detected";
  data [VER_LUA]       .found_fmt  = "  Lua %u.%u.%u detected";

  data [VER_CMAKE]     .not_found = "Cmake ~5not~0 found";
  data [VER_PYTHON]    .not_found = "Python ~5not~0 found";
  data [VER_PKG_CONFIG].not_found = "pkg-config ~5not~0 found";
  data [VER_VCPKG]     .not_found = "vcpkg ~5not~0 found";
  data [VER_LUA]       .not_found = "Lua ~5not~0 found";

  data [VER_CMAKE]     .get_info = cmake_get_info;
  data [VER_PYTHON]    .get_info = py_get_info;
  data [VER_PKG_CONFIG].get_info = pkg_config_get_info;
  data [VER_VCPKG]     .get_info = vcpkg_get_info;
  data [VER_LUA]       .get_info = lua_get_info;

  data [VER_VCPKG]     .extras = vcpkg_extras;
  data [VER_PKG_CONFIG].extras = pkg_config_extras;
  return (data);
}

static void show_ext_versions (void)
{
  int       i;
  int       pad_len = sizeof("  pkg-config 9.99 detected");
  ver_data *v, *v_start = build_ver_data();

  if (!stricmp(lua_get_exe(), "luajit.exe"))
  {
    v_start [VER_LUA].found_fmt = "  LuaJIT %u.%u.%u detected";
    v_start [VER_LUA].not_found = "LuaJIT ~5not~0 found";
  }

  for (v = v_start, i = 0; i < VER_MAX; i++, v++)
  {
    if ((*v->get_info) (&v->exe, &v->version))
    {
      v->len = snprintf (v->found, sizeof(v->found), v->found_fmt, v->version.val_1, v->version.val_2, v->version.val_3);
      if (v->len > pad_len)
         pad_len = v->len;
    }
  }
  for (v = v_start, i = 0; i < VER_MAX; i++, v++)
  {
    if (v->exe)
    {
      if (v->extras)
           (*v->extras) (v, pad_len);
      else C_printf ("%-*s -> ~6%s~0\n", pad_len, v->found, slashify(v->exe, v->slash));
    }
    else
      C_printf ("  %s.\n", v->not_found);
    FREE (v->exe);
  }
}

/**
 * Returns TRUE if program was compiled with
 * "AddressSanitizer" support.
 */
static BOOL asan_enabled (void)
{
#ifdef USE_ASAN
  return (TRUE);
#else
  return (FALSE);
#endif
}

/**
 * Handler for `envtool -V..`:
 * + Show some basic version information:    option `-V`.
 * + Show more detailed version information: option `-VV`.
 */
static int show_version (void)
{
  HWND wnd;
  BOOL wow64 = is_wow64_active();

  C_printf ("%s.\n"
            "  Version ~3%s ~1(%s, %s%s%s)~0 by %s.\n"
            "  Hosted at: ~6%s~0\n",
            who_am_I, VER_STRING, compiler_version(), WIN_VERSTR,
            wow64 ? ", ~1WOW64" : "", asan_enabled() ? ", ASAN" : "",
            AUTHOR_STR, GITHUB_STR);

  wnd = FindWindow (EVERYTHING_IPC_WNDCLASS, 0);
  if (wnd)
  {
    struct ver_info evry_ver;

    if (get_evry_version(wnd, &evry_ver))
         show_evry_version (wnd, &evry_ver);
    else C_printf ("  Everything search engine not responding.\n");
  }
  else
    C_printf ("  Everything search engine not found.\n");

  C_puts ("  Checking Python programs...");
  C_flush();
  py_init();

  if (_isatty(STDOUT_FILENO))
       C_printf ("\r                             \r");
  else C_putc ('\n');

  show_ext_versions();

  if (opt.do_version >= 2)
  {
    const char *OS_name = os_name();

    if (!strncmp(OS_name, "Win-10", 6))
    {
      OS_name = os_full_version();
      C_printf ("  OS-version: Win-10; %s, %s bits.\n", os_full_version(), os_bits());
    }
    else if (!strncmp(OS_name, "Win-11", 6))
    {
      OS_name = os_full_version();
      C_printf ("  OS-version: Win-11; %s, %s bits.\n", os_full_version(), os_bits());
    }
    else
      C_printf ("  OS-version: %s (%s bits).\n", OS_name, os_bits());

    C_printf ("  User-name:  \"%s\", %slogged in as Admin.\n", get_user_name(), is_user_admin() ? "" : "not ");
    C_printf ("  ConEmu:     %sdetected.\n", opt.under_conemu ? "" : "not ");
    C_puts   ("  CPU-Info:   ");
    print_core_temp_info();

    C_puts ("\n  Compile command and ~3CFLAGS~0:");
    compiler_print_build_cflags();

    C_puts ("\n  Link command and ~3LDFLAGS~0:");
    compiler_print_build_ldflags();

    C_printf ("\n  Supported compilers on ~3PATH~0:\n");
    compiler_init (TRUE, opt.do_version >= 3);

    if (opt.do_version >= 3)
    {
      unsigned num;

      C_printf ("\n  Pythons on ~3PATH~0:");
      py_searchpaths();

      num = pkg_config_list_installed();
      TRACE (2, "pkg_config_list_installed(): %u.\n", num);

      num = vcpkg_list_installed (opt.do_version >= 3);
      TRACE (2, "vcpkg_list_installed(): %u.\n", num);
    }
  }
  return (0);
}

/**
 * Printer for illegal program usage. <br>
 * Call `exit(-1)` when done.
 */
static void usage (const char *fmt, ...)
{
  va_list args;

  C_setraw (1);   /* since 'C_init()' was probably NOT called */
  va_start (args, fmt);
  C_vprintf (fmt, args);
  va_end (args);
  exit (-1);
}

/**
 * Print a help-page of all program options.
 */
static int show_help (void)
{
  #define PFX_GCC  "~4<prefix>-~3gcc"
  #define PFX_GPP  "~4<prefix>-~3g++"

  const char **py = py_get_variants();
  const char  *pfx;

  C_printf ("Environment check & search tool.\n\n"
            "Usage: %s ~6[options] <--mode> <file-spec>~0\n", who_am_I);

  C_puts ("  ~6<--mode>~0 can be at least one of these:\n"
          "    ~6--cmake~0        check and search in ~3%CMAKE_MODULE_PATH%~0 and it's built-in module-path.\n"
          "    ~6--evry~0[=~3host~0]  check and search in the ~6EveryThing database~0.     ~2[1]~0\n"
          "    ~6--inc~0          check and search in ~3%INCLUDE%~0.                   ~2[2]~0\n"
          "    ~6--lib~0          check and search in ~3%LIB%~0 and ~3%LIBRARY_PATH%~0.    ~2[2]~0\n"
          "    ~6--lua~0          check and search in ~3%LUA_PATH%~0 and ~3%LUA_CPATH%~0.\n"
          "    ~6--man~0          check and search in ~3%MANPATH%~0.\n"
          "    ~6--path~0         check and search in ~3%PATH%~0.\n"
          "    ~6--pkg~0          check and search in ~3%PKG_CONFIG_PATH%~0.\n"
          "    ~6--python~0[=~3X~0]   check and search in ~3%PYTHONPATH%~0 and ~3sys.path[]~0. ~2[3]~0\n"
          "    ~6--vcpkg~0[=~3all~0]~0  check and search for ~3VCPKG~0 packages.             ~2[4]~0\n"
          "    ~6--check~0        check for missing directories in ~4all~0 supported environment variables\n"
          "                   and missing files in ~3HKx\\Microsoft\\Windows\\CurrentVersion\\App Paths~0 keys.\n"
          "                   Add ~6-v~0 / ~6--verbose~0 for more checks.\n");

  C_puts ("  ~6[options]~0\n"
          "    ~6--descr~0        show 4NT/TCC file-description.\n"
          "    ~6--grep~0=~3content~0 search found file(s) for ~3content~0 also.\n"
          "    ~6--no-ansi~0      don't print colours using ANSI sequences (effective for CygWin/ConEmu only).\n"
          "    ~6--no-app~0       don't scan ~3HKCU\\" REG_APP_PATH "~0 and\n"
          "                              ~3HKLM\\" REG_APP_PATH "~0.\n"
          "    ~6--no-colour~0    don't print using colours.\n"
          "    ~6--no-cwd~0       don't add current directory to the search-paths.\n"
          "    ~6--no-~3<comp>~0    in ~6--include~0 or ~6--lib~0 mode, do not check for compiler:\n"
          "                   ~3comp={" PFX_GCC ", " PFX_GPP ", borland, clang, intel, msvc, watcom}~0.\n"
          "    ~6--no-prefix~0    don't check any ~4<prefix>~0-ed ~6gcc/g++~0 programs.    ~2[2]~0\n"
          "    ~6--no-sys~0       don't scan ~3HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment~0.\n"
          "    ~6--no-usr~0       don't scan ~3HKCU\\Environment~0.\n"
          "    ~6--owner~0        shown owner of the file (shows all owners).\n"
          "    ~6--owner~0=~3spec~0   shown only files/directories matching owner ~3spec~0.\n"
          "    ~6--owner~0=~3!spec~0  shown only files/directories ~4not~0 matching owner ~3spec~0.\n"
          "    ~6--pe~0           print checksum, version-info and signing status for PE-files.\n");

  C_puts ("    ~6--32~0           report only 32-bit PE-files in ~6--path~0, ~6--vcpkg~0 or ~6--evry~0 modes.\n");
  C_puts ("    ~6--64~0           report only 64-bit PE-files in ~6--path~0, ~6--vcpkg~0 or ~6--evry~0 modes.\n");

  C_puts ("    ~6--signed~0       check for ~4all~0 digital signature with the ~6--pe~0 option.\n"
          "    ~6--signed=0~0     report only ~4PE~0-files files that are ~6unsigned~0.\n"
          "    ~6--signed=1~0     report only ~4PE~0-files files that are ~6signed~0.\n"
          "    ~6-c~0, ~6--case~0     be case-sensitive.\n"
          "    ~6-d~0, ~6--debug~0    set debug level (level 2, ~3-dd~0 sets ~3PYTHONVERBOSE=1~0 in ~6--python~0 mode).\n"
          "    ~6-D~0, ~6--dir~0      looks only for directories matching ~6<file-spec>~0.\n"
          "    ~6-k~0, ~6--keep~0     keep temporary files used in ~6--python~0 mode.\n"
          "    ~6-o~0, ~6--only~0     show only files that matches the ~6--grep~0 ~3content~0 \n");

  C_printf ("    ~6-r~0, ~6--regex~0    enable Regular Expressions in all ~6<--mode>~0 searches.\n"
            "    ~6-s~0, ~6--size~0     show size of files or directories found.\n"
            "    ~6-S~3x~0, ~6--sort ~3x~0  sort files on a combination of ~3x=[%s]~0 (not yet effective).\n",
            get_sort_methods());

  C_puts ("    ~6-q~0, ~6--quiet~0    disable warnings.\n"
          "    ~6-t~0, ~6--test~0     do some internal tests. Use ~6--owner~0, ~6--py~0 or ~6--evry~0 for extra tests  ~2[3]~0.\n"
          "    ~6-T~0             show file times in sortable decimal format. E.g. \"~620121107.180658~0\".\n"
          "    ~6-u~0             show all paths on Unix format: \"~3c:/ProgramFiles/~0\".\n"
          "    ~6-v~0, ~6--verbose~0  increase verbosity level.\n"
          "    ~6-V~0, ~6--version~0  show program version information. ~6-VV~0 and ~6-VVV~0  prints more info.\n"
          "    ~6-h~0, ~6--help~0     show this help.\n\n");

  C_puts ("    ~6--evry~0 remote FTP options:\n"
          "      ~6-H~0, ~6--host~0    hostname/IPv4-address. Can be used multiple times.\n"
          "                    alternative syntax is ~6--evry=<host>~0.\n");

  C_puts ("\n"
          "  ~2[1]~0 The ~6--evry~0 option requires that the Everything search engine is installed.\n"
          "      Ref. ~3https://www.voidtools.com/support/everything/~0\n"
          "      For remote FTP search(es) (~6--evry=[host-name|IP-address]~0), a user/password\n"
          "      should be specified in your ~6%APPDATA%/.netrc~0 or ~6%APPDATA%/.authinfo~0 files or\n"
          "      you can use the \"~6user:passwd@host_or_IP-address:~3port~0\" syntax.\n"
          "\n"
          "      To perform raw searches, append a modifier like:\n"
          "        envtool ~6--evry~0 ~3*.exe~0 rc:today                     - find all ~3*.exe~0 files changed today.\n"
          "        envtool ~6--evry~0 ~3*.mp3~0 title:Hello                  - find all ~3*.mp3~0 files with a title starting with Hello.\n"
          "        envtool ~6--evry~0 ~3f*~0 empty:                          - find all empty directories matching ~3f*~0.\n"
          "        envtool ~6--evry~0 ~3Makefile.am~0 content:pod2man        - find all ~3Makefile.am~0 containing ~3pod2man~0.\n"
          "        envtool ~6--evry~0 ~3M*.mp3~0 artist:Madonna \"year:<2002\" - find all Madonna ~3M*.mp3~0 titles issued prior to 2002.\n"
          "\n"
          "      Ref: https://www.voidtools.com/support/everything/recent_changes/\n"
          "           https://www.voidtools.com/support/everything/searching/#functions\n"

          "\n"
          "  ~2[2]~0 Unless ~6--no-prefix~0 is used, the ~3%C_INCLUDE_PATH%~0, ~3%CPLUS_INCLUDE_PATH%~0 and\n"
          "      ~3%LIBRARY_PATH%~0 are also used by spawning " PFX_GCC "~0 and " PFX_GPP "~0.\n"
          "      These ~4<prefix>~0-es are built-in:");

  for (pfx = compiler_GCC_prefix_first(); pfx; )
  {
    if (*pfx == '\0')
    {
      pfx = compiler_GCC_prefix_next();
      continue;
    }

    C_printf (" ~6%.*s~0", (int)(strlen(pfx)-1), pfx);
    pfx = compiler_GCC_prefix_next();
    if (pfx)
         C_putc (',');
    else C_putc ('.');
  }

  C_puts ("\n\n"
          "  ~2[3]~0 The ~6--python~0 option can be detailed further with ~3=X~0:\n");

  for (; *py; py++)
  {
    unsigned v = py_variant_value (*py, NULL);

    if (v == ALL_PYTHONS)
         C_printf ("      ~6%-6s~0 use all of the below Python programs (when found).\n", *py);
    else C_printf ("      ~6%-6s~0 use a %s program only.\n", *py, py_variant_name(v));
  }
  C_puts ("             otherwise use only first Python found on PATH (i.e. the default).\n"
          "      A ~6--py=~0[=~3X~0] ~6--test file.py~0 will execute ~6file.py~0 at end of tests.\n");

  C_puts ("\n  ~2[4]~0 This needs the ~6vcpkg.exe~0 program on ~3%PATH%~0 with a set of ~6ports~0 and ~6CONTROL~0 files.\n"
          "      Used as ~6--vcpkg=all~0, it will list install status of any packages matching ~6<file-spec>~0.\n"
          "      ~3Ref: https://github.com/Microsoft/vcpkg.git~0\n");

  C_puts ("\n"
          "  Notes:\n"
          "    Option ~6-c~0 applies both to the ~6<file-spec>~0 and the ~6--grep=content~0.\n"
          "    ~6<file-spec>~0 accepts Posix ranges. E.g. \"~6[a-f]*.txt~0\".\n"
          "    ~6<file-spec>~0 matches both files and directories. If ~6-D~0/~6--dir~0 is used, only\n"
          "                matching directories are reported.\n"
          "    Quote argument if it contains a shell-character [~6^&%~0]."
          " E.g. use ~6--regex \"^foo%%\\.exe$\"~0\n"
          "    Commonly used options can be set in ~3%ENVTOOL_OPTIONS%~0.\n");
  return (0);
}

smartlist_t *dir_array_head (void)
{
  return (dir_array);
}

/**
 * Add (or insert at position 0) the `dir` to the `dir_array` smartlist.
 *
 * \param[in] dir         the directory to add to the smartlist.
 * \param[in] is_cwd      TRUE if `dir` is the current working directory.
 * \param[in] insert_at0  insert the new `d` element at position 0 in `dir_array`.
 *
 * Since this function could be called with a `dir` from `expand_env_var()`,
 * we check here if it returned with no `%`.
 */
static struct directory_array *dir_array_add_or_insert (const char *dir, BOOL is_cwd, BOOL  insert_at0)
{
  struct directory_array *d = CALLOC (1, sizeof(*d));
  struct stat st;
  int    max, i, exp_ok = (dir && *dir != '%');
  BOOL   exists = FALSE;
  BOOL   is_dir = FALSE;

  if (!opt.lua_mode && safe_stat(dir, &st, NULL) == 0)
     is_dir = exists = _S_ISDIR (st.st_mode);

  d->cyg_dir  = NULL;
  d->dir      = STRDUP (dir);
  d->exp_ok   = exp_ok;
  d->exist    = exp_ok && exists;
  d->is_dir   = is_dir;
  d->is_cwd   = is_cwd;
#if 0
  d->file     = file;
  d->line     = line;
#endif

  /* Can we have >1 native dirs?
   *
   * Use `stricmp()` since on MinGW-w64, a native directory becomes partially
   * uppercase for some strange reason.
   * E.g. "C:\WINDOWS\sysnative".
   */
  d->is_native = (stricmp(dir, sys_native_dir) == 0);

  /* Some issue with MinGW's `stat()` on a sys_native directory.
   * Even though `GetFileAttributes()` in `safe_stat()` says it's a
   * directory, `stat()` doesn't report it as such. So just fake it.
   */
#if defined(__MINGW32__)
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
    TRACE (2, "Ignore native dir '%s'.\n", dir);
  }
#else
  if (d->is_native && !have_sys_native_dir)
     TRACE (2, "Native dir '%s' doesn't exist.\n", dir);
  else if (!d->exist)
  {
    /* An issue with 'stat()' on MSVC-2013. So just pretend it
     * exist if it has a sys-native prefix.
     */
    if (d->is_native && have_sys_native_dir)
         d->exist = d->is_dir = TRUE;
    else TRACE (2, "'%s' doesn't exist.\n", dir);
  }
#endif

#if defined(__CYGWIN__)
  {
    char cyg_dir [_MAX_PATH];
    int  rc = cygwin_conv_path (CCP_WIN_A_TO_POSIX, d->dir, cyg_dir, sizeof(cyg_dir));

    if (rc == 0)
       d->cyg_dir = STRDUP (cyg_dir);
    TRACE (2, "cygwin_conv_path(): rc: %d, '%s'\n", rc, cyg_dir);
  }
#endif

  if (insert_at0)
       smartlist_insert (dir_array, 0, d);
  else smartlist_add (dir_array, d);

  if (is_cwd || !exp_ok)
     return (NULL);

  max = smartlist_len (dir_array);
  for (i = 0; i < max-1; i++)
  {
    const struct directory_array *d2 = smartlist_get (dir_array, i);

    if (str_equal(dir, d2->dir))
       d->num_dup++;
  }
  return (d);
}

struct directory_array *dir_array_add (const char *dir, BOOL is_cwd)
{
  return dir_array_add_or_insert (dir, is_cwd, FALSE);
}

struct directory_array *dir_array_prepend (const char *dir, BOOL is_cwd)
{
  return dir_array_add_or_insert (dir, is_cwd, TRUE);
}

/**
 * `smartlist_wipe()` helper.
 *
 * \param[in] _r  The item in the `reg_array` smartlist to free.
 */
static void _reg_array_free (void *_r)
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
 * \param[in] _d  The item in the `dir_array` smartlist to free.
 */
void dir_array_wiper (void *_d)
{
  struct directory_array *d = (struct directory_array*) _d;
  int    i, max = d->dirent2 ? smartlist_len (d->dirent2) : 0;

  for (i = 0; i < max; i++)
  {
    struct dirent2 *de = smartlist_get (d->dirent2, i);

    FREE (de->d_name);
    FREE (de);
  }
  smartlist_free (d->dirent2);

  FREE (d->dir);
  FREE (d->cyg_dir);
//FREE (d->env_var);
  FREE (d);
}

/**
 * Add file-name elements to the `reg_array` smartlist:
 *  \param[in] key     the key the entry came from: `HKEY_CURRENT_USER` or `HKEY_LOCAL_MACHINE`.
 *  \param[in] fname   the result from `RegEnumKeyEx()`; name of each key.
 *  \param[in] fqfn    the result from `enum_sub_values()`. The Fully Qualified File Name.
 *
 * Note: `basename (fqfn)` may NOT be equal to `fname` (aliasing). That's the reason
 *       we store `real_fname` too.
 */
struct registry_array  *reg_array_add (HKEY key, const char *fname, const char *fqfn)
{
  struct registry_array *reg;
  struct stat  st;
  const  char *base;
  int    rc;

  base = basename (fqfn);
  if (base == fqfn)
  {
    TRACE (1, "fqfn (%s) contains no '\\' or '/'\n", fqfn);
    return (NULL);
  }

  reg = CALLOC (1, sizeof(*reg));
  smartlist_add (reg_array, reg);

  rc = safe_stat (fqfn, &st, NULL);
  reg->mtime      = st.st_mtime;
  reg->fsize      = st.st_size;
  reg->fname      = STRDUP (fname);
  reg->real_fname = STRDUP (base);
  reg->path       = dirname (fqfn);
  reg->exist      = (rc == 0) && FILE_EXISTS (fqfn);
  reg->key        = key;
  _fix_drive (reg->path);
  return (reg);
}

/**
 * Sort the `reg_array` on `path` + `real_fname`.
 */
static int reg_array_compare (const void **_a, const void **_b)
{
  const struct registry_array *a = *_a;
  const struct registry_array *b = *_b;
  char  fqfn_a [_MAX_PATH];
  char  fqfn_b [_MAX_PATH];
  char  slash = (opt.show_unix_paths ? '/' : '\\');

  if (!a->path || !a->real_fname || !b->path || !b->real_fname)
     return (0);

  snprintf (fqfn_a, sizeof(fqfn_a), "%s%c%s", slashify(a->path, slash), slash, a->real_fname);
  snprintf (fqfn_b, sizeof(fqfn_b), "%s%c%s", slashify(b->path, slash), slash, b->real_fname);

  if (opt.case_sensitive)
       return strcmp (fqfn_a, fqfn_b);
  else return stricmp (fqfn_a, fqfn_b);
}

static void reg_array_print (const char *intro)
{
  const struct registry_array *reg;
  int   i, max, slash = (opt.show_unix_paths ? '/' : '\\');

  TRACE (3, intro);

  max = smartlist_len (reg_array);
  for (i = 0; i < max; i++)
  {
    reg = smartlist_get (reg_array, i);
    TRACE (3, "%2d: FQFN: %s%c%s.\n", i, reg->path, slash, reg->real_fname);
  }
}

static void reg_array_sort (void)
{
  reg_array_print ("before smartlist_sort():\n");
  smartlist_sort (reg_array, reg_array_compare);
  reg_array_print ("after smartlist_sort():\n");
}

smartlist_t *reg_array_head (void)
{
  return (reg_array);
}

void reg_array_free (void)
{
  smartlist_wipe (reg_array, _reg_array_free);
}

void dir_array_free (void)
{
  smartlist_wipe (dir_array, dir_array_wiper);
}

/**
 * Check and warn when a component on form `c:\dir with space` is found.
 * I.e. a path without quotes `"c:\dir with space"`.
 */
static void check_component (const char *env_name, char *tok, int is_cwd)
{
  char *p, *end = strchr (tok, '\0');

  if (!opt.quiet)
  {
    p = strchr (tok, ' ');
    if (opt.quotes_warn && p && (*tok != '"' || end[-1] != '"'))
       WARN ("%s: \"%s\" needs to be enclosed in quotes.\n", env_name, tok);

#if !defined(__CYGWIN__)
    /*
     * Check for missing drive-letter (`x:`) in component.
     */
    if (!is_cwd && IS_SLASH(tok[0]) && !str_equal_n(tok, "/cygdrive/", 10))
       WARN ("%s: \"%s\" is missing a drive letter.\n", env_name, tok);
#else
    ARGSUSED (is_cwd);
#endif

    /* Warn on `x:` (a missing trailing slash)
     */
    if (strlen(tok) <= 3 && isalpha((int)tok[0]) && tok[1] == ':' && !IS_SLASH(tok[2]))
       WARN ("%s: Component \"%s\" should be \"%s%c\".\n", env_name, tok, tok, DIR_SEP);
  }

  p = strchr (tok, '%');
  if (p)
    WARN ("%s: unexpanded component \"%s\".\n", env_name, tok);
}

/**
 * Parses an environment string and returns all components as an array of
 * `struct directory_array` pointing into the global `dir_array` smartlist.
 *
 * This works since we handle only one env-var at a time. The `dir_array`
 * gets cleared in `dir_array_free()` first (in case it was used already).
 *
 * Add current working directory first if `opt.no_cwd == 0` and
 * environment variable does NOT starts with "." or ".\".
 *
 * Convert CygWin style paths to Windows paths: `"/cygdrive/x/.."` -> `"x:/.."`.
 */
smartlist_t *split_env_var (const char *env_name, const char *value)
{
  char *tok, *val, *_end;
  int   is_cwd, max, i;
  char  sep [2];
  BOOL  cwd_added = FALSE;

  if (!value)
  {
    TRACE (1, "split_env_var(\"%s\", NULL)' called!\n", env_name);
    return (NULL);
  }

  val = STRDUP (value);  /* Freed before we return */
  dir_array_free();

  if (str_equal_n(val, "/cygdrive/", 10))
  {
    const char *p = strchr (val, ';');

    if (p && !opt.quiet)
       WARN ("%s: Using ';' and \"/cygdrive\" together is suspisious.\n", env_name);

    path_separator = ':';    /* Assume all components are separated by ':' */
  }

  sep[0] = (char) path_separator;
  sep[1] = '\0';

  tok = _strtok_r (val, sep, &_end);
  is_cwd = (!strcmp(val, ".") || !strcmp(val, ".\\") || !strcmp(val, "./"));

  TRACE (1, "'val': \"%s\". 'tok': \"%s\", is_cwd: %d\n", val, tok, is_cwd);

 /*
  * If `val` doesn't start with ".\" or "./", we should possibly add that
  * first since the search along e.g. %LIB% will include the current
  * directory (cwd) in the search implicitly. This is not always the case for
  * all `env` variables. E.g. Gnu-C's preprocessor doesn't include "." in
  * the %C_INCLUDE_PATH% by default.
  */
  i = 0;
  if (!opt.no_cwd && !is_cwd)
  {
    dir_array_add (current_dir, TRUE);
    cwd_added = TRUE;
  }

  max = INT_MAX;

  for ( ; i < max && tok; i++)
  {
    /* Remove trailing `\\`, `/` or `\\"` from environment component
     * unless it's a simple `"c:\"`.
     */
    char *end = strchr (tok, '\0');

    if (end > tok+3)
    {
      if (end[-1] == '\\' || end[-1] == '/')
        end[-1] = '\0';
      else if (end[-2] == '\\' && end[-1] == '"')
        end[-2] = '\0';
    }

    check_component (env_name, tok, is_cwd);

    if (*tok == '"' && end[-1] == '"')   /* Remove quotes */
    {
      tok++;
      end[-1] = '\0';
    }

    /* _stati64(".") doesn't work. Hence turn "." into `current_dir`.
     */
    is_cwd = (!strcmp(tok, ".") || !strcmp(tok, ".\\") || !strcmp(tok, "./"));
    if (is_cwd)
    {
      if (i > 0 && !opt.under_conemu)
         WARN ("Having \"%s\" not first in \"%s\" is asking for trouble.\n", tok, env_name);
      tok = current_dir;
    }
#if !defined(__CYGWIN__)
    else if (strlen(tok) >= 12 && str_equal_n(tok, "/cygdrive/", 10))
    {
      static char buf [_MAX_PATH];  /* static since it could be used after we return */

      snprintf (buf, sizeof(buf), "%c:/%s", tok[10], tok+12);
      TRACE (1, "CygPath conv: '%s' -> '%s'\n", tok, buf);
      tok = buf;
    }
#endif

    if (str_equal(tok, current_dir))
    {
      if (!cwd_added)
         dir_array_add (tok, TRUE);
      cwd_added = TRUE;
    }
    else
      dir_array_add (tok, FALSE);

    tok = _strtok_r (NULL, sep, &_end);
  }

  /* Insert CWD at position 0 if not done already
   */
  if (!cwd_added && !opt.no_cwd)
     dir_array_prepend (current_dir, TRUE);

  FREE (val);
  return (dir_array);
}

/*

 Improve dissection of .sys-files. E.g.:

envtool.exe --evry -d --pe pwdspio.sys
envtool.c(2218): file_spec: pwdspio.sys
envtool.c(1383): Everything_SetSearchA ("regex:^pwdspio\.sys$").
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

/**
 * In case a file or directory contains a `"~"`, switch to raw mode.
 */
void print_raw (const char *file, const char *before, const char *after)
{
  int raw;

  if (before)
     C_puts (before);
  raw = C_setraw (1);
  C_puts (file);
  C_setraw (raw);
  if (after)
     C_puts (after);
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
    TRACE (2, "fspec: '%s', *sub_dir: '%s'\n", fspec, *sub_dir);
  }

  /**
   * Similar for a given 'envtool --py module.subdir' syntax, we must
   * split and store into 'fspec' and 'subdir'.
   * Unless '.subdir' matches '.*' or '.py*'.
   */
  p = strchr (fspec, '.');
  if (opt.do_python && p && strnicmp(p, ".*", 2) && strnicmp(p, ".py", 3))
  {
    memcpy (&subdir, fspec, p-fspec);
    subdir [p-fspec] = '/';
    subdir [p-fspec+1] = '\0';
    *sub_dir = subdir;
    fspec = p+1;
    TRACE (2, "fspec: '%s', *sub_dir: '%s'\n", fspec, *sub_dir);
  }

 /**
  * Since `FindFirstFile()` doesn't work with POSIX ranges, replace
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

  TRACE (1, "fspec: %s, *sub_dir: %s\n", fspec, *sub_dir);

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

  TRACE (1, "  RegOpenKeyEx (%s\\%s, %s):\n                  %s\n",
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
                       (BYTE*)&data, &data_size);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    val32 = *(DWORD*) &data[0];
    val64 = *(LONG64*) &data[0];

    if (type == REG_EXPAND_SZ && strchr(data, '%'))
       expand_env_var (data, data, sizeof(data), TRUE);

    switch (type)
    {
      case REG_SZ:
      case REG_EXPAND_SZ:
      case REG_MULTI_SZ:
           TRACE (1, "    num: %lu, %s, value: \"%s\", data: \"%s\"\n",
                     num, reg_type_name(type),
                     value[0] ? value : "(no value)",
                     data[0]  ? data  : "(no data)");
           if (!*ret && data[0])
           {
             static char ret_data [_MAX_PATH];
             const char *dot = strrchr (data, '.');

             /* Found 1st data-value with extension we're looking for. Return it.
              */
             if (dot && !stricmp(dot, ext))
                *ret = _strlcpy (ret_data, data, sizeof(ret_data));
           }
           break;

      case REG_LINK:
           TRACE (1, "    num: %lu, REG_LINK, value: \"%" WIDESTR_FMT "\", data: \"%" WIDESTR_FMT "\"\n",
                     num, (wchar_t*)value, (wchar_t*)data);
           break;

      case REG_DWORD_BIG_ENDIAN:
           val32 = reg_swap_long (*(DWORD*)&data[0]);
           FALLTHROUGH()

      case REG_DWORD:
           TRACE (1, "    num: %lu, %s, value: \"%s\", data: %lu\n",
                     num, reg_type_name(type), value[0] ? value : "(no value)", (u_long)val32);
           break;

      case REG_QWORD:
           TRACE (1, "    num: %lu, REG_QWORD, value: \"%s\", data: %" S64_FMT "\n",
                     num, value[0] ? value : "(no value)", val64);
           break;

      case REG_NONE:
           break;

      default:
           TRACE (1, "    num: %lu, unknown REG_type %lu\n", num, (u_long)type);
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

  TRACE (1, "  RegOpenKeyEx (%s\\%s, %s):\n                   %s\n",
          reg_top_key_name(top_key), REG_APP_PATH, reg_access_name(acc), win_strerror(rc));

  for (num = 0; rc == ERROR_SUCCESS; num++)
  {
    char  sub_key [700];
    char  fname [512];
    const char *fqfn;
    DWORD size = sizeof(fname);

    rc = RegEnumKeyEx (key, num, fname, &size, NULL, NULL, NULL, NULL);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    TRACE (1, "  RegEnumKeyEx(): num %d: %s\n", num, fname);

    snprintf (sub_key, sizeof(sub_key), "%s\\%s", REG_APP_PATH, fname);

    if (enum_sub_values(top_key, sub_key, &fqfn))
       reg_array_add (top_key, fname, fqfn);
  }
  if (key)
     RegCloseKey (key);

  reg_array_sort();
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

  TRACE (1, "RegOpenKeyEx (%s\\%s, %s):\n                 %s\n",
          reg_top_key_name(top_key), sub_key, reg_access_name(acc), win_strerror(rc));

  for (num = 0; rc == ERROR_SUCCESS; num++)
  {
    char  name  [100]         = "<none>";
    char  value [MAX_ENV_VAR] = "<none>";
    DWORD nsize = sizeof(name);
    DWORD vsize = sizeof(value);
    DWORD type;

    rc = RegEnumValue (key, num, name, &nsize, NULL, &type, (BYTE*)&value, &vsize);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    if (type == REG_EXPAND_SZ && strchr(value, '%'))
       expand_env_var (value, value, sizeof(value), TRUE);

    if (!stricmp(name, "PATH"))
       *path = STRDUP (value);

    else if (!stricmp(name, "INCLUDE"))
       *inc = STRDUP (value);

    else if (!stricmp(name, "LIB"))
       *lib = STRDUP (value);

#if 0
    TRACE (1, "num %2lu, %s, %s=%.40s%s\n",
            (u_long)num, reg_type_name(type), name, value,
            strlen(value) > 40 ? "..." : "");
#else
    TRACE (1, "num %2lu, %s, %s=%s\n",
            (u_long)num, reg_type_name(type), name, value);
#endif
  }
  if (key)
     RegCloseKey (key);

  TRACE (1, "\n");
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
                          arr->is_dir, arr->exp_ok, env, key);
  }
  dir_array_free();
  return (found);
}

static int scan_system_env (void)
{
  int found = 0;

  report_header_set ("Matches in HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment:\n");

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

  report_header_set ("Matches in HKCU\\Environment:\n");

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

    TRACE (1, "i=%2d: exist=%d, match=%d, key=%s, fname=%s, path=%s\n",
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
        struct report r;

        snprintf (fqfn, sizeof(fqfn), "%s%c%s", arr->path, DIR_SEP, arr->real_fname);
        memset (&r, '\0', sizeof(r));
        r.file    = fqfn;
        r.content = opt.grep.content;
        r.mtime   = arr->mtime;
        r.fsize   = arr->fsize;
        r.key     = arr->key;
        if (report_file(&r))
           found++;
      }
    }
  }
  reg_array_free();
  return (found);
}

static int do_check_registry (void)
{
  int found = 0;

  report_header_set ("Matches in HKCU\\%s:\n", REG_APP_PATH);
  build_reg_array_app_path (HKEY_CURRENT_USER);
  found += report_registry (REG_APP_PATH);

  report_header_set ("Matches in HKLM\\%s:\n", REG_APP_PATH);
  build_reg_array_app_path (HKEY_LOCAL_MACHINE);
  found += report_registry (REG_APP_PATH);

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
    if (strcmp(ff_data.cFileName, ".") && strcmp(ff_data.cFileName, ".."))
       num_entries++;
  }
  while (num_entries == 0 && FindNextFile(handle, &ff_data));

  TRACE (3, "%s(): at least %d entries in '%s'.\n", __FUNCTION__, num_entries, dir);
  FindClose (handle);
  return (num_entries == 0);
}

/**
 * Try to match `str` against the global regular expression in `opt.file_spec`.
 */
static BOOL regex_match (const char *str)
{
  memset (&re_matches, '\0', sizeof(re_matches));
  re_err = regexec (&re_hnd, str, DIM(re_matches), re_matches, 0);
  TRACE (3, "regex() pattern '%s' against '%s'. re_err: %d\n", opt.file_spec, str, re_err);

  if (re_err == REG_NOMATCH)
     return (FALSE);

  if (re_err == REG_NOERROR)
     return (TRUE);

  regerror (re_err, &re_hnd, re_errbuf, sizeof(re_errbuf));
  TRACE (0, "Error while matching \"%s\": %d\n", str, re_err);
  return (FALSE);
}

/**
 * Process directory specified by `path` and report any matches
 * to the global `opt.file_spec`.
 */
int process_dir (const char *path, int num_dup, BOOL exist, BOOL check_empty,
                 BOOL is_dir, BOOL exp_ok, const char *prefix, HKEY key)
{
  HANDLE          handle;
  WIN32_FIND_DATA ff_data;
  BOOL            ff_more;
  char            fqfn  [_MAX_PATH];  /* Fully qualified file-name */
  char            _path [_MAX_PATH];  /* Copy of 'path' */
  int             found = 0;
  char           *end;

  /* We need to set these only once; `opt.file_spec` is constant throughout the program.
   */
  static char *fspec  = NULL;
  static char *subdir = NULL;  /* Looking for a `opt.file_spec` with a sub-dir part in it. */

  _strlcpy (_path, path, sizeof(_path));
  end = strrchr (_path, '\0');
  if (end > _path && IS_SLASH(end[-1]))
     end[-1] = '\0';

  if (num_dup > 0)
  {
#if 0     /* \todo */
    WARN ("%s: directory \"%s\" is duplicated at position %d. Skipping.\n", prefix, _path, dup_pos);
#else
    WARN ("%s: directory \"%s\" is duplicated. Skipping.\n", prefix, _path);
#endif
    return (0);
  }

  if (!exp_ok)
  {
    WARN ("%s: directory \"%s\" has an unexpanded value.\n", prefix, _path);
    return (0);
  }

  if (!exist)
  {
    WARN ("%s: directory \"%s\" doesn't exist.\n", prefix, _path);
    return (0);
  }

  if (!is_dir)
     WARN ("%s: directory \"%s\" isn't a directory.\n", prefix, _path);

  if (!opt.file_spec)
  {
    TRACE (1, "\n");
    return (0);
  }

  if (check_empty && is_dir && dir_is_empty(_path))
     WARN ("%s: directory \"%s\" is empty.\n", prefix, _path);

  if (!fspec)
     fspec = (opt.use_regex ? "*" : fix_filespec(&subdir));

  snprintf (fqfn, sizeof(fqfn), "%s%c%s%s", _path, DIR_SEP, subdir ? subdir : "", fspec);
  handle = FindFirstFile (fqfn, &ff_data);
  if (handle == INVALID_HANDLE_VALUE)
  {
    TRACE (1, "\"%s\" not found.\n", fqfn);
    return (0);
  }

  for (ff_more = TRUE; ff_more; ff_more = FindNextFile(handle, &ff_data))
  {
    struct stat   st;
    char  *base, *file = ff_data.cFileName;
    char  *true_fname = NULL;
    char  *end_pos;
    int    match, len;
    BOOL   is_junction;
    BOOL   ignore = ((file[0] == '.' && file[1] == '\0') ||                   /* current dir entry */
                     (file[0] == '.' && file[1] == '.' && file[2] == '\0'));  /* parent dir entry */

    TRACE (1, "ff_data.cFileName \"%s\", ff_data.dwFileAttributes: 0x%08lX, ignore: %d.\n",
            ff_data.cFileName, ff_data.dwFileAttributes, ignore);

    if (ignore)
       continue;

    len = snprintf (fqfn, sizeof(fqfn), "%s%c", _path, DIR_SEP);
    if (len < 0 || len >= sizeof(fqfn))  /* 'fqfn[]' too small! Or some other error? */
       continue;

    base = fqfn + len;
    snprintf (base, sizeof(fqfn)-len, "%s%s", subdir ? subdir : "", file);

    is_dir      = ((ff_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    is_junction = ((ff_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);

    if (opt.use_regex)
    {
      if (regex_match(fqfn) && safe_stat(fqfn, &st, NULL) == 0)
      {
        struct report r;

        r.file        = fqfn;
        r.content     = opt.grep.content;
        r.mtime       = st.st_mtime;
        r.fsize       = st.st_size;
        r.is_dir      = is_dir;
        r.is_junction = is_junction;
        r.key         = key;
        if (report_file(&r))
        {
          found++;
       // regex_print (&re_hnd, re_matches, fqfn);
        }
      }
      continue;
    }

    if (opt.case_sensitive && subdir)
    {
      true_fname = fqfn;
      if (get_actual_filename(&true_fname, FALSE))
      {
        _strlcpy (fqfn, true_fname, sizeof(fqfn));
        FREE (true_fname);
        end_pos = 1 + path_ltrim (fqfn, path);
        TRACE (2, "subdir: '%s', end_pos: '%s'.\n", subdir, end_pos);
      }
    }

    file  = slashify2 (fqfn, fqfn, DIR_SEP);
    match = fnmatch (opt.file_spec, base, fnmatch_case(0) | FNM_FLAG_NOESCAPE);

#if 0
    if (opt.man_mode)
    {
      TRACE (2, "opt.file_spec: \"%s\", base: \"%s\".\n", opt.file_spec, base);
      if (match == FNM_NOMATCH)
         continue;
    }

    if (match == FNM_NOMATCH && strchr(opt.file_spec, '~'))
    {
      /* The case where `opt.file_spec` is a SFN, `fnmatch()` doesn't work.
       * What to do?
       */
    }
    else
#endif

    if (match == FNM_NOMATCH)
    {
      /* The case where `base` is a dotless file, `fnmatch()` doesn't work.
       * I.e. if `opt.file_spec` == "ratio.*" and `base` == "ratio", we qualify
       *      this as a match.
       */
      if (!is_dir && !opt.dir_mode && !opt.man_mode)
      {
        if (str_equal_n(base, opt.file_spec, strlen(base)))
           match = FNM_MATCH;
      }
    }

    if (is_dir && opt.do_lib)  /* A directory is never a match for a library */
       match = FNM_NOMATCH;

    TRACE (2, "Testing \"%s\". is_dir: %d, is_junction: %d, %s\n",
           file, is_dir, is_junction, fnmatch_res(match));

    if (match == FNM_MATCH && safe_stat(file, &st, NULL) == 0)
    {
      struct report r;

      r.file        = file;
      r.content     = opt.grep.content;
      r.mtime       = st.st_mtime;
      r.fsize       = st.st_size;
      r.is_dir      = is_dir;
      r.is_junction = is_junction;
      r.key         = key;
      if (report_file(&r))
         found++;
    }
  }

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
       TRACE (1, "%s: '%s' okay\n", name, dir);
  else TRACE (1, "%s: '%s', GetLastError(): %lu\n", name, dir, (unsigned long)GetLastError());

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

/**
 * Figure out if `file` can have a shadow in `%WinDir%\sysnative`.
 * Makes no sense on Win64.
 */
static const char *get_sysnative_file (const char *file, struct stat *st)
{
#if (IS_WIN64 == 0)
  if (str_equal_n(sys_dir, file, strlen(sys_dir)) && sys_native_dir[0])
  {
    static char shadow [_MAX_PATH+2];
    struct stat st2;

    snprintf (shadow, sizeof(shadow), "%s\\%s", sys_native_dir, file+strlen(sys_dir)+1);
#if 0
    if (safe_stat(file, &st2, NULL) == 0)
       *st = st2;
#else
    ARGSUSED (st2);
#endif
    return (shadow);
  }
#endif

  ARGSUSED (st);
  return (file);
}

/**
 * \todo
 * \parblock
 *   If the result returns a file on a remote disk (X:) and the
 *   remote computer is down, EveryThing will return the the entry in
 *   it's database. But then the stat() below will fail after a long
 *   SMB timeout (SessTimeOut, default 60 sec).
 *
 *   Try to detect this if `file[0:1]` is `X:` prior to calling
 *   `stat()`. <br>
 *   Use `GetFileAttributes(file)` and test if `GetLastError()`
 *   returns `ERROR_BAD_NETPATH` ??
 *
 *  Or simply exclude the remote disk `X:` in the query.
 *  \b E.g:
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
 *   ```
 *   Status       Local     External                  Network
 *   -------------------------------------------------------------------------------
 *   Disconnected X:        \\DONALD\X-PARTITION      Microsoft Windows Network
 *   ^^
 *   ```
 *  where to get this state?
 *
 * \endparblock
 */
static int report_evry_file (const char *file, time_t mtime, UINT64 fsize, BOOL *is_shadow)
{
  struct stat   st;
  struct report r;
  BOOL        is_dir = FALSE;
  const char *file2;
  size_t      len = strlen (file);
  DWORD       attr = 0;

  memset (&st, '\0', sizeof(st));
  *is_shadow = FALSE;

  if (len >= 260)   /* MAX_PATH */
  {
    wchar_t w_file [32*1024];

    wcscpy (w_file, L"\\\\?\\");
    if (mbchar_to_wchar(w_file+4, DIM(w_file)-4, file))
       attr = GetFileAttributesW (w_file);
  }

  /* Do not use the slower `safe_stat()` unless needed.
   * See below.
   */
  if (attr == 0)
     attr = GetFileAttributes (file);

  if (attr != INVALID_FILE_ATTRIBUTES)
     is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY);

  else if (attr == INVALID_FILE_ATTRIBUTES)
  {
    file2 = get_sysnative_file (file, &st);
    if (file2 != file)
    {
      TRACE (1, "shadow: '%s' -> '%s'\n", file, file2);
      *is_shadow = TRUE;
    }
    file = file2;
  }

  if (st.st_mtime == 0)
  {
    /* If EveryThing prior to ver 1.4.1 is used, these are not set.
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

  memset (&r, '\0', sizeof(r));
  r.file        = file;
  r.content     = opt.grep.content;
  r.mtime       = mtime;
  r.fsize       = is_dir ? (__int64)-1 : fsize;
  r.is_dir      = is_dir;
  r.key         = HKEY_EVERYTHING;
  return report_file (&r);
}

/**
 * Check if EveryThing database is loaded.
 */
static BOOL evry_is_db_loaded (HWND wnd)
{
  BOOL loaded = (BOOL) Everything_IsDBLoaded();

  TRACE (1, "wnd: %p, loaded: %d.\n", wnd, loaded);
  return (loaded);
}

/**
 * Check if EveryThing is busy indexing it's database.
 */
static BOOL evry_is_busy (HWND wnd)
{
  BOOL busy = (BOOL) Everything_IsDBBusy();

  TRACE (1, "wnd: %p, busy: %d.\n", wnd, busy);
  return (busy);
}

/**
 * If EveryThing is busy indexing itself, wait for maximum `sec` before returning.
 * Or return FALSE if user pressed a key first.
 */
static BOOL evry_busy_wait (HWND wnd, UINT sec)
{
  BOOL busy = evry_is_busy (wnd);

  C_flush();
  if (kbhit())
     return (FALSE);

  if (busy && sec > 1)
  {
    Sleep (1000);
    C_puts ("~5.~0");
    C_flush();
    return evry_busy_wait (wnd, --sec);
  }
  return (!busy);
}

/**
 * The handler for option `--evry`. Search the EveryThing database.
 */
static int do_check_evry (void)
{
  DWORD  i, err, num, request_flags, response_flags, version = 0;
  DWORD  start_time, end_time;
  char   query_buf [_MAX_PATH+8];
  char  *query = query_buf;
  char  *dir   = NULL;
  char  *base  = NULL;
  int    found = 0;
  size_t len;
  HWND   wnd;
  struct ver_info evry_ver = { 0, 0, 0, 0 };

  wnd = FindWindow (EVERYTHING_IPC_WNDCLASS, 0);
  if (!wnd)
  {
    C_printf ("~5Everything search engine not found.~0\n");
    return (0);
  }

  num_evry_dups = 0;

  if (evry_bitness == bit_unknown)
     get_evry_bitness (wnd);

  if (get_evry_version(wnd, &evry_ver))
     version = (evry_ver.val_1 << 16) + (evry_ver.val_2 << 8) + evry_ver.val_3;

  TRACE (1, "version %u.%u.%u, build: %u\n",
          evry_ver.val_1,
          evry_ver.val_2,
          evry_ver.val_3,
          evry_ver.val_4);

  if (opt.evry_raw)
  {
    query = evry_raw_query();
    len = strlen (query);
  }
  else
  {
    /* EveryThing seems not to support `\\`. Must split the `opt.file_spec`
     * into a `dir` and `base` part.
     */
    if (strpbrk(opt.file_spec, "/\\"))
    {
      dir  = dirname (opt.file_spec);   /* Allocates memory */
      base = basename (opt.file_spec);
    }

    /* With option `-D/--dir`, match only folders.
     */
    if (opt.dir_mode && !opt.use_regex)
    {
      len = snprintf (query_buf, sizeof(query_buf), "regex:\"^.*\\\\%s$\" folder:",
                      translate_shell_pattern(opt.file_spec));
      TRACE (1, "Simple directory mode: '%s', opt.file_spec: '%s'.\n",
             query_buf, opt.file_spec);
    }
    else
    {
      if (opt.use_regex)
      {
        len = snprintf (query_buf, sizeof(query_buf), "regex:\"%s\"", opt.file_spec);
        if (opt.dir_mode)
           len += snprintf (query_buf+len, sizeof(query_buf)-len, " folder:");
      }
      else if (dir)
      {
        /* If user didn't use the `-r/--regex` option, we must convert
         * `opt.file_spec` into a RegExp compatible format.
         * E.g. "ez_*.py" -> "^ez_.*\.py$"
         */
        len = snprintf (query_buf, sizeof(query_buf), "regex:%s\\\\%s", dir, base);
      }
      else
        len = snprintf (query_buf, sizeof(query_buf), "regex:^%s$", translate_shell_pattern(opt.file_spec));
    }

    /* Query contents with "--grep content"
     */
    if (opt.grep.content && len > 0)
    {
      TRACE (1, "opt.grep.content: '%s'\n", opt.grep.content);
   // snprintf (query_buf+len, sizeof(query_buf)-len, "content: %s", opt.grep.content);
    }

    FREE (dir);
  }

  Everything_SetMatchCase (opt.case_sensitive);

  switch (opt.sort_methods[0])
  {
    case SORT_FILE_NAME:
         Everything_SetSort (EVERYTHING_SORT_PATH_ASCENDING);
         break;

    case SORT_FILE_DATE:
    case SORT_FILE_TIME:
         Everything_SetSort (EVERYTHING_SORT_DATE_MODIFIED_ASCENDING);
         break;

    case SORT_FILE_SIZE:
         Everything_SetSort (EVERYTHING_SORT_SIZE_ASCENDING);
         break;

    default:
         break;
  }

  request_flags = Everything_GetRequestFlags();

  /* The request flags: EVERYTHING_REQUEST_SIZE and/or EVERYTHING_REQUEST_DATE_MODIFIED
   * needs v. 1.4.1 or later.
   * Ref:
   *   https://www.voidtools.com/support/everything/sdk/everything_setrequestflags/
   *
   * But do not request the file size/time since that could be "old information" when
   * files are frequently updated.
   */
#if 0
  if (version >= 0x010401)
  {
    request_flags |= EVERYTHING_REQUEST_SIZE | EVERYTHING_REQUEST_DATE_MODIFIED;
    Everything_SetRequestFlags (request_flags);
    request_flags = Everything_GetRequestFlags();  /* should be the same as set above */
  }
#else
  ARGSUSED (version);
#endif

  start_time = GetTickCount();

  TRACE (1, "Everything_SetSearchA (\"%s\").\n"
             "                 Everything_SetMatchCase (%d).\n",
             query, opt.case_sensitive);

  Everything_SetSearchA (query);
  Everything_QueryA (TRUE);

  end_time = GetTickCount();

  err = Everything_GetLastError();
  TRACE (1, "Everything_Query: %s\n", evry_strerror(err));

  if (halt_flag > 0)
     return (0);

  if (err == EVERYTHING_ERROR_IPC)
  {
    WARN ("Everything IPC service is not running.\n");
    return (0);
  }

  if (!evry_is_db_loaded(wnd))
  {
    WARN ("Everything database is not loaded.\n");
    return (0);
  }

  if (evry_is_busy(wnd))
  {
    if (wnd && opt.evry_busy_wait)
    {
      WARN ("Everything is busy loading it's database. Waiting %u sec. ", opt.evry_busy_wait);
      if (!evry_busy_wait(wnd, opt.evry_busy_wait))
         return (0);
    }
    else
    {
      WARN ("Everything is busy loading it's database.\n");
      return (0);
    }
  }

  num = Everything_GetNumResults();
  TRACE (1, "Everything_GetNumResults() num: %lu, err: %s\n",
          (u_long)num, evry_strerror(Everything_GetLastError()));

  if (opt.beep.enable && end_time >= start_time && (end_time - start_time) >= opt.beep.limit)
     Beep (opt.beep.freq, opt.beep.msec);

  if (num == 0)
  {
    if (opt.use_regex)
         WARN ("Nothing matched your regexp \"%s\".\n"
               "Are you sure it is correct? Try quoting it.\n", opt.file_spec);
    else WARN ("Nothing matched your search \"%s\".\n", opt.file_spec);
    return (0);
  }

  /* Sort results by path (ignore case).
   * This will fail if `request_flags` has either `EVERYTHING_REQUEST_SIZE`
   * or `EVERYTHING_REQUEST_DATE_MODIFIED` since version 2 of the query protocol
   * is used.
   * Ref: The comment in Everything.c; "//TODO: sort list2"
   */
  Everything_SortResultsByPath();
  err = Everything_GetLastError();
  if (err != EVERYTHING_OK)
  {
    TRACE (2, "Everything_SortResultsByPath(), err: %s\n", evry_strerror(err));
    Everything_SetLastError (EVERYTHING_OK);
  }

  for (i = 0; i < num; i++)
  {
    char   prev [_MAX_PATH];
    char   file [_MAX_PATH];
    UINT64 fsize = (__int64)-1;  /* since a 0-byte file is valid */
    time_t mtime = 0;
    BOOL   is_shadow = FALSE;
    int    ignore;

    if (halt_flag > 0)
       break;

    if (i == 0)
       prev[0] = '\0';

    len = Everything_GetResultFullPathName (i, file, sizeof(file));
    err = Everything_GetLastError();
    if (len == 0 || err != EVERYTHING_OK)
    {
      TRACE (2, "Everything_GetResultFullPathName(), err: %s\n",
              evry_strerror(err));
      len = 0;
      break;
    }

    /* Handle unquoted files with spaces specially.
     */
    if (*file != '"' && strchr(file, ' '))
    {
      char quoted_file [_MAX_PATH];
      snprintf (quoted_file, sizeof(quoted_file), "\"%s\"", file);
      ignore = cfg_ignore_lookup ("[EveryThing]", quoted_file);
    }
    else
      ignore = cfg_ignore_lookup ("[EveryThing]", file);

    TRACE (1, "cfg_ignore_lookup(\"[EveryThing]\", \"%s\") -> %d\n", file, ignore);

    if (ignore)
    {
      num_evry_ignored++;
      continue;
    }

    response_flags = 0;

    if (request_flags & EVERYTHING_REQUEST_DATE_MODIFIED)
    {
      FILETIME ft;

      if (Everything_GetResultDateModified(i, &ft))
      {
        response_flags |= EVERYTHING_REQUEST_DATE_MODIFIED;
        mtime = FILETIME_to_time_t (&ft);
        TRACE (2, "%3lu: Everything_GetResultDateModified(), mtime: %.24s\n",
                i, mtime ? ctime(&mtime) : "<N/A>");
      }
      else
      {
        err = Everything_GetLastError();
        TRACE (2, "%3lu: Everything_GetResultDateModified(), err: %s\n",
                i, evry_strerror(err));
      }
    }
    if (request_flags & EVERYTHING_REQUEST_SIZE)
    {
      LARGE_INTEGER fs;

      if (Everything_GetResultSize(i, &fs))
      {
        response_flags |= EVERYTHING_REQUEST_SIZE;
        fsize = ((UINT64)fs.u.HighPart << 32) + fs.u.LowPart;
        TRACE (2, "%3lu: Everything_GetResultSize(), %s\n",
                i, get_file_size_str(fsize));
      }
      else
      {
        err = Everything_GetLastError();
        TRACE (2, "%3lu: Everything_GetResultSize(), err: %s\n",
                i, evry_strerror(err));
      }
    }

    if ((response_flags & EVERYTHING_REQUEST_DATE_MODIFIED) == 0 ||
        (response_flags & EVERYTHING_REQUEST_SIZE) == 0 )
    {
      struct stat st;

      if (safe_stat(file, &st, NULL) == 0)
      {
        mtime = st.st_mtime;
        if (opt.show_size)
           fsize = st.st_size;
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

/**
 * The main work-horse of this program.
 */
int do_check_env (const char *env_name)
{
  smartlist_t *list;
  int          i, max, found = 0;
  BOOL         check_empty = FALSE;
  char        *orig_e = getenv_expand (env_name);

  if (!orig_e)
  {
    TRACE (1, "Env-var %s not defined.\n", env_name);
    return (0);
  }

  if (!strcmp(env_name, "PATH") ||
      !strcmp(env_name, "LIB") ||
      !strcmp(env_name, "LIBRARY_PATH") ||
      !strcmp(env_name, "INCLUDE") ||
      !strcmp(env_name, "C_INCLUDE_PATH") ||
      !strcmp(env_name, "CPLUS_INCLUDE_PATH"))
    check_empty = TRUE;

  list = split_env_var (env_name, orig_e);
  max  = smartlist_len (list);
  for (i = 0; i < max; i++)
  {
    struct directory_array *arr = smartlist_get (list, i);

    if (check_empty && arr->exist && !arr->is_cwd)
       arr->check_empty = check_empty;

    if (arr->is_cwd)
       TRACE (1, "arr->dir: '%s', arr->is_cwd: 1\n", arr->dir);

    if (!arr->done)
       found += process_dir (arr->dir, arr->num_dup, arr->exist, arr->check_empty,
                             arr->is_dir, arr->exp_ok, env_name, NULL);
    arr->done = TRUE;
  }
  dir_array_free();
  FREE (orig_e);
  return (found);
}

/**
 * Check a single MANPAGE directory for man-page match(es).
 */
static int check_man_dir (const char *dir, const char *env_name)
{
  const char *_dir;

  if (!strcmp(dir, "\\"))
       _dir = current_dir;
  else _dir = dir;

  TRACE (1, "Looking for \"%s\" in \"%s\".\n", opt.file_spec, _dir);
  return process_dir (dir, 0, TRUE, TRUE, 1, TRUE, env_name, HKEY_MAN_FILE);
}

/**
 * The `MANPATH` checking needs to be recursive (1 level); check all
 * `man*` and `cat*` directories under each directory in `%MANPATH`.
 *
 * If `".\\"` (or `"./"`) is in `%MANPATH`, test for existence in `current_dir`
 * first.
 */
static int do_check_manpath (void)
{
  struct directory_array *arr;
  smartlist_t *list;
  int          i, j, max, found = 0;
  char        *orig_e;
  BOOL         save1, save2, done_cwd = FALSE;
  static const char env_name[] = "MANPATH";

  /* \todo
   *   This should be all directories matching "man?[pn]" or "cat?[pn]".
   *   Make this array into a smartlist too?
   */
  static const char *sub_dirs[] = { "",
                                    "cat0", "cat1", "cat2", "cat3", "cat4",
                                    "cat5", "cat6", "cat7", "cat8", "cat9",
                                    "man0", "man1", "man2", "man3", "man4",
                                    "man5", "man6", "man7", "man8", "man9", "mann"
                                  };

  /* Do not implicit add current directory in searches.
   * Unless `%MANPATH` contain a "./;" or a ".\;".
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

  report_header_set ("Matches in %%%s:\n", env_name);

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

    /* If `%MANPATH` contains a `./`, check in current-dir.
     * If the path contains multiple `./`, do this only once (with a warning).
     * Also if current-dir contains `man*` sub-directories, check them too in the
     * below for-loop.
     */
    if (arr->is_cwd)
    {
      if (done_cwd)
           WARN ("%s: Contains multiple \".\\\".\n", env_name);
      else found += check_man_dir (".\\", env_name);
      done_cwd = TRUE;
    }

    if (done_cwd)
         j = 1;
    else j = 0;

    for ( ; j < DIM(sub_dirs); j++)
    {
      char dir [_MAX_PATH];

      snprintf (dir, sizeof(dir), "%s%c%s", arr->dir, DIR_SEP, sub_dirs[j]);
      if (FILE_EXISTS(dir))
         found += check_man_dir (dir, env_name);
    }
  }

  opt.no_cwd   = save1;
  opt.man_mode = save2;
  dir_array_free();
  FREE (orig_e);
  return (found);
}

/**
 * Search for VCPKG installed packages matching the `opt.file_spec`.
 * If option `--vcpkg=all` was used, show the package status for `opt.file_spec` regardless
 * of being installed.
 */
static int do_check_vcpkg (void)
{
  unsigned num;

  if (vcpkg_get_only_installed())
       report_header_set ("Matches for installed VCPKG packages:\n");
  else report_header_set ("Matches for any available VCPKG package:\n");

  report_header_print();

  num = vcpkg_find (opt.file_spec);

  if (num == 0 && vcpkg_get_only_installed())
     C_printf ("Try the option ~6--vcpkg=all~0 to check if the package is available.\n");

  if (!opt.quiet && *vcpkg_last_error())
     C_printf ("%s.\n", vcpkg_last_error());
  return (num);
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
           { "no-watcom",   no_argument,       NULL, 0 },    /* 31 */
           { "no-borland",  no_argument,       NULL, 0 },
           { "no-clang",    no_argument,       NULL, 0 },    /* 33 */
           { "no-intel",    no_argument,       NULL, 0 },
           { "no-msvc",     no_argument,       NULL, 0 },    /* 35 */
           { "owner",       optional_argument, NULL, 0 },
           { "check",       no_argument,       NULL, 0 },    /* 37 */
           { "signed",      optional_argument, NULL, 0 },
           { "no-cwd",      no_argument,       NULL, 0 },    /* 39 */
           { "sort",        required_argument, NULL, 0 },
           { "lua",         no_argument,       NULL, 0 },    /* 41 */
           { "vcpkg",       optional_argument, NULL, 0 },
           { "descr",       no_argument,       NULL, 0 },    /* 43 */
           { "keep",        no_argument,       NULL, 0 },
           { "grep",        required_argument, NULL, 0 },    /* 45 */
           { "only",        no_argument,       NULL, 0 },
           { "case",        no_argument,       NULL, 'c' },  /* 47 */
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
            &opt.no_watcom,           /* 31 */
            &opt.no_borland,
            &opt.no_clang,            /* 33 */
            &opt.no_intel,
            &opt.no_msvc,             /* 35 */
            &opt.show_owner,
            &opt.do_check,            /* 37 */
            (int*)&opt.signed_status,
            &opt.no_cwd,              /* 39, Since option '-S' is handled specially. This address is not used. */
            (int*)1,
            &opt.do_lua,              /* 41 */
            &opt.do_vcpkg,
            &opt.show_descr,          /* 43 */
            &opt.keep_temp,
            (int*)&opt.grep.content,  /* 45 */
            &opt.grep.only,
            (int*)&opt.case_sensitive /* 47 */
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

  TRACE (2, "optarg: '%s'\n", arg);
  ASSERT (arg);

  for (i = 0; py[i]; i++)
     if (!stricmp(arg, py[i]))
     {
       v = py_variant_value (arg, NULL);
       break;
    }

  if (v == UNKNOWN_PYTHON)
  {
    char buf[100], *p = buf;
    int  len, left = sizeof(buf)-1;

    for (i = 0; py[i] && left > 4; i++)
    {
      len = snprintf (p, left, "\"%s\", ", py[i]);
      p    += len;
      left -= len;
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
 * `getopt_long()` handler for option `--vcpkg=<arg>`.
 */
static void set_vcpkg_options (const char *arg)
{
  TRACE (2, "optarg: '%s'\n", arg);
  ASSERT (arg);
  if (!strcmp(arg, "all"))
       vcpkg_set_only_installed (FALSE);
  else usage ("Illegal '--vcpkg' option: '%s'.\n"
              "Use '--vcpkg' or 'vcpkg=all'.\n", arg);
}

/**
 * `getopt_long()` handler for option `-H arg`, `--host arg` or `--evry:arg`
 * used by remote ETP queries.
 */
static void set_evry_options (const char *arg)
{
  if (arg)
  {
    if (!opt.evry_host)
       opt.evry_host = smartlist_new();
    smartlist_add_strdup (opt.evry_host, arg);
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
    if (*arg == '1' || !stricmp(arg, "on") || !stricmp(arg, "yes"))
       opt.signed_status = SIGN_CHECK_SIGNED;

    else if (*arg == '0' || !stricmp(arg, "off") || !stricmp(arg, "no"))
       opt.signed_status = SIGN_CHECK_UNSIGNED;
  }

  TRACE (2, "got long option \"--signed %s\" -> opt.signed_status: %s\n",
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
     smartlist_add_strdup (opt.owners, arg);
  else
  if (smartlist_len(opt.owners) == 0)
     smartlist_add_strdup (opt.owners, "*");
}

/**
 * The handler for short options called from `getopt_long()`.
 *
 * \param[in] o    an alphabetical letters given in `c->short_opt` and `getopt_parse(c)`.
 * \param[in] arg  the optional argument for the option.
 */
static void set_short_option (int o, const char *arg)
{
  char *err_opt;

  TRACE (2, "got short option '%c' (%d).\n", o, o);

  switch (o)
  {
    case 'h':
         opt.do_help = 1;
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
    case 'o':
         opt.grep.only = 1;
         break;
    case 'r':
         opt.use_regex = 1;
         break;
    case 's':
         opt.show_size = 1;
         break;
    case 'S':
         if (!set_sort_method(arg, &err_opt))
            usage ("Illegal \"-S\" method '%s'. Use a combination of: %s\n",
                   err_opt, get_sort_methods());
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
         usage ("Illegal option: '%c' (%d)\n", optopt, optopt);
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

  ASSERT (o >= 0);
  ASSERT (o < DIM(values_tab));

  ASSERT (values_tab[o]);
  ASSERT (long_options[o].name);

  TRACE (2, "got long option \"--%s\" with argument \"%s\".\n",
          long_options[o].name, arg);

  if (!strcmp("evry", long_options[o].name))
  {
    set_evry_options (arg);
    opt.do_evry = 1;
    return;
  }

  if (!strcmp("signed", long_options[o].name))
  {
    set_signed_options (arg);
    return;
  }

  if (!strcmp("sort", long_options[o].name))
  {
    char *err_opt;

    if (!set_sort_method(arg, &err_opt))
       usage ("Illegal \"--sort\" method '%s'. Use a combination of: %s\n",
              err_opt, get_sort_methods());
    return;
  }

  if (!strcmp("owner", long_options[o].name))
  {
    set_owner_options (arg);
    return;
  }

  if (!strcmp("grep", long_options[o].name))
  {
    opt.grep.content = STRDUP (arg);
    return;
  }

  if (arg)
  {
    if (!strcmp("python", long_options[o].name))
    {
      opt.do_python++;
      set_python_variant (arg);
    }

    else if (!strcmp("vcpkg", long_options[o].name))
    {
      opt.do_vcpkg++;
      set_vcpkg_options (arg);
    }

    else if (!strcmp("debug", long_options[o].name))
      opt.debug = atoi (arg);

    else if (!strcmp("host", long_options[o].name))
      set_evry_options (arg);
  }
  else
  {
    val_ptr = values_tab [o];
    new_value = *val_ptr + 1;

    TRACE (2, "got long option \"--%s\". Setting value %d -> %d. o: %d.\n",
            long_options[o].name, *val_ptr, new_value, o);

    *val_ptr = new_value;
  }
}

/**
 * Parse the command-line.
 *
 * if there are several non-options, an `--evry` search must have `opt.evry_raw == TRUE` to
 * pass the remaining cmd-line unchanged to `Everything_SetSearchA()`.
 */
static void parse_cmdline (int _argc, const char **_argv)
{
  command_line *c = &opt.cmd_line;

  c->env_opt       = "ENVTOOL_OPTIONS";
  c->short_opt     = "+chH:vVdDkorsS:tTuq";
  c->long_opt      = long_options;
  c->set_short_opt = set_short_option;
  c->set_long_opt  = set_long_option;
  getopt_parse (c, _argc, _argv);

  if (c->argc0 > 0 && c->argc - c->argc0 >= 1)
     opt.file_spec = STRDUP (c->argv[c->argc0]);

  if ((c->argc0 > 0) && (c->argc - c->argc0 >= 2))
  {
    FREE (opt.file_spec);
    opt.file_spec = str_join (c->argv + c->argc0, " ");
    opt.evry_raw  = TRUE;
  }

  TRACE (2, "c->argc0:      %d\n", c->argc0);
  TRACE (2, "opt.file_spec: '%s'\n", opt.file_spec);
  TRACE (2, "opt.evry_raw:  %d\n", opt.evry_raw);
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

  if (!opt.PE_check && !opt.do_vcpkg && !opt.do_man && !opt.do_cmake && (opt.only_32bit || opt.only_64bit))
     opt.PE_check = TRUE;

  if (opt.do_check &&
      (opt.do_path  || opt.do_lib || opt.do_include || opt.do_evry   || opt.do_man ||
       opt.do_cmake || opt.do_pkg || opt.do_vcpkg   || opt.do_python || opt.do_lua ||
       opt.do_tests))
  {
    WARN ("Option '--check' should be used alone.\n");
    return (0);
  }

  if (opt.evry_host)
  {
    if (opt.signed_status != SIGN_CHECK_NONE)
       WARN ("Option '--sign' is not supported for a remote search.\n");
    if (opt.PE_check)
       WARN ("Option '--pe' is not supported for a remote search.\n");
    if (opt.show_owner)
       WARN ("Option '--owner' is not supported for a remote search.\n");
  }

  if (opt.grep.content)
  {
    if (opt.evry_host)
       WARN ("Option '--grep' is not supported for a remote search.\n");
    else if (opt.do_vcpkg)
       WARN ("Option '--grep' is not supported for a VCPKG search.\n");
  }
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

  Everything_CleanUp();

  dir_array_free();

  FREE (who_am_I);

  FREE (system_env_path);
  FREE (system_env_lib);
  FREE (system_env_inc);

  FREE (user_env_path);
  FREE (user_env_lib);
  FREE (user_env_inc);
  FREE (opt.file_spec);
  FREE (opt.grep.content);

  compiler_exit();

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
  cfg_exit (opt.cfg_file);
  opt.cfg_file = NULL;

  vcpkg_exit();
  lua_exit();
  pkg_config_exit();
  file_descr_exit();
  cache_exit();
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
     Everything_KillThread();

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
static void init_all (const char *argv0)
{
  char        buf [_MAX_PATH];
  const char *user = get_user_name();

  atexit (cleanup);
  crtdbug_init();

  tzset();
  setlocale (LC_ALL, "");
  memset (&opt, 0, sizeof(opt));
  opt.under_conemu   = C_conemu_detected();
  opt.under_winterm  = C_winterm_detected();
  opt.under_appveyor = (stricmp(user, "APPVYR-WIN\\appveyor") == 0);
  opt.under_github   = (stricmp(user, "??\\??") == 0);
  opt.evry_busy_wait = 2;

#ifdef __CYGWIN__
  opt.under_cygwin = 1;
#endif

  if (GetModuleFileName(NULL, buf, sizeof(buf)))
       who_am_I = STRDUP (buf);
  else who_am_I = STRDUP (argv0);

  program_name = who_am_I;

  C_use_colours = 1;  /* Turned off by "--no-colour" */

  vcpkg_set_only_installed (TRUE);

  dir_array = smartlist_new();
  reg_array = smartlist_new();

  file_descr_init();

  current_dir[0] = '.';
  current_dir[1] = DIR_SEP;
  current_dir[2] = '\0';
  getcwd (current_dir, sizeof(current_dir));
  _fix_drive (current_dir);

  sys_dir[0] = sys_native_dir[0] = '\0';
  if (GetSystemDirectory(sys_dir, sizeof(sys_dir)))
  {
    const char *rslash = strrchr (sys_dir, '\\');

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
 * The config-file handler for "beep.*" key/value pairs.
 */
static void cfg_beep_handler (const char *key, const char *value)
{
  if (!stricmp(key, "enable"))
     opt.beep.enable = atoi (value);

  else if (!stricmp(key, "limit"))
     opt.beep.limit = (unsigned) atoi (value);

  else if (!stricmp(key, "freq"))
     opt.beep.freq = (unsigned) atoi (value);

  else if (!stricmp(key, "msec"))
     opt.beep.msec = (unsigned) atoi (value);
}

/**
 * The config-file handler for "ETP.*" key/value pairs.
 */
static void cfg_ETP_handler (const char *key, const char *value)
{
  if (!stricmp(key, "buffered_io"))
     opt.use_buffered_io = atoi (value);

  else if (!stricmp(key, "nonblock_io"))
     opt.use_nonblock_io = atoi (value);
}

/**
 * The config-file handler for "grep.*" key/value pairs.
 */
static void cfg_grep_handler (const char *key, const char *value)
{
  if (!stricmp(key, "max_matches"))
     opt.grep.max_matches = atoi (value);
}

/**
 * The config-file handler for key/value pairs in the "[Shadow]" section.
 */
static void shadow_ignore_handler (const char *section, const char *key, const char *value)
{
  if (!stricmp(key, "dtime"))
  {
    char     *end;
    ULONGLONG val = _strtoi64 (value, &end, 10);

    if (end == value || *end != '\0' || val == _I64_MAX || val == _I64_MIN)
         TRACE (1, "illegal dtime: '%s'\n", value);
    else opt.shadow_dtime = 10000000ULL * val;        /* Convert to 100 nsec file-time units */
    TRACE (1, "opt.shadow_dtime: %0.f sec.\n", (double)opt.shadow_dtime/1E7);
  }
  else
    cfg_ignore_handler (section, key, value);
}

#ifdef NOT_YET
/*
 * "colour.1 = bright yellow"   -> Map color "~1" to bright yellow on default background ( == 6 | FOREGROUND_INTENSITY)
 * "colour.2 = bri red on blue" -> Map color "~2" to bright red on blue background ( == 4 | FOREGROUND_INTENSITY + 16*4)
 */
static void colour_handler (const char *key, const char *value)
{
  int key_idx = atoi (key);
  C_init_colour (key_idx, value);
}
#endif

/**
 * The config-file parser for key/value pairs *not* in any section
 * (at the start of the `%APPDATA%/envtool.cfg` file).
 */
static void envtool_cfg_handler (const char *section, const char *key, const char *value)
{
  if (!strnicmp(key, "beep.", 5))
  {
    TRACE (2, "%s: Calling 'cfg_beep_handler (\"%s\", \"%s\")'.\n", section, key+5, value);
    cfg_beep_handler (key+5, value);
  }
  else if (!strnicmp(key, "cache.", 6))
  {
    TRACE (2, "%s: Calling 'cache_config (\"%s\", \"%s\")'.\n", section, key+6, value);
    cache_config (key+6, value);
  }
  else if (!strnicmp(key, "ETP.", 4))
  {
    TRACE (2, "%s: Calling 'cfg_ETP_handler (\"%s\", \"%s\")'.\n", section, key+4, value);
    cfg_ETP_handler (key+4, value);
  }
  else if (!strnicmp(key, "grep.", 5))
  {
    TRACE (2, "%s: Calling 'cfg_grep_handler (\"%s\", \"%s\")'.\n", section, key+5, value);
    cfg_grep_handler (key+5, value);
  }
  else if (!strnicmp(key, "lua.", 4))
  {
    TRACE (2, "%s: Calling 'lua_cfg_handler (\"%s\", \"%s\")'.\n", section, key+4, value);
    lua_cfg_handler (key+4, value);
  }
#ifdef NOT_YET
  else if (!strnicmp(key, "colour.", 6))
  {
    TRACE (2, "%s: Calling 'colour_handler (\"%s\", \"%s\")'.\n", section, key+6, value);
    colour_handler (key+6, value);
  }
#endif
}

/**
 * The config-file handler for key/value pairs in the "[Everything]" section.
 */
static void evry_cfg_handler (const char *section, const char *key, const char *value)
{
  if (!stricmp(key, "busy_wait"))
       opt.evry_busy_wait = atoi (value);
  else cfg_ignore_handler (section, key, value);
}

/**
 * Our main entry point:
 *  + Initialise program.
 *  + Parse the command line.
 *  + Evaluate given options for conflicts.
 *  + Initialise the color.c module (`C_init()`).
 *  + Open and parse `"%APPDATA%/envtool.cfg"`.
 *  + Check if `%WINDIR%\\sysnative` and/or `%WINDIR%\\SysWOW64` exists.
 *  + Install signal-handlers for `SIGINT` and `SIGILL`.
 *  + Call the appropriate functions based on command-line options.
 *  + Finally call `report_final()` to report findings.
 */
int MS_CDECL main (int argc, const char **argv)
{
  int found = 0;

  init_all (argv[0]);

  parse_cmdline (argc, argv);

  C_no_ansi = opt.no_ansi;

  /* Use ANSI-sequences under ConEmu, 'AppVeyor', 'Github Actions' or if "%COLOUR_TRACE >= 2" (for testing).
   * Nullifies the "--no-ansi" option.
   */
  if (opt.under_conemu || opt.under_appveyor || opt.under_github || opt.under_cygwin || C_trace_level() >= 2)
     C_use_ansi_colours = 1;

  if (opt.no_colours)
     C_use_colours = C_use_ansi_colours = 0;

  C_init();

  if (!eval_options())
     return (1);

  opt.cfg_file = cfg_init ("%APPDATA%/envtool.cfg",
                           "",               envtool_cfg_handler,
                           "[Compiler]",     cfg_ignore_handler,
                           "[Registry]",     cfg_ignore_handler,
                           "[Python]",       cfg_ignore_handler,
                           "[PE-resources]", cfg_ignore_handler,
                           "[EveryThing]",   evry_cfg_handler,
                           "[Login]",        auth_envtool_handler,
                           "[Shadow]",       shadow_ignore_handler,
                           NULL);

  cfg_ignore_dump();

  if (opt.use_cache)
     cache_init();

  check_sys_dirs();

  /* Sometimes the IPC connection to the EveryThing Database will hang.
   * Clean up if user presses ^C.
   * SIGILL handler is needed for test_libssp().
   */
  signal (SIGINT, halt);
  signal (SIGILL, halt);

  if (opt.do_help)
     return show_help();

  if (opt.do_version)
     return show_version();

  if (opt.do_python)
     py_init();

  if (opt.do_lua)
     lua_init();

  if (opt.do_check)
     return do_check();

  if (opt.do_tests)
     return do_tests();

  if (opt.do_evry && !opt.do_path)
     opt.no_sys_env = opt.no_usr_env = opt.no_app_path = 1;

  if (opt.do_lib || opt.do_include)
     compiler_init (FALSE, FALSE);

  if (!(opt.do_path || opt.do_lib || opt.do_include))
     opt.no_sys_env = opt.no_usr_env = 1;

  if (!opt.do_path && !opt.do_include && !opt.do_lib && !opt.do_python && !opt.do_lua &&
      !opt.do_evry && !opt.do_cmake   && !opt.do_man && !opt.do_pkg && !opt.do_vcpkg)
     usage ("Use at least one of; \"--evry\", \"--cmake\", \"--inc\", \"--lib\", "
            "\"--lua\", \"--man\", \"--path\", \"--pkg\", \"--vcpkg\" and/or \"--python\".\n");

  if (!opt.file_spec)
     usage ("You must give a filespec to search for.\n");

  if (!opt.evry_raw && !opt.dir_mode)
  {
    if (!opt.use_regex)
    {
      char *end, *dot, *fspec;

      if (strchr(opt.file_spec, '~') > opt.file_spec)
      {
        fspec = opt.file_spec;
        opt.file_spec = _fix_path (fspec, NULL);
        FREE (fspec);
      }

      end = strrchr (opt.file_spec, '\0');
      dot = strrchr (opt.file_spec, '.');
      if (!dot && !opt.do_vcpkg && !opt.do_python)
      {
        if (opt.do_pkg && end > opt.file_spec && end[-1] != '*')
           opt.file_spec = str_acat (opt.file_spec, ".pc*");

        else if (opt.do_cmake && !str_endswith(opt.file_spec, ".cmake"))
           opt.file_spec = str_acat (opt.file_spec, ".cmake*");

        else if (end > opt.file_spec && end[-1] != '*' && end[-1] != '$')
           opt.file_spec = str_acat (opt.file_spec, ".*");
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

  TRACE (1, "opt.file_spec: '%s'\n", opt.file_spec);

  if (!opt.no_sys_env)
     found += scan_system_env();

  if (!opt.no_usr_env)
     found += scan_user_env();

  if (opt.do_path)
  {
    if (!opt.no_app_path)
       found += do_check_registry();

    report_header_set ("Matches in %%PATH:\n");
    found += do_check_env ("PATH");
  }

  if (opt.do_lib)
  {
    if (!opt.no_gcc)
       found += compiler_check_libraries (CC_GNU_GCC);

    if (!opt.no_gpp)
       found += compiler_check_libraries (CC_GNU_GXX);

    if (!opt.no_msvc)
       found += compiler_check_libraries (CC_MSVC);

    if (!opt.no_borland)
       found += compiler_check_libraries (CC_BORLAND);

    if (!opt.no_clang)
       found += compiler_check_libraries (CC_CLANG);

    if (!opt.no_intel)
       found += compiler_check_libraries (CC_INTEL);

    if (!opt.no_watcom)
       found += compiler_check_libraries (CC_WATCOM);
  }

  if (opt.do_include)
  {
    if (!opt.no_gcc)
       found += compiler_check_includes (CC_GNU_GCC);

    if (!opt.no_gpp)
       found += compiler_check_includes (CC_GNU_GXX);

    if (!opt.no_msvc)
       found += compiler_check_includes (CC_MSVC);

    if (!opt.no_borland)
       found += compiler_check_includes (CC_BORLAND);

    if (!opt.no_clang)
       found += compiler_check_includes (CC_CLANG);

    if (!opt.no_intel)
       found += compiler_check_includes (CC_INTEL);

    if (!opt.no_watcom)
       found += compiler_check_includes (CC_WATCOM);
  }

  if (opt.do_cmake)
     found += cmake_search();

  if (opt.do_man)
     found += do_check_manpath();

  if (opt.do_pkg)
     found += pkg_config_search (opt.file_spec);

  if (opt.do_lua)
     found += lua_search (opt.file_spec);

  if (opt.do_vcpkg)
     found += do_check_vcpkg();

  if (opt.do_python)
     found += py_search();

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

      report_header_set ("Matches from %s:\n", host);
      found += do_check_evry_ept (host);
    }
    if (max  == 0)
    {
      report_header_set ("Matches from EveryThing:\n");
      found += do_check_evry();
    }
  }

  ARGSUSED (argc);

  report_final (found);
  return (found ? 0 : 1);
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

/*
 * Returns a copy of `de`.
 */
static struct dirent2 *copy_de (const struct dirent2 *de)
{
  struct dirent2 *copy = MALLOC (sizeof(*copy));

  TRACE (2, "Adding '%s'\n", de->d_name);
  memcpy (copy, de, sizeof(*copy));
  copy->d_link = NULL;
  copy->d_name = STRDUP (de->d_name);
  slashify2 (copy->d_name, copy->d_name, '\\');
  return (copy);
}

/**
 * Traverse a `dir` and look for files matching file-spec.
 *
 * \return A smartlist of `struct dirent2*` entries.
 *
 * \todo Use `scandir2()` instead. This can match a more advanced `file_spec`
 *       that `fnmatch()` supports. Like `*.[ch]`.
 */
smartlist_t *get_matching_files (const char *dir, const char *file_spec)
{
  struct dirent2     *de;
  struct od2x_options dir_opt;
  smartlist_t        *dir_list;
  DIR2               *dp;

  memset (&dir_opt, '\0', sizeof(dir_opt));
  dir_opt.pattern = file_spec;
  dir_opt.sort    = OD2X_FILES_FIRST;

  if (!is_directory(dir) || (dp = opendir2x(dir, &dir_opt)) == NULL)
     return (NULL);

  dir_list = smartlist_new();

  while ((de = readdir2(dp)) != NULL)
  {
    if (!(de->d_attrib & FILE_ATTRIBUTE_DIRECTORY) /* &&
        fnmatch(file_spec, de->d_name, FNM_FLAG_NOCASE) == FNM_MATCH */)
      smartlist_add (dir_list, copy_de(de));
  }
  closedir2 (dp);
  return (dir_list);
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
static BOOL is_shadow_candidate (const struct dirent2 *this_de,
                                 const struct dirent2 *prev_de,
                                 FILETIME *newest, FILETIME *oldest)
{
  const char *this_base = basename (this_de->d_name);
  const char *prev_base = basename (prev_de->d_name);
  ULONGLONG   this_ft, prev_ft, diff;

  if (stricmp(this_base, prev_base))
     return (FALSE);

  if (cfg_ignore_lookup("[Shadow]", this_base) ||
      cfg_ignore_lookup("[Shadow]", this_de->d_name) ||
      cfg_ignore_lookup("[Shadow]", prev_de->d_name))
  {
    TRACE (2, "Ignoring file '%s' and '%s'.\n", this_de->d_name, prev_de->d_name);
    return (FALSE);
  }

  this_ft = (((ULONGLONG)this_de->d_time_write.dwHighDateTime) << 32) + this_de->d_time_write.dwLowDateTime;
  prev_ft = (((ULONGLONG)prev_de->d_time_write.dwHighDateTime) << 32) + prev_de->d_time_write.dwLowDateTime;
  diff = prev_ft - this_ft;

  if (this_ft && this_ft < prev_ft && diff > opt.shadow_dtime)
  {
    TRACE (1, "Write-time of '%s' shadows '%s'.\n", this_de->d_name, prev_de->d_name);
    *newest = prev_de->d_time_write;
    *oldest = this_de->d_time_write;
    return (TRUE);
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
    return (TRUE);
  }
#endif

  return (FALSE);
}

/**
 * Traverse dir-lists of 2 directories and add a "shadow warning" to `shadow_list`
 * if an older file is found in `prev_dir->dir` (which is ahead of `this_dir->dir`
 * in the path for this env-var).
 *
 * \eg. with a `PATH=f:\\ProgramFiler\\Python27;f:\\CygWin32\\bin` and these files:
 * ```
 *   f:\ProgramFiler\Python27\python2.7.exe   24.06.2011  12:38   (oldest)
 *   f:\CygWin32\bin\python2.7.exe            20.03.2019  18:32
 * ```
 *
 * then the oldest `python2.7.exe` shadows the newest `python2.7.exe`.
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
  struct directory_array *arr_i, *arr_j;
  smartlist_t            *shadows;
  int                     i, j, max;

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

static void put_dirlist_to_cache (const char *env_var, smartlist_t *dirs)
{
  const struct directory_array *da;
  int   i, max = smartlist_len (dirs);

  for (i = 0; i < max; i++)
  {
    da = smartlist_get (dirs, i);
    cache_putf (SECTION_ENV_DIR, "env_dir_%s_%d = %s", env_var, i, da->dir);
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
  smartlist_t *list = NULL;
  int          i, errors, max = 0;
  char        *value;
  const struct directory_array *arr;

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

    if (str_equal_n("/cygdrive/", arr->dir, 10))
         _strlcpy (fbuf, arr->dir, sizeof(fbuf));
    else slashify2 (fbuf, arr->dir, opt.show_unix_paths ? '/' : '\\');

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
      print_raw (fbuf, "~6", NULL);

      attr = GetFileAttributes (arr->dir);
      if ((attr != INVALID_FILE_ATTRIBUTES) &&
          (attr & FILE_ATTRIBUTE_REPARSE_POINT) &&
          get_disk_type(arr->dir[0]) != DRIVE_REMOTE &&
          get_reparse_point (arr->dir, link, sizeof(link)))
      {
        C_puts ("\n      -> ");
        print_raw (_fix_drive(link), "~4", NULL);
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

  C_printf ("~6%2d~0 elements\n", max);
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

  C_printf ("Checking ~3%s\\%s~0:\n", reg_top_key_name(key), REG_APP_PATH);

  build_reg_array_app_path (key);
  max  = smartlist_len (reg_array);
  for (i = errors = 0; i < max; i++)
  {
    const struct registry_array *arr = smartlist_get (reg_array, i);
    char  fqfn [_MAX_PATH+2];
    char  fbuf [_MAX_PATH];

    slashify2 (fbuf, arr->path, opt.show_unix_paths ? '/' : '\\');

    if (opt.verbose)
    {
      char  fbuf2 [_MAX_PATH];
      char *fname;

      C_printf ("   [%2d]: ~6", i);

      snprintf (fbuf2, sizeof(fbuf2), "%s\\%s", arr->path, arr->fname);
      fname = fbuf2;

      if (get_actual_filename(&fname, FALSE))
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

    if (!is_directory(fbuf) && !cfg_ignore_lookup("[Registry]",fbuf))
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
     C_puts ("~2OK~0,             ");
  else
     C_puts ("~5Error~0,          ");

  C_printf ("~6%2d~0 elements\n", max);
  reg_array_free();
}

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
 */
static void compare_user_sys_env (const char *env_var, const char *sys_value, int indent1)
{
  const char *end, *user_value = getenv (env_var);
  char       *sys_val = getenv_expand_sys (sys_value);
  size_t      len, indent2;

  if (sys_val)
     sys_value = sys_val;

  len = strlen (sys_value);
  end = sys_value + len - 1;
  if (IS_SLASH(*end))
  {
    len--;
    end--;
  }
  if (*end == ';')
     len--;

  C_printf ("  ~3%-*s~0", indent1, env_var);
  indent2 = indent1 + sizeof("SYSTEM = ") + 2;

  if (!user_value)
  {
    C_puts ("~6SYSTEM = ");
    print_env_val (sys_value, indent2);
    C_printf ("  %*s~2USER   = <None>~0\n", indent1, "");
  }
  else if (strnicmp(user_value, sys_value, len))
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

/**
 * Compare all environment values for SYSTEM and check if there is a difference
 * in the corresponding USER variable.
 */
static void do_check_user_sys_env (void)
{
  smartlist_t *list;
  int          i, max;
  size_t       len, longest_env = 0;
  char        *env, *val;

  C_puts ("\nComparing ~6SYSTEM~0 and ~2USER~0 environments:\n");

  if (!getenv_system(&list))
  {
    C_printf ("CreateEnvironmentBlock() failed: %s.\n", win_strerror(GetLastError()));
    return;
  }

  TRACE (1, "C_screen_width(): %d\n", (int)C_screen_width());
  max = smartlist_len (list);
  for (i = 0; i < max; i++)
  {
    env = smartlist_get (list, i);
    val = strchr (env, '=');
    *val++ = '\0';
    len = strlen (env);
    if (len > longest_env)
       longest_env = len;
  }

  for (i = 0; i < max; i++)
  {
    env = smartlist_get (list, i);
    val = strchr (env, '\0') + 1;
    compare_user_sys_env (env, val, (int)(2 + longest_env));
  }
  smartlist_free_all (list);
}

/**
 * The handler for mode `"--check"`.
 * Check the Registry keys even if `opt.no_app_path` is set.
 *
 * \struct environ_fspec
 * Check which file-spec in which environment variable.
 */
struct environ_fspec {
       const char *fspec;
       const char *env;
     };

static struct environ_fspec envs[] = {
            { "*.exe",   "PATH"                },
            { "*.lib",   "LIB"                 },
            { "*.a",     "LIBRARY_PATH"        },
            { "*.h",     "INCLUDE"             },
            { "*.h",     "C_INCLUDE_PATH"      },
            { "*",       "CPLUS_INCLUDE_PATH"  },
            { NULL,      "MANPATH"             },
            { NULL,      "PKG_CONFIG_PATH"     },
            { "*.py?",   "PYTHONPATH"          },
            { "*.cmake", "CMAKE_MODULE_PATH"   },
            { "*.pm",    "AUTOM4TE_PERLLIBDIR" },
            { "*.pm",    "PERLLIBDIR"          },
            { NULL,      "CLASSPATH"           },  /* No support for these. But do it anyway. */
            { "*.go",    "GOPATH"              },
            { NULL,      "FOO"                 }   /* Check that non-existing env-vars are also checked */
          };

static int do_check (void)
{
  struct ver_info cmake_ver;
  char *cmake_exe;
  char *sys_env_path = NULL;
  char *sys_env_inc  = NULL;
  char *sys_env_lib  = NULL;
  char  status [100+_MAX_PATH];
  int   i, save, num;
  int   index = 0;

  /* Do not implicitly add current directory in these searches.
   */
  save = opt.no_cwd;
  opt.no_cwd = 1;

  for (i = 0; i < DIM(envs); i++)
  {
    const char *env  = envs[i].env;
    const char *spec = envs[i].fspec;
    int   indent = (int) (sizeof("AUTOM4TE_PERLLIBDIR") - strlen(env));

    C_printf ("Checking ~3%%%s%%~0:%*c", env, indent, ' ');
    if (opt.verbose)
       C_putc ('\n');

    check_env_val (env, spec, &num, status, sizeof(status));
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
    cmake_get_info_registry (NULL, &index, HKEY_CURRENT_USER);
    C_putc ('\n');

    C_printf ("Checking ~3HKEY_LOCAL_MACHINE\\%s~0 keys:\n", KITWARE_REG_NAME);
    cmake_get_info_registry (NULL, &index, HKEY_LOCAL_MACHINE);
    C_putc ('\n');
  }

  opt.no_cwd = save;

  scan_reg_environment (HKEY_LOCAL_MACHINE,
                        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                        &sys_env_path, &sys_env_inc, &sys_env_lib);

  check_env_val_reg (split_env_var(NULL, sys_env_path), "PATH");
  check_env_val_reg (split_env_var(NULL, sys_env_inc), "INCLUDE");
  check_env_val_reg (split_env_var(NULL, sys_env_lib), "LIB");

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
     do_check_user_sys_env();

  FREE (sys_env_path);
  FREE (sys_env_inc);
  FREE (sys_env_lib);
  return (0);
}
