/**\file    tests.c
 * \ingroup Misc
 * \brief   Functions for testing various features.
 *          Called on `envtool -t` or `envtool --test`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "tests.h"
#include "color.h"
#include "auth.h"
#include "Everything_ETP.h"
#include "envtool_py.h"
#include "envtool.h"
#include "cache.h"
#include "vcpkg.h"
#include "dirlist.h"

extern bool find_vstudio_init (void);

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
    const directory_array *arr = smartlist_get (list, i);
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
    if (arr->is_native && !arr->exist)
       C_puts ("  ~5**native dir not existing**~0");
    else if (!arr->exist)
       C_puts ("  ~5**not existing**~0");
    else if (!arr->is_dir)
       C_puts ("  ~5**not a dir**~0");

    C_putc ('\n');
  }
  dir_array_free();
  FREE (value);
  C_printf ("  ~3%d elements~0\n\n", i);
}

static int cmake_version_cb (char *buf, int index)
{
  TRACE (2, "buf: '%s', index: %d.\n", buf, index);
  return (0);
}

/**
 * \struct test_table1
 * The structure used in `test_searchpath()`.
 */
struct test_table1 {
       const char *file;   /**< the file to test in `searchpath()` */
       const char *env;    /**< the environment variable to use */
     };

static const struct test_table1 tab1[] = {
                  { "kernel32.dll",  "PATH" },
                  { "notepad.exe",   "PATH" },
                  { "python.exe",    "PATH" },

                  /* Relative file-name test:
                   *   `c:\Windows\system32\Resources\Themes\aero.theme` is present in Win-8.1+
                   *   and `c:\Windows\system32` should always be on PATH.
                   */
                  { "..\\Resources\\Themes\\aero.theme", "PATH" },

                  { "./envtool.c",        "FOO-BAR" },       /* CWD should always be at pos 0 regardless of env-var. */
                  { "msvcrt.lib",         "LIB" },
                  { "libgcc.a",           "LIBRARY_PATH" },
                  { "libgmon.a",          "LIBRARY_PATH" },
                  { "stdio.h",            "INCLUDE" },
                  { "../../../Lib/os.py", "PYTHONPATH" },

                  /* test if `searchpath()` works for Short File Names
                   * (%WinDir\systems32\PresentationHost.exe).
                   * SFN seems not to be available on Win-7+.
                   * "PRESEN~~1.EXE" = "PRESEN~1.EXE" since `C_printf()` is used.
                   */
                  { "PRESEN~~1.EXE",     "PATH" },

                  /* test if `searchpath()` works with "%WinDir%\sysnative" on Win-7+.
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
 * Tests for `searchpath()`.
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
         /* 5 */  { FNM_MATCH,   "foo\\*",       "foo\\barney",    FNM_FLAG_NOESCAPE|FNM_FLAG_PATHNAME },
         /* 6 */  { FNM_MATCH,   "foo\\*",       "foo\\barney",    0 },
         /* 7 */  { FNM_NOMATCH, "mil[!k]-bar*", "milk-bar",       0 },
         /* 8 */  { FNM_MATCH,   "mil[!k]-bar*", "milf-bar",       0 },
         /* 9 */  { FNM_MATCH,   "mil[!k]-bar?", "milf-barn",      0 },
                };

/**
 * Tests for `fnmatch()`.
 *
 * `test_table::expect` does not work with `opt.case_sensitive`.
 * I.e. `envtool --test -c`.
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

    C_printf (" fnmatch (\"%s\",%*s\"%s\",%*s0x%02X): %s\n",
              t->pattern, (int)(13-len1), "", t->fname, (int)(15-len2), "",
              (UINT)flags, fnmatch_res(rc));
  }
  C_putc ('\n');
}

/**
 * Tests for some functions in misc.c.
 */
struct test_table3 {
       const char *file;    /**< the file to test in `get_file_ext()` */
       const char *expect;  /**< the expected result */
     };

