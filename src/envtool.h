/** \file envtool.h
 */
#ifndef _ENVTOOL_H
#define _ENVTOOL_H

#define VER_STRING  "1.3"
#define MAJOR_VER   1
#define MINOR_VER   3

#define AUTHOR_STR    "Gisle Vanem <gvanem@yahoo.no>"
#define GITHUB_STR    "https://github.com/gvanem/EnvTool"

#if defined(_UNICODE) || defined(UNICODE)
#error This program is not longer UNICODE compatible. Good riddance Microsoft.
#endif

/*
 * PellesC has '_MSC_VER' as a built-in (if option '-Ze' is used).
 * Hence test for '__POCC__' before '_MSC_VER'.
 */
#if defined(__POCC__)
  #ifdef _DEBUG
    #define BUILDER  "PellesC, debug"
  #else
    #define BUILDER  "PellesC, release"
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

#elif defined(__CYGWIN__)
  #define BUILDER  "CygWin"

#elif defined(__MINGW64_VERSION_MAJOR)
  #define BUILDER  "MinGW64/TDM-gcc"

#elif defined(__MINGW32__)
  #define BUILDER  "MinGW"

#elif defined(__WATCOMC__)
  #define BUILDER  "OpenWatcom"

#else
  #define BUILDER  "??"
#endif

/* 64-bit Windows targets
 */
#if defined(WIN64) || defined(_WIN64) || defined(_M_X64) || defined(_M_IA64) || defined(_M_AMD64) || defined(__MINGW64__)
  #define WIN_VERSTR  "Win64"
  #define IS_WIN64    1
#else
  #define WIN_VERSTR  "Win32"
  #define IS_WIN64    0
#endif

#if !defined(RC_INVOKED)  /* rest of file */

#if (defined(__MINGW32__) || defined(__CYGWIN__)) && defined(_FORTIFY_SOURCE)
  /*
   * Enable GNU LibSSP; "Stack Smashing Protector".
   *   Ref: http://aconole.brad-x.com/papers/exploits/ssp/intro
   */
  #if (_FORTIFY_SOURCE == 1) && defined(INSIDE_ENVTOOL_C)
    #pragma message ("Using _FORTIFY_SOURCE=1")
  #elif (_FORTIFY_SOURCE == 2) && defined(INSIDE_ENVTOOL_C)
    #pragma message ("Using _FORTIFY_SOURCE=2")
  #endif

  #include <ssp/stdio.h>
  #include <ssp/string.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <ctype.h>
#include <io.h>
#include <limits.h>
#include <sys/stat.h>
#include <windows.h>

#if defined(__CYGWIN__)
 /*
  * <limits.h> defines PATH_MAX to 4096. That seems excessive for our use.
  * Note that 'long' on CygWin64 is 8 bytes. Hence the cast using '(u_long)'
  * in many places where 'printf ("%lu", dword_value)' is used. A 'DWORD'
  * on CygWin64 is still 32-bit.
  */
  #include <unistd.h>
  #include <alloca.h>
  #include <wchar.h>
  #include <sys/types.h>
  #include <sys/cygwin.h>

  #define _MAX_PATH               _POSIX_PATH_MAX   /* 256 */
  #define _S_ISDIR(mode)          S_ISDIR (mode)
  #define _S_ISREG(mode)          S_ISREG (mode)
  #define _popen(cmd, mode)       popen (cmd, mode)
  #define _pclose(fil)            pclose (fil)
  #define _fileno(f)              fileno (f)
  #define _isatty(fd)             isatty (fd)
  #define _tempnam(dir,prefix)    tempnam (dir,prefix)
  #define _wcsdup(s)              wcsdup (s)

  #define stricmp(s1, s2)         strcasecmp (s1, s2)
  #define strnicmp(s1, s2, len)   strncasecmp (s1, s2, len)
  #define DEV_NULL                "/dev/null"

  extern char  *_itoa (int value, char *buf, int radix);
  extern UINT64 filelength (int fd);

