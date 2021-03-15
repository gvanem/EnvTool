/** \file report.c
 *  \ingroup Misc
 *
 * \brief Print the reported file or directory.
 */

#include "envtool.h"
#include "color.h"
#include "pkg-config.h"
#include "Everything_ETP.h"
#include "ignore.h"
#include "description.h"
#include "report.h"

/**
 * Report time and name of `file`.
 *
 * Also: if the match came from a
 * registry search, report which key had the match.
 */
static int    found_in_hkey_current_user = 0;
static int    found_in_hkey_current_user_env = 0;
static int    found_in_hkey_local_machine = 0;
static int    found_in_hkey_local_machine_sess_man = 0;
static int    found_in_python_egg = 0;
static int    found_in_default_env = 0;
static UINT64 total_size = 0;
static int    longest_file_so_far = 0;
static char   report_header [_MAX_PATH+50];

DWORD num_version_ok = 0;
DWORD num_verified = 0;
DWORD num_evry_dups = 0;
DWORD num_evry_ignored = 0;

static const char *mmap_max;
static BOOL        first_match = FALSE;

/**
 * Print a chunk of a match-line. Replace a `<TAB>` with 2 spaces.
 * Stop printing:
 *  * when `max_len` character printed.
 *  * or when a newline is found.
 *  * or `p` pointer reached beyond `mmap_max`.
 */
static size_t save_chunk (FMT_buf *fmt, const char *str, size_t max_len)
{
  const char *p = str;
  size_t      len = 0;

  for (p = str; *p != '\r' && *p != '\n' && len < max_len && p < mmap_max; len++, p++)
  {
    if (*p == '\t')
         buf_puts (fmt, "  ");
    else if (*p == '~')
         buf_puts (fmt, "~~");
    else buf_putc (fmt, *p);
    if (fmt->buffer_left < 2)
       break;
  }
  return (len);
}

/**
 * Print a match as a "grep --line-number 'content' file" would do:
 *   2: * 'content' rest of line.
 *   ^     ^
 *   |     |__ match in hightlighted colour    (white on red background)
 *   |________ line_num in hightlighted colour (bright green)
 *
 * \todo If these are > 1 matches on the same line, remember the previous
 *       output (using a `FMT_BUF`), and merge current result with the previous.
 *
 * \todo Add a configurable `before-context` and `after-context`. Similar to grep.
 */
static void save_match (FMT_buf *fmt, DWORD line_num, const char *line, const char *match, size_t match_len, size_t line_max)
{
  const char *rest, *indent = "        ";
  size_t      len, rest_max;

  if (first_match)
     buf_putc (fmt, '\n');
  first_match = FALSE;

  len = buf_printf (fmt, "%s~2%lu:~0 ", indent, line_num);
  len += save_chunk (fmt, line, match - line);

  buf_puts (fmt, "~8");  /* bright white on red background */
  len += save_chunk (fmt, match, match_len);
  buf_puts (fmt, "~0");

  rest = match + match_len;
  rest_max = C_screen_width() - 1 - len;
  rest_max = min (line_max, rest_max);
  save_chunk (fmt, rest, rest_max);
  buf_putc (fmt, '\n');
}

/**
 * Open a `file` in memory-mapped mode and search for `content`.
 * The search is case-sensitive if `opt.case_sensitive == TRUE`.
 *
 * \param[in] file     the file to search.
 * \param[in] content  the content in `file` to search for.
 * \retval The number of matches found in `file`.
 */
