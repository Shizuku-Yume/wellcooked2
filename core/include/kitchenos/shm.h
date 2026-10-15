/*
 * shm.h —— KitchenOS 共享世界状态与输入槽（模块契约）
 *
 * 为什么不把这块塞进 bridge.h：Bridge 是唯一写者，而 Scheduler、Chef Worker、
 * Supervisor 都是读者。谁都不该为了读一眼世界状态就去链接 Bridge 的模块。
 *
 * 并发模型（冻结，别改）：
 *   - 世界状态：Bridge 独占写、其它进程并发读 → seqlock。写者把序号改奇、写屏障、
 *     改数据、写屏障、序号改偶；读者两次读到的序号相同才认这份快照。
 *   - 输入槽：每个厨师一格，只有该厨师的 Worker 写自己那格 → 每格一个独立 seqlock。
 *     Worker 写完 sem_post("/kitchenos_input_ready")。Bridge sem_timedwait 等到有动静后
 *     必须回槽里核对 frame_id：计数信号量分不清是哪一格、哪一帧，它只是门铃，
 *     "第 N 帧该槽是否就绪"的唯一权威是槽本身。
 *
 * 版本与残留：段头第一个字段是 magic + version，创建时还会把结构尺寸一起写进去。
 * 版本、magic 或尺寸任一不匹配都拒绝挂接并给出明确错误 —— 绝不复用布局不同的旧段，
 * 那会把字段读串而不是崩掉，是最难查的一类 bug。
 *
 * 名字与目录可配：默认对象 /kitchenos_state、信号量 /kitchenos_input_ready，都在
 * /dev/shm（tmpfs）。可用环境变量 KITCHENOS_SHM_NAME / KITCHENOS_SEM_NAME /
 * KITCHENOS_SHM_DIR，或直接填 kos_shm_opts 覆盖。测试给每次运行取带 pid 的独立名字，
 * 彼此互不干扰，崩溃残留也撞不上。
 *
 * 所有失败都以返回码表达并填 kos_error（errno + 出错位置），库代码不 abort、不 exit。
 */

#ifndef KITCHENOS_SHM_H
#define KITCHENOS_SHM_H

/* 特性宏必须早于任何系统头文件：-std=c99 会把 shm_open/sem_timedwait/clock_nanosleep
 * 这些 POSIX.1-2008 接口的声明藏起来。命令行已经给了同一个值时这里不覆盖，
 * 免得和 Makefile 的 -D 打架。 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <semaphore.h>

/* ============================================================ 规模上限 */
/*
 * 共享内存里不能有指针、不能动态分配、也不能按需 realloc，所以所有集合都是
 * "定长数组 + 显式计数"：读者只看前 count 项。
 *
 * 上限按课程演示规模取：游戏最多 4 名本地厨师，留一倍余量到 8；同屏订单 16；
 * 一道菜最多 6 种食材；工作台/锅/盘子堆合计 32 个。
 * 改这些数字会改变段布局，必须同时升 KOS_SHM_VERSION。
 */
#define KOS_SHM_MAX_CHEFS 8
#define KOS_SHM_MAX_ORDERS 16
#define KOS_SHM_MAX_ORDER_INGREDIENTS 6
#define KOS_SHM_MAX_RESOURCES 32

/* 定长字符串带宽（含结尾 NUL）。名字类字段用插件上报的原样短名，
 * 例如 "tomato"、"order_003"、"pot_0"、"cutting_board_1"。 */
#define KOS_SHM_NAME_MAX 32    /* 厨师手上物品、订单 id/recipe、资源名、目标物 */
#define KOS_SHM_STATE_MAX 24   /* "idle"/"moving"/"cooking"/"burning" 之类 */
#define KOS_SHM_CONTENT_MAX 32 /* 锅里/板上的内容，插件用 "a+b" 拼串 */
#define KOS_SHM_INGREDIENT_MAX 24 /* 单个食材名 */

