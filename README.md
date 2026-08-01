# MCP Server

`mcp_server` 是独立运行的 MCP 后端服务。给 Codex 使用时，需要配套
`mcp_stdio_proxy_adapter`：Codex 通过 stdio 启动 adapter，adapter 再通过 TCP
连接到正在运行的 `mcp_server`。

典型链路：

```text
Codex <stdio> mcp_stdio_proxy_adapter <tcp> mcp_server
```

## 目录

- `src/`：服务端源码。
- `include/`：公开头文件。
- `config/`：构建配置，包含 Linux 风格 `defconfig` 和平台 CMake 初始配置。
- `external/`：锁定的第三方源码，默认使用 bundled libuv、jansson 和 Mbed TLS。
- `mcp_stdio_proxy_adapter/`：由本仓库 gitlink 锁定的 stdio/TCP 适配器子仓库。
- `tests/`：smoke tests。

## 初始化依赖

首次构建前先初始化子模块：

```bash
git submodule update --init --recursive
```

Linux 也可以使用：

```bash
./scripts/bootstrap.sh
```

Windows PowerShell：

```powershell
.\scripts\bootstrap.ps1
```

Mbed TLS 的 Git 标签需要在构建时生成部分源码。Linux 构建环境若尚未安装生成器依赖，
先在仓库根目录执行：

```bash
python3 -m pip install -r external/mbedtls/scripts/basic.requirements.txt
```

## 编译

### Linux 风格 defconfig

`mcp_server` 支持类似 Linux 内核的 `CONFIG_*` 配置文件：

```config
CONFIG_MCP_FILE_TRANSFER_PLUGIN=y
CONFIG_MCP_FILE_TRANSFER_PLUGIN=m
# CONFIG_MCP_FILE_TRANSFER_PLUGIN is not set
```

其中 `y` 表示编进 `mcp_server`，`m` 表示生成可通过
`plugin_tools.insmod` 加载的动态插件，未设置表示不编译该插件。默认配置为
`m`：

`.config` 只有在显式传入 `-DMCP_KCONFIG_CONFIG=...` 时才参与 CMake 配置；
使用 `-C config/*.cmake` 时，以对应 `.cmake` 文件中的 `MCP_*` 设置为准。

对 `server.send` / `server.recv` 还需要额外确认“运行中的服务”真的启用了
MFT1 文件传输插件。若构建结果是 `MCP_FILE_TRANSFER_PLUGIN=m`，则仅生成
`mcp_file_transfer_plugin.so` 并不会自动让服务具备文件传输能力；启动后的
`mcp_server` 还需要显式执行 `plugin_tools.insmod`。否则运行态工具列表里不会
出现 `server.send` / `server.recv`，对端也不会协商 `whole-file`
capability，调用时会报 `MFT1 whole-file capability is not negotiated`。

```bash
cd mcp_server
make defconfig
make build
```

等价 CMake 入口：

```bash
cmake -S mcp_server -B mcp_server/build -DMCP_KCONFIG_CONFIG=mcp_server/config/defconfig -DCMAKE_BUILD_TYPE=Release
cmake --build mcp_server/build --parallel
```

`Kconfig` 中 `MCP_SERVER_CORE`、`MCP_PLUGIN_MANAGER_CORE` 和
`MCP_TOOL_SERVER_DISCOVERY` 对应必须内建的核心功能，显示为 `CONFIG_*=y`，
语义上类似 Linux 内核里被 `select` 拉起、不能模块化的核心项。

### Linux

在仓库根目录执行：

```bash
cmake -S mcp_server -B mcp_server/build -C mcp_server/config/linux_defconfig.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build mcp_server/build --parallel

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

```

产物路径通常是：

```text
mcp_server/build/src/mcp_server
mcp_server/build/mcp_stdio_proxy_adapter/mcp_stdio_proxy_adapter
```

### AArch64 交叉编译

该入口以下面的 `aarch64-none-linux-gnu` 工具链为例，目标系统仍按 Linux
构建，第三方库默认使用 `external/` 中锁定的 bundled 源码一起交叉编译。
交叉编译产物不能在宿主机直接运行 smoke tests，因此 ARM 默认配置关闭
`MCP_BUILD_TESTS`。
当前复用的 toolchain 文件名仍保留旧的 Buildroot triplet，但配置时会读取
`CROSS_COMPILE`，并在 `MCP_ARM_BUILDROOT_SDK/bin` 下查找对应编译器。

在仓库根目录执行：

```bash
export ARCH=arm
export CROSS_COMPILE=aarch64-none-linux-gnu
export MCP_ARM_BUILDROOT_SDK=/opt/buildtools/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu
export PATH="$PATH:$MCP_ARM_BUILDROOT_SDK/bin"

repo_root=$(pwd)
cmake -S mcp_server \
  -B mcp_server/build-arm \
  -C "$repo_root/mcp_server/config/arm_buildroot_defconfig.cmake" \
  -DCMAKE_TOOLCHAIN_FILE="$repo_root/mcp_server/cmake/toolchains/arm-buildroot-linux-gnueabihf.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build mcp_server/build-arm --parallel
```

