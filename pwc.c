/*
 * pwc — PowerCode CLI
 *
 * Core commands that work without the full extension registry:
 *   link, stage, run, setup
 *
 * install / uninstall / search / update / get / get-a / set — stubs for now.
 *
 * Android note: core binaries must live under nativeLibraryDir as lib*.so
 * (exec from files/ is blocked). TerminalSession symlinks libpwc.so →
 * ~/.local/bin/pwc; jsh builtin `setup` can also place pwc on PATH.
 * `pwc setup` creates multi-call symlinks for libtoybox.so.
 *
 * Build (via build.sh):
 *   NDK clang -O2 -fPIE -pie -o libpwc.so pwc.c
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
        "  setup              symlink toybox commands into ~/.local/bin\n"
        "  link <file>        symlink a file into ~/.local/bin + chmod +x\n"
        "  stage <file> [name]  copy into $PWC_BIN (runtime) + link\n"
        "  run <file> [args]  exec a file (may fail on Android files/)\n"
        "  install <name>     install extension (not yet)\n"
        "  uninstall <name>   remove extension (not yet)\n"
        "  search <name>      search registry (not yet)\n"
        "  update [name]      update extensions (not yet)\n"
        "  get <url>          download file (not yet)\n"
        "  get-a <url>        API request (not yet)\n"
        "  set graphic        choose graphics backend (not yet)\n"
    );
}

/* Resolve ~/.local/bin, creating parents if needed. */
static int ensure_local_bin(char *out, size_t out_size) {
    const char *home = getenv("HOME");
    if (home == NULL) {
        fprintf(stderr, "pwc: $HOME is not set\n");
        return 1;
    }

    char local[PATH_MAX];
    snprintf(local, sizeof(local), "%s/.local", home);
    snprintf(out, out_size, "%s/.local/bin", home);

    if (mkdir(local, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "pwc: could not create %s: %s\n", local, strerror(errno));
        return 1;
    }
    if (mkdir(out, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "pwc: could not create %s: %s\n", out, strerror(errno));
        return 1;
    }
    return 0;
}

/* ── toybox multi-call names ───────────────────────────────────────── */
static const char *TOYBOX_LINKS[] = {
    "ls", "cp", "mv", "rm", "mkdir", "rmdir", "cat", "echo",
    "pwd", "touch", "chmod", "chown", "ln", "stat", "df", "du",
    "ps", "kill", "sleep", "head", "tail", "wc", "grep", "find",
    "xargs", "sort", "uniq", "cut", "tr", "basename", "dirname",
    "realpath", "which", "id", "whoami", "uname", "date", "true",
    "false", "test", "[", "env", "printenv", "clear", "seq",
    "tar", "gzip", "gunzip", "base64", "md5sum", "sha256sum",
    "ifconfig", "netstat", "ping",
    NULL
};

/*
 * Locate libtoybox.so:
 *   1) $PWC_NATIVE_LIB/libtoybox.so  (TerminalSession should export this)
 *   2) same directory as /proc/self/exe (when pwc is libpwc.so)
 */
static int find_libtoybox(char *out, size_t out_size) {
    const char *native = getenv("PWC_NATIVE_LIB");
    if (native && native[0]) {
        snprintf(out, out_size, "%s/libtoybox.so", native);
        if (access(out, R_OK) == 0)
            return 0;
    }

    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, out_size, "%s/libtoybox.so", self);
            if (access(out, R_OK) == 0)
                return 0;
        }
    }

    fprintf(stderr,
        "pwc: setup: libtoybox.so not found\n"
        "  Rebuild APK with prebuilt/toybox-aarch64 → lib/arm64-v8a/libtoybox.so\n"
        "  and export PWC_NATIVE_LIB from TerminalSession.\n");
    return 1;
}

/* `pwc setup` — symlink toybox applets into ~/.local/bin (on $PATH) */
static int cmd_setup(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    char local_bin[PATH_MAX];
    if (ensure_local_bin(local_bin, sizeof(local_bin)) != 0)
        return 1;

    char toybox[PATH_MAX];
    if (find_libtoybox(toybox, sizeof(toybox)) != 0)
        return 1;

    printf("pwc setup: toybox → %s\n", toybox);
    printf("pwc setup: links → %s\n", local_bin);

    int ok = 0, fail = 0;
    for (int i = 0; TOYBOX_LINKS[i] != NULL; i++) {
        char linkpath[PATH_MAX];
        snprintf(linkpath, sizeof(linkpath), "%s/%s", local_bin, TOYBOX_LINKS[i]);
        unlink(linkpath);
        if (symlink(toybox, linkpath) != 0) {
            fprintf(stderr, "  skip %s: %s\n", TOYBOX_LINKS[i], strerror(errno));
            fail++;
        } else {
            ok++;
        }
    }

    {
        char linkpath[PATH_MAX];
        snprintf(linkpath, sizeof(linkpath), "%s/toybox", local_bin);
        unlink(linkpath);
        if (symlink(toybox, linkpath) == 0)
            ok++;
        else
            fail++;
    }

    printf("pwc setup: %d ok, %d failed\n", ok, fail);
    if (ok > 0)
        printf("try: ls   which ls   toybox\n");
    return fail && !ok ? 1 : 0;
}

