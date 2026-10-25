/*
 * daemon.h -- 运行目录、pid 文件锁与标准 daemonize（模块契约）。
 *
 * Supervisor 是仓库里唯一允许 daemonize 的进程（bridge.c 的注释写明它自己不管进程
 * 守护）。这里把三件容易写错、又被验收直接观察的事集中到一个模块：
 *
 *   1. 运行目录必须落在 Linux 原生文件系统上。仓库可能挂在 9p（WSL2 的 /mnt/d），
 *      而 pid 文件的 fcntl 锁在 9p 上没有可靠语义；默认值因此是 tmpfs 上的绝对路径。
 *   2. 单实例靠 fcntl(F_SETLK) 写锁，不靠"pid 文件存在"——崩溃残留的 pid 文件必须
 *      能被下一个实例接管，反过来两个活实例绝不能同时拿到锁。
 *   3. daemonize 的父进程要等子进程"已经拿到锁、打开日志"后再退出，否则启动失败
 *      （例如第二个实例）会以一个虚假的 0 退出码报告成功。
 */
#ifndef KITCHENOS_DAEMON_H
#define KITCHENOS_DAEMON_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <sys/types.h>

/* kos_error 的定义在 shm.h；另起一个错误结构只会让调用方多一层翻译。 */
#include <kitchenos/shm.h>

/* 运行目录长度：系统临时目录 + 项目名远小于 512，留足空间又便于栈上放。 */
#define KOS_DAEMON_PATH_MAX 512

#define KOS_PIDFILE_NAME "kitchend.pid"
#define KOS_RUNTIME_ENV "KITCHENOS_RUNTIME_DIR"

typedef struct {
    int fd;                          /* 持锁的 fd；-1 = 未加锁 */
    int locked;                      /* 1 = F_WRLCK 已生效 */
    pid_t pid;                       /* 写入文件的 pid */
    char path[KOS_DAEMON_PATH_MAX];  /* 绝对路径 */
} kos_pidfile;

/*
 * 解析并创建运行目录（含 logs/ 与 runs/ 两个子目录），把绝对路径写进 buf。
 * 优先级：override（非空）> $KITCHENOS_RUNTIME_DIR > $XDG_RUNTIME_DIR/kitchenos >
 * /tmp/kitchenos-<uid>。任何一级目录建不出来都返回 KOS_ERR_IO，err 里带路径。
 */
int kos_runtime_dir(const char *override, char *buf, size_t len, kos_error *err);

/*
 * 多实例运行目录：instance < 0 或给了明确的 override 时与 kos_runtime_dir 相同；
 * 否则在默认根目录下再建 `instance-<k>/`（logs/ 与 runs/ 各一份），两台 Supervisor
 * 缺省就不会争同一个 pid 文件与日志。Supervisor 的 `--instance k` 用它。
 */
int kos_runtime_dir_instance(const char *override, int instance, char *buf, size_t len,
                             kos_error *err);

/* runtime/logs/<name>；名字不能含 '/'，否则返回 KOS_ERR_INVAL。 */
int kos_runtime_log_path(const char *runtime_dir, const char *name, char *buf, size_t len,
                         kos_error *err);

/*
 * 打开 path 并加 fcntl 写锁，把本进程 pid 写进去。
 * 锁已被别人持有时返回 KOS_ERR_EXISTS（err.context 说明是哪个文件）；其他失败返回
 * KOS_ERR_IO。成功时 pf->fd 保持打开，锁在 close 前一直有效——fcntl 记录锁不是
 * 继承的，所以 fork 之后才调用。
 */
int kos_pidfile_acquire(const char *path, kos_pidfile *pf, kos_error *err);

/* remove_file != 0 时先 unlink 再 close；重复调用安全。 */
void kos_pidfile_release(kos_pidfile *pf, int remove_file);

typedef struct {
    int ready_fd; /* 子进程用来报告启动结果的管道写端；-1 = 没走 daemonize */
} kos_daemon;

/*
 * 标准 daemonize：fork → 父进程阻塞等待就绪字节后退出（字节 0 = 启动成功，其他 =
 * 子进程的退出码）→ 子进程 setsid/umask(022)/chdir("/")/关继承 fd/dup2 到
 * /dev/null。返回时只在子进程里继续执行。
 *
 * 调用前必须把所有路径解析成绝对路径：chdir("/") 之后相对路径不再指向原处。
 */
int kos_daemonize(kos_daemon *d, kos_error *err);

/* 报告启动结果并关掉就绪管道；code 0 表示启动成功。foreground 下是空操作。 */
void kos_daemon_ready(kos_daemon *d, int code);

#endif /* KITCHENOS_DAEMON_H */
