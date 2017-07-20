/*
 * Functions for remote queries using EveryThing's ETP protocol.
 * Ref:
 *   http://www.voidtools.com/support/everything/etp/
 *   https://www.voidtools.com/forum/viewtopic.php?t=1790
 *
 * This file is part of envtool
 *
 * By Gisle Vanem <gvanem@yahoo.no> August 2017.
 *
 * \todo:
 *   Add support for "user:password@hostname:port" syntax in 'do_check_evry_ept()'.
 */

/* Suppress warning for 'inet_addr()' and 'gethostbyname()'
 */
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <winsock2.h>
#include <shlobj.h>

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
       struct sockaddr_in sa;
       SOCKET             sock;
       const char        *host;
       WORD               port;
       const char        *user;
       const char        *password;
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

static const char *ETP_tracef (struct state_CTX *ctx, const char *fmt, ...);
static const char *ETP_state_name (ETP_state f);

static BOOL state_send_pass    (struct state_CTX *ctx);
static BOOL state_await_login  (struct state_CTX *ctx);
static BOOL state_200          (struct state_CTX *ctx);
static BOOL state_RESULT_COUNT (struct state_CTX *ctx);
static BOOL state_PATH         (struct state_CTX *ctx);
static BOOL state_closing      (struct state_CTX *ctx);
static BOOL state_resolve      (struct state_CTX *ctx);
static BOOL state_connect      (struct state_CTX *ctx);

static time_t FILETIME_to_time_t (const FILETIME *ft);

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
 * And add to 'netrc' smartlist.
 */
static BOOL netrc_parse_line (const char *line)
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
    smartlist_add (netrc, ni);
    return (TRUE);
  }
  if (sscanf(line, fmt2, user, passw) == 2)
  {
    ni = CALLOC (1, sizeof(*ni));
    ni->user       = STRDUP (user);
    ni->passw      = STRDUP (passw);
    ni->is_default = TRUE;
    smartlist_add (netrc, ni);
    return (TRUE);
  }
  return (FALSE);
}

/*
 * Get the path to the ".netrc" file, parse it and append entries to the
 * 'netrc' smartlist.
 */
int netrc_init (void)
{
  char    home [_MAX_PATH];
  char    file [_MAX_PATH];
  char   *p;
  FILE   *f = NULL;
  HRESULT rc = SHGetFolderPath (NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_DEFAULT, home);

  if (rc == S_OK)
  {
    snprintf (file, sizeof(file), "%s\\.netrc", home);
    DEBUGF (3, "opening '%s'\n", file);
    f = fopen (file, "r");
  }
  else
  {
    WARN ("Failed to find the APPDATA-folder. Authenticated logins will not work.\n");
    return (0);
  }

  if (!FILE_EXISTS(file))
  {
    WARN ("\"%s\" does not exist. Authenticated logins will not work.\n", file);
    return (0);
  }
  if (!f)
  {
    WARN ("Failed to open \"%s\". Authenticated logins will not work.\n", file);
    return (0);
  }

  netrc = smartlist_new();

  while (1)
  {
    char buf[500];

    if (!fgets(buf,sizeof(buf)-1,f))   /* EOF */
       break;

    for (p = buf; *p && isspace(*p); )
        p++;

    if (*p != '#' && *p != ';')
       netrc_parse_line (buf);
  }
  fclose (f);
  return (1);
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
 * No "\r\n"; it will be added here.
 */
static int send_cmd (struct state_CTX *ctx, const char *fmt, ...)
{
  va_list args;
  int     len, rc;

  va_start (args, fmt);

  len = vsnprintf (ctx->tx_buf, sizeof(ctx->tx_buf), fmt, args);
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

  if (sscanf(ctx->rx_ptr, "SIZE %I64u", &ctx->fsize) == 1)
  {
    ETP_tracef (ctx, "size: %s", get_file_size_str(ctx->fsize));
    return (TRUE);
  }

  if (sscanf(ctx->rx_ptr, "DATE_MODIFIED %I64u", (UINT64*)&ft) == 1)
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
    WARN ("Unexpected response: \"%s\"\n", ctx->rx_buf);
    ctx->state = state_closing;
  }
  return (TRUE);
}

