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
#include "json.h"
#include "vcpkg.h"

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
 * `CONTROL` files keywords we look for:
 *
 * \def CONTROL_BUILD_DEPENDS
 *      "Build-Depends:" the list of build dependencies for this package (?).
 *
 * \def CONTROL_DESCRIPTION
 *      "Description:" - The descriptions of a package follows this.
 *
 * \def CONTROL_DEFAULT_FEATURES
 *      "Default-Features:" - What features/options are there for this node?
 *
 * \def CONTROL_HOMEPAGE
 *      "Homepage:" - The URL of it's home-page.
 *
 * \def CONTROL_SOURCE
 *      "Source:" - The source-name is the name of a package following this.
 *
 * \def CONTROL_SUPPORTS
 *      "Supports:" - The supported platform(s). Not used yet.

 * \def CONTROL_VERSION
 *      "Version:" - The version-info of a package follows this.
 */
#define CONTROL_BUILD_DEPENDS      "Build-Depends:"
#define CONTROL_DESCRIPTION        "Description:"
#define CONTROL_DEFAULT_FEATURES   "Default-Features:"
#define CONTROL_HOMEPAGE           "Homepage:"
#define CONTROL_SOURCE             "Source:"
#define CONTROL_SUPPORTS           "Supports:"
#define CONTROL_VERSION            "Version:"

/**
 * `<vcpkg_root>/installed/status` file keywords we look for:
 *
 * \def STATUS_ABI
 *      "Abi:" Some kind of hash value (SHA1, SHA256 or SHA512?) for the installed package.
 *
 * \def STATUS_ARCH
 *      "Architecture:" the architecture (x86/x64/arm etc.) for this package.
 *
 * \def STATUS_DEFAULT_FEATURES
 *      "Default-Features:" - What features are there for this package?
 *
 * \def STATUS_DEPENDS
 *      "Depends:" the list of installed dependencies for this package.
 *
 * \def STATUS_PACKAGE
 *      "Package:" - The name of the package follows this.
 *
 * \def STATUS_STATUS
 *      "Status:" the install status for this package.
 *
 * \def STATUS_VERSION
 *      "Version:" - The version-info of a package follows this.
 */
#define STATUS_ABI               "Abi:"
#define STATUS_ARCH              "Architecture:"
#define STATUS_DEFAULT_FEATURES  "Default-Features:"
#define STATUS_DEPENDS           "Depends:"
#define STATUS_PACKAGE           "Package:"
#define STATUS_STATUS            "Status:"
#define STATUS_VERSION           "Version:"

/**
 * \def VCPKG_MAX_NAME
 * \def VCPKG_MAX_VERSION
 * \def VCPKG_MAX_URL
 * \def VCPKG_MAX_PLAT
 */
#define VCPKG_MAX_NAME      30   /**< Max size of a `port_node::package` or `vcpkg_package::package` entry. */
#define VCPKG_MAX_VERSION   30   /**< Max size of a `port_node::version` or `vcpkg_package::version` entry. */
#define VCPKG_MAX_URL      200   /**< Max size of a `port_node::homepage` entry. */
#define VCPKG_MAX_PLAT      10   /**< Max number of `port_node::platforms[]` */
#define VCPKG_MAX_STATUS    30
#define VCPKG_MAX_ARCH      30

/**
 * \enum VCPKG_platform
 * The platform enumeration.
 *
 * If a package is *not* e.g. `x86`, the stored value in `VCPKG_plat_list`
 * is `VCPKG_plat_x86 + 1`.
 */
typedef enum VCPKG_platform {
        VCPKG_plat_ALL     = 0,        /**< Package is for all supported OSes. */
        VCPKG_plat_WINDOWS = 0x0002,   /**< Package is for Windows only (desktop, not UWP). */
        VCPKG_plat_UWP     = 0x0004,   /**< Package is for Universal Windows Platform only. */
        VCPKG_plat_LINUX   = 0x0008,   /**< Package is for Linux only. */
        VCPKG_plat_x86     = 0x0010,   /**< Package is for x86 processors only. */
        VCPKG_plat_x64     = 0x0020,   /**< Package is for x64 processors only. */
        VCPKG_plat_ARM     = 0x0040,   /**< Package is for ARM processors only. */
        VCPKG_plat_ANDROID = 0x0080,   /**< Package is for Android only. */
        VCPKG_plat_OSX     = 0x0100,   /**< Package is for Apple's OSX only. */
        VCPKG_plat_STATIC  = 0x0200    /**< Package is for a static build also. */
      } VCPKG_platform;

/**
 * \typedef VCPKG_plat_list
 *
 * A list of `VCPKG_platform` values supported for a package.
 */
typedef VCPKG_platform VCPKG_plat_list [VCPKG_MAX_PLAT];

/**
 * \typedef port_node
 *
 * The structure of a single VCPKG package entry in the `ports_list`.
 */
typedef struct port_node {
        char            package [VCPKG_MAX_NAME];     /**< The package name. */
        char            version [VCPKG_MAX_VERSION];  /**< The version. */
        char            homepage [VCPKG_MAX_URL];     /**< The URL of it's home-page. */
        char           *description;                  /**< The description. */
        BOOL            have_CONTROL;                 /**< TRUE if this is a CONTROL-node. */
        BOOL            have_JSON;                    /**< TRUE if this is a JSON-node. */
        VCPKG_plat_list platforms;                    /**< The supported platform(s) and "static" status */
        smartlist_t    *features;                     /**< The features; a smartlist of `char *`. */
        smartlist_t    *depends;                      /**< The dependencies; a smartlist of `char *`. */
        smartlist_t    *supports;                     /**< The supported platform(s) and "static" status; a smartlist of `enum VCPKG_platform`.  */
      } port_node;

/**
 * \typedef vcpkg_package
 *
 * The structure of a single installed VCPKG package or the
 * structure of a package-dependency.
 */
typedef struct vcpkg_package {
        char             package [VCPKG_MAX_NAME];    /**< The package name. */
        char             version [VCPKG_MAX_VERSION]; /**< The version. */
        char             status  [VCPKG_MAX_STATUS];  /**< The install/purge status. */
        char             arch    [VCPKG_MAX_ARCH];    /**< The OS/CPU and ("-static"). */
        VCPKG_plat_list  platforms;                   /**< The supported platform(s) and "static" status */
        BOOL             installed;                   /**< At least 1 combination is installed */
        BOOL             purged;                      /**< Not installed; ready to be removed/updated */
        const port_node *link;                        /**< A link to the corresponding CONTROL/JSON node */
        smartlist_t     *depends;                     /**< What package(s) it depends on; a smartlist of `char *` */
        smartlist_t     *install_info;                /**< A list of `/bin`, `/lib` and `/include` files installed. This is never written/read to/from cache-file */
        smartlist_t     *features;                    /**< The features; a smartlist of `char *` */
      } vcpkg_package;

/**
 * The list of `CONTROL`, `JSON` and `portfile.cmake` file entries.
 * A smartlist of `port_node`.
 */
static smartlist_t *ports_list;

/**
 * A list of available packages found in `CONTROL` or `vcpkg.json` files
 * under `<vcpkg_root>/ports`. A smartlist of `vcpkg_package`.
 */
static smartlist_t *available_packages;

/**
 * A list of actually installed packages found under `<vcpkg_root>/installed/<platform>[-static]`.
 *
 * A smartlist of `vcpkg_package`.
 */
static smartlist_t *installed_packages;

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
static char last_err_str [_MAX_PATH+50];

/**
 * The recursion-level for sub-dependency checking.
 */
static int sub_level = 0;

/**
 * Print details on installed packages only.
 */
static BOOL only_installed = TRUE;

/**
 * The VCPKG version information.
 */
static struct ver_info vcpkg_ver;

/**
 * The platforms we support when parsing in `CONTROL_add_dependency_platform()`.
 */
static const struct search_list platforms [] = {
                              { VCPKG_plat_WINDOWS, "windows" },
                              { VCPKG_plat_LINUX,   "linux"   },
                              { VCPKG_plat_UWP,     "uwp"     },
                              { VCPKG_plat_ARM,     "arm"     },
                              { VCPKG_plat_ANDROID, "android" },
                              { VCPKG_plat_OSX,     "osx"     },
                              { VCPKG_plat_x86,     "x86"     },
                              { VCPKG_plat_x64,     "x64"     },

                              /**
                               * static build assumed unless "!static" given in
                               * `CONTROL` or `vcpkg.json` file
                               */
                              { VCPKG_plat_STATIC,  "static"  }
                            };

static BOOL        get_control_node (int *index_p, const port_node **node_p, const char *package_spec);
static const char *get_platform_name (const VCPKG_plat_list p);
static BOOL        get_depend_name (const VCPKG_plat_list p_list, const char **name);
static BOOL        get_installed_info (vcpkg_package *package);
static const char *get_installed_dir (const vcpkg_package *package);
static const char *get_packages_dir (const vcpkg_package *package);
static int         get_plat_value (VCPKG_platform platform, int idx, const char **name);

static BOOL  is_plat_supported (const VCPKG_plat_list p_list, unsigned platform);
static BOOL  is_x86_supported (const VCPKG_plat_list p_list);
static BOOL  is_x64_supported (const VCPKG_plat_list p_list);
static BOOL  is_windows_supported (const VCPKG_plat_list p_list);
static BOOL  is_uwp_supported (const VCPKG_plat_list p_list);
static BOOL  is_static_supported (const VCPKG_plat_list p_list);
static int   compare_port_node (const void **_a, const void **_b);
static int   compare_package (const void **_a, const void **_b);
static void *find_available_package (const char *pkg_name);
static void *find_installed_package (int *index_p, const char *pkg_name, const char *arch);
static void *find_or_alloc_package_dependency (const vcpkg_package *package);
static int   print_top_dependencies (FMT_buf *fmt_buf, const port_node *node, int indent);
static int   print_sub_dependencies (FMT_buf *fmt_buf, const port_node *node, int indent, smartlist_t *sub_package_list);
static BOOL  print_install_info     (FMT_buf *fmt_buf, const char *package, int indent1);
static int   json_parse_file (port_node *node, const char *file);

/**
 * regex stuff
 */
static regex_t    re_hnd;
static regmatch_t re_matches[3];  /**< regex sub-expressions */
static int        re_err;         /**< last regex error-code */
static char       re_errbuf[10];  /**< regex error-buffer */

/**
 * Free the memory allocated to `re_hnd`.
 */
static void regex_free (void)
{
  if (re_hnd.buffer)
     regfree (&re_hnd);
}

/**
 * Print the sub expressions in `re_matches[]`.
 */
_WUNUSED_FUNC_OFF()
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
  TRACE (1, "regex() pattern '%s' against '%s'. re_err: %d\n", pattern, str, re_err);

  if (re_err == REG_NOMATCH)
     return (FALSE);

  if (re_err == REG_NOERROR)
     return (TRUE);

  regerror (re_err, &re_hnd, re_errbuf, sizeof(re_errbuf));
  TRACE (1, "Error while matching \"%s\": %s (%d)\n", str, re_errbuf, re_err);
  return (FALSE);
}
_WUNUSED_FUNC_POP()

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
 * So they are not recursed and printed more than once.
 */
static BOOL sub_package_found (const char *package, smartlist_t *sub_package_list)
{
  const char *pkg;
  int   i, max = smartlist_len (sub_package_list);

  for (i = 0; i < max; i++)
  {
    pkg = smartlist_get (sub_package_list, i);
    if (!strcmp(pkg, package))
         return (TRUE);
  }
  /* Simply add the 'char*' pointer to "already found list"
   */
  smartlist_add (sub_package_list, (void*)package);
  return (FALSE);
}

