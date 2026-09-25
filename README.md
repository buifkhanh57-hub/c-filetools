# c-filetools

Small file utilities written in portable C11. Currently ships `wc` —
a miniature word-count tool supporting `-l` (lines), `-w` (words) and
`-c` (bytes) flags, multiple files, stdin, and totals.

## Build & run
```bash
make run
printf 'hello world\nsecond line\n' | ./build/wc -l -w
```
