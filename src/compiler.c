/**
 * \file    compiler.c
 * \ingroup Compiler
 *
 * Functions for extracting internal include and
 * library paths from compliers.
 *
 * It currently handles GNU-compilers (`*gcc.exe`, `*g++.exe`), MSVC,
 * clang-cl, Borland, Intel and Watcom.
 *
 * It also returns `compiler_version()` for the compiler
 * that are supported in the build.
 */
#include "envtool.h"
#include "ignore.h"
#include "cache.h"
#include "color.h"
#include "cfg_file.h"
#include "compiler.h"

#if defined(__CYGWIN__)
#include <cygwin/version.h>
#endif

/**
 * \typedef inc_setup_func
 * The function to the setup function for searching include files.
 */
typedef int (*inc_setup_func) (const struct compiler_info *cc);

/**
 * \typedef lib_setup_func
 * The function to the setup function for searching library files.
 */
typedef int (*lib_setup_func) (const struct compiler_info *cc);

/**
 * \typedef compiler_info
 * The information for each `compiler_type` is kept in this structure.
 */
typedef struct compiler_info {
        char          *short_name;         /**< the short name we're looking for on `%PATH` */
        char          *full_name;          /**< the full name if found `%PATH` */
        char           inc_env [20];       /**< the include lookup env-var. E.g. `%C_INCLUDE_PATH` for all `"*gcc.exe"` compilers */
        char           lib_env [20];       /**< the library lookup env-var. E.g. `%LIBRARY_PATH` for all `"*gcc.exe"` compilers */
        compiler_type  type;               /**< what type is it? */

        inc_setup_func setup_include_path; /**< the include-search setup function */
        const char    *setup_include_fmt;  /**< and it's format used by `popen_run()` */

        lib_setup_func setup_library_path; /**< the library-search setup function */
        const char    *setup_library_fmt;  /**< and it's format used by `popen_run()` */

        BOOL           ignore;             /**< shall we ignore it? */
        BOOL           no_prefix;          /**< shall we check GCC prefixed gcc/g++? */
      } compiler_info;

/**
 * The information for all added compilers is in this list;
 * an array of `compiler_info` created by `compiler_init()`.
 */
static smartlist_t *all_cc = NULL;

static size_t longest_cc          = 0;
static BOOL   ignore_all_gcc      = FALSE;
static BOOL   ignore_all_gpp      = FALSE;
static BOOL   ignore_all_clang    = FALSE;
static BOOL   ignore_all_intel    = FALSE;
static BOOL   ignore_all_borland  = FALSE;
static BOOL   ignore_all_msvc     = FALSE;
static BOOL   ignore_all_watcom   = FALSE;
static BOOL   looks_like_cygwin   = FALSE;
static BOOL   found_search_line   = FALSE;
static BOOL   searching_LLVM_libs = FALSE;

static char   cygwin_fqfn [_MAX_PATH];
static char  *cygwin_root = NULL;
static char  *bcc_root = NULL;
static char  *watcom_dirs [4];
static char  *prev_CPATH;
static char  *prev_C_INCLUDE_PATH;
static char  *prev_CPLUS_INCLUDE_PATH;
static char  *prev_LIBRARY_PATH;

static void   compiler_add (const compiler_info *_cc, const char *inc_env,  const char *lib_env, BOOL from_cache);
static void   compiler_add_gcc (void);
static void   compiler_add_msvc (void);
static void   compiler_add_clang (void);
static void   compiler_add_intel (void);
static void   compiler_add_borland (void);
static void   compiler_add_watcom (void);
static int    dir_array_make_unique (const char *where, const char *compiler_full_name);

static int    GCC_print_compiler_info (const compiler_info *cc, BOOL print_lib_path);
static int    GCC_LLVM_setup_include_path (const compiler_info *cc);
static int    GCC_LLVM_setup_library_path (const compiler_info *cc);

/**
 * The list of prefixes for gnu C/C++ compilers.
 *
 * \eg we try `gcc.exe` ... `avr-gcc.exe` to figure out the
 *     `%C_INCLUDE_PATH`, `%CPLUS_INCLUDE_PATH` and `%LIBRARY_PATH`.
 *     Unless one of the `<path>/<prefix>-gcc.exe` are in the
 *     `[Compiler]` ignore-list in the `"%APPDATA%/envtool.cfg"` config-file.
 *
 * \todo add more prefixes from `%APPDATA%/envtool.cfg` here?
 */
static const char *GCC_prefixes[] = {
                  "",
                  "x86_64-w64-mingw32-",
                  "i386-mingw32-",
                  "i686-w64-mingw32-",
                  "avr-",
                  NULL
                };

/**
 * Help index for `compiler_GCC_prefix_first()` and `compiler_GCC_prefix_next()`.
 */
static int pfx_next_idx = -1;

/**
 * \def INC_DUMP_FMT_GCC
 * \def INC_DUMP_FMT_CLANG
 * \def INC_DUMP_FMT_INTEL_DPCPP
 * \def INC_DUMP_FMT_INTEL_ICX
 *
 * Formats for dumping built-in include paths in GCC and LLVM based compilers.
 * The first parameter could be `" -save-temps"` for a Cygwin GCC program (opt.debug >= 1).
 * The second parameter could be `" -m32"` or `" -m64"` for a dual-mode GCC program.
 * The other parameters are always for `"NUL"` or `"/dev/null"`.
 */
#define INC_DUMP_FMT_GCC          "%s%s -o %s -v -dM -xc -c - < %s 2>&1"            /**< For `gcc.exe` or `g++.exe`*/
#define INC_DUMP_FMT_CLANG        "%s%s -o %s -v -dM -xc -c - < %s 2>&1"            /**< For `clang.exe` */
#define INC_DUMP_FMT_INTEL_DPCPP  "%s%s -o %s -v -dM -xc++ -c -Tc - < %s 2>&1"      /**< For `dpcpp.exe` */
#define INC_DUMP_FMT_INTEL_ICX    "%s%s -o %s -v -dM -xc   -c -Tc - < %s 2>&1"      /**< For `icx.exe` */

/**
 * Formats for dumping built-in library paths in GCC and LLVM based compilers.
 * The first parameter could be `" -m32"` or `" -m64"` for a dual-mode GCC program.
 */
#define LIB_DUMP_FMT        "%s -print-search-dirs 2>&1"
#define LIB_DUMP_FMT_DPCPP  "%s -clang:print-search-dirs 2>&1"
#define LIB_DUMP_FMT_ICX    "%s -clang:print-search-dirs 2>&1"

static BOOL push_env (const char *env_name, char **value);
static BOOL pop_env (const char *env_name, char **value);

/**
 * Push and pop environment values:
 */
#define PUSH_ENV(e) push_env (#e, &prev_##e)
#define POP_ENV(e)  pop_env (#e, &prev_##e)

/**
 * Free the memory allocated by `compiler_init()`.
 */
void compiler_exit (void)
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
 * + if `cc->full_name` is non-NULL (i.e. found), check the ignore-list for that.
 * + if `cc->full_name` is NULL, check the ignore-list for the `cc->short_name`.
 *
 * \eg if the config-file contains a `"ignore = i386-mingw32-gcc.exe"`, and
 *     `"i386-mingw32-gcc.exe"` is not found, don't try to spawn it (since it
 *     will fail).
 *
 * \param[in] cc the the `compiler_info` to check.
 */
