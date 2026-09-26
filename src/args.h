/*
 * args.h — shared, dependency-free command line parsing for diskdive.
 *
 * Every subcommand declares a table of dd_option entries and hands its
 * argv slice to dd_args_parse(). The parser understands:
 *   --long            --long value    --long=value
 *   -s value          -svalue         clustered flags (-abc)
 *   "--" (everything after it is positional)
 * Global options (--json, --si, --color, --no-color, -h/--help) are
 * recognized anywhere without being part of a command table.
 */
#ifndef DD_ARGS_H
#define DD_ARGS_H

#include <stddef.h>
#include <stdio.h>

#include "util.h"

/** Number of elements of a static array. */
#define DD_NELTS(a) (sizeof(a) / sizeof((a)[0]))

/** Option descriptor. @p id must be unique inside one command table. */
typedef struct {
    const char *lname;   /**< long name without "--", never NULL        */
    char sname;          /**< short name, '\0' when none                */
    int has_arg;         /**< 1 when the option takes a value           */
    int id;              /**< value stored in dd_pair.id                */
    const char *arghint; /**< meta-var for help ("N", "SIZE", ...)      */
    const char *help;    /**< one-line description for --help           */
} dd_option;

/** Ids reserved by the parser for global options. */
enum {
    DD_ID_JSON = 1001,   /**< --json      */
    DD_ID_SI = 1002,     /**< --si        */
    DD_ID_COLOR_ON = 1003,   /**< --color    */
    DD_ID_COLOR_OFF = 1004,  /**< --no-color */
    DD_ID_HELP = 1005    /**< -h / --help */
};

/** One parsed option occurrence. @p val is NULL for flags. */
typedef struct {
    int id;
    const char *val;     /**< borrowed pointer into argv, never freed */
} dd_pair;

/** Parse result: option pairs plus positional arguments. */
typedef struct {
    dd_pair *pairs;      /**< all option occurrences in order */
    size_t npairs;
    size_t paircap;
    char **pos;          /**< positional arguments (borrowed) */
    size_t npos;
    size_t poscap;
} dd_args;

/** Shared global settings; a single instance is kept process-wide. */
typedef struct {
    int json;           /**< --json seen                       */
    int si;             /**< --si seen (decimal units)         */
    int color_mode;     /**< DD_COLOR_AUTO / FORCE_ON / OFF    */
    int help;           /**< -h / --help seen                  */
} dd_globals;

/** Access the process-wide globals (initialized to defaults). */
dd_globals *dd_globals_get(void);

void dd_globals_init(dd_globals *g);

/**
 * Parse @p argc/@p argv (argv[0] is expected to be the command name and
 * is skipped by the caller). Global options mutate @p g; command options
 * are appended to @p out. On success returns 0. On a syntax error returns
 * -1 and fills @p err ("unknown option '--foo'" etc.); callers should
 * pass that to dd_usage_error().
 */
int dd_args_parse(int argc, char **argv, const dd_option *tbl, size_t ntbl,
                  dd_globals *g, dd_args *out, char *err, size_t errn);

/** Release memory owned by @p a (strings are borrowed, not freed). */
void dd_args_free(dd_args *a);

/** 1 if option @p id was given at least once. */
int dd_args_has(const dd_args *a, int id);

/** Value of the LAST occurrence of @p id, or NULL when absent. */
const char *dd_args_get(const dd_args *a, int id);

/** Number of occurrences of @p id (for repeatable options). */
size_t dd_args_count(const dd_args *a, int id);

/** Value of the @p nth (0-based) occurrence of @p id, or NULL. */
const char *dd_args_get_nth(const dd_args *a, int id, size_t nth);

/** Render an aligned options table (used by every --help). */
void dd_print_options(FILE *out, const dd_option *tbl, size_t n);

/** Print "diskdive: <err>", a hint and the option table, then exit(2). */
void dd_usage_error(const char *cmd, const char *err,
                    const dd_option *tbl, size_t n);

/** Parse a strictly positive integer ("--depth 3"); 0 ok, -1 on error. */
int dd_parse_count(const char *s, size_t *out, char *err, size_t errn);

#endif /* DD_ARGS_H */
