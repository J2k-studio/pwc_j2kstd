/*
 * jsh — PowerCode's own shell (phase 4→5: job control, tab completion, history,
 * line-editing, aliases, escapes, extra builtins)
 *
 * Currently supports:
 *   - Reads commands line by line from stdin
 *   - Variables ($x), $?, $$, export, unset
 *   - Chaining: &&, ||, ;
 *   - Pipes: |  (any number of stages)
 *   - Redirection: <, >, >>  (per pipeline stage)
 *   - Wildcards: *, ?, [abc]
 *   - Quoting: "..." (expands $vars, no glob), '...' (fully literal)
 *   - Backslash escapes: bare words (\x → x for any x, including spaces),
 *     double quotes (\" \\ \$ only — bash-compatible)
 *   - Strips comments (# ...), quote-aware
 *   - fork() + exec() to run real programs, then waits for them
 *   - built-ins: cd, pwd, exit, clear, history, unset, type, which,
 *     alias, unalias, export, jobs, fg, bg, break, continue,
 *     source, ., wait, tab-text
 *     (single-command only — not inside a pipeline)
 *   - Aliases: alias name='value' / alias name="value" / unalias name
 *     (expanded only for the first word, like bash)
 *   - command-not-found suggestions (edit distance against $PATH)
 *   - line-buffered stdout so jsh's own output never lags behind children
 *   - job control: each foreground command/pipeline gets its own process
 *     group and the terminal, so Ctrl+C only kills the running child, never jsh
 *   - persistent, cross-session command history (~/.jsh_history), Up/Down recall
 *   - Tab completion: command names (cached from $PATH at startup, not
 *     re-scanned per keystroke) or filenames (cwd, scanned live)
 *   - double-Tab completion list: first Tab on multiple matches does
 *     nothing, second Tab in a row shows the full list — avoids dumping
 *     a wall of text from one accidental press
 *   - dimmed ghost-text suggestion while typing: history first, falls back
 *     to the cached $PATH command list if history has no match (first
 *     word only — never guesses at filenames for later arguments)
 *   - `tab-text on|off` — master switch for both ghost-text and the
 *     double-Tab list, in case either turns out to be more distracting
 *     than helpful during beta
 *   - standard line-editing shortcuts: Ctrl+A/E (start/end), Ctrl+U/K (kill
 *     to start/end), Ctrl+W (delete word back), Ctrl+L (clear screen),
 *     Home/End, Delete, Left/Right, word-jump (Ctrl+Arrow, Alt+Arrow, Alt+B/F)
 *
 * Job control (phase 5):
 *   - background `&` (e.g. `sleep 30 &`)
 *   - `jobs`, `fg [%n]`, `bg [%n]`
 *   - Ctrl+Z suspend → stopped job; `fg`/`bg` to resume
 *   - automatic "Done" notification for finished background jobs
 *
 * Control flow (C-like):
 *   - if (cond) { ... } [else { ... }]
 *   - while (cond) { ... }
 *   - for (init; cond; step) { ... }
 *   - break / continue inside loops
 *   - multi-line blocks with continuation prompt `>`
 *   - conditions: true/false, == !=, -eq/-ne/-lt/-gt/-le/-ge,
 *     -f/-d/-e/-z/-n, ! neg, or a command (exit status)
 *
 * Also:
 *   - command substitution $(...) — nested ok; unquoted result stays one word
 *   - source / .  — run a script in the current shell
 *   - wait [job|pid] — wait for background jobs
 *   - Ctrl+R reverse-i-search through history
 *
 * Not yet supported:
 *   - shell functions, here-documents, bracketed paste
 *   - subshells (...), export NAME=value one-liner
 *   - read builtin, arrays, $((arith))
 *
 * Build:
 *   gcc -o jsh jsh.c
 *   ./jsh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>

#define JSH_MAX_LINE 1024
#define JSH_MAX_ARGS 64
#define JSH_MAX_SUGGESTIONS 3
#define JSH_SUGGEST_DISTANCE 2  /* max edit distance to be considered "close" */
#define JSH_MAX_VARS 128
#define JSH_MAX_VAR_NAME 64
#define JSH_MAX_VAR_VALUE 512
#define JSH_MAX_SEGMENTS 16   /* max commands chained with &&, ||, ; on one line */
#define JSH_MAX_HISTORY 500
#define JSH_MAX_COMPLETIONS 32
#define JSH_MAX_ALIASES 64
#define JSH_MAX_JOBS 16
#define JSH_MAX_STMT 8192   /* multi-line if/while/for block buffer */
#define JSH_MAX_LOOP_DEPTH 32

/* ── shell variables ──────────────────────────────────────────────
 * Plain variables ($name = value) live only inside jsh until `export`
 * is called on them — matching bash's behavior. Exported ones go into
 * the real process environment via setenv(), so child processes (python,
 * node, etc.) see them automatically. */
typedef struct {
    char name[JSH_MAX_VAR_NAME];
    char value[JSH_MAX_VAR_VALUE];
    int exported;
} jsh_var_t;

static jsh_var_t g_vars[JSH_MAX_VARS];
static int g_var_count = 0;

/* ── aliases ──────────────────────────────────────────────────────
 * Simple first-word aliases, matching bash's common case.
 * `alias ll='ls -la'` stores name="ll", value="ls -la".
 * On dispatch, if argv[0] matches an alias name, the value is
 * re-tokenized and spliced in place of the original first word.
 * No recursive expansion, no "alias -p" pretty-print beyond listing. */
typedef struct {
    char name[JSH_MAX_VAR_NAME];
    char value[JSH_MAX_VAR_VALUE];
} jsh_alias_t;

static jsh_alias_t g_aliases[JSH_MAX_ALIASES];
static int g_alias_count = 0;

/* Exit status of the most recently run command — what $? expands to.
 * Starts at 0 (success) before anything has run, matching bash. */
static int g_last_status = 0;

/* Loop control — break/continue set these; run_while/run_for observe them.
 * g_loop_depth > 0 means we are inside at least one while/for body. */
static int g_loop_depth = 0;
static int g_break_flag = 0;
static int g_continue_flag = 0;

static int eval_simple_arith(const char *expr, long *out); /* defined with control flow */
static int dispatch_line(char *line); /* defined later — used by $(...) and source */
static int statement_is_complete(const char *stmt); /* defined with control flow */

/* Master switch for the "extra" interactive typing aids: dimmed ghost-text
 * suggestions and the double-Tab completion list. Both are being rolled
 * out as a beta — `tab-text off` turns both off instantly without needing
 * a rebuild, in case they turn out to be more annoying than helpful.
 * Tab still does single-match completion either way; it's the visual/list
 * parts specifically that this gates. Default on. */
static int g_tab_text_enabled = 1;

/* Cache of every completable command name (builtins + everything executable
 * across $PATH), built once at startup instead of re-scanning $PATH's
 * directories on every single Tab press. A phone's storage is slower than
 * a desktop's, and re-running opendir()/readdir() across the whole PATH on
 * every keystroke-adjacent action is exactly the kind of thing that causes
 * the typing lag this feature was asked to avoid. */
#define JSH_MAX_PATH_CMDS 1024 /* initial capacity hint only — grows dynamically beyond this, see cache_path_commands() */
static char **g_path_cmds = NULL;
static int g_path_cmd_count = 0;
static int g_path_cmd_capacity = 0;

/* ── job control ──────────────────────────────────────────────────
 * Only active in interactive mode (a real terminal). Each foreground
 * command/pipeline gets its own process group, and the terminal is handed
 * to that group while it runs — this is what makes Ctrl+C during
 * execution hit ONLY the running child(ren), not jsh itself. Without this,
 * jsh and its children would share one process group, and a Ctrl+C could
 * kill jsh along with whatever it was running.
 *
 * Background jobs (`cmd &`), Ctrl+Z stop, and jobs/fg/bg are tracked in
 * g_jobs[]. Completed background jobs are reported as "Done" just before
 * the next prompt (polled, no SIGCHLD handler required). */
static int g_interactive = 0;
static pid_t g_shell_pgid = 0;

typedef enum { JOB_RUNNING = 0, JOB_STOPPED, JOB_DONE } jsh_job_state_t;

typedef struct {
    int              used;
    int              id;          /* 1-based job number shown to the user */
    pid_t            pgid;
    pid_t            pids[JSH_MAX_SEGMENTS];
    int              npids;
    char             cmdline[JSH_MAX_LINE];
    jsh_job_state_t  state;
    int              status;      /* wait status of the last process */
    int              notified;    /* 1 once "Done" has been printed */
} jsh_job_t;

static jsh_job_t g_jobs[JSH_MAX_JOBS];
static int g_next_job_id = 1;

static jsh_var_t *find_var(const char *name) {
    for (int i = 0; i < g_var_count; i++) {
        if (strcmp(g_vars[i].name, name) == 0) return &g_vars[i];
    }
    return NULL;
}

/* Set (or update) a shell variable. If it's already exported, keep the
 * real environment in sync too, so `export X; X = "new"` still updates
 * what child processes see. */
static void set_var(const char *name, const char *value) {
    jsh_var_t *v = find_var(name);
    if (v == NULL) {
        if (g_var_count >= JSH_MAX_VARS) {
            fprintf(stderr, "jsh: too many variables (limit %d)\n", JSH_MAX_VARS);
            return;
        }
        v = &g_vars[g_var_count++];
        snprintf(v->name, sizeof(v->name), "%s", name);
        v->exported = 0;
    }
    snprintf(v->value, sizeof(v->value), "%s", value);
    if (v->exported) {
        setenv(v->name, v->value, 1);
    }
}

/* `export NAME` — mark an existing shell variable as visible to child
 * processes from now on. Matches bash: export doesn't itself set a value,
 * it just promotes an existing (or empty) variable into the environment. */
static void export_var(const char *name) {
    jsh_var_t *v = find_var(name);
    if (v == NULL) {
        /* export a variable that doesn't exist yet — bash allows this too,
         * it just creates it empty and marks it exported */
        set_var(name, "");
        v = find_var(name);
    }
    v->exported = 1;
    setenv(v->name, v->value, 1);
}

/* Look up a variable's value for expansion. $? and $$ are handled specially
 * by the caller before this is reached. Falls back to the real environment
 * (so $HOME, $PATH etc. still work even though jsh never explicitly sets
 * them as shell variables), then to "" if nothing matches — bash-standard:
 * undefined variables expand to an empty string, not an error. */
static const char *lookup_var(const char *name) {
    jsh_var_t *v = find_var(name);
    if (v != NULL) return v->value;

    const char *env_val = getenv(name);
    if (env_val != NULL) return env_val;

    return "";
}

/* Remove a shell variable. If it was exported, also drop it from the
 * real environment so children no longer see it. Returns 0 on success,
 * 1 if the name was not found (still treated as non-fatal, like bash). */
static int unset_var(const char *name) {
    for (int i = 0; i < g_var_count; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            if (g_vars[i].exported) {
                unsetenv(g_vars[i].name);
            }
            /* compact the array */
            memmove(&g_vars[i], &g_vars[i + 1],
                    (size_t)(g_var_count - i - 1) * sizeof(jsh_var_t));
            g_var_count--;
            return 0;
        }
    }
    return 1;
}

/* ── aliases ─────────────────────────────────────────────────────── */
static jsh_alias_t *find_alias(const char *name) {
    for (int i = 0; i < g_alias_count; i++) {
        if (strcmp(g_aliases[i].name, name) == 0) return &g_aliases[i];
    }
    return NULL;
}

static void set_alias(const char *name, const char *value) {
    jsh_alias_t *a = find_alias(name);
    if (a == NULL) {
        if (g_alias_count >= JSH_MAX_ALIASES) {
            fprintf(stderr, "jsh: too many aliases (limit %d)\n", JSH_MAX_ALIASES);
            return;
        }
        a = &g_aliases[g_alias_count++];
        snprintf(a->name, sizeof(a->name), "%s", name);
    }
    snprintf(a->value, sizeof(a->value), "%s", value);
}

static int unset_alias(const char *name) {
    for (int i = 0; i < g_alias_count; i++) {
        if (strcmp(g_aliases[i].name, name) == 0) {
            memmove(&g_aliases[i], &g_aliases[i + 1],
                    (size_t)(g_alias_count - i - 1) * sizeof(jsh_alias_t));
            g_alias_count--;
            return 0;
        }
    }
    return 1;
}

/* ── command history ───────────────────────────────────────────────
 * Persisted to ~/.jsh_history so it survives across sessions — matches
 * the earlier decision that history should remember every command
 * permanently unless explicitly edited/cleared. Loaded once at startup,
 * appended to on disk immediately after each command (not just at exit),
 * so a crash or force-kill doesn't lose anything already typed. */
static char *g_history[JSH_MAX_HISTORY];
static int g_history_count = 0;
static char g_history_path[PATH_MAX];

static void history_init(void) {
    const char *home = getenv("HOME");
    if (home == NULL) home = "/";
    snprintf(g_history_path, sizeof(g_history_path), "%s/.jsh_history", home);

    FILE *f = fopen(g_history_path, "r");
    if (f == NULL) return; /* no history file yet — fine, starts empty */

    /* Two-pass read: a file with more than JSH_MAX_HISTORY lines (normal
     * after real, sustained use) must keep the MOST RECENT entries, not
     * whichever ones happen to come first. Reading top-down and stopping
     * at the cap (the original approach) silently loaded the OLDEST
     * commands instead — meaning every restart would throw away exactly
     * the history a person actually wants (what they just did) once the
     * file grew past the cap. Counting first, then skipping to the right
     * offset, keeps this correct without holding the whole file in memory. */
    char line[JSH_MAX_LINE];
    long total_lines = 0;
    while (fgets(line, sizeof(line), f) != NULL) total_lines++;

    long to_skip = total_lines - JSH_MAX_HISTORY;
    if (to_skip < 0) to_skip = 0;

    rewind(f);
    long skipped = 0;
    while (skipped < to_skip && fgets(line, sizeof(line), f) != NULL) skipped++;

    while (fgets(line, sizeof(line), f) != NULL && g_history_count < JSH_MAX_HISTORY) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (l == 0) continue;
        g_history[g_history_count++] = strdup(line);
    }
    fclose(f);
}

/* Rewrites ~/.jsh_history from scratch using the current in-memory history
 * array. Called when the in-memory list evicts its oldest entry (i.e. the
 * file would otherwise keep growing forever, one line appended per command,
 * across the entire lifetime of the app) — this keeps the on-disk file
 * bounded to roughly JSH_MAX_HISTORY lines too, compacted in one shot
 * instead of rewriting on every single command. */
static void history_compact_file(void) {
    FILE *f = fopen(g_history_path, "w");
    if (f == NULL) return;
    for (int i = 0; i < g_history_count; i++) {
        fprintf(f, "%s\n", g_history[i]);
    }
    fclose(f);
}

/* Adds `line` to in-memory history and appends it to the history file.
 * Skips empty lines and exact repeats of the immediately preceding entry
 * (typing the same command twice in a row shouldn't spam the list). */
static void history_add(const char *line) {
    if (line[0] == '\0') return;
    if (g_history_count > 0 && strcmp(g_history[g_history_count - 1], line) == 0) return;

    int evicted = 0;
    if (g_history_count >= JSH_MAX_HISTORY) {
        /* drop the oldest entry to make room — a fixed cap keeps memory
         * bounded even after a very long-running session */
        free(g_history[0]);
        memmove(&g_history[0], &g_history[1], (JSH_MAX_HISTORY - 1) * sizeof(char *));
        g_history_count--;
        evicted = 1;
    }
    g_history[g_history_count++] = strdup(line);

    if (evicted) {
        /* The in-memory list just dropped its oldest entry, which means
         * plain appending would let the on-disk file grow forever (every
         * command, for the entire lifetime of the app, never trimmed).
         * Rewriting from the now-correctly-bounded in-memory array keeps
         * the file capped too — a bit more I/O for this one command, but
         * only once per eviction, not on every line. */
        history_compact_file();
    } else {
        FILE *f = fopen(g_history_path, "a");
        if (f != NULL) {
            fprintf(f, "%s\n", line);
            fclose(f);
        }
    }
}

/* Strip comments: anything from an unquoted '#' onward is removed.
 * Quote-aware — a '#' inside "..." or '...' is just a literal character,
 * not the start of a comment (e.g. echo "price #1" must keep the #1). */
static void strip_comment(char *line) {
    char in_quote = '\0';
    for (char *p = line; *p != '\0'; p++) {
        if (in_quote != '\0') {
            if (*p == in_quote) in_quote = '\0';
            continue;
        }
        if (*p == '"' || *p == '\'') {
            in_quote = *p;
            continue;
        }
        if (*p == '#') {
            *p = '\0';
            return;
        }
    }
}

/* Trim trailing newline / whitespace */
static void rtrim(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                        line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
}

/* Expand every $-reference found anywhere inside `src` (not just whole-token
 * matches) — handles $?, $$, and $NAME, including cases like "$a-$b" or
 * "prefix$x". Stops each variable name at the first character that isn't
 * alphanumeric/underscore, same as bash. Unknown variables expand to "". */
/* Run `cmd` in a child, capture stdout into `out`. Trailing newlines are
 * stripped (bash behavior). Returns 0 on success. Uses fork + dispatch_line
 * so builtins, pipes, and variables all work inside $(...). */