修改 `arm_buildroot_defconfig.cmake` 后，需要重新执行上面的 `cmake -S ... -B ...`
配置命令，再执行 `cmake --build ...`；构建输出会显示
`MCP file transfer plugin mode: y/m/n`，可用它确认实际生效的插件模式。

如果使用 Linux 风格 Kconfig/defconfig 配置，先生成并按需编辑 `.config`：

```bash
cd mcp_server
make defconfig
# 编辑 .config，例如 CONFIG_MCP_FILE_TRANSFER_PLUGIN=y/m/not set
cd ..
```

然后用同一套交叉工具链配置 CMake：

```bash
export ARCH=arm
export CROSS_COMPILE=aarch64-none-linux-gnu
export MCP_ARM_BUILDROOT_SDK=/opt/buildtools/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu
export PATH="$PATH:$MCP_ARM_BUILDROOT_SDK/bin"

repo_root=$(pwd)
cmake -S mcp_server \
  -B mcp_server/build-arm \
  -DMCP_KCONFIG_CONFIG="$repo_root/mcp_server/.config" \
  -DCMAKE_TOOLCHAIN_FILE="$repo_root/mcp_server/cmake/toolchains/arm-buildroot-linux-gnueabihf.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build mcp_server/build-arm --parallel
```

产物路径通常是：

```text
mcp_server/build-arm/src/mcp_server
```

可用下面命令确认产物架构：

```bash
file mcp_server/build-arm/src/mcp_server
```

### Windows

在仓库根目录使用 PowerShell 执行：

```powershell
cmake -S . -B build -C config\windows_defconfig.cmake
cmake --build build --config Release

cmake -S mcp_stdio_proxy_adapter -B mcp_stdio_proxy_adapter\build
cmake --build mcp_stdio_proxy_adapter\build --config Release
```

产物路径通常是：

```text
mcp_server\build\src\Release\mcp_server.exe
mcp_stdio_proxy_adapter\build\Release\mcp_stdio_proxy_adapter.exe
```

## 运行 MCP Server

`system.shell_exec` 始终注册。server 启动时优先读取 `MCP_SHELL_EXEC_CONFIG` 显式指定的
JSON；未指定时依次尝试编译期源码路径和安装路径，均不可用才使用 hard profile。
是否启用 runtime control 不影响已加载策略的执行，`shell_enabled` 的 hard default
为 `true`。

### Runtime Sandbox Control

`system.sandbox_ctl` 在 `tools/list` 中始终可见，但默认不可调用。生产配置示例位于
`config/tools/shell_exec.json`；启用 control 前必须把其中公开的示例 token 替换为随机
secret，并在启动 server 前设置：

```text
MCP_SHELL_EXEC_CONFIG=<absolute path to shell_exec.json>
MCP_ENABLE_SANDBOX_CTL=1
```

bearer token 只从 JSON v2 的 `control.token` 读取，不再读取
`MCP_SANDBOX_CTL_TOKEN`。`MCP_ENABLE_SANDBOX_CTL` 只能是未设置、空、`0` 或 `1`；
设为 `1` 时，配置缺失、JSON/version 无效或 token 为空都会使 server 启动失败。
未设置、空或 `0` 时 control 不可调用，且未指定 JSON 也能使用 hard profile 启动；
一旦显式设置 `MCP_SHELL_EXEC_CONFIG`，配置错误仍会使启动失败。

#### 开关的作用

沙盒策略、运行时控制接口和 shell 执行是三个不同的开关：

| 开关 | 作用 | 关闭后的行为 |
| --- | --- | --- |
| `MCP_SHELL_EXEC_CONFIG=<absolute path>` | 启动时加载 JSON v2 策略；文件中的 `defaults` 立即约束新执行 | 未设置时尝试编译期/安装路径，仍不可用则使用 hard profile |
| `MCP_ENABLE_SANDBOX_CTL=1` | 允许用 `system.sandbox_ctl` 查询或修改当前进程的策略 | 未设置、空或 `0` 时控制接口不可调用，但已加载的 JSON 策略仍生效 |
| `defaults.shell_enabled` / runtime `overrides.shell_enabled` | 允许或拒绝 `system.shell_exec` 和 Unix `system.shell_start` 创建新执行 | `false` 才是停止接受新 shell 执行的开关 |
| runtime `sandbox_enabled` | 在 JSON 策略加 runtime overrides 与编译期 hard profile 之间切换 | `false` 使用 hard profile，通常更宽松；它不是“禁用 shell”或安全停机开关 |

`sandbox_enabled` 初始为 `true`。需要紧急停止命令执行时应把 `shell_enabled`
override 设为 `false`，不要把 `sandbox_enabled` 设为 `false`。

