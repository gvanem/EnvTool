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

/**
 * \def CFG_MAX_SECTIONS  Maximum number of sections in a config-file.
 * \def CFG_SECTION_LEN   Maximum length of a section name.
 * \def CFG_KEYWORD_LEN   Maximum length of a keyword.
 * \def CFG_VALUE_LEN     Maximum length of a value.
 * \def FGETS_BUF_LEN     Length of the `fgets()` buffer
 * \def SSCANF_BUF_LEN    Length of the `sscanf()` buffer
 */
#define CFG_MAX_SECTIONS  10
#define CFG_SECTION_LEN   40
#define CFG_KEYWORD_LEN   40
#define CFG_VALUE_LEN    512
#define FGETS_BUF_LEN    (CFG_VALUE_LEN + CFG_KEYWORD_LEN + 2)
#define SSCANF_BUF_LEN   (CFG_KEYWORD_LEN + CFG_VALUE_LEN + 2*6 + 7)  /* 2 * strlen("%[^= ]") + strlen(" = [^\r\n]") */

typedef struct CFG_FILE {
        FILE        *file;
        char        *fname;                        /**< The name of this config-file. */
        unsigned     line;                         /**< The line number in `config_get_line()`. */
        int          num_sections;                 /**< Number of sections / handlers set in `cfg_init()`. */
        const char  *sections [CFG_MAX_SECTIONS];  /**< The sections this structure handles. */
        cfg_handler  handlers [CFG_MAX_SECTIONS];  /**< The config-handlers for this config-file. */
        smartlist_t *list;                         /**< A `smartlist_t` of `struct cfg_node *` */

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
 * Return the next line from the config-file with key, value and
 * section.
 *
 * \param[in] cf  the config-file structure of the current config-file (`cf->fname`).
 * \retval    0   when we have reached end-of-file.
 * \retval   >0   the line-number of the current line.
 */
static unsigned config_get_line (CFG_FILE *cf)
{
  char *p, *q, *l_quote, *r_quote;

  cf->keyword[0] = cf->value[0] = '\0';

  while (1)
  {
    char buf [FGETS_BUF_LEN];
    char fmt [SSCANF_BUF_LEN];

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

    /* Got a `[section]` line. Find a `key = val`' on next `fgets()`.
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
      TRACE (3, "line %u: keyword: '%s', value: '%s'\n", cf->line, cf->keyword, cf->value);
      continue;
    }

    r_quote = strrchr (cf->value, '\"');
    l_quote = strchr (cf->value, '\"');

    /* Remove trailing `;` or `#` comment characters.
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
    str_rtrim (cf->value);
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

  for (i = 0; i < cf->num_sections; i++)
  {
    if (section && !stricmp(section, cf->sections[i]))
    {
      TRACE (3, "Matched section '%s' at index %d.\n", cf->sections[i], i);
      return (cf->handlers[i]);
    }
  }
  return (NULL);
}

/*
 * warn_clang_style(). Print a warning for a cfg-file warning similar to how clang does it:
 *  c:\Users\Gisle\AppData\Roaming\envtool.cfg(97): Section [Shadow], Unhandled setting: 'xdtime=100000'.
 *                                                                                        ^~~~~~~~~~~~~
 */
static void warn_clang_style (const CFG_FILE *cf, const char *section, const char *key, const char *value)
{
  size_t kv_len = strlen (key) + strlen (value);
  int    save, len;
  char   slash = (opt.show_unix_paths ? '/' : '\\');
  char   cfg_name [_MAX_PATH];

  slashify2 (cfg_name, cf->fname, slash);
  if (kv_len > 50)
  {
    C_printf ("~6%s(%u):\n", cfg_name, cf->line);
    len = C_printf ("~5  Section %s, Unhandled setting: '%s=%s'\n~2", section, key, value);
  }
  else
    len = C_printf ("~6%s(%u): ~5Section %s, Unhandled setting: '%s=%s'\n~2", cfg_name, cf->line, section, key, value);

  save = C_setraw (1);
  C_printf ("%-*s^%s\n", (int)(len - kv_len - 3), "", str_repeat('~', kv_len));
  C_setraw (save);
  C_puts ("~0");
}

/**
 * Parse the config-file given in `cf->file`.
 * Build the `cf->list` smartlist as it is parsed.
 *
 * \param[in] cf  the config-file structure.
 */
static void parse_config_file (CFG_FILE *cf)
{
  TRACE (3, "file: %s.\n", cf->fname);

  while (1)
  {
    struct cfg_node *cfg;
    char            *p;
    size_t           p_size = CFG_SECTION_LEN + CFG_KEYWORD_LEN + 16;
    cfg_handler      handler;

    if (!config_get_line(cf))
       break;

    if (!cf->section[0])
       strcpy (cf->section, "<None>");

    TRACE (3, "line %2u: [%s]: %s = %s\n",
           cf->line, cf->section, cf->keyword, cf->value);

    /* Ignore "foo = <empty value>"
     */
    if (!*cf->value)
       continue;

    cfg = MALLOC (sizeof(*cfg) + p_size);
    p = (char*) (cfg + 1);
    cfg->section = p;

    snprintf (cfg->section, p_size - sizeof(*cfg), "[%s] %s", cf->section, cf->keyword);
    p = strchr (cfg->section, ']');
    p[1] = '\0';
    cfg->key   = p + 2;
    cfg->value = getenv_expand2 (cf->value);   /* Allocates memory */
    smartlist_add (cf->list, cfg);

    handler = lookup_section_handler (cf, cfg->section);
    if (handler)
    {
      if (!(*handler) (cfg->section, cfg->key, cfg->value))
         warn_clang_style (cf, cfg->section, cfg->key, cfg->value);
    }
    else
    {
      if (!cfg->section || cfg->section[0] == '\0')
         TRACE (3, "%s(%u): Keyword '%s' = '%s' in the CFG_GLOBAL section.\n",
                cf->fname, cf->line, cfg->key, cfg->value);
      else
         TRACE (3, "%s(%u): Keyword '%s' = '%s' in unknown section '%s'.\n",
                cf->fname, cf->line, cfg->key, cfg->value, cfg->section);
    }
  }
}

/**
 * Open a config-file with a number of `[section]` and `key = value` pairs.
 * Build up the `cf->list` as we go along and calling the parsers
 * in the var-arg list.
 *
 * \param[in] fname   The config-file to parse.
 * \param[in] section The first section to handle.
 *                    The next `va_arg` is the callback function for this section.
 */
CFG_FILE *cfg_init (const char *fname, const char *section, ...)
{
  CFG_FILE *cf;
  va_list   args;
  int       i;

  cf = CALLOC (sizeof(*cf), 1);
  cf->list  = smartlist_new();
  cf->fname = getenv_expand2 (fname);   /* Allocates memory */
  cf->file  = fopen (cf->fname, "rt");
  if (!cf->file)
  {
    WARN ("Failed to open \"%s\" (%s).\n", cf->fname, strerror(errno));
    cfg_exit (cf);
    return (NULL);
  }

  va_start (args, section);
  for (i = 0; section && i < CFG_MAX_SECTIONS; section = va_arg(args, const char*), i++)
  {
    if (!*section)
       section = "[<None>]";
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
 * Clean-up after `cfg_init()`.
 *
 * \param[in] cf  the config-file structure.
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

    FREE (cfg->value);
    FREE (cfg);
  }

  smartlist_free (cf->list);
  FREE (cf->fname);
  FREE (cf);
}