static int run_command_capture(const char *cmd, char *out, size_t out_size) {
    out[0] = '\0';
    int pfd[2];
    if (pipe(pfd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pfd[0]);
        if (dup2(pfd[1], STDOUT_FILENO) < 0) _exit(1);
        close(pfd[1]);
        /* Child inherits shell variables via COW; dispatch_line mutates a
         * private copy. Job-control terminal hand-off is disabled here by
         * not being the interactive session leader path — we just run. */
        char buf[JSH_MAX_LINE];
        snprintf(buf, sizeof(buf), "%s", cmd);
        int r = dispatch_line(buf);
        _exit(r == -1 ? 0 : (g_last_status & 0xff));
    }

    close(pfd[1]);
    size_t oi = 0;
    char tmp[512];
    ssize_t n;
    while ((n = read(pfd[0], tmp, sizeof(tmp))) > 0) {
        for (ssize_t k = 0; k < n && oi + 1 < out_size; k++) {
            out[oi++] = tmp[k];
        }
    }
    out[oi] = '\0';
    close(pfd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    /* Strip all trailing newlines (bash: trailing newlines removed) */
    while (oi > 0 && (out[oi - 1] == '\n' || out[oi - 1] == '\r')) {
        out[--oi] = '\0';
    }
    return 0;
}

static void expand_string(const char *src, char *out, size_t out_size) {
    size_t oi = 0;
    for (size_t i = 0; src[i] != '\0' && oi + 1 < out_size; ) {
        if (src[i] != '$' || src[i + 1] == '\0') {
            out[oi++] = src[i++];
            continue;
        }

        i++; /* skip '$' */

        if (src[i] == '?') {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", g_last_status);
            for (size_t k = 0; buf[k] != '\0' && oi + 1 < out_size; k++) out[oi++] = buf[k];
            i++;
            continue;
        }

        if (src[i] == '$') {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", (int)getpid());
            for (size_t k = 0; buf[k] != '\0' && oi + 1 < out_size; k++) out[oi++] = buf[k];
            i++;
            continue;
        }

        /* Command substitution: $(...) with nested-paren awareness */
        if (src[i] == '(') {
            i++; /* skip '(' */
            int depth = 1;
            size_t start = i;
            while (src[i] != '\0' && depth > 0) {
                if (src[i] == '(') depth++;
                else if (src[i] == ')') depth--;
                if (depth > 0) i++;
            }
            if (depth != 0) {
                /* unmatched — emit literally */
                out[oi++] = '$';
                out[oi++] = '(';
                i = start;
                continue;
            }
            /* src[i] is the closing ')' */
            size_t clen = i - start;
            char cmd[JSH_MAX_LINE];
            if (clen >= sizeof(cmd)) clen = sizeof(cmd) - 1;
            memcpy(cmd, src + start, clen);
            cmd[clen] = '\0';
            i++; /* skip ')' */

            char captured[JSH_MAX_VAR_VALUE];
            if (run_command_capture(cmd, captured, sizeof(captured)) == 0) {
                for (size_t k = 0; captured[k] != '\0' && oi + 1 < out_size; k++)
                    out[oi++] = captured[k];
            }
            continue;
        }

        size_t name_start = i;
        while ((src[i] >= 'a' && src[i] <= 'z') || (src[i] >= 'A' && src[i] <= 'Z') ||
               (src[i] >= '0' && src[i] <= '9') || src[i] == '_') {
            i++;
        }

        if (i == name_start) {
            /* '$' followed by nothing variable-name-like — keep it literal */
            out[oi++] = '$';
            continue;
        }

        char name[JSH_MAX_VAR_NAME];
        size_t name_len = i - name_start;
        if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
        memcpy(name, src + name_start, name_len);
        name[name_len] = '\0';

        const char *val = lookup_var(name);
        for (size_t k = 0; val[k] != '\0' && oi + 1 < out_size; k++) out[oi++] = val[k];
    }
    out[oi] = '\0';
}

/* Split a line into argv[] by whitespace (basic — no quoted-string support yet) */
/* Storage for expanded tokens — tokenize() points argv[] entries here instead
 * of directly into the input line, since expansion can change the length. */
static char g_expanded[JSH_MAX_ARGS][JSH_MAX_VAR_VALUE];

/* True if `s` contains any character this shell treats as a glob metachar.
 * Kept intentionally small (*, ?, [) — matches the wildcard set the roadmap
 * asked for, not full POSIX glob (no brace expansion, no ~, etc). */
static int has_glob_chars(const char *s) {
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '*' || *p == '?' || *p == '[') return 1;
    }
    return 0;
}

/* A small built-in table of short descriptions for common commands, shown
 * next to each candidate in the double-Tab match list (e.g. "clang" alone
 * doesn't tell you it's a compiler — this does). Not exhaustive by design:
 * covers the commands people actually reach for on a dev-focused shell.
 * Anything not in this table just shows its bare name, which is still
 * useful, just without the extra context. */
typedef struct {
    const char *name;
    const char *desc;
} jsh_cmd_desc_t;

static const jsh_cmd_desc_t g_cmd_descriptions[] = {
    { "ls",      "list directory contents" },
    { "cd",      "change directory" },
    { "cat",     "print file contents" },
    { "cp",      "copy files" },
    { "mv",      "move/rename files" },
    { "rm",      "remove files" },
    { "mkdir",   "create directories" },
    { "chmod",   "change file permissions" },
    { "ps",      "list running processes" },
    { "kill",    "terminate a process" },
    { "find",    "search for files" },
    { "grep",    "search text with patterns" },
    { "awk",     "text processing" },
    { "sed",     "stream text editor" },
    { "tar",     "archive files (.tar)" },
    { "make",    "build automation" },
    { "ld",      "linker" },
    { "nano",    "text editor" },
    { "vi",      "text editor" },
    { "vim",     "text editor" },
    { "clang",   "C/C++ compiler, e.g. clang -O2 -o out file.c" },
    { "clang++", "C++ compiler, e.g. clang++ -O2 -o out file.cpp" },
    { "gcc",     "C compiler, e.g. gcc -O2 -o out file.c" },
    { "g++",     "C++ compiler, e.g. g++ -O2 -o out file.cpp" },
    { "clangd",  "C/C++ language server (editor autocomplete)" },
    { "clang-format", "reformat C/C++ source code" },
    { "clang-tidy",   "C/C++ linter / static analysis" },
    { "python",  "run Python scripts/interpreter" },
    { "python3", "run Python scripts/interpreter" },
    { "pip",     "install Python packages" },
    { "node",    "run JavaScript (Node.js)" },
    { "npm",     "install Node.js packages" },
    { "git",     "version control" },
    { "ssh",     "secure remote shell" },
    { "curl",    "transfer data from/to a URL" },
    { "wget",    "download files from the web" },
    { "alias",   "define or list command aliases" },
    { "unalias", "remove aliases" },
    { "clear",   "clear the terminal screen" },
    { "history", "show command history" },
    { "unset",   "remove shell variables" },
    { "type",    "describe how a command name is interpreted" },
    { "which",   "locate a command in $PATH" },
    { "export",  "mark variables for export to child processes" },
    { "pwd",     "print working directory" },
    { "exit",    "exit the shell" },
    { "jobs",    "list active jobs" },
    { "fg",      "resume job in foreground" },
    { "bg",      "resume job in background" },
    { "break",   "exit a for/while loop" },
    { "continue","next iteration of a for/while loop" },
    { "source",  "run a script in the current shell" },
    { ".",       "alias for source" },
    { "wait",    "wait for background jobs" },
    { NULL, NULL }
};

/* Returns the description for `name`, or NULL if not in the table. */
static const char *lookup_cmd_description(const char *name) {
    for (int i = 0; g_cmd_descriptions[i].name != NULL; i++) {
        if (strcmp(g_cmd_descriptions[i].name, name) == 0) {
            return g_cmd_descriptions[i].desc;
        }
    }
    return NULL;
}

/* qsort comparator for sorting matched filenames alphabetically, same as `ls` */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Builds g_path_cmds[] once: builtins + every executable name found across
 * every directory in $PATH, deduplicated and sorted. Meant to be called
 * once at startup (after setup_path() has finalized $PATH) — NOT on every
 * Tab press. If $PATH changes later (e.g. a future `pwc install` session),
 * this would need to be called again to pick up new commands; it isn't
 * re-run automatically since jsh doesn't currently modify $PATH mid-session. */
/* Appends `name` to g_path_cmds, growing the array (doubling capacity) as
 * needed. No artificial ceiling — on a $PATH with tens of thousands of
 * binaries (a full dev container, or a heavily-packaged Termux install),
 * a fixed cap would silently drop real commands from Tab completion
 * partway through the scan, in whatever order readdir() happened to
 * return them (not alphabetical) — which is exactly the bug found during
 * testing: `clang` and its whole family were dropped because the old
 * 1024-entry cap filled up on OTHER commands first. */
static void path_cmds_add(const char *name) {
    if (g_path_cmd_count >= g_path_cmd_capacity) {
        int new_cap = (g_path_cmd_capacity == 0) ? JSH_MAX_PATH_CMDS : g_path_cmd_capacity * 2;
        char **grown = realloc(g_path_cmds, (size_t)new_cap * sizeof(char *));
        if (grown == NULL) return; /* out of memory — stop growing, keep what we have so far */
        g_path_cmds = grown;
        g_path_cmd_capacity = new_cap;
    }
    g_path_cmds[g_path_cmd_count++] = strdup(name);
}

static void cache_path_commands(void) {
    g_path_cmd_count = 0;

    static const char *builtins[] = {
        "cd", "pwd", "exit", "export", "unset", "clear", "history",
        "type", "which", "alias", "unalias", "jobs", "fg", "bg",
        "break", "continue", "source", ".", "wait", "tab-text", NULL
    };
    for (int i = 0; builtins[i] != NULL; i++) {
        path_cmds_add(builtins[i]);
    }

    const char *path_env = getenv("PATH");
    if (path_env == NULL) return;
    char *path_copy = strdup(path_env);
    if (path_copy == NULL) return;

    /* Collect everything first, duplicates and all — checking for a
     * duplicate against every existing entry on each insert (the old
     * approach) is O(n) per command and O(n^2) overall, which gets slow
     * once there are tens of thousands of binaries on PATH. Instead: just
     * append everything (O(1) each), then sort once and dedup in a single
     * linear pass afterward — O(n log n) total. */
    char *dir = strtok(path_copy, ":");
    while (dir != NULL) {
        DIR *d = opendir(dir);
        if (d != NULL) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                path_cmds_add(entry->d_name);
            }
            closedir(d);
        }
        dir = strtok(NULL, ":");
    }
    free(path_copy);

    qsort(g_path_cmds, g_path_cmd_count, sizeof(char *), cmp_str);

    /* Single-pass dedup: after sorting, identical entries are always
     * adjacent, so this just walks once and drops repeats. */
    int write_i = 0;
    for (int read_i = 0; read_i < g_path_cmd_count; read_i++) {
        if (write_i > 0 && strcmp(g_path_cmds[write_i - 1], g_path_cmds[read_i]) == 0) {
            free(g_path_cmds[read_i]);
            continue;
        }
        g_path_cmds[write_i++] = g_path_cmds[read_i];
    }
    g_path_cmd_count = write_i;
}

/* ── tab completion ───────────────────────────────────────────────
 * Fills `matches` with candidates whose name starts with `word`.
 *   is_first_word == 1 : completing a command name → filtered from the
 *                         g_path_cmds[] cache built at startup (no disk
 *                         access at all — this is what keeps Tab feeling
 *                         instant instead of scanning $PATH every press)
 *   is_first_word == 0 : completing an argument → filenames in the current
 *                         directory (the common case: paths are painful to
 *                         type by hand on a phone keyboard). This part still
 *                         reads the directory live, since cwd contents
 *                         change far more often than $PATH does.
 * Caller owns the returned strings and must free() each one. */
static int find_completions(const char *word, int is_first_word, char *matches[JSH_MAX_COMPLETIONS]) {
    int count = 0;
    size_t wlen = strlen(word);

    if (is_first_word) {
        for (int i = 0; i < g_path_cmd_count && count < JSH_MAX_COMPLETIONS; i++) {
            if (strncmp(g_path_cmds[i], word, wlen) == 0) {
                matches[count++] = strdup(g_path_cmds[i]);
            }
        }
    } else {
        DIR *d = opendir(".");
        if (d != NULL) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL && count < JSH_MAX_COMPLETIONS) {
                if (word[0] != '.' && entry->d_name[0] == '.') continue; /* hide dotfiles unless asked */
                if (strncmp(entry->d_name, word, wlen) == 0) {
                    matches[count++] = strdup(entry->d_name);
                }
            }
            closedir(d);
        }
    }

    qsort(matches, count, sizeof(char *), cmp_str);
    return count;
}

/* Write a single literal token into argv[*argc] using g_expanded storage,
 * and advance *argc. Shared by the "no match" fallback paths below so the
 * literal-keep behavior only has to be written once. */
static void push_literal(const char *text, char *argv[JSH_MAX_ARGS], int *argc) {
    if (*argc >= JSH_MAX_ARGS - 1) return;
    snprintf(g_expanded[*argc], sizeof(g_expanded[*argc]), "%s", text);
    argv[*argc] = g_expanded[*argc];
    (*argc)++;
}

/* Expand a single glob pattern (already $-expanded) against the current
 * directory's entries, appending every match into argv[]/g_expanded starting
 * at *argc. If nothing matches, keeps the pattern itself as a literal word —
 * this is bash's default behavior (no error, no silent disappearance).
 *
 * Uses fnmatch() with FNM_PERIOD so a leading '.' in a filename is only
 * matched by a pattern that itself starts with '.' — same hidden-file rule
 * bash follows, so `rm *.log` never silently touches dotfiles. */
static void expand_glob_token(const char *pattern, char *argv[JSH_MAX_ARGS], int *argc) {
    DIR *d = opendir(".");
    if (d == NULL) {
        push_literal(pattern, argv, argc);
        return;
    }

    char *matches[JSH_MAX_ARGS];
    int match_count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && match_count < JSH_MAX_ARGS - 1) {
        if (fnmatch(pattern, entry->d_name, FNM_PERIOD) == 0) {
            matches[match_count++] = strdup(entry->d_name);
        }
    }
    closedir(d);

    if (match_count == 0) {
        push_literal(pattern, argv, argc);
        return;
    }

    qsort(matches, match_count, sizeof(char *), cmp_str);

    for (int i = 0; i < match_count; i++) {
        push_literal(matches[i], argv, argc);
        free(matches[i]);
    }
}

/* Reads one word starting at *pp (leading whitespace must already be
 * skipped by the caller). Handles the three word forms jsh understands:
 *
 *   "..."  double-quoted — $ expansion happens, glob does not
 *   '...'  single-quoted — fully literal, no expansion, no glob at all
 *   bare   everything else — $ expansion AND glob both apply
 *
 * A quote character only opens a quoted word when it's the very first
 * character — one appearing mid-word is just a literal character. jsh
 * doesn't support gluing quoted and unquoted pieces into one word (e.g.
 * foo"bar baz" is not supported — write it as one whole-word quote).
 *
 * The line has already been validated by quotes_are_balanced() before this
 * ever runs, so an opening quote here is always guaranteed to find its
 * matching close.
 *
 * Writes the word's raw, un-expanded content (quotes stripped, escapes
 * resolved) into `out`, advances *pp past the word, and reports which
 * kind it was via *quoted (0 = bare, '"' = double, '\'' = single).
 *
 * Escape rules (bash-compatible subset):
 *   bare words:   \x → x for any x (including space, so "file\ name" works)
 *   double quotes: only \" \\ \$ are special; other \X stay as \X
 *   single quotes: no escapes at all — backslash is literal
 */
static void read_word(char **pp, char *out, size_t out_size, char *quoted) {
    char *p = *pp;

    if (*p == '"' || *p == '\'') {
        char q = *p;
        p++;
        size_t oi = 0;
        while (*p != '\0' && *p != q) {
            if (q == '"' && *p == '\\' && p[1] != '\0') {
                char next = p[1];
                if (next == '"' || next == '\\' || next == '$') {
                    if (oi + 1 < out_size) out[oi++] = next;
                    p += 2;
                    continue;
                }
                /* other backslashes inside "..." stay literal (bash style) */
            }
            if (oi + 1 < out_size) out[oi++] = *p;
            p++;
        }
        out[oi] = '\0';
        if (*p == q) p++; /* skip the closing quote */
        *quoted = q;
        *pp = p;
        return;
    }

    /* bare word — process backslash escapes, stop at unescaped whitespace.
     * $(...) is treated as an atomic unit so spaces inside the substitution
     * do not split the token (bash behavior). */
    size_t oi = 0;
    while (*p != '\0') {
        if (*p == '\\' && p[1] != '\0') {
            if (oi + 1 < out_size) out[oi++] = p[1];
            p += 2;
            continue;
        }
        if (*p == ' ' || *p == '\t') break;
        /* Atomic $(...) — copy through matching ')' */
        if (*p == '$' && p[1] == '(') {
            if (oi + 1 < out_size) out[oi++] = *p++;
            if (oi + 1 < out_size) out[oi++] = *p++;
            int depth = 1;
            while (*p != '\0' && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                if (oi + 1 < out_size) out[oi++] = *p;
                p++;
            }
            continue;
        }
        if (oi + 1 < out_size) out[oi++] = *p;
        p++;
    }
    out[oi] = '\0';
    *quoted = '\0';
    *pp = p;
}

static int tokenize(char *line, char *argv[JSH_MAX_ARGS]) {
    int argc = 0;
    char *p = line;

    while (argc < JSH_MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        char raw[JSH_MAX_VAR_VALUE];
        char quoted;
        read_word(&p, raw, sizeof(raw), &quoted);

        if (quoted == '\'') {
            /* single-quoted: fully literal — not even $ expansion runs */
            push_literal(raw, argv, &argc);
            continue;
        }

        char expanded[JSH_MAX_VAR_VALUE];
        expand_string(raw, expanded, sizeof(expanded));

        if (quoted == '"') {
            /* double-quoted: $ expanded above, but never globbed —
             * echo "*.log" must print the literal text *.log */
            push_literal(expanded, argv, &argc);
        } else {
            /* bare word: $ expansion + glob, same as before quoting existed */
            if (has_glob_chars(expanded)) {
                expand_glob_token(expanded, argv, &argc);
            } else {
                push_literal(expanded, argv, &argc);
            }
        }
    }

    argv[argc] = NULL;
    return argc;
}

