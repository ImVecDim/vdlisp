# vd：一个带 LLVM JIT 的迷你 Lisp（C++20）

本项目实现了一个小型 Lisp 解释器，并在“纯数值调用”的热路径上集成了基于 LLVM MCJIT 的即时编译（JIT）。同时提供交互式 REPL（readline 历史记录）、脚本执行、`require` 模块加载，以及带文件/行/列与调用链（宏展开/函数调用）的错误定位。

## 特性概览

- 解释器：S 表达式解析、词法作用域环境、函数与宏
- 基础数据类型：`nil`、number（`double`）、string、symbol、pair/list、function、macro
- 内置函数（部分）：`+ - * / < > <= >= = cons car cdr setcar setcdr list type parse print error exit require`
- 特殊形式（不自动求值参数）：`quote`、`quasiquote`、`unquote`、`set`、`fn`、`macro`、`let`、`while`、`cond`、`apply`
- 错误报告：`file:line:col` + 源码行 + `^` 指示列；宏/函数调用链辅助定位
- JIT：当用户函数在“参数全为 number”的情况下变热后，自动尝试 JIT 编译；失败或返回非 number 会自动回退解释器

## 依赖

- C++20 编译器（默认 `clang++`）

> 注意：项目在 CMake 中已设置为使用 C++20，并在 target 级别强制 `-std=c++20` 以覆盖 `llvm-config` 可能添加的旧 `-std=` 标志。
- LLVM（用于 JIT。优先通过 `llvm-config` 获取编译/链接标志）
- GNU readline（用于 REPL 的行编辑与历史记录）
- （可选）AddressSanitizer：用于内存问题排查（见下文）

说明：构建使用 `llvm-config`（通过 `CMake` 检测）来获取 LLVM 的编译与链接标志；如果系统上没有 `llvm-config`，CMake 会尝试在常见路径回退查找。

## 内存与泄漏检测

运行时对象（string/pair/func/macro/env 等）采用引用计数（见 `RcBase`），`Value` 使用 NaN-boxing 表示来保存 number 或指针 payload。

为了便于泄漏检测工具（ASan/LSan/Valgrind）判断生命周期，程序在退出前会执行一次“尽力清理”：

- 正常从 `main` 返回时：通过 RAII guard 调用 `State::shutdown_and_purge_pools()`

`shutdown_and_purge_pools()` 会清理全局引用（符号表、模块缓存、环境链等）并主动断开一些常见循环引用（例如闭包与环境之间的环），从而让引用计数能够回收更多对象。

如果你想用 ASan 跑一遍：

- 生成一个 Debug + ASan 构建目录：
  - `cmake -S . -B build_asan -D CMAKE_BUILD_TYPE=Debug -D ENABLE_LTO=OFF -D CMAKE_CXX_FLAGS="-fsanitize=address -g"`
  - `cmake --build build_asan -j$(nproc)`
- 运行（示例）：`ASAN_OPTIONS=detect_leaks=1 ./build_asan/vdlisp tests/pool_test.lisp`

### 内存与生命周期（Env 引用计数与 EnvGuard）

- 本项目对 `Env` 使用显式引用计数（继承自 `RcBase`），通过 `retain_env` / `release_env` 管理引用；`FuncData` / `MacroData` 的 `closure_env` 在分配时会 `retain`，在销毁时会 `release`。  
- 为保证异常安全和避免临时引用泄漏，引入了 `EnvGuard`（RAII）：在你需要暂时持有 `Env*` 时创建 `EnvGuard`，作用域结束时自动 `release`。示例：

```cpp
Env *e = S.make_env(parent);
EnvGuard eg(e); // 作用域结束时自动 release
// 使用 e
```

- 注意事项与建议：
  - **循环引用不会被引用计数回收**（这是引用计数的固有限制）。在长期运行的场景下，尽量避免构造长寿命的循环结构；程序退出时应调用 `State::shutdown_and_purge_pools()` 来断开常见循环（仓库已实现相关断开逻辑）。
  - 当前引用计数为 **非原子**（非线程安全）。若计划引入多线程访问，应改用原子计数或外部同步。  

如果需要，我可以添加：
- 用于验证引用计数行为的 debug-only 断言或单元测试；或
- 一个简单的长期运行/循环引用示例脚本，以及用于监控内存增长的基准脚本。

## 构建

项目使用 CMake 作为首选构建方式。常用命令如下：

推荐用 out-of-source 构建。常见组合：

- Release（默认）：
  - `cmake -S . -B build -D CMAKE_BUILD_TYPE=Release -D ENABLE_LTO=ON`
  - `cmake --build build -j$(nproc)`
  - 产物：`build/vdlisp`
