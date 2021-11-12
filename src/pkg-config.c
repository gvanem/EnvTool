/** \file    pkg-config.c
 *  \ingroup pkg_config
 *
 *  \brief The PKG-CONFIG functions for the envtool program.
 */

#include "envtool.h"
#include "color.h"
#include "report.h"
#include "cache.h"
#include "pkg-config.h"

static struct ver_info pkgconfig_ver;
static char           *pkgconfig_exe = NULL;

/**
 *
 */
struct pkgconfig_dir {
       char  path [_MAX_PATH];
       HKEY  top_key;
       BOOL  exist;
       BOOL  is_dir;
       BOOL  exp_ok;
       int   num_dup;
     };

struct pkgconfig_node {
       char name [30];
       char description [200];
     };

static smartlist_t *pkgconfig_dirs = NULL;  /* A smartlist_t of 'struct pkgconfig_dir' */
static smartlist_t *pkgconfig_pkg  = NULL;  /* A smartlist_t of 'struct pkgconfig_node' */

/**
 * \def ENV_NAME
 * The environment variable for search-directories.
 */
#define ENV_NAME "PKG_CONFIG_PATH"

/**
 * \def REG_KEY
 * The Registry sub-branch variable for seach-directories. <br>
 * The full key is either `HKEY_CURRENT_USER\Software\pkgconfig\PKG_CONFIG_PATH` or <br>
 * `HKEY_LOCAL_MACHINE\Software\pkgconfig\PKG_CONFIG_PATH`.
 */
#define REG_KEY  "Software\\pkgconfig"

static void add_package (const char *name, char *description)
{
  struct pkgconfig_node *pkg;

  if (!name)
     return;

  pkg = MALLOC (sizeof(*pkg));
  _strlcpy (pkg->name, name, sizeof(pkg->name));
  _strlcpy (pkg->description, description ? str_unquote(description) : "", sizeof(pkg->description));
  smartlist_add (pkgconfig_pkg, pkg);
}

static int pkg_config_version_cb (char *buf, int index)
{
  struct ver_info ver = { 0,0,0,0 };

  ARGSUSED (index);
  if (sscanf(buf, "%d.%d", &ver.val_1, &ver.val_2) == 2)
  {
    memcpy (&pkgconfig_ver, &ver, sizeof(pkgconfig_ver));
    return (1);
  }
  return (0);
}

static int pkg_config_list_all_cb (char *buf, int index)
{
  char *name, *descr, *end;

  name = _strtok_r (buf, " ", &end);
  descr = (*end != '\0') ? end : "";
  descr = str_ltrim (descr);

  TRACE (2, "%3d: %-30s -> %s.\n", index, name, descr);
  add_package (name, descr);
  return (1);
}

/**
 * Build the list of pkg_config packages found.
 * First try the cache. Otherwise spawn `pkg-config.exe --list-all`.
 *
 * This has no relation to the number of `*.pc` files found on `PKG_CONFIG_PATH`.
 * But rather via the output of `pkg-config --list-all`.
 */
static void pkg_config_build_pkg (void)
{
  int max, i = 0;

  if (!pkgconfig_exe)
     return;

  while (1)
  {
    char  format [100];
    char *name  = NULL;
    char *descr = NULL;

    snprintf (format, sizeof(format), "pkgconfig_node_%d = %%s,%%s", i++);
    if (cache_getf (SECTION_PKGCONFIG, format, &name, &descr) != 2)
       break;
    add_package (name, descr);
  }

  max = smartlist_len (pkgconfig_pkg);
  if (max > 0)
     TRACE (1, "Found %d cached pkg-config packages.\n", max);
  else
  {
    /* None found from cache. Try for real
     */
    popen_run (pkg_config_list_all_cb, pkgconfig_exe, "--list-all 2> %s", DEV_NULL);
  }

  max = smartlist_len (pkgconfig_pkg);
  for (i = 0; i < max; i++)
  {
    const struct pkgconfig_node *pkg = smartlist_get (pkgconfig_pkg, i);

    TRACE (2, "%3d: %-30s  descr: %s.\n", i, pkg->name, pkg->description);
    cache_putf (SECTION_PKGCONFIG, "pkgconfig_node_%d = %s,\"%s\"", i, pkg->name, pkg->description);
  }
}

