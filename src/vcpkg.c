/**\file    vcpkg.c
 * \ingroup Misc
 *
 * \brief
 *   An interface for Microsoft's Package Manager VCPKG. <br>
 *   Ref: https://github.com/Microsoft/vcpkg
 */
#include "envtool.h"
#include "smartlist.h"
#include "color.h"
#include "cache.h"
#include "dirlist.h"
#include "regex.h"
#include "vcpkg.h"

// #define USE_str_to_utf8

/**
 * \def BUF_INIT_SIZE
 * The size of the `malloc()` buffer used in the `BUF_INIT()` macro.
 */
#define BUF_INIT_SIZE 500000

/** From Windows-Kit's <ctype.h> comment:
 *   The C Standard specifies valid input to a ctype function ranges from -1 to 255.
 */
#define VALID_CH(c)   ((c) >= -1 && (c) <= 255)

/**
 * `CONTROL` or `status` file keywords we look for:
 *
 * \def CONTROL_PACKAGE
 *      "Package:" - The name of the package follows this.
 *
 * \def CONTROL_DESCRIPTION
 *      "Description:" - The descriptions of a package follows this.
 *
 * \def CONTROL_FEATURE
 *      "Feature": - What features/options are there for this package?
 *
 * \def CONTROL_HOMEPAGE
 *      "Homepage:" - The URL of it's home-page.
 *
 * \def CONTROL_SOURCE
 *      "Source:" - The source-name is the name of a package following this.
 *
 * \def CONTROL_VERSION
 *      "Version:" - The version-info of a package follows this.
 *
 * \def CONTROL_BUILD_DEPENDS
 *      "Build-Depends:" the list of build dependencies for this package (?).
 *
 * \def CONTROL_DEPENDS
 *      "Depends:" the list of installed dependencies for this package.
 *
 * \def CONTROL_STATUS
 *      "Status:" the install status for this package.
 *
 * \def CONTROL_ARCH
 *      "Architecture:" the architecture (x86/x64/arm etc.) for this package.
 *
 * \def CONTROL_ABI
 *      "Abi:" Some kind of hash value (SHA1, SHA256 or SHA512?) for the installed package.
 */
#define CONTROL_PACKAGE       "Package:"
#define CONTROL_DESCRIPTION   "Description:"
#define CONTROL_FEATURE       "Feature:"
#define CONTROL_HOMEPAGE      "Homepage:"
#define CONTROL_SOURCE        "Source:"
#define CONTROL_VERSION       "Version:"
#define CONTROL_BUILD_DEPENDS "Build-Depends:"
#define CONTROL_DEPENDS       "Depends:"
#define CONTROL_STATUS        "Status:"
#define CONTROL_ARCH          "Architecture:"
#define CONTROL_ABI           "Abi:"

/**
 * \def VCPKG_MAX_NAME
 * \def VCPKG_MAX_VERSION
 * \def VCPKG_MAX_URL
 */
#define VCPKG_MAX_NAME      30   /**< Max size of a `port_node::package` entry. */
#define VCPKG_MAX_VERSION   30   /**< Max size of a `port_node::version` entry. */
#define VCPKG_MAX_URL      200   /**< Max size of a `port_node::homepage` entry. */

/**
 * \enum VCPKG_platform
 * The platform enumeration.
 */
typedef enum VCPKG_platform {
        VCPKG_plat_ALL     = 0,        /**< Package is for all supported OSes. */
        VCPKG_plat_WINDOWS = 0x0001,   /**< Package is for Windows only (desktop, not UWP). */
        VCPKG_plat_UWP     = 0x0002,   /**< Package is for Universal Windows Platform only. */
        VCPKG_plat_LINUX   = 0x0004,   /**< Package is for Linux only. */
        VCPKG_plat_x86     = 0x0008,   /**< Package is for x86 processors only. */
        VCPKG_plat_x64     = 0x0010,   /**< Package is for x64 processors only. */
        VCPKG_plat_ARM     = 0x0020,   /**< Package is for ARM processors only. */
        VCPKG_plat_ANDROID = 0x0040,   /**< Package is for Android only. */
        VCPKG_plat_OSX     = 0x0080    /**< Package is for Apple's OSX only. */
      } VCPKG_platform;

/**
 * \def VCPKG_platform_INVERSE
 * Package is the inverse of the above specified bit.
 */
#define VCPKG_platform_INVERSE  0x8000

/**
 * \typedef port_node
 * The structure of a single VCPKG package entry in the `ports_list`.
 */
typedef struct port_node {
        char   package [VCPKG_MAX_NAME];     /**< The package name. */
        char   version [VCPKG_MAX_VERSION];  /**< The version. */
        char   homepage [VCPKG_MAX_URL];     /**< The URL of it's home-page. */
        char  *description;                  /**< The description. */
        BOOL   have_CONTROL;                 /**< TRUE if this is a CONTROL-node. */

        /** The dependencies; a smartlist of `struct vcpkg_package`.
         */
        smartlist_t *deps;

        /** The features; a smartlist of `char *`.
         */
        smartlist_t *features;
      } port_node;

/**
 * \typedef vcpkg_package
 * The structure of a single installed VCPKG package or the
 * structure of a package-dependency.
 */
typedef struct vcpkg_package {
        char          *package;       /**< The package name */
        char          *version;       /**< The version */
        char          *status;        /**< The install/purge status */
        char          *depends;       /**< What package(s) it depends on */
        char          *arch;          /**< The OS/CPU and ("-static") */
        char          *ABI_tag;       /**< Some kind of hash value (SHA1, SHA256 or SHA512?) */
        VCPKG_platform platform;      /**< The supported OS platform */
        BOOL           Static;        /**< A `-static` package */
        port_node     *link;          /**< A link to the corresponding CONTROL node */
        smartlist_t   *install_info;  /**< A list of `/bin`, `/lib` and `/include` files installed */
      } vcpkg_package;


/**
 * The list of `CONTROL` and `portfile.cmake` file entries.
 * A smartlist of `port_node`.
 */
static smartlist_t *ports_list;

/**
 * A list of available and installable packages found in `CONTROL` files
 * under `<vcpkg_root>/ports`.
 * A smartlist of `vcpkg_package`.
 */
static smartlist_t *available_packages;

/**
 * The `built_packages` is a list of built packages found under
 * `<vcpkg_root>/packages/<package>-<platform>[-static]`.
 *
 *  \eg. A command `vcpkg list pcre`, this could list:
 *    pcre:x64-windows                    8.44             Perl Compatible Regular Expressions
 *    pcre:x86-windows                    8.44             Perl Compatible Regular Expressions
 *    pcre:x86-windows-static             8.44             Perl Compatible Regular Expressions
 *
 *  But without any more details! One has to use the command `vcpkg owns pcre` to
 *  get details such as:
 *   \code
 *     pcre:x86-windows-static: x86-windows-static/debug/lib/pcre16d.lib
 *     pcre:x86-windows-static: x86-windows-static/debug/lib/pcre32d.lib
 *     ...
 *     pcre:x86-windows-static: x86-windows-static/include/pcre.h
 *     pcre:x86-windows-static: x86-windows-static/include/pcre_scanner.h
 *     pcre:x86-windows-static: x86-windows-static/include/pcre_stringpiece.h
 *     pcre:x86-windows-static: x86-windows-static/include/pcrecpp.h
 *     pcre:x86-windows-static: x86-windows-static/include/pcrecpparg.h
 *     pcre:x86-windows-static: x86-windows-static/include/pcreposix.h
 *     pcre:x86-windows-static: x86-windows-static/lib/pcre.lib
 *     pcre:x86-windows-static: x86-windows-static/lib/pcre16.lib
 *     pcre:x86-windows-static: x86-windows-static/lib/pcre32.lib
 *     pcre:x86-windows-static: x86-windows-static/lib/pcrecpp.lib
 *     pcre:x86-windows-static: x86-windows-static/lib/pcreposix.lib
 *     pcre:x86-windows-static: x86-windows-static/share/pcre/copyright
 *     pcre:x86-windows-static: x86-windows-static/share/pcre/vcpkg_abi_info.txt
 *     gdk-pixbuf:x86-windows: x86-windows/tools/gdk-pixbuf/pcre.dll
 *     glib:x86-windows: x86-windows/tools/glib/pcre.dll
 *     ...
 *  \endcode
 *
 * So the contents would be installed under:
 *  \code
 *   <vcpkg_root>/packages/pcre_x64-windows
 *   <vcpkg_root>/packages/pcre_x86-windows
 *   <vcpkg_root>/packages/pcre_x86-windows-static
 *  \endcode
 *
 *  And if `pcre:x86-windows-static` was actually successfully built, it's .lib-files would be:
 *  `<vcpkg_root>/installed/x86-windows-static/lib/pcre*.lib`  (ready for use by a `_RELEASE` version build)
 *
 * But these could also be left-overs from broken `vcpkg install <pkg>` (in case vcpkg would just cache them).
 */
static smartlist_t *built_packages;

/**
 * A list of actually installed packages found under `<vcpkg_root>/installed/<platform>[-static]`.
 *
 * A smartlist of `vcpkg_package`.
 */
static smartlist_t *installed_packages;

/**
 * A list of "already found" sub-packages.
 */
static smartlist_t *sub_package_list;

/**
 * Save nodes relative to this directory to save memory.
 */
static char *vcpkg_root;

/**
 * The fully qualified name of 'vcpkg.exe'.
 */
static char *vcpkg_exe;

/**
 * Save last error-text here.
 * (no trailing `".\n"` here).
 */
static char last_err_str [200];

/**
 * The recursion-level for sub-dependency checking.
 */
static int sub_level = 0;

/**
 * Print details on installed packages only.
 */
static BOOL only_installed = TRUE;

/**
 * Do we have a `<vcpkg_root>\\installed` directory?
 */
static BOOL have_installed_dir = FALSE;