- Debug（更适合调试/回溯）：
  - `cmake -S . -B build_debug -D CMAKE_BUILD_TYPE=Debug -D ENABLE_LTO=OFF`
  - `cmake --build build_debug -j$(nproc)`
  - 产物：`build_debug/vdlisp`

- 使用 `ccache` 缓存编译（可显著加速重复构建）：
  - 临时方法：
    - `export CC="ccache clang" && export CXX="ccache clang++"`
    - 然后运行上面的 `cmake` 命令。
  - CMake 的更高阶用法：`-D CMAKE_CXX_COMPILER_LAUNCHER=ccache`（取决于 CMake 版本）。

常见目标产物（路径）：
- `build/vdlisp`


## 运行

### 交互式 REPL

直接运行（以 Release 构建为例）：

- `./build/vdlisp`

特性：
- 提示符为 `> `
- 使用 readline 历史记录文件：`~/.VDLISP__history`
- 每次输入会 `parse` + `eval`，并打印结果

### 执行 Lisp 脚本

- `./build/vdlisp path/to/file.lisp`

行为：
- 读取文件并 `parse_all`，依次执行（类似 `do`），最后把“最后一个表达式的值”打印到 stdout
- 同时会在全局环境绑定变量 `argv`，内容为“文件名之后的命令行参数列表”（string list）

### 示例

仓库里可直接跑的脚本（也用于测试）：

- 分配/生命周期压力测试：[tests/pool_test.lisp](tests/pool_test.lisp)
- JIT 控制流覆盖：[tests/jit_control_forms.lisp](tests/jit_control_forms.lisp)
- 更长时间运行的压力用例：[tests/longrun.lisp](tests/longrun.lisp)

## 语言速览

### 基本语法

- 列表：`(f a b c)`
- 注释：以 `;` 开始，到行末结束
- `nil`：空值/空表（解析 token `nil` 会得到空指针表示）
- 字符串：双引号，支持转义 `\n \t \r \\ \"`

### 语法（精简概览）

- 词法简洁：Token 由空白或者分隔符 `(` `)` `'` `` ` `` `,` `"` `;` 划分；注释以 `;` 到行末。
- 原子：数值、字符串、符号（任意非分隔符序列）与 `nil`。
- 真值：启动时将 `#t` 绑定为真值符号；任何非 `nil` 均视为真。
- 列表：以括号表示 `(a b c)`；支持点尾记法 `(a b . c)` 表示 dotted-tail。
- 引用：`'x` 等价于 `(quote x)`；支持反引号 `` ` `` （quasiquote）与逗号 `,`（unquote）。
- 数字：支持浮点表示法（可含指数部分），解析与 C `strtod` 兼容。
- 字符串：双引号，支持常见转义序列 `\n \t \r \" \\`。

更多精确词法/语法定义见：[grammar.ebnf](grammar.ebnf)

### 真值

- `nil` 为假
- 其它非 `nil` 都为真
- 启动时会在全局环境绑定：
  - `#t`：真值符号（推荐在 `cond` 的默认分支中使用 `#t`）

### 列表与 pair

- `list`：直接返回其参数链表（不会复制）
- `cons`/`car`/`cdr`
- `setcar`/`setcdr`：原地修改 pair

### 变量与作用域

- `(set x expr)`：在当前环境链中查找并更新；若未找到则在当前环境绑定
- `(let (x 1 y 2) body...)`：创建新环境并绑定变量，然后依次执行 body

### 函数与调用

- `(fn (x y) body...)`：用户函数（闭包捕获 `let`/外层环境）
- 传参是按位置匹配；当前实现不做多余实参/缺少实参的复杂处理

### 可变参数（点尾 / dotted-tail）

- 解析器支持点尾（dotted-tail）语法。例如 `(fn (a b . rest) ...)` 或在列表字面量中 `(a b . rest)`。
- 在函数或宏定义的参数列表中，如果参数列表包含一个裸符号作为点尾（如 `. rest`），调用时解释器会把多余的实参打包成一个 Lisp 列表并绑定到该符号（`rest`）。
- 示例：
  - `(set f (fn (a b . rest) (list a b rest)))` 及 `(f 1 2 3 4)` 会返回 `(1 2 (3 4))`。
  - 宏也支持点尾：`(set m (macro (a . rest) (list a rest)))`。
- 实现说明：点尾在解析期被识别，`.` 后面的表达式被作为列表的 cdr 拼接；在调用期，绑定逻辑会检测裸符号参数并把剩余的实参赋予它。


### 宏

