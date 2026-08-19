/*
 * pwc — PowerCode's own CLI tool
 *
 * This is a standalone program (like `ls` or `cat`) that jsh finds via
 * $PATH and runs like any other command — no special-casing inside jsh.
 *
 * Phase 1 status:
 *   - `pwc link <file>`  : fully working (symlink into ~/.local/bin + chmod +x)
 *   - everything else    : dispatch structure is in place, but prints
 *                          "not implemented yet" because it depends on
 *                          pieces that don't exist yet (.pwck format,
 *                          the extension registry, network layer)
 *
 * Build:
 *   gcc -o pwc pwc.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

static void print_usage(void) {
    fprintf(stderr,
        "usage: pwc <command> [args]\n"
        "\n"
        "commands:\n"
        "  install <name>     install a program/language/backend\n"
        "  uninstall <name>   remove it\n"
        "  search <name>      search the extension registry\n"
        "  update [name]      update everything, or one extension\n"
        "  get <url>          download a file (like wget)\n"
        "  get-a <url>        make an API call (like curl)\n"
        "  link <file>        symlink a file into ~/.local/bin and chmod +x\n"
        "  set graphic        choose the graphics backend\n"
    );
}

/* Resolve ~/.local/bin, creating it (and its parents) if missing.
 * Returns 0 on success, writes the path into `out`. */
static int ensure_local_bin(char *out, size_t out_size) {
    const char *home = getenv("HOME");
    if (home == NULL) {
        fprintf(stderr, "pwc: link: $HOME is not set\n");
        return 1;
    }

    char local[PATH_MAX];
    snprintf(local, sizeof(local), "%s/.local", home);
    snprintf(out, out_size, "%s/.local/bin", home);

    /* mkdir -p equivalent for just these two levels — good enough since
     * this is the only path we ever need to ensure exists here */
    if (mkdir(local, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "pwc: link: could not create %s: %s\n", local, strerror(errno));
        return 1;
    }
    if (mkdir(out, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "pwc: link: could not create %s: %s\n", out, strerror(errno));
        return 1;
    }

    return 0;
}

/* `pwc link <file>` — symlink the file into ~/.local/bin (which is on $PATH)
 * and mark it executable, so it can be run by name from anywhere without `./` */
static int cmd_link(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: pwc link <file>\n");
        return 1;
    }

    const char *file = argv[2];

    char abs_path[PATH_MAX];
    if (realpath(file, abs_path) == NULL) {
        fprintf(stderr, "pwc: link: %s: %s\n", file, strerror(errno));
        return 1;
    }

    struct stat st;
    if (stat(abs_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "pwc: link: %s is not a regular file\n", abs_path);
        return 1;
    }

    char local_bin[PATH_MAX];
    if (ensure_local_bin(local_bin, sizeof(local_bin)) != 0) {
        return 1;
    }

    /* Use the file's own name as the linked command name */
    const char *base = strrchr(abs_path, '/');
    base = (base != NULL) ? base + 1 : abs_path;

    char link_path[PATH_MAX];
    snprintf(link_path, sizeof(link_path), "%s/%s", local_bin, base);

    /* Remove any existing link/file at the destination first, so re-running
     * `pwc link` on an updated file just works instead of failing on EEXIST */
    unlink(link_path);

    if (symlink(abs_path, link_path) != 0) {
        fprintf(stderr, "pwc: link: could not create symlink: %s\n", strerror(errno));
        return 1;
    }

    if (chmod(abs_path, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH) != 0) {
        fprintf(stderr, "pwc: link: warning: could not chmod +x %s: %s\n",
                abs_path, strerror(errno));
        /* not fatal — the symlink was still created */
    }

    printf("linked: %s -> %s\n", link_path, abs_path);
    printf("you can now run '%s' from anywhere\n", base);
    return 0;
}

/* Placeholder for commands that depend on pieces not built yet
 * (.pwck format, the extension registry, the network layer) */
static int cmd_not_implemented(const char *name) {
    fprintf(stderr,
        "pwc: %s: not implemented yet\n"
        "(this depends on the extension system, which isn't built yet)\n",
        name);
    return 1;
}


/* stage: copy binary into $PWC_RUNTIME_BIN or $HOME/../code hint — actually
 * the Java side stages into codeCache. From CLI we copy into
 * $HOME/.local/bin and also $PWC_BIN if set (TerminalSession exports it). */
static int cmd_stage(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: pwc stage <file> [name]\n");
        fprintf(stderr, "  Copies file into the runtime bin dir and chmod +x.\n");
        fprintf(stderr, "  Set by the app: PWC_BIN=.../code_cache/bin\n");
        return 1;
    }
    const char *src = argv[2];
    const char *name = (argc >= 4) ? argv[3] : NULL;
    if (name == NULL) {
        const char *base = strrchr(src, '/');
        name = base ? base + 1 : src;
    }

    const char *bin = getenv("PWC_BIN");
    if (bin == NULL || bin[0] == '\0') {
        /* fallback: ~/.local/bin only (may not exec on all devices) */
        char local[PATH_MAX];
        if (ensure_local_bin(local, sizeof(local)) != 0) return 1;
        bin = local;
    }

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/%s", bin, name);

    FILE *in = fopen(src, "rb");
    if (!in) {
        fprintf(stderr, "pwc: stage: cannot open %s: %s\n", src, strerror(errno));
        return 1;
    }
    FILE *out = fopen(dest, "wb");
    if (!out) {
        fprintf(stderr, "pwc: stage: cannot write %s: %s\n", dest, strerror(errno));
        fclose(in);
        return 1;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "pwc: stage: write error\n");
            fclose(in); fclose(out);
            return 1;
        }
    }
    fclose(in);
    fclose(out);
    chmod(dest, 0755);

    /* also link into ~/.local/bin */
    char local[PATH_MAX];
    if (ensure_local_bin(local, sizeof(local)) == 0) {
        char linkpath[PATH_MAX];
        snprintf(linkpath, sizeof(linkpath), "%s/%s", local, name);
        unlink(linkpath);
        if (symlink(dest, linkpath) != 0) {
            /* copy if symlink fails */
            fprintf(stderr, "pwc: stage: symlink warn: %s\n", strerror(errno));
        }
    }

    printf("staged: %s\n", dest);
    printf("try: %s\n", name);
    return 0;
}

static int cmd_run(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: pwc run <file> [args...]\n");
        return 1;
    }
    /* execvp replaces this process — jsh sees exit status of the program */
    execvp(argv[2], &argv[2]);
    fprintf(stderr, "pwc: run: %s: %s\n", argv[2], strerror(errno));
    if (errno == EACCES) {
        fprintf(stderr,
            "hint: Android blocked exec. Use: pwc stage <file>  then run from PATH\n"
            "      or ship core tools as lib*.so in the APK.\n");
    }
    return 127;
}


int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "stage") == 0) {
        return cmd_stage(argc, argv);
    }
    if (strcmp(cmd, "run") == 0) {
        return cmd_run(argc, argv);
    }
    if (strcmp(cmd, "link") == 0) {
        return cmd_link(argc, argv);
    }

    if (strcmp(cmd, "install") == 0 || strcmp(cmd, "uninstall") == 0 ||
        strcmp(cmd, "search")  == 0 || strcmp(cmd, "update")    == 0 ||
        strcmp(cmd, "get")     == 0 || strcmp(cmd, "get-a")     == 0 ||
        strcmp(cmd, "set")     == 0) {
        return cmd_not_implemented(cmd);
    }

    fprintf(stderr, "pwc: unknown command: %s\n", cmd);
    print_usage();
    return 1;
}