static void check_ignore (compiler_info *cc)
{
  BOOL ignore = FALSE;

  /* "envtool --no-prefix .." given and this `cc->short_name` is
   * a prefixed `*-gcc.exe` or `*-g++.exe`. Not used for other `CC_x` types.
   */
  if (cc->no_prefix)
     ignore = TRUE;

  /* "envtool --no-gcc .." given and this `cc->type == CC_GNU_GCC`.
   */
  else if (cc->type == CC_GNU_GCC && opt.no_gcc)
     ignore = TRUE;

  /* "envtool --no-g++ .." given and this `cc->type == CC_GNU_GXX`.
   */
  else if (cc->type == CC_GNU_GXX && opt.no_gpp)
     ignore = TRUE;

  /* "envtool --no-watcom .." given and this `cc->type == CC_WATCOM`.
   */
  else if (cc->type == CC_WATCOM && opt.no_watcom)
     ignore = TRUE;

  /* "envtool --no-borland .." given and this `cc->type == CC_BORLAND`.
   */
  else if (cc->type == CC_BORLAND && opt.no_borland)
     ignore = TRUE;

  /* "envtool --no-clang .." given and this `cc->type == CC_CLANG`.
   */
  else if (cc->type == CC_CLANG && opt.no_clang)
     ignore = TRUE;

  /* "envtool --no-intel .." given and this `cc->type == CC_INTEL`.
   */
  else if (cc->type == CC_INTEL && opt.no_intel)
     ignore = TRUE;

  else if (cc->full_name)
    ignore = cfg_ignore_lookup ("[Compiler]", cc->full_name);

  /* Last chance to check ignore.
   */
  if (!ignore)
     ignore = cfg_ignore_lookup ("[Compiler]", cc->short_name);

  TRACE (1, "Checking %s (%s), ignore: %d.\n",
          cc->short_name, cc->full_name ? cc->full_name : "<not found>", ignore);

  cc->ignore = ignore;
}

/**
 * Cache functions for compilers.
 * Parses the cache keywords `compiler_exe_X`, `compiler_inc_X_Y` and `compiler_lib_X_Y`.
 */
static int get_all_exe_from_cache (void)
{
  int i = 0;
  int found = 0;

  while (1)
  {
    compiler_info cc;
    char    *inc_env, *lib_env;
    char     format [50];

    snprintf (format, sizeof(format), "compiler_exe_%d = %%d,%%d,%%d,%%s,%%s,%%s,%%s", i);
    if (cache_getf (SECTION_COMPILER, format,
                    &cc.type, &cc.ignore, &cc.no_prefix, &inc_env, &lib_env, &cc.short_name, &cc.full_name) != 7)
       break;
    compiler_add (&cc, inc_env, lib_env, TRUE);
    found++;
    i++;
  }
  TRACE (1, "Found %d cached compilers.\n", found);
  return (found);
}

/**
 * Put all compiler programs to the cache.
 * If one is not found on PATH, write a `-` for it's `full_name` instead.
 */
static void put_all_exe_to_cache (void)
{
  int i;
  int max = smartlist_len (all_cc);

  for (i = 0; i < max; i++)
  {
    const compiler_info *cc = smartlist_get (all_cc, i);
    char  format [50];

    snprintf (format, sizeof(format), "compiler_exe_%d = %%d,%%d,%%d,%%s,%%s,%%s,%%s", i);
    cache_putf (SECTION_COMPILER, format, cc->type, cc->ignore, cc->no_prefix,
                cc->inc_env, cc->lib_env, cc->short_name, cc->full_name ? cc->full_name : "-");
  }
}

static int get_inc_dirs_from_cache (const compiler_info *cc)
{
  int i = 0;
  int found = 0;

  while (1)
  {
    char  format [50];
    char *inc_dir;

    snprintf (format, sizeof(format), "compiler_inc_%d_%d = %%s", cc->type, i);
    if (cache_getf (SECTION_COMPILER, format, &inc_dir) != 1)
       break;

    dir_array_add (inc_dir, FALSE);
    found++;
    i++;
  }
  TRACE (1, "Found %d cached inc-dirs for '%s'.\n", found, cc->full_name);
  return (found);
}

/*
 * Build the empty `dir_array` smartlist from the cached information
 * for a specific compiler `type`.
 */
static int get_lib_dirs_from_cache (const compiler_info *cc)
{
  int i = 0;
  int found = 0;

  while (1)
  {
    char  format [50];
    char *lib_dir;

    snprintf (format, sizeof(format), "compiler_lib_%d_%d = %%s", cc->type, i);
    if (cache_getf (SECTION_COMPILER, format, &lib_dir) != 1)
       break;
    dir_array_add (lib_dir, FALSE);
    found++;
    i++;
  }
  TRACE (1, "Found %d cached lib-dirs for '%s'.\n", found, cc->full_name);
  return (found);
}

/*
 * Put the include paths we've found to the cache.
 * But do not cache a directory if it's the Current Directory.
 */
static int put_inc_dirs_to_cache (const compiler_info *cc)
{
  smartlist_t *dir_array = dir_array_head();
  int          i, max = smartlist_len (dir_array);

  for (i = 0; i < max; i++)
  {
    const struct directory_array *d = smartlist_get (dir_array, i);

    if (!d->is_cwd)
       cache_putf (SECTION_COMPILER, "compiler_inc_%d_%d = %s", cc->type, i, d->dir);
  }
  return (max);
}

/*
 * Put the library paths we've found to the cache.
 * But do not cache a directory if it's the Current Directory.
 */
static int put_lib_dirs_to_cache (const compiler_info *cc)
{
  smartlist_t *dir_array = dir_array_head();
  int          i, max = smartlist_len (dir_array);

  for (i = 0; i < max; i++)
  {
    const struct directory_array *d = smartlist_get (dir_array, i);

    if (!d->is_cwd)
       cache_putf (SECTION_COMPILER, "compiler_lib_%d_%d = %s", cc->type, i, d->dir);
  }
  return (max);
}

/**
 * Return compilers full name with correct slash characters.
 */
