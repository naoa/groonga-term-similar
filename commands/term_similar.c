/* Copyright(C) 2015 Naoya Murakami <naoya@createfield.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
  02110-1301  USA
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <limits.h>
#include <groonga/plugin.h>
#include <groonga/token_filter.h>

#define CONST_STR_LEN(x) x, x ? sizeof(x) - 1 : 0

#ifdef __GNUC__
# define GNUC_UNUSED __attribute__((__unused__))
#else
# define GNUC_UNUSED
#endif

#define DEFAULT_SORTBY          "_score"
#define DEFAULT_OUTPUT_COLUMNS  "_key,_score"

typedef struct {
  double score;
  int n_subrecs;
  int subrecs[1];
} grn_rset_recinfo;

/*
static double
cosine_similarity(double *a, double *b, unsigned int vector_length)
{
  double dot = 0.0, denom_a = 0.0, denom_b = 0.0 ;
  unsigned int i;
   for(i = 0u; i < vector_length; ++i) {
      dot += a[i] * b[i] ;
      denom_a += a[i] * a[i] ;
      denom_b += b[i] * b[i] ;
  }
  return dot / (sqrt(denom_a) * sqrt(denom_b)) ;
}
*/

static int
cut_invalid_char_byte(grn_ctx *ctx, int length, grn_obj *term)
{
  int char_length;
  int rest_length = GRN_TEXT_LEN(term);
  const char *rest = GRN_TEXT_VALUE(term);
  grn_encoding encoding = GRN_CTX_GET_ENCODING(ctx);
  char_length = grn_plugin_charlen(ctx, rest, rest_length, encoding);
  if (char_length > 1) {
    length -= length % char_length;
  }
  return length;
}

static void
get_best_match_record(grn_ctx *ctx, grn_obj *res, grn_obj *buf, const char *sortby_val, unsigned int sortby_len)
{
  grn_obj *sorted;
  if ((sorted = grn_table_create(ctx, NULL, 0, NULL, GRN_OBJ_TABLE_NO_KEY, NULL, res))) {
    uint32_t nkeys;
    grn_table_sort_key *keys;
    const char *oc_val;
    unsigned int oc_len;
    int offset = 0;
    int limit = 1;
    if (!sortby_val || !sortby_len) {
      sortby_val = DEFAULT_SORTBY;
      sortby_len = sizeof(DEFAULT_SORTBY) - 1;
    }
    if (!oc_val || !oc_len) {
      oc_val = DEFAULT_OUTPUT_COLUMNS;
      oc_len = sizeof(DEFAULT_OUTPUT_COLUMNS) - 1;
    }

    if ((keys = grn_table_sort_key_from_str(ctx, sortby_val, sortby_len, res, &nkeys))) {
      grn_table_sort(ctx, res, offset, limit, sorted, keys, nkeys);
      grn_table_cursor *tc;
      if ((tc = grn_table_cursor_open(ctx, sorted, NULL, 0, NULL, 0, 0, -1, GRN_CURSOR_BY_ID))) {
        grn_id id;
        grn_obj *column = grn_obj_column(ctx, sorted,
                                         GRN_COLUMN_NAME_KEY,
                                         GRN_COLUMN_NAME_KEY_LEN);
        if ((id = grn_table_cursor_next(ctx, tc))) {
          GRN_BULK_REWIND(buf);
          grn_obj_get_value(ctx, column, id, buf);
        }
        grn_obj_unlink(ctx, column);
        grn_table_cursor_close(ctx, tc);
      }
      grn_table_sort_key_close(ctx, keys, nkeys);
    }
    grn_obj_unlink(ctx, sorted);
  } else {
    GRN_PLUGIN_ERROR(ctx, GRN_LOG_ERROR, "[term_similar] cannot create temporary sort table.");
  }
}