#### `shell_exec.json` 参数

JSON v2 使用严格结构：下面列出的对象和字段都必须存在，未知字段、重复 key、错误类型
或空 token 会使显式配置加载失败。`defaults` 是启动默认值，也是没有 runtime override
时的生效值。

| 路径 | 模板值 | 控制内容 |
| --- | --- | --- |
| `version` | `2` | 配置格式版本；当前只接受整数 `2` |
| `control.token` | `"123"` | `system.sandbox_ctl` bearer token，长度为 1..4096 bytes；模板值仅供示例，部署前必须替换 |
| `defaults.shell_enabled` | `true` | 是否允许创建新的同步或异步 shell 执行 |
| `defaults.command_length` | `65536` | `command` 的最大 UTF-8 byte 数，不含结尾 NUL |
| `defaults.timeout_ms` | `300000` | 默认执行超时，也是请求参数 `timeout_ms` 当前允许的最大值 |
| `defaults.output_bytes` | `65536` | stdout、stderr 各自最多保留的 byte 数；合流时只使用 stdout 限额，超出会标记 truncated |
| `defaults.once_read_stdout_err_chunk_size` | `1024` | 每次从 stdout/stderr pipe 读取的最大 byte 数；只控制内部读取块，不出现在 shell job 响应中 |
| `defaults.capture_stderr` | `true` | 是否捕获 stderr 并返回；为 `false` 时子进程 stderr 使用 server 的 stderr handle |
| `defaults.merge_stderr_to_stdout` | `false` | 捕获 stderr 时是否把它合并进 stdout；`capture_stderr=false` 时该值强制按 `false` 生效 |

`defaults.execution` 控制进程的启动方式、工作目录和环境：

| 路径 | 模板值 | 控制内容 |
| --- | --- | --- |
| `defaults.execution.mode` | `"shell"` | `shell` 通过 `shell_path + shell_arg` 执行命令；`exec` 不经过 shell，Unix 下当前把 `command` 整体作为可执行文件路径且不拆分 argv |
| `defaults.execution.shell_path` | `"/bin/sh"` | `mode=shell` 时启动的解释器；Windows 部署通常应改为 `cmd.exe` 或其绝对路径 |
| `defaults.execution.shell_arg` | `"-c"` | 放在命令前传给解释器的单个参数；Windows `cmd.exe` 通常使用 `/C`，允许空字符串 |
| `defaults.execution.working_directory` | `"/tmp/mcp-shell"` | 默认工作目录；不存在时只创建该目录本身，不递归创建父目录 |
| `defaults.execution.inherit_env` | `false` | 是否继承 server 启动时捕获的环境快照 |
| `defaults.execution.request_cwd_allowed` | `true` | 是否允许单次 `shell_exec`/`shell_start` 用 `cwd` 覆盖默认工作目录 |
| `defaults.execution.request_env_allowed` | `true` | 是否允许单次执行用 `env` 增补或覆盖环境变量 |
| `defaults.execution.kill_process_group_on_timeout` | `true` | Unix 超时时是否终止整个新进程组；Windows 当前报告 `unsupported` |
| `defaults.execution.run_as_user` | `""` | Unix 执行前切换到的用户名；空字符串表示不切换，能否切换取决于 server 启动账号权限 |
| `defaults.execution.run_as_group` | `""` | Unix 执行前切换到的组名；空字符串表示不切换 |
| `defaults.execution.env` | `PATH`/`HOME`/`LANG` | 固定环境变量层；覆盖继承环境中的同名项，之后还可由请求 `env` 覆盖 |

`defaults.limits` 在 Unix 上对应子进程的 rlimit。数值 `0` 是实际限制值，不表示
“无限制”；如需放宽应在 hard max 范围内填写明确值。Windows 当前不支持这五项限制。

| 路径 | 模板值 | Unix 控制内容 |
| --- | --- | --- |
| `defaults.limits.cpu_seconds` | `3600` | `RLIMIT_CPU`，CPU 时间秒数 |
| `defaults.limits.memory_bytes` | `2147483648` | `RLIMIT_AS`，进程虚拟地址空间 byte 数 |
| `defaults.limits.file_size_bytes` | `2147483648` | `RLIMIT_FSIZE`，可创建文件的最大 byte 数 |
| `defaults.limits.open_files` | `64` | `RLIMIT_NOFILE`，打开文件描述符数量 |
| `defaults.limits.processes` | `16` | `RLIMIT_NPROC`，目标用户可创建的进程数量 |
| `defaults.isolation.require_non_root` | `false` | Unix 在身份切换后拒绝仍以 root 执行；Windows 状态报告为 `reject_only` |

`bounds` 限定 JSON default 和 runtime override 可取的范围；超出编译期 hard 范围、
边界顺序错误或 default 不在边界内时，该字段回退到 hard default/bounds，并在
`system.sandbox_ctl get` 的 `diagnostics` 中报告原因。