#else
  #include <direct.h>
  #define  stat     _stati64
  #define  DEV_NULL "NUL"

  #undef  _MAX_PATH
  #define _MAX_PATH 512

  #if !defined(_WINSOCK2API_) && !defined(_WINSOCK2_H) && !defined(_BSDTYPES_DEFINED)
    #define u_long unsigned long
    #define _BSDTYPES_DEFINED
  #endif
#endif

#if defined(__POCC__)
  #include <wctype.h>
  #include <wchar.h>

  #define __FUNCTION__    __func__
  #undef  stat
  #define tzset()        ((void)0)

  /*
   * Lots of these:
   *   warning #1058: Invalid token produced by ##, from ',' and 'env_name'.
   */
  #pragma warn (disable: 1058)

  /*
   * warning #2130: Result of comparison is constant.
   */
  #pragma warn (disable: 2130)

#elif defined(_MSC_VER) && defined(_DEBUG)
  #undef  _malloca          /* Avoid MSVC-9 <malloc.h>/<crtdbg.h> name-clash */
  #define _CRTDBG_MAP_ALLOC
  #include <crtdbg.h>

  /* Use this in `FATAL()` to` avoid huge report of leaks from CrtDbg.
   */
  #define CRTDBG_CHECK_OFF() \
          _CrtSetDbgFlag (~_CRTDBG_LEAK_CHECK_DF & _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG))
#endif

#ifndef CRTDBG_CHECK_OFF
#define CRTDBG_CHECK_OFF()
#endif

/* Using "Visual Leak Detector" in _RELEASE mode is possible via the
 * '-DVLD_FORCE_ENABLE' flag. But not advisable according to:
 * https://github.com/KindDragon/vld/wiki
 *
 * VLD is useful for '_MSC_VER' only.
 */
#if defined(USE_VLD)
#include <vld.h>
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
 * '_Pragma()' stuff for 'gcc' and 'clang'.
 */
#if defined(__GNUC__)
  #define GCC_VERSION  (10000 * __GNUC__ + 100 * __GNUC_MINOR__ + __GNUC_PATCHLEVEL__)
#else
  #define GCC_VERSION  0
#endif

#if defined(__clang__) || (GCC_VERSION >= 40600)
  #define _PRAGMA(x) _Pragma (#x)
#else
  #define _PRAGMA(x)
#endif

#if defined(__clang__) && (__clang_major__ >= 10)
  #define FALLTHROUGH()  __attribute__((fallthrough));
#else
  #define FALLTHROUGH()
#endif


/*
 * To turn off these annoying warnings:
 *   misc.c(3552,36):  warning: precision used with 'S' conversion specifier, resulting in undefined behavior [-Wformat]
 *   DEBUGF (2, "SubstitutionName: '%.*S'\n", (int)(slen/2), sub_name);
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
#if defined(_MSC_VER) && !defined(__POCC__)
  #define __FILE()           basename (__FILE__)
  #define snprintf           _snprintf
#else
  #define __FILE()           __FILE__
#endif

#if defined(__CYGWIN__)
  #define DIR_SEP            '/'
#else
  #define DIR_SEP            '\\'
#endif

#ifndef _S_ISDIR
  #define _S_ISDIR(mode)     (((mode) & _S_IFMT) == _S_IFDIR)
#endif

#ifndef _S_ISREG
  #define _S_ISREG(mode)     (((mode) & _S_IFMT) == _S_IFREG)
#endif

#define FILE_EXISTS(f)       _file_exists (f)
#define IS_SLASH(c)          ((c) == '\\' || (c) == '/')

#if defined(__GNUC__) || defined(__clang__)
  #define ATTR_PRINTF(_1,_2) __attribute__((format(printf,_1,_2)))
  #define ATTR_UNUSED()      __attribute__((unused))
  #define WIDESTR_FMT        "S"
#else
  #define ATTR_PRINTF(_1,_2)
  #define ATTR_UNUSED()

  #ifdef __POCC__
    #define WIDESTR_FMT      "ls"
  #else
    #define WIDESTR_FMT      "ws"
  #endif
#endif

#ifdef __CYGWIN__
  #define S64_FMT "lld"
  #define U64_FMT "llu"
#else
  #define S64_FMT "I64d"
  #define U64_FMT "I64u"
#endif

/*
 * Format for printing an hex linear address.
 * E.g. printf (buf, "0x%"ADDR_FMT, ADDR_CAST(ptr));
 */