/**
 * Memory allocated for the `<vcpkg_root>/installed/vcpkg/status` file.
 * This is allocated in `parse_status_file()` and not freed until `vcpkg_exit()` is called.
 */
static char *f_status_mem = NULL;

/**
 * The VCPKG version information.
 */
static struct ver_info vcpkg_ver;

/**
 * The platforms we support when parsing in `make_depend_platform()`.
 */
static const struct search_list platforms[] = {
                              { VCPKG_plat_WINDOWS, "windows" },
                              { VCPKG_plat_LINUX,   "linux"   },
                              { VCPKG_plat_UWP,     "uwp"     },
                              { VCPKG_plat_ARM,     "arm"     },
                              { VCPKG_plat_ANDROID, "android" },
                              { VCPKG_plat_OSX,     "osx"     },
                              { VCPKG_plat_x86,     "x86"     },
                              { VCPKG_plat_x64,     "x64"     },
                            };

static BOOL                 get_control_node (int *index_p, port_node **node_p, const char *package_spec);
static const char          *get_depend_name (const vcpkg_package *dep);
static unsigned             get_depend_platform (unsigned platform, BOOL *Not);
static const vcpkg_package *get_install_info (int *index_p, const char *package);
static BOOL                 get_installed_info (vcpkg_package *pkg);
static const char          *get_installed_dir (const vcpkg_package *pkg);
static void                 pkg_dump_file_info (const vcpkg_package *pkg, const char *indent);

static int  print_top_dependencies (FMT_buf *fmt_buf, const port_node *node, int indent);
static int  print_sub_dependencies (FMT_buf *fmt_buf, const port_node *node, int indent);
static BOOL print_install_info     (FMT_buf *fmt_buf, const char *package, int indent1);

/**
 * regex stuff
 */
static regex_t    re_hnd;
static regmatch_t re_matches[3];  /**< regex sub-expressions */
static int        re_err;         /**< last regex error-code */
static char       re_errbuf[10];  /**< regex error-buffer */

/**
 * Print the sub expressions in `re_matches[]`.
 */
static void regex_print (const regex_t *re, const regmatch_t *rm, const char *str)
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
     C_puts ("None");
  C_putc ('\n');
}

/**
 * Free the memory allocated to `re_hnd`.
 */
static void regex_free (void)
{
  if (re_hnd.buffer)
     regfree (&re_hnd);
}

/**
 * Try to match `str` against the regular expression in `pattern`.
 */
static BOOL regex_match (const char *str, const char *pattern)
{
  memset (&re_matches, '\0', sizeof(re_matches));
  if (!re_hnd.buffer)
  {
    re_err = regcomp (&re_hnd, pattern, REG_EXTENDED | REG_ICASE);
    if (re_err)
    {
      regerror (re_err, &re_hnd, re_errbuf, sizeof(re_errbuf));
      WARN ("Invalid regular expression \"%s\": %s (%d)\n", pattern, re_errbuf, re_err);
      regex_free();
      return (FALSE);
    }
  }

  re_err = regexec (&re_hnd, str, DIM(re_matches), re_matches, 0);
  DEBUGF (1, "regex() pattern '%s' against '%s'. re_err: %d\n", pattern, str, re_err);

  if (re_err == REG_NOMATCH)
     return (FALSE);

  if (re_err == REG_NOERROR)
     return (TRUE);

  regerror (re_err, &re_hnd, re_errbuf, sizeof(re_errbuf));
  DEBUGF (0, "Error while matching \"%s\": %s (%d)\n", str, re_errbuf, re_err);
  return (FALSE);
}

static void regex_test (const char *str, const char *pattern)
{
  if (regex_match(str, pattern))
     regex_print (&re_hnd, re_matches, str);
}

#if defined(USE_str_to_utf8)
static wchar_t *str_to_utf8 (const char *text, wchar_t *w_out, size_t w_out_size)
{
  int      w_size = MultiByteToWideChar (CP_UTF8, MB_PRECOMPOSED | MB_USEGLYPHCHARS, text, -1, NULL, 0);
  wchar_t *w_buf = alloca (w_out_size);

  if (w_size == 0 || w_size > w_out_size)
     return (NULL);

  if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, w_buf, w_size))
     return (NULL);
  NormalizeString (NormalizationKC, w_buf, -1, w_out, w_size);
  return (w_out);
}
#endif

/**
 * Return the value of `only_installed`
 */
BOOL vcpkg_get_only_installed (void)
{
  return (only_installed);
}

/**
 * Set the value of `only_installed` and return the current value.
 */
BOOL vcpkg_set_only_installed (BOOL True)
{
  BOOL current = only_installed;

  only_installed = True;
  return (current);
}

/**
 * Manage a list of already found packages visited in `print_sub_dependencies()`.
 * So they are not recursed and printed more than 1 time.
 */
static BOOL sub_package_found (const char *package)
{
  int i, max = smartlist_len (sub_package_list);

  for (i = 0; i < max; i++)
      if (smartlist_get(sub_package_list,i) == package)
         return (TRUE);
  return (FALSE);
}

/**
 * Dump the information of `CONTROL` nodes with a `node->package`
 * matching `package_spec`.
 *
 * The output of 'vcpkg search' is similar to this:
 * ```
 *  3fd                  2.6.2            C++ Framework For Fast Development
 *  abseil               2018-09-18       an open-source collection designed to augment the C++ standard library. Abseil...
 *  ace                  6.5.2            The ADAPTIVE Communication Environment
 *  alac                 2017-11-03-c3... The Apple Lossless Audio Codec (ALAC) is a lossless audio codec developed by A...
 *  alac-decoder         0.2              ALAC C implementation of a decoder, written from reverse engineering the file ...
 *  alembic              1.7.9            Alembic is an open framework for storing and sharing scene data that includes ...
 *  allegro5             5.2.4.0          Allegro is a cross-platform library mainly aimed at video game and multimedia ...
 *  anax                 2.1.0-3          An open source C++ entity system. <https://github.com/miguelmartin75/anax>
 *  angle                2017-06-14-8d... A conformant OpenGL ES implementation for Windows, Mac and Linux. The goal of ...
 * ```
 *
 * \note The truncated descriptions and no dependency information.
 *
 * We'll do it recursively if `opt.verbose >= 1`.
 * Something like this, with `envtool --vcpkg=all -vv 3f*' showing `sub_level == 2`:
 * ```
 *   3fd:                  C++ Framework For Fast Development
 *   version:              2.6.2
 *   dependencies:         boost-lockfree (windows), boost-regex (windows), poco (windows), sqlite3, rapidxml
 *     boost-lockfree:
 *       boost-align
 *         boost-assert
 *         boost-config
 *         boost-core
 *         boost-static-assert
 *         boost-throw-exception
 *         boost-vcpkg-helpers: <none>
 *       boost-array
 *         boost-assert
 *         boost-config
 *         boost-core
 *         boost-detail
 *         boost-static-assert
 *         boost-throw-exception
 *         boost-vcpkg-helpers: <none>
 *       boost-assert
 *         boost-config
 *         boost-vcpkg-helpers: <none>
 *       boost-atomic
 *         boost-assert
 *         boost-build
 *         boost-config
 *         boost-integer
 *         boost-modular-build-helper
 *         boost-type-traits
 *         boost-vcpkg-helpers: <none>
 *       boost-config
 *         boost-vcpkg-helpers: <none>
 *       boost-core
 *       boost-integer
 *       boost-mpl
 *       boost-parameter
 *       boost-predef
 *       boost-static-assert
 *       boost-tuple
 *       boost-type-traits
 *       boost-utility
 *       boost-vcpkg-helpers: <none>
 *
 *     boost-regex:
 *       boost-assert
 *       boost-build
 *       boost-compatibility
 *       boost-concept-check
 *       boost-config
 *       boost-container-hash
 *       boost-core
 *       boost-detail
 *       boost-integer
 *       boost-iterator
 *       boost-modular-build-helper
 *       boost-mpl
 *       boost-smart-ptr
 *       boost-static-assert
 *       boost-throw-exception
 *       boost-type-traits
 *       boost-vcpkg-helpers: <none>
 *
 *     poco:
 *       zlib
 *       pcre
 *       sqlite3
 *       expat
 *
 *     sqlite3:   <none>
 *     rapidxml:  <none>
 *
 *  1 match found for "3f*" with 33 unique sub-dependants.
 * ```
 */
static unsigned vcpkg_find_internal (FMT_buf *fmt_buf, const char *package_spec)
{
  port_node *node;
  int        i = 0,  indent, padding, num_deps;
  unsigned   matches = 0;

  while (get_control_node(&i, &node, package_spec))
  {
    const char *package = node->package;

    matches++;
    padding = VCPKG_MAX_NAME - (int)strlen (package);
    padding = max (0, padding-2);

    if (sub_level == 0)
    {
      indent = buf_printf (fmt_buf, "  ~6%s~0: %*s", package, padding, "") - 4;

#ifdef USE_str_to_utf8
      if (node->description)
      {
        char     buf [1000];
        wchar_t  wbuf [1000];
        wchar_t *utf8 = str_to_utf8 (node->description, wbuf, DIM(wbuf));

        snprintf (buf, sizeof(buf), "%S", utf8);
        buf_puts_long_line (fmt_buf, buf, indent);
      }
      else
        buf_puts_long_line (fmt_buf, "<none>", indent);
#else
      buf_puts_long_line (fmt_buf, node->description ? node->description : "<none>", indent);
#endif

      buf_printf (fmt_buf, "  %-*s%s\n", indent-2, "version: ", node->version[0] ? node->version : "<none>");
      buf_printf (fmt_buf, "  %-*s%s\n", indent-2, "homepage:", node->homepage);
    }
    else
    {
      indent = 2;
      buf_printf (fmt_buf, "%-*s%s:\n", indent + 2*sub_level, "", package);
    }

    num_deps = print_top_dependencies (fmt_buf, node, indent-2);

    if (num_deps > 1 && opt.verbose >= 1)
       print_sub_dependencies (fmt_buf, node, indent);

    if (sub_level == 0)
    {
      if (print_install_info(fmt_buf, package, indent-2))
           C_puts (fmt_buf->buffer_start);
      else matches--;

      buf_reset (fmt_buf);
    }
  }
  return (matches);
}

