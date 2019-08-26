/**\file    auth.c
 * \ingroup Authentication
 *
 * Used to login to a remote EveryThing FTP-server before doing queries.
 *
 * The syntax `"~/xx"` (meaning file `"xx"` in the user's home directory) really
 * means `"%APPDATA%\\xx"`.
 *
 * This file is part of envtool.
 *
 * By Gisle Vanem <gvanem@yahoo.no> August 2017.
 */
#include "color.h"
#include "envtool.h"
#include "smartlist.h"
#include "auth.h"

/**\typedef login_source
 * The source of the login information_:
 * either `~/.netrc`, `~/.authinfo` or `~/envtool.cfg`.
 */
typedef enum login_source {
        LOGIN_NETRC = 1,
        LOGIN_AUTHINFO,
        LOGIN_ENVTOOL_CFG,
      } login_source;

/**\struct login_info
 * Data for each parsed entry from one of the files in `enum login_source`.
 */
struct login_info {
       BOOL         is_default;  /**< This is the `default` user/password entry for non-matching lookups */
       login_source src;         /**< Which file this entry came from */
       char        *host;        /**< The hostname of the entry */
       char        *user;        /**< The username of the entry */
       char        *passw;       /**< The password of the entry */
       int          port;        /**< The network port of the entry. Only if from `~/.authinfo` or `~/envtool.cfg` */
     };

/** The smartlist of "struct login_info" entries.
 */
static smartlist_t *login_list = NULL;

/**
 * Return the name of the login source.
 */
static const char *login_src_name (enum login_source src)
{
  return ((src == LOGIN_NETRC)       ? "NETRC"       :
          (src == LOGIN_AUTHINFO)    ? "AUTHINFO"    :
          (src == LOGIN_ENVTOOL_CFG) ? "ENVTOOL_CFG" : "?");
}

/**
 * Common to both netrc_init() and authinfo_init().
 * The \ref login_list is grown in the below `parser` functions.
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

/**
 * Free the memory allocated in the \ref login_list smartlist.<br>
 * But only if `li->src` matches `src`.
 */