static int report_grep_file (FMT_buf *fmt, const char *file, const char *content)
{
  HANDLE        hnd_file, mmap_file;
  LARGE_INTEGER fsize;
  const char   *mmap_buf, *p;
  const char   *line_start, *line_end, *match;
  DWORD         line_num = 1, err = 0;
  size_t        match_len = strlen (content);
  int           matches = 0;

  if (opt.debug >= 1)
     buf_putc (fmt, '\n');

  TRACE (1, "grepping file '%s' for '%s'.\n", file, content);

  hnd_file = CreateFile (file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hnd_file == INVALID_HANDLE_VALUE)
  {
    err = GetLastError();
    TRACE (1, "Could not open file: %s.\n", win_strerror(err));
    return (-(int)err);
  }

  if (!GetFileSizeEx(hnd_file, &fsize))
  {
    err = GetLastError();
    TRACE (1, "Could not get file-size: %s.\n", win_strerror(err));
    CloseHandle (hnd_file);
    return (-(int)err);
  }

  mmap_file = CreateFileMapping (hnd_file, NULL, PAGE_READONLY, 0, 0, NULL);
  if (!mmap_file)
  {
    err = GetLastError();
    TRACE (1, "CreateFileMapping() failed: %s.\n", win_strerror(err));
    CloseHandle (hnd_file);
    return (-(int)err);
  }

  mmap_buf = MapViewOfFile (mmap_file, FILE_MAP_READ, 0, 0, 0);
  if (!mmap_buf)
  {
    err = GetLastError();
    TRACE (1, "MapViewOfFile() failed: %s.\n", win_strerror(err));
    CloseHandle (hnd_file);
    CloseHandle (mmap_file);
    return (-(int)err);
  }

  mmap_max = mmap_buf + ((UINT64)fsize.HighPart << 32) + fsize.LowPart;
  TRACE (1, "view range: 0x%p - 0x%p. fsize: %" U64_FMT " bytes.\n",
         mmap_buf, mmap_max-1, ((UINT64)fsize.HighPart << 32) + fsize.LowPart);

  /* Detect and ignore files with binary content
   */
  p = mmap_max;
  if (p > mmap_buf + 100)
     p = mmap_buf + 100;

  if (memchr(mmap_buf, '\0', p - mmap_buf))
  {
    TRACE (1, "Ignoring binary file %s.\n", file);
    opt.grep.binary_files++;
    goto quit;
  }

  match = line_end = NULL;
  first_match = TRUE;

  for (p = line_start = mmap_buf; p < mmap_max; p++)
  {
    if (p[0] == '\r' && p[1] == '\n')
    {
      line_start = ++p + 1;  /* MSDOS terminated file */
      line_num++;
    }
    else if (p[0] == '\n')
    {
      line_start = p + 1;    /* Unix terminated file */
      line_num++;
    }
    else if (str_equal_n(p, content, match_len))
    {
      TRACE (1, "Found at line: %lu, ofs: %zu -> '%.*s'\n",
             line_num, (size_t)(p - mmap_buf), (int)match_len, p);
      match = p;
      p += match_len;
      line_end = memchr (p, '\r', mmap_max - p);
      if (!line_end)
         line_end = memchr (p, '\n', mmap_max - p);
      if (!line_end)
         line_end = mmap_max;
    }
    if (match)
    {
      save_match (fmt, line_num, line_start, match, match_len, line_end - line_start);
      match = NULL;
      opt.grep.num_matches++;
      if (++matches >= (int)opt.grep.max_matches && opt.grep.max_matches)
      {
        buf_puts (fmt, "        ...\n");
        break;
      }
    }
  }

quit:
  UnmapViewOfFile (mmap_buf);
  CloseHandle (hnd_file);
  CloseHandle (mmap_file);
  return (matches);
}

/**
  Use this as an indication that the EveryThing database is not up-to-date with
 * the reality; files have been deleted after the database was last updated.
 * Unless we're running a 64-bit version of envtool and a file was found in
 * the `sys_native_dir[]` and we `have_sys_native_dir == 0`.
 */
static int found_everything_db_dirty = 0;

/**
 * Local functions.
 */
static BOOL get_wintrust_info (const char *file, char *dest, size_t dest_size);
static int  get_trailing_indent (const char *file);
static BOOL get_PE_file_brief (const struct report *r, char *dest, size_t dest_size);
static void print_PE_file_details (const char *filler);

/**
 * Increment total size for found files.
 */
void incr_total_size (UINT64 size)
{
  total_size += size;
}

/**
 * This is the main printer for a file/dir.
 * Prints any notes, time-stamp, size, file/dir name.
 * Also any she-bang statements, links for a gzipped man-page,
 * PE-information like resource version or trust information
 * and file-owner.
 *
 * \param[in] r  The `struct report *` of the file or directory to report.
 *
 * \note The `r` structure can be modified by a `r->pre_action` or `r->post_action` function.
 */