/* 对象名、信号量名、目录名的缓冲区长度（Linux 名字上限 255）。 */
#define KOS_SHM_PATH_MAX 256

/* ======================================================== 版本与默认值 */

/* "KOS1"：段头第一个字段。用可读魔数是为了 `xxd /dev/shm/kitchenos_state`
 * 时一眼能看出这是不是我们的段。 */
#define KOS_SHM_MAGIC 0x4b4f5331u
#define KOS_SHM_VERSION 1u

#define KOS_SHM_DEFAULT_NAME "/kitchenos_state"
#define KOS_SHM_DEFAULT_SEM_NAME "/kitchenos_input_ready"
#define KOS_SHM_DEFAULT_DIR "/dev/shm"

#define KOS_SHM_ENV_NAME "KITCHENOS_SHM_NAME"
#define KOS_SHM_ENV_SEM_NAME "KITCHENOS_SEM_NAME"
#define KOS_SHM_ENV_DIR "KITCHENOS_SHM_DIR"

/* 世界状态读取的默认尝试次数：写者持奇序号的时间在微秒级，而每次尝试只是一次
 * 几 KB 的 memcpy，几十次足够；给不出结果说明写者卡住或有人没走 seqlock，
 * 这时报 KOS_ERR_BUSY 比无限自旋诚实。 */
#define KOS_SHM_READ_ATTEMPTS 64

/* ============================================================== 错误码 */

enum {
    KOS_OK = 0,
    KOS_ERR_INVAL = -1,   /* 参数不合法（空指针、下标越界、名字格式错） */
    KOS_ERR_NOMEM = -2,   /* calloc/mmap 失败 */
    KOS_ERR_IO = -3,      /* 系统调用失败，errno 记在 kos_error.sys_errno */
    KOS_ERR_EXISTS = -4,  /* 要求新建，但对象已存在（绝不复用旧段） */
    KOS_ERR_MISSING = -5, /* 要求挂接，但对象不存在 */
    KOS_ERR_MAGIC = -6,   /* 段头魔数不对，不是 KitchenOS 的段 */
    KOS_ERR_VERSION = -7, /* 版本不匹配 */
    KOS_ERR_LAYOUT = -8,  /* 结构尺寸/大小不匹配（不同编译单元的结构漂移） */
    KOS_ERR_DIR = -9,     /* shm 目录不可用，或对象没落在配置的目录里 */
    KOS_ERR_RANGE = -10,  /* 槽号/厨师号越界 */
    KOS_ERR_EMPTY = -11,  /* 目标从未被写过（世界状态或输入槽） */
    KOS_ERR_STALE = -12,  /* 槽里有内容，但不是请求的那一帧 */
    KOS_ERR_BUSY = -13,   /* 尝试次数用尽仍读到撕裂的快照 */
    KOS_ERR_TIMEOUT = -14 /* 等就绪信号量超时 */
};

/*
 * 失败上下文。为什么不只返回 errno：同一个 EINVAL 可能来自 shm_open、ftruncate
 * 或 sem_open，日志里分不清。context 写清"哪一步、哪个名字、什么值"，
 * sys_errno 只在真的由系统调用引起时非 0。
 */
typedef struct {
    int code;           /* KOS_ERR_* */
    int sys_errno;      /* 失败系统调用的 errno，没有则为 0 */
    char context[192];  /* "shm_open(/kitchenos_state)"、"版本不匹配: 段 2 / 本程序 1" */
} kos_error;

void kos_error_clear(kos_error *err);

/* 把错误渲染成一行可读文本，返回写入长度（不含 NUL）；buf 不够长会截断。 */
int kos_error_format(const kos_error *err, char *buf, size_t len);

/* 错误码的短名字，例如 "KOS_ERR_VERSION"；未知码返回 "KOS_ERR_UNKNOWN"。 */
const char *kos_strerror(int code);

/* ========================================================== 动作与槽 */