unsigned vcpkg_find (const char *package_spec)
{
  FMT_buf  fmt_buf;
  unsigned num;

  sub_package_list = smartlist_new();

  vcpkg_init();

  BUF_INIT (&fmt_buf, BUF_INIT_SIZE, 1);

  sub_level = 0;
  num = vcpkg_find_internal (&fmt_buf, package_spec);
  sub_level = 0;

  BUF_EXIT (&fmt_buf);

  smartlist_free (sub_package_list);
  return (num);
}

/**
 * Print the package sub-dependencies for a `CONTROL` node.
 */
static int print_sub_dependencies (FMT_buf *fmt_buf, const port_node *node, int indent)
{
  const vcpkg_package *dep1, *dep2;
  int   i, i_max, j, j_max, found;

  if (!node->deps || smartlist_len(node->deps) == 0)
  {
    if (sub_level == 0)
       buf_printf (fmt_buf, "%-*s<none>\n", indent, "");
    return (0);
  }

  i_max = smartlist_len (node->deps);
  j_max = smartlist_len (available_packages);

  for (i = found = 0; i < i_max; i++)
  {
    dep1 = smartlist_get (node->deps, i);

    for (j = 0; j < j_max; j++)
    {
      dep2 = smartlist_get (available_packages, j);
      if (dep1->package == dep2->package && !sub_package_found(dep1->package))
      {
        found++;

        /* Add to "already found list"
         */
        smartlist_add (sub_package_list, (void*)dep1->package);

        /* Will call 'get_control_node()' only once
         */
        ++sub_level;
        vcpkg_find_internal (fmt_buf, dep1->package);
        --sub_level;
      }
    }
  }
#if 0
  if (found == 0 && sub_level == 0)
     buf_puts (fmt_buf, "None found\n");
#endif
  return (found);
}

/**
 * Print the package top-dependencies for a `CONTROL` node.
 * Return the number of dependencies at top.
 */
static int print_top_dependencies (FMT_buf *fmt_buf, const port_node *node, int indent)
{
  const  vcpkg_package *d;
  size_t longest_package = 0;
  int    dep, max_dep;

  if (sub_level > 0)
  {
    if (!node->deps)
       return (0);
  }
  else
  {
    buf_printf (fmt_buf, "  %-*s", indent, "dependencies:");
    if (!node->deps)
    {
      buf_puts (fmt_buf, "<none>\n");
      return (0);
    }
  }

  max_dep = smartlist_len (node->deps);

  /* First, get the value for 'longest_package'
   */
  for (dep = 0; dep < max_dep; dep++)
  {
    d = smartlist_get (node->deps, dep);
    longest_package = max (strlen(d->package), longest_package);
  }

  for (dep = 0; dep < max_dep; dep++)
  {
    BOOL Not;

    d = smartlist_get (node->deps, dep);
    get_depend_platform (d->platform, &Not);

    if (sub_level > 0)
       buf_printf (fmt_buf, "%-*s%s;\n", indent + 2*sub_level, "", d->package);
    else
    {
      if (dep > 0)
         buf_printf (fmt_buf, "%-*s", indent+2, "");

      buf_printf (fmt_buf, "%-*s  platform: ", (int)longest_package, d->package);
      if (Not)
           buf_printf (fmt_buf, "!(%s)", get_depend_name(d));
      else buf_printf (fmt_buf, "%s", get_depend_name(d));
      buf_printf (fmt_buf, " (0x%04X)\n", d->platform);
    }
  }
  return (max_dep);
}

/**
 * Get the `deps->platform` and any inverse of it.
 */
static unsigned get_depend_platform (unsigned platform, BOOL *Not)
{
  if (Not)
     *Not = FALSE;

  if (platform == VCPKG_plat_ALL)
     return (VCPKG_plat_ALL);

  if (platform & VCPKG_platform_INVERSE) /* Sign bit set */
  {
    if (Not)
       *Not = TRUE;
    platform &= ~VCPKG_platform_INVERSE;
  }
  return (platform);
}

/**
 * Split a line like "!uwp&!windows" and fill `dep` list for it.
 * On the first call, do it recursively.
 */
static void make_depend_platform (vcpkg_package *dep, char *platform, BOOL recurse)
{
  BOOL     Not = FALSE;
  unsigned val;

  if (*platform == '!')
  {
    platform++;
    Not = TRUE;
  }

  val = list_lookup_value (platform, platforms, DIM(platforms));
  if (val != UINT_MAX)
  {
    dep->platform |= (VCPKG_platform) val;

    /* Not for this `deps->platform`?
     */
    if (Not)
       dep->platform |= VCPKG_platform_INVERSE;
  }
  else if (recurse)
  {
    char *tok_end, *tok = _strtok_r (platform, "&", &tok_end);

    while (tok)
    {
      make_depend_platform (dep, tok, FALSE);
      tok = _strtok_r (NULL, "&", &tok_end);
    }
  }
}

/**
 * Split a line like "x86-windows" and (on the first call, do it recursively)
 * return the `VCPKG_platform_x` value for it.
 */
static enum VCPKG_platform make_package_platform (char *platform, BOOL recurse)
{
  VCPKG_platform ret = 0;
  unsigned       val = list_lookup_value (platform, platforms, DIM(platforms));

  if (val != UINT_MAX)
     return (VCPKG_platform)val;

  if (recurse)
  {
    char *tok_end, *tok = _strtok_r (platform, "-", &tok_end);

    while (tok)
    {
      ret |= make_package_platform (tok, FALSE);
      tok = _strtok_r (NULL, "-", &tok_end);
    }
  }
  return (ret);
}

/**
 * Split a line like "curl_x86-windows[-static]" into cpu and OS and check if these are legal.
 */
static BOOL legal_package_name (const char *package)
{
  char *cpu, *copy = NULL;
  BOOL  cpu_ok = FALSE;

  if (!package)
     goto quit;

  copy = STRDUP (package);
  cpu  = strchr (copy, '_');
  DEBUGF (2, "package: '%s', cpu: '%.4s'.\n", package, cpu ? cpu+1 : "<None>");

  if (!cpu)
     goto quit;

  cpu++;
  cpu_ok = (!strnicmp(cpu, "x86-", 4) || !strnicmp(cpu, "x64-", 4));
  if (cpu_ok)
  {
    cpu += 4;
    if (list_lookup_value (cpu, platforms, DIM(platforms)) == UINT_MAX)
       cpu_ok = FALSE;
  }

quit:
  FREE (copy);
  return (cpu_ok);
}

/**
 * Search the global `available_packages` for a matching `dep1->package`.
 * If found return a pointer to it.
 *
 * If not found, create a new `vcpkg_package` entry and add to the
 * `available_packages` list. And then return a pointer to it.
 *
 * This is to save memory; no need to call `CALLOC()` for every `port_node::deps`
 * entry in the list. Hence many `port_node::deps` entries will have pointer to
 * the same location.
 */
static void *find_or_alloc_dependency (const vcpkg_package *dep1)
{
  vcpkg_package *dep2;
  int   i, max = smartlist_len (available_packages);

  for (i = 0; i < max; i++)
  {
    dep2 = smartlist_get (available_packages, i);
    if (!memcmp(dep2, dep1, sizeof(*dep2)))
       return (dep2);
  }
  dep2 = CALLOC (sizeof(*dep2), 1);
  memcpy (dep2, dep1, sizeof(*dep2));
  dep2->package = STRDUP (dep1->package);
  smartlist_add (available_packages, dep2);
  return (dep2);
}

/**
 * Split a line like:
 *   "openssl (!uwp&!windows), curl (!uwp&!windows)"
 *
 * first into tokens of:
 *   "openssl (!uwp&!windows)" and "curl (!uwp&!windows)".
 *
 * If a token contains a "(xx)" part, pass that to `make_depend_platform()`
 * which recursively figures out the platforms for the package.
 *
 * Add a package-dependency to `node` as long as there are more ","
 * tokens in `str` to parse.
 */
static void make_dependencies (port_node *node, char *str)
{
  char *tok, *tok_end, *p;
  int   str0 = str[0];

  if (strchr(str, ')') > strchr(str, '('))
     DEBUGF (2, "str: '%s'\n", str);

  if (str0 == '\0' || (VALID_CH(str0) && isspace(str0) && str[1] == '\0'))
  {
    DEBUGF (2, "Empty dependencies! str: '%s'\n", str);
    return;
  }

  ASSERT (node->deps == NULL);
  node->deps = smartlist_new();

  tok = _strtok_r (str, ",", &tok_end);

  while (tok)
  {
    vcpkg_package dep;
    char  package [2*VCPKG_MAX_NAME];
    char  platform [51];
    char *l_paren;

    memset (&dep, '\0', sizeof(dep));

    p = str_trim (tok);
    p = _strlcpy (package, p, sizeof(package));

    l_paren = strchr (package, '(');
    if (l_paren && sscanf(l_paren+1, "%50[^)])", platform) == 1)
    {
      *l_paren = '\0';
      p = str_trim (package);
      DEBUGF (2, "platform: '%s', tok: '%s', tok_end: '%s'\n", platform, tok, tok_end);
      make_depend_platform (&dep, platform, TRUE);
    }
    dep.package = p;
    smartlist_add (node->deps, find_or_alloc_dependency(&dep));

    tok = _strtok_r (NULL, ",", &tok_end);
  }
}

/**
 * Parse the content of a `CONTROL` file and add it's contents to `node`.
 */
