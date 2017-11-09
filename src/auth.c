/** \file auth.c
 *
 * \brief
 * Functions for parsing and lookup of host/user records in
 *   '%APPDATA%/.netrc' and
 *   '%APPDATA%/.authinfo'.
 *
 * Used in remote queries in Everything_ETP.c.
 *
 * This file is part of envtool.
 *
 * By Gisle Vanem <gvanem@yahoo.no> August 2017.
 */
#include "color.h"
#include "envtool.h"
#include "smartlist.h"
#include "auth.h"

struct login_info {
       BOOL  is_default;
       BOOL  is_netrc;
       char *host;
       char *user;
       char *passw;
       int   port;
     };

static smartlist_t *login_list = NULL;

/*
 * Common to both 'netrc_init()' and 'authinfo_init()'.
 * The 'login_list' is grown in the 'parser' functions below.
 */
static int common_init (const char *fname, smartlist_parse_func parser)
{
  char        *file = getenv_expand (fname);
  smartlist_t *sl;
  BOOL         first_time = (login_list == NULL);

  sl = file ? smartlist_read_file (file, parser) : NULL;
  FREE (file);

  if (!sl)
     return (0);

  if (first_time)
     login_list = sl;
  else
  {
    smartlist_append (login_list, sl);
    smartlist_free (sl);
  }
  return (1);
}

/*
 * Free the memory allocated in the 'login_list' smartlist.
 */
static void common_exit (BOOL is_netrc)
{
  int i, max;

  if (!login_list)
     return;

  max = smartlist_len (login_list);
  DEBUGF (2, "is_netrc: %d, length of list now: %d\n", is_netrc, max);

  for (i = 0; i < max; i++)
  {
    struct login_info *li = smartlist_get (login_list, i);

    DEBUGF (2, "i: %2d, li->is_netrc: %d, do %sdelete this.\n",
            i, li->is_netrc, (li->is_netrc == is_netrc) ? "" : "not ");

    if (li->is_netrc != is_netrc)
       continue;

    FREE (li->host);
    FREE (li->user);
    FREE (li->passw);
    FREE (li);
    smartlist_del (login_list, i);
    max--;
    i--;
  }

  DEBUGF (2, "length of list now: %d\n", smartlist_len(login_list));

  if (smartlist_len(login_list) == 0)
  {
    smartlist_free (login_list);
    login_list = NULL;
  }
}

/*
 * Search the 'login_list' smartlist for 'host'.
 */
static const struct login_info *common_lookup (const char *host, BOOL is_netrc)
{
  const struct login_info *def_li = NULL;
  int   i, max = login_list ? smartlist_len (login_list) : 0;

  for (i = 0; i < max; i++)
  {
    const struct login_info *li = smartlist_get (login_list, i);
    char  buf [300];

    if (li->is_netrc != is_netrc)
       continue;

    if (li->is_default)
       def_li = li;

    snprintf (buf, sizeof(buf), "is_netrc: %d, host: '%s', user: '%s', passw: '%s', port: %d\n",
              li->is_netrc, li->is_default ? "*default*" : li->host, li->user, li->passw, li->port);

    if (opt.do_tests)
    {
      C_setraw (1);
      C_printf ("  %s", buf);
      C_setraw (0);
    }
    else
      DEBUGF (3, buf);

    if (li->host && host && !stricmp(host, li->host))
       return (li);
  }
  if (def_li)
     return (def_li);
  return (NULL);
}

/*
 * Parse a line from '~/.netrc'. Match lines like:
 *   machine <host> login <user> password <password>
 * Or
 *   default login <user> password <password>
 *
 * And add to the 'login_list' smartlist.
 */
static void netrc_parse (smartlist_t *sl, const char *line)
{
  struct login_info *li = NULL;
  char   host [256];
  char   user [50];
  char   passw[50];
  const char *fmt1 = "machine %256s login %50s password %50s";
  const char *fmt2 = "default login %50s password %50s";

  if (sscanf(line, fmt1, host, user, passw) == 3)
  {
    li = CALLOC (1, sizeof(*li));
    li->host     = STRDUP (host);
    li->user     = STRDUP (user);
    li->passw    = STRDUP (passw);
    li->is_netrc = TRUE;
  }
  else if (sscanf(line, fmt2, user, passw) == 2)
  {
    li = CALLOC (1, sizeof(*li));
    li->user       = STRDUP (user);
    li->passw      = STRDUP (passw);
    li->is_default = TRUE;
    li->is_netrc   = TRUE;
  }
  if (li)
     smartlist_add (sl, li);
}

/*
 * Parse a line from '~/.authinfo'. Match lines like:
 *   machine <host> port <num> login <user> password <password>
 * Or
 *   default port <num> login <user> password <password>
 *
 * And add to the 'login_list' smartlist.
 */
static void authinfo_parse (smartlist_t *sl, const char *line)
{
  struct login_info *li = NULL;
  char   host [256];
  char   user [50];
  char   passw[50];
  int    port = 0;
  const char *fmt1 = "machine %256s port %d login %50s password %50s";
  const char *fmt2 = "default port %d login %50s password %50s";

  if (sscanf(line, fmt1, host, &port, user, passw) == 4 && port > 0 && port < USHRT_MAX)
  {
    li = CALLOC (1, sizeof(*li));
    li->host     = STRDUP (host);
    li->user     = STRDUP (user);
    li->passw    = STRDUP (passw);
    li->port     = port;
    li->is_netrc = FALSE;
  }
  else if (sscanf(line, fmt2, &port, user, passw) == 3 && port > 0 && port < USHRT_MAX)
  {
    li = CALLOC (1, sizeof(*li));
    li->user       = STRDUP (user);
    li->passw      = STRDUP (passw);
    li->port       = port;
    li->is_default = TRUE;
    li->is_netrc   = FALSE;
  }
  if (li)
     smartlist_add (sl, li);
}

int netrc_init (void)
{
  return common_init ("%APPDATA%\\.netrc", netrc_parse);
}

int authinfo_init (void)
{
  return common_init ("%APPDATA%\\.authinfo", authinfo_parse);
}

void netrc_exit (void)
{
  common_exit (TRUE);
}

void authinfo_exit (void)
{
  common_exit (FALSE);
}

/*
 * Use this externally.
 * 'netrc_lookup (NULL, NULL, NULL)' can be used for test/debug.
 */
int netrc_lookup (const char *host, const char **user, const char **passw)
{
  const struct login_info *li = common_lookup (host, TRUE);

  if (!li)
     return (0);

  if (user)
     *user = li->user;
  if (passw)
     *passw = li->passw;
  return (1);
}

/*
 * Use this externally.
 * 'authinfo_lookup (NULL, NULL, NULL, NULL)' can be used for test/debug.
 */
int authinfo_lookup (const char *host, const char **user, const char **passw, int *port)
{
  const struct login_info *li = common_lookup (host, FALSE);

  if (!li)
     return (0);

  if (user)
     *user = li->user;
  if (passw)
     *passw = li->passw;
  if (port)
     *port = li->port;
  return (1);
}