| 路径 | 控制内容 |
| --- | --- |
| `bounds.command_length`、`bounds.timeout_ms`、`bounds.output_bytes`、`bounds.once_read_stdout_err_chunk_size` | 对应数值字段的 `min`/`max`；hard 范围依次为 1..65536、1..3600000、0..1048576、64..65536 |
| `bounds.execution.mode.allowed` | runtime 可选择的执行模式集合，只接受 `shell` 和/或 `exec`，且必须包含 default |
| `bounds.execution.shell_path`、`bounds.execution.shell_arg`、`bounds.execution.working_directory` | 对应 UTF-8 字符串的 `min_bytes`/`max_bytes`；`working_directory` 的范围也约束请求 `cwd` |
| `bounds.execution.allowed_env` | 最终环境的 `max_items` 与每个 `NAME=VALUE` 的 `item_max_bytes`；名称虽为 `allowed_env`，不是变量名 allowlist |
| `bounds.execution.run_as_user`、`bounds.execution.run_as_group` | 身份字符串的 `min_bytes`，可选 `max_bytes`；未写 `max_bytes` 时 hard max 为 255 |
| `bounds.limits.*` | 对应 rlimit 字段的 `min`/`max`，runtime 更新只能落在该范围内 |

配置文件包含明文 bearer token。Unix 上应由运行 server 的账号持有并设置为 `0600`；
Windows 上应移除继承权限，只给运行服务的账号授予读取权限。不要把示例 token 用于
生产，也不要把 token 放入命令行、日志或环境变量。token 只适合可信 stdio/pipe、
loopback，或已由外部加密隧道保护的连接；它不是传输加密，不能在其他主体可访问的
明文 TCP listener 上发送。

控制状态只存在于当前 server 进程内，不写回 JSON 配置；重启后 revision 恢复为
`0`，runtime overrides 清空。JSON defaults、bounds 和 token 也只在启动时读取，
修改文件后必须重启。单个 JSON bound 超过编译期 hard max 时，仅该字段回退到 hard
default/bounds，`get` 会返回字段级 diagnostic；其他合法字段继续生效。
`sandbox_enabled=false` 会使用 hard profile，但保留 overrides，重新开启后恢复；它
不是强隔离开关，也不会提升 mcp_server 的 OS 权限。

启动时同时指定配置和 control gate。Linux 示例：

```bash
MCP_SHELL_EXEC_CONFIG="$PWD/config/tools/shell_exec.json" \
MCP_ENABLE_SANDBOX_CTL=1 \
./build/src/mcp_server
```

Windows PowerShell 示例：

```powershell
$env:MCP_SHELL_EXEC_CONFIG = (Resolve-Path "config\tools\shell_exec.json").Path
$env:MCP_ENABLE_SANDBOX_CTL = "1"
.\build\src\Release\mcp_server.exe
```

使用 control 时，先读取状态，再使用返回的 `revision` 执行 compare-and-swap 更新。
以下 JSON 都是 `system.sandbox_ctl` 的 arguments。读取状态：

```json
{"action":"get","token":"<token>"}
```

停止接受新命令：

```json
{"action":"update","token":"<token>","expected_revision":0,"overrides":{"shell_enabled":false}}
```

重新允许命令，并把默认/最大超时改为 1000 ms：

```json
{"action":"update","token":"<token>","expected_revision":1,"overrides":{"shell_enabled":true,"timeout_ms":1000}}
```

仅在明确需要绕过 JSON 策略时切到 hard profile；overrides 会保留但暂不生效：

```json
{"action":"update","token":"<token>","expected_revision":2,"sandbox_enabled":false}
```

重新启用 JSON 策略和已保存的 overrides：

```json
{"action":"update","token":"<token>","expected_revision":3,"sandbox_enabled":true}
```

清空全部 overrides、把 `sandbox_enabled` 恢复为 `true`，并回到 JSON defaults：

```json
{"action":"reset","token":"<token>","expected_revision":4}
```

单次执行可在策略允许的范围内传入 `timeout_ms`、`cwd` 和 `env`：

```json
{"command":"pwd","timeout_ms":1000,"cwd":"/tmp/mcp-shell","env":{"LANG":"C"}}
```

以上对象是 `system.shell_exec` 的 arguments；Windows 命令和路径应改为目标平台语法。
runtime update 中把某个 override 设为 `null` 可只删除该 override，恢复对应 JSON default。

更新和 reset 只影响之后的新 `system.shell_exec` 或 Unix `system.shell_start` 执行，
不会终止既有 job。Unix 支持可表示的 RLIMIT_CPU/AS/FSIZE/NOFILE/NPROC、身份切换和
进程组超时清理；身份切换仍受启动账号权限约束。Windows 使用完整 Unicode environment
block，但五项 rlimit 与非空身份切换不支持，`require_non_root` 是 reject-only，
`system.shell_start` 未实现。状态接口会如实报告 `enforced`、`unsupported`、
`reject_only` 或 `ignored`。