/* Try to parse `line` as a variable assignment: $name = value
 * Returns 1 and fills `name`/`value` (value already $-expanded) on match,
 * 0 if the line isn't an assignment at all (leaves it for normal dispatch). */
static int try_parse_assignment(const char *line, char *name, size_t name_size,
                                 char *value, size_t value_size) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    if (*p != '$') return 0;
    p++;

    size_t ni = 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '_') {
        if (ni + 1 < name_size) name[ni++] = *p;
        p++;
    }
    name[ni] = '\0';
    if (ni == 0) return 0;

    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    /* Strip one layer of surrounding quotes if present. Double-quoted values
     * still get $ expanded (matching the tokenize() rule for double quotes);
     * single-quoted values are fully literal — even $x inside stays as the
     * two literal characters $x, matching bash. */
    size_t len = strlen(p);
    if (len >= 2 && p[0] == '"' && p[len - 1] == '"') {
        char unquoted[JSH_MAX_VAR_VALUE];
        size_t copy_len = len - 2;
        if (copy_len >= sizeof(unquoted)) copy_len = sizeof(unquoted) - 1;
        memcpy(unquoted, p + 1, copy_len);
        unquoted[copy_len] = '\0';
        expand_string(unquoted, value, value_size);
    } else if (len >= 2 && p[0] == '\'' && p[len - 1] == '\'') {
        size_t copy_len = len - 2;
        if (copy_len >= value_size) copy_len = value_size - 1;
        memcpy(value, p + 1, copy_len);
        value[copy_len] = '\0';
    } else {
        expand_string(p, value, value_size);
    }

    return 1;
}

/* built-in: cd — must be a built-in because it changes jsh's own state (cwd);
 * this cannot be an external binary (a forked child's cd wouldn't affect the parent) */
static int builtin_cd(char *argv[]) {
    const char *target = argv[1];
    if (target == NULL || strcmp(target, "~") == 0) {
        target = getenv("HOME");
        if (target == NULL) target = "/";
    }

    if (chdir(target) != 0) {
        /* Use the real errno message (ENOENT/EACCES/ENOTDIR etc.)
         * instead of one generic error for every case */
        fprintf(stderr, "jsh: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    return 0;
}

/* built-in: pwd — prints the real current directory straight from the syscall,
 * not a cached variable that might be stale */
static int builtin_pwd(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s\n", cwd);
        return 0;
    }
    fprintf(stderr, "jsh: pwd: %s\n", strerror(errno));
    return 1;
}

/* built-in: clear — ANSI clear screen + home cursor */
static int builtin_clear(void) {
    printf("\033[H\033[2J");
    fflush(stdout);
    return 0;
}


/* 1 if executable `name` exists on $PATH (or as absolute path) */
static int cmd_on_path(const char *name) {
    if (name == NULL || name[0] == '\0') return 0;
    if (strchr(name, '/') != NULL)
        return access(name, X_OK) == 0;
    const char *path_env = getenv("PATH");
    if (path_env == NULL) return 0;
    char *path_copy = strdup(path_env);
    if (path_copy == NULL) return 0;
    int found = 0;
    for (char *dir = strtok(path_copy, ":"); dir != NULL; dir = strtok(NULL, ":")) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, name);
        if (access(full, X_OK) == 0) {
            found = 1;
            break;
        }
    }
    free(path_copy);
    return found;
}

/* built-in: ls — simple directory listing (CMD-ish: name + type hint)
 * Usage: ls [path ...]
 * Shows directories with trailing / and notes empty dirs. */
static int builtin_ls(char *argv[]) {
    int status = 0;
    int show_all = 0; /* -a: show dotfiles */
    int path_start = 1;

    for (int i = 1; argv[i] != NULL; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (int k = 1; argv[i][k]; k++) {
                if (argv[i][k] == 'a') show_all = 1;
            }
            path_start = i + 1;
        } else {
            break;
        }
    }

    int any = 0;
    for (int i = path_start; argv[i] != NULL; i++) {
        any = 1;
        const char *path = argv[i];
        DIR *d = opendir(path);
        if (d == NULL) {
            struct stat st;
            if (stat(path, &st) == 0) {
                printf("%s\n", path);
            } else {
                fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
                status = 1;
            }
            continue;
        }
        if (argv[path_start + 1] != NULL) printf("%s:\n", path);
        struct dirent *e;
        int count = 0;
        while ((e = readdir(d)) != NULL) {
            /* always skip . and .. */
            if (e->d_name[0] == '.' && (e->d_name[1] == '\0' ||
                (e->d_name[1] == '.' && e->d_name[2] == '\0')))
                continue;
            /* hide other dotfiles unless -a (Termux/ls default) */
            if (!show_all && e->d_name[0] == '.')
                continue;
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
                printf("%s/\n", e->d_name);
            else
                printf("%s\n", e->d_name);
            count++;
        }
        closedir(d);
        if (count == 0) printf("(empty)\n");
    }
    if (!any) {
        char *av[4];
        int n = 0;
        av[n++] = "ls";
        if (show_all) av[n++] = "-a";
        av[n++] = ".";
        av[n] = NULL;
        return builtin_ls(av);
    }
    return status;
}


/* built-in: mkdir — create directory
 * Usage: mkdir <name> [...] */
static int builtin_mkdir(char *argv[]) {
    if (argv[1] == NULL) {
        fprintf(stderr, "mkdir: Needs 1 argument\n");
        fprintf(stderr, "  tip: mkdir mydir   then  cd mydir\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; argv[i] != NULL; i++) {
        if (mkdir(argv[i], 0755) != 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        } else {
            /* CMD-like confirmation with timestamp */
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char tbuf[64];
            if (tm) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", tm);
            else snprintf(tbuf, sizeof(tbuf), "?");
            printf("    Directory created: %s  (%s)\n", argv[i], tbuf);
        }
    }
    return status;
}

/* built-in: rmdir */
static int builtin_rmdir(char *argv[]) {
    if (argv[1] == NULL) {
        fprintf(stderr, "rmdir: Needs 1 argument\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; argv[i] != NULL; i++) {
        if (rmdir(argv[i]) != 0) {
            fprintf(stderr, "rmdir: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}

/* built-in: help — short CMD/Termux style tips */
static int builtin_help(char *argv[]) {
    (void)argv;
    printf("\033[38;2;86;156;214mjsh\033[0m — PowerCode shell\n");
    printf("\n");
    printf("Navigation\n");
    printf("  cd          go to $HOME (~)\n");
    printf("  cd <dir>    enter directory\n");
    printf("  cd ..       parent directory\n");
    printf("  pwd         print full path\n");
    printf("  ls [path]   list files (dirs end with /)\n");
    printf("\n");
    printf("Files\n");
    printf("  mkdir <n>   create directory (shows date)\n");
    printf("  rmdir <n>   remove empty directory\n");
    printf("  clear       clear screen\n");
    printf("  history     command history\n");
    printf("\n");
    printf("FHS layout (sandbox)\n");
    printf("  ~           $HOME  =  .../files/home\n");
    printf("  ../bin      from ~ → .../files/bin\n");
    printf("  ../usr/bin  from ~ → .../files/usr/bin\n");
    printf("  from ~/subdir use  cd ../../bin\n");
    printf("\n");
    printf("Setup\n");
    printf("  setup       bootstrap pwc + PATH tips\n");
    printf("\n");
    return 0;
}

/* Find libNAME.so: $PWC_NATIVE_LIB, PATH lib dirs, /proc/self/exe dir */
static int find_native_lib(const char *soname, char *out, size_t out_size) {
    const char *native = getenv("PWC_NATIVE_LIB");
    if (native && native[0]) {
        snprintf(out, out_size, "%s/%s", native, soname);
        if (access(out, R_OK) == 0) return 0;
    }
    const char *path_env = getenv("PATH");
    if (path_env) {
        char *copy = strdup(path_env);
        if (copy) {
            for (char *dir = strtok(copy, ":"); dir; dir = strtok(NULL, ":")) {
                if (strstr(dir, "/lib") == NULL)
                    continue;
                snprintf(out, out_size, "%s/%s", dir, soname);
                if (access(out, R_OK) == 0) {
                    free(copy);
                    return 0;
                }
            }
            free(copy);
        }
    }
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, out_size, "%s/%s", self, soname);
            if (access(out, R_OK) == 0) return 0;
        }
    }
    out[0] = '\0';
    return 1;
}

static int link_one(const char *target, const char *linkpath) {
    unlink(linkpath);
    if (symlink(target, linkpath) != 0) {
        fprintf(stderr, "  skip %s: %s\n", linkpath, strerror(errno));
        return 1;
    }
    return 0;
}

/* built-in: setup — one command: FHS + pwc + toybox links */
static int builtin_setup(char *argv[]) {
    (void)argv;
    const char *home = getenv("HOME");
    if (home == NULL) home = ".";

    char local_bin[PATH_MAX], usr_bin[PATH_MAX], bin_dir[PATH_MAX], tmp_dir[PATH_MAX];
    snprintf(local_bin, sizeof(local_bin), "%s/.local/bin", home);

    char files[PATH_MAX];
    snprintf(files, sizeof(files), "%s", home);
    char *slash = strrchr(files, '/');
    if (slash && slash != files) *slash = '\0';

    snprintf(usr_bin, sizeof(usr_bin), "%s/usr/bin", files);
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", files);
    snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", files);

    {
        char local[PATH_MAX];
        snprintf(local, sizeof(local), "%s/.local", home);
        mkdir(home, 0755);
        mkdir(local, 0755);
        mkdir(local_bin, 0755);
        mkdir(usr_bin, 0755);
        mkdir(bin_dir, 0755);
        mkdir(tmp_dir, 0755);
    }

    printf("\033[38;2;78;201;176m[setup]\033[0m FHS ready\n");
    printf("  HOME      %s\n", home);
    printf("  local/bin %s\n", local_bin);

    char libpwc[PATH_MAX];
    char pwc_link[PATH_MAX];
    snprintf(pwc_link, sizeof(pwc_link), "%s/pwc", local_bin);

    if (find_native_lib("libpwc.so", libpwc, sizeof(libpwc)) == 0) {
        if (link_one(libpwc, pwc_link) == 0)
            printf("\033[38;2;78;201;176m[setup]\033[0m pwc -> %s\n", libpwc);
        else
            printf("\033[38;2;220;160;80m[setup]\033[0m pwc link failed\n");
    } else {
        printf("\033[38;2;220;160;80m[setup]\033[0m libpwc.so not found\n");
    }

    static const char *TOYBOX_LINKS[] = {
        "ls", "cp", "mv", "rm", "mkdir", "rmdir", "cat", "echo",
        "pwd", "touch", "chmod", "chown", "ln", "stat", "df", "du",
        "ps", "kill", "sleep", "head", "tail", "wc", "grep", "find",
        "xargs", "sort", "uniq", "cut", "tr", "basename", "dirname",
        "realpath", "which", "id", "whoami", "uname", "date", "true",
        "false", "test", "[", "env", "printenv", "clear", "seq",
        "tar", "gzip", "gunzip", "base64", "md5sum", "sha256sum",
        "ifconfig", "netstat", "ping", "toybox",
        NULL
    };

    char libtoy[PATH_MAX];
    int ok = 0, fail = 0;
    if (find_native_lib("libtoybox.so", libtoy, sizeof(libtoy)) == 0) {
        printf("\033[38;2;78;201;176m[setup]\033[0m toybox -> %s\n", libtoy);
        for (int i = 0; TOYBOX_LINKS[i] != NULL; i++) {
            char linkpath[PATH_MAX];
            snprintf(linkpath, sizeof(linkpath), "%s/%s", local_bin, TOYBOX_LINKS[i]);
            if (link_one(libtoy, linkpath) == 0) ok++;
            else fail++;
        }
        printf("\033[38;2;78;201;176m[setup]\033[0m links: %d ok, %d failed\n", ok, fail);
    } else {
        printf("\033[38;2;220;160;80m[setup]\033[0m libtoybox.so not found\n");
    }

    printf("\n");
    if (access(pwc_link, X_OK) == 0)
        printf("Try:  pwc   ls -la   which ls   toybox\n");
    else
        printf("Try:  setup again after rebuild APK with libpwc.so + libtoybox.so\n");

    return (ok > 0 || access(pwc_link, X_OK) == 0) ? 0 : 1;
}


/* built-in: history — list in-memory history (most recent at the bottom) */
static int builtin_history(void) {
    for (int i = 0; i < g_history_count; i++) {
        printf("%5d  %s\n", i + 1, g_history[i]);
    }
    return 0;
}

/* built-in: unset NAME [NAME ...] — drop shell variables */
static int builtin_unset(char *argv[]) {
    int status = 0;
    if (argv[1] == NULL) {
        fprintf(stderr, "usage: unset NAME [NAME ...]\n");
        return 1;
    }
    for (int i = 1; argv[i] != NULL; i++) {
        if (unset_var(argv[i]) != 0) {
            /* not found is not fatal, but we still report non-zero if any failed */
            status = 1;
        }
    }
    return status;
}

/* Resolve a command name to a path (or note that it is a builtin/alias).
 * Used by both `type` and `which`. Returns 0 if found, 1 otherwise. */
static int resolve_command(const char *name, int verbose) {
    /* aliases first */
    jsh_alias_t *a = find_alias(name);
    if (a != NULL) {
        if (verbose) printf("%s is aliased to `%s'\n", name, a->value);
        else printf("%s\n", a->value);
        return 0;
    }

    /* known builtins */
    static const char *builtins[] = {
        "cd", "pwd", "exit", "export", "unset", "clear", "history",
        "type", "which", "alias", "unalias", "jobs", "fg", "bg",
        "break", "continue", "source", ".", "wait", "tab-text", NULL
    };
    for (int i = 0; builtins[i] != NULL; i++) {
        if (strcmp(name, builtins[i]) == 0) {
            if (verbose) printf("%s is a shell builtin\n", name);
            else printf("%s\n", name);
            return 0;
        }
    }

    /* search $PATH */
    const char *path_env = getenv("PATH");
    if (path_env == NULL) path_env = "/usr/bin:/bin";
    char *path_copy = strdup(path_env);
    if (path_copy == NULL) return 1;

    char *dir = strtok(path_copy, ":");
    while (dir != NULL) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, name);
        if (access(full, X_OK) == 0) {
            if (verbose) printf("%s is %s\n", name, full);
            else printf("%s\n", full);
            free(path_copy);
            return 0;
        }
        dir = strtok(NULL, ":");
    }
    free(path_copy);

    if (verbose) fprintf(stderr, "jsh: type: %s: not found\n", name);
    return 1;
}

static int builtin_type(char *argv[]) {
    if (argv[1] == NULL) {
        fprintf(stderr, "usage: type NAME [NAME ...]\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; argv[i] != NULL; i++) {
        if (resolve_command(argv[i], 1) != 0) status = 1;
    }
    return status;
}

static int builtin_which(char *argv[]) {
    if (argv[1] == NULL) {
        fprintf(stderr, "usage: which NAME [NAME ...]\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; argv[i] != NULL; i++) {
        if (resolve_command(argv[i], 0) != 0) status = 1;
    }
    return status;
}

/* Parse a single alias definition from a raw string of the form
 *   name='value'   or   name="value"   or   name=value
 * Handles the common `alias ll='ls -la'` case that ordinary tokenize
 * would split on the space inside the quotes. Returns 1 on success. */
static int parse_one_alias_def(const char *text) {
    while (*text == ' ' || *text == '\t') text++;
    if (*text == '\0') return 0;

    char name[JSH_MAX_VAR_NAME];
    size_t ni = 0;
    while (*text != '\0' && *text != '=' && *text != ' ' && *text != '\t') {
        if (ni + 1 < sizeof(name)) name[ni++] = *text;
        text++;
    }
    name[ni] = '\0';
    if (ni == 0 || *text != '=') return 0;
    text++; /* skip '=' */

    char value[JSH_MAX_VAR_VALUE];
    size_t len = strlen(text);
    /* strip one layer of surrounding quotes if present */
    if (len >= 2 && ((text[0] == '\'' && text[len - 1] == '\'') ||
                     (text[0] == '"' && text[len - 1] == '"'))) {
        size_t copy_len = len - 2;
        if (copy_len >= sizeof(value)) copy_len = sizeof(value) - 1;
        memcpy(value, text + 1, copy_len);
        value[copy_len] = '\0';
    } else {
        snprintf(value, sizeof(value), "%s", text);
    }
    set_alias(name, value);
    return 1;
}

/* built-in: alias [name[=value] ...]
 *   alias                  → list all
 *   alias name             → show one
 *   alias name='value'     → define (also accepts name="value" or name=value)
 *
 * When called from dispatch we prefer the raw stage text for definitions
 * so that spaces inside quotes survive. */
static int builtin_alias(char *argv[]) {
    if (argv[1] == NULL) {
        for (int i = 0; i < g_alias_count; i++) {
            printf("alias %s='%s'\n", g_aliases[i].name, g_aliases[i].value);
        }
        return 0;
    }

    for (int i = 1; argv[i] != NULL; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL) {
            /* show one */
            jsh_alias_t *a = find_alias(argv[i]);
            if (a != NULL) {
                printf("alias %s='%s'\n", a->name, a->value);
            } else {
                fprintf(stderr, "jsh: alias: %s: not found\n", argv[i]);
                return 1;
            }
        } else {
            /* define from already-tokenized form (no spaces in value) */
            char name[JSH_MAX_VAR_NAME];
            size_t nlen = (size_t)(eq - argv[i]);
            if (nlen == 0 || nlen >= sizeof(name)) {
                fprintf(stderr, "jsh: alias: invalid name\n");
                return 1;
            }
            memcpy(name, argv[i], nlen);
            name[nlen] = '\0';
            set_alias(name, eq + 1);
        }
    }
    return 0;
}

/* Handle `alias ...` using the raw stage text so quoted multi-word values
 * work. Falls back to the argv-based builtin_alias for listing / showing. */
static int handle_alias_command(const char *raw_stage, char *argv[], int argc) {
    (void)argc;
    /* Skip the leading "alias" keyword and any whitespace */
    const char *p = raw_stage;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "alias", 5) != 0) return builtin_alias(argv);
    p += 5;
    if (*p != ' ' && *p != '\t' && *p != '\0') return builtin_alias(argv);
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '\0') {
        /* bare `alias` → list */
        return builtin_alias(argv);
    }

    /* If there is no '=' anywhere, treat as "show these names" */
    if (strchr(p, '=') == NULL) {
        return builtin_alias(argv);
    }

    /* One or more name=value definitions. For simplicity we support a
     * single definition per command (the common interactive case).
     * Multiple definitions on one line without careful quoting remain
     * best-effort via the argv path. */
    if (parse_one_alias_def(p)) {
        return 0;
    }
    return builtin_alias(argv);
}

