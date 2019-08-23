/**\file    cfg_file.c
 * \ingroup Misc
 * \brief   Functions for parsing a config-file.
 */
#include <stdio.h>
#include <stdlib.h>

#include "smartlist.h"
#include "cfg_file.h"
#include "envtool.h"

/**
 * The default "do nothing" parser.
 */
static void none_or_global_parser (const char *section, const char *key, const char *value, unsigned line);

/** The parsers for each 'CFG_x' section.
 */
static cfg_parser cfg_parsers [CFG_MAX_SECTIONS];

/** A list of 'struct cfg_node'
 */
static smartlist_t *cfg_list;

/** The current config-file we are parsing.
 */
static char *cfg_file = NULL;

/**
 * Return the next line from the config-file with key, value and
 * section. Increment line of config-file.
 *
 * \param[in]  fil        the `FILE*` of the current config-file (`cfg_file`).
 * \param[out] line       the line-number of the current config-file (`cfg_file`).
 * \param[in]  key_p      a pointer to a `char*` that the "key" gets assigned to.
 * \param[in]  val_p      a pointer to a `char*` that the "value" gets assigned to.
 * \param[in]  section_p  a pointer to a `char*` that the "section" gets assigned to.
 * \retval    0           when we have 'reached end-of-file'.
 * \retval    1           there is more to read.
 */
static int config_get_line (FILE      *fil,
                            unsigned  *line,
                            char     **key_p,
                            char     **val_p,
                            char     **section_p)
{
  static char section [40] = { '\0' };
  char   key [256];
  char   val [512];

  while (1)
  {
    char *p, *q;
    char  buf [1000];

    if (!fgets(buf,sizeof(buf)-1,fil))   /* EOF */
       return (0);

    for (p = buf; *p && isspace((int)*p); )
        p++;

    if (*p == '#' || *p == ';')
    {
      (*line)++;
      continue;
    }

    /* Got a '[section]' line. Find a 'key = val' on next 'fgets()'.
     */
    if (sscanf(p,"[%[^]\r\n]", section) == 1)
    {
      (*line)++;
      continue;
    }
    if (sscanf(p,"%[^= ] = %[^\r\n]", key, val) != 2)
    {
      (*line)++;
      continue;
    }

    q = strrchr (val, '\"');
    p = strchr (val, ';');

    /* Remove trailing ';' or '#' comment characters.
     */
    if (p > q)
       *p = '\0';
    p = strchr (val, '#');
    if (p > q)
       *p = '\0';
    break;
  }

  (*line)++;
  *section_p = STRDUP (section);
  *key_p     = STRDUP (key);
  *val_p     = STRDUP (str_rtrim(val));
  return (1);
}

/**
 * Given a section-number, lookup the parser for it.
 *
 * \param[in] section  the section enum value to look for.
 * \retval             the section parser function.
 */
static cfg_parser lookup_parser (enum cfg_sections section)
{
  ASSERT (section < CFG_MAX_SECTIONS);
  if (!cfg_parsers[section])
     cfg_parsers[section] = none_or_global_parser;
  return (cfg_parsers[section]);
}

/**
 * Add a parser function for a section.
 *
 * \param[in] section  the section enum value.
 * \param[in] parser   the parser function for this section.
 */
void cfg_add_parser (enum cfg_sections section, cfg_parser parser)
{
  ASSERT (section < CFG_MAX_SECTIONS);
  cfg_parsers[section] = parser;
}

/*
 * Given a section-name, lookup the 'enum cfg_section' for the name.
 *
 * \param[in] section  the section name.
 * \retval             the section enum value.
 */
static enum cfg_sections lookup_section (const char *section)
{
#define CHK_SECTION(s)   do {                           \
                           if (!stricmp(section, #s+4)) \
                              return (s);               \
                         } while (0)

  if (!section)
     return (CFG_NONE);

  if (!section[0])
     return (CFG_GLOBAL);

  CHK_SECTION (CFG_REGISTRY);
  CHK_SECTION (CFG_COMPILER);
  CHK_SECTION (CFG_EVERYTHING);
  CHK_SECTION (CFG_PYTHON);
  CHK_SECTION (CFG_PE_RESOURCES);
  CHK_SECTION (CFG_LOGIN);

  if (!stricmp(section,"PE-resources"))
     return (CFG_PE_RESOURCES);
  return (CFG_NONE);
}

/**
 * Parse the config-file given in 'file'.
 * Build the 'cfg_list' smartlist as it is parsed.
 *
 * \param[in] file  the config-file to parse.
 */
static int parse_config_file (FILE *file)
{
  char    *key, *value, *section;
  unsigned line  = 0;
  unsigned lines = 0;

  DEBUGF (2, "file: %s.\n", cfg_file);

  while (1)
  {
    section = key = value = NULL;   /* set in config_get_file() */

    if (!config_get_line(file,&line,&key,&value,&section))
       break;

    lines++;

    DEBUGF (3, "line %2u: [%s]: %s = %s\n",
            line, section[0] == '\0' ? "<None>" : section, key, value);

    /* Ignore "foo = <empty value>"
     */
    if (*value)
    {
      struct cfg_node *cfg = CALLOC (sizeof(*cfg), 1);

      cfg->section = section;
      cfg->key     = key;
      cfg->value   = value;
      smartlist_add (cfg_list, cfg);
      (*lookup_parser (lookup_section(section))) (section, key, value, line);
    }
  }
  return (lines);
}

static void none_or_global_parser (const char *section, const char *key, const char *value, unsigned line)
{
  if (section[0] == '\0')
     DEBUGF (1, "%s(%u): Keyword '%s' = '%s' in the CFG_GLOBAL section.\n",
             cfg_file, line, key, value);
  else
     DEBUGF (1, "%s(%u): Keyword '%s' = '%s' in unknown section '%s'.\n",
             cfg_file, line, key, value, section);
}

/**
 * Open a config-file with `[section]` and `key = value` pairs.
 * Build up the 'cfg_list' as we go along and calling the parsers
 * added with `cfg_add_parser()`.
 *
 * \param[in] fname  the config-file to parse.
 */
void cfg_init (const char *fname)
{
  FILE *f;

  if (cfg_file)
     FATAL ("cfg_init() is not reentrant; call cfg_exit() first.\n");

  cfg_file = getenv_expand (fname);
  if (!cfg_file)
     return;

  f = fopen (cfg_file, "rt");
  if (f)
  {
    cfg_list = smartlist_new();
    parse_config_file (f);
    fclose (f);
  }
}

/**
 * Clean-up after 'cfg_init()'.
 * Now ready to parse another config-file.
 */
void cfg_exit (void)
{
  int i, max = cfg_list ? smartlist_len (cfg_list) : 0;

  for (i = 0; i < max; i++)
  {
    struct cfg_node *cfg = smartlist_get (cfg_list, i);

    FREE (cfg->section);
    FREE (cfg->key);
    FREE (cfg->value);
    FREE (cfg);
  }
  FREE (cfg_file);
  smartlist_free (cfg_list);
  cfg_list = NULL;
}
