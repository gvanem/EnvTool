/** \file envtool.h
 */
#ifndef _ENVTOOL_H
#define _ENVTOOL_H

#define VER_STRING  "1.5"
#define MAJOR_VER   1
#define MINOR_VER   5

#define AUTHOR_STR    "Gisle Vanem <gvanem@yahoo.no>"
#define GITHUB_STR    "https://github.com/gvanem/EnvTool"

#if defined(_UNICODE) || defined(UNICODE)
#error "This program is not longer UNICODE compatible. Good riddance Microsoft."
#endif

#if defined(IS_ZIG_CC)
  /*
   * No 'Debug' mode in zig-lang
   */
  #define BUILDER  "zig-lang, release"

#elif defined(__INTEL_LLVM_COMPILER)
  #ifdef _DEBUG
    #define BUILDER  "Intel oneAPI DPC++, debug"
  #else
    #define BUILDER  "Intel oneAPI DPC++, release"
  #endif

#elif defined(__clang__)
  #ifdef _DEBUG
    #define BUILDER  "Clang-CL, debug"
  #else
    #define BUILDER  "Clang-CL, release"
  #endif

#elif defined(_MSC_VER)
  #ifdef _DEBUG
    #define BUILDER  "Visual-C, debug"
  #else
    #define BUILDER  "Visual-C, release"
  #endif

#else
  #define BUILDER  "??"
#endif

/* 64-bit Windows targets
 */
#if defined(WIN64) || defined(_WIN64) || defined(_M_X64) || defined(_M_IA64) || defined(_M_AMD64)
  #define WIN_VERSTR  "Win64"
  #define IS_WIN64    1
#else
  #define WIN_VERSTR  "Win32"
  #define IS_WIN64    0
#endif

#if !defined(RC_INVOKED)  /* rest of file */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <ctype.h>
#include <io.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <windows.h>
#include <conio.h>
#include <direct.h>

#define  stat     _stati64

#undef  _MAX_PATH
#define _MAX_PATH 512

#if !defined(_WINSOCK2API_) && !defined(_WINSOCK2_H)
  #define u_long unsigned long
#endif

#if defined(_MSC_VER) && defined(_DEBUG)
  #undef  _malloca          /* Avoid MSVC-9 <malloc.h>/<crtdbg.h> name-clash */
  #define _CRTDBG_MAP_ALLOC
  #include <crtdbg.h>

  /* Use this in `FATAL()` to` avoid huge report of leaks from CrtDbg.
   */
  #define CRTDBG_CHECK_OFF() \
          _CrtSetDbgFlag (~_CRTDBG_LEAK_CHECK_DF & _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG))
#else
  #define CRTDBG_CHECK_OFF()  ((void)0)
#endif

#if defined(__DOXYGEN__)
  #undef  _CRTDBG_MAP_ALLOC
  #define _POSIX_PATH_MAX  256
#endif

#include "getopt_long.h"

#if defined(_DEBUG)
  #define ASSERT(expr) do {                                            \
                         if (!(expr))                                  \
                            FATAL ("Assertion `%s' failed.\n", #expr); \
                       } while (0)
#else
  #define ASSERT(expr) (void) (expr)
#endif

/*
 * Define a '_PRAGMA()' for clang.
 * This includes the Intel LLVM-based compliers too (since '__clang__' is a built-in).
 */
#if defined(__clang__)
  #define _PRAGMA(x) _Pragma (#x)
#else
  #define _PRAGMA(x)
#endif

#if defined(__clang__) && (__clang_major__ >= 10)
  #define FALLTHROUGH()  __attribute__((fallthrough));
#else
  #define FALLTHROUGH()
#endif

#if defined(__clang__) && (__clang_major__ >= 13)
  /*
   * Turn off these:
   *  ./envtool.h(551,14): warning: identifier '_strlcpy' is reserved because it starts with '_'
   *  at global scope [-Wreserved-identifier]
   *  extern char *_strlcpy       (char *dst, const char *src, size_t sz);
   *               ^
   */
  #define _WRESERVED_ID_OFF()  _PRAGMA (clang diagnostic push) \
                               _PRAGMA (clang diagnostic ignored "-Wreserved-identifier")
  #define _WRESERVED_ID_POP()  _PRAGMA (clang diagnostic pop)

  #define _WFUNC_CAST_OFF()    _PRAGMA (clang diagnostic push) \
                               _PRAGMA (clang diagnostic ignored "-Wcast-function-type")
  #define _WFUNC_CAST_POP()    _PRAGMA (clang diagnostic pop)

#else
  #define _WRESERVED_ID_OFF()
  #define _WRESERVED_ID_POP()
  #define _WFUNC_CAST_OFF()
  #define _WFUNC_CAST_POP()
#endif

