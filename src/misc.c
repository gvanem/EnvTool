
/*
 * Various support functions for EnvTool.
 * fnmatch(), basename() and dirname() are taken from djgpp and modified.
 */

/*
 * So that <sec_api/string_s.h> gets included in <string.h>.
 */
#ifndef MINGW_HAS_SECURE_API
#define MINGW_HAS_SECURE_API 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <io.h>
#include <windows.h>
#include <wincon.h>
#include <winioctl.h>
#include <shlobj.h>

/*
 * Suppress warning:
 *   imagehlp.h(1873): warning C4091: 'typedef ': ignored on left of '' when no variable is declared
 */
#ifdef _MSC_VER
#pragma warning (disable:4091)
#endif

#include <imagehlp.h>

#if defined(__CYGWIN__)
  #include <cygwin/version.h>
#endif

#include "color.h"
#include "envtool.h"

#ifndef IMAGE_FILE_MACHINE_ALPHA
#define IMAGE_FILE_MACHINE_ALPHA 0x123456
#endif

#ifndef KEY_WOW64_32KEY
#define KEY_WOW64_32KEY         0x0200
#endif

#ifndef KEY_WOW64_64KEY
#define KEY_WOW64_64KEY         0x0100
#endif

#define TOUPPER(c)    toupper ((int)(c))
#define TOLOWER(c)    tolower ((int)(c))

#if !defined(__CYGWIN__)

struct mem_head {
       unsigned long    marker;
       size_t           size;
       char             file [20];  /* allocated at file/line */
       unsigned         line;
       struct mem_head *next;       /* 36 bytes = 24h */
     };

static struct mem_head *mem_list = NULL;

#if !defined(_CRTDBG_MAP_ALLOC) && !defined(__CYGWIN__)
  static DWORD  mem_max      = 0;  /* Max bytes allocated at one time */
  static size_t mem_reallocs = 0;  /* # of realloc() */
#endif

static size_t mem_allocs = 0;      /* # of allocations */
static size_t mem_frees  = 0;      /* # of mem-frees */

static void add_to_mem_list (struct mem_head *m, const char *file, unsigned line)
{
  m->next = mem_list;
  m->line = line;
  _strlcpy (m->file, file, sizeof(m->file));
  mem_list = m;
}

#define IS_MARKER(m) ( ( (m)->marker == MEM_MARKER) || ( (m)->marker == MEM_FREED) )

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
    break;
  }
  if (i > max_loops)
     FATAL ("max-loops (%u) exceeded. mem_list munged from line %u!?\n",
            max_loops, line);
}

#ifdef NOT_USED
static struct mem_head *mem_list_get_head (void *ptr)
{
  struct mem_head *m;

  for (m = mem_list; m; m = m->next)
      if (m == ptr)
         return (m);
  return (NULL);
}
#endif
#endif  /* !__CYGWIN__ */

/*
 * Open a fname and check if there's a "PK" signature in header.
 */
int check_if_zip (const char *fname)
{
  static const char header[4] = { 'P', 'K', 3, 4 };
  const char *ext;
  char   buf [sizeof(header)];
  FILE  *f;
  int    rc = 0;

  /* Return 0 if extension is neither ".egg" nor ".zip"
   */
  ext = get_file_ext (fname);
  if (stricmp(ext,"egg") && stricmp(ext,"zip"))
     return (0);

  f = fopen (fname, "rb");
  if (f)
  {
    rc = (fread(&buf,1,sizeof(buf),f) == sizeof(buf) &&
          !memcmp(&buf,&header,sizeof(buf)));
    fclose (f);
  }
  if (rc)
     DEBUGF (1, "\"%s\" is a ZIP-file.\n", fname);
  return (rc);
}

/*
 * Open a fname and check if there's a "GZIP" or "TAR.GZ" signature in header.
 */
int check_if_gzip (const char *fname)
{
  static const BYTE header[4] = { 0x1F, 0x8B, 0x08, 0x08 };
  const char *ext;
  char   buf [sizeof(header)];
  FILE  *f;
  BOOL   is_gzip, is_tgz;
  int    rc = 0;

  /* Accept only ".gz" or ".tgz" extensions.
   */
  ext = get_file_ext (fname);
  is_gzip = (stricmp(ext,"gz") == 0);
  is_tgz  = (stricmp(ext,"tgz") == 0);

  if (!is_gzip && !is_tgz)
  {
    DEBUG_NL (1);
    DEBUGF (1, "\"%s\" does have wrong extension: '%s'.\n", fname, ext);
    return (0);
  }

  f = fopen (fname, "rb");
  if (f)
  {
    rc = (fread(&buf,1,sizeof(buf),f) == sizeof(buf));
    if (is_gzip && !memcmp(&buf,&header,sizeof(buf)))
       rc += 1;
    else if (is_tgz && !memcmp(&buf,&header,sizeof(buf)-1))
       rc += 1;
    fclose (f);
  }
  DEBUG_NL (1);
  DEBUGF (1, "\"%s\" is %sa GZIP-file.\n", fname, rc == 2 ? "": "not ");
  return (rc == 2);
}

/*
 * Open a GZIP-file and extract first line to check if it contains a
 * ".so real-file-name". This is typical for CygWin man-pages.
 * Return result as "<dir_name>/real-file-name". Which is just an
 * assumption; the "real-file-name" can be anywhere on %MANPATH%.
 */
static char gzip_link_name [_MAX_PATH];

static int gzip_cb (char *buf, int index)
{
  if (index == 0 && strlen(buf) < sizeof(gzip_link_name)-3 &&
      sscanf(buf, ".so %s", gzip_link_name) == 1)
     return (1);
  return (-1);  /* causes popen_run() to quit */
}

const char *get_gzip_link (const char *file)
{
  static char gzip [_MAX_PATH];
  static BOOL done = FALSE;

  if (!done)
  {
    const char *p = searchpath ("gzip.exe", "PATH");
    if (p)
      _strlcpy (gzip, slashify(p,'\\'), sizeof(gzip));
    done = TRUE;
  }

  if (!gzip[0])
     return (NULL);

  gzip_link_name[0] = '\0';
  if (popen_runf(gzip_cb, "\"%s\" -cd %s 2> %s", gzip, file, DEV_NULL) > 0)
  {
    static char fqfn_name [_MAX_PATH];
    char       *dir_name = dirname (file);

    DEBUGF (2, "gzip_link_name: \"%s\", dir_name: \"%s\".\n", gzip_link_name, dir_name);
    snprintf (fqfn_name, sizeof(fqfn_name), "%s%c%s", dir_name, DIR_SEP, gzip_link_name);
    FREE (dir_name);
    if (opt.show_unix_paths)
       return slashify (fqfn_name, '/');
    return (fqfn_name);
  }
  return (NULL);
}

/*
 * Open a fname, read the optional header in PE-header.
 *  - For verifying it's signature.
 *  - Showing the version information (if any) in it's resources.
 */
static const IMAGE_DOS_HEADER *dos;
static const IMAGE_NT_HEADERS *nt;
static char  file_buf [sizeof(*dos) + 4*sizeof(*nt)];

static enum Bitness last_bitness = -1;

