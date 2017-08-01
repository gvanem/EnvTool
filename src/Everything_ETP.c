/*
 * Functions for remote queries using EveryThing's ETP protocol.
 * Ref:
 *   http://www.voidtools.com/support/everything/etp/
 *   https://www.voidtools.com/forum/viewtopic.php?t=1790
 *
 * This file is part of envtool
 *
 * By Gisle Vanem <gvanem@yahoo.no> August 2017.
 */

/* Suppress warning for 'inet_addr()' and 'gethostbyname()'
 */
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <windows.h>

#include "color.h"
#include "envtool.h"
#include "smartlist.h"
#include "Everything_ETP.h"

#ifndef RECV_TIMEOUT
#define RECV_TIMEOUT 2000   /* 2 sec */
#endif

/* Forward definition.
 */
struct state_CTX;

/* All functions for ETP transfers must match this function-type.
 */
typedef BOOL (*ETP_state) (struct state_CTX *ctx);

/* The context used throughout the ETP transfer.
 * Keeping this together should make the ETP transfer
 * fully reentrant.
 */
struct state_CTX {
       ETP_state          state;
       WSADATA            wsadata;
       struct sockaddr_in sa;
       SOCKET             sock;
       int                port;
       char              *raw_url;
       char               hostname [200];
       char               username [30];
       char               password [30];
       DWORD              timeout;
       char               rx_buf [500];
       char              *rx_ptr;
       char               tx_buf [500];
       char               trace_buf [1000];
       char              *trace_ptr;
       size_t             trace_left;
       unsigned           results_expected;
       unsigned           results_got;
       unsigned           results_ignore;

       /* These are set in state_PATH()
        */
       time_t             mtime;
       UINT64             fsize;
       char               path [_MAX_PATH];
     };

static int    parse_host_spec     (struct state_CTX *ctx, const char *pattern, ...);
static time_t FILETIME_to_time_t  (const FILETIME *ft);
static const char *ETP_tracef     (struct state_CTX *ctx, const char *fmt, ...);
static const char *ETP_state_name (ETP_state f);

static BOOL state_init         (struct state_CTX *ctx);
static BOOL state_exit         (struct state_CTX *ctx);
static BOOL state_parse_url    (struct state_CTX *ctx);
static BOOL state_netrc_lookup (struct state_CTX *ctx);
static BOOL state_send_login   (struct state_CTX *ctx);
static BOOL state_send_pass    (struct state_CTX *ctx);
static BOOL state_await_login  (struct state_CTX *ctx);
static BOOL state_send_query   (struct state_CTX *ctx);
static BOOL state_200          (struct state_CTX *ctx);
static BOOL state_RESULT_COUNT (struct state_CTX *ctx);
static BOOL state_PATH         (struct state_CTX *ctx);
static BOOL state_closing      (struct state_CTX *ctx);
static BOOL state_resolve      (struct state_CTX *ctx);
static BOOL state_connect      (struct state_CTX *ctx);

struct netrc_info {
       BOOL  is_default;
       char *host;
       char *user;
       char *passw;
     };

static smartlist_t *netrc;

/*
 * Parse a line from .netrc. Match lines like:
 *   machine <host> login <user> password <password>
 * Or
 *   default login <user> password <password>
 *
 * And add to the given smartlist.
 */
