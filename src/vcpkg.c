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
 * The list of packages found in `CONTROL` files.
 * A smartlist of `struct vcpkg_depend`.
 */
static smartlist_t *vcpkg_packages;

/**
 * Save nodes releative to this directory to save memory.
 */
static char vcpkg_base_dir [_MAX_PATH];

/**
 * Save last error-text here.
 */
static char last_err_str [200];

/**
 * The recursion-level for sub-dependency checking.
 */
static int sub_level = 0;

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
                              { VCPKG_plat_x64,     "x64"     },
                            };
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

/**
 * Print the package sub-dependencies for a `CONTROL` node.
 */
static void print_sub_dependencies (const struct vcpkg_node *node, int indent)
{
  const struct vcpkg_depend *dep1, *dep2;
  int   i, i_max, j, j_max, found;

  if (!node->deps || smartlist_len(node->deps) == 0)
  {
    C_printf ("%-*sNothing\n", indent, "");
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
      if (dep1->package == dep2->package)
      {
        found++;

        /* Will call vcpkg_get_control() only once
         */
        vcpkg_dump_control (dep1->package);
      }
    }
  }
  if (found == 0)
     C_puts ("None found\n");
}

/**
 * An alternative way to print the sub-dependencies.
 */
static void print_sub_dependencies2 (const struct vcpkg_node *parent_node, int indent)
{
  const struct vcpkg_node   *node;
  const struct vcpkg_depend *dep1, *dep2;
  int   i = 0;
  int   j = 0, j_max;

  while (vcpkg_get_control(&i, &node, "*"))
  {
    if (stricmp(parent_node->package,node->package))
       continue;

    j_max = parent_node->deps ? smartlist_len (parent_node->deps) : 0;
    for (j = 1; j < j_max; j++)
    {
      dep1 = smartlist_get (parent_node->deps, j);
      if (stricmp(dep1->package,parent_node->package))
         continue;

      if (++sub_level <= 4)
      {
        C_printf ("    %2d: %-*s\n", sub_level, 2*sub_level + indent, dep1->package);
        print_sub_dependencies2 (node, indent+2);
      }
      sub_level--;
    }
    if (j_max < 1)
       C_printf ("    %2d: <no sub-deps>\n");
  }
}

/**
 * Print the package top-dependencies for a `CONTROL` node.
 */