/*
 * Turn off these annoying warnings in clang-cl >= v13:
 *   win_ver.c(58,24): warning: cast from 'FARPROC' (aka 'long long (*)()') to 'func_NetWkstaGetInfo' (aka 'unsigned long
 *         (*)(unsigned short *, unsigned long, unsigned char **)') converts to incompatible function type [-Wcast-function-type]
 *   p_NetWkstaGetInfo = (func_NetWkstaGetInfo) GetProcAddress (hnd, "NetWkstaGetInfo");
 *                       ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * Use as:
 *   p_NetWkstaGetInfo = GETPROCADDRESS (func_NetWkstaGetInfo, hnd, "NetWkstaGetInfo");
 */
#define GETPROCADDRESS(cast, mod, func) _WFUNC_CAST_OFF()                 \
                                        (cast) GetProcAddress (mod, func) \
                                        _WFUNC_CAST_POP()

/*
 * To turn off these annoying warnings:
 *   misc.c(3552,36):  warning: precision used with 'S' conversion specifier, resulting in undefined behavior [-Wformat]
 *   TRACE (2, "SubstitutionName: '%.*S'\n", (int)(slen/2), sub_name);
 *                                 ~~^~
 */
#define _WFORMAT_OFF()       _PRAGMA (GCC diagnostic push) \
                             _PRAGMA (GCC diagnostic ignored "-Wformat")
#define _WFORMAT_POP()       _PRAGMA (GCC diagnostic pop)

#define _WUNUSED_FUNC_OFF()  _PRAGMA (GCC diagnostic push) \
                             _PRAGMA (GCC diagnostic ignored "-Wunused-function")
#define _WUNUSED_FUNC_POP()  _PRAGMA (GCC diagnostic pop)

/*
 * MSVC (in debug) sometimes returns the full path.
 * Strip the directory part.
 */
#if defined(_MSC_VER)
  #define __FILE()           basename (__FILE__)
  #define snprintf           _snprintf
#else
  #define __FILE()           __FILE__
#endif

#define DIR_SEP            '\\'

#ifndef _S_ISDIR
  #define _S_ISDIR(mode)     (((mode) & _S_IFMT) == _S_IFDIR)
#endif

#ifndef _S_ISREG
  #define _S_ISREG(mode)     (((mode) & _S_IFMT) == _S_IFREG)
#endif

#ifndef _tzname
  #define _tzname tzname
#endif

#define FILE_EXISTS(f)       _file_exists (f)
#define IS_SLASH(c)          ((c) == '\\' || (c) == '/')

#if defined(__clang__)
  #define ATTR_PRINTF(_1,_2) __attribute__((format(printf,_1,_2)))
  #define ATTR_UNUSED()      __attribute__((unused))
  #define WIDESTR_FMT        "S"
#else
  #define ATTR_PRINTF(_1,_2)
  #define ATTR_UNUSED()
  #define WIDESTR_FMT      "ws"
#endif

#define S64_FMT "I64d"
#define U64_FMT "I64u"

#ifndef _I64_MIN
#define _I64_MIN  -9223372036854775806LL
#endif

#ifndef _I64_MAX
#define _I64_MAX  9223372036854775807LL
#endif

#ifndef ALIGN_4
#define ALIGN_4  __declspec (align (4))
#endif

/*
 * Format for printing an hex linear address.
 * E.g. printf (buf, "0x%"ADDR_FMT, ADDR_CAST(ptr));
 */
#if defined(__x86_64__) || defined(_M_X64)     /* 64-bit targets */
  #if defined(__clang__) || defined(__GNUC__)  /* clang or zig */
    #define ADDR_FMT      "016llX"
  #elif defined(_MSC_VER)
    #define ADDR_FMT      "016I64X"
  #else
    #error "Unsupported compiler"
  #endif
  #define ADDR_CAST(x)  ((unsigned long long)(x))

#else                                        /* 32-bit targets */
  #define ADDR_FMT     "08lX"
  #define ADDR_CAST(x)  ((unsigned long)(x))
#endif

/**
 * All MS compilers insists that 'main()', signal-handlers, `atexit()` functions and
 * var-arg functions must be defined as `cdecl`. This is only an issue if a program
 * is using `fastcall` globally (cl option `-Gr`).
 */
#if defined(_MSC_VER)
  #define MS_CDECL __cdecl
#else
  #define MS_CDECL
#endif

#ifndef STDOUT_FILENO
#define STDOUT_FILENO  1
#endif

#ifndef UINT64
#define UINT64  unsigned __int64
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HKEY_PYTHON_PATH               (HKEY) 0x7FF0
#define HKEY_PYTHON_EGG                (HKEY) 0x7FF1
#define HKEY_EVERYTHING                (HKEY) 0x7FF2
#define HKEY_EVERYTHING_ETP            (HKEY) 0x7FF3
#define HKEY_MAN_FILE                  (HKEY) 0x7FF4
#define HKEY_INC_LIB_FILE              (HKEY) 0x7FF5
#define HKEY_PKG_CONFIG_FILE           (HKEY) 0x7FF6
#define HKEY_CMAKE_FILE                (HKEY) 0x7FF7
#define HKEY_LUA_FILE                  (HKEY) 0x7FF8  /* A .lua file */
#define HKEY_LUA_DLL                   (HKEY) 0x7FF9  /* A .dll for Lua */
#define HKEY_LOCAL_MACHINE_SESSION_MAN (HKEY) (HKEY_LOCAL_MACHINE + 0xFF) /* HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment */
#define HKEY_CURRENT_USER_ENV          (HKEY) (HKEY_CURRENT_USER + 0xFF)  /* HKCU\Environment */