static void parse_netrc (smartlist_t *sl, const char *line)
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
  char *file = getenv_expand ("%APPDATA%\\.netrc");

  netrc = smartlist_read_file (file, parse_netrc);
  if (!netrc)
     WARN ("Failed to open \"%s\". Authenticated logins will not work.\n", file);
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
         C_printf ("  %s", buf);
    else DEBUGF (3, buf);

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
 * Do a remote EveryThing search:
 *   Connect to a remote ETP-server, login, send the SEARCH and parameters.
 *
 * The protocol goes something like this:
 *    C -> USER anonymous (or <none>)
 *    C -> EVERYTHING SEARCH <spec>
 *    C -> EVERYTHING PATH_COLUMN 1
 *    C -> EVERYTHING SIZE_COLUMN 1
 *    C -> EVERYTHING DATE_MODIFIED_COLUMN 1
 *    C -> EVERYTHING QUERY
 *    ...
 *    S ->  200-Query results
 *    S ->  RESULT_COUNT 3
 *    ...
 *    S -> 200 End.
 *
 * Which can be accomplished with a .bat-file and a plain ftp client:
 *
 *  @echo off
 *  echo USER                                     > etp-commands
 *  echo QUOTE EVERYTHING SEARCH notepad.exe     >> etp-commands
 *  echo QUOTE EVERYTHING PATH_COLUMN 1          >> etp-commands
 *  echo QUOTE EVERYTHING SIZE_COLUMN 1          >> etp-commands
 *  echo QUOTE EVERYTHING DATE_MODIFIED_COLUMN 1 >> etp-commands
 *  echo QUOTE EVERYTHING QUERY                  >> etp-commands
 *  echo BYE                                     >> etp-commands
 *
 *  c:\> ftp -s:etp-commands 10.0.0.37
 *
 *  Connected to 10.0.0.37.
 *  220 Welcome to Everything ETP/FTP
 *  530 Not logged on.
 *  User (10.0.0.37:(none)):
 *  230 Logged on.
 *  ftp> QUOTE EVERYTHING SEARCH notepad.exe
 *  200 Search set to (notepad.exe).
 *  ftp> QUOTE EVERYTHING PATH_COLUMN 1
 *  200 Path column set to (1).
 *  ftp> QUOTE EVERYTHING QUERY
 *  200-Query results
 *   RESULT_COUNT 3
 *   PATH C:\Windows
 *   SIZE 236032
 *   DATE_MODIFIED 131343347638616569
 *   FILE notepad.exe
 *   PATH C:\Windows\System32
 *   SIZE 236032
 *   DATE_MODIFIED 131343347658304156
 *   FILE notepad.exe
 *   PATH C:\Windows\WinSxS\x86_microsoft-windows-notepad_31bf3856ad364e35_10.0.15063.0_none_240fcb30f07103a5
 *   SIZE 236032
 *   DATE_MODIFIED 131343347658304156
 *   FILE notepad.exe
 *  200 End.
 *  ftp> BYE
 *  221 Goodbye.
 */

/*
 * Receive a response.
 * Call recv() for 1 character at a time!
 * This is to keep it simple; avoid the need for a non-blocking socket and select().
 * But the recv() will hang if used against a non "line oriented" protocol.
 * But only for max 2 seconds since 'setsockopt()' with SO_RCVTIMEO was set to 2 sec.
 */
static char *recv_line (struct state_CTX *ctx)
{
  int len = 0;

  ctx->rx_ptr = ctx->rx_buf;

  while (ctx->rx_ptr < ctx->rx_buf + sizeof(ctx->rx_buf)-1)
  {
    if (recv(ctx->sock, ctx->rx_ptr, 1, 0) != 1)
       break;
    len++;
    if (*ctx->rx_ptr == '\n')   /* Assumes '\r' already was received */
       break;
    ctx->rx_ptr++;
  }
  *ctx->rx_ptr = '\0';
  strip_nl (ctx->rx_buf);
  ctx->rx_ptr = str_ltrim (ctx->rx_buf);
  ETP_tracef (ctx, "Rx: \"%s\", len: %d\n", ctx->rx_ptr, len);
  return (ctx->rx_ptr);
}

/*
 * Send a single-line command to the server side.
 * Do not use a "\r\n" termination; it will be added here.
 */
static int send_cmd (struct state_CTX *ctx, const char *fmt, ...)
{
  va_list args;
  int     len, rc;

  va_start (args, fmt);

  len = vsnprintf (ctx->tx_buf, sizeof(ctx->tx_buf)-3, fmt, args);
  ctx->tx_buf[len] = '\r';
  ctx->tx_buf[len+1] = '\n';

  rc = send (ctx->sock, ctx->tx_buf, len+2, 0);
  ETP_tracef (ctx, "Tx: \"%.*s\\r\\n\", len: %d\n", len, ctx->tx_buf, rc);
  va_end (args);
  return (rc < 0 ? -1 : 0);
}