static int builtin_unalias(char *argv[]) {
    if (argv[1] == NULL) {
        fprintf(stderr, "usage: unalias NAME [NAME ...]\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; argv[i] != NULL; i++) {
        if (unset_alias(argv[i]) != 0) {
            fprintf(stderr, "jsh: unalias: %s: not found\n", argv[i]);
            status = 1;
        }
    }
    return status;
}

/* Build a short prompt path: shows "~" for HOME and "~/..." for anything under it
 * (Termux-style).
 *
 * Matching order:
 *  1. $HOME (normalized, trailing slash stripped)
 *  2. Android sandbox heuristic: path contains "/files/home" → treat that
 *     segment as the home root (covers the case where the app set cwd to
 *     .../files/home but forgot to export HOME, which is why the prompt
 *     was stuck on the literal word "home")
 *  3. Last path segment only (never leak the full /data/data/... path)
 */
static void short_prompt_path(char *out, size_t out_size) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        snprintf(out, out_size, "?");
        return;
    }

    /* Normalize: drop trailing slash(es) from cwd (except root "/") */
    size_t cwd_len = strlen(cwd);
    while (cwd_len > 1 && cwd[cwd_len - 1] == '/') {
        cwd[--cwd_len] = '\0';
    }

    /* 1) Prefer $HOME */
    const char *home_env = getenv("HOME");
    if (home_env != NULL && home_env[0] != '\0') {
        char home[PATH_MAX];
        snprintf(home, sizeof(home), "%s", home_env);

        size_t home_len = strlen(home);
        while (home_len > 1 && home[home_len - 1] == '/') {
            home[--home_len] = '\0';
        }

        if (strncmp(cwd, home, home_len) == 0) {
            if (cwd[home_len] == '\0') {
                snprintf(out, out_size, "~");
                return;
            }
            if (cwd[home_len] == '/') {
                snprintf(out, out_size, "~%s", cwd + home_len);
                return;
            }
        }
    }

    /* 2) Android app sandbox: .../files/home[/...] */
    const char *marker = strstr(cwd, "/files/home");
    if (marker != NULL) {
        const char *after = marker + strlen("/files/home");
        if (*after == '\0') {
            snprintf(out, out_size, "~");
            return;
        }
        if (*after == '/') {
            snprintf(out, out_size, "~%s", after);
            return;
        }
    }

    /* 3) Outside known home — last segment only */
    const char *base = strrchr(cwd, '/');
    snprintf(out, out_size, "%s", base != NULL ? base + 1 : cwd);
}

/* Builds the full colored prompt string (path in blue + gradient "!>>")
 * Termux-style path (~) + original PowerCode gradient marker. */
static void build_prompt(char *out, size_t out_size) {
    char shortpath[PATH_MAX];
    short_prompt_path(shortpath, sizeof(shortpath));
    snprintf(out, out_size,
             "\033[38;2;86;156;214m%s\033[0m "
             "\033[38;2;90;160;255m!"
             "\033[38;2;160;120;255m>"
             "\033[38;2;220;90;220m>"
             "\033[0m ",
             shortpath);
}

/* Classic Levenshtein edit distance — how many single-character edits
 * (insert/delete/substitute) turn `a` into `b`. Used to rank "close enough"
 * command name guesses when a command isn't found. */
static int levenshtein(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    int *prev = malloc((lb + 1) * sizeof(int));
    int *curr = malloc((lb + 1) * sizeof(int));
    if (prev == NULL || curr == NULL) {
        free(prev);
        free(curr);
        return INT_MAX;
    }

    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;

    for (size_t i = 1; i <= la; i++) {
        curr[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = curr[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int min = del < ins ? del : ins;
            if (sub < min) min = sub;
            curr[j] = min;
        }
        int *tmp = prev;
        prev = curr;
        curr = tmp;
    }

    int result = prev[lb];
    free(prev);
    free(curr);
    return result;
}

/* Scan every directory in $PATH for executable names close to `cmd` (by edit
 * distance) and print up to JSH_MAX_SUGGESTIONS suggestions. Best-effort: if
 * $PATH is huge this is O(n) over directory entries, which is fine for a
 * shell's occasional "command not found" case. */
static void suggest_similar_commands(const char *cmd) {
    const char *path_env = getenv("PATH");
    if (path_env == NULL || cmd[0] == '\0') return;

    size_t cmd_len = strlen(cmd);
    /* Scale the threshold to the length of what was typed — for very short
     * commands (1-2 chars) a distance-2 match is basically noise (e.g. "e"
     * would match "[", "am", etc). Longer commands can tolerate more typos. */
    int max_dist = (cmd_len <= 2) ? 1 : JSH_SUGGEST_DISTANCE;

    char *path_copy = strdup(path_env);
    if (path_copy == NULL) return;

    char *best_names[JSH_MAX_SUGGESTIONS];
    int best_dist[JSH_MAX_SUGGESTIONS];
    int found = 0;
    for (int i = 0; i < JSH_MAX_SUGGESTIONS; i++) {
        best_names[i] = NULL;
        best_dist[i] = max_dist + 1;
    }

    char *dir = strtok(path_copy, ":");
    while (dir != NULL) {
        DIR *d = opendir(dir);
        if (d != NULL) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL) {
                if (entry->d_name[0] == '.') continue;

                int dist = levenshtein(cmd, entry->d_name);

                /* Prefer matches that share the first character — a typo of
                 * "ls" is far more likely to start with 'l' than not */
                if (dist <= max_dist && entry->d_name[0] != cmd[0]) {
                    dist += 1;
                }

                if (dist <= max_dist) {
                    /* Skip duplicates already in the list (same binary can
                     * appear in multiple PATH dirs, e.g. symlinked coreutils) */
                    int is_dup = 0;
                    for (int k = 0; k < JSH_MAX_SUGGESTIONS; k++) {
                        if (best_names[k] != NULL && strcmp(best_names[k], entry->d_name) == 0) {
                            is_dup = 1;
                            break;
                        }
                    }
                    if (is_dup) continue;

                    /* Insert into the best[] list, keeping it sorted by distance
                     * (simple insertion, list is tiny) */
                    for (int i = 0; i < JSH_MAX_SUGGESTIONS; i++) {
                        if (dist < best_dist[i]) {
                            for (int j = JSH_MAX_SUGGESTIONS - 1; j > i; j--) {
                                best_dist[j] = best_dist[j - 1];
                                free(best_names[j]);
                                best_names[j] = best_names[j - 1];
                                best_names[j - 1] = NULL;
                            }
                            best_dist[i] = dist;
                            best_names[i] = strdup(entry->d_name);
                            found = 1;
                            break;
                        }
                    }
                }
            }
            closedir(d);
        }
        dir = strtok(NULL, ":");
    }

    free(path_copy);

    if (found) {
        fprintf(stderr, "Did you mean:\n");
        for (int i = 0; i < JSH_MAX_SUGGESTIONS; i++) {
            if (best_names[i] != NULL) {
                fprintf(stderr, "  %s\n", best_names[i]);
                free(best_names[i]);
            }
        }
    }
}

/* NOTE: plain external-command execution now goes through run_pipeline()
 * (a single-stage pipeline is just "one command"), which also handles
 * < / > / >> redirection. See run_pipeline() and spawn_stage() below. */

/* Try to parse `line` as `export $NAME` or `export NAME`. Must be checked
 * against the RAW line before tokenize()/expansion runs — otherwise "$name"
 * would already have been replaced by its VALUE, and we'd end up exporting
 * a variable named after the value instead of the actual variable. */
static int try_parse_export(const char *line, char *name, size_t name_size) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    const char *kw = "export";
    size_t kwlen = strlen(kw);
    if (strncmp(p, kw, kwlen) != 0) return 0;
    p += kwlen;

    if (*p != ' ' && *p != '\t' && *p != '\0') return 0; /* e.g. "exporter" isn't "export" */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') p++;

    size_t ni = 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '_') {
        if (ni + 1 < name_size) name[ni++] = *p;
        p++;
    }
    name[ni] = '\0';
    return 1; /* it IS an export command, even if name ends up empty (usage error) */
}

/* Checks whether every "..." and '...' in `line` has a matching close.
 * Single-pass, no nesting logic needed: once inside a quote, only that
 * same quote character closes it (a different quote char inside is just
 * a literal, same as bash: 'it says "hi"' is one single-quoted word).
 * Called once on the whole raw line before operator-splitting/tokenizing,
 * so every quote-aware scan downstream can assume it never runs off the
 * end of the string mid-quote. */
static int quotes_are_balanced(const char *line) {
    char in_quote = '\0';
    for (const char *p = line; *p != '\0'; p++) {
        if (in_quote != '\0') {
            if (*p == in_quote) in_quote = '\0';
            continue;
        }
        if (*p == '"' || *p == '\'') in_quote = *p;
    }
    return in_quote == '\0';
}

/* Which control operator preceded a given segment when a line is chained
 * with &&, ||, or ; — OP_NONE only applies to the first segment. */
typedef enum { OP_NONE, OP_AND, OP_OR, OP_SEQ } jsh_op_t;

/* Split a raw line into command segments joined by &&, ||, or ; — mutates
 * `line` in place (drops '\0' at each operator, like strtok) and returns
 * the segment count. segments[i]/ops[i] give the command text and the
 * operator that led into it (ops[0] is always OP_NONE).
 *
 * NOTE: this splits on the raw text, the same way assignment/export
 * detection does — jsh has no quoting yet, so a literal "&&" typed inside
 * what will eventually be a quoted string would still split here. This is
 * a known, temporary limitation until quoting is implemented; not a bug. */
static int split_operators(char *line, char *segments[JSH_MAX_SEGMENTS],
                            jsh_op_t ops[JSH_MAX_SEGMENTS]) {
    int count = 0;
    char *p = line;
    char in_quote = '\0';

    segments[count] = p;
    ops[count] = OP_NONE;
    count++;

    while (*p != '\0' && count < JSH_MAX_SEGMENTS) {
        if (in_quote != '\0') {
            if (*p == in_quote) in_quote = '\0';
            p++;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            in_quote = *p;
            p++;
            continue;
        }

        jsh_op_t op = OP_NONE;
        int op_len = 0;

        if (p[0] == '&' && p[1] == '&') { op = OP_AND; op_len = 2; }
        else if (p[0] == '|' && p[1] == '|') { op = OP_OR; op_len = 2; }
        else if (p[0] == ';') { op = OP_SEQ; op_len = 1; }

        if (op == OP_NONE) {
            p++;
            continue;
        }

        *p = '\0';
        p += op_len;
        while (*p == ' ' || *p == '\t') p++;

        segments[count] = p;
        ops[count] = op;
        count++;
    }

    return count;
}

/* Split a single command segment (already isolated from &&/||/; above) into
 * pipeline stages at top-level '|' characters — quote-aware, same approach
 * as split_operators. Mutates `segment` in place. A lone '|' surviving here
 * is a real pipe: "||" was already consumed as a chain operator upstream. */
static int split_pipe(char *segment, char *stages[JSH_MAX_SEGMENTS]) {
    int count = 0;
    char *p = segment;
    char in_quote = '\0';

    stages[count++] = p;

    while (*p != '\0' && count < JSH_MAX_SEGMENTS) {
        if (in_quote != '\0') {
            if (*p == in_quote) in_quote = '\0';
            p++;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            in_quote = *p;
            p++;
            continue;
        }
        if (*p == '|') {
            *p = '\0';
            p++;
            while (*p == ' ' || *p == '\t') p++;
            stages[count++] = p;
            continue;
        }
        p++;
    }

    return count;
}

/* Redirection extracted from one pipeline stage's argv. `in_path`/`out_path`
 * are NULL when not requested. These point into g_expanded[] storage from
 * that stage's tokenize() call, so they only need to stay valid until this
 * stage has been forked (see run_pipeline) — fork()'s copy-on-write memory
 * keeps the child's copy intact even after the parent moves on to the next
 * stage's tokenize() call, which reuses the same g_expanded buffer. */
typedef struct {
    const char *in_path;
    const char *out_path;
    int append; /* 1 for >>, 0 for > */
} jsh_redir_t;

/* Scans a tokenized argv for "<", ">", ">>" and pulls each one (plus the
 * filename that follows it) out of argv, compacting the array in place so
 * the redirection tokens never reach execvp as real arguments. Returns 0 on
 * success, -1 on a syntax error (operator with no filename after it). */
static int extract_redirection(char *argv[], int *argc, jsh_redir_t *redir) {
    redir->in_path = NULL;
    redir->out_path = NULL;
    redir->append = 0;

    int write_i = 0;
    for (int read_i = 0; read_i < *argc; read_i++) {
        int is_redir = (strcmp(argv[read_i], "<") == 0) ||
                        (strcmp(argv[read_i], ">") == 0) ||
                        (strcmp(argv[read_i], ">>") == 0);

        if (!is_redir) {
            argv[write_i++] = argv[read_i];
            continue;
        }

        if (read_i + 1 >= *argc) {
            fprintf(stderr, "jsh: syntax error: expected a filename after %s\n", argv[read_i]);
            return -1;
        }

        if (strcmp(argv[read_i], "<") == 0) {
            redir->in_path = argv[read_i + 1];
        } else if (strcmp(argv[read_i], ">") == 0) {
            redir->out_path = argv[read_i + 1];
            redir->append = 0;
        } else {
            redir->out_path = argv[read_i + 1];
            redir->append = 1;
        }

        read_i++; /* also consume the filename token */
    }

    argv[write_i] = NULL;
    *argc = write_i;
    return 0;
}


/* ── job table helpers ──────────────────────────────────────────── */

static jsh_job_t *job_alloc(void) {
    for (int i = 0; i < JSH_MAX_JOBS; i++) {
        if (!g_jobs[i].used) {
            memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
            g_jobs[i].used = 1;
            g_jobs[i].id = g_next_job_id++;
            if (g_next_job_id > 9999) g_next_job_id = 1;
            return &g_jobs[i];
        }
    }
    return NULL;
}

static void job_free(jsh_job_t *j) {
    if (j) j->used = 0;
}

static jsh_job_t *job_by_id(int id) {
    for (int i = 0; i < JSH_MAX_JOBS; i++) {
        if (g_jobs[i].used && g_jobs[i].id == id) return &g_jobs[i];
    }
    return NULL;
}

/* Most recent non-done job (for bare `fg` / `bg`). */
static jsh_job_t *job_current(void) {
    jsh_job_t *best = NULL;
    for (int i = 0; i < JSH_MAX_JOBS; i++) {
        if (!g_jobs[i].used) continue;
        if (g_jobs[i].state == JOB_DONE) continue;
        if (best == NULL || g_jobs[i].id > best->id) best = &g_jobs[i];
    }
    return best;
}

/* Reap any children that have exited without blocking. Updates job state
 * and records status. Does not print notifications (see jobs_notify_done). */
static void jobs_reap(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        for (int i = 0; i < JSH_MAX_JOBS; i++) {
            if (!g_jobs[i].used) continue;
            for (int k = 0; k < g_jobs[i].npids; k++) {
                if (g_jobs[i].pids[k] != pid) continue;
                g_jobs[i].status = status;
                if (WIFSTOPPED(status)) {
                    g_jobs[i].state = JOB_STOPPED;
                } else {
                    /* Mark this pid as reaped */
                    g_jobs[i].pids[k] = -1;
                    int alive = 0;
                    for (int m = 0; m < g_jobs[i].npids; m++) {
                        if (g_jobs[i].pids[m] > 0) { alive = 1; break; }
                    }
                    if (!alive) {
                        g_jobs[i].state = JOB_DONE;
                        g_jobs[i].notified = 0;
                    }
                }
                break;
            }
        }
    }
}

/* Print "Done" for finished background jobs, then free their slots.
 * Called once per prompt so the user sees completion without a SIGCHLD
 * handler racing the line editor. */
static void jobs_notify_done(void) {
    jobs_reap();
    for (int i = 0; i < JSH_MAX_JOBS; i++) {
        if (!g_jobs[i].used) continue;
        if (g_jobs[i].state == JOB_DONE && !g_jobs[i].notified) {
            printf("[%d]  Done                    %s\n",
                   g_jobs[i].id, g_jobs[i].cmdline);
            g_jobs[i].notified = 1;
            job_free(&g_jobs[i]);
        }
    }
}

/* Parse optional job spec: "%1", "%", "1", or NULL → current job.
 * Returns the job or NULL (and prints an error). */
static jsh_job_t *job_parse_spec(const char *spec) {
    if (spec == NULL || spec[0] == '\0' || strcmp(spec, "%") == 0) {
        jsh_job_t *j = job_current();
        if (j == NULL) fprintf(stderr, "jsh: no current job\n");
        return j;
    }
    if (spec[0] == '%') spec++;
    char *end = NULL;
    long id = strtol(spec, &end, 10);
    if (end == spec || *end != '\0' || id <= 0) {
        fprintf(stderr, "jsh: invalid job spec: %s\n", spec);
        return NULL;
    }
    jsh_job_t *j = job_by_id((int)id);
    if (j == NULL) fprintf(stderr, "jsh: job %ld not found\n", id);
    return j;
}