static void
output(grn_ctx *ctx, grn_obj *res, int offset, int limit, const char *sortby_val, unsigned int sortby_len)
{
  grn_obj *sorted;
  if ((sorted = grn_table_create(ctx, NULL, 0, NULL, GRN_OBJ_TABLE_NO_KEY, NULL, res))) {
    uint32_t nkeys;
    grn_obj_format format;
    grn_table_sort_key *keys;
    const char *oc_val;
    unsigned int oc_len;
    if (!sortby_val || !sortby_len) {
      sortby_val = DEFAULT_SORTBY;
      sortby_len = sizeof(DEFAULT_SORTBY) - 1;
    }
    if (!oc_val || !oc_len) {
      oc_val = DEFAULT_OUTPUT_COLUMNS;
      oc_len = sizeof(DEFAULT_OUTPUT_COLUMNS) - 1;
    }

    if ((keys = grn_table_sort_key_from_str(ctx, sortby_val, sortby_len, res, &nkeys))) {
      grn_table_sort(ctx, res, offset, limit, sorted, keys, nkeys);

      GRN_OBJ_FORMAT_INIT(&format, grn_table_size(ctx, res), 0, limit, offset);
      format.flags = GRN_OBJ_FORMAT_WITH_COLUMN_NAMES;
      grn_obj_columns(ctx, sorted, oc_val, oc_len, &format.columns);
      grn_ctx_output_obj(ctx, sorted, &format);
      GRN_OBJ_FORMAT_FIN(ctx, &format);
      grn_table_sort_key_close(ctx, keys, nkeys);
    }
    grn_obj_unlink(ctx, sorted);
  } else {
    GRN_PLUGIN_ERROR(ctx, GRN_LOG_ERROR, "[term_similar] cannot create temporary sort table.");
  }
}

static void
get_index_column(grn_ctx *ctx, grn_obj *table, grn_obj **index_column)
{
  grn_hash *columns;
  if ((columns = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                 GRN_OBJ_TABLE_HASH_KEY))) {
    if (grn_table_columns(ctx, table, "", 0, (grn_obj *)columns)) {
      grn_id *key;
      GRN_HASH_EACH(ctx, columns, id, &key, NULL, NULL, {
        grn_obj *column = grn_ctx_at(ctx, *key);
        if (column->header.type == GRN_COLUMN_INDEX) {
          *index_column = column;
          grn_obj_unlink(ctx, column);
          break;
        }
        grn_obj_unlink(ctx, column);
      });
    }
    grn_hash_close(ctx, columns);
  }
}

static void
predictive_search(grn_ctx *ctx, grn_obj* table, grn_obj *res, char *term, int term_length)
{
  grn_table_cursor *tc;
  if ((tc = grn_table_cursor_open(ctx, table, term, term_length, NULL, 0, 0, -1, GRN_CURSOR_PREFIX))) {
    grn_id id;
    while ((id = grn_table_cursor_next(ctx, tc))) {
      void *value;
      if (grn_hash_add(ctx, (grn_hash *)res, &id, sizeof(grn_id), &value, NULL)) {
        grn_rset_recinfo *ri;
        ri = value;
        ri->score = 0;
      }
    }
  }
  grn_table_cursor_close(ctx, tc);
}

static void
calc_keyboard_distance_with_df_boost(grn_ctx *ctx, grn_obj *res, grn_obj *term,
                                     int distance_threshold, grn_obj *index_column, int df_threshold)
{
  grn_obj *var;
  grn_obj *expr;
  GRN_EXPR_CREATE_FOR_QUERY(ctx, res, expr, var);
  if (expr) {
    grn_table_cursor *tc;
    grn_obj *score = grn_obj_column(ctx, res,
                                    GRN_COLUMN_NAME_SCORE,
                                    GRN_COLUMN_NAME_SCORE_LEN);
    grn_obj *key = grn_obj_column(ctx, res,
                                  GRN_COLUMN_NAME_KEY,
                                  GRN_COLUMN_NAME_KEY_LEN);
    grn_expr_append_obj(ctx, expr,
                        score,
                        GRN_OP_GET_VALUE, 1);
    grn_expr_append_obj(ctx, expr,
                        grn_ctx_get(ctx, CONST_STR_LEN("keyboard_distance")),
                        GRN_OP_PUSH, 1);
    grn_expr_append_obj(ctx, expr,
                        key,
                        GRN_OP_GET_VALUE, 1);
    grn_expr_append_const(ctx, expr, term, GRN_OP_PUSH, 1);
    grn_expr_append_op(ctx, expr, GRN_OP_CALL, 2);
    grn_expr_append_op(ctx, expr, GRN_OP_PLUS_ASSIGN, 2);
    grn_obj df;
    GRN_UINT32_INIT(&df, 0);

    if ((tc = grn_table_cursor_open(ctx, res, NULL, 0, NULL, 0, 0, -1, GRN_CURSOR_BY_ID))) {
      grn_id id;
      grn_obj score_value;
      grn_obj key_value;
      GRN_FLOAT_INIT(&score_value, 0);
      GRN_TEXT_INIT(&key_value, 0);
      while ((id = grn_table_cursor_next(ctx, tc)) != GRN_ID_NIL) {
        unsigned int org_id;
        GRN_RECORD_SET(ctx, var, id);
        grn_expr_exec(ctx, expr, 0);
        GRN_BULK_REWIND(&score_value);
        GRN_BULK_REWIND(&key_value);
        grn_obj_get_value(ctx, score, id, &score_value);
        grn_obj_get_value(ctx, key, id, &key_value);
        grn_table_get_key(ctx, res, id, &org_id, sizeof(unsigned int));
        if ((GRN_FLOAT_VALUE(&score_value) <= 0 || GRN_FLOAT_VALUE(&score_value) > distance_threshold) &&
            !(index_column && df_threshold > 0 && GRN_TEXT_LEN(term) == GRN_TEXT_LEN(&key_value) &&
            strncmp(GRN_TEXT_VALUE(term), GRN_TEXT_VALUE(&key_value), GRN_TEXT_LEN(&key_value)) == 0)) {
            grn_table_cursor_delete(ctx, tc);
        }
        if (index_column && df_threshold) {
          GRN_BULK_REWIND(&df);
          grn_obj_get_value(ctx, index_column, org_id, &df);
          if (GRN_UINT32_VALUE(&df)) {
            if ((int)GRN_UINT32_VALUE(&df) > df_threshold) {
              grn_obj_set_value(ctx, score, id, &df, GRN_OBJ_SET);
            } else {
              grn_table_cursor_delete(ctx, tc);
            }
          }
        }
      }
      grn_obj_unlink(ctx, &score_value);
      grn_obj_unlink(ctx, &key_value);
      grn_table_cursor_close(ctx, tc);
    }
    grn_obj_unlink(ctx, score);
    grn_obj_unlink(ctx, key);
    grn_obj_unlink(ctx, expr);
    grn_obj_unlink(ctx, &df);
  } else {
    GRN_PLUGIN_LOG(ctx, GRN_LOG_ERROR,
       "[term_similar] error on building expr. for calcurating edit distance");
  }
}