static void CONTROL_parse (port_node *node, const char *file)
{
  FILE *f = fopen (file, "r");
  char *p, buf [1000]; /* Enough? */

  if (!f)
  {
    DEBUGF (2, "Failed to open %s.\n", file);
    return;
  }

  while (fgets(buf,sizeof(buf)-1,f))
  {
    str_strip_nl (buf);
    p = str_ltrim (buf);

    DEBUGF (4, "p: '%s'\n", p);

    /* In case 'node->homepage' etc. contains a '~', replace with "~~".
     */
    if (!node->description && str_match(p,CONTROL_DESCRIPTION,&p))
    {
      node->description = STRDUP (p);
      str_replace2 ('~', "~~", node->description, sizeof(node->description));
    }
    else if (!node->version[0] && str_match(p,CONTROL_VERSION,&p))
    {
      _strlcpy (node->version, p, sizeof(node->version));
      str_replace2 ('~', "~~", node->version, sizeof(node->version));
    }
    else if (str_match(p,CONTROL_HOMEPAGE,&p))
    {
      _strlcpy (node->homepage, p, sizeof(node->homepage));
      str_replace2 ('~', "~~", node->homepage, sizeof(node->homepage));
    }
    else if (str_match(p,CONTROL_FEATURE,&p))
    {
      if (!node->features)
         node->features = smartlist_new();
      DEBUGF (3, "Adding feature: '%s'\n", p);
      smartlist_add (node->features, STRDUP(p));
    }
    else if (!node->deps && str_match(p,CONTROL_BUILD_DEPENDS,&p))
    {
      strtok (p, "[");
      if (opt.debug >= 10)
         regex_test (p, "[[:alnum:]_-]+");
      make_dependencies (node, p);
    }
  }
  fclose (f);
}

/**
 * \todo
 *  Parse `file` for LOCAL package location or REMOTE package URL
 */
static void portfile_cmake_parse (port_node *node, const char *file)
{
  ARGSUSED (node);
  ARGSUSED (file);
}

/**
 * Traverse a `dir` relative to `vcpkg_root` looking for sub-directories
 * (first level only).
 *
 * \return A smartlist of sub-directories.
 *         All these are relative to `vcpkg_root` to save some memory.
 */
static smartlist_t *build_dir_list (const char *dir)
{
  struct dirent2 *de;
  smartlist_t    *dir_list;
  DIR2           *dp;
  char            dir2 [_MAX_PATH];
  size_t          ofs = strlen (vcpkg_root) + 1;

  snprintf (dir2, sizeof(dir2), "%s\\%s", vcpkg_root, dir);
  if (!is_directory(dir2) || (dp = opendir2x(dir2, NULL)) == NULL)
  {
    snprintf (last_err_str, sizeof(last_err_str), "No such directory %s", dir2);
    return (NULL);
  }

  dir_list = smartlist_new();

  while ((de = readdir2(dp)) != NULL)
  {
    if (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY)
    {
      const char *d = de->d_name + ofs;

      DEBUGF (2, "Adding '%s'\n", d);
      smartlist_add (dir_list, STRDUP(d));
    }
  }
  closedir2 (dp);
  return (dir_list);
}

/**
 * Look in `<vcpkg_root>\\ports\\dir` for `CONTROL` or `portfile.cmake` files and add
 * to `ports_list`.
 */
static void build_ports_list (const char *dir, int ports_index)
{
  port_node  *node;
  char        file [_MAX_PATH];
  const char *pkg = dir + strlen ("ports\\");

  snprintf (file, sizeof(file), "%s\\ports\\%s\\CONTROL", vcpkg_root, pkg);
  if (FILE_EXISTS(file))
  {
    DEBUGF (2, "%d: Building port-node for %s.\n", ports_index, file);

    node = CALLOC (sizeof(*node), 1);
    node->have_CONTROL = TRUE;
    _strlcpy (node->package, pkg, sizeof(node->package));
    strcpy (node->homepage, "<none>");

    CONTROL_parse (node, file);
    smartlist_add (ports_list, node);
  }

  snprintf (file, sizeof(file), "%s\\%s\\portfile.cmake", vcpkg_root, pkg);
  if (FILE_EXISTS(file))
  {
    DEBUGF (2, "%d: Building port-node for %s.\n", ports_index, file);

    node = CALLOC (sizeof(*node), 1);
    node->have_CONTROL = FALSE;
    _strlcpy (node->package, pkg, sizeof(node->package));
    strcpy (node->homepage, "<none>");

    portfile_cmake_parse (node, file);
    smartlist_add (ports_list, node);
  }
}

/**
 * Build up the `ports_list` smartlist from file-cache.
 */
static void build_ports_list_from_cache (void)
{
  port_node *node;
  int  rc, i = 0;

  while (1)
  {
    char  format [1000];  /* Room for e.g. 'port_node_0 = 3fd,2.6.2-3,<none>,1,"C++ Framework For Fast Development"' */
    char *package = NULL, *version = NULL, *homepage = NULL, *description = NULL;
    int   have_CONTROL = 0;

    snprintf (format, sizeof(format), "port_node_%d = %%s,%%s,%%s,%%d,%%s", i);
    rc = cache_getf (SECTION_VCPKG, format, &package, &version, &homepage, &have_CONTROL, &description);
    DEBUGF (2, "port_node from cache (%s\\%s):\n"
               "     package: '%s', version: '%s', homepage: '%s', have_CONTROL: %d, description: '%s'.\n",
            vcpkg_root, package, package, version, homepage, have_CONTROL, description);

    if (rc != 5)
      break;

    node = CALLOC (sizeof(*node), 1);
    node->have_CONTROL = TRUE;
    node->description = STRDUP (str_unquote(description));
    _strlcpy (node->package, package, sizeof(node->package));
    _strlcpy (node->homepage, homepage, sizeof(node->homepage));
    _strlcpy (node->version, version, sizeof(node->version));
    smartlist_add (ports_list, node);
    i++;
  }
}

static char **split_value_to_vector (char *value)
{
  static char *vector [10];
  char  *p, *tok_end;
  int    i;

  memset (&vector, '\0', sizeof(vector));
  vector[0] = value;
  p = _strtok_r (value, ",", &tok_end);
  for (i = 1; p && i < DIM(vector)-1; i++, p = _strtok_r(NULL,",",&tok_end))
      vector[i] = p;
  if (p)
     DEBUGF (1, "Too many commas in 'value'.\n");
  return (vector);
}

/**
 * Build up each `node->deps` from file-cache and add to correct place in `ports_list`.
 *
 * \eg. a "port_deps_10 = ilmbase,hdf5" should add "ilmbase" and "hdf5" to `node->deps`
 * for the 10th entry in `ports_list`.
 */
static void build_port_deps_from_cache (void)
{
  port_node *node;
  int  rc, j, i = 0;

  while (1)
  {
    char format[1000], *value, **deps;

    snprintf (format, sizeof(format), "port_deps_%d = %%s", i);
    rc = cache_getf (SECTION_VCPKG, format, &value);
    DEBUGF (1, "port_deps_%d from cache: '%s'\n", i, value);

    if (rc < 1)
       break;

    node = smartlist_get (ports_list, i);

#if 0
    make_dependencies (node, value);
#else
    deps = split_value_to_vector (value);
    ASSERT (node->deps == NULL);
    node->deps = smartlist_new();
    for (j = 0; deps[j]; j++)
    {
      smartlist_add (node->deps, STRDUP(deps[j]));
      DEBUGF (1, "port_deps_%d: deps[%d]: '%s'\n", i, j, deps[j]);
    }
#endif
    i++;
  }
}

/**
 * Build the cache of features.
 * \eg.
 *   with a "port_node_1057 = realsense2,...",
 *   means 'realsense2' has this feature-string in cache:
 *   "port_features_1057 = tools,openni2,tm2"
 */
static void build_port_features_from_cache (void)
{
  port_node *node;
  int  rc, j, i = 0;

  while (1)
  {
    char format[1000], *value, **features;

    snprintf (format, sizeof(format), "port_features_%d = %%s", i);
    rc = cache_getf (SECTION_VCPKG, format, &value);
    DEBUGF (2, "port_features_%d from cache: '%s'\n", i, value);

    if (rc < 1)
       break;

    features = split_value_to_vector (value);
    node = smartlist_get (ports_list, i);
    ASSERT (node->features == NULL);
    node->features = smartlist_new();
    for (j = 0; features[j]; j++)
    {
      smartlist_add (node->features, STRDUP(features[j]));
      DEBUGF (1, "port_features_%d: features[%d]: '%s'\n", i, j, features[j]);
    }
    i++;
  }
}

static void put_dependants_to_cache (int port_num, const port_node *node)
{
  int   i, max = smartlist_len (node->deps);
  char  value [10000] = "-";
  char  format [100];
  char *p    = value;
  char *end  = value + sizeof(value) - 1;
  int   left = sizeof(value);

  snprintf (format, sizeof(format), "port_deps_%d = %%s", port_num);

  for (i = 0; i < max && left > 3; i++)
  {
    const struct vcpkg_package *dep = smartlist_get (node->deps, i);

    p += snprintf (p, left, "%s,", dep->package);
    left = end - p;
  }
  if (p > value && p[-1] == ',')
     p[-1] = '\0';
  cache_putf (SECTION_VCPKG, format, value);
}

static void put_features_to_cache (int port_num, const port_node *node)
{
  int   i, max = smartlist_len (node->features);
  char  value [10000] = "";
  char  format [1000];
  char *p    = value;
  char *end  = value + sizeof(value) - 1;
  int   left = sizeof(value);

  snprintf (format, sizeof(format), "port_features_%d = %%s", port_num);

  for (i = 0; i < max && left > 3; i++)
  {
    const char *feature = smartlist_get (node->features, i);

    p += snprintf (p, left, "%s,", feature);
    left = end - p;
  }
  if (p > value && p[-1] == ',')
     p[-1] = '\0';
  cache_putf (SECTION_VCPKG, format, value);
}

/**
 * Return a pointer to `last_err_str`.
 */
const char *vcpkg_last_error (void)
{
  return (last_err_str);
}

