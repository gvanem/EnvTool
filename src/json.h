/** \file json.h
 *  \ingroup Misc
 */
#ifndef _JSON_H
#define _JSON_H

/**
 * This JSON parser is based on these files:
 *   https://github.com/zserge/jsmn/blob/master/jsmn.h
 *   https://github.com/zserge/jsmn/blob/master/example/simple.c
 *
 * \typedef JSON_type_t
 *
 * JSON type identifier. Basic types are:
 *  o Object
 *  o Array
 *  o String
 *  o Other primitive: number, boolean (true/false) or null
 */
typedef enum JSON_type_t {
        JSON_UNDEFINED = 0,
        JSON_OBJECT    = 1,
        JSON_ARRAY     = 2,
        JSON_STRING    = 3,
        JSON_PRIMITIVE = 4
      } JSON_type_t;

/**
 * \typedef JSON_err
 *
 * Error status from `JSON_parse()`
 */
typedef enum JSON_err {
        JSON_ERROR_NO_TOK = -1,   /**< Not enough tokens were provided in `JSON_parse()` */
        JSON_ERROR_INVAL  = -2,   /**< Invalid character inside JSON string */
        JSON_ERROR_PART   = -3    /**< The string is not a full JSON packet, more bytes expected */
      } JSON_err;

/**
 * \typedef JSON_tok_t
 *
 * JSON token description.
 * type     type (object, array, string etc.)
 * start    start position in JSON data string
 * end      end position in JSON data string
 */
typedef struct JSON_tok_t {
        JSON_type_t type;
        int         start;
        int         end;
        int         size;
        int         is_key;
      } JSON_tok_t;

/**
 * \typedef JSON_parser
 *
 * Contains an array of token blocks available. Also stores
 * the string being parsed now and current position in that string.
 */
typedef struct JSON_parser {
        uint32_t      pos;        /**< Offset in the JSON string */
        uint32_t      tok_next;   /**< Next token to allocate */
        int           tok_super;  /**< Superior token node, e.g parent object or array */
    //  uint32_t      line;       /**< Current parser line of input (unreliable, does not work) */
    //  uint32_t      column;     /**< Current parser column of input (unreliable, does not work) */
      } JSON_parser;

void              JSON_init (JSON_parser *parser);
int               JSON_parse (JSON_parser *parser, const char *js, size_t len, JSON_tok_t *tokens, size_t num_tokens);
int               JSON_parse_primitive (JSON_parser *parser, const char *js, size_t len, JSON_tok_t *tokens, size_t num_tokens);
size_t            JSON_get_total_size (const JSON_tok_t *token);
int               JSON_str_eq (const JSON_tok_t *tok, const char *buf, const char *str);
const JSON_tok_t *JSON_get_token_by_index (const JSON_tok_t *token, JSON_type_t type, int index);
const char       *JSON_typestr (JSON_type_t t);
const char       *JSON_strerror (JSON_err e);

#endif