int report_file (struct report *r)
{
  const char *note = NULL;
  const char *link = NULL;
  const char *ext  = NULL;
  const char *description;
  char        size [40] = "?";
  int         len;
  UINT64      fsize;
  BOOL        have_it = TRUE;
  BOOL        show_dir_size = TRUE;
  BOOL        show_pc_files_only = FALSE;
  BOOL        show_this_file = TRUE;
  BOOL        possible_PE_file = TRUE;

  FMT_buf     fmt_buf_time_size;
  FMT_buf     fmt_buf_file_info;
  FMT_buf     fmt_buf_owner_info;
  FMT_buf     fmt_buf_ver_info;
  FMT_buf     fmt_buf_trust_info;
  FMT_buf     fmt_buf_grep_info;

  BUF_INIT (&fmt_buf_time_size, 100, 0);
  BUF_INIT (&fmt_buf_file_info, 100 + _MAX_PATH, 0);
  BUF_INIT (&fmt_buf_owner_info, 100, 0);
  BUF_INIT (&fmt_buf_ver_info, 100, 0);
  BUF_INIT (&fmt_buf_trust_info, 100, 0);
  BUF_INIT (&fmt_buf_grep_info, 10000, 0);

  r->filler = "      ";

#if defined(__clang__) || defined(__GNUC__) || (defined(_MSC_VER) && _MSC_VER >= 1900)
  if (r->key == HKEY_PKG_CONFIG_FILE && 0)
  {
    r->pre_action = r->post_action = NULL;
    if (opt.verbose >= 1)
       r->post_action = pkg_config_get_details2;
    return report_file2 (r);
  }
#endif

  if (r->key == HKEY_CURRENT_USER)
  {
    found_in_hkey_current_user++;
    note = " (1)  ";
  }
  else if (r->key == HKEY_LOCAL_MACHINE)
  {
    found_in_hkey_local_machine++;
    note = " (2)  ";
  }
  else if (r->key == HKEY_CURRENT_USER_ENV && !r->is_cwd)
  {
    found_in_hkey_current_user_env++;
    note = " (3)  ";
  }
  else if (r->key == HKEY_LOCAL_MACHINE_SESSION_MAN && !r->is_cwd)
  {
    found_in_hkey_local_machine_sess_man++;
    note = " (4)  ";
  }
  else if (r->key == HKEY_PYTHON_EGG)
  {
    found_in_python_egg++;
    possible_PE_file = FALSE;
    note = " (5)  ";
  }
  else if (r->key == HKEY_MAN_FILE)
  {
    possible_PE_file = FALSE;
  }
  else if (r->key == HKEY_EVERYTHING)
  {
#if (IS_WIN64)
    /*
     * If e.g. a 32-bit EveryThing program is finding matches in "%WinDir\\System32",
     * don't set `found_everything_db_dirty=1` when we don't `have_sys_native_dir`.
     */
    if (r->mtime == 0 &&
        (!have_sys_native_dir || !strnicmp(r->file,sys_native_dir,strlen(sys_native_dir))))
       have_it = FALSE;
#endif

    if (have_it && r->mtime == 0 && !(r->is_dir ^ opt.dir_mode))
    {
      found_everything_db_dirty = 1;
      note = " (6)  ";
    }
  }
  else if (r->key == HKEY_EVERYTHING_ETP)
  {
    show_dir_size = FALSE;
    possible_PE_file = FALSE;
  }
  else if (r->key == HKEY_PKG_CONFIG_FILE)
  {
    show_pc_files_only = TRUE;
    possible_PE_file = FALSE;
  }
  else
  {
    found_in_default_env++;
  }

  if (r->is_dir)
     note = "<DIR> ";

  if ((!r->is_dir && opt.dir_mode) || !have_it)
  {
    show_this_file = FALSE;
    return (0);
  }

  if (show_pc_files_only)
  {
    ext = get_file_ext (r->file);
    if (stricmp(ext,"pc"))
    {
      show_this_file = FALSE;
      return (0);
    }
  }

 /*
  * Recursively get the size of files under directory matching `file`.
  * For a Python search with 'opt.show_size' (i.e. 'envtool --py -s foo*'),
  * report the size of the branch 'foo*' as 'opt.dir_mode' was specified.
  *
  * The ETP-server (r->key == HKEY_EVERYTHING_ETP) can not reliably report size
  * of directories.
  */
  if (opt.show_size && show_dir_size && (opt.dir_mode || r->key == HKEY_PYTHON_PATH))
  {
    if (r->is_dir)
         fsize = get_directory_size (r->file);
    else fsize = r->fsize;
    snprintf (size, sizeof(size), " - %s", get_file_size_str(fsize));
    incr_total_size (fsize);
  }
  else if (opt.show_size)
  {
    snprintf (size, sizeof(size), " - %s", get_file_size_str(r->fsize));
    if (r->fsize < (__int64)-1)
    {
      if (r->key == HKEY_EVERYTHING_ETP)
           incr_total_size (r->fsize);
      else incr_total_size (get_file_alloc_size (r->file, r->fsize));
    }
  }
  else
    size[0] = '\0';

  report_header_print();

  if (possible_PE_file && opt.PE_check)
  {
    static DWORD num_version_ok_last = 0;

    show_this_file = get_PE_file_brief (r, fmt_buf_ver_info.buffer_start, fmt_buf_ver_info.buffer_size);
    if (show_this_file && opt.signed_status != SIGN_CHECK_NONE)
    {
      show_this_file = get_wintrust_info (r->file, fmt_buf_trust_info.buffer_start, fmt_buf_trust_info.buffer_size);
      if (!show_this_file && num_version_ok_last < num_version_ok)
         num_version_ok--;  /* Fix this counter for the  'report_final()' */
    }
    num_version_ok_last = num_version_ok;
  }

  buf_printf (&fmt_buf_time_size, "~3%s~0%s%s: ",
              note ? note : r->filler, get_time_str(r->mtime), size);

  /* The remote `file` from EveryThing is not something Windows knows
   * about. Hence no point in trying to get the DomainName + AccountName
   * for it.
   */
  if (opt.show_owner && r->key != HKEY_EVERYTHING_ETP)
  {
    char       *account_name = NULL;
    const char *found_owner = NULL;
    BOOL        inverse = FALSE;

    if (get_file_owner(r->file, NULL, &account_name))
    {
      int i, max = smartlist_len (opt.owners);

      /* Show only the file/directory if it matches (or not matches) one of the
       * owners in `opt.owners`.
       * With `opt.owners == "*"`, match all.
       * With `opt.owners == "!*"`, match none.
       *
       * E.g. with:
       *   envtool --man --owner=Admin*  pkcs7*
       *   show only Man-pages matching "pkcs7*" and owners "Admin*":
       *
       *   envtool --man --owner=!Admin* pkcs7*
       *   show only Man-pages matching "pkcs7*" and owners not matching "Admin*":
       */
      if (max > 0)
         show_this_file = FALSE; /* Assume no, if there are >= 1 owner-patterns to check for */

      for (i = 0; i < max; i++)
      {
        const char *owner = smartlist_get (opt.owners, i);

        if (owner[0] == '!' && fnmatch(owner+1, account_name, fnmatch_case(0)) == FNM_NOMATCH)
        {
          inverse = TRUE;
          found_owner = owner + 1;
          show_this_file = TRUE;
          break;
        }
        else if (fnmatch(owner, account_name, fnmatch_case(0)) == FNM_MATCH)
        {
          found_owner = owner;
          show_this_file = TRUE;
          break;
        }
      }
    }

    if (found_owner)
    {
      TRACE (2, "account_name (%s) %smatches owner (%s).\n", account_name, inverse ? "does not " : "", found_owner);
      buf_printf (&fmt_buf_owner_info, "%-18s", str_shorten(account_name,18));
    }
    else
    {
      buf_printf (&fmt_buf_owner_info, "%-18s", account_name ? account_name : "<None>");
      TRACE (2, "account_name (%s) did not match any wanted owner(s) for file '%s'.\n",
             account_name, basename(r->file));
    }
    FREE (account_name);
  }

  /* `slashify2()` will remove excessive `/` or `\\` anywhere in the name.
   * Add a trailing slash to directories.
   */
  buf_printf (&fmt_buf_file_info, "%s%c", r->file, r->is_dir ? DIR_SEP: '\0');
  slashify2 (fmt_buf_file_info.buffer_start, fmt_buf_file_info.buffer_start,
             opt.show_unix_paths ? '/' : '\\');

  if (!r->is_dir && r->key == HKEY_MAN_FILE)
  {
    ext  = get_file_ext (r->file);
    link = get_man_link (r->file);
#if 0
    if (!link && !isdigit((int)*ext))
       link = get_gzip_link (r->file);
#endif

    if (link)
       buf_printf (&fmt_buf_file_info, "%*s(%s)", get_trailing_indent(r->file), " ", link);
  }
  else if (!r->is_dir)
  {
    const char *shebang = check_if_shebang (r->file);

    if (shebang)
       buf_printf (&fmt_buf_file_info, "%*s(%s)", get_trailing_indent(r->file), " ", shebang);
  }

  if (r->content && !r->is_dir)
  {
    int matches = report_grep_file (&fmt_buf_grep_info, link ? link : r->file, r->content);

    if (opt.grep.only && matches == 0)
       show_this_file = FALSE;
  }

  if (!show_this_file)
  {
    get_PE_version_info_free();
    return (0);
  }

  len = C_puts (fmt_buf_time_size.buffer_start);
  C_puts (fmt_buf_owner_info.buffer_start);

  print_raw (fmt_buf_file_info.buffer_start, NULL, NULL);

  if (opt.show_descr && (description = file_descr_get(r->file)) != NULL && *description)
  {
    int raw;

    C_puts ("~6");
    raw = C_setraw (1);

    C_printf ("\n%*s", len-2, "");
    C_puts_long_line (description, len-2);
    C_setraw (raw);
    C_puts ("~0");
  }

  if (r->content)
  {
    C_puts (fmt_buf_grep_info.buffer_start);
  }

  /* All this must be printed on the next line
   */
  if (opt.PE_check && fmt_buf_ver_info.buffer_start[0])
  {
    C_printf ("%-60s", fmt_buf_ver_info.buffer_start);
    C_puts (fmt_buf_trust_info.buffer_start);
    print_PE_file_details (r->filler);
  }

  if (r->key == HKEY_PKG_CONFIG_FILE && opt.verbose > 0)
     pkg_config_get_details (r->file, r->filler);

  C_putc ('\n');
  return (1);
}

