/*
 * scheduler_fixture.h -- 单元测试里启动真实 wellcooked-scheduler 的小夹具。
 *
 * 只做三件事：把 config/maps/s_wellcooked_demo_2 的 kitchen/recipes 复制到临时目录，
 * 把工位 kind 加上 pid 后缀（资源信号量名因此不会撞上别的通道/别的测试），再起一个
 * 不接世界状态的 Scheduler。没有 shm 时 chefs 坐标未知，派工仍然会写 decisions.jsonl，
 * 正好用来检查决策字段和 SIGHUP 轮换。
 *
 * 头文件里的函数都标了 unused：test_assign 与 test_policy 各只用到一部分。
 */
#ifndef WELLCOOKED_TEST_SCHEDULER_FIXTURE_H
#define WELLCOOKED_TEST_SCHEDULER_FIXTURE_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "wellcooked/child.h"
#include "wellcooked/json.h"
#include "wellcooked/kitchen.h"
#include "wellcooked/recipes.h"
#include "wellcooked/resource.h"
#include "wellcooked/shm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KSFX_PATH_MAX 512
#define KSFX_SCENE "s_wellcooked_demo_2"

/* 用例在 ksfx_init_ex 前设置它来换地图（init 会 memset 结构体）。 */
static const char *ksfx_scene_override;

typedef struct {
    char dir[KSFX_PATH_MAX];
    char kitchen_path[KSFX_PATH_MAX];
    char recipes_path[KSFX_PATH_MAX];
    char orders_path[KSFX_PATH_MAX];
    char schedule_path[KSFX_PATH_MAX];
    char runtime_dir[KSFX_PATH_MAX];
    char runs_dir[KSFX_PATH_MAX];
    char state_name[64];
    char sem_name[64];
    kos_kitchen kitchen;
    pid_t pid;
    int chefs; /* 0 = 旧默认 2；测试在 spawn 前改 */
    int verbose; /* 1 = 给 Scheduler 加 --verbose（调试用） */
    int instance; /* 每个测试进程一个实例号：资源信号量与 mq 名不撞同机并发跑的套件 */
    char scene[64]; /* 空 = KSFX_SCENE；用例可改成 s_wellcooked_demo_3 用板/多灶 */
    int owns_test_shm;
} ksfx_run;

__attribute__((unused)) static void ksfx_copy(char *dst, size_t cap, const char *src)
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

__attribute__((unused)) static void ksfx_sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

__attribute__((unused)) static int ksfx_join(char *out, size_t cap, const char *base,
                                            const char *name)
{
    int n = snprintf(out, cap, "%s/%s", base, name);

    return n >= 0 && (size_t)n < cap ? 0 : -1;
}

__attribute__((unused)) static int ksfx_mkdir(const char *path)
{
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

__attribute__((unused)) static char *ksfx_read_file(const char *path, size_t *len_out)
{
    FILE *file = fopen(path, "rb");
    long size;
    char *text;

    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    text = calloc((size_t)size + 1, 1);
    if (text != NULL && fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        text = NULL;
    }
    fclose(file);
    if (text != NULL) {
        *len_out = (size_t)size;
    }
    return text;
}

__attribute__((unused)) static int ksfx_write_file(const char *path, const char *text,
                                                   size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t off = 0;

    if (fd < 0) {
        return -1;
    }
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    return close(fd) == 0 ? 0 : -1;
}

/* 把 text 里所有 old 替换成 new，返回新分配的字符串。 */
__attribute__((unused)) static char *ksfx_replace(const char *text, const char *old,
                                                  const char *new_text)
{
    size_t old_len = strlen(old);
    size_t new_len = strlen(new_text);
    size_t count = 0;
    const char *scan = text;
    char *out;
    char *dst;
    size_t text_len;

    while ((scan = strstr(scan, old)) != NULL) {
        count++;
        scan += old_len;
    }
    text_len = strlen(text);
    out = calloc(text_len + count * (new_len > old_len ? new_len - old_len : 0) + 1, 1);
    if (out == NULL) {
        return NULL;
    }
    dst = out;
    scan = text;
    for (;;) {
        const char *hit = strstr(scan, old);

        if (hit == NULL) {
            break;
        }
        memcpy(dst, scan, (size_t)(hit - scan));
        dst += hit - scan;
        memcpy(dst, new_text, new_len);
        dst += new_len;
        scan = hit + old_len;
    }
    strcpy(dst, scan);
    return out;
}

__attribute__((unused)) static int ksfx_remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *ent;

    if (dir == NULL) {
        return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
    }
    while ((ent = readdir(dir)) != NULL) {
        char child[KSFX_PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (ksfx_join(child, sizeof child, path, ent->d_name) != 0) {
            continue;
        }
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            (void)ksfx_remove_tree(child);
        } else {
            (void)unlink(child);
        }
    }
    closedir(dir);
    return rmdir(path);
}