/*
 * Print the resulting match:
 *  'name' is either a file-name within a 'ctx->path' (is_dir=FALSE).
 *  Or a folder-name within a 'ctx->path'             (is_dir=TRUE).
 */
static BOOL report_file_ept (struct state_CTX *ctx, const char *name, BOOL is_dir)
{
  if (opt.dir_mode && !is_dir)
     ctx->results_ignore++;
  else
  {
    char fullname [_MAX_PATH];

    snprintf (fullname, sizeof(fullname), "%s\\%s", ctx->path, name);
    report_file (fullname, ctx->mtime, ctx->fsize, is_dir, FALSE, HKEY_EVERYTHING_ETP);
  }
  ctx->mtime = 0;
  ctx->fsize = 0;
  ctx->results_got++;
  return (TRUE);
}

/*
 * PATH state: gooble up the results.
 * Expect 'results_expected' results set in 'RESULT_COUNT_state()'.
 * When "200 End" is received, enter 'state_closing'.
 */
static BOOL state_PATH (struct state_CTX *ctx)
{
  FILETIME ft;

  recv_line (ctx);

  if (!strncmp(ctx->rx_ptr, "PATH ", 5))
  {
    _strlcpy (ctx->path, ctx->rx_ptr+5, sizeof(ctx->path));
    ETP_tracef (ctx, "path: %s", ctx->path);
    return (TRUE);
  }

  if (sscanf(ctx->rx_ptr, "SIZE %" U64_FMT, &ctx->fsize) == 1)
  {
    ETP_tracef (ctx, "size: %s", str_trim((char*)get_file_size_str(ctx->fsize)));
    return (TRUE);
  }

  if (sscanf(ctx->rx_ptr, "DATE_MODIFIED %" U64_FMT, (UINT64*)&ft) == 1)
  {
    ctx->mtime = FILETIME_to_time_t (&ft);
    ETP_tracef (ctx, "mtime: %.24s", ctime(&ctx->mtime));
    return (TRUE);
  }

  if (!strncmp(ctx->rx_ptr, "FILE ", 5))
  {
    const char *file = ctx->rx_ptr+5;

    ETP_tracef (ctx, "file: %s", file);
    return report_file_ept (ctx, file, FALSE);
  }

  if (!strncmp(ctx->rx_ptr, "FOLDER ", 7))
  {
    const char *folder = ctx->rx_ptr+7;

    ETP_tracef (ctx, "folder: %s", folder);
    return report_file_ept (ctx, folder, TRUE);
  }

  if (strncmp(ctx->rx_ptr,"200 End",7))
     WARN ("Unexpected response: \"%s\"\n", ctx->rx_buf);

  ctx->state = state_closing;
  return (TRUE);
}

/*
 * RESULT_COUNT state: get the "RESULT_COUNT n\r\n" string.
 * Goto 'state_PATH()'.
 */
static BOOL state_RESULT_COUNT (struct state_CTX *ctx)
{
  recv_line (ctx);

  if (sscanf(ctx->rx_ptr,"RESULT_COUNT %u", &ctx->results_expected) == 1)
  {
    ctx->state = state_PATH;
    return (TRUE);
  }
  if (!strncmp(ctx->rx_ptr,"200 End",7))  /* Premature "200 End". No results? */
  {
    ctx->state = state_closing;
    return (TRUE);
  }
  WARN ("Unexpected response: \"%s\"\n", ctx->rx_buf);
  ctx->state = state_closing;
  return (TRUE);
}

/*
 * State entered after commands was sent:
 *   Swallow received lines until "200-xx\r\n" is received.
 *   Then enter 'state_RESULT_COUNT()'.
 */
static BOOL state_200 (struct state_CTX *ctx)
{
  recv_line (ctx);
  if (!strncmp(ctx->rx_ptr,"200-",4))
     ctx->state = state_RESULT_COUNT;
  else if (*ctx->rx_ptr != '2')
  {
    WARN ("This is not an ETP server; response was: \"%s\"\n", ctx->rx_buf);
    ctx->state = state_closing;
  }
  return (TRUE);
}