int report_file2 (struct report *r)
{
  FMT_buf     fmt_buf_file_info;
  FMT_buf     fmt_buf_time_size;
  const char *note = NULL;

  if (r->pre_action && !(*r->pre_action) (r))
     return (0);

  BUF_INIT (&fmt_buf_file_info, 100 + _MAX_PATH, 0);
  BUF_INIT (&fmt_buf_time_size, 100, 0);

  buf_printf (&fmt_buf_time_size, "~2%s~3%s%s: ",
              note ? note : r->filler, get_time_str(r->mtime), get_file_size_str(r->fsize));

  buf_printf (&fmt_buf_file_info, "~6%s%c~0", r->file, r->is_dir ? DIR_SEP: '\0');

  C_puts (fmt_buf_time_size.buffer_start);
  C_puts (fmt_buf_file_info.buffer_start);

  if (r->post_action)
    (*r->post_action) (r);

  C_putc ('\n');
  return (1);
}

/**
 * Print the `report_header` once once for each `--mode`.
 */
void report_header_print (void)
{
  if (report_header[0])
     C_printf ("~3%s", report_header);
  C_puts ("~0");
  report_header[0] = '\0';
  longest_file_so_far = 0;
}

/**
 * Set the `report_header`.
 */
void report_header_set (const char *fmt, ...)
{
  va_list args;

  va_start (args, fmt);
  if (!fmt || !fmt[0])
       report_header[0] = '\0';
  else vsnprintf (report_header, sizeof(report_header), fmt, args);
  va_end (args);
}