/**
 * Return a `vcpkg_package*` structure for an installed package matching `package`.
 * There can be 2 (or more) packages with the same name but for different bitness.
 *
 * \eg. `pkg->platform == VCPKG_plat_WINDOWS | VCPKG_plat_x86` and
 *      `pkg->platform == VCPKG_plat_UWP | VCPKG_plat_x64` etc.
 *
 * \param[in,out] index_p   A pointer to a value representing the index to search from.
 *                          This value is updated on return from this function.
 *
 * \param[in]     package   The package name to search for among the installed VCPKG packages.
 */
static const vcpkg_package *get_install_info (int *index_p, const char *package)
{
  const vcpkg_package *pkg;
  int   i, max = smartlist_len (installed_packages);

  for (i = *index_p; i < max; i++)
  {
    pkg = smartlist_get (installed_packages, i);
    if (!strcmp(package, pkg->package))
    {
      *index_p = i + 1;
      return (pkg);
    }
  }
  return (NULL);
}

/**
 * Print the description for a `port_node`.
 */
static void node_dump_description (const port_node *node)
{
  int len  = C_puts ("     ~6description:~0  ") - 2;
  int save = C_setraw (1);

  if (node->description)
  {
#ifdef USE_str_to_utf8
    char     buf [1000];
    wchar_t  wbuf [1000];
    wchar_t *utf8 = str_to_utf8 (node->description, wbuf, DIM(wbuf));

    snprintf (buf, sizeof(buf), "%S", utf8);
    C_puts_long_line (buf, len);
#else
    C_puts_long_line (node->description, len);
#endif
   }
  else
    C_puts ("<none>\n");
  C_setraw (save);
}

/**
 * Dump the dependencies for a `port_node`.
 */
static void node_dump_deps (const port_node *node, size_t width)
{
  int i, max = node->deps ? smartlist_len (node->deps) : 0;
  int len, len0 = C_puts ("     ~6dependencies:~0 ") - 2;

  len = len0;
  for (i = 0; i < max; i++)
  {
    const vcpkg_package *dep = smartlist_get (node->deps, i);
    const vcpkg_package *next_dep;
    BOOL   Not;
    size_t next_len = 0;

    len += C_puts (dep->package);
    get_depend_platform (dep->platform, &Not);
    if (Not)
         len += C_printf (" !(%s)", get_depend_name(dep));
    else len += C_printf (" (%s)", get_depend_name(dep));

    if (i < max-1)
    {
      len += C_puts (", ");
      next_dep = smartlist_get (node->deps, i+1);
      next_len = 5 + strlen (next_dep->package) + strlen (get_depend_name(next_dep));
      if (len + next_len >= width)
      {
        C_putc ('\n');
        len = C_puts (str_repeat(' ', len0));
      }
    }
  }
  if (i == 0)
     C_puts ("<none>");
  C_putc ('\n');
}

/**
 * Dump the features for a `port_node`.
 */
static void node_dump_features (const port_node *node, size_t width)
{
  int i, max = node->features ? smartlist_len (node->features) : 0;
  int len, len0 = C_puts ("     ~6features:~0     ") - 2;

  len = len0;
  for (i = 0; i < max; i++)
  {
    size_t next_len = 0;

    len += C_puts (smartlist_get(node->features, i));
    if (i < max-1)
    {
      len += C_puts (", ");
      next_len = 2 + strlen (smartlist_get(node->features, i+1));
      if (len + next_len >= width)
      {
        C_putc ('\n');
        len = C_puts (str_repeat(' ', len0));
      }
    }
  }
  if (i == 0)
     C_puts ("<none>");
  C_putc ('\n');
}

/**
 * Iterate over all package files and get number and their total file-size.
 */
static const char *get_package_files_size (const vcpkg_package *pkg)
{
  UINT64   f_size = 0;
  unsigned i, max = smartlist_len (pkg->install_info);

  for (i = 0; i < max; i++)
  {
    struct stat st;
    char        path [_MAX_PATH];
    const char *file = smartlist_get (pkg->install_info, i);

    snprintf (path, sizeof(path), "%s\\installed\\%s", vcpkg_root, file);
    if (safe_stat(path, &st, NULL) == 0)
       f_size += get_file_alloc_size (path, st.st_size);
  }
  incr_total_size (f_size);
  return str_ltrim ((char*)get_file_size_str(f_size));
}

/**
 * Print information for an installed package in `ports_list`.
 */
static void pkg_dump_file_info (const vcpkg_package *pkg, const char *indent)
{
  const char *dir = get_installed_dir (pkg);
  unsigned    num = smartlist_len (pkg->install_info);

  C_printf ("%s~6installed:    YES~0\n", indent);
  C_printf ("%s~6location:     %s~0, %u files", indent, dir, num);

  if (opt.show_size)
     C_printf (", %s", get_package_files_size(pkg));
  C_puts ("\n\n");
}

/**
 * Dump the parsed information from `ports_list`.
 */
static void dump_nodes (void)
{
  const char *indent = "     ";
  int    i, num, max = smartlist_len (ports_list);
  size_t width = C_screen_width();

  /* Print a simple header.
   */
  C_printf ("~6Num  ~3Package~0 / ~6Version\n");
  C_puts (str_repeat('=', 100));
  C_putc ('\n');

  for (i = num = 0; i < max; i++)
  {
    const port_node     *node = smartlist_get (ports_list, i);
    const vcpkg_package *pkg;
    const char *version = node->version;
    int         zero = 0;

    if (!node->have_CONTROL)
       continue;

    if (*version == '\0' || *version == ' ')
       version = "<unknown>";

    C_printf ("~7%4d ~3%s~0 / ~6%s~0\n", ++num, node->package, version);
    C_printf ("%s~6homepage:~0     %s\n", indent, node->homepage);

    node_dump_description (node);
    node_dump_deps (node, width);
    node_dump_features (node, width);

    pkg = get_install_info (&zero, node->package);
    if (pkg)
         pkg_dump_file_info (pkg, indent);
    else C_printf ("%s~6installed:    NO~0\n\n", indent);
  }
}

/**
 * Traverse the smartlist `ports_list` and
 * return the number of nodes where `node->have_CONTROL == have_CONTROL`.
 */
static unsigned vcpkg_get_num (BOOL have_CONTROL)
{
  int      i, max;
  unsigned num = 0;

  max = ports_list ? smartlist_len (ports_list) : 0;

  for (i = 0; i < max; i++)
  {
    const port_node *node = smartlist_get (ports_list, i);

    if (node->have_CONTROL == have_CONTROL)
       num++;
  }
  return (num);
}

/**
 * Build the smartlist `ports_list *` representing all available VCPKG packages
 * (ignoring whether a package is installed or not).
 *
 * \param[in] dirs  The `smartlist_t*` of the directories to build the
 *                  `port_node*` list from.
 * \retval          The number of all node types.
 */
static unsigned get_all_available (const smartlist_t *dirs, BOOL from_cache)
{
  unsigned num_ports_list;

  if (dirs)
  {
    int i, max = smartlist_len (dirs);

    DEBUGF (2, "Found %d %sVCPKG port directories.\n", max, from_cache ? "cached " : "");
    for (i = 0; i < max; i++)
    {
      if (!from_cache)
         build_ports_list (smartlist_get(dirs,i), i);
    }
    if (from_cache)
    {
      build_ports_list_from_cache();
      build_port_deps_from_cache();
      build_port_features_from_cache();
    }
  }

  num_ports_list = smartlist_len (ports_list);
  if (num_ports_list == 0)
     snprintf (last_err_str, sizeof(last_err_str), "No ~6VCPKG~0 packages found%s", from_cache ? " in cache" : "");
  return (num_ports_list);
}

/**
 * Try to set the `vcpkg_root` based on a `%VCPKG_ROOT%` env-var.
 */
static BOOL get_base_env (void)
{
  const char *env = getenv ("VCPKG_ROOT");

  if (!env)
  {
    _strlcpy (last_err_str, "Env-var ~5VCPKG_ROOT~0 not defined", sizeof(last_err_str));
    return (FALSE);
  }
  if (!is_directory(env))
  {
    _strlcpy (last_err_str, "~5VCPKG_ROOT~0 points to a non-existing directory", sizeof(last_err_str));
    return (FALSE);
  }
  if (!vcpkg_root)
     vcpkg_root = _fix_path (env, NULL);
  return (TRUE);
}

/**
 * Try to set the `vcpkg_root` based on directory of `vcpkg.exe`.
 */
static BOOL get_base_exe (const char *exe)
{
  char *dir;

  if (!exe)
  {
    _strlcpy (last_err_str, "vcpkg.exe not found on PATH", sizeof(last_err_str));
    return (FALSE);
  }
  dir = dirname (exe);

  /* Returns a fully qualified directory name in case `cwd == dir`.
   */
  if (!vcpkg_root)
     vcpkg_root = _fix_path (dir, NULL);

  FREE (dir);
  return (TRUE);
}

/**
 * Traverse `install_packages` and add the `platform` for the `package`.
 */
static unsigned set_installed_package_platform (const char *package, VCPKG_platform platform)
{
  unsigned i, max = smartlist_len (installed_packages);

  for (i = 0; i < max; i++)
  {
    vcpkg_package *pkg = smartlist_get (installed_packages, i);

    if (!stricmp(pkg->package, package))
    {
      pkg->platform = platform;
      return (unsigned) platform;
    }
  }
  return (UINT_MAX);
}

/**
 * Parse a line from `vcpkg_parse_status_file()` and add elements to
 * the package at `*pkg`.
 *
 * Look for stuff like:
 * ```
 *   Package:      pybind11
 *   Version:      2.2.4
 *   Depends:      python3
 *   Architecture: x86-windows
 *   Abi:          d99c304a1e7c194349764a9a753c5cbcf5f14b5a
 *   Status:       purge ok not-installed
 * ```
 *
 * First the `line` gets 0-terminated and next call is ready to
 * parse the next line. This will start at `eol+1` (end-of-line plus 1).
 * Caller must ensure this function does not parse beyond the allocated
 * file-buffer.
 */