/*
 * Close the socket.
 * The next cycle of the state-machine should force an immediately quit.
 */
static BOOL state_closing (struct state_CTX *ctx)
{
  ETP_tracef (ctx, "closesocket()");
  if (ctx->sock != INVALID_SOCKET)
     closesocket (ctx->sock);
  ctx->sock = INVALID_SOCKET;

  if (ctx->results_expected > 0 && ctx->results_got < ctx->results_expected)
     WARN ("Expected %u results, but received only %u.\n", ctx->results_expected, ctx->results_got);
  return (FALSE);   /* quit the state machine */
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
 * 'ctx->user' and 'ctx->password' must match 'foo/bar'.
 */
static BOOL state_send_login (struct state_CTX *ctx)
{
  int rc;

  if (ctx->user && ctx->password)
  {
    rc = send_cmd (ctx, "USER %s", ctx->user);
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

    snprintf (buf, sizeof(buf), "Failed to login; USER %s.\n", ctx->user);
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
 * Check if 'ctx->host' is simply an IPv4-address.
 * If TRUE
 *   enter 'state_connect'.
 * else
 *   call 'gethostbyname()' to get the IPv4-address.
 *   Then enter 'state_connect'.
 */
static BOOL state_resolve (struct state_CTX *ctx)
{
  struct hostent *he;

  if (!ctx->host[0])
  {
    WARN ("Empty hostname!\n");
    return (FALSE);   /* quit the state machine */
  }

  memset (&ctx->sa, 0, sizeof(ctx->sa));
  ctx->sa.sin_addr.s_addr = inet_addr (ctx->host);

  /* If 'ctx->host' is not simply an IPv4-address, it
   * must be resolved to an IPv4-address first.
   */
  if (ctx->sa.sin_addr.s_addr == INADDR_NONE)
  {
    if (!opt.quiet)
        C_printf ("Resolving %s...", ctx->host);
    C_flush();
    he = gethostbyname (ctx->host);

    if (!he)
    {
      WARN (" Unknown host.\n");
      return (FALSE);   /* quit the state machine */
    }
    C_putc ('\r');
    ctx->sa.sin_addr.s_addr = *(u_long*) he->h_addr_list[0];
  }
  ctx->state = state_connect;
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
    WARN ("Failed to create socket.\n");
    ETP_tracef (ctx, "Failed to create socket.");
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
    WARN (" Failed to connect.\n");
    ETP_tracef (ctx, "Failed to connect.");
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
 * The initialiser for our simple state-machine.
 */
static BOOL SM_init (struct state_CTX *ctx, ETP_state init_state)
{
  WSADATA wsadata;

  memset (ctx, 0, sizeof(*ctx));
  ctx->state        = init_state;
  ctx->trace_buf[0] = '?';
  ctx->trace_buf[1] = '\0';
  ctx->trace_ptr    = ctx->trace_buf;
  ctx->trace_left   = sizeof(ctx->trace_buf);
  ctx->sock         = INVALID_SOCKET;
  ctx->timeout      = RECV_TIMEOUT;

  if (WSAStartup(MAKEWORD(1,1), &wsadata))
  {
    WARN ("Failed to start Winsock, err: %d.\n", WSAGetLastError());
    return (FALSE);
  }
  return (TRUE);
}

/*
 * The finitialiser for our simple state-machine.
 */
static void SM_exit (struct state_CTX *ctx)
{
  WSACleanup();
  ARGSUSED (ctx);
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

static int ftp_state_machine (const char *host, WORD port,
                              const char *user, const char *password)
{
  struct state_CTX ctx;

  if (!SM_init(&ctx, state_resolve))
     return (0);

  ctx.host     = host;
  ctx.port     = port;
  ctx.user     = user;
  ctx.password = password;

  DEBUGF (2, "ctx.host:     '%s'\n", ctx.host);
  DEBUGF (2, "ctx.user:     '%s'\n", ctx.user);
  DEBUGF (2, "ctx.password: '%s'\n", ctx.password);
  DEBUGF (2, "ctx.port:      %u\n",  ctx.port);

  SM_run (&ctx);
  SM_exit (&ctx);
  return (ctx.results_got - ctx.results_ignore);
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

  IF_VALUE (state_resolve);
  IF_VALUE (state_connect);
  IF_VALUE (state_send_login);
  IF_VALUE (state_send_pass);
  IF_VALUE (state_await_login);
  IF_VALUE (state_200);
  IF_VALUE (state_closing);
  IF_VALUE (state_send_query);
  IF_VALUE (state_RESULT_COUNT);
  IF_VALUE (state_PATH);
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

static char username [30];
static char password [30];
static char host_addr [200];
static int  port = 21;

/*
 * MSVC v1800 or older seems to be lacking 'vsscanf()'.
 * Create our own using the CRT function '_input_s_l()'.
 */
#if defined(_MSC_VER) && (_MSC_VER <= 1800)
  extern int __cdecl _input_s_l (FILE       *stream,
                                 const char *format,
                                 void       *locale,
                                 va_list     arglist);

  static int _vsscanf2 (const char *buf, const char *pattern, va_list args)
  {
    FILE *fil = stdin;

    setvbuf (fil, (char*)buf, _IOLBF, sizeof(host_addr));
    return _input_s_l (fil, pattern, NULL, args);
  }
  #define vsscanf(buf, pattern, args) _vsscanf2 (buf, pattern, args)
#endif

static int parse_host_spec (const char *pattern, ...)
{
  int     n;
  va_list args;

  va_start (args, pattern);

  username[0] = '\0';
  password[0] = '\0';
  host_addr[0] = '\0';

  n = vsscanf (opt.evry_host, pattern, args);
  va_end (args);
  DEBUGF (2, "pattern: '%s'\n"
             "                       n: %d,"
             " username: '%s', password: '%s', host_addr: '%s', port: %d\n",
          pattern, n, username, password, host_addr, port);
  return (n);
}

/*
 * Called from envtool.c:
 *   if 'opt.evry_host != NULL', this function gets called instead of
 *   'do_check_evry()' (which only searches for local results).
 *
 * Check if 'opt.evry_host' matches one of these formats:
 *   "user:passwd@host_or_IP-address<:port>".    Both user-name and password.
 *   "user@host_or_IP-address<:port>".           Only user-name.
 */
int do_check_evry_ept (void)
{
  int  rc, n;
  BOOL use_netrc = TRUE;

 /* Check simple case of "host_or_IP-address<:port>" first.
  */
  n = parse_host_spec ("%80[^:]:%d", host_addr, &port);
  if ((n == 1 || n == 2) && !strchr(opt.evry_host,'@'))
     use_netrc = TRUE;
  else
  {
    /* Check for "user:passwd@host_or_IP-address<:port>".
     */
    n = parse_host_spec ("%30[^:@]:%30[^:@]@%80[^:]:%d", username, password, host_addr, &port);
    if (n == 3 || n == 4)
       use_netrc = FALSE;

    /* Check for "user@host_or_IP-address<:port>".
     */
    else
    {
      n = parse_host_spec ("%30[^:@]@%80[^:@]:%d", username, host_addr, &port);
      if (n == 2 || n == 3)
         use_netrc = FALSE;
    }
  }

  if (use_netrc)
  {
    const char *user  = NULL;
    const char *passw = NULL;

    netrc_init();
    netrc_lookup (host_addr, &user, &passw);
    rc = ftp_state_machine (host_addr, port, user, passw);
    netrc_exit();
  }
  else
    rc = ftp_state_machine (host_addr, port, username, password);
  return (rc);
}

