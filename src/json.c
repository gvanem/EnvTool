#include "envtool.h"
#include "color.h"
#include "json.h"

/**
 * Returns a string naming token type `t`.
 */
const char *JSON_typestr (JSON_type_t t)
{
  return (t == JSON_UNDEFINED ? "UNDEFINED" :
          t == JSON_OBJECT    ? "OBJECT"    :
          t == JSON_ARRAY     ? "ARRAY"     :
          t == JSON_STRING    ? "STRING"    :
          t == JSON_PRIMITIVE ? "PRIMITIVE" : "?");
}

/**
 * Returns an error-string for error `e`.
 */
const char *JSON_strerror (JSON_err e)
{
  return (e == JSON_ERROR_NO_TOK ? "JSON_ERROR_NO_TOK" :
          e == JSON_ERROR_INVAL  ? "JSON_ERROR_INVAL"  :
          e == JSON_ERROR_PART   ? "JSON_ERROR_PART"   : "?");
}

/**
 * Allocates a fresh unused token from the token pool.
 */
static JSON_tok_t *JSON_alloc_token (JSON_parser *parser, JSON_tok_t *tokens, size_t num_tokens)
{
  JSON_tok_t *tok;

  if (parser->tok_next >= num_tokens)
     return (NULL);

  tok = &tokens [parser->tok_next++];
  tok->start = tok->end = -1;
  tok->size = 0;
  tok->is_key = 0;
  return (tok);
}

/**
 * Fills token type and set boundaries.
 */
static void JSON_fill_token (JSON_tok_t *token, JSON_type_t type, int start, int end)
{
  token->type  = type;
  token->start = start;
  token->end   = end;
  token->size  = 0;
}

/**
 * Fills next available token with JSON primitive.
 */
int JSON_parse_primitive (JSON_parser *parser, const char *js, size_t len, JSON_tok_t *tokens, size_t num_tokens)
{
  JSON_tok_t *token;
  int         start = parser->pos;

  for ( ; parser->pos < len && js[parser->pos]; parser->pos++)
  {
    switch (js[parser->pos])
    {
      case ':':
      case '\t':
      case '\r':
      case '\n':
      case ' ':
      case ',':
      case ']':
      case '}':
           goto found;
    }
    if (js[parser->pos] < 32 || js[parser->pos] >= 127)
    {
      parser->pos = start;
      return (JSON_ERROR_INVAL);
    }
  }

found:
  if (!tokens)
  {
    parser->pos--;
    return (0);
  }
  token = JSON_alloc_token (parser, tokens, num_tokens);
  if (!token)
  {
    parser->pos = start;
    TRACE (2, "No more tokens\n");
    return (JSON_ERROR_NO_TOK);
  }
  JSON_fill_token (token, JSON_PRIMITIVE, start, parser->pos);
  parser->pos--;
  return (0);
}

/**
 * Fills next token with JSON string.
 */
int JSON_parse_string (JSON_parser *parser, const char *js, size_t len, JSON_tok_t *tokens, size_t num_tokens)
{
  JSON_tok_t *token;
  int         i, start = parser->pos;
  char        c;

  parser->pos++;

  /* Skip starting quote
   */
  for ( ; parser->pos < len && js[parser->pos]; parser->pos++)
  {
    c = js[parser->pos];

    if (c == '\"')   /* Quote: end of string */
    {
      if (!tokens)
         return (0);

      token = JSON_alloc_token (parser, tokens, num_tokens);
      if (!token)
      {
        parser->pos = start;
        TRACE (2, "No more tokens\n");
        return (JSON_ERROR_NO_TOK);
      }
      JSON_fill_token (token, JSON_STRING, start+1, parser->pos);
      return (0);
    }

    if (c == '\\' && parser->pos + 1 < len)  /* Backslash: Quoted symbol expected */
    {
      parser->pos++;
      switch (js[parser->pos])
      {
        /* Allowed escaped symbols */
        case '\"':
        case '/':
        case '\\':
        case 'b':
        case 'f':
        case 'r':
        case 'n':
        case 't':
             break;

        /* Allows escaped symbol '\uXXXX' */
        case 'u':
             parser->pos++;
             for(i = 0; i < 4 && parser->pos < len && js[parser->pos]; i++)
             {
               /* If it isn't a hex character we have an error */
               if (!((js[parser->pos] >= 48 && js[parser->pos] <= 57) ||  /* 0-9 */
                     (js[parser->pos] >= 65 && js[parser->pos] <= 70) ||  /* A-F */
                     (js[parser->pos] >= 97 && js[parser->pos] <= 102)))  /* a-f */
               {
                 parser->pos = start;
                 return (JSON_ERROR_INVAL);
               }
               parser->pos++;
             }
             parser->pos--;
             break;

        /* Unexpected symbol */
        default:
             parser->pos = start;
             return (JSON_ERROR_INVAL);
      }
    }
  }
  parser->pos = start;
  return (JSON_ERROR_PART);
}

/**
 * Run JSON parser. It parses a JSON data string into and array of tokens, each describing
 * a single JSON object.
 */