static const char *compiler_full_name (const compiler_info *cc)
{
  static char fqfn [_MAX_PATH];

  if (opt.show_unix_paths)
     return slashify2 (fqfn, cc->full_name, '/');
  return (cc->full_name);
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
 * Return TRUE if we shall ignore all compilers of this type.
 */
static BOOL check_ignore_all (compiler_type type)
{
  int i, num = 0;
  int ignore = 0;
  int max = smartlist_len (all_cc);

  for (i = 0; i < max; i++)
  {
    const compiler_info *cc = smartlist_get (all_cc, i);

    if (cc->type != type)
       continue;

     num++;
     if (cc->ignore)
        ignore++;
  }
  return (ignore >= num);
}

/**
 * In `--lib` or `--inc` mode, search the `PATH` for all supported compilers.
 *
 * \param[in] print_info      If called from `show_version()`, print additional
 *                            information on each compiler (unless it is in the ignore-list).
 * \param[in] print_lib_path  If called from `show_version()` and `envtool -VVV` was used,
 *                            print the internal GCC library paths too.
 */
void compiler_init (BOOL print_info, BOOL print_lib_path)
{
  struct compiler_info *cc;
  BOOL   at_least_one_gcc = FALSE;
  int    i, max, ignored, num_gxx;
  int    save;

  ASSERT (all_cc == NULL);
  all_cc = smartlist_new();

  save = opt.show_unix_paths;
  if (!print_info)
     opt.show_unix_paths = 0;

  max = get_all_exe_from_cache();
  if (max == 0)
  {
    compiler_add_gcc();
    compiler_add_msvc();
    compiler_add_clang();
    compiler_add_intel();
    compiler_add_borland();
    compiler_add_watcom();
  }

  opt.show_unix_paths = save;

  max = smartlist_len (all_cc);
  for (i = 0; i < max; i++)
      check_ignore (smartlist_get(all_cc, i));

  longest_cc = get_longest_short_name();

  ignore_all_gcc     = check_ignore_all (CC_GNU_GCC);
  ignore_all_gpp     = check_ignore_all (CC_GNU_GXX);
  ignore_all_clang   = check_ignore_all (CC_CLANG);
  ignore_all_intel   = check_ignore_all (CC_INTEL);
  ignore_all_borland = check_ignore_all (CC_BORLAND);
  ignore_all_msvc    = check_ignore_all (CC_MSVC);
  ignore_all_watcom  = check_ignore_all (CC_WATCOM);

  TRACE (1, "ignore_all_gcc: %d, ignore_all_gpp: %d, ignore_all_clang: %d, "
            "ignore_all_intel: %d, ignore_all_borland: %d, ignore_all_watcom: %d.\n",
         ignore_all_gcc, ignore_all_gpp, ignore_all_clang,
         ignore_all_intel, ignore_all_borland, ignore_all_watcom);

  put_all_exe_to_cache();

  if (!print_info)
     return;

  /* Count the number of compilers that were ignored.
   * And print some information if a `CC_GNU_GCC` or `CC_GNU_GXX` compiler
   * was not ignored.
   */
  ignored = num_gxx = 0;
  for (i = 0; i < max; i++)
  {
    cc = smartlist_get (all_cc, i);
    if (cc->ignore)
         ignored++;
    else num_gxx += GCC_print_compiler_info (cc, print_lib_path);

    if (!at_least_one_gcc)
       at_least_one_gcc = (cc->type == CC_GNU_GCC || cc->type == CC_GNU_GXX);
  }

  /* Print the footnote only if at least 1 'gcc*' / 'g++*' was actually found on PATH.
   */
  if (print_lib_path && at_least_one_gcc && num_gxx > 0)
     C_puts ("    ~3(1)~0: internal GCC library paths.\n");

  if (ignored == 0)
     return;

  /* Show the ignored ones:
   */
  C_puts ("\n    Ignored:\n");
  for (i = 0; i < max; i++)
  {
    cc = smartlist_get (all_cc, i);
    if (!cc->ignore)
       continue;

    if (cc->full_name)
         C_printf ("      %s\n", compiler_full_name(cc));
    else C_printf ("      %s  ~5Not found~0\n", cc->short_name);
  }
}

/**
 * Lookup the first GNU prefix.
 */
const char *compiler_GCC_prefix_first (void)
{
  const char *ret;

  pfx_next_idx = 0;
  ret = GCC_prefixes [pfx_next_idx];
  if (!ret)
       pfx_next_idx = -1;
  else pfx_next_idx++;
  return (ret);
}

/**
 * Lookup the next GNU prefix.
 */
const char *compiler_GCC_prefix_next (void)
{
  const char *ret;

  if (pfx_next_idx < DIM(GCC_prefixes)-1)
       ret = GCC_prefixes [pfx_next_idx];
  else ret = NULL;

  if (!ret)
       pfx_next_idx = -1;
  else pfx_next_idx++;
  return (ret);
}

/**
 * Add all supported GNU gcc/g++ compilers to the `all_cc` smartlist.
 * But only add the first `"*gcc.exe"` / `"*g++.exe"` found on `PATH`.
 *
 * The first pair added has no prefix (simply `"gcc.exe"` / `"g++.exe"`).
 * The others pairs use the prefixes in `GCC_prefixes[]`.
 */
static void compiler_add_gcc (void)
{
  const char   *pfx;
  char          short_name[30];
  compiler_info cc;

  for (pfx = compiler_GCC_prefix_first(); pfx;
       pfx = compiler_GCC_prefix_next())
  {
    snprintf (short_name, sizeof(short_name), "%sgcc.exe", pfx);
    cc.no_prefix  = (*pfx && opt.gcc_no_prefixed);
    cc.type       = CC_GNU_GCC;
    cc.short_name = short_name;
    compiler_add (&cc, "C_INCLUDE_PATH", "LIBRARY_PATH", FALSE);

    snprintf (short_name, sizeof(short_name), "%sg++.exe", pfx);
    cc.no_prefix  = (*pfx && opt.gcc_no_prefixed);
    cc.type       = CC_GNU_GXX;
    cc.short_name = short_name;
    compiler_add (&cc, "C_INCLUDE_PATH", "LIBRARY_PATH", FALSE);
  }
}

/**
 * Simple; only add the first `cl.exe` found on `PATH`.
 * \todo
 *   do as with `envtool --path cl.exe` does and add all `cl.exe` found
 *   on PATH to the list.
 */
static void compiler_add_msvc (void)
{
  compiler_info cc;

  cc.type       = CC_MSVC;
  cc.short_name = "cl.exe";
  compiler_add (&cc, "INCLUDE", "LIB", FALSE);
}

/**
 * Search and add supported clang compilers to the `all_cc` smartlist.
 */
static void compiler_add_clang (void)
{
  compiler_info cc;

  cc.type       = CC_CLANG;
  cc.short_name = "clang.exe";
  compiler_add (&cc, "INCLUDE", "LIB", FALSE);

  cc.type       = CC_CLANG;
  cc.short_name = "clang-cl.exe";
  compiler_add (&cc, "INCLUDE", "LIB", FALSE);
}

/**
 * Search and add supported Intel compilers to the `all_cc` smartlist.
 */
static void compiler_add_intel (void)
{
  compiler_info cc;

  cc.type       = CC_INTEL;
  cc.short_name = "icx.exe";
  compiler_add (&cc, "CPATH", "LIB", FALSE);

  cc.short_name = "dpcpp.exe";
  compiler_add (&cc, "CPATH", "LIB", FALSE);
}

/**
 * Search and add supported Borland compilers to the `all_cc` smartlist.
 */
static void compiler_add_borland (void)
{
  static const char *bcc[] = {
                    "bcc32.exe",
                 // "bcc32x.exe",
                    "bcc32c.exe"
                  };
  int i;

  for (i = 0; i < DIM(bcc); i++)
  {
    compiler_info cc;

    cc.type       = CC_BORLAND;
    cc.short_name = (char*) bcc[i];
    compiler_add (&cc, "INCLUDE", "LIB", FALSE);
  }
}

/**
 * Search and add supported Watcom compilers to the `all_cc` smartlist.
 */
static void compiler_add_watcom (void)
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

                    /* Museum stuff; Alpha AXP, C/C++ compilers
                     */
                    "wccaxp.exe",
                    "wppaxp.exe"
#endif
                  };
  int i;

  for (i = 0; i < DIM(wcc); i++)
  {
    compiler_info cc;

    cc.type       = CC_WATCOM;
    cc.short_name = (char*) wcc[i];
    compiler_add (&cc, "WATCOM", "LIB", FALSE);
  }
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

  if (!memcmp(path, &cyg_usr, sizeof(cyg_usr)-1) || !memcmp(path, &cyg_drv, sizeof(cyg_drv)-1))
  {
    looks_like_cygwin = TRUE;
    TRACE (2, "looks_like_cygwin = %d, cygwin_root: '%s'\n", looks_like_cygwin, cygwin_root);
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

/**
 * Print a warning on last error from a GCC or LLVM `popen_run()` callback.
 */
static void compiler_popen_warn (const compiler_info *cc, int rc)
{
  const char *full_name = cc->full_name;
  const char *err       = popen_last_line();
  BOOL  could_be_cygwin = (cc->type == CC_GNU_GCC || cc->type == CC_GNU_GXX);

  if (*err != '\0')
     err = strstr (err, "error: ");

  if (could_be_cygwin && cygwin_fqfn[0])
     full_name = cygwin_fqfn;

  WARN ("Calling %s returned %d.\n", full_name, rc);
  if (err && !opt.quiet)
     C_printf (":\n  %s.\n", err);
}

/**
 * Special extra handling of `clang.exe` and `dpcpp.exe` library paths.
 *
 * If `clang -print-search-dirs` gives:
 * ```
 *  libraries: =C:\ProgramFiles\LLVM-13-32bit\lib\clang\13.0.0
 * ```
 *
 * add:
 * ```
 *   C:\ProgramFiles\LLVM-13-32bit\lib\clang\13.0.0\windows\lib
 *   C:\ProgramFiles\LLVM-13-32bit\lib
 * ```
 *
 * to the list too.
 * Similar for Intel's `dpcpp.exe`.
 */
static void LLVM_extra_library_paths (const char *base_lib)
{
  char dir [_MAX_PATH];
  int  i, is_dir;
  static const char *extras[] = { "\\lib\\windows", "\\..\\.." };

  for (i = 0; i < DIM(extras); i++)
  {
    _strlcpy (dir, base_lib, sizeof(dir));
    str_cat (dir, sizeof(dir), extras[i]);
    _fix_path (dir, dir);
    is_dir = is_directory (dir);
    if (is_dir)
       dir_array_add (dir, FALSE);
    TRACE (2, "is_dir: %d, dir: '%s'\n", is_dir, dir);
  }
}

/**
 * `popen_run()` callback used by `(*cc->setup_include_path()`.
 * The `cc->setup_include_fmt` is set to one of these dump-formats:
 *  \li `INC_DUMP_FMT_GCC`
 *  \li `INC_DUMP_FMT_CLANG`
 *  \li `INC_DUMP_FMT_INTEL_ICX`
 */
static int GCC_LLVM_find_include_path_cb (char *buf, int index)
{
  static const char start[] = "#include <...> search starts here:";
  static const char end[]   = "End of search list.";
  const  char *p;

  if (!found_search_line && !memcmp(buf, &start, sizeof(start)-1))
  {
    found_search_line = TRUE;
    return (0);
  }

  if (found_search_line)
  {
    p = str_ltrim (buf);
    check_if_cygwin (p);

    /* got: "End of search list.". No more paths expected.
     */
    if (!memcmp(buf, &end, sizeof(end)-1))
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
        TRACE (2, "CygWin path detected. Converting '%s' -> '%s'\n", p, result);
        p = _fix_drive (result);
      }
      /* otherwise add 'p' as-is */
    }
    else
#endif
    {
      char buf2 [_MAX_PATH];
      const char *q = str_trim (buf);

      p = _fix_path (q, buf2);

#if !defined(__CYGWIN__)
      if (looks_like_cygwin && cygwin_root)
      {
        snprintf (buf2, sizeof(buf2), "%s%s", cygwin_root, q);
        p = buf2;
      }
#endif
    }

    dir_array_add (p, !stricmp(current_dir, p));
    TRACE (3, "line: '%s'\n", p);
    return (1);
  }

  ARGSUSED (index);
  return (0);
}

