/*
 * Functions for remote queries using EveryThing's ETP protocol.
 * Ref:
 *   http://www.voidtools.com/support/everything/etp/
 *   https://www.voidtools.com/forum/viewtopic.php?t=1790
 *
 * This file is part of envtool
 *
 * By Gisle Vanem <gvanem@yahoo.no> August 2011.
 *
 */
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <winsock2.h>

#include "color.h"
#include "envtool.h"
#include "Everything_ETP.h"

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
       struct sockaddr_in sa;
       SOCKET             sock;
       const char        *host;
       unsigned           port;
       char               rx_buf [500];
       char              *rx_ptr;
       char               tx_buf [500];
       const char        *search_spec;
       char               trace_buf [1000];
       char              *trace_ptr;
       size_t             trace_left;
       unsigned           results_expected;
       unsigned           results_got;
       unsigned           total_time;
       unsigned           max_time;
       ETP_state          state;
     };

static const char *ETP_tracef (struct state_CTX *ctx, const char *fmt, ...);
static const char *ETP_state_name (ETP_state f);

static BOOL state_200          (struct state_CTX *ctx);
static BOOL state_RESULT_COUNT (struct state_CTX *ctx);
static BOOL state_PATH         (struct state_CTX *ctx);
static BOOL state_closing      (struct state_CTX *ctx);
static BOOL state_resolve      (struct state_CTX *ctx);
static BOOL state_connect      (struct state_CTX *ctx);

static time_t FILETIME_to_time_t (UINT64 ft);

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
 * With can be accomplishes with  a .bat-file and a plain Windows ftp client:

     @echo off
     echo USER                                     > etp-commands
     echo QUOTE EVERYTHING SEARCH notepad.exe     >> etp-commands
     echo QUOTE EVERYTHING PATH_COLUMN 1          >> etp-commands
     echo QUOTE EVERYTHING SIZE_COLUMN 1          >> etp-commands
     echo QUOTE EVERYTHING DATE_MODIFIED_COLUMN 1 >> etp-commands
     echo QUOTE EVERYTHING QUERY                  >> etp-commands
     echo BYE                                     >> etp-commands

     ftp -s:etp-commands 10.0.0.37

     Connected to 10.0.0.37.
     220 Welcome to Everything ETP/FTP
     530 Not logged on.
     User (10.0.0.37:(none)):
     230 Logged on.
     ftp> QUOTE EVERYTHING SEARCH notepad.exe
     200 Search set to (notepad.exe).
     ftp> QUOTE EVERYTHING PATH_COLUMN 1
     200 Path column set to (1).
     ftp> QUOTE EVERYTHING QUERY
     200-Query results
      RESULT_COUNT 3
      PATH C:\Windows
      SIZE 236032
      DATE_MODIFIED 131343347638616569
      FILE notepad.exe
      PATH C:\Windows\System32
      SIZE 236032
      DATE_MODIFIED 131343347658304156
      FILE notepad.exe
      PATH C:\Windows\WinSxS\x86_microsoft-windows-notepad_31bf3856ad364e35_10.0.15063.0_none_240fcb30f07103a5
      SIZE 236032
      DATE_MODIFIED 131343347658304156
      FILE notepad.exe
     200 End.
     ftp> BYE
     221 Goodbye.

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
 * PATH state: gooble up the results.
 * Expect 'results_expected' results set in 'RESULT_COUNT_state()'.
 * When "200 End" is received, enter 'state_closing'.
 */