int check_if_PE (const char *fname, enum Bitness *bits)
{
  BOOL   is_exe, is_pe;
  BOOL   is_32Bit = FALSE;
  BOOL   is_64Bit = FALSE;
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
    DEBUGF (3, "%s: failed fread(). errno: %d\n", fname, errno);
    return (FALSE);
  }

  dos = (const IMAGE_DOS_HEADER*) file_buf;
  nt  = (const IMAGE_NT_HEADERS*) ((const BYTE*)file_buf + dos->e_lfanew);

  DEBUG_NL (3);

  /* Probably not a PE-file at all.
   * Check 'nt < file_buf' too in case 'e_lfanew' folds 'nt' to a negative value.
   */
  if ( (char*)nt > file_buf + sizeof(file_buf) ||
       (char*)nt < file_buf )
  {
    DEBUGF (3, "%s: NT-header at wild offset.\n", fname);
    return (FALSE);
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

  DEBUGF (3, "%s: is_exe: %d, is_pe: %d, is_32Bit: %d, is_64Bit: %d.\n",
          fname, is_exe, is_pe, is_32Bit, is_64Bit);
  return (is_exe && is_pe);
}

/*
 * Verify the checksum of last opened file above.
 * if 'CheckSum == 0' is set to 0, it meants "don't care"
 * (similar to in UDP).
 */
int verify_PE_checksum (const char *fname)
{
  const IMAGE_OPTIONAL_HEADER64 *oh;
  DWORD file_sum, header_sum, calc_chk_sum, rc;

  assert (nt);

  if (last_bitness == bit_32)
    file_sum = nt->OptionalHeader.CheckSum;

  else if (last_bitness == bit_64)
  {
    oh = (const IMAGE_OPTIONAL_HEADER64*) &nt->OptionalHeader;
    file_sum = oh->CheckSum;
  }
  else
    return (FALSE);

  DEBUGF (1, "last_bitness: %u, Opt magic: 0x%04X, file_sum: 0x%08lX\n",
             last_bitness, nt->OptionalHeader.Magic, (u_long)file_sum);

  rc = MapFileAndCheckSum ((PTSTR)fname, &header_sum, &calc_chk_sum);
  DEBUGF (1, "rc: %lu, 0x%08lX, 0x%08lX\n",
          (u_long)rc, (u_long)header_sum, (u_long)calc_chk_sum);
  return (file_sum == 0 || header_sum == calc_chk_sum);
}

/*
 * Check if running under WOW64; "Windows 32-bit on Windows 64-bit".
 * Ref:
 *   http://en.wikipedia.org/wiki/WoW64
 *   http://everything.explained.today/WoW64/
 */
BOOL is_wow64_active (void)
{
  static BOOL rc    = FALSE;
  static BOOL init  = FALSE;
  static BOOL wow64 = FALSE;

  if (init)
     goto quit;

#if (IS_WIN64 == 0)
  {
    typedef BOOL (WINAPI *func_IsWow64Process) (HANDLE proc, BOOL *wow64);
    func_IsWow64Process p_IsWow64Process;

    const char *dll = "kernel32.dll";
    HANDLE hnd = LoadLibrary (dll);

    if (!hnd || hnd == INVALID_HANDLE_VALUE)
    {
      DEBUGF (1, "Failed to load %s; %s\n",
              dll, win_strerror(GetLastError()));
      init = TRUE;
      return (rc);
    }

    p_IsWow64Process = (func_IsWow64Process) GetProcAddress (hnd, "IsWow64Process");
    if (!p_IsWow64Process)
    {
      DEBUGF (1, "Failed to find \"p_IsWow64Process()\" in %s; %s\n",
              dll, win_strerror(GetLastError()));
      FreeLibrary (hnd);
      init = TRUE;
      return (rc);
    }

    if (p_IsWow64Process)
       if ((*p_IsWow64Process) (GetCurrentProcess(), &wow64))
          rc = wow64;
    FreeLibrary (hnd);
  }
#endif  /* IS_WIN64 */

  init = TRUE;

quit:
  DEBUGF (2, "IsWow64Process(): rc: %d, wow64: %d.\n", rc, wow64);
  return (rc);
}

/*
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
  return (type == REG_SZ               ? "REG_SZ" :
          type == REG_MULTI_SZ         ? "REG_MULTI_SZ" :
          type == REG_EXPAND_SZ        ? "REG_EXPAND_SZ" :
          type == REG_LINK             ? "REG_LINK" :
          type == REG_BINARY           ? "REG_BINARY" :
          type == REG_DWORD            ? "REG_DWORD" :
          type == REG_RESOURCE_LIST    ? "REG_RESOURCE_LIST" :
          type == REG_DWORD_BIG_ENDIAN ? "REG_DWORD_BIG_ENDIAN" :
          type == REG_QWORD            ? "REG_QWORD" : "?");
}

const char *reg_top_key_name (HKEY key)
{
  return (key == HKEY_LOCAL_MACHINE ? "HKEY_LOCAL_MACHINE" :
          key == HKEY_CURRENT_USER  ? "HKEY_CURRENT_USER" :
          "?");
}

const char *reg_access_name (REGSAM acc)
{
  #define ADD_VALUE(v)  { v, #v }

  static const struct search_list access[] = {
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

/*
 * Swap bytes in a 32-bit value.
 */
DWORD reg_swap_long (DWORD val)
{
  return ((val & 0x000000FFU) << 24) |
         ((val & 0x0000FF00U) <<  8) |
         ((val & 0x00FF0000U) >>  8) |
         ((val & 0xFF000000U) >> 24);
}

/*
 * Removes end-of-line termination from a string.
 */
char *strip_nl (char *s)
{
  char *p;

  if ((p = strrchr(s,'\n')) != NULL) *p = '\0';
  if ((p = strrchr(s,'\r')) != NULL) *p = '\0';
  return (s);
}

/*
 * Trim leading blanks (space/tab) from a string.
 */
char *str_ltrim (char *s)
{
  assert (s != NULL);

  while (s[0] && s[1] && isspace((int)s[0]))
       s++;
  return (s);
}

/*
 * Trim trailing blanks (space/tab) from a string.
 */
char *str_rtrim (char *s)
{
  size_t n;

  assert (s != NULL);
  n = strlen (s);
  while (n)
  {
    if (!isspace((int)s[--n]))
       break;
    s[n] = '\0';
  }
  return (s);
}

/*
 * Trim leading and trailing blanks (space/tab) from a string.
 */
char *str_trim (char *s)
{
  return str_rtrim (str_ltrim(s));
}

/*
 * Return the left-trimmed place where paths 'p1' and 'p2' are similar.
 * Not case sensitive. Treats '/' and '\\' equally.
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
 * Return nicely formatted string "xx,xxx,xxx"
 * with thousand separators (left adjusted).
 */
const char *qword_str (UINT64 val)
{
  static char buf [30];
  char   tmp [30], *p;
  int    i, j, len = snprintf (tmp, sizeof(tmp), "%" U64_FMT, val);

  p = buf + len;
  *p-- = '\0';

  for (i = len, j = -1; i >= 0; i--, j++)
  {
    if (j > 0 && (j % 3) == 0)
      *p-- = ',';
    *p-- = tmp[i];
  }
  return (p+1);
}