/**
 * Get the number of pkg_config packages found.
 */
unsigned pkg_config_get_num_installed (void)
{
  int num;

  pkg_config_init();
  num = pkgconfig_pkg ? smartlist_len (pkgconfig_pkg) : 0;
  return (num);
}

/**
 * Print the pkg_config packages found.
 */
unsigned pkg_config_list_installed (void)
{
  int i, max   = pkgconfig_pkg  ? smartlist_len (pkgconfig_pkg) : 0;
  int num_dirs = pkgconfig_dirs ? smartlist_len (pkgconfig_dirs) : 0;
  int indent;

  C_printf ("\n  Found %u ~3pkg-config~0 packages in ~3%d~0 directories:\n", max, num_dirs);
  for (i = 0; i < max; i++)
  {
    const struct pkgconfig_node *pkg = smartlist_get (pkgconfig_pkg, i);

    indent = C_printf ("    %-25s", pkg->name);
    C_puts_long_line (pkg->description, indent);
  }
  return (max);
}

/**
 * Find the version and location `pkg-config.exe` (on `PATH`).
 * First from the cache, then do it for real.
 *
 * In case Cygwin is installed and a `<CYGWIN_ROOT>/bin/pkg-config` is on PATH, check
 * if it's symlinked to a `<CYGWIN_ROOT>/bin/pkgconf.exe` program.
 */
static BOOL pkg_config_get_info_internal (void)
{
  static char exe_copy [_MAX_PATH];

  cache_getf (SECTION_PKGCONFIG, "pkgconfig_exe = %s", &pkgconfig_exe);
  cache_getf (SECTION_PKGCONFIG, "pkgconfig_version = %d,%d", &pkgconfig_ver.val_1, &pkgconfig_ver.val_2);

  if (pkgconfig_exe && !FILE_EXISTS(pkgconfig_exe))
  {
    cache_del (SECTION_PKGCONFIG, "pkgconfig_exe");
    cache_del (SECTION_PKGCONFIG, "pkgconfig_version");
    memset (&pkgconfig_ver, '\0', sizeof(pkgconfig_ver));
    pkgconfig_exe = NULL;
    return pkg_config_get_info_internal();
  }

  /* The above recursive call will get here (having done nothing prior to that)
   */
  if (!pkgconfig_exe)
  {
    const char *cyg_exe = searchpath ("pkg-config", "PATH");

    if (cyg_exe)
         pkgconfig_exe = (char*) get_sym_link (cyg_exe);
    else pkgconfig_exe = searchpath ("pkg-config.exe", "PATH");
  }

  if (!pkgconfig_exe)
     return (FALSE);

  pkgconfig_exe = slashify2 (exe_copy, pkgconfig_exe, '\\');
  cache_putf (SECTION_PKGCONFIG, "pkgconfig_exe = %s", pkgconfig_exe);

  /* A valid version from the cache?
   */
  if (!VALID_VER(pkgconfig_ver) &&
      popen_run(pkg_config_version_cb, pkgconfig_exe, "--version") > 0)
     cache_putf (SECTION_PKGCONFIG, "pkgconfig_version = %d,%d", pkgconfig_ver.val_1, pkgconfig_ver.val_2);

  TRACE (2, "ver: %d.%d.\n", pkgconfig_ver.val_1, pkgconfig_ver.val_2);
  return (VALID_VER(pkgconfig_ver));
}

/**
 * Return pkg-config information to caller.
 */
BOOL pkg_config_get_info (char **exe, struct ver_info *ver)
{
  pkg_config_init();

  if (pkgconfig_exe && VALID_VER(pkgconfig_ver))
  {
    *exe = STRDUP (pkgconfig_exe);
    *ver = pkgconfig_ver;
    return (TRUE);
  }
  return (FALSE);
}

