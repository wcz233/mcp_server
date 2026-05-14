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
- `config/`：构建配置，只保留 Linux/Windows 两版。
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

### Linux

在仓库根目录执行：

```bash
cmake -S mcp_server -B mcp_server/build -C mcp_server/config/linux_defconfig.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build mcp_server/build --parallel

cmake -S mcp_stdio_proxy_adapter -B mcp_stdio_proxy_adapter/build -DCMAKE_BUILD_TYPE=Release
cmake --build mcp_stdio_proxy_adapter/build --parallel
```

产物路径通常是：

```text
mcp_server/build/src/mcp_server
mcp_stdio_proxy_adapter/build/mcp_stdio_proxy_adapter
```

### Windows

在仓库根目录使用 PowerShell 执行：

```powershell
cmake -S mcp_server -B mcp_server\build -C mcp_server\config\windows_defconfig.cmake
cmake --build mcp_server\build --config Release

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

### Windows

PowerShell：

```powershell
$env:MCP_ENABLE_STDIO = "0"
$env:MCP_ENABLE_TCP = "1"
$env:MCP_TCP_HOST = "192.168.222.128"
$env:MCP_TCP_PORT = "18767"
$env:MCP_ENABLE_SHELL_EXEC = "1"
.\mcp_server\build\src\Release\mcp_server.exe
```

如果只允许本机访问，可以把 `MCP_TCP_HOST` 设为 `127.0.0.1`。

## 配置 Codex

Codex 侧配置的是 `mcp_stdio_proxy_adapter`，不是直接配置 `mcp_server`。
adapter 通常由 Codex 按 `config.toml` 自动启动，不需要单独常驻运行。
编辑 Codex 的 `config.toml`，加入：

```toml
[mcp_servers.mcp]
command = "D:\\Project\\2025-12-02\\mcp\\mcp_stdio_proxy_adapter\\build\\Release\\mcp_stdio_proxy_adapter.exe"
args = [
  "--transport", "tcp",
  "--host", "192.168.222.128",
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
  "--host", "192.168.222.128",
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
- `MCP_ENABLE_SHELL_EXEC=1`：允许 `system.shell_exec` 执行主机命令。

TCP 协议使用 4 字节大端长度头加 JSON body。`mcp_stdio_proxy_adapter`
负责把 Codex stdio JSON-RPC 转换成该 TCP framed 协议。