## 网络访问白名单

`mcp_server` 启动时优先读取 `MCP_NETWORK_ACCESS_CONFIG` 显式指定的 JSON；未指定时
依次尝试编译期源码路径和安装路径，均不存在时使用 `enabled=false` 的 hard profile。
生产模板位于 `config/network/network_access.json`：

```json
{
  "version": 1,
  "enabled": true,
  "allowlist": {
    "ips": ["127.0.0.1", "192.168.16.136", "192.168.16.137"]
  },
  "discovery": {
    "peers": [
      {"ip": "192.168.16.136", "discovery_port": 18767},
      {"ip": "192.168.16.137", "discovery_port": 18767}
    ]
  }
}
```

JSON v1 使用严格结构：所有字段必须存在，未知字段、重复 key、错误类型、非法或重复
IPv4 地址都会使启动失败。`enabled=true` 时 `allowlist.ips` 不得为空；
`discovery.peers[].ip` 必须同时位于白名单中，`discovery_port` 范围为 `1..65535`。
第一版只接受精确 IPv4，不接受 CIDR、主机名或 IPv6。

策略启用后，TCP、发现 UDP 和编译启用的普通 UDP transport 都按来源 IP 过滤；发现包
中的声明地址和最终连接目标也必须位于白名单中。广播发现停止，启动、定时、
`server.list_servers` 和离线通知只向 `discovery.peers` 单播。stdio 和 pipe 不受影响。
需要本机 TCP adapter 时必须显式加入 `127.0.0.1`。策略只在启动时读取，修改后需重启。

显式配置缺失或任何已发现配置无效都会阻止启动，避免安全配置错误后静默开放。
策略启用时不得再设置 `MCP_DISCOVERY_HOSTS`；`enabled=false` 时保持原有广播和网络访问
行为。该策略提供基于来源 IP 的进程内访问控制，不替代应用层身份认证或主机防火墙；
对存在 IP 欺骗、ARP 劫持等能力的同网段攻击者，应同时使用防火墙和隔离网络。

## 部署与开机自启

### 部署约定

构建目录只用于生成候选产物，长期运行的服务应指向独立且稳定的部署目录。
一次部署至少包含目标平台的 `mcp_server` 及与该版本匹配的
`config/tools/shell_exec.json`、`config/network/network_access.json`，并遵循下面的顺序：

1. 记录 Git commit 和工作区状态，在目标平台完成构建与测试。
2. 确认目标主机架构；交叉编译 ABI 不确定时，优先在目标机临时目录原生构建。
3. 传输后核对文件大小和 SHA-256，不以传输调用返回成功代替完整性检查。
4. 在覆盖前备份当前二进制和配置；Windows 必须先停止服务以释放可执行文件锁，
   Linux 可先写入同目录临时文件再原子重命名。
5. 重启后同时验证服务状态、实际进程路径、部署文件哈希和 MCP 工具面；任一失败都
   使用同一部署批次的备份回退。

仓库中的 `config/tools/shell_exec.json` 是带公开示例 token 的基线模板。若启用
`MCP_ENABLE_SANDBOX_CTL=1`，应先在仓库外生成经批准的部署配置并替换 token；此时
目标端哈希应与该部署配置比较，而不是与仓库模板比较。不要只更新二进制而遗漏配置
结构或 policy 变更。

### Linux

临时运行时，把 `MCP_TCP_HOST` 改成客户端可访问的目标机地址：

```bash
cd /path/to/mcp_server
MCP_ENABLE_STDIO=0 \
MCP_ENABLE_TCP=1 \
MCP_TCP_HOST=192.168.16.136 \
MCP_TCP_PORT=18767 \
MCP_TLS_CA_FILE=/etc/mcp/tls/ca.cert.pem \
MCP_TLS_CERT_FILE=/etc/mcp/tls/node.cert.pem \
MCP_TLS_KEY_FILE=/etc/mcp/tls/node.key.pem \
MCP_SHELL_EXEC_CONFIG="$PWD/config/tools/shell_exec.json" \
./build/src/mcp_server
```

已有 systemd 服务的升级示例。`APPROVED_CONFIG` 可以指向仓库模板，也可以指向保存在
仓库外、已替换 token 的部署配置：

