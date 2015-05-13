#ifndef _ENVTOOL_H
#define _ENVTOOL_H

#define VER_STRING  "0.96"
#define MAJOR_VER   0
#define MINOR_VER   96

#define CHECK_PREFIXED_GCC 0

#define AUTHOR_STR    "Gisle Vanem <gvanem@yahoo.no>"

#if defined(_MSC_VER)
  #ifdef _DEBUG
    #define BUILDER  "Visual-C, debug"
  #else
    #define BUILDER  "Visual-C, release"
  #endif

#elif defined(__MINGW64_VERSION_MAJOR)
  #define BUILDER  "MingW64/TDM-gcc"

#elif defined(__MINGW32__)
  #define BUILDER  "MingW"

#elif defined(__CYGWIN__)
  #define BUILDER  "CygWin"

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
  #if (_FORTIFY_SOURCE == 1)
    #pragma message ("Using _FORTIFY_SOURCE==1")
  #elif (_FORTIFY_SOURCE == 2)
    #pragma message ("Using _FORTIFY_SOURCE==2")
  #endif
  #include <ssp/stdio.h>
  #include <ssp/string.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <ctype.h>
#include <io.h>
#include <limits.h>
#include <sys/stat.h>
#include <windows.h>

#ifdef __CYGWIN__
 /*
  * <limits.h> defines PATH_MAX to 4096. That seems excessive for our use.
  */
  #define _MAX_PATH    _POSIX_PATH_MAX   /* 256 */
  #define _S_ISDIR(m)  S_ISDIR (m)
  #define _S_ISREG(m)  S_ISREG (m)
  #define _popen(c,m)  popen (c,m)
  #define _pclose(f)   pclose (f)

  #include <unistd.h>
  #include <alloca.h>
  #include <sys/cygwin.h>
#else
  #include <direct.h>
#endif

#if defined(_MSC_VER) && defined(_DEBUG)
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
#if defined(_MSC_VER)
  #define __FILE()           basename (__FILE__)
  #define snprintf           _snprintf
#else
  #define __FILE()           __FILE__
#endif

#if defined(WIN32) || defined(_WIN32)
  #define DIR_SEP            '\\'
  #define FILE_EXISTS(file)  (access(file,0) == 0)
#else
  #define DIR_SEP            '/'
  #define FILE_EXISTS(file)  (chmod(file,0) == -1 ? 0 : 1)
#endif

#ifndef _S_ISDIR
  #define _S_ISDIR(m)        (((m) & _S_IFMT) == _S_IFDIR)
#endif

#ifndef _S_ISREG
  #define _S_ISREG(m)        (((m) & _S_IFMT) == _S_IFREG)
#endif

#ifdef __GNUC__
  #define ATTR_PRINTF(_1,_2) __attribute__((format(printf,_1,_2)))
#else
  #define ATTR_PRINTF(_1,_2)
#endif

#ifdef __CYGWIN__
  #define S64_FMT "%lld"
#else
  #define S64_FMT "%I64d"
#endif

#ifndef STDOUT_FILENO
#define STDOUT_FILENO  1
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HKEY_PYTHON_EGG                (HKEY) 0x7FFF
#define HKEY_EVERYTHING                (HKEY) 0x7FFE
#define HKEY_LOCAL_MACHINE_SESSION_MAN (HKEY) (HKEY_LOCAL_MACHINE + 0xFF) /* HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment */
#define HKEY_CURRENT_USER_ENV          (HKEY) (HKEY_CURRENT_USER + 0xFF)  /* HKCU\Environment */

extern int   show_unix_paths, add_cwd;
extern int   debug, quiet, verbose;
extern char *file_spec;

extern void  report_file (const char *file, time_t mtime, BOOL is_dir, HKEY key);
extern int   process_dir (const char *path, int num_dup, BOOL exist,
                          BOOL is_dir, BOOL exp_ok, const char *prefix, HKEY key);

/* Stuff in misc.c:
 */
extern int debug_printf  (const char *format, ...) ATTR_PRINTF (1,2);

#if 0  /* Retired functions. See color.h. */
  extern int Cputs         (int attr, const char *buf);
  extern int Cprintf       (int attr, const char *format, ...) ATTR_PRINTF (2,3);
  extern int Cvprintf      (int attr, const char *format, va_list args);
#endif

