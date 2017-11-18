/**
 * \file    ignore.c
 * \ingroup Misc
 * \brief
 *   Support for reading a config-file with things to ignore.
 */

#include "envtool.h"
#include "color.h"
#include "smartlist.h"
#include "ignore.h"

static const char *sections[] = {
                  "Registry", "Python", "PE-resources"
                };

/**\struct ignore_node
 */
struct ignore_node {
       const char *section;  /** The section; one of the above */
       char       *value;
     };

static smartlist_t *ignore_list;

/**
 * Callback for \c smartlist_read_file():
 *
 * Accepts only strings like "ignore = xx" from the config-file.
 * Add to \c ignore_list with the correct "section".
 *
 * \param[in] sl    the smartlist to add the string-value to.
 * \param[in] line  the prepped string-value from the file opened in
 *                  \c cfg_ignore_init().
 */
static void cfg_parse (smartlist_t *sl, char *line)
{
  char   ignore [256], *p;
  struct ignore_node *node = NULL;
  BOOL   add_it = FALSE;
  static const char  *section;

  strip_nl (line);
  p = str_trim (line);

  if (!stricmp(p,"[Registry]"))
  {
    section = sections[0];
    return;
  }
  if (!stricmp(p,"[Python]"))
  {
    section = sections[1];
    return;
  }
  if (!stricmp(p,"[PE-resources]"))
  {
    section = sections[2];
    return;
  }
  if (*p == '[')
  {
    WARN ("Ignoring unknown section: %s.\n", p);
    return;
  }

  ignore[0] = '\0';

  if (section && sscanf(p, "ignore = %256s", ignore) == 1 && ignore[0] != '\"')
    add_it = TRUE;

  /* For stuff to "ignore with spaces", use this quite complex
   * sscanf() expression.
   *
   * Ref: https://msdn.microsoft.com/en-us/library/xdb9w69d.aspx
   */
  if (ignore[0] == '\"' &&
     sscanf(p, "ignore = \"%256[-_ :()\\/.a-zA-Z0-9^\"]\"", ignore) == 1)
    add_it = TRUE;

  if (add_it)
  {
    node = MALLOC (sizeof(*node));
    node->section = section;
    node->value   = STRDUP (ignore);
    smartlist_add (sl, node);
    DEBUGF (3, "[%s]: '%s'.\n", section, ignore);
  }
}

/**
 * Try to open and parse a config-file.
 * \param[in] fname  the config-file.
 */
int cfg_ignore_init (const char *fname)
{
  char *file = getenv_expand (fname);

  DEBUGF (3, "file: %s\n", file);
  if (file)
  {
    ignore_list = smartlist_read_file (file, (smartlist_parse_func)cfg_parse);
    FREE (file);
  }
  return (ignore_list != NULL);
}

/**
 * Lookup a \c value to test for ignore. Compare the \c section too.
 *
 * \param[in] section  look for the \c value in this section.
 * \param[in] value    the string-value to check.
 *
 * \retval 0 the \c section and \c value was not something to ignore.
 * \retval 1 the \c section and \c value was found in the \c ignore_list.
 */
int cfg_ignore_lookup (const char *section, const char *value)
{
  int i, max = ignore_list ? smartlist_len (ignore_list) : 0;

  for (i = 0; i < max; i++)
  {
    const struct ignore_node *node = smartlist_get (ignore_list, i);

    if (section && stricmp(section, node->section))
       continue;
    if (!stricmp(value, node->value))
    {
      DEBUGF (3, "Found '%s' in [%s].\n", value, section);
      return (1);
    }
  }
  return (0);
}

/**
 * Free the memory allocated in the \c ignore_list smartlist.
 * Called from \c cleanup() in envtool.c.
 */
void cfg_ignore_exit (void)
{
  int i, max;

  if (!ignore_list)
     return;

  max = smartlist_len (ignore_list);
  for (i = 0; i < max; i++)
  {
    struct ignore_node *node = smartlist_get (ignore_list, i);

    DEBUGF (3, "%d: ignore: '%s'\n", i, node->value);
    FREE (node->value);
    FREE (node);
  }
  smartlist_free (ignore_list);
}

