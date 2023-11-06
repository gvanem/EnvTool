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
static const search_list sections[] = {
                       { 0, "[Compiler]" },
                       { 1, "[Registry]" },
                       { 2, "[Path]" },
                       { 3, "[Python]" },
                       { 4, "[PE-resources]" },
                       { 5, "[EveryThing]" },
                       { 6, "[LUA]" },     /* Only used in lua.c */
                       { 7, "[Login]" },   /* Only used in auth.c */
                       { 8, "[Shadow]" }   /* Use in 'envtool ---check -v' */
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
 * Help indices for `cfg_ignore_first()` and `cfg_ignore_next()`.
 */
static int  next_idx = -1;
static UINT curr_sec = UINT_MAX;

/**
 * Parser for `parse_config_file()` in `cfg_file.c`:
 *
 * Accepts only strings like `"ignore = xx"` from the config-file.
 * Add to `ignore_list` in the correct `sections[]` slot.
 *
 * \param[in] section  the section from the file opened in `parse_config_file()`.
 * \param[in] key      the key from the file opened in `parse_config_file()`.
 * \param[in] value    the malloced value from the file opened in `parse_config_file()`.
 */
bool cfg_ignore_handler (const char *section, const char *key, const char *value)
{
  if (section && !stricmp(key, "ignore"))
  {
    struct ignore_node *node;
    unsigned idx;

    if (!ignore_list)
       ignore_list = smartlist_new();

    idx = list_lookup_value (section, sections, DIM(sections));
    if (idx == UINT_MAX)
    {
      WARN ("Ignoring unknown section: %s.\n", section);
      return (true);
    }

    node = MALLOC (sizeof(*node));
    node->section = sections[idx].name;
    node->value   = value;
    smartlist_add (ignore_list, node);
    TRACE (3, "%s: ignore = '%s'\n", node->section, node->value);
    return (true);
  }
  return (false);
}

/**
 * Lookup a `value` to test for ignore. Compare the `section` too.
 *
 * \param[in] section  Look for the `value` in this `section`.
 * \param[in] value    The string-value to check.
 *
 * \retval false the `section` and `value` was not found in the `ignore_list`.
 * \retval true  the `section` and `value` was found in the `ignore_list`.
 */
bool cfg_ignore_lookup (const char *section, const char *value)
{
  int i, max;

  if (section[0] != '[' || !ignore_list)
     return (false);

  max = smartlist_len (ignore_list);
  for (i = 0; i < max; i++)
  {
    const struct ignore_node *node = smartlist_get (ignore_list, i);

    /* Not this section, try the next
     */
    if (stricmp(section, node->section))
       continue;

    /* An exact match.
     * This is case-sensitive if option '-c' was used.
     */
    if (str_equal(value, node->value))
    {
      TRACE (3, "Found '%s' in %s.\n", value, section);
      return (true);
    }

    /* A wildcard match.
     * This is case-sensitive if option '-c' was used.
     */
    if (fnmatch(node->value, value, fnmatch_case(FNM_FLAG_NOESCAPE | FNM_FLAG_PATHNAME)) == FNM_MATCH)
    {
      TRACE (3, "Wildcard match for '%s' in %s.\n", value, section);
      return (true);
    }
  }
  return (false);
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
    TRACE (3, "No such section: %s.\n", section);
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

  /* cfg_ignore_first() not called or nothing to ignore was
   * found by cfg_ignore_first().
   */
  if (next_idx == -1 || curr_sec == UINT_MAX)
     return (NULL);

  max = ignore_list ? smartlist_len (ignore_list) : 0;
  for (i = next_idx; i < max; i++)
  {
    node = smartlist_get (ignore_list, i);
    if (!stricmp(section, sections[curr_sec].name) && !stricmp(section, node->section))
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
    const char *section = sections[i].name;
    const char *ignored;

    j = 0;
    for (ignored = cfg_ignore_first(section);
         ignored;
         ignored = cfg_ignore_next(section), j++)
        ;
    TRACE (3, "section: %-15s: num: %d.\n", section, j);
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