/* 输入动作种类。字符串名字由 kos_action_kind_name() 给出，与插件协议里的 action 取值同名：
 * move / stop / interact / pickup / dash 五个都上线（protocol/README.md §3.6 +
 * protocol/schema/action.schema.json），bridge.c 的 build_action_from_slot() 各有一个构造器，
 * 所以 Worker 往槽里写什么，插件就收到什么。NONE 不是动作，是"这个 Worker 本帧没话说"，
 * Bridge 按 idle 的 stop 处理。插件的 press/release/tap 不在这里 —— 要按帧调度原始按键时，
 * 再同时扩这个枚举与线上枚举。 */
typedef enum {
    KOS_ACTION_NONE = 0,
    KOS_ACTION_MOVE = 1,
    KOS_ACTION_STOP = 2,
    KOS_ACTION_INTERACT = 3,
    KOS_ACTION_PICKUP = 4,
    KOS_ACTION_DASH = 5
} kos_action_kind;

typedef struct {
    uint64_t frame_id;                     /* 0 表示这格还从没写过 */
    int32_t chef_id;                       /* 该槽归属的厨师，-1 = 未指定 */
    int32_t kind;                          /* kos_action_kind */
    float target_x;
    float target_y;
    char target_object[KOS_SHM_NAME_MAX];  /* 交互目标，如 "pot_0"；空串表示无 */
    uint32_t checksum; /* 写者填的载荷校验，消费者只读，见 kos_action_checksum() */
    uint32_t reserved;
} kos_input_action;

/*
 * 一格输入槽 = 一个 seqlock 序号 + 一份动作载荷。
 *
 * 为什么要 checksum：只有序号相等这一条判据时，"写者中途死掉、下一个写者把序号
 * 补回偶数"这种场景会让读者接受一份撕裂的载荷。载荷自带校验后，读者永远只认
 * 自洽的快照，"第 N 帧该槽是否就绪"才谈得上是确定的是/否。
 */
typedef struct {
    uint64_t seq;             /* 只有该厨师的 Worker 写；奇 = 正在写 */
    kos_input_action action;
} kos_input_slot;

/* ========================================================== 世界状态 */

typedef struct {
    int32_t id;
    float x;
    float y;
    char holding[KOS_SHM_NAME_MAX]; /* 手上物品名；空串 = 空手 */
    char state[KOS_SHM_STATE_MAX];  /* 插件给的 "idle"/"moving" 等 */
} kos_chef;

typedef struct {
    char id[KOS_SHM_NAME_MAX];    /* "order_003"，与插件上报一致（原始 id 是字符串） */
    char recipe[KOS_SHM_NAME_MAX];/* 菜名；未知时为空串 */
    int32_t time_remaining;       /* 剩余秒数，0 = 无/已过期 */
    int32_t ingredient_count;     /* 下面 ingredients[] 里有效的前 N 项 */
    char ingredients[KOS_SHM_MAX_ORDER_INGREDIENTS][KOS_SHM_INGREDIENT_MAX];
} kos_order;

typedef struct {
    char name[KOS_SHM_NAME_MAX];       /* "pot_0"、"cutting_board_1"、"plate_stack" */
    char state[KOS_SHM_STATE_MAX];     /* "free"/"occupied"/"cooking"/"cooked"/"burning" */
    char content[KOS_SHM_CONTENT_MAX]; /* 内容物，插件用 "a+b" 拼串 */
    float time_left;                   /* 烹饪剩余秒；不适用时为 0 */
    int32_t chef_id;                   /* 占用者厨师号，-1 = 无人/未知 */
    int32_t count;                     /* 计数类资源（plate_stack）的数量 */
} kos_resource;

/* 显式 padding：让字段偏移不依赖编译器的隐式对齐，跨编译单元读同一段才稳定。
 * 这些字节无意义，任何人都不该读它们。 */
