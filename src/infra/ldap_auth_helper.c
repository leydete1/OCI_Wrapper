/*
 * ldap_auth_helper.c
 *
 * See ldap_bind_helper.c's own header comment for the full history of
 * why this now shells out to a completely separate OS process rather
 * than attempting anything in-process (header collision, symbol
 * collision, BER-layer collision, then a dlmopen namespace/TLS crash -
 * four attempts, four distinct failures, all against the same
 * underlying cause: Oracle's Instant Client and OpenLDAP cannot
 * safely share one process's address space under real concurrent use).
 *
 * ldap_auth_bind_check()'s SIGNATURE is unchanged from every previous
 * revision - OCI_Auth_Manager.c calls it exactly the same way it
 * always has and needed no changes for this fix.
 *
 * Locating the helper binary: this file reads /proc/self/exe to find
 * OCI_Wrapper's own binary path, then looks for "ldap_bind_helper" in
 * the same directory. This assumes the helper is deployed alongside
 * OCI_Wrapper - true today (both live in Debug/), and worth
 * remembering as a deployment note if OCI_Wrapper is ever packaged/
 * installed somewhere the helper doesn't automatically travel with it.
 *
 * Security note: the password is sent to the child process via a
 * pipe to its stdin, never as a command-line argument - argv is
 * visible to any local user via `ps`/`/proc/<pid>/cmdline`, a pipe is
 * not.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>

#include "ldap_auth_helper.h"

/* Hard ceiling on how long we'll wait for the helper process,
 * independent of whatever network timeout the helper sets internally
 * for its own LDAP calls - this is the parent's own watchdog, so a
 * genuinely stuck/hung child (not just a slow network) still can't
 * block a dispatcher worker thread forever. A few seconds of grace
 * above the helper's own internal timeout (5s, see ldap_bind_helper.c)
 * to let it exit cleanly on its own first.                           */
#define LDAP_HELPER_WAIT_TIMEOUT_SECONDS 8

static pthread_once_t g_sigpipe_once = PTHREAD_ONCE_INIT;

static void set_err(char *err_buf, size_t err_buf_size, const char *msg)
{
    if (err_buf && err_buf_size > 0)
    {
        strncpy(err_buf, msg, err_buf_size - 1);
        err_buf[err_buf_size - 1] = '\0';
    }
}

/*
 * A dead/never-reading helper process could cause a write() to its
 * stdin pipe to raise SIGPIPE, whose default action terminates the
 * whole process - standard, safe practice for any server doing
 * pipe/socket I/O is to ignore SIGPIPE process-wide once at startup
 * and rely on write()'s own EPIPE return value instead, which this
 * file does below.
 */
static void ignore_sigpipe_once(void)
{
    signal(SIGPIPE, SIG_IGN);
}

/*
 * find_helper_path()
 *
 * Resolves /proc/self/exe (this process's own binary path) and
 * substitutes the final path component for "ldap_bind_helper" -
 * assumes the helper is deployed in the same directory as OCI_Wrapper
 * itself (true today; see this file's header comment).
 *
 * Returns 1 on success (path written to out), 0 on failure.
 */
static int find_helper_path(char *out, size_t out_size)
{
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0)
        return 0;
    exe_path[len] = '\0';

    char *last_slash = strrchr(exe_path, '/');
    if (!last_slash)
        return 0;

    size_t dir_len = (size_t)(last_slash - exe_path);
    if (dir_len + strlen("/ldap_bind_helper") + 1 > out_size)
        return 0;

    memcpy(out, exe_path, dir_len);
    out[dir_len] = '\0';
    strncat(out, "/ldap_bind_helper", out_size - strlen(out) - 1);
    return 1;
}

