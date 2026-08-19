/*
 * pty_jni.cpp — PowerCode PTY bridge
 *
 * Opens a PTY, forks, execs jsh (or a shell path passed from Java),
 * and exposes read/write/close to Java via JNI.
 *
 * Build (via build.sh):
 *   aarch64-linux-android26-clang++ -shared -fPIC \
 *     -o libpty_jni.so pty_jni.cpp -llog
 */

#include <jni.h>
#include <android/log.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#define LOG_TAG "pwc-pty"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ── session state (one foreground session for MVP) ─────────────── */

static int   g_master_fd = -1;
static pid_t g_child_pid = -1;

static void session_reset(void) {
    if (g_master_fd >= 0) {
        close(g_master_fd);
        g_master_fd = -1;
    }
    g_child_pid = -1;
}

/* openpty is not always in bionic with the same symbol — implement with
 * posix_openpt / grantpt / unlockpt which Android NDK provides. */
static int open_pty(int *master, int *slave) {
    int m = posix_openpt(O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (m < 0) {
        LOGE("posix_openpt: %s", strerror(errno));
        return -1;
    }
    if (grantpt(m) != 0 || unlockpt(m) != 0) {
        LOGE("grantpt/unlockpt: %s", strerror(errno));
        close(m);
        return -1;
    }
    char *name = ptsname(m);
    if (name == NULL) {
        LOGE("ptsname failed");
        close(m);
        return -1;
    }
    int s = open(name, O_RDWR | O_NOCTTY);
    if (s < 0) {
        LOGE("open slave %s: %s", name, strerror(errno));
        close(m);
        return -1;
    }
    *master = m;
    *slave = s;
    return 0;
}

/*
 * Java:
 *   native int nativeStart(String shellPath, String cwd, String[] envp);
 * Returns 0 on success, -1 on failure.
 */
extern "C" JNIEXPORT jint JNICALL
Java_com_pwc_app_terminal_TerminalSession_nativeStart(
        JNIEnv *env, jobject /*thiz*/,
        jstring jShellPath, jstring jCwd, jobjectArray jEnvp) {

    if (g_master_fd >= 0) {
        /* already running — close previous */
        if (g_child_pid > 0) {
            kill(g_child_pid, SIGHUP);
            waitpid(g_child_pid, NULL, WNOHANG);
        }
        session_reset();
    }

    const char *shell_path = env->GetStringUTFChars(jShellPath, nullptr);
    const char *cwd = jCwd ? env->GetStringUTFChars(jCwd, nullptr) : nullptr;

    /* Copy paths before fork — child must not call JNI */
    char shell_buf[512];
    char cwd_buf[512];
    snprintf(shell_buf, sizeof(shell_buf), "%s", shell_path ? shell_path : "/system/bin/sh");
    snprintf(cwd_buf, sizeof(cwd_buf), "%s", cwd ? cwd : "");

    /* Build env array before fork if provided */
    char **child_env = nullptr;
    int env_owned = 0;
    if (jEnvp != nullptr) {
        jsize n = env->GetArrayLength(jEnvp);
        child_env = (char **)malloc((size_t)(n + 1) * sizeof(char *));
        if (child_env) {
            env_owned = 1;
            for (jsize i = 0; i < n; i++) {
                auto js = (jstring)env->GetObjectArrayElement(jEnvp, i);
                const char *s = env->GetStringUTFChars(js, nullptr);
                child_env[i] = strdup(s ? s : "");
                env->ReleaseStringUTFChars(js, s);
                env->DeleteLocalRef(js);
            }
            child_env[n] = nullptr;
        }
    }

    env->ReleaseStringUTFChars(jShellPath, shell_path);
    if (cwd) env->ReleaseStringUTFChars(jCwd, cwd);

    int master = -1, slave = -1;
    if (open_pty(&master, &slave) != 0) {
        if (env_owned && child_env) {
            for (char **p = child_env; *p; p++) free(*p);
            free(child_env);
        }
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("fork: %s", strerror(errno));
        close(master);
        close(slave);
        if (env_owned && child_env) {
            for (char **p = child_env; *p; p++) free(*p);
            free(child_env);
        }
        return -1;
    }

    if (pid == 0) {
        /* ── child: session leader + controlling tty ── */
        close(master);

        setsid();
        ioctl(slave, TIOCSCTTY, 0);

        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) close(slave);

        if (cwd_buf[0] != '\0') {
            chdir(cwd_buf);
        }

        if (child_env) {
            execle(shell_buf, shell_buf, (char *)nullptr, child_env);
        } else {
            execl(shell_buf, shell_buf, (char *)nullptr);
        }
        const char *err = strerror(errno);
        write(STDERR_FILENO, "exec failed: ", 13);
        write(STDERR_FILENO, err, strlen(err));
        write(STDERR_FILENO, "\n", 1);
        _exit(127);
    }

    /* ── parent ── */
    close(slave);
    if (env_owned && child_env) {
        for (char **p = child_env; *p; p++) free(*p);
        free(child_env);
    }

    g_master_fd = master;
    g_child_pid = pid;

    int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) fcntl(master, F_SETFL, flags | O_NONBLOCK);

    LOGI("PTY started pid=%d shell=%s", (int)pid, shell_buf);
    return 0;
}