static void print_top_dependencies (const struct vcpkg_node *node, int indent)
{
  const struct vcpkg_depend *d;
  size_t len, longest_package = 0;
  int    dep, max_dep;

  C_printf ("  %-*s", indent, "dependants:");
  if (!node->deps)
  {
    C_puts ("Nothing\n\n");
    return;
  }

  max_dep = smartlist_len (node->deps);

  /* First, get the value for 'longest_package'
   */
  for (dep = 0; dep < max_dep; dep++)
  {
    d   = smartlist_get (node->deps, dep);
    len = strlen (d->package);
    if (len > longest_package)
       longest_package = len;
  }

  for (dep = 0; dep < max_dep; dep++)
  {
    BOOL Not;
    int  p_val;

    d     = smartlist_get (node->deps, dep);
    p_val = vcpkg_get_dep_platform (d, &Not);

    if (dep > 0)
       C_printf ("  %-*s", indent, "");
    C_printf ("%-*s  platform: %s%s (0x%04X)\n",
              (int)longest_package, d->package,
              Not ? "not " : "",
              vcpkg_get_dep_name(d), d->platform);
  }
  C_putc ('\n');

  if (opt.verbose >= 1)
     print_sub_dependencies2 (node, indent);
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
 * Get the `deps->platform` and any inversion of it.
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
    /* Set the `deps->platform` or the inverse of it.
     */
    if (Not)
         dep->platform |= (VCPKG_platform_INVERSE | (VCPKG_platform)val);
    else dep->platform |= (VCPKG_platform)val;
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
 * Search the global vcpkg_packages for a matching `dep->package`.
 * If found return a pointer to it.
 *
 * If not found, create a new `struct vcpkg_depend` entry and add to the
 * vcpkg_packages list. And then return a pointer to it.
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
    }
    else if (!strnicmp(p,CONTROL_FEATURE,sizeof(CONTROL_FEATURE)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_FEATURE) - 1);
      make_features (node, p);
    }
    else if (!node->deps && !strnicmp(p,CONTROL_BUILD_DEPENDS,sizeof(CONTROL_BUILD_DEPENDS)-1))
    {
      p = str_ltrim (p + sizeof(CONTROL_BUILD_DEPENDS) - 1);
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
 * Traverse the `vcpkg_base_dir[]` directory looking for directories.
 * Returns a smartlist of directories to check for in `build_vcpkg_nodes()`.
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
 * Try to set the `vcpkg_base_dir[]` based on `%VCPKG_ROOT%`.
 */
static BOOL get_base_env (void)
{
  const char *env = getenv ("VCPKG_ROOT");
  char       *end;

  if (!env)
  {
    _strlcpy (last_err_str, "Env-var ~5VCPKG_ROOT~0 not defined", sizeof(last_err_str));
    return (FALSE);
  }

  end = strchr (env, '\0') - 1;
  if (IS_SLASH(*end))
       snprintf (vcpkg_base_dir, sizeof(vcpkg_base_dir), "%sports", env);
  else snprintf (vcpkg_base_dir, sizeof(vcpkg_base_dir), "%s\\ports", env);
  return (TRUE);
}

/**
 * Try to set the `vcpkg_base_dir[]` based on directory of `vcpkg.exe`.
 */
static BOOL get_base_exe (void)
{
  const char *exe = searchpath ("vcpkg.exe", "PATH");
  char       *dir;

  if (!exe)
  {
    _strlcpy (last_err_str, "vcpkg.exe not on %%PATH.\n", sizeof(last_err_str));
    return (FALSE);
  }
  dir = dirname (exe);
  snprintf (vcpkg_base_dir, sizeof(vcpkg_base_dir), "%s\\ports", dir);
  FREE (dir);
  return (TRUE);
}

/**
 * Try to set the `vcpkg_base_dir[]` location. Either based on:
 *  \li an existing directory `%VCPKG_ROOT\ports` or
 *  \li `dirname (searchpath("vcpkg.exe")) + "\\ports"`.
 */
static BOOL get_basedir (void)
{
  if (!get_base_env() && !get_base_exe())
     return (FALSE);

  if (!is_directory(vcpkg_base_dir))
  {
    snprintf (last_err_str, sizeof(last_err_str),
              "~6%s~0 points to a non-existing directory", vcpkg_base_dir);
    return (FALSE);
  }
  return (TRUE);
}

/**
 * Just dump the parsed information from all `vcpkg_nodes`.
 */
static void debug_dump (void)
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
  i = C_printf ("%4s %-*s%-*s %ss\n",
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
      int  p_val;

      dep = smartlist_get (node->deps, j);
      C_printf ("%-*s  ", VCPKG_MAX_NAME, dep->package);

      p_val = vcpkg_get_dep_platform (dep, &Not);
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
  return vcpkg_get_num (TRUE);
}

/**
 * Return the number of `portfile.cmake` nodes.
 */
unsigned vcpkg_get_num_portfile (void)
{
  return vcpkg_get_num (FALSE);
}

/**
 * Build the smartlist `vcpkg_nodes`.
 *
 * \retval The number of all node-types.
 */
unsigned vcpkg_get_list (void)
{
  smartlist_t *dirs;
  unsigned     len;
  int          i, max;

  if (!get_basedir())
     return (0);

  ASSERT (vcpkg_nodes == NULL);
  ASSERT (vcpkg_packages == NULL);

  vcpkg_nodes    = smartlist_new();
  vcpkg_packages = smartlist_new();

  dirs = get_dir_list (vcpkg_base_dir);
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
    if (opt.debug >= 1)
       debug_dump();
  }
  return (len);
}

/**
 * Free the memory allocated for `vcpkg_packages`.
 */
static void free_packages (void)
{
  if (vcpkg_packages)
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
    if (node->features)
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
  free_packages();
  free_nodes();
  regex_free();
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

    if (node->have_CONTROL && fnmatch(package_spec, node->package, FNM_FLAG_NOCASE) == FNM_MATCH)
    {
      DEBUGF (2, "i=%2d, index=%2d, package: %s\n", i, index, node->package);
      *node_p = node;
      *index_p = i + 1;
      return (TRUE);
    }
  }
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
unsigned vcpkg_dump_control (const char *package_spec)
{
  const struct vcpkg_node *node;
  int      i = 0,  old, indent, padding;
  unsigned matches = 0;

  C_printf ("Dumping CONTROL for packages matching ~6%s~0.\n", package_spec);

  while (vcpkg_get_control(&i, &node, package_spec))
  {
    const char *package = node->package;

    matches++;
    padding = VCPKG_MAX_NAME - strlen (package);
    padding = max (0, padding);

    indent = C_printf ("  ~6%s~0: %*s", package, padding, "");
    indent -= 4;

    /* In case the `node->description` or `node->version` contains a `~'
     */
    old = C_setraw (1);

    if (node->description)
         print_long_line (node->description, indent+2);
    else C_puts ("<none>\n");

    C_printf ("  %-*s%s\n", indent, "version:", node->version[0] ? node->version : "<none>");
    C_setraw (old);

    print_top_dependencies (node, indent);
  }
  return (matches);
}