/*
 * Close the socket and goto 'state_exit'.
 */
static BOOL state_closing (struct state_CTX *ctx)
{
  ETP_tracef (ctx, "closesocket()");
  if (ctx->sock != INVALID_SOCKET)
     closesocket (ctx->sock);
  ctx->sock = INVALID_SOCKET;

  if (ctx->results_expected > 0 && ctx->results_got < ctx->results_expected)
     WARN ("Expected %u results, but received only %u.\n", ctx->results_expected, ctx->results_got);

  ctx->state = state_exit;
  return (TRUE);
}

/*
 * Send the search parameters and the QUERY command.
 * If the 'send()' call fail, enter 'state_closing'.
 * Otherwise, enter 'state_200'.
 */
static BOOL state_send_query (struct state_CTX *ctx)
{
  /* Always 'REGEX 1', but translate from a shell-pattern if
   * 'opt.use_regex == 0'.
   */
  send_cmd (ctx, "EVERYTHING REGEX 1");
  send_cmd (ctx, "EVERYTHING CASE %d", opt.case_sensitive);

  if (opt.use_regex)
       send_cmd (ctx, "EVERYTHING SEARCH %s", opt.file_spec);
  else send_cmd (ctx, "EVERYTHING SEARCH ^%s$", translate_shell_pattern(opt.file_spec));

  send_cmd (ctx, "EVERYTHING PATH_COLUMN 1");
  send_cmd (ctx, "EVERYTHING SIZE_COLUMN 1");
  send_cmd (ctx, "EVERYTHING DATE_MODIFIED_COLUMN 1");

  if (send_cmd(ctx, "EVERYTHING QUERY") < 0)
       ctx->state = state_closing;
  else ctx->state = state_200;
  return (TRUE);
}

/*
 * Send the USER name and optionally the 'ctx->password'.
 * "USER" can be empty if the Everything.ini has a setting like:
 *   etp_server_username=
 *
 * If the .ini-file contains:
 *   etp_server_username=foo
 *   etp_server_password=bar
 * 'ctx->username' and 'ctx->password' must match 'foo/bar'.
 */
static BOOL state_send_login (struct state_CTX *ctx)
{
  int rc;

  if (ctx->username[0] && ctx->password[0])
  {
    rc = send_cmd (ctx, "USER %s", ctx->username);
    ctx->state = state_send_pass;
  }
  else
  {
    rc = send_cmd (ctx, "USER");
    ctx->state = state_await_login;
  }

  /* Ignore the "220 Welcome to Everything..." message.
   */
  recv_line (ctx);

  if (rc < 0)   /* Tx failed! */
     ctx->state = state_closing;
  return (TRUE);
}

/*
 * We sent the USER/PASS commands.
 * Await the "230 Logged on" message and enter 'state_send_query'.
 * If server replies with a "530" message ("Login or password incorrect"),
 * enter 'state_closing'.
 */
static BOOL state_await_login (struct state_CTX *ctx)
{
  recv_line (ctx);

  /* 230: Server accepted our login.
   */
  if (!strncmp(ctx->rx_ptr,"230",3))
     ctx->state = state_send_query;
  else
  {
    /* Any 5xx message is fatal here; close.
     */
    char buf[100];

    snprintf (buf, sizeof(buf), "Failed to login; USER %s.\n", ctx->username);
    WARN (buf);
    ETP_tracef (ctx, buf);
    ctx->state = state_closing;
  }
  return (TRUE);
}

/*
 * We're prepared to send a password. But if server replies with
 * "230 Logged on", we know it ignores passwords (dangerous!).
 * So proceed to 'state_send_query' state.
 */
static BOOL state_send_pass (struct state_CTX *ctx)
{
  recv_line (ctx);

  if (!strcmp(ctx->rx_ptr,"230 Logged on."))
       ctx->state = state_send_query;   /* ETP server ignores passwords */
  else if (send_cmd(ctx, "PASS %s", ctx->password) < 0)
       ctx->state = state_closing;      /* Transmit failed; close */
  else ctx->state = state_await_login;  /* "PASS" sent okay, await loging confirmation */
  return (TRUE);
}