static void test_misc (void)
{
  static const struct test_table3 ext_tests[] = {
              { "c:\\foo\\.\\bar\\baz.c",   "c"   },
              { "foo\\.\\bar\\baz",         ""    },
              { "c:\\foo\\bar\\baz.pc",     "pc"  },
              { "c:\\foo\\bar\\baz.pc.old", "old" },
            };
  static int stat_okay = 0;
  static const struct test_table3 stat_tests[] = {
              { "c:\\pagefile.sys", (const char*)&stat_okay },
              { "c:\\swapfile.sys", (const char*)&stat_okay },
              { "c:\\",             (const char*)&stat_okay }
            };

  const struct test_table3 *t;
  DWORD err;
  int   i;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  C_printf ("  check_if_cwd_in_search_path(~6\"envtool.exe\"~0):   %s~0\n",
            check_if_cwd_in_search_path("envtool.exe") ? "~2YES" : "~5NO");

  C_printf ("  check_if_cwd_in_search_path(~6\".\\envtool.exe\"~0): %s~0\n\n",
            check_if_cwd_in_search_path(".\\envtool.exe") ? "~2YES" : "~5NO");

  i = 0;
  for (t = ext_tests; i < DIM(ext_tests); t++, i++)
  {
    const char *rc  = get_file_ext (t->file);
    size_t      len = strlen (t->file);

    C_printf ("%s~0 get_file_ext (\"%s\") %*s -> \"%s\"\n",
              !strcmp(rc, t->expect) ? "~2  OK  " : "~5  FAIL",
              t->file, (int)(22-len), "", rc);
  }
  C_putc ('\n');

  i = 0;
  for (t = stat_tests; i < DIM(stat_tests); t++, i++)
  {
    struct stat st;
    int    rc  = safe_stat (t->file, &st, &err);
    size_t len = strlen (t->file);

    C_printf ("%s~0 safe_stat (\"%s\") %*s -> %2d, size: %s, ctime: %s, err: %lu\n",
              rc == *t->expect ? "~2  OK  " : "~5  FAIL", t->file, (int)(15-len), "", rc,
              get_file_size_str(st.st_size), get_time_str(st.st_ctime), err);
  }
  C_putc ('\n');
}

/**
 * Tests for `slashify()`.
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
 * Tests for `_fix_path()`.
 * Canonize the horrendous pathnames reported from `gcc -v`.
 *
 * It doesn't matter if these paths or files exists or not. `_fix_path()`
 * (i.e. `GetFullPathName()`) should canonizes these regardless.
 */
static void test_fix_path (void)
{
  static const char *files[] = {
    "f:\\CygWin64\\bin\\../lib/gcc/x86_64-w64-mingw32/6.4.0/include",              /* exists here */
    "f:\\CygWin64\\bin\\../lib/gcc/x86_64-w64-mingw32/6.4.0/include\\ssp\\ssp.h",  /* exists here */
    "f:\\CygWin64\\lib/gcc/i686-pc-mingw32/4.7.3/../../../perl5",                  /* exists here */
    "/usr/libexec/../include/w32api",                                              /* CygWin64 output, exists here */
    "c:\\\\Windows\\\\system32/drivers/etc\\\\hosts"                               /* fix doubled '\', exists here */
  };
  const char *f;
  char *rc1;
  int   i, rc2;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(files); i++)
  {
    char buf [_MAX_PATH], *cyg_result = "";
    bool is_dir;

    f = files [i];
    rc1 = _fix_path (f, buf);    /* has only `\\` slashes */
    rc2 = FILE_EXISTS (buf);
    is_dir = is_directory (rc1);

    if (opt.show_unix_paths)
       rc1 = slashify2 (buf, buf, '/');

    C_printf ("  _fix_path (\"%s\")\n", f);

    if (!rc2)
         C_printf ("   ~5exists 0, is_dir %d%s~0", is_dir, cyg_result);
    else C_printf ("   ~2exists 1, is_dir %d%s~0", is_dir, cyg_result);

    C_printf (" -> %s\n", rc1);
  }
  C_putc ('\n');
}

/*
 * The `CSIDL_LOCAL_APPDATA` folder.
 * Save it's value to build up the path for `test_AppxReparsePoints()` later.
 */
