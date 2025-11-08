/**\file    misc.c
 * \ingroup Misc
 * \brief   Various support functions for EnvTool
 * \note    fnmatch(), basename() and dirname() are taken from djgpp and modified.
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <io.h>
#include <wchar.h>
#include <windows.h>
#include <wincon.h>
#include <winioctl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <accctrl.h>
#include <aclapi.h>
#include <sddl.h>

/*
 * Suppress warning:
 *   imagehlp.h(1873): warning C4091: 'typedef ': ignored on left of '' when no variable is declared
 */
#ifdef _MSC_VER
#pragma warning (disable:4091)
#endif

#include <imagehlp.h>

#include "color.h"
#include "envtool.h"
#include "dirlist.h"

#ifndef IMAGE_FILE_MACHINE_ALPHA
#define IMAGE_FILE_MACHINE_ALPHA 0x123456
#endif

#ifndef KEY_WOW64_32KEY
#define KEY_WOW64_32KEY          0x0200
#endif

#ifndef KEY_WOW64_64KEY
#define KEY_WOW64_64KEY          0x0100
#endif

/** From Windows-Kit's <ctype.h> comment:
 *   The C Standard specifies valid input to a ctype function ranges from -1 to 255.
 */
#define VALID_CH(c)   ((c) >= -1 && (c) <= 255)

#define TOUPPER(c)    toupper ((int)(c))
#define TOLOWER(c)    tolower ((int)(c))

static HANDLE kernel32_hnd, userenv_hnd;

#if !defined(_CRTDBG_MAP_ALLOC)
  /**
   * Internal memory-leak tracker that is *only* enabled in
   * `_RELEASE` builds.
   * Since `_DEBUG` builds (on MSVC at least), should be good enough.
   *
   * All these allocations starts with this header.
   *
   * \struct mem_head
   */
  struct mem_head {
         unsigned long    marker;     /**< Magic marker. Equals MEM_MARKER or MEM_FREED */
         size_t           size;       /**< length of allocation including the size of this header */
         char             file [20];  /**< allocation happened in file */
         unsigned         line;       /**< and at line */
         struct mem_head *next;       /**< size is 36 bytes = 24h */
       };

  static struct mem_head *mem_list = NULL; /**< The linked list of our allocations */
  static size_t mem_reallocs    = 0;       /**< Number of realloc() */
  static DWORD  mem_max         = 0;       /**< Max bytes allocated at one time */
  static DWORD  mem_allocated   = 0;       /**< Total bytes allocated */
  static DWORD  mem_deallocated = 0;       /**< Bytes deallocated */
  static size_t mem_allocs      = 0;       /**< Number of allocations */
  static size_t mem_frees       = 0;       /**< Number of mem-frees */

  /**
   * Add this memory block to the \ref mem_list.
   * \param[in] m    the block to add.
   * \param[in] file the file where the allocation occured.
   * \param[in] line the line of the file where the allocation occured.
   */
  static void add_to_mem_list (struct mem_head *m, const char *file, unsigned line)
  {
    m->next = mem_list;
    m->line = line;
    _strlcpy (m->file, file, sizeof(m->file));
    mem_list = m;
    mem_allocated += (DWORD) m->size;
    if (mem_allocated > mem_max)
       mem_max = mem_allocated;
    mem_allocs++;
  }

  /**
   * \def IS_MARKER(m)
   * Verify that the memory block `m` is valid; <br>
   * either marked as used (`MEM_MARKER`) or as freed (`MEM_FREED`).
   */
  #define IS_MARKER(m) ( ( (m)->marker == MEM_MARKER) || ( (m)->marker == MEM_FREED) )

  /**
   * Delete this memory block from the \ref mem_list.
   * \param[in] m    the block to delete.
   * \param[in] line the line where this function was called.
   */
  static void del_from_mem_list (const struct mem_head *m, unsigned line)
  {
    struct mem_head *m1, *prev;
    unsigned i, max_loops = (unsigned) (mem_allocs - mem_frees);

    ASSERT (max_loops > 0);

    for (m1 = prev = mem_list, i = 1; m1 && i <= max_loops; m1 = m1->next, i++)
    {
      if (!IS_MARKER(m1))
         FATAL ("m->marker: 0x%08lX munged from line %u!?\n", m1->marker, line);

      if (m1 != m)
         continue;

      if (m == mem_list)
           mem_list   = m1->next;
      else prev->next = m1->next;
      mem_deallocated += (DWORD) m->size;
      mem_allocated   -= (DWORD) m->size;
      break;
    }
    if (i > max_loops)
       FATAL ("max-loops (%u) exceeded. mem_list munged from line %u!?\n",
              max_loops, line);
  }
#endif  /* _CRTDBG_MAP_ALLOC */

#ifdef NOT_USED
/**
 * Returns a pointer to the `mem_head` for this `ptr`.
 *
 * \param[in] ptr the pointer to get the `mem_head` for.
 * \retval    the `mem_head`
 */
static struct mem_head *mem_list_get_head (void *ptr)
{
  struct mem_head *m;

  for (m = mem_list; m; m = m->next)
      if (m == ptr)
         return (m);
  return (NULL);
}
#endif

/**
 * We need to use `K32GetModuleFileNameExA()`, `IsWow64Process()` and
 * `SetThreadErrorMode()` dynamically (since these are not available on Win-XP).
 * Try to load them from `kernel32.dll` only once.
 *
 * \typedef func_GetModuleFileNameEx
 * \typedef func_SetThreadErrorMode
 * \typedef func_IsWow64Process
 * \def WINAPI __stdcall
 */
typedef BOOL (WINAPI *func_GetModuleFileNameEx) (HANDLE proc, DWORD flags, char *fname, DWORD size);
typedef BOOL (WINAPI *func_SetThreadErrorMode) (DWORD new_mode, DWORD *old_mode);
typedef BOOL (WINAPI *func_IsWow64Process) (HANDLE proc, BOOL *wow64);

/**
 * Window Vista's+ `SearchPath()` (in `kernel32.dll`) uses this function while
 * searching for .EXEs.
 *
 * \todo Could be used in searchpath_internal()?
 *
 * \see
 *   https://msdn.microsoft.com/en-us/library/ms684269
 */
typedef BOOL (WINAPI *func_NeedCurrentDirectoryForExePathA) (const char *exe_name);

/**
 * \typedef func_ExpandEnvironmentStringsForUserA
 *
 * Since these functions are not available on Win-XP, dynamically load "userenv.dll"
 * and get the function-pointer to `ExpandEnvironmentStringsForUserA()`.
 *
 * \note The MSDN documentation for `ExpandEnvironmentStringsForUser`()` is
 *       wrong. The return-value is *not* a `BOOL`, but it returns the length
 *       of the expanded buffer (similar to `ExpandEnvironmentStrings()`).
 *  \see https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-expandenvironmentstringsa
 *  \see https://learn.microsoft.com/en-gb/windows/win32/api/userenv/nf-userenv-expandenvironmentstringsforusera
 */
typedef DWORD (WINAPI *func_ExpandEnvironmentStringsForUserA) (
                       HANDLE      token,
                       const char *src,
                       char       *dest,
                       DWORD       dest_size);

typedef BOOL (WINAPI *func_CreateEnvironmentBlock) (
                      void  **env_block,
                      HANDLE  token,
                      BOOL    inherit);

typedef BOOL (WINAPI *func_DestroyEnvironmentBlock) (
                      void *environment);

static func_GetModuleFileNameEx              p_GetModuleFileNameEx;
static func_SetThreadErrorMode               p_SetThreadErrorMode;
static func_IsWow64Process                   p_IsWow64Process;
static func_NeedCurrentDirectoryForExePathA  p_NeedCurrentDirectoryForExePathA; /* used only in tests.c */
static func_ExpandEnvironmentStringsForUserA p_ExpandEnvironmentStringsForUserA;
static func_CreateEnvironmentBlock           p_CreateEnvironmentBlock;
static func_DestroyEnvironmentBlock          p_DestroyEnvironmentBlock;

/**
 * Initialise the above function pointers once.
 */
void init_misc (void)
{
  static bool done = false;

  if (done)
     return;

  kernel32_hnd = LoadLibrary ("kernel32.dll");
  userenv_hnd  = LoadLibrary ("userenv.dll");

  if (!kernel32_hnd)
     TRACE (1, "Failed to load kernel32.dll; %s\n", win_strerror(GetLastError()));

  if (!userenv_hnd)
     TRACE (1, "Failed to load userenv.dll; %s\n", win_strerror(GetLastError()));

  if (kernel32_hnd)
  {
    p_GetModuleFileNameEx             = GETPROCADDRESS (func_GetModuleFileNameEx, kernel32_hnd, "K32GetModuleFileNameExA");
    p_SetThreadErrorMode              = GETPROCADDRESS (func_SetThreadErrorMode, kernel32_hnd, "SetThreadErrorMode");
    p_IsWow64Process                  = GETPROCADDRESS (func_IsWow64Process, kernel32_hnd, "IsWow64Process");
    p_NeedCurrentDirectoryForExePathA = GETPROCADDRESS (func_NeedCurrentDirectoryForExePathA, kernel32_hnd, "NeedCurrentDirectoryForExePathA");
  }

  if (userenv_hnd)
  {
    p_ExpandEnvironmentStringsForUserA = GETPROCADDRESS (func_ExpandEnvironmentStringsForUserA,
                                                         userenv_hnd,
                                                         "ExpandEnvironmentStringsForUserA");

    p_CreateEnvironmentBlock = GETPROCADDRESS (func_CreateEnvironmentBlock,
                                               userenv_hnd,
                                               "CreateEnvironmentBlock");

    p_DestroyEnvironmentBlock = GETPROCADDRESS (func_DestroyEnvironmentBlock,
                                                userenv_hnd,
                                                "DestroyEnvironmentBlock");

  }
  done = true;
}

/**
 * Unload `userenv.dll` and `kernel32.dll` when the above function pointes are
 * no longer needed. Do it in the reverse order of `LoadLibrary()`.
 */
void exit_misc (void)
{
#if 0    /* Avoid some mysterious crash inside 'userenv.dll'. Doesn't happen every time. */
  if (userenv_hnd)
     FreeLibrary (userenv_hnd);
#endif
  if (kernel32_hnd)
     FreeLibrary (kernel32_hnd);
  kernel32_hnd = userenv_hnd = NULL;
}

/**
 * If given a `fname` without any extension, open the `fname` and check if
 * there's a she-bang line on 1st line.
 *
 * Accepts `"#!/xx"` or `"#! /xx"` and even stuff like:
 *  ```
 *  #!f:\ProgramFiler\Python36\python3.EXE
 *  ```
 * that some packaging tools will generate.
 */
const char *check_if_shebang (const char *fname)
{
  static char shebang [_MAX_PATH];
  const char *ext = get_file_ext (fname);
  char  *p;
  FILE  *f;

  /* Return NULL if `fname` have an extension.
   */
  if (*ext)
     return (NULL);

  memset (&shebang, '\0', sizeof(shebang));
  f = fopen (fname, "rb");
  if (f)
  {
    shebang[0] = (char) fgetc (f);
    shebang[1] = (char) fgetc (f);
    shebang[2] = (char) fgetc (f);
    if (shebang[2] == ' ')
       shebang[2] = (char) fgetc (f);
    fread (shebang+3, 1, sizeof(shebang)-3, f);
    fclose (f);
  }

  if (strncmp(shebang, "#!", 2))
     return (NULL);

  /** If it's a Unix file with 2 "\r\r" in the `shebang[]` buffer,
   *  we cannot use `str_strip_nl()`. That will only remove the last
   *  `\r`. Look for the 1st `\n` or `\r` and remove them.
   */
  p = strchr (shebang, '\n');
  if (p)
     *p = '\0';
  p = strchr (shebang, '\r');
  if (p)
     *p = '\0';

  /** Drop any space; this is usually arguments for this
   *  specific interpreter.
   */
  p = strchr (shebang, ' ');
  if (strncmp(shebang, "#!/usr/bin/env ", 15) && p)
     *p = '\0';

  TRACE (1, "shebang: \"%s\"\n", shebang);
  return (shebang);
}

/**
 * Open a `fname` and check if there's a `"PK\3\4"` signature in the first 4 bytes.
 */
bool check_if_zip (const char *fname)
{
  static const char header [4] = { 'P', 'K', 3, 4 };
  const char *ext;
  char   buf [sizeof(header)];
  FILE  *f;
  bool   rc = false;

  /** Return 0 if extension is neither `.egg` nor `.zip` nor `.whl`.
   */
  ext = get_file_ext (fname);
  if (stricmp(ext, "egg") && stricmp(ext, "whl") && stricmp(ext, "zip"))
     return (false);

  f = fopen (fname, "rb");
  if (f)
  {
    rc = (fread(&buf, 1, sizeof(buf), f) == sizeof(buf) &&
          !memcmp(&buf, &header, sizeof(buf)));
    fclose (f);
  }
  TRACE (2, "\"%s\" is %sa ZIP-file.\n", fname, rc ? "" : "not ");
  return (rc);
}

/**
 * Open a `fname` and check if there's a `"GZIP"` or `"TAR.GZ"` signature in header.
 *
 * Gzip format:
 *   https://commandlinefanatic.com/cgi-bin/showarticle.cgi?article=art053
 */
bool check_if_gzip (const char *fname)
{
  static const BYTE header1 [4] = { 0x1F, 0x8B, 0x08, 0x08 };
  static const BYTE header2 [4] = { 0x1F, 0x8B, 0x08, 0x00 };
  const char *ext;
  char         buf [sizeof(header1)];
  FILE        *f;
  bool         is_gzip, is_tgz;
  bool         rc = false;

  /** Accept only `.gz`, `.tgz` or `.tar.gz` extensions.
   */
  ext = get_file_ext (fname);
  is_gzip = (stricmp(ext, "gz") == 0);
  is_tgz  = (stricmp(ext, "tgz") == 0 || stricmp(ext, "tar.gz") == 0);

  if (!is_gzip && !is_tgz)
  {
    TRACE_NL (2);
    TRACE (2, "\"%s\" has wrong extension: '%s'.\n", fname, ext);
    return (false);
  }

  f = fopen (fname, "rb");
  if (f)
  {
    if (fread(&buf, 1, sizeof(buf), f) == sizeof(buf) &&
        (!memcmp(&buf, &header1, sizeof(buf)) || !memcmp(&buf, &header2, sizeof(buf))) )
       rc = true;
    fclose (f);
  }
  TRACE_NL (2);
  TRACE (2, "\"%s\" is %sa GZIP-file.\n", fname, rc ? "": "not ");
  return (rc);
}

/**
 * Helper variable and callback function for `get_gzip_link()`.
 */
static char gzip_link_name [_MAX_PATH];

static int gzip_cb (char *buf, int index)
{
  if (index == 0 && strlen(buf) < sizeof(gzip_link_name) - 3 &&
      sscanf(buf, ".so %s", gzip_link_name) == 1)
     return (1);
  return (-1);  /* causes `popen_run()` to quit */
}

/**
 * Open a GZIP-file and extract first line to check if it contains a
 * `.so gzip_link_name`. This is typical for CygWin man-pages.
 *
 * Return result as `gzip_link_name`. I.e. without any dir-name since
 * the `gzip_link_name` can be anywhere on `%MANPATH%`.
 */
const char *get_gzip_link (const char *file)
{
  static char gzip_exe [_MAX_PATH];
  static bool done = false;
  const char *p;

  if (!done)
  {
    p = searchpath ("gzip.exe", "PATH");
    if (p)
       slashify2 (gzip_exe, p, '\\');
    done = true;
  }

  if (!gzip_exe[0])
     return (NULL);

  gzip_link_name[0] = '\0';

  if (popen_run(gzip_cb, gzip_exe, "-cd %s 2> NUL", file) > 0)
  {
    TRACE (2, "gzip_link_name: \"%s\".\n", gzip_link_name);
    return slashify2 (gzip_link_name, gzip_link_name, opt.show_unix_paths ? '/' : '\\');
  }
  return (NULL);
}

/**
 * Open a raw MAN-file and check if first line contains a
 * `.so real-file-name`. This is typical for CygWin man-pages.
 *
 * Return result as `<dir_name>/real-file-name`. Which is just an
 * assumption; the `real-file-name` can be anywhere on `%MANPATH%`.
 * Or the `real-file-name` can be `real-file-name.gz`.
 */
const char *get_man_link (const char *file)
{
  char  buf [_MAX_PATH];
  FILE *f = fopen (file, "r");

  if (!f)
     return (NULL);

  memset (buf, '\0', sizeof(buf));
  if (fread(&buf, 1, sizeof(buf)-1, f) > 0 && !strncmp(buf, ".so ", 4))
  {
    static char fqfn_name [_MAX_PATH];
    char       *dir_name = dirname (file);
    const char *base = basename (str_strip_nl(buf+4));

    fclose (f);
    TRACE_NL (1);
    TRACE (1, "get_man_link: \"%s\", dir_name: \"%s\".\n", base, dir_name);
    snprintf (fqfn_name, sizeof(fqfn_name), "%s%c%s", dir_name, DIR_SEP, base);
    FREE (dir_name);
    if (opt.show_unix_paths)
       return slashify2 (fqfn_name, fqfn_name, '/');
    return (fqfn_name);
  }

  fclose (f);
  return (NULL);
}

/**
 * Open a file and check if first line contains a
 * `!<symlink>real-file-name`. This is typical for several CygWin programs.
 *
 * Return result as `<dir_name>/real-file-name`. Which is just an
 * assumption; the `real-file-name` can be anywhere on `%MANPATH%`.
 */
const char *get_sym_link (const char *file)
{
  char  buf [_MAX_PATH];
  FILE *f = fopen (file, "r");

  if (!f)
     return (NULL);

  memset (buf, '\0', sizeof(buf));
  if (fread(&buf, 1, sizeof(buf)-1, f) > 0 && !strncmp(buf, "!<symlink>", 10))
  {
    static char fqfn_name [_MAX_PATH];
    char       *dir_name = dirname (file);
    const char *base = basename (str_strip_nl(buf+10));

    fclose (f);
    TRACE_NL (1);
    snprintf (fqfn_name, sizeof(fqfn_name), "%s%c%s", dir_name, DIR_SEP, base);
    TRACE (1, "get_sym_link: \"%s\"\n", fqfn_name);
    FREE (dir_name);
    if (opt.show_unix_paths)
       return slashify2 (fqfn_name, fqfn_name, '/');
    return (fqfn_name);
  }

  fclose (f);
  return (NULL);
}

/**
 * Open a `fname` and check for a Windows Shell-Link signature.
 */
bool check_if_shell_link (const char *fname)
{
  static const char header [4] = { 0x4C, 0, 0, 0 };
  const char *ext;
  char   buf [sizeof(header)];
  FILE  *f;
  bool   rc = false;

  /** Return false if extension is not `.lnk`.
   */
  ext = get_file_ext (fname);
  if (stricmp(ext, "lnk"))
     return (false);

  f = fopen (fname, "rb");
  if (f)
  {
    rc = (fread(&buf, 1, sizeof(buf), f) == sizeof(buf) &&
          !memcmp(&buf, &header, sizeof(buf)));
    fclose (f);
  }
  TRACE (2, "\"%s\" is %sa LNK-file.\n", fname, rc ? "" : "not ");
  return (rc);
}

/**
 * \todo Use the IShellLink component
 */
const char *get_shell_link (const char *file)
{
  (void) file;
  return (NULL);
}

bool check_if_cwd_in_search_path (const char *program)
{
  if (!p_NeedCurrentDirectoryForExePathA)
     return (true);
  return (*p_NeedCurrentDirectoryForExePathA) (program);
}

/**
 * Helper variables for `check_if_PE()`.
 */
static const IMAGE_DOS_HEADER *dos;
static const IMAGE_NT_HEADERS *nt;
static char  file_buf [sizeof(*dos) + 4*sizeof(*nt)];

static enum Bitness last_bitness = -1;

/**
 * Open a fname, read the optional header in PE-header.
 *  + For verifying it's signature.
 *  + Showing the version information (if any) in it's resources.
 */