/**
 * Get the `PKG_CONFIG_PATH` from Registry.
 */
static smartlist_t *pkg_config_reg_keys (HKEY top_key)
{
  char  buf [16*1024] = "";
  char *tok, *end;
  int   i;
  DWORD type = 0;
  DWORD buf_size = sizeof(buf);
  DWORD rc = RegGetValue (top_key, REG_KEY, ENV_NAME, RRF_RT_REG_SZ,
                          &type, (void*)&buf, &buf_size);

  TRACE (1, "  RegGetValue (%s\\%s, %s), type: %s:\n -> %s\n",
         reg_top_key_name(top_key), REG_KEY, win_strerror(rc), reg_type_name(type), buf);

  if (rc != ERROR_SUCCESS || type != REG_SZ)
     return (NULL);

  for (i = 0, tok = buf; tok; i++)
  {
    if (i == 0)
        tok = _strtok_r (tok, ";", &end);
    TRACE (1, "tok[%d]: '%s'\n", i, tok);
    reg_array_add (top_key, tok, tok);
    tok = _strtok_r (NULL, ";", &end);
  }
  return reg_array_head();
}

/**
 * Get and print more verbose details in a pkg-config `.pc` file.
 * Look for lines like:
 * ```
 *  Description: Python bindings for cairo
 *  Version: 1.8.10
 * ```
 *
 * and print this like `Python bindings for cairo (v1.8.10) `
 */
int pkg_config_get_details (const char *pc_file, const char *filler)
{
  FILE *f = fopen (pc_file, "rt");
  char  buf [1000], *p;
  char  descr [1000] = { "" };
  char  version [50] = { "" };

  if (!f)
     return (0);

  while (fgets(buf, sizeof(buf), f))
  {
    p = str_ltrim (buf);
    sscanf (p, "Description: %999[^\r\n]", descr);
    sscanf (p, "Version: %49s", version);
  }
  if (descr[0] && version[0])
     C_printf ("\n%s%s (v%s)", filler, str_ltrim(descr), str_ltrim(version));
  fclose (f);
  return (1);
}

int pkg_config_get_details2 (struct report *r)
{
  return pkg_config_get_details (r->file, r->filler);
}

/**
 * Create a single smartlist with unique paths from 2 smartlists.
 *
 * \param[in]  dir The smartlist of directories from env-var `PKG_CONFIG_PATH`.
 * \param[in]  reg The smartlist of directories from Registry.
 *
 * This function assumes the directories in `dir` are unique. It will for each
 * element in `dir` check if it's present in `reg`. If not found in `dir`,
 * it will append it to `pkgconfig_dirs`.
 */
static void merge_directories (smartlist_t *dir, smartlist_t *reg)
{
  struct pkgconfig_dir *pkdir;
  int    i, j;
  int    dir_max = dir ? smartlist_len(dir) : 0;
  int    reg_max = reg ? smartlist_len(reg) : 0;

  for (i = 0; i < dir_max; i++)
  {
    const struct directory_array *arr = smartlist_get (dir, i);

    pkdir = CALLOC (sizeof(*pkdir), 1);
    _strlcpy (pkdir->path, arr->dir, sizeof(pkdir->path));
    pkdir->top_key = NULL;
    pkdir->exist    = arr->exist;
    pkdir->is_dir   = arr->is_dir;
    pkdir->exp_ok   = arr->exp_ok;
    pkdir->num_dup  = arr->num_dup;
    smartlist_add (pkgconfig_dirs, pkdir);
  }

  /* Loop over the `reg` list and add only if not already in `pkgconfig_dirs`.
   */
  for (i = 0; i < reg_max; i++)
  {
    const struct registry_array *arr1 = smartlist_get (reg, i);
    const struct pkgconfig_dir  *arr2;
    BOOL  add_it = TRUE;

    for (j = 0; j < dir_max; j++)
    {
      arr2 = smartlist_get (pkgconfig_dirs, j);
      if (!stricmp(arr1->fname, arr2->path))
      {
        add_it = FALSE;
        break;
      }
    }
    if (!add_it)
       continue;

    pkdir = CALLOC (sizeof(*pkdir), 1);
    _strlcpy (pkdir->path, arr1->fname, sizeof(pkdir->path));
    pkdir->top_key = arr1->key;
    pkdir->exist   = arr1->exist;
    pkdir->is_dir  = arr1->exist;
    pkdir->exp_ok  = TRUE;
    smartlist_add (pkgconfig_dirs, pkdir);
  }
}

