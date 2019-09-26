/**\file    cfg_file.c
 * \ingroup Misc
 * \brief   Functions for parsing a config-file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "color.h"
#include "cfg_file.h"
#include "envtool.h"

#define CFG_MAX_SECTIONS  10
#define CFG_SECTION_LEN   40
#define CFG_KEYWORD_LEN   40
#define CFG_VALUE_LEN    512

typedef struct CFG_FILE {
        FILE        *file;
        char        *fname;                        /**< The name of this config-file. */
        unsigned     line;                         /**< The line number in `config_get_line()`. */
        int          num_sections;                 /**< Number of sections / handlers set in `cfg_init()`. */
        const char  *sections [CFG_MAX_SECTIONS];  /**< The sections this structure handles. */
        cfg_handler  handlers [CFG_MAX_SECTIONS];  /**< The config-handlers for this config-file. */
        smartlist_t *list;                         /**< A 'smartlist_t' of 'struct cfg_node *' */

        /** The work-buffers used by `config_get_line()`.
         */
        char section [CFG_SECTION_LEN+1];
        char keyword [CFG_KEYWORD_LEN+1];
        char value   [CFG_VALUE_LEN+1];
      } CFG_FILE;

/**
 * \struct cfg_node
 * The structure for each key/value pair in a config-file.
 */
struct cfg_node {
       char  *section;  /**< The name of the section (allocated) */
       char  *key;      /**< The key-name (allocated) */
       char  *value;    /**< The key value (allocated) */
     };

/**
 * The default "do nothing" parser.
 */
static void none_or_global_handler (const char *section, const char *key, const char *value);

/** The current config-file we are parsing.
 */
static const char *cfg_fname = NULL;

/** The current line of the config-file we are parsing.
 */
static unsigned cfg_line;

/**
 * Return the next line from the config-file with key, value and
 * section.
 *
 * \param[in] cf  the config-file structure of the current config-file (`cf->fname`).
 * \retval    0   when we have 'reached end-of-file'.
 * \retval   >0   the line-number of the current line.
 */
static unsigned config_get_line (CFG_FILE *cf)
{
  char *p, *q, *l_quote, *r_quote;

  cf->keyword[0] = cf->value[0] = '\0';

  while (1)
  {
    char buf [1000];
    char fmt [100];

    ASSERT (sizeof(buf) >= CFG_VALUE_LEN + CFG_KEYWORD_LEN + 2);

    if (!fgets(buf, sizeof(buf), cf->file))   /* EOF */
       return (0);

    /* Remove leading spaces
     */
    p = str_ltrim  (buf);

    /* Ignore newlines or comment lines
     */
    if (strchr("\r\n#;", *p))
    {
      cf->line++;
      continue;
    }

    /* Got a '[section]' line. Find a 'key = val' on next 'fgets()'.
     */
    snprintf (fmt, sizeof(fmt), "[%%%d[^]\r\n]", CFG_SECTION_LEN);
    if (sscanf(p, fmt, cf->section) == 1)
    {
      cf->line++;
      continue;
    }

    snprintf (fmt, sizeof(fmt), "%%%d[^= ] = %%%d[^\r\n]", CFG_KEYWORD_LEN, CFG_VALUE_LEN);
    if (sscanf(p, fmt, cf->keyword, cf->value) != 2)
    {
      cf->line++;
      DEBUGF (3, "line %u: keyword: '%s', value: '%s'\n", cf->line, cf->keyword, cf->value);
      continue;
    }

    r_quote = strrchr (cf->value, '\"');
    l_quote = strchr (cf->value, '\"');

    /* Remove trailing ';' or '#' comment characters.
     * First check for a correctly quoted string value.
     */
    if (l_quote && r_quote && r_quote > l_quote)
         q = r_quote;
    else q = cf->value;

    p = strchr (q, ';');
    if (p)
       *p = '\0';
    p = strchr (q, '#');
    if (p)
       *p = '\0';

    break;
  }
  return (++cf->line);
}

/**
 * Given a section, return the `cfg_handler` for it.
 *
 * \param[in] cf       the config-file structure for the config-file.
 * \param[in] section  the section string.
 */
static cfg_handler lookup_section_handler (CFG_FILE *cf, const char *section)
{
  int i;

  cfg_line  = cf->line;
  cfg_fname = cf->fname;

  for (i = 0; i < cf->num_sections; i++)
  {
    if (section && !stricmp(section, cf->sections[i]))
    {
      DEBUGF (3, "Matched section '%s' at index %d.\n", cf->sections[i], i);
      return (cf->handlers[i]);
    }
  }
  return (none_or_global_handler);
}

