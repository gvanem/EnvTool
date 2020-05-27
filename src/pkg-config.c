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

/**
 * Find the version and location `pkg-config.exe` (on `PATH`).
 *
 * In case Cygwin is installed and a `<CYGWIN_ROOT>/bin/pkg-config` is on PATH, check
 * if it's symlinked to a `<CYGWIN_ROOT>/bin/pkgconf.exe` program.
 */
static struct ver_info pkgconfig_ver;
static char           *pkgconfig_exe = NULL;

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

/**
 * Find the number of pkg_config packages installed.
 * This has no relation to the number of `*.pc` files found on `PKG_CONFIG_PATH` or
 * the number reported via `pkg-config --list-all`.
 */
unsigned pkg_config_get_num_installed (void)
{
  return (1); // just for now.
}

BOOL pkg_config_get_info (char **exe, struct ver_info *ver)
{
  static char exe_copy [_MAX_PATH];

  *exe = NULL;
  *ver = pkgconfig_ver;

  /* We have already done this
   */
  if (pkgconfig_exe && VALID_VER(pkgconfig_ver))
  {
    *exe = STRDUP (pkgconfig_exe);
    return (TRUE);
  }

  DEBUGF (2, "ver: %d.%d.%d.\n", ver->val_1, ver->val_2, ver->val_3);

  cache_getf (SECTION_PKGCONFIG, "pkgconfig_exe = %s", &pkgconfig_exe);
  cache_getf (SECTION_PKGCONFIG, "pkgconfig_version = %d,%d", &pkgconfig_ver.val_1, &pkgconfig_ver.val_2);

  if (pkgconfig_exe && !FILE_EXISTS(pkgconfig_exe))
  {
    cache_del (SECTION_PKGCONFIG, "pkgconfig_version");
    cache_del (SECTION_PKGCONFIG, "pkgconfig_exe");
    memset (&pkgconfig_ver, '\0', sizeof(pkgconfig_ver));
    pkgconfig_exe = NULL;
    return pkg_config_get_info (exe, ver);
  }

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
  *exe = STRDUP (pkgconfig_exe);

  cache_putf (SECTION_PKGCONFIG, "pkgconfig_exe = %s", pkgconfig_exe);

  if (!VALID_VER(pkgconfig_ver) && popen_runf(pkg_config_version_cb, "\"%s\" --version", pkgconfig_exe) > 0)
     cache_putf (SECTION_PKGCONFIG, "pkgconfig_version = %d,%d", pkgconfig_ver.val_1, pkgconfig_ver.val_2);

  *ver = pkgconfig_ver;
  DEBUGF (2, "ver: %d.%d.%d.\n", ver->val_1, ver->val_2, ver->val_3);

  return (pkgconfig_exe && VALID_VER(pkgconfig_ver));
}

/*
 * Get the `PKG_CONFIG_PATH` from Registry under: <br>
 *   `HKEY_CURRENT_USER\Software\pkgconfig\PKG_CONFIG_PATH`  or
 *   `HKEY_LOCAL_MACHINE\Software\pkgconfig\PKG_CONFIG_PATH`.
 */
#define PKG_CONFIG_REG_KEY "Software\\pkgconfig\\PKG_CONFIG_PATH"

static smartlist_t *pkg_config_reg_keys (HKEY top_key)
{
  HKEY   key;
  int    i = 0;
  char   buf [16*1024];
  DWORD  rc, buf_size = sizeof(buf);
  REGSAM acc = reg_read_access();

  rc  = RegOpenKeyEx (top_key, PKG_CONFIG_REG_KEY, 0, acc, &key);

  DEBUGF (1, "  RegOpenKeyEx (%s\\%s, %s):\n                   %s\n",
          reg_top_key_name(top_key), PKG_CONFIG_REG_KEY, reg_access_name(acc), win_strerror(rc));

  if (rc != ERROR_SUCCESS)
     return (NULL);

  while (RegEnumValue(key, i++, buf, &buf_size, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
  {
    char  path [_MAX_PATH];
    DWORD type, path_len = sizeof(path);

    if (RegQueryValueEx(key, buf, NULL, &type, (BYTE*)path, &path_len) == ERROR_SUCCESS && type == REG_SZ)
       DIR_ARRAY_ADD (path, str_equal(path,current_dir));
    buf_size = sizeof(buf);
  }
  RegCloseKey (key);
  return dir_array_head();
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
 * Search and check along `%PKG_CONFIG_PATH%` for a
 * matching `<filespec>.pc` file.
 */
int pkg_config_search (const char *search_spec)
{
  smartlist_t *list;
  int          i, max, num, prev_num = 0, found = 0;
  BOOL         do_warn = FALSE;
  char        *orig_e;
  static const char env_name[] = "PKG_CONFIG_PATH";

  orig_e = getenv_expand (env_name);
  list = orig_e ? split_env_var (env_name, orig_e) : NULL;

  if (!list)
  {
    list = pkg_config_reg_keys (HKEY_CURRENT_USER);
    if (!list)
       list = pkg_config_reg_keys (HKEY_LOCAL_MACHINE);
    if (!list)
    {
      WARN ("%s not defined in environment nor in the Registry\n", env_name);
      return (0);
    }
  }

  set_report_header ("Matches in %%%s:\n", env_name);

  max = smartlist_len (list);
  for (i = 0; i < max; i++)
  {
    const struct directory_array *arr = smartlist_get (list, i);

    DEBUGF (2, "Checking in dir '%s'\n", arr->dir);
    num = process_dir (arr->dir, 0, arr->exist, TRUE, arr->is_dir, arr->exp_ok,
                       env_name, HKEY_PKG_CONFIG_FILE, FALSE);

    if (arr->num_dup == 0 && prev_num > 0 && num > 0)
       do_warn = TRUE;
    if (prev_num == 0 && num > 0)
       prev_num = num;
    found += num;
  }

  dir_array_free();
  FREE (orig_e);

  if (do_warn && !opt.quiet)
  {
    WARN ("Note: ");
    C_printf ("~6There seems to be several '%s' files in different %%%s directories.\n"
              "      \"pkg-config\" will only select the first.~0\n", search_spec, env_name);
  }
  return (found);
}

