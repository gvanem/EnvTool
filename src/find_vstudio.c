/*
 * Helper library for location Visual Studio installations
 * using the COM-based query API.
 *
 * Copyright (c) Microsoft Corporation
 * Licensed to PSF under a contributor agreement
 *
 * Version history
 *  2017-05: Initial contribution (Steve Dower)
 *
 * Taken from PyFindVS module and rewritten into pure C
 * without any Python specific code. The original is here:
 *   https://github.com/zooba/pyfindvs/blob/master/pyfindvs/pyfindvs.cpp
 */
#define CINTERFACE
#define COBJMACROS

#include <windows.h>
#include <tchar.h>
#include <objbase.h>
#include <shobjidl.h>

#include "color.h"
#include "envtool.h"

#if defined(_MSC_VER) /* Rest of file */
/*
 * Some of the contents of 'Python3.x\src\PC\external\include\Setup.Configuration.h:
 *
 * "B41463C3-8866-43B5-BC33-2B0676F7F42E"   // ISetupInstance
 *
 * "89143C9A-05AF-49B0-B717-72E218A2185C"   // ISetupInstance2
 *
 * "6380BCFF-41D3-4B2E-8B2E-BF8A6810C848"   // IEnumSetupInstance
 *
 * "46DCCD94-A287-476A-851E-DFBC2FFDBC20"   // ISetupErrorState
 *
 * "9871385B-CA69-48F2-BC1F-7A37CBF0B1EF"   // ISetupErrorState2
 *
 * Look at
 *   https://github.com/dns/WinAPI-Embed-Browser/blob/master/embed-browser.c
 *   https://msdn.microsoft.com/en-us/library/windows/desktop/dd387925%28v=vs.85%29.aspx?f=255&MSPPError=-2147217396
 *
 * for an example of 'ActiveX-Components in plain C'.
 *
 * Look at
 *   https://stackoverflow.com/questions/2187425/how-do-i-use-a-com-dll-with-loadlibrary-in-c
 *
 * for an example on using `DllGetClassObject()` directly.
 */

static CLSID CLSID_SetupConfiguration;
static GUID  IID_ISetupConfiguration;
static GUID  IID_ISetupConfiguration2;
static IID  *g_iid = NULL;
static char *g_crashinfo;

static void print_and_compare_guid_str (const GUID *guid, const char *ascii_in)
{
  char    a_result [40];
  wchar_t w_result [40];

  strcpy (a_result, "{??}");
  if (StringFromGUID2(guid, w_result, (int)DIM(w_result)))
  {
    if (!wchar_to_mbchar(a_result, sizeof(a_result), w_result))
       strcpy (a_result, "{??}");
  }
  TRACE (1, "GUID: %s, %sthe same.\n", a_result, strcmp(ascii_in, a_result) ? "not " : "");
}

static void build_GUIDs (void)
{
  HRESULT hr;
  const char    *a_str;
  const wchar_t *w_str;

  #undef  _T
  #define _T(s)       L##s
  #define SET_GUID(a) do { a_str = a; w_str = _T(a); } while (0)

  /*
   * Visual-Studio 2022 keys:
   *   HKEY_CLASSES_ROOT\CLSID\{177F0C4A-1CD3-4DE7-A32C-71DBBB9FA36D}
   *   HKEY_CLASSES_ROOT\CLSID\{D84C3A54-4501-436D-B4F9-750E5F727802}
   *
   * HKEY_CLASSES_ROOT\CLSID\{177F0C4A-1CD3-4DE7-A32C-71DBBB9FA36D}\InprocServer32 ->
   *   c:\ProgramData\Microsoft\VisualStudio\Setup\x64\Microsoft.VisualStudio.Setup.Configuration.Native.dll
   *
   * HKEY_CLASSES_ROOT\WOW6432Node\CLSID\{177F0C4A-1CD3-4DE7-A32C-71DBBB9FA36D}\InprocServer32 ->
   *   c:\ProgramData\Microsoft\VisualStudio\Setup\x86\Microsoft.VisualStudio.Setup.Configuration.Native.dll
   *
   * HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\{177F0C4A-1CD3-4DE7-A32C-71DBBB9FA36D}\InprocServer32 ->
   *   c:\ProgramData\Microsoft\VisualStudio\Setup\x64\Microsoft.VisualStudio.Setup.Configuration.Native.dll
   *
   * HKEY_LOCAL_MACHINE\SOFTWARE\Classes\WOW6432Node\CLSID\{177F0C4A-1CD3-4DE7-A32C-71DBBB9FA36D}\InprocServer32 ->
   *   c:\ProgramData\Microsoft\VisualStudio\Setup\x86\Microsoft.VisualStudio.Setup.Configuration.Native.dll
   */
  SET_GUID ("{177F0C4A-1CD3-4DE7-A32C-71DBBB9FA36D}");
  hr = CLSIDFromString (w_str, &CLSID_SetupConfiguration);
  if (FAILED(hr))
       TRACE (1, "hr: %s\n", win_strerror(hr));
  else print_and_compare_guid_str (&CLSID_SetupConfiguration, a_str);

  SET_GUID ("{42843719-DB4C-46C2-8E7C-64F1816EFD5B}");
  hr = IIDFromString (w_str, &IID_ISetupConfiguration);
  if (FAILED(hr))
       TRACE (1, "hr: %s\n", win_strerror(hr));
  else print_and_compare_guid_str (&IID_ISetupConfiguration, a_str);

  SET_GUID ("{26AAB78C-4A60-49D6-AF3B-3C35BC93365D}");
  hr = IIDFromString (w_str, &IID_ISetupConfiguration2);
  if (FAILED(hr))
       TRACE (1, "hr: %s\n", win_strerror(hr));
  else print_and_compare_guid_str (&IID_ISetupConfiguration2, a_str);

  #undef  _T
}