static grn_obj *
command_table_term_similar(grn_ctx *ctx, GNUC_UNUSED int nargs, GNUC_UNUSED grn_obj **args,
                           grn_user_data *user_data)
{
  grn_obj *var;
  grn_obj *table = NULL;
  grn_obj *index_column = NULL;
  grn_obj *res;
  grn_obj *term;
  char *table_name = NULL;
  int table_name_length = -1;
  char *term_str = NULL;
  int term_length = 0;
  double prefix_length = 3;
  int distance_threshold = 30;
  int limit = -1;
  int df_threshold = 0;

  var = grn_plugin_proc_get_var(ctx, user_data, "term", -1);
  if (GRN_TEXT_LEN(var) != 0) {
    term = var;
    term_str = GRN_TEXT_VALUE(var);
    term_length = GRN_TEXT_LEN(var);
  } else {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT, "[term_similar] term is missing");
    return NULL;
  }
  var = grn_plugin_proc_get_var(ctx, user_data, "table", -1);
  if (GRN_TEXT_LEN(var) != 0) {
    table_name = GRN_TEXT_VALUE(var);
    table_name_length = GRN_TEXT_LEN(var);
  } else {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT, "[term_similar] table name is missing");
    return NULL;
  }
  var = grn_plugin_proc_get_var(ctx, user_data, "prefix_length", -1);
  if (GRN_TEXT_LEN(var) != 0) {
    prefix_length = atof(GRN_TEXT_VALUE(var));
  }
  var = grn_plugin_proc_get_var(ctx, user_data, "distance_threshold", -1);
  if (GRN_TEXT_LEN(var) != 0) {
    distance_threshold = atoi(GRN_TEXT_VALUE(var));
  }
  var = grn_plugin_proc_get_var(ctx, user_data, "df_threshold", -1);
  if (GRN_TEXT_LEN(var) != 0) {
    df_threshold = atoi(GRN_TEXT_VALUE(var));
  }
  var = grn_plugin_proc_get_var(ctx, user_data, "limit", -1);
  if (GRN_TEXT_LEN(var) != 0) {
    limit = atoi(GRN_TEXT_VALUE(var));
  }
  if (prefix_length > term_length) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "[term_similar] term length is shorter than prefix length: <%d:%f>",
                     term_length, prefix_length);
    return NULL;
  }

  table = grn_ctx_get(ctx, table_name, table_name_length);
  if (!table) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "[term_similar] table doesn't exist: <%.*s>",
                     table_name_length, table_name);
    return NULL;
  }

  get_index_column(ctx, table, &index_column);
  if ((res = grn_table_create(ctx, NULL, 0, NULL,
                              GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC, table, NULL))) {
    int prefix_term_length = (int)prefix_length;
    if (prefix_length < 1) {
      prefix_term_length = (int)(term_length * prefix_length);
      prefix_term_length = cut_invalid_char_byte(ctx, prefix_term_length, term);
    }
    predictive_search(ctx, table, res, term_str, prefix_term_length);
    calc_keyboard_distance_with_df_boost(ctx, res, term, distance_threshold, index_column, df_threshold);
    if (index_column && df_threshold) {
      output(ctx, res, 0, limit, "-_score", strlen("-_score"));
    } else {
      output(ctx, res, 0, limit, "_score", strlen("_score"));
    }
    grn_obj_close(ctx, res);
  } else {
    GRN_PLUGIN_LOG(ctx, GRN_LOG_ERROR, "[term_similar] cannot create temporary table.");
  }

  if (table) {
    grn_obj_unlink(ctx, table);
  }
  if (index_column) {
    grn_obj_unlink(ctx, index_column);
  }

  return NULL;
}