/**
 * Callback for all GCC/LLVM based `LIB_DUMP_FMT` commands.
 */
static int GCC_LLVM_find_library_path_cb (char *buf, int index)
{
  const char prefix[] = "libraries: =";
  char  buf2 [_MAX_PATH];
  char  sep[2], *p, *tok, *rc, *end;
  int   i = 0;

  if (strncmp(buf, prefix, sizeof(prefix)-1) || strlen(buf) <= sizeof(prefix))
  {
    TRACE (2, "not a 'libraries' line; buf '%.40s', index: %d\n", buf, index);
    return (0);
  }

  p = buf + sizeof(prefix) - 1;

  check_if_cygwin (p);

  sep[0] = looks_like_cygwin ? ':' : ';';
  sep[1] = '\0';

  for (i = 0, tok = strtok(p, sep); tok; tok = strtok(NULL, sep), i++)
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
        end = rc ? strrchr (rc, '\\') : NULL;
        if (end)
           *end = '\0';
      }
      else
#endif
      {
        rc = _fix_path (tok, buf2);
      }
    }

    TRACE (2, "tok %d: '%s'\n", i, rc);
    if (searching_LLVM_libs)
       LLVM_extra_library_paths (rc);

    dir_array_add (rc, FALSE);
  }
  ARGSUSED (end);
  return (i);
}

/**
 * The include-directory for C++ headers is not reported in the
 * `GCC_LLVM_find_include_path_cb()` callback.
 *
 * Insert a `x/c++` to the list where a `c++` subdirectory is found.
 *
 * \note This function is only called for `cc->type == CC_GNU_GXX`.
 */