/**
 * Dump the information of `CONTROL` or `vcpk.json` nodes with a `node->package`
 * matching `package_spec`.
 *
 * The output of 'vcpkg search dml' is similar to this:
 * ```
 *  dmlc                 2019-08-12-4     DMLC-Core is the backbone library to support all DMLC projects, offers the bri...
 *  dmlc[openmp]                          Build with openmp
 * ```
 *
 * \note The truncated descriptions and no dependency information. It does neither support wildcards.
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
 *  1 match found for "3f*" with 33 unique sub-dependencies.
 * ```
 */
static unsigned vcpkg_find_internal (FMT_buf *fmt_buf, const char *package_spec, smartlist_t *sub_package_list)
{
  const port_node *node;
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
      buf_puts_long_line (fmt_buf, node->description ? node->description : "<none>", indent);
      buf_printf (fmt_buf, "  %-*s%s%s\n", indent-2, "version: ", node->version[0] ? node->version : "<none>", node->have_JSON ? " (have_JSON)" : "");
      buf_printf (fmt_buf, "  %-*s%s\n", indent-2, "homepage:", node->homepage[0] ? node->homepage : "<none>");
    }
    else
    {
      indent = 2;
      buf_printf (fmt_buf, "%-*s%s:\n", indent + 2*sub_level, "", package);
    }

    num_deps = print_top_dependencies (fmt_buf, node, indent-2);

    if (opt.verbose >= 1 && num_deps > 1)
       print_sub_dependencies (fmt_buf, node, indent, sub_package_list);

    if (sub_level == 0)
    {
      if (print_install_info(fmt_buf, package, indent-2) || num_deps == 0)
           C_puts (fmt_buf->buffer_start);
      else matches--;

      buf_reset (fmt_buf);
    }
  }
  return (matches);
}

unsigned vcpkg_find (const char *package_spec)
{
  FMT_buf      fmt_buf;
  unsigned     num;
  smartlist_t *sub_package_list = smartlist_new();

  vcpkg_init();

  BUF_INIT (&fmt_buf, BUF_INIT_SIZE, 1);

  sub_level = 0;
  num = vcpkg_find_internal (&fmt_buf, package_spec, sub_package_list);
  sub_level = 0;

  BUF_FREE (&fmt_buf);

  smartlist_free (sub_package_list);
  return (num);
}

/**
 * Print the package sub-dependencies for a `CONTROL` or `vcpkg.json` node.
 */