const char *dword_str (DWORD val)
{
  return qword_str ((UINT64)val);
}

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
           pattern = range_match (pattern, test, flags & FNM_FLAG_NOCASE);
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
           /* FALLTHROUGH */

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

char *fnmatch_res (int rc)
{
  return (rc == FNM_MATCH   ? "FNM_MATCH"   :
          rc == FNM_NOMATCH ? "FNM_NOMATCH" : "??");
}

/*
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

/*
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

  if (*fname && fname[1] == ':')
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

/*
 * Create a full MS-DOS path name from the components.
 */
void make_path (char *path, const char *drive, const char *dir, const char *filename, const char *ext)
{
#if !defined(__CYGWIN__)
  _makepath (path, drive, dir, filename, ext);
#endif
}

#if !defined(__CYGWIN__)
/*
 * Create a CygWin compatible path name from a Windows path.
 */
char *make_cyg_path (const char *path, char *result)
{
  char *p = STRDUP (slashify(path, '/'));
  char  buf [_MAX_PATH+20];

  if (strlen(p) > 2 && p[1] == ':' && IS_SLASH(p[2]))
  {
    sprintf (buf, "/cygdrive/%c/%s", tolower(p[0]), p+3);
    _strlcpy (result, buf, _MAX_PATH);
  }
  else
    _strlcpy (result, p, _MAX_PATH);
  FREE (p);
  return (result);
}

/*
 * The wide version of the above.
 */
wchar_t *make_cyg_pathw (const wchar_t *path, wchar_t *result)
{
  wchar_t *p = WCSDUP (path);
  wchar_t  buf [_MAX_PATH+20];

  if (wcslen(p) > 2 && p[1] == ':' && IS_SLASH(p[2]))
  {
    _swprintf (buf, L"/cygdrive/%c/%s", towlower(p[0]), p+3);
    wcsncpy (result, buf, _MAX_PATH);
  }
  else
    wcsncpy (result, p, _MAX_PATH);
  FREE (p);
  return (result);
}
#endif

/*
 * Canonize file and paths names. E.g. convert this:
 *   g:\mingw32\bin\../lib/gcc/x86_64-w64-mingw32/4.8.1/include
 * into something more readable:
 *   g:\mingw32\lib\gcc\x86_64-w64-mingw32\4.8.1\include
 *
 * I.e. turns 'path' into a fully-qualified path.
 *
 * Note: the 'path' doesn't have to exist.
 *       assumes 'result' is at least '_MAX_PATH' characters long (if non-NULL).
 */
char *_fix_path (const char *path, char *result)
{
  if (!path || !*path)
  {
    DEBUGF (1, "given a bogus 'path': '%s'\n", path);
    errno = EINVAL;
    return (NULL);
  }

  if (!result)
     result = CALLOC (_MAX_PATH, 1);

 /* GetFullPathName() doesn't seems to handle
  * '/' in 'path'. Convert to '\\'.
  *
  * Note: the 'result' file or path may not exists.
  *       Use 'FILE_EXISTS()' to test.
  *
  * to-do: maybe use GetLongPathName()?
  */
  path = slashify (path, '\\');
  if (!GetFullPathName(path, _MAX_PATH, result, NULL))
  {
    DEBUGF (2, "GetFullPathName(\"%s\") failed: %s\n",
            path, win_strerror(GetLastError()));

    /* 'GetFullPathName()' handle the case where 'path == result'.
     * So only copy the result if 'path != result'.
     */
    if (result != path)
       _strlcpy (result, path, _MAX_PATH);
  }
  return _fix_drive (result);
}

/*
 * For consistency, report drive-letter in lower case.
 */
char *_fix_drive (char *path)
{
  size_t len = strlen (path);

  if (len >= 3 && path[1] == ':' && IS_SLASH(path[2]))
     path[0] = TOLOWER (path[0]);
  return (path);
}

/*
 * Returns ptr to 1st character in file's extension.
 * Returns ptr to '\0' if no extension.
 */
const char *get_file_ext (const char *file)
{
  const char *end, *dot, *s;

  assert (file);
  while ((s = strpbrk(file, ":/\\")) != NULL)  /* step over drive/path part */
     file = s + 1;

  end = strrchr (file, '\0');
  dot = strrchr (file, '.');
  return ((dot > file) ? dot+1 : end);
}

/*
 * Create a %TEMP-file and return it's allocated name.
 */
char *create_temp_file (void)
{
#if defined(__POCC__)
  char *tmp = tmpnam ("envtool-tmp");
#else
  char *tmp = _tempnam (NULL, "envtool-tmp");
#endif

  if (tmp)
  {
    char *t = STRDUP (tmp);

    DEBUGF (2, " %s() tmp: '%s'\n", __FUNCTION__, tmp);
    free (tmp);
    return (t);     /* Caller must FREE() */
  }
  DEBUGF (2, " %s() _tempname() failed: %s\n", __FUNCTION__, strerror(errno));
  return (NULL);
}

/*
 * Turn off default error-mode. E.g. if a CD-ROM isn't ready, we'll get a GUI
 * popping up to notify us. Turn that off and handle such errors ourself.
 *
 * SetErrorMode()       is per process.
 * SetThreadErrorMode() is per thread on Win-7+.
 */
void set_error_mode (int on_off)
{
  static UINT old_mode = 0;

  if (on_off)
       SetErrorMode (old_mode);
  else old_mode = SetErrorMode (SEM_FAILCRITICALERRORS);

  DEBUGF (2, "on_off: %d, SetErrorMode (%d): %s\n",
          on_off, old_mode, win_strerror(GetLastError()));
}

/*
 * Check if a disk is ready. disk is ['A'..'Z'].
 */
