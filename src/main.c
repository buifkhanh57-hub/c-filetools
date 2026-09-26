/*
 * main.c — diskdive entry point: global options, command dispatch and
 * the top-level help screen.
 *
 * diskdive — a professional disk-usage analyzer for POSIX systems.
 * Copyright (c) 2026 Bui Bao Khanh. MIT license (see README.md).
 *
 * Layout of a command line:
 *     diskdive [global options] <command> [command options] [PATH]
 * Global options (--json --si --color --no-color -h/--help) are accepted
 * both before and after the command word; the subcommand parsers share
 * one dd_globals instance, so both positions behave identically.
 */
#include "args.h"
#include "util.h"

#include "ages.h"
#include "clean.h"
#include "du.h"
#include "dupfind.h"
#include "report.h"
#include "top.h"
#include "tree.h"
#include "types.h"

#include <string.h>

typedef struct {
    const char *name;    /**< command word                       */
    const char *summary; /**< one-line description for --help    */
    int (*run)(int argc, char **argv); /**< *_cmd entry point    */
} dd_command;

static const dd_command g_commands[] = {
    { "du",     "per-directory disk usage, sorted and with bars", du_cmd },
    { "top",    "the N largest files (streaming min-heap)",      top_cmd },
    { "dups",   "duplicate files: size + hash + byte-exact",      dups_cmd },
    { "types",  "usage breakdown by file extension",              types_cmd },
    { "ages",   "file age distribution in five buckets",          ages_cmd },
    { "tree",   "depth-limited tree view with exact sizes",       tree_cmd },
    { "clean",  "cache/tmp junk candidates (dry run by default)", clean_cmd },
    { "report", "everything in one overview (text or --json)",    report_cmd }
};

#define DD_NCOMMANDS (sizeof(g_commands) / sizeof(g_commands[0]))

/* ------------------------------------------------------------------ */
/* Top-level help                                                      */
/* ------------------------------------------------------------------ */

static void main_usage(FILE *out)
{
    size_t i;

    dd_fprintf(out,
        "%s %s — a professional disk-usage analyzer for POSIX systems\n\n"
        "Usage: %s <command> [PATH] [options]\n\n"
        "Commands:\n",
        dd_progname, DD_VERSION, dd_progname);
    for (i = 0; i < DD_NCOMMANDS; i++) {
        dd_fprintf(out, "  %-8s %s\n", g_commands[i].name, g_commands[i].summary);
    }
    DD_FPUTS("\nGlobal options (accepted anywhere, before or after the command):\n"
             "  --json          machine-readable output where supported\n"
             "  --si            decimal units (KB/MB) instead of binary (KiB/MiB)\n"
             "  --color         force ANSI colors (default: auto)\n"
             "  --no-color      never emit ANSI colors (also: NO_COLOR env)\n"
             "  -h, --help      show help for the top level or for <command>\n"
             "  --version       print version and exit\n"
             "\nExamples:\n"
             "  diskdive du /var --depth 1\n"
             "  diskdive dups ~/photos\n"
             "  diskdive clean .            (dry run — nothing is deleted)\n"
             "  diskdive report . --json\n"
             "\nExit codes: 0 ok · 1 runtime error · 2 usage error\n"
             "Documentation: see README.md. License: MIT.\n", out);
}

/* ------------------------------------------------------------------ */
/* Pre-command option scan                                             */
/* ------------------------------------------------------------------ */

/**
 * Consume global options that appear before the command word.
 * Returns the index of the command word in @p argv, @p argc when no
 * command follows, or -1 after printing an error (caller returns 2).
 * Side effects: may print the top-level help/version and ask the caller
 * to exit 0 (via @p done).
 */
static int scan_global_options(int argc, char **argv, dd_globals *g, int *done)
{
    int i = 1;

    *done = 0;
    for (; i < argc; i++) {
        const char *tok = argv[i];

        if (tok[0] != '-' || tok[1] == '\0') {
            return i; /* the command word (or "-" / a path) */
        }
        if (strcmp(tok, "--") == 0) {
            return (i + 1 < argc) ? i + 1 : argc;
        }
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            main_usage(stdout);
            *done = 1;
            return argc;
        }
        if (strcmp(tok, "--version") == 0 || strcmp(tok, "-V") == 0) {
            dd_fprintf(stdout, "%s %s\n", dd_progname, DD_VERSION);
            *done = 1;
            return argc;
        }
        if (strcmp(tok, "--json") == 0) {
            g->json = 1;
        } else if (strcmp(tok, "--si") == 0) {
            g->si = 1;
        } else if (strcmp(tok, "--color") == 0) {
            g->color_mode = DD_COLOR_FORCE_ON;
        } else if (strcmp(tok, "--no-color") == 0) {
            g->color_mode = DD_COLOR_FORCE_OFF;
        } else {
            dd_error("unknown global option '%s'", tok);
            dd_fprintf(stderr, "Try '%s --help' for more information.\n", dd_progname);
            return -1;
        }
    }
    return argc;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    dd_globals *g = dd_globals_get();
    const dd_command *cmd = NULL;
    size_t i;
    int idx, done = 0;

    dd_set_progname(dd_basename(argv[0]));
    dd_globals_init(g);

    idx = scan_global_options(argc, argv, g, &done);
    if (done) {
        return DD_EXIT_OK;
    }
    if (idx < 0) {
        return DD_EXIT_USAGE;
    }
    dd_color_apply(g->color_mode);

    if (idx >= argc) {
        dd_error("no command given (try '%s --help')", dd_progname);
        return DD_EXIT_USAGE;
    }

    for (i = 0; i < DD_NCOMMANDS; i++) {
        if (strcmp(g_commands[i].name, argv[idx]) == 0) {
            cmd = &g_commands[i];
            break;
        }
    }
    if (!cmd) {
        dd_error("unknown command '%s' (try '%s --help')", argv[idx], dd_progname);
        return DD_EXIT_USAGE;
    }

    /* *_cmd expects argv[0] to be the command word itself */
    return cmd->run(argc - idx, argv + idx);
}
