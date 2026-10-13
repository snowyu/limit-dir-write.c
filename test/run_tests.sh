#!/bin/sh
# limit-dir 集成测试：真实运行沙箱，验证文件系统行为
#
# 原则：
#   1. 所有文件系统状态都在 mktemp -d 世界（$D）里构造，
#      不触碰真实 HOME、会话目录（/run/user/$UID）或源码仓库；
#   2. 依赖家目录的行为（~ 展开、默认可写表）通过覆盖 HOME /
#      XDG_RUNTIME_DIR 环境变量在 fake 目录下验证；
#   3. 任何中途改变 cwd 的脚本，路径型变量必须在开头一次性绝对化。
set -u
BIN="${1:-./limit-dir}"

# 在任何 cd 之前把 BIN 解析为绝对路径（防 cd 后相对路径失效）
case "$BIN" in
    /*) ;;                                    # 已是绝对路径
    */*) BIN="$PWD/${BIN#./}" ;;              # ./limit-dir 或 build/limit-dir
    *)   BIN="$(command -v "$BIN")" ;;        # 从 PATH 查找
esac
[ -x "$BIN" ] || { echo "Bail out! $BIN not found/not executable"; exit 2; }

pass=0; fail=0

ok()   { pass=$((pass+1)); echo "ok $pass - $1"; }
nok()  { fail=$((fail+1)); echo "not ok $pass - $1"; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else nok "$1 (want=$2 got=$3)"; fi; }

# ---- CLI 行为（不写文件，任意 cwd 下运行即可）----
"$BIN" --help >/dev/null 2>&1;                    check "--help exit 0"    0 $?
"$BIN" -h       >/dev/null 2>&1;                  check "-h exit 0"        0 $?
"$BIN"          >/dev/null 2>&1;                  check "no cmd exit 1"    1 $?
"$BIN" -x foo   >/dev/null 2>&1;                  check "bad opt exit 1"   1 $?
"$BIN" -w       >/dev/null 2>&1;                  check "-w noarg exit 1"  1 $?
"$BIN" true     >/dev/null 2>&1;                  check "exit code pass"   0 $?
"$BIN" false    >/dev/null 2>&1;                  check "exit code fail"   1 $?
"$BIN" sh -c 'exit 42' >/dev/null 2>&1;           check "exit code 42"     42 $?

# ---- 测试世界 ----
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
# 有意为之：以下所有相对路径（含"当前目录可写"测试）都基于 $D。
# limit-dir 放行的是其启动时的 cwd，故测试脚本在此 cd，
# 让被测进程以 $D 为 cwd 启动。BIN 已绝对化，cd 不影响其可用性。
cd "$D" || exit 1

# fake HOME / fake XDG_RUNTIME_DIR：验证 ~ 展开与运行时目录逻辑，
# 不碰真实家目录和 systemd 会话目录（后者含真实 agent socket）
FAKEHOME="$D/fakehome"
FAKERUN="$D/fakerun"
mkdir -p "$FAKEHOME" "$FAKERUN"

# ---- 文件系统行为 ----
( "$BIN" sh -c 'echo ok > f.txt' ) && [ -f f.txt ] \
    && ok "cwd writable" || nok "cwd writable"
"$BIN" sh -c "echo bad > $D-outside.txt" 2>/dev/null && nok "outside writable" \
    || ok "outside writable"
"$BIN" sh -c "cat /etc/hostname > f2.txt" && [ -s f2.txt ] \
    && ok "outside readable" || nok "outside readable"
"$BIN" sh -c 'echo x > /dev/null' \
    && ok "/dev/null writable" || nok "/dev/null writable"
"$BIN" sh -c 'head -c4 /dev/urandom > r.bin' && [ "$(wc -c < r.bin)" = 4 ] \
    && ok "/dev/urandom readable" || nok "/dev/urandom readable"
"$BIN" sh -c 'mkdir sub && touch sub/f && rm sub/f && rmdir sub' \
    && ok "mkdir/rm in cwd" || nok "mkdir/rm in cwd"

# ---- 限制不可逃逸（子进程继承）----
# 注意必须用 --strict：默认模式下 /tmp 在放行表内，写入会成功，
# 那样测不出"限制不可逃逸"。strict 下 /tmp 不可写，嵌套子进程
# 的写入必须失败——这验证 Landlock 限制随 fork/exec 继承。
"$BIN" --strict sh -c "sh -c 'echo bad > /tmp/ld-escape-test'" 2>/dev/null \
    && nok "nested escape blocked" || ok "nested escape blocked"
[ -e /tmp/ld-escape-test ] && nok "no leftover outside" || ok "no leftover outside"

# ---- --strict ----
"$BIN" --strict sh -c 'echo x > /tmp/ld-strict-test' 2>/dev/null \
    && nok "strict blocks /tmp" || ok "strict blocks /tmp"
"$BIN" --strict sh -c 'echo x > strict-ok.txt' \
    && ok "strict allows cwd" || nok "strict allows cwd"
HOME="$FAKEHOME" XDG_RUNTIME_DIR="$FAKERUN" \
    "$BIN" --strict sh -c "echo x > $FAKERUN/t" 2>/dev/null \
    && nok "strict blocks XDG_RUNTIME_DIR" || ok "strict blocks XDG_RUNTIME_DIR"

# ---- -w 与 ~ 展开（fake HOME 下验证）----
mkdir -p "$FAKEHOME/.ld-test-dir"
# 外层 -w 由 limit-dir 的 expand_home 展开，内层重定向路径由子 shell
# 展开：两者读到同一个 HOME，落点一致正是 -w ~/xxx 的核心契约
HOME="$FAKEHOME" "$BIN" -w '~/.ld-test-dir' \
    sh -c 'echo x > ~/.ld-test-dir/w.txt' \
    && [ -f "$FAKEHOME/.ld-test-dir/w.txt" ] \
    && ok "-w with ~ expands (inner/outer agree)" \
    || nok "-w with ~ expands (inner/outer agree)"

# 默认可写目录表中的 ~/.cache 应在 fake HOME 下生效
HOME="$FAKEHOME" "$BIN" sh -c 'echo x > ~/.cache/c.txt' \
    && [ -f "$FAKEHOME/.cache/c.txt" ] \
    && ok "default rw: ~/.cache under fake HOME" \
    || nok "default rw: ~/.cache under fake HOME"

# strict 应挡住 ~/.cache（默认放行对家目录同样受开关控制）
HOME="$FAKEHOME" "$BIN" --strict sh -c 'echo x > ~/.cache/s.txt' 2>/dev/null \
    && nok "strict blocks ~/.cache" || ok "strict blocks ~/.cache"

# ---- -w 目录不存在必须报错（路径在 $D 世界内，避免硬编码根路径）----
"$BIN" -w "$D/nonexistent-dir-xyz" true 2>/dev/null \
    && nok "missing -w dir errors" || ok "missing -w dir errors"

# ---- 命令名以 - 开头 ----
echo '#!/bin/sh
echo dash-cmd-ok' > "$D/-dashcmd"; chmod +x "$D/-dashcmd"
out=$("$BIN" -- "$D/-dashcmd" 2>&1)
[ "$out" = "dash-cmd-ok" ] && ok "-- allows dash command" || nok "-- allows dash command"

# 清理由 trap EXIT 兜底（覆盖 kill/中断场景），此处无需逐一 rm

echo
echo "# $pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
