/**\file    win_ver.c
 * \ingroup Misc
 * \brief
 *   Gets the OS version from various sources.
 */
#ifndef BUILD_WINDOWS
#define BUILD_WINDOWS      /* Because of "'GetVersionExW': was declared deprecated" */
#endif

#ifndef USE_RTDLL_VERSION
#define USE_RTDLL_VERSION
#endif

#include <stdio.h>
#include <windows.h>
#include <lm.h>

#include "envtool.h"
#include "color.h"

#ifndef VER_PLATFORM_WIN32_CE
#define VER_PLATFORM_WIN32_CE 3
#endif

#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY  0x00010000
#endif

static char serv_pack[20] = { '\0' };
static char build_str[20] = { '\0' };

static BOOL is_server_os (const OSVERSIONINFOEXW *os)
{
  return (os->wProductType == VER_NT_SERVER ||
          os->wProductType == VER_NT_DOMAIN_CONTROLLER);
}

static BOOL is_home_os (const OSVERSIONINFOEXW *os)
{
  return ( (os->wProductType == VER_NT_WORKSTATION) ||
           (os->wSuiteMask & VER_SUITE_PERSONAL) );
}

BOOL get_wksta_version (DWORD *major, DWORD *minor, DWORD *platform, DWORD level)
{
  typedef NET_API_STATUS (WINAPI *func_NetWkstaGetInfo) (
                          IN  LPWSTR  servername,
                          IN  DWORD   level,
                          OUT BYTE  **bufptr);

  typedef NET_API_STATUS (WINAPI *func_NetApiBufferFree) (
                          IN  void *Buffer);

  HINSTANCE             hnd = LoadLibraryA ("Netapi32.dll");
  func_NetWkstaGetInfo  p_NetWkstaGetInfo;
  func_NetApiBufferFree p_NetApiBufferFree;
  BYTE                 *data;
  BOOL                  rc = FALSE;

  if (!hnd)
     return (rc);

  p_NetWkstaGetInfo  = GETPROCADDRESS (func_NetWkstaGetInfo, hnd, "NetWkstaGetInfo");
  p_NetApiBufferFree = GETPROCADDRESS (func_NetApiBufferFree, hnd, "NetApiBufferFree");

  if (p_NetWkstaGetInfo && p_NetApiBufferFree &&
      (*p_NetWkstaGetInfo)(NULL,level,&data) == NERR_Success)
  {
    if (level == 100)
    {
      const WKSTA_INFO_100 *info = (const WKSTA_INFO_100*) data;

      *major    = info->wki100_ver_major;
      *minor    = info->wki100_ver_minor;
      *platform = info->wki100_platform_id;

      TRACE (1, "  major:     0x%08lX\n", *(u_long*)major);
      TRACE (1, "  minor:     0x%08lX\n", *(u_long*)minor);
      TRACE (1, "  platform:  %lu\n", *(u_long*)platform);
    }
    else if (level == 102)
    {
      const WKSTA_INFO_102 *info = (const WKSTA_INFO_102*) data;

      *major    = info->wki102_ver_major;
      *minor    = info->wki102_ver_minor;
      *platform = info->wki102_platform_id;

      TRACE (1, "  major:     0x%08lX\n", *(u_long*)major);
      TRACE (1, "  minor:     0x%08lX\n", *(u_long*)minor);
      TRACE (1, "  platform:  %lu\n", *(u_long*)platform);
      TRACE (1, "  comp-name: %S\n", info->wki102_computername);
      TRACE (1, "  langroup:  %S\n", info->wki102_langroup);
      TRACE (1, "  langroot:  %S\n", info->wki102_lanroot);
      TRACE (1, "  users:     %lu\n", info->wki102_logged_on_users);
    }
    else
      FATAL ("level must be 100 or 102.\n");

    (*p_NetApiBufferFree) (data);
    rc = TRUE;
  }

  if (hnd)
     FreeLibrary (hnd);
  return (rc);
}

