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
#include "dirlist.h"
#include "regex.h"
#include "vcpkg.h"

/**
 * `CONTROL` file keywords we look for:
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
 *      the list of packages this package depends on.
 */
#define CONTROL_DESCRIPTION   "Description:"
#define CONTROL_FEATURE       "Feature:"
#define CONTROL_HOMEPAGE      "Homepage:"
#define CONTROL_SOURCE        "Source:"
#define CONTROL_VERSION       "Version:"
#define CONTROL_BUILD_DEPENDS "Build-Depends:"

/**
 * The list of `CONTROL` and `portfile.cmake` file entries.
 * A smartlist of `struct vcpkg_node`.
 */
static smartlist_t *vcpkg_nodes;

/**
 * A list of available and installable packages found in `CONTROL` files
 * under `<vcpkg_root>/ports`.
 * A smartlist of `struct vcpkg_package`.
 */
static smartlist_t *available_packages;

/**
 * A list of installed packages found under `<vcpkg_root>/packages/<package>-<platform>`.
 */
static smartlist_t *installed_packages;

/* A list of "already found" sub-packages.
 */
static smartlist_t *sub_package_list;

/**
 * Save nodes relative to this directory to save memory.
 */
static char *vcpkg_root;

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

static BOOL        get_control_node (int *index_p, const struct vcpkg_node **node_p, const char *package_spec);
static const char *get_depend_name (const struct vcpkg_package *dep);
static unsigned    get_depend_platform (unsigned platform, BOOL *Not);

static int  print_top_dependencies (FMT_buf *fmt_buf, const struct vcpkg_node *node, int indent);
static int  print_sub_dependencies (FMT_buf *fmt_buf, const struct vcpkg_node *node, int indent);
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
 *   dependants:           boost-lockfree (windows), boost-regex (windows), poco (windows), sqlite3, rapidxml
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
  const struct vcpkg_node *node;
  int      i = 0,  indent, padding, num_deps;
  unsigned matches = 0;

  while (get_control_node(&i, &node, package_spec))
  {
    const char *package = node->package;

    matches++;
    padding = VCPKG_MAX_NAME - strlen (package);
    padding = max (0, padding-2);

    if (sub_level == 0)
    {
      indent = buf_printf (fmt_buf, "  ~6%s~0: %*s", package, padding, "") - 4;

      buf_puts_long_line (fmt_buf, node->description ? node->description : "<none>", indent);
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

  BUF_INIT (&fmt_buf, 200000);

  sub_level = 0;
  num = vcpkg_find_internal (&fmt_buf, package_spec);
  sub_level = 0;

  smartlist_free (sub_package_list);
  return (num);
}

/**
 * Print the package sub-dependencies for a `CONTROL` node.
 */
static int print_sub_dependencies (FMT_buf *fmt_buf, const struct vcpkg_node *node, int indent)
{
  const struct vcpkg_package *dep1, *dep2;
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

        /* Will call get_control_node() only once
         */
        ++sub_level;
        vcpkg_find_internal (fmt_buf, dep1->package);
        --sub_level;
      }
    }
  }
  if (found == 0 && sub_level == 0)
     buf_puts (fmt_buf, "None found\n");
  return (found);
}

/**
 * Print the package top-dependencies for a `CONTROL` node.
 * Return the number of dependencies at top.
 */