bool check_if_PE (const char *fname, enum Bitness *bits)
{
  bool   is_exe, is_pe;
  bool   is_32Bit = false;
  bool   is_64Bit = false;
  size_t len = 0;
  FILE  *f = fopen (fname, "rb");

  last_bitness = bit_unknown;
  if (bits)
     *bits = bit_unknown;

  if (f)
  {
    len = fread (&file_buf, 1, sizeof(file_buf), f);
    fclose (f);
  }
  dos = NULL;
  nt  = NULL;

  if (len < sizeof(file_buf))
  {
    TRACE (3, "%s: failed fread(). errno: %d\n", fname, errno);
    return (false);
  }

  dos = (const IMAGE_DOS_HEADER*) file_buf;
  nt  = (const IMAGE_NT_HEADERS*) ((const BYTE*)file_buf + dos->e_lfanew);

  TRACE_NL (3);

  /* Probably not a PE-file at all.
   * Check `nt < file_buf` too in case `e_lfanew` folds `nt` to a negative value.
   */
  if ((char*)nt > file_buf + sizeof(file_buf) || (char*)nt < file_buf)
  {
    TRACE (3, "%s: NT-header at wild offset.\n", fname);
    return (false);
  }

  is_exe = (dos->e_magic  == IMAGE_DOS_SIGNATURE);  /* 'MZ' */
  is_pe  = (nt->Signature == IMAGE_NT_SIGNATURE);   /* 'PE\0\0 ' */

  if (is_pe)
  {
    is_32Bit = (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC);
    is_64Bit = (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);

    if (is_32Bit)
       last_bitness = bit_32;
    else if (is_64Bit)
       last_bitness = bit_64;
  }
  else if (is_exe)
  {
    const IMAGE_FILE_HEADER *fil_hdr = (const IMAGE_FILE_HEADER*) dos;

    if (fil_hdr->Machine != IMAGE_FILE_MACHINE_AMD64 &&
        fil_hdr->Machine != IMAGE_FILE_MACHINE_ALPHA &&
        fil_hdr->Machine != IMAGE_FILE_MACHINE_IA64)
      last_bitness = bit_16;  /* Just a guess */
  }

  if (bits)
     *bits = last_bitness;

  TRACE (3, "%s: is_exe: %d, is_pe: %d, is_32Bit: %d, is_64Bit: %d.\n",
         fname, is_exe, is_pe, is_32Bit, is_64Bit);
  return (is_exe && is_pe);
}

/**
 * Verify the checksum of last opened file above.
 * if `CheckSum == 0` is set to 0, it meants `"don't care"`
 */
bool verify_PE_checksum (const char *fname)
{
  const IMAGE_OPTIONAL_HEADER64 *oh;
  DWORD file_sum, header_sum, calc_chk_sum, rc;

  ASSERT (nt);

  if (last_bitness == bit_32)
     file_sum = nt->OptionalHeader.CheckSum;

  else if (last_bitness == bit_64)
  {
    oh = (const IMAGE_OPTIONAL_HEADER64*) &nt->OptionalHeader;
    file_sum = oh->CheckSum;
  }
  else
    return (false);

  TRACE (1, "last_bitness: %d, Opt magic: 0x%04X, file_sum: 0x%08lX\n",
         last_bitness, nt->OptionalHeader.Magic, (u_long)file_sum);

  rc = MapFileAndCheckSum ((PTSTR)fname, &header_sum, &calc_chk_sum);
  TRACE (1, "rc: %lu, 0x%08lX, 0x%08lX\n",
         (u_long)rc, (u_long)header_sum, (u_long)calc_chk_sum);
  return (file_sum == 0 || header_sum == calc_chk_sum);
}

/**
 * Check if running under WOW64; `Windows 32-bit on Windows 64-bit`.
 *
 * \see
 *  + https://en.wikipedia.org/wiki/WoW64
 *  + https://everything.explained.today/WoW64/
 */
bool is_wow64_active (void)
{
  bool rc    = false;
  BOOL wow64 = FALSE;

  init_misc();

#if (IS_WIN64 == 0)
  if (p_IsWow64Process &&
     (*p_IsWow64Process) (GetCurrentProcess(), &wow64))
    rc = wow64;
#endif

  TRACE (2, "IsWow64Process(): rc: %d, wow64: %d.\n", rc, wow64);
  return (rc);
}

/**
 * Return a `time_t` for a file in the `DATE_MODIFIED` response.
 * The `ft` is in UTC zone.
 */
time_t FILETIME_to_time_t (const FILETIME *ft)
{
  SYSTEMTIME st, lt;
  struct tm  tm;

  if (!FileTimeToSystemTime(ft, &st) ||
      !SystemTimeToTzSpecificLocalTime(NULL, &st, &lt))
     return (0);

  memset (&tm, '\0', sizeof(tm));
  tm.tm_year  = lt.wYear - 1900;
  tm.tm_mon   = lt.wMonth - 1;
  tm.tm_mday  = lt.wDay;
  tm.tm_hour  = lt.wHour;
  tm.tm_min   = lt.wMinute;
  tm.tm_sec   = lt.wSecond;
  tm.tm_isdst = -1;
  return mktime (&tm);
}

/**
 * Dynamic use of `K32GetModuleFileNameExA`.
 * Win-XP does not have this function. Hence load it dynamicallay similar
 * to above.
 *
 * \param[in] proc     The handle of the process to get the filname for.
 *                     Or NULL if our own process.
 * \param[in] filename Assumed to hold `MAX_PATH` characters.
 * \retval false       if this function is not available.
 * \retval             the return value from `(*p_GetModuleFileNameEx)()`.
 */
bool get_module_filename_ex (HANDLE proc, char *filename)
{
  init_misc();

  if (p_GetModuleFileNameEx)
     return (*p_GetModuleFileNameEx) (proc, 0, filename, _MAX_PATH);
  return (false);
}

/**
 * In 'getopt_parse()', a quoted 'content:"xx yy"' string have
 * lost their quotes ("). Therefore we must add these here by
 * creating a proper quoted 'content:\"xx yy\"' string.
 *
 * Called before call to `Everything_SetSearch()` and in `state_send_query()`.
 *
 * \todo Do this for other EveryThing functions that accept spaces.
 */
char *evry_raw_query (void)
{
  static char query [_MAX_PATH+100];
  const char *content  = strstr (opt.file_spec, " content:");
  size_t      cont_len = strlen (" content:");

  if (content && strchr(content+cont_len,' '))
  {
    snprintf (query, sizeof(query), "%.*s content:\"%s\"",
              (int)(content - opt.file_spec), opt.file_spec, content + cont_len);
    TRACE (2, "Creating quoted 'content:' query: '%s'\n", query);
    return (query);
  }
  return (opt.file_spec);
}

#if (_WIN32_WINNT >= 0x0500)
/**
 * `LookupAccountSid()` often returns `ERROR_NONE_MAPPED` for SIDs like: <br>
 * `S-1-5-21-3396768664-3120275132-3847281217-1001`.
 *
 * Cache this SID-string here since `ConvertSidToStringSid()` is
 * pretty expensive.
 */
static const char *sid_owner_cache (PSID sid)
{
  static bool done = false;
  static char sid_buf1 [200] = { '\0' };
  static char sid_buf2 [200] = { '\0' };

  if (!done)
  {
    DWORD sid_len = GetLengthSid (sid);
    char *sid_str = NULL;

    if (sid_len < sizeof(sid_buf1))
    {
      CopySid (sid_len, sid_buf1, sid);
      ConvertSidToStringSid (sid, &sid_str);
      if (sid_str && strlen(sid_str) < sizeof(sid_buf2))
         strcpy (sid_buf2, sid_str);
      TRACE (1, "sid_buf2: '%s', EqualSid(): %s.\n", sid_buf2, EqualSid(sid_buf1,sid) ? "Yes" : "No");
      LocalFree (sid_str);
    }
  }
  done = true;
  if (EqualSid(sid, sid_buf1) && sid_buf2[0])
     return (sid_buf2);
  return (NULL);
}
#endif /* _WIN32_WINNT >= 0x0500 */

/**
 * Get the Domain and Account name for a file or directory.
 *
 * Except for Cygwin where it tries to emulate what `ls -la` does.
 * But it doesn't quite show the same owner information.
 *
 * \param[in]     file            the file or directory to get the domain and account-name for.
 *
 * \param[in,out] domain_name_p   on input a caller-supplied `char **` pointer.
 *                                on output (if success), set to the domain-name of the owner.
 *                                Must be free()'d by the caller if set to non-NULL here.
 *
 * \param[in,out] account_name_p  on input a caller-supplied `char **` pointer.
 *                                on output (if success), set to the account-name of the owner.
 *                                Must be free()'d by the caller if set to non-NULL here.
 *
 * \param[out] sid_p              The `sid` for the `file` as obtained from `GetSecurityInfo()`.
 *                                The caller must use `LocalFree()` on `*sid_p` if non-NULL.
 *
 * Adapted from: <br>
 *   https://msdn.microsoft.com/en-us/library/windows/desktop/aa446629(v=vs.85).aspx
 *
 * \see
 *  + GetSecurityInfo()
 *    https://docs.microsoft.com/en-us/windows/desktop/api/aclapi/nf-aclapi-getsecurityinfo
 *  + LookupAccountSid()
 *    https://docs.microsoft.com/en-us/windows/desktop/api/winbase/nf-winbase-lookupaccountsida
 */
static bool get_file_owner_internal (const char *file, char **domain_name_p, char **account_name_p, void **sid_p)
{
  DWORD        rc, attr, err;
  bool         rc2, is_dir;
  DWORD        account_name_sz = 0;
  DWORD        domain_name_sz  = 0;
  char        *domain_name;
  char        *account_name;
  SID_NAME_USE sid_use = SidTypeUnknown;
  void        *sid_owner = NULL;
  HANDLE       hnd;
  void        *sid = NULL;
  const char  *sid_str;
  const char  *system_name = NULL;

  *domain_name_p  = NULL;
  *account_name_p = NULL;
  *sid_p          = NULL;

  attr = GetFileAttributes (file);
  is_dir = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);

  /* Get the handle of the file object.
   */
  hnd = CreateFile (file, GENERIC_READ, FILE_SHARE_READ,
                    NULL, OPEN_EXISTING,
                    is_dir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
                    NULL);

  if (hnd == INVALID_HANDLE_VALUE)
  {
    *account_name_p = STRDUP ("<No access>");
    TRACE (1, "CreateFile (\"%s\") error = %s\n", file, win_strerror(GetLastError()));
    return (false);
  }

  /* Get the owner SID of the file.
   */
  rc = GetSecurityInfo (hnd, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                        &sid_owner, NULL, NULL, NULL, &sid);

  *sid_p = sid;  /* LocalFree() in caller */
  CloseHandle (hnd);

  /* Check GetSecurityInfo() error condition.
   */
  if (rc != ERROR_SUCCESS)
  {
    TRACE (1, "GetSecurityInfo error = %s\n", win_strerror(GetLastError()));
    return (false);
  }

  /* First call to LookupAccountSid() to get the sizes of account/domain names.
   */
  rc2 = LookupAccountSid (system_name, sid_owner,
                          NULL, (DWORD*)&account_name_sz,
                          NULL, (DWORD*)&domain_name_sz,
                          &sid_use);

  TRACE (2, "sid_use: %d\n", sid_use);

  if (!rc2)
  {
    err = GetLastError();
    if (err != ERROR_INSUFFICIENT_BUFFER)
    {
      TRACE (1, "(1): Error in LookupAccountSid(): %s.\n", win_strerror(err));

#if (_WIN32_WINNT >= 0x0500)
      /**
       * If no mapping between SID and account-name, just return the
       * account-name as a SID-string. And no domain-name.
       *
       * How the SID is built up is documented here:
       *  + https://msdn.microsoft.com/en-us/library/windows/desktop/aa379597(v=vs.85).aspx
       *  + https://msdn.microsoft.com/en-us/library/windows/desktop/aa379649(v=vs.85).aspx
       */
      if (err == ERROR_NONE_MAPPED && sid_use == SidTypeUnknown)
      {
        sid_str = sid_owner_cache (sid_owner);
        if (sid_str)
        {
          *account_name_p = STRDUP (sid_str);
          *domain_name_p  = NULL;
          return (true);
        }
      }
#endif
      return (false);
    }
  }

  account_name = MALLOC (account_name_sz);
  if (!account_name)
     return (false);

  domain_name = MALLOC (domain_name_sz);
  if (!domain_name)
  {
    FREE (account_name);
    return (false);
  }

  /* Second call to LookupAccountSid() to get the account/domain names.
   */
  rc2 = LookupAccountSid (system_name,               /* name of local or remote computer */
                          sid_owner,                 /* security identifier */
                          account_name,              /* account name buffer */
                          (DWORD*)&account_name_sz,  /* size of account name buffer */
                          domain_name,               /* domain name */
                          (DWORD*)&domain_name_sz,   /* size of domain name buffer */
                          &sid_use);                 /* SID type */
  if (!rc2)
  {
    err = GetLastError();
    if (err == ERROR_NONE_MAPPED)
         TRACE (1, "Account owner not found for specified SID.\n");
    else TRACE (1, "(2) Error in LookupAccountSid(): %s.\n", win_strerror(err));
    FREE (domain_name);
    FREE (account_name);
    return (false);
  }

  *account_name_p = account_name;
  *domain_name_p  = domain_name;
  return (true);
}

bool get_file_owner (const char *file, char **domain_name_p, char **account_name_p)
{
  void *sid_p;
  char *dummy1 = NULL;
  char *dummy2 = NULL;
  bool  rc;

  if (!domain_name_p)
     domain_name_p = &dummy1;
  if (!account_name_p)
     account_name_p = &dummy2;

  rc = get_file_owner_internal (file, domain_name_p, account_name_p, &sid_p);

  if (sid_p)
     LocalFree (sid_p);
  FREE (dummy1);
  FREE (dummy2);
  return (rc);
}

#if defined(NOT_USED_YET)
/**
 * Get a list of hidden Windows accounts:
 * \see https://superuser.com/questions/248315/list-of-hidden-virtual-windows-user-accounts/638376
 *
 * \code
 * c:\> powershell "get-wmiobject -class win32_account -namespace 'root\cimv2' | sort caption | format-table caption, __CLASS, FullName"
 *
 *  caption                                     __CLASS             FullName
 *  -------                                     -------             --------
 *  INTEL-I7\Administrator                      Win32_UserAccount
 *  INTEL-I7\Administratorer                    Win32_Group
 *  INTEL-I7\Alle                               Win32_SystemAccount
 * \endcode
 *
 * Or: \code
 *  c:\> powershell "get-wmiobject -class win32_account -namespace 'root\cimv2' | sort caption"
 * \endcode
 * for more details.
 */
#endif

/**
 * Return true if directory is truly readable.
 */
bool is_directory_readable (const char *path)
{
  if (!is_directory(path))
     return (false);
  return is_directory_accessible (path, GENERIC_READ);
}

/**
 * Return true if directory is truly writeable.
 */
bool is_directory_writable (const char *path)
{
  if (!is_directory(path))
     return (false);
  return is_directory_accessible (path, GENERIC_WRITE);
}

/**
 * Based on
 *   http://blog.aaronballman.com/2011/08/how-to-check-access-rights/
 *
 * \see
 *  + GetFileSecurity()
 *    https://docs.microsoft.com/en-us/windows/desktop/api/winbase/nf-winbase-getfilesecuritya
 *  + OpenProcessToken()
 *    https://docs.microsoft.com/en-us/windows/desktop/api/processthreadsapi/nf-processthreadsapi-openprocesstoken
 */
bool is_directory_accessible (const char *path, DWORD access)
{
  bool     answer = false;
  DWORD    length = 0;
  HANDLE   token  = NULL;
  DWORD    access_flg;
  SECURITY_INFORMATION sec_info;
  SECURITY_DESCRIPTOR *security = NULL;

  /* Figure out buffer size. `GetFileSecurity()` should not succeed.
   */
  sec_info = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
  if (GetFileSecurity(path, sec_info, NULL, 0, &length))
     return (false);

  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
     return (false);

  security = CALLOC (1, length);
  if (!security)
     return (false);

  /* GetFileSecurity() should succeed.
   */
  if (!GetFileSecurity(path, sec_info, security, length, &length))
     return (false);

  access_flg = TOKEN_IMPERSONATE | TOKEN_QUERY | TOKEN_DUPLICATE | STANDARD_RIGHTS_READ;

  if (OpenProcessToken(GetCurrentProcess(), access_flg, &token))
  {
    HANDLE impersonated_token = NULL;

    if (DuplicateToken(token, SecurityImpersonation, &impersonated_token))
    {
      GENERIC_MAPPING mapping;
      PRIVILEGE_SET   privileges          = { 0 };
      DWORD           grantedAccess       = 0;
      DWORD           privilegesLength    = sizeof(privileges);
      BOOL            result              = FALSE;
      DWORD           genericAccessRights = access;

      mapping.GenericRead    = FILE_GENERIC_READ;
      mapping.GenericWrite   = FILE_GENERIC_WRITE;
      mapping.GenericExecute = FILE_GENERIC_EXECUTE;
      mapping.GenericAll     = FILE_ALL_ACCESS;

      MapGenericMask (&genericAccessRights, &mapping);
      if (AccessCheck(security, impersonated_token, genericAccessRights, &mapping,
                      &privileges, &privilegesLength, &grantedAccess, &result))
         answer = result;
      CloseHandle (impersonated_token);
    }
    CloseHandle (token);
  }
  FREE (security);
  return (answer);
}

/**
 * Helper functions for Registry access.
 */
REGSAM reg_read_access (void)
{
  REGSAM access = KEY_READ;

#if (IS_WIN64)
  access |= KEY_WOW64_32KEY;
#elif 0
  if (is_wow64_active())
     access |= KEY_WOW64_64KEY;
#endif

  return (access);
}

const char *reg_type_name (DWORD type)
{
  #define CHECK_TYPE(v) if (type == v) return (#v)

  CHECK_TYPE (REG_SZ);
  CHECK_TYPE (REG_MULTI_SZ);
  CHECK_TYPE (REG_EXPAND_SZ);
  CHECK_TYPE (REG_LINK);
  CHECK_TYPE (REG_BINARY);
  CHECK_TYPE (REG_DWORD);
  CHECK_TYPE (REG_RESOURCE_LIST);
  CHECK_TYPE (REG_DWORD_BIG_ENDIAN);
  CHECK_TYPE (REG_QWORD);
  return ("?");
}

const char *reg_top_key_name (HKEY key)
{
  #define CHECK_KEY(v) if (key == v) return (#v)

  CHECK_KEY (HKEY_LOCAL_MACHINE);
  CHECK_KEY (HKEY_CURRENT_USER);
  CHECK_KEY (HKEY_CLASSES_ROOT);
  CHECK_KEY (HKEY_CURRENT_CONFIG);
  CHECK_KEY (HKEY_USERS);
  return ("?");
}

const char *reg_access_name (REGSAM acc)
{
  #define ADD_VALUE(v)  { v, #v }

  static const search_list access[] = {
                           ADD_VALUE (KEY_CREATE_LINK),
                           ADD_VALUE (KEY_CREATE_SUB_KEY),
                           ADD_VALUE (KEY_ENUMERATE_SUB_KEYS),
                           ADD_VALUE (KEY_NOTIFY),
                           ADD_VALUE (KEY_QUERY_VALUE),
                           ADD_VALUE (KEY_SET_VALUE),
                           ADD_VALUE (KEY_WOW64_32KEY),
                           ADD_VALUE (KEY_WOW64_64KEY)
                         };

  acc &= ~STANDARD_RIGHTS_READ;  /* == STANDARD_RIGHTS_WRITE, STANDARD_RIGHTS_EXECUTE */

  if ((acc & KEY_ALL_ACCESS) == KEY_ALL_ACCESS)
     return ("KEY_ALL_ACCESS");
  return flags_decode (acc, access, DIM(access));
}

/**
 * Swap bytes a 32-bit value.
 */
DWORD reg_swap_long (DWORD val)
{
  return ((val & 0x000000FFU) << 24) |
         ((val & 0x0000FF00U) <<  8) |
         ((val & 0x00FF0000U) >>  8) |
         ((val & 0xFF000000U) >> 24);
}

/**
 * Removes end-of-line termination from a string.
 *
 * Works for "\r\n" or "\n" terminated strings only.
 * (not MAC "\r" format).
 */
char *str_strip_nl (char *str)
{
  char *p;

  p = strrchr (str, '\n');
  if (p)
     *p = '\0';

  p = strrchr (str, '\r');
  if (p)
     *p = '\0';
  return (str);
}

/**
 * Trim leading blanks (space/tab) from a string.
 */
char *str_ltrim (char *str)
{
  ASSERT (str != NULL);

  while (str[0] && str[1] && VALID_CH((int)str[0]) && isspace((int)str[0]))
       str++;
  return (str);
}

/**
 * Trim trailing blanks (space/tab) from a string.
 */
char *str_rtrim (char *str)
{
  size_t n;
  int    ch;

  ASSERT (str != NULL);
  n = strlen (str);
  if (n > 0)
  {
    n--;
    while (n)
    {
      ch = (int)str [n];
      if (VALID_CH(ch) && !isspace(ch))
         break;
      str[n--] = '\0';
    }
  }
  return (str);
}

/**
 * Trim leading and trailing blanks (space/tab) from a string.
 */
char *str_trim (char *s)
{
  return str_rtrim (str_ltrim(s));
}

/**
 * Return true if string `s1` starts with `s2`.
 *
 * Ignore casing of both strings.
 * And drop leading blanks in `s1` first.
 */
bool str_startswith (const char *s1, const char *s2)
{
  size_t s1_len;
  size_t s2_len;

  s1 = str_ltrim ((char*)s1);
  s1_len = strlen (s1);
  s2_len = strlen (s2);

  if (s2_len > s1_len)
     return (false);

  if (!strnicmp (s1, s2, s2_len))
     return (true);
  return (false);
}

/**
 * Return true if string `s1` ends with `s2`.
 */
bool str_endswith (const char *s1, const char *s2)
{
  const char *s1_end, *s2_end;

  if (strlen(s2) > strlen(s1))
     return (false);

  s1_end = strchr (s1, '\0') - 1;
  s2_end = strchr (s2, '\0') - 1;

  while (s2_end >= s2)
  {
    if (*s1_end != *s2_end)
       break;
    s1_end--;
    s2_end--;
  }
  return (s2_end == s2 - 1);
}

