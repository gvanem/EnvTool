/*
 * Gets the OS version from various sources.
 *
 * Build:
 *  gcc -Wall  -DWIN_VER_TEST -o win_ver.exe win_ver.c misc.c color.c
 *  cl -nologo -DWIN_VER_TEST -Fe./win_ver.exe win_ver.c misc.c color.c
 */

#define BUILD_WINDOWS      /* Because of "'GetVersionExW': was declared deprecated" */
#define USE_RTDLL_VERSION

#include <stdio.h>
#include <windows.h>
#include <lm.h>

#include "envtool.h"

#if defined(__GNUC__)
  #define WIDESTR_FMT  "S"
#else
  #define WIDESTR_FMT  "ws"
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

BOOL get_wksta_version (DWORD *major, DWORD *minor, DWORD *platform)
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
      (*p_NetWkstaGetInfo)(NULL,100,&data) == NERR_Success)
  {
    const WKSTA_INFO_100 *info = (const WKSTA_INFO_100*) data;

    *major    = info->wki100_ver_major;
    *minor    = info->wki100_ver_minor;
    *platform = info->wki100_platform_id;
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
             os.dwMajorVersion, os.dwMinorVersion, os.dwPlatformId,
             os.wProductType, os.szCSDVersion);

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
               osw.dwMajorVersion, osw.dwMinorVersion, osw.dwPlatformId,
               osw.wProductType, osw.szCSDVersion);
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
     sprintf (build_str, "%lu", p_os->dwBuildNumber);

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
       return ((p_os->wProductType == VER_NT_WORKSTATION) ? "Win-10 " : "Win-10 Server");

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
  else if (p_os->dwPlatformId == VER_PLATFORM_WIN32s)
     return ("Win-3.1");

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
  {
    ptr  += snprintf (ptr, left, ". Build %s", build_str);
    left -= ptr - buf;
  }
  return (buf);
}

#if defined(WIN_VER_TEST)

struct prog_options opt;

int main (void)
{
  DWORD major, minor, platform;
  BOOL  rc;
  const char *ver;

  opt.debug = 1;

  rc = get_wksta_version (&major, &minor, &platform);
  DEBUGF (1, "Result from NetWkstaGetInfo():\n");
  if (!rc)
     DEBUGF (1, "  failed\n");
  else
  {
    DEBUGF (1, "  major:    0x%08lX\n", major);
    DEBUGF (1, "  minor:    0x%08lX\n", minor);
    DEBUGF (1, "  platform: %lu\n", platform);
  }

  ver = os_name();
  DEBUGF (1, "Result from os_name():\n  %s\n", ver);
  return (0);
}
#endif