#if defined(__x86_64__) || defined(_M_X64)   /* 64-bit targets */
  #if defined(_MSC_VER) || defined(__MINGW32__)
    #define ADDR_FMT      "016I64X"
  #elif defined(__CYGWIN__)                  /* CygWin64 */
    #define ADDR_FMT     "016llX"
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
#if defined(_MSC_VER) && !defined(__POCC__)
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

typedef struct beep_info {
        BOOL      enable;
        unsigned  limit;
        unsigned  freq;
        unsigned  msec;
      } beep_info;

/** struct prog_options
 *
 * All vital program options are set here.
 *
 * Set by `parse_cmdline()` and the handlers `set_short_option()`
 * and `set_short_option()`.
 */
struct prog_options {
       int             debug;
       int             verbose;
       int             quiet;
       int             fatal_flag;
       int             quotes_warn;
       int             no_cwd;
       int             show_unix_paths;
       int             show_owner;
       int             show_descr;
       smartlist_t    *owners;
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
       int             man_mode;
       int             PE_check;
       SignStatus      signed_status;
       int             help;
       int             show_size;
       int             only_32bit;
       int             only_64bit;
       int             gcc_no_prefixed;
       int             no_gcc;
       int             no_gpp;
       int             no_watcom;
       int             no_borland;
       int             no_clang;
       int             do_tests;
       int             do_evry;
       int             do_version;
       int             do_path;
       int             do_lib;
       int             do_include;
       int             do_python;
       int             do_man;
       int             do_cmake;
       int             do_pkg;
       int             do_vcpkg;
       int             do_check;
       int             conv_cygdrive;
       int             case_sensitive;
       int             keep_temp;         /**< cmd-line `-k`; do not delete any temporary files from `popen_run_py()` */
       int             under_conemu;      /**< TRUE if running under ConEmu console-emulator */
       int             under_appveyor;    /**< TRUE if running under AppVeyor */
       enum SortMethod sort_methods[10];  /**< the specified sort methods */
       BOOL            evry_raw;          /**< use raw non-regex searches */
       smartlist_t    *evry_host;
       char           *file_spec;
       beep_info       beep;
       command_line    cmd_line;
       void           *cfg_file;          /**< The config-file structure returned by `cfg_init()`. */
       ULONGLONG       shadow_dtime;      /**< The files delta-time to ignore in `is_shadow_candidate()`. */
     };

extern struct prog_options opt;
extern char  *program_name;       /* used by getopt_long.c */
extern HANDLE Everything_hthread; /* set by Everything.c */

extern BOOL have_sys_native_dir, have_sys_wow64_dir;

extern volatile int halt_flag;

extern char sys_dir        [_MAX_PATH];
extern char sys_native_dir [_MAX_PATH];
extern char sys_wow64_dir  [_MAX_PATH];
extern int  path_separator;

extern void incr_total_size (UINT64 size);

extern void set_report_header (const char *fmt, ...);

extern int  report_file (const char *file, time_t mtime, UINT64 fsize,
                         BOOL is_dir, BOOL is_junction, HKEY key);

extern int  process_dir (const char *path, int num_dup, BOOL exist, BOOL check_empty,
                         BOOL is_dir, BOOL exp_ok, const char *prefix, HKEY key, BOOL recursive);

extern void         print_raw (const char *file, const char *before, const char *after);
extern smartlist_t *split_env_var (const char *env_name, const char *value);

/**
 * \struct directory_array
 */