static int print_sub_dependencies (FMT_buf *fmt_buf, const port_node *node, int indent, smartlist_t *sub_package_list)
{
  int i, i_max, j, j_max, found;

  if (!node->depends || smartlist_len(node->depends) == 0)
  {
    if (sub_level == 0)
       buf_printf (fmt_buf, "%-*s<none>\n", indent, "");
    return (0);
  }

  i_max = smartlist_len (node->depends);
  j_max = smartlist_len (available_packages);

  for (i = found = 0; i < i_max; i++)
  {
    const char *dep1 = smartlist_get (node->depends, i);

    for (j = 0; j < j_max; j++)
    {
      const vcpkg_package *dep2 = smartlist_get (available_packages, j);

      if (strcmp(dep1, dep2->package))  /* 'dep2->package' is not in dependencies of 'node->depends' */
         continue;

      if (sub_package_found(dep1, sub_package_list))   /* already shown dependencies of for this 'node->depends' */
         continue;

      /* Will call 'get_control_node()' only once
       */
      found++;
      ++sub_level;
      vcpkg_find_internal (fmt_buf, dep1, sub_package_list);
      --sub_level;
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
  const char *pkg_name;
  size_t longest_package = 0;
  int    i, max;

  if (sub_level > 0)
  {
    if (!node->depends || smartlist_len(node->depends) == 0)
       return (0);
  }
  else
  {
    buf_printf (fmt_buf, "  %-*s", indent, "dependencies:");
    if (!node->depends || smartlist_len(node->depends) == 0)
    {
      buf_puts (fmt_buf, "<none>\n");
      return (0);
    }
  }

  max = smartlist_len (node->depends);

  /* First, get the value for 'longest_package'
   */
  for (i = 0; i < max; i++)
  {
    pkg_name = smartlist_get (node->depends, i);
    longest_package = max (strlen(pkg_name), longest_package);
  }

  for (i = 0; i < max; i++)
  {
    const vcpkg_package *package;
    const char *name;
    BOOL        supported;

    pkg_name = smartlist_get (node->depends, i);
    package = find_available_package (pkg_name);

    if (sub_level > 0)
       buf_printf (fmt_buf, "%-*s%s;\n", indent + 2*sub_level, "", pkg_name);
    else if (package)
    {
      if (i > 0)
         buf_printf (fmt_buf, "%-*s", indent+2, "");

      buf_printf (fmt_buf, "%-*s  platform: ", (int)longest_package, pkg_name);
      supported = get_depend_name (package->platforms, &name);
      if (!supported)
           buf_printf (fmt_buf, "!(%s)", name);
      else buf_printf (fmt_buf, "%s", name);
      buf_printf (fmt_buf, " (0x%04X)\n", package->platforms[0]);
    }
  }
  return (max);
}

/**
 * Split a line like "!uwp&!windows" and fill the `package->platforms[]` array for it.
 * On the first call, do it recursively.
 */
static void CONTROL_add_dependency_platform (vcpkg_package *package, const char *plat_buf, int i, BOOL recurse)
{
  char    *platform = strdupa (plat_buf);
  unsigned val, Not = 0;

  if (*platform == '!')
  {
    platform++;
    Not = 1;    /* Sets the "not for this platform" bit. */
  }

  val = list_lookup_value (platform, platforms, DIM(platforms));
  if (val != UINT_MAX)
  {
    package->platforms[i] = (VCPKG_platform) val | Not;
    return;
  }
  if (recurse && i < VCPKG_MAX_PLAT)
  {
    char *tok_end, *tok = _strtok_r (platform, "&", &tok_end);

    while (tok)
    {
      CONTROL_add_dependency_platform (package, tok, i+1, FALSE);
      tok = _strtok_r (NULL, "&", &tok_end);
    }
  }
}

/**
 * Split a line like "x86-windows" and (on the first call, do it recursively)
 * set the `VCPKG_platform_x` value for it.
 */
static BOOL make_package_platform (vcpkg_package *package, const char *platform, int i, BOOL recurse)
{
  unsigned val = list_lookup_value (platform, platforms, DIM(platforms));

  if (val != UINT_MAX && i < VCPKG_MAX_PLAT)
  {
    package->platforms[i] = val;
    return (TRUE);
  }

  if (recurse)
  {
    char *tok_end, *tok, *copy = strdupa (platform);

    for (tok = _strtok_r(copy, "-", &tok_end); tok;
         tok = _strtok_r(NULL, "-", &tok_end))
    {
      if (make_package_platform(package, tok, i, FALSE))
         ++i;
    }
  }
  return (FALSE);
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
  TRACE (2, "package: '%s', cpu: '%.4s'.\n", package, cpu ? cpu+1 : "<None>");

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
 * Split a line like:
 *   "openssl (!uwp&!windows), curl (!uwp&!windows)"
 *
 * first into tokens of:
 *   "openssl (!uwp&!windows)" and "curl (!uwp&!windows)".
 *
 * If a token contains a "(xx)" part, pass that to `CONTROL_add_dependency_platform()`
 * which recursively figures out the platform(s) for the package.
 *
 * Add a package-dependency to `node` as long as there are more ","
 * tokens in `str` to parse.
 */
static void CONTROL_add_dependencies (port_node *node, char *str)
{
  char *tok, *tok_end;
  int   str0 = str[0];

  if (strchr(str, ')') > strchr(str, '('))
     TRACE (2, "str: '%s'\n", str);

  if (str0 == '\0' || (VALID_CH(str0) && isspace(str0) && str[1] == '\0'))
  {
    TRACE (2, "Empty dependencies! str: '%s'\n", str);
    return;
  }

  for (tok = _strtok_r(str,  ",", &tok_end); tok;
       tok = _strtok_r(NULL, ",", &tok_end))
  {
    vcpkg_package package;
    char  pkg_name [2*VCPKG_MAX_NAME];
    char  platform [50+1];
    char *p, *l_paren;

    memset (&package, '\0', sizeof(package));

    p = str_trim (tok);
    p = _strlcpy (pkg_name, p, sizeof(pkg_name));

    l_paren = strchr (pkg_name, '(');
    if (l_paren && sscanf(l_paren+1, "%50[^)])", platform) == 1)
    {
      *l_paren = '\0';
      p = str_trim (pkg_name);
      TRACE (2, "platform: '%s', tok: '%s', tok_end: '%s'\n", platform, tok, tok_end);
      CONTROL_add_dependency_platform (&package, platform, 0, TRUE);
    }
    smartlist_add (node->depends, STRDUP(p));  // !! fixme: the 'package.platform[]' is now lost
  }
}

/**
 * Parse the content of a `CONTROL` file and add it's contents to `node`.
 */
static int CONTROL_parse (port_node *node, const char *file)
{
  FILE *f = fopen (file, "r");
  char *p, *next, buf [3000];   /* Enough? */
  int   num = 0;

  if (!f)
  {
    TRACE (2, "Failed to open %s.\n", file);
    return (0);
  }

  while (fgets(buf, sizeof(buf)-1, f))
  {
    str_strip_nl (buf);
    p = str_ltrim (buf);

    TRACE (4, "p: '%s'\n", p);

    /* In case 'node->homepage' etc. contains a '~', replace with "~~".
     */
    if (!node->description && str_match(p, CONTROL_DESCRIPTION, &next))
    {
      str_replace2 ('~', "~~", next, sizeof(buf) - (next - p));
      node->description = STRDUP (next);
      num++;
    }
    else if (!node->version[0] && str_match(p, CONTROL_VERSION, &next))
    {
      str_replace2 ('~', "~~", next, sizeof(buf) - (next - p));
      _strlcpy (node->version, next, sizeof(node->version));
      num++;
    }
    else if (!node->homepage[0] && str_match(p, CONTROL_HOMEPAGE, &next))
    {
      str_replace2 ('~', "~~", next, sizeof(buf) - (next - p));
      _strlcpy (node->homepage, next, sizeof(node->homepage));
      num++;
    }
    else if (str_match(p, CONTROL_DEFAULT_FEATURES, &next))
    {
      ASSERT (node->features == NULL);
      node->features = smartlist_split_str (next, ", ");
      TRACE (3, "Adding feature(s): '%s'\n", next);
      num++;
    }
    else if (str_match(p, CONTROL_BUILD_DEPENDS, &next))
    {
      CONTROL_add_dependencies (node, next);
      num++;
    }
#if 0
    else if (str_match(p, CONTROL_SUPPORTS, &next))
    {
      smartlist_addu (node->supports, get_supported_platform_from_str(next));
      num++;
    }
#endif
  }
  fclose (f);
  return (num);
}

/**
 * \todo
 *  Parse `file` for LOCAL package location or REMOTE package URL
 */
static int portfile_cmake_parse (port_node *node, const char *file)
{
  ARGSUSED (node);
  ARGSUSED (file);
  return (0);
}

/**
 * Traverse a `dir` relative to `vcpkg_root` looking for sub-directories
 * (first level only).
 *
 * \param[in] dir_list       The smartlist to add a directory to. All directories are relative
 *                           to `vcpkg_root` to save some memory.
 * \param[in] dir            The directory to build `dir_list` from.
 * \param[in] check_CONTROL  Check for a `CONTROL` in each directory.
 *                           If it's missing, do not add the directory to `dir_list`.
 */
static void build_dir_list (smartlist_t *dir_list, const char *dir, BOOL check_CONTROL)
{
  struct dirent2 *de;
  DIR2           *dp;
  char            abs_dir [_MAX_PATH];
  size_t          ofs = strlen (vcpkg_root) + 1;

  snprintf (abs_dir, sizeof(abs_dir), "%s\\%s", vcpkg_root, dir);
  if (!is_directory(abs_dir) || (dp = opendir2(abs_dir)) == NULL)
  {
    snprintf (last_err_str, sizeof(last_err_str), "No such directory %s", abs_dir);
    return;
  }

  while ((de = readdir2(dp)) != NULL)
  {
    if (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY)
    {
      const char *d = de->d_name + ofs;
      char  CONTROL_file [_MAX_PATH];

      /* Check for a `CONTROL` file in this directory?
       */
      if (check_CONTROL)
      {
        snprintf (CONTROL_file, sizeof(CONTROL_file), "%s\\CONTROL", de->d_name);
        if (!FILE_EXISTS(CONTROL_file))
        {
          TRACE (1, "Missing '%s'\n", CONTROL_file);
          continue;
        }
      }
      TRACE (2, "Adding '%s'\n", d);
      smartlist_add (dir_list, STRDUP(d));
    }
  }
  closedir2 (dp);
}

/**
 * Look in `<vcpkg_root>\\ports\\<dir>\\` for `CONTROL`, `vcpkg.json` or `portfile.cmake` files
 * and add the parsed results to `ports_list`.
 */
static void get_port_info_from_disk (const char *port_dir, int ports_index)
{
  port_node  *node;
  char        CONTROL_file [_MAX_PATH];
  char        JSON_file [_MAX_PATH];
  char        port_file [_MAX_PATH];
  const char *package_name = port_dir + strlen ("ports\\");

  snprintf (CONTROL_file, sizeof(CONTROL_file), "%s\\ports\\%s\\CONTROL", vcpkg_root, package_name);
  snprintf (JSON_file, sizeof(JSON_file), "%s\\ports\\%s\\vcpkg.json", vcpkg_root, package_name);
  snprintf (port_file, sizeof(port_file), "%s\\ports\\%s\\portfile.cmake", vcpkg_root, package_name);

  if (FILE_EXISTS(CONTROL_file))
  {
    TRACE (2, "%d: Building port-node for %s.\n", ports_index, CONTROL_file);

    node = CALLOC (sizeof(*node), 1);
    node->have_CONTROL = TRUE;
    node->depends  = smartlist_new();
    node->supports = smartlist_new();
    smartlist_addu (node->supports, VCPKG_plat_ALL);
    _strlcpy (node->package, package_name, sizeof(node->package));

    CONTROL_parse (node, CONTROL_file);
    smartlist_add (ports_list, node);

    if (smartlist_getu(node->supports, 0) == VCPKG_plat_ALL)
       smartlist_del (node->supports, 0);
  }
  else if (FILE_EXISTS(JSON_file))
  {
    TRACE (1, "%d: Building JSON port-node for %s.\n", ports_index, JSON_file);

    node = CALLOC (sizeof(*node), 1);
    node->have_JSON = TRUE;
    node->depends  = smartlist_new();
    node->features = smartlist_new();
    node->supports = smartlist_new();
    smartlist_addu (node->supports, VCPKG_plat_ALL);

    _strlcpy (node->package, package_name, sizeof(node->package));

    if (!json_parse_file(node, JSON_file))
       TRACE (1, "parse_JSON_file (\"%s\") failed.\n", JSON_file);
    smartlist_add (ports_list, node);
  }

  if (FILE_EXISTS(port_file))
  {
    TRACE (2, "%d: Building port-node for %s.\n", ports_index, port_file);

#if 0   // Not ready for this code
    node = CALLOC (sizeof(*node), 1);
    _strlcpy (node->package, package_name, sizeof(node->package));
    strcpy (node->homepage, "-");
    portfile_cmake_parse (node, port_file);
    smartlist_add (ports_list, node);
#else
    node = NULL;
    portfile_cmake_parse (node, port_file);
#endif
  }
}

/**
 * Build the `ports_list` smartlist from file-cache.
 */
static int get_ports_list_from_cache (void)
{
  port_node *node;
  int  i;

  for (i = 0;; i++)
  {
    char format [100], *package, *version, *homepage, *description;
    int  have_CONTROL, have_JSON, rc;

    /*
     * Parse a cache-line like:
     * port_node_0 = 3fd, 0, 1, 2.6.3, -, "C++ Framework For Fast Development"
     *                ^   ^  ^  ^      ^  ^
     *     package ___|   |  |  |      |  |__description
     *                    |  |  |      |____ homepage
     *                    |  |  |____ version
     *                    |  |_______ have_JSON
     *                    |___________have_CONTROL
     *
     *
     */
    snprintf (format, sizeof(format), "port_node_%d = %%s,%%d,%%d,%%s,%%s,%%s", i);
    rc = cache_getf (SECTION_VCPKG, format, &package, &have_CONTROL, &have_JSON, &version, &homepage, &description);
    TRACE (2, "port_node from cache, rc: %d: (%s\\%s):\n"
               "     package: '%s', have_CONTROL: %d, have_JSON: %d, version: '%s', homepage: '%s', description: '%s'.\n",
            rc, vcpkg_root, package, package, have_CONTROL, have_JSON, version, homepage, description);

    if (rc != 6)
      break;

    node = CALLOC (sizeof(*node), 1);
    node->have_CONTROL = have_CONTROL;
    node->have_JSON    = have_JSON;
    node->description  = STRDUP (str_unquote(description));
    _strlcpy (node->package, package, sizeof(node->package));
    _strlcpy (node->version, version, sizeof(node->version));
    _strlcpy (node->homepage, homepage, sizeof(node->homepage));
    smartlist_add (ports_list, node);
  }
  smartlist_sort (ports_list, compare_port_node);
  return (i);
}

/**
 * Build each `node->depends` from file-cache and add to
 * correct place in `ports_list`.
 *
 * \eg.
 *   A `port_deps_10 = "ilmbase,hdf5"` should add `ilmbase` and `hdf5` to `node->depends`
 *   for the 10th entry in `ports_list`.
 *
 *   And a `port_deps_11 = -` should keep `node->depends == NULL`.
 */
static int get_port_deps_from_cache (int max)
{
  port_node *node;
  int  i;

  for (i = 0; i < max; i++)
  {
    char key[100], *value;

    snprintf (key, sizeof(key), "port_deps_%d", i);
    value = (char*) cache_get (SECTION_VCPKG, key);
    TRACE (2, "port_deps_%d from cache: '%s'\n", i, value);
    if (!value)
       break;

    if (*value == '\0' || *value == '-')
       continue;

    node = smartlist_get (ports_list, i);
    ASSERT (node->depends == NULL);
    node->depends = smartlist_split_str (value, ", ");
  }
  return (i);
}

/**
 * Build the port-node features from cache.
 * \eg.
 *   with a "port_node_1057 = realsense2,...",
 *   means 'realsense2' has this feature-string in cache:
 *   "port_features_1057 = tools,openni2,tm2"
 */
static int get_port_features_from_cache (int max)
{
  port_node *node;
  int  i;

  for (i = 0; i < max; i++)
  {
    char key[1000], *value;  /* defaults to no features */

    snprintf (key, sizeof(key), "port_features_%d", i);
    value = (char*) cache_get (SECTION_VCPKG, key);
    TRACE (2, "port_features_%d from cache: '%s'\n", i, value);
    if (!value)
       break;

    if (*value == '-' || *value == '\0')
       continue;

    node = smartlist_get (ports_list, i);
    ASSERT (node->features == NULL);
    node->features = smartlist_split_str (value, ", ");
  }
  return (i);
}


/**
 * Return a pointer to `last_err_str`.
 */
const char *vcpkg_last_error (void)
{
  return (last_err_str);
}

void vcpkg_clear_error (void)
{
  last_err_str[0] = '\0';
}

/**
 * Print the description for a node in `ports_list`.
 */
static void dump_port_description (const port_node *node, const char *indent)
{
  int len  = C_puts (indent) + C_puts ("~6description:~0  ") - 2;
  int save = C_setraw (1);

  if (node->description)
       C_puts_long_line (node->description, len);
  else C_puts ("<none>\n");
  C_setraw (save);
}

/**
 * Dump the dependencies for a `port_node`.
 *
 * If a dependency is for a specific platforms (or not), print the dependency like: \n
 *   `boost (windows), ` (or `boost (!uwp), `).
 *
 * If a dependency is for all platforms, print the dependency simply like: \n
 *    `boost, `.
 */
static void dump_port_dependencies (const port_node *node, const char *indent)
{
  int   i, len, max;
  char *depencencies;

  len = C_puts (indent) + C_puts ("~6dependencies:~0 ") - 2;
  max = node->depends ? smartlist_len (node->depends) : 0;
  for (i = 0; i < max; i++)
  {
    const vcpkg_package *dep = find_available_package (smartlist_get(node->depends, i));
    const char *name;
    BOOL  supported;

    if (!dep)
       break;
#if 0
    len += C_puts (dep->package);
    supported = get_depend_name (dep->platforms, &name);
    if (strcmp(name, "all"))
    {
      if (supported)
           len += C_printf (" (%s)", name);
      else len += C_printf (" !(%s)", name);
    }
#else
  ARGSUSED (name);
  ARGSUSED (supported);
#endif
  }

  depencencies = smartlist_join_str (node->depends, ", ");
  C_puts_long_line (depencencies ? depencencies : "<none>", len);
  FREE (depencencies);
}

/**
 * Dump the features for a node in `ports_list`.
 */
static void dump_port_features (const port_node *node, const char *indent)
{
  int   len = C_puts (indent) + C_puts ("~6features:~0     ") - 2;
  char *features = smartlist_join_str (node->features, ", ");

  C_puts_long_line (features ? features : "<none>", len);
  FREE (features);
}

/**
 * Dump the `supports` field of a node in `ports_list`.
 */
static void dump_port_supports (const port_node *node, const char *indent)
{
  int i, num, max = node->supports ? smartlist_len (node->supports) : 0;
  int len = C_puts (indent) + C_puts ("~6supports:~0     ") - 2;

  if (max == 0)
  {
    C_puts ("<none>\n");
    return;
  }
  for (i = num = 0; i < max; i++)
  {
    const char *name;
    unsigned    value = smartlist_getu (node->supports, i);
    int         supported = get_plat_value (value, i, &name);

    if (i > 0)
       C_printf ("%*s", len, "");
    if (supported >= 0)
    {
      C_printf ("0x%04X: %s%s\n", value, supported ? "" : "!", name);
      num++;
    }
  }
  if (num == 0)
     C_puts ("all\n");
}

/**
 * Iterate over all installed package files and get the total file-size as a string.
 */
static const char *get_package_files_size (vcpkg_package *package)
{
  UINT64 f_size = 0;
  int    i, max = package->install_info ? smartlist_len (package->install_info) : 0;

  for (i = 0; i < max; i++)
  {
    struct stat st;
    char        path [_MAX_PATH];
    const char *file = smartlist_get (package->install_info, i);

    ASSERT (file[0]);

    snprintf (path, sizeof(path), "%s\\installed\\%s", vcpkg_root, file);
    if (safe_stat(path, &st, NULL) == 0)
       f_size += get_file_alloc_size (path, st.st_size);
  }
  incr_total_size (f_size);
  return str_ltrim ((char*)get_file_size_str(f_size));
}

/**
 * Print information for an installed package in `installed_packages`.
 */
static void print_installed_package_info (vcpkg_package *package, const char *indent)
{
  const char *dir = get_installed_dir (package);
  unsigned    num = smartlist_len (package->install_info);

  C_printf ("%s~6installed:    YES~0\n", indent);
  C_printf ("%s~6location:     %s~0, %u files", indent, dir, num);

  if (opt.show_size)
     C_puts (get_package_files_size(package));
  C_puts ("\n\n");
}

/**
 * Print information for a not installed package in `installed_packages`.
 * Normally a package with "Status: purge ok not-installed".
 */
static void print_installed_package_purge_info (const vcpkg_package *package, const char *indent)
{
  C_printf ("%s~6installed:    NO~0\n", indent);
  C_printf ("%s~6status:       %s~0\n\n", indent, package->status);
}

/**
 * Dump the parsed information from `ports_list`.
 */
static void dump_ports_list (void)
{
  const char *indent = "      ";
  int    num_available = 0, num_installed = 0;
  int    i, max = smartlist_len (ports_list);

  /* Print a simple header.
   */
  C_printf ("%d nodes in 'ports_list':\n~6Index ~3Package~0 / ~6Version\n", max);
  C_puts (str_repeat('=', 100));
  C_putc ('\n');

  for (i = 0; i < max; i++)
  {
    const port_node *node = smartlist_get (ports_list, i);
    const char      *version = node->version;
    vcpkg_package   *package;
    int              zero = 0;

    if (!node->have_CONTROL && !node->have_JSON)
       continue;

    if (*version == '\0' || *version == ' ')
       version = "<unknown>";

    C_printf ("~7%4d  ~3%s~0 / ~6%s~0%s\n", num_available, node->package, version, node->have_JSON ? " (have_JSON)" : "");

    dump_port_description (node, indent);
    C_printf ("%s~6homepage:~0     %s\n", indent, node->homepage[0] ? node->homepage : "<none>");

    dump_port_dependencies (node, indent);
    dump_port_features (node, indent);
    dump_port_supports (node, indent);

    num_available++;

    /* \todo: iterate to find all installed packages matching these architectures:
     *        'x86-windows', 'x86-windows-static', 'x64-windows', 'x64-windows-static',
     */
    package = find_installed_package (&zero, node->package, NULL);
    if (!package)
    {
      C_printf ("%s~6installed:    NO~0\n\n", indent);
      continue;
    }
    num_installed++;

    if (package->install_info)
         print_installed_package_info (package, indent);
    else print_installed_package_purge_info (package, indent);
  }
  C_printf ("num_available: %4d\n"
            "num_installed: %4d\n\n", num_available, num_installed);
}

/**
 * Dump the information for `installed_packages`.
 */
static void dump_installed_packages (void)
{
  int i, i_max = smartlist_len (installed_packages);

  C_puts (str_repeat('=', 120));
  C_printf ("\n%d packages in 'installed_packages' (disregarding different architectures):\n"
            "Package                Version            Architecture        "
            "install_info   link       Platforms             Features\n", i_max);

  for (i = 0; i < i_max; i++)
  {
    const vcpkg_package *package = smartlist_get (installed_packages, i);
    const char          *Platforms = "-";
    char                *features;

    if (package->link)
    {
      const port_node *node = package->link;
      char             buf[10000], *p = buf;
      int              left = (int) sizeof(buf);
      int              j, len, j_max = node->supports ? smartlist_len (node->supports) : 0;
      unsigned         value;

      strcpy (p, "all");
      for (j = 0; j < j_max && left > 9; j++)
      {
        value = smartlist_getu (node->supports, j);
        len = snprintf (p, left, "0x%04X, ", value);
        left -= len;
        p    += len;
      }
      if (j > 0)
      {
        Platforms = buf;
        p[-2] = '\0';
      }
    }
    C_printf ("%-20.20s   %-18s %-18s  ", package->package, package->version, package->arch);
    C_printf ("%p       %p   %-20s  ", package->install_info, package->link, Platforms);

    features = smartlist_join_str (package->features, ", ");
    C_puts (features ? features : "-");
    FREE (features);
    C_putc ('\n');
  }
  C_puts ("\n\n");
}

/**
 * Traverse the smartlist `ports_list` and
 * return the number of nodes where:
 *  \li `node->have_CONTROL == have_CONTROL`  or
 *  \li `node->have_JSON == have_JSON`.
 */
static unsigned vcpkg_get_num (BOOL have_CONTROL, BOOL have_JSON)
{
  const port_node *node;
  unsigned num = 0;
  int      i, max = ports_list ? smartlist_len (ports_list) : 0;

  for (i = 0; i < max; i++)
  {
    node = smartlist_get (ports_list, i);
    if (node->have_CONTROL == have_CONTROL || node->have_JSON == have_JSON)
       num++;
  }
  return (num);
}

/**
 * Build the smartlist `ports_list *` representing all available VCPKG packages
 * (ignoring whether a package is installed or not).
 *
 * \param[in] port_dirs   The `smartlist_t*` of the directories to build the `port_node*` list
 *                        from if `from_cache` is FALSE.
 * \param[in] from_cache  Build the `ports_list` from cache only.
 *
 * \retval The length of the `ports_list`.
 */
static int get_all_available (const smartlist_t *port_dirs, BOOL from_cache)
{
  int i, max;

  if (port_dirs)
  {
    max = smartlist_len (port_dirs);

    if (from_cache)
    {
      TRACE (2, "Found %d cached VCPKG port directories.\n", max);
      max = get_ports_list_from_cache();
      get_port_deps_from_cache (max);
      get_port_features_from_cache (max);

      /* The 'available_packages' list should already been built
       * by 'get_available_packages_from_cache()'.
       */
    }
    else
    {
      TRACE (2, "Found %d VCPKG port directories.\n", max);
      for (i = 0; i < max; i++)
      {
        vcpkg_package *package  = CALLOC (sizeof(*package), 1);
        char          *port_dir = smartlist_get (port_dirs, i);
        const char    *pkg_name = port_dir + strlen ("ports\\");

        get_port_info_from_disk (port_dir, i);
        _strlcpy (package->package, pkg_name, sizeof(package->package));
        smartlist_add (available_packages, package);
      }
      smartlist_sort (available_packages, compare_package);
    }
  }
  max = smartlist_len (ports_list);
  if (max == 0)
     snprintf (last_err_str, sizeof(last_err_str), "No ~6VCPKG~0 packages found%s",
               from_cache ? " in cache" : "");
  return (max);
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
 * Parse a line from `vcpkg_parse_status_file()` and add elements to
 * the package at `*package`.
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
static int vcpkg_parse_status_line (vcpkg_package *package, char **line_p)
{
  char *next, *eol, *line;

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

  TRACE (2, "line: '%.50s'.\n", line);

  if (str_match(line, STATUS_PACKAGE, &next))
  {
    _strlcpy (package->package, next, sizeof(package->package));
    return (1);
  }
  if (str_match(line, STATUS_ARCH, &next))
  {
    _strlcpy (package->arch, next, sizeof(package->arch));
    return (1);
  }
  if (str_match(line, STATUS_VERSION, &next))
  {
    _strlcpy (package->version, next, sizeof(package->version));
    str_replace2 ('~', "~~", package->version, sizeof(package->version));
    return (1);
  }
  if (str_match(line, STATUS_DEPENDS, &next))
  {
    ASSERT (package->depends == NULL);
    package->depends = smartlist_split_str (next, ", ");
    return (1);
  }
  if (str_match(line, STATUS_DEFAULT_FEATURES, &next))
  {
    ASSERT (package->features == NULL);
    package->features = smartlist_split_str (next, ", ");
    return (1);
  }
  return (0);
}

/**
 * Compare 2 `vcpkg_package *` records on name, architecture and version.
 */
static int compare_package (const void **_a, const void **_b)
{
  const vcpkg_package *a = *_a;
  const vcpkg_package *b = *_b;
  int   rc = stricmp (a->package, b->package);

  if (rc == 0)
     rc = stricmp (a->arch, b->arch);

  if (rc == 0)  /* same name and arch */
     rc = stricmp (a->version, b->version);
  return (rc);
}

/**
 * Compare 2 `port_node *` records on name.
 */
static int compare_port_node (const void **_a, const void **_b)
{
  const port_node *a = *_a;
  const port_node *b = *_b;

  return stricmp (a->package, b->package);
}

/**
 * Free memory for a single `package *` structure.
 */
static void free_package (vcpkg_package *package, BOOL free_package)
{
  smartlist_free_all (package->install_info);
  smartlist_free_all (package->depends);
  smartlist_free_all (package->features);
  package->install_info = package->depends = package->features = NULL;
  if (free_package)
     FREE (package);
}

/*
 * Compare the package features of 2 packages given by `features1 *` and `features2 *`.
 */
static BOOL equal_features (smartlist_t *features1, smartlist_t *features2)
{
  char *feature1, *feature2;
  BOOL  rc;

  if (!features1 && !features2)
     return (TRUE);

#if 0
  if ((features1 && !features2) || (!features1 && features2))
     return (FALSE);
#else
  if (!((BOOL)features1 ^ (BOOL)features2))
     return (FALSE);
#endif

  feature1 = smartlist_join_str (features1, ",");
  feature2 = smartlist_join_str (features2, ",");
  rc = (strcmp (feature1, feature2) == 0);
  FREE (features1);
  FREE (features2);
  return (rc);
}

/**
 * Check for any duplicates before adding another package to `installed_packages`.
 * We ignore all with the same name and older versions.
 */
static BOOL add_this_package (vcpkg_package *package, char **why_not)
{
  int i, max;

  *why_not = "-";

  if (package->status[0] && !str_startswith(package->status, "install ok"))
  {
    *why_not = "wrong status";
    return (FALSE);
  }

  if (!package->version[0] || package->version[0] == '?')
  {
    *why_not = "missing version";
    return (FALSE);
  }

  if (!package->arch[0])
  {
    *why_not = "missing arch";
    return (FALSE);
  }

  if (!get_installed_info(package))
  {
    *why_not = "missing info .list files (1)";
    return (FALSE);
  }

#if 0
  if (!get_packages_dir(package))
  {
    *why_not = "missing package dir";
    return (FALSE);
  }
#endif

  max = smartlist_len (installed_packages);
  for (i = 0; i < max; i++)
  {
    vcpkg_package *This = smartlist_get (installed_packages, i);

    if (strcmp(This->package, package->package))                    /* different package name */
       continue;
    if (strcmp(This->arch, package->arch))                          /* different architecture */
       continue;
    if (!equal_features(This->features, package->features))         /* different features */
       continue;
    if (strcmp(This->version, package->version))                    /* different version */
    {
      TRACE (1, "This->version: '%s', package->version: %s'\n", This->version, package->version);
      break;
    }
  }

  // if (i == max)

  if (!get_installed_info(package))
  {
    *why_not = "missing info .list files (2)";
    return (FALSE);
  }
  return (TRUE);
}

/**
 * Open and parse the `<vcpkg_root>/installed/vcpkg/status` file.
 * Build the `installed_packages` smartlist as we go along.
 * Not called if we have this information in the cache-file.
 *
 * The memory for this file is not freed until `vcpkg_exit()` is called since
 * this memory are used in e.g. `installed_packages::package` names.
 */
static int vcpkg_parse_status_file (void)
{
  struct stat   st;
  vcpkg_package package, *package2;
  FILE         *f;
  char          file [_MAX_PATH];
  char         *f_end, *f_ptr, *f_status_mem, *why_not;
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

  TRACE (2, "Building 'installed_packages' from %s (%u bytes).\n", file, (unsigned)f_size);

  memset (&package, '\0', sizeof(package));
  package.version[0] = '?';

  for (f_ptr = f_status_mem; f_ptr <= f_end; )
  {
    num_parsed += vcpkg_parse_status_line (&package, &f_ptr);
    if (*f_ptr == '\r' || *f_ptr == '\n')  /* An empty line ends this record */
    {
      f_ptr++;
      num_total++;
      TRACE (2, "reached EOR for package '%s'. num_parsed: %d, num_total: %d\n",
              package.package, num_parsed, num_total);
    }
    else
      continue;   /* continue parsing the same record */

    if (str_endswith(package.arch, "-static"))
    {
      TRACE (2, "package '%s' is 'static': '%s'.\n", package.package, package.arch);
      // !! todo clear any 'VCPKG_plat_STATIC' values in 'platform->platforms[]' list
    }

    if (add_this_package(&package, &why_not))
    {
      TRACE (1, "Adding package: '%s', arch: '%s'\n", package.package, package.arch);
      package2 = MALLOC (sizeof(*package2));
      memcpy (package2, &package, sizeof(*package2));
      package2->installed = TRUE;
      smartlist_add (installed_packages, package2);
    }
    else
    {
      TRACE (1, "Ignoring package: '%s': %s"
                "\n                                 (arch: '%s', ver: '%s')\n",
             package.package, why_not, package.arch, package.version);
      free_package (&package, FALSE);
      vcpkg_clear_error();
    }

    /* Ready for the next record of another package
     */
    memset (&package, '\0', sizeof(package));
    package.version[0] = '?';
    num_parsed = 0;
  }

  FREE (f_status_mem);
  smartlist_sort (installed_packages, compare_package);
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
  if (sscanf(buf, "Vcpkg package management program version %d-%d-%d",
             &ver.val_1, &ver.val_2, &ver.val_3) >= 2)
  {
    memcpy (&vcpkg_ver, &ver, sizeof(vcpkg_ver));
    return (1);
  }
  return (0);
}

/*
 * Write all collected information back to the file-cache.
 *
 * First the nodes in the `ports_list`.
 */
static void put_port_deps_to_cache (const port_node *node, int port_num)
{
  char format [100], *dependencies;

  snprintf (format, sizeof(format), "port_deps_%d = %%s", port_num);
  dependencies = smartlist_join_str (node->depends, ",");
  cache_putf (SECTION_VCPKG, format, dependencies ? dependencies : "-");
  FREE (dependencies);
}

static void put_port_features_to_cache (const port_node *node, int port_num)
{
  char format [1000], *features;

  snprintf (format, sizeof(format), "port_features_%d = %%s", port_num);
  features = smartlist_join_str (node->features, ",");
  cache_putf (SECTION_VCPKG, format, features ? features : "-");
  FREE (features);
}

static void put_port_dirs_to_cache (smartlist_t *dirs)
{
  int i, max = smartlist_len (dirs);

  for (i = 0; i < max; i++)
     cache_putf (SECTION_VCPKG, "port_dir_%d = %s", i, (const char*)smartlist_get(dirs, i));
}

static void put_packages_dirs_to_cache (smartlist_t *dirs)
{
  int i, max = smartlist_len (dirs);

  for (i = 0; i < max; i++)
     cache_putf (SECTION_VCPKG, "packages_dir_%d = %s", i, (const char*)smartlist_get(dirs, i));
}

static void put_available_packages_to_cache (void)
{
  int i, max = available_packages ? smartlist_len (available_packages) : 0;

  for (i = 0; i < max; i++)
  {
    const vcpkg_package *package = smartlist_get (available_packages, i);
    const vcpkg_package *inst_package;
    char *dependencies;
    int   zero = 0;
    int   installed = 1;

    inst_package = find_installed_package (&zero, package->package, NULL);
    if (inst_package && !strnicmp(inst_package->status, "purge", 5))
       installed = 0;

    dependencies = smartlist_join_str (package->depends, ",");
    cache_putf (SECTION_VCPKG, "available_package_%d = %s,%d,%s,%s,%s,\"%s\"", i,
                package->package, installed,
                package->version[0] ? package->version : "-",
                package->status[0]  ? package->status  : "-",
                package->arch[0]    ? package->arch    : "-",
                dependencies        ? dependencies     : "-");
    FREE (dependencies);
  }
}

static void put_installed_packages_to_cache (void)
{
  int i, max = installed_packages ? smartlist_len (installed_packages) : 0;

  for (i = 0; i < max; i++)
  {
    const vcpkg_package *package = smartlist_get (installed_packages, i);
    char *dependencies;
    int   installed = 1;

    if (!strnicmp(package->status, "purge", 5))
       installed = 0;

    dependencies = smartlist_join_str (package->depends, ",");
    cache_putf (SECTION_VCPKG, "installed_package_%d = %s,%d,%s,%s,%s,\"%s\"", i,
                package->package, installed,
                package->version[0] ? package->version : "-",
                package->status[0]  ? package->status  : "-",
                package->arch[0]    ? package->arch    : "-",
                dependencies        ? dependencies     : "-");
    FREE (dependencies);
  }
}

static void put_ports_list_to_cache (void)
{
  int i, max = ports_list ? smartlist_len (ports_list) : 0;

  for (i = 0; i < max; i++)
  {
    const port_node *node = smartlist_get (ports_list, i);

    if (node->have_CONTROL || node->have_JSON)
       cache_putf (SECTION_VCPKG, "port_node_%d = %s,%d,%d,%s,%s,\"%s\"", i,
                   node->package, node->have_CONTROL, node->have_JSON,
                   node->version[0]  ? node->version     : "-",
                   node->homepage[0] ? node->homepage    : "-",
                   node->description ? node->description : "-");
    put_port_deps_to_cache (node, i);
    put_port_features_to_cache (node, i);
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

  TRACE (2, "ver: %d.%d.%d.\n", ver->val_1, ver->val_2, ver->val_3);

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
      popen_run(vcpkg_version_cb, vcpkg_exe, "version") > 0)
     cache_putf (SECTION_VCPKG, "vcpkg_version = %d,%d,%d", vcpkg_ver.val_1, vcpkg_ver.val_2, vcpkg_ver.val_3);

  *ver = vcpkg_ver;
  TRACE (2, "ver: %d.%d.%d.\n", ver->val_1, ver->val_2, ver->val_3);

  return (vcpkg_exe && vcpkg_ver.val_1 + vcpkg_ver.val_2 > 0);
}

/**
 * Build the `ports_dirs` smartlist from file-cache.
 */
static int get_ports_dirs_from_cache (smartlist_t **ports_dirs)
{
  int i;

  *ports_dirs = smartlist_new();
  for (i = 0;; i++)
  {
    char format[100], *dir;

    snprintf (format, sizeof(format), "port_dir_%d = %%s", i);
    if (cache_getf(SECTION_VCPKG, format, &dir) != 1)
       break;
    smartlist_add (*ports_dirs, STRDUP(dir));
  }
  return (i);
}

/**
 * Build the `packages_dirs` smartlist from file-cache.
 */
static int get_packages_dirs_from_cache (smartlist_t **packages_dirs)
{
  int i;

  *packages_dirs = smartlist_new();
  for (i = 0;; i++)
  {
    char format[100], *dir;

    snprintf (format, sizeof(format), "packages_dir_%d = %%s", i);
    if (cache_getf(SECTION_VCPKG, format, &dir) != 1)
       break;
    smartlist_add (*packages_dirs, STRDUP(dir));
  }
  return (i);
}

/**
 * Build the `installed_packages` smartlist from file-cache.
 */
static int get_installed_packages_from_cache (void)
{
  int i;

  for (i = 0;; i++)
  {
    vcpkg_package *package;
    char  format [100], *pkg_name, *version, *status, *arch, *dependencies;
    int   rc, installed;

    snprintf (format, sizeof(format), "installed_package_%d = %%s,%%d,%%s,%%s,%%s,%%s", i);
    rc = cache_getf (SECTION_VCPKG, format, &pkg_name, &installed, &version, &status, &arch, &dependencies);
    if (rc != 6)
       break;

    if (!installed || *arch == '-')
       continue;

    package = CALLOC (sizeof(*package), 1);
    _strlcpy (package->package, pkg_name, sizeof(package->package));
    _strlcpy (package->arch, arch, sizeof(package->arch));

    if (*version != '-')
       _strlcpy (package->version, version, sizeof(package->version));

    if (*status != '-')
       _strlcpy (package->status, status, sizeof(package->status));

    if (strcmp(dependencies, "\"-\""))
       package->depends = smartlist_split_str (dependencies, ", ");

    get_installed_info (package);
    smartlist_add (installed_packages, package);
  }
  return smartlist_len (installed_packages);
}

/**
 * Build the `available_packages` smartlist from file-cache.
 */
static int get_available_packages_from_cache (void)
{
  int i;

  for (i = 0;; i++)
  {
    vcpkg_package *package;
    char  format [100], *pkg_name, *version, *status, *arch, *dependencies;
    int   rc, installed;

    snprintf (format, sizeof(format), "available_package_%d = %%s,%%d,%%s,%%s,%%s,%%s", i);
    rc = cache_getf (SECTION_VCPKG, format, &pkg_name, &installed, &version, &status, &arch, &dependencies);
    if (rc != 6)
       break;

    package = CALLOC (sizeof(*package), 1);
    _strlcpy (package->package, pkg_name, sizeof(package->package));

    if (*version != '-')
       _strlcpy (package->version, version, sizeof(package->version));

    if (*status != '-')
       _strlcpy (package->status, status, sizeof(package->status));

    if (*arch != '-')
       _strlcpy (package->arch, arch, sizeof(package->arch));

    if (strcmp(dependencies, "\"-\""))
       package->depends = smartlist_split_str (dependencies, ", ");

    smartlist_add (available_packages, package);
  }
  smartlist_sort (available_packages, compare_package);
  return smartlist_len (available_packages);
}

/**
 * Initialise VCPKG globals once and build the list of all
 * available and installed packages.
 */
void vcpkg_init (void)
{
  smartlist_t   *ports_dirs;
  smartlist_t   *packages_dirs;
  vcpkg_package *package;
  int            num_cached_available_packages;
  int            num_cached_packages_dirs;
  int            num_cached_ports_dirs;
  int            num_cached_installed_packages;
  int            i, j, max;
  BOOL           vcpkg_ok;
  static         BOOL done = FALSE;

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

  ASSERT (available_packages == NULL);
  ASSERT (installed_packages == NULL);
  ASSERT (ports_list         == NULL);

  available_packages = smartlist_new();
  installed_packages = smartlist_new();
  ports_list         = smartlist_new();

  last_err_str[0] = '\0';   /* clear any error-string set */

  num_cached_available_packages = get_available_packages_from_cache();
  num_cached_packages_dirs      = get_packages_dirs_from_cache (&packages_dirs);
  num_cached_ports_dirs         = get_ports_dirs_from_cache (&ports_dirs);
  num_cached_installed_packages = get_installed_packages_from_cache();

  /* If not from cache, build a dirlist using readdir2().
   * And then put that to cache.
   */
  if (smartlist_len(ports_dirs) == 0)
  {
    build_dir_list (ports_dirs, "ports", FALSE);
    put_port_dirs_to_cache (ports_dirs);
  }

  if (smartlist_len(packages_dirs) == 0)
  {
    build_dir_list (packages_dirs, "packages", TRUE);
    put_packages_dirs_to_cache (packages_dirs);
  }

  get_all_available (ports_dirs, num_cached_ports_dirs + num_cached_installed_packages > 0);

  smartlist_free_all (ports_dirs);

  /**
   * If we have no `<vcpkg_root>\\installed` directory. Hence no installed packages nor
   * status-file to parse. So there is no point using the cached info either.
   */
  if (!get_installed_dir(NULL))
       max = 0;
  else if (num_cached_installed_packages > 0)
       max = smartlist_len (installed_packages);
  else max = vcpkg_parse_status_file();

  for (i = j = 0; i < max; i++)
  {
    package = smartlist_get (installed_packages, i);
    get_control_node (&j, &package->link, package->package);
  }

  if (!packages_dirs || num_cached_installed_packages > 0)
       max = 0;
  else max = smartlist_len (packages_dirs);

  /**
   * Loop over all our packages directories:
   * `<vcpkg_root>\\packages\\xx`
   *
   * and figure out which belongs to the `installed_packages` list.
   * I.e. under `<vcpkg_root>\\installed\\`
   *
   * Handle only non-empty directories.
   * No need to do this if we have read the cached `installed_packages`.
   */
  for (i = 0; i < max; i++)
  {
    const char *homepage;
    char       *p, *q;

    /**
     * If e.g. `dirs` contains "packages\\sqlite3_x86-windows", add a node
     * with this to the `installed_packages` smartlist:
     *   `package->package     = "sqlite3"`
     *   `package->platform[0] = VCPKG_plat_x86`
     *   `package->platform[1] = VCPKG_plat_WINDOWS`.
     *   `package->link`       = a pointer into `ports_list` for more detailed info.
     */
    p = (char*) smartlist_get (packages_dirs, i) + strlen ("packages\\");
    q = strchr (p+1, '_');
    if (q && q - p < sizeof(package->package) && legal_package_name(p))
    {
      j = 0;
      package = CALLOC (sizeof(*package), 1);
      *q = '\0';
      _strlcpy (package->package, p, sizeof(package->package));
      make_package_platform (package, q+1, 0, TRUE);
      get_control_node (&j, &package->link, package->package);
      smartlist_add (installed_packages, package);

      if (package->link)
           homepage = package->link->homepage;
      else homepage = "?";

      TRACE (1, "package: %-20s  %-50s  platform: 0x%04X (%s).\n",
              package->package, homepage, package->platforms[0],
              flags_decode(package->platforms[0], platforms, DIM(platforms)));
    }
  }

  if (packages_dirs)
     smartlist_free_all (packages_dirs);

  if (opt.verbose >= 3)
  {
    dump_ports_list();
    dump_installed_packages();
  }
}

/**
 * Return the number of `CONTROL` nodes.
 */
unsigned vcpkg_get_num_CONTROLS (void)
{
  unsigned num_CONTROLS;

  vcpkg_init();

  num_CONTROLS = vcpkg_get_num (TRUE, FALSE);
  if (num_CONTROLS == 0)
     _strlcpy (last_err_str, "No CONTROL files for VCPKG found", sizeof(last_err_str));
  return (num_CONTROLS);
}

/**
 * Return the number of `JSON` nodes.
 */
unsigned vcpkg_get_num_JSON (void)
{
  unsigned num_JSON;

  vcpkg_init();

  num_JSON = vcpkg_get_num (FALSE, TRUE);
  if (num_JSON == 0)
     _strlcpy (last_err_str, "No JSON files for VCPKG found", sizeof(last_err_str));
  return (num_JSON);
}

/**
 * Return the number of `portfile.cmake` nodes.
 * \note Currently not used.
 */
unsigned vcpkg_get_num_portfile (void)
{
  unsigned num_portfiles;

  vcpkg_init();

  num_portfiles = vcpkg_get_num (FALSE, FALSE);
  if (num_portfiles == 0)
     _strlcpy (last_err_str, "No portfiles for VCPKG found", sizeof(last_err_str));
  return (num_portfiles);
}

/**
 * Return the number of installedt packages.
 */
unsigned vcpkg_get_num_installed (void)
{
  unsigned num_installed;

  vcpkg_init();

  num_installed = installed_packages ? smartlist_len (installed_packages) : 0;
  TRACE (2, "Found %u `installed` directories.\n", num_installed);
  return (num_installed);
}

/**
 * Construct an relative sub-directory name based on platform.
 * \eg. platform: `VCPKG_plat_x86 | VCPKG_plat_WINDOWS` returns `"x86-windows"`.
 *
 * Do not care about LINUX, OSX, ANDROID etc.
 */
static const char *get_platform_name (const VCPKG_plat_list p)
{
  static char ret [_MAX_PATH];
  const char *cpu, *os, *Static;

  if (is_x86_supported(p))
     cpu = "x86";
  else if (is_x64_supported(p))
     cpu = "x64";
  else
     return (NULL);

  if (is_windows_supported(p))
     os = "windows";
  else if (is_uwp_supported(p))
     os = "uwp";
  else
     return (NULL);

  if (is_static_supported(p))
       Static = "-static";
  else Static = "";

  snprintf (ret, sizeof(ret), "%s-%s%s", cpu, os, Static);
  return (ret);
}

/**
 * Get the `package->platforms` name.
 * Return TRUE is the "not bit" is not set.
 */
static BOOL get_depend_name (const VCPKG_plat_list p_list, const char **name)
{
  unsigned val = p_list[0];

  if (val == VCPKG_plat_ALL)
       *name = "all";
  else *name = flags_decode (val & ~1, platforms, DIM(platforms));
  return ((val & 1) != 1);
}

/**
 * Construct an absolute directory-name for an installed package.
 *
 * \eg
 *  \li package->package:      "sqlite3"
 *  \li package->platforms[0]: `VCPKG_plat_x86`
 *  \li package->platforms[1]: `VCPKG_plat_WINDOWS`
 *  \li returns:               `"<vcpkg_root>\\installed\\x86-windows"`
 *                             (or `"<vcpkg_root>\\installed\\x86-windows"`)
 */
static const char *get_installed_dir (const vcpkg_package *package)
{
  static char dir [_MAX_PATH];

  if (package)
  {
    snprintf (dir, sizeof(dir), "%s\\installed\\%s", vcpkg_root, package->arch);
    TRACE (2, "platform_name: '%s', dir: '%s'\n", get_platform_name(package->platforms), dir);
  }
  else
    snprintf (dir, sizeof(dir), "%s\\installed", vcpkg_root);

  if (!is_directory(dir))
  {
    snprintf (last_err_str, sizeof(last_err_str), "No status directory '%s'", dir);
    return (NULL);
  }

  if (opt.show_unix_paths)
     slashify2 (dir, dir, '/');
  return (dir);
}

/**
 * Construct an relative directory-name for an built package.
 *
 * \eg
 *  \li package->package:        "sqlite3"
 *  \li package->link->version:  "3.24.0-1"
 *  \li package->platforms[0]:   `VCPKG_plat_x86`
 *  \li package->platforms[1]:   `VCPKG_plat_WINDOWS`
 *  \li returns:                 "packages\\sqlite3_x86-windows"
 */
static const char *get_packages_dir (const vcpkg_package *package)
{
  static char dir [_MAX_PATH];

  snprintf (dir, sizeof(dir), "%s\\packages\\%s_%s",
            vcpkg_root, package->package, package->arch);

  TRACE (2, "architecture: '%s', dir: '%s'\n", package->arch, dir);

  if (!is_directory(dir))
  {
    snprintf (last_err_str, sizeof(last_err_str), "No such directory '%s'", dir);
    return (NULL);
  }
  return (dir + strlen(vcpkg_root) + 1);
}

/**
 * For a `package`, print the information obtained from `get_installed_info()`.
 *
 * Print files in these directories:
 *  \li the `bin` directory
 *  \li the `lib` directory.
 *  \li the `include` directory
 *  \li the `share` directory
 *
 * If `opt.show_size == TRUE`, print the total file-size of a package (ignoring
 * files that are not under the above directories).
 */
static void print_package_info (vcpkg_package *package, FMT_buf *fmt_buf, int indent)
{
  char path [_MAX_PATH];
  int  i, max = smartlist_len (package->install_info);

  for (i = 0; i < max; i++)
  {
    buf_printf (fmt_buf, "%*s%s\n", i > 0 ? indent : 0, "",
                (const char*)smartlist_get (package->install_info, i));
  }

  if (opt.show_size)
     buf_printf (fmt_buf, "%*s~3%s~0", indent, "", get_package_files_size(package));

  if (max == 0)
  {
    char slash = (opt.show_unix_paths ? '/' : '\\');

    snprintf (path, sizeof(path), "%s\\installed\\%s\\", vcpkg_root, package->arch);
    buf_printf (fmt_buf, "%*sNo entries for package `%s` under\n%*s%s.",
                indent, "", package->package, indent, "", slashify2(path, path, slash));
  }
  buf_putc (fmt_buf, '\n');
}

/**
 * Print a brief list of installed packages.
 *
 * Only print the package description similar to `pkg_config_list_installed()`.
 */
static void print_package_brief (const vcpkg_package *package, FMT_buf *fmt_buf, int indent)
{
  const port_node *node;
  int   i = 0;

  if (get_control_node(&i, &node, package->package))
       buf_puts_long_line (fmt_buf, node->description ? node->description : "<none>", indent);
  else buf_printf (fmt_buf, "No node (%s)\n", package->arch);
}

/**
 * Parser for a single `*.list` file for a specific package.
 *
 * Extract lines looking like these:
 * \code
 *  x86-windows-static/include/
 *  x86-windows-static/include/zconf.h
 *  x86-windows-static/include/zlib.h
 *  x86-windows-static/lib/
 *  x86-windows-static/lib/pkgconfig/
 *  x86-windows-static/lib/pkgconfig/zlib.pc
 *  x86-windows-static/lib/zlib.lib
 * \endcode
 *
 * Add wanted `char *` elements to this smartlist as we parse the file.
 */
static const char *wanted_arch;

static void info_parse (smartlist_t *sl, char *buf)
{
  char *q, *p = buf;

  str_strip_nl (buf);
  q = strchr (p, '\0') - 1;

  if (!str_startswith(buf, wanted_arch) ||  /* Does not match "x86-windows-static" */
      *q == '/')                            /* Ignore directory lines like "x86-windows-static/" */
     return;

  q = p + strlen (wanted_arch);

  /* Add only files matching "x86-windows-static/bin", "x86-windows-static/lib" and "x86-windows-static/include"
   */
  if (str_startswith(q, "/bin") || str_startswith(q, "/lib") || str_startswith(q, "/include"))
  {
    if (!opt.show_unix_paths)
       p = slashify2 (p, p, '\\');
    smartlist_add (sl, STRDUP(p));
    TRACE (2, "adding: '%s'.\n", p);
  }
}

/**
 * Open and parse a `*.list` file to get the `bin`, `lib` and `include` files
 * for an installed `package->package`.
 *
 * \eg.
 *   For `zlib:x86-windows` with version `1.2.11-6`, open and parse the file
 *   `<vcpkg_root>/installed/vcpkg/info/zlib_1.2.11-6_x86-windows.list`.
 *
 * Return TRUE if `*.list` file exists and the length of the `package->install_info`
 * list is greater than zero.
 */
static BOOL get_installed_info (vcpkg_package *package)
{
  if (!package->install_info)
  {
    char file [_MAX_PATH];

    snprintf (file, sizeof(file), "%s\\installed\\vcpkg\\info\\%s_%s_%s.list",
              vcpkg_root, package->package, package->version, package->arch);

    if (!FILE_EXISTS(file))
    {
      TRACE (2, "Package '%s' is not installed; no '%s'.\n", package->package, file);
      return (FALSE);
    }

    wanted_arch = package->arch;
    TRACE (1, "Getting package information from '%s' (arch: %s).\n", file, wanted_arch);

    package->install_info = smartlist_read_file (file, (smartlist_parse_func)info_parse);
    make_package_platform (package, package->arch, 0, TRUE);
  }
  return (package->install_info && smartlist_len(package->install_info) > 0);
}

/**
 * Print a list of installed packages.
 *
 * \note Only called from `show_version()` in envtool.c.
 */
unsigned vcpkg_list_installed (BOOL detailed)
{
  const char    *only = "";
  const char    *dir;
  unsigned       i, indent, num_ignored, max = 0;
  FMT_buf        fmt_buf;
  vcpkg_package *package;

  vcpkg_init();

  fmt_buf.buffer = NULL;

  if (installed_packages)
  {
    BUF_INIT (&fmt_buf, BUF_INIT_SIZE, 1);
    max = smartlist_len (installed_packages);
  }

  if (opt.only_32bit)
     only = ". These are for x86";
  else if (opt.only_64bit)
     only = ". These are for x64";

  for (i = num_ignored = 0; i < max; i++)
  {
    package = smartlist_get (installed_packages, i);

    if (opt.only_32bit && !(package->platforms[0] & VCPKG_plat_x86))
    {
      num_ignored++;
      continue;
    }
    if (opt.only_64bit && !(package->platforms[0] & VCPKG_plat_x64))
    {
      num_ignored++;
      continue;
    }

    if (!get_installed_dir(package))
    {
      TRACE (1, "%d: Failed 'get_installed_dir()' for '%s': %s\n", i, package->package, last_err_str);
      num_ignored++;
      continue;
    }

    if (!package->install_info)
    {
      TRACE (1, "%d: No install_info for '%s'; arch: '%s'\n", i, package->package, package->arch);
      num_ignored++;
      continue;
    }

    indent = buf_printf (&fmt_buf, "    %-25s", package->package);

    if (detailed)
         print_package_info (package, &fmt_buf, indent);
    else print_package_brief (package, &fmt_buf, indent);
  }

  if (max - num_ignored == 0)  /* all ignored */
     only = "";

  dir = get_installed_dir (NULL);
  if (dir)
       C_printf ("\n  Found %u installed ~3VCPKG~0 packages under ~3%s~0%s:\n", max, dir, only);
  else C_printf ("\n  Found 0 installed ~3VCPKG~0 packages.\n");

  if (max)
     C_puts (fmt_buf.buffer_start);

  if (fmt_buf.buffer)
     BUF_FREE (&fmt_buf);

  return (max - num_ignored);
}

/**
 * Free the memory allocated for `ports_list`.
 */
static void free_ports_list (void)
{
  int i, max = ports_list ? smartlist_len (ports_list) : 0;

  for (i = 0; i < max; i++)
  {
    port_node *node = smartlist_get (ports_list, i);

    smartlist_free_all (node->depends);
    smartlist_free_all (node->features);
    smartlist_free (node->supports);
    FREE (node->description);
    FREE (node);
  }
  smartlist_free (ports_list);
  ports_list = NULL;
}

/**
 * Free the memory allocated for smartlists, `vcpkg_*` variables
 * and and regex buffer.
 */
void vcpkg_exit (void)
{
  vcpkg_package *package;
  int   i, max;

  if (opt.use_cache && vcpkg_root)
  {
    cache_putf (SECTION_VCPKG, "vcpkg_root = %s", vcpkg_root);
    put_available_packages_to_cache();
    put_installed_packages_to_cache();
    put_ports_list_to_cache();
  }

  max = installed_packages ? smartlist_len (installed_packages) : 0;
  for (i = 0; i < max; i++)
  {
    package = smartlist_get (installed_packages, i);
    free_package (package, TRUE);
  }

  max = available_packages ? smartlist_len (available_packages) : 0;
  for (i = 0; i < max; i++)
  {
    package = smartlist_get (available_packages, i);
    free_package (package, TRUE);
  }
  smartlist_free (available_packages);
  smartlist_free (installed_packages);

  installed_packages = available_packages = NULL;

  free_ports_list();
  regex_free();

  FREE (vcpkg_exe);
  FREE (vcpkg_root);
}

/**
 * Get the index at or above `index` that matches `package_spec` in the `ports_list`.
 * Modify `*index_p` on output to the next index to check.
 */
static BOOL get_control_node (int *index_p, const port_node **node_p, const char *package_spec)
{
  int i, index, max = ports_list ? smartlist_len (ports_list) : 0;

  *node_p = NULL;
  index   = *index_p;
  for (i = index; i < max; i++)
  {
    const port_node *node = smartlist_get (ports_list, i);

    if ((node->have_CONTROL || node->have_JSON) &&
        fnmatch(package_spec, node->package, fnmatch_case(0)) == FNM_MATCH)
    {
      TRACE (2, "index=%d, i=%d, package: %s\n", index, i, node->package);
      *node_p  = node;
      *index_p = i + 1;
      return (TRUE);
    }
  }
  return (FALSE);
}

/**
 * Print a "installed: YES" if `package_name` is found in `installed_packages`. \n
 * Print a "installed: NO" otherwise.
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
static BOOL print_install_info (FMT_buf *fmt_buf, const char *package_name, int indent1)
{
  const char    *dir, *yes_no, *cpu = NULL;
  unsigned       num_installed = vcpkg_get_num_installed();
  unsigned       num_ignored = 0;
  int            found, index = 0;
  vcpkg_package *package = NULL;

  if (num_installed == 0 || (package = find_installed_package(&index, package_name, NULL)) == NULL)
       yes_no = C_BR_RED   "NO\n";
  else yes_no = C_BR_GREEN "YES: ";

  buf_printf (fmt_buf, "  %-*s%s~0", indent1, "installed:", yes_no);

  if (vcpkg_get_only_installed() && !package)
  {
    buf_putc (fmt_buf, '\n');
    return (FALSE);
  }

  if (opt.only_32bit)
     cpu = "x86";
  else if (opt.only_64bit)
     cpu = "x64";

  for (found = 0; package; package = find_installed_package(&index, package_name, NULL))
  {
    if (opt.only_32bit && !(package->platforms[0] & VCPKG_plat_x86))
    {
      num_ignored++;
      continue;
    }
    if (opt.only_64bit && !(package->platforms[0] & VCPKG_plat_x64))
    {
      num_ignored++;
      continue;
    }

    if (found > 0)
       buf_printf (fmt_buf, "  %*s%s~0", indent1, "", yes_no);

    if (package->install_info)
       buf_printf (fmt_buf, "%s, %u files", package->arch, smartlist_len(package->install_info));

    if (opt.show_size)
       buf_printf (fmt_buf, " %s", get_package_files_size(package));
    buf_putc (fmt_buf, '\n');

    dir = get_installed_dir (package);
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

/**
 * Search in `available_packages` for a matching `pkg_name`.
 * Return a pointer to it or NULL.
 */
static void *find_available_package (const char *pkg_name)
{
  int i, max = available_packages ? smartlist_len (available_packages) : 0;

  for (i = 0; i < max; i++)
  {
    vcpkg_package *package = smartlist_get (available_packages, i);

    if (!strcmp(pkg_name, package->package))
       return (package);
  }
  return (NULL);
}

/**
 * Return a `vcpkg_package*` structure for an installed package matching `pkg_name`.
 * There can be 2 (or more) packages with the same name but for different architectures.
 *
 * \eg. `package->platforms[]` contains `VCPKG_plat_WINDOWS` and `VCPKG_plat_x86` or
 *      `package->platforms[]` contains `VCPKG_plat_UWP` and `VCPKG_plat_x64` etc.
 *
 * \param[in,out]   index_p   A pointer to a value used as the first and next index to search from.
 *                            This value is set on exit from this function.
 * \param[in]       pkg_name  The package name to search for among the installed VCPKG packages.
 * \param[in]       arch      The architecture to check for if `pkg_name` matches. Optional.
 */
static void *find_installed_package (int *index_p, const char *pkg_name, const char *arch)
{
  vcpkg_package *package;
  int   i, max = smartlist_len (installed_packages);

  for (i = *index_p; i < max; i++)
  {
    package = smartlist_get (installed_packages, i);
    if (!strcmp(package->package, pkg_name))
    {
      if (arch && strcmp(package->arch, arch))
         continue;

      *index_p = i + 1;
      return (package);
    }
  }
  return (NULL);
}

/**
 * Search the global `available_packages` for a matching `package->package`.
 * If found return a pointer to it.
 *
 * If not found, create a new `vcpkg_package` entry and add to the
 * `available_packages` list. And then return a pointer to it.
 *
 * This is to save memory; no need to call `CALLOC()` for every `port_node::deps`
 * entry in the list. Hence many `port_node::deps` entries will have a pointer to
 * the same location.
 */
_WUNUSED_FUNC_OFF()
static void *find_or_alloc_package_dependency (const vcpkg_package *package)
{
  vcpkg_package *package2 = find_available_package (package->package);

  if (!package2)
  {
    package2 = MALLOC (sizeof(*package2));
    *package2 = *package;
    smartlist_add (available_packages, package2);
  }
  return (package2);
}
_WUNUSED_FUNC_POP()

/**
 * Get the name of a `platform`.
 */
static int get_plat_value (VCPKG_platform platform, int idx, const char **name)
{
  static unsigned last_platform = UINT_MAX;
  unsigned val = (platform & ~1);
  int      rc  = (platform & 1) ? 0 : 1;

  if (platform == VCPKG_plat_ALL)
       *name = "all";
  else *name = list_lookup_name (val, platforms, DIM(platforms));

  if (idx > 0 && (val == VCPKG_plat_ALL || last_platform == VCPKG_plat_ALL))
  {
    rc = -1;
    last_platform = UINT_MAX;
  }
  else
    last_platform = val;
  return (rc);
}

/**
 * Test if platform `platform` is in `p_list`.
 */
static BOOL is_plat_supported (const VCPKG_plat_list p_list, unsigned platform)
{
  int i;

  if (p_list[0] == VCPKG_plat_ALL)
     return (TRUE);

  for (i = 0; i < VCPKG_MAX_PLAT; i++)
      if (platform == p_list[i])
         return (TRUE);
  return (FALSE);
}

static BOOL is_x86_supported (const VCPKG_plat_list p_list)
{
  return !is_plat_supported (p_list, VCPKG_plat_x86 | 1);
}

static BOOL is_x64_supported (const VCPKG_plat_list p_list)
{
  return !is_plat_supported (p_list, VCPKG_plat_x64 | 1);
}

static BOOL is_windows_supported (const VCPKG_plat_list p_list)
{
  return !is_plat_supported (p_list, VCPKG_plat_WINDOWS | 1);
}

static BOOL is_uwp_supported (const VCPKG_plat_list p_list)
{
  return !is_plat_supported (p_list, VCPKG_plat_UWP | 1);
}

static BOOL is_static_supported (const VCPKG_plat_list p_list)
{
  return !is_plat_supported (p_list, VCPKG_plat_STATIC | 1);
}

/**
 * Add all package dependencies for this `*node`.
 * This function MUST only be used after the `available_packages` list is ready.
 */
static void json_add_dependencies (port_node *node, const char *buf, const JSON_tok_t *token)
{
  const JSON_tok_t *token2;
  int   i;

  for (i = 0;; i++)
  {
    token2 = JSON_get_token_by_index (token, JSON_ARRAY, i);
    if (!token2)
       break;

    if (token2->type == JSON_STRING)
    {
      const char   *str = buf + token2->start;
      int           len = token2->end - token2->start;  /* do not add +1 since the string should be quoted */

      TRACE (1, "%2d: dependency: '%.*s'\n", i, len, str);
      smartlist_add (node->depends, str_ndup(str, len));
    }
  }
}

/**
 * Add all package features for this `*node`.
 */
static void json_add_features (port_node *node, char *buf, const JSON_tok_t *token)
{
  const JSON_tok_t *token2;
  int   i;

  for (i = 0;; i++)
  {
    token2 = JSON_get_token_by_index (token, JSON_OBJECT, i);
    if (!token2)
       break;

    if (token2->type == JSON_STRING)
    {
      const char *str = buf + token2->start;
      int         len = token2->end - token2->start;  /* do not add +1 since the string should be quoted */

      TRACE (1, "%2d: feature: '%.*s'\n", i, len, str);
      smartlist_add (node->features, str_ndup(str, len));
    }
  }
}

/**
 * A package description in a `vcpkg.json` file is normally a simple `JSON_STRING`.
 * But in the case of long descriptions, it can be split up in a `JSON_ARRAY`.
 * In the latter case we use a fixed `merger*` smartlist to convert it into a char string.
 */
static void json_add_description (port_node *node, char *buf, const JSON_tok_t *token)
{
  char  *str;
  size_t len;
  int    i;

  if (node->description)
     return;

  if (token->type == JSON_ARRAY)
  {
    smartlist_t *merger = smartlist_new();

    for (i = 0; i < token->size; i++)
    {
      const JSON_tok_t *token2 = token + i + 1;

      str = buf + token2->start;
      len = token2->end - token2->start;  /* do not use +1 since the string should be quoted */

      smartlist_add (merger, str_ndup(str, len));
      str_replace2 ('~', "~~", str, sizeof(node->description));
      TRACE (2, "  descr[%d]: '%.*s'\n", i, len, str);
    }
    node->description = smartlist_join_str (merger, NULL);
    smartlist_free_all (merger);
  }
  else
  {
    str = buf + token->start;
    len = token->end - token->start;
    node->description = str_unquote (str_ndup(str, len));
    str_replace2 ('~', "~~", node->description, sizeof(node->description));
  }
  TRACE (1, "description: '%s'\n", node->description);
}

/*
 * Split a string like "(x64 | arm64) & (linux | osx | windows)" into tokens
 * and set the `VCPKG_plat_list[]` value for them.
 */
static BOOL json_make_supports (port_node *node, const char *buf, int i, BOOL recurse)
{
  static unsigned Not = 0;
  BOOL     next_and = FALSE;
  BOOL     next_or  = FALSE;
  unsigned val;
  char    *platform  = strdupa (buf);
  char    *platform0 = strdupa (buf); /* For TRACE() */

#if 0
  char or_group [1000];

  if (sscanf(platform, "(%100[^)])", or_group) == 1)
  {
    TRACE (1, "or_group: '%s'\n", or_group);
    if (json_make_supports (node, or_group, i, recurse))
       ++i;
    strcpy (platform, or_group + strlen(or_group));
  }
#endif

  if (*platform == '!')
  {
    platform++;
    Not = 1;
  }

  val = list_lookup_value (platform, platforms, DIM(platforms));
  if (val == UINT_MAX && recurse)
  {
    char *tok_end, *tok = _strtok_r (platform, "&| ", &tok_end);

    while (tok)
    {
      TRACE (1, "i: %d, tok: '%s', tok_end: '%s'\n", i, tok, tok_end);

      if (*tok_end == '&')
         next_and = TRUE;
      else if (*tok_end == '|')
         next_or = TRUE;

      if (json_make_supports (node, tok, i, *tok_end ? TRUE : FALSE))
         ++i;
      tok = _strtok_r (NULL, "&| ", &tok_end);
    }
  }

  if (val != UINT_MAX)
  {
    TRACE (1, "platform: '%s', platforms[%d]: 0x%04X, Not: %d, recurse: %d\n",
            platform0, i, val, Not, recurse);
    node->platforms [i] = val | Not;
    Not = 0;
    return (TRUE);
  }

  TRACE (1, "platform: '%s', platforms[%d]: 0x%04X, Not: %d, recurse: %d\n",
          platform0, i, node->platforms[i], Not, recurse);
  return (FALSE);
}

/**
 * Call the `JSON_parse()` on a string read from file.
 *
 * \note This function can be called recursively.
 */
int json_parse_buf (port_node *node, const char *file, char *buf, size_t buf_len)
{
  JSON_parser p;
  JSON_tok_t  t[200];   /* We expect no more tokens than this per OBJECT */
  size_t      len;
  int         i, j, rc;
  char       *str, *str_copy;

  if (opt.debug >= 1)
     C_putc ('\n');
  TRACE (1, "Parsing '%s'\n", file);

  JSON_init (&p);
  rc = JSON_parse (&p, buf, buf_len, t, DIM(t));
  if (rc < 0)
  {
    TRACE (1, "Failed to parse '%s': %d/%s\n", file, rc, JSON_strerror(rc));
    return (0);
  }

  if (rc < 1 || t[0].type != JSON_OBJECT)
  {
    TRACE (1, "Failed to parse '%s': JSON_OBJECT expected\n", file);
    return (0);
  }

  /* Loop over all keys of the root object;
   * The `key` is at `t[i].start` and it's value is at `t[i+1].start`.
   */
  for (i = 1; i < rc; i++)
  {
    if (JSON_str_eq(&t[i], buf, "name"))
    {
      str = buf + t[i+1].start;
      len = t[i+1].end - t[i+1].start + 1;
      len = min (len, sizeof(node->package));
      if (!node->package[0])
          _strlcpy (node->package, str, len);
      TRACE (1, "package:     '%s'\n", node->package);
      i += t[i+1].size + 1;
    }
    else if (JSON_str_eq(&t[i], buf, "version-string"))
    {
      str = buf + t[i+1].start;
      len = t[i+1].end - t[i+1].start + 1;
      len = min (len, sizeof(node->version));
      _strlcpy (node->version, str, len);
      str_replace2 ('~', "~~", node->version, sizeof(node->version));
      TRACE (1, "version:     '%s'\n", node->version);
      i += t[i+1].size + 1;
    }
    else if (JSON_str_eq(&t[i], buf, "description"))
    {
      json_add_description (node, buf, &t[i+1]);
      i += t[i+1].size + 1;
    }
    else if (JSON_str_eq(&t[i], buf, "homepage"))
    {
      str = buf + t[i+1].start;
      len = t[i+1].end - t[i+1].start + 1;
      len = min (len, sizeof(node->homepage));
      _strlcpy (node->homepage, str, len);
      str_replace2 ('~', "~~", node->homepage, sizeof(node->homepage));
      TRACE (1, "homepage:    '%s'\n", node->homepage);
      i += t[i+1].size + 1;
    }
    else if (JSON_str_eq(&t[i], buf, "supports"))
    {
      if (smartlist_getu(node->supports, 0) == VCPKG_plat_ALL)
         smartlist_del (node->supports, 0);

#if !defined(JSON_TEST) || 1
      str = buf + t[i+1].start;
      len = t[i+1].end - t[i+1].start;     /* Do not add +1 since the value is quoted */

      TRACE (1, "supports:    '%.*s'\n", len, str);
      str_copy = str_ndup (str, len);
      json_make_supports (node, str_copy, 0, TRUE);
      FREE (str_copy);
      for (j = 0; node->platforms[j] != VCPKG_plat_ALL; j++)
          smartlist_addu (node->supports, node->platforms[j]);

#else
      static const char *test_string[] = {
                        "x64 & windows & !static",
                        "windows & !static",
                        "!arm & !uwp",
                        "windows & !arm & !uwp & !static"
                      };
      for (j = 0; j < DIM(test_string); j++)
      {
        int k;

        C_putc ('\n');
        TRACE (1, "test_string: '%s'\n", test_string[j]);
        json_make_supports (node, test_string[j], 0, TRUE);

        for (k = 0; node->platforms[k] != VCPKG_plat_ALL; k++)
           smartlist_addu (node->supports, node->platforms[k]);
      }
#endif
      i += t[i+1].size + 1;
    }
    else if (JSON_str_eq(&t[i], buf, "dependencies"))
    {
      json_add_dependencies (node, buf, &t[i+1]);
      i += t[i+1].size + 1;
    }
    else if (JSON_str_eq(&t[i], buf, "features"))
    {
      json_add_features (node, buf, &t[i+1]);
      i += t[i+1].size + 1;
      break;                /* We're finished since "features" is always last */
    }
    else
    {
      str = buf + t[i].start;
      len = t[i].end - t[i].start;
      TRACE (2, "Unhandled key/value (type %s, size: %u): '%.*s'\n", JSON_typestr(t[i].type), t[i].size, len, str);
      i += t[i+1].size + 1;
    }
  }
  return (rc);
}

static int json_parse_file (port_node *node, const char *file)
{
  struct stat st;
  char   *f_mem = NULL;
  size_t  f_size, f_read;
  FILE   *f;
  int     r = 0;
  DWORD   win_err;

  memset (&st, '\0', sizeof(st));
  if (safe_stat(file, &st, &win_err) || st.st_size == 0)
  {
    TRACE (1, "Failed to get the file-size of %s. win_err: %lu\n", file, win_err);
    return (0);
  }

  f = fopen (file, "rb");
  if (!f)
  {
    TRACE (1, "Failed to open %s.\n", file);
    return (0);
  }

  f_size = (size_t) st.st_size;
  f_mem = MALLOC (f_size + 1);
  f_read = fread (f_mem, 1, f_size, f);
  fclose (f);
  if (f_read != f_size)
  {
    TRACE (1, "Failed to read the whole file %s. Only %u bytes, errno: %d (%s)\n",
            file, (unsigned)f_read, errno, strerror(errno));
    return (0);
  }

  f_mem [f_read] = '\0';
  r = json_parse_buf (node, file, f_mem, f_read);
  FREE (f_mem);
  return (r);
}

static void json_node_dump (const port_node *node)
{
  int i, len, len0, save, width, max;

  C_puts ("\n~6dumping node:~0\n");
  C_printf ("~3  name:~0         %s\n", node->package);
  C_printf ("~3  version:~0      %s\n", node->version);
  C_printf ("~3  homepage:~0     %s\n", node->homepage);

  //-------------------------------------------------------------------------

  len0 = C_puts ("~3  description:~0  ") - 2;
  save = C_setraw (1);

  if (node->description)
       C_puts_long_line (node->description, len0);
  else C_puts ("<none>\n");
  C_setraw (save);

  //-------------------------------------------------------------------------

  C_puts ("~3  features:~0     ");
  max = smartlist_len (node->features);
  for (i = 0; i < max; i++)
      C_printf ("%s%s", (const char*)smartlist_get(node->features, i), i < max-1 ? ", " : "");
  if (i == 0)
     C_puts ("<none>");
  C_putc ('\n');

  //-------------------------------------------------------------------------

  len0 = C_puts ("~3  supports:~0     ") - 2;
  max = smartlist_len (node->supports);
  for (i = 0; i < max; i++)
  {
    const char *name;
    unsigned    val = smartlist_getu (node->supports, i);
    int         supported = get_plat_value (val, i, &name);

    if (i > 0)
       C_printf ("%*s", len0, "");
    if (supported >= 0)
       C_printf ("0x%04X: %s%s\n", val, supported ? "" : "!", name);
  }
  if (i == 0)
     C_puts ("<none>\n");

  //-------------------------------------------------------------------------

  width = C_screen_width();
  len0 = C_puts ("~3  dependencies:~0 ") - 2;
  len = len0;
  max = smartlist_len (node->depends);
  for (i = 0; i < max; i++)
  {
    const char *pkg_name = smartlist_get (node->depends, i);

    len += C_printf ("%s", pkg_name);
    if (i < max-1)  /* Check if the next package name fits on this line */
    {
      const char *next_pkg = smartlist_get (node->depends, i+1);
      size_t      next_len = strlen (next_pkg);

      len += C_puts (", ");
      if (len + (int)next_len >= width)
         len = C_printf ("\n%*s", len0, "");
    }
  }
  if (i == 0)
     C_puts ("<none>");
  C_putc ('\n');
}

/*
 * Called from test.c if 'opt.do_vcpkg > 0'.
 */
int test_vcpkg_json_parser (void)
{
  port_node node = { "" };

  available_packages = smartlist_new();
  node.depends = smartlist_new();
  node.features = smartlist_new();
  node.supports = smartlist_new();
  smartlist_addu (node.supports, VCPKG_plat_ALL);

  json_parse_file (&node, "test.json");
  json_node_dump (&node);

  smartlist_free_all (node.depends);
  FREE (node.description);
  smartlist_free_all (node.features);
  smartlist_free (node.supports);
  smartlist_free_all (available_packages);
  return (0);
}