/**
 * Match a string `str` against word `what`.
 * Set `*next` to position after `what` in `str`.
 */
bool str_match (const char *str, const char *what, char **next)
{
  size_t len = strlen (what);

  if (str_startswith(str, what))
  {
    *next = str_ltrim ((char*)str + len);
    return (true);
  }
  return (false);
}

/**
 * Comparisions of file-names:
 * Use `strnicmp()` or `strncmp()` depending on `opt.case_sensitive`.
 */
int str_equal_n (const char *s1, const char *s2, size_t len)
{
  int rc;

  if (opt.case_sensitive)
       rc = strncmp (s1, s2, len);
  else rc = strnicmp (s1, s2, len);
  return (rc == 0 ? 1 : 0);
}

/**
 * Ditto for `strcmp()` and `stricmp()`.
 */
int str_equal (const char *s1, const char *s2)
{
  int rc;

  if (opt.case_sensitive)
       rc = strcmp (s1, s2);
  else rc = stricmp (s1, s2);
  return (rc == 0 ? 1 : 0);
}

/*
 * Replace all 'ch1' with 'ch2' in string 'str'.
 */
char *str_replace (int ch1, int ch2, char *str)
{
  char *s = str;

  while (s && *s)
  {
    if (*s == ch1)
        *s = (char)ch2;
    s++;
  }
  return (str);
}

/*
 * Replace all 'ch' with 'replace' in string 'str' growing the
 * size of 'str' limited by 'max_size'.
 */
char *str_replace2 (int ch, const char *replace, char *str, size_t max_size)
{
  char  *p, *copy;
  size_t ofs = 0, left = max_size;
  int    num = 0;

  if (!strchr(str, ch))   /* Nothing to do here */
     return (str);

  copy = p = alloca (max_size);

  while (str[ofs] && left > 1)
  {
    if (str[ofs] == ch)
    {
      _strlcpy (p, replace, left);
      p    += strlen (replace);
      left -= strlen (replace);
      num++;
    }
    else
    {
      *p++ = str[ofs];
      left--;
    }
    ofs++;
  }
  *p = '\0';
  return strcpy (str, copy);
}

/**
 * Remote quotes (`"`) around a string `str`.
 */
char *str_unquote (char *str)
{
  char *p, *q;

  p = str;
  if (*p == '"')
     p++;

  q = strchr (p, '\0');
  if (q[-1] == '"')
     q--;
  *q = '\0';
  if (q > p)
       memmove (str, p, q - p + 1);
  else *str = '\0';       /* `str` is an empty string or quoted like `""` */
  return (str);
}

/**
 * Return true if string `str` is quoted with `quote`.
 */
static int _str_isquoted (const char *str, char quote)
{
  char *l_quote = strchr (str, quote);
  char *r_quote = strrchr (str, quote);

  return (l_quote && r_quote && r_quote > l_quote);
}

/**
 * Return true if string `str` is properly quoted with `"` or `'`.
 */
int str_isquoted (const char *str)
{
  return _str_isquoted(str, '\"') || _str_isquoted(str, '\'');
}

/**
 * A `strtok_r()` function taken from libcurl:
 *
 * Copyright (C) 1998 - 2007, Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * \param[in,out] ptr  on first call, the string to break apart looking for `sep` strings.
 * \param[in]     sep  the separator string to look for.
 * \param[in]     end  the pointer to the end. Ignored on 1st call.
 */
char *_strtok_r (char *ptr, const char *sep, char **end)
{
  if (!ptr)
  {
    /* we got NULL input so then we get our last position instead */
    ptr = *end;
  }

  /* pass all letters that are including in the separator string */
  while (*ptr && strchr(sep, *ptr))
    ++ptr;

  if (*ptr)
  {
    /* so this is where the next piece of string starts */
    char *start = ptr;

    /* set the end pointer to the first byte after the start */
    *end = start + 1;

    /* scan through the string to find where it ends, it ends on a
     * null byte or a character that exists in the separator string.
     */
    while (**end && !strchr(sep, **end))
      ++*end;

    if (**end)
    {
      /* the end is not a null byte */
      **end = '\0';  /* zero terminate it! */
      ++*end;        /* advance the last pointer to beyond the null byte */
    }
    return (start);  /* return the position where the string starts */
  }
  /* we ended up on a null byte, there are no more strings to find! */
  return (NULL);
}

/**
 * Return a shorten string with length `max_len` such that
 * it looks like `"abcde...12345"`.
 * I.e. equally many starting and ending characters.
 */
char *str_shorten (const char *str, size_t max_len)
{
  static char buf [200];
  const char *end  = strchr (str, '\0');
  int         dots_len = 3;
  int         shift = 0;
  size_t      len;

  if (strlen(str) <= max_len || max_len < 2)
     return (char*) str;

  if (max_len == 2)
       len = 1, dots_len = 0;
  else if (max_len == 3)
       len = 1, dots_len = 1;
  else if (max_len == 4)
       len = 1, dots_len = 2;
  else if (max_len == 5)
       len = 1, dots_len = 2, shift++;
  else
  {
    len = (max_len - 3) / 2;
    if ((max_len & 1) == 0)   /* an even number */
       shift++;
  }
  snprintf (buf, sizeof(buf), "%.*s%.*s%.*s",
            (int)len, str, dots_len, "...", (int)(len+shift), end-len-shift);
  return (buf);
}

/**
 * Return the left-trimmed place where paths 'p1' and 'p2' are similar.
 * Not case sensitive. Treats `/` and `\\` equally.
 */
char *path_ltrim (const char *p1, const char *p2)
{
  for ( ; *p1 && *p2; p1++, p2++)
  {
    if (IS_SLASH(*p1) || IS_SLASH(*p2))
       continue;
    if (TOUPPER(*p1) != TOUPPER(*p2))
       break;
  }
  return (char*) p1;
}

/**
 * Return nicely formatted string `"xx,xxx,xxx"` for `val`.
 * With thousand separators (left adjusted).
 *
 * \param[in] val an 64-bit unsigned value.
 */
char *str_qword (UINT64 val)
{
  static char buf [40];
  char   tmp [30], *p;
  int    i, j, len = snprintf (tmp, sizeof(tmp), "%" U64_FMT, val);

  p = buf + sizeof(buf) - 1;
  *p-- = '\0';

  /* Copy 'tmp[]' into 'buf[]' in reverse order. Add commas after each 3 digit.
   */
  for (i = len, j = -1; i >= 0; i--, j++)
  {
    if (j > 0 && (j % 3) == 0)
       *p-- = ',';
    *p-- = tmp [i];
  }
  return (p+1);
}

/**
 * Return nicely formatted string `xx,xxx,xxx` for `val`.
 * With thousand separators (left adjusted).
 *
 * \param[in] val an 32-bit unsigned value.
 */
char *str_dword (DWORD val)
{
  return str_qword ((UINT64)val);
}

/**
 * Return string like "is" for 'val == 0' or 'val == 1' or
 *                    "are" for 0 or 'val > 1'.
 */
char *str_plural (DWORD val, const char *singular, const char *plural)
{
 if (val == 0 || val > 1)
    return (char*) plural;
  return (char*) singular;
}

/**
 * Checks a file-name for illegal characters.
 *
 * \ref https://learn.microsoft.com/en-gb/windows/win32/fileio/naming-a-file
 */
bool legal_file_name (const char *fname)
{
  const char *p;

  for (p = fname; *p; p++)
  {
    if (*p >= 1 && *p <= 31)
       return (false);
    if (strchr ("|<>\"?*", *p))
       return (false);
  }
  return (true);
}

/**
 * Find the first slash in a file-name.
 * \param[in] s the file-name to search in.
 */
static const char *find_slash (const char *s)
{
  while (*s)
  {
    if (IS_SLASH(*s))
       return (s);
    s++;
  }
  return (NULL);
}

/**
 * Test a character `test` for match of a `pattern`.
 * For a `pattern == "!x"`, check if `test != x`.
 */
static const char *range_match (const char *pattern, char test, int nocase)
{
  char c, c2;
  int  negate, ok;

  negate = (*pattern == '!');
  if (negate)
     ++pattern;

  for (ok = 0; (c = *pattern++) != ']'; )
  {
    if (c == 0)
       return (0);    /* illegal pattern */

    if (*pattern == '-' && (c2 = pattern[1]) != 0 && c2 != ']')
    {
      if (c <= test && test <= c2)
         ok = 1;
      if (nocase &&
          TOUPPER(c)    <= TOUPPER(test) &&
          TOUPPER(test) <= TOUPPER(c2))
         ok = 1;
      pattern += 2;
    }
    else if (c == test)
      ok = 1;
    else if (nocase && (TOUPPER(c) == TOUPPER(test)))
      ok = 1;
  }
  return (ok == negate ? NULL : pattern);
}

/**
 * Returns the flag for a case-sensitive `fnmatch()`
 * depending on `opt.case_sensitive`.
 */
int fnmatch_case (int flags)
{
  if (opt.case_sensitive == 0)
     flags |= FNM_FLAG_NOCASE;
  return (flags);
}

/**
 * File-name match.
 * Match a `string` against a `pattern` for a match.
 */
int fnmatch (const char *pattern, const char *string, int flags)
{
  char c, test;

  while (1)
  {
    c = *pattern++;

    switch (c)
    {
      case 0:
           return (*string == 0 ? FNM_MATCH : FNM_NOMATCH);

      case '?':
           test = *string++;
           if (test == 0 || (IS_SLASH(test) && (flags & FNM_FLAG_PATHNAME)))
              return (FNM_NOMATCH);
           break;

      case '*':
           c = *pattern;
           /* collapse multiple stars */
           while (c == '*')
               c = *(++pattern);

           /* optimize for pattern with '*' at end or before '/' */
           if (c == 0)
           {
             if (flags & FNM_FLAG_PATHNAME)
                return (find_slash(string) ? FNM_NOMATCH : FNM_MATCH);
             return (FNM_MATCH);
           }
           if (IS_SLASH(c) && (flags & FNM_FLAG_PATHNAME))
           {
             string = find_slash (string);
             if (!string)
                return (FNM_NOMATCH);
             break;
           }

           /* general case, use recursion */
           while ((test = *string) != '\0')
           {
             if (fnmatch(pattern, string, flags) == FNM_MATCH)
                return (FNM_MATCH);
             if (IS_SLASH(test) && (flags & FNM_FLAG_PATHNAME))
                break;
             ++string;
           }
           return (FNM_NOMATCH);

      case '[':
           test = *string++;
           if (!test || (IS_SLASH(test) && (flags & FNM_FLAG_PATHNAME)))
              return (FNM_NOMATCH);
           pattern = range_match (pattern, test, fnmatch_case(flags));
           if (!pattern)
              return (FNM_NOMATCH);
           break;

      case '\\':
           if (!(flags & FNM_FLAG_NOESCAPE) && pattern[1] && strchr("*?[\\", pattern[1]))
           {
             c = *pattern++;
             if (c == 0)
             {
               c = '\\';
               --pattern;
             }
             if (c != *string++)
                return (FNM_NOMATCH);
             break;
           }
           FALLTHROUGH()

      default:
           if (IS_SLASH(c) && IS_SLASH(*string))
           {
             string++;
             break;
           }
           if (flags & FNM_FLAG_NOCASE)
           {
             if (TOUPPER(c) != TOUPPER(*string++))
                return (FNM_NOMATCH);
           }
           else
           {
             if (c != *string++)
                return (FNM_NOMATCH);
           }
           break;
    } /* switch (c) */
  }   /* while (1) */
}

/**
 * Returns a string result for a `fnmatch()` result in `rc`.
 */
char *fnmatch_res (int rc)
{
  return (rc == FNM_MATCH   ? "FNM_MATCH"   :
          rc == FNM_NOMATCH ? "FNM_NOMATCH" : "??");
}

/**
 * Strip drive-letter, directory and suffix from a filename.
 */
char *basename (const char *fname)
{
  const char *base = fname;

  if (fname && *fname)
  {
    if (fname[0] && fname[1] == ':')
    {
      fname += 2;
      base = fname;
    }
    while (*fname)
    {
      if (IS_SLASH(*fname))
         base = fname + 1;
      fname++;
    }
  }
  return (char*) base;
}

/**
 * Return the malloc'ed directory part of a filename.
 */
char *dirname (const char *fname)
{
  const char *p = fname;
  const char *slash = NULL;
  size_t      dirlen;
  char       *dirpart;

  if (!fname)
     return (NULL);

  if (fname[0] && fname[1] == ':')
  {
    slash = fname + 1;
    p += 2;
  }

  /* Find the rightmost slash.
   */
  while (*p)
  {
    if (IS_SLASH(*p))
       slash = p;
    p++;
  }

  if (slash == NULL)
  {
    fname = ".";
    dirlen = 1;
  }
  else
  {
    /* Remove any trailing slashes.
     */
    while (slash > fname && (IS_SLASH(slash[-1])))
        slash--;

    /* How long is the directory we will return?
     */
    dirlen = slash - fname + (slash == fname || slash[-1] == ':');
    if (*slash == ':' && dirlen == 1)
       dirlen += 2;
  }

  dirpart = MALLOC (dirlen + 1);
  if (dirpart)
  {
    strncpy (dirpart, fname, dirlen);

    if (slash && *slash == ':' && dirlen == 3)
       dirpart[2] = '.';      /* for "x:foo" return "x:." */
    dirpart[dirlen] = '\0';
  }
  return (dirpart);
}

/**
 * Create a CygWin compatible path name from a Windows path.
 * ASCII-version.
 */
char *make_cyg_path (const char *path, char *result)
{
  char  buf [_MAX_PATH+20];
  char *p = slashify2 (buf, path, '/');

  if (strlen(p) > 2 && p[1] == ':' && IS_SLASH(p[2]))
  {
    snprintf (result, _MAX_PATH, "/cygdrive/%c/%s", tolower(p[0]), p+3);
    return (result);
  }
  return _strlcpy (result, p, _MAX_PATH);
}

/**
 * Create a CygWin compatible path name from a Windows path.
 * UNICODE-version.
 */
wchar_t *make_cyg_pathw (const wchar_t *path, wchar_t *result)
{
  wchar_t *p = WCSDUP (path);

  if (wcslen(p) > 2 && p[1] == ':' && IS_SLASH(p[2]))
       _snwprintf (result, _MAX_PATH, L"/cygdrive/%c/%s", towlower(p[0]), p+3);
  else wcsncpy (result, p, _MAX_PATH);
  FREE (p);
  return (result);
}

/**
 * Canonicalize file and paths names. E.g. convert this:
 * \code
 *   f:\mingw32\bin\../lib/gcc/x86_64-w64-mingw32/4.8.1/include
 * \endcode
 *
 * into something more readable:
 * \code
 *   f:\mingw32\lib\gcc\x86_64-w64-mingw32\4.8.1\include
 * \endcode
 *
 * I.e. turns `path` into a fully-qualified path.
 *
 * \note The `path` doesn't have to exist.
 *       Assumes `result` is at least `_MAX_PATH` characters long (if non-NULL).
 */
char *_fix_path (const char *path, char *result)
{
  if (!path || !*path)
  {
    TRACE (1, "given a bogus 'path': '%s'\n", path);
    errno = EINVAL;
    return (NULL);
  }

  if (!result)
     result = CALLOC (_MAX_PATH+1, 1);

 /* GetFullPathName() doesn't seems to handle
  * '/' in 'path'. Convert to '\\'.
  *
  * Note: the 'result' file or path may not exists.
  *       Use 'FILE_EXISTS()' to test.
  *
  * to-do: maybe use GetLongPathName()?
  */
  slashify2 (result, path, '\\');
  if (!GetFullPathName(result, _MAX_PATH, result, NULL))
     TRACE (2, "GetFullPathName(\"%s\") failed: %s\n",
            path, win_strerror(GetLastError()));

  return _fix_drive (result);
}

/**
 * For consistency, report drive-letter in lower case.
 */
char *_fix_drive (char *path)
{
  size_t len = strlen (path);

  if (len >= 3 && path[1] == ':' && IS_SLASH(path[2]))
     path[0] = (char) TOLOWER (path[0]);
  return (path);
}

/**
 * Return true if `path` starts with a drive-letter (`A:` - `Z:`)
 * and is followed by a slash (`\\` or `/`).
 */
bool _has_drive (const char *path)
{
  int disk = TOUPPER (path[0]);

  if (disk >= 'A' && disk <= 'Z' && strlen(path) >= 3 &&
      path[1] == ':' && IS_SLASH(path[2]))
     return (true);
  return (false);
}

/**
 * Return true if `path` starts with a drive-letter (`A:` - `Z:`).
 * Not required to be followed by a slash (`\\` or `/`).
 */
bool _has_drive2 (const char *path)
{
  int disk = TOUPPER (path[0]);

  if (disk >= 'A' && disk <= 'Z' && strlen(path) >= 2)
     return (true);
  return (false);
}

/**
 * Returns ptr to 1st character in file's extension.
 * Returns ptr to '\0' if no extension.
 */
const char *get_file_ext (const char *file)
{
  const char *end, *dot, *s;

  ASSERT (file);
  while ((s = strpbrk(file, ":/\\")) != NULL)  /* step over drive/path part */
     file = s + 1;

  end = strrchr (file, '\0');
  dot = strrchr (file, '.');
  return ((dot > file) ? dot+1 : end);
}

/**
 * Returns true if `file` is a directory.
 */
bool is_directory (const char *path)
{
  struct stat st;

  if (safe_stat(path, &st, NULL) == 0)
     return (_S_ISDIR(st.st_mode));
  return (false);
}

/**
 * Handle files like `c:\pagefile.sys` specially.
 *
 * These could be locked and `GetFileAttributes()` always fails on such files.
 * Best alternative is to use `FindFirstFile()`.
 */
int safe_stat_sys (const char *file, struct stat *st, DWORD *win_err)
{
  WIN32_FIND_DATA ff_data;
  HANDLE          hnd;
  DWORD           err = 0;

  memset (st, '\0', sizeof(*st));
  st->st_size = (off_t)-1;     /* signal if stat() fails */

  memset (&ff_data, '\0', sizeof(ff_data));
  hnd = FindFirstFile (file, &ff_data);
  if (hnd == INVALID_HANDLE_VALUE)
     err = GetLastError();
  else
  {
    if (ff_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
       err = ERROR_DIRECTORY;
    else
    {
      st->st_ctime = FILETIME_to_time_t (&ff_data.ftCreationTime);
      st->st_mtime = FILETIME_to_time_t (&ff_data.ftLastAccessTime);
      st->st_size = ((UINT64)ff_data.nFileSizeHigh << 32) + ff_data.nFileSizeLow;
    }
    FindClose (hnd);
  }
  TRACE (1, "file: '%s', attr: 0x%08lX, err: %lu, mtime: %" U64_FMT " fsize: %s\n",
         file, (unsigned long)ff_data.dwFileAttributes, err, (UINT64)st->st_mtime, get_file_size_str(st->st_size));

  if (win_err)
     *win_err = err;
  return (err ? -1 : 0);
}

/**
 * A bit safer `stat()`.
 * If given a hidden / system file (like `c:\\pagefile.sys`), some
 * `stat()` implementations can crash. MSVC would be one case.
 *
 * \return any `GetLastError()` is set in `*win_err`.
 * \retval 0   okay (the same as `stat()`).
 * \retval -1  fail. `errno` set (the same as `stat()`).
 *
 * \note directories are passed directly to `stat()`.
 */
int safe_stat (const char *file, struct stat *st, DWORD *win_err)
{
  DWORD   err = 0;
  DWORD   attr = 0;
  bool    is_dir;
  HANDLE  hnd = INVALID_HANDLE_VALUE;
  size_t  len = strlen (file);
  wchar_t w_file [32*1024] = { 0 };

  if (win_err)
     *win_err = 0;

  /* https://docs.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileattributesa
   */
  if (len >= 260)   /* MAX_PATH */
  {
    wcscpy (w_file, L"\\\\?\\");
    if (mbchar_to_wchar(w_file+4, DIM(w_file)-4, file))
       attr = GetFileAttributesW (w_file);
  }

  if (attr == 0)
     attr = GetFileAttributes (file);

  if (attr == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_SHARING_VIOLATION)
     return safe_stat_sys (file, st, win_err);

  memset (st, '\0', sizeof(*st));
  st->st_size = (off_t)-1;     /* signal if stat() fails */

  if (w_file[0] == 0)
  {
    is_dir = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
    if (is_dir)
       return stat (file, st);

    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & (FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM)))
       return stat (file, st);
  }

  /**
   * Need to check for other Hidden/System files here.
   * Also for files >= MAX_PATH.
   */
  if (attr != INVALID_FILE_ATTRIBUTES && (w_file[0] || (attr & (FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM))))
  {
    FILETIME      c_ftime;  /* Get 'lpCreationTime' */
    FILETIME      m_ftime;  /* Get 'lpLastWriteTime' */
    LARGE_INTEGER fsize;

    if (w_file[0])
         hnd = CreateFileW (w_file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, attr, NULL);
    else hnd = CreateFileA (file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, attr, NULL);

    if (hnd != INVALID_HANDLE_VALUE)
    {
      if (GetFileTime(hnd, &c_ftime, NULL, &m_ftime))
      {
        st->st_ctime = FILETIME_to_time_t (&c_ftime);
        st->st_mtime = FILETIME_to_time_t (&m_ftime);
      }
      if (GetFileSizeEx(hnd, &fsize))
         st->st_size = ((UINT64)fsize.HighPart << 32) + fsize.LowPart;
      CloseHandle (hnd);
    }
    else
      err = GetLastError();
  }
  else
    err = GetLastError();

  if (w_file[0])
      TRACE (1, "w_file: '%S', attr: 0x%08lX, hnd: %p, err: %lu, mtime: %" U64_FMT " fsize: %s\n",
             w_file, (unsigned long)attr, hnd, err, (UINT64)st->st_mtime, get_file_size_str(st->st_size));
  else TRACE (1, "file: '%s', attr: 0x%08lX, hnd: %p, err: %lu, mtime: %" U64_FMT " fsize: %s\n",
              file, (unsigned long)attr, hnd, err, (UINT64)st->st_mtime, get_file_size_str(st->st_size));

  if (win_err)
     *win_err = err;
  return (err ? -1 : 0);
}