/** \enum SignStatus
 *  Used with the `--pe` and `--signed[=0|1]` options to filter PE-files
 *  according to signed-status.
 *  This signed-status is obtained by `wintrust_check()`.
 */
typedef enum SignStatus {

        /** Do not report signed status of any PE-files. <br>
         * I.e. option `"--signed[=0|1]"` was not used.
         */
        SIGN_CHECK_NONE,

        /** Report signed status of all PE-files. <br>
         * I.e. option `"--signed"` was used.
         */
        SIGN_CHECK_ALL,

        /** Report only unsigned PE-files. <br>
         *  I.e. option `"--signed=0"` was used.
         */
        SIGN_CHECK_UNSIGNED,

        /** Report only signed PE-files. <br>
         *  I.e. option `"--signed=1"` was used.
         */
        SIGN_CHECK_SIGNED
      } SignStatus;

#include "sort.h"
#include "smartlist.h"
#include "report.h"

typedef struct beep_info {
        bool      enable;
        unsigned  limit;
        unsigned  freq;
        unsigned  msec;
      } beep_info;

typedef struct grep_info {
        char   *content;       /**< The file-content to search for in `report_grep_file()`. */
        DWORD   num_matches;   /**< The total number of matches found in all files. */
        DWORD   max_matches;   /**< The maximum number of matches to print for each file. */
        DWORD   binary_files;  /**< The number of files detected as binary files. */
        int     only;          /**< Show file(s) that only matches 'content' (not yet). */
      } grep_info;

/**\typedef prog_options
 *
 * All vital program options are set here.
 *
 * Set by `parse_cmdline()` the handlers `set_short_option()` and `set_short_option()`.<br>
 * The handler(s) in `cfg_init()` also sets these program-options.
 */
typedef struct prog_options {
        int             debug;
        int             verbose;
        int             quiet;
        int             fatal_flag;
        int             quotes_warn;
        int             no_cwd;
        int             show_unix_paths;
        int             show_owner;
        smartlist_t    *owners;
        int             show_descr;
        int             decimal_timestamp;
        int             no_sys_env;
        int             no_usr_env;
        int             no_app_path;
        int             no_colours;
        int             no_ansi;
        int             use_cache;
        int             use_regex;
        int             use_buffered_io;
        int             use_nonblock_io;
        int             dir_mode;
        int             lua_mode;
        int             man_mode;
        int             PE_check;
        SignStatus      signed_status;
        int             show_size;
        int             only_32bit;
        int             only_64bit;
        int             gcc_no_prefixed;
        int             no_gcc;
        int             no_gpp;
        int             no_watcom;
        int             no_borland;
        int             no_clang;
        int             no_intel;
        int             no_msvc;
        int             do_tests;
        int             do_evry;
        int             do_version;
        int             do_path;
        int             do_lib;
        int             do_include;
        int             do_python;
        int             do_man;
        int             do_cmake;
        int             do_lua;
        int             do_pkg;
        int             do_vcpkg;
        int             do_check;
        int             do_help;
        int             case_sensitive;
        int             keep_temp;          /**< cmd-line `-k`; do not delete any temporary files from `popen_run_py()` */
        bool            under_conemu;       /**< true if running under ConEmu console-emulator */
        bool            under_winterm;      /**< true if running under WindowsTerminal */
        bool            under_appveyor;     /**< true if running under AppVeyor */
        bool            under_github;       /**< true if running under Github Actions */
        enum SortMethod sort_methods [10];  /**< the specified sort methods */
        bool            force_evry;         /**< force a specific Everything SDK */
        bool            use_evry3;          /**< use Everything SDK 3 functions */
        bool            evry_raw;           /**< use raw non-regex searches */
        UINT            evry_busy_wait;     /**< max number of seconds to wait for a busy EveryThing */
        smartlist_t    *evry_host;
        char           *file_spec;
        beep_info       beep;
        command_line    cmd_line;
        void           *cfg_file;           /**< The config-file structure returned by `cfg_init()`. */
        ULONGLONG       shadow_dtime;       /**< The files delta-time to ignore in `is_shadow_candidate()`. */
        grep_info       grep;
      } prog_options;

extern prog_options opt;
extern char  *program_name;       /* used by getopt_long.c */

extern bool have_sys_native_dir, have_sys_wow64_dir;

extern volatile int halt_flag;