static grn_obj *
func_term_similar(grn_ctx *ctx, GNUC_UNUSED int nargs, GNUC_UNUSED grn_obj **args,
                  grn_user_data *user_data)
{
  grn_obj *result;
  grn_obj *term = args[0];
  grn_obj *table_name = args[1];
  grn_obj *table;
  grn_obj *index_column = NULL;
  grn_obj *res;
  double prefix_length = 3;
  int prefix_term_length = 3;
  int distance_threshold = 25;
  int min_length = 5;
  int df_threshold = 5;

  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(table_name), GRN_TEXT_LEN(table_name));
  result = grn_plugin_proc_alloc(ctx, user_data, GRN_DB_SHORT_TEXT, 0);

  if (nargs > 2) {
    prefix_length = GRN_FLOAT_VALUE(args[2]);
  }
  if (nargs > 3) {
    distance_threshold = GRN_INT32_VALUE(args[3]);
  }
  if (nargs > 4) {
    df_threshold = GRN_INT32_VALUE(args[4]);
  }
  if (nargs > 5) {
    min_length = GRN_INT32_VALUE(args[5]);
  }
  if (prefix_length < 1) {
    prefix_term_length = (int)(prefix_length * GRN_TEXT_LEN(term));
    prefix_term_length = cut_invalid_char_byte(ctx, prefix_term_length, term);
  }

  if (prefix_term_length > GRN_TEXT_LEN(term) || min_length > GRN_TEXT_LEN(term)) {
    if (table) {
      grn_obj_unlink(ctx, table);
    }
    return result;
  }

  get_index_column(ctx, table, &index_column);
  if ((res = grn_table_create(ctx, NULL, 0, NULL,
                              GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC, table, NULL))) {
    predictive_search(ctx, table, res, GRN_TEXT_VALUE(term), prefix_term_length);
    calc_keyboard_distance_with_df_boost(ctx, res, term, distance_threshold, index_column, df_threshold);
    {
      grn_obj buf;
      GRN_TEXT_INIT(&buf, 0);
      GRN_BULK_REWIND(&buf);
      if (index_column && df_threshold) {
        get_best_match_record(ctx, res, &buf, "-_score", strlen("-_score"));
      } else {
        get_best_match_record(ctx, res, &buf, "_score", strlen("_score"));
      }
      if (GRN_TEXT_LEN(&buf) &&
          !(GRN_TEXT_LEN(term) == GRN_TEXT_LEN(&buf) &&
           strncmp(GRN_TEXT_VALUE(term), GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf)) == 0)) {
        GRN_TEXT_SET(ctx, result, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf));
      }
      grn_obj_unlink(ctx, &buf);
    }
    grn_obj_close(ctx, res);
  } else {
    GRN_PLUGIN_LOG(ctx, GRN_LOG_ERROR, "[term_similar] cannot create temporary table.");
  }

  if (table) {
    grn_obj_unlink(ctx, table);
  }
  if (index_column) {
    grn_obj_unlink(ctx, index_column);
  }

  return result;
}