/*
 * Check if 'ctx->hostname' is simply an IPv4-address.
 * If TRUE
 *   enter 'state_connect'.
 * else
 *   call 'gethostbyname()' to get the IPv4-address.
 *   Then enter 'state_connect'.
 */
static BOOL state_resolve (struct state_CTX *ctx)
{
  struct hostent *he;

  ETP_tracef (ctx, "ctx->hostname: '%s'\n", ctx->hostname);
  ETP_tracef (ctx, "ctx->username: '%s'\n", ctx->username);
  ETP_tracef (ctx, "ctx->password: '%s'\n", ctx->password);
  ETP_tracef (ctx, "ctx->port:      %u\n",  ctx->port);

  /* An 'inet_addr ("")' will on Winsock return our own IP-address!
   * Avoid that.
   */
  if (!ctx->hostname[0])
  {
    WARN ("Empty hostname!\n");
    goto fail;
  }

  /* If 'ctx->hostname' is not simply an IPv4-address, it
   * must be resolved to an IPv4-address first.
   */
  ctx->sa.sin_addr.s_addr = inet_addr (ctx->hostname);
  if (ctx->sa.sin_addr.s_addr == INADDR_NONE)
  {
    if (!opt.quiet)
        C_printf ("Resolving %s...", ctx->hostname);
    C_flush();
    he = gethostbyname (ctx->hostname);

    if (!he)
    {
      WARN (" Unknown host.\n");
      goto fail;
    }
    ctx->sa.sin_addr.s_addr = *(u_long*) he->h_addr_list[0];
    C_putc ('\r');
  }

  ctx->state = state_connect;
  return (TRUE);

fail:
  ctx->state = state_exit;
  return (TRUE);
}

/*
 * When the IPv4-address is known, create the socket and perform the connect().
 * If successful, enter 'state_send_login'.
 */
static BOOL state_connect (struct state_CTX *ctx)
{
  ctx->sock = socket (AF_INET, SOCK_STREAM, 0);
  if (ctx->sock == INVALID_SOCKET)
  {
    WARN ("Failed to create socket, err: %d.\n", WSAGetLastError());
    ETP_tracef (ctx, "Failed to create socket, err: %d.\n", WSAGetLastError());
    ctx->state = state_closing;
    return (TRUE);
  }

  ctx->sa.sin_family = AF_INET;
  ctx->sa.sin_port   = htons (ctx->port);
  setsockopt (ctx->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ctx->timeout, sizeof(ctx->timeout));

  if (!opt.quiet)
     C_printf ("Connecting to %s (port %u)...", inet_ntoa(ctx->sa.sin_addr), ctx->port);

  C_flush();

  if (connect(ctx->sock, (const struct sockaddr*)&ctx->sa, sizeof(ctx->sa)) < 0)
  {
    WARN (" Failed to connect, err: %d.\n", WSAGetLastError());
    ETP_tracef (ctx, "Failed to connect, err: %d.\n", WSAGetLastError());
    ctx->state = state_closing;
  }
  else
  {
    if (!opt.quiet)
       C_putc ('\n');
    ctx->state = state_send_login;
  }
  return (TRUE);
}

/*
 * Lookup the USER/PASSWORD for 'ctx->hostname' in the .netrc file.
 * Enter 'state_resolve' even if those are not found.
 */
static BOOL state_netrc_lookup (struct state_CTX *ctx)
{
  const char *user  = NULL;
  const char *passw = NULL;

  ETP_tracef (ctx, "Gettting USER/PASS in '%%APPDATA%%\\.netrc' for '%s'\n", ctx->hostname);

  if (netrc_init() && !netrc_lookup(ctx->hostname, &user, &passw))
     WARN ("No user/password found for host \"%s\".\n", ctx->hostname);

  if (user)
     _strlcpy (ctx->username, user, sizeof(ctx->username));

  if (passw)
     _strlcpy (ctx->password, passw, sizeof(ctx->password));

  netrc_exit();
  ctx->state = state_resolve;
  return (TRUE);
}