extern char sys_dir        [_MAX_PATH];
extern char sys_native_dir [_MAX_PATH];
extern char sys_wow64_dir  [_MAX_PATH];
extern char current_dir    [_MAX_PATH];
extern int  path_separator;

extern void incr_total_size (UINT64 size);

extern int report_file (report *r);

extern int process_dir (const char *path, int num_dup, bool exist, bool check_empty,
                        bool is_dir, bool exp_ok, const char *prefix, HKEY key);

extern void         print_raw (const char *file, const char *before, const char *after);
extern smartlist_t *split_env_var (const char *env_name, const char *value);

/**
 * \typedef directory_array
 */
typedef struct directory_array {
        char        *dir;         /**< FQFN of this entry */
        char        *cyg_dir;     /**< the Cygwin POSIX form of the above */
        int          exist;       /**< does it exist? */
        int          is_native;   /**< and is it a native dir; like `%WinDir\sysnative` */
        int          is_dir;      /**< and is it a dir; `_S_ISDIR()` */
        int          is_cwd;      /**< and is it equal to `current_dir[]` */
        int          exp_ok;      /**< `expand_env_var()` returned with no `%`? */
        int          num_dup;     /**< is duplicated elsewhere in `%VAR%`? */
        bool         check_empty; /**< check if it contains at least 1 file? */
        bool         done;        /**< alreay processed */
     // char        *env_var;     /**< the env-var these directories came from (or NULL) */
        smartlist_t *dirent2;     /**< List of `struct dirent2` for this directory; used in `check_shadow_files()` only */
      } directory_array;

/**
 * \typedef registry_array
 */
typedef struct registry_array {
        char       *fname;        /**< basename of this entry. I.e. the name of the enumerated key. */
        char       *real_fname;   /**< normally the same as above unless aliased. E.g. "winzip.exe -> "winzip32.exe" */
        char       *path;         /**< path of this entry */
        int         exist;        /**< does it exist? */
        time_t      mtime;        /**< file modification time */
        UINT64      fsize;        /**< file size */
        HKEY        key;          /**< The `top_key` used in `RegOpenKeyEx()` */
      } registry_array;

/**
 * Interface functions for the `dir_array` and `reg_array` smartlists in envtool.c.
 */
extern directory_array *dir_array_add (const char *dir, bool is_cwd);
extern registry_array  *reg_array_add (HKEY key, const char *fname, const char *fqfn);

extern smartlist_t *dir_array_head (void);
extern void         dir_array_free (void);
extern void         dir_array_wiper (void *);

extern smartlist_t *reg_array_head (void);
extern void         reg_array_free (void);

extern smartlist_t *get_matching_files (const char *dir, const char *file_spec);
extern int          do_check_env (const char *env_name);

/**
 * \def REG_APP_PATH
 * The Registry key under `HKEY_CURRENT_USER` or `HKEY_LOCAL_MACHINE`.
 */
#define REG_APP_PATH    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths"

/**
 * \struct shadow_entry
 * Structure used in `envtool --check -v`.
 */
struct shadow_entry {
       char     *shadowed_file;        /**< the file being shadowed by an older file earlier in an env-var. */
       char     *shadowing_file;       /**< the file that shadows for the newer file. */
       FILETIME  shadowed_FILE_TIME;   /**< dirent2::d_time_create or dirent2::d_time_write. */
       FILETIME  shadowing_FILE_TIME;  /**< dirent2::d_time_create or dirent2::d_time_write. */
     };

/*
 * Defined in newer <sal.h> for MSVC.
 */
#ifndef _Printf_format_string_
#define _Printf_format_string_
#endif

/* Stuff in misc.c:
 */
int debug_printf (_Printf_format_string_ const char *format, ...) ATTR_PRINTF (1,2);

/*
 * According to:
 *  https://devblogs.microsoft.com/oldnewthing/20100203-00/?p=15083
 *  https://learn.microsoft.com/en-gb/windows/win32/api/processenv/nf-processenv-getenvironmentvariablea
 */
#define MAX_ENV_VAR  32767

#if defined(__clang__)
  #define strdupa(s)  (__extension__ ({ \
                       const char *s_in = (s);                \
                       size_t      s_len = strlen (s_in) + 1; \
                       char       *s_out = alloca (s_len);    \
                       (char*) memcpy (s_out, s_in, s_len);   \
                      }))
#else
  #define strdupa(s)  strcpy (alloca(strlen(s) + 1), s)
#endif

extern void   init_misc     (void);
extern void   exit_misc     (void);

extern char *_strlcpy       (char *dst, const char *src, size_t sz);
extern char *_strtok_r      (char *ptr, const char *sep, char **end);