static int vcpkg_parse_status_line (vcpkg_package *pkg, char **line_p)
{
  char *p, *eol, *line;

  line = *line_p;
  eol = strchr (line, '\n');
  if (eol)
     *eol = '\0';
  else
  {
    eol = strchr (line, '\r');
    if (eol)
        *eol = '\0';
    else eol = strchr (line, '\0'); /* could be the last line in file w/o EOF */
  }
  *line_p = eol + 1;

  DEBUGF (2, "line: '%.50s'.\n", line);

  if (str_match(line, CONTROL_PACKAGE, &p))
  {
    pkg->package = p;
    return (1);
  }

  if (str_match(line, CONTROL_VERSION, &p))
  {
    pkg->version = p;
    str_replace2 ('~', "~~", pkg->version, sizeof(pkg->version));
    return (1);
  }

  if (str_match(line, CONTROL_DEPENDS, &p))
  {
    pkg->depends = p;
    return (1);
  }

  if (str_match(line, CONTROL_STATUS, &p))
  {
    pkg->status = p;
    return (1);
  }

  if (str_match(line, CONTROL_ARCH, &p))
  {
    pkg->arch = p;
    return (1);
  }

  if (str_match(line, CONTROL_ABI, &p))
  {
    pkg->ABI_tag = p;
    return (1);
  }
  return (0);
}

/**
 * Open and parse the `<vcpkg_root>/installed/vcpkg/status` file.
 * Build up a `installed_packages` smartlist as we go along.
 *
 * Also check against orpaned packages ("Status: purge ok") that is present
 * in his `installed_packages` list, but not in the `built_packages` list.
 *
 * The memory for this file is not freed until `vcpkg_exit()` is called since
 * this memory are used in e.g. `installed_packages::package` names.
 *
 * \todo Maybe use a `mmap()` style feature instead?
 */
static int vcpkg_parse_status_file (void)
{
  struct stat   st;
  vcpkg_package pkg, *copy;
  FILE         *f;
  char          file [_MAX_PATH];
  char         *f_end, *f_ptr;
  size_t        f_size, f_read;
  int           num_parsed = 0;  /* number of parsed lines in current record */
  int           num_total = 0;   /* number of total packages parsed */
  DWORD         win_err;

  snprintf (file, sizeof(file), "%s\\installed\\vcpkg\\status", vcpkg_root);

  memset (&st, '\0', sizeof(st));
  if (safe_stat(file, &st, &win_err) || st.st_size == 0)
  {
    WARN ("Failed to get the file-size of %s. win_err: %lu\n", file, win_err);
    return (0);
  }

  f = fopen (file, "rb");
  if (!f)
  {
    WARN ("Failed to open %s.\n", file);
    return (0);
  }

  if (st.st_size >= ULONG_MAX)
  {
    WARN ("File %s is too big %" S64_FMT ".\n", file, st.st_size);
    fclose (f);
    return (0);
  }

  f_size = (size_t) st.st_size;
  f_status_mem = MALLOC (f_size + 1);
  f_read = fread (f_status_mem, 1, f_size, f);
  if (f_read != f_size)
  {
    WARN ("Failed to read the whole file %s. Only %u bytes, errno: %d (%s)\n",
          file, (unsigned)f_read, errno, strerror(errno));
    fclose (f);
    return (0);
  }

  fclose (f);
  f_end = f_status_mem + f_size;
  *f_end = '\0';

  DEBUGF (2, "Building 'installed_packages' from %s (%u bytes).\n", file, (unsigned)f_size);

  memset (&pkg, '\0', sizeof(pkg));

  for (f_ptr = f_status_mem; f_ptr <= f_end; )
  {
    num_parsed += vcpkg_parse_status_line (&pkg, &f_ptr);
    if (*f_ptr == '\r' ||*f_ptr == '\n')
    {
      f_ptr++;
      num_total++;
      DEBUGF (2, "reached EOR for package '%s'. num_parsed: %d, num_total: %d\n",
              pkg.package, num_parsed, num_total);
    }
    else
      continue;

    if (str_endswith(pkg.arch,"-static"))
    {
      pkg.Static = TRUE;
      DEBUGF (2, "package '%s' is 'static': '%s'.\n", pkg.package, pkg.arch);
    }
    if (get_installed_info(&pkg))
    {
      copy = CALLOC (sizeof(*copy), 1);
      *copy = pkg;
      smartlist_add (installed_packages, copy);
    }

    /* Ready for the next record of another package
     */
    memset (&pkg, '\0', sizeof(pkg));
    num_parsed = 0;
  }
  return smartlist_len (installed_packages);
}

static int vcpkg_version_cb (char *buf, int index)
{
  struct ver_info ver = { 0,0,0,0 };

  ARGSUSED (index);
  if (sscanf(buf, "Vcpkg package management program version %d.%d.%d",
             &ver.val_1, &ver.val_2, &ver.val_3) >= 2)
  {
    memcpy (&vcpkg_ver, &ver, sizeof(vcpkg_ver));
    return (1);
  }
  return (0);
}

/*
 * Write all collected information back to the file-cache.
 */
static void put_all_to_cache (void)
{
  int i, max;

  if (!opt.use_cache || !vcpkg_root)
     return;

  cache_putf (SECTION_VCPKG, "vcpkg_root = %s", vcpkg_root);

  max = available_packages ? smartlist_len (available_packages) : 0;
  for (i = 0; i < max; i++)
  {
    const vcpkg_package *pkg = smartlist_get (available_packages, i);
    int   zero = 0, installed;

    if (get_install_info(&zero, pkg->package))
         installed = 1;
    else installed = 0;

    cache_putf (SECTION_VCPKG, "available_package_%d = %s,%s,%s,%s,%s,%d", i,
                pkg->package,
                pkg->version ? pkg->version : "-",
                pkg->status  ? pkg->status  : "-",
                pkg->depends ? pkg->depends : "-",
                pkg->arch    ? pkg->arch    : "-",
                installed);
  }

  max = installed_packages ? smartlist_len (installed_packages) : 0;
  for (i = 0; i < max; i++)
  {
    const vcpkg_package *pkg = smartlist_get (installed_packages, i);

    cache_putf (SECTION_VCPKG, "installed_package_%d = %s,%s,%s,%s,%s", i,
                pkg->package,
                pkg->version ? pkg->version : "-",
                pkg->status  ? pkg->status  : "-",
                pkg->depends ? pkg->depends : "-",
                pkg->arch    ? pkg->arch    : "-");
  }

  max = ports_list ? smartlist_len (ports_list) : 0;
  for (i = 0; i < max; i++)
  {
    const port_node *node = smartlist_get (ports_list, i);

    if (node->have_CONTROL)
         cache_putf (SECTION_VCPKG, "port_node_%d = %s,%s,%s,%d,\"%s\"",
                     i, node->package, node->version,
                     node->homepage, node->have_CONTROL, node->description);
    else cache_putf (SECTION_VCPKG, "ports_cmake_%d = %s",
                     i, node->package);

    if (node->deps)
       put_dependants_to_cache (i, node);

    if (node->features)
       put_features_to_cache (i, node);
  }
}

/**
 * Find the location and version for `vcpkg.exe` (on `PATH`).
 */
BOOL vcpkg_get_info (char **exe, struct ver_info *ver)
{
  static char exe_copy [_MAX_PATH];

  *exe = NULL;
  *ver = vcpkg_ver;

  /* We have already done this
   */
  if (vcpkg_exe && (vcpkg_ver.val_1 + vcpkg_ver.val_2) > 0)
  {
    *exe = STRDUP (vcpkg_exe);
    return (TRUE);
  }

  DEBUGF (2, "ver: %d.%d.%d.\n", ver->val_1, ver->val_2, ver->val_3);

  cache_getf (SECTION_VCPKG, "vcpkg_exe = %s", &vcpkg_exe);
  cache_getf (SECTION_VCPKG, "vcpkg_version = %d,%d,%d", &vcpkg_ver.val_1, &vcpkg_ver.val_2, &vcpkg_ver.val_3);
  if (vcpkg_exe && !FILE_EXISTS(vcpkg_exe))
  {
    cache_del (SECTION_VCPKG, "vcpkg_exe");
    cache_del (SECTION_VCPKG, "vcpkg_version");
    memset (&vcpkg_ver, '\0', sizeof(vcpkg_ver));
    vcpkg_exe = NULL;
    return vcpkg_get_info (exe, ver);
  }

  if (!vcpkg_exe)
     vcpkg_exe = searchpath ("vcpkg.exe", "PATH");

  if (!vcpkg_exe)
     return (FALSE);

  vcpkg_exe = slashify2 (exe_copy, vcpkg_exe, '\\');
  *exe = STRDUP (vcpkg_exe);

  cache_putf (SECTION_VCPKG, "vcpkg_exe = %s", vcpkg_exe);

  if (vcpkg_ver.val_1 + vcpkg_ver.val_2 == 0 &&
      popen_runf(vcpkg_version_cb, "\"%s\" version", vcpkg_exe) > 0)
     cache_putf (SECTION_VCPKG, "vcpkg_version = %d,%d,%d", vcpkg_ver.val_1, vcpkg_ver.val_2, vcpkg_ver.val_3);

  *ver = vcpkg_ver;
  DEBUGF (2, "ver: %d.%d.%d.\n", ver->val_1, ver->val_2, ver->val_3);

  return (vcpkg_exe && vcpkg_ver.val_1 + vcpkg_ver.val_2 > 0);
}

/**
 * Initialise VCPKG globals once and build the list of built packages (`built_packages`).
 * This might NOT be the same as installed packages; usually more built than installed
 * packages if something broke under-way.
 */
