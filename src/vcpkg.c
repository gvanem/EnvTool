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
 *      the descriptions of a package follows this.
 *
 * \def CONTROL_SOURCE
 *      the source-name is the name of a package following this.
 *
 * \def CONTROL_VERSION
 *      the version-info of a package follows this.
 *
 * \def CONTROL_BUILD_DEPENDS
 *      the list of packages this package depends on.
 */
#define CONTROL_DESCRIPTION   "Description:"
#define CONTROL_FEATURE       "Feature:"
#define CONTROL_SOURCE        "Source:"
#define CONTROL_VERSION       "Version:"
#define CONTROL_BUILD_DEPENDS "Build-Depends:"

/**
 * The list of `CONTROL` and `portfile.cmake` file entries.
 * A smartlist of `struct vcpkg_node`.
 */
static smartlist_t *vcpkg_nodes;

/**
 * A list of installable packages found in `CONTROL` files.
 * A smartlist of `struct vcpkg_depend`.
 */
static smartlist_t *vcpkg_packages;

/**
 * A list of installed packages found under `<vcpkg_root>/packages/<package>-<platform>`.
 */
static smartlist_t *vcpkg_installed_packages;

/* A list of "already found" sub-packages.
 */
static smartlist_t *sub_package_list;

/**
 * Save nodes relative to this directory to save memory.
 */
static char *vcpkg_root;

/**
 * Save last error-text here.
 */
static char last_err_str [200];

/**
 * The recursion-level for sub-dependency checking.
 */
static int sub_level = 0;

/**
 * Print details on installed packages only.
 */
BOOL vcpkg_only_installed = TRUE;

/**
 * The platforms we support when parsing in `make_dep_platform()`.
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

unsigned vcpkg_dump_control_internal (FMT_buf *fmt_buf, const char *spec);

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

/*
 * Try to match 'str' against the regular expression in 'pattern'.
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

/*
 * Manage a list of already found packages visited in `print_sub_dependencies()`.
 * So they are not recursed twice.
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
 * Print the package sub-dependencies for a `CONTROL` node.
 */
static void print_sub_dependencies (FMT_buf *fmt_buf, const struct vcpkg_node *node, int indent)
{
  const struct vcpkg_depend *dep1, *dep2;
  int   i, i_max, j, j_max, found;

  if (!node->deps || smartlist_len(node->deps) == 0)
  {
    if (sub_level == 0)
       buf_printf (fmt_buf, "%-*sNothing\n", indent, "");
    return;
  }

  i_max = smartlist_len (node->deps);
  j_max = smartlist_len (vcpkg_packages);

  for (i = found = 0; i < i_max; i++)
  {
    dep1 = smartlist_get (node->deps, i);

    for (j = 0; j < j_max; j++)
    {
      dep2 = smartlist_get (vcpkg_packages, j);
      if (dep1->package == dep2->package && !sub_package_found(dep1->package))
      {
        found++;

        /* Add to "already found list"
         */
        smartlist_add (sub_package_list, (void*)dep1->package);

        /* Will call vcpkg_get_control() only once
         */
        ++sub_level;
        vcpkg_dump_control_internal (fmt_buf, dep1->package);
        --sub_level;
      }
    }
  }
  if (found == 0 && sub_level == 0)
     buf_puts (fmt_buf, "None found\n");
}

/**
 * Print the package top-dependencies for a `CONTROL` node.
 * Return the number of depencencies at top.
 */
static int print_top_dependencies (FMT_buf *fmt_buf, const struct vcpkg_node *node, int indent)
{
  const struct vcpkg_depend *d;
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
      buf_puts (fmt_buf, "Nothing\n");
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
    vcpkg_get_dep_platform (d, &Not);

    if (sub_level > 0)
       buf_printf (fmt_buf, "%-*s%s;\n", indent + 2*sub_level, "", d->package);
    else
    {
      if (dep > 0)
         buf_printf (fmt_buf, "%-*s", indent+2, "");

      buf_printf (fmt_buf, "%-*s  platform: ", (int)longest_package, d->package);
      if (Not)
           buf_printf (fmt_buf, "!(%s)", vcpkg_get_dep_name(d));
      else buf_printf (fmt_buf, "%s", vcpkg_get_dep_name(d));
      buf_printf (fmt_buf, " (0x%04X)\n", d->platform);
    }
  }
  return (max_dep);
}