/**
 * Create a `%TEMP%-file`.
 * \return An allocated string of the file-name.
 *         Caller must call `FREE()` on it after use.
 */
char *create_temp_file (void)
{
  char *tmp = _tempnam (NULL, "envtool-tmp");

  if (tmp)
  {
    char *t = STRDUP (tmp);

    TRACE (2, " %s() tmp: '%s'\n", __FUNCTION__, tmp);
    free (tmp);     /* Free CRT data */
    return (t);     /* Caller must FREE() this */
  }
  TRACE (2, " %s(): _tempnam() failed: %s\n", __FUNCTION__, strerror(errno));
  return (NULL);
}

/**
 * Allocate memory for `file`. Then return memory with allocated file-content.
 */
char *fopen_mem (const char *file, size_t *_f_size)
{
  struct stat st;
  FILE       *f;
  char       *f_ptr;
  size_t      f_read, f_size;
  DWORD       win_err;

  memset (&st, '\0', sizeof(st));
  if (safe_stat(file, &st, &win_err) || st.st_size == 0)
  {
    TRACE (1, "Failed to get the file-size of %s. win_err: %lu\n", file, win_err);
    return (NULL);
  }

  f = fopen (file, "rb");
  if (!f)
  {
    TRACE (1, "Failed to open %s.\n", file);
    return (NULL);
  }

  if (st.st_size >= ULONG_MAX)
  {
    TRACE (1, "File %s is too big %" S64_FMT ".\n", file, st.st_size);
    fclose (f);
    return (NULL);
  }

  f_size = (size_t) st.st_size;
  f_ptr = MALLOC (f_size + 1);
  f_read = fread (f_ptr, 1, f_size, f);
  fclose (f);
  if (f_read != f_size)
  {
    TRACE (1, "Failed to read the whole file %s. Only %u bytes, errno: %d (%s)\n",
           file, (unsigned)f_read, errno, strerror(errno));
    FREE (f_ptr);
    return (NULL);
  }
  *_f_size = f_size;
  f_ptr [f_read] = '\0';
  return (f_ptr);
}

/**
 * Turn off default error-mode. E.g. if a CD-ROM isn't ready, we'll get a GUI
 * popping up to notify us. Turn that off and handle such errors ourself.
 *
 * + `SetErrorMode()`       is per process.
 * + `SetThreadErrorMode()` is per thread on Win-7+.
 */
void set_error_mode (int restore)
{
  init_misc();

  if (p_SetThreadErrorMode)
  {
    static DWORD old_mode = 0;
    DWORD  mode = restore ? old_mode : SEM_FAILCRITICALERRORS;
    bool   rc;

    if (restore)
         rc = (*p_SetThreadErrorMode) (mode, NULL);
    else rc = (*p_SetThreadErrorMode) (mode, &old_mode);
    TRACE (2, "restore: %d, SetThreadErrorMode (0x%04lX), rc: %d.\n",
           restore, (unsigned long)mode, rc);
  }
  else
  {
    static UINT old_mode = 0;
    UINT   mode = restore ? old_mode : SEM_FAILCRITICALERRORS;

    if (restore)
         SetErrorMode (mode);
    else old_mode = SetErrorMode (mode);
    TRACE (2, "restore: %d, SetErrorMode (0x%04X).\n", restore, mode);
  }
}

/**
 * Get a cached `cluster_size` for `disk`. (`A:` - `Z:`).
 * Only works on local disks; I.e. `disk-type == DRIVE_FIXED`.
 *
 * \retval true  on success.
 * \retval false if disk out of range or if `GetDiskFreeSpace()` fails.
 * \ref https://www.foldersizes.com/wordpress/index.php/2020/07/actual-vs-allocated-disk-space.htm
 */
bool get_disk_cluster_size (int disk, DWORD *size)
{
  static DWORD cluster_size  ['Z' - 'A' + 1];
  static bool  is_local_disk ['Z' - 'A' + 1];
  static char  root[] = "?:\\";

  DWORD  sect_per_cluster, bytes_per_sector, free_clusters, total_clusters;
  bool   rc;
  char  *err = "<none>";
  int    i;

  disk = TOUPPER (disk);
  if (disk < 'A' || disk > 'Z') /* What to do? */
     return (false);

  i = disk - 'A';
  ASSERT (i >= 0 && i < DIM(is_local_disk));
  ASSERT (i >= 0 && i < DIM(cluster_size));

  if (cluster_size[i] && is_local_disk[i])
  {
    if (size)
       *size = cluster_size[i];
    return (true);
  }

  root[0] = (char) disk;
  sect_per_cluster = bytes_per_sector = free_clusters = total_clusters = 0;

  if (!GetDiskFreeSpace(root, &sect_per_cluster, &bytes_per_sector, &free_clusters, &total_clusters))
  {
    is_local_disk[i] = false;
    err = win_strerror (GetLastError());
    rc  = false;
  }
  else
  {
    is_local_disk[i] = true;
    cluster_size[i]  = sect_per_cluster * bytes_per_sector;
    rc = true;
  }

  TRACE (1, "GetDiskFreeSpace (\"%s\"): sect_per_cluster: %lu, bytes_per_sector: %lu, total_clusters: %lu, error: %s\n",
         root, (unsigned long)sect_per_cluster, (unsigned long)bytes_per_sector, (unsigned long)total_clusters, err);

  TRACE (1, "%c: (local: %d) -> cluster_size: %lu\n", disk, is_local_disk[i], cluster_size[i]);

  if (rc && size)
     *size = cluster_size[i];
  return (rc);
}

/**
 * Get the allocation size of a file or directory.
 *
 * This uses cached information from the above `get_disk_cluster_size()`. <br>
 * Currently only works on local disks; `disk-type == DRIVE_FIXED`.
 * Otherwise it simply returns the `size`.
 *
 * `size == (UINT64)-1` means it's a directory.
 * If `file` does not starts with a `x:`, `x:` should be the current disk.
 */
UINT64 get_file_alloc_size (const char *file, UINT64 size)
{
  DWORD  cluster_size;
  UINT64 num_clusters;
  int    disk = *file;
  static int curr_disk = 0;

  if (_has_drive2(file))  /* Not so strict as `_has_drive()` */
     disk = *file;
  else
  {
    if (curr_disk == 0)
    {
      curr_disk = _getdrive();
      if (curr_disk >= 1)
         curr_disk = TOLOWER (curr_disk + 'A' - 1);
      TRACE (2, "curr_disk: %c:\n", curr_disk);
    }
    disk = curr_disk;
  }

  if (!disk || !get_disk_cluster_size(disk, &cluster_size))
  {
    if (size == (UINT64)-1)
       return (0);
    return (size);
  }

  /* I assume a directory allocates 1 cluster_size.
   * I'm not sure this is correct.
   */
  if (size == (UINT64)-1)
     return (cluster_size);

  num_clusters = size / cluster_size;
  if (size % cluster_size)
     num_clusters++;
  return (num_clusters * cluster_size);
}

/**
 * Get the compressed size of a file.
 */
bool get_file_compr_size (const char *file, UINT64 *fsize)
{
  DWORD loDword, hiDword = 0;

  *fsize = (UINT64)-1;
  loDword = GetCompressedFileSize (file, &hiDword);

  if (loDword == INVALID_FILE_SIZE)
     return (false);
  *fsize = hiDword;
  *fsize <<= 32;
  *fsize += loDword;
  return (true);
}

/**
 * Get the size of files in a directory by walking
 * recursively in all sub-directories under `dir`.
 */
UINT64 get_directory_size (const char *dir)
{
  struct dirent2 **namelist = NULL;
  int    i, n = scandir2 (dir, &namelist, NULL, NULL);
  UINT64 size = 0;

  for (i = 0; i < n; i++)
  {
    int   is_dir      = (namelist[i]->d_attrib & FILE_ATTRIBUTE_DIRECTORY);
    int   is_junction = (namelist[i]->d_attrib & FILE_ATTRIBUTE_REPARSE_POINT);
    const char *link;

    if (is_junction)
    {
      link = namelist[i]->d_link ? namelist[i]->d_link : "?";
      TRACE (1, "Not recursing into junction \"%s\"\n", link);
      size += get_file_alloc_size (dir, (UINT64)-1);
    }
    else if (is_dir)
    {
      TRACE (1, "Recursing into \"%s\"\n", namelist[i]->d_name);
      size += get_file_alloc_size (namelist[i]->d_name, (UINT64)-1);
      size += get_directory_size (namelist[i]->d_name);
    }
    else
      size += get_file_alloc_size (namelist[i]->d_name, namelist[i]->d_fsize);
  }

  while (n--)
    FREE (namelist[n]);
  FREE (namelist);

  return (size);
}

/**
 * Return the type of `disk`.
 */
UINT get_disk_type (int disk)
{
  static const search_list disk_types[] = {
                           ADD_VALUE (DRIVE_UNKNOWN),
                           ADD_VALUE (DRIVE_NO_ROOT_DIR),
                           ADD_VALUE (DRIVE_REMOVABLE),
                           ADD_VALUE (DRIVE_FIXED),
                           ADD_VALUE (DRIVE_REMOTE),
                           ADD_VALUE (DRIVE_CDROM),
                           ADD_VALUE (DRIVE_RAMDISK)
                         };
  char root[] = "?:\\";
  UINT type;

  root[0] = (char) disk;
  type = GetDriveType (root);

  TRACE (2, "GetDriveType (\"%s\"): type: %s (%u).\n",
         root, list_lookup_name(type, disk_types, DIM(disk_types)), type);
  return (type);
}

/**
 * Get the volume mount point where the specified disk is mounted.
 */
bool get_volume_path (int disk, char **mount)
{
  char  *err    = "<none>";
  char   root[] = "?:\\";
  bool   rc = false;
  static char res [2*_MAX_PATH];

  root[0] = (char) disk;
  if (!GetVolumePathName(root, res, sizeof(res)))
       err = win_strerror (GetLastError());
  else rc = true;

  if (rc && mount)
     *mount = res;
  TRACE (2, "GetVolumePathName (\"%s\"): error: %s, res: \"%s\"\n",
         root, err, rc ? res : "N/A");
  return (rc);
}

/**
 * Check if a disk is ready.
 * \param[in] disk  the disk to check: `['A'..'Z']`.
 */
int disk_ready (int disk)
{
  int    rc1 = 0, rc2 = 0;
  char   path [8];
  HANDLE hnd;

  snprintf (path, sizeof(path), "\\\\.\\%c:", TOUPPER(disk));
  set_error_mode (0);

  TRACE (2, "Calling CreateFile (\"%s\").\n", path);
  hnd = CreateFile (path, GENERIC_READ | FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

  if (hnd == INVALID_HANDLE_VALUE)
  {
    DWORD err = GetLastError();

    TRACE (2, "  failed: %s\n", win_strerror(err));

    /* A lack of privilege mean the device "\\\\.\\x:" exists
     */
    if (err != ERROR_ACCESS_DENIED)
    {
      rc1 = -1;
      goto quit;
    }
  }
  rc1 = 1;

#if 0
  {
    DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                   FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SECURITY;

    char  buf [sizeof(FILE_NOTIFY_INFORMATION) + _MAX_PATH + 3];
    DWORD size = sizeof(buf);
    bool  rd_change = ReadDirectoryChangesA (hnd, &buf, size, FALSE, filter, NULL, NULL, NULL);

    if (!rd_change)
    {
      TRACE (2, "ReadDirectoryChanges(): failed: %s\n", win_strerror(GetLastError()));
      rc2 = 0;
    }
    else
    {
      const FILE_NOTIFY_INFORMATION *fni = (const FILE_NOTIFY_INFORMATION*) &buf;

      TRACE (2, "fni->NextEntryOffset: %lu\n", fni->NextEntryOffset);
      TRACE (2, "fni->Action:          %lu\n", fni->Action);
      TRACE (2, "fni->FileNameLength:  %lu\n", fni->FileNameLength);
      TRACE (2, "fni->FileName:        \"%.*S\"\n", (int)fni->FileNameLength, fni->FileName);
      rc2 = 1;
    }
  }
#endif

quit:
  if (hnd != INVALID_HANDLE_VALUE)
     CloseHandle (hnd);
  set_error_mode (1);
  return (rc1 | rc2);
}

/**
 * Return a cached status for a `disk` ready status.
 * \param[in] disk  the disk to check: `['A'..'Z']`.
 */
bool chk_disk_ready (int disk)
{
  static bool checked ['Z' - 'A' + 1];
  static int  status  ['Z' - 'A' + 1];
  int    i;

  disk = TOUPPER (disk);
  if (disk < 'A' || disk > 'Z') /* What to do? */
     return (true);

  i = disk - 'A';
  ASSERT (i >= 0 && i < (int)sizeof(checked));

  if (!checked[i])
  {
    status[i] = disk_ready (disk);

    /* A success from `CreateFile()` above is not enough indication
     * if the `disk-type != DRIVE_FIXED`.
     */
    if (status[i] == 1)
    {
      set_error_mode (0);

      if (get_disk_type(disk) == DRIVE_FIXED)
         status[i] = get_disk_cluster_size (disk, NULL);

      set_error_mode (1);
    }
    TRACE (3, "drive: %c, status: %d.\n", disk, status[i]);
  }
  checked [i] = true;
  return (status[i] >= 1);
}

/**
 * This used to be a macro in envtool.h.
 */
int _file_exists (const char *file)
{
  if (_has_drive(file) && !chk_disk_ready((int)file[0]))
  {
    TRACE (2, "Disk %c: not ready.\n", file[0]);
    return (0);
  }
  if (GetFileAttributes(file) != INVALID_FILE_ATTRIBUTES)
     return (1);

  TRACE (2, "File '%s' does not exist.\n", file);
  return (0);
}

/**
 * Convert compact UUIDs like:
 *   6dceedd62edc8337ea153c73497e3d9e
 *
 * to this more "normal" Windows form:
 *   {6DCEEDD6-2EDC-8337-EA15-3C73-497E3D9E}
 */
char *_fix_uuid (const char *uuid, char *result)
{
  char  *p;
  size_t i, len;

  if (!uuid || !*uuid)
  {
    TRACE (1, "given a bogus 'uuid': '%s'\n", uuid);
    errno = EINVAL;
    return (NULL);
  }

  len = strlen (uuid) + 7 + 1;
  if (!result)
     result = MALLOC (len);

  if (*uuid == '{')   /* assume it's already on Windows OLE32 form */
     return strcpy (result, uuid);

  p = result;
  *p++ = '{';
  for (i = 0; *uuid && *uuid != '}'; i++)
  {
    if (*uuid != '-' && (i == 8 || i == 13 || i == 18 || i == 23 || i == 28))
         *p++ = '-';
    else *p++ = *uuid++;
  }
  *p++ = '}';
  *p = '\0';
  return strupr (result);
}

/**
 * Return true if this program is executed as an `elevated` process.
 *
 * Taken from Python 3.5's `src/PC/bdist_wininst/install.c`.
 */
bool is_user_admin (void)
{
  typedef BOOL (WINAPI *func_IsUserAnAdmin) (void);
  func_IsUserAnAdmin p_IsUserAnAdmin;

  HMODULE shell32 = LoadLibrary ("shell32.dll");
  bool    rc;

  /* This function isn't guaranteed to be available.
   */
  if (!shell32)
     return (false);

  p_IsUserAnAdmin = GETPROCADDRESS (func_IsUserAnAdmin, shell32, "IsUserAnAdmin");
  if (!p_IsUserAnAdmin)
       rc = is_user_admin2();
  else rc = (*p_IsUserAnAdmin)();

  FreeLibrary (shell32);
  return (rc);
}

/*
 * The same taken from NPcap and modified.
 */
bool is_user_admin2 (void)
{
  BOOL  is_admin = FALSE;
  DWORD rc = ERROR_SUCCESS;
  SID  *admin_group = NULL;

  /* Allocate and initialize a SID of the administrators group.
   */
  SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;

  if (!AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                (PSID*)&admin_group))
  {
    rc = GetLastError();
    goto cleanup;
  }

  /* Determine whether the SID of administrators group is enabled in
   * the primary access token of the process.
   */
  if (!CheckTokenMembership(NULL, admin_group, &is_admin))
     rc = GetLastError();

cleanup:
  if (admin_group)
     FreeSid (admin_group);

  TRACE (1, "is_user_admin2(): rc: %lu, %s\n", rc, is_admin? "yes" : "no");
  return (bool) is_admin;
}

/**
 * Return name of logged-in user.
 *
 * First try `GetUserNameEx()` available in Win-2000 Pro.<br>
 * Then fall-back to a `GetUserName()` if not present in `Secur32.dll`.
 * \see
 *   GetUserNameExA()
 *   https://msdn.microsoft.com/en-us/library/windows/desktop/ms724435(v=vs.85).aspx
 */
const char *get_user_name (void)
{
  #define NameSamCompatible 2

  typedef BOOL (WINAPI *func_GetUserNameEx) (int format, char *user, ULONG *user_len);
  func_GetUserNameEx  p_GetUserNameEx;
  static char         user [100] = { "?" };
  ULONG               ulen = sizeof(user);
  HMODULE             secur32;

  if (user[0] != '?')   /* Already done this */
     return (user);

  secur32 = LoadLibrary ("secur32.dll");

  /* This function isn't guaranteed to be available (and it can't hurt
   * to leave the library loaded)
   */
  if (secur32)
  {
    p_GetUserNameEx = GETPROCADDRESS (func_GetUserNameEx, secur32, "GetUserNameExA");
    if (p_GetUserNameEx)
      (*p_GetUserNameEx) (NameSamCompatible, user, &ulen);
    FreeLibrary (secur32);
  }

  if (user[0] == '?')      /* No 'GetUserNameExA()' function or it failed */
     GetUserName (user, &ulen);

  return (user);
}

#ifndef USER_CET_ENVIRONMENT_WIN32_PROCESS
#define USER_CET_ENVIRONMENT_WIN32_PROCESS  0x00000000UL
#endif

#ifndef USER_CET_ENVIRONMENT_SGX2_ENCLAVE
#define USER_CET_ENVIRONMENT_SGX2_ENCLAVE  0x00000002UL
#endif

#ifndef USER_CET_ENVIRONMENT_VBS_ENCLAVE
#define USER_CET_ENVIRONMENT_VBS_ENCLAVE  0x00000010UL
#endif

#ifndef USER_CET_ENVIRONMENT_VBS_BASIC_ENCLAVE
#define USER_CET_ENVIRONMENT_VBS_BASIC_ENCLAVE  0x00000011UL
#endif

/**
 * Check and print info on:
 * "user-mode Hardware-enforced Stack Protection is available for the specified environment."
 * CET == Control-flow Enforcement Technology
 *
 * \ref
 *   https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-isusercetavailableinenvironment
 *   https://learn.microsoft.com/en-us/cpp/build/reference/cetcompat?view=msvc-170
 *   https://github.com/474172261/windows-CET
 */
