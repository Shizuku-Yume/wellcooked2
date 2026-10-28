/*
 * silent_child.c -- 测试替身：收心跳管道参数，但从不写心跳，也不退出。
 *
 * 健康检查只有在"进程还活着但不再响应"时才有意义；kill -9 由 SIGCHLD 就报了，根本
 * 走不到 SIGALRM。这个替身让 `child_unresponsive` 这条路径能在一次测试里被观察到，
 * 而不是靠读代码相信它存在。SIGTERM 仍然响应，测试收尾不会留孤儿。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    struct timespec ts = {0, 200 * 1000 * 1000L};

    (void)argc;
    (void)argv;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);

    while (!g_stop) {
        (void)nanosleep(&ts, NULL);
    }
    return 0;
}