extern char *str_sep        (char **s, const char *delim);
extern char *str_acat       (char *s1, const char *s2);
extern int   str_cat        (char *dst, size_t dst_size, const char *src);
extern char *str_ndup       (const char *s, size_t sz);
extern char *str_nstr       (const char *s1, const char *s2, size_t len);
extern void *str_memmem     (const void *m1, size_t m1_len, const void *m2, size_t m2_len);
extern char *str_join       (char *const *arr, const char *sep);
extern char *str_strip_nl   (char *s);
extern char *str_ltrim      (char *s);
extern char *str_rtrim      (char *s);
extern char *str_trim       (char *s);
extern bool  str_startswith (const char *s, const char *with);
extern bool  str_endswith   (const char *s, const char *with);
extern bool  str_match      (const char *s, const char *what, char **next);
extern char *str_repeat     (int ch, size_t num);
extern char *str_replace    (int ch1, int ch2, char *str);
extern char *str_replace2   (int ch, const char *s, char *str, size_t max_size);
extern char *str_unquote    (char *s);
extern int   str_isquoted   (const char *s);
extern char *str_reverse    (char *str);
extern int   str_equal      (const char *s1, const char *s2);
extern int   str_equal_n    (const char *s1, const char *s2, size_t len);
extern char *str_shorten    (const char *str, size_t max_len);
extern char *str_qword      (UINT64 val);
extern char *str_dword      (DWORD val);
extern char *str_plural     (DWORD val, const char *singular, const char *plural);

extern char *searchpath     (const char *file, const char *env_var);
extern int   searchpath_pos (void);
extern char *_fix_uuid (const char *uuid, char *result);
extern char *_fix_path      (const char *path, char *result);
extern char *_fix_drive     (char *path);
extern char *path_ltrim     (const char *p1, const char *p2);
extern char *basename       (const char *fname);
extern char *dirname        (const char *fname);
extern char *slashify       (const char *path, char use);
extern char *slashify2      (char *buf, const char *path, char use);
extern char *win_strerror   (unsigned long err);
extern char *ws2_strerror   (int err);
extern bool  mbchar_to_wchar(wchar_t *result, size_t result_size, const char    *a_buf);
extern bool  wchar_to_mbchar(char    *result, size_t result_size, const wchar_t *w_buf);
extern char *fopen_mem      (const char *file, size_t *_f_size);

extern void  set_error_mode (int on_off);
extern int  _file_exists    (const char *file);

extern char *evry_raw_query    (void);
extern char *getenv_expand     (const char *variable);
extern char *getenv_expand2    (const char *variable);
extern char *getenv_expand_sys (const char *variable);
extern bool  getenv_system     (smartlist_t **sl);

extern UINT   get_disk_type         (int disk);
extern bool   get_disk_cluster_size (int disk, DWORD *size);
extern UINT64 get_directory_size    (const char *dir);
extern UINT64 get_file_alloc_size   (const char *file, UINT64 size);
extern bool   get_file_compr_size   (const char *file, UINT64 *fsize);
extern int    disk_ready            (int disk);
extern bool   chk_disk_ready        (int disk);
extern bool  _has_drive             (const char *path);
extern bool  _has_drive2            (const char *path);
extern bool   is_directory          (const char *file);
extern int    safe_stat             (const char *file, struct stat *st, DWORD *win_err);
extern int    safe_stat_sys         (const char *file, struct stat *st, DWORD *win_err);
extern UINT   count_digit           (UINT64 n);
extern bool   legal_file_name       (const char *fname);

extern char       *make_cyg_path (const char *path, char *result);
extern wchar_t    *make_cyg_pathw (const wchar_t *path, wchar_t *result);

extern const char *get_user_name (void);
extern bool        is_user_admin (void);
extern bool        is_user_admin2 (void);
extern int         is_cygwin_tty (int fd);
extern bool        print_core_temp_info (void);
extern bool        print_user_cet_info (void);
extern void        spinner_start (void);
extern void        spinner_stop (void);
extern void        spinner_pause (bool on_off);

extern void        format_and_print_line (const char *line, int indent);
extern void        print_long_line (const char *line, size_t indent);
extern void        print_long_line2 (const char *line, size_t indent, int break_at);
extern char       *translate_shell_pattern (const char *pattern);
extern void        test_shell_pattern (void);
extern void        hex_dump (int dbg_level, const char *intro, const void *data_p, size_t datalen);
extern const char *dump10 (const void *data_p, unsigned size);
extern bool        get_module_filename_ex (HANDLE proc, char *filename);
extern time_t      FILETIME_to_time_t (const FILETIME *ft);

/* Fix for `_MSC_VER <= 1800` (Visual Studio 2012 or older) which is lacking `vsscanf()`.
 */
#if (defined(_MSC_VER) && (_MSC_VER <= 1800) && !defined(_VCRUNTIME_H))
  int _vsscanf2 (const char *buf, const char *fmt, va_list args);
  #define vsscanf(buf, fmt, args) _vsscanf2 (buf, fmt, args)
#endif

/* Windows security related functions for files/directories:
 */