/*
 * Check if 'ctx->raw_url' matches one of these formats:
 *   "user:passwd@host_or_IP-address<:port>".    Both user-name and password.
 *   "user@host_or_IP-address<:port>".           Only user-name.
 *   "host_or_IP-address<:port>".                Only host/IP-address (+ port).
 */
static BOOL state_parse_url (struct state_CTX *ctx)
{
  BOOL use_netrc = TRUE;
  int  n;

  ETP_tracef (ctx, "Cracking the host-spec: '%s'.\n", ctx->raw_url);

  /* Check simple case of "host_or_IP-address<:port>" first.
   */
  n = parse_host_spec (ctx, "%200[^:]:%d", ctx->hostname, &ctx->port);

  if ((n == 1 || n == 2) && !strchr(ctx->raw_url,'@'))
     use_netrc = TRUE;
  else
  {
    /* Check for "user:passwd@host_or_IP-address<:port>".
     */
    n = parse_host_spec (ctx, "%30[^:@]:%30[^:@]@%200[^:]:%d", ctx->username, ctx->password, ctx->hostname, &ctx->port);
    if (n == 3 || n == 4)
       use_netrc = FALSE;

    /* Check for "user@host_or_IP-address<:port>".
     */
    else
    {
      n = parse_host_spec (ctx, "%30[^:@]@%200[^:@]:%d", ctx->username, ctx->hostname, &ctx->port);
      if (n == 2 || n == 3)
         use_netrc = FALSE;
    }
  }

  if (use_netrc)
       ctx->state = state_netrc_lookup;
  else ctx->state = state_resolve;
  return (TRUE);
}

static BOOL state_init (struct state_CTX *ctx)
{
  ETP_tracef (ctx, "WSAStartup().\n");

  if (WSAStartup(MAKEWORD(1,1), &ctx->wsadata))
  {
    WARN ("Failed to start Winsock, err: %d.\n", WSAGetLastError());
    ctx->state = state_exit;
  }
  else
    ctx->state = state_parse_url;
  return (TRUE);
}

static BOOL state_exit (struct state_CTX *ctx)
{
  FREE (ctx->raw_url);
  ETP_tracef (ctx, "WSACleanup()");
  if (ctx->wsadata.iMaxSockets > 0)
     WSACleanup();
  return (FALSE);
}

/*
 * Run the state-machine until a state-func returns FALSE.
 */
static void SM_run (struct state_CTX *ctx)
{
  while (1)
  {
    ETP_state old_state = ctx->state;
    BOOL      rc = (*ctx->state) (ctx);

    if (opt.debug >= 2)
       C_printf ("~2%s~0 -> ~2%s\n~6%s~0\n",
                 ETP_state_name(old_state),
                 ETP_state_name(ctx->state),
                 ETP_tracef(ctx, NULL));
    if (!rc)
       break;
  }
}

/*
 * Save a piece of trace-information into the context.
 * Retrieve it when 'fmt == NULL'.
 */
static const char *ETP_tracef (struct state_CTX *ctx, const char *fmt, ...)
{
  va_list args;
  int     i, len;

  if (opt.debug < 2)
     return (ctx->trace_buf);

  if (!fmt)
  {
    *ctx->trace_ptr = '\0';
    ctx->trace_ptr  = ctx->trace_buf;
    ctx->trace_left = sizeof(ctx->trace_buf);
    return (ctx->trace_buf);
  }

  for (i = 0; i < 6; i++)
  {
    *ctx->trace_ptr++ = ' ';
    ctx->trace_left--;
  }
  va_start (args, fmt);
  len = vsnprintf (ctx->trace_ptr, ctx->trace_left, fmt, args);
  ctx->trace_left -= len;
  ctx->trace_ptr  += len;

  va_end (args);
  return (ctx->trace_buf);
}

/*
 * Return the name of a state-function. Handy for tracing.
 */