BOOL get_rtdll_version (OSVERSIONINFOEXW *os)
{
  typedef LONG (WINAPI *func_RtlGetVersion) (OSVERSIONINFOW *ver_info);

  HINSTANCE          hnd = LoadLibraryA ("ntdll.dll");
  func_RtlGetVersion p_RtlGetVersion;
  BOOL               rc = FALSE;

  if (!hnd)
     return (rc);

  p_RtlGetVersion = GETPROCADDRESS (func_RtlGetVersion, hnd, "RtlGetVersion");
  if (p_RtlGetVersion)
  {
    memset (os, '\0', sizeof(*os));
    os->dwOSVersionInfoSize = sizeof(*os);
    rc = ((*p_RtlGetVersion)((OSVERSIONINFOW*)os) == 0);
  }

  if (hnd)
     FreeLibrary (hnd);
  return (rc);
}

static const char *get_os_version (void)
{
  OSVERSIONINFOEXW os, osw, *p_os;
  DWORD            os_ver;
  BOOL             rc, equal;
  int              ofs = 0;

  serv_pack[0] = '\0';
  build_str[0] = '\0';
  memset (&os, '\0', sizeof(os));
  os.dwOSVersionInfoSize = sizeof(os);

  /* Win-95 fails if dwOSVersionInfoSize==sizeof(OSVERSIONINFOEXW).
   * But since 'OSVERSIONINFOW' is a subset of 'OSVERSIONINFOEXW', this
   * won't hurt.
   */
  if (!GetVersionExW((OSVERSIONINFOW*)&os))
  {
     os.dwOSVersionInfoSize = sizeof(OSVERSIONINFOW);
     if (!GetVersionExW((OSVERSIONINFOW*)&os))
        return ("WIN-??");
  }

  TRACE (1, "Data from GetVersionExW():\n"
            "  os.dwMajorVersion: 0x%08lX\n"
            "  os.dwMinorVersion: 0x%08lX\n"
            "  os.dwPlatformId:   0x%08lX\n"
            "  os.wProductType:   0x%02X\n"
            "  os.szCSDVersion:   '%" WIDESTR_FMT "'\n",
            (u_long)os.dwMajorVersion, (u_long)os.dwMinorVersion,
            (u_long)os.dwPlatformId, os.wProductType, os.szCSDVersion);

#if defined(USE_RTDLL_VERSION)
  rc = get_rtdll_version (&osw);
  equal = (memcmp (&os, &osw, os.dwOSVersionInfoSize) == 0);

  if (!rc)
     TRACE (1, "RtlGetVersion() failed\n");
  else
  {
    TRACE (1, "Data from RtlGetVersion():\n"
              "  os.dwMajorVersion: 0x%08lX\n"
              "  os.dwMinorVersion: 0x%08lX\n"
              "  os.dwPlatformId:   0x%08lX\n"
              "  os.wProductType:   0x%02X\n"
              "  os.szCSDVersion:   '%" WIDESTR_FMT "'\n",
              (u_long)osw.dwMajorVersion, (u_long)osw.dwMinorVersion,
              (u_long)osw.dwPlatformId, osw.wProductType, osw.szCSDVersion);
  }

  if (rc && !equal)
  {
    p_os = &osw;
    TRACE (1, "  os != osw. Using osw data from RtlGetVersion().\n");
  }
  else
    p_os = &os;

#else
  p_os = &os;
  equal = TRUE;
#endif

  os_ver = (p_os->dwMajorVersion << 16) + p_os->dwMinorVersion;

  if (p_os->wServicePackMajor)
     ofs = sprintf (serv_pack, "SP%u", p_os->wServicePackMajor);

  if (p_os->wServicePackMajor && p_os->wServicePackMinor)
     sprintf (serv_pack+ofs, ".%u", p_os->wServicePackMinor);

  if (p_os->dwBuildNumber)
     sprintf (build_str, "%lu", (u_long)p_os->dwBuildNumber);

  if (p_os->dwPlatformId == VER_PLATFORM_WIN32_NT)
  {
    if (os_ver == 0x50000)
       return ("Win-2000");

    if (os_ver == 0x50001)
       return (is_home_os(p_os) ? "Win-XP Home" : "Win-XP Home");

    if (os_ver == 0x50002)
       return (is_server_os(p_os) ? "Win-Server 2003" : "Win-XP 64-bit");

    if (os_ver == 0x60000)
       return ((p_os->wProductType == VER_NT_WORKSTATION) ? "Win-Vista" : "Win-Server 2008");

    if (os_ver == 0x60001)
       return (is_server_os(p_os) ? "Win-Server 2008/R2" : "Win-7");

    if (os_ver == 0x60002)
       return (is_home_os(p_os) ? "Win-8 Home" : "Win-8");

    if (os_ver == 0x60003)
       return (is_home_os(p_os) ? "Win-8.1 Home" : "Win-8.1");

    if (os_ver == 0xA0000)
    {
      if (p_os->dwBuildNumber >= 22000)
          return ((p_os->wProductType == VER_NT_WORKSTATION) ? "Win-11" : "Win-11 Server");
      return ((p_os->wProductType == VER_NT_WORKSTATION) ? "Win-10" : "Win-10 Server");
    }

    if (HIWORD(os_ver) == 4)
       return ("Win-NT 4.x");

    if (HIWORD(os_ver) == 3)
       return ("Win-NT 3.x");
  }
  else if (p_os->dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
  {
    if (os_ver == 0x40000)
       return ("Win-95");

    if (os_ver == (0x40000+10))
       return ("Win-98");

    if (os_ver == (0x40000+90))
       return ("Win-ME");

     return ("Win-3.1");
  }
  else if (p_os->dwPlatformId == VER_PLATFORM_WIN32_CE)
     return ("Win-CE");  /* just a test */

  else if (p_os->dwPlatformId == VER_PLATFORM_WIN32s)
     return ("Win-32s");

  return ("Win-??");
}

const char *os_name (void)
{
  static char buf[100];
  char       *ptr = buf;
  size_t      left = sizeof(buf);

  ptr  += snprintf (ptr, left, get_os_version());
  left -= ptr - buf;

  if (serv_pack[0])
  {
    ptr  += snprintf (ptr, left, " %s", serv_pack);
    left -= ptr - buf;
  }
  if (build_str[0])
     snprintf (ptr, left, ". Build %s", build_str);
  return (buf);
}

const char *os_bits (void)
{
  char dir [_MAX_PATH];

  if (sizeof(void*) == 8)
     return ("64");

  if (GetSystemWow64Directory(dir, sizeof(dir)))
     return ("64");
  return ("32");
}

/*
 * To get this 'ReleaseId', the only source seems to be here:
 *   "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ReleaseId"
 *
 * And the 'Update Build Revison':
 *   "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\UBR"
 *
 * The winver program shows the version-info like this:
 *   Version  1709 (OS-build 16299.64)
 *   ReleaseId^         Build^ UBR ^
 */
#define CURRENT_VER_KEY  "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"

static const char *get_registry_value (const char *wanted_sub_key, DWORD wanted_type)
{
  static char ret_buf [100];
  DWORD  rc, flags, value, type = 0;
  DWORD  buf_size = sizeof(ret_buf);
  char   key_name [100];

  snprintf (key_name, sizeof(key_name), "%s\\%s", CURRENT_VER_KEY, wanted_sub_key);

  flags = (1 << wanted_type);     /* From 'REG_x* to 'RRF_RT_REG_x' */
  if (is_wow64_active())
     flags |= RRF_SUBKEY_WOW6464KEY;

  rc = RegGetValue (HKEY_LOCAL_MACHINE, CURRENT_VER_KEY, wanted_sub_key, flags, &type, (void*)&ret_buf, &buf_size);

  TRACE (1, "  RegGetValue (%s\\%s, %s), type: %s.\n",
         reg_top_key_name(HKEY_LOCAL_MACHINE), key_name, win_strerror(rc), reg_type_name(type));

  if (rc != ERROR_SUCCESS || type != wanted_type)
     return (NULL);

  if (type == REG_DWORD)
  {
    value = *(DWORD*)&ret_buf;
    _itoa (value, ret_buf, 10);
  }
  return (ret_buf);
}

/*
 * Not all Windows have a "DisplayVersion" in Registry.
 * But on "Windows 20H2", the "ReleaseId" is confusingly "2009".
 * So return "DisplayVersion" instead if found.
 */
const char *os_release_id (void)
{
  const char *disp = get_registry_value ("DisplayVersion", REG_SZ);

  if (disp)
     return (disp);
  return get_registry_value ("ReleaseId", REG_SZ);
}

const char *os_update_build_rev (void)
{
  return get_registry_value ("UBR", REG_DWORD);
}

const char *os_current_build (void)
{
  return get_registry_value ("CurrentBuild", REG_SZ);
}

/*
 * Return the "InstallDate" from Registry.
 * This is the time-stamp of the last reinstall.
 */
time_t os_last_install_date (void)
{
  const char *date = get_registry_value ("InstallDate", REG_DWORD);
  char       *end;
  time_t      rc = 0;

  if (date)
  {
    rc = strtoul (date, &end, 10);
    if (end == date)
    {
      TRACE (1, "Illegal date: '%s'\n", date);
      rc = 0;
    }
  }
  return (rc);
}

/*
 * Try to determine the time-stamp of the first OS installation
 * by looking at the creation time of the `c:\bootmgr` file.
 *
 * Actually the `c:\$Recycle.Bin` folder could be a little bit older.
 *
 * Ref: https://stackoverflow.com/questions/170617/how-do-i-find-the-install-time-and-date-of-windows
 */
time_t os_first_install_date (void)
{
  struct stat st;

  if (safe_stat_sys("c:\\bootmgr", &st, NULL))
     return (0);
  return (st.st_ctime);
}

/**
 * Try to build a version string as 'winver.exe':
 *
 * Version x (OS-build y.z)
 *         ^           ^ ^
 *         |           | |__ from 'os_update_build_rev()'.
 *         |           |____ from 'build_str[]'
 *         |________________ from 'os_release_id()'.
 *
 */
const char *os_full_version (void)
{
  const char *x, *z;
  const char *ver = get_os_version();  /* get the 'build_str' */
  static char ret [100];
  char       *p = ret;

  x = os_release_id();
  if (x && build_str[0])
  {
    p += sprintf (ret, "version %s (OS-build %s", x, build_str);
    z = os_update_build_rev();
    if (z)
    {
      *p++= '.';
      strcat (p, z);
      p += strlen(z);
    }
    *p++= ')';
    *p= '\0';
  }
  else
    _strlcpy (ret, ver, sizeof(ret));

  return (ret);
}

#if defined(WIN_VER_TEST)

struct prog_options opt;

int MS_CDECL main (int argc, char **argv)
{
  DWORD       major, minor, platform;
  BOOL        rc;
  time_t      date;
  const char *ver, *release, *build, *full;

  if (argc >= 2 && !strncmp(argv[1], "-d", 2))
  {
    if (!strcmp(argv[1], "-dd"))
         opt.debug = 2;
    else opt.debug = 1;
  }

  C_init();

  rc = get_wksta_version (&major, &minor, &platform, 100);
  C_puts ("Result from NetWkstaGetInfo(), level 100:\n");
  if (!rc)
     C_puts ("  failed\n");

  rc = get_wksta_version (&major, &minor, &platform, 102);
  C_puts ("Result from NetWkstaGetInfo(), level 102:\n");
  if (!rc)
     C_puts ("  failed\n");

  ver = os_name();
  C_printf ("Result from os_name():               %s\n", ver);
  C_printf ("Result from os_bits():               %s bits\n", os_bits());

  release = os_release_id();
  C_printf ("Result from os_release_id():         %s\n", release ? release : "<none>");

  build = os_update_build_rev();
  C_printf ("Result from os_update_build_rev():   %s\n", build ? build : "<none>");

  full = os_full_version();
  C_printf ("Result from os_full_version():       %s\n", full ? full : "<none>");

  date = os_first_install_date();
  C_printf ("Result from os_first_install_date(): %s\n", date ? get_time_str(date) : "<none>");

  date = os_last_install_date();
  C_printf ("Result from os_last_install_date():  %s\n", date ? get_time_str(date) : "<none>");
  return (0);
}
#endif