__attribute__((unused)) static const char *const ksfx_kinds[] = {
    "serving", "dispenser", "cooker", "sink", "container", "bin", "mixer",
    "clean_plate_stack", "plate_stack"};

/*
 * 建立临时地图副本与一盘可派的订单。返回 0 表示资源信号量也已创建，清理时必须
 * 调 ksfx_cleanup()。
 */
__attribute__((unused)) static int ksfx_init_ex(ksfx_run *r, int with_orders)
{
    char source[KSFX_PATH_MAX];
    char relative[128];
    char suffix[KOS_KITCHEN_NAME_MAX];
    char orders[1024];
    static const char schedule_text[] =
        "{\n"
        "  \"weights\": {\"w_d\": 2.0, \"w_t\": 0.1, \"w_c\": 0.5, \"w_u\": 3.0,"
        " \"w_z\": 1.0},\n"
        "  \"cell_ms\": 107,\n"
        "  \"wash_ms\": 2000\n"
        "}\n";
    kos_error err;
    kos_recipes recipes;
    char *text;
    size_t len = 0;
    int dish;
    size_t i;

    memset(r, 0, sizeof *r);
    r->pid = -1;
    if (ksfx_scene_override != NULL) {
        ksfx_copy(r->scene, sizeof r->scene, ksfx_scene_override);
    }
    ksfx_copy(r->dir, sizeof r->dir, "/tmp/kos-sched-fx-XXXXXX");
    if (mkdtemp(r->dir) == NULL) {
        return -1;
    }
    if (ksfx_join(r->kitchen_path, sizeof r->kitchen_path, r->dir, "kitchen.json") != 0 ||
        ksfx_join(r->recipes_path, sizeof r->recipes_path, r->dir, "recipes.json") != 0 ||
        ksfx_join(r->orders_path, sizeof r->orders_path, r->dir, "orders.json") != 0 ||
        ksfx_join(r->schedule_path, sizeof r->schedule_path, r->dir, "schedule.json") != 0 ||
        ksfx_join(r->runtime_dir, sizeof r->runtime_dir, r->dir, "runtime") != 0 ||
        ksfx_join(r->runs_dir, sizeof r->runs_dir, r->dir, "runs") != 0) {
        return -1;
    }
    snprintf(r->state_name, sizeof r->state_name, "/kos_fx_state_%ld", (long)getpid());
    snprintf(r->sem_name, sizeof r->sem_name, "/kos_fx_sem_%ld", (long)getpid());
    /* --instance 的合法范围是 0..255（child.c 的 parse_long）。 */
    r->instance = (int)(getpid() % 250) + 1;
    snprintf(suffix, sizeof suffix, "%ld", (long)getpid());

    if (r->scene[0] == '\0') {
        ksfx_copy(r->scene, sizeof r->scene, KSFX_SCENE);
    }
    snprintf(relative, sizeof relative, "maps/%s/kitchen.json", r->scene);
    if (kos_child_default_config_path(relative, source, sizeof source) != 0) {
        return -1;
    }
    text = ksfx_read_file(source, &len);
    if (text == NULL) {
        return -1;
    }
    for (i = 0; i < sizeof ksfx_kinds / sizeof ksfx_kinds[0]; i++) {
        char old_token[64];
        char new_token[96];
        char *replaced;

        snprintf(old_token, sizeof old_token, "\"%s\"", ksfx_kinds[i]);
        snprintf(new_token, sizeof new_token, "\"zz%s_%s\"", suffix, ksfx_kinds[i]);
        replaced = ksfx_replace(text, old_token, new_token);
        free(text);
        if (replaced == NULL) {
            return -1;
        }
        text = replaced;
    }
    if (ksfx_write_file(r->kitchen_path, text, strlen(text)) != 0) {
        free(text);
        return -1;
    }
    free(text);
    len = 0;

    snprintf(relative, sizeof relative, "maps/%s/recipes.json", r->scene);
    if (kos_child_default_config_path(relative, source, sizeof source) != 0) {
        return -1;
    }
    text = ksfx_read_file(source, &len);
    if (text == NULL) {
        return -1;
    }
    for (i = 0; i < sizeof ksfx_kinds / sizeof ksfx_kinds[0]; i++) {
        char old_token[64];
        char new_token[96];
        char *replaced;

        snprintf(old_token, sizeof old_token, "\"%s\"", ksfx_kinds[i]);
        snprintf(new_token, sizeof new_token, "\"zz%s_%s\"", suffix, ksfx_kinds[i]);
        replaced = ksfx_replace(text, old_token, new_token);
        free(text);
        if (replaced == NULL) {
            return -1;
        }
        text = replaced;
    }
    if (ksfx_write_file(r->recipes_path, text, strlen(text)) != 0) {
        free(text);
        return -1;
    }
    free(text);

    memset(&recipes, 0, sizeof recipes);
    kos_error_clear(&err);
    if (kos_recipes_load(r->recipes_path, &recipes, &err) != KOS_OK ||
        recipes.dish_count == 0) {
        return -1;
    }
    dish = 0;
    if (with_orders) {
        snprintf(orders, sizeof orders,
                 "{\"seed\":1,\"level\":\"%s\",\"menu\":[{\"recipes\":[\"%.31s\"]}]}\n",
                 r->scene, recipes.dishes[dish].dish);
    } else {
        /* 空菜单：Scheduler 只会有 wash 一个候选，用来单独验证洗盘生成。 */
        snprintf(orders, sizeof orders, "{\"menu\":[]}\n");
    }
    if (ksfx_write_file(r->orders_path, orders, strlen(orders)) != 0 ||
        ksfx_write_file(r->schedule_path, schedule_text, strlen(schedule_text)) != 0) {
        return -1;
    }
    snprintf(relative, sizeof relative, "maps/%s/zones.json", r->scene);
    if (kos_child_default_config_path(relative, source, sizeof source) == 0 && access(source, R_OK) == 0) {
        char target_zones[KSFX_PATH_MAX];
        if (ksfx_join(target_zones, sizeof target_zones, r->dir, "zones.json") == 0) {
            text = ksfx_read_file(source, &len);
            if (text != NULL) {
                (void)ksfx_write_file(target_zones, text, len);
                free(text);
            }
        }
    }
    if (ksfx_mkdir(r->runtime_dir) != 0 || ksfx_mkdir(r->runs_dir) != 0) {
        return -1;
    }
    {
        char logs[KSFX_PATH_MAX];

        if (ksfx_join(logs, sizeof logs, r->runtime_dir, "logs") != 0 ||
            ksfx_mkdir(logs) != 0) {
            return -1;
        }
    }
    memset(&r->kitchen, 0, sizeof r->kitchen);
    kos_error_clear(&err);
    if (kos_kitchen_load(r->kitchen_path, &r->kitchen, &err) != KOS_OK) {
        return -1;
    }
    {
        kos_zones zones;
        kos_error_clear(&err);
        if (kos_zones_for_kitchen(r->kitchen_path, &zones, &err) == KOS_OK && zones.from_file) {
            (void)kos_zones_register_handoff(&zones, &r->kitchen, &err);
        }
    }
    kos_error_clear(&err);
    if (kos_resources_create(&r->kitchen, r->instance, &err) != KOS_OK) {
        return -1;
    }
    return 0;
}

