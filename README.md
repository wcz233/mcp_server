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
- `external/`：锁定的第三方源码，默认使用 bundled libuv 和 jansson。
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
mcp_stdio_proxy_adapter/build/mcp_stdio_proxy_adapter
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

`system.shell_exec` 默认禁用。只有在可信环境中才设置 `MCP_ENABLE_SHELL_EXEC=1`。

### Linux

把 `MCP_TCP_HOST` 改成 Codex 所在机器能访问到的地址：

```bash
MCP_ENABLE_STDIO=0 \
MCP_ENABLE_TCP=1 \
MCP_TCP_HOST=192.168.222.128 \
MCP_TCP_PORT=18767 \
MCP_ENABLE_SHELL_EXEC=1 \
./mcp_server/build/src/mcp_server
```

开机自启脚本:

```shell
sudo vim /etc/systemd/system/mcp_server.service
```

```shell
[Unit]
Description=MCP Server
After=network.target

[Service]
Type=simple
Environment=MCP_ENABLE_STDIO=0
Environment=MCP_ENABLE_TCP=1
Environment=MCP_TCP_HOST=192.168.16.3
Environment=MCP_TCP_PORT=18767
Environment=MCP_ENABLE_SHELL_EXEC=1
ExecStart=/home/alinx/prj/mcp/mcp_server/build/src/mcp_server
WorkingDirectory=/home/alinx/prj/mcp/mcp_server/build/src
Restart=always
RestartSec=3

# 如果你想指定用户运行，取消下面注释并改成实际用户名
# User=alinx
# Group=alinx

[Install]
WantedBy=multi-user.target
```

```shell
sudo systemctl enable mcp_server.service
sudo systemctl restart mcp_server.service
```

### Windows

PowerShell：

```powershell
$env:MCP_ENABLE_STDIO = "0"
$env:MCP_ENABLE_TCP = "1"
$env:MCP_TCP_HOST = "192.168.16.2"
$env:MCP_TCP_PORT = "18767"
$env:MCP_ENABLE_SHELL_EXEC = "1"
.\build\src\Release\mcp_server.exe
```

CMD:

```cmd
set MCP_ENABLE_STDIO=0
set MCP_ENABLE_TCP=1
set MCP_TCP_HOST=192.168.16.2
set MCP_TCP_PORT=18767
set MCP_ENABLE_SHELL_EXEC=1
.\build\src\Release\mcp_server.exe
```

如果只允许本机访问，可以把 `MCP_TCP_HOST` 设为 `127.0.0.1`。

开机自启服务：
下载 nssm- the Non-Sucking Service Manager，并加入 path:

```web-idl
https://nssm.cc/release/nssm-2.24.zip
```

创建 start_mcp_server.bat :

```bat
@echo off
set MCP_ENABLE_STDIO=0
set MCP_ENABLE_TCP=1
set MCP_TCP_HOST=192.168.16.2
set MCP_TCP_PORT=18767
set MCP_ENABLE_SHELL_EXEC=1

cd /d D:\Project\2025-12-02\mcp\mcp_server
.\build\src\Release\mcp_server.exe
```

```cmd
nssm install mcp_server
```

在弹出来的图形界面中:

```cmd
Path
C:\Windows\System32\cmd.exe

Startup directory
D:\Project\2025-12-02\mcp\mcp_server

Arguments
/c "D:\Project\2025-12-02\mcp\start_mcp_server.bat"

Service name
mcp_server
```
配置好 nssm 之后，打开一个 cmd 配置开机自启:

```cmd
#开机自启
sc config mcp_server start= auto
#sc config mcp_server start= delayed-auto
#立即启动
net start mcp_server
#查看当前状态
sc query mcp_server
#从开机自启改为手动启动
sc config mcp_server start= demand
```



## 配置 Codex

Codex 侧配置的是 `mcp_stdio_proxy_adapter`，不是直接配置 `mcp_server`。
adapter 通常由 Codex 按 `config.toml` 自动启动，不需要单独常驻运行。
编辑 Codex 的 `config.toml`，加入：

```toml
[mcp_servers.mcp]
command = "D:\\Project\\2025-12-02\\mcp\\mcp_stdio_proxy_adapter\\build\\Release\\mcp_stdio_proxy_adapter.exe"
args = [
  "--transport", "tcp",
  "--host", "192.168.16.3",
  "--port", "18767",
  "--timeout-ms", "3000"
]
```

Linux 路径示例：

```toml
[mcp_servers.mcp]
command = "/home/alinx/prj/mcp/mcp_stdio_proxy_adapter/build/mcp_stdio_proxy_adapter"
args = [
  "--transport", "tcp",
  "--host", "192.168.16.3",
  "--port", "18767",
  "--timeout-ms", "3000"
]
```

`--host` 和 `--port` 必须与 `mcp_server` 运行时的 `MCP_TCP_HOST`、
`MCP_TCP_PORT` 对应。

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
- `MCP_ENABLE_DISCOVERY=1`：TCP 模式下开启 mcp_server 局域网发现；默认开启。
- `MCP_DISCOVERY_PORT=<port>`：UDP 发现监听端口；默认等于 `MCP_TCP_PORT`。
- `MCP_UDP_BROADCAST_LISTEN_PORT=<port>`：UDP 广播目标端口；默认等于 `MCP_TCP_PORT`。
- `MCP_DISCOVERY_BIND_HOST=<ip>`：UDP 发现监听地址；默认 `0.0.0.0`。
- `MCP_DISCOVERY_ADVERTISE_HOST=<ip>`：广播中声明给对端连接的地址；默认使用 UDP 来源地址。
- `MCP_DISCOVERY_HOSTS=<ip[:port],...>`：额外单播发现目标，适合测试或禁止广播的网络。
- `MCP_DISCOVERY_PROXY_TIMEOUT_MS=<ms>`：`gateway.proxy_tool` 未显式传入 `proxy_timeout_ms` 时的代理等待超时；默认 `5000`。
- `MCP_ENABLE_SHELL_EXEC=1`：允许 `system.shell_exec` 执行主机命令。

TCP 协议使用 4 字节大端长度头加 JSON body。`mcp_stdio_proxy_adapter`
负责把 Codex stdio JSON-RPC 转换成该 TCP framed 协议。

`server.list_servers` 工具会主动触发一次发现广播，短暂等待响应后返回 JSON
文本，结构包含 `total` 和 `servers`；每个 server 记录 `address`、`ip`、`port`
和 `system_status`。