/*
     1   2   3   4   5	 6   7   8   9  10  11  12  13
  ------------------------------------------------------
 1|  `   1   2   3   4   5   6   7   8   9   0   -   =
 2|  q   w   e   r   t   y   u   i   o   p   [   ]   \
 3|  a   s   d   f   g   h   j   k   l   ;   '
 4|  z   x   c   v   b   n   m   ,   .   /
 5|                 ' '

     1   2   3   4   5   6   7   8   9  10  11  12  13
  ------------------------------------------------------
 1|  ~   !   @   #   $   %   ^   &   *   (   )   +
 2|  Q   W   E   R   T   Y   U   I   O   P   {   }   |
 3|  A   S   D   F   G   H   J   K   L   :   "
 4|  Z   X   C   V   B   N   M   <   >   ?
 5|                 ' '

 */

static double
qwerty_map_y(const char key)
{
  switch (key) {
  case '`' :
  case '1' :
  case '2' :
  case '3' :
  case '4' :
  case '5' :
  case '6' :
  case '7' :
  case '8' :
  case '9' :
  case '0' :
  case '-' :
  case '=' :
  case '~' :
  case '!' :
  case '@' :
  case '#' :
  case '$' :
  case '%' :
  case '^' :
  case '&' :
  case '*' :
  case '(' :
  case ')' :
  case '+' :
    return 1.0;
  case 'q' :
  case 'w' :
  case 'e' :
  case 'r' :
  case 't' :
  case 'u' :
  case 'i' :
  case 'o' :
  case 'p' :
  case '[' :
  case ']' :
  case '\\' :
  case 'Q' :
  case 'W' :
  case 'E' :
  case 'R' :
  case 'T' :
  case 'Y' :
  case 'U' :
  case 'I' :
  case 'O' :
  case 'P' :
  case '{' :
  case '}' :
  case '|' :
    return 2.0;
  case 'a' :
  case 's' :
  case 'd' :
  case 'f' :
  case 'g' :
  case 'h' :
  case 'j' :
  case 'k' :
  case 'l' :
  case ';' :
  case '\'' :
  case 'A' :
  case 'S' :
  case 'D' :
  case 'F' :
  case 'G' :
  case 'H' :
  case 'J' :
  case 'K' :
  case 'L' :
  case ':' :
  case '"' :
    return 3.0;
  case 'z' :
  case 'x' :
  case 'c' :
  case 'v' :
  case 'b' :
  case 'n' :
  case 'm' :
  case ',' :
  case '.' :
  case '/' :
  case 'Z' :
  case 'X' :
  case 'C' :
  case 'V' :
  case 'B' :
  case 'N' :
  case 'M' :
  case '<' :
  case '>' :
  case '?' :
    return 4.0;
  case ' ' :
    return 5.0;
  default :
    break;
  }
  return 0;
}

static double
qwerty_map_x(const char key)
{
  switch (key) {
  case '`' :
  case 'q' :
  case 'a' :
  case 'z' :
  case '~' :
  case 'Q' :
  case 'A' :
  case 'Z' :
    return 1.0;
  case '1' :
  case 'w' :
  case 's' :
  case 'x' :
  case '!' :
  case 'W' :
  case 'S' :
  case 'X' :
    return 2.0;
  case '2' :
  case 'e' :
  case 'd' :
  case 'c' :
  case '@' :
  case 'E' :
  case 'D' :
  case 'C' :
    return 3.0;
  case '3' :
  case 'r' :
  case 'f' :
  case 'v' :
  case '#' :
  case 'R' :
  case 'F' :
  case 'V' :
    return 4.0;
  case '4' :
  case 't' :
  case 'g' :
  case 'b' :
  case '$' :
  case 'T' :
  case 'G' :
  case 'B' :
  case ' ' :
    return 5.0;
  case '5' :
  case 'y' :
  case 'h' :
  case 'n' :
  case '%' :
  case 'Y' :
  case 'H' :
  case 'N' :
    return 6.0;
  case '6' :
  case 'u' :
  case 'j' :
  case 'm' :
  case '^' :
  case 'U' :
  case 'J' :
  case 'M' :
    return 7.0;
  case '7' :
  case 'i' :
  case 'k' :
  case ',' :
  case '&' :
  case 'I' :
  case 'K' :
  case '<' :
    return 8.0;
  case '8' :
  case 'o' :
  case 'l' :
  case '.' :
  case '*' :
  case 'O' :
  case 'L' :
  case '>' :
    return 9.0;
  case '9' :
  case 'p' :
  case ';' :
  case '/' :
  case '(' :
  case 'P' :
  case ':' :
  case '?' :
    return 10.0;
  case '0' :
  case '[' :
  case '\'' :
  case ')' :
  case '{' :
  case '"' :
    return 11.0;
  case '-' :
  case ']' :
  case '+' :
  case '}' :
    return 12.0;
  case '=' :
  case '\\' :
  case '|' :
    return 13.0;
  default :
    break;
  }
  return 0;
}