__attribute__((unused)) static int ksfx_init(ksfx_run *r)
{
    return ksfx_init_ex(r, 1);
}

__attribute__((unused)) static int ksfx_spawn_ex(ksfx_run *r, const char *strategy,
                                                 const char *dispatch)
{
    char exe[KSFX_PATH_MAX];
    char bin[KSFX_PATH_MAX];
    char err_path[KSFX_PATH_MAX];
    char chefs[16];
    char tick[16];
    char instance[16];
    char *slash;
    char *argv[40];
    ssize_t n;
    int ac = 0;
    pid_t pid;

    if (r == NULL || r->pid > 0) {
        return -1;
    }
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) {
        return -1;
    }
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL) {
        return -1;
    }
    *slash = '\0';
    if (snprintf(bin, sizeof bin, "%s/wellcooked-scheduler", exe) >= (int)sizeof bin ||
        ksfx_join(err_path, sizeof err_path, r->runtime_dir, "scheduler-stderr.log") != 0) {
        return -1;
    }
    snprintf(chefs, sizeof chefs, "%d", r->chefs > 0 ? r->chefs : 2);
    snprintf(tick, sizeof tick, "50");

    argv[ac++] = bin;
    argv[ac++] = (char *)"--runtime-dir";
    argv[ac++] = r->runtime_dir;
    argv[ac++] = (char *)"--runs-dir";
    argv[ac++] = r->runs_dir;
    argv[ac++] = (char *)"--kitchen";
    argv[ac++] = r->kitchen_path;
    argv[ac++] = (char *)"--recipes";
    argv[ac++] = r->recipes_path;
    argv[ac++] = (char *)"--orders";
    argv[ac++] = r->orders_path;
    argv[ac++] = (char *)"--chefs";
    argv[ac++] = chefs;
    argv[ac++] = (char *)"--tick-ms";
    argv[ac++] = tick;
    snprintf(instance, sizeof instance, "%d", r->instance);
    argv[ac++] = (char *)"--instance";
    argv[ac++] = instance;
    if (r->verbose) {
        argv[ac++] = (char *)"--verbose";
    }
    argv[ac++] = (char *)"--strategy";
    argv[ac++] = (char *)strategy;
    if (dispatch != NULL) {
        argv[ac++] = (char *)"--dispatch";
        argv[ac++] = (char *)dispatch;
    }
    argv[ac++] = (char *)"--state-name";
    argv[ac++] = r->state_name;
    argv[ac++] = (char *)"--sem-name";
    argv[ac++] = r->sem_name;
    argv[ac] = NULL;
    {
        kos_shm_opts opts;
        kos_shm *shm = NULL;
        kos_error err;
        kos_world_state world;
        memset(&opts, 0, sizeof opts);
        opts.object_name = r->state_name;
        opts.sem_name = r->sem_name;
        opts.perm = 0600;
        kos_error_clear(&err);
        if (kos_shm_open(&opts, KOS_SHM_CREATE, &shm, &err) == KOS_OK) {
            r->owns_test_shm = 1;
            kos_world_state_reset(&world);
            world.has_round_time = 1;
            world.round_time_limit = 120.0f;
            world.round_time_left = 119.0f;
            world.game_time_ms = 1000;
            if (kos_shm_world_write(shm, &world) != KOS_OK) {
                (void)kos_shm_close(shm, NULL);
                return -1;
            }
            (void)kos_shm_close(shm, NULL);
        }
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        int fd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) {
                close(fd);
            }
        }
        execv(bin, argv);
        _exit(127);
    }
    r->pid = pid;
    return pid;
}

