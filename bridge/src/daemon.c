/*
 * daemon.c -- 运行目录、pid 文件锁与标准 daemonize。
 *
 * 顺序在 supervisor.c 里是固定的：先解析绝对路径，再 daemonize，再拿 pid 锁，最后开
 * 日志。锁必须在 fork 之后拿（fcntl 记录锁不随 fork 继承），而日志必须在拿到锁之后开
 * —— 否则第二个实例会往第一个实例的日志里写一行"启动失败"，那份日志就不再是单写者。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* realpath() is POSIX but glibc only declares it under XSI; daemonize needs it
 * because chdir("/") happens after every path is resolved. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <kitchenos/daemon.h>

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if (cap == 0) {
        return;
    }
    if (src != NULL) {
        while (i + 1 < cap && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static int set_error(kos_error *err, int code, int sys_errno, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int set_error(kos_error *err, int code, int sys_errno, const char *fmt, ...)
{
    va_list ap;

    if (err != NULL) {
        err->code = code;
        err->sys_errno = sys_errno;
        va_start(ap, fmt);
        vsnprintf(err->context, sizeof err->context, fmt, ap);
        va_end(ap);
    }
    return code;
}

/* mkdir -p，忽略 EEXIST；任何一级失败都带上完整路径返回。 */
static int make_dirs(const char *path, mode_t mode, kos_error *err)
{
    char work[KOS_DAEMON_PATH_MAX];
    size_t i;

    if (path == NULL || strlen(path) >= sizeof work) {
        return set_error(err, KOS_ERR_INVAL, 0, "路径过长: %s", path ? path : "(null)");
    }
    copy_str(work, sizeof work, path);

    for (i = 1; work[i] != '\0'; i++) {
        if (work[i] != '/') {
            continue;
        }
        work[i] = '\0';
        if (mkdir(work, mode) != 0 && errno != EEXIST) {
            return set_error(err, KOS_ERR_IO, errno, "mkdir('%s')", work);
        }
        work[i] = '/';
    }
    if (mkdir(work, mode) != 0 && errno != EEXIST) {
        return set_error(err, KOS_ERR_IO, errno, "mkdir('%s')", work);
    }

    {
        struct stat st;

        if (stat(work, &st) != 0) {
            return set_error(err, KOS_ERR_IO, errno, "stat('%s')", work);
        }
        if (!S_ISDIR(st.st_mode)) {
            return set_error(err, KOS_ERR_DIR, 0, "不是目录: %s", work);
        }
    }
    return KOS_OK;
}

int kos_runtime_dir(const char *override, char *buf, size_t len, kos_error *err)
{
    char raw[KOS_DAEMON_PATH_MAX];
    char resolved[KOS_DAEMON_PATH_MAX];
    char logs[KOS_DAEMON_PATH_MAX];
    const char *candidate = NULL;
    int rc;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (buf == NULL || len == 0) {
        return KOS_ERR_INVAL;
    }

    if (override != NULL && override[0] != '\0') {
        candidate = override;
    } else if (getenv(KOS_RUNTIME_ENV) != NULL && getenv(KOS_RUNTIME_ENV)[0] != '\0') {
        candidate = getenv(KOS_RUNTIME_ENV);
    }
    if (candidate != NULL) {
        if (snprintf(raw, sizeof raw, "%s", candidate) >= (int)sizeof raw) {
            return set_error(err, KOS_ERR_INVAL, 0, "运行目录过长: %s", candidate);
        }
    } else {
        const char *xdg = getenv("XDG_RUNTIME_DIR");

        if (xdg != NULL && xdg[0] == '/') {
            if (snprintf(raw, sizeof raw, "%s/kitchenos", xdg) >= (int)sizeof raw) {
                return set_error(err, KOS_ERR_INVAL, 0, "XDG_RUNTIME_DIR 过长: %s", xdg);
            }
        } else if (snprintf(raw, sizeof raw, "/tmp/kitchenos-%lu",
                            (unsigned long)getuid()) >= (int)sizeof raw) {
            return set_error(err, KOS_ERR_INVAL, 0, "默认运行目录过长: %s", raw);
        }
    }

    rc = make_dirs(raw, 0700, err);
    if (rc != KOS_OK) {
        return rc;
    }
    if (snprintf(logs, sizeof logs, "%s/logs", raw) >= (int)sizeof logs) {
        return set_error(err, KOS_ERR_INVAL, 0, "日志目录过长: %s", raw);
    }
    rc = make_dirs(logs, 0755, err);
    if (rc != KOS_OK) {
        return rc;
    }
    if (snprintf(logs, sizeof logs, "%s/runs", raw) >= (int)sizeof logs) {
        return set_error(err, KOS_ERR_INVAL, 0, "runs 目录过长: %s", raw);
    }
    rc = make_dirs(logs, 0755, err);
    if (rc != KOS_OK) {
        return rc;
    }

    /* chdir("/") 之后相对路径就指错地方了，所以在这里一次性换成绝对路径。 */
    if (realpath(raw, resolved) == NULL) {
        return set_error(err, KOS_ERR_IO, errno, "realpath('%s')", raw);
    }
    if (strlen(resolved) + 1 > len) {
        return set_error(err, KOS_ERR_INVAL, 0, "运行目录缓冲区太小: %s", resolved);
    }
    copy_str(buf, len, resolved);
    return KOS_OK;
}