/* Wait for a foreground job; handles normal exit, signal death, and
 * Ctrl+Z (WIFSTOPPED). Returns the exit-style status for $?. */
static int job_wait_foreground(jsh_job_t *job) {
    if (g_interactive && job->pgid > 0) {
        tcsetpgrp(STDIN_FILENO, job->pgid);
    }

    int last_status = 0;
    int stopped = 0;
    for (int i = 0; i < job->npids; i++) {
        if (job->pids[i] <= 0) continue;
        int status = 0;
        pid_t r = waitpid(job->pids[i], &status, WUNTRACED);
        if (r < 0) continue;
        last_status = status;
        if (WIFSTOPPED(status)) {
            stopped = 1;
            job->pids[i] = r; /* still alive, stopped */
        } else {
            job->pids[i] = -1;
        }
    }

    if (g_interactive && job->pgid > 0) {
        tcsetpgrp(STDIN_FILENO, g_shell_pgid);
    }

    if (stopped) {
        job->state = JOB_STOPPED;
        job->status = last_status;
        printf("\n[%d]+  Stopped                 %s\n", job->id, job->cmdline);
        return 128 + SIGTSTP;
    }

    /* fully finished */
    int code = WIFEXITED(last_status) ? WEXITSTATUS(last_status)
             : (WIFSIGNALED(last_status) ? 128 + WTERMSIG(last_status) : 1);
    job_free(job);
    return code;
}

static int builtin_jobs(char *argv[]) {
    (void)argv;
    jobs_reap();
    for (int i = 0; i < JSH_MAX_JOBS; i++) {
        if (!g_jobs[i].used) continue;
        if (g_jobs[i].state == JOB_DONE) continue;
        const char *st = (g_jobs[i].state == JOB_STOPPED) ? "Stopped" : "Running";
        printf("[%d]  %-24s %s\n", g_jobs[i].id, st, g_jobs[i].cmdline);
    }
    return 0;
}

static int builtin_fg(char *argv[]) {
    jsh_job_t *j = job_parse_spec(argv[1]);
    if (j == NULL) return 1;
    if (j->state == JOB_DONE) {
        fprintf(stderr, "jsh: fg: job %d has terminated\n", j->id);
        return 1;
    }
    printf("%s\n", j->cmdline);
    if (j->state == JOB_STOPPED) {
        kill(-j->pgid, SIGCONT);
        j->state = JOB_RUNNING;
    }
    return job_wait_foreground(j);
}

static int builtin_bg(char *argv[]) {
    jsh_job_t *j = job_parse_spec(argv[1]);
    if (j == NULL) return 1;
    if (j->state != JOB_STOPPED) {
        fprintf(stderr, "jsh: bg: job %d is already running\n", j->id);
        return 1;
    }
    kill(-j->pgid, SIGCONT);
    j->state = JOB_RUNNING;
    printf("[%d]+ %s &\n", j->id, j->cmdline);
    return 0;
}

static int builtin_break(char *argv[]) {
    (void)argv;
    if (g_loop_depth <= 0) {
        fprintf(stderr, "jsh: break: only meaningful in a for/while loop\n");
        return 1;
    }
    g_break_flag = 1;
    return 0;
}

static int builtin_continue(char *argv[]) {
    (void)argv;
    if (g_loop_depth <= 0) {
        fprintf(stderr, "jsh: continue: only meaningful in a for/while loop\n");
        return 1;
    }
    g_continue_flag = 1;
    return 0;
}

/* source / . — execute a file in the current shell (shares variables). */
static int builtin_source(char *argv[]) {
    const char *path = argv[1];
    if (path == NULL) {
        fprintf(stderr, "usage: source FILE\n");
        return 1;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "jsh: source: %s: %s\n", path, strerror(errno));
        return 1;
    }

    char stmt[JSH_MAX_STMT];
    stmt[0] = '\0';
    int first = 1;
    char line[JSH_MAX_LINE];
    int result = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll - 1] == '\n' || line[ll - 1] == '\r'))
            line[--ll] = '\0';

        if (first) {
            snprintf(stmt, sizeof(stmt), "%s", line);
            first = 0;
        } else {
            size_t sl = strlen(stmt);
            if (sl + 1 < sizeof(stmt)) {
                stmt[sl] = '\n';
                snprintf(stmt + sl + 1, sizeof(stmt) - sl - 1, "%s", line);
            }
        }

        if (!statement_is_complete(stmt)) continue;

        result = dispatch_line(stmt);
        stmt[0] = '\0';
        first = 1;
        if (result == -1) break; /* exit inside sourced file */
    }

    /* leftover incomplete statement */
    if (!first && stmt[0] != '\0') {
        const char *c = stmt;
        while (*c == ' ' || *c == '\t' || *c == '\n') c++;
        if (*c) result = dispatch_line(stmt);
    }

    fclose(f);
    return (result == -1) ? -1 : g_last_status;
}

/* wait [job_spec|pid] — wait for background jobs to finish.
 * No args: wait for all. %N: job id. number: pid. */
static int builtin_wait(char *argv[]) {
    jobs_reap();

    if (argv[1] == NULL) {
        /* Wait for every tracked running/stopped job */
        for (int i = 0; i < JSH_MAX_JOBS; i++) {
            if (!g_jobs[i].used) continue;
            if (g_jobs[i].state == JOB_DONE) continue;
            if (g_jobs[i].state == JOB_STOPPED) {
                /* resume then wait, matching bash wait on a stopped job */
                kill(-g_jobs[i].pgid, SIGCONT);
                g_jobs[i].state = JOB_RUNNING;
            }
            for (int k = 0; k < g_jobs[i].npids; k++) {
                if (g_jobs[i].pids[k] > 0) {
                    int st = 0;
                    waitpid(g_jobs[i].pids[k], &st, 0);
                    g_jobs[i].pids[k] = -1;
                    g_jobs[i].status = st;
                }
            }
            g_jobs[i].state = JOB_DONE;
            g_jobs[i].notified = 0;
            g_last_status = WIFEXITED(g_jobs[i].status)
                          ? WEXITSTATUS(g_jobs[i].status) : 1;
        }
        jobs_notify_done();
        return 0;
    }

    /* Specific job (%N or N) or pid */
    const char *spec = argv[1];
    jsh_job_t *j = NULL;
    if (spec[0] == '%') {
        j = job_parse_spec(spec);
        if (j == NULL) return 1;
    } else {
        /* Prefer job id if it matches a known job; else treat as pid */
        int as_num = atoi(spec);
        j = job_by_id(as_num);
        if (j == NULL) {
            pid_t target = (pid_t)as_num;
            for (int i = 0; i < JSH_MAX_JOBS && j == NULL; i++) {
                if (!g_jobs[i].used) continue;
                for (int k = 0; k < g_jobs[i].npids; k++) {
                    if (g_jobs[i].pids[k] == target) { j = &g_jobs[i]; break; }
                }
            }
            if (j == NULL) {
                int st = 0;
                if (waitpid(target, &st, 0) < 0) {
                    fprintf(stderr, "jsh: wait: pid %d: %s\n", (int)target, strerror(errno));
                    return 1;
                }
                g_last_status = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
                return 0;
            }
        }
    }

    if (j->state == JOB_STOPPED) {
        kill(-j->pgid, SIGCONT);
        j->state = JOB_RUNNING;
    }
    for (int k = 0; k < j->npids; k++) {
        if (j->pids[k] > 0) {
            int st = 0;
            waitpid(j->pids[k], &st, 0);
            j->pids[k] = -1;
            j->status = st;
        }
    }
    j->state = JOB_DONE;
    j->notified = 0;
    g_last_status = WIFEXITED(j->status) ? WEXITSTATUS(j->status) : 1;
    jobs_notify_done();
    return 0;
}

/* Runs one stage of a pipeline (or a lone command when stage_count == 1).
 * Wires stdin/stdout to the given pipe fds first, then applies any explicit
 * redirection from that stage's own argv — explicit redirection always
 * wins over pipe wiring, matching bash (e.g. `cmd1 | cmd2 > file` writes
 * cmd2's output to the file, not into a pipe that has no reader). */
static pid_t spawn_stage(char *argv[], int fd_in, int fd_out, const jsh_redir_t *redir, pid_t *job_pgid) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("jsh: fork failed");
        return -1;
    }

    if (pid == 0) {
        /* ── child process ── */
        /* Join the job's process group (first process becomes leader).
         * Double-call with parent is the standard race-safe pattern. */
        {
            pid_t pgid = (*job_pgid == 0) ? getpid() : *job_pgid;
            setpgid(0, pgid);
        }

        /* Restore default signal behavior so Ctrl+C / Ctrl+Z affect the
         * child, not only when interactive (harmless in scripts). */
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        if (fd_in >= 0) {
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
        }
        if (fd_out >= 0) {
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
        }

        if (redir->in_path != NULL) {
            int fd = open(redir->in_path, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "jsh: %s: %s\n", redir->in_path, strerror(errno));
                _exit(1);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        if (redir->out_path != NULL) {
            int flags = O_WRONLY | O_CREAT | (redir->append ? O_APPEND : O_TRUNC);
            int fd = open(redir->out_path, flags, 0644);
            if (fd < 0) {
                fprintf(stderr, "jsh: %s: %s\n", redir->out_path, strerror(errno));
                _exit(1);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }

        execvp(argv[0], argv);
        fprintf(stderr, "jsh: %s: command not found\n", argv[0]);
        suggest_similar_commands(argv[0]);
        _exit(127);
    }

    /* ── parent process ── */
    {
        /* Always assign a process group so background job tracking works
         * even when stdin is not a tty (scripts / piped tests). Terminal
         * hand-off remains gated on g_interactive. */
        pid_t pgid = (*job_pgid == 0) ? pid : *job_pgid;
        setpgid(pid, pgid);
        if (*job_pgid == 0) *job_pgid = pgid;
    }

    return pid;
}

/* Runs a full pipeline: one or more '|'-separated stages, each with its own
 * optional redirection. For stage_count == 1 this is just "run one external
 * command, possibly with < / > / >> redirection" — no pipe(2) call needed.
 * Builtins (cd, pwd, exit) are NOT supported inside a pipeline or alongside
 * redirection; dispatch_single only calls this for external commands.
 *
 * background != 0 → start the job in the background (do not wait, do not
 * give it the terminal). cmdline is the text stored in the job table. */
static void run_pipeline(char *stage_texts[], int stage_count,
                         int background, const char *cmdline) {
    int prev_read_fd = -1;
    pid_t pids[JSH_MAX_SEGMENTS];
    int npids = 0;
    pid_t job_pgid = 0; /* 0 until the first child is spawned, then that child's pid */

    for (int s = 0; s < stage_count; s++) {
        char *argv[JSH_MAX_ARGS];
        int argc = tokenize(stage_texts[s], argv);

        if (argc == 0) {
            fprintf(stderr, "jsh: syntax error: empty command in pipeline\n");
            if (prev_read_fd >= 0) close(prev_read_fd);
            g_last_status = 2;
            return;
        }

        jsh_redir_t redir;
        if (extract_redirection(argv, &argc, &redir) != 0 || argc == 0) {
            if (prev_read_fd >= 0) close(prev_read_fd);
            g_last_status = 2;
            return;
        }

        int is_last = (s == stage_count - 1);
        int pipefd[2] = { -1, -1 };
        if (!is_last && pipe(pipefd) != 0) {
            perror("jsh: pipe failed");
            if (prev_read_fd >= 0) close(prev_read_fd);
            g_last_status = 1;
            return;
        }

        fflush(stdout); /* keep jsh's own buffered output ordered before the child runs */

        pid_t pid = spawn_stage(argv, prev_read_fd, is_last ? -1 : pipefd[1], &redir, &job_pgid);
        if (pid > 0 && npids < JSH_MAX_SEGMENTS) {
            pids[npids++] = pid;
        }

        if (prev_read_fd >= 0) close(prev_read_fd);
        if (!is_last) {
            close(pipefd[1]);
            prev_read_fd = pipefd[0];
        }
    }

    if (npids == 0 || job_pgid == 0) {
        g_last_status = 1;
        return;
    }

    /* Record the job in the table (both fg and bg — fg needs the slot so
     * Ctrl+Z can leave a Stopped entry behind). */
    jsh_job_t *job = job_alloc();
    if (job == NULL) {
        fprintf(stderr, "jsh: too many jobs (limit %d)\n", JSH_MAX_JOBS);
        /* still try to wait so we don't leak zombies */
        if (!background) {
            for (int i = 0; i < npids; i++) waitpid(pids[i], NULL, 0);
        }
        g_last_status = 1;
        return;
    }
    job->pgid = job_pgid;
    job->npids = npids;
    for (int i = 0; i < npids; i++) job->pids[i] = pids[i];
    job->state = JOB_RUNNING;
    snprintf(job->cmdline, sizeof(job->cmdline), "%s",
             cmdline != NULL ? cmdline : "?");

    if (background) {
        /* Background: leave the job running, print the classic notification,
         * do not hand over the terminal. */
        printf("[%d] %d\n", job->id, (int)job_pgid);
        g_last_status = 0;
        return;
    }

    /* Foreground: give terminal, wait (including WUNTRACED for Ctrl+Z). */
    g_last_status = job_wait_foreground(job);
}

/* Run one command segment (no &&/||/; in it) — everything dispatch_line
 * used to do directly before operator chaining was added. Returns 0 to
 * keep running, -1 if this segment was `exit`. */

/* If the first word of `stage` is an alias, replace that word with the
 * alias value (in a static buffer) and return the new text. Otherwise
 * return `stage` unchanged. Used so aliases work both as standalone
 * commands and as the first stage of a pipeline (e.g. `ll | head`). */
static char *expand_stage_alias(char *stage) {
    static char buf[JSH_MAX_LINE];
    while (*stage == ' ' || *stage == '\t') stage++;
    if (*stage == '\0') return stage;

    /* extract first word (stop at space or shell operator) */
    char word[JSH_MAX_VAR_NAME];
    size_t wi = 0;
    const char *p = stage;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '|' &&
           *p != '<' && *p != '>' && *p != ';' && *p != '&' &&
           wi + 1 < sizeof(word)) {
        word[wi++] = *p++;
    }
    word[wi] = '\0';
    if (wi == 0) return stage;

    jsh_alias_t *a = find_alias(word);
    if (a == NULL) return stage;

    /* rebuild: alias_value + remainder of stage after the word */
    snprintf(buf, sizeof(buf), "%s%s", a->value, p);
    return buf;
}