static void common_exit (enum login_source src)
{
  int i, max;

  if (!login_list)
  {
    DEBUGF (2, "%s: length of list now: %s\n", login_src_name(src), "<N/A>");
    return;
  }

  max = smartlist_len (login_list);
  DEBUGF (2, "%s: length of list now: %d\n", login_src_name(src), max);

  for (i = 0; i < max; i++)
  {
    struct login_info *li = smartlist_get (login_list, i);

    DEBUGF (2, "i: %d, %-12s, do %sdelete this.\n",
            i,  login_src_name(li->src), (li->src == src) ? "" : "not ");

    if (!li || li->src != src)
       continue;

    if (li->src != LOGIN_ENVTOOL_CFG)
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

/**
 * Search the `login_list` smartlist for `host`. <br>
 * The `li->src` must also match `src`.
 */
static const struct login_info *common_lookup (const char *host, enum login_source src)
{
  const struct login_info *def_li = NULL;
  int   i, save, max = login_list ? smartlist_len (login_list) : 0;

  for (i = 0; i < max; i++)
  {
    const struct login_info *li = smartlist_get (login_list, i);
    char  buf [300];

    if (li->src != src)
       continue;

    if (li->is_default)
       def_li = li;

    snprintf (buf, sizeof(buf), "%-12s host: '%s', user: '%s', passw: '%s', port: %d\n",
              login_src_name(li->src), li->is_default ? "*default*" : li->host,
              li->user, li->passw, li->port);

    if (opt.do_tests)
    {
      save = C_setraw (1);
      C_printf ("  %s", buf);
      C_setraw (save);
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

/**
 * Parse a line from `"~/.netrc"`. Match lines like:
 *   \code machine <host> login <user> password <password> \endcode
 * Or
 *   \code default login <user> password <password> \endcode
 *
 * And add to the \ref login_list smartlist.
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
    li->src      = LOGIN_NETRC;
  }
  else if (sscanf(line, fmt2, user, passw) == 2)
  {
    li = CALLOC (1, sizeof(*li));
    li->user       = STRDUP (user);
    li->passw      = STRDUP (passw);
    li->is_default = TRUE;
    li->src        = LOGIN_NETRC;
  }
  if (li)
     smartlist_add (sl, li);
}

/**
 * Parse a line from `"~/.authinfo"`. Match lines like: <br>
 *   `machine <host> port <num> login <user> password <password>`
 * Or <br>
 *   `default port <num> login <user> password <password>`
 *
 * And add to the `login_list` smartlist.
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
    li->src      = LOGIN_AUTHINFO;
  }
  else if (sscanf(line, fmt2, &port, user, passw) == 3 && port > 0 && port < USHRT_MAX)
  {
    li = CALLOC (1, sizeof(*li));
    li->user       = STRDUP (user);
    li->passw      = STRDUP (passw);
    li->port       = port;
    li->is_default = TRUE;
    li->src        = LOGIN_AUTHINFO;
  }
  if (li)
     smartlist_add (sl, li);
}

/**
 * Parse a line from `"~/envtool.cfg"`. Match lines like:
 *   ```
 *    [Login]
 *    <host> = <user> / <password>
 *    <host> = <user> / <password> / port <port>
 *   ```
 * And add to the `login_list` smartlist.
 */
void auth_envtool_handler (const char *section, const char *key, const char *value)
{
  char user [256];
  char passw[256];
  int  port = 0;
  int  num = sscanf (value, "%255[^ /] / %255[^ /] / port %d", user, passw, &port);

  if (num >= 2)
  {
    struct login_info *li = CALLOC (1, sizeof(*li));

    if (!login_list)
       login_list = smartlist_new();

    li->host     = (char*) key;
    li->user     = STRDUP (user);
    li->passw    = STRDUP (passw);
    li->port     = port;
    li->src      = LOGIN_ENVTOOL_CFG;
    DEBUGF (2, "num: %d, host: '%s', user: '%s', passwd: '%s', port: %d.\n", num, li->host, li->user, li->passw, li->port);
    smartlist_add (login_list, li);
  }
  ARGSUSED (section);
}

/**
 * Open and parse the `"%APPDATA%\\.netrc"` file only once.
 */
static int netrc_init (void)
{
  static int last_rc = -1;

  if (last_rc == -1)
     last_rc = common_init ("%APPDATA%\\.netrc", netrc_parse);
  return (last_rc);
}

/**
 * Open and parse the `"%APPDATA%\\.authinfo"` file only once.
 */
static int authinfo_init (void)
{
  static int last_rc = -1;

  if (last_rc == -1)
     last_rc = common_init ("%APPDATA%\\.authinfo", authinfo_parse);
  return (last_rc);
}

/**
 * Free the login_list entries assosiated with the `"%APPDATA%\\.netrc"` file.
 */
void netrc_exit (void)
{
  common_exit (LOGIN_NETRC);
}

/**
 * Free the login_list entries assosiated with the `"%APPDATA%\\.authinfo"` file.
 */
void authinfo_exit (void)
{
  common_exit (LOGIN_AUTHINFO);
}

/**
 * Free the login_list entries assosiated with the `"%APPDATA%\\envtool.cfg"` file.
 */
void envtool_cfg_exit (void)
{
  common_exit (LOGIN_ENVTOOL_CFG);
}

static int return_login (const struct login_info *li, const char **user, const char **passw, int *port)
{
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

/**
 * Use this externally like:
 * `netrc_lookup (NULL, NULL, NULL)` can be used for test/debug.
 */
int netrc_lookup (const char *host, const char **user, const char **passw)
{
  const struct login_info *li;

  if (!netrc_init())
     return (0);

  li = common_lookup (host, LOGIN_NETRC);
  return return_login (li, user, passw, NULL);
}

/**
 * Use this externally like:
 * `authinfo_lookup (NULL, NULL, NULL, NULL)` can be used for test/debug.
 */
int authinfo_lookup (const char *host, const char **user, const char **passw, int *port)
{
  const struct login_info *li;

  if (!authinfo_init())
     return (0);

  li = common_lookup (host, LOGIN_AUTHINFO);
  return return_login (li, user, passw, port);
}

/**
 * Use this externally like:
 * `envtool_cfg_lookup (NULL, NULL, NULL, NULL)` can be used for test/debug.
 */
int envtool_cfg_lookup (const char *host, const char **user, const char **passw, int *port)
{
  const struct login_info *li;

  li = common_lookup (host, LOGIN_ENVTOOL_CFG);

  /* Since a `~/envtool.cfg` does not have a default login entry,
   */
  if (!li && !host)
     return (1);
  return return_login (li, user, passw, port);
}