void vcpkg_init (void)
{
  smartlist_t *ports_dirs    = NULL;
  smartlist_t *packages_dirs = NULL;
  int          num_cached_ports_dirs = 0;
  int          i, j, max;
  char         format [100], *dir;
  BOOL         vcpkg_ok;
  static       BOOL done = FALSE;

  if (done)
     return;

  done = TRUE;

  vcpkg_get_info (&vcpkg_exe, &vcpkg_ver);

  if (cache_getf(SECTION_VCPKG, "vcpkg_root = %s", &vcpkg_root) == 1)
     vcpkg_root = STRDUP (vcpkg_root);

  /**
   * If not in cache, try to set the `vcpkg_root` location.
   * Based either on:
   *  \li - an existing directory `%VCPKG_ROOT%` or
   *  \li - The directory name of `vcpkg_exe`.
   */
  vcpkg_ok = get_base_env() && get_base_exe (vcpkg_exe);
  if (!vcpkg_ok)
     return;

  if (get_installed_dir(NULL))
     have_installed_dir = TRUE;

  ASSERT (built_packages     == NULL);
  ASSERT (installed_packages == NULL);
  ASSERT (available_packages == NULL);
  ASSERT (ports_list         == NULL);

  built_packages     = smartlist_new();
  installed_packages = smartlist_new();
  available_packages = smartlist_new();
  ports_list         = smartlist_new();

  last_err_str[0] = '\0';   /* clear any error-string set */

#if 0
  num_cached_ports_dirs    = get_ports_dirs_from_cache();
  num_cached_packages_dirs = get_packages_dirs_from_cache();

#else
  i = 0;
  while (1)
  {
    snprintf (format, sizeof(format), "port_dir_%d = %%s", i);
    if (cache_getf(SECTION_VCPKG, format, &dir) != 1)
       break;

    if (i == 0)
       ports_dirs = smartlist_new();
    smartlist_add (ports_dirs, STRDUP(dir));
    i++;
  }
  num_cached_ports_dirs = i;

  i = 0;
  while (1)
  {
    snprintf (format, sizeof(format), "packages_dir_%d = %%s", i);
    if (cache_getf(SECTION_VCPKG, format, &dir) != 1)
       break;

    if (i == 0)
       packages_dirs = smartlist_new();
    smartlist_add (packages_dirs, STRDUP(dir));
    i++;
  }
#endif

  /* If not from cache, build a dirlist using readdir()
   */
  if (!ports_dirs || smartlist_len(ports_dirs) == 0)
     ports_dirs = build_dir_list ("ports");

  if (!packages_dirs || smartlist_len(packages_dirs) == 0)
     packages_dirs = build_dir_list ("packages");

  get_all_available (ports_dirs, num_cached_ports_dirs > 0);

  if (ports_dirs)
     smartlist_free_all (ports_dirs);

  max = have_installed_dir ? vcpkg_parse_status_file() : 0;
  for (i = j = 0; i < max; i++)
  {
    vcpkg_package *pkg = smartlist_get (installed_packages, i);

    get_control_node (&j, &pkg->link, pkg->package);
  }

  if (packages_dirs)
       max = smartlist_len (packages_dirs);
  else max = 0;

  /**
   * Loop over all our packages directory
   * `<vcpkg_root>/packages/`
   *
   * and figure out which belongs to the
   * `<vcpkg_root>/installed/`  i.e. the `installed_packeges` list.
   *
   * directory. Some packages may have been orpaned.
   */
  for (i = 0; i < max; i++)
  {
    vcpkg_package *node;
    char *homepage, *p, *q;

    /**
     * If e.g. `dirs` contains "packages\\sqlite3_x86-windows", add a node
     * with this to the `built_packages` smartlist:
     *   `node->package  = "sqlite3"`
     *   `node->platform = VCPKG_plat_x86 | VCPKG_plat_WINDOWS`.
     *   `node->link`    = a pointer into `ports_list` for more detailed info.
     */
    p = (char*) smartlist_get (packages_dirs, i) + strlen ("packages\\");
    q = strchr (p+1, '_');
    if (q && q - p < sizeof(node->package) && legal_package_name(p))
    {
      unsigned installed;

      j = 0;
      node = CALLOC (sizeof(*node), 1);
      *q = '\0';
      node->package = p;
      node->platform = make_package_platform (q+1, TRUE);
      get_control_node (&j, &node->link, node->package);
      smartlist_add (built_packages, node);

      if (node->link)
           homepage = node->link->homepage;
      else homepage = "?";

      installed = set_installed_package_platform (node->package, node->platform);
      ASSERT (installed != UINT_MAX);

      DEBUGF (1, "package: %-20s  %-50s  platform: 0x%04X (%s).\n",
              node->package, homepage, node->platform,
              flags_decode(node->platform, platforms, DIM(platforms)));
    }
  }
  smartlist_free_all (packages_dirs);

  if (opt.verbose >= 3)
     dump_nodes();
}

/**
 * Return the number of `CONTROL` nodes.
 */
unsigned vcpkg_get_num_CONTROLS (void)
{
  unsigned num_CONTROLS;

  vcpkg_init();

  num_CONTROLS = vcpkg_get_num (TRUE);
  if (num_CONTROLS == 0)
     _strlcpy (last_err_str, "No CONTROL files for VCPKG found", sizeof(last_err_str));
  return (num_CONTROLS);
}

/**
 * Return the number of `portfile.cmake` nodes.
 * \note Currently not used.
 */
unsigned vcpkg_get_num_portfile (void)
{
  unsigned num_portfiles;

  vcpkg_init();

  num_portfiles = vcpkg_get_num (FALSE);
  if (num_portfiles == 0)
     _strlcpy (last_err_str, "No portfiles for VCPKG found", sizeof(last_err_str));
  return (num_portfiles);
}

/**
 * Return the number of built packages.
 */
unsigned vcpkg_get_num_built (void)
{
  unsigned num_built;

  vcpkg_init();

  num_built = built_packages ? smartlist_len (built_packages) : 0;
  DEBUGF (2, "Found %u `packages` directories.\n", num_built);
  return (num_built);
}

/**
 * Return the number of installedt packages.
 */
unsigned vcpkg_get_num_installed (void)
{
  unsigned num_installed;

  vcpkg_init();

  num_installed = installed_packages ? smartlist_len (installed_packages) : 0;
  DEBUGF (2, "Found %u `installed` directories.\n", num_installed);
  return (num_installed);
}

/**
 * Construct an relative sub-directory name based on platform.
 * \eg. platform: `VCPKG_plat_x86 | VCPKG_plat_WINDOWS` returns `"x86-windows"`.
 *
 * We do not care about LINUX, OSX, ANDROID etc.
 */
static const char *get_platform_name (VCPKG_platform platform)
{
  static char ret [_MAX_PATH];

  if (platform & VCPKG_plat_x64)
       strcpy (ret, "x64-");
  else strcpy (ret, "x86-");

  platform &= ~(VCPKG_plat_x86 | VCPKG_plat_x64);
  _strlcpy (ret+4, flags_decode(platform, platforms, DIM(platforms)), sizeof(ret)-4);
  return (ret);
}

/**
 * Get the `deps->platform` name(s) disregarding
 * the `VCPKG_platform_INVERSE` bit.
 */
static const char *get_depend_name (const vcpkg_package *dep)
{
  unsigned val = dep->platform;

  if (val == VCPKG_plat_ALL)
     return ("all");
  val &= ~VCPKG_platform_INVERSE;
  return flags_decode (val, platforms, DIM(platforms));
}

/**
 * Construct an absolute directory-name for an installed package.
 *
 * \eg
 *  \li pkg->package:     "sqlite3"
 *  \li pkg->platform:    VCPKG_plat_x86 | VCPKG_plat_WINDOWS
 *  \li returns:          "<vcpkg_root>\\installed\\x86-windows"
 *                        (or "<vcpkg_root>/installed/x86-windows")
 */
static const char *get_installed_dir (const vcpkg_package *pkg)
{
  static char dir [_MAX_PATH];

  if (pkg)
  {
    snprintf (dir, sizeof(dir), "%s\\installed\\%s", vcpkg_root, pkg->arch);
    DEBUGF (2, "platform_name: '%s', dir: '%s'\n", get_platform_name(pkg->platform), dir);
  }
  else
    snprintf (dir, sizeof(dir), "%s\\installed", vcpkg_root);

  if (!is_directory(dir))
  {
    snprintf (last_err_str, sizeof(last_err_str), "No status file '%s'", dir);
    return (NULL);
  }
  if (opt.show_unix_paths)
     slashify2 (dir, dir, '/');
  return (dir);
}

#if defined(NOT_NEEDED)
/**
 * Construct an relative directory-name for an built package.
 *
 * \eg
 *  \li pkg->package:        "sqlite3"
 *  \li pkg->link->version:  "3.24.0-1"
 *  \li pkg->platform:       VCPKG_plat_x86 | VCPKG_plat_WINDOWS
 *  \li returns:             "packages\\sqlite3_x86-windows"
 */
static const char *get_packages_dir (const vcpkg_package *pkg)
{
  static char dir [_MAX_PATH];

  if (!pkg->link)
     return (NULL);     /* something is seriously wrong */

  snprintf (dir, sizeof(dir), "%s\\packages\\%s_%s",
            vcpkg_root, pkg->package, get_platform_name(pkg->platform));

  DEBUGF (2, "architecture: '%s', dir: '%s'\n", pkg->arch, dir);

  if (!is_directory(dir))
  {
    snprintf (last_err_str, sizeof(last_err_str), "No such directory %s", dir);
    return (NULL);
  }
  return (dir + strlen(vcpkg_root) + 1);
}
#endif

/**
 * For a package `pkg` print the information obtained from `get_installed_info()`.
 *
 * Print files in these directories:
 *  \li the `bin` directory
 *  \li the `lib` directory.
 *  \li the `include` directory
 *  \li the `share` directory
 *
 * If òpt.show_size == TRUE`, print the total file-size of a package (ignoring
 * files that are not under the above directories).
 */