static int dispatch_single(char *line) {
    /* Trim trailing newline/whitespace first — needed so quote detection in
     * assignment parsing (below) isn't thrown off by a trailing '\n' from
     * fgets() sitting after the closing quote. */
    rtrim(line);

    /* Check for `$name = value` assignment before comment-stripping, so a
     * literal '#' inside a quoted value isn't mistaken for a comment.
     * (Comments are still stripped for everything else below.) */
    {
        char name[JSH_MAX_VAR_NAME];
        char value[JSH_MAX_VAR_VALUE];
        if (try_parse_assignment(line, name, sizeof(name), value, sizeof(value))) {
            /* Support `$i = $i + 1` style increments used in for/while bodies */
            long num;
            if (eval_simple_arith(value, &num) == 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%ld", num);
                set_var(name, buf);
            } else {
                set_var(name, value);
            }
            g_last_status = 0;
            return 0;
        }
    }

    /* Same reasoning as assignment above: must check the raw line before
     * tokenize() expands "$name" into its value */
    {
        char name[JSH_MAX_VAR_NAME];
        if (try_parse_export(line, name, sizeof(name))) {
            if (name[0] == '\0') {
                fprintf(stderr, "usage: export $NAME\n");
                g_last_status = 1;
            } else {
                export_var(name);
                g_last_status = 0;
            }
            return 0;
        }
    }

    strip_comment(line);
    rtrim(line);

    /* Empty line, or nothing left after stripping the comment → skip */
    char *check = line;
    while (*check == ' ' || *check == '\t') check++;
    if (*check == '\0') return 0;

    /* Detect trailing background operator `&` (not `&&`). Quote-aware so
     * `echo "a & b"` is not treated as background. */
    int background = 0;
    {
        char in_quote = '\0';
        char *amp = NULL;
        for (char *p = line; *p != '\0'; p++) {
            if (in_quote != '\0') {
                if (*p == in_quote) in_quote = '\0';
                continue;
            }
            if (*p == '"' || *p == '\'') { in_quote = *p; continue; }
            if (*p == '&' && p[1] != '&') amp = p;
            else if (*p != ' ' && *p != '\t') amp = NULL; /* non-space after & cancels */
        }
        if (amp != NULL) {
            /* Ensure nothing but whitespace follows the & */
            char *q = amp + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '\0') {
                background = 1;
                *amp = '\0';
                rtrim(line);
            }
        }
    }

    /* Save a copy of the (possibly background-stripped) command text for
     * the job table display. */
    char cmdline_buf[JSH_MAX_LINE];
    snprintf(cmdline_buf, sizeof(cmdline_buf), "%s", line);
    if (background) {
        size_t cl = strlen(cmdline_buf);
        if (cl + 2 < sizeof(cmdline_buf)) {
            cmdline_buf[cl] = ' ';
            cmdline_buf[cl + 1] = '&';
            cmdline_buf[cl + 2] = '\0';
        }
    }

    /* Split this segment into pipeline stages at top-level '|'. A single
     * stage with no pipe is just "one command" — builtins (exit/cd/pwd) are
     * only meaningful there, since piping into/out of them doesn't make
     * sense (there's no jsh process on the other end of the pipe to affect). */
    char *stages[JSH_MAX_SEGMENTS];
    int stage_count = split_pipe(line, stages);

    /* Expand aliases in every stage (first word only). This covers both
     * standalone commands and pipelines such as `ll | head`. */
    for (int s = 0; s < stage_count; s++) {
        stages[s] = expand_stage_alias(stages[s]);
    }

    if (stage_count == 1) {
        char *argv[JSH_MAX_ARGS];
        int argc = tokenize(stages[0], argv);
        if (argc == 0) return 0;

        /* Background makes no sense for shell-state builtins — reject. */
        if (background) {
            static const char *no_bg[] = {
                "cd", "exit", "export", "unset", "alias", "unalias",
                "tab-text", "fg", "bg", "jobs", NULL
            };
            for (int i = 0; no_bg[i] != NULL; i++) {
                if (strcmp(argv[0], no_bg[i]) == 0) {
                    fprintf(stderr, "jsh: %s: cannot run in background\n", argv[0]);
                    g_last_status = 1;
                    return 0;
                }
            }
        }

        if (strcmp(argv[0], "exit") == 0) {
            return -1; /* signal the main loop to exit */
        }

        if (strcmp(argv[0], "cd") == 0) {
            g_last_status = builtin_cd(argv);
            return 0;
        }

        if (strcmp(argv[0], "pwd") == 0) {
            g_last_status = builtin_pwd();
            return 0;
        }

        if (strcmp(argv[0], "clear") == 0) {
            g_last_status = builtin_clear();
            return 0;
        }

        /* Prefer external on PATH (toybox after pwc setup); else builtin */
        if (strcmp(argv[0], "ls") == 0) {
            if (!cmd_on_path("ls")) {
                g_last_status = builtin_ls(argv);
                return 0;
            }
            /* fall through -> run_pipeline / execvp */
        }

        if (strcmp(argv[0], "mkdir") == 0) {
            if (!cmd_on_path("mkdir")) {
                g_last_status = builtin_mkdir(argv);
                return 0;
            }
        }

        if (strcmp(argv[0], "rmdir") == 0) {
            if (!cmd_on_path("rmdir")) {
                g_last_status = builtin_rmdir(argv);
                return 0;
            }
        }

        if (strcmp(argv[0], "help") == 0) {
            g_last_status = builtin_help(argv);
            return 0;
        }

        if (strcmp(argv[0], "setup") == 0) {
            g_last_status = builtin_setup(argv);
            return 0;
        }

        if (strcmp(argv[0], "history") == 0) {
            g_last_status = builtin_history();
            return 0;
        }

        if (strcmp(argv[0], "unset") == 0) {
            g_last_status = builtin_unset(argv);
            return 0;
        }

        if (strcmp(argv[0], "type") == 0) {
            g_last_status = builtin_type(argv);
            return 0;
        }

        if (strcmp(argv[0], "which") == 0) {
            g_last_status = builtin_which(argv);
            return 0;
        }

        if (strcmp(argv[0], "alias") == 0) {
            g_last_status = handle_alias_command(stages[0], argv, argc);
            return 0;
        }

        if (strcmp(argv[0], "unalias") == 0) {
            g_last_status = builtin_unalias(argv);
            return 0;
        }

        if (strcmp(argv[0], "jobs") == 0) {
            g_last_status = builtin_jobs(argv);
            return 0;
        }

        if (strcmp(argv[0], "fg") == 0) {
            g_last_status = builtin_fg(argv);
            return 0;
        }

        if (strcmp(argv[0], "bg") == 0) {
            g_last_status = builtin_bg(argv);
            return 0;
        }

        if (strcmp(argv[0], "break") == 0) {
            g_last_status = builtin_break(argv);
            return 0;
        }

        if (strcmp(argv[0], "continue") == 0) {
            g_last_status = builtin_continue(argv);
            return 0;
        }

        if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
            int sr = builtin_source(argv);
            if (sr == -1) return -1;
            g_last_status = sr;
            return 0;
        }

        if (strcmp(argv[0], "wait") == 0) {
            g_last_status = builtin_wait(argv);
            return 0;
        }

        if (strcmp(argv[0], "tab-text") == 0) {
            if (argc == 2 && strcmp(argv[1], "on") == 0) {
                g_tab_text_enabled = 1;
                printf("tab-text: on\n");
                g_last_status = 0;
            } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
                g_tab_text_enabled = 0;
                printf("tab-text: off\n");
                g_last_status = 0;
            } else {
                fprintf(stderr, "usage: tab-text on|off\n");
                g_last_status = 1;
            }
            return 0;
        }

    }

    run_pipeline(stages, stage_count, background, cmdline_buf);
    return 0;
}


/* ── control flow (C-like if / while / for) ───────────────────────
 *
 * Syntax (braces required for the body):
 *   if (condition) { body } [else { body }]
 *   while (condition) { body }
 *   for (init; condition; step) { body }
 *
 * condition may be:
 *   true / false
 *   -f/-d/-e path, -z/-n string
 *   a == b, a != b
 *   a -eq/-ne/-lt/-gt/-le/-ge b   (numeric)
 *   ! condition
 *   anything else → run as a command, true if exit status 0
 *
 * else must share the line with the closing `}` of the if-body when
 * written across multiple lines, e.g.:
 *   if (true) {
 *     echo a
 *   } else {
 *     echo b
 *   }
 */

/* Quote-aware brace depth of `s`. Returns -1 if quotes are unbalanced. */
static int brace_depth(const char *s) {
    char in_quote = '\0';
    int depth = 0;
    for (const char *p = s; *p; p++) {
        if (in_quote) {
            if (*p == '\\' && in_quote == '"' && p[1]) { p++; continue; }
            if (*p == in_quote) in_quote = '\0';
            continue;
        }
        if (*p == '"' || *p == '\'') { in_quote = *p; continue; }
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
    }
    if (in_quote) return -1;
    return depth;
}

/* Skip leading whitespace. */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Does `s` start with keyword `kw` as a whole word? */
static int starts_with_kw(const char *s, const char *kw) {
    s = skip_ws(s);
    size_t klen = strlen(kw);
    if (strncmp(s, kw, klen) != 0) return 0;
    char c = s[klen];
    return (c == '\0' || c == ' ' || c == '\t' || c == '(' || c == '\n');
}

/* True when `stmt` is a complete unit we can parse/run — quotes balanced,
 * braces balanced, and if it begins with if/while/for then at least one
 * `{...}` body has been closed. */
static int statement_is_complete(const char *stmt) {
    if (!quotes_are_balanced(stmt)) return 0;
    int depth = brace_depth(stmt);
    if (depth < 0) return 0;
    if (depth > 0) return 0;

    if (starts_with_kw(stmt, "if") || starts_with_kw(stmt, "while") ||
        starts_with_kw(stmt, "for")) {
        /* Must contain at least one '{' so bare `if (x)` keeps reading */
        int saw_brace = 0;
        char in_quote = '\0';
        for (const char *p = stmt; *p; p++) {
            if (in_quote) {
                if (*p == in_quote) in_quote = '\0';
                continue;
            }
            if (*p == '"' || *p == '\'') { in_quote = *p; continue; }
            if (*p == '{') { saw_brace = 1; break; }
        }
        if (!saw_brace) return 0;
    }
    return 1;
}

/* Extract text between matching parentheses starting at `*pp` which must
 * point at '('. Advances *pp past the closing ')'. Returns 0 on success. */
static int extract_paren(const char **pp, char *out, size_t out_size) {
    const char *p = *pp;
    if (*p != '(') return -1;
    p++;
    int depth = 1;
    char in_quote = '\0';
    size_t oi = 0;
    while (*p && depth > 0) {
        if (in_quote) {
            if (*p == '\\' && in_quote == '"' && p[1]) {
                if (oi + 1 < out_size) out[oi++] = *p++;
                if (oi + 1 < out_size) out[oi++] = *p++;
                continue;
            }
            if (*p == in_quote) in_quote = '\0';
            if (oi + 1 < out_size) out[oi++] = *p;
            p++;
            continue;
        }
        if (*p == '"' || *p == '\'') { in_quote = *p; if (oi + 1 < out_size) out[oi++] = *p; p++; continue; }
        if (*p == '(') depth++;
        else if (*p == ')') {
            depth--;
            if (depth == 0) { p++; break; }
        }
        if (depth > 0 && oi + 1 < out_size) out[oi++] = *p;
        p++;
    }
    out[oi] = '\0';
    if (depth != 0) return -1;
    *pp = p;
    return 0;
}

/* Extract a `{ ... }` block starting at `*pp` (must point at '{').
 * Advances *pp past the closing '}'. Body (without outer braces) goes
 * into out. Returns 0 on success. */
static int extract_brace_block(const char **pp, char *out, size_t out_size) {
    const char *p = skip_ws(*pp);
    if (*p != '{') return -1;
    p++;
    int depth = 1;
    char in_quote = '\0';
    size_t oi = 0;
    while (*p && depth > 0) {
        if (in_quote) {
            if (*p == '\\' && in_quote == '"' && p[1]) {
                if (oi + 1 < out_size) out[oi++] = *p++;
                if (oi + 1 < out_size) out[oi++] = *p++;
                continue;
            }
            if (*p == in_quote) in_quote = '\0';
            if (oi + 1 < out_size) out[oi++] = *p;
            p++;
            continue;
        }
        if (*p == '"' || *p == '\'') { in_quote = *p; if (oi + 1 < out_size) out[oi++] = *p; p++; continue; }
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) { p++; break; }
        }
        if (depth > 0 && oi + 1 < out_size) out[oi++] = *p;
        p++;
    }
    out[oi] = '\0';
    if (depth != 0) return -1;
    *pp = p;
    return 0;
}

/* Expand a condition token (may contain $vars) into `out`. */
static void expand_cond_token(const char *in, char *out, size_t out_size) {
    expand_string(in, out, out_size);
}

/* Forward decl — dispatch_line is defined below; body runner needs it. */
static int dispatch_line(char *line);

/* Run each non-empty line of `body` through dispatch_line. Respects
 * break/continue flags. Returns -1 on exit, 0 otherwise. */
static int run_block_body(const char *body) {
    char linebuf[JSH_MAX_LINE];
    const char *p = body;
    while (*p) {
        if (g_break_flag || g_continue_flag) return 0;

        /* take one line */
        size_t li = 0;
        while (*p && *p != '\n' && li + 1 < sizeof(linebuf)) {
            linebuf[li++] = *p++;
        }
        linebuf[li] = '\0';
        if (*p == '\n') p++;

        const char *check = skip_ws(linebuf);
        if (*check == '\0') continue;

        int r = dispatch_line(linebuf);
        if (r == -1) return -1;
    }
    return 0;
}

/* Evaluate a condition string (contents of `(...)`). Returns 1 (true) or 0. */
static int eval_condition(const char *cond_raw) {
    char cond[JSH_MAX_LINE];
    const char *src = skip_ws(cond_raw);
    /* trim trailing ws into cond */
    size_t len = strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' ||
                       src[len - 1] == '\n')) len--;
    if (len >= sizeof(cond)) len = sizeof(cond) - 1;
    memcpy(cond, src, len);
    cond[len] = '\0';

    if (cond[0] == '\0') return 0;

    /* negation */
    if (cond[0] == '!') {
        const char *rest = skip_ws(cond + 1);
        return !eval_condition(rest);
    }

    /* true / false literals */
    if (strcmp(cond, "true") == 0) return 1;
    if (strcmp(cond, "false") == 0) return 0;

    /* unary file/string tests: -f -d -e -z -n */
    if (cond[0] == '-' && cond[1] != '\0' &&
        (cond[2] == ' ' || cond[2] == '\t')) {
        char op = cond[1];
        const char *arg = skip_ws(cond + 2);
        char expanded[JSH_MAX_VAR_VALUE];
        expand_cond_token(arg, expanded, sizeof(expanded));
        if (op == 'z') return expanded[0] == '\0';
        if (op == 'n') return expanded[0] != '\0';
        if (op == 'f' || op == 'd' || op == 'e') {
            struct stat st;
            if (stat(expanded, &st) != 0) return 0;
            if (op == 'e') return 1;
            if (op == 'f') return S_ISREG(st.st_mode);
            if (op == 'd') return S_ISDIR(st.st_mode);
        }
    }

    /* binary operators — scan for == != -eq -ne -lt -gt -le -ge */
    {
        const char *ops[] = { "==", "!=", "-eq", "-ne", "-lt", "-gt", "-le", "-ge", NULL };
        for (int oi = 0; ops[oi]; oi++) {
            size_t olen = strlen(ops[oi]);
            /* find op as a whole token (spaces around preferred, but also
             * allow tight ==) */
            char in_quote = '\0';
            for (const char *p = cond; *p; p++) {
                if (in_quote) {
                    if (*p == in_quote) in_quote = '\0';
                    continue;
                }
                if (*p == '"' || *p == '\'') { in_quote = *p; continue; }
                if (strncmp(p, ops[oi], olen) == 0) {
                    /* split left / right */
                    char left[JSH_MAX_VAR_VALUE], right[JSH_MAX_VAR_VALUE];
                    size_t llen = (size_t)(p - cond);
                    while (llen > 0 && (cond[llen - 1] == ' ' || cond[llen - 1] == '\t')) llen--;
                    if (llen >= sizeof(left)) llen = sizeof(left) - 1;
                    memcpy(left, cond, llen);
                    left[llen] = '\0';

                    const char *rp = p + olen;
                    while (*rp == ' ' || *rp == '\t') rp++;
                    snprintf(right, sizeof(right), "%s", rp);
                    size_t rlen = strlen(right);
                    while (rlen > 0 && (right[rlen - 1] == ' ' || right[rlen - 1] == '\t'))
                        right[--rlen] = '\0';

                    char le[JSH_MAX_VAR_VALUE], re[JSH_MAX_VAR_VALUE];
                    expand_cond_token(left, le, sizeof(le));
                    expand_cond_token(right, re, sizeof(re));

                    if (strcmp(ops[oi], "==") == 0) return strcmp(le, re) == 0;
                    if (strcmp(ops[oi], "!=") == 0) return strcmp(le, re) != 0;

                    long a = strtol(le, NULL, 10);
                    long b = strtol(re, NULL, 10);
                    if (strcmp(ops[oi], "-eq") == 0) return a == b;
                    if (strcmp(ops[oi], "-ne") == 0) return a != b;
                    if (strcmp(ops[oi], "-lt") == 0) return a < b;
                    if (strcmp(ops[oi], "-gt") == 0) return a > b;
                    if (strcmp(ops[oi], "-le") == 0) return a <= b;
                    if (strcmp(ops[oi], "-ge") == 0) return a >= b;
                }
            }
        }
    }

    /* Fallback: run as a command pipeline; true if exit status 0.
     * Use a mutable copy because dispatch_line mutates its argument. */
    {
        char cmd[JSH_MAX_LINE];
        snprintf(cmd, sizeof(cmd), "%s", cond);
        dispatch_line(cmd);
        return g_last_status == 0;
    }
}

/* Try to evaluate a simple arithmetic expression for for-loop steps:
 * supports N, $var, $var + N, $var - N after expansion of vars. */
static int eval_simple_arith(const char *expr, long *out) {
    char expanded[JSH_MAX_VAR_VALUE];
    expand_string(expr, expanded, sizeof(expanded));
    const char *p = skip_ws(expanded);
    char *end = NULL;
    long a = strtol(p, &end, 10);
    if (end == p) return -1;
    p = skip_ws(end);
    if (*p == '\0') { *out = a; return 0; }
    char op = *p;
    if (op != '+' && op != '-') return -1;
    p = skip_ws(p + 1);
    long b = strtol(p, &end, 10);
    if (end == p) return -1;
    p = skip_ws(end);
    if (*p != '\0') return -1;
    *out = (op == '+') ? (a + b) : (a - b);
    return 0;
}

/* Handle `$name = expr` where expr may be simple arithmetic. Falls back
 * to normal assignment parsing. */
static void run_assignment_or_arith(const char *text) {
    char name[JSH_MAX_VAR_NAME];
    char value[JSH_MAX_VAR_VALUE];
    if (try_parse_assignment(text, name, sizeof(name), value, sizeof(value))) {
        long num;
        if (eval_simple_arith(value, &num) == 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%ld", num);
            set_var(name, buf);
        } else {
            set_var(name, value);
        }
        g_last_status = 0;
        return;
    }
    /* not an assignment — run as a normal command line */
    char buf[JSH_MAX_LINE];
    snprintf(buf, sizeof(buf), "%s", text);
    dispatch_line(buf);
}

static int run_if_statement(const char *stmt) {
    const char *p = skip_ws(stmt);
    if (strncmp(p, "if", 2) != 0) return 1;
    p = skip_ws(p + 2);
    char cond[JSH_MAX_LINE];
    if (extract_paren(&p, cond, sizeof(cond)) != 0) {
        fprintf(stderr, "jsh: if: expected (condition)\\n");
        g_last_status = 2;
        return 0;
    }
    p = skip_ws(p);
    char then_body[JSH_MAX_STMT];
    if (extract_brace_block(&p, then_body, sizeof(then_body)) != 0) {
        fprintf(stderr, "jsh: if: expected { body }\\n");
        g_last_status = 2;
        return 0;
    }
    p = skip_ws(p);
    char else_body[JSH_MAX_STMT];
    int has_else = 0;
    if (starts_with_kw(p, "else")) {
        p = skip_ws(p + 4);
        if (extract_brace_block(&p, else_body, sizeof(else_body)) != 0) {
            fprintf(stderr, "jsh: if: else without { body }\\n");
            g_last_status = 2;
            return 0;
        }
        has_else = 1;
    }

    int r = 0;
    if (eval_condition(cond)) {
        r = run_block_body(then_body);
    } else if (has_else) {
        r = run_block_body(else_body);
    } else {
        g_last_status = 0;
    }
    return r;
}