struct directory_array {
       char        *dir;         /**< FQDN of this entry */
       char        *cyg_dir;     /**< The Cygwin POSIX form of the above */
       int          exist;       /**< does it exist? */
       int          is_native;   /**< and is it a native dir; like `%WinDir\sysnative` */
       int          is_dir;      /**< and is it a dir; `_S_ISDIR()` */
       int          is_cwd;      /**< and is it equal to `current_dir[]` */
       int          exp_ok;      /**< `expand_env_var()` returned with no `%`? */
       int          num_dup;     /**< is duplicated elsewhere in `%VAR%`? */
       BOOL         check_empty; /**< check if it contains at least 1 file? */
       unsigned     line;        /**< Debug: at what line was `add_to_dir_array()` called */
       smartlist_t *dirent2;     /**< List of `struct dirent2` for this directory; used in `check_shadow_files()` only */
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

extern void free_reg_array (void);
extern void free_dir_array (void);

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
#if defined(__POCC__)
_CRTCHK(printf,1,2)
#endif
int debug_printf (_Printf_format_string_ const char *format, ...) ATTR_PRINTF (1,2);

/*
 * According to:
 *  http://msdn.microsoft.com/en-us/library/windows/desktop/ms683188(v=vs.85).aspx
 */
#define MAX_ENV_VAR  32767

extern void   init_misc     (void);
extern void   exit_misc     (void);

extern char *_strlcpy       (char *dst, const char *src, size_t sz);
extern char *_strtok_r      (char *ptr, const char *sep, char **end);

extern char *str_sep        (char **s, const char *delim);
extern char *str_acat       (char *s1, const char *s2);
extern int   str_cat        (char *dst, size_t dst_size, const char *src);
extern char *str_ndup       (const char *s, size_t sz);
extern char *str_join       (char *const *arr, const char *sep);
extern char *str_strip_nl   (char *s);
extern char *str_ltrim      (char *s);
extern char *str_rtrim      (char *s);
extern char *str_trim       (char *s);
extern BOOL  str_startswith (const char *s, const char *with);
extern BOOL  str_endswith   (const char *s, const char *with);
extern BOOL  str_match      (const char *s, const char *what, char **next);
extern char *str_repeat     (int ch, size_t num);
extern char *str_replace    (int ch1, int ch2, char *str);
extern char *str_replace2   (int ch, const char *s, char *str, size_t max_size);
extern char *str_unquote    (char *s);
extern int   str_isquoted   (const char *s);
extern char *str_reverse    (char *str);
extern int   str_equal      (const char *s1, const char *s2);
extern int   str_equal_n    (const char *s1, const char *s2, size_t len);
extern char *str_shorten    (const char *str, size_t max_len);

extern char *searchpath     (const char *file, const char *env_var);
extern int   searchpath_pos (void);
extern char *_fix_path      (const char *path, char *result);
extern char *_fix_drive     (char *path);
extern char *path_ltrim     (const char *p1, const char *p2);
extern char *basename       (const char *fname);
extern char *dirname        (const char *fname);
extern int   _is_DOS83      (const char *fname);
extern char *slashify       (const char *path, char use);
extern char *slashify2      (char *buf, const char *path, char use);
extern char *win_strerror   (unsigned long err);
extern char *ws2_strerror   (int err);
extern BOOL  wchar_to_mbchar(char *result, size_t result_size, const wchar_t *buf);

extern void  set_error_mode (int on_off);
extern int  _file_exists    (const char *file);

extern char *evry_raw_query    (void);
extern char *getenv_expand     (const char *variable);
extern char *getenv_expand2    (const char *variable);
extern char *getenv_expand_sys (const char *variable);
extern BOOL  getenv_system     (smartlist_t **sl);

extern UINT   get_disk_type         (int disk);
extern BOOL   get_disk_cluster_size (int disk, DWORD *size);
extern UINT64 get_directory_size    (const char *dir);
extern UINT64 get_file_alloc_size   (const char *file, UINT64 size);
extern int    disk_ready            (int disk);
extern BOOL   chk_disk_ready        (int disk);
extern BOOL  _has_drive             (const char *path);
extern BOOL   is_directory          (const char *file);
extern int    safe_stat             (const char *file, struct stat *st, DWORD *win_err);

extern char       *make_cyg_path (const char *path, char *result);
extern wchar_t    *make_cyg_pathw (const wchar_t *path, wchar_t *result);

extern const char *compiler_version (void);
extern const char *get_user_name (void);
extern BOOL        is_user_admin (void);
extern BOOL        is_user_admin2 (void);
extern int         is_cygwin_tty (int fd);

extern const char *qword_str (UINT64 val);
extern const char *dword_str (DWORD val);
extern const char *plural_str (DWORD val, const char *singular, const char *plural);

extern void        format_and_print_line (const char *line, int indent);
extern void        print_long_line (const char *line, size_t indent);
extern char       *translate_shell_pattern (const char *pattern);
extern BOOL        test_shell_pattern (void);
extern void        hex_dump (const void *data_p, size_t datalen);
extern const char *dump10 (const void *data_p, unsigned size);
extern BOOL        get_module_filename_ex (HANDLE proc, char *filename);
extern time_t      FILETIME_to_time_t (const FILETIME *ft);

/* Fix for `_MSC_VER <= 1800` (Visual Studio 2012 or older) which is lacking `vsscanf()`.
 */
#if (defined(_MSC_VER) && (_MSC_VER <= 1800) && !defined(_VCRUNTIME_H))
  int _vsscanf2 (const char *buf, const char *fmt, va_list args);
  #define vsscanf(buf, fmt, args) _vsscanf2 (buf, fmt, args)
#endif

/* Windows security related functions for files/directories:
 */
extern BOOL get_file_owner (const char *file, char **domain_name, char **account_name);
extern BOOL is_directory_accessible (const char *path, DWORD access);
extern BOOL is_directory_readable (const char *path);
extern BOOL is_directory_writable (const char *path);


/** Functions for handling Reparse Points:
 *  (Junctions and Symlinks).
 */
extern const char *last_reparse_err;
extern BOOL        get_reparse_point (const char *dir, char *result,
                                      size_t result_size);

/** \struct ver_info
 *
 * Generic program version information (in resource).
 *
 *  Implemented by        | For what
 * -----------------------|---------------------------
 *  get_python_info()     | Supported Python programs.
 *  get_evry_version()    | EveryThing File-database.
 *  get_PE_version_info() | PE-image version info.
 */
struct ver_info {
       unsigned val_1;   /**< Major */
       unsigned val_2;   /**< Minor */
       unsigned val_3;   /**< Micro */
       unsigned val_4;   /**< Build (unused in envtool_py.c) */
     };

/** \def VALID_VER
 *  Check if ver was filled okay.
 *  Accept programs with major-version == 0. Hence assume if `major+minor == 0`,
 *  the version was not correctly set.
 */
#define VALID_VER(ver) (ver.val_1 + ver.val_2 > 0)

/** \struct search_list
 *  A generic search-list type.
 */
struct search_list {
       unsigned    value;  /**< the value */
       const char *name;   /**< the name of the associated value */
     };

/** \enum Bitness
 *  Used by check_if_PE() to retrieve the bitness of a PE-file.
 */
enum Bitness {
     bit_unknown = 0, /**< the bitness is unknown (not a PE-file?). */
     bit_16,          /**< 16-bit MSDOS file; never really set. */
     bit_32,          /**< 32-bit PE-file. */
     bit_64           /**< 64-bit PE-file. */
   };

extern const char *list_lookup_name (unsigned value, const struct search_list *list, int num);
extern unsigned    list_lookup_value (const char *name, const struct search_list *list, int num);
extern const char *flags_decode (DWORD flags, const struct search_list *list, int num);
extern const char *get_file_size_str (UINT64 size);
extern const char *get_time_str (time_t t);
extern const char *get_time_str_FILETIME (const FILETIME *ft);
extern const char *get_file_ext (const char *file);
extern char       *create_temp_file (void);
extern const char *check_if_shebang (const char *fname);
extern int         check_if_zip (const char *fname);
extern int         check_if_gzip (const char *fname);
extern BOOL        check_if_cwd_in_search_path (const char *program);
extern const char *get_gzip_link (const char *file);
extern const char *get_man_link (const char *file);
extern int         check_if_PE (const char *fname, enum Bitness *bits);
extern int         verify_PE_checksum (const char *fname);
extern BOOL        is_wow64_active (void);

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
        char   *buffer;        /**< the `alloca()` buffer */
        char   *buffer_start;  /**< as above + 4 bytes (past the marker) */
        char   *buffer_pos;    /**< current position in the buffer */
        size_t  buffer_size;   /**< number of bytes allocated in the buffer */
        size_t  buffer_left;   /**< number of bytes left in the buffer */
        int     on_heap;       /**< `buffer` is on the heap (not the stack) */
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
 *   \param use_heap  If TRUE, allocate using `MALLOC()`. Otherwise use `alloca()`.
 */
#define BUF_INIT(fmt_buf, size, use_heap) do {                    \
        DWORD   *_marker;                                         \
        FMT_buf *_buf = fmt_buf;                                  \
        size_t    _sz = size + 2*sizeof(DWORD);                   \
                                                                  \
        _buf->on_heap = use_heap;                                 \
        _buf->buffer  = use_heap ? MALLOC (_sz) : alloca (_sz);   \
        _marker  = (DWORD*) _buf->buffer;                         \
        *_marker = FMT_BUF_MARKER;                                \
        _marker  = (DWORD*) (_buf->buffer + _sz - sizeof(DWORD)); \
        *_marker = FMT_BUF_MARKER;                                \
        _buf->buffer_start  = _buf->buffer + sizeof(DWORD);       \
        _buf->buffer_pos    = _buf->buffer_start;                 \
        _buf->buffer_size   = size;                               \
        _buf->buffer_left   = size;                               \
        _buf->buffer_pos[0] = '\0';                               \
     /* memset (_buf->buffer_pos, '\0', size); */                 \
      } while (0)