bool print_user_cet_info (void)
{
  typedef BOOL (WINAPI *func_IsUserCetAvailableInEnvironment) (DWORD);
  func_IsUserCetAvailableInEnvironment p_IsUserCetAvailableInEnvironment;

  HMODULE kernel32 = LoadLibrary ("kernel32.dll");
  size_t  i;
  static const search_list cet_values[] = {
               ADD_VALUE (USER_CET_ENVIRONMENT_WIN32_PROCESS),
               ADD_VALUE (USER_CET_ENVIRONMENT_SGX2_ENCLAVE),
               ADD_VALUE (USER_CET_ENVIRONMENT_VBS_ENCLAVE),
               ADD_VALUE (USER_CET_ENVIRONMENT_VBS_BASIC_ENCLAVE)
             };

  if (!kernel32)
  {
    C_puts ("Fail to load `kernel32.dll`!\n");
    return (false);
  }

  p_IsUserCetAvailableInEnvironment = GETPROCADDRESS (func_IsUserCetAvailableInEnvironment,
                                                      kernel32, "IsUserCetAvailableInEnvironment");
  if (!p_IsUserCetAvailableInEnvironment)
  {
    C_puts ("Funtion `kernel32!IsUserCetAvailableInEnvironment()` not present\n");
    FreeLibrary (kernel32);
    return (false);
  }

  DWORD64 mask [2] = { 0, 0 };

  if (!GetProcessMitigationPolicy(GetCurrentProcess(), ProcessMitigationOptionsMask, mask, sizeof(mask)))
       C_printf ("~6GetProcessMitigationPolicy():~0 %s\n", win_strerror (GetLastError()));
  else C_printf ("~6GetProcessMitigationPolicy():~0 0x%016llX, 0x%016llX\n", mask[0], mask[1]);

  for (i = 0; i < DIM(cet_values); i++)
  {
    BOOL rc = (*p_IsUserCetAvailableInEnvironment) (cet_values[i].value);

    C_printf ("              ~6%-38s~0 -> %d.\n", cet_values[i].name, rc);
  }
  C_putc ('\n');
  FreeLibrary (kernel32);
  return (true);
}

/**
 * Similar to `strncpy()`, but always returns `dst` with 0-termination.
 * Does *not* return a `size_t` as Posix `strlcpy()` does.
 *
 * \param[in] dst  the destination buffer to copy to.
 * \param[in] src  the source buffer to copy from.
 * \param[in] len  the max size of `dst` buffer (including final 0-termination).
 */
char *_strlcpy (char *dst, const char *src, size_t len)
{
  size_t slen;

  ASSERT (src != NULL);
  ASSERT (dst != NULL);
  ASSERT (len > 0);

  slen = strlen (src);
  if (slen < len)
     return strcpy (dst, src);

  memcpy (dst, src, len-1);
  dst [len-1] = '\0';
  return (dst);
}

/**
 * Return a string with `ch` repeated `num` times.
 *
 * Limited to 200 characters.
 */
char *str_repeat (int ch, size_t num)
{
  static char buf [200];
  char  *p = buf;
  size_t i;

  *p = '\0';
  for (i = 0; i < num && i < sizeof(buf)-1; i++)
     *p++ = (char) ch;
  *p = '\0';
  return (buf);
}

/**
 * Get next token from string `*stringp`, where tokens are possibly empty
 * strings separated by characters from `delim`.
 *
 * Writes NULs into the string at `*stringp` to end tokens.
 *
 * `delim` need not remain constant from call to call.
 *
 * On return, `*stringp` points past the last `NUL` written (if there might
 * be further tokens), or is `NULL` (if there are definitely no more tokens).
 *
 * If `*stringp` is NULL, `str_sep()` returns `NULL`.
 */
char *str_sep (char **stringp, const char *delim)
{
  int         c, sc;
  char       *tok, *s = *stringp;
  const char *spanp;

  if (!s)
     return (NULL);

  for (tok = s; ; )
  {
    c = *s++;
    spanp = delim;
    do
    {
      sc = *spanp++;
      if (sc == c)
      {
        if (c == '\0')
             s = NULL;
        else s[-1] = '\0';
        *stringp = s;
        return (tok);
      }
    }
    while (sc != 0);
  }
  return (NULL);
  /* NOTREACHED */
}

/**
 * `"string allocate and concatinate"`.
 * Assumes `s1` is allocated. Thus `FREE(s1)` after `str_acat()` is done.
 */
char *str_acat (char *s1, const char *s2)
{
  size_t sz = strlen(s1) + strlen(s2) + 1;
  char  *s  = MALLOC (sz);
  char  *start = s;

  sz = strlen (s1);
  memcpy (s, s1, sz);   /* copy 's1' into new space for both 's1' and 's2' */
  FREE (s1);
  s += sz;              /* advance to end of 's' */
  sz = strlen (s2) + 1;
  memcpy (s, s2, sz);   /* copy 's2' after 's1' */
  return (start);
}

/**
 * Allocate and return a new string containing the
 * first `n` characters of `s`.
 */
char *str_ndup (const char *s, size_t sz)
{
  char *dup = MALLOC (sz+1);

  strncpy (dup, s, sz);
  dup [sz] = '\0';
  return (dup);
}

/**
 * Taken from Newlib:
 *
 * Locates the first occurrence in the memory region pointed to
 * by `haystack` with length `haystack_len` of the sequence of bytes
 * pointed to by `needle` of length `needle_len`.
 */
void *str_memmem (const void *haystack, size_t haystack_len, const void *needle, size_t needle_len)
{
  const char *hs = haystack;
  const char *ne = needle;
  const char *end;
  int         c;
  size_t      i;

  if (needle_len == 0)
     return (void *)hs;

  end = hs + haystack_len - needle_len;
  for (c = ne[0] ; hs <= end; hs++)
  {
    if (hs[0] != c)
       continue;
    for (i = needle_len - 1; i != 0; i--)
       if (hs[i] != ne[i])
          break;
    if (i == 0)
       return (void*) hs;
  }
  return (NULL);
}

/**
 * A `strstr()` limited to `len` characters.
 *
 * \retval  Returns a pointer to the located string inside `haystack`.
 *          Or NULL if `needle` was not found in `haystack`.
 */
char *str_nstr (const char *haystack, const char *needle, size_t haystack_len)
{
  size_t needle_len = strnlen (needle, haystack_len);
  char  *s;

  if (needle_len < haystack_len || !needle[needle_len])
  {
    s = str_memmem (haystack, haystack_len, needle, needle_len);
    if (s && !memchr(haystack, '\0', s - haystack))
       return (s);
  }
  return (NULL);
}

/**
 * Concatinate 2 strings; `src` to `dst.
 * If we have `strcat_s()`, use the `dst_size`.
 */
int str_cat (char *dst, size_t dst_size, const char *src)
{
  return strcat_s (dst, dst_size, src);
}

/**
 * Create a joined string from an array of strings.
 *
 * \param[in] arr  the array of strings to join and return as a single string.
 * \param[in] sep  the separator between the `arr` elements; after the first up-to the 2nd last
 *
 * \retval NULL  if `arr` is empty
 * \retval !NULL a `MALLOC()`-ed string of the concatinated result.
 */
char *str_join (char * const *arr, const char *sep)
{
  char  *p,  *ret = NULL;
  int    i, num;
  size_t sz = 0;

  if (!arr || !arr[0])
     return (NULL);

  /* Get the needed size for `ret`
   */
  for (i = num = 0; arr[i]; i++, num++)
      sz += strlen (arr[i]) + strlen (sep);

  sz++;
  sz -= strlen (sep);      /* No `sep` after last `arr[]` */
  p = ret = MALLOC (sz);
  for (i = 0; i < num; i++)
  {
    strcpy (p, arr[i]);
    p = strchr (p, '\0');
    if (i < num - 1)
       strcpy (p, sep);
    p = strchr (p, '\0');
  }
  return (ret);
}

/**
 * For consistency and nice looks, replace (single or multiple) `\\`
 * with single `/` if use == `/`. And vice-versa.
 *
 * All (?) Windows core functions functions should handle
 * `/` just fine.
 */
char *slashify (const char *path, char use)
{
  static char buf [_MAX_PATH];
  char   *s = buf;
  const char *p;
  const char *end = path + strlen(path);

 /* Do not collapse multiple slashes here
  */
  if (!strnicmp(path, "\\\\wsl.", 6))
  {
    strncpy (s, path, 6);
    path += 6;
    s    += 6;
  }

  for (p = path; p < end; p++)
  {
    if (IS_SLASH(*p))
    {
      *s++ = use;
      while (p < end && IS_SLASH(p[1]))  /* collapse multiple slashes */
          p++;
    }
    else
      *s++ = *p;
    ASSERT (s < buf+sizeof(buf)-1);
  }
  *s = '\0';
  return _fix_drive (buf);
}

/**
 * As above, but copy into `buf`.
 * \note `path` and `buf` can point to the same location.
 */
char *slashify2 (char *buf, const char *path, char use)
{
  const char *p, *end = strchr (path, '\0');
  char       *s = buf;

 /* Do not collapse multiple slashes here
  */
  if (!strnicmp(path, "\\\\wsl.", 6))
  {
    strncpy (s, path, 6);
    path += 6;
    s    += 6;
  }

  for (p = path; p < end; p++)
  {
    if (IS_SLASH(*p))
    {
      *s++ = use;
      while (p < end && IS_SLASH(p[1]))  /* collapse multiple slashes */
          p++;
    }
    else
      *s++ = *p;
    ASSERT (s < buf+_MAX_PATH-1);
  }
  *s = '\0';
  return _fix_drive (buf);
}

/**
 * \b {Heuristic alert!}
 *
 * Return 1 if file `A` is newer than file `B`.
 * Based on modification times `mtime_a`, `mtime_b` and file-versions
 * returned from `show_version_info()`.
 *
 * Currently not used.
 */
int compare_file_time_ver (time_t mtime_a, time_t mtime_b,
                           struct ver_info ver_a, struct ver_info ver_b)
{
  ARGSUSED (mtime_a);
  ARGSUSED (mtime_b);
  ARGSUSED (ver_a);
  ARGSUSED (ver_b);
  return (0);  /* \todo */
}

/**
 * Return error-string for `err` from `kernel32.dll`.
 */
static bool get_error_from_kernel32 (DWORD err, char *buf, DWORD buf_len)
{
  HMODULE mod = GetModuleHandle ("kernel32.dll");
  bool    rc = false;

  if (mod)
  {
    rc = FormatMessageA (FORMAT_MESSAGE_FROM_HMODULE,
                         mod, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                         buf, buf_len, NULL);
  }
  return (rc);
}

#ifndef ERROR_VIRUS_INFECTED
#define ERROR_VIRUS_INFECTED   225L
#endif

#ifndef ERROR_VIRUS_DELETED
#define ERROR_VIRUS_DELETED    226L
#endif

/**
 * Return err-number+string for `err`. Use only with `GetLastError()`.
 * Does not handle libc `errno` values. Remove trailing `[\r\n]`.
 */
char *win_strerror (unsigned long err)
{
  static  char buf[512+20];
  char    err_buf[512], *p;
  HRESULT hr = 0;

  if (HRESULT_SEVERITY(err))
     hr = err;

  if (err == ERROR_SUCCESS)
     strcpy (err_buf, "No error");

  else if (err == ERROR_BAD_EXE_FORMAT)
     strcpy (err_buf, "Bad EXE format");

  else if (err == ERROR_VIRUS_INFECTED)
     strcpy (err_buf, "Virus Infected!");

  else if (err == ERROR_VIRUS_DELETED)
     strcpy (err_buf, "Virus Deleted!");

  else if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err,
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           err_buf, sizeof(err_buf)-1, NULL))
  {
    if (!get_error_from_kernel32(err,err_buf, sizeof(err_buf)-1))
       strcpy (err_buf, "Unknown error");
  }

  if (hr != 0)
       snprintf (buf, sizeof(buf), "0x%08lX: %s", (unsigned long)hr, err_buf);
  else snprintf (buf, sizeof(buf), "%lu: %s", err, err_buf);
  str_strip_nl (buf);
  p = strrchr (buf, '.');
  if (p && p[1] == '\0')
     *p = '\0';
  return (buf);
}

/**
 * Returns a string for Winsock error-code.
 * \param[in] err  the error-code.
 *
 * These strings are stored by `kernel32.dll` and not in
 * `ws2_32.dll`.
 */
char *ws2_strerror (int err)
{
  static char buf [500];
  char        err_buf [510];

  if (err == 0)
     return ("No error");

  init_misc();

  if (kernel32_hnd &&
      FormatMessageA (FORMAT_MESSAGE_FROM_HMODULE,
                     kernel32_hnd, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     err_buf, sizeof(err_buf), NULL))
  {
    str_strip_nl (buf);
    snprintf (buf, sizeof(buf), "(%d) %s", err, err_buf);
    return (buf);
  }

  snprintf (buf, sizeof(buf), "%d?", err);
  return (buf);
}

#if !defined(_CRTDBG_MAP_ALLOC)
/**
 * A `strdup()` that fails if no memory. It's pretty hopeless to continue
 * this program if `strdup()` fails.
 */
char *strdup_at (const char *str, const char *file, unsigned line)
{
  struct mem_head *head;
  size_t len = strlen (str) + 1 + sizeof(*head);

  head = malloc (len);

  if (!head)
     FATAL ("'strdup()' failed at %s, line %u\n", file, line);

  memcpy (head+1, str, len - sizeof(*head));
  head->marker = MEM_MARKER;
  head->size   = len;
  add_to_mem_list (head, file, line);
  return (char*) (head + 1);
}

/**
 * Similar to `wcsdup()`.
 */
wchar_t *wcsdup_at (const wchar_t *str, const char *file, unsigned line)
{
  struct mem_head *head;
  size_t len = sizeof(wchar_t) * (wcslen(str) + 1);

  len += sizeof(*head);
  head = malloc (len);

  if (!head)
     FATAL ("'wcsdup()' failed at %s, line %u\n", file, line);

  memcpy (head+1, str, len - sizeof(*head));
  head->marker = MEM_MARKER;
  head->size   = len;
  add_to_mem_list (head, file, line);
  return (wchar_t*) (head + 1);
}

/**
 * A `malloc()` that fails if no memory. It's pretty hopeless to continue
 * this program if `malloc()` fails.
 */
void *malloc_at (size_t size, const char *file, unsigned line)
{
  struct mem_head *head;

  size += sizeof(*head);

  head = malloc (size);

  if (!head)
     FATAL ("'malloc (%u)' failed at %s, line %u\n",
            (unsigned)(size-sizeof(*head)), file, line);

  head->marker = MEM_MARKER;
  head->size   = size;
  add_to_mem_list (head, file, line);
  return (head + 1);
}

/**
 * A `calloc()` that fails if no memory. It's pretty hopeless to continue
 * this program if `calloc()` fails.
 */
void *calloc_at (size_t num, size_t size, const char *file, unsigned line)
{
  struct mem_head *head;

  size = (size * num) + sizeof(*head);

  head = calloc (1, size);

  if (!head)
     FATAL ("'calloc (%u, %u)' failed at %s, line %u\n",
            (unsigned)num, (unsigned)(size-sizeof(*head)), file, line);

  head->marker = MEM_MARKER;
  head->size   = size;
  add_to_mem_list (head, file, line);
  return (head + 1);
}

/**
 * A `realloc()` that fails if no memory. It's pretty hopeless to continue
 * this program if `realloc()` fails.
 */
void *realloc_at (void *ptr, size_t size, const char *file, unsigned line)
{
  struct mem_head *p;

  if (ptr == NULL)
     return malloc_at (size, file, line);

  if (size == 0)
  {
    free_at (ptr, file, line);
    return (NULL);
  }

  p = (struct mem_head*) ptr;
  p--;

  if (p->marker != MEM_MARKER)
     FATAL ("'realloc()' of unknown block at %s, line %u\n", file, line);

  if (p->size - sizeof(*p) < size)
  {
    ptr = malloc_at (size, file, line);
    size = p->size - sizeof(*p);
    memmove (ptr, p+1, size);        /* since memory could be overlapping */
    del_from_mem_list (p, __LINE__);
    mem_reallocs++;
    free (p);
  }
  return (ptr);
}

/**
 * A `free()` that checks the `ptr` and decrements the `mem_frees` value.
 */
void free_at (void *ptr, const char *file, unsigned line)
{
  struct mem_head *head = (struct mem_head*) ptr;

  head--;
  if (!ptr)
     FATAL ("'free(NULL)' called at %s, line %u\n", file, line);

  if (head->marker == MEM_FREED)
     FATAL ("double 'free()' of block detected at %s, line %u\n", file, line);

  if (head->marker != MEM_MARKER)
     FATAL ("'free()' of unknown block at %s, line %u.\n", file, line);

  head->marker = MEM_FREED;
  del_from_mem_list (head, __LINE__);
  mem_frees++;
  free (head);
}
#endif  /* !_CRTDBG_MAP_ALLOC */

/**
 * Print a report of memory-counters and warn on any unfreed memory blocks.
 */
void mem_report (void)
{
#if !defined(_CRTDBG_MAP_ALLOC)
  const struct mem_head *m;
  unsigned     num;

  C_printf ("~0  Max memory at one time: %sytes.\n", str_trim((char*)get_file_size_str(mem_max)));
  C_printf ("  Total # of allocations: %u.\n", (unsigned int)mem_allocs);
  C_printf ("  Total # of realloc():   %u.\n", (unsigned int)mem_reallocs);
  C_printf ("  Total # of frees:       %u.\n", (unsigned int)mem_frees);

  for (m = mem_list, num = 0; m; m = m->next, num++)
  {
    C_printf ("  Un-freed memory 0x%p at %s (%u). %u bytes: \"%s\"\n",
              m+1, m->file, m->line, (unsigned int)m->size,
              dump10(m+1, (unsigned)m->size));
    if (num > 20)
    {
      C_printf ("  ..and more.\n");
      break;
    }
  }
  if (num == 0)
     C_printf ("  No un-freed memory.\n");
  C_flush();
#endif
}

#if defined(_MSC_VER) && defined(_DEBUG)
/**
 * Only one global mem-state.
 */
static _CrtMemState last_state;

/**
 * In `_DEBUG`-mode, remember the `last_state` as the CRT-memory
 * start-up state.
 *
 * \note
 *   Enable this for MSVC and clang-cl only.
 */
void crtdbug_init (void)
{
  _HFILE file  = _CRTDBG_FILE_STDERR;
  int    mode  = _CRTDBG_MODE_FILE;
  int    flags = _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_DELAY_FREE_MEM_DF;

  _CrtSetReportFile (_CRT_ASSERT, file);
  _CrtSetReportMode (_CRT_ASSERT, mode);
  _CrtSetReportFile (_CRT_ERROR, file);
  _CrtSetReportMode (_CRT_ERROR, mode);
  _CrtSetReportFile (_CRT_WARN, file);
  _CrtSetReportMode (_CRT_WARN, mode);
  _CrtSetDbgFlag (flags | _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG));
  _CrtMemCheckpoint (&last_state);
}

/**
 * In `_DEBUG`-mode, compare the `last_state` set in `crtdbug_init()` to
 * check if there is a significant difference in the mem-state.
 *
 * This function should be called as late as possible.
 *
 * \note This function is *not* called if the `FATAL()` macro was used.
 *       The the default `SIGABRT` handler (via `abort()`) is used to
 *       report any leaks.
 */
void crtdbug_exit (void)
{
  _CrtMemState new_state, diff_state;

  /* If 'FATAL()' was called, we'll get tons of leaks here. Just ignore those.
   */
  if (opt.fatal_flag)
     return;

  _CrtMemCheckpoint (&new_state);

  /* No significant difference in the mem-state. So just get out.
   */
  if (!_CrtMemDifference(&diff_state, &last_state, &new_state))
     return;

  _CrtCheckMemory();
  _CrtSetDbgFlag (0);
#if 0
  if (opt.debug)
       _CrtMemDumpStatistics (&last_state);
  else _CrtMemDumpAllObjectsSince (&last_state);
#endif
  _CrtDumpMemoryLeaks();
}

#else
void crtdbug_init (void)
{
}
void crtdbug_exit (void)
{
}
#endif

/**
 * A `snprintf()` replacement to print and append to a local `FMT_buf*`
 * initialised using `BUF_INIT()`.
 */
int buf_printf (const char *file, unsigned line, FMT_buf *fmt_buf, _Printf_format_string_ const char *format, ...)
{
  va_list      args;
  int          len;
  size_t       fmt_len = strlen (format);
  const DWORD *marker;
  char        *end;

  if (fmt_len >= fmt_buf->buffer_left)
     FATAL ("'fmt_buf->buffer_left' too small. Called from %s(%u).\n", file, line);

  marker = (const DWORD*) fmt_buf->buffer;
  if (*marker != FMT_BUF_MARKER)
     FATAL ("'First marked destroyed or 'BUF_INIT()' not called. Called from %s(%u).\n", file, line);

  marker = (const DWORD*) (fmt_buf->buffer + fmt_buf->buffer_size + sizeof(DWORD));
  if (*marker != FMT_BUF_MARKER)
     FATAL ("Last marked destroyed. Called from %s(%u).\n", file, line);

  /* Terminate first. Because with `_MSC_VER < 1900` and `fmt_buf->buffer_left`
   * exactly large enough for the result, `vsnprintf()` will not add a trailing NUL.
   */
  *(fmt_buf->buffer_start + fmt_buf->buffer_size - 1) = '\0';

  va_start (args, format);
  vsnprintf (fmt_buf->buffer_pos, fmt_buf->buffer_left, format, args);
  va_end (args);

  /* Do not assume POSIX compliance of above `vnsprintf()` function.
   * Force next call to `buf_printf()` to append at the 'end' position.
   */
  end = strchr (fmt_buf->buffer_pos, '\0');
  len = (int) (end - fmt_buf->buffer_pos);

  /* Assume `len` is always positive.
   */
  fmt_buf->buffer_left -= len;
  fmt_buf->buffer_pos  += len;
  return (len);
}

/**
 * A `puts()` replacement to print and append to a local `FMT_buf*`
 * initialised using `BUF_INIT()`.
 */