extern bool get_file_owner (const char *file, char **domain_name, char **account_name);
extern bool is_directory_accessible (const char *path, DWORD access);
extern bool is_directory_readable (const char *path);
extern bool is_directory_writable (const char *path);


/** Functions for handling Reparse Points:
 *  (Junctions and Symlinks).
 */
extern const char *last_reparse_err;
extern bool        get_reparse_point (const char *dir, char *result, size_t result_size);
extern bool        is_special_link (void);

/** \typedef struct ver_info
 *
 * Generic program version information (in resource).
 *
 *  Implemented by        | For what
 * -----------------------|---------------------------
 *  get_python_info()     | Supported Python programs.
 *  get_evry_version()    | EveryThing File-database.
 *  get_PE_version_info() | PE-image version info.
 */
typedef struct ver_info {
        unsigned val_1;   /**< Major */
        unsigned val_2;   /**< Minor */
        unsigned val_3;   /**< Micro */
        unsigned val_4;   /**< Build (unused in envtool_py.c) */
      } ver_info;

/** \def VALID_VER
 *  Check if ver was filled okay.
 *  Accept programs with major-version == 0. Hence assume if `major+minor == 0`,
 *  the version was not correctly set.
 */
#define VALID_VER(ver) (ver.val_1 + ver.val_2 > 0)

/** \typedef enum ver_index
 * The 5 programs we get version-information from on "envtool -V" command.
 */
typedef enum ver_index {
        VER_CMAKE,
        VER_PYTHON,
        VER_PKG_CONFIG,
        VER_VCPKG,
        VER_LUA,
        VER_MAX
      } ver_index;

/** \typedef struct ver_data
 * The version-information for each program filled on a "envtool -V" command.
 */
typedef struct ver_data {
        const char *found_fmt;
        const char *not_found;
        char       *exe;
        char        slash;
        char        found [100];
        int         len;
        ver_info    version;
        bool      (*get_info) (char **exe, struct ver_info *ver);
        void      (*extras) (const struct ver_data *v, int pad_len);
      } ver_data;

/** \typedef search_list
 *  A generic search-list type.
 */
typedef struct search_list {
        unsigned    value;  /**< the value */
        const char *name;   /**< the name of the associated value */
      } search_list;

/** \enum Bitness
 *  Used by check_if_PE() to retrieve the bitness of a PE-file.
 */
enum Bitness {
     bit_unknown = 0, /**< the bitness is unknown (not a PE-file?). */
     bit_16,          /**< 16-bit MSDOS file; never really set. */
     bit_32,          /**< 32-bit PE-file. */
     bit_64           /**< 64-bit PE-file. */
   };

extern const char *list_lookup_name (unsigned value, const search_list *list, int num);
extern unsigned    list_lookup_value (const char *name, const search_list *list, int num);
extern const char *flags_decode (DWORD flags, const search_list *list, int num);
extern const char *get_file_size_str (UINT64 size);
extern const char *get_time_str (time_t t);
extern const char *get_time_str_FILETIME (const FILETIME *ft);
extern const char *get_file_ext (const char *file);
extern char       *create_temp_file (void);
extern const char *check_if_shebang (const char *fname);
extern bool        check_if_zip (const char *fname);
extern bool        check_if_gzip (const char *fname);
extern bool        check_if_cwd_in_search_path (const char *program);
extern bool        check_if_shell_link (const char *fname);
extern const char *get_gzip_link (const char *file);
extern const char *get_man_link (const char *file);
extern const char *get_sym_link (const char *file);
extern const char *get_shell_link (const char *file);
extern bool        check_if_PE (const char *fname, enum Bitness *bits);
extern bool        verify_PE_checksum (const char *fname);
extern bool        is_wow64_active (void);

extern REGSAM      reg_read_access (void);
extern const char *reg_type_name (DWORD type);
extern const char *reg_top_key_name (HKEY key);
extern const char *reg_access_name (REGSAM acc);
extern DWORD       reg_swap_long (DWORD val);

extern void crtdbug_init (void);
extern void crtdbug_exit (void);

/* Functions in show_ver.c
 */
extern int    get_PE_version_info (const char *file, struct ver_info *ver);
extern char  *get_PE_version_info_buf (void);
extern void   get_PE_version_info_free (void);

/** \typedef FMT_buf
 *  The type used in formatting a string-buffer on the stack.<br>
 *  Used by buf_printf(), buf_puts() and buf_putc().
 */
typedef struct FMT_buf {
        char   *buffer;        /**< the `alloca()` (or `malloc()`) buffer */
        char   *buffer_start;  /**< as above + 4 bytes (past the marker) */
        char   *buffer_pos;    /**< current position in the buffer */
        size_t  buffer_size;   /**< number of bytes allocated in the buffer */
        size_t  buffer_left;   /**< number of bytes left in the buffer */
        bool    on_heap;       /**< `buffer` is on the heap (not the stack) */
      } FMT_buf;

/** \def FMT_BUF_MARKER
 *  A magic marker to detect under/over-run of a FMT_buf.
 */