/**
 * Return the indentation needed for the next `she-bang` or `man-file link`
 * to align up more nicely.
 * Not ideal since we don't know the length of all files we need to report.
 */
static int get_trailing_indent (const char *file)
{
  static int indent = 0;
  int    len = (int) strlen (file);

  if (longest_file_so_far == 0 || len > longest_file_so_far)
     longest_file_so_far = len;

  if (len <= longest_file_so_far)
     indent = 1 + longest_file_so_far - len;

  TRACE (2, "longest_file_so_far: %d, len: %d, indent: %d\n",
         longest_file_so_far, len, indent);
  return (indent);
}

/**
 * Print the Resource-version details after any `wintrust_check()` results has
 * been printed. The details come from `get_PE_file_brief()` and `get_PE_version_info()`.
 * Which can be retrieved using `get_PE_version_info_buf()`.
 */
static void print_PE_file_details (const char *filler)
{
  char *line, *ver_trace = get_PE_version_info_buf();
  int   save, i;

  if (!ver_trace)
     return;

  save = C_setraw (1);  /* In case version-info contains a "~" (SFN). */

  for (i = 0, line = strtok(ver_trace,"\n"); line; line = strtok(NULL,"\n"), i++)
  {
    const char *colon  = strchr (line, ':');
    size_t      indent = strlen (filler);

    if (colon)
    {
      if (colon && colon[1] == ' ')
      {
        char   ignore [200];
        size_t len = min (sizeof(ignore)-1, colon-line+1);

        _strlcpy (ignore, line, len);
        if (cfg_ignore_lookup("[PE-resources]",str_trim(ignore)))
           continue;
      }
      indent += colon - line + 1;
    }
    if (i == 0)
       C_putc ('\n');
    C_puts (filler);
    C_puts_long_line (line, indent+1);
  }
  C_setraw (save);
  get_PE_version_info_free();
}