typedef struct {
    int64_t score;
    int64_t game_time; /* 本局已进行秒数 */
    uint32_t chef_count;
    uint32_t reserved0;
    kos_chef chefs[KOS_SHM_MAX_CHEFS];
    uint32_t order_count;
    uint32_t reserved1;
    kos_order orders[KOS_SHM_MAX_ORDERS];
    uint32_t resource_count;
    uint32_t reserved2;
    kos_resource resources[KOS_SHM_MAX_RESOURCES];
    /* 下面两个字段由写者填、读者用来自证，业务代码不要读： */
    uint64_t write_seq; /* 发布这份快照时的 seqlock 序号（必为偶数） */
    uint32_t checksum;  /* 本结构 checksum 字段之前所有字节的 FNV-1a */
    uint32_t reserved3;
} kos_world_state;

/* ============================================================== 段布局 */

/*
 * 一个段 = 段头 + 世界状态 + N 个输入槽。mmap 出来就是这块内存本身。
 * 段头里那些尺寸字段是给"另一个编译单元编出来的同版本程序"看的：
 * 两边结构体大小不一致时宁可拒绝挂接，也不要按错位读。
 */
typedef struct {
    uint32_t magic;      /* KOS_SHM_MAGIC */
    uint32_t version;    /* KOS_SHM_VERSION */
    uint32_t header_size;/* sizeof(kos_shm_segment) */
    uint32_t world_size; /* sizeof(kos_world_state) */
    uint32_t slot_size;  /* sizeof(kos_input_slot) */
    uint32_t slot_count; /* = KOS_SHM_MAX_CHEFS */
    uint32_t max_chefs;
    uint32_t max_orders;
    uint32_t max_resources;
    uint32_t max_order_ingredients;
    int64_t creator_pid;
    int64_t created_at_ms; /* CLOCK_REALTIME 毫秒，只用于日志与排障 */
    uint64_t world_seq;    /* 世界状态 seqlock 序号：奇 = 有写者在改 */
    uint32_t reserved[4];
    kos_world_state world;
    kos_input_slot slots[KOS_SHM_MAX_CHEFS];
} kos_shm_segment;

/* 段大小上限的编译期自检：共享内存段是整块映射的，长到几十 KB 就该重新想布局，
 * 而不是默默让每个读者每帧多拷几十 KB。 */
typedef char kos_shm_segment_size_ok[(sizeof(kos_shm_segment) <= 16384) ? 1 : -1];

/* ============================================================== 句柄 */

/*
 * 打开后的句柄。seg 公开是因为段布局本身就是模块契约：读者直接
 * shm->seg->world.chefs[i].x 取值即可，不必为每个字段再造一层访问器；
 * 但**必须**走 kos_shm_world_read()/kos_shm_slot_* 拿一致快照，
 * 直接读 seg->world 拿到的是正在被改的内存。
 */
typedef struct kos_shm {
    int fd;             /* 对象 fd，保留用于 fstat 与排障 */
    sem_t *sem;         /* 就绪信号量（已映射，未拥有其生命周期） */
    kos_shm_segment *seg;
    size_t map_size;
    int created;        /* 本进程是否创建了这个段 */
    uint64_t write_base;/* 内部：进行中的世界状态写入将发布的偶数序号 */
    char object_name[KOS_SHM_PATH_MAX]; /* 传给 shm_open 的名字 */
    char sem_name[KOS_SHM_PATH_MAX];
    char dir[KOS_SHM_PATH_MAX];         /* 对象实际所在的 tmpfs 目录 */
} kos_shm;

/* ============================================================ 打开选项 */

typedef enum {
    KOS_SHM_ATTACH = 0,  /* 只能用已存在的段（读者、重连的 Bridge） */
    KOS_SHM_CREATE = 1,  /* 新建；同名已存在则 KOS_ERR_EXISTS */
    KOS_SHM_RECREATE = 2 /* 先清掉同名旧段再新建：只给启动时的崩溃残留用 */
} kos_shm_mode;