#define FMT_BUF_MARKER  0xDEAFBABE

/** \def BUF_INIT()
 *  Macro to setup the FMT_buf on stack or on the heap.
 *   \param fmt_buf   The buffer-structure to initialise.
 *   \param size      The size to allocate for the maximum string.
 *                    4 bytes are added to this to fit the magic markers.
 *   \param use_heap  If true, allocate using `MALLOC()`. Otherwise use `alloca()`.
 */
#define BUF_INIT(fmt_buf, size, use_heap) do {                    \
        DWORD   *_marker;                                         \
        FMT_buf *_buf = fmt_buf;                                  \
        size_t   _sz = size + 2*sizeof(DWORD);                    \
                                                                  \
        _buf->on_heap = use_heap;                                 \
        _buf->buffer  = use_heap ? MALLOC (_sz) : alloca (_sz);   \
        _marker  = (DWORD*) _buf->buffer;                         \
        *_marker = FMT_BUF_MARKER;                                \
        _marker  = (DWORD*) (_buf->buffer + _sz - sizeof(DWORD)); \
        *_marker = FMT_BUF_MARKER;                                \
        _buf->buffer_start   = _buf->buffer + sizeof(DWORD);      \
        _buf->buffer_pos     = _buf->buffer_start;                \
        _buf->buffer_size    = size;                              \
        _buf->buffer_left    = size;                              \
        _buf->buffer_pos [0] = '\0';                              \
      } while (0)

#define BUF_FREE(fmt_buf) do {   \
        FMT_buf *_buf = fmt_buf; \
        if (_buf->on_heap)       \
           FREE (_buf->buffer);  \
      } while (0)

int  buf_printf         (const char *file, unsigned line, FMT_buf *fmt_buf, _Printf_format_string_ const char *format, ...) ATTR_PRINTF (4,5);
void buf_puts_long_line (const char *file, unsigned line, FMT_buf *fmt_buf, const char *str, size_t indent);
int  buf_puts           (const char *file, unsigned line, FMT_buf *fmt_buf, const char *string);
int  buf_putc           (const char *file, unsigned line, FMT_buf *fmt_buf, int ch);
void buf_reset          (FMT_buf *fmt_buf);

#define BUF_PRINTF(fmt_buf, format, ...)          buf_printf (__FILE__, __LINE__, fmt_buf, format, __VA_ARGS__)
#define BUF_PUTS_LONG_LINE(fmt_buf, str, indent)  buf_puts_long_line (__FILE__, __LINE__, fmt_buf, str, indent)
#define BUF_PUTS(fmt_buf, str)                    buf_puts (__FILE__, __LINE__, fmt_buf, str)
#define BUF_PUTC(fmt_buf, ch)                     buf_putc (__FILE__, __LINE__, fmt_buf, ch)


/* Stuff in win_ver.c:
 */
extern const char *os_name (void);
extern const char *os_bits (void);
extern const char *os_release_id (void);
extern const char *os_update_build_rev (void);
extern const char *os_current_build (void);
extern const char *os_full_version (void);
extern const char *os_KUSER_SHARED_DATA (void);
extern time_t      os_last_install_date (void);
extern time_t      os_first_install_date (void);

/* Stuff in win_trust.c:
 *
 * Support older SDKs
 */
#ifndef TRUST_E_NOSIGNATURE
#define TRUST_E_NOSIGNATURE          (HRESULT) 0x800B0100
#endif

#ifndef TRUST_E_PROVIDER_UNKNOWN
#define TRUST_E_PROVIDER_UNKNOWN     (HRESULT) 0x800B0001
#endif

#ifndef TRUST_E_SUBJECT_NOT_TRUSTED
#define TRUST_E_SUBJECT_NOT_TRUSTED  (HRESULT) 0x800B0004
#endif

#ifndef TRUST_E_SUBJECT_FORM_UNKNOWN
#define TRUST_E_SUBJECT_FORM_UNKNOWN (HRESULT) 0x800B0003
#endif

extern char       *wintrust_signer_subject,    *wintrust_signer_issuer;
extern char       *wintrust_timestamp_subject, *wintrust_timestamp_issuer;
extern DWORD       wintrust_check (const char *pe_file, bool details, bool revoke_check);
extern const char *wintrust_check_result (DWORD rc);
extern void        wintrust_cleanup (void);
extern bool        wintrust_dump_pkcs7_cert (void);

/* Simple debug-malloc functions:
 */
#define MEM_MARKER  0xDEAFBABE
#define MEM_FREED   0xDEADBEAF

extern void    *malloc_at  (size_t size, const char *file, unsigned line);
extern void    *calloc_at  (size_t num, size_t size, const char *file, unsigned line);
extern void    *realloc_at (void *ptr, size_t size, const char *file, unsigned line);
extern char    *strdup_at  (const char *str, const char *file, unsigned line);
extern wchar_t *wcsdup_at  (const wchar_t *str, const char *file, unsigned line);
extern void     free_at    (void *ptr, const char *file, unsigned line);
extern void     mem_report (void);