static int run_while_statement(const char *stmt) {
    const char *p = skip_ws(stmt);
    if (strncmp(p, "while", 5) != 0) return 1;
    p = skip_ws(p + 5);
    char cond[JSH_MAX_LINE];
    if (extract_paren(&p, cond, sizeof(cond)) != 0) {
        fprintf(stderr, "jsh: while: expected (condition)\\n");
        g_last_status = 2;
        return 0;
    }
    p = skip_ws(p);
    char body[JSH_MAX_STMT];
    if (extract_brace_block(&p, body, sizeof(body)) != 0) {
        fprintf(stderr, "jsh: while: expected { body }\\n");
        g_last_status = 2;
        return 0;
    }

    if (g_loop_depth >= JSH_MAX_LOOP_DEPTH) {
        fprintf(stderr, "jsh: while: max loop depth exceeded\\n");
        g_last_status = 1;
        return 0;
    }

    g_loop_depth++;
    int r = 0;
    int guard = 0;
    while (eval_condition(cond)) {
        if (++guard > 100000) {
            fprintf(stderr, "jsh: while: iteration limit exceeded (possible infinite loop)\n");
            g_last_status = 1;
            break;
        }
        g_break_flag = 0;
        g_continue_flag = 0;
        r = run_block_body(body);
        if (r == -1) break;
        if (g_break_flag) { g_break_flag = 0; break; }
        if (g_continue_flag) { g_continue_flag = 0; continue; }
    }
    g_loop_depth--;
    if (r != -1) g_last_status = 0;
    return r;
}

static int run_for_statement(const char *stmt) {
    const char *p = skip_ws(stmt);
    if (strncmp(p, "for", 3) != 0) return 1;
    p = skip_ws(p + 3);

    /* for (init; cond; step) { body } */
    char paren[JSH_MAX_LINE];
    if (extract_paren(&p, paren, sizeof(paren)) != 0) {
        fprintf(stderr, "jsh: for: expected (init; cond; step)\\n");
        g_last_status = 2;
        return 0;
    }
    p = skip_ws(p);
    char body[JSH_MAX_STMT];
    if (extract_brace_block(&p, body, sizeof(body)) != 0) {
        fprintf(stderr, "jsh: for: expected { body }\\n");
        g_last_status = 2;
        return 0;
    }

    /* split paren into init / cond / step at top-level ';' */
    char init[JSH_MAX_LINE], cond[JSH_MAX_LINE], step[JSH_MAX_LINE];
    init[0] = cond[0] = step[0] = '\0';
    {
        char *parts[3] = { init, cond, step };
        int pi = 0;
        size_t oi = 0;
        char in_quote = '\0';
        for (const char *q = paren; *q && pi < 3; q++) {
            if (in_quote) {
                if (*q == in_quote) in_quote = '\0';
                if (oi + 1 < JSH_MAX_LINE) parts[pi][oi++] = *q;
                continue;
            }
            if (*q == '"' || *q == '\'') { in_quote = *q; if (oi + 1 < JSH_MAX_LINE) parts[pi][oi++] = *q; continue; }
            if (*q == ';') {
                parts[pi][oi] = '\0';
                pi++;
                oi = 0;
                continue;
            }
            if (oi + 1 < JSH_MAX_LINE) parts[pi][oi++] = *q;
        }
        if (pi < 3) parts[pi][oi] = '\0';
        if (pi != 2) {
            fprintf(stderr, "jsh: for: expected (init; cond; step)\\n");
            g_last_status = 2;
            return 0;
        }
    }

    if (g_loop_depth >= JSH_MAX_LOOP_DEPTH) {
        fprintf(stderr, "jsh: for: max loop depth exceeded\\n");
        g_last_status = 1;
        return 0;
    }

    /* init */
    {
        const char *ip = skip_ws(init);
        if (*ip) run_assignment_or_arith(ip);
    }

    g_loop_depth++;
    int r = 0;
    int guard = 0;
    while (1) {
        if (++guard > 100000) {
            fprintf(stderr, "jsh: for: iteration limit exceeded (possible infinite loop)\n");
            g_last_status = 1;
            break;
        }
        const char *cp = skip_ws(cond);
        /* empty condition → true (C semantics) */
        if (*cp && !eval_condition(cp)) break;

        g_break_flag = 0;
        g_continue_flag = 0;
        r = run_block_body(body);
        if (r == -1) break;
        if (g_break_flag) { g_break_flag = 0; break; }
        /* continue falls through to step */

        const char *sp = skip_ws(step);
        if (*sp) run_assignment_or_arith(sp);
        if (g_continue_flag) g_continue_flag = 0;
    }
    g_loop_depth--;
    if (r != -1) g_last_status = 0;
    return r;
}

/* Run a single control-flow statement that begins at `stmt`.
 * Returns 0 or -1 like dispatch_line. On parse failure sets status and
 * returns 0. Used by try_run_control after isolating the CF portion. */
static int run_control_kw(const char *stmt) {
    if (starts_with_kw(stmt, "if")) return run_if_statement(stmt);
    if (starts_with_kw(stmt, "while")) return run_while_statement(stmt);
    if (starts_with_kw(stmt, "for")) return run_for_statement(stmt);
    return 1;
}

/* Find where an if/while/for statement ends (past its final '}').
 * Returns pointer to the first char after the statement, or NULL on error. */
static const char *control_stmt_end(const char *line) {
    const char *p = skip_ws(line);
    if (starts_with_kw(p, "if")) p = skip_ws(p + 2);
    else if (starts_with_kw(p, "while")) p = skip_ws(p + 5);
    else if (starts_with_kw(p, "for")) p = skip_ws(p + 3);
    else return NULL;

    char tmp[JSH_MAX_LINE];
    if (extract_paren(&p, tmp, sizeof(tmp)) != 0) return NULL;
    p = skip_ws(p);
    if (extract_brace_block(&p, tmp, sizeof(tmp)) != 0) return NULL;
    p = skip_ws(p);
    if (starts_with_kw(p, "else")) {
        p = skip_ws(p + 4);
        if (extract_brace_block(&p, tmp, sizeof(tmp)) != 0) return NULL;
    }
    return p;
}

/* If `line` begins with if/while/for, run that statement (only the CF
 * portion) and then any trailing `; ...` commands. Returns 0/-1 on
 * handled, or 1 if not control flow. */
static int try_run_control(char *line) {
    if (!starts_with_kw(line, "if") &&
        !starts_with_kw(line, "while") &&
        !starts_with_kw(line, "for")) {
        return 1;
    }

    const char *end = control_stmt_end(line);
    if (end == NULL) {
        /* fall through to dedicated runner for a proper error message */
        return run_control_kw(line);
    }

    /* Isolate the control-flow text in a mutable buffer */
    char cfbuf[JSH_MAX_STMT];
    size_t cflen = (size_t)(end - line);
    if (cflen >= sizeof(cfbuf)) cflen = sizeof(cfbuf) - 1;
    memcpy(cfbuf, line, cflen);
    cfbuf[cflen] = '\0';

    int r = run_control_kw(cfbuf);
    if (r == -1) return -1;

    /* Trailing text after the CF statement (e.g. `; echo x; $i = $i + 1`) */
    const char *rest = skip_ws(end);
    if (*rest == ';') rest = skip_ws(rest + 1);
    if (*rest != '\0') {
        char restbuf[JSH_MAX_LINE];
        snprintf(restbuf, sizeof(restbuf), "%s", rest);
        /* If break/continue was set by the CF body, skip the rest of the
         * physical line (matches "break; echo should-not-run"). */
        if (g_break_flag || g_continue_flag) return 0;
        r = dispatch_line(restbuf);
    }
    return r;
}

/* Process a full input line — splits it into &&/||/;-separated segments and
 * runs each one via dispatch_single(), short-circuiting based on the exit
 * status of the previous segment (g_last_status). Returns 0 to keep running,
 * -1 if `exit` was hit anywhere in the chain. */
static int dispatch_line(char *line) {
    rtrim(line);

    if (!quotes_are_balanced(line)) {
        fprintf(stderr, "jsh: unterminated quote\n");
        g_last_status = 2;
        return 0;
    }

    /* Control-flow statements own the whole line (no &&/||/; chaining
     * mixed with if/while/for in this version). */
    {
        int cr = try_run_control(line);
        if (cr != 1) return cr; /* 0 = ran ok, -1 = exit */
    }

    char *segments[JSH_MAX_SEGMENTS];
    jsh_op_t ops[JSH_MAX_SEGMENTS];
    int seg_count = split_operators(line, segments, ops);

    for (int i = 0; i < seg_count; i++) {
        /* && only runs if the previous segment succeeded (status 0);
         * || only runs if it failed. Checking g_last_status directly here
         * — rather than tracking a separate "skip" flag across the loop —
         * is enough because we only ever need to know the *immediately
         * preceding* result, which is exactly what g_last_status already is. */
        if (ops[i] == OP_AND && g_last_status != 0) continue;
        if (ops[i] == OP_OR  && g_last_status == 0) continue;

        int result = dispatch_single(segments[i]);
        if (result == -1) return -1;
    }

    return 0;
}

/* jsh owns its $PATH at startup instead of blindly trusting whatever the
 * parent process handed it. This guarantees ~/.local/bin (where `pwc link`
 * places things) is always reachable, even if the inherited PATH doesn't
 * have it — which is exactly the bug we just hit: `pwc link` symlinked a
 * program into ~/.local/bin, but jsh couldn't find it because that
 * directory wasn't on PATH at all. */
static void setup_path(void) {
    const char *home = getenv("HOME");
    if (home == NULL) home = "/";

    char local_bin[PATH_MAX];
    snprintf(local_bin, sizeof(local_bin), "%s/.local/bin", home);

    const char *inherited = getenv("PATH");
    if (inherited == NULL) inherited = "/usr/bin:/bin";

    /* Don't duplicate it if it's already there for some reason */
    if (strstr(inherited, local_bin) != NULL) {
        return;
    }

    char new_path[PATH_MAX * 2];
    snprintf(new_path, sizeof(new_path), "%s:%s", local_bin, inherited);
    setenv("PATH", new_path, 1);
}

/* ── raw-mode interactive line editor ────────────────────────────────
 * Only used when stdin is a real terminal (see `interactive` in main()).
 * Piped/non-interactive input still goes through plain fgets() below,
 * completely unchanged — this only replaces how a human typing at a
 * real keyboard gets their line read, so arrow-key history recall and
 * Tab completion work. */

static struct termios g_orig_termios;
static int g_raw_mode_active = 0;

static void disable_raw_mode(void) {
    if (g_raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode_active = 0;
    }
}

static void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    /* Turn off: local echo (we draw the line ourselves), canonical mode
     * (read byte-by-byte instead of waiting for Enter), and signal
     * generation (Ctrl+C is handled manually below instead of raising
     * SIGINT, so we can just clear the in-progress line like real shells do) */
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_mode_active = 1;
    atexit(disable_raw_mode); /* safety net if jsh exits unexpectedly mid-line */
}

/* Reads one line interactively with prompt redraw, history recall
 * (Up/Down), cursor movement (Left/Right), Backspace, Tab completion, and
 * Ctrl+C (cancels the current line) / Ctrl+D (EOF on an empty line).
 * Returns 1 with the line in `out` on Enter, 0 on EOF. */
/* Word-boundary helpers for word-wise navigation/deletion (Ctrl+Left/Right,
 * Alt+B/F, Ctrl+W) — skip any run of spaces, then skip the word itself. */
static size_t word_start_before(const char *buf, size_t cursor) {
    size_t i = cursor;
    while (i > 0 && buf[i - 1] == ' ') i--;
    while (i > 0 && buf[i - 1] != ' ') i--;
    return i;
}

static size_t word_end_after(const char *buf, size_t len, size_t cursor) {
    size_t i = cursor;
    while (i < len && buf[i] == ' ') i++;
    while (i < len && buf[i] != ' ') i++;
    return i;
}

/* Finds a "here's probably what you're typing" suggestion, shown as dimmed
 * ghost text ahead of the cursor while typing. Two-tier lookup:
 *
 *   1. History — most recent entry that starts with `buf` and is longer
 *      than it. Searches newest-first so the most recently used matching
 *      command wins (most likely to still be relevant). This is the same
 *      signal a future learning layer (J2k) would read from — right now
 *      it's a direct history lookup, but the data it's built on (what the
 *      user actually typed, in order) is exactly what a smarter ranking
 *      would need too.
 *
 *   2. $PATH cache — only tried if history has no match, and only while
 *      still typing the first word (a ghost suggestion for the 2nd+ word
 *      would need to guess a filename, which is far less reliable than
 *      guessing a command name). Picks the alphabetically-first match from
 *      g_path_cmds[] (already sorted), so the choice is predictable rather
 *      than arbitrary — this is what surfaces things like `am` for "a" even
 *      if the user has never typed `am` before, which is what was actually
 *      being asked for: seeing what else exists, not just repeating history. */
static int find_ghost_suggestion(const char *buf, char *out, size_t out_size) {
    size_t blen = strlen(buf);
    if (blen == 0) return 0;

    for (int i = g_history_count - 1; i >= 0; i--) {
        if (strncmp(g_history[i], buf, blen) == 0 && strlen(g_history[i]) > blen) {
            snprintf(out, out_size, "%s", g_history[i]);
            return 1;
        }
    }

    if (strchr(buf, ' ') == NULL) {
        for (int i = 0; i < g_path_cmd_count; i++) {
            if (strncmp(g_path_cmds[i], buf, blen) == 0 && strlen(g_path_cmds[i]) > blen) {
                snprintf(out, out_size, "%s", g_path_cmds[i]);
                return 1;
            }
        }
    }

    return 0;
}

/* History-only half of find_ghost_suggestion(), used specifically for
 * Tab-accept decisions. A history match is a full remembered command line —
 * unambiguous, so Tab should accept it immediately. The $PATH-fallback tier
 * is deliberately NOT included here: it just picks the alphabetically-first
 * candidate as a *preview*, but when there are several real matches, Tab
 * should go through find_completions()/the double-Tab list instead of
 * silently locking in that one guess (see the Tab handler below — this is
 * the fix for a bug where multi-match command names never reached the list,
 * because the $PATH ghost always intercepted the first Tab press first). */
static int find_history_ghost(const char *buf, char *out, size_t out_size) {
    size_t blen = strlen(buf);
    if (blen == 0) return 0;

    for (int i = g_history_count - 1; i >= 0; i--) {
        if (strncmp(g_history[i], buf, blen) == 0 && strlen(g_history[i]) > blen) {
            snprintf(out, out_size, "%s", g_history[i]);
            return 1;
        }
    }
    return 0;
}

/* Counts the visible (on-screen) length of a string, skipping over ANSI
 * escape sequences (\033[...letter) — used to figure out how many actual
 * terminal columns the prompt takes up, since the color codes themselves
 * don't occupy any screen space. */
static int visible_len(const char *s) {
    int len = 0;
    for (const char *p = s; *p != '\0'; ) {
        if (p[0] == '\033' && p[1] == '[') {
            p += 2;
            while (*p != '\0' && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) p++;
            if (*p != '\0') p++; /* skip the final letter that ends the sequence */
            continue;
        }
        len++;
        p++;
    }
    return len;
}

/* Current terminal width in columns, via the kernel's own idea of the
 * window size (ioctl). This is what makes the line editor aware that a
 * long command on a narrow phone screen will WRAP across multiple visual
 * rows — without this, the old single-row "\r\033[K" redraw left stale,
 * duplicated prompt fragments behind whenever a line got long enough to
 * wrap (exactly the bug found during testing). Falls back to 80 if the
 * ioctl fails for some reason (e.g. not actually attached to a terminal). */
static int get_terminal_cols(void) {
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;
}