/**
 * The "do nothing" parser to catch sections not hooked by others.
 */
static void none_or_global_handler (const char *section, const char *key, const char *value)
{
  if (!section || section[0] == '\0')
     DEBUGF (3, "%s(%u): Keyword '%s' = '%s' in the CFG_GLOBAL section.\n",
             cfg_fname, cfg_line, key, value);
  else
     DEBUGF (3, "%s(%u): Keyword '%s' = '%s' in unknown section '%s'.\n",
             cfg_fname, cfg_line, key, value, section);
}

/**
 * Parse the config-file given in 'cf->file'.
 * Build the 'cf->list' smartlist as it is parsed.
 *
 * \param[in] cf  the config-file structure.
 */
static void parse_config_file (CFG_FILE *cf)
{
  DEBUGF (3, "file: %s.\n", cfg_fname);

  while (1)
  {
    struct cfg_node *cfg;
    char            *val, buf [40];
    cfg_handler      handler;

    if (!config_get_line(cf))
       break;

    DEBUGF (3, "line %2u: [%s]: %s = %s\n",
            cf->line, cf->section[0] == '\0' ? "<None>" : cf->section,
            cf->keyword, cf->value);

    /* Ignore "foo = <empty value>"
     */
    if (!*cf->value)
       continue;

    cfg = MALLOC (sizeof(*cfg));
    if (cf->section[0])
    {
      snprintf (buf, sizeof(buf), "[%s]", cf->section);
      cfg->section = STRDUP (buf);
    }
    else
      cfg->section = NULL;

    cfg->key = STRDUP (cf->keyword);

    str_rtrim (cf->value);
    if (strchr(cf->value,'%'))
         val = getenv_expand (cf->value);   /* Allocates memory */
    else val = NULL;
    if (val)
         cfg->value = STRDUP (val);
    else cfg->value = STRDUP (cf->value);

    smartlist_add (cf->list, cfg);

    handler = lookup_section_handler (cf, cfg->section);
    (*handler) (cfg->section, cfg->key, cfg->value);
  }
}

/**
 * Open a config-file with a number of `[section]` and `key = value` pairs.
 * Build up the 'cf->list' as we go along and calling the parsers
 * in the var-arg list.
 *
 * \param[in] fname   the config-file to parse.
 * \param[in] section the first section to handle.
 */
CFG_FILE *cfg_init (const char *fname, const char *section, ...)
{
  CFG_FILE *cf;
  va_list   args;
  char     *_fname;
  int       i;

  if (strchr(fname,'%'))
       _fname = getenv_expand (fname);    /* Allocates memory */
  else _fname = STRDUP (fname);

  if (!_fname)
     return (NULL);

  cf = CALLOC (sizeof(*cf), 1);
  cf->list  = smartlist_new();
  cf->fname = _fname;
  cfg_fname = cf->fname;
  cf->file  = fopen (cf->fname, "rt");
  if (!cf->file)
  {
    cfg_exit (cf);
    return (NULL);
  }

  for (i = 0; i < CFG_MAX_SECTIONS; i++)
  {
    cf->sections[i] = NULL;
    cf->handlers[i] = none_or_global_handler;
  }

  va_start (args, section);
  for (i = 0; section && i < CFG_MAX_SECTIONS; section = va_arg(args, const char*), i++)
  {
    cf->sections [i] = section;
    cf->handlers [i] = va_arg (args, cfg_handler);
    cf->num_sections++;
  }
  if (i == CFG_MAX_SECTIONS && va_arg(args, const char*))
     WARN ("Too many sections. Max %d.\n", CFG_MAX_SECTIONS);

  va_end (args);
  parse_config_file (cf);
  fclose (cf->file);
  cf->file = NULL;
  return (cf);
}

/**
 * Clean-up after 'cfg_init()'.
 */
void cfg_exit (CFG_FILE *cf)
{
  int i, max;

  if (!cf)
     return;

  max = cf->list ? smartlist_len (cf->list) : 0;
  for (i = 0; i < max; i++)
  {
    struct cfg_node *cfg = smartlist_get (cf->list, i);

    FREE (cfg->section);
    FREE (cfg->key);
    FREE (cfg->value);
    FREE (cfg);
  }

  smartlist_free (cf->list);
  FREE (cf->fname);
  FREE (cf);
  cfg_fname = NULL;
  cfg_line  = 0;
}
