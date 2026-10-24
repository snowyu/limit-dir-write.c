# limit-dir

基于 Linux Landlock 的轻量级文件系统沙箱工具。
让任意程序**只能在指定目录内读写，文件系统其余部分一律只读**——
无需 root、无需容器、无需修改目标程序。

## 特性

- ✅ **零权限要求**：普通用户即可使用，不依赖 sudo / user namespace
- ✅ **无侵入**：以包装器方式运行任意命令，限制对程序透明
- ✅ **继承性**：被限制进程及其所有子进程（fork/exec）同样受限
- ✅ **细粒度**：读写、创建、删除、重命名等操作分别管控
- ✅ **单文件**：约 270 行 C，仅依赖 glibc 和内核头文件

## 环境要求
- Linux 内核 **≥ 5.13**（Landlock 引入版本；5.19+ 支持更完整）
- gcc / clang
- 3.12 以下内核请使用 [bubblewrap](https://github.com/containers/bubblewrap) 方案

检查内核是否支持：

```bash
# 输出 >= 1 表示支持
python3 -c 'print(open("/sys/kernel/security/lsm").read())' | grep -qw landlock \
    && echo 支持 || echo 不支持
```
## 编译与安装

```bash
make            # 编译
sudo make install   # 安装到 /usr/local/bin（可通过 PREFIX= 改路径）
```
自定义安装位置：
```bash
make install PREFIX=$HOME/.local
```
卸载：
```bash
sudo make uninstall
```
## 使用方法
```
limit-dir [-w DIR]... COMMAND [ARGS...]
```
| 参数 | 说明 |
|---|---|
| `-w DIR` | 额外开放 DIR 为可写目录，可重复指定（支持 `~` 展开） |
| `--strict` | 关闭默认可写目录，仅保留当前目录与 `-w` 目录 |
| `-h, --help` | 显示帮助 |
| `--` | 结束参数解析（命令名以 `-` 开头时使用） |

### 示例
```bash
# 1. app 只能读写当前目录，其它地方一律只读
limit-dir app
# 2. 额外开放两个可写目录
limit-dir -w /tmp -w ~/.cache app
# 3. 保护性地解压不可信压缩包
cd ~/extract && limit-dir tar xf /downloads/suspicious.tar.gz
# 4. 运行会疯狂写缓存的构建工具
limit-dir -w ~/.cargo -w /tmp cargo build
# 5. 命令名以 - 开头时
limit-dir -- ./-weird-tool arg1
```
被限制的程序尝试越权写入时，会收到标准的 `EACCES` / `EROFS` 错误
（表现为 "Permission denied" / "Read-only file system"），
多数程序能自然处理。

## 工作原理

程序利用 Linux 内核的 **Landlock** LSM（Linux Security Module）：

1. 创建一个 ruleset，声明要管控的文件系统权限集合；
2. 对 `/`（整个文件系统）添加规则：只放行"读文件、读目录"；
3. 对当前目录及 `-w` 指定的目录添加规则：放行全部权限；
4. 调用 `prctl(PR_SET_NO_NEW_PRIVS)` 并 `landlock_restrict_self`
   将规则集应用到当前进程；
5. `execvp` 启动目标程序——限制随进程树继承，无法逃离。
内核版本兼容性通过运行时探测 ABI 版本实现：
| 内核 ABI | 新增能力 | 处理方式 |
|---|---|---|
| 1 (5.13) | 基础读写/增删权限 | 必须支持 |
| 2 (5.19) | 跨目录移动/重命名 | 自动启用 |
| 3 (6.2) | 截断文件 | 自动启用 |

## 已知限制与设计取舍

### 设计取舍（有替代方案）

**默认放行的可写目录**

默认额外放行 `/tmp`、`~/.cache`、`~/.config`、`~/.local`、`~/.gnupg`、
`~/.npm`、`~/.bun` 及 `$XDG_RUNTIME_DIR` 的读写权限，以保证多数程序
（含 gpg 签名、agent socket 等）正常运行。代价与缓解：

- `~/.local/bin` 可写意味着程序可植入**沙箱外也会执行的代码**
  （它常位于 `PATH` 前部）——高安全场景请使用 `--strict`；
- `~/.gnupg` 默认可写以支持 gpg 签名（锁文件、trustdb 更新）；
  如需防止应用篡改密钥，使用仓库根目录的 `./limit-dir-gpg`，
  它在临时副本中运行 gpg，原件保持只读；
- `--strict` 关闭上述全部默认放行，仅保留当前目录与 `-w` 指定目录。

**沙箱不能阻止读取**

默认策略下整个文件系统（包括 `~/.ssh`、`~/.gnupg` 中的私钥）
对沙箱内进程**可读**。这不是疏漏：Landlock 无法表达“拒读某个
目录但放行特定子进程”——拒读会连合法使用者一并挡住。若需对
应用隐藏密钥等敏感数据，应结合专用代理进程（如隔离的
ssh-agent / gpg-agent）架构，不属于本工具范畴。

### Landlock 模型限制（内核机制层面）

以下能力受限于 Landlock 的设计模型，**在纯用户态、无特权的
前提下无法实现**，列出以说明设计边界：

| 期望能力 | 现状 | 原因 |
|---|---|---|
| "只允许修改自己创建的文件" | ❌ | Landlock 是纯路径 + 操作类型模型，无文件所有权（provenance）跟踪 |
| 目录可写但排除个别文件 | ❌ | 规则为**并集语义**，只能逐级放宽，不能做减法 |
| 运行时动态加规则（如跟踪新建文件） | ❌ | 无文件创建 hook；需 LSM BPF（要求 root 加载 BPF 程序） |
| 拒绝读取特定目录 | ❌ | 无法区分"进程自身"与"子进程"的读取需求 |
| 写时复制（CoW）覆盖层 | ❌ | 需 overlayfs + mount 权限，依赖 user namespace 或 root |

实现上述能力的对应技术路线：

- **LSM BPF**：挂 `file_open` / `path_mknod` 探针，用 map 记录
  inode 归属，可实现"创建者可写"等细粒度语义；代价是加载 BPF
  程序需要 root，且与内核版本强耦合；
- **bubblewrap + overlayfs**：把可写目录做成写时复制层，
  运行结束后 diff / 丢弃；代价是依赖非特权 user namespace
  （部分发行版默认禁用）。

两者都突破了本工具"无特权、单文件、零依赖"的定位，故未采用。
临时副本（如 `limit-dir-gpg`）是在该约束下最接近的替代。

### 其它限制

- **不限制网络**：Landlock 只管文件系统，进程仍可联网
  （如需隔离网络可配合 `unshare -n` 或 `bwrap --unshare-net`）
- **不限制内存、CPU、进程数**等资源（请使用 cgroups / systemd）
- 少数特殊文件系统（如某些 overlayfs 配置）可能不受 Landlock 管控
- Landlock 是**不可逆**的：限制生效后，进程自身无法解除
  （这正是安全性的来源，但也意味着 `-w` 必须在启动时指定）

## 常见问题

**Q: 为什么不用 chroot？**
A: chroot 需要 root，且只是改变根目录，不做权限管控；
Landlock 无需特权且是真正的访问控制。

**Q: 程序报 "Permission denied" 但我确实给了 -w？**
A: `-w` 支持 `~` 展开（与 shell 对 `~` 的展开结果一致）；若目标
目录是符号链接，建议指向 `realpath` 展开后的真实路径。
另外注意脚本解释器路径（如 `#!/usr/bin/env`）需要可执行权限，
当前规则已放行全盘执行权限。

**Q: 如何验证限制生效？**
A: `make test`，或手动：
```bash
mkdir /tmp/sandbox && cd /tmp/sandbox
limit-dir bash -c 'echo ok > here.txt; echo bad > /tmp/bad.txt'
# here.txt 创建成功，/tmp/bad.txt 报 Read-only file system
```
## 许可证

MIT License

---

几个使用说明：

1. **`make test`** = 单元测试（参数解析、`~` 展开、规则表校验）+ 集成测试（CLI 行为、沙箱读写边界、`--strict`、默认可写表、限制不可逃逸）。测试内容只写入 `/tmp` 下的临时世界，绝不写入真实 `$HOME`（家目录相关行为全部用 fake HOME 验证）；全部通过输出 `# N passed, 0 failed`，任一失败返回非零退出码，方便接入 CI；注意集成测试需要 Landlock 内核支持；
2. **`make install`** 安装 `limit-dir` 与 `limit-dir-gpg` 两个文件；**`install -Dm755`** 会自动创建目标目录，兼容 `DESTDIR=` 用于打包（如 `make install DESTDIR=pkg/`）；
3. **`rules.conf`** 是尚未实现的配置文件设计草案（当前版本不读取），详见该文件头部说明；