/* `pwc link <file>` */
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
    if (ensure_local_bin(local_bin, sizeof(local_bin)) != 0)
        return 1;

    const char *base = strrchr(abs_path, '/');
    base = (base != NULL) ? base + 1 : abs_path;

    char link_path[PATH_MAX];
    snprintf(link_path, sizeof(link_path), "%s/%s", local_bin, base);
    unlink(link_path);

    if (symlink(abs_path, link_path) != 0) {
        fprintf(stderr, "pwc: link: could not create symlink: %s\n", strerror(errno));
        return 1;
    }

    if (chmod(abs_path, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH) != 0) {
        fprintf(stderr, "pwc: link: warning: chmod +x %s: %s\n",
                abs_path, strerror(errno));
    }

    printf("linked: %s -> %s\n", link_path, abs_path);
    printf("you can now run '%s' from anywhere\n", base);
    return 0;
}

/* `pwc stage <file> [name]` — copy into $PWC_BIN + symlink ~/.local/bin */
static int cmd_stage(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: pwc stage <file> [name]\n");
        fprintf(stderr, "  Copies into $PWC_BIN (code_cache/bin) and links ~/.local/bin.\n");
        return 1;
    }
    const char *src = argv[2];
    const char *name = (argc >= 4) ? argv[3] : NULL;
    if (name == NULL) {
        const char *base = strrchr(src, '/');
        name = base ? base + 1 : src;
    }

    const char *bin = getenv("PWC_BIN");
    char local_fallback[PATH_MAX];
    if (bin == NULL || bin[0] == '\0') {
        if (ensure_local_bin(local_fallback, sizeof(local_fallback)) != 0)
            return 1;
        bin = local_fallback;
    } else {
        if (mkdir(bin, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "pwc: stage: mkdir %s: %s\n", bin, strerror(errno));
            return 1;
        }
    }

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/%s", bin, name);

    FILE *in = fopen(src, "rb");
    if (!in) {
        fprintf(stderr, "pwc: stage: open %s: %s\n", src, strerror(errno));
        return 1;
    }
    FILE *out = fopen(dest, "wb");
    if (!out) {
        fprintf(stderr, "pwc: stage: write %s: %s\n", dest, strerror(errno));
        fclose(in);
        return 1;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "pwc: stage: write error\n");
            fclose(in);
            fclose(out);
            return 1;
        }
    }
    fclose(in);
    fclose(out);
    chmod(dest, 0755);

    char local[PATH_MAX];
    if (ensure_local_bin(local, sizeof(local)) == 0) {
        char linkpath[PATH_MAX];
        snprintf(linkpath, sizeof(linkpath), "%s/%s", local, name);
        unlink(linkpath);
        if (symlink(dest, linkpath) != 0)
            fprintf(stderr, "pwc: stage: symlink warn: %s\n", strerror(errno));
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
    execvp(argv[2], &argv[2]);
    fprintf(stderr, "pwc: run: %s: %s\n", argv[2], strerror(errno));
    if (errno == EACCES) {
        fprintf(stderr,
            "hint: Android blocked exec from app files/.\n"
            "      Use lib*.so in the APK, or: pwc stage <file>\n");
    }
    return 127;
}

static int cmd_not_implemented(const char *name) {
    fprintf(stderr,
        "pwc: %s: not implemented yet\n"
        "(needs extension registry / .pwck / network)\n",
        name);
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "setup") == 0)
        return cmd_setup(argc, argv);
    if (strcmp(cmd, "link") == 0)
        return cmd_link(argc, argv);
    if (strcmp(cmd, "stage") == 0)
        return cmd_stage(argc, argv);
    if (strcmp(cmd, "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(cmd, "install") == 0 || strcmp(cmd, "uninstall") == 0 ||
        strcmp(cmd, "search") == 0 || strcmp(cmd, "update") == 0 ||
        strcmp(cmd, "get") == 0 || strcmp(cmd, "get-a") == 0 ||
        strcmp(cmd, "set") == 0) {
        return cmd_not_implemented(cmd);
    }

    fprintf(stderr, "pwc: unknown command: %s\n", cmd);
    print_usage();
    return 1;
}
