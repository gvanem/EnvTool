/**
 * \file    ignore.c
 * \ingroup Misc
 * \brief
 *   Support for reading a config-file with things to
 *   ignore at run-time.
 */
#include "envtool.h"
#include "color.h"
#include "smartlist.h"
#include "ignore.h"

const struct search_list sections[] = {
                       { 0, "[Compiler]" },
                       { 1, "[Registry]" },
                       { 2, "[Python]" },
                       { 3, "[PE-resources]" }
                     };

/**\struct ignore_node
 */
struct ignore_node {
       const char *section;  /** The section; one of the above */
       char       *value;
     };

/** A dynamic array of \c "ignore_node".
 */
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
  char   ignore [256], *p, *q;
  struct ignore_node *node = NULL;
  BOOL   add_it = FALSE;
  static const char *section = NULL;

  strip_nl (line);
  p = str_trim (line);

  if (*p == '[')
  {
    unsigned idx = list_lookup_value (p, sections, DIM(sections));

    if (idx == UINT_MAX)
         WARN ("Ignoring unknown section: %s.\n", p);
    else section = sections[idx].name; /* remember the section for next line */
    return;
  }

  if (!section)
     return;

  /* Remove trailing comments and spaces before that.
   */
  q = strchr (line, '#');
  if (q)
  {
    *q-- = '\0';
    while (*q == ' ' || *q == '\t')
       *q-- = '\0';
  }

  ignore[0] = '\0';

  if (sscanf(p, "ignore = %256s", ignore) == 1 && ignore[0] != '\"')
     add_it = TRUE;

  p = strchr (line, '\0');
  q = strchr (line, '\"');
  if (ignore[0] == '\"' && p[-1] == '\"' && p - q >= 3 && p < line+sizeof(ignore))
  {
    p[-1] = '\0';
    _strlcpy (ignore, q+1, sizeof(ignore));
    add_it = TRUE;
  }

  if (add_it)
  {
    node = MALLOC (sizeof(*node));
    node->section = section;
    node->value   = STRDUP (ignore);
    smartlist_add (sl, node);
    DEBUGF (3, "%s: '%s'.\n", section, ignore);
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
  cfg_ignore_dump();
  return (ignore_list != NULL);
}

/**
 * Lookup a \c value to test for ignore. Compare the \c section too.
 *
 * \param[in] section  Look for the \c value in this section.
 * \param[in] value    The string-value to check.
 *
 * \retval 0 the \c section and \c value was not something to ignore.
 * \retval 1 the \c section and \c value was found in the \c ignore_list.
 */
int cfg_ignore_lookup (const char *section, const char *value)
{
  int i, max;

  if (section[0] != '[' || !ignore_list)
     return (0);

  max = smartlist_len (ignore_list);
  for (i = 0; i < max; i++)
  {
    const struct ignore_node *node = smartlist_get (ignore_list, i);

    if (!stricmp(section, node->section) && !stricmp(value, node->value))
    {
      DEBUGF (3, "Found '%s' in %s.\n", value, section);
      return (1);
    }
  }
  return (0);
}

/**
 * Helper indices for \c cfg_ignore_first() and
 * \c cfg_ignore_next().
 */
static int  next_idx = -1;
static UINT curr_sec = UINT_MAX;

/**
 * Lookup the first ignored \c value in a \c section.
 */
const char *cfg_ignore_first (const char *section)
{
  const struct ignore_node *node;
  int   i, max;
  UINT  idx = list_lookup_value (section, sections, DIM(sections));

  if (idx == UINT_MAX)
  {
    DEBUGF (2, "No such section: %s.\n", section);
    goto not_found;
  }

  max = ignore_list ? smartlist_len (ignore_list) : 0;
  for (i = 0; i < max; i++)
  {
    node = smartlist_get (ignore_list, i);
    if (!stricmp(section, node->section))
    {
      next_idx = i + 1;
      curr_sec = idx;
      return (node->value);
    }
  }

not_found:
  next_idx = -1;
  curr_sec = UINT_MAX;
  return (NULL);
}

/**
 * Lookup the next ignored \c value in the same \c section.
 */
const char *cfg_ignore_next (const char *section)
{
  const struct ignore_node *node;
  int   i, max;

  /* cfg_ignore_first() not called or no ignorables found
   * by cfg_ignore_first().
   */
  if (next_idx == -1 || curr_sec == UINT_MAX)
     return (NULL);

  max = ignore_list ? smartlist_len (ignore_list) : 0;
  for (i = next_idx; i < max; i++)
  {
    node = smartlist_get (ignore_list, i);
    if (!stricmp(section,sections[curr_sec].name) &&
        !stricmp(section, node->section))
    {
      next_idx = i + 1;
      return (node->value);
    }
  }
  next_idx = -1;
  return (NULL);
}

/**
 * Dump number of ignored values in all sections.
 */
void cfg_ignore_dump (void)
{
  int i, j;

  for (i = 0; i < DIM(sections); i++)
  {
    const char *ignored, *section = sections[i].name;

    j = 0;
    for (ignored = cfg_ignore_first(section);
         ignored;
         ignored = cfg_ignore_next(section), j++)
        ;
    DEBUGF (3, "section: %-15s: num: %d.\n", section, j);
  }
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

    FREE (node->value);
    FREE (node);
  }
  smartlist_free (ignore_list);
  ignore_list = NULL;
}

#if 0
/*
 * Thinking loud; future generic section lookup of
 * a handler give a "[Section]" with a key and value.
 */
static const char *Compiler_handler (const char *key, const char *value)
{
  return (NULL);
}

static const char *Registry_handler (const char *key, const char *value)
{
  return (NULL);
}

static const char *Python_handler (const char *key, const char *value)
{
  return (NULL);
}

static const char *PE_resources_handler (const char *key, const char *value)
{
  return (NULL);
}

typedef const char * (*handler_t) (const char *key, const char *value);

static const handler_t handler_tab [DIM(sections)] = {
                       Compiler_handler,
                       Registry_handler,
                       Python_handler,
                       PE_resources_handler
                     };

static handler_t cfg_lookup_handler (const char *section)
{
  unsigned idx = list_lookup_value (section, sections, DIM(sections));

  if (idx < UINT_MAX && (int)sections[idx].value >= 0 && sections[idx].value < DIM(handler_tab))
     return handler_tab [sections[idx].value];
  return (NULL);
}

const char *cfg_Compiler_lookup (const char *key, const char *value)
{
  handler_t handler = cfg_lookup_handler ("[Compiler]");
  if (handler)
     return (*handler) (key, value);
  return (NULL);
}

int cfg_ignore_lookup2 (const char *section, const char *value)
{
  handler_t handler = cfg_lookup_handler (section);
  if (handler)
     return (int) (*handler) ("ignore", value);
  return (0);
}
#endif