typedef struct {
    const char *object_name; /* NULL → $KITCHENOS_SHM_NAME → KOS_SHM_DEFAULT_NAME */
    const char *sem_name;    /* NULL → $KITCHENOS_SEM_NAME → KOS_SHM_DEFAULT_SEM_NAME */
    const char *dir;         /* NULL → $KITCHENOS_SHM_DIR → KOS_SHM_DEFAULT_DIR */
    mode_t perm;             /* 0 → 0666（仍受 umask 影响） */
} kos_shm_opts;

/* 填默认值：先清空，再按 env → 常量 的顺序取名字。 */
void kos_shm_opts_init(kos_shm_opts *opts);

/*
 * 打开（并映射）段。成功时 *out 由本模块分配，交给 kos_shm_close() 释放。
 *
 * dir 的语义：POSIX 把命名共享内存对象放在**唯一一个** tmpfs 目录里
 * （Linux 是 /dev/shm），而且 glibc 会拒绝含内嵌 '/' 的名字，所以名字选不了
 * 子目录。dir 因此被当作"这些对象应该落在哪个目录"的断言：创建/挂接后会核对
 * 对象确实出现在 dir 里，不在就报 KOS_ERR_DIR，而不是让人以为写进了别处。
 */
int kos_shm_open(const kos_shm_opts *opts, kos_shm_mode mode, kos_shm **out, kos_error *err);

/* 解除映射并关闭 fd/信号量（不删对象）。shm 为 NULL 时返回 KOS_ERR_INVAL。 */
int kos_shm_close(kos_shm *shm, kos_error *err);

/* 删掉对象与信号量。对象本就不存在也算成功（清理要幂等）。 */
int kos_shm_unlink(const kos_shm_opts *opts, kos_error *err);

/* ======================================================== 世界状态读写 */

/* 把世界状态清成"空厨房"：计数全 0、得分与时间 0。 */
void kos_world_state_reset(kos_world_state *world);

/*
 * 写者（Bridge）三件套：
 *   w = kos_shm_world_write_begin(shm);   // 序号转奇，拿到可写载荷
 *   ... 逐字段填 w ...
 *   kos_shm_world_write_end(shm);         // 填 write_seq/checksum，序号转偶
 * begin 与 end 必须成对，中间不要做慢操作（读文件、写日志、等 socket）：
 * 那段时间所有读者都在重试。
 */
kos_world_state *kos_shm_world_write_begin(kos_shm *shm);
void kos_shm_world_write_end(kos_shm *shm);

/* 一次性写入整份快照（内部就是 begin + memcpy + end）。 */
int kos_shm_world_write(kos_shm *shm, const kos_world_state *world);

/*
 * 读者：拷一份自洽的快照到 *out。
 * max_attempts = 0 时用 KOS_SHM_READ_ATTEMPTS，它限的是**真正拷过载荷的次数**：
 * 撞上写者正在写时先等它写完再重试，等待本身不消耗预算（否则读者会在写者那几微秒里
 * 把预算烧光，报出一堆假 KOS_ERR_BUSY）。attempts_out 可空，成功时回报实际拷了几次
 * （>1 说明撞上过写者）。撕裂次数用尽仍不收敛 → KOS_ERR_BUSY。
 *
 * 只有返回 KOS_OK 时 *out 才有意义：读到撕裂的那次拷贝是直接落进调用者缓冲的
 * （少一次 7 KB 拷贝，读路径上每个读者都要付），失败返回时里面的内容不作数 ——
 * 判断"这格有没有东西"一律看返回码，别看缓冲。
 */
int kos_shm_world_read(kos_shm *shm, kos_world_state *out, uint32_t max_attempts,
                       uint32_t *attempts_out);

/* 载荷校验和（FNV-1a，覆盖 checksum 字段之前的字节）。 */
uint32_t kos_world_checksum(const kos_world_state *world);