int buf_puts (const char *file, unsigned line, FMT_buf *fmt_buf, const char *string)
{
  size_t       str_len = strlen (string);
  const DWORD *marker;

  if (str_len >= fmt_buf->buffer_left)
  {
    int size = (int) (fmt_buf->buffer_size + str_len) + 1;
    FATAL ("'fmt_buf->buffer_size' too small. Try 'BUF_INIT(&fmt_buf,%d)'. Called from %s(%u).\n",
           size, file, line);
  }

  marker = (const DWORD*) fmt_buf->buffer;
  if (*marker != FMT_BUF_MARKER)
     FATAL ("'First marked destroyed or 'BUF_INIT()' not called. Called from %s(%u).\n",
            file, line);

  marker = (const DWORD*) (fmt_buf->buffer + fmt_buf->buffer_size + sizeof(DWORD));
  if (*marker != FMT_BUF_MARKER)
     FATAL ("Last marked destroyed. Called from %s(%u).\n", file, line);

  strcpy (fmt_buf->buffer_pos, string);
  fmt_buf->buffer_left -= str_len;
  fmt_buf->buffer_pos  += str_len;
  return (int)(str_len);
}

/**
 * A `putc()` replacement to print a single character to a `FMT_buf*`.
 */
int buf_putc (const char *file, unsigned line, FMT_buf *fmt_buf, int ch)
{
  if (fmt_buf->buffer_left <= 1)
  {
    int size = (int) (fmt_buf->buffer_size + 2);
    FATAL ("'fmt_buf->buffer_size' too small. Try 'BUF_INIT(&fmt_buf,%d)'. Called from %s(%u).\n",
           size, file, line);
  }
  fmt_buf->buffer_left--;
  *fmt_buf->buffer_pos++ = (char) ch;
  *fmt_buf->buffer_pos = '\0';
  return (1);
}

/**
 * Print a long line to a `FMT_buf*`.
 *
 * A line is wrapped at a space (` `) to fit the screen-width nicely.
 * If the console is redirected, wrap at the `UINT_MAX` edge (practically
 * no wrapping at all).
 */
void buf_puts_long_line (const char *file, unsigned line, FMT_buf *fmt_buf, const char *string, size_t indent)
{
  size_t      width = (C_screen_width() == 0) ? UINT_MAX : C_screen_width();
  size_t      left  = width - indent;
  const char *c = string;

  while (*c)
  {
    /* Break a long line only at a space.
     * Check if room for a long string before we must break the line.
     */
    if (*c == ' ')
    {
      const char *p = strchr (c+1, ' ');

      if (!p)
         p = strchr (c+1, '\0');

      if (left < 2 || (left <= (size_t)(p - c)))
      {
        buf_printf (file, line, fmt_buf, "\n%*c", (int)indent, ' ');
        left = width - indent;
        string = ++c;
        continue;
      }
      /* Drop multiple spaces.
       */
      if (c > string && isspace((int)c[-1]))
      {
        string = ++c;
        continue;
      }
    }
    buf_putc (file, line, fmt_buf, *c++);
    left--;
  }
  buf_putc (file, line, fmt_buf, '\n');
}

/**
 * Restart using a `FMT_buf*`. <br>
 * Call this after `fmt_buf->buffer_start` has been used (printed) and before
 * adding more text to the buffer (using e.g. `buf_printf()`).
 */
void buf_reset (FMT_buf *fmt_buf)
{
  fmt_buf->buffer_pos  = fmt_buf->buffer_start;
  fmt_buf->buffer_left = fmt_buf->buffer_size;
}

/**
 * Return a nicely formatted string for a file-size
 * given as an `UINT64`.
 *
 * \note
 *  + Uses the SI-unit post-fixes.
 *  + A Yottabyte (or `Yobibyte == 1024^8`) is too large for an `UINT64`.
 *  + A `size == -1` is used as indication of an unknown size.
 */