static double
euclidean_distance(double x1, double y1, double x2, double y2)
{
  double diffx = x1 - x2;
  double diffy = y1 - y2;
  double diffx_sqr = pow(diffx, 2);
  double diffy_sqr = pow(diffy, 2);
  double distance = sqrt(diffx_sqr + diffy_sqr);
  return distance;
}

static double
keyboard_distance(const char key1, const char key2)
{
  return euclidean_distance(
    qwerty_map_x(key1), qwerty_map_y(key1),
    qwerty_map_x(key2), qwerty_map_y(key2)
  );
}

#define DIST(ox,oy) (dists[((lx + 1) * (oy)) + (ox)])
#define DELETION_COST 1
#define INSERTION_COST 1
#define SUBSTITUTION_COST 1
#define TRANSPOSITION_COST 1

static grn_obj *
func_keyboard_distance(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
#define N_REQUIRED_ARGS 2
#define MAX_ARGS 4
  double d = 0;
  grn_obj *obj;
  if (nargs >= N_REQUIRED_ARGS && nargs <= MAX_ARGS) {
    uint32_t cx, lx, cy, ly;
    double *dists;
    char *px, *sx = GRN_TEXT_VALUE(args[0]), *ex = GRN_BULK_CURR(args[0]);
    char *py, *sy = GRN_TEXT_VALUE(args[1]), *ey = GRN_BULK_CURR(args[1]);
    unsigned int deletion_cost = 1;
    unsigned int insertion_cost = 1;
    unsigned int substitution_cost = 1;
    unsigned int transposition_cost = 1;
    grn_bool with_transposition = GRN_TRUE;
    grn_bool with_keyboard = GRN_TRUE;
    if (nargs == 3) {
      with_transposition = GRN_BOOL_VALUE(args[2]);
    }
    if (nargs == 4) {
      with_keyboard = GRN_BOOL_VALUE(args[3]);
    }
    if (with_keyboard) {
      deletion_cost = 10;
      insertion_cost = 10;
      substitution_cost = 10;
      transposition_cost = 10;
    }
    for (px = sx, lx = 0; px < ex && (cx = grn_charlen(ctx, px, ex)); px += cx, lx++);
    for (py = sy, ly = 0; py < ey && (cy = grn_charlen(ctx, py, ey)); py += cy, ly++);
    if ((dists = GRN_PLUGIN_MALLOC(ctx, (lx + 1) * (ly + 1) * sizeof(double)))) {
      uint32_t x, y;
      for (x = 0; x <= lx; x++) { DIST(x, 0) = x; }
      for (y = 0; y <= ly; y++) { DIST(0, y) = y; }
      for (x = 1, px = sx; x <= lx; x++, px += cx) {
        cx = grn_charlen(ctx, px, ex);
        for (y = 1, py = sy; y <= ly; y++, py += cy) {
          cy = grn_charlen(ctx, py, ey);
          if (cx == cy && !memcmp(px, py, cx)) {
            DIST(x, y) = DIST(x - 1, y - 1);
          } else {
            double a;
            double b;
            double c;
            a = DIST(x - 1, y) + deletion_cost;
            b = DIST(x, y - 1) + insertion_cost;
            c = DIST(x - 1, y - 1) + substitution_cost;
            if (with_keyboard) {
              b += keyboard_distance(px[1], py[0]);
              c += keyboard_distance(px[0], py[0]);
            }
            DIST(x, y) = ((a < b) ? ((a < c) ? a : c) : ((b < c) ? b : c));
            if (with_transposition && x > 1 && y > 1
                && cx == cy
                && memcmp(px, py - cy, cx) == 0
                && memcmp(px - cx, py, cx) == 0) {
              double t = DIST(x - 2, y - 2) + transposition_cost;
              DIST(x, y) = ((DIST(x, y) < t) ? DIST(x, y) : t);
            }
          }
        }
      }
      d = DIST(lx, ly);
      GRN_PLUGIN_FREE(ctx, dists);
    }
  }
  if ((obj = grn_plugin_proc_alloc(ctx, user_data, GRN_DB_FLOAT, 0))) {
    GRN_FLOAT_SET(ctx, obj, d);
  }
  return obj;
#undef N_REQUIRED_ARGS
#undef MAX_ARGS
}

