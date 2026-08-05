# Prosto

完整使用指南（中文）

概述
----
Prosto（Prosto+）是一个轻量的脚本式编程语言解释器与交互式环境，项目包含解释器源码和已编译的 Windows 可执行文件。目标是提供一个易用的 REPL 与脚本运行环境，适合日常自动化、工具编写与教学用途。

已编译发布
----
Windows 二进制（zip 包，包含 prosto.exe）：
https://github.com/Everechnology/Prosto/releases/download/Release/prosto.zip

快速开始（下载并运行）
-------------------
1. 下载并解压：
   - 资源管理器右键解压，或在 PowerShell 中：
     Expand-Archive -Path .\prosto.zip -DestinationPath .\prosto

2. 运行：
   - 双击 prosto.exe，或在命令行中进入目录并运行：
     .\prosto.exe

3. 两种工作模式：
   - 交互式 REPL（不带参数运行） — 适合探索、测试表达式与快速调试。提示符：ptcp>
   - 运行脚本（将脚本路径作为第一个参数） — 适合批处理、脚本化任务：
     .\prosto.exe myscript.ptcp

REPL（交互式）详解
-------------------
启动 REPL 后，会显示欢迎信息和内置帮助命令：

- 可用命令（在 REPL 单独输入）：
  - exit / quit  — 退出 REPL
  - vars        — 列出当前全局变量与其值
  - funcs       — 列出已定义的函数及参数
  - help        — 简短帮助

- 表达式与语句处理：
  - 单行表达式将被自动识别并求值，结果打印在下一行。
  - 赋值语句、函数/类定义或控制结构可写为完整语句或多行代码块。
  - 支持用 { } 包围多行代码块，REPL 会根据大括号深度提示是否继续输入（提示变为 ...）。

示例交互：
  ptcp> 1 + 2
  3
  ptcp> x = 10
  ptcp> vars
    x = 10
  ptcp> def square(a) { return a * a }
  ptcp> square(4)
  16

语言基础（入门）
----------------
以下为常用语法示例（基于解释器源码的行为）：

1) 变量与类型
  - 动态类型；常见类型：数值（整型/浮点）、字符串、布尔、列表、字典、函数、对象等。
  - 示例：
    a = 123
    b = 3.14
    s = "hello"
    lst = [1, 2, 3]
    d = {"k": "v"}

2) 表达式与运算
  - 支持常见算术、比较和逻辑运算符。
  - 示例：
    1 + 2 * (3 - 4) / 5
    a == 123

3) 控制流
  - if/else、for、while、switch/case（或类似结构）可用，示例：
    if (x > 0) { print("positive") } else { print("non-positive") }

4) 函数定义
  - 定义与调用：
    def add(a, b) { return a + b }
    print(add(1,2))
  - 支持函数作为一等值传递，支持可选/默认参数（视内置实现）和变长参数（kwargs 形式）

5) 模块与导入
  - 可以导入包或脚本文件（源码实现中有 importPackage 与导入文件的逻辑）。
  - 若按包组织，解释器可能会查找 package 目录下的 main_init.ptcp 作为入口（请参照源码的导入语义）。

内置常用函数（示例）
-------------------
解释器实现中包含若干内置函数与对象（常见内置）：
- print(...) — 打印到标准输出
- http 请求函数（doHttpRequest / httpDownload） — 进行网络请求/下载
- 文件系统对象（EFCObject） — 读取、写入、移动、拷贝文件或目录
- JSON 与 sqlite 工具（valueToJson / jsonToValue / sqlite3 支持）
- 编码/加密工具（hex/base64/evp/hmac 等）

注意：完整的内置函数列表和签名可以在 sec/prosto_builtins.cpp 与 sec/prosto_common.hpp 中查看（或在 REPL 中通过 funcs/vars + 内置帮助打印）。如果需要，我可以把所有内置函数与示例整理到 README 中。

