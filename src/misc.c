
/*
 * Various support functions for EnvTool.
 * fnmatch() and basename() are taken from djgpp and modified.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <io.h>
#include <windows.h>
#include <wincon.h>
#include <imagehlp.h>

#include "envtool.h"

#define DEBUG_STREAM   stdout
#define NORMAL_STREAM  stdout

#define IS_SLASH(c)  ((c) == '\\' || (c) == '/')
#define TOUPPER(c)   toupper ((int)(c))

struct mem_head {
       unsigned long    marker;
       size_t           size;
       char             file [20];  /* allocated at file/line */
       unsigned         line;
       struct mem_head *next;
     };

static struct mem_head *mem_list = NULL;

static size_t mem_max    = 0;  /* Max bytes allocated at one time */
static size_t mem_allocs = 0;  /* # of allocations */
static size_t mem_frees  = 0;  /* # of mem-frees */

static void add_to_mem_list (struct mem_head *m, const char *file, unsigned line)
{
  m->next = mem_list;
  m->line = line;
  _strlcpy (m->file,file, sizeof(m->file));
  mem_list = m;
}

static void del_from_mem_list (const struct mem_head *m)
{
  struct mem_head *m1, *prev;

  for (m1 = prev = mem_list; m1; m1 = m1->next)
  {
    if (m1 != m)
       continue;
    if (m == mem_list)
         mem_list   = m1->next;
    else prev->next = m1->next;
    break;
  }
}


/*
 * Open a fname and check if there's a "PK" signature in header.
 */
