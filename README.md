# diskdive

**A professional disk-usage analyzer for POSIX systems — one static binary, eight subcommands, zero dependencies.**

![Language](https://img.shields.io/badge/language-C11-blue)
![Standard](https://img.shields.io/badge/std-c11%20%7C%20POSIX.1--2008-informational)
![Build](https://img.shields.io/badge/build-make%20%7C%20-Wall%20-Wextra%20clean-success)
![Tests](https://img.shields.io/badge/tests-57%2F57%20passing-brightgreen)
![License](https://img.shields.io/badge/license-MIT-lightgrey)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20BSD-orange)

---

## Overview

`diskdive` answers the questions every developer eventually asks about a directory tree:

* *Where did all my disk space go?* → `du`, `tree`, `report`
* *Which files are the biggest?* → `top`
* *Do I have the same file stored twice?* → `dups`
* *What kind of data lives here?* → `types`
* *How stale is this tree?* → `ages`
* *What can I safely delete right now?* → `clean`

It is a single C11 binary with no runtime dependencies beyond libc. The walk
engine touches the filesystem exactly once per query, every number it prints
is derived from a full-depth traversal (limits only affect *rendering*, never
accuracy), and destructive operations are opt-in behind a dry run by default.

```
$ diskdive du /var --depth 1
Disk usage for /var (on-disk sizes, sorted by size)
   8.2 GiB    61.3%         12  █████████▍  /var/lib
   3.1 GiB    23.2%        104  ███▌        /var/log
   1.4 GiB    10.5%         31  █▌          /var/cache
 512.0 MiB     3.7%          6  ▌           /var/tmp
  96.0 MiB     0.7%          4  ░           /var/spool
  24.0 MiB     0.2%          1  ░           /var/opt
 4.0 KiB       0.0%          1  ░           /var/empty
Total: 13.4 GiB (12.9 GiB apparent) · 159 files · 9,412 directories
```

## Features

* **Exact totals, always.** `--depth` and `--top N` only limit what is
  *printed*; the walk itself is always full-depth, so "Total:" never lies.
* **Two size metrics.** On-disk usage (`st_blocks × 512`, what `df` cares
  about) by default, apparent size (`st_size`) with `--apparent`.
* **Human or machine.** Every analytical subcommand has a `--json` mode that
  emits a single, parseable document. Units: binary (KiB/MiB) by default,
  decimal (KB/MB) with `--si`; raw counts with `--bytes`.
* **Safe duplicate detection.** Size prefilter → streaming FNV-1a/64 hash →
  byte-exact `memcmp` verification, so hash collisions can never produce a
  false positive. Hard links are detected and reported, not double-counted.
* **Bounded memory.** `top` uses a streaming min-heap of N entries;
  `dups` groups by sorting index arrays — neither materializes the whole tree.
* **Defensive walking.** Symlinks are never followed unless `--follow` is
  given, directory loops are caught via `(dev, ino)` bookkeeping, unreadable
  entries are counted and reported while the walk continues.
* **Deterministic output.** Directory entries are visited in byte-sorted
  order per directory, so two runs over an unchanged tree print identical text.
* **Deletion you can trust.** `clean` is a dry run unless you type more
  flags; see [Safety notes](#safety-notes).
* **Zero configuration.** No config files, no daemon, no cache — run it and
  read the answer.

## Requirements

| Requirement | Notes                                        |
|-------------|----------------------------------------------|
| POSIX.1-2008 system | Linux, macOS, the BSDs all qualify   |
| C11 compiler        | `cc`, `clang`, `gcc` — anything C11  |
| GNU `make` or BSD `make` | only for the one-line build     |
| `sh`                | only to run `tests/run_tests.sh`     |

There are **no third-party dependencies**; everything is libc + POSIX.

## Installation

```sh
git clone <your-fork-url> diskdive
cd diskdive
make            # builds ./diskdive with -std=c11 -Wall -Wextra -O2
```

Install it on your `PATH` (optional):

```sh
sudo install -m 0755 diskdive /usr/local/bin/diskdive
```

Verify:

```sh
./diskdive --version
# diskdive 1.0.0
```

The build is strict: the Makefile pins `-std=c11 -Wall -Wextra` (they are not
overridable) and the tree compiles with **zero warnings**. `_XOPEN_SOURCE=700`
is defined by the build to expose the POSIX.1-2008 API surface that
`-std=c11`'s strict-ISO mode would otherwise hide.

## Quick Start

```sh
# 1. Where is the space? (top-level dirs of the current tree)
diskdive du .

# 2. The 10 biggest files under ~/Downloads
diskdive top ~/Downloads -n 10

# 3. Wasted bytes from duplicate photos
diskdive dups ~/Pictures

# 4. Is this checkout full of stale junk?
diskdive clean .          # dry run — deletes nothing
```

A first run on a fresh tree looks like this:

```sh
$ diskdive du ~/projects/webapp --depth 1 --sort files
Disk usage for /home/khanh/projects/webapp (on-disk sizes, sorted by file count)
   4.2 MiB    48.2%         83  ████████▎  /home/khanh/projects/webapp/src
   2.1 MiB    24.1%         41  ████▏       /home/khanh/projects/webapp/tests
   1.6 MiB    18.4%         12  ███         /home/khanh/projects/webapp/docs
 612.0 KiB     6.9%          5  █▏          /home/khanh/projects/webapp/scripts
 152.0 KiB     1.7%          3  ░           /home/khanh/projects/webapp/.github
Total: 8.7 MiB (8.4 MiB apparent) · 144 files · 7 directories
```

## Usage

```
diskdive [global options] <command> [PATH] [options]
```

Global options are accepted **anywhere** — before or after the command word —
because both parsers write into one process-wide settings struct:

| Option              | Effect                                              |
|---------------------|-----------------------------------------------------|
| `--json`            | machine-readable output where supported             |
| `--si`              | decimal units (KB, MB) instead of binary (KiB, MiB) |
| `--color`           | force ANSI colors even when piped                   |
| `--no-color`        | never emit ANSI colors (also honored: `NO_COLOR`)   |
| `-h`, `--help`      | top-level or per-command help                       |
| `--version`, `-V`   | print `diskdive 1.0.0` and exit                     |

Exit codes: `0` success · `1` runtime error (bad path, I/O failure) ·
`2` usage error (unknown option/command).

### Subcommands

| Command  | Purpose                                                              |
|----------|----------------------------------------------------------------------|
| `du`     | per-directory usage table with percentage bars, sortable             |
| `top`    | the N largest files via a bounded streaming min-heap                 |
| `dups`   | duplicate files: size prefilter → FNV-1a/64 → byte-exact verification |
| `types`  | usage aggregated by lowercased file extension + category             |
| `ages`   | file age distribution in five mtime buckets, newest/oldest highlights |
| `tree`   | depth-limited tree drawing with exact aggregated directory sizes      |
| `clean`  | cache/tmp junk candidates; **dry run by default**                     |
| `report` | all of the above condensed into one text or JSON overview             |

### `du`

```
Usage: diskdive du [PATH] [options]

Options:
  -d, --depth N        limit the printed depth (0 = unlimited, default 2)
  -s, --sort KEY       sort rows by size|name|files (default: size)
  -n, --top N          print only the first N rows (0 = all, default 40)
  -A, --apparent       rank by apparent size (st_size) instead of on-disk blocks
  -b, --bytes          print raw byte counts instead of human units
  -L, --follow         follow symbolic links (default: never)
  -x, --one-file-system
                       do not descend into other mount points
```

The walk always covers the whole tree; `--depth` only filters rows, so the
`Total:` line is exact at any depth. Percentage bars are relative to the root.

### `top`

```sh
$ diskdive top . -n 3 --bytes
Largest files in . (top 3, apparent sizes)
    #          SIZE   % TOTAL  LAST MODIFIED     PATH
   1     1,048,576     99.7%  2026-09-25 20:38  ./media/big.bin
   2         2,048      0.2%  2026-09-25 20:38  ./media/photo.jpg
   3           200      0.0%  2026-09-25 20:38  ./dup1.txt
11 files scanned, 1.0 MiB apparent
```

Options: `-n, --limit N` (default 20), `-m, --min-size SIZE` (suffix syntax:
`10K`, `1.5M`, `2GiB`), `-b, --bytes`, `-L, --follow`.

### `dups`

```sh
$ diskdive dups ~/Pictures
Duplicate files in /home/khanh/Pictures (FNV-1a/64, byte-exact verified)
Group 1: 3 files × 200 B — wasted 400 B
  [1] /home/khanh/Pictures/docs/deep/dup3.txt
  [2] /home/khanh/Pictures/dup1.txt
  [3] /home/khanh/Pictures/dup2.txt
Summary: 1 duplicate group · 2 redundant files · 400 B wasted (0.0% of scanned 1.0 MiB)
```

Options: `-m, --min-size SIZE` (default 1 byte), `-q, --quick` (hash only the
first 64 KiB — still byte-verified), `-s, --summary` (footer only),
`-b, --bytes`, `-L, --follow`.

Groups are ordered by wasted bytes (descending). Files whose hashes match but
contents differ are silently re-grouped by the verification pass; files that
share an inode (hard links) are counted once and reported as
`Skipped N hard-linked files (same inode)`.

### `types`

```sh
$ diskdive types . --bytes --top 4
Usage by extension in .
  EXT            FILES          SIZE        %  BAR         CATEGORY
  bin                2     1,048,586    99.7%  ██████████  binary
  jpg                1         2,048     0.2%  ░░░░░░░░░░  image
  txt                4           611     0.1%  ░░░░░░░░░░  document
  log                1            40     0.0%  ░░░░░░░░░░  other
Total: 11 files, 1,051,329 across 7 extension types
```

Extensions are lowercased, dotfiles map to `(none)`, and each row gets a
category label (source, headers, document, data, image, audio, video,
archive, binary, font, disk image, other). Options: `-n, --top N`,
`-s, --sort bytes|count|name`, `-b, --bytes`, `-L, --follow`.

### `ages`

```sh
$ diskdive ages .
File age distribution in . (by mtime)
  AGE              FILES          SIZE        %  BAR
  <1 day              10       1.0 MiB    90.9%  █████████░
  1-7 days             0           0 B     0.0%  ░░░░░░░░░░
  7-30 days            0           0 B     0.0%  ░░░░░░░░░░
  30-365 days          0           0 B     0.0%  ░░░░░░░░░░
  >1 year              1          10 B     9.1%  █░░░░░░░░░
Newest: ./.cache/blob.bin (2026-09-25 20:38, 1m ago)
Oldest: ./junk.tmp (2020-01-01 00:00, 6.7y ago)
```

Options: `-b, --bytes`, `-L, --follow`.

### `tree`

```sh
$ diskdive tree . --ascii --depth 2
. [1.0 MiB]
|-- media/ [1.0 MiB]
|   |-- big.bin  1.0 MiB
|   `-- photo.jpg  2.0 KiB
|-- docs/ [211 B]
|   `-- deep/ [211 B, ...]
|-- data.json  26 B
|-- dup1.txt  200 B
`-- dup2.txt  200 B

5 directories, 10 files
```

Directory sizes are exact (the walk is full-depth); frontier directories carry
a `, ...` marker. Options: `-d, --depth N` (default 3), `-a, --all` (include
hidden), `-A, --ascii`, `-s, --sort name|size`, `-b, --bytes`, `-L, --follow`.
`tree` deliberately rejects `--json` (the text drawing *is* the product).

### `clean`

See [Safety notes](#safety-notes) before using `--delete`.

```sh
$ diskdive clean .
Clean candidates in .
DRY RUN — nothing will be deleted
  [dir ]         10 B  ./.cache  (cache directory)
  [dir ]          8 B  ./build/node_modules  (cache directory)
  [file]         10 B  ./junk.tmp  (matched *.tmp, 2459d old)
  [file]         40 B  ./notes.log  (matched *.log, 0d old)
4 items · 68 B reclaimable
Re-run with --delete to remove these items.
```

Built-in file rules: `*.tmp *.temp *.swp *.swo *.bak *~ *.log .DS_Store`.
Built-in directory rules: `__pycache__ node_modules .cache .pytest_cache
.mypy_cache`. Options: `-g, --glob PATTERN` and `-r, --dir NAME` extend the
rules, `-o, --older-than DAYS` filters by age, `-d, --delete` enables
deletion, `-y, --yes` skips the typed confirmation, `-b, --bytes`,
`-L, --follow`. `clean` rejects `--json`.

### `report`

```sh
$ diskdive report . --json > report.json   # one machine-readable document
$ diskdive report .
diskdive report — .

== Overview ==
  Size:      1.0 MiB on disk (1.0 MiB apparent)
  Entries:   11 files, 6 directories, 0 symlinks

== Largest directories ==
  1.      1.0 MiB   96.6%  ██████████  ./media
  ...

== Duplicates ==
  1 group · 2 redundant files · 400 B wasted (quick scan)

== Clean candidates ==
  4 items · 68 B reclaimable (dry run; see 'diskdive clean')
```

Sections: Overview · Largest directories · Largest files · Extensions · Age ·
Duplicates · Clean candidates. Options: `-f, --full` hashes whole files in the
duplicates section instead of the default 64 KiB quick scan.

## Output formats

* **Text** (default): aligned tables, Unicode bars and box drawing
  (`--ascii` downgrades `tree`). Colors are auto-detected from the terminal,
  forced with `--color`, suppressed with `--no-color` or the `NO_COLOR`
  environment variable.
* **JSON** (`--json`): one document per run — `du` nests directories
  recursively (`children` arrays, one level per depth, sorted by the chosen
  key), `top`/`dups`/`types`/`ages` emit flat item lists, `report` merges
  every section into a single object. All JSON is escaped for control
  characters and safe to feed to `jq` or any parser:

```sh
$ diskdive --json dups . | python3 -m json.tool
{
    "path": ".",
    "min_size": 1,
    "quick": false,
    "groups": 1,
    "redundant_files": 2,
    "wasted_bytes": 400,
    "hardlink_skips": 0,
    "items": [
        {
            "size": 200,
            "files": 3,
            "wasted": 400,
            "paths": ["./docs/deep/dup3.txt", "./dup1.txt", "./dup2.txt"]
        }
    ]
}
```

## Project Structure

```
diskdive/
├── Makefile              one-target build; pins -std=c11 -Wall -Wextra
├── LICENSE               MIT
├── README.md             this file
├── .gitignore            build artifacts
├── tests/
│   └── run_tests.sh      57 end-to-end assertions (sh, POSIX)
└── src/                  ~6.3k lines of C11
    ├── main.c            entry point: global options, dispatch, top help
    ├── args.c/.h         shared option parser (long/short/clustered/--)
    ├── util.c/.h         allocation, strbuf, sizes, time, paths, colors, JSON
    ├── scanner.c/.h      the only filesystem walker (visitors, loop guard)
    ├── hash.c/.h         streaming FNV-1a/64 + chunked file hashing
    ├── du.c/.h           post-order aggregation tree + du table/JSON
    ├── top.c/.h          bounded min-heap ranking + top table/JSON
    ├── dupfind.c/.h      3-stage duplicate pipeline + group report/JSON
    ├── types.c/.h        extension buckets + category map + table/JSON
    ├── ages.c/.h         five mtime buckets + newest/oldest + table/JSON
    ├── tree.c/.h         box-drawing printer on top of the du tree
    ├── clean.c/.h        junk rules, dry-run printer, guarded rm -rf
    └── report.c/.h       composes every core into text/JSON overview
```

## Architecture

```
            main.c  ── global opts, dispatch
               │
        ┌──────┼───────────────────────────────┐
        ▼      ▼        ▼        ▼       ▼      ▼
       du     top     dups    types   ages   clean     (report reuses all)
        │      │        │        │       │      │
        └──────┴────────┴────────┴───────┴──────┘
                        │  scan_walk / scan_collect_files
                        ▼
                    scanner.c  ── readdir, lstat, loop guard
                        │
                      hash.c  (dups/report only)
```

Design decisions worth knowing:

1. **One walker.** Only `scanner.c` calls `readdir()`. Every feature is a
   visitor (`on_file`, `on_dir_pre/post`, `on_symlink`, `on_error`) over the
   same deterministic walk, so symlink policy, hidden handling and error
   tolerance behave identically everywhere.
2. **Post-order aggregation.** `du_analyze()` builds an in-memory tree where
   each directory rolls its own bytes plus its children's into `total_*`.
   Printing, sorting, percentages and JSON are pure memory operations —
   the filesystem is touched once.
3. **The depth split.** Traversal is always full-depth; `--depth` only
   limits *rendering* (table rows, JSON nesting, tree drawing). That is why
   totals stay exact while output stays small.
4. **Verification over trust.** `dups` never believes a hash match: groups
   are confirmed with chunked `memcmp` re-reads. Collisions cost one extra
   comparison; false positives are impossible by construction.
5. **Bounded resources.** `top` keeps N entries in a min-heap; `dups` sorts
   index arrays instead of hashing into tables; the walker grows path buffers
   dynamically instead of relying on `PATH_MAX`.
6. **Colors as a layer.** Every emitter asks `dd_c(code)`, a single global
   switch resolved once per run from `--color/--no-color`, `NO_COLOR`, `TERM`
   and `isatty()`.

## Safety notes

Deletion is the one thing a disk tool must never get wrong, so `clean` is
built as a sequence of guards:

* **Dry run is the default.** Without `--delete` the command only prints
  candidates under a `DRY RUN — nothing will be deleted` banner.
* **Confirmation.** `--delete` without `--yes` requires typing `yes` on an
  interactive terminal; on a pipe it refuses and exits `1` rather than guess.
* **The root is off-limits.** After resolving the scan root with
  `realpath()`, `/` is rejected outright.
* **No traversal games.** Deletion re-`lstat()`s each entry, unlinks
  symlinks instead of following them, and `rmdir()`s directories bottom-up.
* **Cache dirs are leaves.** Matching directories (`node_modules`,
  `.cache`, …) are measured but not descended into, so a nested
  `node_modules` inside `node_modules` appears exactly once.
* **Errors are counted, not swallowed.** A failed `unlink` increments the
  error counter, is reported in `Deleted N items · freed X · N errors`, and
  flips the exit code to `1`.

## Testing

```sh
make test          # builds if needed, then runs ./tests/run_tests.sh
```

The suite builds a deterministic tree in `/tmp` (three exact duplicates, an
aged `*.tmp`, nested directories, a hidden cache dir, a 1 MiB file), runs
every subcommand against it, and asserts on real output:

* `du` totals must equal the byte count computed by `find` — exact match;
* `dups` must find one group of 3 files with 400 B wasted;
* `clean` dry run must leave every candidate on disk, and `--delete` on a
  non-terminal must refuse;
* every `--json` variant is parsed with `python3 -m json.tool`;
* exit codes `0/1/2` are asserted for success, bad paths and usage errors;
* global options must behave identically before and after the command.

Current status: **57/57 passing.**

## FAQ

**Why on-disk sizes by default?**
Because `st_blocks × 512` is what the filesystem actually reserves and what
`df` reports. Pass `--apparent` (or `--bytes` with `du`) when you want the
sum of `st_size` values, e.g. for tarball estimates.

**Why is my 10-byte file "4.0 KiB"?**
That is the on-disk block granularity, not a bug. Use `--apparent` to see `10 B`.

**Is `dups` safe to trust?**
Yes. Hash matches are always confirmed byte-for-byte before being reported,
so the worst case is a slightly slower run, never a wrong group.

**Why doesn't `tree` support `--json`?**
The drawing *is* the output; for machine consumption `du --json` carries the
same aggregated tree with real numbers.

**Why do hidden files show up in `du` but not in `tree`?**
`du` is an accounting tool — hiding `.cache` would make totals disagree with
`df`. `tree` is a reading tool and follows the `ls` convention; pass
`--all` to include dot-entries.

**How does `--si` differ from `--bytes`?**
`--si` switches human units to powers of 1000 (KB, MB). `--bytes` switches
off human units entirely and prints comma-grouped raw counts.

**Does it handle symlink loops?**
Yes — with `--follow`, visited directories are tracked by `(dev, ino)` and
revisits are skipped; without `--follow` symlinks are never descended at all.

**What about permissions errors?**
Unreadable entries increment an error counter, print a warning, and the walk
continues; the summary line reports how many entries could not be read.

## Roadmap

* [ ] `export` subcommand: CSV/JSONL dumps of any report for spreadsheets
* [ ] Multi-threaded hashing for `dups` on large trees
* [ ] `watch` mode: incremental re-scan with inotify/FSEvents
* [ ] `--exclude` glob filters for the walker (all subcommands)
* [ ] Size-history snapshots (`diskdive snapshot` + `diff` between runs)
* [ ] Optional blake2b hashing mode for auditable duplicate reports

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Bui Bao Khanh.

---
**by Bui Bao Khanh**
