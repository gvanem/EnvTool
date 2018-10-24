/**
 * \file    ignore.c
 * \ingroup Misc
 * \brief
 *   Support for reading a config-file with things to
 *   ignore at run-time.
 *
 * This file will probably be extended to handle configuration of
 * other settings later.
 */
#include "envtool.h"
#include "color.h"
#include "smartlist.h"
#include "ignore.h"

/**
 * The list of sections we handle here.
 */
static const struct search_list sections[] = {
                              { 0, "[Compiler]" },
                              { 1, "[Registry]" },
                              { 2, "[Python]" },
                              { 3, "[PE-resources]" },
                              { 4, "[EveryThing]" },
                              { 5, "[Login]" }  /* Only used in auth.c */
                            };

/**\struct ignore_node
 */
struct ignore_node {
       const char *section;  /** The section; one of the ones in `sections[]` */
       char       *value;    /** The value to ignore (allocated by STRDUP()) */
     };

/** A dynamic array of ignore_node.
 * \anchor ignore_list
 */
static smartlist_t *ignore_list = NULL;

/**
 * Callback for smartlist_read_file():
 *
 * Accepts only strings like `"ignore = xx"` from the config-file.
 * Add to \ref ignore_list in the correct `sections[]` slot.
 *
 * \param[in] sl    the smartlist to add the string-value to.
 * \param[in] line  the prepped string-value from the file opened in
 *                  cfg_ignore_init().
 */
static void cfg_parse (smartlist_t *sl, const char *line)
{
  char   ignore [256];
  char  *start, *copy;
  struct ignore_node *node = NULL;
  BOOL   quoted = FALSE;
  int    num;
  static const char *section = NULL;

  /* Fast exit on simple lines
   */
  if (*line == '\0' || *line == '\r' || *line == '\n')
     return;

  copy  = STRDUP (line);
  start = str_ltrim (copy);

  if (*start == '[')
  {
    unsigned idx = list_lookup_value (strip_nl(start), sections, DIM(sections));

    if (idx == UINT_MAX)
         WARN ("Ignoring unknown section: %s.\n", start);
    else section = sections[idx].name;   /* remember the section for next line */
    goto quit;
  }

  if (!section)
     goto quit;

  ignore[0] = '\0';
  num = sscanf (start,"ignore = \"%255[^\"]\"", ignore);
  if (num == 1)
       quoted = TRUE;
  else num = sscanf (start, "ignore = %255[^#\r\n]", ignore);

  if (num == 1)
  {
    node = MALLOC (sizeof(*node));
    node->section = section;
    node->value   = STRDUP (str_rtrim(ignore));
    smartlist_add (sl, node);
    DEBUGF (3, "%s: %s: '%s'\n", quoted ? "quoted" : "unquoted", node->section, node->value);
  }
  else
    DEBUGF (3, "num=%d, ignore: '%s'.\n", num, ignore);

quit:
  FREE (copy);
}

/**
 * Try to open and parse a config-file.
 *
 * \param[in] fname  the config-file.
 */
int cfg_ignore_init (const char *fname)
{
  char *file = getenv_expand (fname);

  DEBUGF (3, "file: %s\n", file);
  if (file)
  {
    ignore_list = smartlist_read_file (file, cfg_parse);
    FREE (file);
  }
  cfg_ignore_dump();
  return (ignore_list != NULL);
}

/**
 * Lookup a `value` to test for ignore. Compare the `section` too.
 *
 * \param[in] section  Look for the `value` in this `section`.
 * \param[in] value    The string-value to check.
 *
 * \retval 0 the `section` and `value` was not something to ignore.
 * \retval 1 the `section` and `value` was found in the \ref ignore_list.
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

     /* Not this section, try the next
      */
    if (stricmp(section, node->section))
       continue;

    /* An exact case-insensitive match
     */
    if (!stricmp(value, node->value))
    {
      DEBUGF (3, "Found '%s' in %s.\n", value, section);
      return (1);
    }

    /* A wildcard case-insensitive match
     */
    if (fnmatch(node->value, value, FNM_FLAG_NOCASE) == FNM_MATCH)
    {
      DEBUGF (3, "Wildcard match for '%s' in %s.\n", value, section);
      return (1);
    }
  }
  return (0);
}

/**
 * Help indices for cfg_ignore_first() and cfg_ignore_next().
 */
static int  next_idx = -1;
static UINT curr_sec = UINT_MAX;

/**
 * Lookup the first ignored `value` in a `section`.
 *
 * \param[in] section  the `section` to start in.
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
 * Lookup the next ignored `value` in the same `section`.
 *
 * \param[in] section  the `section` to search in.
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
    if (!stricmp(section, sections[curr_sec].name) &&
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
 * Free the memory allocated in the ignore_list smartlist.
 * Called from cleanup() in envtool.c.
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
/**
 * \todo
 *   Thinking out loud:
 *     A future generic section lookup of a handler given
 *     a "[Section]" with a key and value.
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