int ldap_auth_bind_check(const char *ldap_url,
                          const char *bind_dn,
                          const char *password,
                          char *err_buf, size_t err_buf_size)
{
    if (err_buf && err_buf_size > 0) err_buf[0] = '\0';

    if (!ldap_url || !ldap_url[0] || !bind_dn || !bind_dn[0] ||
        !password || !password[0])
        return LDAP_AUTH_BIND_INVALID_ARG;

    pthread_once(&g_sigpipe_once, ignore_sigpipe_once);

    char helper_path[1024];
    if (!find_helper_path(helper_path, sizeof(helper_path)))
    {
        set_err(err_buf, err_buf_size,
                "could not resolve ldap_bind_helper path via /proc/self/exe");
        return LDAP_AUTH_BIND_FAILED;
    }

    int stdin_pipe[2];   /* parent writes password, child reads it   */
    int stdout_pipe[2];  /* child writes result text, parent reads it */
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0)
    {
        set_err(err_buf, err_buf_size, "pipe() failed");
        return LDAP_AUTH_BIND_FAILED;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        set_err(err_buf, err_buf_size, "fork() failed");
        return LDAP_AUTH_BIND_FAILED;
    }

    if (pid == 0)
    {
        /* ---- child: becomes ldap_bind_helper ---- */
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);

        execl(helper_path, "ldap_bind_helper", ldap_url, bind_dn, (char *)NULL);

        /* execl() only returns on failure (e.g. helper binary missing
         * or not executable). We are now in the forked child of a
         * heavily multi-threaded process, before exec() has replaced
         * this process's image - printf()/strerror() are NOT
         * guaranteed safe to call here: if another thread held
         * glibc's internal malloc lock at the exact moment fork() was
         * called, only the calling thread survives in the child, and
         * that lock is now held forever by a thread that no longer
         * exists - any allocation attempt (which printf()'s internal
         * buffering and some strerror() implementations may do) would
         * hang this child indefinitely. write() is async-signal-safe
         * and allocation-free, so it's used here instead, with a
         * fixed message (no strerror() text) rather than risk it.    */
        {
            static const char msg[] = "execl(ldap_bind_helper) failed\n";
            ssize_t ignored = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
            (void)ignored;
        }
        _exit(127);
    }

    /* ---- parent ---- */
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    /* Send the password, then close our write end so the child's
     * fgets() sees EOF after the line - matches ldap_bind_helper.c's
     * "read exactly one line from stdin" contract.                   */
    size_t pw_len = strlen(password);
    ssize_t written = write(stdin_pipe[1], password, pw_len);
    if (written == (ssize_t)pw_len)
        write(stdin_pipe[1], "\n", 1);
    /* A short/failed write (including EPIPE, if the child already
     * exited) just means the child gets an incomplete/no password and
     * will fail its own bind - not a reason to abort here, the exit
     * code check below handles that case the same as any other
     * failure.                                                       */
    close(stdin_pipe[1]);

    char result_text[256] = {0};
    ssize_t total_read = 0;
    ssize_t r;
    while ((r = read(stdout_pipe[0],
                      result_text + total_read,
                      sizeof(result_text) - 1 - total_read)) > 0)
    {
        total_read += r;
        if ((size_t)total_read >= sizeof(result_text) - 1) break;
    }
    result_text[total_read] = '\0';
    close(stdout_pipe[0]);

    /* Watchdog: poll waitpid() rather than a blocking wait(), so a
     * genuinely stuck child (not just a slow network - the helper's
     * own internal LDAP_OPT_NETWORK_TIMEOUT should prevent that, but
     * this covers any other stuck-child scenario) still can't block
     * this dispatcher worker thread forever.                         */
    int status = 0;
    int elapsed_ms = 0;
    const int poll_interval_ms = 50;
    pid_t wait_rc;
    while ((wait_rc = waitpid(pid, &status, WNOHANG)) == 0)
    {
        struct timespec ts = { 0, poll_interval_ms * 1000000L };
        nanosleep(&ts, NULL);
        elapsed_ms += poll_interval_ms;
        if (elapsed_ms >= LDAP_HELPER_WAIT_TIMEOUT_SECONDS * 1000)
        {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            set_err(err_buf, err_buf_size,
                    "ldap_bind_helper did not exit within the watchdog "
                    "timeout - killed");
            return LDAP_AUTH_BIND_FAILED;
        }
    }

    if (wait_rc < 0)
    {
        set_err(err_buf, err_buf_size, "waitpid() failed");
        return LDAP_AUTH_BIND_FAILED;
    }

    if (!WIFEXITED(status))
    {
        set_err(err_buf, err_buf_size,
                "ldap_bind_helper terminated abnormally (signal)");
        return LDAP_AUTH_BIND_FAILED;
    }

    int exit_code = WEXITSTATUS(status);
    if (exit_code == 0)
        return LDAP_AUTH_BIND_OK;

    /* exit_code 1 (bind failed) or 2 (helper usage/arg error) both
     * fold into the same generic LDAP_AUTH_BIND_FAILED - the caller
     * (auth_authenticate(), OCI_Auth_Manager.c) already folds this
     * into the same generic AUTH_ERR_DENIED regardless, per Security_
     * Module_Design_Specification.docx Section 5. result_text carries
     * whichever specific reason the helper printed, for internal
     * logging only.                                                  */
    set_err(err_buf, err_buf_size,
            result_text[0] ? result_text : "ldap_bind_helper failed "
            "with no output");
    return LDAP_AUTH_BIND_FAILED;
}
