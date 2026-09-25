/*
 * c-filetools — wc.c
 * A miniature word-count utility (like wc) with multiple file support.
 * Build: make
 * Usage: ./wc [-l] [-w] [-c] [file ...]
 *   -l  count lines only
 *   -w  count words only
 *   -c  count bytes only
 * Without flags all three are printed. With no files, reads stdin.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int lines;
    int words;
    int bytes;
} Counts;

static void count_stream(FILE *fp, Counts *out) {
    int c;
    int prev_space = 1;
    while ((c = fgetc(fp)) != EOF) {
        out->bytes++;
        if (c == '\n') out->lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            prev_space = 1;
        } else if (prev_space) {
            out->words++;
            prev_space = 0;
        }
    }
}

static void print_counts(const char *name, const Counts *c, int show_l, int show_w, int show_c) {
    if (!show_l && !show_w && !show_c) {
        printf("%7d %7d %8d  %s\n", c->lines, c->words, c->bytes, name);
        return;
    }
    if (show_l) printf("%7d ", c->lines);
    if (show_w) printf("%7d ", c->words);
    if (show_c) printf("%8d ", c->bytes);
    printf("  %s\n", name);
}

static int process_file(const char *path, const Counts *total_out_hint,
                        int show_l, int show_w, int show_c) {
    FILE *fp = stdin;
    const char *name = "-";
    if (strcmp(path, "-") != 0) {
        fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, "wc: cannot open %s\n", path);
            return 1;
        }
        name = path;
    }
    Counts c = {0, 0, 0};
    count_stream(fp, &c);
    print_counts(name, &c, show_l, show_w, show_c);
    if (fp != stdin) fclose(fp);
    (void)total_out_hint;
    return 0;
}

int main(int argc, char **argv) {
    int show_l = 0, show_w = 0, show_c = 0;
    int file_args = 0;

    /* first pass: parse flags */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *p = argv[i] + 1; *p; p++) {
                switch (*p) {
                    case 'l': show_l = 1; break;
                    case 'w': show_w = 1; break;
                    case 'c': show_c = 1; break;
                    default:
                        fprintf(stderr, "wc: unknown flag -%c\n", *p);
                        return 2;
                }
            }
        } else {
            file_args++;
        }
    }

    int errors = 0;
    Counts total = {0, 0, 0};

    if (file_args == 0) {
        Counts c = {0, 0, 0};
        count_stream(stdin, &c);
        print_counts("-", &c, show_l, show_w, show_c);
        return 0;
    }

    /* second pass: process files, accumulate totals */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') continue;
        FILE *fp = fopen(argv[i], "rb");
        if (!fp) {
            fprintf(stderr, "wc: cannot open %s\n", argv[i]);
            errors++;
            continue;
        }
        Counts c = {0, 0, 0};
        count_stream(fp, &c);
        fclose(fp);
        print_counts(argv[i], &c, show_l, show_w, show_c);
        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
    }

    if (file_args > 1) {
        print_counts("total", &total, show_l, show_w, show_c);
    }
    return errors ? 1 : 0;
}