int disk_ready (int disk)
{
  int    rc1 = 0, rc2 = 0;
  char   path [8];
  HANDLE hnd;

  snprintf (path, sizeof(path), "\\\\.\\%c:", toupper(disk));
  set_error_mode (0);

  DEBUGF (2, "Calling CreateFile (\"%s\").\n", path);
  hnd = CreateFile (path, GENERIC_READ | FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (hnd == INVALID_HANDLE_VALUE)
  {
    DEBUGF (2, "  failed: %s\n", win_strerror(GetLastError()));
    rc1 = -1;
    goto quit;
  }
  rc1 = 1;

#if 0
  {
    DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                   FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SECURITY;

    char  buf [sizeof(FILE_NOTIFY_INFORMATION) + _MAX_PATH + 3];
    DWORD size = sizeof(buf);
    BOOL  rd_change = ReadDirectoryChangesA (hnd, &buf, size, FALSE, filter, NULL, NULL, NULL);

    if (!rd_change)
    {
      DEBUGF (2, "ReadDirectoryChanges(): failed: %s\n", win_strerror(GetLastError()));
      rc2 = 0;
    }
    else
    {
      const FILE_NOTIFY_INFORMATION *fni = (const FILE_NOTIFY_INFORMATION*) &buf;

      DEBUGF (2, "fni->NextEntryOffset: %lu\n", fni->NextEntryOffset);
      DEBUGF (2, "fni->Action:          %lu\n", fni->Action);
      DEBUGF (2, "fni->FileNameLength:  %lu\n", fni->FileNameLength);
      DEBUGF (2, "fni->FileName:        \"%.*S\"\n", (int)fni->FileNameLength, fni->FileName);
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

/*
 * Return TRUE if this program is executed as an 'elevated' process.
 * Taken from Python 3.5's "src/PC/bdist_wininst/install.c".
 */
BOOL is_user_admin (void)
{
  typedef BOOL (WINAPI *func_IsUserAnAdmin) (void);
  func_IsUserAnAdmin p_IsUserAnAdmin;

  HMODULE shell32 = LoadLibrary ("shell32.dll");
  BOOL    rc;

  /* This function isn't guaranteed to be available.
   */
  if (!shell32 || shell32 == INVALID_HANDLE_VALUE)
     return (FALSE);

  p_IsUserAnAdmin = (func_IsUserAnAdmin) GetProcAddress (shell32, "IsUserAnAdmin");
  if (!p_IsUserAnAdmin)
       rc = FALSE;
  else rc = (*p_IsUserAnAdmin)();

  FreeLibrary (shell32);
  return (rc);
}

/*
 * Return name of logged-in user.
 * First try GetUserNameEx() available in Win-2000 Pro.
 * Then fall-back to a GetUserName() if not present in Secur32.dll.
 * Ref:
 *   https://msdn.microsoft.com/en-us/library/windows/desktop/ms724435(v=vs.85).aspx
 */
#define NameSamCompatible 2

const char *get_user_name (void)
{
  typedef BOOL (WINAPI *func_GetUserNameEx) (int format, char *user, ULONG *user_len);
  func_GetUserNameEx  p_GetUserNameEx;
  static char         user[100];
  ULONG               ulen;
  HMODULE             secur32 = LoadLibrary ("secur32.dll");

  strcpy (user, "?");

  /* This function isn't guaranteed to be available (and it can't hurt
   * to leave the library loaded)
   */
  if (!secur32 || secur32 == INVALID_HANDLE_VALUE)
     goto quit;

  ulen = sizeof(user);
  p_GetUserNameEx = (func_GetUserNameEx) GetProcAddress (secur32, "GetUserNameExA");
  if (p_GetUserNameEx)
       (*p_GetUserNameEx) (NameSamCompatible, user, &ulen);
  else GetUserName (user, &ulen);

quit:
  if (secur32 && secur32 != INVALID_HANDLE_VALUE)
     FreeLibrary (secur32);
  return (user);
}

/*
 * Similar to strncpy(), but always returns 'dst' with 0-termination.
 */
char *_strlcpy (char *dst, const char *src, size_t len)
{
  size_t slen;

  assert (src != NULL);
  assert (dst != NULL);
  assert (len > 0);

  slen = strlen (src);
  if (slen < len)
     return strcpy (dst, src);

  memcpy (dst, src, len-1);
  dst [len-1] = '\0';
  return (dst);
}

/*
 * Get next token from string *stringp, where tokens are possibly-empty
 * strings separated by characters from delim.
 *
 * Writes NULs into the string at *stringp to end tokens.
 * delim need not remain constant from call to call.
 * On return, *stringp points past the last NUL written (if there might
 * be further tokens), or is NULL (if there are definitely no more tokens).
 *
 * If *stringp is NULL, strsep returns NULL.
 */
char *_strsep (char **stringp, const char *delim)
{
  int         c, sc;
  char       *tok, *s = *stringp;
  const char *spanp;

  if (!s)
     return (NULL);

  for (tok = s;;)
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
        else s[-1] = 0;
        *stringp = s;
        return (tok);
      }
    } while (sc != 0);
  }
  return (NULL);
  /* NOTREACHED */
}

/*
 * "string allocate and concatinate".
 * Assumes 's1' is allocated. Thus 'free(s1)' after concat is done.
 */
char *_stracat (char *s1, const char *s2)
{
  size_t sz = strlen(s1) + strlen(s2) + 1;
  char  *s  = MALLOC (sz);
  char *start = s;

  sz = strlen (s1);
  memcpy (s, s1, sz);
  FREE (s1);
  s += sz;
  sz = strlen (s2) + 1;
  memcpy (s, s2, sz);
  return (start);
}

/*
 * For consistency and nice looks, replace (single or multiple) '\\'
 * with single '/' if use == '/'. And vice-versa.
 * All (?) Windows core functions functions should handle
 * '/' just fine.
 */
char *slashify (const char *path, char use)
{
  static char buf [_MAX_PATH];
  char   *s = buf;
  const char *p;
  const char *end = path + strlen(path);

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
    ASSERT (s < buf + sizeof(buf));
  }
  *s = '\0';
  return (buf);
}

/*
 * Heuristic alert!
 *
 * Return 1 if file A is newer than file B.
 * Based on modification times 'mtime_a', 'mtime_b' and file-versions
 * returned from show_version_info().
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

/*
 * Return error-string for 'err' from kernel32.dll.
 */
static BOOL get_error_from_kernel32 (DWORD err, char *buf, DWORD buf_len)
{
  HMODULE mod = GetModuleHandle ("kernel32.dll");
  BOOL    rc = FALSE;

  if (mod && mod != INVALID_HANDLE_VALUE)
  {
    rc = FormatMessageA (FORMAT_MESSAGE_FROM_HMODULE,
                         mod, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                         buf, buf_len, NULL);
  }
  return (rc);
}

/*
 * Return err-number+string for 'err'. Use only with GetLastError().
 * Does not handle libc errno's. Remove trailing [\r\n.]
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

  else if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err,
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           err_buf, sizeof(err_buf)-1, NULL))
  {
    if (!get_error_from_kernel32(err,err_buf, sizeof(err_buf)-1))
       strcpy (err_buf, "Unknown error");
  }

  if (hr)
       snprintf (buf, sizeof(buf), "0x%08lX: %s", hr, err_buf);
  else snprintf (buf, sizeof(buf), "%lu: %s", err, err_buf);
  strip_nl (buf);
  p = strrchr (buf, '.');
  if (p && p[1] == '\0')
     *p = '\0';
  return (buf);
}


#if !defined(_CRTDBG_MAP_ALLOC) && !defined(__CYGWIN__)

/*
 * A strdup() that fails if no memory. It's pretty hopeless to continue
 * this program if strdup() fails.
 */
char *strdup_at (const char *str, const char *file, unsigned line)
{
  struct mem_head *head;
  size_t len = strlen (str) + 1 + sizeof(*head);

  head = malloc (len);

  if (!head)
     FATAL ("strdup() failed at %s, line %u\n", file, line);

  memcpy (head+1, str, len - sizeof(*head));
  head->marker = MEM_MARKER;
  head->size   = len;
  add_to_mem_list (head, file, line);
  mem_max += (DWORD) len;
  mem_allocs++;
  return (char*) (head+1);
}

/*
 * Similar to wcsdup()
 */
wchar_t *wcsdup_at (const wchar_t *str, const char *file, unsigned line)
{
  struct mem_head *head;
  size_t len = sizeof(wchar_t) * (wcslen(str) + 1);

  len += sizeof(*head);
  head = malloc (len);

  if (!head)
     FATAL ("wcsdup() failed at %s, line %u\n", file, line);

  memcpy (head+1, str, len - sizeof(*head));
  head->marker = MEM_MARKER;
  head->size   = len;
  add_to_mem_list (head, file, line);
  mem_max += (DWORD) len;
  mem_allocs++;
  return (wchar_t*) (head+1);
}

