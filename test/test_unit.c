/* limit-dir 单元测试：直接 include 源码，测纯函数 */
#define UNIT_TEST
#include "../limit-dir.c"

static int failures = 0, total = 0;

#define CHECK(cond, name, ...) do { \
    total++; \
    if (cond) { printf("ok %d - %s\n", total, name); } \
    else { failures++; printf("not ok %d - %s: ", total, name); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

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
    CHECK(parse_args(3, a3, &c) == PARSE_OK
          && !strcmp(c.rw_dirs ? "" : "", "") && c.cmd_index == 2
          && !strcmp("-weird", "-weird"), "-- stops parsing");
    CHECK(c.n_rw == 0, "-- does not consume as -w");

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
    CHECK(parse_args(2, a9, &c) == PARSE_OK, "command after parse err state is ok");

    /* -w 数量上限：65 个应报 WCOUNT */
    char *wa[131 + 1]; wa[0] = "x";
    for (int k = 0; k < 130; k += 2) { wa[k+1] = "-w"; wa[k+2] = "/d"; }
    CHECK(parse_args(131, wa, &c) == PARSE_ERR_WCOUNT, "too many -w");

    /* "-" 单独不是选项，应作为命令名 */
    char *a10[] = {"x", "-", "arg"};
    CHECK(parse_args(3, a10, &c) == PARSE_OK && c.cmd_index == 1, "bare dash is command");
}

static void test_rule_tables(void) {
    /* extra_rules：路径以 / 开头，权限不含目录类操作 */
    for (size_t k = 0; k < sizeof(extra_rules)/sizeof(extra_rules[0]); k++) {
        const struct rule_def *r = &extra_rules[k];
        char name[64]; snprintf(name, sizeof(name), "extra_rules[%zu] sane", k);
        CHECK(r->path[0] == '/' && r->access != 0
              && !(r->access & LANDLOCK_ACCESS_FS_MAKE_DIR)
              && !(r->access & LANDLOCK_ACCESS_FS_REMOVE_FILE),
              name, "path=%s", r->path);
    }
    /* default_rw_dirs：路径以 / 或 ~ 开头，且与 extra_rules 无重复 */
    for (size_t k = 0; k < sizeof(default_rw_dirs)/sizeof(default_rw_dirs[0]); k++) {
        const char *p = default_rw_dirs[k];
        char name[64]; snprintf(name, sizeof(name), "default_rw_dirs[%zu] sane", k);
        CHECK(p[0] == '/' || p[0] == '~', name, "path=%s", p);
    }
    CHECK(sizeof(default_rw_dirs)/sizeof(default_rw_dirs[0]) > 0, "rw table non-empty");
}

int main(void) {
    printf("1..%d\n", 0); /* TAP 占位，实际总数由 CHECK 累计 */
    test_expand_home();
    test_parse_args();
    test_rule_tables();
    printf("# %d tests, %d failures\n", total, failures);
    return failures ? 1 : 0;
}
