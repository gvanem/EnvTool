/**\file    get_file_assoc.c
 * \ingroup Misc
 *
 * \brief
 *   Gets the **File Associations** (`ASSOCSTR_EXECUTABLE`) for a file extension
 *   using `AssocQueryStringA()`: \n
 *    https://docs.microsoft.com/en-us/windows/desktop/api/shlwapi/nf-shlwapi-assocquerystringa
 */
#if !defined(_WIN32_WINNT)
  #undef  _WIN32_WINNT
  #define _WIN32_WINNT   0x0A00  /* Windows-10 */
#endif

#if !defined(_WIN32_IE)
  #undef  _WIN32_IE
  #define _WIN32_IE      0x800   /* _WIN32_IE_IE80 */
#endif

#include <windows.h>
#include <shlwapi.h>

#include "envtool.h"
#include "color.h"
#include "get_file_assoc.h"

#define ADD_VALUE(v)  { (unsigned)(v), #v }

static const struct search_list assoc_values[] = {
                    ADD_VALUE (ASSOCSTR_COMMAND),
                    ADD_VALUE (ASSOCSTR_EXECUTABLE),
                    ADD_VALUE (ASSOCSTR_FRIENDLYDOCNAME),
                    ADD_VALUE (ASSOCSTR_FRIENDLYAPPNAME),
                    ADD_VALUE (ASSOCSTR_NOOPEN),
                    ADD_VALUE (ASSOCSTR_SHELLNEWVALUE),
                    ADD_VALUE (ASSOCSTR_DDECOMMAND),
                    ADD_VALUE (ASSOCSTR_DDEIFEXEC),
                    ADD_VALUE (ASSOCSTR_DDEAPPLICATION),
                    ADD_VALUE (ASSOCSTR_DDETOPIC),
                    ADD_VALUE (ASSOCSTR_INFOTIP),
#if defined(_WIN32_IE) && (_WIN32_IE >= _WIN32_IE_IE60)
                    ADD_VALUE (ASSOCSTR_QUICKTIP),
                    ADD_VALUE (ASSOCSTR_TILEINFO),
                    ADD_VALUE (ASSOCSTR_CONTENTTYPE),
                    ADD_VALUE (ASSOCSTR_DEFAULTICON),
                    ADD_VALUE (ASSOCSTR_SHELLEXTENSION),
#endif
#if defined(_WIN32_IE) && (_WIN32_IE >= _WIN32_IE_IE80)
                    ADD_VALUE (ASSOCSTR_DROPTARGET),
                    ADD_VALUE (ASSOCSTR_DELEGATEEXECUTE),
#endif
#if defined(_MSC_VER) && (_WIN32_WINNT >= 0x0A00)
                    ADD_VALUE (ASSOCSTR_PROGID),
                    ADD_VALUE (ASSOCSTR_APPID),
                    ADD_VALUE (ASSOCSTR_APPPUBLISHER),
                    ADD_VALUE (ASSOCSTR_APPICONREFERENCE),
#endif
                  };

static char last_err [300];

/**
 * Return the error-string from the last failed function.
 */
const char *get_file_assoc_last_err (void)
{
  return (last_err);
}

/**
 * Print all registered associations for an file-extension.
 */
BOOL get_file_assoc_all (const char *extension)
{
  HRESULT hr;
  char    buf [1024];
  DWORD   buf_len;
  int     i, failed = 0;

  last_err[0] = '\0';

  for (i = 0; i < DIM(assoc_values); i++)
  {
    printf ("  %2d:  %-26s: ", i, assoc_values[i].name);
    buf_len = sizeof(buf);

    hr = AssocQueryStringA (0, assoc_values[i].value,
                            extension, NULL, buf, &buf_len);
    if (hr == S_OK)
       printf ("%s\n", buf);
    else
    {
      _strlcpy (last_err, win_strerror(hr), sizeof(last_err));
      printf ("Failed: %s\n", last_err);
      failed++;
    }
  }
  if (failed == 0)
     last_err[0] = '\0';
  return (failed > 0 ? FALSE : TRUE);
}

/**
 * Retrieves the registered friendly-name (`*program_descr`) and
 * executable program (`*program_exe`) associated for an file-extension.
 */