/*
 * A malloc() that fails if no memory. It's pretty hopeless to continue
 * this program if strdup() fails.
 */
void *malloc_at (size_t size, const char *file, unsigned line)
{
  struct mem_head *head;

  size += sizeof(*head);

  head = malloc (size);

  if (!head)
     FATAL ("malloc (%u) failed at %s, line %u\n",
            (unsigned)(size-sizeof(*head)), file, line);

  head->marker = MEM_MARKER;
  head->size   = size;
  add_to_mem_list (head, file, line);
  mem_max += (DWORD) size;
  mem_allocs++;
  return (head+1);
}

/*
 * A calloc() that fails if no memory. It's pretty hopeless to continue
 * this program if calloc() fails.
 */
void *calloc_at (size_t num, size_t size, const char *file, unsigned line)
{
  struct mem_head *head;

  size = (size * num) + sizeof(*head);

  head = calloc (1, size);

  if (!head)
     FATAL ("calloc (%u, %u) failed at %s, line %u\n",
            (unsigned)num, (unsigned)(size-sizeof(*head)), file, line);

  head->marker = MEM_MARKER;
  head->size   = size;
  add_to_mem_list (head, file, line);
  mem_max += (DWORD) size;
  mem_allocs++;
  return (head+1);
}

/*
 * A realloc() that fails if no memory. It's pretty hopeless to continue
 * this program if realloc() fails.
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
     FATAL ("realloc() of unknown block at %s, line %u\n", file, line);

  if (p->size - sizeof(*p) < size)
  {
    ptr = malloc_at (size, file, line);
    size = p->size - sizeof(*p);
    memmove (ptr, p+1, size);        /* Since memory could be overlapping */
    del_from_mem_list (p, __LINE__);
    mem_max -= (DWORD) p->size;
    mem_reallocs++;
    free (p);
  }
  return (ptr);
}

/*
 * A free() that checks the 'ptr' and decrements the 'mem_frees' value.
 */
void free_at (void *ptr, const char *file, unsigned line)
{
  struct mem_head *head = (struct mem_head*)ptr;

  head--;
  if (!ptr || head->marker == MEM_FREED)
     FATAL ("double free() of block detected at %s, line %u\n", file, line);

  if (head->marker != MEM_MARKER)
     FATAL ("free() of unknown block at %s, line %u.\n", file, line);

  head->marker = MEM_FREED;
  mem_max -= (DWORD) head->size;
  del_from_mem_list (head, __LINE__);
  mem_frees++;
  free (head);
}
#endif  /* !_CRTDBG_MAP_ALLOC && !__CYGWIN__ */