示例脚本（example.ptcp）
------------------------
下面给出一个简单的示例脚本（将此内容保存为 example.ptcp）：

# example.ptcp
x = 1
while (x <= 5) {
    print("x = " + x)
    x = x + 1
}

def fib(n) {
    if (n <= 1) return n
    a = 0; b = 1
    for (i = 2; i <= n; i = i + 1) {
        t = a + b
        a = b; b = t
    }
    return b
}

print(fib(10))

运行：
  .\prosto.exe example.ptcp

构建与开发者指南
-----------------
若需从源码编译或进行二次开发：

依赖（常见）
- nlohmann/json
- sqlite3
- libzip (zip.h)
- libcurl
- OpenSSL (EVP/HMAC)
- CMake 或 Visual Studio 工具链

使用 vcpkg（推荐）
1. 安装 vcpkg 并集成到系统（参考 vcpkg 文档）。
2. 在项目根目录运行：
   vcpkg install nlohmann-json sqlite3 libzip libcurl openssl
3. 使用 CMake，指定 vcpkg toolchain 文件：
   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
   cmake --build build --config Release

使用 Visual Studio
- 打开 CMakeLists.txt 或将仓库作为 Visual Studio 的 CMake 项目打开，选择 Release/x64，然后构建。

打包发行（建议）
- 若发布 Windows 可执行，建议：
  - 使用静态链接（如可能）减少运行时依赖；或
  - 将所有需要的 DLL 一并打包到 zip 中（例如 libcurl、libcrypto、libssl、msvcp/ vcruntime 等）并在 README 中说明依赖。

调试与故障排查
----------------
- 在命令行中运行程序以便看到错误输出，而不是直接双击。
- 常见错误与排查：
  - 缺少运行时 DLL：安装 Microsoft Visual C++ Redistributable 或把依赖 DLL 放在 exe 同目录。
  - 导入/包找不到 main_init.ptcp：检查包目录结构和相对路径。
  - 权限问题：尝试以管理员权限运行或选择用户有写权限的目录。

如何在项目中编写更复杂程序（设计建议）
-----------------------------------
1. 把功能拆成小模块：将小任务写成独立的 .ptcp 文件并通过 import/加载来复用。
2. 在脚本中尽量捕捉和处理错误（如果语言提供 try/except 风格语句）。
3. 使用函数封装 I/O、网络、数据库访问等副作用操作，便于测试。
4. 对于长期运行或并发任务，了解解释器对多线程/多进程的支持（源码中有 multithreading/multiprocess 的敏感点，需要注意安全沙箱）。

贡献与反馈
----------
- 报告 Bug 或提出功能建议：在 GitHub 仓库打开 Issue，提供可复现步骤与最小示例。
- 提交代码：Fork -> 新分支 -> 修改 -> Pull Request。请在 PR 描述中写明改动目的和影响范围。

许可证
----
- 该项目使用 **SSPL** 作为开源许可证，在使用本项目之前，请确保您遵守 **SSPL** 中的条目

常见问题（FAQ）
---------------
Q: 如何在脚本中打印调试信息？
A: 使用 print(...)，并在命令行中运行脚本以查看控制台输出。

Q: 如何使用 SQLite？
A: 源码中集成了 sqlite3，可通过内置绑定使用数据库功能（示例和 API 可在 prosto 的内置模块中查找）。

Q: 我可以将 Prosto 嵌入到其他程序吗？
A: 源码结构将解释器实现为 C++ 类（Interpreter），可以在 C++ 程序中集成并以 API 方式嵌入（需按源码修改并编译成库）。

附录：源代码参考位置
------------------
- 交互与入口： [src/main.cpp]
- REPL 支持： [sec/prosto_repl.cpp]
- 运行时与内置： [sec/prosto_runtime.cpp]
- 内置函数实现： [sec/prosto_builtins.cpp]
- 公共声明： [sec/prosto_common.hpp]