static void print_package_info (const vcpkg_package *pkg, FMT_buf *fmt_buf, int indent)
{
  char slash, path [_MAX_PATH];
  int  i, max = smartlist_len (pkg->install_info);

  for (i = 0; i < max; i++)
  {
    buf_printf (fmt_buf, "%*s%s\n", i > 0 ? indent : 0, "",
                (const char*)smartlist_get (pkg->install_info, i));
  }

  if (opt.show_size)
     buf_printf (fmt_buf, "%*s~3%s~0", indent, "", get_package_files_size(pkg));

  if (max == 0)
  {
    slash = (opt.show_unix_paths ? '/' : '\\');
    snprintf (path, sizeof(path), "%s\\installed\\%s\\", vcpkg_root, pkg->arch);
    buf_printf (fmt_buf, "%*sNo entries for package `%s` under\n%*s%s.",
                indent, "", pkg->package, indent, "", slashify2(path, path, slash));
  }
  buf_putc (fmt_buf, '\n');
}

/**
 * Parser for a single `*.list` file for a specific package.
 * Add wanted `char *` elements to this smartlist as we parse the file.
 */
static const char *wanted_arch;

static void info_parse (smartlist_t *sl, char *buf)
{
  char *q, *p = buf;

  str_strip_nl (buf);
  q = strchr (p, '\0') - 1;

  if (!str_startswith(buf,wanted_arch) || *q == '/')
     return;

  q = p + strlen (wanted_arch);
  if (str_startswith(q,"/bin") || str_startswith(q,"/lib") ||
      str_startswith(q,"/include") || str_startswith(q,"/share"))
  {
    if (!opt.show_unix_paths)
       p = slashify2 (p, p, '\\');
    smartlist_add (sl, STRDUP(p));
    DEBUGF (2, "adding: '%s'.\n", p);
  }
}

/**
 * Open and parse a `*.list` file to get the `bin`, `lib` and `include` files
 * for an installed `pkg->package`.
 *
 * \eg.
 *   For `zlib:x86-windows` with version `1.2.11-6`, open and parse the file
 *   `<vcpkg_root>/installed/vcpkg/info/zlib_1.2.11-6_x86-windows.list`.
 *
 * Return TRUE if `*.list` file exists and the length of the `pkg->install_info`
 * list is greater than zero.
 */
static BOOL get_installed_info (vcpkg_package *pkg)
{
  char file [_MAX_PATH];

  snprintf (file, sizeof(file), "%s\\installed\\vcpkg\\info\\%s_%s_%s.list",
            vcpkg_root, pkg->package, pkg->version, pkg->arch);

  if (!FILE_EXISTS(file))
  {
    DEBUGF (2, "Package '%s' is not installed; no '%s'.\n", pkg->package, file);
    return (FALSE);
  }
  DEBUGF (2, "Getting package information from '%s'.\n", file);
  wanted_arch = pkg->arch;
  pkg->install_info = smartlist_read_file (file, (smartlist_parse_func)info_parse);
  return (pkg->install_info != NULL);
}

/**
 * Print a list of installed packages.
 *
 * \note Only called from `show_version()` in envtool.c.
 */
unsigned vcpkg_list_installed (void)
{
  const char *only = "";
  const char *last = "";
  unsigned    i, indent, num_installed, num_ignored;
  FMT_buf     fmt_buf;

  vcpkg_init();

  num_installed = installed_packages ? smartlist_len (installed_packages) : 0;

  BUF_INIT (&fmt_buf, BUF_INIT_SIZE, 1);

  if (opt.only_32bit)
     only = ". These are for x86";
  else if (opt.only_64bit)
     only = ". These are for x64";

  for (i = num_ignored = 0; i < num_installed; i++)
  {
    const vcpkg_package *pkg = smartlist_get (installed_packages, i);

    if (opt.only_32bit && !(pkg->platform & VCPKG_plat_x86))
    {
      num_ignored++;
      continue;
    }
    if (opt.only_64bit && !(pkg->platform & VCPKG_plat_x64))
    {
      num_ignored++;
      continue;
    }
    if (!strcmp(last, pkg->package))
    {
      num_ignored++;
      continue;
    }
    if (!pkg->install_info || !get_installed_dir(pkg))
    {
      num_ignored++;
      continue;
    }
    indent = buf_printf (&fmt_buf, "    %-25s", pkg->package);

    print_package_info (pkg, &fmt_buf, indent);
    last = pkg->package;
  }

  if (num_installed - num_ignored == 0)
     only = "";

  if (have_installed_dir)
       C_printf ("\n  Found %u installed ~3VCPKG~0 packages under ~3%s~0%s:\n",
                 num_installed - num_ignored, get_installed_dir(NULL), only);
  else C_printf ("\n  Found 0 installed ~3VCPKG~0 packages.\n");

  C_puts (fmt_buf.buffer_start);

  BUF_EXIT (&fmt_buf);
  return (num_installed - num_ignored);
}

/**
 * Free the memory allocated for `ports_list`.
 */
static void free_nodes (void)
{
  int i, max = ports_list ? smartlist_len (ports_list) : 0;

  for (i = 0; i < max; i++)
  {
    port_node *node = smartlist_get (ports_list, i);

    smartlist_free (node->deps);
    smartlist_free_all (node->features);
    FREE (node->description);
    FREE (node);
  }
  smartlist_free (ports_list);
  ports_list = NULL;
}

/**
 * Free the memory allocated for `vcpkg_installed`.
 */
static void free_installed (void)
{
  int i, max;

  if (!installed_packages)
     return;

  max = smartlist_len (installed_packages);
  for (i = 0; i < max; i++)
  {
    vcpkg_package *pkg = smartlist_get (installed_packages, i);

    smartlist_free_all (pkg->install_info);
    FREE (pkg);
  }
  smartlist_free (installed_packages);
  installed_packages = NULL;
}

/**
 * Free the memory allocated for smartlists and regex buffer.
 */
void vcpkg_exit (void)
{
  vcpkg_package *pkg;
  int   i, max = available_packages ? smartlist_len (available_packages) : 0;

  put_all_to_cache();

  for (i = 0; i < max; i++)
  {
    pkg = smartlist_get (available_packages, i);
    FREE (pkg->package);
  }

  FREE (f_status_mem);
  smartlist_free_all (built_packages);
  smartlist_free_all (available_packages);

  free_installed();
  free_nodes();
  regex_free();

  installed_packages = built_packages = available_packages = NULL;
  FREE (vcpkg_exe);
  FREE (vcpkg_root);
}

/**
 * Get the index at or above `index` that matches `package_spec`.
 * Modify `*index_p` on output to the next index to check.
 */
static BOOL get_control_node (int *index_p, port_node **node_p, const char *package_spec)
{
  int i, index, max = ports_list ? smartlist_len (ports_list) : 0;

  *node_p = NULL;
  index   = *index_p;
  for (i = index; i < max; i++)
  {
    port_node *node = smartlist_get (ports_list, i);

    if (node->have_CONTROL &&
        fnmatch(package_spec, node->package, FNM_FLAG_NOCASE) == FNM_MATCH)
    {
      DEBUGF (2, "i=%d, index=%d, package: %s\n", i, index, node->package);
      *node_p  = node;
      *index_p = i + 1;
      return (TRUE);
    }
  }
  return (FALSE);
}

/**
 * Print a "installed: YES" or "installed: NO" depending on whether package is
 * found in `installed_packages`.
 *
 * Also print all supported platforms (x86, x64 etc.) and locations for bin, lib and headers.
 *
 * \todo
 *   Print that similar to how `pkg-config` does it:
 *   \code
 *     pkg-config --libs --cflags --msvc-syntax gr-wxgui:
 *       -If:/gv/dx-radio/gnuradio/include
 *       -libpath:f:/gv/dx-radio/gnuradio/lib gnuradio-runtime.lib gnuradio-pmt.lib
 *   \endcode
 */
static BOOL print_install_info (FMT_buf *fmt_buf, const char *package, int indent1)
{
  const vcpkg_package *pkg = NULL;
  const char          *dir, *yes_no, *cpu = NULL;
  unsigned             num_installed = vcpkg_get_num_installed();
  unsigned             num_ignored = 0;
  int                  found, i = 0;

  if (num_installed == 0 || (pkg = get_install_info(&i,package)) == NULL)
       yes_no = C_BR_RED   "NO\n";
  else yes_no = C_BR_GREEN "YES: ";

  buf_printf (fmt_buf, "  %-*s%s~0", indent1, "installed:", yes_no);

  if (vcpkg_get_only_installed() && !pkg)
  {
    buf_putc (fmt_buf, '\n');
    return (FALSE);
  }

  if (opt.only_32bit)
     cpu = "x86";
  else if (opt.only_64bit)
     cpu = "x64";

  for (found = 0; pkg; pkg = get_install_info(&i,package))
  {
    if (opt.only_32bit && !(pkg->platform & VCPKG_plat_x86))
    {
      num_ignored++;
      continue;
    }
    if (opt.only_64bit && !(pkg->platform & VCPKG_plat_x64))
    {
      num_ignored++;
      continue;
    }

    if (found > 0)
       buf_printf (fmt_buf, "  %*s%s~0", indent1, "", yes_no);

    buf_printf (fmt_buf, "%s, %u files", pkg->arch, smartlist_len(pkg->install_info));
    if (opt.show_size)
         buf_printf (fmt_buf, ", %s\n", get_package_files_size(pkg));
    else buf_putc (fmt_buf, '\n');

    dir = get_installed_dir (pkg);
    if (dir)
    {
      buf_printf (fmt_buf, "  %*s%s~0\n", indent1, "", dir);
      found++;
    }
  }

  if (found == 0 && cpu)
  {
    buf_printf (fmt_buf, "But not for `%s` platform.\n", cpu);
    return (FALSE);
  }
  buf_putc (fmt_buf, '\n');
  return (TRUE);
}