/**
 * With the "--pe" (and "--32" or "--64") option, check if a `file` is a PE-file.
 * If so, save the checksum, version-info, signing-status for later when
 * `report_file()` is ready to print this info.
 */
static BOOL get_PE_file_brief (const struct report *r, char *dest, size_t dest_size)
{
  struct ver_info ver;
  enum Bitness    bits;
  const char     *bitness;
  BOOL            chksum_ok  = FALSE;
  BOOL            version_ok = FALSE;

  *dest = '\0';

  if (r->key == HKEY_INC_LIB_FILE || r->key == HKEY_MAN_FILE ||
      r->key == HKEY_EVERYTHING_ETP || r->key == HKEY_PKG_CONFIG_FILE)
     return (FALSE);

  if (!check_if_PE(r->file, &bits))
     return (FALSE);

  if (opt.only_32bit && bits != bit_32)
     return (FALSE);

  if (opt.only_64bit && bits != bit_64)
     return (FALSE);

  memset (&ver, 0, sizeof(ver));
  chksum_ok  = verify_PE_checksum (r->file);
  version_ok = get_PE_version_info (r->file, &ver);
  if (version_ok)
     num_version_ok++;

  bitness = (bits == bit_32) ? "~232" :
            (bits == bit_64) ? "~364" : "~5?";

  /** Do not add a `\n` since `wintrust_check()` is called right after this function.
   */
  snprintf (dest, dest_size, "\n%sver ~6%u.%u.%u.%u~0, %s~0-bit, Chksum %s~0",
            r->filler, ver.val_1, ver.val_2, ver.val_3, ver.val_4,
            bitness, chksum_ok ? "~2OK" : "~5fail");
  return (TRUE);
}

/**
 * With the "--pe" (and "--32" or "--64") option, and if `file` is a PE-file
 * (verified in above function), do a check for any signatures.
 *
 * \todo
 *   If the PE has a SECURITY data-directory, try to extract it's raw data;
 *     \code
 *       const WIN_CERTIFICATE *cert = ...
 *       if (cert->wCertificateType == WIN_CERT_TYPE_PKCS_SIGNED_DATA ||
 *           cert->wCertificateType == WIN_CERT_TYPE_X509)
 *          use 'libssl_1.1.dll' and call 'PKCS7_verify()' on 'cert+1'
 *     \endcode
 *
 *    Refs:
 *      https://github.com/zed-0xff/pedump/blob/master/lib/pedump/security.rb
 *      http://pedump.me/1d82d1e52ca97759a6f90438e59e7dc7/#signature
 */
static BOOL get_wintrust_info (const char *file, char *dest, size_t dest_size)
{
  char  *p = dest;
  size_t left = dest_size;
  DWORD  rc = wintrust_check (file, TRUE, FALSE);

  *p = '\0';

  switch (rc)
  {
    case ERROR_SUCCESS:
         p    += snprintf (dest, left, " ~2(Verified");
         left -= p - dest;
         num_verified++;
         break;
    case TRUST_E_NOSIGNATURE:
    case TRUST_E_SUBJECT_FORM_UNKNOWN:
    case TRUST_E_PROVIDER_UNKNOWN:
         p    += snprintf (dest, left, " ~5(Not signed");
         left -= p - dest;
         break;
    case TRUST_E_SUBJECT_NOT_TRUSTED:
         p    += snprintf (dest, left, " ~5(Not trusted");
         left -= p - dest;
         break;
  }

  if (wintrust_signer_subject)
       snprintf (p, left, ", %s)~0.", wintrust_signer_subject);
  else snprintf (p, left, ")~0.");

  wintrust_cleanup();

  switch (opt.signed_status)
  {
    case SIGN_CHECK_NONE:
         return (FALSE);
    case SIGN_CHECK_ALL:
         return (TRUE);
    case SIGN_CHECK_SIGNED:
         if (rc == ERROR_SUCCESS)
            return (TRUE);
         return (FALSE);
    case SIGN_CHECK_UNSIGNED:
         if (rc != ERROR_SUCCESS)
            return (TRUE);
         return (FALSE);
  }
  return (FALSE);
}