#define BUF_EXIT(fmt_buf) do {   \
        FMT_buf *_buf = fmt_buf; \
        if (_buf->on_heap)       \
           FREE (_buf->buffer);  \
      } while (0)

#if defined(__POCC__)
_CRTCHK(printf,2,3)
#endif
int  buf_printf (FMT_buf *fmt_buf, _Printf_format_string_ const char *format, ...) ATTR_PRINTF (2,3);
void buf_puts_long_line (FMT_buf *fmt_buf, const char *line, size_t indent);
int  buf_puts   (FMT_buf *fmt_buf, const char *string);
int  buf_putc   (FMT_buf *fmt_buf, int ch);
void buf_reset  (FMT_buf *fmt_buf);

/* Stuff in win_ver.c:
 */
extern const char *os_name (void);
extern const char *os_bits (void);
extern const char *os_release_id (void);
extern const char *os_update_build_rev (void);
extern const char *os_full_version (void);

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
extern DWORD       wintrust_check (const char *pe_file, BOOL details, BOOL revoke_check);
extern const char *wintrust_check_result (DWORD rc);
extern void        wintrust_cleanup (void);
extern BOOL        wintrust_dump_pkcs7_cert (const char *file);

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

int   popen_run  (popen_callback callback, const char *cmd);
int   popen_runf (popen_callback callback, const char *fmt, ...);
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
 * \def DEBUGF(level, ...)
 *   Print a debug-statement if opt.debug >= level.
 *   \param level  the level needed to trigger a print-out.
 *   \param ...    a var-arg list of format an arguments (just like `printf()`).
 *
 * \def DEBUG_NL(level)
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

#define DEBUGF(level, ...)  do {                                        \
                              if (opt.debug >= level) {                 \
                                debug_printf ("%s(%u): ",               \
                                              __FILE(), __LINE__);      \
                                debug_printf (__VA_ARGS__);             \
                              }                                         \
                            } while (0)

#define DEBUG_NL(level)    do {                                         \
                              if (opt.debug >= level)                   \
                                C_putc ('\n');                          \
                           } while (0)

#define WARN(...)           do {                                        \
                              if (!opt.quiet) {                         \
                                C_puts ("~5");                          \
                                C_printf (__VA_ARGS__);                 \
                                C_puts ("~0");                          \
                              }                                         \
                            } while (0)

#define FATAL(...)          do {                                        \
                              CRTDBG_CHECK_OFF();                       \
                              fflush (stdout);                          \
                              fprintf (stderr, "\nFatal: %s(%u): ",     \
                                       __FILE(), __LINE__);             \
                              fprintf (stderr, ##__VA_ARGS__);          \
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