void mem_report (void)
{
#if !defined(_CRTDBG_MAP_ALLOC) && !defined(__CYGWIN__)
  const struct mem_head *m;
  unsigned     num;

  C_printf ("~0  Max memory at one time: %sytes.\n", str_trim((char*)get_file_size_str(mem_max)));
  C_printf ("  Total # of allocations: %u.\n", (unsigned int)mem_allocs);
  C_printf ("  Total # of realloc():   %u.\n", (unsigned int)mem_reallocs);
  C_printf ("  Total # of frees:       %u.\n", (unsigned int)mem_frees);

  for (m = mem_list, num = 0; m; m = m->next, num++)
  {
    C_printf ("  Un-freed memory 0x%p at %s (%u). %u bytes: \"%s\"\n",
              m+1, m->file, m->line, (unsigned int)m->size, dump10(m+1,m->size));
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

#if defined(__POCC__) && !defined(__MT__) && defined(_M_AMD64)
  /*
   * Some bug (?) causes a reference to this symbol in 'pocc -MD -Tx64-coff'.
   */
  DWORD __ullongfix;
#endif

const char *get_file_size_str (UINT64 size)
{
  static const char *suffixes[] = { "B ", "kB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB" };
  static char buf [10];
  int    i = 0;
  double m;

  while (size >= 1024ULL)
  {
    size /= 1024ULL;
    i++;
  }

  /* Round up
   */
  m = floor ((double)size);
  if (size >= m + 0.5)
     m++;

  snprintf (buf, sizeof(buf), "%4.0f %s", m, suffixes[i]);
  return (buf);
}

/*
 * Return a time-string for 'time_t==0' (non-time).
 */
const char *empty_time (void)
{
  return (opt.decimal_timestamp ? "00000000.000000" : "01 Jan 1970 - 00:00:00");
}

/*
 * strftime() under MSVC sometimes crashes mysteriously. Use this
 * home-grown version. Also tests on 'time_t == 0' which often
 * is returned on a 'stat()' of a protected .sys-file.
 */
#define USE_STRFTIME 0

const char *get_time_str (time_t t)
{
  const struct tm *tm;
  static char      res [50];

  if (t == 0)
     return empty_time();

  tm = localtime (&t);
  if (!tm)
     return empty_time();

#if (USE_STRFTIME)
  if (opt.decimal_timestamp)
       strftime (res, sizeof(res), "%Y%m%d.%H%M%S", tm);
  else strftime (res, sizeof(res), "%d %b %Y - %H:%M:%S", tm);
#else
  {
    const  char *_month;
    static const char *months [12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
                                     };
    if (opt.decimal_timestamp)
       snprintf (res, sizeof(res), "%04d%02d%02d.%02d%02d%02d",
                 1900+tm->tm_year, 1+tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    else
    {
      if (tm->tm_mon >= 0 && tm->tm_mon < DIM(months))
           _month = months [tm->tm_mon];
      else _month = "???";
      snprintf (res, sizeof(res), "%02d %s %04d - %02d:%02d:%02d",
                tm->tm_mday, _month, 1900+tm->tm_year, tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
  }
#endif
  return (res);
}

/*
 * Function that prints the line argument while limiting it
 * to at most 'MAX_CHARS_PER_LINE'. An appropriate number
 * of spaces are added on subsequent lines.
 *
 * Stolen from Wget (main.c) and simplified.
 */
#define MAX_CHARS_PER_LINE 80

void format_and_print_line (const char *line, int indent)
{
  char *token, *line_dup = STRDUP (line);
  int   remaining_chars = 0;

  /* We break on spaces.
   */
  token = strtok (line_dup, " ");
  while (token)
  {
    /* If a token is much larger than the maximum
     * line length, we print the token on the next line.
     */
    if (remaining_chars <= (int)strlen(token))
    {
      C_printf ("\n%*c", indent, ' ');
      remaining_chars = MAX_CHARS_PER_LINE - indent;
    }
    C_printf ("%s ", token);
    remaining_chars -= strlen (token) + 1;  /* account for " " */
    token = strtok (NULL, " ");
  }
  C_putc ('\n');
  FREE (line_dup);
}

/*
 * As above, but without a STRDUP().
 */
void print_long_line (const char *line, size_t indent)
{
  size_t      room, left = MAX_CHARS_PER_LINE - indent;
  const char *c = line;

  while (*c)
  {
    /* Break a long line only at 'break char'.
     * Check if room for a flag-component ("foo|") before we must break the line.
     */
    if (*c == ' ')
    {
      room = (size_t) (line - strchr(c+1,' '));
      if (c[1] && room < left)
      {
        C_printf ("%c\n%*c", *c++, (int)indent, ' ');
        left = MAX_CHARS_PER_LINE - indent;
        line = c;
        continue;
      }
    }
    C_putc (*c++);
    left--;
  }
  C_putc ('\n');
}


/*
 * Search 'list' for 'value' and return it's name.
 */
const char *list_lookup_name (unsigned value, const struct search_list *list, int num)
{
  static char buf[10];

  while (num > 0 && list->name)
  {
    if (list->value == value)
       return (list->name);
    num--;
    list++;
  }
  return _itoa (value,buf,10);
}

/*
 * Search 'list' for 'name' and return it's 'value'.
 */
unsigned list_lookup_value (const char *name, const struct search_list *list, int num)
{
  while (num > 0 && list->name)
  {
    if (!stricmp(name,list->name))
       return (list->value);
    num--;
    list++;
  }
  return (UINT_MAX);
}

const char *flags_decode (DWORD flags, const struct search_list *list, int num)
{
  static char buf[300];
  char  *ret  = buf;
  char  *end  = buf + sizeof(buf) - 1;
  size_t left = end - ret;
  int    i;

  *ret = '\0';
  for (i = 0; i < num; i++, list++)
      if (flags & list->value)
      {
        ret += snprintf (ret, left, "%s+", list->name);
        left = end - ret;
        flags &= ~list->value;
      }
  if (flags)           /* print unknown flag-bits */
     ret += snprintf (ret, left, "0x%08lX+", (u_long)flags);
  if (ret > buf)
     *(--ret) = '\0';   /* remove '+' */
  return (buf);
}

/*
 * Not used.
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

int debug_printf (const char *format, ...)
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

/*
 * A wrapper for popen().
 *  'cmd':      the program + args to run.
 *  'callback': function to call for each line from popen().
 *              This function should return number of matches.
 *              The callback is allowed to modify the given 'buf'.
 *
 * Returns total number of matches from 'callback'.
 */
int popen_run (popen_callback callback, const char *cmd)
{
  char   buf[1000];
  int    i = 0;
  int    j = 0;
  size_t len;
  FILE  *f;
  char  *env = getenv ("COMSPEC");
  char  *cmd2;
  const char *comspec = "";
  const char *setdos  = "";

  /* OpenWatcom's popen() always uses cmd.exe regardless of %COMSPEC.
   * If we're using 4NT/TCC shell, set all variable expansion to off
   * by prepending "setdos /x-3" to 'cmd' buffer.
   */
  if (env)
  {
    DEBUGF (3, "%%COMSPEC: %s.\n", env);
#if !defined(__WATCOMC__)
    env = strlwr (basename(env));
    if (!strcmp(env,"4nt.exe") || !strcmp(env,"tcc.exe"))
       setdos = "setdos /x-3 &";
#endif
  }
  else
    comspec = "set COMSPEC=cmd.exe &";

  len = strlen(setdos) + strlen(comspec) + strlen(cmd) + 1;

  /*
   * Use MALLOC() here because of gcc warning on 'alloca()' used previously:
   *  'warning: stack protector not protecting local variables: variable length buffer [-Wstack-protector]'
   */
  cmd2 = MALLOC (len);

#ifdef __CYGWIN__
  strcpy (cmd2, slashify(cmd, '/'));
  if (!system(NULL))
  {
    WARN ("/bin/sh not found.\n");
    goto quit;
  }
#else
  strcpy (cmd2, setdos);
  strcat (cmd2, comspec);
  strcat (cmd2, cmd);
#endif

  DEBUGF (3, "Trying to run '%s'\n", cmd2);

  f = _popen (cmd2, "r");
  if (!f)
  {
    DEBUGF (1, "failed to call _popen(); errno=%d.\n", errno);
    goto quit;
  }

  while (fgets(buf,sizeof(buf)-1,f))
  {
    int rc;

    strip_nl (buf);
    DEBUGF (3, " _popen() buf: '%s'\n", buf);
    if (!buf[0] || !callback)
       continue;
    rc = (*callback) (buf, i++);
    if (rc < 0)
       break;
    j += rc;
  }
  _pclose (f);

quit:
  FREE (cmd2);
  return (j);
}

/*
 * A var-arg version of 'popen_run()'.
 */
int popen_runf (popen_callback callback, const char *fmt, ...)
{
  char cmd [5000];
  va_list args;

  va_start (args, fmt);
  vsnprintf (cmd, sizeof(cmd), fmt, args);
  va_end (args);
  return popen_run (callback, cmd);
}

/*
 * Returns the expanded version of an environment variable.
 * Stolen from curl. But I wrote the Win32 part of it...
 *
 * E.g. If "INCLUDE=c:\VC\include;%C_INCLUDE_PATH%" and
 * "C_INCLUDE_PATH=c:\MinGW\include", the expansion returns
 * "c:\VC\include;c:\MinGW\include".
 *
 * Note: Windows (cmd only?) requires a trailing '%' in
 *       "%C_INCLUDE_PATH".
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
  if (strchr(variable,'%'))
  {
    /* buf2 == variable if not expanded.
     */
    ret = ExpandEnvironmentStrings (variable, buf2, sizeof(buf2));
    if (ret > 0 && ret < sizeof(buf2) &&
        !strchr(buf2,'%'))    /* no variables still un-expanded */
      env = buf2;
  }

  rc = (env && env[0]) ? STRDUP(env) : NULL;
  DEBUGF (3, "env: '%s', expanded: '%s'\n", orig_var, rc);
  return (rc);
}

/*
 * Translate a shell-pattern to a regular expression.
 * From:
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

       case '?':
            *out++ = '.';
            break;

      default:
            *out++ = c;
            break;
    }
  }

  if (i == i_max)
     WARN ("'pattern' in translate_shell_pattern() is too large (%u bytes).\n",
           (unsigned)len);
  *out = '\0';
  return (res);
}

void hex_dump (const void *data_p, size_t datalen)
{
  const BYTE *data = (const BYTE*) data_p;
  UINT  ofs;

  for (ofs = 0; ofs < datalen; ofs += 16)
  {
    UINT j;

    if (ofs == 0)
         printf ("%u:%s%04X: ", (unsigned int)datalen,
                 datalen > 9999 ? " "    :
                 datalen > 999  ? "  "   :
                 datalen > 99   ? "   "  :
                 datalen > 9    ? "    " :
                                  "     ",
                 ofs);
    else printf ("       %04X: ", ofs);

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

const char *dump10 (const void *data, unsigned size)
{
  static char ret [15];
  unsigned  ofs;
  int       ch;

  for (ofs = 0; ofs < sizeof(ret)-4 && ofs < size; ofs++)
  {
    ch = ((const BYTE*)data) [ofs];
    if (ch < ' ')            /* non-printable */
         ret [ofs] = '.';
    else ret [ofs] = ch;
    ret [ofs+1] = '\0';
  }
  if (ofs < (int)size)
     strcat (ret, "...");
  return (ret);
}

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
    else ret [ofs] = ch;
    ret [ofs+1] = '\0';
  }
  if (ofs < (int)size)
     strcat (ret, "...");
  return (ret);
}