```bash
cd /path/to/mcp_server
cmake -S . -B build -C config/linux_defconfig.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
(cd build && ctest --output-on-failure)

DEPLOY_ID="$(git rev-parse --short=12 HEAD)-$(date +%Y%m%d-%H%M%S)"
BUILD_BIN="$PWD/build/src/mcp_server"
APPROVED_CONFIG="$PWD/config/tools/shell_exec.json"
TARGET_ROOT=/opt/mcp_server
TARGET_BIN="$TARGET_ROOT/mcp_server"
TARGET_CONFIG="$TARGET_ROOT/config/tools/shell_exec.json"

file "$BUILD_BIN"
sha256sum "$BUILD_BIN" "$APPROVED_CONFIG"
sudo install -d -m 0755 "$TARGET_ROOT/config/tools"
if sudo test -f "$TARGET_BIN"; then
  sudo cp -p "$TARGET_BIN" "$TARGET_BIN.bak-$DEPLOY_ID"
fi
if sudo test -f "$TARGET_CONFIG"; then
  sudo cp -p "$TARGET_CONFIG" "$TARGET_CONFIG.bak-$DEPLOY_ID"
fi
sudo install -m 0755 "$BUILD_BIN" "$TARGET_BIN.new-$DEPLOY_ID"
sudo install -m 0600 "$APPROVED_CONFIG" "$TARGET_CONFIG.new-$DEPLOY_ID"
sha256sum "$BUILD_BIN" "$APPROVED_CONFIG"
sudo sha256sum "$TARGET_BIN.new-$DEPLOY_ID" "$TARGET_CONFIG.new-$DEPLOY_ID"
sudo mv "$TARGET_BIN.new-$DEPLOY_ID" "$TARGET_BIN"
sudo mv "$TARGET_CONFIG.new-$DEPLOY_ID" "$TARGET_CONFIG"
sudo systemctl restart mcp_server.service
```

首次部署时执行到两个 `mv`，跳过尚不存在的服务重启，再创建下面的 unit 并执行
`enable --now`。

创建 `/etc/systemd/system/mcp_server.service`。仅在配置文件中的示例 token 已被替换后，
才取消 `MCP_ENABLE_SANDBOX_CTL` 行的注释：

```systemd
[Unit]
Description=MCP Server
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
Environment=MCP_ENABLE_STDIO=0
Environment=MCP_ENABLE_TCP=1
Environment=MCP_TCP_HOST=192.168.16.136
Environment=MCP_TCP_PORT=18767
Environment=MCP_TLS_CA_FILE=/etc/mcp/tls/ca.cert.pem
Environment=MCP_TLS_CERT_FILE=/etc/mcp/tls/node.cert.pem
Environment=MCP_TLS_KEY_FILE=/etc/mcp/tls/node.key.pem
Environment=MCP_SHELL_EXEC_CONFIG=/opt/mcp_server/config/tools/shell_exec.json
# Environment=MCP_ENABLE_SANDBOX_CTL=1
ExecStart=/opt/mcp_server/mcp_server
WorkingDirectory=/opt/mcp_server
Restart=always
RestartSec=3

# 如需降权运行，取消注释并改成实际账号，同时调整部署目录权限。
# User=alinx
# Group=alinx

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now mcp_server.service
sudo systemctl is-active mcp_server.service
sudo systemctl show mcp_server.service \
  -p MainPID -p ExecMainStartTimestamp -p ExecMainStatus --no-pager
sudo sha256sum /opt/mcp_server/mcp_server \
  /opt/mcp_server/config/tools/shell_exec.json
```

若升级后验证失败，使用同一个 `DEPLOY_ID` 回退，不要混用不同批次的二进制和配置：

```bash
sudo systemctl stop mcp_server.service
sudo cp -p "/opt/mcp_server/mcp_server.bak-$DEPLOY_ID" \
  /opt/mcp_server/mcp_server
sudo cp -p "/opt/mcp_server/config/tools/shell_exec.json.bak-$DEPLOY_ID" \
  /opt/mcp_server/config/tools/shell_exec.json
sudo systemctl start mcp_server.service
```

### Windows

临时运行示例：

```powershell
$env:MCP_ENABLE_STDIO = "0"
$env:MCP_ENABLE_TCP = "1"
$env:MCP_TCP_HOST = "192.168.16.2"
$env:MCP_TCP_PORT = "18767"
$env:MCP_SHELL_EXEC_CONFIG = (Resolve-Path "config\tools\shell_exec.json").Path
.\build\src\Release\mcp_server.exe
```

如果只允许本机访问，可以把 `MCP_TCP_HOST` 设为 `127.0.0.1`。已有 Windows 服务的
升级示例使用独立部署目录 `D:\tools\mcp_interconnect_system`：