typedef enum {
        eNone = 0,
        eLocal = 1,
        eRegistered = 2,
        eNoRebootRequired = 4,
        eNoErrors = 8,
    //  eComplete = MAXUINT
      } InstanceState;

/* Forwards
 */
typedef struct ISetupInstance         ISetupInstance;
typedef struct ISetupInstance2        ISetupInstance2;
typedef struct ISetupPackageReference ISetupPackageReference;
typedef struct ISetupErrorState       ISetupErrorState;
typedef struct ISetupPropertyStore    ISetupPropertyStore;
typedef struct IEnumSetupInstances    IEnumSetupInstances;

/* Look in <combaseapi.h> for the C expansion of 'DECLARE_INTERFACE_()'.
 */
#undef  INTERFACE
#define INTERFACE   ISetupInstance
DECLARE_INTERFACE_ (ISetupInstance, IUnknown)
{
  STDMETHOD  (QueryInterface) (THIS_ REFIID, void**);
  STDMETHOD_ (HRESULT, GetInstanceId) (THIS_ BSTR *pbstrInstanceId);
  STDMETHOD_ (HRESULT, GetInstallDate) (THIS_ LPFILETIME pInstallDate);
  STDMETHOD_ (HRESULT, GetInstallationName) (THIS_ BSTR *pbstrInstallationName);
  STDMETHOD_ (HRESULT, GetInstallationPath) (THIS_ BSTR *pbstrInstallationPath);
  STDMETHOD_ (HRESULT, GetInstallationVersion) (THIS_ BSTR *pbstrInstallationVersion);
  STDMETHOD_ (HRESULT, GetDisplayName) (THIS_ LCID lcid, BSTR *pbstrDisplayName);
  STDMETHOD_ (HRESULT, GetDescription) (THIS_ LCID lcid, BSTR *pbstrDescription);
  STDMETHOD_ (HRESULT, ResolvePath) (THIS_ LPCOLESTR pwszRelativePath, BSTR *pbstrAbsolutePath);
  STDMETHOD_ (HRESULT, Release) (THIS);
};

#undef  INTERFACE
#define INTERFACE   ISetupInstance2
DECLARE_INTERFACE_ (ISetupInstance2, IUnknown)
{
  STDMETHOD  (QueryInterface) (THIS_ REFIID, void**);
  STDMETHOD_ (HRESULT, GetDisplayName) (THIS_ LCID lcid, BSTR *pbstrDisplayName);
  STDMETHOD_ (HRESULT, GetState) (THIS_ InstanceState *pState);
  STDMETHOD_ (HRESULT, GetPackages) (THIS_ SAFEARRAY **ppsaPackages);
  STDMETHOD_ (HRESULT, GetProduct) (THIS_ ISetupPackageReference **ppPackage);
  STDMETHOD_ (HRESULT, GetProductPath) (THIS_ BSTR *pbstrProductPath);
  STDMETHOD_ (HRESULT, GetErrors) (THIS_ ISetupErrorState **ppErrorState);
  STDMETHOD_ (HRESULT, IsLaunchable) (THIS_ VARIANT_BOOL *pfIsLaunchable);
  STDMETHOD_ (HRESULT, IsComplete) (THIS_ VARIANT_BOOL *pfIsComplete);
  STDMETHOD_ (HRESULT, GetProperties) (THIS_ ISetupPropertyStore **ppProperties);
  STDMETHOD_ (HRESULT, GetEnginePath) (THIS_ BSTR *pbstrEnginePath);
  STDMETHOD_ (HRESULT, Release) (THIS);
};