/*
 * Reverse string 'str' in place.
 */
char *strreverse (char *str)
{
  int i, j;

  for (i = 0, j = strlen(str)-1; i < j; i++, j--)
  {
    char c = str[i];
    str[i] = str[j];
    str[j] = c;
  }
  return (str);
}

#if defined(__CYGWIN__)
/*
 * Taken from:
 *   http://stackoverflow.com/questions/190229/where-is-the-itoa-function-in-linux
 */
char *_itoa (int value, char *buf, int radix)
{
  int sign, i = 0;

  ASSERT (radix == 10);
  sign = value;

  if (sign < 0)      /* record sign */
     value = -value; /* make 'value' positive */

  do                 /* generate digits in reverse order */
  {
    buf[i++] = (value % 10) + '0';
  }
  while ((value /= 10) > 0);

  if (sign < 0)
     buf [i++] = '-';
  buf [i] = '\0';
  return strreverse (buf);
}
#endif

/*
 * Functions for getting at Reparse Points (Junctions and Symlinks).
 * Code from:
 *  http://blog.kalmbach-software.de/2008/02/
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

const char *last_reparse_err;

static BOOL reparse_err (int dbg_level, const char *fmt, ...)
{
  static char err_buf [1000];
  va_list args;

  if (!fmt)
  {
    err_buf[0] = '\0';
    return (TRUE);
  }

  va_start (args, fmt);
  vsnprintf (err_buf, sizeof(err_buf), fmt, args);
  va_end (args);
  last_reparse_err = err_buf;
  DEBUGF (dbg_level, last_reparse_err);
  return (FALSE);
}

#define REPARSE_DATA_BUFFER_HEADER_SIZE  FIELD_OFFSET (struct REPARSE_DATA_BUFFER, GenericReparseBuffer)

#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE  (16*1024)
#endif

/* Stuff missing in OpenWatcom 2.0
 */
#ifndef IsReparseTagMicrosoft
#define IsReparseTagMicrosoft(_tag) (_tag & 0x80000000)
#endif

#ifndef FSCTL_GET_REPARSE_POINT
  #define CTL_CODE(DeviceType,Function,Method,Access) \
                   (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))

  #define FSCTL_GET_REPARSE_POINT   CTL_CODE(FILE_DEVICE_FILE_SYSTEM,42,METHOD_BUFFERED,FILE_ANY_ACCESS)
#endif

BOOL wchar_to_mbchar (size_t len, const wchar_t *buf, char *result)
{
  int   num;
  DWORD cp;
  const char *def_char;

  if (len >= _MAX_PATH)
     return reparse_err (1, "len: %u too large.", len);

#if 1
  cp = CP_ACP;
  def_char = "?";
#else
  cp = CP_UTF8;
  def_char = NULL;
#endif

  num = WideCharToMultiByte (cp, 0, buf, len, result, _MAX_PATH, def_char, NULL);
  if (num == 0)
     return reparse_err (1, "WideCharToMultiByte(): %s\n",
                         win_strerror(GetLastError()));

  DEBUGF (2, "len: %u, num: %d, result: '%s'\n", (unsigned)len, num, result);
  return (TRUE);
}