const char *get_file_size_str (UINT64 size)
{
  static const char *suffixes[] = { "B ", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB" };
  static char buf [10];
  int    i   = 0;
  int    rem = 0;

  if (size == (__int64)-1)
     return strcpy (buf, "   ?   ");

  while (size >= 1024ULL)
  {
    if ((size % 1024ULL) >= 512ULL)
         rem = 1;
    else rem = 0;
    size /= 1024ULL;
    i++;
  }

  /* Round up
   */
  snprintf (buf, sizeof(buf), "%4u %s", (unsigned)size + rem, suffixes[i]);
  return (buf);
}

/**
 * Return a time-string for `time_t == 0` (non-time).
 */
const char *empty_time (void)
{
  return (opt.decimal_timestamp ? "00000000.000000" : "01 Jan 1970 - 00:00:00");
}

/**
 * Return number of digits in `n`.
 */
UINT count_digit (UINT64 n)
{
  UINT count = 0;

  if (n == 0)
     return (1);

  while (n)
  {
    n /= 10ULL;
    count++;
  }
  return (count);
}

/**
 * Return a nicely formatted string for a `time_t`.
 *
 * `strftime()` under MSVC sometimes crashes mysteriously. So use this
 * home-grown version.
 *
 * Tests for `time_t == 0` which is returned from `safe_stat()`
 * of e.g. a protected `.sys`-file.
 *
 * Use 2 buffers in round-robin.
 */
const char *get_time_str (time_t t)
{
  static char  buf [2][50];
  static int   idx = 0;
  const struct tm *tm;
  char  *res = buf [idx];

  if (t == 0)
     return empty_time();

  tm = localtime (&t);
  if (!tm)
     return empty_time();

  idx++;
  if (opt.decimal_timestamp)
     snprintf (res, sizeof(buf[0]), "%04d%02d%02d.%02d%02d%02d",
               1900+tm->tm_year, 1+tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
  else
  {
    static const char *months [12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
                                     };
    const char *_month;

    if (tm->tm_mon >= 0 && tm->tm_mon < DIM(months))
         _month = months [tm->tm_mon];
    else _month = "???";
    snprintf (res, sizeof(buf[0]), "%02d %s %04d - %02d:%02d:%02d",
              tm->tm_mday, _month, 1900+tm->tm_year, tm->tm_hour, tm->tm_min, tm->tm_sec);
  }
  idx &= 1;
  return (res);
}

/*
 * Return a time-string for a `ft` like:
 * 28 Oct 2019 - 16:22:49
 *
 * Use 2 buffers in round-robin.
 */
const char *get_time_str_FILETIME (const FILETIME *ft)
{
  static char  buf [2][50];
  static int   idx = 0;
  static const char *months [12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
                                   };
  const char *_month;
  char       *res = buf [idx];
  SYSTEMTIME  st, lt;

  if (!FileTimeToSystemTime(ft, &st) || !SystemTimeToTzSpecificLocalTime(NULL, &st, &lt))
     return ("?");

  if (lt.wMonth <= DIM(months))
       _month = months [lt.wMonth-1];
  else _month = "???";

  if (opt.decimal_timestamp)
       snprintf (res, sizeof(buf[0]), "%04d%02d%02d.%02d%02d%02d",
                 lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
  else snprintf (res, sizeof(buf[0]), "%02d %s %04u - %02u:%02u:%02u",
                 lt.wDay, _month, lt.wYear, lt.wHour, lt.wMinute, lt.wSecond);
  idx++;
  idx &= 1;
  return (res);
}

/**
 * Function that prints the line argument while limiting it
 * to at most `C_screen_width()`.
 *
 * If the console is redirected (`C_screen_width() == 0`), the "screen width"
 * is infinite (or `UINT_MAX`).
 *
 * An appropriate number of spaces are added on subsequent lines.
 * Multiple spaces (`"  "`) are collapsed into one space.
 *
 * Stolen from Wget (main.c) and simplified.
 */
void format_and_print_line (const char *line, int indent)
{
  char  *token, *line_dup = STRDUP (line);
  size_t width = (C_screen_width() == 0) ? UINT_MAX : C_screen_width();
  size_t left  = width - indent;

  /* We break on spaces.
   */
  token = strtok (line_dup, " ");
  while (token)
  {
    /* If a token is much larger than the maximum
     * line length, we print the token on the next line.
     */
    if (left <= strlen(token)+1)
    {
      C_printf ("\n%*c", indent, ' ');
      left = width - indent;
    }
    C_printf ("%s ", token);
    left -= strlen (token) + 1;  /* account for " " */
    token = strtok (NULL, " ");
  }
  C_putc ('\n');
  FREE (line_dup);
}

/**
 * A function similar to `format_and_print_line()`,
 * but without a `STRDUP()`.
 */
void print_long_line (const char *line, size_t indent)
{
  size_t      width = (C_screen_width() == 0) ? UINT_MAX : C_screen_width();
  size_t      left  = width - indent;
  const char *c = line;

  while (*c)
  {
    /* Break a long line only at a space.
     * Check if room for a long string before we must break the line.
     */
    if (*c == ' ')
    {
      const char *p = strchr (c+1, ' ');

      if (!p)
         p = strchr (c+1, '\0');

      if (left < 2 || (left <= (size_t)(p - c)))
      {
        C_printf ("\n%*c", (int)indent, ' ');
        left = width - indent;
        line = ++c;
        continue;
      }
      /* Drop multiple spaces.
       */
      if (c > line && isspace((int)c[-1]))
      {
        line = ++c;
        continue;
      }
    }
    C_putc (*c++);
    left--;
  }
  C_putc ('\n');
}

/**
 * A function similar to `print_long_line()`,
 * but break a line at another character than a space.
 *
 * And do not print a trailing newline.
 */
void print_long_line2 (const char *line, size_t indent, int break_at)
{
  size_t      width = (C_screen_width() == 0) ? UINT_MAX : C_screen_width() - 1;
  size_t      left  = width - indent;
  const char *c = line;

  while (*c)
  {
    /* Break a long line only at the `break_at` character.
     * Check if room for a long string before we must break the line.
     */
    if (*c == break_at)
    {
      const char *p = strchr (c+1, break_at);

      if (!p)
         p = strchr (c+1, '\0');

      if (left < 2 || (left <= (size_t)(p - c)))
      {
        C_printf ("%c\n%*c", break_at, (int)indent, ' ');
        left = width - indent;
        line = ++c;
        continue;
      }
      /* Drop multiple spaces.
       */
      if (c > line && isspace((int)c[-1]))
      {
        line = ++c;
        continue;
      }
    }
    C_putc (*c++);
    left--;
  }
}

/**
 * `_MSC_VER <= 1800` (Visual Studio 2012 or older) is lacking `vsscanf()`.
 * Create our own using `sscanf()`. <br>
 * Scraped from:
 *   https://stackoverflow.com/questions/2457331/replacement-for-vsscanf-on-msvc
 *
 * If using the Windows-Kit (`_VCRUNTIME_H` is defined) it should have a
 * working `vsscanf()`. I'm not sure if using `_MSC_VER <= 1800` with the Windows-Kit is possible.
 */
#if (defined(_MSC_VER) && (_MSC_VER <= 1800) && !defined(_VCRUNTIME_H))
int _vsscanf2 (const char *buf, const char *fmt, va_list args)
{
  void *a[5];  /* 5 args is enough here */
  int   i;

  for (i = 0; i < DIM(a); i++)
     a[i] = va_arg (args, void*);
  return sscanf (buf, fmt, a[0], a[1], a[2], a[3], a[4]);
}
#endif


#if defined(NOT_USED_YET)
/**
 * Create another console window for debugging.
 */
void create_console (void)
{
  if (AllocConsole())
  {
    WORD color = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_RED;
    freopen ("CONOUT$", "wt", stdout);
    SetConsoleTitle ("Debug Console");
    SetConsoleTextAttribute (GetStdHandle(STD_OUTPUT_HANDLE), color);
  }
}
#endif  /* NOT_USED_YET */

/**
 * Search `list` for `value` and return it's name.
 */
const char *list_lookup_name (unsigned value, const search_list *list, int num)
{
  static char buf[10];

  while (num > 0 && list->name)
  {
    if (list->value == value)
       return (list->name);
    num--;
    list++;
  }
  return _itoa (value, buf, 10);
}

/**
 * Search `list` for `name` and return it's `value`.
 */
unsigned list_lookup_value (const char *name, const search_list *list, int num)
{
  while (num > 0 && list->name)
  {
    if (!stricmp(name, list->name))
       return (list->value);
    num--;
    list++;
  }
  return (UINT_MAX);
}

/**
 * Decode a `DWORD` value in `flags` and return a string composed of
 * names in `list`.
 *
 * See `reg_access_name()` for an example:
 * \code
 *  reg_access_name (KEY_CREATE_LINK | KEY_CREATE_SUB_KEY | 0x4000) (== reg_access_name (0x0020 | 0x0004 | 0x4000)
 * \endcode
 *
 * would return the string `0x4000 + KEY_CREATE_LINK+KEY_CREATE_SUB_KEY`
 * since `0x4000` is *not* a known flag in `reg_access_name()`.
 */
const char *flags_decode (DWORD flags, const search_list *list, int num)
{
  static char buf[300];
  char  *ret  = buf;
  char  *end  = buf + sizeof(buf) - 1;
  size_t left = end - ret;
  int    i, len;

  *ret = '\0';
  for (i = 0; i < num; i++, list++)
      if (flags & list->value)
      {
        len = snprintf (ret, left, "%s+", list->name);
        if (len < 0 || len >= (int)left)
           break;
        ret  += len;
        left -= len;
        flags &= ~list->value;
      }

  if (flags && left >= 15)  /* print unknown flag-bits */
     ret += snprintf (ret, left, "0x%08lX+", (u_long)flags);
  if (ret > buf)
     *(--ret) = '\0';   /* remove '+' */
  return (buf);
}

/**
 * The var-arg print function used in e.g. the `TRACE()` macro.
 */
int debug_printf (_Printf_format_string_ const char *format, ...)
{
  int     raw, rc;
  va_list args;

  va_start (args, format);
  raw = C_setraw (1);
  rc = C_vprintf (format, args);
  C_setraw (raw);
  va_end (args);
  return (rc);
}

/**
 * Helper buffer for the `while` loop in `popen_run()`.
 */
static char popen_last [1000];

/**
 * Return the last line in the `fgets()` loop below.
 */
char *popen_last_line (void)
{
  return (popen_last);
}

/**
 * Duplicate and fix the `cmd` before calling `popen()`.
 * The caller `popen_run()` must free the return value.
 */
static char *popen_setup (const char *cmd)
{
  char       *cmd2;
  char       *env = getenv ("COMSPEC");
  const char *comspec = "";
  const char *setdos  = "";
  size_t      len;

  /**
   * If we're using 4NT/TCC shell, set all variable expansion to off
   * by prepending `setdos /x-3 & ` to `cmd` buffer.
   */
  if (env)
  {
    TRACE (3, "%%COMSPEC: %s.\n", env);
    env = strlwr (basename(env));
    if (!strcmp(env, "4nt.exe") || !strcmp(env, "tcc.exe"))
       setdos = "setdos /x-3 & ";
  }
  else
    comspec = "set COMSPEC=cmd.exe & ";

  len = strlen(setdos) + strlen(comspec) + strlen(cmd) + 1;

  /* Allocate an extended command-line for `_popen()`.
   */
  cmd2 = MALLOC (len);
  if (cmd2)
  {
    strcpy (cmd2, setdos);
    strcat (cmd2, comspec);
    strcat (cmd2, cmd);
  }
  return (cmd2);
}

/**
 * A var-arg wrapper for `_popen()` that takes care of quoting the command:
 *
 * \eg. `popen_run (callback, "some program.exe", "--help")`
 *      gets converted to `_popen ("\"some program.exe\" --help", "r")`.
 *
 * \param[in] callback  Optional function to call for each line from `popen()`.
 *                      This function should return number of matches.
 *                      The `callback` is allowed to modify the `buf` given to it.
 * \param[in] cmd       The mandatory program to run.
 * \param[in] arg       Optional argument(s) for the program.
 *
 * \retval -1   if `cmd` was not found or `_popen()` fails for some reason. `errno` should be set.
 * \retval >=0  total number of matches from `callback`.
 */
int popen_run (popen_callback callback, const char *cmd, const char *arg, ...)
{
  char   quote = '"';
  char   line_buf [5000];
  char   cmd_buf [1000], *p;
  size_t left;
  int    line, i, rc;
  FILE  *f;
  char  *cmd2;

  TRACE (2, "cmd: '%s'.\n", cmd);

  p = cmd_buf;
  left = sizeof(cmd_buf) - 1;

  if (strchr(cmd, ' ') && !str_isquoted(cmd))
  {
    *p++ = quote;
    _strlcpy (p, cmd, left);
    p += strlen (cmd);
    *p++ = quote;
    left -= 2 + strlen (cmd);
  }
  else
  {
    _strlcpy (p, cmd, left);
    p    += strlen (cmd);
    left -= strlen (cmd);
  }
  *p = '\0';

  slashify2 (cmd_buf, cmd_buf, '\\');

  if (arg)
  {
    va_list args;
    int     len;

    *p++ = ' ';
    left--;
    va_start (args, arg);
    len = vsnprintf (p, left, arg, args);
    if (len < 0 || len >= (int)left)  /* 'cmd_buf[]' too small */
         left = 0;
    else left -= len;
    va_end (args);
  }
  TRACE (2, "left: %d, cmd_buf: '%s'.\n", (int)left, cmd_buf);

  cmd2 = popen_setup (cmd_buf);
  if (!cmd2)
     return (-1);

  *popen_last_line() = '\0';

  TRACE (3, "Trying to run '%s'\n", cmd2);

  f = _popen (cmd2, "r");
  if (!f)
  {
    TRACE (1, "failed to call _popen(); errno=%d.\n", errno);
    FREE (cmd2);
    return (-1);
  }

  line = i = 0;
  while (fgets(line_buf, sizeof(line_buf)-1, f))
  {
    str_strip_nl (line_buf);
    TRACE (3, " _popen() line_buf: '%s'\n", line_buf);
    _strlcpy (popen_last, line_buf, sizeof(popen_last));
    if (!line_buf[0] || !callback)
       continue;

    rc = (*callback) (line_buf, line++);
    i += rc;
    if (rc < 0)
       break;
  }
  rc = _pclose (f);
  if (rc == -1)
       TRACE (2, " _pclose(): -1, errno: %d.\n", errno);
  else TRACE (2, " _pclose(): %d.\n", rc);
  FREE (cmd2);
  return (i);
}

/*
 * Scraped from:
 *  https://stackoverflow.com/questions/14147138/capture-output-of-spawned-process-to-string
 *
 * and heavily modifed.
 */
typedef struct popen2_st {
        HANDLE               child_stdout_read;
        HANDLE               reader;
        PROCESS_INFORMATION  pi;
        char                 cmd_buf [1000];
        char                 stdout_data [16*1024];
        popen_callback       callback;
        DWORD                callback_ret;
        DWORD                exit_code;
        int                  timeout;    /* timeout in milliseconds or -1 for INIFINTE */
      } popen2_st;

static int peek_pipe (HANDLE pipe, char *data, int size)
{
  char   buffer [4*1024];
  DWORD  read = 0;
  DWORD  available = 0;
  BOOL   rc = PeekNamedPipe (pipe, NULL, sizeof(buffer), NULL, &available, NULL);
  int    n, bytes;

  if (!rc)
     return (-1);

  else if (available > 0)
  {
    bytes = min (sizeof(buffer), available);
    rc = ReadFile (pipe, buffer, bytes, &read, NULL);
    if (!rc)
       return (-1);

    if (data && size > 0)
    {
      n = min (size - 1, (int)read);
      memcpy (data, buffer, n);
      data[n + 1] = '\0';   /* zero terminate */
      return (n);
    }
  }
  return (0);
}

static DWORD WINAPI threaded_pipe_read (void *arg)
{
  popen2_st *popen   = (popen2_st*) arg;
  ULONGLONG  start_t = GetTickCount64();
  ULONGLONG  spent_t;
  bool       got_newline;
  char      *out       = popen->stdout_data;
  int        out_bytes = sizeof(popen->stdout_data) - 1;
  char      *p = out;
  int        line = 0, rc = 0, read_stdout;

  for (;;)
  {
    read_stdout = peek_pipe (popen->child_stdout_read, out, out_bytes);
    if (read_stdout < 0)  /* pipe got closed */
       break;

    if (popen->timeout > 0)
    {
      spent_t = GetTickCount64() - start_t;
      if (spent_t > (ULONGLONG) popen->timeout)    /* read-timeout on pipe */
         break;
    }
    if (read_stdout > 0)
    {
      got_newline = (strpbrk (p, "\r\n") != NULL);
      TRACE (2, "got_newline: %d, got %d bytes from pipe: '%s'\n", got_newline, read_stdout, p);

      out       += read_stdout;
      out_bytes -= read_stdout;

      if (got_newline && popen->callback)
      {
        rc = (*popen->callback) (p, line++);
        popen->callback_ret += rc;
        p = out;
        if (rc < 0)
           break;
      }
    }

    /* If nothing has been read from pipe,
     * wait for at least 1 millisecond (more likely 16)
     */
    if (read_stdout == 0)
       WaitForSingleObject (popen->child_stdout_read, 1);
  }
  *out = '\0';
  return (0);
}

static DWORD create_child_process (popen2_st *popen)
{
  SECURITY_ATTRIBUTES sa = { 0 };
  STARTUPINFO         siStartInfo = { 0 };
  HANDLE              child_stdout_write = INVALID_HANDLE_VALUE;
  DWORD               err;
  BOOL                rc;

  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;

  if (!CreatePipe(&popen->child_stdout_read, &child_stdout_write, &sa, 0))
     return GetLastError();

  if (!SetHandleInformation(popen->child_stdout_read, HANDLE_FLAG_INHERIT, 0))
     return GetLastError();

  siStartInfo.cb = sizeof(STARTUPINFO);
  siStartInfo.hStdOutput = child_stdout_write;
  siStartInfo.dwFlags   |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  siStartInfo.wShowWindow = SW_HIDE;
  SetLastError (0);
  rc = CreateProcessA (NULL,
                       popen->cmd_buf,     /* the command to run */
                       NULL,               /* process security attributes */
                       NULL,               /* primary thread security attributes */
                       TRUE,               /* handles are inherited */
                       CREATE_NO_WINDOW,   /* creation flags */
                       NULL,               /* use parent's environment */
                       NULL,               /* use parent's current directory */
                       &siStartInfo,       /* STARTUPINFO pointer */
                       &popen->pi);        /* receives PROCESS_INFORMATION */

  err = GetLastError();
  CloseHandle (child_stdout_write);
  if (!rc)
  {
    CloseHandle (popen->child_stdout_read);
    popen->child_stdout_read = INVALID_HANDLE_VALUE;
  }
  return (rc ? ERROR_SUCCESS : err);
}

static DWORD popen_threaded (popen2_st *popen)
{
  DWORD rc;

  popen->exit_code = 0;
  popen->stdout_data[0] = '\0';

  rc = create_child_process (popen);
  if (rc == ERROR_SUCCESS)
  {
    popen->reader = CreateThread (NULL, 0, threaded_pipe_read, popen, 0, NULL);
    if (!popen->reader)
    {
      rc = GetLastError();
      TerminateProcess (popen->pi.hProcess, ECANCELED);
    }
    else
    {
      BOOL thread_done  = WaitForSingleObject (popen->pi.hThread, popen->timeout) == 0;
      BOOL process_done = WaitForSingleObject (popen->pi.hProcess, popen->timeout) == 0;

      if (!thread_done || !process_done)
         TerminateProcess (popen->pi.hProcess, ETIME);

      GetExitCodeProcess (popen->pi.hProcess, &popen->exit_code);
      CloseHandle (popen->pi.hThread);
      CloseHandle (popen->pi.hProcess);
      CloseHandle (popen->child_stdout_read);
      popen->child_stdout_read = INVALID_HANDLE_VALUE;
      WaitForSingleObject (popen->reader, INFINITE); // join thread
      CloseHandle (popen->reader);
    }
  }
  popen->stdout_data [sizeof(popen->stdout_data) - 1] = '\0';
  return (rc);
}

/**
 * A var-arg wrapper similar to `popen_run()`.
 *
 * But uses `CreateProcess()` and `PeekNamedPipe()` in a separate thread to read
 * `stdout` from the child-process.
 */
DWORD popen_run2 (popen_callback callback, const char *cmd, const char *arg, ...)
{
  popen2_st popen;
  size_t    left = sizeof(popen.cmd_buf) - 1;
  char     *p = popen.cmd_buf;
  DWORD     rc;

  memset (&popen, '\0', sizeof(popen));
  popen.callback = callback;
  popen.timeout  = -1;

  _strlcpy (p, cmd, left);
  p    += strlen (cmd);
  left -= strlen (cmd);
  *p = '\0';

  if (arg)
  {
    va_list args;
    int     len;

    *p++ = ' ';
    left--;
    va_start (args, arg);
    len = vsnprintf (p, left, arg, args);
    if (len < 0 || len >= (int)left)  /* 'popen.cmd_buf[]' too small */
         left = 0;
    else left -= len;
    va_end (args);
  }

  rc = popen_threaded (&popen);
  if (rc != ERROR_SUCCESS)
  {
    TRACE (1, "popen_threaded() failed(): %s.\n", win_strerror(rc));
    return (0);
  }
  TRACE (2, "popen.exit_code: %lu, popen.stdout: '%s'\n", popen.exit_code, popen.stdout_data);

  if (popen.exit_code || popen.stdout_data[0] == '\0')
     rc = 0;
  else if (callback)
     rc = popen.callback_ret;
  return (rc);
}

/**
 * Returns the expanded version of an environment variable.
 * Stolen from curl. But I wrote the Win32 part of it...
 *
 * \eg If `INCLUDE=c:\VC\include;%C_INCLUDE_PATH%` and
 *   + `C_INCLUDE_PATH=c:\MinGW\include`, the expansion returns
 *   + `c:\VC\include;c:\MinGW\include`.
 *
 * \note Windows (cmd only?) requires a trailing `%` in
 *       `%C_INCLUDE_PATH`.
 */
char *getenv_expand (const char *variable)
{
  const char *orig_var = variable;
  char *rc, *env = NULL;
  char  buf1 [MAX_ENV_VAR], buf2 [MAX_ENV_VAR];
  DWORD ret;

  /* Don't use getenv(); it doesn't find variable added after program was
   * started. Don't accept truncated results (i.e. rc >= sizeof(buf1)).
   */
  ret = GetEnvironmentVariable (variable, buf1, sizeof(buf1));
  if (ret > 0 && ret < sizeof(buf1))
  {
    env = buf1;
    variable = buf1;
  }
  if (strchr(variable, '%'))
  {
    /* buf2 == variable if not expanded.
     */
    ret = ExpandEnvironmentStrings (variable, buf2, sizeof(buf2));
    if (ret > 0 && ret < sizeof(buf2) &&
        !strchr(buf2,'%'))    /* no variables still un-expanded */
      env = buf2;
  }

  rc = (env && env[0]) ? STRDUP(env) : NULL;
  TRACE (3, "env: '%s', expanded: '%s'\n", orig_var, rc);
  return (rc);
}

/**
 * Similar to `getenv_expand()`, but always returns a non-NULL allocated string.
 */
char *getenv_expand2 (const char *variable)
{
  char *e = getenv_expand (variable);

  if (!e)
     return STRDUP (variable);
  return (e);
}

/**
 * As above, but expand an environment variable for SYSTEM.
 * This will do similar to what 4NT/TCC's `set /s foo` command does.
 *
 * \param  variable   The environment variable to expand.
 * \retval !NULL      An allocated string of the expanded result.
 * \retval NULL       If the expansion failed.
 *
 * \note If the SYSTEM is 64-bit and the program is 32-bit, the
 *       `ExpandEnvironmentStringsForUserA()` system-call always seems to
 *       use WOW64 to return a (possibly wrong) value.
 */
char *getenv_expand_sys (const char *variable)
{
  DWORD size = 0;
  char  buf [MAX_ENV_VAR];
  char *rc = NULL;

  init_misc();

  if (!p_ExpandEnvironmentStringsForUserA)
  {
    TRACE (1, "p_ExpandEnvironmentStringsForUserA not available. Using ExpandEnvironmentStrings() instead.\n");
    rc = getenv_expand (variable);
  }
  else
  {
    size = (*p_ExpandEnvironmentStringsForUserA) (NULL, variable, buf, sizeof(buf));
    if (size == 0)
         TRACE (1, "ExpandEnvironmentStringsForUser() failed: %s.\n",
                win_strerror(GetLastError()));
    else rc = STRDUP (buf);
    TRACE (3, "size: %lu, variable: '%s', expanded: '%s'\n", size, variable, rc);
  }
  return (rc);
}

/**
 * Get the values from the System environment block.
 *
 * Should later be called from `do_check()` to check for mismatches
 * in the User environment block.
 */
bool getenv_system (smartlist_t **sl)
{
  void          *env_blk;
  smartlist_t   *list;
  const wchar_t *env;

  init_misc();

  *sl = NULL;
  env_blk = NULL;

  if (!p_CreateEnvironmentBlock || !p_DestroyEnvironmentBlock)
  {
    TRACE (1, "'CreateEnvironmentBlock()' and/or 'DestroyEnvironmentBlock()' not available.\n");
    return (false);
  }

  if (!(*p_CreateEnvironmentBlock)(&env_blk, NULL, FALSE) || !env_blk)
  {
    TRACE (1, "'CreateEnvironmentBlock()' failed: %s.\n", win_strerror(GetLastError()));
    return (false);
  }

  list = smartlist_new();
  env  = env_blk;

  /* Loop over `env_blk`.
   * The block ends with two nulls (\0\0).
   */
  while (1)
  {
    size_t len = 1 + wcslen (env);
    char  *str = CALLOC (2*len, 1);

    if (!wchar_to_mbchar(str, 2*len, env))
    {
      FREE (str);
      break;
    }

    str [len-1] = '\0';
    smartlist_add (list, str);
    TRACE (2, "str: '%s'.\n", str);

    env += 1 + wcslen (env);
    if (!env[0])
       break;
  }

  if (!(*p_DestroyEnvironmentBlock) (env_blk))
     TRACE (1, "DestroyEnvironmentBlock() failed: %s.\n", win_strerror(GetLastError()));
  *sl = list;
  return (true);
}

/**
 * Translate a shell-pattern to a regular expression.
 * \see
 *   https://mail.python.org/pipermail/python-list/2003-August/244415.html
 */
char *translate_shell_pattern (const char *pattern)
{
  static char res [_MAX_PATH];
  char       *out = res;
  size_t      i, len = strlen (pattern);
  size_t      i_max  = sizeof(res) - 1;

  for (i = 0; i < len && i < i_max; i++)
  {
     int c = *pattern++;

     switch (c)
     {
       case '*':
            *out++ = '.';
            *out++ = '*';
            break;

       case '.':
            *out++ = '\\';
            *out++ = '.';
            break;

       case '+':
            *out++ = '\\';
            *out++ = '+';
            break;

       case '\\':
            *out++ = '\\';
            *out++ = '\\';
            break;

       case '$':
            *out++ = '\\';
            *out++ = '$';
            break;

       case '\"':
            *out++ = '\\';
            *out++ = '\"';
            break;

       case '?':
            *out++ = '.';
            break;

      default:
            *out++ = (char) c;
            break;
    }
  }

  if (i == i_max)
     WARN ("'pattern' in translate_shell_pattern() is too large (%u bytes).\n",
           (unsigned)len);
  *out = '\0';
  return (res);
}

/**
 * A simple test for `translate_shell_pattern()`.
 */
void test_shell_pattern (void)
{
  static const struct {
    const  char *test_pattern;
    const  char *expect;
  } patterns[] = {
    { "\\ ",     "\\\\ "   },
    { "* ",      ".* "     },
    { ". ",      "\\. "    },
    { "+ ",      "\\+ "    },
    { "\\ ",     "\\\\ "   },
    { "$ ",      "\\$ "    },
    { "? ",      ". "      },
    { "\" ",     "\\\" "   },
    { "foo-bar", "foo-bar" },
  };
  int i;

  for (i = 0; i < DIM(patterns); i++)
  {
    const char *result = translate_shell_pattern (patterns[i].test_pattern);
    bool        equal  = !strcmp (result, patterns[i].expect);

    printf ("out: '%-15s' -> %s\n", result, equal ? "OKAY" : "FAILED");
  }
}

/**
 * Dump a block of data as hex chars.
 * Starts with printing the data-length and offset.
 *
 * \eg
 * \code
 *   hex-dump WSL-tag:
 *   19: 0000: 00 00 00 00 03 AC B5 02-00 00 00 00 18 00 00 00 .....zz.........
 *       0010: A1 D3 BC                                        í++
 * \endcode
 */
void hex_dump (int dbg_level, const char *intro, const void *data_p, size_t datalen)
{
  const BYTE *data = (const BYTE*) data_p;
  UINT  ofs;

  if (opt.debug < dbg_level)
     return;

  printf ("  %s: %s\n", intro, datalen == 0 ? "no data" : "");
  if (datalen == 0)
     return;

  for (ofs = 0; ofs < datalen; ofs += 16)
  {
    UINT j;

    if (ofs == 0)
         printf ("  %u:%s%04X: ", (unsigned int)datalen,
                 datalen > 9999 ? " "    :
                 datalen > 999  ? "  "   :
                 datalen > 99   ? "   "  :
                 datalen > 9    ? "    " :
                                  "     ",
                 ofs);
    else printf ("         %04X: ", ofs);

    for (j = 0; j < 16 && j+ofs < datalen; j++)
        printf ("%02X%c", (unsigned)data[j+ofs],
                j == 7 && j+ofs < datalen-1 ? '-' : ' ');

    for ( ; j < 16; j++)       /* pad line to 16 positions */
        fputs ("   ", stdout);

    for (j = 0; j < 16 && j+ofs < datalen; j++)
    {
      int ch = data[j+ofs];

      if (ch < ' ')            /* non-printable */
           putc ('.', stdout);
      else putc (ch, stdout);
    }
    putc ('\n', stdout);
  }
}

/**
 * Format and return a printable string of maximum 10 bytes.
 */
const char *dump10 (const void *data, unsigned size)
{
  static char ret [20];
  unsigned  ofs;
  int       ch;

  for (ofs = 0; ofs < sizeof(ret)-4 && ofs < size; ofs++)
  {
    ch = ((const BYTE*)data) [ofs];
    if (ch < ' ')            /* non-printable */
         ret [ofs] = '.';
    else ret [ofs] = (char) ch;
    ret [ofs+1] = '\0';
  }
  if (ofs < size)
     strcat (ret, "...");
  return (ret);
}

/**
 * Format and return a hex-dump string for maximum 20 bytes.
 */
const char *dump20 (const void *data, unsigned size)
{
  static char ret [25];
  unsigned  ofs;
  int       ch;

  for (ofs = 0; ofs < sizeof(ret)-4 && ofs < size; ofs++)
  {
    ch = ((const BYTE*)data) [ofs];
    if (ch < ' ')            /* non-printable */
         ret [ofs] = '.';
    else ret [ofs] = (char) ch;
    ret [ofs+1] = '\0';
  }
  if (ofs < size)
     strcat (ret, "...");
  return (ret);
}

/**
 * Return a reverse of string `str` in place.
 */
char *str_reverse (char *str)
{
  int i, j;

  for (i = 0, j = (int)strlen(str)-1; i < j; i++, j--)
  {
    char c = str[i];
    str[i] = str[j];
    str[j] = c;
  }
  return (str);
}

/**
 * Functions for getting at Reparse Points (Junctions and Symlinks).
 * \see
 *   + http://blog.kalmbach-software.de/2008/02/
 *   + https://github.com/0xbadfca11/junk/blob/master/read_reparse_tag.cpp
 */
struct REPARSE_DATA_BUFFER {
       ULONG  ReparseTag;
       USHORT ReparseDataLength;
       USHORT Reserved;
       union {
         struct {
           USHORT SubstituteNameOffset;
           USHORT SubstituteNameLength;
           USHORT PrintNameOffset;
           USHORT PrintNameLength;
           ULONG  Flags;            /* It seems that the docs is missing this entry (at least 2008-03-07) */
           WCHAR  PathBuffer [1];
         } SymbolicLinkReparseBuffer;
         struct {
           USHORT SubstituteNameOffset;
           USHORT SubstituteNameLength;
           USHORT PrintNameOffset;
           USHORT PrintNameLength;
           WCHAR  PathBuffer [1];
         } MountPointReparseBuffer;
         struct {
           UCHAR DataBuffer [1];
         } GenericReparseBuffer;
       };
     };

/**
 * Extra "Windows Subsystem for Linux" `IO_REPARSE_TAG` values:
 *  \def IO_REPARSE_TAG_AF_UNIX    Used in WSL to represent a UNIX domain socket.
 *  \def IO_REPARSE_TAG_LX_FIFO    Used in WSL to represent a UNIX FIFO (named pipe).
 *  \def IO_REPARSE_TAG_LX_CHR     Used in WSL to represent a UNIX character special file.
 *  \def IO_REPARSE_TAG_LX_BLK     Used in WSL to represent a UNIX block special file.
 *  \def IO_REPARSE_TAG_LX_SYMLINK Used in WSL to represent a UNIX symbolic link.
 *
 * \ref https://winprotocoldoc.blob.core.windows.net/productionwindowsarchives/MS-FSCC/%5BMS-FSCC%5D.pdf
 * \ref https://github.com/0xbadfca11/lxsstat/wiki/WSL-filesystem
 */
#ifndef IO_REPARSE_TAG_AF_UNIX
#define IO_REPARSE_TAG_AF_UNIX  0x80000023
#endif

#ifndef IO_REPARSE_TAG_LX_FIFO
#define IO_REPARSE_TAG_LX_FIFO  0x80000024
#endif

#ifndef IO_REPARSE_TAG_LX_CHR
#define IO_REPARSE_TAG_LX_CHR   0x80000025
#endif

#ifndef IO_REPARSE_TAG_LX_BLK
#define IO_REPARSE_TAG_LX_BLK   0x80000026
#endif

#ifndef IO_REPARSE_TAG_LX_SYMLINK
#define IO_REPARSE_TAG_LX_SYMLINK   0xA000001DL
#endif

/* App execution aliases are commonly used by Windows Store apps.
 */
#ifndef IO_REPARSE_TAG_APPEXECLINK
#define IO_REPARSE_TAG_APPEXECLINK  0x8000001BL
#endif

/* A Cygwin AF_UNIX socket reparse points.
 */
#ifndef IO_REPARSE_TAG_CYGUNIX
#define IO_REPARSE_TAG_CYGUNIX  0x00006375
#endif

/*
 * Taken from Cygwin's winsup/cygwin/path.cc
 *
 * \ref https://github.com/mirror/newlib-cygwin/blob/master/winsup/cygwin/path.cc
 */
typedef struct REPARSE_LX_SYMLINK_BUFFER {
        DWORD ReparseTag;
        WORD  ReparseDataLength;
        WORD  Reserved;          /* == 0 */
        struct {
          DWORD FileType;        /* == 2 for symlinks */
          char  PathBuffer [1];  /* POSIX path as given to symlink(2).
                                  * Path is not \0 terminated.
                                  */
        } LxSymlinkReparseBuffer;
      } REPARSE_LX_SYMLINK_BUFFER;

typedef struct REPARSE_APPEXECLINK_BUFFER {
        DWORD ReparseTag;
        WORD  ReparseDataLength;
        WORD  Reserved;
        struct  {
          DWORD Version;       /* Take member name with a grain of salt. */
          WCHAR Strings [1];   /* Four serialized, NUL-terminated WCHAR strings:
                                *  0 - Package ID
                                *  1 - Entry Point
                                *  2 - Executable Path
                                *  3 - Application Type
                                */
        } AppExecLinkReparseBuffer;
      } REPARSE_APPEXECLINK_BUFFER;

static const char *tag_name (DWORD tag)
{
  return (tag == IO_REPARSE_TAG_AF_UNIX     ? "IO_REPARSE_TAG_AF_UNIX"     :
          tag == IO_REPARSE_TAG_LX_FIFO     ? "IO_REPARSE_TAG_LX_FIFO"     :
          tag == IO_REPARSE_TAG_LX_CHR      ? "IO_REPARSE_TAG_LX_CHR"      :
          tag == IO_REPARSE_TAG_LX_BLK      ? "IO_REPARSE_TAG_LX_BLK"      :
          tag == IO_REPARSE_TAG_LX_SYMLINK  ? "IO_REPARSE_TAG_LX_SYMLINK"  :
          tag == IO_REPARSE_TAG_APPEXECLINK ? "IO_REPARSE_TAG_APPEXECLINK" :
          tag == IO_REPARSE_TAG_CYGUNIX     ? "IO_REPARSE_TAG_CYGUNIX"     : NULL);
}

const char *last_reparse_err;
static bool was_special_link = false;

static bool reparse_err (int dbg_level, const char *fmt, ...)
{
  static char err_buf [1000];
  va_list args;

  err_buf [0] = '\0';
  if (!fmt)
     return (true);

  va_start (args, fmt);
  vsnprintf (err_buf, sizeof(err_buf), fmt, args);
  va_end (args);
  last_reparse_err = err_buf;
  TRACE (dbg_level, last_reparse_err);
  return (false);
}

#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE  (16*1024)
#endif

#ifndef IsReparseTagMicrosoft
#define IsReparseTagMicrosoft(_tag)     (_tag & 0x80000000)
#endif

#ifndef IsReparseTagNameSurrogate
#define IsReparseTagNameSurrogate(_tag) (_tag & 0x20000000)
#endif

#ifndef FSCTL_GET_REPARSE_POINT
  #define CTL_CODE(DeviceType, Function, Method, Access) ( ((DeviceType) << 16) | \
                                                           ((Access)     << 14) | \
                                                           ((Function)   << 2)  | \
                                                           (Method) )

  #define FSCTL_GET_REPARSE_POINT  CTL_CODE (FILE_DEVICE_FILE_SYSTEM /* == 0x00000009 */, \
                                             42,                                          \
                                             METHOD_BUFFERED         /* == 0 */,          \
                                             FILE_ANY_ACCESS         /* == 0 */)
#endif

/*
 * Mainly used in 'get_reparse_point()'. Hence the use of 'reparse_err()'.
 */
#define WIDECHAR_ERR(...)  reparse_err (__VA_ARGS__)

bool wchar_to_mbchar (char *result, size_t result_size, const wchar_t *w_buf)
{
  int    size_needed, rc;
  size_t w_size;
  DWORD  cp = CP_ACP;
  const  char *def_char = "?";

  /* Figure out the size needed for the conversion.
   */
  w_size = wcslen (w_buf);
  size_needed = WideCharToMultiByte (cp, 0, w_buf, (int)w_size, NULL, 0, def_char, NULL);
  if (size_needed == 0)
     return WIDECHAR_ERR (1, "1: WideCharToMultiByte(): %s", win_strerror(GetLastError()));

  if (size_needed > (int)result_size)
     return WIDECHAR_ERR (1, "result_size too small (%u). Need %d bytes for WideCharToMultiByte (\"%.10" WIDESTR_FMT "...\").",
                          result_size, size_needed, w_buf);

  rc = WideCharToMultiByte (cp, 0, w_buf, size_needed, result, size_needed, def_char, NULL);
  if (rc <= 0)
     return WIDECHAR_ERR (1, "2: WideCharToMultiByte(): %s", win_strerror(GetLastError()));

  result [rc] = '\0';    /* 0-terminate result and clear any previous error */
  WIDECHAR_ERR (0, NULL);

  TRACE (2, "rc: %d, result: '%s'\n", rc, result);
  return (true);
}

bool mbchar_to_wchar (wchar_t *result, size_t result_size, const char *a_buf)
{
  size_t size_needed = MultiByteToWideChar (CP_ACP, 0, a_buf, -1, NULL, 0);

  if (size_needed == 0 || size_needed >= result_size)  /* including NUL-termination */
     return (false);

  if (!MultiByteToWideChar(CP_ACP, 0, a_buf, (int)strlen(a_buf)+1, result, (int)size_needed))
  {
    TRACE (2, "GetLastError(): %s.\n", win_strerror(GetLastError()));
    return (false);
  }
  return (true);
}

/**
 * The `DeviceIoControl()` returns sensible information for a
 * remote `dir`. But the returned drive-letter is wrong!
 *
 * So it is a good idea to call `get_disk_type(dir[0])` and verify
 * that it returns `DRIVE_FIXED` first.
 *
 * \ref https://github.com/mirror/newlib-cygwin/blob/master/winsup/cygwin/path.cc
 */
bool get_reparse_point (const char *dir, char *result, size_t result_size)
{
  struct REPARSE_DATA_BUFFER *rdata;
  HANDLE      hnd;
  size_t      ofs, plen, slen;
  wchar_t    *print_name, *sub_name;
  const char *tag;
  DWORD       ret_len, share_mode, flags;
  bool        rc;

  was_special_link = false;
  last_reparse_err = NULL;
  *result = '\0';
  reparse_err (0, NULL);

  TRACE (2, "Finding target of dir: '%s'.\n", dir);

  share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
  hnd = CreateFile (dir, FILE_READ_EA, share_mode,
                    NULL, OPEN_EXISTING, flags, NULL);

  if (hnd == INVALID_HANDLE_VALUE)
     return reparse_err (1, "Could not open dir '%s'; %s", dir, win_strerror(GetLastError()));

  rdata = alloca (MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
  rc = DeviceIoControl (hnd, FSCTL_GET_REPARSE_POINT, NULL, 0,
                        rdata, MAXIMUM_REPARSE_DATA_BUFFER_SIZE,
                        &ret_len, NULL);

  if (ret_len < MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
     *((BYTE*)rdata + ret_len) = '\0';

  CloseHandle (hnd);

  if (!rc)
     return reparse_err (1, "DeviceIoControl(): ret_len: %lu, %s",
                         ret_len, win_strerror(GetLastError()));

  if (!IsReparseTagMicrosoft(rdata->ReparseTag))
     return reparse_err (1, "Not a Microsoft-reparse point - could not query data!");

  if (rdata->ReparseTag == IO_REPARSE_TAG_SYMLINK)
  {
    TRACE (2, "A Symbolic-Link:\n");

    slen     = rdata->SymbolicLinkReparseBuffer.SubstituteNameLength;
    ofs      = rdata->SymbolicLinkReparseBuffer.SubstituteNameOffset / sizeof(wchar_t);
    sub_name = rdata->SymbolicLinkReparseBuffer.PathBuffer + ofs;

    plen       = rdata->SymbolicLinkReparseBuffer.PrintNameLength;
    ofs        = rdata->SymbolicLinkReparseBuffer.PrintNameOffset / sizeof(wchar_t);
    print_name = rdata->SymbolicLinkReparseBuffer.PathBuffer + ofs;
  }
  else if (rdata->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
  {
    TRACE (2, "A Mount-Point:\n");

    slen     = rdata->MountPointReparseBuffer.SubstituteNameLength;
    ofs      = rdata->MountPointReparseBuffer.SubstituteNameOffset / sizeof(wchar_t);
    sub_name = rdata->MountPointReparseBuffer.PathBuffer + ofs;

    plen       = rdata->MountPointReparseBuffer.PrintNameLength;
    ofs        = rdata->MountPointReparseBuffer.PrintNameOffset / sizeof(wchar_t);
    print_name = rdata->MountPointReparseBuffer.PathBuffer + ofs;
  }
  else if (rdata->ReparseTag == IO_REPARSE_TAG_LX_SYMLINK)
  {
    const REPARSE_LX_SYMLINK_BUFFER *lx = (const REPARSE_LX_SYMLINK_BUFFER*) rdata;
    size_t sz = lx->ReparseDataLength - sizeof(lx->LxSymlinkReparseBuffer.FileType);

    TRACE (1, "WSL-symlink: '%.*s'\n", (int)sz, lx->LxSymlinkReparseBuffer.PathBuffer);
    if (result_size < sz)
       return (false);

    _strlcpy (result, lx->LxSymlinkReparseBuffer.PathBuffer, sz);
    was_special_link = true;
    return (true);
  }
  else if (rdata->ReparseTag == IO_REPARSE_TAG_APPEXECLINK)
  {
    const REPARSE_APPEXECLINK_BUFFER *app = (const REPARSE_APPEXECLINK_BUFFER*) rdata;
    const WCHAR *buf  = app->AppExecLinkReparseBuffer.Strings;
    size_t       size = app->ReparseDataLength / sizeof(WCHAR);
    int          i;
    size_t       app_len = 0;
    WCHAR       *app_name = NULL;
    const char  *app_exec[4] = { "package-ID ",
                                 "entry-point",
                                 "executable ",
                                 "app-type" };

    /* App execution aliases have a payload of four NUL-separated wide string:
     * package id, entry point, executable and application type.
     * Return the "executable" as the link-result.
     */
    for (i = 0; i < DIM(app_exec) && size > 0; i++)
    {
      size_t n = wcsnlen (buf, size - 1);

      TRACE (1, "AppX: %s: '%.*ws'\n", app_exec[i], (int)(n * sizeof(wchar_t)), buf);
      if (i == 2)
      {
        app_len = (n + 1) * sizeof(wchar_t);
        app_name = alloca (app_len);
        wcscpy (app_name, buf);
      }
      buf  += n + 1;
      size -= n + 1;
    }

    if (!app_name || result_size < app_len/2)
       return (false);

    was_special_link = true;
    return wchar_to_mbchar (result, result_size, app_name);
  }
  else
  {
    tag = tag_name (rdata->ReparseTag);
    if (tag)
    {
      reparse_err (1, "Unsupported reparse tag: %s == 0x%08lX%s\n",
                   tag, rdata->ReparseTag, IsReparseTagNameSurrogate(rdata->ReparseTag) ? " (surrogate)" : "");

      hex_dump (2, "hex-dump tag", &rdata->GenericReparseBuffer.DataBuffer, rdata->ReparseDataLength);
      return (false);
    }
    return reparse_err (1, "Unknown reparse tag: 0x%08lX??\n", rdata->ReparseTag);
  }

  /* Account for 0-termination
   */
  slen++;
  plen++;

  sub_name [slen/2]   = L'\0';
  print_name [plen/2] = L'\0';

  TRACE (2, "  SubstitutionName: '%S'\n", sub_name);
  TRACE (2, "  PrintName:        '%S'\n", print_name);

  hex_dump (3, "hex-dump sub_name", sub_name, slen);
  hex_dump (3, "hex-dump print_name", print_name, plen);

  if (result_size < plen)
     return (false);
  return wchar_to_mbchar (result, result_size, print_name);
}

/**
 * Return true if the last call to `get_reparse_point()` returned a
 * WSL or AppX type link.
 */
bool is_special_link (void)
{
  return (was_special_link);
}

/**
 * Check if a file-descriptor is coming from CygWin.
 * Applications now could call `is_cygwin_tty(STDIN_FILENO)` in order
 * to detect whether they are running from Cygwin/MSys terminal.
 *
 * By Mihail Konev `<k.mvc@ya.ru>` for the MinGW-w64 project.
 */
#undef FILE_EXISTS

#include <winternl.h>

#if defined(_MSC_VER)
  typedef struct {
          UNICODE_STRING Name;
        } OBJECT_NAME_INFORMATION2;

  #define ObjectNameInformation 1
#else
  #define OBJECT_NAME_INFORMATION2 OBJECT_NAME_INFORMATION
#endif

int is_cygwin_tty (int fd)
{
  typedef LONG (NTAPI *func_NtQueryObject) (HANDLE, OBJECT_INFORMATION_CLASS, void*, ULONG, ULONG*);
  func_NtQueryObject   p_NtQueryObject;
  intptr_t             h_fd;
  HMODULE              mod;

  /* NtQueryObject needs space for `OBJECT_NAME_INFORMATION.Name->Buffer` also.
   */
  char                      ntfn_bytes [sizeof(OBJECT_NAME_INFORMATION2) + MAX_PATH * sizeof(WCHAR)];
  OBJECT_NAME_INFORMATION2 *ntfn = (OBJECT_NAME_INFORMATION2*) ntfn_bytes;
  LONG                      status;
  ULONG                     ntfn_size = sizeof(ntfn_bytes);
  wchar_t                   c, *s;
  USHORT                    i;

  h_fd = _get_osfhandle (fd);
  if (!h_fd || h_fd == (intptr_t)INVALID_HANDLE_VALUE)
  {
    TRACE (2, "_get_osfhandle (%d) failed\n", fd);
    errno = EBADF;
    return (0);
  }

  mod = GetModuleHandle ("ntdll.dll");
  if (!mod)
  {
    TRACE (2, "Failed to load ntdll.dll; %s\n", win_strerror(GetLastError()));
    goto no_tty;
  }

  p_NtQueryObject = GETPROCADDRESS (func_NtQueryObject, mod, "NtQueryObject");
  if (!p_NtQueryObject)
  {
    TRACE (2, "NtQueryObject() not found in ntdll.dll.\n");
    goto no_tty;
  }

  memset (ntfn, 0, ntfn_size);
  status = (*p_NtQueryObject) ((HANDLE)h_fd, ObjectNameInformation, ntfn, ntfn_size, &ntfn_size);
  if (!NT_SUCCESS(status))
  {
    TRACE (2, "NtQueryObject() failed; status: %ld\n", status);

    /* If it is not NUL (i.e. `\Device\Null`, which would succeed),
     * then normal `isatty()` could be consulted.
     */
    if (_isatty(fd))
       return (1);
    goto no_tty;
  }

  s = ntfn->Name.Buffer;
  s [ntfn->Name.Length/sizeof(WCHAR)] = 0;

  /* Look for `\Device\NamedPipe\(cygwin|msys)-[a-fA-F0-9]{16}-pty[0-9]{1,4}-(from-master|to-master|to-master-cyg)`
   */
  if (wcsncmp(s, L"\\Device\\NamedPipe\\", 18))
  {
    TRACE (2, "Not a Cygwin pipe: '%" WIDESTR_FMT "'.\n", s);
    goto no_tty;
  }

  s += 18;

  if (!wcsncmp(s, L"cygwin-", 7))
       s += 7;
  else if (!wcsncmp(s, L"msys-", 5))
       s += 5;
  else goto no_tty;

  for (i = 0; i < 16; i++)
  {
    c = *s++;

    if (!wcschr(L"abcdefABCDEF0123456789", c))
       goto no_tty;
  }

  if (wcsncmp(s, L"-pty", 4))
     goto no_tty;
  s += 4;

  for (i = 0; i < 4; i++, s++)
  {
    c = *s;
    if (!(c >= '0' && c <= '9'))
        break;
  }

  if (i == 0)
     goto no_tty;

  if (wcscmp(s, L"-from-master") &&
      wcscmp(s, L"-to-master")   &&
      wcscmp(s, L"-to-master-cyg"))
     goto no_tty;

  return (1);

no_tty:
  errno = EINVAL;
  return (0);
}

/*
 * A "CPU Temperature Monitor" sample rewritten from:
 *   https://www.alcpu.com/CoreTemp/developers.html
 * and
 *   https://www.alcpu.com/CoreTemp/main_data/CoreTempSDK.zip
 *
 * The following piece of code demonstrates how you could load the DLL dynamically at run time
 * This sample demonstrates the use of a function to retrieve the data from the DLL rather than
 * a proxy class like in the sample above, this can be used by C users or other languages.
 */
typedef struct CORE_TEMP_SHARED_DATA {
        unsigned int   uiLoad [256];
        unsigned int   uiTjMax [128];
        unsigned int   uiCoreCnt;
        unsigned int   uiCPUCnt;
        float          fTemp [256];
        float          fVID;
        float          fCPUSpeed;
        float          fFSBSpeed;
        float          fMultipier;
        char           sCPUName [100];
        unsigned char  ucFahrenheit;
        unsigned char  ucDeltaToTjMax;
      } CORE_TEMP_SHARED_DATA;

typedef struct CORE_TEMP_SHARED_DATA_EX {
        CORE_TEMP_SHARED_DATA ver1;

        // Added for Ver. 2 of the protocol.
        unsigned char ucTdpSupported;
        unsigned char ucPowerSupported;
        unsigned int  uiStructVersion;
        unsigned int  uiTdp [128];
        float         fPower [128];
        float         fMultipliers [256];
      } CORE_TEMP_SHARED_DATA_EX;

typedef BOOL (WINAPI *func_GetCoreTempInfo) (CORE_TEMP_SHARED_DATA *pData);

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32  0x00000800
#endif

static bool get_core_temp_info (CORE_TEMP_SHARED_DATA *ct_data, const char *indent)
{
  func_GetCoreTempInfo p_GetCoreTempInfoAlt;
  ULONG   index;
  UINT    i, j;
  char    temp_type;
#if (IS_WIN64)
  HMODULE ct_dll = LoadLibraryEx ("GetCoreTempInfo.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
  HMODULE ct_dll = LoadLibrary ("GetCoreTempInfo.dll");
#endif

  if (!ct_dll)
  {
    TRACE (1, "\"GetCoreTempInfo.dll\" is not installed.\n");
    return (false);
  }

  p_GetCoreTempInfoAlt = GETPROCADDRESS (func_GetCoreTempInfo, ct_dll, "fnGetCoreTempInfoAlt");
  if (!p_GetCoreTempInfoAlt)
  {
    TRACE (1, "\nError: The function \"fnGetCoreTempInfo\" in \"GetCoreTempInfo.dll\" could not be found.\n");
    FreeLibrary (ct_dll);
    return (false);
  }

  memset (ct_data, '\0', sizeof(*ct_data));
  if (!(*p_GetCoreTempInfoAlt)(ct_data))
  {
    C_printf ("\"Core Temp\" is not running or shared memory could not be read.\n");
    FreeLibrary (ct_dll);
    return (false);
  }

  temp_type = ct_data->ucFahrenheit ? 'F' : 'C';

  /* This should be the 1st "normal" line of output; no indenting
   */
  C_printf ("~6CPU Name:~0  %s\n"
            "%s~6CPU Speed:~0 %.2fMHz (%.2f x %.2f)\n",
            ct_data->sCPUName, indent,
            (double)ct_data->fCPUSpeed, (double)ct_data->fFSBSpeed, (double)ct_data->fMultipier);

  C_printf ("%s~6CPU VID~0:   %.4fv, physical CPUs: ~6%u~0, cores per CPU: ~6%u~0\n",
            indent, (double)ct_data->fVID, ct_data->uiCPUCnt, ct_data->uiCoreCnt);

  for (i = 0; i < ct_data->uiCPUCnt; i++)
  {
    C_printf ("%s~6CPU #%u~0, Tj.max: ~6%u%c~0:\n", indent, i, ct_data->uiTjMax[i], temp_type);
    for (j = 0; j < ct_data->uiCoreCnt; j++)
    {
      const char *indent2 = "";

      if (j < 10 && ct_data->uiCoreCnt >= 10)
         indent2 = " ";

      index = j + (i * ct_data->uiCoreCnt);
      if (ct_data->ucDeltaToTjMax)
           C_printf ("%s  ~6Core #%s%lu~0: %.2f%c to Tj.max, %2u%% load\n",
                     indent, indent2, index, (double)ct_data->fTemp[index], temp_type, ct_data->uiLoad[index]);
      else C_printf ("%s  ~6Core #%s%lu~0: %.2f%c, %2u%% load\n",
                     indent, indent2, index, (double)ct_data->fTemp[index], temp_type, ct_data->uiLoad[index]);
    }
  }
  FreeLibrary (ct_dll);
  return (true);
}

/**
 * Print the information from the CoreTemp driver.
 */
bool print_core_temp_info (void)
{
  CORE_TEMP_SHARED_DATA ct_data;

  /* Try to open the 'Core Temp' mutex
   */
  HANDLE ev = CreateEvent (NULL, FALSE, FALSE, "Local\\PowerInfoMutex");
  bool   is_running = (ev == NULL && GetLastError() == ERROR_INVALID_HANDLE);

  if (is_running)
  {
    /* From:
     *   https://docs.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventa
     *
     * If lpName matches the name of another kind of object in the same namespace (such as an existing
     * semaphore, mutex, waitable timer, job, or file-mapping object), the function fails and the GetLastError
     * function returns ERROR_INVALID_HANDLE. This occurs because these objects share the same namespace.
     */
    return get_core_temp_info (&ct_data, "              ");
  }
  C_printf ("\"Core Temp\" is not running.\n");
  return (false);
}

/**
 * A simple rotating spinner text-bar running it's own thread.
 */
static HANDLE spinner_hnd = INVALID_HANDLE_VALUE;
static bool   spin_pause = false;
static int    spin_idx = 0;

#ifdef USE_FANCY_BAR
  static int  spin_y_pos;
  static char spin_bar[] = "\xB1\xB1\xB1\xB1\xB1\xB1\xB1\xB1\xB1\xB1\xB1\xB1\xB1\xB1\xB1\r";

  static HANDLE  hStdout = INVALID_HANDLE_VALUE;
  static CONSOLE_SCREEN_BUFFER_INFO csbi;
#endif

/**
 * The timer-callback that performs the spinner-bar.
 */
static void CALLBACK spinner_handler (void *param, BOOLEAN timer_fired)
{
  if (spin_pause)
     return;

#ifdef USE_FANCY_BAR
  COORD coord;

  coord.X = spin_idx++;
  coord.Y = spin_y_pos;
  SetConsoleCursorPosition (hStdout, coord);
  C_puts ("~2*~0");
  spin_idx %= sizeof(spin_bar);

#else
  fputc ("-\\|//" [spin_idx++], stdout);
  fputc ('\b', stdout);
  spin_idx &= 3;
#endif

  ARGSUSED (param);
  ARGSUSED (timer_fired);
}

/**
 * Create a kernel32 timer with a 200 msec period.
 */
void spinner_start (void)
{
  DWORD flags = WT_EXECUTELONGFUNCTION;

  if (!_isatty(STDOUT_FILENO))
     return;

  if (spinner_hnd && spinner_hnd != INVALID_HANDLE_VALUE)
     return;

  if (!CreateTimerQueueTimer(&spinner_hnd, NULL, spinner_handler, NULL, 200, 200, flags))
  {
    TRACE (1, "CreateTimerQueueTimer() failed %s\n", win_strerror(GetLastError()));
    spinner_hnd = NULL;
  }
  else
  {
    spin_idx = 0;
#ifdef USE_FANCY_BAR
    hStdout = GetStdHandle (STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo (hStdout, &csbi);
    spin_y_pos = csbi.dwCursorPosition.Y;
    C_printf ("~8%s~0", spin_bar);
#else
    fputc (' ', stdout);
#endif
  }
}

/**
 * Pause printing of the spinner bar.
 */
void spinner_pause (bool on_off)
{
  spin_pause = on_off;
}

/**
 * Stop the spinner timer-thread.
 */
void spinner_stop (void)
{
  if (spinner_hnd && spinner_hnd != INVALID_HANDLE_VALUE)
  {
#ifdef USE_FANCY_BAR
    printf ("%*s~0", (int)strlen(spin_bar), "");
#else
    fputc (' ', stdout);
#endif
    DeleteTimerQueueTimer (NULL, spinner_hnd, NULL);
  }
  spin_idx = 0;
  spinner_hnd = INVALID_HANDLE_VALUE;
}
