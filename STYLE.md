# Coding style

Rules for the C and shell code in this repository.  Code that breaks them
is fixed, not excused.  Comments, documentation and commit messages are in
English.

## C

### Base

C follows the NetBSD kernel normal form (KNF), `/usr/share/misc/style` on
NetBSD.  The points that matter most, and the choices KNF leaves open:

- One tab (8 columns) per level; continuation lines get 4 more spaces.
  Lines are at most 80 columns.
- The return type of a function definition goes on a line of its own; the
  function name starts in column 1; the opening brace of a function is on
  a line of its own.  Other braces stay on the line of their statement.
- A space after keywords (`if`, `for`, `while`, `switch`, `return`), none
  after function names, casts or `sizeof`.  `sizeof(x)` is always
  parenthesised; `return` values are not.
- Braces around a single-statement body are left out, unless another
  branch of the same `if` needs them; then every branch has them.
- Local variables are declared at the top of a block, sorted by size and
  then by name, and are not initialised in the declaration unless the
  value is a constant that never changes.
- Compare pointers with `NULL`, characters with `'\0'` and numbers with
  `0`; `!` only for booleans.  Compare system call results with `-1`.
- `(void)` in front of a call whose result is deliberately ignored,
  including `printf`-family calls in library code.
- `switch` cases are not indented relative to `switch`; a case that falls
  through says `/* FALLTHROUGH */`.

### Language and portability

- C99 and POSIX.1-2008, plus `<err.h>`.  No compiler extensions and no
  platform `#ifdef`s outside one compatibility header.
- The code must build without warnings under gcc and clang with the flags
  in the Makefile and `-Werror`, on Linux, NetBSD and MINIX 3.
- Fixed-width types (`uint16_t`, `uint32_t`, `uint64_t`) for values that
  come from or go to the disk; `size_t` for sizes in memory; `off_t` for
  file offsets.  Print them with `<inttypes.h>` macros (`PRIu32`) or cast
  to `uintmax_t` and use `%ju`; never cast to `unsigned` to fit `%u`.
- Arithmetic that can overflow is done in a wider type and checked before
  it is narrowed.  A cast that silences a warning needs a comment saying
  why it is safe.

### Structure

- `src/mfs.[ch]` is the library.  It never prints, never exits and has no
  global state: everything lives in `struct mfs`.  Every function returns
  0 or a byte count on success and a negative `errno` value on failure.
- Every function that is not part of the library interface is `static`.
  Every library function is declared in `mfs.h` with a comment that says
  what it does, what it returns and what the caller owns.
- A function does one thing and fits on a screen (about 60 lines).  Long
  command implementations are split into helpers.
- On-disk layouts are described once, as named constants (offsets and
  sizes per version), next to a comment that shows the layout.  No bare
  numeric offsets in code.
- No fixed-size buffers for data whose size comes from outside (paths,
  names, file contents) unless the size is checked and overflow is
  reported.  No `static` buffers inside functions.
- `goto` only to jump to the cleanup code at the end of a function.

### Safety

- Everything read from an image is untrusted.  Every number that is used
  as an index, a count, a block or zone number or a size is checked
  against the limits of the file system before it is used.
- Every allocation and every system call is checked.
- A command reports each problem once, with the image and path it
  concerns, and goes on where it can.  Exit status: 0 success, 1 failure,
  2 usage error.  Messages go through `warn`/`warnx` from `<err.h>`.

### Comments

- Every file starts with a paragraph on what the file is for.
- Comments say why, or give the rule the code follows (with the source of
  the on-disk format where it matters).  They do not repeat the code.
- Multi-line comments are sentences, in KNF block form.

## Shell

### Base

- POSIX `sh` only.  The scripts must run under the NetBSD `sh` (the
  `/bin/sh` of MINIX 3), dash, `bash --posix` and busybox `sh`.  Not
  allowed: `local`, arrays, `[[ ]]`, `function`, `source`, `$'...'`,
  `echo -n`/`echo -e` (use `printf`), `==` in `test`, process
  substitution, `{a,b}` brace expansion.
- One tab per level; lines at most 80 columns; `then` and `do` on the
  same line as `if`, `while` and `for`.
- `$(...)`, never backquotes; `$((...))` for arithmetic.
- Every expansion is quoted, except where word splitting is intended;
  such a place has a comment.
- `if ...; then ...; else ...; fi` for choices.  Never `a && b || c`: if
  `b` fails, `c` runs as well.
- No `set -e`; each status that matters is checked explicitly.

### Functions and variables

- Function names are lower case with underscores.  Variables that only
  one function uses start with `_` and the function name
  (`_poke_bytes`), since POSIX `sh` has no `local`.
- Global variables of a script are lower case and set near the top;
  environment variables that the user may set are upper case and have
  a default given with `: "${NAME:=default}"`.

### Tests

- Each script sources `tests/lib.sh`, keeps its files under `$T`, and
  ends with `finish`.
- Every check goes through a `check_*` helper of `lib.sh`, which prints
  one TAP line.  Scripts do not call `pass` and `fail`; a check that the
  helpers cannot express gets a new helper.  `skip` is for checks that
  cannot run where the suite runs, such as those that need util-linux.
- A check name is `<variant>: <what should hold>`, as a statement.
- A check tests one fact.  When a failure is reported, the lines under
  it say what was expected and what came out.
- Offsets and sizes of on-disk structures come from helpers in `lib.sh`
  that know the layout of each version; test scripts do not spell out
  byte offsets.
- Test data that several scripts use lives in files under `tests/`.
- awk programs longer than a few lines live in files of their own
  (`tests/*.awk`) and use POSIX awk only.

## Editors

`.editorconfig` records the whitespace rules above for editors that read
it.

## Checks

- `make check` and `make check-sanitize` pass on Linux and NetBSD.
- `TEST_SHELL=dash`, `TEST_SHELL="bash --posix"` and
  `TEST_SHELL="busybox sh"` pass.
- Where `shellcheck` is installed, `shellcheck -s sh` reports nothing
  for the scripts; a warning that is deliberately ignored is disabled on
  the line with a comment giving the reason.