int JSON_parse (JSON_parser *parser, const char *js, size_t len, JSON_tok_t *tokens, unsigned int num_tokens)
{
  JSON_tok_t *token;
  int         r, i, count = parser->tok_next;

  for (; parser->pos < len && js[parser->pos]; parser->pos++)
  {
    JSON_type_t type;
    char c = js [parser->pos];

    switch (c)
    {
      case '{':
      case '[':
           count++;
           if (!tokens)
              break;

           token = JSON_alloc_token (parser, tokens, num_tokens);
           if (!token)
           {
             TRACE (2, "No more tokens\n");
             return (JSON_ERROR_NO_TOK);
           }
           if (parser->tok_super != -1)
              tokens[parser->tok_super].size++;

           token->type = (c == '{' ? JSON_OBJECT : JSON_ARRAY);
           token->start = parser->pos;
           parser->tok_super = parser->tok_next - 1;
           break;

      case '}':
      case ']':
           if (!tokens)
              break;
           type = (c == '}' ? JSON_OBJECT : JSON_ARRAY);
           for (i = parser->tok_next - 1; i >= 0; i--)
           {
             token = &tokens[i];
             if (token->start != -1 && token->end == -1)
             {
               if (token->type != type)
                  return (JSON_ERROR_INVAL);

               parser->tok_super = -1;
               token->end = parser->pos + 1;
               break;
             }
           }
           /* Error if unmatched closing bracket */
           if (i == -1)
              return (JSON_ERROR_INVAL);
           for (; i >= 0; i--)
           {
             token = &tokens[i];
             if (token->start != -1 && token->end == -1)
             {
               parser->tok_super = i;
               break;
             }
           }
           break;

      case '\"':
           r = JSON_parse_string (parser, js, len, tokens, num_tokens);
           if (r < 0)
              return (r);
           count++;
           if (parser->tok_super != -1 && tokens)
              tokens[parser->tok_super].size++;
           break;

      case '\n':
      case '\t':
      case '\r':
      case ' ':
           break;

      case ':':
           if (tokens && parser->tok_next >= 1)
           {
             JSON_tok_t *key = &tokens [parser->tok_next-1];

             if (key && key->type == JSON_STRING)
                key->is_key = 1;
           }
           parser->tok_super = parser->tok_next - 1;
           break;

      case ',':
           if (tokens && parser->tok_super != -1 &&
               tokens[parser->tok_super].type != JSON_ARRAY &&
               tokens[parser->tok_super].type != JSON_OBJECT)
           {
             for (i = parser->tok_next - 1; i >= 0; i--)
             {
               if (tokens[i].type == JSON_ARRAY || tokens[i].type == JSON_OBJECT)
               {
                 if (tokens[i].start != -1 && tokens[i].end == -1)
                 {
                   parser->tok_super = i;
                   break;
                 }
               }
             }
           }
           break;

      /* In non-strict mode every unquoted value is a primitive */
      default:
           r = JSON_parse_primitive (parser, js, len, tokens, num_tokens);
           if (r < 0)
              return (r);
           count++;
           if (tokens && parser->tok_super != -1)
              tokens[parser->tok_super].size++;
           break;
    }
  }

  if (tokens)
  {
    for (i = parser->tok_next - 1; i >= 0; i--)
    {
      /* Unmatched opened object or array
       */
      if (tokens[i].start != -1 && tokens[i].end == -1)
         return (JSON_ERROR_PART);
    }
  }
  return (count);
}

/**
 * Creates a new parser based on a given buffer with an array of tokens.
 */
void JSON_init (JSON_parser *parser)
{
  memset (parser, '\0', sizeof(*parser));
  parser->tok_super = -1;
}

int JSON_str_eq (const JSON_tok_t *tok, const char *buf, const char *str)
{
  size_t len = tok->end - tok->start;

  if (tok->type == JSON_STRING && (int)strlen(str) == len && !strnicmp(buf + tok->start, str, len))
     return (1);
  return (0);
}

/**
 * Scraped from https://github.com/zserge/jsmn/pull/166/files
 * By Maxim Menshikov
 *
 * Get the size (in tokens) consumed by token and its children.
 */
size_t JSON_get_total_size (const JSON_tok_t *token)
{
  int   rc = 0;
  int   i, j;
  const JSON_tok_t *key;

  if (token->type == JSON_PRIMITIVE || token->type == JSON_STRING)
  {
    rc = 1;
  }
  else if (token->type == JSON_OBJECT)
  {
    for (i = j = 0; i < token->size; i++)
    {
      key = token + 1 + j;
      j += JSON_get_total_size (key);
      if (key->size > 0)
         j += JSON_get_total_size (token + 1 + j);
    }
    rc = j + 1;
  }
  else if (token->type == JSON_ARRAY)
  {
    for (i = j = 0; i < token->size; i++)
        j += JSON_get_total_size (token + 1 + j);
    rc = j + 1;
  }
  return (rc);
}

/**
 * Get token with a given index inside the JSON_ARRAY or JSON_OBJECT defined by 'token' parameter.
 */
const JSON_tok_t *JSON_get_token_by_index (const JSON_tok_t *token, JSON_type_t type, int index)
{
  const JSON_tok_t *token2;
  int   i, total_size;

  if (token->type != type)
     return (NULL);

  total_size = JSON_get_total_size (token);
  for (i = 1; i < total_size; i++)
  {
    token2 = token + i;
    if (index == 0)
       return (token2);
    i += JSON_get_total_size (token2) - 1;
    --index;
  }
  return (NULL);
}