/* 1 = 这份快照自洽；0 = 被撕裂或不是本模块写的。 */
int kos_world_state_check(const kos_world_state *world);

/* ======================================================== 输入槽读写 */

/*
 * Worker 侧：把自己那格填好并 sem_post 门铃。内部是"序号转奇 → 写 → 转偶 → post"，
 * 所以调用者只要给一份完整的动作，不需要自己管 seqlock。
 */
int kos_shm_slot_publish(kos_shm *shm, uint32_t slot, const kos_input_action *action);

/* Bridge 侧：读该槽当前最后一版（不判帧）。从未写过 → KOS_ERR_EMPTY。 */
int kos_shm_slot_read(kos_shm *shm, uint32_t slot, kos_input_action *out);

/* Bridge 侧：只有当该槽正好是 frame_id 这一版时才拷出来。
 * 别的帧 → KOS_ERR_STALE；从未写过 → KOS_ERR_EMPTY；读到撕裂 → KOS_ERR_BUSY。 */
int kos_shm_slot_read_for_frame(kos_shm *shm, uint32_t slot, uint64_t frame_id,
                                kos_input_action *out);

/* "第 frame_id 帧、第 slot 格就绪了吗"：1 = 是，0 = 否，<0 = 参数/访问错误。
 * 这是确定性的是/否判定（序号 + 载荷校验都对上才算就绪），不含任何等待。 */
int kos_shm_slot_frame_ready(kos_shm *shm, uint32_t slot, uint64_t frame_id);

/* ========================================================== 就绪信号量 */

/* 投币（Worker 写完槽后调用；kos_shm_slot_publish 已经代劳）。 */
int kos_shm_post_ready(kos_shm *shm);

/* 等门铃。timeout_ms < 0 表示一直等；0 表示只探一下；>0 超时返回 KOS_ERR_TIMEOUT。 */
int kos_shm_wait_ready(kos_shm *shm, int timeout_ms);

/* 丢掉积压的门铃（开新帧前清场），返回丢掉的个数。 */
int kos_shm_drain_ready(kos_shm *shm);

/* ============================================================ 帧屏障 */

/*
 * 等 [0, chef_count) 每一格都装着 frame_id。
 *   - 全就绪：返回 KOS_OK，*missing_mask = 0。
 *   - 超时：返回 KOS_ERR_TIMEOUT，*missing_mask 里第 k 位为 1 表示第 k 格没到，
 *     其余格的就绪状态保持有效（Bridge 据此对这些厨师下发 idle）。
 * 门铃只是唤醒手段：每被唤醒一次就重新核对所有槽，所以过期的 post 只会造成
 * 一次多余的检查，不会造成误判。
 */
int kos_shm_wait_frame(kos_shm *shm, uint64_t frame_id, uint32_t chef_count, int timeout_ms,
                       uint64_t *missing_mask);

/*
 * 等每一格都出现比 last_frame[k] 更新的版本，用来对接"frame_id 由另一侧自己递增"
 * 的场景：Worker 从 1 往上数，Bridge 也在数自己的帧号，两边不可能精确对齐，
 * 但"这一帧有没有新输入"照样能判定。
 * 每格就绪时把该格的 last_frame[k] 推进到刚消费的 frame_id（超时返回时也一样，
 * 已经拿到的不会重复等）。
 *
 * *missing_mask 是**累积**语义：只有整轮里始终没交出更新版本的格子才置位。
 * 别把它当瞬时值用 —— 水位一推进，同一格立刻又"不比水位新"了。
 */
int kos_shm_wait_frame_after(kos_shm *shm, uint64_t *last_frame, uint32_t chef_count,
                             int timeout_ms, uint64_t *missing_mask);

/* ================================================================ 杂项 */

/* 动作种类 ←→ 协议里的动作名（"move"/"stop"/...）。未知名字返回 -1。 */
const char *kos_action_kind_name(int kind);
int kos_action_kind_from_name(const char *name);

#endif /* KITCHENOS_SHM_H */