BOOL get_file_assoc (const char *extension, char **program_descr, char **program_exe)
{
  char    buf [1024];
  DWORD   buf_len = sizeof(buf);
  HRESULT hr;

  *program_descr = *program_exe = NULL;

  hr = AssocQueryStringA (0, ASSOCSTR_FRIENDLYDOCNAME,
                          extension, NULL, buf, &buf_len);
  if (hr != S_OK)
  {
    _strlcpy (last_err, win_strerror(hr), sizeof(last_err));
    DEBUGF (1, "Failed: %s\n", last_err);
    return (FALSE);
  }
  *program_descr = STRDUP (buf);

  buf_len = sizeof(buf);
  hr = AssocQueryStringA (ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE,
                          extension, NULL, buf, &buf_len);
  if (hr != S_OK)
  {
    _strlcpy (last_err, win_strerror(hr), sizeof(last_err));
    DEBUGF (1, "Failed: %s\n", last_err);
    return (FALSE);
  }

  last_err[0] = '\0';
  *program_exe = STRDUP (buf);
  return (TRUE);
}

/**
 * Get the actual casing for a file-name by getting the short-name
 * and then the long-name.\n
 * Internally, these functions seems to be using `SHGetFileInfo()`: \n
 *   https://docs.microsoft.com/en-us/windows/desktop/api/shellapi/nf-shellapi-shgetfileinfoa
 *
 * \param[in,out] file_p     A pointer to the file to be converted.\n
 *                           The new proper file-name is returned at the same location.\n
 * \param[in]     allocated  If TRUE, the `*file_p` was allocated by STRDUP() and will
 *                           be FREE() before `*file_p` is set to the new value.
 *
 * \retval  TRUE  if the `*file_p` was converted successfully.
 * \retval  FALSE if the convertion failed, `*file_p` is unchanged.
 *
 * \note
 *   The drive-letter is down-cased using `_fix_drive()` since
 *   `GetLongPathName()` does not touch that.
 *
 * \note
 *  Written with the inspiration from: \n
 *    http://stackoverflow.com/questions/74451/getting-actual-file-name-with-proper-casing-on-windows
 */
BOOL get_actual_filename (char **file_p, BOOL allocated)
{
  char buf [_MAX_PATH], *_new, *file;

  file = *file_p;
  if (GetShortPathNameA(file, buf, sizeof(buf)) == 0)
  {
    _strlcpy (last_err, win_strerror(GetLastError()), sizeof(last_err));
    DEBUGF (1, "Failed: %s\n", last_err);
    return (FALSE);
  }

  _new = MALLOC (_MAX_PATH);
  if (GetLongPathNameA(buf, _new, _MAX_PATH) == 0)
  {
    _strlcpy (last_err, win_strerror(GetLastError()), sizeof(last_err));
    DEBUGF (1, "Failed: %s\n", last_err);
    FREE (_new);
    return (FALSE);
  }

  if (allocated)
     FREE (file);

  _fix_drive (_new);
  *file_p = _new;
  last_err[0] = '\0';

  DEBUGF (1, "\n    short: '%s' ->\n    long:  '%s'\n", buf, *file_p);
  return (TRUE);
}


#if defined(GET_FILE_ASSOC_TEST)
struct prog_options opt;

char *searchpath (const char *file, const char *env_var)
{
  ARGSUSED (file);
  ARGSUSED (env_var);
  return (NULL);
}

static void usage (const char *I_am)
{
  printf ("Usage: %s [.file-extension | *]\n", I_am);
  exit (1);
}

int MS_CDECL main (int argc, char **argv)
{
  const char *extension_to_test;
  char       *program_descr, *program_exe;

  opt.debug = 0;
  C_use_colours = 1;
  crtdbug_init();

  if (argc == 2 && (argv[1][0] == '.' || argv[1][0] == '*'))
       extension_to_test = argv[1];
  else usage (argv[0]);

  C_printf ("File Associations (ASSOCSTR_EXECUTABLE) for ~3%s~0:\n", extension_to_test);

  if (!get_file_assoc(extension_to_test, &program_descr, &program_exe))
     C_printf ("Failed: %s\n", get_file_assoc_last_err());
  else
  {
    /* In case '*program_exe' is something like "F:\PROGRA~1\WINZIP\winzip32.exe",
     * convert to a long-name which should not contain any '~' SFN character.
     */
    if (*program_exe != '\0' && FILE_EXISTS(program_exe))
       get_actual_filename (&program_exe, TRUE);

    C_printf ("  ~3%s~0 -> ~6%s~0\n", program_descr, program_exe);

    C_printf ("\nAll Associations for ~3%s~0:\n", extension_to_test);
    get_file_assoc_all (extension_to_test);
  }

  FREE (program_descr);
  FREE (program_exe);

  crtdbug_exit();
  return (0);
}
#endif  /* GET_FILE_ASSOC_TEST */


