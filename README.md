# vd：一个带 LLVM JIT 的迷你 Lisp（C++17）

本项目实现了一个小型 Lisp 解释器，并在“纯数值调用”的热路径上集成了基于 LLVM MCJIT 的即时编译（JIT）。同时提供交互式 REPL（readline 历史记录）、脚本执行、`require` 模块加载，以及带文件/行/列与调用链（宏展开/函数调用）的错误定位。

## 特性概览

- 解释器：S 表达式解析、词法作用域环境、函数与宏
- 基础数据类型：`nil`、number（`double`）、string、symbol、pair/list、function、macro
- 内置函数（部分）：`+ - * / < > <= >= = cons car cdr setcar setcdr list type parse print error exit require`
- 特殊形式（不自动求值参数）：`quote`、`quasiquote`、`unquote`、`set`、`fn`、`macro`、`let`、`while`、`cond`、`apply`
- 错误报告：`file:line:col` + 源码行 + `^` 指示列；宏/函数调用链辅助定位
- JIT：当用户函数在“参数全为 number”的情况下变热后，自动尝试 JIT 编译；失败或返回非 number 会自动回退解释器

## 依赖

- C++17 编译器（默认 `clang++`）
- LLVM（需要 `llvm-config`，并链接 `core`、`native` 等库）
- GNU readline（用于 REPL 的行编辑与历史记录）
- Boost（使用 `boost::object_pool` 作为运行时对象内存池）

说明：构建使用 `llvm-config`（通过 `CMake` 检测）来获取 LLVM 的编译与链接标志；如果系统上没有 `llvm-config`，CMake 会尝试在常见路径回退查找。

## 内存池与泄漏检测

运行时对象（string/pair/func/macro 等）使用 `boost::object_pool` 分配，以减少频繁 `new/delete` 的开销，并配合 NaN-boxing 让 `Value` 内部保存原始指针。

为了让 ASan/LSan/Valgrind 这类工具更容易判断“是否真的泄漏”，程序在退出前会进行一次统一清理：

- 正常从 `main` 返回时：通过 RAII guard 调用 `State::shutdown_and_purge_pools()`
- 调用 Lisp 内置 `(exit ...)` 时：在 `std::exit(...)` 之前同样会调用 `State::shutdown_and_purge_pools()`

`shutdown_and_purge_pools()` 的目标是：

- 先释放解释器侧的全局引用（模块缓存、符号表等），让 `Ptr`/`Value` 的析构能安全发生
- 再对各个 pool 执行 best-effort 的 `purge_memory()`，并销毁/重置 pool，以尽量把内存归还给系统

提示：如果你在跑泄漏检测，可以使用基于 Debug 的构建并启用 AddressSanitizer：

- 使用 CMake 生成 Debug/ASan 构建，例如：
  - `cmake -S . -B build -D ENABLE_LTO=OFF -DCMAKE_BUILD_TYPE=Debug`
  - 编辑 `CMakeLists.txt` 或在命令行添加 `-DCMAKE_CXX_FLAGS="-fsanitize=address -g"`，然后 `cmake --build build`
  - 运行时示例：`ASAN_OPTIONS=detect_leaks=1 build/vdlisp`

## 构建

项目使用 CMake 作为首选构建方式。常用命令如下：

- 生成（禁用 LTO 以加快开发迭代）：
  - `cmake -S . -B build -D ENABLE_LTO=OFF`
- 构建（使用所有 CPU 内核并行）：
  - `cmake --build build -j$(nproc)`
- 产物：`build/vdlisp`
- 启用 LTO/高优化：设置 `-D ENABLE_LTO=ON` 或切换到 `Release` 配置。

- 使用 `ccache` 缓存编译（可显著加速重复构建）：
  - 临时方法：
    - `export CC="ccache clang" && export CXX="ccache clang++"`
    - 然后运行上面的 `cmake` 命令。
  - CMake 的更高阶用法：`-D CMAKE_CXX_COMPILER_LAUNCHER=ccache`（取决于 CMake 版本）。

常见目标产物（路径）：
- `build/vdlisp`


## 运行

### 交互式 REPL

直接运行：

- `./vdlisp`

特性：
- 提示符为 `> `
- 使用 readline 历史记录文件：`~/.VDLISP__history`
- 每次输入会 `parse` + `eval`，并打印结果

### 执行 Lisp 脚本

- `./vdlisp path/to/file.lisp`

行为：
- 读取文件并 `parse_all`，依次执行（类似 `do`），最后把“最后一个表达式的值”打印到 stdout
- 同时会在全局环境绑定变量 `argv`，内容为“文件名之后的命令行参数列表”（string list）

### 示例

- 最小示例（仓库内）：[simple_test.lisp](simple_test.lisp)
- `cond` 示例：[cond_test.lisp](cond_test.lisp)
- JIT/函数示例：[tmp_jit_test.lisp](tmp_jit_test.lisp)

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
  - `#t`：真值符号
  - `else`：`cond` 中的别名（绑定为 `#t`）

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

- `(cond (test1 expr...) (test2 expr...) (else expr...))`
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
- JIT 代码可通过桥接函数 `VDLISP__call_from_jit` 回调解释器（见 [src/jit/jit_bridge.cpp](src/jit/jit_bridge.cpp)）
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

- [include/](include/)：头文件
  - [include/vdlisp.hpp](include/vdlisp.hpp)：`State`/接口
  - [include/nanbox.hpp](include/nanbox.hpp)：值表示（NaN-boxing）、`FuncData`/`MacroData`
  - [include/core.hpp](include/core.hpp)：核心内置注册接口
  - [include/require.hpp](include/require.hpp)：`require` 实现（内联注册）
  - [include/jit/](include/jit/)：JIT 相关头
- [src/](src/)：实现
  - [src/vdlisp.cpp](src/vdlisp.cpp)：解释器主体 + REPL + 主入口 + JIT 触发
  - [src/helpers.cpp](src/helpers.cpp)：解析器、错误定位与通用 helper
  - [src/core.cpp](src/core.cpp)：核心内置函数/特殊形式注册
  - [src/jit/](src/jit/)：LLVM IR 生成与 JIT 编译器
- [tests/](tests/)：测试脚本与测试用例
- [compile_commands.json](compile_commands.json)：便于 clangd/IDE 提示

## 开发提示

- 如果你在本地用 clangd：仓库已提供 [compile_commands.json](compile_commands.json)
- 如果你想加语言层语法糖：可以创建 `scripts/lang_basics.lisp`，程序启动会自动加载（若文件存在）

## AI提示

- 部分代码使用AI编写或修改