static BOOL state_PATH (struct state_CTX *ctx)
{
  static time_t mtime;
  static UINT64 fsize;
  static char   path [_MAX_PATH];
  char          fullname [_MAX_PATH];
  char          file [_MAX_PATH];
  UINT64        ft;
  SYSTEMTIME    st;

  recv_line (ctx);

  if (sscanf(ctx->rx_ptr, "FILE %256s", file) == 1)
  {
    ETP_tracef (ctx, "file: %s", file);
    ctx->results_got++;
#if 0
    if (!ctx->mtime || !ctx->fsize || !ctx->path[0])
    {
      WARN ("Got FILE, but no PATH, SIZE and/or DATE_MODIFIED?\n");
      ctx->state = state_closing;
    }
#endif
    snprintf (fullname, sizeof(fullname), "%s\\%s", path, file);
    report_file (fullname, mtime, fsize, FALSE, FALSE, HKEY_EVERYTHING_ETP);
    mtime = 0;
    fsize = 0;
    return (TRUE);
  }

  if (sscanf(ctx->rx_ptr, "PATH %256s", path) == 1)
  {
    ETP_tracef (ctx, "path: %s", path);
    return (TRUE);
  }

  if (sscanf(ctx->rx_ptr, "SIZE %I64u", &fsize) == 1)
  {
    ETP_tracef (ctx, "size: %s", get_file_size_str(fsize));
    return (TRUE);
  }

  if (sscanf(ctx->rx_ptr, "DATE_MODIFIED %I64u", &ft) == 1)
  {
    mtime = FILETIME_to_time_t (ft);
    FileTimeToSystemTime ((const FILETIME*)&ft, &st);

    ETP_tracef (ctx, "mtime: %.24s, sys-time: %02d/%02d/%04d %02d:%02d",
                ctime(&mtime), st.wMonth, st.wDay, st.wYear, st.wHour, st.wMinute);
    return (TRUE);
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
 * Close the socket. The next state-machine should quit the STM immediately.
 */
static BOOL state_closing (struct state_CTX *ctx)
{
  closesocket (ctx->sock);
  ctx->sock = INVALID_SOCKET;

  if (ctx->results_expected > 0 && ctx->results_got != ctx->results_expected)
     WARN ("Expected %u results, but received only %u.\n", ctx->results_expected, ctx->results_got);
  return (FALSE);   /* quit the state machine */
}

static BOOL state_send_query (struct state_CTX *ctx)
{
  send_cmd (ctx, "EVERYTHING SEARCH %s", ctx->search_spec);
  send_cmd (ctx, "EVERYTHING PATH_COLUMN 1");
  send_cmd (ctx, "EVERYTHING SIZE_COLUMN 1");
  send_cmd (ctx, "EVERYTHING DATE_MODIFIED_COLUMN 1");
  if (send_cmd(ctx, "EVERYTHING QUERY") < 0)
       ctx->state = state_closing;
  else ctx->state = state_200;
  return (TRUE);
}

static BOOL state_send_login (struct state_CTX *ctx)
{
  if (send_cmd(ctx, "USER anonymous") < 0)
       ctx->state = state_closing;
  else ctx->state = state_send_query;
  return (TRUE);
}

static BOOL state_resolve (struct state_CTX *ctx)
{
  memset (&ctx->sa, 0, sizeof(ctx->sa));
  C_printf ("Connecting to %s...\r", ctx->host);

  ctx->sa.sin_addr.s_addr = inet_addr (ctx->host);
  if (ctx->sa.sin_addr.s_addr == INADDR_NONE)
  {
    struct hostent *he = gethostbyname (ctx->host);

    if (!he)
    {
      WARN ("Unknown host %s.\n", ctx->host);
      return (FALSE);   /* quit the state machine */
    }
    ctx->sa.sin_addr.s_addr = *(u_long*) he->h_addr_list[0];
  }
  ctx->state = state_connect;
  return (TRUE);
}

static BOOL state_connect (struct state_CTX *ctx)
{
  ctx->sock = socket (AF_INET, SOCK_STREAM, 0);
  ctx->sa.sin_family = AF_INET;
  ctx->sa.sin_port   = htons (ctx->port);

  if (connect(ctx->sock, (const struct sockaddr*)&ctx->sa, sizeof(ctx->sa)) < 0)
  {
    WARN ("Failed to connect to %s: %s.\n", ctx->host, win_strerror(WSAGetLastError()));
    ctx->state = state_closing;
  }
  else
    ctx->state = state_send_login;
  return (TRUE);
}

static void SM_init (struct state_CTX *ctx, ETP_state init_state)
{
  memset (ctx, 0, sizeof(*ctx));
  ctx->state = init_state;
  ctx->trace_buf[0] = '?';
  ctx->trace_buf[1] = '\0';
  ctx->trace_ptr    = ctx->trace_buf;
  ctx->trace_left   = sizeof(ctx->trace_buf);
}

static int ftp_state_machine (const char *host, unsigned port, const char *spec)
{
  struct state_CTX ctx;
  WSADATA wsadata;

  SM_init (&ctx, state_resolve);
  ctx.search_spec = spec;
  ctx.host        = host;
  ctx.port        = port;
  ctx.max_time    = 10*1000;

  if (WSAStartup(MAKEWORD(1,1), &wsadata))
  {
    WARN ("Failed to start Winsock, err: %d.\n", WSAGetLastError());
    return (0);
  }

  while (1)
  {
    ETP_state old_state = ctx.state;
    BOOL      rc = (*ctx.state) (&ctx);

    if (opt.debug >= 2)
       C_printf ("state: %-20s -> %-20s\n~6%s~0\n",
                 ETP_state_name(old_state),
                 ETP_state_name(ctx.state),
                 ETP_tracef(&ctx, NULL));

#if 0  /* \todo */
    SleepEx (1, TRUE);
    if (++ctx.total_time > ctx.max_time)
       break;
#endif
    if (!rc)
      break;
  }

  WSACleanup();
  return (ctx.results_got);
}

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

static const char *ETP_state_name (ETP_state f)
{
  #define ADD_VALUE(f)  { (unsigned)&f, #f }
  static const struct search_list functions[] = {
                                  ADD_VALUE (state_resolve),
                                  ADD_VALUE (state_connect),
                                  ADD_VALUE (state_send_login),
                                  ADD_VALUE (state_200),
                                  ADD_VALUE (state_closing),
                                  ADD_VALUE (state_send_query),
                                  ADD_VALUE (state_RESULT_COUNT),
                                  ADD_VALUE (state_PATH)
                                };
  return list_lookup_name ((unsigned)f, functions, DIM(functions));
}

/*
 * Number of seconds between the beginning of the Windows epoch
 * (Jan. 1, 1601) and the Unix epoch (Jan. 1, 1970).
 */
#define DELTA_EPOCH_IN_SEC 11644473600

static time_t FILETIME_to_time_t (UINT64 ft)
{
  ft /= 10000000;            /* from 100 nano-sec periods to sec */
  ft -= DELTA_EPOCH_IN_SEC;  /* from Windows epoch to Unix epoch */
  return (ft);
}

int do_check_evry_ept (void)
{
  unsigned port = 21;
  char    *colon, host_addr [200];

  _strlcpy (host_addr, opt.evry_host, sizeof(host_addr));
  colon = strchr (host_addr, ':');
  if (colon && (port = atoi(colon+1)) != 0)
     *colon = '\0';

  return ftp_state_machine (host_addr, port, opt.file_spec);
}