__attribute__((unused)) static int ksfx_spawn(ksfx_run *r, const char *strategy)
{
    return ksfx_spawn_ex(r, strategy, "flexible");
}


/* runs/<ts>/decisions.jsonl：临时 runs 目录是新建的，只会有一个子目录。 */
__attribute__((unused)) static int ksfx_decisions_path(const ksfx_run *r, char *out,
                                                       size_t cap)
{
    DIR *dir = opendir(r->runs_dir);
    struct dirent *ent;
    int found = -1;

    if (dir == NULL) {
        return -1;
    }
    while ((ent = readdir(dir)) != NULL) {
        char candidate[KSFX_PATH_MAX];

        if (ent->d_name[0] == '.') {
            continue;
        }
        if (ksfx_join(candidate, sizeof candidate, r->runs_dir, ent->d_name) == 0 &&
            ksfx_join(out, cap, candidate, "decisions.jsonl") == 0 &&
            access(out, R_OK) == 0) {
            found = 0;
            break;
        }
    }
    closedir(dir);
    return found;
}

/*
 * 等到 decisions.jsonl 里出现 needle，最多 timeout_ms。成功时把路径写进 out。
 * 用轮询而不是 inotify：只跑几秒，简单可靠。
 */
__attribute__((unused)) static int ksfx_wait_decisions(const ksfx_run *r, const char *needle,
                                                       int timeout_ms, char *out,
                                                       size_t cap)
{
    int waited = 0;

    while (waited <= timeout_ms) {
        char path[KSFX_PATH_MAX];
        char *text;
        size_t len = 0;
        int hit = 0;

        if (ksfx_decisions_path(r, path, sizeof path) == 0) {
            text = ksfx_read_file(path, &len);
            if (text != NULL) {
                hit = strstr(text, needle) != NULL;
                free(text);
            }
            if (hit) {
                if (out != NULL) {
                    ksfx_copy(out, cap, path);
                }
                return 0;
            }
        }
        ksfx_sleep_ms(20);
        waited += 20;
    }
    return -1;
}