int kos_runtime_log_path(const char *runtime_dir, const char *name, char *buf, size_t len,
                         kos_error *err)
{
    if (err != NULL) {
        kos_error_clear(err);
    }
    if (runtime_dir == NULL || name == NULL || buf == NULL || len == 0 || name[0] == '\0' ||
        strchr(name, '/') != NULL) {
        return set_error(err, KOS_ERR_INVAL, 0, "日志文件名不合法: %s",
                         name ? name : "(null)");
    }
    if (snprintf(buf, len, "%s/logs/%s", runtime_dir, name) >= (int)len) {
        return set_error(err, KOS_ERR_INVAL, 0, "日志路径过长: %s", name);
    }
    return KOS_OK;
}

int kos_pidfile_acquire(const char *path, kos_pidfile *pf, kos_error *err)
{
    struct flock lock;
    char text[64];
    int fd;
    int len;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (path == NULL || pf == NULL) {
        return KOS_ERR_INVAL;
    }
    memset(pf, 0, sizeof *pf);
    pf->fd = -1;
    pf->pid = getpid();
    copy_str(pf->path, sizeof pf->path, path);

    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return set_error(err, KOS_ERR_IO, errno, "open('%s')", path);
    }

    memset(&lock, 0, sizeof lock);
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (fcntl(fd, F_SETLK, &lock) != 0) {
        int saved = errno;

        close(fd);
        if (saved == EACCES || saved == EAGAIN) {
            return set_error(err, KOS_ERR_EXISTS, saved,
                             "pid 文件已被另一个实例加锁: %s", path);
        }
        return set_error(err, KOS_ERR_IO, saved, "fcntl(F_SETLK, '%s')", path);
    }
    pf->fd = fd;
    pf->locked = 1;

    len = snprintf(text, sizeof text, "%ld\n", (long)pf->pid);
    if (ftruncate(fd, 0) != 0 || (len > 0 && write(fd, text, (size_t)len) != len)) {
        int saved = errno;

        kos_pidfile_release(pf, 1);
        return set_error(err, KOS_ERR_IO, saved, "写 pid 文件失败: %s", path);
    }
    /* 内容不是锁的依据，但崩溃后的人要靠它知道上一个持锁者是谁。fsync 失败不致命。 */
    (void)fsync(fd);
    return KOS_OK;
}

void kos_pidfile_release(kos_pidfile *pf, int remove_file)
{
    if (pf == NULL || pf->fd < 0) {
        return;
    }
    if (remove_file) {
        (void)unlink(pf->path);
    }
    close(pf->fd);
    pf->fd = -1;
    pf->locked = 0;
}

static void daemon_fail(int ready_fd, kos_error *err, int sys_errno, const char *what)
{
    char code = 1;

    if (err != NULL) {
        err->code = KOS_ERR_IO;
        err->sys_errno = sys_errno;
        snprintf(err->context, sizeof err->context, "%s", what);
    }
    (void)write(ready_fd, &code, 1);
    _exit(1);
}

int kos_daemonize(kos_daemon *d, kos_error *err)
{
    int fds[2];
    pid_t pid;
    int devnull;
    long max_fd;
    long fd;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (d == NULL) {
        return KOS_ERR_INVAL;
    }
    d->ready_fd = -1;

    if (pipe(fds) != 0) {
        return set_error(err, KOS_ERR_IO, errno, "pipe(%s)", "daemon-ready");
    }
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    pid = fork();
    if (pid < 0) {
        int saved = errno;

        close(fds[0]);
        close(fds[1]);
        return set_error(err, KOS_ERR_IO, saved, "fork(%s)", "daemon");
    }
    if (pid > 0) {
        char code = 1;
        ssize_t n;

        close(fds[1]);
        do {
            n = read(fds[0], &code, 1);
        } while (n < 0 && errno == EINTR);
        close(fds[0]);
        /* 子进程在报告前就死了 → n==0；两种情况都不能报告成功。 */
        _exit(n == 1 ? (unsigned char)code : 1);
    }

    close(fds[0]);
    if (setsid() < 0) {
        daemon_fail(fds[1], err, errno, "setsid");
    }
    umask(022);
    if (chdir("/") != 0) {
        daemon_fail(fds[1], err, errno, "chdir(\"/\")");
    }

    devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) {
        daemon_fail(fds[1], err, errno, "open(\"/dev/null\")");
    }
    if (dup2(devnull, STDIN_FILENO) < 0 || dup2(devnull, STDOUT_FILENO) < 0 ||
        dup2(devnull, STDERR_FILENO) < 0) {
        daemon_fail(fds[1], err, errno, "dup2(/dev/null)");
    }
    if (devnull > STDERR_FILENO) {
        close(devnull);
    }

    max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) {
        max_fd = 65536;
    }
    for (fd = 3; fd < max_fd; fd++) {
        if (fd != fds[1]) {
            close((int)fd);
        }
    }

    d->ready_fd = fds[1];
    return KOS_OK;
}

void kos_daemon_ready(kos_daemon *d, int code)
{
    char byte = (char)(code & 0xff);

    if (d == NULL || d->ready_fd < 0) {
        return;
    }
    (void)write(d->ready_fd, &byte, 1);
    close(d->ready_fd);
    d->ready_fd = -1;
}