static char win_apps [_MAX_PATH];

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

  static const search_list sh_folders[] = {
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
               ADD_VALUE (CSIDL_COMMON_APPDATA),
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

  #if 0  /* The official values of the above */
    #define CSIDL_INTERNET                  0x0001        /* Internet Explorer (icon on desktop) */
    #define CSIDL_PROGRAMS                  0x0002        /* Start Menu\Programs */
    #define CSIDL_CONTROLS                  0x0003        /* My Computer\Control Panel */
    #define CSIDL_PRINTERS                  0x0004        /* My Computer\Printers */
    #define CSIDL_PERSONAL                  0x0005        /* My Documents */
    #define CSIDL_FAVORITES                 0x0006        /* <user name>\Favorites */
    #define CSIDL_STARTUP                   0x0007        /* Start Menu\Programs\Startup */
    #define CSIDL_RECENT                    0x0008        /* <user name>\Recent */
    #define CSIDL_SENDTO                    0x0009        /* <user name>\SendTo */
    #define CSIDL_BITBUCKET                 0x000a        /* <desktop>\Recycle Bin */
    #define CSIDL_STARTMENU                 0x000b        /* <user name>\Start Menu */
    #define CSIDL_MYMUSIC                   0x000d        /* "My Music" folder */
    #define CSIDL_MYVIDEO                   0x000e        /* "My Videos" folder */
    #define CSIDL_DESKTOPDIRECTORY          0x0010        /* <user name>\Desktop */
    #define CSIDL_DRIVES                    0x0011        /* My Computer */
    #define CSIDL_NETWORK                   0x0012        /* Network Neighborhood (My Network Places) */
    #define CSIDL_NETHOOD                   0x0013        /* <user name>\nethood */
    #define CSIDL_FONTS                     0x0014        /* windows\fonts */
    #define CSIDL_TEMPLATES                 0x0015
    #define CSIDL_COMMON_STARTMENU          0x0016        /* All Users\Start Menu */
    #define CSIDL_COMMON_PROGRAMS           0X0017        /* All Users\Start Menu\Programs */
    #define CSIDL_COMMON_STARTUP            0x0018        /* All Users\Startup */
    #define CSIDL_COMMON_DESKTOPDIRECTORY   0x0019        /* All Users\Desktop */
    #define CSIDL_APPDATA                   0x001a        /* <user name>\Application Data */
    #define CSIDL_PRINTHOOD                 0x001b        /* <user name>\PrintHood */
    #define CSIDL_LOCAL_APPDATA             0x001c        /* <user name>\Local Settings\Applicaiton Data (non roaming) */
    #define CSIDL_ALTSTARTUP                0x001d        /* non localized startup */
    #define CSIDL_COMMON_ALTSTARTUP         0x001e        /* non localized common startup */
    #define CSIDL_COMMON_FAVORITES          0x001f
    #define CSIDL_INTERNET_CACHE            0x0020
    #define CSIDL_COOKIES                   0x0021
    #define CSIDL_HISTORY                   0x0022
    #define CSIDL_COMMON_APPDATA            0x0023        /* All Users\Application Data */
    #define CSIDL_WINDOWS                   0x0024        /* GetWindowsDirectory() */
    #define CSIDL_SYSTEM                    0x0025        /* GetSystemDirectory() */
    #define CSIDL_PROGRAM_FILES             0x0026        /* C:\Program Files */
    #define CSIDL_MYPICTURES                0x0027        /* C:\Program Files\My Pictures */
    #define CSIDL_PROFILE                   0x0028        /* USERPROFILE */
    #define CSIDL_SYSTEMX86                 0x0029        /* x86 system directory on RISC */
    #define CSIDL_PROGRAM_FILESX86          0x002a        /* x86 C:\Program Files on RISC */
    #define CSIDL_PROGRAM_FILES_COMMON      0x002b        /* C:\Program Files\Common */
    #define CSIDL_PROGRAM_FILES_COMMONX86   0x002c        /* x86 Program Files\Common on RISC */
    #define CSIDL_COMMON_TEMPLATES          0x002d        /* All Users\Templates */
    #define CSIDL_COMMON_DOCUMENTS          0x002e        /* All Users\Documents */
    #define CSIDL_COMMON_ADMINTOOLS         0x002f        /* All Users\Start Menu\Programs\Administrative Tools */
    #define CSIDL_ADMINTOOLS                0x0030        /* <user name>\Start Menu\Programs\Administrative Tools */
    #define CSIDL_CONNECTIONS               0x0031        /* Network and Dial-up Connections */
    #define CSIDL_COMMON_MUSIC              0x0035        /* All Users\My Music */
    #define CSIDL_COMMON_PICTURES           0x0036        /* All Users\My Pictures */
    #define CSIDL_COMMON_VIDEO              0x0037        /* All Users\My Video */
    #define CSIDL_RESOURCES                 0x0038        /* Resource Direcotry */
    #define CSIDL_RESOURCES_LOCALIZED       0x0039        /* Localized Resource Direcotry */
    #define CSIDL_COMMON_OEM_LINKS          0x003a        /* Links to All Users OEM specific apps */
    #define CSIDL_CDBURN_AREA               0x003b        /* USERPROFILE\Local Settings\Application Data\Microsoft\CD Burning */
    #define CSIDL_COMPUTERSNEARME           0x003d        /* Computers Near Me (computered from Workgroup membership) */
  #endif

  int i, slash = opt.show_unix_paths ? '/' : '\\';

  C_printf ("~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(sh_folders); i++)
  {
    char               buf [_MAX_PATH];
    const search_list *folder   = sh_folders + i;
    const char        *flag_str = opt.verbose ? "SHGFP_TYPE_CURRENT" : "SHGFP_TYPE_DEFAULT";
    DWORD              flag     = opt.verbose ?  SHGFP_TYPE_CURRENT  :  SHGFP_TYPE_DEFAULT;
    HRESULT            rc       = SHGetFolderPath (NULL, folder->value, NULL, flag, buf);

    if (rc == S_OK)
    {
      slashify2 (buf, buf, slash);
      if (folder->value == CSIDL_LOCAL_APPDATA)
         snprintf (win_apps, sizeof(win_apps), "%s%cMicrosoft%cWindowsApps", buf, slash, slash);
    }
    else
      snprintf (buf, sizeof(buf), "~5Failed: %s", win_strerror(rc));

    C_printf ("  ~3SHGetFolderPath~0 (~6%s~0, ~6%s~0):\n    ~2%s~0\n",
              folder->name, flag_str, buf);
  }
  C_putc ('\n');
}

/**
 * Test Windows' Reparse Points (Junctions and directory symlinks).
 *
 * Also make a test similar to the `dir /AL` command (Attribute Reparse Points):
 *
 * `cmd.exe /c dir c:\ /s /AL`
 * `tcc.exe /c dir c:\ /sal`
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
  const char *p;
  char  result [_MAX_PATH];
  bool  rc;
  int   i, slash = opt.show_unix_paths ? '/' : '\\';

  C_printf ("~3%s():~0\n", __FUNCTION__);

  for (i = 0; i < DIM(points); i++)
  {
    p = points[i];
    result [0] = '?';
    result [1] = '\0';
    rc = get_reparse_point (p, result, sizeof(result));

    C_printf ("  %d: \"%s\" %*s->", i, p, (int)(26-strlen(p)), "");

    if (!rc)
         C_printf (" ~5%s~0\n", last_reparse_err);
    else C_printf (" \"%s\"\n", slashify2(result, result, slash));
  }

#if defined(__INTEL_LLVM_COMPILER)
  char advisor_dir [_MAX_PATH];

  p = advisor_dir;
  snprintf (advisor_dir, sizeof(advisor_dir), "%s\\advisor\\latest", getenv("ONEAPI_ROOT"));
  rc = get_reparse_point (advisor_dir, result, sizeof(result));

  C_printf ("  %d: \"%s\" %*s->", i, p, (int)(26-strlen(p)), "");

  if (!rc)
       C_printf (" ~5%s~0\n", last_reparse_err);
  else C_printf (" \"%s\"\n", slashify2(result, result, slash));
#endif

  C_putc ('\n');
}

/**
 * List some EXE files in the "c:\Users\{user}\AppData\Local\Microsoft\WindowsApp"
 * folder and get their true targets. They should all be AppX reparse-points.
 *
 * Result would some cryptic location like:
 *  0: "c:\Users\gvane\AppData\Local\Microsoft\WindowsApps\winget.exe" ->
 *     "c:\Program Files\WindowsApps\Microsoft.DesktopAppInstaller_1.27.350.0_x64__8wekyb3d8bbwe\winget.exe"
 */
static void test_AppxReparsePoints (void)
{
  struct od2x_options opts;
  int    num = 0, max = 10;
  int    slash = opt.show_unix_paths ? '/' : '\\';
  DIR2  *dp;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  memset (&opts, '\0', sizeof(opts));
  opts.pattern = "*.exe";
  if (opt.verbose >= 1)
  {
    max = INT_MAX;
    opts.sort = OD2X_FILES_FIRST;
    opts.recursive = 1;    /* does not work yet */
    opts.pattern = "*";
  }

  dp = opendir2x (win_apps, &opts);

  while (num < max)
  {
    struct dirent2 *de = readdir2 (dp);
    char   result [_MAX_PATH];
    char  *file;
    bool   rc;

    TRACE (1, "de->d_name: '%s'\n", de ? de->d_name : "<none>");

    if (!de || !de->d_name)
       break;

    if (stricmp(get_file_ext(de->d_name), "exe"))
       continue;

    result [0] = '?';
    result [1] = '\0';
    file = slashify2 (de->d_name, de->d_name, slash);
    rc = get_reparse_point (file, result, sizeof(result));

    C_printf ("  %d: \"%s\" ->\n", num, file);
    if (!rc)
         C_printf ("     ~5%s~0\n", last_reparse_err);
    else C_printf ("     \"%s\"\n", slashify2(result, result, slash));
    num++;
  }

  if (dp)
     closedir2 (dp);

  if (num == 0)
     C_printf ("  No '%s%c%s' files found!\n", win_apps, slash, opts.pattern);
  C_putc ('\n');
}

/**
 * Helper function for 'test_auth()'.
 */
static void print_parsing (const char *file, int rc)
{
  int  slash = opt.show_unix_paths ? '/' : '\\';
  char path [_MAX_PATH];
  const char *appdata = getenv ("APPDATA");

  snprintf (path, sizeof(path), "%s\\%s", appdata, file);
  C_printf ("  Parsing ~6%-50s~0", slashify(path, slash));
  if (rc == 0)
       C_puts ("~5FAIL.~0\n");
  else C_puts ("~2OK.~0\n");
}

/**
 * Test the parsing of `%APPDATA%/.netrc`, `%APPDATA%/.authinfo` and `%APPDATA%/envtool.cfg`.
 */
static void test_auth (void)
{
  int rc1, rc2, rc3;

  C_printf ("~3%s():~0\n", __FUNCTION__);

  rc1 = netrc_lookup (NULL, NULL, NULL);
  rc2 = authinfo_lookup (NULL, NULL, NULL, NULL);
  rc3 = envtool_cfg_lookup (NULL, NULL, NULL, NULL);

  print_parsing (".netrc", rc1);
  print_parsing (".authinfo", rc2);
  print_parsing ("envtool.cfg", rc3);
  C_putc ('\n');
}

/**
 * Test PE-file WinTrust crypto signature verification.
 *
 * Optionally calling `get_file_owner()` for each file if
 * option `--owner` was used on command-line.
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
    char   path [_MAX_PATH+2];
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

    wintrust_rc = wintrust_check (file, false, false);

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

  wintrust_dump_pkcs7_cert();
  C_putc ('\n');
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

/**
 * This should run when user-name is:
 *  + `APPVYR-WIN\appveyor` on AppVeyor. Or something like:
 *  + `fv-az319-316\runneradmin` on Gihub Actions.
 *
 * Check if it finds `cmake.exe` on it's `%PATH%` which should contain
 * `c:\Program Files (x86)\CMake\bin`.
 */
static void test_AppVeyor_GitHub (void)
{
  const char *cmake;
  int   rc, save;

  C_printf ("~3%s():~0\n", __FUNCTION__);
  cmake = searchpath ("cmake.exe", "PATH");

  if (!cmake)
  {
    C_printf ("cmake.exe not on %%PATH.\n");
    return;
  }
  save = opt.debug;
  opt.debug = 3;
  rc = popen_run (cmake_version_cb, cmake, "-version");
  C_printf ("popen_run() reported %d: %s\n\n", rc, cmake);
  opt.debug = save;
}

/**
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

/**
 * A simple test for Python functions
 */
static int test_python_funcs (void)
{
  const command_line *c;
  char *str = NULL;
  bool  do_import = false;

  if (halt_flag)
     return (1);

  c = &opt.cmd_line;
  if (c->argc0 > 0)
  {
    char *py_argv [10];
    char  buf [_MAX_PATH];
    int   i, j;

    memset (&py_argv, '\0', sizeof(py_argv));
    i = 0;
    for (j = c->argc0; c->argv[j]; i++, j++)
    {
      /* If 1st arg is not an existing file, assume it's
       * a "import .." statement. Then run it as such.
       */
      if (i == 0 && !FILE_EXISTS(c->argv[j]))
         do_import = true;

      if (i == DIM(py_argv)-1)
      {
        WARN ("Too many Python args. Max: %d.\n", DIM(py_argv)-1);
        break;
      }
      py_argv [i] = c->argv[j];
    }

    if (!do_import)
       py_argv[0] = _fix_path (py_argv[0], buf);
    str = py_execfile ((const char**)py_argv, false, do_import);
  }
  else
    py_test();

  FREE (str);
  return (0);
}

/**
 * The handler for option `-t / --test`.
 */
int do_tests (void)
{
  if (opt.do_evry && opt.evry_host)
  {
    test_ETP_host();
    return (0);
  }

  if (opt.do_python)
     return test_python_funcs();

  if (opt.do_vcpkg)
     return vcpkg_json_parser_test();

  test_split_env ("PATH");
  test_split_env ("MANPATH");

  test_searchpath();
  test_fnmatch();
  test_misc();
  test_PE_wintrust();
  test_slashify();
  test_fix_path();
  test_disk_ready();
  test_SHGetFolderPath();
  test_ReparsePoints();
  test_AppxReparsePoints();

  if (opt.under_appveyor || opt.under_github)
     test_AppVeyor_GitHub();

  test_auth();

#if defined(_MSC_VER) && !defined(_DEBUG)
  C_putc ('\n');
  find_vstudio_init();
#endif

#if defined(USE_SQLITE3)
  // test_sqlite3();
#endif

  return (0);
}