/* func_edit_distance_bp() is only surported to 1byte char */
static grn_obj *
func_edit_distance_bp(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
#define N_REQUIRED_ARGS 2
#define MAX_ARGS 3
#define MAX_WORD_SIZE 64
  uint32_t score = 0;
  grn_obj *obj;
  if (nargs >= N_REQUIRED_ARGS && nargs <= MAX_ARGS) {
    unsigned int i, j;
    const unsigned char *search = (const unsigned char *)GRN_TEXT_VALUE(args[0]);
    unsigned int search_length = GRN_TEXT_LEN(args[0]);
    const unsigned char *compared = (const unsigned char *)GRN_TEXT_VALUE(args[1]);
    unsigned int compared_length = GRN_TEXT_LEN(args[1]);
    uint64_t char_vector[UCHAR_MAX] = {0};
    uint64_t top;
    uint64_t VP = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t VN = 0;
    grn_bool with_transposition = GRN_TRUE;
    if (nargs == MAX_ARGS) {
      with_transposition = GRN_BOOL_VALUE(args[2]);
    }
    if (search_length > MAX_WORD_SIZE) {
      goto exit;
    }
    top = (1ULL << (search_length - 1));
    score = search_length;
    for (i = 0; i < search_length; i++) {
      char_vector[search[i]] |= (1ULL << i);
    }
    for (j = 0; j < compared_length; j++) {
      uint64_t D0 = 0, HP, HN;
      if (with_transposition) {
        uint64_t PM[2];
        if (j > 0) {
          PM[0] = char_vector[compared[j - 1]];
        } else {
          PM[0] = 0;
        }
        PM[1] = char_vector[compared[j]];
        D0 = ((( ~ D0) & PM[1]) << 1ULL) & PM[0];
        D0 = D0 | (((PM[1] & VP) + VP) ^ VP) | PM[1] | VN;
      } else {
        uint64_t PM;
        PM = char_vector[compared[j]];
        D0 = (((PM & VP) + VP) ^ VP) | PM | VN;
      }
      HP = VN | ~(D0 | VP);
      HN = VP & D0;
      if (HP & top) {
        score++;
      } else if (HN & top) {
        score--;
      }
      VP = (HN << 1ULL) | ~(D0 | ((HP << 1ULL) | 1ULL));
      VN = D0 & ((HP << 1ULL) | 1ULL);
    }
  }
  if ((obj = grn_plugin_proc_alloc(ctx, user_data, GRN_DB_FLOAT, 0))) {
    GRN_FLOAT_SET(ctx, obj, score);
  }
exit:
  return obj;
#undef N_REQUIRED_ARGS
#undef MAX_ARGS
#undef MAX_WORD_SIZE
}

typedef struct {
  grn_obj *table;
  grn_token_mode mode;
  grn_obj value;
  grn_obj *index_column;
  grn_tokenizer_token token;
  double prefix_ratio;
  int distance_threshold;
  int min_length;
  int df_threshold;
} grn_typo_token_filter;

static void *
typo_init(grn_ctx *ctx, grn_obj *table, grn_token_mode mode)
{
  grn_typo_token_filter *token_filter;
  const char *env;

  if (mode != GRN_TOKEN_GET) {
    return NULL;
  }

  token_filter = GRN_PLUGIN_MALLOC(ctx, sizeof(grn_typo_token_filter));
  if (!token_filter) {
    GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                     "[token-filter][typo] "
                     "failed to allocate grn_typo_token_filter");
    return NULL;
  }
  token_filter->table = table;
  token_filter->mode = mode;
  GRN_TEXT_INIT(&(token_filter->value), 0);
  grn_tokenizer_token_init(ctx, &(token_filter->token));

  env = getenv("GRN_TERM_SIMILAR_DF_THRESHOLD");
  if (env) {
    token_filter->df_threshold = atoi(env);
  } else {
    token_filter->df_threshold = 0;
  }
  env = getenv("GRN_TERM_SIMILAR_MIN_LENGTH");
  if (env) {
    token_filter->min_length = atoi(env);
  } else {
    token_filter->min_length = 4;
  }
  env = getenv("GRN_TERM_SIMILAR_DISTANCE_THRESHOLD");
  if (env) {
    token_filter->distance_threshold = atoi(env);
  } else {
    token_filter->distance_threshold = 13;
  }
  env = getenv("GRN_TERM_SIMILAR_PREFIX_RATIO");
  if (env) {
    token_filter->prefix_ratio = atof(env);
  } else {
    token_filter->prefix_ratio = 0.8;
  }
  if (token_filter->df_threshold) {
    get_index_column(ctx, token_filter->table, &(token_filter->index_column));
  }

  return token_filter;
}