/*
 * native int nativeRead(byte[] buf);
 * Returns bytes read, 0 on no data (EAGAIN), -1 on EOF/error.
 */
extern "C" JNIEXPORT jint JNICALL
Java_com_pwc_app_terminal_TerminalSession_nativeRead(
        JNIEnv *env, jobject /*thiz*/, jbyteArray jBuf) {

    if (g_master_fd < 0) return -1;

    jsize len = env->GetArrayLength(jBuf);
    if (len <= 0) return 0;

    jbyte *buf = env->GetByteArrayElements(jBuf, nullptr);
    ssize_t n = read(g_master_fd, buf, (size_t)len);
    int saved = errno;
    env->ReleaseByteArrayElements(jBuf, buf, 0);

    if (n > 0) return (jint)n;
    if (n == 0) return -1; /* EOF */
    if (saved == EAGAIN || saved == EWOULDBLOCK) return 0;
    LOGE("read: %s", strerror(saved));
    return -1;
}

/*
 * native int nativeWrite(byte[] buf, int off, int len);
 */
extern "C" JNIEXPORT jint JNICALL
Java_com_pwc_app_terminal_TerminalSession_nativeWrite(
        JNIEnv *env, jobject /*thiz*/,
        jbyteArray jBuf, jint off, jint len) {

    if (g_master_fd < 0 || len <= 0) return -1;

    jbyte *buf = env->GetByteArrayElements(jBuf, nullptr);
    ssize_t n = write(g_master_fd, buf + off, (size_t)len);
    int saved = errno;
    env->ReleaseByteArrayElements(jBuf, buf, JNI_ABORT);

    if (n < 0) {
        LOGE("write: %s", strerror(saved));
        return -1;
    }
    return (jint)n;
}

/*
 * native void nativeResize(int rows, int cols);
 */
extern "C" JNIEXPORT void JNICALL
Java_com_pwc_app_terminal_TerminalSession_nativeResize(
        JNIEnv * /*env*/, jobject /*thiz*/, jint rows, jint cols) {

    if (g_master_fd < 0 || rows <= 0 || cols <= 0) return;

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    if (ioctl(g_master_fd, TIOCSWINSZ, &ws) != 0) {
        LOGE("TIOCSWINSZ: %s", strerror(errno));
    }
}

/*
 * native void nativeStop();
 */
extern "C" JNIEXPORT void JNICALL
Java_com_pwc_app_terminal_TerminalSession_nativeStop(
        JNIEnv * /*env*/, jobject /*thiz*/) {

    if (g_child_pid > 0) {
        kill(g_child_pid, SIGHUP);
        int status = 0;
        waitpid(g_child_pid, &status, 0);
        LOGI("child exited status=%d", status);
    }
    session_reset();
}

/*
 * native int nativeIsRunning();
 * 1 if child alive, 0 otherwise.
 */
extern "C" JNIEXPORT jint JNICALL
Java_com_pwc_app_terminal_TerminalSession_nativeIsRunning(
        JNIEnv * /*env*/, jobject /*thiz*/) {

    if (g_child_pid <= 0) return 0;
    int status = 0;
    pid_t r = waitpid(g_child_pid, &status, WNOHANG);
    if (r == 0) return 1;          /* still running */
    if (r == g_child_pid) {
        g_child_pid = -1;
        return 0;
    }
    return 0;
}