int check_if_zip (const char *fname)
{
  static const char header[4] = { 'P', 'K', 3, 4 };
  const char *ext;
  char   buf[4];
  FILE  *f;
  int    rc = 0;

  /* Return 0 if extension is not ".egg" or ".zip"
   */
  ext = get_file_ext (fname);
  if (stricmp(ext,"egg") && strcmp(ext,"zip"))
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
 * Open a fname, read the optional header in PE-header.
 *  - For verify it's signature.
 *  - Showing the version information (if any) in it's resources.
 */
static const IMAGE_DOS_HEADER *dos;
static const IMAGE_NT_HEADERS *nt;
static char  file_buf [sizeof(*dos) + sizeof(*nt)];

int check_if_pe (const char *fname)
{
  BOOL   is_exe, is_pe;
  size_t len = 0;
  FILE  *f = fopen (fname, "rb");

  if (f)
  {
    len = fread (&file_buf, 1, sizeof(file_buf), f);
    fclose (f);
  }
  dos = NULL;
  nt  = NULL;

  if (len < sizeof(file_buf))
     return (FALSE);

  dos = (const IMAGE_DOS_HEADER*) file_buf;
  nt  = (const IMAGE_NT_HEADERS*) ((const BYTE*)file_buf + dos->e_lfanew);

  /* Probably not a PE-file at all.
   */
  if ((char*)nt > file_buf + sizeof(file_buf))
     return (FALSE);

  is_exe = (LOBYTE(dos->e_magic) == 'M' && HIBYTE(dos->e_magic) == 'Z');
  is_pe  = (nt->Signature == IMAGE_NT_SIGNATURE);   /* 'PE\0\0 ' */

  return (is_exe && is_pe);
}

/*
 * Verify the checksum of last opened file above.
 */
int verify_pe_checksum (const char *fname)
{
  DWORD file_sum, header_sum, calc_chk_sum, rc;

  assert (nt);
  file_sum = nt->OptionalHeader.CheckSum;
  DEBUGF (1, "Opt magic: 0x%04X, file_sum: 0x%08lX\n", nt->OptionalHeader.Magic, file_sum);

  rc = MapFileAndCheckSum ((PTSTR)fname, &header_sum, &calc_chk_sum);
  DEBUGF (1, "rc: %lu, 0x%08lX, 0x%08lX\n", rc, header_sum, calc_chk_sum);
  return (header_sum == calc_chk_sum);
}

/*
 * Check if running under WOW64.
 */
BOOL is_wow64_active (void)
{
  BOOL rc    = FALSE;
  BOOL wow64 = FALSE;

#if (IS_WIN64 == 0)
  typedef BOOL (WINAPI *func_IsWow64Process) (HANDLE proc, BOOL *wow64);
  func_IsWow64Process p_IsWow64Process;

  const char *dll = "kernel32.dll";
  HANDLE hnd = LoadLibrary (dll);

  if (!hnd || hnd == INVALID_HANDLE_VALUE)
  {
    DEBUGF (1, "Failed to load %s; %s\n",
            dll, win_strerror(GetLastError()));
    return (rc);
  }

  p_IsWow64Process = (func_IsWow64Process) GetProcAddress (hnd, "IsWow64Process");
  if (!p_IsWow64Process)
  {
    DEBUGF (1, "Failed to find \"p_IsWow64Process()\" in %s; %s\n",
            dll, win_strerror(GetLastError()));
    FreeLibrary (hnd);
    return (rc);
  }

  if (p_IsWow64Process)
     if ((*p_IsWow64Process) (GetCurrentProcess(), &wow64))
        rc = wow64;
  FreeLibrary (hnd);
#endif  /* IS_WIN64 */

  DEBUGF (2, "IsWow64Process(): rc: %d, wow64: %d.\n", rc, wow64);
  return (rc);
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

/**
 * Trim leading and trailing blanks (space/tab) from a string.
 */
char *str_trim (char *s)
{
  size_t n;

  assert (s != NULL);

  while (s[0] && s[1] && isspace((int)s[0]))
       s++;

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

#ifdef NOT_USED
/*
 * Split a 'path' into a 'dir' and 'name'.
 * If e.g. 'path' = "c:\\Windows\\System32\\", 'file' becomes "".
 */
int split_path (const char *path, char *dir, char *file)
{
  const char *slash = strrchr (path, '/');

  if (dir)
     *dir = '\0';

  if (file)
    *file = '\0';

  if (!slash)
     slash = strrchr (path, '\\');

  if (!slash || *(slash+1) == '\0')
     ;
  return (0);
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
char *_fixpath (const char *path, char *result)
{
  if (!path || !*path)
  {
    DEBUGF (1, "given a bogus 'path'\n");
    errno = EINVAL;
    return (NULL);
  }

  if (!result)
     result = CALLOC (_MAX_PATH, 1);

 /* GetFullPathName() doesn't seems to handle
  * '/' in 'path'. Convert to '\\'.
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
    _strlcpy (result, path, _MAX_PATH);
  }
  return (result);
}

/*
 * Returns ptr to 1st character in file's extension.
 * Returns ptr to '\0' if no extension.
 */
const char *get_file_ext (const char *file)
{
  const char *end, *dot;

#if 0
  assert (file);
  file = basename (file);
#else
  const char *s;

  assert (file);
  while ((s = strpbrk(file, ":/\\")) != NULL)  /* step over drive/path part */
     file = s + 1;
#endif

  end = strrchr (file, '\0');
  dot = strrchr (file, '.');
  return ((dot > file) ? dot+1 : end);
}

/*
 * Create a %TEMP-file and return it's allocated name.
 */
char *create_temp_file (void)
{
  char *tmp = _tempnam (NULL, "envtool-tmp");

  if (tmp)
  {
    char *t = STRDUP (tmp);
    DEBUGF (2, " %s() tmp: '%s'\n", __FUNCTION__, tmp);
    free (tmp);
    return (t);     /* Caller must free() */
  }
  DEBUGF (2, " %s() _tempname() failed: %s\n", __FUNCTION__, strerror(errno));
  return (NULL);
}

/*
 * Similar to strncpy(), but always returns 'dst' with 0-termination.
 */
char *_strlcpy (char *dst, const char *src, size_t len)
{
  assert (src != NULL);
  assert (dst != NULL);
  assert (len > 0);

  if (strlen(src) < len)
     return strcpy (dst, src);

  memcpy (dst, src, len);
  dst [len-1] = '\0';
  return (dst);
}

/*
 * For consistentcy and nice looks, replace (single or multiple) '\\'
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
  }
  *s = '\0';
  return (buf);
}

/*
 * Heristic alert! return 1 if file-A is newer than file-B.
 * Based on modification times 'mtime_a', 'mtime_b' and file-versions
 * returned from show_version_info().
 */
int compare_file_time_ver (time_t mtime_a, time_t mtime_b,
                           struct ver_info ver_a, struct ver_info ver_b)
{
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
   //FreeLibrary (mod);
  }
  return (rc);
}

/*
 * Return err-number+string for 'err'. Use only with GetLastError().
 * Does not handle libc errno's. Remove trailing [\r\n.]
 */
char *win_strerror (unsigned long err)
{
  static char buf[512+20];
  char   err_buf[512], *p;

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

  snprintf (buf, sizeof(buf), "%lu %s", err, err_buf);
  strip_nl (buf);
  p = strrchr (buf, '.');
  if (p && p[1] == '\0')
     *p = '\0';
  return (buf);
}

/*
 * A strdup() that fails if no memory. It's pretty hopeless to continue
 * this program if strdup() fails.
 */
char *strdup_at (const char *str, const char *file, unsigned line)
{
  struct mem_head *head;
  size_t len = strlen (str) + 1 + sizeof(*head);

#if defined(_CRTDBG_MAP_ALLOC)     /* cl -MDd .. */
  head = _malloc_dbg (len, _NORMAL_BLOCK, file, line);
#else
  head = malloc (len);
#endif

  if (!head)
     FATAL ("strdup() failed at %s, line %u\n", file, line);

  memcpy (head+1, str, len - sizeof(*head));
  head->marker = MEM_MARKER;
  head->size   = len;
  add_to_mem_list (head, file, line);
  mem_max += sizeof(*head) + head->size;
  mem_allocs++;
  return (char*) (head+1);
}

/*
 * A calloc() that fails if no memory. It's pretty hopeless to continue
 * this program if calloc() fails.
 */
void *calloc_at (size_t num, size_t size, const char *file, unsigned line)
{
  struct mem_head *head;

  size = (size * num) + sizeof(*head);

#if defined(_CRTDBG_MAP_ALLOC)     /* cl -MDd .. */
  head = _calloc_dbg (1, size, _NORMAL_BLOCK, file, line);
#else
  head = calloc (1, size);
#endif

  if (!head)
     FATAL ("calloc() failed at %s, line %u\n", file, line);

  head->marker = MEM_MARKER;
  head->size   = size;
  add_to_mem_list (head, file, line);
  mem_max += sizeof(*head) + size;
  mem_allocs++;
  return (head+1);
}

/*
 * A realloc() that fails if no memory. It's pretty hopeless to continue
 * this program if realloc() fails.
 */
void *realloc_at (void *ptr, size_t size, const char *file, unsigned line)
{
  struct mem_head *head = (struct mem_head*) ptr;
  void  *p;

  size += sizeof(*head);

#if defined(_CRTDBG_MAP_ALLOC)  /* cl -MDd .. */
  p = _realloc_dbg (head, size, _NORMAL_BLOCK, file, line);
#else
  p = realloc (head, size);
#endif

  if (!p)
     FATAL ("realloc() failed at %s, line %u\n", file, line);

  head->marker = MEM_MARKER;
  head->size   = size;
  if (p != head)
  {
    del_from_mem_list (p);
    add_to_mem_list (head, file, line);
    mem_max += sizeof(*head) + size;
    mem_allocs++;
  }
  return (p);
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
     FATAL ("free() of unknown block at %s, line %u\n", file, line);

  head->marker = MEM_FREED;
  mem_max -= sizeof(*head) + head->size;
  mem_frees++;
  del_from_mem_list (head);
  free (head);
}

void mem_report (void)
{
  const struct mem_head *m;
  unsigned     num;

  Cprintf (COLOUR_REPORT, "  Max memory at one time: %u bytes.\n", (unsigned int)mem_max);
  Cprintf (COLOUR_REPORT, "  Total # of allocations: %u.\n", (unsigned int)mem_allocs);
  Cprintf (COLOUR_REPORT, "  Total # of frees:       %u.\n", (unsigned int)mem_frees);

  if (!mem_list)
     Cprintf (COLOUR_REPORT, "  No un-freed memory.\n");

  for (m = mem_list, num = 0; m; m = m->next, num++)
  {
    Cprintf (COLOUR_REPORT, "  Un-freed memory 0x%p at %s (%u)\n", m+1, m->file, m->line);
    if (num > 20)
    {
      Cprintf (COLOUR_REPORT, "  ..and more.\n");
      break;
    }
  }
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
                j == 7 ? '-' : ' ');  /* no beeps */

    for ( ; j < 16; j++)       /* pad line to 16 positions */
        fputs ("   ", stdout);

    for (j = 0; j < 16 && j+ofs < datalen; j++)
    {
      int ch = data[j+ofs];

      if (ch < ' ')            /* non-printable */
           putchar ('.');
      else putchar (ch);
    }
    putchar ('\n');
  }
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
     ret += snprintf (ret, left, "0x%08lX+", flags);
  if (ret > buf)
     *(--ret) = '\0';   /* remove '+' */
  return (buf);
}

/*
 * Print to WinConsole using colours.
 */
static HANDLE stdout_hnd = INVALID_HANDLE_VALUE;
static CONSOLE_SCREEN_BUFFER_INFO csbi;

static void __cdecl exit_console (void)
{
  if (stdout_hnd != INVALID_HANDLE_VALUE)
  {
    SetConsoleTextAttribute (stdout_hnd, csbi.wAttributes);
    CloseHandle (stdout_hnd);
  }
  stdout_hnd = INVALID_HANDLE_VALUE;
}

static void init_console (void)
{
  SECURITY_ATTRIBUTES sa;
  BOOL rc;

  stdout_hnd = INVALID_HANDLE_VALUE;

  /* Don't use WinCon if stdout is redirected.
   */
  if (!isatty(STDOUT_FILENO))
  {
    color = FALSE;
    return;
  }

  memset (&sa, 0, sizeof(sa));
  sa.nLength              = sizeof (sa);
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle       = TRUE;

  /* Using CreateFile() we get the true console handle, avoiding
   * any redirection.
   */
  stdout_hnd = CreateFile ("CONOUT$",
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, 0, NULL);
  rc = GetConsoleScreenBufferInfo (stdout_hnd, &csbi);
  DEBUGF (1, "GetConsoleScreenBufferInfo(): rc %d\n", rc);
}

int Cputs (int attr, const char *buf)
{
  static BOOL init = FALSE;
  int         rc;

  if (color && !init)
  {
    init_console();
    atexit (exit_console);
  }
  init = TRUE;

  if (stdout_hnd == INVALID_HANDLE_VALUE)
     rc = fputs (buf, NORMAL_STREAM);
  else
  {
    DWORD written = 0;

    if (attr == 0)
         attr = csbi.wAttributes;
    else attr |= csbi.wAttributes & 0xF0;

    SetConsoleTextAttribute (stdout_hnd, attr);
    WriteConsole (stdout_hnd, buf, (DWORD)strlen(buf), &written, NULL);
    SetConsoleTextAttribute (stdout_hnd, csbi.wAttributes);
    rc = written;
  }
  return (rc);
}

int Cprintf (int attr, const char *format, ...)
{
  int     len;
  char    buf[3000];
  va_list args;

  va_start (args, format);
  vsnprintf (buf, sizeof(buf), format, args);
  len = Cputs (attr, buf);
  va_end (args);
  return (len);
}

int Cvprintf (int attr, const char *format, va_list args)
{
  int  len;
  char buf[3000];

  vsnprintf (buf, sizeof(buf), format, args);
  len = Cputs (attr, buf);
  return (len);
}

int debug_printf (const char *format, ...)
{
  int      rc;
  va_list args;

  va_start (args, format);
  rc = vfprintf (DEBUG_STREAM, format, args);
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
int popen_run (const char *cmd, popen_callback callback)
{
  char   buf[1000];
  int    i = 0;
  int    j = 0;
  FILE  *f;
  char  *env = getenv ("COMSPEC");
  char  *cmd2;
  const char *comspec = "";
  const char *setdos = "";

  /*
   * OpenWatcom's popen() always uses cmd.exe regardles of %COMSPEC.
   * If we're using 4NT shell, set all variable expansion to off
   * by prepending "setdos /x-3" to 'cmd' buffer.
   */
  if (env)
  {
    DEBUGF (1, "%%COMSPEC: %s.\n", env);
    strupr (env);
#if !defined(__WATCOMC__)
    if (strstr(env,"4NT.EXE"))
       setdos = "setdos /x-3 &";
#endif
  }
  else
    comspec = "set COMSPEC=cmd.exe &";

  cmd2 = alloca (strlen(setdos) + strlen(comspec) + strlen(cmd) + 1);
  strcpy (cmd2, setdos);
  strcat (cmd2, comspec);
  strcat (cmd2, cmd);

  DEBUGF (1, "Trying to run '%s'\n", cmd2);

#ifdef __CYGWIN__
  if (!system(NULL))
  {
    WARN ("/bin/sh not found.\n");
    return (0);
  }
#endif

  f = _popen (cmd2, "r");
  if (!f)
  {
    DEBUGF (1, "failed to call _popen(); errno=%d.\n", errno);
    return (0);
  }

  while (fgets(buf,sizeof(buf)-1,f))
  {
    int rc;

    strip_nl (buf);
    DEBUGF (2, " _popen() buf: '%s'\n", buf);
    if (!buf[0])
       continue;
    rc = (*callback) (buf, i++);
    if (rc < 0)
       break;
    j += rc;
  }
  _pclose (f);
  return (j);
}

/*
 * Translate a shell PATTERN to a regular expression.
 * From:
 *   https://mail.python.org/pipermail/python-list/2003-August/244415.html
 */
char *translate_shell_pattern (const char *pattern)
{
  static char res [_MAX_PATH];
  char       *out = res;
  size_t      i, len = strlen (pattern);

  *out++ = '^';

  for (i = 0; i < len; i++)
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
  *out++ = '$';
  *out++ = '\0';
  return (res);
}
