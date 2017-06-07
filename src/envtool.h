#ifndef _ENVTOOL_H
#define _ENVTOOL_H

#define VER_STRING  "1.0"
#define MAJOR_VER   1
#define MINOR_VER   0

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

#if defined(__MINGW32__) && defined(_FORTIFY_SOURCE)
  /*
   * Enable GNU LibSSP; "Stack Smashing Protector".
   *   Ref: http://aconole.brad-x.com/papers/exploits/ssp/intro
   */
  #if (_FORTIFY_SOURCE == 1) && defined(INSIDE_ENVOOL_C)
    #pragma message ("Using _FORTIFY_SOURCE=1")
  #elif (_FORTIFY_SOURCE == 2) && defined(INSIDE_ENVOOL_C)
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

  #define _MAX_PATH              _POSIX_PATH_MAX   /* 256 */
  #define _S_ISDIR(mode)         S_ISDIR (mode)
  #define _S_ISREG(mode)         S_ISREG (mode)
  #define _popen(cmd, mode)      popen (cmd, mode)
  #define _pclose(fil)           pclose (fil)
  #define _fileno(f)             fileno (f)
  #define _isatty(fd)            isatty (fd)
  #define _tempnam(dir,prefix)   tempnam (dir,prefix)
  #define _wcsdup(s)             wcsdup (s)

  #define stricmp(s1, s2)        strcasecmp (s1, s2)
  #define strnicmp(s1, s2, len)  strncasecmp (s1, s2, len)
  #define DEV_NULL               "/dev/null"

  extern char *_itoa (int value, char *buf, int radix);

#else
  #include <direct.h>
  #define  stat     _stati64
  #define  DEV_NULL "NUL"

  #if !defined(_WINSOCK2API_) && !defined(_WINSOCK2_H) && !defined(_BSDTYPES_DEFINED)
    #define u_long unsigned long
    #define _BSDTYPES_DEFINED
  #endif
#endif

#if defined(__POCC__)
  #include <wchar.h>

  #define _MAX_PATH       MAX_PATH   /* 260 */
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
  #undef  _malloca                /* Avoid MSVC-9 <malloc.h>/<crtdbg.h> name-clash */
  #define _CRTDBG_MAP_ALLOC
  #include <crtdbg.h>
#endif

#if defined(_DEBUG)
  #define ASSERT(expr) do {                                            \
                         if (!(expr))                                  \
                            FATAL ("Assertion `%s' failed.\n", #expr); \
                       } while (0)
#else
  #define ASSERT(expr) (void) (expr)
#endif

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

#if defined(WIN32) || defined(_WIN32)
  #define DIR_SEP            '\\'
#else /* I.e. CygWin */
  #define DIR_SEP            '/'
#endif

#if defined(__CYGWIN__)
  #include <sys/stat.h>
  /*
   * Cannot use 'GetFileAttributes()' in case file is on Posix form.
   * E.g. "/cygdrive/c/foo"
   */
  static inline int FILE_EXISTS (const char *f)
  {
    struct stat st;
    return (stat(f,&st) == 0);
  }
#else
  static __inline int FILE_EXISTS (const char *f)
  {
    return (GetFileAttributes(f) != INVALID_FILE_ATTRIBUTES);
  }
#endif


#ifndef _S_ISDIR
  #define _S_ISDIR(mode)     (((mode) & _S_IFMT) == _S_IFDIR)
#endif

#ifndef _S_ISREG
  #define _S_ISREG(mode)     (((mode) & _S_IFMT) == _S_IFREG)
#endif

#define IS_SLASH(c)          ((c) == '\\' || (c) == '/')

#ifdef __GNUC__
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
  #elif defined(__CYGWIN__)                  /* CygWin64  */  || \
        defined(TRAVIS_BUILD)                /* Travis-CI build */
    #define ADDR_FMT     "016llX"
  #else
    #error "Unsupported compiler"
  #endif
  #define ADDR_CAST(x)  ((unsigned long long)(x))

#else                                        /* 32-bit targets */
  #define ADDR_FMT     "08lX"
  #define ADDR_CAST(x)  ((unsigned long)(x))
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

#define HKEY_PYTHON_EGG                (HKEY) 0x7FFF
#define HKEY_EVERYTHING                (HKEY) 0x7FFE
#define HKEY_MAN_FILE                  (HKEY) 0x7FFD
#define HKEY_INC_LIB_FILE              (HKEY) 0x7FFC
#define HKEY_LOCAL_MACHINE_SESSION_MAN (HKEY) (HKEY_LOCAL_MACHINE + 0xFF) /* HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment */
#define HKEY_CURRENT_USER_ENV          (HKEY) (HKEY_CURRENT_USER + 0xFF)  /* HKCU\Environment */

struct prog_options {
       int   debug;
       int   verbose;
       int   quiet;
       int   quotes_warn;
       int   add_cwd;
       int   show_unix_paths;
       int   decimal_timestamp;
       int   no_sys_env;
       int   no_usr_env;
       int   no_app_path;
       int   no_colours;
       int   use_regex;
       int   dir_mode;
       int   man_mode;
       int   PE_check;
       int   do_tests;
       int   help;
       int   show_size;
       int   only_32bit;
       int   only_64bit;
       int   gcc_no_prefixed;
       int   no_gcc;
       int   no_gpp;
       int   do_evry;
       int   do_version;
       int   do_path;
       int   do_lib;
       int   do_include;
       int   do_python;
       int   do_man;
       int   do_cmake;
       int   do_pkg;
       int   conv_cygdrive;
       char *file_spec;
       char *file_spec_re;
     };

extern struct prog_options opt;