static const char *ETP_state_name (ETP_state f)
{
  #define IF_VALUE(_f) if (f == _f) return (#_f)

  IF_VALUE (state_init);
  IF_VALUE (state_parse_url);
  IF_VALUE (state_netrc_lookup);
  IF_VALUE (state_resolve);
  IF_VALUE (state_connect);
  IF_VALUE (state_send_login);
  IF_VALUE (state_send_pass);
  IF_VALUE (state_await_login);
  IF_VALUE (state_200);
  IF_VALUE (state_send_query);
  IF_VALUE (state_RESULT_COUNT);
  IF_VALUE (state_PATH);
  IF_VALUE (state_closing);
  IF_VALUE (state_exit);
  return ("?");
}

/*
 * Return a 'time_t' for a file in the 'DATE_MODIFIED' response.
 * The 'ft' is in UTC zone.
 */
static time_t FILETIME_to_time_t (const FILETIME *ft)
{
  SYSTEMTIME st, lt;
  struct tm  tm;

  if (!FileTimeToSystemTime(ft,&st) ||
      !SystemTimeToTzSpecificLocalTime(NULL,&st,&lt))
     return (0);

  memset (&tm, '\0', sizeof(tm));
  tm.tm_year  = lt.wYear - 1900;
  tm.tm_mon   = lt.wMonth - 1;
  tm.tm_mday  = lt.wDay;
  tm.tm_hour  = lt.wHour;
  tm.tm_min   = lt.wMinute;
  tm.tm_sec   = lt.wSecond;
  tm.tm_isdst = -1;
  return mktime (&tm);
}

/*
 * Called from envtool.c:
 *   if the 'opt.evry_host' smartlist is not empty, this function gets called
 *   for each ETP-host in the smartlist.
 *
 * \todo:
 *   The 'netrc_init()' + 'netrc_exit()' is currently done for each host.
 *   Should do something better.
 */
int do_check_evry_ept (const char *host)
{
  struct state_CTX ctx;

  memset (&ctx, 0, sizeof(ctx));
  ctx.state        = state_init;
  ctx.trace_buf[0] = '?';
  ctx.trace_buf[1] = '\0';
  ctx.trace_ptr    = ctx.trace_buf;
  ctx.trace_left   = sizeof(ctx.trace_buf);
  ctx.sock         = INVALID_SOCKET;
  ctx.timeout      = RECV_TIMEOUT;
  ctx.raw_url      = STRDUP (host);
  ctx.port         = 21;
  SM_run (&ctx);
  return (ctx.results_got - ctx.results_ignore);
}

/*
 * _MSC_VER <= 1700 (Visual Studio 2012 or older) is lacking 'vsscanf()'.
 * Create our own using 'sscanf()'. Scraped from:
 *   https://stackoverflow.com/questions/2457331/replacement-for-vsscanf-on-msvc
 *
 * If using the Windows-Kit ('_VCRUNTIME_H' is defined) it should have a
 * working 'vsscanf()'. I'm not sure if using MSC_VER <= 1700 with the
 * Windows-Kit is possible. But if using Clang-cl, test this function with
 * it.
 */
#if (defined(_MSC_VER) && (_MSC_VER <= 1800) && !defined(_VCRUNTIME_H)) || defined(__clang__)
  static int _vsscanf2 (const char *buf, const char *fmt, va_list args)
  {
    void *a[5];  /* 5 args is enough here */
    int   i;

    for (i = 0; i < DIM(a); i++)
       a[i] = va_arg (args, void*);
    return sscanf (buf, fmt, a[0], a[1], a[2], a[3], a[4]);
  }
  #define vsscanf(buf, fmt, args) _vsscanf2 (buf, fmt, args)
#endif

static int parse_host_spec (struct state_CTX *ctx, const char *pattern, ...)
{
  int     n;
  va_list args;

  va_start (args, pattern);

  ctx->username[0] = ctx->password[0] = ctx->hostname[0] = '\0';

  n = vsscanf (ctx->raw_url, pattern, args);
  va_end (args);

  ETP_tracef (ctx,
              "pattern: '%s'\n"
              "      n: %d, username: '%s', password: '%s', hostname: '%s', port: %d\n",
              pattern, n, ctx->username, ctx->password, ctx->hostname, ctx->port);
  return (n);
}