/**
 * Print a summary at end of program.
 */
void report_final (int found)
{
  BOOL do_warn = FALSE;
  char duplicates [50] = "";
  char ignored [50] = "";

  if ((found_in_hkey_current_user || found_in_hkey_current_user_env ||
       found_in_hkey_local_machine || found_in_hkey_local_machine_sess_man) &&
       found_in_default_env)
  {
    /* We should only warn if a match finds file(s) from different sources.
     */
    do_warn = opt.quiet ? FALSE : TRUE;

    /* No need to warn if the total HKEY counts <= found_in_default_env.
     * Since it would probably mean the file(s) were found in the same location.
     */
    if ((found_in_hkey_current_user + found_in_hkey_current_user_env +
         found_in_hkey_local_machine + found_in_hkey_local_machine_sess_man) <= found_in_default_env)
      do_warn = FALSE;
  }

  if (do_warn || found_in_python_egg)
     C_putc ('\n');

  if (found && found_in_hkey_current_user)
     C_printf ("~3 (1): found in \"HKEY_CURRENT_USER\\%s\".~0\n", REG_APP_PATH);

  if (found && found_in_hkey_local_machine)
     C_printf ("~3 (2): found in \"HKEY_LOCAL_MACHINE\\%s\".~0\n", REG_APP_PATH);

  if (found && found_in_hkey_current_user_env)
     C_printf ("~3 (3): found in \"HKEY_CURRENT_USER\\%s\".~0\n", "Environment");

  if (found && found_in_hkey_local_machine_sess_man)
     C_printf ("~3 (4): found in \"HKEY_LOCAL_MACHINE\\%s\".~0\n",
               "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");

  if (found && found_in_python_egg)
     C_puts ("~3 (5): found in a .zip/.egg in 'sys.path[]'.~0\n");

   if (found_everything_db_dirty)
      C_puts ("~3 (6): EveryThing database is not up-to-date.~0\n");

  if (do_warn)
     C_printf ("\n"
               "  ~5The search found matches outside the default environment (PATH etc.).\n"
               "  Hence running an application from the Start-Button may result in different .EXE/.DLL\n"
               "  to be loaded than from the command-line. Revise the above registry-keys.\n\n~0");

  if (num_evry_dups)
     snprintf (duplicates, sizeof(duplicates), " (%lu duplicated)",
               (unsigned long)num_evry_dups);
  else if (ETP_num_evry_dups)
     snprintf (duplicates, sizeof(duplicates), " (%lu duplicated)",
               (unsigned long)ETP_num_evry_dups);

  if (num_evry_ignored)
     snprintf (ignored, sizeof(ignored), " (%lu ignored)",
               (unsigned long)num_evry_ignored);

  C_printf ("%s %s", dword_str((DWORD)found), plural_str(found, "match", "matches"));
  C_printf (" found for \"%s\"%s%s.", opt.file_spec, duplicates, ignored);

  if (opt.show_size && total_size > 0)
     C_printf (" Totalling %s (%s bytes). ",
               str_trim((char*)get_file_size_str(total_size)),
               qword_str(total_size));

  if (opt.grep.content && !opt.evry_host)
  {
    C_printf (" With %s %s for the \"--grep\" content. ",
               qword_str(opt.grep.num_matches),
               plural_str(opt.grep.num_matches, "match", "matches"));
  }

  if (opt.evry_host)
  {
    if (opt.debug >= 1 && ETP_total_rcv)
       C_printf ("\n%s bytes received from ETP-host(s).", dword_str(ETP_total_rcv));
  }
  else if (opt.PE_check)
  {
    C_printf (" %lu have PE-version info.", (unsigned long)num_version_ok);

    if (opt.signed_status != SIGN_CHECK_NONE)
       C_printf (" %lu %s verified.", (unsigned long)num_verified, plural_str(num_verified, "is", "are"));
  }
  C_putc ('\n');
}

