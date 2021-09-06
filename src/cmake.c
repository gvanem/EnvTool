/**\file    cmake.c
 * \ingroup Misc
 * \brief   Functions for CMake.
 */
#include "envtool.h"
#include "cache.h"
#include "cmake.h"

/**
 * \def KITWARE_REG_NAME
 * The Kitware (Cmake) Registry key name under HKCU or HKLM.
 */
#define KITWARE_REG_NAME "Software\\Kitware\\CMake\\Packages"

/**
 * Get the value and data for a Kitware sub-key. <br>
 * Like:
 *   `reg.exe query  HKCU\Software\Kitware\CMake\Packages\gflags`
 * does:
 * ```
 * HKEY_CURRENT_USER\Software\Kitware\CMake\Packages\gflags
 *    6dceedd62edc8337ea153c73497e3d9e    REG_SZ    f:/ProgramFiler-x86/gflags/lib/cmake/gflags
 *    ^                                             ^
 *    |__ ret_uuid                                  |___ ret_path
 * ```
 */
static BOOL cmake_get_value_path (HKEY top_key, const char *key_name, char **ret_uuid, char **ret_path)
{
  HKEY   key = NULL;
  DWORD  rc;
  u_long num = 0;
  REGSAM acc = reg_read_access();
  static char _ret_uuid [100];
  static char _ret_path [512];

  *ret_uuid = *ret_path = NULL;

  rc = RegOpenKeyEx (top_key, key_name, 0, acc, &key);
  while (rc == ERROR_SUCCESS)
  {
    char  uuid [200] = "\0";
    char  path [512] = "\0";
    DWORD uuid_size  = sizeof(uuid);
    DWORD path_size  = sizeof(path);
    DWORD type       = REG_NONE;

    rc = RegEnumValue (key, num++, uuid, &uuid_size, NULL, &type, (BYTE*)&path, &path_size);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    if (type != REG_SZ)
       continue;

    if (uuid[0])
       *ret_uuid = _strlcpy (_ret_uuid, uuid, sizeof(_ret_uuid));
    if (path[0])
       *ret_path = _strlcpy (_ret_path, path, sizeof(_ret_path));
  }
  if (key)
     RegCloseKey (key);
  return (*ret_uuid && *ret_path);
}

/**
 * Iterate over Registry keys to find location of `.cmake` files. <br>
 * Does what the command:
 *   reg.exe query HKCU\Software\Kitware\CMake\Packages /s /reg:32
 *   reg.exe query HKLM\Software\Kitware\CMake\Packages /s /reg:32
 * does.
 */
int cmake_get_info_registry (int *index, HKEY top_key)
{
  HKEY   key = NULL;
  int    num;
  REGSAM acc = reg_read_access();
  DWORD  rc  = RegOpenKeyEx (top_key, KITWARE_REG_NAME, 0, acc, &key);

  for (num = 0; rc == ERROR_SUCCESS; num++)
  {
    char  package_key [512];
    char  package [100];
    char *uuid = "?";
    char *path = "?";
    DWORD size = sizeof(package);

    rc = RegEnumKeyEx (key, num, package, &size, NULL, NULL, NULL, NULL);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    snprintf (package_key, sizeof(package_key), "%s\\%s", KITWARE_REG_NAME, package);
    cmake_get_value_path (top_key, package_key, &uuid, &path);
    uuid = _fix_uuid (uuid, NULL);
    path = _fix_path (path, NULL);
    cache_putf (SECTION_CMAKE, "cmake_key%d = %s\\%s,%s,%s", *index, reg_top_key_name(top_key), package_key, uuid, path);
    FREE (uuid);
    FREE (path);
    (*index)++;
  }
  if (key)
     RegCloseKey (key);
  return (num);
}

/**
 * Get CMake Registry keys from the cache.
 */
int cmake_cache_info_registry (void)
{
  char format [50], *key, *uuid, *path;
  int  i = 0, found = 0;

  while (1)
  {
    snprintf (format, sizeof(format), "cmake_key%d = %%s,%%s,%%s", i);
    if (cache_getf(SECTION_CMAKE, format, &key, &uuid, &path) != 3)
       break;
    TRACE (1, "%s: %s, %s\n", key, uuid, path);
    found++;
    i++;
  }
  TRACE (1, "Found %d cached entries for Cmake.\n", found);
  return (found);
}

/**
 * Before checking the `CMAKE_MODULE_PATH`, we need to find the version and location
 * of `cmake.exe` (on `PATH`). Then assume it's built-in Module-path is relative to this.
 * E.g:
 * \code
 *   cmake.exe     -> f:\MinGW32\bin\CMake\bin\cmake.exe.
 *   built-in path -> f:\MinGW32\bin\CMake\share\cmake-{major_ver}.{minor_ver}\Modules
 * \endcode
 */
static struct ver_info cmake_ver;
static char           *cmake_exe = NULL;

static int cmake_version_cb (char *buf, int index)
{
  static char     prefix[] = "cmake version ";
  struct ver_info ver = { 0,0,0,0 };
  char           *p = buf + sizeof(prefix) - 1;

  if (str_startswith(buf, prefix) &&
      sscanf(p, "%d.%d.%d", &ver.val_1, &ver.val_2, &ver.val_3) >= 2)
  {
    memcpy (&cmake_ver, &ver, sizeof(cmake_ver));
    return (1);
  }
  ARGSUSED (index);
  return (0);
}

BOOL cmake_get_info (char **exe, struct ver_info *ver)
{
  *ver = cmake_ver;
  *exe = NULL;

  /* We have already done this
   */
  if (cmake_exe && VALID_VER(cmake_ver))
  {
    *exe = STRDUP (cmake_exe);
    return (TRUE);
  }

  TRACE (2, "ver: %d.%d.%d.\n", ver->val_1, ver->val_2, ver->val_3);

  cache_getf (SECTION_CMAKE, "cmake_exe = %s", &cmake_exe);
  cache_getf (SECTION_CMAKE, "cmake_version = %d,%d,%d", &cmake_ver.val_1, &cmake_ver.val_2, &cmake_ver.val_3);

  if (cmake_exe && !FILE_EXISTS(cmake_exe))
  {
    cache_del (SECTION_CMAKE, "cmake_exe");
    cache_del (SECTION_CMAKE, "cmake_version");
    memset (&cmake_ver, '\0', sizeof(cmake_ver));
    cmake_exe = NULL;
    return cmake_get_info (exe, ver);
  }

  if (!cmake_exe)
     cmake_exe = searchpath ("cmake.exe", "PATH");

  if (!cmake_exe)
     return (FALSE);

  cache_putf (SECTION_CMAKE, "cmake_exe = %s", cmake_exe);
  *exe = STRDUP (cmake_exe);

  if (!VALID_VER(cmake_ver) && popen_run(cmake_version_cb, cmake_exe, "-version") > 0)
     cache_putf (SECTION_CMAKE, "cmake_version = %d,%d,%d", cmake_ver.val_1, cmake_ver.val_2, cmake_ver.val_3);

  *ver = cmake_ver;
  TRACE (2, "ver: %d.%d.%d.\n", ver->val_1, ver->val_2, ver->val_3);

  return (cmake_exe && VALID_VER(cmake_ver));
}