static void
typo_filter(grn_ctx *ctx,
             grn_token *current_token,
             grn_token *next_token,
             void *user_data)
{
  grn_typo_token_filter *token_filter = user_data;
  grn_id id;
  grn_obj *term;
  grn_obj *res;
  int prefix_term_length;

  if (!token_filter) {
    return;
  }

  term = grn_token_get_data(ctx, current_token);

  id = grn_table_get(ctx,
                     token_filter->table,
                     GRN_TEXT_VALUE(term),
                     GRN_TEXT_LEN(term));
  if (id != GRN_ID_NIL) {
    return;
  }

  prefix_term_length = (int)(token_filter->prefix_ratio * GRN_TEXT_LEN(term));
  prefix_term_length = cut_invalid_char_byte(ctx, prefix_term_length, term);
  if (prefix_term_length > GRN_TEXT_LEN(term) || token_filter->min_length > GRN_TEXT_LEN(term)) {
    return;
  }

  if ((res = grn_table_create(ctx, NULL, 0, NULL,
                              GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC, token_filter->table, NULL))) {
    predictive_search(ctx, token_filter->table, res, GRN_TEXT_VALUE(term), prefix_term_length);
    calc_keyboard_distance_with_df_boost(ctx, res, term,
                                         token_filter->distance_threshold,
                                         token_filter->index_column,
                                         token_filter->df_threshold);
    GRN_BULK_REWIND(&(token_filter->value));
    if (token_filter->index_column && token_filter->df_threshold) {
      get_best_match_record(ctx, res, &(token_filter->value), "-_score", strlen("-_score"));
    } else {
      get_best_match_record(ctx, res, &(token_filter->value), "_score", strlen("_score"));
    }
    /* unreadable */
    if (GRN_TEXT_LEN(&(token_filter->value)) &&
        !(GRN_TEXT_LEN(term) == GRN_TEXT_LEN(&(token_filter->value)) &&
         strncmp(GRN_TEXT_VALUE(term), GRN_TEXT_VALUE(&(token_filter->value)),
         GRN_TEXT_LEN(&(token_filter->value))) == 0)) {
      grn_token_set_data(ctx, next_token,
                         GRN_TEXT_VALUE(&(token_filter->value)),
                         GRN_TEXT_LEN(&(token_filter->value)));
    }
    grn_obj_close(ctx, res);
  } else {
    GRN_PLUGIN_LOG(ctx, GRN_LOG_ERROR, "[typo_filter] cannot create temporary table.");
  }

}

static void
typo_fin(grn_ctx *ctx, void *user_data)
{
  grn_typo_token_filter *token_filter = user_data;
  if (!token_filter) {
    return;
  }
  grn_tokenizer_token_fin(ctx, &(token_filter->token));
  grn_obj_unlink(ctx, &(token_filter->value));
  GRN_PLUGIN_FREE(ctx, token_filter);
}

grn_rc
GRN_PLUGIN_INIT(GNUC_UNUSED grn_ctx *ctx)
{
  return GRN_SUCCESS;
}

grn_rc
GRN_PLUGIN_REGISTER(grn_ctx *ctx)
{
  grn_expr_var vars[6];

  grn_proc_create(ctx, "keyboard_distance", -1, GRN_PROC_FUNCTION,
                  func_keyboard_distance, NULL, NULL, 0, NULL);

  grn_proc_create(ctx, "edit_distance_bp", -1, GRN_PROC_FUNCTION,
                  func_edit_distance_bp, NULL, NULL, 0, NULL);

  grn_plugin_expr_var_init(ctx, &vars[0], "term", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[2], "prefix_length", -1);
  grn_plugin_expr_var_init(ctx, &vars[3], "distance_threshold", -1);
  grn_plugin_expr_var_init(ctx, &vars[4], "df_threshold", -1);
  grn_plugin_expr_var_init(ctx, &vars[5], "limit", -1);
  grn_plugin_command_create(ctx, "table_term_similar", -1, command_table_term_similar, 6, vars);

  grn_proc_create(ctx, "term_similar", -1, GRN_PROC_FUNCTION,
                  func_term_similar, NULL, NULL, 0, NULL);

  grn_token_filter_register(ctx,
                            "TokenFilterTypo", -1,
                            typo_init,
                            typo_filter,
                            typo_fin);

  return ctx->rc;
}

grn_rc
GRN_PLUGIN_FIN(GNUC_UNUSED grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