/**
 * Initialise this module. Only once.
 */
void pkg_config_init (void)
{
  smartlist_t *list_env, *list_reg;
  char        *orig_e;

  if (pkgconfig_pkg)
     return;

  pkgconfig_pkg = smartlist_new();
  pkgconfig_dirs = smartlist_new();

  orig_e = getenv_expand (ENV_NAME);
  list_env = orig_e ? split_env_var (ENV_NAME, orig_e) : NULL;

  list_reg = pkg_config_reg_keys (HKEY_CURRENT_USER);
  if (!list_reg)
     list_reg = pkg_config_reg_keys (HKEY_LOCAL_MACHINE);

  merge_directories (list_env, list_reg);
  dir_array_free();
  reg_array_free();
  FREE (orig_e);

  pkg_config_get_info_internal();
  pkg_config_build_pkg();
}

/**
 * Called from `cleanup()` to free memory allocated here.
 */
void pkg_config_exit (void)
{
  smartlist_free_all (pkgconfig_dirs);
  smartlist_free_all (pkgconfig_pkg);
  pkgconfig_pkg = pkgconfig_dirs = NULL;
}

/**
 * Search and check along `pkg_config_dirs` for a
 * matching `<filespec>.pc` file.
 */
int pkg_config_search (const char *search_spec)
{
  int  i, max, num, prev_num = 0, found = 0;
  BOOL do_warn = FALSE;

  pkg_config_init();

  if (smartlist_len(pkgconfig_dirs) == 0)
  {
    WARN ("%s not defined in environment nor in the Registry\n", ENV_NAME);
    return (0);
  }

  report_header_set ("Matches in %%%s:\n", ENV_NAME);

  max = smartlist_len (pkgconfig_dirs);
  for (i = 0; i < max; i++)
  {
    struct pkgconfig_dir *dir = smartlist_get (pkgconfig_dirs, i);
    char   prefix [30];

    TRACE (2, "Checking in %s dir '%s'\n", dir->top_key ? "Registry" : "environment", dir->path);

    if (dir->top_key == HKEY_CURRENT_USER)
       snprintf (prefix, sizeof(prefix), "[HKCU\\%s]", REG_KEY);
    else if (dir->top_key == HKEY_LOCAL_MACHINE)
       snprintf (prefix, sizeof(prefix), "[HKLM\\%s]", REG_KEY);
    else if (dir->top_key == NULL)
       _strlcpy (prefix, ENV_NAME, sizeof(prefix));
    else
       _strlcpy (prefix, "PkgConfig?", sizeof(prefix));

    num = process_dir (dir->path, 0, dir->exist, TRUE, dir->is_dir, dir->exp_ok,
                       prefix, HKEY_PKG_CONFIG_FILE, FALSE);
    if (dir->num_dup == 0 && prev_num > 0 && num > 0)
       do_warn = TRUE;
    found += num;
  }

  if (do_warn && !opt.quiet)
  {
    WARN ("Note: ");
    C_printf ("~6There seems to be several '%s' files in different %%%s directories.\n"
              "      \"pkg-config\" will only select the first.~0\n", search_spec, ENV_NAME);
  }
  return (found);
}

void pkg_config_extras (const struct ver_data *v, int pad_len)
{
  unsigned num = pkg_config_get_num_installed();

  C_printf ("%-*s -> ~6%s~0", pad_len, v->found, slashify(v->exe, v->slash));
  if (num >= 1)
     C_printf (" (%u .pc files installed).", num);
  C_putc ('\n');
}