#if defined(_CRTDBG_MAP_ALLOC)
  #define MALLOC        malloc
  #define CALLOC        calloc
  #define REALLOC       realloc
  #define STRDUP        strdup
  #define WCSDUP        wcsdup
  #define FREE(p)       (p ? (void) (free(p), p = NULL) : (void)0)  // -V595
#else
  #define MALLOC(s)     malloc_at (s, __FILE(), __LINE__)
  #define CALLOC(n,s)   calloc_at (n, s, __FILE(), __LINE__)
  #define REALLOC(p,s)  realloc_at (p, s, __FILE(), __LINE__)
  #define STRDUP(s)     strdup_at (s, __FILE(), __LINE__)
  #define WCSDUP(s)     wcsdup_at (s, __FILE(), __LINE__)
  #define FREE(p)       (p ? (void) (free_at((void*)p, __FILE(), __LINE__), p = NULL) : (void)0)
#endif

/* Wrapper for popen().
 */
typedef int (*popen_callback) (char *buf, int index);

int   popen_run  (popen_callback callback, const char *cmd, const char *arg, ...);
DWORD popen_run2 (popen_callback callback, const char *cmd, const char *arg, ...);
char *popen_last_line (void);

/* fnmatch() ret-values and flags:
 */
#define FNM_MATCH          1
#define FNM_NOMATCH        0

#define FNM_FLAG_NOESCAPE  0x01
#define FNM_FLAG_PATHNAME  0x02
#define FNM_FLAG_NOCASE    0x04

extern int   fnmatch      (const char *pattern, const char *string, int flags);
extern int   fnmatch_case (int flags);
extern char *fnmatch_res  (int rc);

/* Handy macros: */

/** \def DIM(arr)
 *   Return the number of elements in an arbitrary array.
 *   \param arr the array.
 *
 * \def ARGSUSED(foo)
 *   Avoid a compiler warning on an used variable.
 *   \param foo the variable.
 *
 * \def TRACE(level, ...)
 *   Print a debug-statement if opt.debug >= level.
 *   \param level  the level needed to trigger a print-out.
 *   \param ...    a var-arg list of format an arguments (just like `printf()`).
 *
 * \def TRACE_NL(level)
 *   Print a newline if opt.debug >= level.
 *
 * \def WARN(...)
 *   Unless "--quiet" option was used, print a warning in red colour.
 *   \param ...   a var-arg list of format an arguments (just like `printf()`).
 *
 * \def FATAL(...)
 *   Print a fatal message on `stderr` and exit the program.
 *   \param ...   a var-arg list of format an arguments (just like `printf()`).
 */
#define DIM(arr)       (int) (sizeof(arr) / sizeof(arr[0]))
#define ARGSUSED(foo)  (void)foo

#define TRACE(level, ...)  do {                                    \
                             if (opt.debug >= level) {             \
                                debug_printf ("%s(%d): ",          \
                                              __FILE(), __LINE__); \
                                debug_printf (__VA_ARGS__);        \
                             }                                     \
                           } while (0)

#define TRACE_NL(level)    do {                      \
                             if (opt.debug >= level) \
                                C_putc ('\n');       \
                           } while (0)

#define WARN(...)           do {                                        \
                              if (!opt.quiet) {                         \
                                 C_puts ("~5");                         \
                                 C_printf (__VA_ARGS__);                \
                                 C_puts ("~0");                         \
                                 C_flush();                             \
                              }                                         \
                            } while (0)

/*
 * As above, but must be used with a local 'ignore' variable:
 */
#define WARN2(...)          do {                                        \
                              if (!ignore && !opt.quiet) {              \
                                 C_puts ("~5");                         \
                                 C_printf (__VA_ARGS__);                \
                                 C_puts ("~0");                         \
                                 C_flush();                             \
                              }                                         \
                            } while (0)

#define FATAL(...)          do {                                        \
                              CRTDBG_CHECK_OFF();                       \
                              fflush (stdout);                          \
                              fprintf (stderr, "\nFatal: %s(%d): ",     \
                                       __FILE(), __LINE__);             \
                              fprintf (stderr, ##__VA_ARGS__);          \
                              opt.fatal_flag = 1;                       \
                              if (IsDebuggerPresent())                  \
                                   abort();                             \
                              else ExitProcess (GetCurrentProcessId()); \
                            } while (0)


#define FAST_EXIT()         do {                                        \
                              CRTDBG_CHECK_OFF();                       \
                              fflush (stdout);                          \
                              opt.fatal_flag = 1;                       \
                              if (IsDebuggerPresent())                  \
                                   abort();                             \
                              else ExitProcess (GetCurrentProcessId()); \
                            } while (0)

#ifdef __cplusplus
}
#endif

#endif /* RC_INVOKED */
#endif
