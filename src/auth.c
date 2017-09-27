/*
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

struct netrc_info {
       BOOL  is_default;
       char *host;
       char *user;
       char *passw;
     };

struct auth_info {
       BOOL  is_default;
       char *host;
       char *user;
       char *passw;
       int   port;
     };

static smartlist_t *netrc, *authinfo;

/*
 * Parse a line from '~/.netrc'. Match lines like:
 *   machine <host> login <user> password <password>
 * Or
 *   default login <user> password <password>
 *
 * And add to the given smartlist.
 */
static void netrc_parse (smartlist_t *sl, const char *line)
{
  struct netrc_info *ni;
  char   host [256];
  char   user [50];
  char   passw[50];
  const char *fmt1 = "machine %256s login %50s password %50s";
  const char *fmt2 = "default login %50s password %50s";

  if (sscanf(line, fmt1, host, user, passw) == 3)
  {
    ni = CALLOC (1, sizeof(*ni));
    ni->host  = STRDUP (host);
    ni->user  = STRDUP (user);
    ni->passw = STRDUP (passw);
    smartlist_add (sl, ni);
  }
  else if (sscanf(line, fmt2, user, passw) == 2)
  {
    ni = CALLOC (1, sizeof(*ni));
    ni->user       = STRDUP (user);
    ni->passw      = STRDUP (passw);
    ni->is_default = TRUE;
    smartlist_add (sl, ni);
  }
}

int netrc_init (void)
{
  const char *fname = "%APPDATA%\\.netrc";
  char       *file = getenv_expand (fname);

  netrc = file ? smartlist_read_file (file, netrc_parse) : NULL;
  if (!netrc)
     WARN ("Failed to open \"%s\". Authenticated logins will not work.\n", fname);
  FREE (file);
  return (netrc != NULL);
}

/*
 * Free the memory allocated in the 'netrc' smartlist.
 */
void netrc_exit (void)
{
  int i, max;

  if (!netrc)
     return;

  max = smartlist_len (netrc);
  for (i = 0; i < max; i++)
  {
    struct netrc_info *ni = smartlist_get (netrc, i);

    FREE (ni->host);
    FREE (ni->user);
    FREE (ni->passw);
    FREE (ni);
  }
  smartlist_free (netrc);
}

/*
 * Lookup the user/password for a 'host' in the 'netrc' smartlist.
 */
static const struct netrc_info *_netrc_lookup (const char *host)
{
  const struct netrc_info *def_ni = NULL;
  int   i, max;

  if (!netrc)
     return (NULL);

  max = smartlist_len (netrc);
  for (i = 0; i < max; i++)
  {
    const struct netrc_info *ni = smartlist_get (netrc, i);
    char  buf [300];

    if (ni->is_default)
       def_ni = ni;

    snprintf (buf, sizeof(buf), "host: '%s', user: '%s', passw: '%s', is_default: %d.\n",
              ni->host, ni->user, ni->passw, ni->is_default);

    if (opt.do_tests)
    {
      C_setraw (1);
      C_printf ("  %s", buf);
      C_setraw (0);
    }
    else
      DEBUGF (3, buf);

    if (ni->host && host && !stricmp(host, ni->host))
       return (ni);
  }
  if (def_ni)
     return (def_ni);
  return (NULL);
}

/*
 * Use this externally.
 * 'netrc_lookup (NULL, NULL, NULL)' can be used for test/debug.
 */
int netrc_lookup (const char *host, const char **user, const char **passw)
{
  const struct netrc_info *ni = _netrc_lookup (host);

  if (ni)
  {
    if (user)
       *user  = ni->user;
    if (passw)
       *passw = ni->passw;
  }
  return (ni != NULL);
}

/*
 * Parse a line from '~/.authinfo'. Match lines like:
 *   machine <host> port <num> login <user> password <password>
 *
 * And add to the given smartlist.
 */
static void authinfo_parse (smartlist_t *sl, const char *line)
{
  struct auth_info *ai;
  char   host [256];
  char   user [50];
  char   passw[50];
  int    port;
  const char *fmt1 = "machine %256s port %d login %50s password %50s";
  const char *fmt2 = "default port %d login %50s password %50s";

  if (sscanf(line, fmt1, host, &port, user, passw) == 4 && port > 0 && port < USHRT_MAX)
  {
    ai = CALLOC (1, sizeof(*ai));
    ai->host  = STRDUP (host);
    ai->user  = STRDUP (user);
    ai->passw = STRDUP (passw);
    ai->port  = port;
    smartlist_add (sl, ai);
  }
  else if (sscanf(line, fmt2, &port, user, passw) == 3)
  {
    ai = CALLOC (1, sizeof(*ai));
    ai->user       = STRDUP (user);
    ai->passw      = STRDUP (passw);
    ai->port       = port;
    ai->is_default = TRUE;
    smartlist_add (sl, ai);
  }
}

static int authinfo_init (void)
{
  const char *fname = "%APPDATA%\\.authinfo";
  char       *file = getenv_expand (fname);

  authinfo = file ? smartlist_read_file (file, authinfo_parse) : NULL;
  if (!authinfo)
     WARN ("Failed to open \"%s\". Authenticated logins will not work.\n", fname);
  FREE (file);
  return (authinfo != NULL);
}

/*
 * Free the memory allocated in the 'authinfo' smartlist.
 */
void authinfo_exit (void)
{
  int i, max;

  if (!authinfo)
     return;

  max = smartlist_len (authinfo);
  for (i = 0; i < max; i++)
  {
    struct auth_info *ai = smartlist_get (authinfo, i);

    FREE (ai->host);
    FREE (ai->user);
    FREE (ai->passw);
    FREE (ai);
  }
  smartlist_free (authinfo);
}

/*
 * Lookup the user/password/port for a 'host' in the 'authinfo' smartlist.
 */
static const struct auth_info *_authinfo_lookup (const char *host)
{
  const struct auth_info *def_ai = NULL;
  int   i, max;

  if (!authinfo)
     return (NULL);

  max = smartlist_len (authinfo);
  for (i = 0; i < max; i++)
  {
    const struct auth_info *ai = smartlist_get (authinfo, i);
    char  buf [300];

    if (ai->is_default)
       def_ai = ai;

    snprintf (buf, sizeof(buf), "host: '%s', user: '%s', passw: '%s', port: %d\n",
              ai->host, ai->user, ai->passw, ai->port);

    if (opt.do_tests)
    {
      C_setraw (1);
      C_printf ("  %s", buf);
      C_setraw (0);
    }
    else
      DEBUGF (3, buf);

    if (ai->host && host && !stricmp(host, ai->host))
       return (ai);
  }
  if (def_ai)
     return (def_ai);
  return (NULL);
}

/*
 * Use this externally.
 * 'authinfo_lookup (NULL, NULL, NULL, NULL)' can be used for test/debug.
 */
int authinfo_lookup (const char *host, const char **user, const char **passw, int *port)
{
  const struct auth_info *ai = _authinfo_lookup (host);

  if (ai)
  {
    if (user)
       *user  = ai->user;
    if (passw)
       *passw = ai->passw;
    if (port)
       *port = ai->port;
  }
  return (ai != NULL);
}

