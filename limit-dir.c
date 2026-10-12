#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/landlock.h>

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif

/* ---- 参数解析（可单测）---- */

struct config {
    const char *rw_dirs[64];
    int         n_rw;
    int         strict;
    int         cmd_index;   /* 命令在 argv 中的起始下标，-1 表示无命令 */
};

enum parse_result { PARSE_OK = 0, PARSE_HELP, PARSE_ERR_OPT,
                    PARSE_ERR_WARG, PARSE_ERR_WCOUNT, PARSE_ERR_NOCMD };

static enum parse_result parse_args(int argc, char **argv, struct config *cfg) {
    int i = 1;
    cfg->n_rw = 0;
    cfg->strict = 0;
    cfg->cmd_index = -1;

    while (i < argc) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
            return PARSE_HELP;
        else if (!strcmp(argv[i], "-w")) {
            if (i + 1 >= argc) return PARSE_ERR_WARG;
            if (cfg->n_rw >= (int)(sizeof(cfg->rw_dirs) / sizeof(cfg->rw_dirs[0])))
                return PARSE_ERR_WCOUNT;
            cfg->rw_dirs[cfg->n_rw++] = argv[i + 1];
            i += 2;
        }
        else if (!strcmp(argv[i], "--strict")) { cfg->strict = 1; i++; }
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else if (argv[i][0] == '-' && argv[i][1] != '\0')
            return PARSE_ERR_OPT;
        else break;
    }
    if (i >= argc) return PARSE_ERR_NOCMD;
    cfg->cmd_index = i;
    return PARSE_OK;
}

/* 展开路径开头的 "~" 为 $HOME，返回静态缓冲区或原串 */
static const char *expand_home(const char *path) {
    static char buf[4096];
    if (path[0] != '~') return path;
    const char *home = getenv("HOME");
    if (!home || !path[1]) return path;             // 无 HOME 或就是 "~"
    if (path[1] != '/') return path;                // "~user" 形式不处理
    if (snprintf(buf, sizeof(buf), "%s%s", home, path + 1) >= (int)sizeof(buf))
        return path;                                // 超长则原样返回（后续报错）
    return buf;
}

static int ll_create(const struct landlock_ruleset_attr *a, size_t s, unsigned f) {
    return syscall(__NR_landlock_create_ruleset, a, s, f);
}
static int ll_add(int fd, const struct landlock_path_beneath_attr *r) {
    return syscall(__NR_landlock_add_rule, fd, LANDLOCK_RULE_PATH_BENEATH, r, 0);
}
static int ll_restrict(int fd) {
    return syscall(__NR_landlock_restrict_self, fd, 0);
}

/*
 * 添加一条 Landlock 规则。
 * silent: 0 = 目标打开失败时报错退出（必须存在的路径）
 *         1 = 目标不存在时静默跳过（可选路径）
 */