static int read_line(const char *prompt, char *out, size_t out_size) {
    char buf[JSH_MAX_LINE];
    size_t len = 0, cursor = 0;
    buf[0] = '\0';

    int hist_pos = g_history_count; /* == count means "not browsing, fresh line" */
    char saved_current[JSH_MAX_LINE];
    saved_current[0] = '\0';

    /* Tracks whether the previous keystroke was a Tab that hit multiple
     * completion candidates — used so the full match list only appears on
     * a deliberate second Tab press in a row, not the first one. Any other
     * key resets this back to 0. See the Tab handling block below. */
    int tab_pending = 0;

    /* How many terminal rows the previous redraw spanned — needed so the
     * NEXT redraw can move back up to the true top of the render before
     * clearing, instead of only clearing the current row. Without this, a
     * long command that wraps across multiple rows on a narrow (phone)
     * screen leaves stale/duplicated prompt fragments behind on every
     * keystroke — this was a real bug found during on-device testing. */
    int prev_rows = 1;

    enable_raw_mode();

    for (;;) {
        char suggestion[JSH_MAX_LINE];
        int has_suggestion = g_tab_text_enabled && (cursor == len) &&
                              find_ghost_suggestion(buf, suggestion, sizeof(suggestion));

        int cols = get_terminal_cols();
        int prompt_vis = visible_len(prompt);
        size_t displayed_len = has_suggestion ? strlen(suggestion) : len;

        /* Move up to the top row of the PREVIOUS render, then clear
         * everything from there to the end of the screen. "\033[J" clears
         * every wrapped row below in one shot, so this correctly erases a
         * multi-row render regardless of how many rows it spanned. */
        if (prev_rows > 1) printf("\033[%dA", prev_rows - 1);
        printf("\r\033[J");

        fputs(prompt, stdout);
        fwrite(buf, 1, len, stdout);
        if (has_suggestion) {
            /* \033[2m = dim/faint — greys out the suggested remainder so
             * it reads as "preview", not text that's actually been typed */
            printf("\033[2m%s\033[0m", suggestion + len);
        }

        /* Position the real cursor using absolute row/col math instead of a
         * simple "move left N columns" — a relative move breaks the moment
         * the content spans more than one wrapped row, since moving left
         * from row 2 doesn't reliably walk back onto row 1 in every
         * terminal. Computing both positions as (row, col) from the total
         * character count and reconciling them works regardless of wrap. */
        int end_pos = prompt_vis + (int)displayed_len;
        int target_pos = prompt_vis + (int)cursor;
        int end_row = end_pos / cols;
        int target_row = target_pos / cols, target_col = target_pos % cols;

        if (end_row > target_row) printf("\033[%dA", end_row - target_row);
        printf("\r");
        if (target_col > 0) printf("\033[%dC", target_col);
        fflush(stdout);

        /* Record how tall THIS render was so the next iteration knows how
         * far up it needs to move before clearing. +1 converts a 0-based
         * row index into a row count. */
        prev_rows = end_row + 1;

        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n != 1) {
            disable_raw_mode();
            return 0;
        }

        /* Any key other than Tab cancels a pending "press Tab again to see
         * the full list" state — the double-tap only counts if it's truly
         * back-to-back, otherwise it's confusing (e.g. typing more letters
         * then hitting Tab once shouldn't suddenly dump a list). */
        if (c != '\t') tab_pending = 0;

        if (c == '\r' || c == '\n') {
            printf("\n");
            break;
        }

        if (c == 4) { /* Ctrl+D */
            if (len == 0) {
                disable_raw_mode();
                printf("\n");
                return 0;
            }
            continue; /* mid-line Ctrl+D: ignored for now (bash deletes char under cursor) */
        }

        if (c == 18) { /* Ctrl+R — reverse incremental history search */
            char query[JSH_MAX_LINE];
            size_t qlen = 0;
            query[0] = '\0';
            int match_idx = -1;
            for (;;) {
                /* Draw search UI on the current editor row */
                if (prev_rows > 1) printf("\033[%dA", prev_rows - 1);
                printf("\r\033[J");
                if (match_idx >= 0) {
                    printf("(reverse-i-search)`%s': %s", query, g_history[match_idx]);
                } else {
                    printf("(failed reverse-i-search)`%s': ", query);
                }
                fflush(stdout);
                prev_rows = 1;

                unsigned char rc;
                if (read(STDIN_FILENO, &rc, 1) != 1) break;

                if (rc == '\r' || rc == '\n') {
                    /* Accept match into the main buffer */
                    if (match_idx >= 0) {
                        snprintf(buf, sizeof(buf), "%s", g_history[match_idx]);
                        len = strlen(buf);
                        cursor = len;
                    }
                    printf("\n");
                    prev_rows = 1;
                    break;
                }
                if (rc == 3 || rc == 7 || rc == 27) { /* Ctrl+C / Ctrl+G / ESC cancel */
                    printf("\n");
                    prev_rows = 1;
                    /* leave buf unchanged */
                    break;
                }
                if (rc == 127 || rc == 8) { /* backspace in query */
                    if (qlen > 0) query[--qlen] = '\0';
                } else if (rc == 18) {
                    /* another Ctrl+R: find older match */
                    int start = (match_idx > 0) ? match_idx - 1 : g_history_count - 1;
                    match_idx = -1;
                    for (int hi = start; hi >= 0; hi--) {
                        if (qlen == 0 || strstr(g_history[hi], query) != NULL) {
                            match_idx = hi;
                            break;
                        }
                    }
                    continue;
                } else if (rc >= 32 && rc < 127 && qlen + 1 < sizeof(query)) {
                    query[qlen++] = (char)rc;
                    query[qlen] = '\0';
                } else {
                    continue;
                }

                /* Search newest-first for query */
                match_idx = -1;
                for (int hi = g_history_count - 1; hi >= 0; hi--) {
                    if (qlen == 0 || strstr(g_history[hi], query) != NULL) {
                        match_idx = hi;
                        break;
                    }
                }
            }
            continue;
        }

        if (c == 3) { /* Ctrl+C — cancel this line, start fresh, don't exit jsh */
            printf("^C\n");
            len = 0; cursor = 0; buf[0] = '\0';
            hist_pos = g_history_count;
            prev_rows = 1; /* we just moved to a fresh line — nothing above to clear */
            continue;
        }

        if (c == 127 || c == 8) { /* Backspace */
            if (cursor > 0) {
                memmove(buf + cursor - 1, buf + cursor, len - cursor);
                cursor--; len--;
                buf[len] = '\0';
            }
            continue;
        }

        if (c == 1) { /* Ctrl+A — move to start of line */
            cursor = 0;
            continue;
        }

        if (c == 5) { /* Ctrl+E — move to end of line */
            cursor = len;
            continue;
        }

        if (c == 21) { /* Ctrl+U — delete from cursor back to start of line */
            memmove(buf, buf + cursor, len - cursor);
            len -= cursor;
            cursor = 0;
            buf[len] = '\0';
            continue;
        }

        if (c == 11) { /* Ctrl+K — delete from cursor to end of line */
            len = cursor;
            buf[len] = '\0';
            continue;
        }

        if (c == 23) { /* Ctrl+W — delete the word before the cursor */
            size_t wstart = word_start_before(buf, cursor);
            memmove(buf + wstart, buf + cursor, len - cursor);
            len -= (cursor - wstart);
            cursor = wstart;
            buf[len] = '\0';
            continue;
        }

        if (c == 12) { /* Ctrl+L — clear the screen, redraw just the prompt+line */
            printf("\033[H\033[2J");
            prev_rows = 1; /* screen is blank now — nothing above to clear next time */
            continue; /* the top of the loop redraws prompt+buf right after */
        }

        if (c == '\t') {
            /* If there's an active HISTORY-based ghost suggestion, Tab
             * accepts the whole thing immediately — it's a full remembered
             * command line, so there's nothing ambiguous about it.
             * ($PATH-fallback ghost is deliberately excluded here — see
             * find_history_ghost()'s comment: that tier's preview is just
             * the alphabetically-first candidate, and if there are actually
             * several real matches, Tab should go through the normal
             * completion + double-Tab list below instead of silently
             * locking in that one guess.) */
            char suggestion[JSH_MAX_LINE];
            if (g_tab_text_enabled && cursor == len &&
                find_history_ghost(buf, suggestion, sizeof(suggestion))) {
                snprintf(buf, sizeof(buf), "%s", suggestion);
                len = strlen(buf);
                cursor = len;
                continue;
            }

            /* No history ghost active — fall back to word completion
             * (command name or filename). This is also where a $PATH-tier
             * ghost preview (if one was showing) gets resolved for real:
             * single match fills in immediately, multiple matches arm/show
             * the double-Tab list. */
            size_t wstart = cursor;
            while (wstart > 0 && buf[wstart - 1] != ' ') wstart--;

            char word[JSH_MAX_LINE];
            size_t wlen = cursor - wstart;
            if (wlen >= sizeof(word)) wlen = sizeof(word) - 1;
            memcpy(word, buf + wstart, wlen);
            word[wlen] = '\0';

            int is_first_word = (wstart == 0);
            char *matches[JSH_MAX_COMPLETIONS];
            int mcount = find_completions(word, is_first_word, matches);

            if (mcount == 1) {
                /* Single match: always fill immediately, no matter whether
                 * tab-text is on/off or this is a first or second Tap —
                 * this isn't the "noisy" part, it's just useful. */
                size_t mlen = strlen(matches[0]);
                if (len - wlen + mlen < sizeof(buf) - 1) {
                    memmove(buf + wstart + mlen, buf + cursor, len - cursor);
                    memcpy(buf + wstart, matches[0], mlen);
                    len = len - wlen + mlen;
                    cursor = wstart + mlen;
                    buf[len] = '\0';
                }
                tab_pending = 0;
            } else if (mcount > 1 && g_tab_text_enabled) {
                /* Multiple matches: standard bash behavior — first Tab does
                 * nothing but arms tab_pending, second Tab in a row (with no
                 * other key in between) shows the full list. This is what
                 * keeps a stray Tab press from suddenly dumping a wall of
                 * text: it takes a deliberate double-press. */
                if (tab_pending) {
                    printf("\n");
                    for (int i = 0; i < mcount; i++) {
                        const char *desc = lookup_cmd_description(matches[i]);
                        if (desc != NULL) {
                            printf("  %-16s %s\n", matches[i], desc);
                        } else {
                            printf("  %s\n", matches[i]);
                        }
                    }
                    tab_pending = 0;
                    prev_rows = 1; /* just printed real output — nothing above to clear next time */
                } else {
                    tab_pending = 1;
                }
            }
            /* mcount > 1 with tab-text off: intentionally does nothing at
             * all — no list, no bell, matching the plain "off" behavior
             * requested for the beta toggle. */

            for (int i = 0; i < mcount; i++) free(matches[i]);
            continue;
        }

        if (c == 27) { /* ESC — start of an escape sequence (arrow key, Home/End,
                          * Delete, or a word-jump combo like Ctrl+Left/Right) */
            unsigned char next;
            if (read(STDIN_FILENO, &next, 1) != 1) continue;

            /* Meta+B / Meta+F (Alt+Left-word / Alt+Right-word in terminals
             * that send ESC directly followed by the letter, no '[') */
            if (next == 'b') { cursor = word_start_before(buf, cursor); continue; }
            if (next == 'f') { cursor = word_end_after(buf, len, cursor); continue; }

            if (next != '[') continue; /* unrecognized — ignore */

            /* Read the rest of a CSI sequence: optional digits/';', ending in
             * a letter (A-Z) or '~'. Covers plain arrows (ESC[A), Home/End
             * (ESC[H / ESC[F / ESC[1~ / ESC[4~), Delete (ESC[3~), and the
             * modifier forms xterm sends for Ctrl/Alt+Arrow, e.g. ESC[1;5C. */
            char params[16];
            size_t plen = 0;
            unsigned char final = 0;
            for (;;) {
                unsigned char b;
                if (read(STDIN_FILENO, &b, 1) != 1) break;
                if ((b >= '0' && b <= '9') || b == ';') {
                    if (plen + 1 < sizeof(params)) params[plen++] = (char)b;
                    continue;
                }
                final = b; /* letter or '~' — sequence complete */
                break;
            }
            params[plen] = '\0';

            /* Modifier is the number after ';' in forms like "1;5C" — 3 = Alt,
             * 5 = Ctrl. Both trigger word-wise movement here; plain arrows
             * (no ';') fall through to single-character movement below. */
            int has_modifier = (strchr(params, ';') != NULL);

            if (final == 'A') { /* Up — older history */
                if (g_history_count > 0 && hist_pos > 0) {
                    if (hist_pos == g_history_count) {
                        snprintf(saved_current, sizeof(saved_current), "%s", buf);
                    }
                    hist_pos--;
                    snprintf(buf, sizeof(buf), "%s", g_history[hist_pos]);
                    len = strlen(buf);
                    cursor = len;
                }
            } else if (final == 'B') { /* Down — newer history, or back to what was being typed */
                if (hist_pos < g_history_count) {
                    hist_pos++;
                    if (hist_pos == g_history_count) {
                        snprintf(buf, sizeof(buf), "%s", saved_current);
                    } else {
                        snprintf(buf, sizeof(buf), "%s", g_history[hist_pos]);
                    }
                    len = strlen(buf);
                    cursor = len;
                }
            } else if (final == 'C') { /* Right (or Ctrl/Alt+Right = word-forward) */
                if (has_modifier) {
                    cursor = word_end_after(buf, len, cursor);
                } else if (cursor == len) {
                    /* At the end of the buffer with nothing to move past —
                     * accept a ghost-text suggestion here too, same as Tab,
                     * since that's the natural "keep going" key on a phone.
                     * Gated behind g_tab_text_enabled, same as everywhere else. */
                    char suggestion[JSH_MAX_LINE];
                    if (g_tab_text_enabled &&
                        find_ghost_suggestion(buf, suggestion, sizeof(suggestion))) {
                        snprintf(buf, sizeof(buf), "%s", suggestion);
                        len = strlen(buf);
                        cursor = len;
                    }
                } else {
                    cursor++;
                }
            } else if (final == 'D') { /* Left (or Ctrl/Alt+Left = word-backward) */
                cursor = has_modifier ? word_start_before(buf, cursor)
                                      : (cursor > 0 ? cursor - 1 : cursor);
            } else if (final == 'H' || strcmp(params, "1") == 0) { /* Home */
                cursor = 0;
            } else if (final == 'F' || strcmp(params, "4") == 0) { /* End */
                cursor = len;
            } else if (strcmp(params, "3") == 0 && final == '~') { /* Delete (forward) */
                if (cursor < len) {
                    memmove(buf + cursor, buf + cursor + 1, len - cursor - 1);
                    len--;
                    buf[len] = '\0';
                }
            }
            continue;
        }

        /* Printable character: insert at cursor */
        if (c >= 32 && c < 127 && len + 1 < sizeof(buf)) {
            memmove(buf + cursor + 1, buf + cursor, len - cursor);
            buf[cursor] = (char)c;
            cursor++; len++;
            buf[len] = '\0';
        }
    }

    disable_raw_mode();
    snprintf(out, out_size, "%s", buf);
    return 1;
}


int main(void) {
    /* Force stdout to be line-buffered even when jsh isn't attached to a
     * real terminal (e.g. when its output is piped somewhere). By default
     * stdout is FULLY buffered in that case, which is exactly what caused
     * the interleaving bug noted during testing: jsh's own printf() output
     * (prompt, pwd, etc.) could sit in its buffer while a freshly forked
     * child's output reached the terminal first, making the two appear
     * out of order. Line-buffering flushes on every '\n', keeping jsh's
     * own output in sync with whatever children print. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Ensure HOME is set. If the Android app (or parent) didn't pass HOME,
     * default it to the current working directory. This makes the prompt
     * show "~" (Termux-style) instead of the last path segment ("home"),
     * and keeps ~/.jsh_history + ~/.local/bin working correctly. */
    if (getenv("HOME") == NULL) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            setenv("HOME", cwd, 1);
        } else {
            setenv("HOME", "/", 1);
        }
    }

    setup_path();
    cache_path_commands(); /* build the Tab-completion cache once, up front */
    history_init();

    /* Non-interactive mode: if input is coming from a pipe/redirect (not a real
     * terminal), don't print a prompt — useful for running .jsh scripts later */
    int interactive = isatty(fileno(stdin));
    g_interactive = interactive;

    if (interactive) {
        /* Set up job control. This is what separates jsh's own survival
         * from whatever it runs: without it, jsh and its children share
         * one process group, and a Ctrl+C meant for a runaway child could
         * take jsh down with it. */
        g_shell_pgid = getpid();

        /* jsh must be its own process group leader to hand the terminal
         * to child jobs and take it back later. */
        if (setpgid(g_shell_pgid, g_shell_pgid) < 0 && errno != EPERM) {
            perror("jsh: setpgid failed");
        }

        /* SIGTTOU/SIGTTIN would otherwise stop jsh if it tries to control
         * the terminal (tcsetpgrp) while not in the foreground — shouldn't
         * normally happen here, but ignoring them is standard practice for
         * an interactive shell doing job control. jsh also ignores SIGINT
         * itself: Ctrl+C should only ever affect the foreground child job
         * (see run_pipeline), never kill the shell that's running it. */
        signal(SIGTTOU, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN); /* Ctrl+Z should stop the child job, not jsh */

        tcsetpgrp(STDIN_FILENO, g_shell_pgid);

        /* Visible blinking block cursor so the user always sees the insert
         * point (Termux / xterm DECSCUSR). Many Android TerminalViews honor
         * these; harmless if ignored. */
        fputs("\033[?25h", stdout);   /* ensure cursor visible */
        fputs("\033[1 q", stdout);    /* blinking block */
        fputs("\033[?12h", stdout);   /* start blink (att610) */
        fflush(stdout);
    }

    while (1) {
        char stmt[JSH_MAX_STMT];
        stmt[0] = '\0';
        int first = 1;

        /* Collect a complete statement — may span multiple physical lines
         * when braces are open (if/while/for blocks). */
        while (1) {
            char line[JSH_MAX_LINE];

            if (interactive) {
                if (first) {
                    jobs_notify_done();
                    char prompt[256];
                    build_prompt(prompt, sizeof(prompt));
                    if (read_line(prompt, line, sizeof(line)) == 0) {
                        if (stmt[0] == '\0') goto done; /* EOF on empty */
                        break; /* partial stmt + EOF: try to run what we have */
                    }
                } else {
                    /* Continuation prompt for open blocks */
                    if (read_line("\033[90m>\033[0m ", line, sizeof(line)) == 0) {
                        break;
                    }
                }
            } else {
                if (fgets(line, sizeof(line), stdin) == NULL) {
                    if (stmt[0] == '\0') goto done;
                    break;
                }
                /* strip trailing newline from fgets */
                size_t ll = strlen(line);
                while (ll > 0 && (line[ll - 1] == '\n' || line[ll - 1] == '\r'))
                    line[--ll] = '\0';
            }

            /* Append to statement buffer */
            if (first) {
                snprintf(stmt, sizeof(stmt), "%s", line);
                first = 0;
            } else {
                size_t sl = strlen(stmt);
                if (sl + 1 < sizeof(stmt)) {
                    stmt[sl] = '\n';
                    snprintf(stmt + sl + 1, sizeof(stmt) - sl - 1, "%s", line);
                }
            }

            if (statement_is_complete(stmt)) break;
        }

        /* Record the first physical line (or whole one-liner) in history */
        if (interactive) {
            char *check = stmt;
            while (*check == ' ' || *check == '\t' || *check == '\n') check++;
            if (*check != '\0') {
                /* store a single-line version for Up-arrow usefulness:
                 * keep newlines as spaces so history recall is editable */
                char hist[JSH_MAX_LINE];
                size_t hi = 0;
                for (const char *hp = stmt; *hp && hi + 1 < sizeof(hist); hp++) {
                    char c = (*hp == '\n') ? ' ' : *hp;
                    hist[hi++] = c;
                }
                hist[hi] = '\0';
                history_add(hist);
            }
        }

        int result = dispatch_line(stmt);
        if (result == -1) break;

        jobs_notify_done();
    }

done:
    return 0;
}
