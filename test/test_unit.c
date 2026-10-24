/* limit-dir 单元测试：直接 include 源码，测纯函数 */
#ifndef UNIT_TEST
#define UNIT_TEST   /* 兼容直接编译（无 -DUNIT_TEST），避免与 Makefile 重复定义 */
#endif
#include "../limit-dir.c"

static int failures = 0, total = 0;

/* CHECK：无附加诊断；CHECKF：带 printf 格式的诊断信息。
 * 分开定义是为了在 -std=c11 下合法（空 __VA_ARGS__ 展开成 printf()
 * 是硬错误，GNU 的 ##__VA_ARGS__ 则是扩展，不用）。 */
#define CHECK(cond, name) do { \
    total++; \
    if (cond) { printf("ok %d - %s\n", total, name); } \
    else { failures++; printf("not ok %d - %s\n", total, name); } \
} while (0)

#define CHECKF(cond, name, fmt, ...) do { \
    total++; \
    if (cond) { printf("ok %d - %s\n", total, name); } \
    else { failures++; printf("not ok %d - %s: ", total, name); printf(fmt, __VA_ARGS__); printf("\n"); } \
} while (0)

/* 引用仅生产 main() 使用的 static 符号，避免 -Wall 下
 * "defined but not used" 假告警 */
static void keep_prod_symbols(void) {
    (void)usage; (void)add_rule;
    (void)ll_create; (void)ll_add; (void)ll_restrict;
}

/* 静态缓冲区版本的可控 HOME 设置/恢复 */
static void set_home(const char *h) { h ? setenv("HOME", h, 1) : unsetenv("HOME"); }

static void test_expand_home(void) {
    set_home("/home/alice");
    CHECK(!strcmp(expand_home("/tmp"), "/tmp"), "plain path unchanged");
    CHECK(!strcmp(expand_home("~/.cache"), "/home/alice/.cache"), "~ expands");
    CHECK(!strcmp(expand_home("~"), "~"), "bare ~ unchanged");
    CHECK(!strcmp(expand_home("~bob/x"), "~bob/x"), "~user unchanged");

    set_home(NULL);
    CHECK(!strcmp(expand_home("~/.x"), "~/.x"), "no HOME -> unchanged");
    set_home("/home/alice");

    /* 溢出：HOME 超长时 snprintf 截断，应返回原串而非半截路径 */
    static char big[5000];
    memset(big, 'A', sizeof(big) - 1); big[sizeof(big)-1] = 0;
    set_home(big);
    CHECK(!strcmp(expand_home("~/x"), "~/x"), "overflow returns original");
    set_home("/home/alice");
}

static void test_parse_args(void) {
    struct config c;

    char *a1[] = {"x", "ls", "-l"};
    CHECK(parse_args(3, a1, &c) == PARSE_OK && c.cmd_index == 1 && !c.strict,
          "plain command");

    char *a2[] = {"x", "-w", "/tmp", "--strict", "app"};
    CHECK(parse_args(5, a2, &c) == PARSE_OK && c.n_rw == 1
          && !strcmp(c.rw_dirs[0], "/tmp") && c.strict
          && c.cmd_index == 4, "-w and --strict");

    char *a3[] = {"x", "--", "-weird"};
    CHECK(parse_args(3, a3, &c) == PARSE_OK && c.cmd_index == 2 && c.n_rw == 0,
          "-- stops parsing");

    char *a3b[] = {"x", "--strict", "--", "-w"};
    CHECK(parse_args(4, a3b, &c) == PARSE_OK && c.cmd_index == 3
          && c.strict && c.n_rw == 0, "-- prevents trailing -w from parsing");

    char *a4[] = {"x", "--help"};
    CHECK(parse_args(2, a4, &c) == PARSE_HELP, "--help");
    char *a5[] = {"x", "-h"};
    CHECK(parse_args(2, a5, &c) == PARSE_HELP, "-h");

    char *a6[] = {"x", "-x"};
    CHECK(parse_args(2, a6, &c) == PARSE_ERR_OPT, "unknown option");
    char *a7[] = {"x", "-w"};
    CHECK(parse_args(2, a7, &c) == PARSE_ERR_WARG, "-w without arg");
    char *a8[] = {"x"};
    CHECK(parse_args(1, a8, &c) == PARSE_ERR_NOCMD, "no command");
    char *a9[] = {"x", "cmd"};
    CHECK(parse_args(2, a9, &c) == PARSE_OK && c.cmd_index == 1
          && c.n_rw == 0 && !c.strict, "parse resets cfg after prior error");

    /* -w 数量上限：65 个应报 WCOUNT */
    char *wa[131 + 1]; wa[0] = "x";
    for (int k = 0; k < 130; k += 2) { wa[k+1] = "-w"; wa[k+2] = "/d"; }
    CHECK(parse_args(131, wa, &c) == PARSE_ERR_WCOUNT, "too many -w");

    /* "-" 单独不是选项，应作为命令名 */
    char *a10[] = {"x", "-", "arg"};
    CHECK(parse_args(3, a10, &c) == PARSE_OK && c.cmd_index == 1, "bare dash is command");
}

static void test_rule_tables(void) {
    /* extra_rules：路径以 / 开头，权限不含目录类操作，表内无重复 */
    size_t ne = sizeof(extra_rules)/sizeof(extra_rules[0]);
    for (size_t k = 0; k < ne; k++) {
        const struct rule_def *r = &extra_rules[k];
        char name[64]; snprintf(name, sizeof(name), "extra_rules[%zu] sane", k);
        CHECKF(r->path[0] == '/' && r->access != 0
               && !(r->access & LANDLOCK_ACCESS_FS_MAKE_DIR)
               && !(r->access & LANDLOCK_ACCESS_FS_REMOVE_FILE),
               name, "path=%s", r->path);
        for (size_t j = 0; j < k; j++) {
            char dname[64];
            snprintf(dname, sizeof(dname), "extra_rules[%zu] no duplicate", k);
            CHECKF(strcmp(r->path, extra_rules[j].path) != 0,
                   dname, "dup of [%zu]: %s", j, r->path);
        }
    }
    /* default_rw_dirs：路径以 / 或 ~ 开头，表内无重复
     *（/tmp 同时存在于 extra_rules 是有意设计：strict 下仅保留
     *  设备/已存在文件的读写，不属于重复） */
    size_t nr = sizeof(default_rw_dirs)/sizeof(default_rw_dirs[0]);
    for (size_t k = 0; k < nr; k++) {
        const char *p = default_rw_dirs[k];
        char name[64]; snprintf(name, sizeof(name), "default_rw_dirs[%zu] sane", k);
        CHECKF(p[0] == '/' || p[0] == '~', name, "path=%s", p);
        for (size_t j = 0; j < k; j++) {
            char dname[64];
            snprintf(dname, sizeof(dname), "default_rw_dirs[%zu] no duplicate", k);
            CHECKF(strcmp(p, default_rw_dirs[j]) != 0,
                   dname, "dup of [%zu]: %s", j, p);
        }
    }
    CHECK(nr > 0, "rw table non-empty");
}

int main(void) {
    keep_prod_symbols();
    test_expand_home();
    test_parse_args();
    test_rule_tables();
    printf("1..%d\n", total); /* TAP 延迟计划：总数由 CHECK 累计 */
    printf("# %d tests, %d failures\n", total, failures);
    return failures ? 1 : 0;
}