#undef  INTERFACE
#define INTERFACE   ISetupPackageReference
DECLARE_INTERFACE_ (ISetupPackageReference, IUnknown)
{
  STDMETHOD  (QueryInterface) (THIS_ REFIID, void**);
  STDMETHOD_ (HRESULT, GetId) (THIS_ BSTR *pbstrId);
  STDMETHOD_ (HRESULT, GetVersion) (THIS_ BSTR *pbstrVersion);
  STDMETHOD_ (HRESULT, GetChip) (THIS_ BSTR *pbstrChip);
  STDMETHOD_ (HRESULT, GetLanguage) (THIS_ BSTR *pbstrLanguage);
  STDMETHOD_ (HRESULT, GetBranch) (THIS_ BSTR *pbstrBranch);
  STDMETHOD_ (HRESULT, GetType) (THIS_ BSTR *pbstrType);
  STDMETHOD_ (HRESULT, GetUniqueId) (THIS_ BSTR *pbstrUniqueId);
  STDMETHOD_ (HRESULT, GetIsExtension) (THIS_ VARIANT_BOOL *pfIsExtension);
  STDMETHOD_ (HRESULT, Release) (THIS);
};

#undef  INTERFACE
#define INTERFACE   ISetupErrorState
DECLARE_INTERFACE_ (ISetupErrorState, IUnknown)
{
  STDMETHOD  (QueryInterface) (THIS_ REFIID, void**);
  STDMETHOD_ (HRESULT, GetFailedPackages) (THIS_ SAFEARRAY **ppsaFailedPackages);
  STDMETHOD_ (HRESULT, GetSkippedPackages) (THIS_ SAFEARRAY **ppsaSkippedPackages);
  STDMETHOD_ (HRESULT, Release) (THIS);
};

#undef  INTERFACE
#define INTERFACE   ISetupPropertyStore
DECLARE_INTERFACE_ (ISetupPropertyStore, IUnknown)
{
  STDMETHOD  (QueryInterface) (THIS_ REFIID, void**);
  STDMETHOD_ (HRESULT, GetNames) (THIS_ SAFEARRAY **ppsaNames);
  STDMETHOD_ (HRESULT, GetValue) (THIS_ LPCOLESTR pwszName, VARIANT *pvtValue);
  STDMETHOD_ (HRESULT, Release) (THIS);
};

#undef  INTERFACE
#define INTERFACE   IEnumSetupInstances
DECLARE_INTERFACE_ (IEnumSetupInstances, IUnknown)
{
  STDMETHOD  (QueryInterface) (THIS_ REFIID, void**);
  STDMETHOD_ (HRESULT, Next) (THIS_ ULONG celt, ISetupInstance **rgelt, ULONG *pceltFetched);
  STDMETHOD_ (HRESULT, Skip) (THIS_ ULONG celt);
  STDMETHOD_ (HRESULT, Reset) (THIS);
  STDMETHOD_ (HRESULT, Clone) (THIS_ IEnumSetupInstances **ppenum);
  STDMETHOD_ (HRESULT, Release) (THIS);
};

#undef  INTERFACE
#define INTERFACE   ISetupConfiguration
DECLARE_INTERFACE_ (ISetupConfiguration, IUnknown)
{
  STDMETHOD  (QueryInterface) (THIS_ REFIID, void**);
  STDMETHOD_ (HRESULT, EnumInstances) (IEnumSetupInstances **ppEnumInstances);
  STDMETHOD_ (HRESULT, GetInstanceForCurrentProcess) (ISetupInstance **ppInstance);
  STDMETHOD_ (HRESULT, GetInstanceForPath) (LPCWSTR wzPath, ISetupInstance **ppInstance);
  STDMETHOD_ (HRESULT, Release) (THIS);
};

#undef  INTERFACE
#define INTERFACE   ISetupConfiguration2
DECLARE_INTERFACE_ (ISetupConfiguration2, ISetupConfiguration)
{
  STDMETHOD  (QueryInterface) (THIS_ REFIID, void**);
  STDMETHOD_ (HRESULT, EnumAllInstances) (THIS_ IEnumSetupInstances **ppEnumInstances);
  STDMETHOD_ (HRESULT, Release) (THIS);
};