extern char *_strlcpy      (char *dst, const char *src, size_t len);
extern char *strip_nl      (char *s);
extern char *str_trim      (char *s);
extern char *searchpath    (const char *file, const char *env_var);
extern char *getenv_expand (const char *variable);
extern char *_fixpath      (const char *path, char *result);
extern char *path_ltrim    (const char *p1, const char *p2);
extern char *basename      (const char *fname);
extern char *dirname       (const char *fname);
extern int   _is_DOS83     (const char *fname);
extern char *slashify      (const char *path, char use);
extern char *win_strerror  (unsigned long err);
extern char *translate_shell_pattern (const char *pattern);

/* For PE-image version in get_version_info().
 */
struct ver_info {
       unsigned val_1;
       unsigned val_2;
       unsigned val_3;
       unsigned val_4;
     };

/* Generic search-list type.
 */
struct search_list {
       unsigned    value;
       const char *name;
     };

extern const char *flags_decode (DWORD flags, const struct search_list *list, int num);

extern const char *get_file_ext (const char *file);
extern char       *create_temp_file (void);
extern int         get_version_info (const char *file, struct ver_info *ver);
extern char       *get_version_info_buf (void);
extern void        get_version_info_free (void);
extern int         check_if_zip (const char *fname);
extern int         check_if_PE (const char *fname);
extern int         verify_pe_checksum (const char *fname);
extern BOOL        is_wow64_active (void);

/* Simple debug-malloc functions:
 */
#define MEM_MARKER  0xDEAFBABE
#define MEM_FREED   0xDEADBEAF

extern void *malloc_at  (size_t size, const char *file, unsigned line);
extern void *calloc_at  (size_t num, size_t size, const char *file, unsigned line);
extern void *realloc_at (void *ptr, size_t size, const char *file, unsigned line);
extern char *strdup_at  (const char *str, const char *file, unsigned line);
extern void  free_at    (void *ptr, const char *file, unsigned line);
extern void  mem_report (void);

#define MALLOC(s)      malloc_at (s, __FILE(), __LINE__)
#define CALLOC(n,s)    calloc_at (n, s, __FILE(), __LINE__)
#define REALLOC(p,s)   realloc_at (p, s, __FILE(), __LINE__)
#define STRDUP(s)      strdup_at (s, __FILE(), __LINE__)
#define FREE(p)        (p ? (void) (free_at(p, __FILE(), __LINE__), p = NULL) : (void)0)

typedef int (popen_callback) (char *buf, int index);

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

#if 0
  #ifdef _DEBUG
    /*
     * With '-RTCc' in debug-mode, this generates
     *   Run-Time Check Failure #1 - A cast to a smaller
     *   data type has caused a loss of data.  If this was intentional, you should
     *   mask the source of the cast with the appropriate bitmask.  For example:
     *     char c = (i & 0xFF);
     */
    #define loBYTE(w)   ((BYTE) ((w) & 255))
    #define hiBYTE(w)   ((BYTE) ((WORD) ((w) & 255) >> 8))
  #else
    #define loBYTE(w)   (BYTE)(w)
    #define hiBYTE(w)   (BYTE)((WORD)(w) >> 8)
  #endif
#endif

#define DEBUGF(level, fmt, ...)  do {                                           \
                                   if (debug >= level) {                        \
                                     debug_printf ("%s(%u): " fmt,              \
                                       __FILE(), __LINE__, ## __VA_ARGS__);     \
                                   }                                            \
                                 } while (0)

#define WARN(fmt, ...)           do {                                           \
                                   if (!quiet)                                  \
                                      C_printf ("~5" fmt "~0", ## __VA_ARGS__); \
                                 } while (0)

#if 0
  #define FATAL(fmt, ...)        do {                                              \
                                   fprintf (stderr, "Fatal: "fmt, ## __VA_ARGS__); \
                                   exit (1);                                       \
                                 } while (0)

#else
  #define FATAL(fmt, ...)        do {                                        \
                                   fprintf (stderr, "\nFatal: %s(%u): " fmt, \
                                            __FILE(), __LINE__,              \
                                            ## __VA_ARGS__);                 \
                                   if (IsDebuggerPresent())                  \
                                        abort();                             \
                                   else ExitProcess (GetCurrentProcessId()); \
                                 } while (0)
#endif

#ifdef __cplusplus
};
#endif

#endif /* RC_INVOKED */
#endif