__attribute__((unused)) static void ksfx_dump_file(const char *label, const char *path)
{
    size_t len = 0;
    char *text = ksfx_read_file(path, &len);

    if (text == NULL) {
        printf("[fixture] %s: <%s 不存在>\n", label, path);
        return;
    }
    printf("[fixture] %s (%s):\n%s\n", label, path, text);
    free(text);
}

__attribute__((unused)) static void ksfx_dump_logs(const ksfx_run *r)
{
    char path[KSFX_PATH_MAX];

    if (ksfx_join(path, sizeof path, r->runtime_dir, "scheduler-stderr.log") == 0) {
        ksfx_dump_file("stderr", path);
    }
    if (ksfx_join(path, sizeof path, r->runtime_dir, "logs/scheduler.log") == 0) {
        ksfx_dump_file("scheduler.log", path);
    }
}

/* 与 ksfx_wait_decisions 相同，但找同目录的 events.jsonl。 */
__attribute__((unused)) static int ksfx_wait_events(const ksfx_run *r, const char *needle,
                                                    int timeout_ms, char *out, size_t cap)
{
    int waited = 0;

    while (waited <= timeout_ms) {
        char decisions[KSFX_PATH_MAX];
        char path[KSFX_PATH_MAX];
        char *text;
        size_t len = 0;
        int hit = 0;

        if (ksfx_decisions_path(r, decisions, sizeof decisions) == 0) {
            char *slash = strrchr(decisions, '/');

            if (slash != NULL) {
                *slash = '\0';
                if (ksfx_join(path, sizeof path, decisions, "events.jsonl") == 0) {
                    text = ksfx_read_file(path, &len);
                    if (text != NULL) {
                        hit = strstr(text, needle) != NULL;
                        free(text);
                    }
                    if (hit) {
                        if (out != NULL) {
                            ksfx_copy(out, cap, path);
                        }
                        return 0;
                    }
                }
            }
        }
        ksfx_sleep_ms(20);
        waited += 20;
    }
    return -1;
}

/* 等到 Scheduler 日志里出现 needle（mq_ready 之类），最多 timeout_ms。 */
__attribute__((unused)) static int ksfx_wait_log(const ksfx_run *r, const char *needle,
                                                 int timeout_ms)
{
    char path[KSFX_PATH_MAX];
    int waited = 0;

    if (ksfx_join(path, sizeof path, r->runtime_dir, "logs/scheduler.log") != 0) {
        return -1;
    }
    while (waited <= timeout_ms) {
        size_t len = 0;
        char *text = ksfx_read_file(path, &len);

        if (text != NULL) {
            int hit = strstr(text, needle) != NULL;

            free(text);
            if (hit) {
                return 0;
            }
        }
        ksfx_sleep_ms(20);
        waited += 20;
    }
    return -1;
}


__attribute__((unused)) static int ksfx_stop(ksfx_run *r)
{
    int status = 0;
    int waited = 0;

    if (r->pid <= 0) {
        return 0;
    }
    (void)kill(r->pid, SIGTERM);
    while (waited < 3000) {
        pid_t done = waitpid(r->pid, &status, WNOHANG);

        if (done == r->pid) {
            r->pid = -1;
            return 0;
        }
        if (done < 0 && errno != EINTR) {
            r->pid = -1;
            return -1;
        }
        ksfx_sleep_ms(20);
        waited += 20;
    }
    (void)kill(r->pid, SIGKILL);
    (void)waitpid(r->pid, &status, 0);
    r->pid = -1;
    return -1;
}

__attribute__((unused)) static void ksfx_cleanup(ksfx_run *r)
{
    kos_error err;

    if (r->pid > 0) {
        (void)ksfx_stop(r);
    }
    if (r->owns_test_shm) {
        kos_shm_opts opts;
        memset(&opts, 0, sizeof opts);
        opts.object_name = r->state_name;
        opts.sem_name = r->sem_name;
        (void)kos_shm_unlink(&opts, NULL);
    }
    kos_error_clear(&err);
    (void)kos_resources_unlink(&r->kitchen, r->instance, &err);
    /* 调试用：KSFX_KEEP=1 保留临时地图与日志（默认仍然删干净）。 */
    if (r->dir[0] != '\0' && getenv("KSFX_KEEP") == NULL) {
        (void)ksfx_remove_tree(r->dir);
    }
    memset(r, 0, sizeof *r);
    r->pid = -1;
}

#endif /* WELLCOOKED_TEST_SCHEDULER_FIXTURE_H */