BOOL get_reparse_point (const char *dir, char *result, BOOL return_print_name)
{
  struct REPARSE_DATA_BUFFER *rdata;
  HANDLE   hnd;
  size_t   ofs, plen, slen;
  wchar_t *print_name, *sub_name;
  DWORD    ret_len, share_mode, flags;
  BOOL     rc;

  last_reparse_err = NULL;
  *result = '\0';
  reparse_err (0, NULL);

  DEBUGF (2, "Finding target of dir: '%s'.\n", dir);

  share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
  hnd = CreateFile (dir, FILE_READ_EA, share_mode,
                    NULL, OPEN_EXISTING, flags, NULL);

  if (hnd == INVALID_HANDLE_VALUE)
     return reparse_err (1, "Could not open dir '%s'; %s",
                         dir, win_strerror(GetLastError()));

  rdata = alloca (MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
  rc = DeviceIoControl (hnd, FSCTL_GET_REPARSE_POINT, NULL, 0,
                        rdata, MAXIMUM_REPARSE_DATA_BUFFER_SIZE,
                        &ret_len, NULL);

  CloseHandle (hnd);

  if (!rc)
     return reparse_err (1, "DeviceIoControl(): %s",
                         win_strerror(GetLastError()));

  if (!IsReparseTagMicrosoft(rdata->ReparseTag))
     return reparse_err (1, "Not a Microsoft-reparse point - could not query data!");

  if (rdata->ReparseTag == IO_REPARSE_TAG_SYMLINK)
  {
    DEBUGF (2, "Symbolic-Link\n");

    slen     = rdata->SymbolicLinkReparseBuffer.SubstituteNameLength;
    ofs      = rdata->SymbolicLinkReparseBuffer.SubstituteNameOffset / sizeof(wchar_t);
    sub_name = rdata->SymbolicLinkReparseBuffer.PathBuffer + ofs;

    plen       = rdata->SymbolicLinkReparseBuffer.PrintNameLength;
    ofs        = rdata->SymbolicLinkReparseBuffer.PrintNameOffset / sizeof(wchar_t);
    print_name = rdata->SymbolicLinkReparseBuffer.PathBuffer + ofs;
  }
  else if (rdata->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
  {
    DEBUGF (2, "Mount-Point\n");

    slen     = rdata->MountPointReparseBuffer.SubstituteNameLength;
    ofs      = rdata->MountPointReparseBuffer.SubstituteNameOffset / sizeof(wchar_t);
    sub_name = rdata->MountPointReparseBuffer.PathBuffer + ofs;

    plen       = rdata->MountPointReparseBuffer.PrintNameLength;
    ofs        = rdata->MountPointReparseBuffer.PrintNameOffset / sizeof(wchar_t);
    print_name = rdata->MountPointReparseBuffer.PathBuffer + ofs;
  }
  else
    return reparse_err (1, "Not a Mount-Point nor a Symbolic-Link.");

  DEBUGF (2, "SubstitutionName: '%.*S'\n", (int)(slen/2), sub_name);
  DEBUGF (2, "PrintName:        '%.*S'\n", (int)(plen/2), print_name);

  /* Account for 0-termination
   */
  slen++;
  plen++;

  if (opt.debug >= 3)
  {
    DEBUGF (3, "hex-dump sub_name:\n");
    hex_dump (sub_name, slen);

    DEBUGF (3, "hex-dump print_name:\n");
    hex_dump (print_name, plen);
  }

  if (return_print_name)
     return wchar_to_mbchar (plen, print_name, result);
  return wchar_to_mbchar (slen, sub_name, result);
}

#if defined(__POCC__)
  const char *compiler_version (void)
  {
    static char buf[40];
  #ifdef _DEBUG
    #define DBG_REL "debug"
  #else
    #define DBG_REL "release"
  #endif

    snprintf (buf, sizeof(buf), "PellesC ver %d.%d, %s",
              __POCC__ / 100, __POCC__ % 100, DBG_REL);
    return (buf);
  }

#elif defined(__clang__)
  const char *compiler_version (void)
  {
    static char buf[40];
  #ifdef _DEBUG
    #define DBG_REL "debug"
  #else
    #define DBG_REL "release"
  #endif

    snprintf (buf, sizeof(buf), "clang-cl %d.%d.%d, %s",
              __clang_major__, __clang_minor__, __clang_patchlevel__, DBG_REL);
    return (buf);
  }

#elif defined(_MSC_VER)
  /*
   * Ref. http://msdn.microsoft.com/en-us/library/b0084kay(v=vs.120).aspx
   *
   * E.g. "cl /?" prints:
   *    Microsoft (R) C/C++ Optimizing Compiler Version 18.00.31101.x for x86
   *                       = _MSC_FULL_VER - 180000000  ^----       ^_MSC_BUILD
   */
  static const char *msvc_get_micro_ver (void)
  {
    static char buf[20];
    char  *end;

    buf[0] = '\0';

  #if defined(_MSC_FULL_VER)
    #if (_MSC_FULL_VER > 190000000)
      buf[0] = '.';
      _ultoa (_MSC_FULL_VER-190000000, buf+1, 10);
    #elif (_MSC_FULL_VER > 180000000)
      buf[0] = '.';
      _ultoa (_MSC_FULL_VER-180000000, buf+1, 10);
    #endif
  #endif

  #if defined(_MSC_BUILD)
    end = strrchr (buf, '\0');
    *end++ = '.';
    _itoa (_MSC_BUILD, end, 10);
  #endif
    return (buf);
  }

  const char *compiler_version (void)
  {
    static char buf[40];
  #ifdef _DEBUG
    #define DBG_REL "debug"
  #else
    #define DBG_REL "release"
  #endif

    snprintf (buf, sizeof(buf), "Visual-C %d.%02d%s, %s",
              _MSC_VER / 100, _MSC_VER % 100, msvc_get_micro_ver(), DBG_REL);
    return (buf);
  }

#elif defined(__WATCOMC__)
  const char *compiler_version (void)
  {
    static char buf[40];
  #if (__WATCOMC__ >= 1200)
    snprintf (buf, sizeof(buf), "OpenWatcom %d.%d", (__WATCOMC__/100) - 11, (__WATCOMC__ % 100) / 10);
  #else
    snprintf (buf, sizeof(buf), "Watcom C %d.%d", __WATCOMC__/100, __WATCOMC__ % 100);
  #endif
    return (buf);
  }

#elif defined(__MINGW32__)
  /*
   * '__MINGW32__' is defined by BOTH mingw.org and by the MinGW-w64
   * project [1], because both can target Win32. '__MINGW64__' is defined
   * only when targeting Win64 (__x86_64__).
   *
   * [1] http://mingw-w64.sourceforge.net/
   */
  const char *compiler_version (void)
  {
    static char buf[40];
  #if defined(__MINGW64_VERSION_MAJOR)
     snprintf (buf, sizeof(buf), "MinGW-w64 %d.%d (%s), ",
               __MINGW64_VERSION_MAJOR, __MINGW64_VERSION_MINOR, __MINGW64_VERSION_STATE);

  /* mingw.org MinGW. MingW-RT-4+ defines '__MINGW_MAJOR_VERSION'
   */
  #elif defined(__MINGW_MAJOR_VERSION)
    snprintf (buf, sizeof(buf), "MinGW %d.%d, ", __MINGW_MAJOR_VERSION, __MINGW_MINOR_VERSION);
  #else
    snprintf (buf, sizeof(buf), "MinGW %d.%d, ", __MINGW32_MAJOR_VERSION, __MINGW32_MINOR_VERSION);
  #endif
    return (buf);
  }

#elif defined(__CYGWIN__)
  const char *compiler_version (void)
  {
    static char buf[40];
    snprintf (buf, sizeof(buf), "CygWin %d.%d.%d, ", CYGWIN_VERSION_DLL_MAJOR/1000,
              CYGWIN_VERSION_DLL_MAJOR % 1000, CYGWIN_VERSION_DLL_MINOR);
    return (buf);
  }

#else
  const char *compiler_version (void)
  {
    return (BUILDER);
  }
#endif   /* _MSC_VER */

/*
 * Check if a file-descriptor is coming from CygWin.
 * Applications now could call 'is_cygwin_tty(STDIN_FILENO)' in order to detect
 * whether they are running from Cygwin/MSys terminal.
 *
 * By Mihail Konev <k.mvc@ya.ru> for the MinGW-w64 project.
 */
#undef FILE_EXISTS

#include <io.h>
#include <wchar.h>

#if !defined(__POCC__)
#include <winternl.h>
#endif

#if defined(__WATCOMC__) || defined(__POCC__)
  typedef struct {
          USHORT  Length;
          USHORT  MaximumLength;
          WCHAR  *Buffer;
        } UNICODE_STRING;

  #define NTAPI                     __stdcall
  #define NT_SUCCESS(x)             ((LONG)(x) >= 0)
  #define OBJECT_INFORMATION_CLASS  int
#endif

#if defined(_MSC_VER) || defined(__WATCOMC__)
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
  HANDLE               h_fd;

  /* NtQueryObject needs space for OBJECT_NAME_INFORMATION.Name->Buffer also.
   */
  char                      ntfn_bytes [sizeof(OBJECT_NAME_INFORMATION2) + MAX_PATH * sizeof(WCHAR)];
  OBJECT_NAME_INFORMATION2 *ntfn = (OBJECT_NAME_INFORMATION2*) ntfn_bytes;
  LONG                      status;
  ULONG                     ntfn_size = sizeof(ntfn_bytes);
  wchar_t                   c, *s;
  USHORT                    i;

  h_fd = (HANDLE) _get_osfhandle (fd);
  if (!h_fd || h_fd == INVALID_HANDLE_VALUE)
  {
    DEBUGF (2, "_get_osfhandle (%d) failed\n", fd);
    errno = EBADF;
    return (0);
  }

  p_NtQueryObject = (func_NtQueryObject) GetProcAddress (GetModuleHandle("ntdll.dll"), "NtQueryObject");
  if (!p_NtQueryObject)
  {
    DEBUGF (2, "NtQueryObject() not found in ntdll.dll.\n");
    goto no_tty;
  }

  memset (ntfn, 0, ntfn_size);
  status = (p_NtQueryObject) ((HANDLE)h_fd, ObjectNameInformation, ntfn, ntfn_size, &ntfn_size);

  if (!NT_SUCCESS(status))
  {
    DEBUGF (2, "NtQueryObject() failed.\n");

    /* If it is not NUL (i.e. \Device\Null, which would succeed),
     * then normal isatty() could be consulted.
     * */
    if (_isatty(fd))
       return (1);
    goto no_tty;
  }

  s = ntfn->Name.Buffer;
  s [ntfn->Name.Length/sizeof(WCHAR)] = 0;

  /* Look for \Device\NamedPipe\(cygwin|msys)-[a-fA-F0-9]{16}-pty[0-9]{1,4}-(from-master|to-master|to-master-cyg)
   */
  if (wcsncmp(s, L"\\Device\\NamedPipe\\", 18))
  {
    DEBUGF (2, "Not a Cygwin pipe: '%" WIDESTR_FMT "'.\n", s);
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
    if (!((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || (c >= '0' && c <= '9')))
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