static int print_top_dependencies (FMT_buf *fmt_buf, const struct vcpkg_node *node, int indent)
{
  const struct vcpkg_package *d;
  size_t longest_package = 0;
  int    dep, max_dep;

  if (sub_level > 0)
  {
    if (!node->deps)
       return (0);
  }
  else
  {
    buf_printf (fmt_buf, "  %-*s", indent, "dependants:");
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
static void make_depend_platform (struct vcpkg_package *dep, char *platform, BOOL recurse)
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
 * Split a line like "curl_x86-windows" into cpu and OS and check if these are legal.
 */
static BOOL legal_package_name (const char *package)
{
  char *cpu, *copy = NULL;
  BOOL  cpu_ok;

  if (!package)
     goto fail;

  copy = STRDUP (package);
  cpu  = strchr (copy, '_');
  DEBUGF (2, "package: '%s', cpu: '%.4s'.\n", package, cpu ? cpu+1 : "<None>");

  if (!cpu)
     goto fail;

  cpu++;
  cpu_ok = (!strnicmp(cpu, "x86-", 4) || !strnicmp(cpu, "x64-", 4));
  if (!cpu_ok)
     goto fail;

  cpu += 4;
  if (list_lookup_value (cpu, platforms, DIM(platforms)) != UINT_MAX)
     goto okay;

fail:
  FREE (copy);
  return (FALSE);
okay:
  FREE (copy);
  return (TRUE);
}

/**
 * Search the global `available_packages` for a matching `dep->package`.
 * If found return a pointer to it.
 *
 * If not found, create a new `struct vcpkg_package` entry and add to the
 * `available_packages` list. And then return a pointer to it.
 *
 * This is to save memory; no need to call `CALLOC()` for every `vcpkg_node::deps`
 * entry in the list. Hence many `vcpkg_node::deps` entries will have pointer to
 * the same location.
 */
static void *find_or_alloc_dependency (const struct vcpkg_package *dep1)
{
  struct vcpkg_package *dep2;
  int    i, max = smartlist_len (available_packages);

  for (i = 0; i < max; i++)
  {
    dep2 = smartlist_get (available_packages, i);
    if (!memcmp(dep2, dep1, sizeof(*dep2)))
       return (dep2);
  }
  dep2 = CALLOC (sizeof(*dep2), 1);
  memcpy (dep2, dep1, sizeof(*dep2));
  return smartlist_add (available_packages, dep2);
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
 * Add a package-dependency  to `node` as long as there are more ","
 * tokens in `str` to parse.
 */
static void make_dependencies (struct vcpkg_node *node, char *str)
{
  char *tok, *tok_end, *p;

  if (strchr(str, ')') > strchr(str, '('))
     DEBUGF (2, "str: '%s'\n", str);

  if (str[0] == '\0' || (isspace(str[0]) && str[1] == '\0'))
  {
    DEBUGF (2, "Empty dependencies! str: '%s'\n", str);
    return;
  }

  ASSERT (node->deps == NULL);
  node->deps = smartlist_new();

  tok = _strtok_r (str, ",", &tok_end);

  while (tok)
  {
    struct vcpkg_package dep;
    char   package [2*VCPKG_MAX_NAME];
    char   platform [51];
    char  *l_paren;

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

    _strlcpy (dep.package, p, sizeof(dep.package));
    smartlist_add (node->deps, find_or_alloc_dependency(&dep));

    tok = _strtok_r (NULL, ",", &tok_end);
  }
}

static void make_features (struct vcpkg_node *node, const char *str)
{
  if (!node->features)
     node->features = smartlist_new();
  DEBUGF (3, "Adding feature: '%s'\n", str);
  smartlist_add (node->features, STRDUP(str));
}

/**
 * Parse the content of a `CONTROL` file and add it's contents to `node`.
 */
static void CONTROL_parse (struct vcpkg_node *node, const char *file)
{
  FILE *f = fopen (file, "r");
  char *p, buf [5000];

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

    if (!node->description && !strnicmp(p,CONTROL_DESCRIPTION,sizeof(CONTROL_DESCRIPTION)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_DESCRIPTION) - 1);
      node->description = STRDUP (p);
      str_replace2 ('~', "~~", node->description, sizeof(node->description));
    }
    else if (!node->version[0] && !strnicmp(p,CONTROL_VERSION,sizeof(CONTROL_VERSION)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_VERSION) - 1);
      _strlcpy (node->version, p, sizeof(node->version));
      str_replace2 ('~', "~~", node->version, sizeof(node->version));
    }
    else if (!strnicmp(p,CONTROL_FEATURE,sizeof(CONTROL_FEATURE)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_FEATURE) - 1);
      make_features (node, p);
    }
    else if (!strnicmp(p,CONTROL_HOMEPAGE,sizeof(CONTROL_HOMEPAGE)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_HOMEPAGE) - 1);
      _strlcpy (node->homepage, p, sizeof(node->homepage));

      /* In case 'node->homepage' contains a '~', replace with "~~".
       */
      str_replace2 ('~', "~~", node->homepage, sizeof(node->homepage));
    }
    else if (!node->deps && !strnicmp(p,CONTROL_BUILD_DEPENDS,sizeof(CONTROL_BUILD_DEPENDS)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_BUILD_DEPENDS) - 1);
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
static void portfile_cmake_parse (struct vcpkg_node *node, const char *file)
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
 * to `vcpkg_nodes`.
 */