static void GCC_add_gxx_path (void)
{
  struct directory_array *d;
  smartlist_t *dir_array = dir_array_head();
  int          i, j, max = smartlist_len (dir_array);
  char         fqfn [_MAX_PATH];

  for (i = 0; i < max; i++)
  {
    d = smartlist_get (dir_array, i);
    snprintf (fqfn, sizeof(fqfn), "%s%c%s", d->dir, DIR_SEP, "c++");
    if (is_directory(fqfn))
    {
      /* This will be added at `dir_array[max+1]`.
       */
      dir_array_add (fqfn, FALSE);

#if 0
      /* Insert the new `c++` directory at the `i`-th element.
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

/**
 * Find the internal include paths for all GCC or LLVM based compilers.
 */
static int GCC_LLVM_setup_include_path (const compiler_info *cc)
{
  const char *save_temps = "";
  const char *cached = "";
  int   found = 0;
  BOOL  is_gcc;

  is_gcc = (cc->type == CC_GNU_GCC || cc->type == CC_GNU_GXX);
  if (is_gcc)
  {
    /* Figure out why Cygwin refuses to return it's 'include paths'
     */
    if (opt.debug >= 1 && !strnicmp(cc->full_name+2, "\\Cygwin", 7))
       save_temps = " -save-temps";
  }

  dir_array_free();
  setup_cygwin_root (cc);

  found = get_inc_dirs_from_cache (cc);
  if (found > 0)
     cached = "cached ";

  if (found == 0)
  {
    found_search_line = FALSE;

    if (cc->type == CC_INTEL)
    {
      /**
       * Push and pop the value of `"%C_INCLUDE_PATH%"` and `"%CPLUS_INCLUDE_PATH%"` if set.
       * Since when using only `"%CPATH%"` to check the Intel compilers, such GNU-C
       * environment variables confuses this LLVM based compiler.
       */
      PUSH_ENV (C_INCLUDE_PATH);
      PUSH_ENV (CPLUS_INCLUDE_PATH);
    }
    else
    {
      /**
       * Push and pop the value of `"%CPATH%"` if set.
       * Since we use this env-var for Intel compilers only.
       */
      PUSH_ENV (CPATH);
    }

    found = popen_run (GCC_LLVM_find_include_path_cb, cc->full_name, cc->setup_include_fmt,
                       save_temps, "", DEV_NULL, DEV_NULL);

    if (cc->type == CC_INTEL)
    {
      POP_ENV (C_INCLUDE_PATH);
      POP_ENV (CPLUS_INCLUDE_PATH);
    }
    else
      POP_ENV (CPATH);
  }

  if (found > 0)
  {
    TRACE (1, "found %d %sinclude paths for %s.\n", found, cached, cc->full_name);
    if (cc->type == CC_GNU_GXX)
       GCC_add_gxx_path();
  }
  else if (*popen_last_line())
  {
    compiler_popen_warn (cc, found);
  }

  dir_array_make_unique (cc->inc_env, cc->full_name);
  return put_inc_dirs_to_cache (cc);
}

/**
 * Find the internal library paths for all GCC or LLVM based compilers.
 */
static int GCC_LLVM_setup_library_path (const compiler_info *cc)
{
  const char *m_cpu  = "";
  const char *cached = "";
  int   found;
  BOOL  is_gcc;

  if (!strcmp(cc->short_name, "clang-cl.exe") ||
      !strcmp(cc->short_name, "icx.exe"))
     return (0);

  is_gcc = (cc->type == CC_GNU_GCC || cc->type == CC_GNU_GXX);
  if (is_gcc)
  {
    /* Tell `*gcc.exe` to return 32 or 64-bit or both types of libs.
     * (assuming it supports the `-m32/-m64` switches.
     */
    if (opt.only_32bit)
         m_cpu = "-m32 ";
    else if (opt.only_64bit)
         m_cpu = "-m64 ";
  }

  dir_array_free();
  setup_cygwin_root (cc);

  found = get_lib_dirs_from_cache (cc);
  if (found > 0)
     cached = "cached ";

  if (found == 0)
  {
    found_search_line   = FALSE;
    searching_LLVM_libs = (cc->type == CC_CLANG || cc->type == CC_INTEL);

    if (cc->type == CC_INTEL)
    {
      PUSH_ENV (C_INCLUDE_PATH);
      PUSH_ENV (CPLUS_INCLUDE_PATH);
    }
    else
      PUSH_ENV (CPATH);

    /**
     * Push and pop the value of `"%LIBRARY_PATH%"` if set.
     * Used for GNU programs when we want the internal GCC library paths.
     * And not be bothered with any directories set in `"%LIBRARY_PATH%"`.
     */
    PUSH_ENV (LIBRARY_PATH);

    found = popen_run (GCC_LLVM_find_library_path_cb, cc->full_name, cc->setup_library_fmt, m_cpu);

    POP_ENV (LIBRARY_PATH);

    if (cc->type == CC_INTEL)
    {
      POP_ENV (C_INCLUDE_PATH);
      POP_ENV (CPLUS_INCLUDE_PATH);
    }
    else
      POP_ENV (CPATH);

    searching_LLVM_libs = FALSE;
  }

  if (found > 0)
  {
    TRACE (1, "found %d %slibrary paths for %s.\n", found, cached, cc->full_name);
  }
  else if (*popen_last_line())
  {
    compiler_popen_warn (cc, found);
  }

#if defined(__CYGWIN__)
  /*
   * The Windows-API lib-dir isn't among the defaults. Just add it
   * at the end of list anyway. In case it was already reported, we'll
   * remove it below.
   */
  if (is_gcc && looks_like_cygwin)
  {
    char result [_MAX_PATH];
    int  rc = cygwin_conv_path (CCP_POSIX_TO_WIN_A, "/usr/lib/w32api", result, sizeof(result));

    if (rc == 0)
       dir_array_add (result, FALSE);
  }
#endif

  dir_array_make_unique (cc->lib_env, cc->full_name);
  return put_lib_dirs_to_cache (cc);
}

/**
 * Check library-paths found by `(*cc->setup_library_path)` or
 * check include-paths found by `(*cc->setup_include_path)`.
 */
static int process_dirs (const compiler_info *cc, int *num_dirs)
{
  smartlist_t *dir_array = dir_array_head();
  int          i, found, max = smartlist_len (dir_array);

  for (i = found = 0; i < max; i++)
  {
    const struct directory_array *arr = smartlist_get (dir_array, i);
    char  dir [_MAX_PATH];

    TRACE (2, "dir: %s\n", arr->dir);

    _fix_path (arr->dir, dir);
    TRACE (2, "dir: %s\n", dir);
    found += process_dir (dir, arr->num_dup, arr->exist, arr->check_empty,
                          arr->is_dir, arr->exp_ok, cc->short_name, HKEY_INC_LIB_FILE);
  }
  *num_dirs = max;
  dir_array_free();
  return (found);
}

/**
 * Print the internal `"*gcc"` or `"*g++"` library paths returned from
 * `(*cc>setup_library_path)`.
 *
 * Since in `*cc->setup_library_path` we cleared `%LIBRARY_PATH%`
 * (using `PUSH_ENV (LIBRARY_PATH)`), this function will print both internal and
 * externally defined `%LIBRARY_PATH%` directories (since now `%LIBRARY_PATH%` is not cleared).
 */
static void GCC_print_internal_library_dirs (const char *env_name, const char *env_value)
{
  struct directory_array *arr;
  smartlist_t            *dir_array = dir_array_head();
  smartlist_t            *list;
  char                  **copy;
  char                    slash = (opt.show_unix_paths ? '/' : '\\');
  int                     i, j, max;
  BOOL                    done_remark = FALSE;

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
  TRACE (3, "Made a 'copy[]' of %d directories.\n", max);

  dir_array_free();

  list = split_env_var (env_name, env_value);
  max  = list ? smartlist_len (list) : 0;
  TRACE (3, "smartlist for '%s' have %d entries.\n", env_name, max);

  for (i = 0; copy[i]; i++)
  {
    BOOL  found = FALSE;
    const char *dir;

    for (j = 0; j < max; j++)
    {
      arr = smartlist_get (list, j);
      dir = slashify2 (arr->dir, arr->dir, slash);
      if (!stricmp(dir, copy[i]))
      {
        found = TRUE;
        break;
      }
    }
    if (!found)
    {
      C_printf ("%*s%s", (int)(longest_cc+8), "", copy[i]);
      if (!done_remark)
         C_puts (" ~3(1)~0");
      C_putc ('\n');
      done_remark = TRUE;
    }
  }

  for (i = 0; copy[i]; i++)
      FREE (copy[i]);
  dir_array_free();
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
static int GCC_print_compiler_info (const compiler_info *cc, BOOL print_lib_path)
{
  BOOL   is_gcc;
  int    rc = 0;
  size_t len = strlen (cc->short_name);

  C_printf ("    %s%*s -> ", cc->short_name, (int)(longest_cc-len), "");
  if (cc->full_name)
       C_printf ("~6%s~0\n", compiler_full_name(cc));
  else C_printf ("~5Not found~0\n");

  if (!cc->full_name || cc->ignore || !print_lib_path)
     return (0);

  is_gcc = (cc->type == CC_GNU_GCC || cc->type == CC_GNU_GXX);
  if (is_gcc && (*cc->setup_library_path)(cc) > 0)
  {
    char *env = getenv_expand (cc->lib_env);

    GCC_print_internal_library_dirs (cc->lib_env, env);
    FREE (env);
    rc = 1;
  }
  FREE (cygwin_root);
  return (rc);
}

/*
 * Do a last warning if number of directories found for `env_var` is zero.
 */
static void compiler_last_warn (const compiler_info *cc, const char *env_var, int num_dirs)
{
  if (num_dirs == 0 && check_ignore_all(cc->type) == 0)
     WARN ("No %s programs returned any %s paths!?.\n", cc->short_name, env_var);
}

int compiler_check_includes (compiler_type type)
{
  const compiler_info *cc = NULL;
  int   found = 0;
  int   num_dirs = 0;
  int   i, max;

  /* Clear any previous error from 'popen_run()'
   */
  *popen_last_line() = '\0';

  max = smartlist_len (all_cc);
  for (i = 0; i < max; i++)
  {
    cc = smartlist_get (all_cc, i);
    if (cc->type != type || cc->ignore || !cc->full_name)
       continue;

    if ((*cc->setup_include_path)(cc) > 0)
    {
      report_header_set ("Matches in %s %%%s path:\n", compiler_full_name(cc), cc->inc_env);
      found += process_dirs (cc, &num_dirs);
    }
    FREE (cygwin_root);
  }

  if (cc)
  {
    if (strcmp(cc->short_name, "wpp386.exe"))
       compiler_last_warn (cc, cc->inc_env, num_dirs);
  }
  return (found);
}

int compiler_check_libraries (compiler_type type)
{
  const compiler_info *cc = NULL;
  int   found = 0;
  int   num_dirs = 0;
  int   i, max;

  /* Clear any previous error from 'popen_run()'
   */
  *popen_last_line() = '\0';

  max = smartlist_len (all_cc);
  for (i = 0; i < max; i++)
  {
    cc = smartlist_get (all_cc, i);
    if (cc->type != type || cc->ignore || !cc->full_name)
       continue;

    if ((*cc->setup_library_path)(cc) > 0)
    {
      report_header_set ("Matches in %s %%%s path:\n", compiler_full_name(cc), cc->lib_env);
      found += process_dirs (cc, &num_dirs);
    }
    FREE (cygwin_root);
  }

  if (cc)
  {
    if (strcmp(cc->short_name, "wpp386.exe"))
       compiler_last_warn (cc, cc->lib_env, num_dirs);
  }
  return (found);
}

/**
 * Common stuff for Watcom.
 */
static int Watcom_setup_dirs (const compiler_info *_cc, const char *dir0, const char *dir1, const char *dir2)
{
  int  i, found, ignored, max;
  BOOL dir2_found;

  max = smartlist_len (all_cc);
  for (i = found = ignored = 0; i < max; i++)
  {
    const compiler_info *cc = smartlist_get (all_cc, i);

    if (!cc->full_name || cc->type != _cc->type)
       continue;

    found++;
    if (cc->ignore)
       ignored++;
  }

  if (found == 0)
  {
    TRACE (1, "No Watcom compilers found.\n");
    return (0);
  }

  if (ignored >= found)
  {
    TRACE (1, "All Watcom compilers were ignored.\n");
    return (0);
  }

  if (!getenv("WATCOM"))
  {
    TRACE (1, "%%WATCOM%% not defined.\n");
    return (0);
  }

  if (!opt.no_cwd)
     dir_array_add (current_dir, TRUE);

  watcom_dirs[0] = getenv_expand (dir0);
  watcom_dirs[1] = getenv_expand (dir1);
  watcom_dirs[2] = getenv_expand (dir2);

  /* This directory exist only on newer Watcom distos.
   * Like "%WATCOM%\\lh" for Linux headers.
   */
  dir2_found = is_directory (watcom_dirs[2]);

  dir_array_add (watcom_dirs[0], FALSE);
  dir_array_add (watcom_dirs[1], FALSE);
  if (dir2_found)
     dir_array_add (watcom_dirs[2], FALSE);

  return (2 + dir2_found);
}

static void free_watcom_dirs (void)
{
  FREE (watcom_dirs[0]);
  FREE (watcom_dirs[1]);
  FREE (watcom_dirs[2]);
  FREE (watcom_dirs[3]);
}

/**
 * Common stuff for Borland.
 *
 * Setup Borland directories for either a `%INC` or `%LIB` search.
 */
static BOOL setup_borland_dirs (const compiler_info *cc, smartlist_parse_func parser)
{
  smartlist_t *dir_list;
  char        *bin_dir;

  bcc_root = STRDUP(cc->full_name);
  bin_dir = strrchr (bcc_root, '\\');
  if (bin_dir)
     *bin_dir = '\0';

  bin_dir = strrchr (bcc_root, '\\');
  if (bin_dir)
     *bin_dir = '\0';

  TRACE (2, "bcc_root: %s, short_name: %s\n", bcc_root, cc->short_name);

  /* The `bcc*.cfg` filename:
   *   <bcc_root>\bccX.exe -> <bcc_root>\bccX.cfg
   */
  dir_list = smartlist_read_file (parser, "%s\\bin\\%.*s.cfg",
                                  bcc_root, strrchr(cc->short_name, '.') - cc->short_name,
                                  cc->short_name);

  if (!dir_list)
     return (FALSE);

  smartlist_free (dir_list);
  return (TRUE);
}

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
static void bcc32_cfg_parse_inc (smartlist_t *sl, const char *line)
{
  const char *isystem = "-isystem @\\..\\";
  char       *copy = strdupa (line);

  copy = str_strip_nl (str_ltrim(copy));
  TRACE (2, "copy: %s.\n", copy);

  if (!strnicmp(copy, isystem, strlen(isystem)))
  {
    char dir [MAX_PATH];

    snprintf (dir, sizeof(dir), "%s\\%s", bcc_root, copy + strlen(isystem));
    TRACE (2, "dir: %s.\n", dir);
    dir_array_add (dir, FALSE);
  }
  else if (!strncmp(copy, "-I", 2))
  {
    split_env_var ("Borland INC", str_ltrim(copy+2));
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
static void bcc32_cfg_parse_lib (smartlist_t *sl, const char *line)
{
  const char *Ldir = "-L@\\..\\";
  char       *copy = strdupa (line);

  copy = str_strip_nl (str_ltrim(copy));
  TRACE (2, "copy: %s.\n", copy);

  if (!strnicmp(copy, Ldir, strlen(Ldir)))
  {
    char dir [MAX_PATH];

    snprintf (dir, sizeof(dir), "%s\\%s", bcc_root, copy + strlen(Ldir));
    TRACE (2, "dir: %s.\n", dir);
    dir_array_add (dir, FALSE);
  }
  else if (!strncmp(copy, "-L", 2))
  {
    split_env_var ("Borland LIB", str_ltrim(copy+2));
  }
  ARGSUSED (sl);
}

/*
 * For others (MSVC, Borland and Watcom), setup the library path search.
 */
static int other_setup_library_path (const struct compiler_info *cc)
{
  int          found = get_lib_dirs_from_cache (cc);
  const char  *cached = (found > 0 ? "cached" : "");
  smartlist_t *dirs;

  if (found == 0)
  {
    if (cc->type == CC_WATCOM)
    {
      found = Watcom_setup_dirs (cc, "%WATCOM%\\lib386", "%WATCOM%\\lib386\\nt", "%WATCOM%\\lib386\\dos");
      free_watcom_dirs();
    }
    else if (cc->type == CC_BORLAND)
    {
      found = setup_borland_dirs (cc, bcc32_cfg_parse_lib);
      FREE (bcc_root);
    }
    else
    {
      dirs = split_env_var (cc->lib_env, getenv(cc->lib_env));
      found = dirs ? smartlist_len (dirs) : 0;
    }
    TRACE (1, "found %d %slibrary paths for %s.\n", found, cached, cc->full_name);
  }

  dir_array_make_unique (cc->lib_env, cc->full_name);
  return put_lib_dirs_to_cache (cc);
}

/*
 * For others (MSVC, Borland and Watcom), setup the include path search.
 */
static int other_setup_include_path (const struct compiler_info *cc)
{
  int          found = get_inc_dirs_from_cache (cc);
  const char  *cached = (found > 0 ? "cached" : "");
  smartlist_t *dirs;

  if (found == 0)
  {
    if (cc->type == CC_WATCOM)
    {
      found = Watcom_setup_dirs (cc, "%WATCOM%\\h", "%WATCOM%\\h\\nt", "%WATCOM%\\lh");
      free_watcom_dirs();
    }
    else if (cc->type == CC_BORLAND)
    {
      found = setup_borland_dirs (cc, bcc32_cfg_parse_inc);
      FREE (bcc_root);
    }
    else
    {
      dirs = split_env_var (cc->inc_env, getenv(cc->inc_env));
      found = dirs ? smartlist_len (dirs) : 0;
    }
    TRACE (1, "found %d %sinclude paths for %s.\n", found, cached, cc->full_name);
  }

  dir_array_make_unique (cc->inc_env, cc->full_name);
  return put_inc_dirs_to_cache (cc);
}

/**
 * Add a compiler to the `all_cc` smartlist.
 *
 * \param[in] cc         the compiler info set by the caller.
 * \param[in] inc_env    the name for the include-path envirnment variable.
 * \param[in] lib_env    the name for the library-path envirnment variable.
 * \param[in] from_cache the information is retrieved from the cache.

 * If we were called from the cache reader, do not call `searchpath()`.
 */
static void compiler_add (const compiler_info *cc,
                          const char *inc_env,
                          const char *lib_env,
                          BOOL from_cache)
{
  compiler_info *cc_copy;
  char    *full_name;
  BOOL     is_gcc;

  ASSERT (cc->short_name);
  ASSERT (cc->short_name[0]);

  cc_copy = MALLOC (sizeof(*cc_copy));
  *cc_copy = *cc;

  if (from_cache)
       full_name = cc->full_name;
  else full_name = searchpath (cc_copy->short_name, "PATH");

  if (full_name && full_name[0] != '-')
       cc_copy->full_name = STRDUP (full_name);
  else cc_copy->full_name = NULL;

  is_gcc = (cc_copy->type == CC_GNU_GCC || cc_copy->type == CC_GNU_GXX);
  if (!is_gcc)
     cc_copy->no_prefix = FALSE;

  /* Set these func-pointers since these are not cached.
   */
  switch (cc_copy->type)
  {
    case CC_GNU_GCC:
    case CC_GNU_GXX:
         cc_copy->setup_include_path = GCC_LLVM_setup_include_path;
         cc_copy->setup_include_fmt  = INC_DUMP_FMT_GCC;
         break;

    case CC_CLANG:
         cc_copy->setup_include_path = GCC_LLVM_setup_include_path;
         cc_copy->setup_include_fmt  = INC_DUMP_FMT_CLANG;
         break;

    case CC_INTEL:
         cc_copy->setup_include_path = GCC_LLVM_setup_include_path;
         if (!strcmp(cc_copy->short_name, "dpcpp.exe"))
              cc_copy->setup_include_fmt  = INC_DUMP_FMT_INTEL_DPCPP;
         else cc_copy->setup_include_fmt  = INC_DUMP_FMT_INTEL_ICX;
         break;

    case CC_MSVC:
    case CC_BORLAND:
    case CC_WATCOM:
         cc_copy->setup_include_path = other_setup_include_path;
         cc_copy->setup_include_fmt  = NULL;
         break;

    default:
         FATAL ("No 'setup_include_path()' function for '%s'\n", cc_copy->short_name);
         break;
  }

  switch (cc_copy->type)
  {
    case CC_GNU_GCC:
    case CC_GNU_GXX:
    case CC_CLANG:
    case CC_INTEL:
         cc_copy->setup_library_path = GCC_LLVM_setup_library_path;
         cc_copy->setup_library_fmt  = LIB_DUMP_FMT;
         break;

    case CC_MSVC:
    case CC_BORLAND:
    case CC_WATCOM:
         cc_copy->setup_library_path = other_setup_library_path;
         cc_copy->setup_library_fmt  = NULL;
         break;

    default:
         FATAL ("No 'setup_library_path()' function for '%s'\n", cc_copy->short_name);
         break;
  }

  cc_copy->short_name = STRDUP (cc->short_name);

  _strlcpy (cc_copy->inc_env, inc_env, sizeof(cc_copy->inc_env));
  _strlcpy (cc_copy->lib_env, lib_env, sizeof(cc_copy->lib_env));

  smartlist_add (all_cc, cc_copy);
}

/**
 * Lookup the first compiler from the `all_cc` smartlist
 * matching the specified compiler `type`.
 */
struct compiler_info *compiler_lookup (compiler_type type)
{
  int i;
  int max = smartlist_len (all_cc);

  for (i = 0; i < max; i++)
  {
    compiler_info *cc = smartlist_get (all_cc, i);

    if (cc->type == type)
       return (cc);
  }
  return (NULL);
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

  if (b->num_dup > 0 || !b->exist)  /* this will get removed */
  {
    a->num_dup = 0;
    return (0);
  }
  return (1);
}

/**
 * Dump the dir_array before or after `smartlist_make_uniq()`.
 */
static int dir_array_dump (smartlist_t *dir_array, const char *where, const char *note)
{
  int i, max = smartlist_len (dir_array);

  TRACE (2, "%s now%s:\n", where, note);

  for (i = 0; i < max; i++)
  {
    const struct directory_array *dir = smartlist_get (dir_array, i);

    TRACE (2, "  dir_array[%d]: exist:%d, num_dup:%d, %s\n",
            (int)i, dir->exist, dir->num_dup, dir->dir);

#ifdef __CYGWIN__
    TRACE (2, "%53s%s\n", "", dir->cyg_dir);
#endif
  }
  return (max);
}

/**
 * The GNU-C report of directories is a mess. Especially all the duplicates and
 * non-canonical names. CygWin is more messy than others. So just remove the
 * duplicates.
 *
 * Futhermore a `"gcc -print-search-dirs"` will print several library directories
 * that doesn't exist.
 *
 * Hence loop over the `dir_array` smartlist and remove all non-unique items.
 * Also used for Watcom's include-paths.
 *
 * \param[in]  env_var  For which env-var this function was used;
 *                      equals `"%NT_INCLUDE%"` for `compiler_check_watcom_includes()`,
 *                      equals `"%CPATH%"` `compiler_check_intel_includes()` or
 *                      `"library paths"` for `*cc->setup_library_path)`.
 *
 * \param[in] compiler_full_name The full pathname of the compiler to work on.
 */
static int dir_array_make_unique (const char *env_var, const char *compiler_full_name)
{
  smartlist_t *dir_array = dir_array_head();
  int          duplicates, old_len, new_len;

  old_len = dir_array_dump (dir_array, env_var, ", non-unique");
  smartlist_make_uniq (dir_array, dir_array_compare, dir_array_wiper);
  new_len = dir_array_dump (dir_array, env_var, ", unique");

  /* This should always be 0 or positive
   */
  duplicates = (old_len - new_len);

  TRACE (1, "found %d duplicates in `%%%s` for %s.\n", duplicates, env_var, compiler_full_name);
  return (duplicates);
}

/**
 * Push: save an environment variable into a variable
 * and then clear it.
 */
static BOOL push_env (const char *env_name, char **value)
{
  char *env = getenv_expand (env_name);
  char  buf [100];
  BOOL  rc = FALSE;

  *value = env;
  if (env)
  {
    snprintf (buf, sizeof(buf), "%s=", env_name);
    putenv (buf);
    SetEnvironmentVariable (env_name, NULL);
    env = getenv (env_name);
    TRACE (2, "%%%s now: '%s'\n", env_name, env ? env : "<none>");
    rc = TRUE;
  }
  return (rc);
}

/*
 * Pop: undo the above clearing.
 * Restore the environment variable.
 */
static BOOL pop_env (const char *env_name, char **value)
{
  char *env, buf [10+MAX_ENV_VAR];
  BOOL  rc = FALSE;

  if (*value)
  {
    snprintf (buf, sizeof(buf), "%s=%s", env_name, *value);
    putenv (buf);
    SetEnvironmentVariable (env_name, *value);
    env = getenv (env_name);
    TRACE (2, "%%%s now: '%s'\n", env_name, env ? env : "<none>");
    FREE (*value);
    rc = TRUE;
  }
  return (rc);
}

/* 'DBG_REL' is used by MSVC, clang-cl and Intel only
 */
#ifdef _DEBUG
  #define DBG_REL "debug"
#else
  #define DBG_REL "release"
#endif

#if defined(__MINGW32__) || defined(__CYGWIN__)
static const char *gcc_version (void)
{
  static char ver [20];
#ifdef __GNUC_PATCHLEVEL__
  snprintf (ver, sizeof(ver), "%d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
  snprintf (ver, sizeof(ver), "%d.%d", __GNUC__, __GNUC_MINOR__);
#endif
  return (ver);
}
#endif

/**
 * Work-buffer for `compiler_version()`.
 */
static char cc_info_buf [100];

/*
 * Since 'icx' also defines '__clang__', put this #if-test here.
 */
#if defined(__INTEL_LLVM_COMPILER)
  const char *compiler_version (void)
  {
    const char *ver = __VERSION__;
    const char *comp = strstr (ver, " Compiler ");
    size_t      len = strlen (ver);

    if (comp)      /* Cut off version at " Compiler 202x" */
       len = comp - ver;
    snprintf (cc_info_buf, sizeof(cc_info_buf), "%.*s, %s", len, ver, DBG_REL);
    return (cc_info_buf);
  }

#elif defined(__clang__)
  const char *compiler_version (void)
  {
    snprintf (cc_info_buf, sizeof(cc_info_buf), "clang-cl %d.%d.%d, %s",
              __clang_major__, __clang_minor__, __clang_patchlevel__, DBG_REL);
    return (cc_info_buf);
  }

#elif defined(_MSC_VER)
  /**
   * Get the MSVC micro version.
   *
   * \see "Predefined Macros" https://msdn.microsoft.com/en-us/library/b0084kay(v=vs.120).aspx
   *
   * \eg `c:\> cl /?` prints:
   *   ```
   *    Microsoft (R) C/C++ Optimizing Compiler Version 18.00.31101.x for x86
   *                       = _MSC_FULL_VER - 180000000  ^----       ^_MSC_BUILD
   *   ```
   */
  static const char *msvc_get_micro_ver (char *buf)
  {
    *buf = '\0';

  #if defined(_MSC_FULL_VER)
    *buf = '.';
    _ultoa (_MSC_FULL_VER % 100000, buf+1, 10);  /* patch version */
  #endif

  #if defined(_MSC_BUILD)
    if (_MSC_BUILD > 0)
    {
      char *end = strrchr (buf, '\0');
      *end++ = '.';
      _itoa (_MSC_BUILD, end, 10);
    }
  #endif
    return (buf);
  }

  const char *compiler_version (void)
  {
    char micro_ver_buf [20];

    snprintf (cc_info_buf, sizeof(cc_info_buf), "Visual-C %d.%02d%s, %s",
              _MSC_VER / 100, _MSC_VER % 100, msvc_get_micro_ver(micro_ver_buf), DBG_REL);
    return (cc_info_buf);
  }

#elif defined(__MINGW32__)
  /*
   * `__MINGW32__` is defined by BOTH mingw.org and by the MinGW-w64
   * project [1]. Only the latter defines `__MINGW64_VERSION_MAJOR`
   * and `__MINGW64__` is defined only when targeting Win64 (`__x86_64__`).
   *
   * [1] http://mingw-w64.sourceforge.net/
   * [2] http://mingw.org   (hi-jacked domain now)
   */
  const char *compiler_version (void)
  {
  #if defined(__MINGW64_VERSION_MAJOR)
    snprintf (cc_info_buf, sizeof(cc_info_buf), "gcc %s, MinGW-w64 %d.%d",
              gcc_version(), __MINGW64_VERSION_MAJOR, __MINGW64_VERSION_MINOR);

  /* mingw.org MinGW. MingW-RT-4+ defines '__MINGW_MAJOR_VERSION'
   */
  #elif defined(__MINGW_MAJOR_VERSION)
    snprintf (cc_info_buf, sizeof(cc_info_buf), "MinGW %d.%d", __MINGW_MAJOR_VERSION, __MINGW_MINOR_VERSION);
  #else
    snprintf (cc_info_buf, sizeof(cc_info_buf), "MinGW %d.%d", __MINGW32_MAJOR_VERSION, __MINGW32_MINOR_VERSION);
  #endif
    return (cc_info_buf);
  }

#elif defined(__CYGWIN__)
  const char *compiler_version (void)
  {
    snprintf (cc_info_buf, sizeof(cc_info_buf), "gcc %s, CygWin %d.%d.%d",
              gcc_version(), CYGWIN_VERSION_DLL_MAJOR/1000, CYGWIN_VERSION_DLL_MAJOR % 1000,
              CYGWIN_VERSION_DLL_MINOR);
    return (cc_info_buf);
  }

#else
  /*
   * Unsupported compiler; `__WATCOMC__`, `__BORLANDC__`  etc.
   */
  const char *compiler_version (void)
  {
    return _strlcpy (cc_info_buf, BUILDER, sizeof(cc_info_buf));
  }
#endif   /* _MSC_VER */

/**
 * When invoking Doxygen in `./doc` (`__DOXYGEN__` is a built-in variable),
 * do not let Doxygen pre-process this.
 */
#if !defined(__DOXYGEN__)
#if defined(__INTEL_LLVM_COMPILER)
  #define CFLAGS   "cflags_icx.h"
  #define LDFLAGS  "ldflags_icx.h"

#elif defined(__clang__)
  #define CFLAGS   "cflags_clang.h"
  #define LDFLAGS  "ldflags_clang.h"

#elif defined(_MSC_VER)
  #define CFLAGS   "cflags_MSVC.h"
  #define LDFLAGS  "ldflags_MSVC.h"

#elif defined(__MINGW32__)
  #define CFLAGS   "cflags_MinGW.h"
  #define LDFLAGS  "ldflags_MinGW.h"

#elif defined(__CYGWIN__)
  #define CFLAGS   "cflags_CygWin.h"
  #define LDFLAGS  "ldflags_CygWin.h"
#endif
#endif  /* !__DOXYGEN__ */

/**
 * Print the CFLAGS and LDFLAGS we were built with.
 * Called in `show_version()` when `envtool -VV` was used.
 *
 * On a `make depend` (`DOING_MAKE_DEPEND` is defined), do not
 * add the above generated files to the dependency output.
 */
void compiler_print_build_cflags (void)
{
#if defined(CFLAGS) && !defined(DOING_MAKE_DEPEND)
  #include CFLAGS
  C_puts ("\n    ");
  C_puts_long_line (cflags, 4);
#else
  C_puts (" Unknown\n");
#endif
}

void compiler_print_build_ldflags (void)
{
#if defined(LDFLAGS) && !defined(DOING_MAKE_DEPEND)
  #include LDFLAGS
  C_puts ("\n    ");
  C_puts_long_line (ldflags, 4);
#else
  C_puts (" Unknown\n");
#endif
}