/**
 * Get the `deps->platform` name(s) disregarding
 * the VCPKG_platform_INVERSE bit.
 */
const char *vcpkg_get_dep_name (const struct vcpkg_depend *dep)
{
  unsigned val = dep->platform;

  if (val == VCPKG_plat_ALL)
     return ("all");
  val &= ~VCPKG_platform_INVERSE;
  return flags_decode (val, platforms, DIM(platforms));
}

/**
 * Get the `deps->platform` and any inverse of it.
 */
int vcpkg_get_dep_platform (const struct vcpkg_depend *dep, BOOL *Not)
{
  unsigned val = dep->platform;

  if (Not)
     *Not = FALSE;

  if (val == VCPKG_plat_ALL)
     return (VCPKG_plat_ALL);

  if (val & VCPKG_platform_INVERSE) /* Sign bit set */
  {
    if (Not)
       *Not = TRUE;
    val &= ~VCPKG_platform_INVERSE;
  }
  return (val);
}

/**
 * Split a line like "!uwp&!windows" and (on the first call recursively)
 * fill `dep` list for it.
 */
static void make_dep_platform (struct vcpkg_depend *dep, char *platform, BOOL recurse)
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
    char *tok_end, *tok = strtok_s (platform, "&", &tok_end);

    while (tok)
    {
      make_dep_platform (dep, tok, FALSE);
      tok = strtok_s (NULL, "&", &tok_end);
    }
  }
}

/**
 * Split a line like "x86-windows" and (on the first call recursively)
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
    char *tok_end, *tok = strtok_s (platform, "-", &tok_end);

    while (tok)
    {
      ret |= make_package_platform (tok, FALSE);
      tok = strtok_s (NULL, "-", &tok_end);
    }
  }
  return (ret);
}

/**
 * Search the global `vcpkg_packages` for a matching `dep->package`.
 * If found return a pointer to it.
 *
 * If not found, create a new `struct vcpkg_depend` entry and add to the
 * `vcpkg_packages` list. And then return a pointer to it.
 *
 * This is to save memory; no need to call `CALLOC()` for every `vcpkg_node::deps`
 * entry in the list. Hence many `vcpkg_node::deps` entries will have pointer to
 * the same location.
 */
static void *find_or_alloc_dependency (const struct vcpkg_depend *dep1)
{
  struct vcpkg_depend *dep2;
  int    i, max = smartlist_len (vcpkg_packages);

  for (i = 0; i < max; i++)
  {
    dep2 = smartlist_get (vcpkg_packages, i);
    if (!memcmp(dep2, dep1, sizeof(*dep2)))
       return (dep2);
  }
  dep2 = CALLOC (sizeof(*dep2), 1);
  memcpy (dep2, dep1, sizeof(*dep2));
  return smartlist_add (vcpkg_packages, dep2);
}

/**
 * Split a line like:
 *   "openssl (!uwp&!windows), curl (!uwp&!windows)"
 *
 * first into tokens of:
 *   "openssl (!uwp&!windows)" and "curl (!uwp&!windows)".
 *
 * If a token contains a "(xx)" part, pass that to make_dep_platform()
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

  ASSERT (node->deps == NULL);
  node->deps = smartlist_new();

  tok = strtok_s (str, ",", &tok_end);

  while (tok)
  {
    struct vcpkg_depend dep;
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
      make_dep_platform (&dep, platform, TRUE);
    }

    _strlcpy (dep.package, p, sizeof(dep.package));
    smartlist_add (node->deps, find_or_alloc_dependency(&dep));

    tok = strtok_s (NULL, ",", &tok_end);
  }
}

static void make_features (struct vcpkg_node *node, char *str)
{
  if (!node->features)
      node->features = smartlist_new();
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
     return;

  while (fgets(buf,sizeof(buf)-1,f))
  {
    strip_nl (buf);
    p = str_ltrim (buf);

    DEBUGF (4, "p: '%s'\n", p);

    if (!node->description && !strnicmp(p,CONTROL_DESCRIPTION,sizeof(CONTROL_DESCRIPTION)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_DESCRIPTION) - 1);
      node->description = STRDUP (p);
    }
    else if (!node->package[0] && !strnicmp(p,CONTROL_SOURCE,sizeof(CONTROL_SOURCE)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_SOURCE) - 1);
      _strlcpy (node->package, p, sizeof(node->package));
    }
    else if (!node->version[0] && !strnicmp(p,CONTROL_VERSION,sizeof(CONTROL_VERSION)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_VERSION) - 1);
      _strlcpy (node->version, p, sizeof(node->version));
      str_replace ('~', '-', node->version);
    }
    else if (!strnicmp(p,CONTROL_FEATURE,sizeof(CONTROL_FEATURE)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_FEATURE) - 1);
      make_features (node, p);
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
 * Parse `file` for LOCAL package location or REMOTE package URL
 */