```powershell
$Repo = "D:\Project\2025-12-02\mcp\mcp_server"
$TargetRoot = "D:\tools\mcp_interconnect_system"
$DeployId = "$(git -C $Repo rev-parse --short=12 HEAD)-$(Get-Date -Format yyyyMMdd-HHmmss)"
$SourceBin = Join-Path $Repo "build\src\Release\mcp_server.exe"
$ApprovedConfig = Join-Path $Repo "config\tools\shell_exec.json"
$TargetBin = Join-Path $TargetRoot "mcp_server.exe"
$TargetConfig = Join-Path $TargetRoot "config\tools\shell_exec.json"

cmake -S $Repo -B (Join-Path $Repo "build") -C (Join-Path $Repo "config\windows_defconfig.cmake")
cmake --build (Join-Path $Repo "build") --config Release --parallel
Push-Location (Join-Path $Repo "build")
ctest -C Release --output-on-failure
$TestExitCode = $LASTEXITCODE
Pop-Location
if ($TestExitCode -ne 0) { exit $TestExitCode }

New-Item -ItemType Directory -Force -Path (Split-Path $TargetConfig) | Out-Null
if (Test-Path $TargetBin) {
    Copy-Item $TargetBin "$TargetBin.bak-$DeployId"
}
if (Test-Path $TargetConfig) {
    Copy-Item $TargetConfig "$TargetConfig.bak-$DeployId"
}
Stop-Service mcp_server
Copy-Item $SourceBin $TargetBin -Force
Copy-Item $ApprovedConfig $TargetConfig -Force
Start-Service mcp_server

Get-Service mcp_server
Get-CimInstance Win32_Process -Filter "Name='mcp_server.exe'" |
    Select-Object ProcessId, ExecutablePath
Get-FileHash -Algorithm SHA256 $SourceBin, $TargetBin, $ApprovedConfig, $TargetConfig
Test-NetConnection -ComputerName 192.168.16.2 -Port 18767
```

首次部署时先创建目录并复制两个目标文件，跳过备份和 `Stop-Service`/`Start-Service`，
再按下面步骤创建 NSSM 服务。

创建部署目录下的 `start_mcp_server.bat`。使用 `%~dp0` 可以避免服务继续依赖源码或
构建目录：

```bat
@echo off
set MCP_ENABLE_STDIO=0
set MCP_ENABLE_TCP=1
set MCP_TCP_HOST=192.168.16.2
set MCP_TCP_PORT=18767
set "MCP_SHELL_EXEC_CONFIG=%~dp0config\tools\shell_exec.json"
rem 仅在 JSON token 已替换后启用：set MCP_ENABLE_SANDBOX_CTL=1

cd /d "%~dp0"
.\mcp_server.exe
```

