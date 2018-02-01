/**\file    win_ver.c
 * \ingroup Misc
 * \brief
 *   Gets the OS version from various sources.
 */
#define BUILD_WINDOWS      /* Because of "'GetVersionExW': was declared deprecated" */
#define USE_RTDLL_VERSION

#include <stdio.h>
#include <windows.h>
#include <lm.h>

#include "envtool.h"

#ifndef VER_PLATFORM_WIN32_CE
#define VER_PLATFORM_WIN32_CE 3
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

  if (hnd == INVALID_HANDLE_VALUE)
     return (rc);

  p_NetWkstaGetInfo  = (func_NetWkstaGetInfo)  GetProcAddress (hnd, "NetWkstaGetInfo");
  p_NetApiBufferFree = (func_NetApiBufferFree) GetProcAddress (hnd, "NetApiBufferFree");

  if (p_NetWkstaGetInfo && p_NetApiBufferFree &&
      (*p_NetWkstaGetInfo)(NULL,level,&data) == NERR_Success)
  {
    if (level == 100)
    {
      const WKSTA_INFO_100 *info = (const WKSTA_INFO_100*) data;

      *major    = info->wki100_ver_major;
      *minor    = info->wki100_ver_minor;
      *platform = info->wki100_platform_id;

      DEBUGF (1, "  major:     0x%08lX\n", *(u_long*)major);
      DEBUGF (1, "  minor:     0x%08lX\n", *(u_long*)minor);
      DEBUGF (1, "  platform:  %lu\n", *(u_long*)platform);
    }
    else if (level == 102)
    {
      const WKSTA_INFO_102 *info = (const WKSTA_INFO_102*) data;

      *major    = info->wki102_ver_major;
      *minor    = info->wki102_ver_minor;
      *platform = info->wki102_platform_id;

      DEBUGF (1, "  major:     0x%08lX\n", *(u_long*)major);
      DEBUGF (1, "  minor:     0x%08lX\n", *(u_long*)minor);
      DEBUGF (1, "  platform:  %lu\n", *(u_long*)platform);
      DEBUGF (1, "  comp-name: %S\n", info->wki102_computername);
      DEBUGF (1, "  langroup:  %S\n", info->wki102_langroup);
      DEBUGF (1, "  langroot:  %S\n", info->wki102_lanroot);
      DEBUGF (1, "  users:     %lu\n", info->wki102_logged_on_users);
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

  if (hnd == INVALID_HANDLE_VALUE)
     return (rc);

  p_RtlGetVersion = (func_RtlGetVersion) GetProcAddress (hnd, "RtlGetVersion");
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

  DEBUGF (1, "Data from GetVersionExW():\n"
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
     DEBUGF (1, "RtlGetVersion() failed\n");
  else
  {
    DEBUGF (1, "Data from RtlGetVersion():\n"
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
    DEBUGF (1, "  os != osw. Using osw data from RtlGetVersion().\n");
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
       return ((p_os->wProductType == VER_NT_WORKSTATION) ? "Win-10" : "Win-10 Server");

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
  if (GetSystemWow64Directory(dir,sizeof(dir)))
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

static const char *get_registry_value (const char *wanted_value, DWORD wanted_type)
{
  HKEY   key = NULL;
  DWORD  num,  rc;
  BOOL   found = FALSE;
  static char ret_buf [100];

  rc  = RegOpenKeyEx (HKEY_LOCAL_MACHINE, CURRENT_VER_KEY, 0, KEY_READ, &key);

  DEBUGF (1, "RegOpenKeyEx (HKLM\\%s): %s\n", CURRENT_VER_KEY, win_strerror(rc));

  for (num = 0; rc == ERROR_SUCCESS; num++)
  {
    char  value [512] = "\0";
    char  data [512]  = "\0";
    DWORD value_size  = sizeof(value);
    DWORD data_size   = sizeof(data);
    DWORD type        = REG_NONE;

    rc = RegEnumValue (key, num, value, &value_size, NULL, &type, (BYTE*)&data, &data_size);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    switch (type)
    {
      case REG_SZ:
           DEBUGF (1, "  num %lu: %s = %s (REG_SZ).\n", (u_long)num, value, data);
           break;
      case REG_DWORD:
           DEBUGF (1, "  num %lu: %s = %lu (REG_DWORD).\n", (u_long)num, value, *(u_long*)&data[0]);
           break;
      default:
           DEBUGF (1, "  num %lu: %s = ? (%s).\n", (u_long)num, value, reg_type_name(type));
           break;
    }
    if (type == wanted_type && !stricmp(wanted_value,value))
    {
      _strlcpy (ret_buf, data, sizeof(ret_buf));
      found = TRUE;
      break;
    }
  }

  if (key)
     RegCloseKey (key);
  return (found ? ret_buf : NULL);
}

const char *os_release_id (void)
{
  return get_registry_value ("ReleaseId", REG_SZ);
}

const char *os_update_build_rev (void)
{
  return get_registry_value ("UBR", REG_DWORD);
}

#if defined(WIN_VER_TEST)

struct prog_options opt;

int main (void)
{
  DWORD major, minor, platform;
  BOOL  rc;
  const char *ver;

  opt.debug = 1;

  rc = get_wksta_version (&major, &minor, &platform, 100);
  DEBUGF (1, "Result from NetWkstaGetInfo(), level 100:\n");
  if (!rc)
     DEBUGF (1, "  failed\n");

  rc = get_wksta_version (&major, &minor, &platform, 102);
  DEBUGF (1, "Result from NetWkstaGetInfo(), level 102:\n");
  if (!rc)
     DEBUGF (1, "  failed\n");

  ver = os_name();
  DEBUGF (1, "Result from os_name(): %s\n", ver);
  DEBUGF (1, "Result from os_bits(): %s bits\n", os_bits());
  return (0);
}
#endif