extern char   sys_dir        [_MAX_PATH];
extern char   sys_native_dir [_MAX_PATH];

extern int  report_file (const char *file, time_t mtime, UINT64 fsize,
                         BOOL is_dir, BOOL is_junction, HKEY key);

extern int  process_dir (const char *path, int num_dup, BOOL exist,
                         BOOL is_dir, BOOL exp_ok, const char *prefix, HKEY key, BOOL recursive);

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

extern char *_strlcpy      (char *dst, const char *src, size_t len);
extern char *_strsep       (char **s, const char *delim);
extern char *_stracat      (char *s1, const char *s2);
extern char *strip_nl      (char *s);
extern char *str_ltrim     (char *s);
extern char *str_rtrim     (char *s);
extern char *str_trim      (char *s);
extern char *searchpath    (const char *file, const char *env_var);
extern int   searchpath_pos(void);
extern char *getenv_expand (const char *variable);
extern char *_fix_path     (const char *path, char *result);
extern char *_fix_drive    (char *path);
extern char *path_ltrim    (const char *p1, const char *p2);
extern char *basename      (const char *fname);
extern char *dirname       (const char *fname);
extern int   _is_DOS83     (const char *fname);
extern char *slashify      (const char *path, char use);
extern char *win_strerror  (unsigned long err);
extern void  set_error_mode(int on_off);
extern int   disk_ready    (int disk);
extern void  make_cyg_path (const char *path, char *result);

extern const char *compiler_version (void);
extern const char *get_user_name (void);
extern BOOL        is_user_admin (void);
extern int         is_cygwin_tty (int fd);

extern const char *qword_str (UINT64 val);
extern const char *dword_str (DWORD val);

extern void        format_and_print_line (const char *line, int indent);
extern void        print_long_line (const char *line, size_t indent);
extern char       *translate_shell_pattern (const char *pattern);
extern void        hex_dump (const void *data_p, size_t datalen);
extern const char *dump10 (const void *data_p, unsigned size);

/* Functions for handling Reparse Points:
 * (Junctions and Symlinks).
 */
extern const char *last_reparse_err;
extern BOOL        get_reparse_point (const char *dir, char *result,
                                      BOOL return_print_name);

/*
 * Generic program version information (in resource).
 *
 *  Implemented by        | For what
 * -----------------------|---------------------------
 *  get_python_info()     | Supported Python programs.
 *  get_evry_version()    | EveryThing File-database.
 *  get_PE_version_info() | PE-image version info.
 */
struct ver_info {
       unsigned val_1;   /* Major */
       unsigned val_2;   /* Minor */
       unsigned val_3;   /* Micro */
       unsigned val_4;   /* Build (unused in envtool_py.c) */
     };

/* Generic search-list type.
 */
struct search_list {
       unsigned    value;
       const char *name;
     };

/* For check_if_PE().
 */
enum Bitness {
     bit_unknown = 0,
     bit_16,
     bit_32,
     bit_64
   };

extern const char *list_lookup_name (unsigned value, const struct search_list *list, int num);
extern unsigned    list_lookup_value (const char *name, const struct search_list *list, int num);
extern const char *flags_decode (DWORD flags, const struct search_list *list, int num);
extern const char *get_file_size_str (UINT64 size);
extern const char *get_time_str (time_t t);
extern const char *get_file_ext (const char *file);
extern char       *create_temp_file (void);
extern int         get_PE_version_info (const char *file, struct ver_info *ver);
extern char       *get_PE_version_info_buf (void);
extern void        get_PE_version_info_free (void);
extern int         check_if_zip (const char *fname);
extern int         check_if_gzip (const char *fname);
extern const char *get_gzip_link (const char *file);
extern int         check_if_PE (const char *fname, enum Bitness *bits);
extern int         verify_PE_checksum (const char *fname);
extern BOOL        is_wow64_active (void);

extern REGSAM      reg_read_access (void);
extern const char *reg_type_name (DWORD type);
extern const char *reg_top_key_name (HKEY key);
extern const char *reg_access_name (REGSAM acc);
extern DWORD       reg_swap_long (DWORD val);

/* Stuff in win_ver.c:
 */
extern const char *os_name (void);

/* Stuff in win_trust.c:
 */
extern char       *wintrust_subject;
extern DWORD       wintrust_check (const char *pe_file, BOOL details, BOOL revoke_check);
extern const char *wintrust_check_result (DWORD rc);


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

#if defined(_CRTDBG_MAP_ALLOC) || defined(__CYGWIN__)
  #define MALLOC        malloc
  #define CALLOC        calloc
  #define REALLOC       realloc
  #define STRDUP        strdup
  #define WCSDUP        wcsdup
  #define FREE(p)       (p ? (void) (free(p), p = NULL) : (void)0)
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

int popen_run (const char *cmd, popen_callback callback);

/* fnmatch() ret-values and flags:
 */
#define FNM_MATCH          1
#define FNM_NOMATCH        0

#define FNM_FLAG_NOESCAPE  0x01
#define FNM_FLAG_PATHNAME  0x02
#define FNM_FLAG_NOCASE    0x04

extern int   fnmatch     (const char *pattern, const char *string, int flags);
extern char *fnmatch_res (int rc);

/* Handy macros:
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
                              fprintf (stderr, "\nFatal: %s(%u): ",     \
                                       __FILE(), __LINE__);             \
                              fprintf (stderr, ##__VA_ARGS__);          \
                              if (IsDebuggerPresent())                  \
                                   abort();                             \
                              else ExitProcess (GetCurrentProcessId()); \
                            } while (0)

#ifdef __cplusplus
}
#endif

#endif /* RC_INVOKED */
#endif