static void portfile_cmake_parse (struct vcpkg_node *node, const char *file)
{
  ARGSUSED (node);
  ARGSUSED (file);
}

/**
 * Traverse a `dir` looking for sub-directories (1 level deep only).
 *
 * \return a smartlist of directories to check further in
 *         `vcpkg_get_list()` or `vcpkg_get_num_installed()`.
 */
static smartlist_t *get_dir_list (const char *dir)
{
  struct dirent2 *de;
  smartlist_t    *dir_list;
  DIR2           *dp = opendir2x (dir, NULL);

  if (!dp)
     return (NULL); /* Should never happen */

  dir_list = smartlist_new();

  while ((de = readdir2(dp)) != NULL)
  {
    if (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY)
    {
      DEBUGF (2, "Adding to dir_list: '%s'\n", de->d_name);
      smartlist_add (dir_list, STRDUP(de->d_name));
    }
  }
  closedir2 (dp);
  return (dir_list);
}

static void build_vcpkg_nodes (const char *dir)
{
  struct vcpkg_node *node;
  char   file [_MAX_PATH];

  snprintf (file, sizeof(file), "%s\\CONTROL", dir);
  if (FILE_EXISTS(file))
  {
    node = CALLOC (sizeof(*node), 1);
    node->have_CONTROL = TRUE;
    CONTROL_parse (node, file);
    smartlist_add (vcpkg_nodes, node);
  }

  snprintf (file, sizeof(file), "%s\\portfile.cmake", dir);
  if (FILE_EXISTS(file))
  {
    node = CALLOC (sizeof(*node), 1);
    _strlcpy (node->package, basename(dir), sizeof(node->package));
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
 * Try to set the `vcpkg_root` based on `%VCPKG_ROOT%`.
 */
static BOOL get_base_env (void)
{
  const char *env = getenv ("VCPKG_ROOT");

  if (!env)
  {
    _strlcpy (last_err_str, "Env-var ~5VCPKG_ROOT~0 not defined", sizeof(last_err_str));
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

  if (!exe)
  {
    _strlcpy (last_err_str, "vcpkg.exe not on PATH.\n", sizeof(last_err_str));
    return (FALSE);
  }
  vcpkg_root = dirname (exe);
  return (TRUE);
}

/**
 * Try to set the `vcpkg_root` location. Either based on:
 *  \li an existing directory `%VCPKG_ROOT` or
 *  \li `dirname (searchpath("vcpkg.exe"))`.
 */
static BOOL get_basedir (void)
{
  if (!get_base_env() && !get_base_exe())
     return (FALSE);

  if (!is_directory(vcpkg_root))
  {
    snprintf (last_err_str, sizeof(last_err_str),
              "~6%s~0 points to a non-existing directory", vcpkg_root);
    return (FALSE);
  }
  return (TRUE);
}

/**
 * Return a `vcpkg_package*` structure for an installed package matching `package`.
 * There can be 2 (or more) packages with the same name but for different bitness.
 * E.g. `pkg->platform == VCPKG_plat_WINDOWS | VCPKG_plat_x86` and
 *      `pkg->platform == VCPKG_plat_UWP | VCPKG_plat_x64` etc.
 */
static const struct vcpkg_package *get_install_info (int *index_p, const char *package)
{
  const struct vcpkg_package *pkg;
  int   i, max = smartlist_len (vcpkg_installed_packages);

  for (i = *index_p; i < max; i++)
  {
    pkg = smartlist_get (vcpkg_installed_packages, i);
    if (!strcmp(package, pkg->package))
    {
      *index_p = i + 1;
      return (pkg);
    }
  }
  return (NULL);
}

/**
 * Dump the parsed information from all `vcpkg_nodes`.
 */
static void nodes_debug_dump (void)
{
  int i, i_max = smartlist_len (vcpkg_nodes);
  int j, j_max, num, indent;
  int width = (int) C_screen_width();

  /* stdout is redicted
   */
  if (width <= 0)
     width = INT_MAX;

  /* Print a header.
   */
  i = C_printf ("%4s %-*s%-*s  %s\n",
                "Num", VCPKG_MAX_NAME, "Package", VCPKG_MAX_NAME,
                "Dependants", "Platforms");
  C_puts (str_repeat('=', i+4));
  C_putc ('\n');

  for (i = num = 0; i < i_max; i++)
  {
    const struct vcpkg_node   *node = smartlist_get (vcpkg_nodes, i);
    const struct vcpkg_depend *dep;
    const char                *feature;

    if (!node->have_CONTROL)
       continue;

    indent = C_printf ("%4d %-*s", ++num, VCPKG_MAX_NAME, node->package);

    j_max = node->deps ? smartlist_len (node->deps) : 0;
    for (j = 0; j < j_max; j++)
    {
      BOOL Not;

      dep = smartlist_get (node->deps, j);
      C_printf ("%-*s  ", VCPKG_MAX_NAME, dep->package);

      vcpkg_get_dep_platform (dep, &Not);
      if (Not)
           C_printf ("!(%s)\n", vcpkg_get_dep_name(dep));
      else C_printf ("%s\n", vcpkg_get_dep_name(dep));

      if (j < j_max-1)
         C_printf ("%-*s", indent, "");
    }
    if (j == 0)
       C_puts("<none>\n");

    if (node->features)
    {
      int k, k_max = smartlist_len (node->features);
      int len = C_puts ("     Features: ");

      indent = len;
      for (k = 0; k < k_max; k++)
      {
        feature = smartlist_get (node->features, k);
        if (len + (int)strlen(feature) + 2 >= width)
           len = C_printf ("\n%-*s", indent, "") - 1;
        len += C_printf ("%s%s", feature, (k < k_max-1) ? ", " : "\n");
      }
    }
  }
}

/**
 * Traverse the smartlist `vcpkg_nodes` and
 * return the number of nodes where `node->have_CONTROL == have_CONTROL`.
 */
unsigned vcpkg_get_num (BOOL have_CONTROL)
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
 * Return the number of `CONTROL` nodes.
 */
unsigned vcpkg_get_num_CONTROLS (void)
{
  unsigned num = vcpkg_get_num (TRUE);

  if (num == 0)
     _strlcpy (last_err_str, "No CONTROL files for VCPKG found.", sizeof(last_err_str));
  return (num);
}

/**
 * Return the number of `portfile.cmake` nodes.
 */
unsigned vcpkg_get_num_portfile (void)
{
  unsigned num = vcpkg_get_num (FALSE);

  if (num == 0)
     _strlcpy (last_err_str, "No portfiles for VCPKG found.", sizeof(last_err_str));
  return (num);
}

/**
 * Build the list of installed packages; `vcpkg_installed_packages`.
 */
static void build_vcpkg_installed_packages (void)
{
  smartlist_t *dirs;
  char packages_dir [_MAX_PATH];
  char installed_dir [_MAX_PATH];
  int  i, max;

  snprintf (packages_dir, sizeof(packages_dir), "%s\\packages", vcpkg_root);
  snprintf (installed_dir, sizeof(installed_dir), "%s\\installed", vcpkg_root);

  if (!is_directory(packages_dir))
  {
    snprintf (last_err_str, sizeof(last_err_str), "Directory ~6%s~0 does not exist", packages_dir);
    return;
  }

  if (!is_directory(installed_dir))
  {
    snprintf (last_err_str, sizeof(last_err_str), "Directory ~6%s~0 does not exist", installed_dir);
    return;
  }

  dirs = get_dir_list (packages_dir);
  if (!dirs)
  {
    snprintf (last_err_str, sizeof(last_err_str), "Found no packages in ~6%s~0", packages_dir);
    return;
  }

  ASSERT (vcpkg_installed_packages == NULL);
  vcpkg_installed_packages = smartlist_new();

  max = smartlist_len (dirs);

  for (i = 0; i < max; i++)
  {
    struct vcpkg_package *node;
    char  *p, *q;

    /**
     * If e.g. `dirs` contains "<vcpkg_root>\packages\sqlite3_x86-windows", add a node
     * with this to the `vcpkg_installed_packages` smartlist:
     *   `node->package  = "sqlite3"`
     *   `node->platform = VCPKG_plat_x86 | VCPKG_plat_WINDOWS`.
     *   `node->link`    = a pointer into `vcpkg_nodes` for more detailed info.
     */
    p = smartlist_get (dirs, i);
    p += strlen (packages_dir);
    ASSERT (*p == '\\');
    q = strchr (++p, '_');
    if (q && q - p < sizeof(node->package))
    {
      int j = 0;

      node = CALLOC (sizeof(*node), 1);
      _strlcpy (node->package, p, q - p + 1);
      node->platform = make_package_platform (q+1, TRUE);
      vcpkg_get_control(&j, (const struct vcpkg_node**)&node->link, node->package);
      smartlist_add (vcpkg_installed_packages, node);

      DEBUGF (2, "package: '%s', platform: 0x%04X (%s).\n",
              node->package, node->platform,
              flags_decode(node->platform, platforms, DIM(platforms)));
    }
  }
  smartlist_free_all (dirs);
}

/**
 * Return the number of installed packages.
 */
unsigned vcpkg_get_num_installed (void)
{
  static BOOL done = FALSE;
  unsigned len = 0;

  if (!done)
     build_vcpkg_installed_packages();

  len = smartlist_len (vcpkg_installed_packages);
  DEBUGF (2, "Found %u `ports` directories.\n", len);
  done = TRUE;
  return (len);
}

/**
 * Construct an relative sub-directory name based on platform.
 * E.g. platform: VCPKG_plat_x86 | VCPKG_plat_WINDOWS
 *      returns:  "x86-windows".
 *
 * We do not care about LINUX, OSX, ANDROID etc.
 */
static const char *get_platform_name (const struct vcpkg_package *pkg)
{
  static char ret [_MAX_PATH];

  if (pkg->platform & VCPKG_plat_x64)
       strcpy (ret, "x64-");
  else strcpy (ret, "x86-");

  if (pkg->platform & VCPKG_plat_WINDOWS)
     strcat (ret, "windows");
  else if (pkg->platform & VCPKG_plat_UWP)
     strcat (ret, "uwp");

  return (ret);
}

/**
 * Construct an absolute file-name to the .list-file based on package, version and platform.
 * E.g. pkg->package:        "sqlite3"
 *      pkg->link->version:  "3.24.0-1"
 *      pkg->platform:       VCPKG_plat_x86 | VCPKG_plat_WINDOWS
 *      returns:             "<vcpkg_root>\\installed\\vcpkg\\info\\sqlite3_3.24.0-1_x86-windows.list"
 */
static const char *get_info_file (const struct vcpkg_package *pkg)
{
  static char ret [_MAX_PATH];

  snprintf (ret, sizeof(ret), "%s\\installed\\vcpkg\\info\\%s_%s_%s.list",
            vcpkg_root, pkg->package, pkg->link->version, get_platform_name(pkg));
  if (opt.show_unix_paths)
     slashify2 (ret, ret, '/');
  return (ret);
}

/**
 * Parser for a file returned from `get_info_file()`.
 *
 * Ignore the lines ending in '/'.
 * But add all lines that looks like files and add to the given smartlist.
 */
static void parse_info_file (smartlist_t *sl, char *line)
{
  char  file [_MAX_PATH];
  char *slash = strrchr (strip_nl(line), '/');

  if (slash && slash[1] == '\0')
     return;

  snprintf (file, sizeof(file), "%s\\installed\\%s", vcpkg_root, line);
  smartlist_add (sl, STRDUP(file));
  DEBUGF (1, "file: '%s'\n", file);
}

/**
 * For an `info_file` returned by `get_info_file()`, return the full directories to:
 *  \li the `bin` directory
 *  \li the `include` directory
 *  \li the `lib` directory.
 *
 * This is similar to what the command `vcpkg owns <package>.lib` does.
 */
static void
print_verbose_pkg_details (FMT_buf *fmt_buf, const struct vcpkg_package *pkg, const char *file, int indent)
{
  smartlist_t *parts = smartlist_read_file (file, (smartlist_parse_func)parse_info_file);
  char         slash = (opt.show_unix_paths) ? '/' : '\\';
  int          i, max = parts ? smartlist_len (parts) : 0;
  unsigned     h_files = 0;
  unsigned     hpp_files = 0;
  unsigned     bin_files = 0;
  unsigned     lib_files = 0;
  unsigned     cmake_files = 0;
  unsigned     other_files = 0;

  for (i = 0; i < max; i++)
  {
    struct stat st;
    char   file2 [_MAX_PATH];
    char   fsize [80];
    char  *part = smartlist_get (parts, i);
    char  *fname = strrchr (part, '/');

    if (!fname)
       continue;

    if (fname[1] == '\0')
    {
      other_files++;
      continue;
    }

    *fname++ = '\0';
    file2[0] = '\0';
    fsize[0] = '\0';
    DEBUGF (1, "fname: '%s'\n", fname);

    if (str_endswith(part, "/bin"))
    {
      snprintf (file2, sizeof(file2), "%s\\%s", part, fname);
      bin_files++;
    }
    else if (str_endswith(part, "/lib"))
    {
      snprintf (file2, sizeof(file2), "%s\\%s", part, fname);
      lib_files++;
    }
    else if (str_endswith(fname,".cmake"))
    {
      snprintf (file2, sizeof(file2), "%s\\%s", part, fname);
      cmake_files++;
    }
    else if (str_endswith(fname,".h"))
       h_files++;
    else if (str_endswith(fname,".hpp"))
       hpp_files++;
    else if (!stricmp(fname,"usage"))
       other_files++;

    if (file2[0])
    {
      if (opt.show_size && safe_stat(file2,&st,NULL) == 0)
         snprintf (fsize, sizeof(fsize), "%s %s: ",
                   get_time_str(st.st_mtime),
                   get_file_size_str(st.st_size));

      buf_printf (fmt_buf, "%*s%s%s%s\n",
                  indent, "", fsize, "%VCPKG_ROOT%",
                  slashify(file2, slash) + strlen(vcpkg_root)); /* Replace the leading part with `%VCPKG_ROOT%` */
    }
  }

  if (h_files + hpp_files + cmake_files + bin_files + lib_files + other_files == 0)
     buf_printf (fmt_buf, "%*sNo parts for package `%s` in\n%*s%s.\n",
                 indent, "", pkg->package, indent, "", file);

  else if (bin_files + lib_files == 0)
  {
    if (h_files > 0)
       buf_printf (fmt_buf, "%*sOnly %d .h-files for package `%s` in\n%*s%s.\n",
                 indent, "", h_files, pkg->package, indent, "", file);

     if (hpp_files > 0)
        buf_printf (fmt_buf, "%*sOnly %d .hpp-files for package `%s` in\n%*s%s.\n",
                    indent, "", hpp_files, pkg->package, indent, "", file);
  }

  if (parts)
     smartlist_free_all (parts);
}

/**
 * Print a list of installed packages.
 *
 * \todo
 *   Use the `get_info_file()` file to list the `PATH`, `LIB` and `INCLUDE` env-variables
 *   needed to use the `bin`, `lib` and `include` files.
 */
unsigned vcpkg_list_installed (void)
{
  const char *only = "";
  unsigned    i, num = vcpkg_get_num_installed();

  if (opt.only_32bit)
     only = ", x86-only";
  if (opt.only_64bit)
     only = ", x64-only";

  C_printf ("\n  %u installed ~3VCPKG~0 packages (~3%%VCPKG_ROOT%%~0=~6%s~0%s):\n",
            num, vcpkg_root, only);

 /* Should be the same as 'vcpkg_get_num_installed()'
  */
  num = smartlist_len (vcpkg_installed_packages);
  for (i = 0; i < num; i++)
  {
    const struct vcpkg_package *pkg = smartlist_get (vcpkg_installed_packages, i);

    if (opt.only_32bit && !(pkg->platform & VCPKG_plat_x86))
       continue;

    if (opt.only_64bit && !(pkg->platform & VCPKG_plat_x64))
       continue;

    C_printf ("    %-*s %s%s\n", VCPKG_MAX_NAME, pkg->package, "%VCPKG_ROOT%",
              get_info_file(pkg) + strlen(vcpkg_root));
  }
  return (num);
}

/**
 * Build the smartlist `vcpkg_nodes`.
 *
 * \retval The number of all node types.
 */
unsigned vcpkg_get_list (void)
{
  smartlist_t *dirs;
  unsigned     len;
  int          i, max;
  char         ports_dir [_MAX_PATH];

  if (!get_basedir())
     return (0);

  snprintf (ports_dir, sizeof(ports_dir), "%s\\ports", vcpkg_root);
  if (!is_directory(ports_dir))
  {
    snprintf (last_err_str, sizeof(last_err_str),
              "Directory ~6%s~0 does not exist", ports_dir);
    return (0);
  }

  ASSERT (vcpkg_nodes == NULL);
  ASSERT (vcpkg_packages == NULL);

  vcpkg_nodes    = smartlist_new();
  vcpkg_packages = smartlist_new();

  dirs = get_dir_list (ports_dir);
  if (dirs)
  {
    max = smartlist_len (dirs);
    DEBUGF (2, "Found %d VCPKG port directories.\n", max);
    for (i = 0; i < max; i++)
        build_vcpkg_nodes (smartlist_get(dirs,i));
    smartlist_free_all (dirs);
  }

  len = smartlist_len (vcpkg_nodes);
  if (len == 0)
  {
    _strlcpy (last_err_str, "No ~5VCPKG~0 packages found", sizeof(last_err_str));
    vcpkg_free();
  }
  else
  {
    if (opt.verbose >= 3)
       nodes_debug_dump();
  }
  return (len);
}

/**
 * Free the memory allocated for `vcpkg_installed_packages`.
 */
static void free_installed_packages (void)
{
  smartlist_free_all (vcpkg_installed_packages);
  vcpkg_installed_packages = NULL;
}

/**
 * Free the memory allocated for `vcpkg_packages`.
 */
static void free_packages (void)
{
  smartlist_free_all (vcpkg_packages);
  vcpkg_packages = NULL;
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

    if (node->deps)
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
void vcpkg_free (void)
{
  free_installed_packages();
  free_packages();
  free_nodes();
  regex_free();
  FREE (vcpkg_root);
}

/**
 * Get the index at or above `index` that matches `package_spec`.
 * Modify `*index_p` on output to the next index to check.
 */
BOOL vcpkg_get_control (int *index_p, const struct vcpkg_node **node_p, const char *package_spec)
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
      DEBUGF (2, "i=%2d, index=%2d, package: %s\n", i, index, node->package);
      *node_p  = node;
      *index_p = i + 1;
      return (TRUE);
    }
  }
  return (FALSE);
}

/*
 * Print a "installed: YES" or "installed: NO" depending on whether package is found in
 * `vcpkg_installed_packages`.
 *
 * \todo
 *   Also print all supported platforms (x86, x64 etc.) and locations for bin, lib and headers.
 *   Print that similar to how `pkg-config` does it:
 *   `pkg-config --libs --cflags --msvc-syntax gr-wxgui`:
 *     -If:/gv/dx-radio/gnuradio-GNCdevinclude
 *     -libpath:f:/gv/dx-radio/gnuradio-GNCdevlib gnuradio-runtime.lib gnuradio-pmt.lib
 */
static BOOL
print_install_info (FMT_buf *fmt_buf, const char *package, int indent1)
{
  const struct vcpkg_package *pkg = NULL;
  const char                 *yes_no, *cpu = NULL;
  unsigned                    num = vcpkg_get_num_installed();
  int                         found, i = 0;

  if (num == 0 || (pkg = get_install_info(&i,package)) == NULL)
       yes_no = C_BR_RED "NO\n";
  else yes_no = C_BR_GREEN "YES: ";

  buf_printf (fmt_buf, "  %-*s%s~0", indent1, "installed:", yes_no);

  if (vcpkg_only_installed && !pkg)
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
       continue;

    if (opt.only_64bit && !(pkg->platform & VCPKG_plat_x64))
       continue;

    if (found > 0)
       buf_printf (fmt_buf, "  %*s%s~0", indent1, "", yes_no);
    buf_printf (fmt_buf, "%s%c\n", get_platform_name(pkg), opt.verbose >= 1 ? ':' : ' ');
    found++;

    if (opt.verbose >= 1)
    {
      const char *file = get_info_file (pkg);

      print_verbose_pkg_details (fmt_buf, pkg, file, indent1+2);
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
 * Something like this, with `envtool --vcpkg -v 3f*' showing `sub_level == 2`:
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
 *         boost-vcpkg-helpers: Nothing
 *       boost-array
 *         boost-assert
 *         boost-config
 *         boost-core
 *         boost-detail
 *         boost-static-assert
 *         boost-throw-exception
 *         boost-vcpkg-helpers: Nothing
 *       boost-assert
 *         boost-config
 *         boost-vcpkg-helpers: Nothing
 *       boost-atomic
 *         boost-assert
 *         boost-build
 *         boost-config
 *         boost-integer
 *         boost-modular-build-helper
 *         boost-type-traits
 *         boost-vcpkg-helpers: Nothing
 *       boost-config
 *         boost-vcpkg-helpers: Nothing
 *       boost-core
 *       boost-integer
 *       boost-mpl
 *       boost-parameter
 *       boost-predef
 *       boost-static-assert
 *       boost-tuple
 *       boost-type-traits
 *       boost-utility
 *       boost-vcpkg-helpers: Nothing
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
 *       boost-vcpkg-helpers: Nothing
 *
 *     poco:
 *       zlib
 *       pcre
 *       sqlite3
 *       expat
 *
 *     sqlite3:   Nothing
 *     rapidxml:  Nothing
 *
 *  1 match found for "3f*" with 33 unique sub-dependants.
 * ```
 */
unsigned vcpkg_dump_control_internal (FMT_buf *fmt_buf, const char *package_spec)
{
  const struct vcpkg_node *node;
  int      i = 0,  indent, padding, num_deps;
  unsigned matches = 0;

  while (vcpkg_get_control(&i, &node, package_spec))
  {
    const char *package = node->package;

    matches++;
    padding = VCPKG_MAX_NAME - strlen (package);
    padding = max (0, padding-2);

    if (sub_level == 0)
    {
      indent = buf_printf (fmt_buf, "  ~6%s~0: %*s", package, padding, "") - 4;

      buf_puts_long_line (fmt_buf, node->description ? node->description : "<none>", indent);
      buf_printf (fmt_buf, "  %-*s%s\n", indent-2, "version:", node->version[0] ? node->version : "<none>");
    }
    else
    {
      indent = 2;
      buf_printf (fmt_buf, "%-*s%s:\n", indent + 2*sub_level, "", package);
    }

    num_deps = print_top_dependencies (fmt_buf, node, indent-2);

    if (num_deps > 1 && opt.verbose >= 2)
       print_sub_dependencies (fmt_buf, node, indent);

    if (sub_level == 0)
    {
      if (print_install_info (fmt_buf, package, indent-2))
      {
        C_puts (fmt_buf->buffer_start);
        buf_reset (fmt_buf);
      }
      else
        matches--;
    }
  }
  return (matches);
}

unsigned vcpkg_dump_control (const char *package_spec)
{
  FMT_buf  fmt_buf;
  unsigned num;

  sub_package_list = smartlist_new();

  BUF_INIT (&fmt_buf, 200000);

  C_printf ("Dumping CONTROL for packages matching ~6%s~0.\n", package_spec);

  sub_level = 0;
  num = vcpkg_dump_control_internal (&fmt_buf, package_spec);
  sub_level = 0;

  smartlist_free (sub_package_list);
  return (num);
}