static bool get_install_name (void *This, ISetupInstance2 *inst)
{
  OLECHAR *name;
  HRESULT  hr = (*inst->lpVtbl->GetDisplayName) (This, LOCALE_USER_DEFAULT, &name);
  char     str [1000];

  if (FAILED(hr))
  {
    TRACE (1, "hr: %s\n", win_strerror(hr));
    return (false);
  }
  TRACE (1, "name: %" WIDESTR_FMT "\n", name);
  wchar_to_mbchar (str, sizeof(str), name);
  SysFreeString (name);
  return (true);
}

static bool get_install_version (void *This, ISetupInstance *inst)
{
  OLECHAR *ver;
  HRESULT  hr = (*inst->lpVtbl->GetInstallationVersion) (This, &ver);
  char     str [1000];

  if (FAILED(hr))
  {
    TRACE (1, "hr: %s\n", win_strerror(hr));
    return (false);
  }
  TRACE (1, "ver: %" WIDESTR_FMT "\n", ver);
  wchar_to_mbchar (str, sizeof(str), ver);
  SysFreeString (ver);
  return (true);
}

static bool get_install_path (void *This, ISetupInstance *inst)
{
  OLECHAR *path;
  HRESULT  hr = (*inst->lpVtbl->GetInstallationPath) (This, &path);
  char     str [1000];

  if (FAILED(hr))
  {
    TRACE (1, "hr: %s\n", win_strerror(hr));
    return (false);
  }
  TRACE (1, "path: %" WIDESTR_FMT "\n", path);
  wchar_to_mbchar (str, sizeof(str), path);
  SysFreeString (path);
  return (true);
}

static bool get_installed_packages (void *This, ISetupInstance2 *inst)
{
  SAFEARRAY              *sa_packages = NULL;
  ISetupPackageReference *package  = NULL;
  IUnknown              **packages = NULL;
  LONG    i, ub = 0;
  HRESULT hr = (*inst->lpVtbl->GetPackages) (This, &sa_packages);
  char    str [1000];

  if (FAILED(hr))
  {
    TRACE (1, "hr: %s\n", win_strerror(hr));
    goto error;
  }

  hr = SafeArrayAccessData (sa_packages, (void**)&packages);
  if (FAILED(hr))
  {
    TRACE (1, "hr: %s\n", win_strerror(hr));
    goto error;
  }

  if (FAILED(SafeArrayGetUBound(sa_packages, 1, &ub)))
  {
    TRACE (1, "SafeArrayGetUBound() failed\n");
    TRACE (1, "hr: %s\n", win_strerror(hr));
    goto error;
  }

  for (i = 0; i < ub; ++i)
  {
    OLECHAR *id = NULL;

    package = NULL;

    hr = (*packages[i]->lpVtbl->QueryInterface) (This, g_iid, (void**)&package);
    if (FAILED(hr))
    {
      TRACE (1, "QueryInterface() for package %ld failed\n", i);
      TRACE (1, "hr: %s\n", win_strerror(hr));
      goto error;
    }

    hr = (*package->lpVtbl->GetId) (This, &id);
    if (FAILED(hr))
    {
      TRACE (1, "GetId() for package %ld failed\n", i);
      TRACE (1, "hr: %s\n", win_strerror(hr));
      goto error;
    }

    wchar_to_mbchar (str, sizeof(str), id);
    SysFreeString (id);
    TRACE (1, "id: %s\n", str);

    (*package->lpVtbl->Release) (package);
  }

  SafeArrayUnaccessData (sa_packages);
  SafeArrayDestroy (sa_packages);
  return (true);

error:
  if (package)
    (*package->lpVtbl->Release) (package);

  if (sa_packages)
  {
    if (packages)
       SafeArrayUnaccessData (sa_packages);
    SafeArrayDestroy (sa_packages);
  }
  return (false);
}

/**
 * This function will load `Microsoft.VisualStudio.Setup.Configuration.Native.dll`
 * to do the required work via COM.
 */