- `(macro (x) body...)`：宏在“解释期/展开期”执行，返回一个新的 AST，再对该 AST 求值
- 宏相关错误会尽量报告“调用点”位置，并附带 Call chain（包含宏调用点与宏定义位置 `macro-def`）

### 条件与循环

- `(cond (test1 expr...) (test2 expr...) (#t expr...))`
- `(while cond body...)`

说明：代码注释中提到“`if` 作为 primitive 被移除，建议用 `cond` 在语言层实现宏”。当前仓库默认并未内置 `if`，但程序启动时会尝试自动加载 `scripts/lang_basics.lisp`（若存在），你可以在该文件中实现 `if` 等语法糖。

### 其它

- `(apply f lst)`：对列表参数进行展开调用（`f` 与 `lst` 都会被求值）
- `(parse "(+ 1 2)")`：把字符串解析为 AST（返回 list/symbol/number/string 等结构）
- `(type v)`：返回类型名 symbol，如 `number`/`string`/`function`/`jit_func` 等
- `(require "path/to/mod.lisp")`：加载并执行文件，返回其结果；按 canonical path 缓存

## `require` 模块加载

`require` 的行为要点：

- 参数必须是 string
- 若传入相对路径，会优先尝试“相对于调用点文件所在目录”的路径，然后再尝试原始路径
- 使用 canonical/absolute 路径做缓存 key（避免重复加载）
- 错误信息会包含尝试过的路径列表

## JIT（LLVM MCJIT）说明

当前 JIT 策略偏保守，核心点：

- 仅对“用户函数”（`TFUNC`）尝试 JIT
- 仅当一次调用的**实参全部为 number** 时，才走数值热路径统计 `num_call_count`
- 当前可变参数函数无法被 JIT
- 当 `num_call_count > 3` 且尚未编译、也未标记失败时，触发 `global_jit.compileFuncData(fd)`
- 若 JIT 代码执行返回 NaN（例如内部遇到非数值/回调解释器后得到非 number），会禁用该函数的 JIT：
  - 清空 `compiled_code`
  - 标记 `jit_failed = true`
  - 回退到解释器路径保证正确性

可观察性：

- `(type f)`：函数初始为 `function`，JIT 后为 `jit_func`
- `(print f)`：JIT 后会显示 `<jit_func>`（未 JIT 时为 `<function>`）

实现细节提示：
- JIT 代码可通过桥接函数 `VDLISP__call_from_jit` 回调解释器（见 [src/jit/jit.hpp](src/jit/jit.hpp)）
- 运行时使用 NaN-boxing 存储值；程序启动时会检查指针是否能放入 48-bit payload（典型 x86_64 “canonical address” 假设）。若平台不满足会直接退出。

## 测试

- 运行全部测试：`./tests/test.sh`（该脚本会用 CMake 构建并执行测试）

测试脚本为 [tests/test.sh](tests/test.sh)，覆盖：
- 算术与比较
- list/pair 操作
- `parse` 字符串与转义
- `require` 模块加载
- quote / fn / macro 语义（含宏错误定位）
- `cond` / `let` / `while`
- `apply`
- JIT 触发与回退（包括嵌套调用与非数值回退）
- 错误信息需包含文件名/源码行/列 caret

- 点尾（dotted-tail）支持：新增测试用例在 `tests/test.sh`，验证函数与宏在参数列表中使用点尾时的绑定与行为。

## 目录结构

- [src/](src/)：解释器与运行时
  - [src/main.cpp](src/main.cpp)：程序入口（REPL/脚本执行、自动加载 `scripts/lang_basics.lisp`）
  - [src/vdlisp.cpp](src/vdlisp.cpp)：解释器主体（`State`、eval/call、JIT 触发逻辑等）
  - [src/nanbox.hpp](src/nanbox.hpp)：值表示（NaN-boxing）、引用计数基类 `RcBase`、`Env`/`EnvGuard`
  - [src/helpers.cpp](src/helpers.cpp)：解析器、错误定位与通用 helper
  - [src/core.cpp](src/core.cpp)：核心内置函数/特殊形式注册
  - [src/require.hpp](src/require.hpp)：`require`（模块加载/缓存）
  - [src/jit/](src/jit/)：LLVM IR 生成与 MCJIT 编译
- [tests/](tests/)：测试脚本与用例（`tests/test.sh`）
- [scripts/](scripts/)：语言层辅助（启动时可自动加载）
- [grammar.ebnf](grammar.ebnf)：语法定义

## 开发提示

- 如果你想加语言层语法糖：可以创建 `scripts/lang_basics.lisp`，程序启动会自动加载（若文件存在）

## AI提示

- 部分代码使用AI编写或修改