下载 [NSSM](https://nssm.cc/release/nssm-2.24.zip) 并加入 `PATH`，然后使用 PowerShell
创建自动启动服务：

```powershell
nssm install mcp_server "$env:SystemRoot\System32\cmd.exe" '/c "D:\tools\mcp_interconnect_system\start_mcp_server.bat"'
nssm set mcp_server AppDirectory "D:\tools\mcp_interconnect_system"
nssm set mcp_server Start SERVICE_AUTO_START
nssm set mcp_server AppExit Default Restart
nssm start mcp_server
Get-Service mcp_server
```

Windows 回退同样必须先停止服务以释放文件锁：

```powershell
Stop-Service mcp_server
Copy-Item "$TargetBin.bak-$DeployId" $TargetBin -Force
Copy-Item "$TargetConfig.bak-$DeployId" $TargetConfig -Force
Start-Service mcp_server
```

### 重启后的 MCP 验证

服务重启会关闭既有 TCP/MCP 连接。`server_id` 是会话内临时标识，远端服务重启后应
重新执行 `server.list_servers`，并用 `address`、`system_status.hostname` 和
`system_status.machine` 锁定目标，再调用 `system.get_status` 或
`registry.list_tools`。不要把旧连接上的 `Transport closed` 单独判定为部署失败。

在 MCP 客户端重新连接后调用 `tools/list`，至少确认 `system.ping`、
`system.get_status`、`system.shell_exec` 和 `registry.list_tools`。若构建输出显示
`MCP_FILE_TRANSFER_PLUGIN=y`，还应确认 `server.send` 和 `server.recv`；若为 `m`，则需
先加载对应模块。最终以服务状态、目标文件哈希和重连后的工具调用共同作为部署成功
证据。

## 配置 Codex

Codex 侧配置的是 `mcp_stdio_proxy_adapter`，不是直接配置 `mcp_server`。
adapter 通常由 Codex 按 `config.toml` 自动启动，不需要单独常驻运行。
编辑 Codex 的 `config.toml`，加入：

```toml
[mcp_servers.mcp]
command = "D:\\Project\\2025-12-02\\mcp\\mcp_server\\build\\mcp_stdio_proxy_adapter\\Release\\mcp_stdio_proxy_adapter.exe"
args = [
  "--transport", "tcp",
  "--host", "192.168.16.3",
  "--port", "18767",
  "--tls-ca", "D:\\mcp-secrets\\ca.cert.pem",
  "--tls-cert", "D:\\mcp-secrets\\client.cert.pem",
  "--tls-key", "D:\\mcp-secrets\\client.key.pem",
  "--tls-server-name", "mcp-node.example.internal",
  "--timeout-ms", "3000"
]
```

Linux 路径示例：

```toml
[mcp_servers.mcp]
command = "/home/alinx/prj/mcp/mcp_server/build/mcp_stdio_proxy_adapter/mcp_stdio_proxy_adapter"
args = [
  "--transport", "tcp",
  "--host", "192.168.16.3",
  "--port", "18767",
  "--tls-ca", "/etc/mcp/tls/ca.cert.pem",
  "--tls-cert", "/etc/mcp/tls/client.cert.pem",
  "--tls-key", "/etc/mcp/tls/client.key.pem",
  "--tls-server-name", "mcp-node.example.internal",
  "--timeout-ms", "3000"
]
```

`--host` 和 `--port` 必须与 `mcp_server` 运行时的 `MCP_TCP_HOST`、
`MCP_TCP_PORT` 对应。`--tls-server-name` 必须存在于服务端证书 SAN；省略时使用
`--host`。TLS 文件和证书签发策略见 `docs/transport_security.md`。

## 在 Codex 中使用

在 Codex 对话里可以大致按下面形式调用；agent 会辅助修正语法，意思对即可。

查看服务状态：

```text
mcp.system.get_status({})
```

写一个 C 文件：

```text
mcp.system.shell_exec "cat > /home/alinx/prj/hello.c <<'EOF'
#include <stdio.h>
int main(void) {
    printf(\"hello world!\\n\");
    return 0;
}
EOF"
```

编译：

```text
mcp.system.shell_exec "gcc /home/alinx/prj/hello.c -o /home/alinx/prj/hello.out"
```

运行：

```text
mcp.system.shell_exec({"command":"/home/alinx/prj/hello.out","timeout_ms":1000})
```

长时间运行或阻塞命令使用异步 job 工具：

```text
system.shell_start -> system.shell_poll/system.shell_tail/system.shell_wait/system.shell_kill/system.shell_list
```

也可以使用更短的单行写法：

```text
mcp.system.shell_exec "echo '#include <stdio.h>
int main(void){printf(\"hello world!\\n\");return 0;}' > /home/alinx/prj/hello.c"
```

## 常用环境变量

- `MCP_ENABLE_STDIO=0`：关闭 server 自身 stdio，避免和 TCP 模式混用。
- `MCP_ENABLE_TCP=1`：开启 TCP framed listener。
- `MCP_TCP_HOST=<ip>`：TCP 监听地址。
- `MCP_TCP_PORT=<port>`：TCP 监听端口，示例使用 `18767`。
- `MCP_TCP_LISTEN_PORT=<port>`：`MCP_TCP_PORT` 未设置时的兼容别名。
- `MCP_TLS_CA_FILE=<path>`：TCP 模式必填，信任的内部 CA 证书链。
- `MCP_TLS_CERT_FILE=<path>`：TCP 模式必填，本节点证书。
- `MCP_TLS_KEY_FILE=<path>`：TCP 模式必填，本节点私钥；应由文件 ACL 限制读取。
- `MCP_ENABLE_DISCOVERY=1`：TCP 模式下开启 mcp_server 局域网发现；默认开启。
- `MCP_DISCOVERY_PORT=<port>`：UDP 发现监听端口；默认等于 `MCP_TCP_PORT`。
- `MCP_UDP_BROADCAST_LISTEN_PORT=<port>`：UDP 广播目标端口；默认等于 `MCP_TCP_PORT`。
- `MCP_DISCOVERY_BIND_HOST=<ip>`：UDP 发现监听地址；默认 `0.0.0.0`。
- `MCP_DISCOVERY_ADVERTISE_HOST=<ip>`：广播中声明给对端连接的地址；默认使用 UDP 来源地址；该 IP 必须存在于本节点证书 SAN。
- `MCP_DISCOVERY_HOSTS=<ip[:port],...>`：额外单播发现目标，适合测试或禁止广播的网络。
- `MCP_NETWORK_ACCESS_CONFIG=<absolute path>`：启动时加载 JSON v1 网络访问白名单；启用后禁止同时设置 `MCP_DISCOVERY_HOSTS`。
- `MCP_DISCOVERY_PROXY_TIMEOUT_MS=<ms>`：`gateway.proxy_tool` 未显式传入 `proxy_timeout_ms` 时的代理等待超时；默认 `5000`。
- `MCP_SHELL_EXEC_CONFIG=<absolute path>`：显式指定 JSON v2 shell policy；修改后需重启。
- `MCP_ENABLE_SANDBOX_CTL=1`：要求有效 JSON v2 和非空 `control.token`，并启用 process-local control。

TCP 仅接受 TLS 1.3 双向认证连接，不提供同端口明文回退。TLS 内仍使用 4 字节大端
长度头加 JSON/MFT1 body；`mcp_stdio_proxy_adapter` 负责把 Codex stdio JSON-RPC
转换成该加密 framed 协议。

`server.list_servers` 工具会主动触发一次发现广播，短暂等待响应后返回 JSON
文本，结构包含 `total` 和 `servers`；每个 server 记录会话内临时 `server_id`、
`address`、`port`、连接状态和精简的 `system_status`。IP 从 `address` 提取，当前紧凑
结果没有独立的顶层 `ip` 字段。