static bool find_all_instances (void *This)
{
  ISetupConfiguration2 *sc2   = NULL;
  IEnumSetupInstances  *enm   = NULL;
  ISetupInstance       *inst  = NULL;
  ISetupInstance2      *inst2 = NULL;
  ISetupConfiguration  *sc    = (ISetupConfiguration*) This;
  ULONG                 fetched;
  HRESULT               hr = (*sc->lpVtbl->QueryInterface) (This, g_iid, (void**)&sc2);

  TRACE (1, "sc->lpVtbl: 0x%p\n", sc->lpVtbl);

  if (FAILED(hr))
  {
    TRACE (1, "hr: %s\n", win_strerror(hr));
    goto error;
  }

  hr = (*sc2->lpVtbl->EnumAllInstances) (This, &enm);
  if (FAILED(hr))
  {
    TRACE (1, "hr: %s\n", win_strerror(hr));
    goto error;
  }
  if (enm == NULL)
  {
    TRACE (1, "sc2->lpVtbl->EnumAllInstances() failed, hr: %s\n", win_strerror(hr));
    goto error;
  }

  while (SUCCEEDED((*enm->lpVtbl->Next)(This, 1, &inst, &fetched)) && fetched)
  {
    hr = (*inst->lpVtbl->QueryInterface) (This, g_iid, (void**)&inst2);

    if (FAILED(hr) ||
        !get_install_name(This, inst2) ||
        !get_install_version(This, inst) ||
        !get_install_path(This, inst) ||
        !get_installed_packages(This, inst2))
      goto error;
  }

  (*enm->lpVtbl->Release) (This);
  (*sc2->lpVtbl->Release) (This);
  (*sc->lpVtbl->Release) (This);
  return (true);

error:
  if (inst2 && inst2->lpVtbl)
    (*inst2->lpVtbl->Release) (This);

  if (enm && enm->lpVtbl)
    (*enm->lpVtbl->Release) (This);

  if (sc2 && sc2->lpVtbl)
    (*sc2->lpVtbl->Release) (This);

  if (sc && sc->lpVtbl)
    (*sc->lpVtbl->Release) (This);

  return (false);
}

/*
 * Handle runtime exceptions inside `Microsoft.VisualStudio.Setup.Configuration.Native.dll`
 */
static LONG WINAPI handle_exception (EXCEPTION_POINTERS *_exc)
{
  EXCEPTION_RECORD *exc = _exc->ExceptionRecord;
  char   err [1024], *p = err;
  size_t left = sizeof(err);
  int    len;

  len = snprintf (p, left,
                  "ExceptionCode:    0x%lX\n"
                  "ExceptionAddress: 0x%p",
                  exc->ExceptionCode, exc->ExceptionAddress);
  p    += len;
  left -= len;

  if (exc->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      exc->NumberParameters >= 2)
  {
    snprintf (p, left, "\nAccess violation: %s operation at address 0x%p",
              exc->ExceptionInformation[0] ? "write" : "read",
              (void*)exc->ExceptionInformation[1]);
  }

  TRACE (0, "%s\n", err);
  g_crashinfo = STRDUP (err);

  if (IsDebuggerPresent())
     return (EXCEPTION_CONTINUE_SEARCH);
  return (EXCEPTION_EXECUTE_HANDLER);
}

/*
 * Finds all installed versions of Visual Studio.
 *
 * This function will initialize COM temporarily.
 * To avoid impact on other parts of your application,
 * use a new thread to make this call.
 */
bool find_vstudio_init (void)
{
  bool    rc = false;
  HRESULT hr = CoInitializeEx (NULL, COINIT_MULTITHREADED);
  void   *This = NULL;

  /* Increase debug-level when running on AppVeyor (to see more details).
   */
  if (opt.under_appveyor)
     opt.debug = max (opt.debug, 1);

  build_GUIDs();
  g_iid = &IID_ISetupConfiguration;

  TRACE_NL (1);

  if (hr == RPC_E_CHANGED_MODE)
  {
    TRACE (1, "hr: RPC_E_CHANGED_MODE\n");
    hr = CoInitializeEx (NULL, COINIT_APARTMENTTHREADED);
  }

  if (FAILED(hr))
  {
    TRACE (1, "hr: %s\n", win_strerror(hr));
    goto error;
  }

  hr = CoCreateInstance ((const IID*)&CLSID_SetupConfiguration,
                         NULL,
                         CLSCTX_INPROC_SERVER,
                         g_iid,
                         &This);

  if (FAILED(hr) || !This)
  {
    if (hr == REGDB_E_CLASSNOTREG)
         TRACE (1, "hr: REGDB_E_CLASSNOTREG\n");
    else TRACE (1, "hr: %s\n", win_strerror(hr));
  }
  else
  {
    SetUnhandledExceptionFilter (handle_exception);
    rc = find_all_instances (This);
    if (g_crashinfo)
       rc = false;

    FREE (g_crashinfo);
    SetUnhandledExceptionFilter (NULL);
  }
  TRACE_NL (1);

error:
  CoUninitialize();
  return (rc);
}
#endif /* _MSC_VER */
