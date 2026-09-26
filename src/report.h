/*
 * report.h — combined summary report (`report` command).
 *
 * The report composes the analysis cores of the other subcommands into
 * one compact overview:
 *   overview (totals) · largest directories · largest files ·
 *   extensions · age distribution · duplicates (quick scan) ·
 *   clean candidates.
 * It emits either aligned text or a single JSON document (--json).
 */
#ifndef DD_REPORT_H
#define DD_REPORT_H

#include <stdio.h>

#include "scanner.h"
#include "util.h"

/** Options for report_run(). */
typedef struct {
    int quick_dups; /**< 1 = hash only the first 64 KiB per file (default) */
    int si;         /**< 1 = decimal units                                 */
} report_options;

/**
 * Build and print the report for @p root.
 * @p json selects the machine-readable document.
 * Returns DD_EXIT_OK or DD_EXIT_RUNTIME.
 */
int report_run(const char *root, const scan_options *so,
               const report_options *o, int json, FILE *out);

/** Entry point of the `diskdive report` subcommand (argv[0] = "report"). */
int report_cmd(int argc, char **argv);

#endif /* DD_REPORT_H */