static void build_vcpkg_nodes (const char *dir, int ports_index)
{
  struct vcpkg_node *node;
  char        file [_MAX_PATH];
  const char *pkg = dir + strlen ("ports\\");

  snprintf (file, sizeof(file), "%s\\ports\\%s\\CONTROL", vcpkg_root, pkg);
  if (FILE_EXISTS(file))
  {
    DEBUGF (2, "%d: Building node for %s.\n", ports_index, file);

    node = CALLOC (sizeof(*node), 1);
    node->have_CONTROL = TRUE;
    _strlcpy (node->package, pkg, sizeof(node->package));
    strcpy (node->homepage, "<none>");

    CONTROL_parse (node, file);
    smartlist_add (vcpkg_nodes, node);
  }

  snprintf (file, sizeof(file), "%s\\%s\\portfile.cmake", vcpkg_root, pkg);
  if (FILE_EXISTS(file))
  {
    DEBUGF (2, "%d: Building node for %s.\n", ports_index, file);

    node = CALLOC (sizeof(*node), 1);
    _strlcpy (node->package, pkg, sizeof(node->package));
    strcpy (node->homepage, "<none>");

    portfile_cmake_parse (node, file);
    smartlist_add (vcpkg_nodes, node);
  }
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
static const struct vcpkg_package *get_install_info (int *index_p, const char *package)
{
  const struct vcpkg_package *pkg;
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
 * Print the description for a `struct vcpkg_node *`.
 */
static void node_dump_description (const struct vcpkg_node *node)
{
  int len = C_puts ("     ~6description:~0 ") - 2;
  int save = C_setraw (1);

  if (node->description)
       C_puts_long_line (node->description, len);
  else C_puts ("<none>\n");
  C_setraw (save);
}

/**
 * Dump the dependencies for a `struct vcpkg_node *`.
 */
static void node_dump_deps (const struct vcpkg_node *node, size_t width)
{
  int i, max = node->deps ? smartlist_len (node->deps) : 0;
  int len, len0 = C_puts ("     ~6dependants:~0  ") - 2;

  len = len0;
  for (i = 0; i < max; i++)
  {
    const struct vcpkg_package *dep = smartlist_get (node->deps, i);
    const struct vcpkg_package *next_dep;
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
 * Dump the features for a `struct vcpkg_node *`.
 */
static void node_dump_features (const struct vcpkg_node *node, size_t width)
{
  int i, max = node->features ? smartlist_len (node->features) : 0;
  int len, len0 = C_puts ("     ~6features:~0    ") - 2;

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
 * Dump the parsed information from all `vcpkg_nodes`.
 */
static void dump_nodes (void)
{
  int    i, len, len0, num, max = smartlist_len (vcpkg_nodes);
  size_t width = C_screen_width();

  /* Print a header.
   */
  len0 = C_printf ("~6Num  ~2Package~0 / ~7Version               ");
  C_puts ("~3Homepage~0\n");
  C_puts (str_repeat('=', 100));
  C_putc ('\n');

  for (i = num = 0; i < max; i++)
  {
    const struct vcpkg_node *node = smartlist_get (vcpkg_nodes, i);
    const char              *yes_no = "NO";
    const char              *details = "";
    const char              *version = node->version;
    int                      zero = 0;

    if (!node->have_CONTROL)
       continue;

    if (*version == '\0' || *version == ' ')
       version = "<unknown>";

    len = C_printf ("~6%4d ~2%s~0 / ~7%s~3", ++num, node->package, version);
    C_printf ("%-*s%s~0\n", len-len0-1, "", node->homepage);

    node_dump_description (node);
    node_dump_deps (node, width);
    node_dump_features (node, width);

    if (get_install_info(&zero,node->package))
    {
//    details = get_packages_dir (pkg);
      yes_no = "YES";
    }
    C_printf ("     ~6installed:   %s%s~0\n\n", yes_no, details);
  }
}

/**
 * Traverse the smartlist `vcpkg_nodes` and
 * return the number of nodes where `node->have_CONTROL == have_CONTROL`.
 */
static unsigned vcpkg_get_num (BOOL have_CONTROL)
{
  int      i, max;
  unsigned num = 0;

  max = vcpkg_nodes ? smartlist_len (vcpkg_nodes) : 0;

  for (i = 0; i < max; i++)
  {
    const struct vcpkg_node *node = smartlist_get (vcpkg_nodes, i);

    if (node->have_CONTROL == have_CONTROL)
       num++;
  }
  return (num);
}

/**
 * Build the smartlist `vcpkg_nodes *` representing all available VCPKG packages
 * (ignoring whether a package is installed or not).
 *
 * \param[in] dirs  The `smartlist_t*` of the directories to build the
 *                  `vcpkg_node*` list from.
 * \retval          The number of all node types.
 */
static unsigned vcpkg_get_all_available (const smartlist_t *dirs)
{
  unsigned i, num_vcpkg_nodes;

  if (dirs)
  {
    unsigned num = smartlist_len (dirs);

    DEBUGF (2, "Found %d VCPKG port directories.\n", num);
    for (i = 0; i < num; i++)
        build_vcpkg_nodes (smartlist_get(dirs,i), i);
  }

  num_vcpkg_nodes = smartlist_len (vcpkg_nodes);
  if (num_vcpkg_nodes == 0)
     _strlcpy (last_err_str, "No ~6VCPKG~0 packages found", sizeof(last_err_str));

  return (num_vcpkg_nodes);
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
  vcpkg_root = _fix_path (env, NULL);
  return (TRUE);
}

/**
 * Try to set the `vcpkg_root` based on directory of `vcpkg.exe`.
 */
static BOOL get_base_exe (void)
{
  const char *exe = searchpath ("vcpkg.exe", "PATH");
  char       *dir;

  if (!exe)
  {
    _strlcpy (last_err_str, "vcpkg.exe not found on PATH", sizeof(last_err_str));
    return (FALSE);
  }

  dir = dirname (exe);

  /* Returns a fully qualified directory name in case `cwd == dir`.
   */
  vcpkg_root = _fix_path (dir, NULL);
  FREE (dir);
  return (TRUE);
}

/**
 * Initialise VCPKG globals once and build the list of installed packages `installed_packages`.
 */
void vcpkg_init (void)
{
  smartlist_t *ports_dirs;
  smartlist_t *packages_dirs;
  int          i, max = 0;
  static       BOOL done = FALSE;

  if (done)
     return;

  done = TRUE;

  /**
   * Try to set the `vcpkg_root` location. Based either on:
   *  \li  - an existing directory `%VCPKG_ROOT%` or
   *  \li  - The directory name of `searchpath("vcpkg.exe")`.
   */
  if (!get_base_env() && !get_base_exe())
     return;

  ASSERT (installed_packages == NULL);
  ASSERT (available_packages == NULL);
  ASSERT (vcpkg_nodes        == NULL);

  installed_packages = smartlist_new();
  available_packages = smartlist_new();
  vcpkg_nodes        = smartlist_new();

  last_err_str[0] = '\0';   /* clear any error-string set */

  ports_dirs    = build_dir_list ("ports");
  packages_dirs = build_dir_list ("packages");

  vcpkg_get_all_available (ports_dirs);

  if (ports_dirs)
     smartlist_free_all (ports_dirs);

  if (packages_dirs)
     max = smartlist_len (packages_dirs);

  /**
   * Loop over all our installed packages directory
   * `vcpkg_root>/packages/`
   *
   * and figure out which belongs to the
   * `vcpkg_root>/installed/`
   *
   * directory. Some may have been orpaned.
   */

  for (i = 0; i < max; i++)
  {
    struct vcpkg_package *node;
    char  *homepage, *p, *q;

    /**
     * If e.g. `dirs` contains "packages\\sqlite3_x86-windows", add a node
     * with this to the `installed_packages` smartlist:
     *   `node->package  = "sqlite3"`
     *   `node->platform = VCPKG_plat_x86 | VCPKG_plat_WINDOWS`.
     *   `node->link`    = a pointer into `vcpkg_nodes` for more detailed info.
     */
    p = (char*) smartlist_get (packages_dirs, i) + strlen ("packages\\");
    q = strchr (p+1, '_');
    if (q && q - p < sizeof(node->package) && legal_package_name(p))
    {
      int j = 0;

      node = CALLOC (sizeof(*node), 1);
      _strlcpy (node->package, p, q - p + 1);
      node->platform = make_package_platform (q+1, TRUE);
      get_control_node (&j, (const struct vcpkg_node**)&node->link, node->package);
      smartlist_add (installed_packages, node);

      if (node->link)
           homepage = node->link->homepage;
      else homepage = "?";

      DEBUGF (2, "package: %-30s  homepage: %-40s  platform: 0x%04X (%s).\n",
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
     _strlcpy (last_err_str, "No CONTROL files for VCPKG found.", sizeof(last_err_str));
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
     _strlcpy (last_err_str, "No portfiles for VCPKG found.", sizeof(last_err_str));
  return (num_portfiles);
}

/**
 * Return the number of installed packages.
 */
unsigned vcpkg_get_num_installed (void)
{
  unsigned num_installed;

  vcpkg_init();

  num_installed = installed_packages ? smartlist_len (installed_packages) : 0;
  DEBUGF (2, "Found %u `packages` directories.\n", num_installed);
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
static const char *get_depend_name (const struct vcpkg_package *dep)
{
  unsigned val = dep->platform;

  if (val == VCPKG_plat_ALL)
     return ("all");
  val &= ~VCPKG_platform_INVERSE;
  return flags_decode (val, platforms, DIM(platforms));
}

/**
 * Construct an relative directory-name for an installed package.
 *
 * \eg
 *  \li pkg->package:        "sqlite3"
 *  \li pkg->link->version:  "3.24.0-1"
 *  \li pkg->platform:       VCPKG_plat_x86 | VCPKG_plat_WINDOWS
 *  \li returns:             "packages\\sqlite3_x86-windows"
 */
static const char *get_packages_dir (const struct vcpkg_package *pkg)
{
  static char dir [_MAX_PATH];

  if (!pkg->link)
     return (NULL);     /* something is seriously wrong */

  snprintf (dir, sizeof(dir), "%s\\packages\\%s_%s", vcpkg_root, pkg->package, get_platform_name(pkg->platform));
  if (!is_directory(dir))
  {
    snprintf (last_err_str, sizeof(last_err_str), "No such directory %s", dir);
    return (NULL);
  }
  return (dir + strlen(vcpkg_root) + 1);
}

/**
 * For an `dir` returned by `get_packages_dir()`, look for directories to:
 *  \li the `bin` directory
 *  \li the `lib` directory.
 *  \li the `include` directory
 */
static void print_package_details (FMT_buf *fmt_buf, const struct vcpkg_package *pkg,
                                   const char *package_dir, int indent)
{
  smartlist_t *dirs;
  char         slash = (opt.show_unix_paths) ? '/' : '\\';
  int          i, max;

  dirs = build_dir_list (package_dir);
  max = dirs ? smartlist_len (dirs) : 0;

  DEBUGF (1, "max %d, dirs: %p.\n", max, dirs);

  for (i = 0; i < max; i++)
  {
    const char  *dir    = smartlist_get (dirs, i);
    const char  *subdir = strrchr (dir, '\\');

    DEBUGF (1, "subdir %s.\n", subdir);
    if (!subdir)
       continue;

    subdir++;
    if (!stricmp(subdir,"bin") || !stricmp(subdir,"lib") || !stricmp(subdir,"include"))
    {
      char   file [_MAX_PATH];
      UINT64 dsize;

      buf_printf (fmt_buf, "%*s", indent, "");
      snprintf (file, sizeof(file), "%s\\%s", vcpkg_root, dir);
      if (opt.show_size)
      {
        dsize = get_directory_size (file);
        buf_printf (fmt_buf, "%s: ", get_file_size_str(dsize));
      }
      buf_printf (fmt_buf, "%s\n", slashify(file,slash));
    }
  }

  if (max == 0)
     buf_printf (fmt_buf, "%*sNo sub-dirs for package `%s` in\n%*s%s\\%s\\.\n",
                 indent, "", pkg->package, indent, "", vcpkg_root, package_dir);

  smartlist_free_all (dirs);
}

/**
 * Print a list of installed packages.
 *
 * \todo
 *   Use the `get_packages_dir()` file to list the `PATH`, `LIB` and `INCLUDE` env-variables
 *   needed to use the `bin`, `lib` and `include` files.
 */
unsigned vcpkg_list_installed (void)
{
  const char *only = "";
  const char *last = "";
  unsigned    i, num_installed, num_ignored;
  FMT_buf     fmt_buf;

  BUF_INIT (&fmt_buf, 200000);

  vcpkg_init();

  num_installed = vcpkg_get_num_installed();

  if (opt.only_32bit)
     only = ". These are for x86";
  else if (opt.only_64bit)
     only = ". These are for x64";

  num_installed = smartlist_len (installed_packages);

  for (i = num_ignored = 0; i < num_installed; i++)
  {
    const struct vcpkg_package *pkg = smartlist_get (installed_packages, i);

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
    buf_printf (&fmt_buf, "    %s\n", pkg->package);
    last = pkg->package;
  }

  C_printf ("\n  %u installed ~3VCPKG~0 packages%s:\n", num_installed - num_ignored, only);
  C_puts (fmt_buf.buffer_start);

  return (num_installed - num_ignored);
}

/**
 * Free the memory allocated for `vcpkg_nodes`.
 */
static void free_nodes (void)
{
  int i, max;

  if (!vcpkg_nodes)
     return;

  max = smartlist_len (vcpkg_nodes);
  for (i = 0; i < max; i++)
  {
    struct vcpkg_node *node = smartlist_get (vcpkg_nodes, i);

    smartlist_free (node->deps);
    smartlist_free_all (node->features);
    FREE (node->description);
    FREE (node);
  }
  smartlist_free (vcpkg_nodes);
  vcpkg_nodes = NULL;
}

/**
 * Free the memory allocated for smartlists and regex buffer.
 */
void vcpkg_exit (void)
{
  smartlist_free_all (installed_packages);
  smartlist_free_all (available_packages);

  installed_packages = available_packages = NULL;

  free_nodes();
  regex_free();
  FREE (vcpkg_root);
}

/**
 * Get the index at or above `index` that matches `package_spec`.
 * Modify `*index_p` on output to the next index to check.
 */
static BOOL get_control_node (int *index_p, const struct vcpkg_node **node_p, const char *package_spec)
{
  int i, index, max = vcpkg_nodes ? smartlist_len (vcpkg_nodes) : 0;

  *node_p = NULL;
  index   = *index_p;
  for (i = index; i < max; i++)
  {
    const struct vcpkg_node *node = smartlist_get (vcpkg_nodes, i);

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
static BOOL
print_install_info (FMT_buf *fmt_buf, const char *package, int indent1)
{
  const struct vcpkg_package *pkg = NULL;
  const char                 *dir, *yes_no, *cpu = NULL;
  unsigned                    num_installed = vcpkg_get_num_installed();
  unsigned                    num_ignored = 0;
  int                         found, i = 0;

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
    buf_printf (fmt_buf, "%s%c\n", get_platform_name(pkg->platform), opt.verbose >= 1 ? ':' : ' ');

    dir = get_packages_dir (pkg);
    if (dir)
    {
      found++;
      print_package_details (fmt_buf, pkg, dir, indent1+2);
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