static int add_rule(int rs, const char *path, uint64_t access, int silent) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) {
        if (silent) return 0;
        fprintf(stderr, "limit-dir: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct landlock_path_beneath_attr rule = { .allowed_access = access, .parent_fd = fd };
    if (ll_add(rs, &rule)) {
        fprintf(stderr, "limit-dir: add rule %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* ============ 默认文件访问表 ============ */

struct rule_def {
    const char *path;     /* 规则目标 */
    uint64_t    access;   /* 放行的权限 */
    int         optional; /* 1 = 不存在时静默跳过 */
};

/*
 * extra_rules 在 "/" 只读规则之后应用，用于覆盖/追加例外。
 * 注意：Landlock 规则是"并集"，后添加的规则只能放宽、不能收紧，
 * 因此这些规则是对全盘只读策略的局部放行。
 */
static const struct rule_def extra_rules[] = {
    /* 常见无副作用设备文件：程序普遍依赖读写它们 */
    { "/dev/null",    LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, 1 },
    { "/dev/zero",    LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, 1 },
    { "/dev/full",    LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, 1 },
    { "/dev/random",  LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, 1 },
    { "/dev/urandom", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, 1 },
    { "/dev/tty",     LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, 1 },
    { "/tmp",         LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, 1 },
};

/*
 * 默认可写目录表（在 "/" 只读基准之上放行完全读写）。
 * 路径支持开头 "~" 展开为 $HOME。
 * --strict 时跳过本表（当前目录与 -w 指定目录仍可写）。
 */
static const char *const default_rw_dirs[] = {
    "/tmp",
    "~/.cache",
    "~/.config",
    "~/.local",
    "~/.gnupg",
    "~/.gnupg",
    "~/.npm",
};

static _Noreturn void usage(int code) {
    fprintf(code ? stderr : stdout,
        "limit-dir - 在受限文件系统下运行程序（基于 Landlock）\n"
        "\n"
        "用法:\n"
        "  limit-dir [-w DIR]... COMMAND [ARGS...]\n"
        "\n"
        "说明:\n"
        "  在当前目录及通过 -w 指定的目录内，COMMAND 拥有完全读写权限；\n"
        "  文件系统其余部分对 COMMAND 只读（包括其所有子进程）。\n"
        "  无需 root 权限，要求内核 >= 5.13。\n"
        "默认额外可写: /tmp, ~/.cache, ~/.config, ~/.local, ~/.gnupg, ~/.npm,\n"
        "              $XDG_RUNTIME_DIR（agent socket）\n"
        "  （--strict 可关闭这些默认放行，仅保留当前目录与 -w 目录）\n"
        "\n"
        "参数:\n"
        "  -w DIR        额外开放 DIR 为可写目录，可多次指定\n"
        "  --strict      禁用默认可写目录，仅当前目录与 -w 目录可写\n"
        "  -h, --help    显示本帮助并退出\n"
        "  --            结束参数解析，其后的内容作为命令执行\n"
        "\n"
        "示例:\n"
        "  limit-dir bash                    # bash 只能写当前目录\n"
        "  limit-dir -w /tmp -w ~/.cache app # 额外开放 /tmp 和 ~/.cache\n"
        "  limit-dir -- bash -c 'echo hi'    # 命令以 - 开头时用 -- 分隔\n"
        "\n"
        "版本: 1.1\n");
    exit(code);
}

#ifndef UNIT_TEST
int main(int argc, char **argv) {
    struct config cfg;

    switch (parse_args(argc, argv, &cfg)) {
        case PARSE_OK:       break;
        case PARSE_HELP:     usage(0);
        case PARSE_ERR_OPT:  fprintf(stderr, "limit-dir: 未知选项: %s\n\n", argv[1]); usage(1);
        case PARSE_ERR_WARG: fprintf(stderr, "limit-dir: -w 需要一个目录参数\n\n"); usage(1);
        case PARSE_ERR_WCOUNT: fprintf(stderr, "limit-dir: -w 数量过多\n"); return 1;
        case PARSE_ERR_NOCMD:
        default:             fprintf(stderr, "limit-dir: 缺少要执行的命令\n\n"); usage(1);
    }

    int i = cfg.cmd_index;
    int strict = cfg.strict;
    int n_rw   = cfg.n_rw;
    const char **rw_dirs = cfg.rw_dirs;

    // ---- 探测内核 ABI 版本 ----
    int abi = ll_create(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) { perror("limit-dir: Landlock not supported (need kernel >= 5.13)"); return 1; }

    // ---- 计算要管控的权限集合 ----
    uint64_t handled =
        LANDLOCK_ACCESS_FS_EXECUTE |
        LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2) handled |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) handled |= LANDLOCK_ACCESS_FS_TRUNCATE;

    struct landlock_ruleset_attr rs_attr = { .handled_access_fs = handled };
    int rs = ll_create(&rs_attr, sizeof(rs_attr), 0);
    if (rs < 0) { perror("limit-dir: create ruleset"); return 1; }

    // 全盘只读：放行读 + 执行
    uint64_t ro = LANDLOCK_ACCESS_FS_READ_FILE
                | LANDLOCK_ACCESS_FS_READ_DIR
                | LANDLOCK_ACCESS_FS_EXECUTE;
    if (abi >= 2) ro |= LANDLOCK_ACCESS_FS_REFER;

    // ---- 依次应用规则 ----

    // 1. 全盘只读（基准规则）
    if (add_rule(rs, "/", ro, 0)) return 1;

    // 2. 默认例外表（设备文件等，可选路径静默跳过）
    for (size_t k = 0; k < sizeof(extra_rules) / sizeof(extra_rules[0]); k++)
        if (add_rule(rs, extra_rules[k].path, extra_rules[k].access,
                     extra_rules[k].optional))
            return 1;

    // 3. 默认可写目录（--strict 时跳过；optional=1，不存在不报错）
    if (!strict) {
        for (size_t k = 0; k < sizeof(default_rw_dirs) / sizeof(default_rw_dirs[0]); k++)
            if (add_rule(rs, expand_home(default_rw_dirs[k]), handled, 1))
                return 1;
    }

    // 3.5 XDG 运行时目录（gpg-agent/ssh-agent 等在此创建 unix socket）
    if (!strict) {
        char rt[4096];
        const char *x = getenv("XDG_RUNTIME_DIR");
        if (!x || !*x) {
            snprintf(rt, sizeof(rt), "/run/user/%d", getuid());
            x = rt;
        }
        if (add_rule(rs, x, handled, 1)) return 1;
    }

    // 3. 当前目录 + -w 指定的目录：放开全部权限
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return 1; }
    if (add_rule(rs, cwd, handled, 0)) return 1;
    for (int k = 0; k < n_rw; k++)
        if (add_rule(rs, rw_dirs[k], handled, 0)) return 1;

    // ---- 生效并切换身份 ----
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) { perror("prctl"); return 1; }
    if (ll_restrict(rs)) { perror("limit-dir: restrict"); return 1; }

    execvp(argv[i], &argv[i]);
    perror("limit-dir: exec");
    return 127;
}
#endif
