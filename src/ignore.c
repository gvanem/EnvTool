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
       const char *value;    /** The value to ignore (allocated by 'cfg_file.c') */
     };

/** A dynamic array of ignore_node.
 * \anchor ignore_list
 */
static smartlist_t *ignore_list = NULL;

/**
 * Help indices for cfg_ignore_first() and cfg_ignore_next().
 */
static int  next_idx = -1;
static UINT curr_sec = UINT_MAX;

/**
 * Parser for `parse_config_file()` in `cfg_file.c`:
 *
 * Accepts only strings like `"ignore = xx"` from the config-file.
 * Add to \ref ignore_list in the correct `sections[]` slot.
 *
 * \param[in] section the section from the file opened in parse_config_file().
 * \param[in] key     the key from the file opened in parse_config_file().
 * \param[in] value   the value from the file opened in parse_config_file().
 */
void cfg_ignore_handler (const char *section, const char *key, const char *value)
{
  if (!stricmp(key,"ignore"))
  {
    struct ignore_node *node;
    unsigned idx;

    if (!ignore_list)
       ignore_list = smartlist_new();

    idx = list_lookup_value (section, sections, DIM(sections));
    if (idx == UINT_MAX)
    {
      WARN ("Ignoring unknown section: %s.\n", section);
      return;
    }

    node = MALLOC (sizeof(*node));
    node->section = sections[idx].name;
    node->value   = value;
    smartlist_add (ignore_list, node);
    DEBUGF (2, "%s: ignore = '%s'\n", node->section, node->value);
  }
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

    FREE (node);
  }
  smartlist_free (ignore_list);
  ignore_list = NULL;
}
