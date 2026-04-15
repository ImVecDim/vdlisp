# vdlisp 架构与设计文档

本文档详细阐述了 vdlisp 的系统架构与核心设计细节。

## 1. 系统概述 (System Overview)

vdlisp 是一个微型 Lisp 解释器。它的核心由 C++20 开发，并在频繁调用的纯数值（pure numeric）计算函数路径上集成了一个基于 LLVM MCJIT 的即时编译器（JIT）。

**核心能力包括：**
- **词法与解释器**：支持标准 S 表达式解析，提供严格的词法作用域（Lexical Scope）以及用户函数（闭包）和宏（Macro）。
- **即时编译（JIT）加速**：针对纯浮点型（`double`）计算的热点代码无缝启动 LLVM IR 生成与 JIT 编译执行。
- **错误定位引擎**：能够精确跟踪到文件、行、列，支持追踪由于宏展开与深层次函数调用引发的级联调用链。
- **脚本与交互式能力**：支持 Readline 历史记录的 REPL 和 `require` 模块执行能力。

## 2. 数据表示 (Data Representation)

### 2.1 NaN-Boxing (NaN 装箱)
动态语言需要在同一个变量中容纳多种类型。vdlisp 在底层使用 IEEE 754 标准的 64 位浮点数（`double`）机制，并通过 **NaN-Boxing** 技巧实现了高效的单一数据类型表示（见 `Value` 类定义）。
- **空间利用**：标准双精度浮点数有一个标志位、11位指数位、52位尾数位。当指数位全为 1（`0x7FF0000000000000ULL`）时，该数值属于 NaN（Not a Number）。
- **类型标签（Tags）**：利用 NaN 掩码之下的 4 个比特位（48到51位）来编码类型，剩余的低 48 位作为载荷（Payload）用于存储内存指针。
- **内建类型（`Type`）**：
  - `TNUMBER`：非 NaN，本身是一般的 `double`，享有最激进的分支预测（Fast Path）。
  - 其他类型利用指针及Tag标记表示：`TNIL`（空 / 假值）、`TPAIR`（列表节点）、`TSTRING`（字符串）、`TSYMBOL`（符号）、`TFUNC`（用户函数）、`TMACRO`（宏）、`TPRIM`（特殊形式）、`TCFUNC`（C++ 内置函数）。

### 2.2 RcBase (基础数据结构)
由于 vdlisp 中许多数据类型（例如闭包、环境、列表、字符串）需在多处被引用，它们均继承自轻量级的引用计数基类 `RcBase`，包含：
- 内部成员变量 `refs_` (无符号计数器)。
- 内联函数 `inc_ref()` 和 `dec_ref()` 实现增加释放生命周期。

## 3. 内存管理 (Memory Management)

在内存回收策略上，系统基于显式**引用计数（Reference Counting）**为主线。

### 3.1 引用计数与自动释放
通过封装在 `Value` 内部的 `retain()` 与 `release()` 方法管控包含有效指针类型的生命周期分配。由于部分类型如数值与内置函数不作为裸指针指向内存块，它们被标记为不可计数类型（Unrefcounted）。

### 3.2 EnvGuard (环境防具)
解释过程会高频操作环境哈希（`Env`）。为保证发生 C++ 异常时及时的资源退库、防止内存泄露，引入了 RAII（资源获取即初始化）范式保护器 `EnvGuard`。当需要新环境帧时，由 `EnvGuard` 包装临时 `Env*`，在作用域离开时将触发对应封装环境的 `release_env`，安全无痛。

### 3.3 循环引用打破机制
单纯的引用计数会有循环引用泄露的固有缺陷（例如闭包捕获自己所在的环境链）。为使内存检测工具（基于 ASan / Valgrind 等）能正确通过并帮助长寿命脚本安全执行：
- 解释器退出环节（`State::shutdown_and_purge_pools`）提供了一种基于“尽力而为（Best-effort）”的主动打破循环引用手段，主要用于擦除缓存、符号表并显式切断执行期的环路。

## 4. 执行模型 (Execution Model)

### 4.1 Parser (解析器)
代码会由字符串解析为内部的抽象语法树（AST）。利用递归下降读取各种基本符号并组织为嵌套的 Lisp List（`TPAIR`）。Parser 支持点尾（dotted-tail, `(a . b)`）、基础字符串转义及各种宏读取前缀记号（`'`, `` ` ``, `,`）。

### 4.2 Evaluator (求值器) 与特殊形式
求值环节遍历 AST 执行。对于标准链表 `(func arg1 arg2 ...)` 会分为宏扩展与函数求值两步。部分保留字（特殊形式，`TPRIM`）不会由 Evaluator 率先把参数全部求值，而是采用专门的方法拦截：
- `quote` 和 `set`、`let`。
- `cond` 与 `while` 控制流。
- `fn` (创建用户闭包)、`macro` (创建宏转换函数)。

### 4.3 词法作用域 (Lexical Scope)
作用域由 `Env` 类（继承 `RcBase`）管理，内聚包含指向父辈的映射（`Env* parent`）以及局部 `std::unordered_map<std::string, Value> map` 名称值绑定表。系统按链式回溯寻找最近作用的变量。

### 4.4 宏 (Macro) 的特殊期 
宏是在“解释展开期”动态调用的闭包函数，通过把传入的原始 AST 对象视为数据并动态演算后，返回经过改写的 AST ，然后立即触发该展开结点的再求值。宏相关的错误会尽量将调用 Call Chain （基于 `src_call_chain_map` 机制）全部打印处理。

## 5. JIT 编译 (JIT Compilation)

为了解决脚本解释带来的性能损耗，在用户 Lisp 侧数值循环计算环节做了激进 JIT 优化。基于 LLVM MCJIT。

### 5.1 触发机制
针对 `FuncData` 对象内部包含 `num_call_count`，当：
1. 本函数执行频繁且被热度检测触发。
2. 传入的参数**全部**为 `TNUMBER`。
系统则主动尝试将其解释期 AST 即时拉取并编译为 LLVM 的机器码模块 (`void* compiled_code`)。

### 5.2 返回约束 & 降级保护 (Interpreter Fallback)
JIT 生成函数要求具有标准的 C 语言 ABI 签名约束机制（接收 `double*`, 返回 `double`）。在执行内若是遇见：
- 环境跨越（Closure Env 捕获数值）。利用 C 注入函数边界 `VDLISP__jit_lookup_number` 在外侧运行期链式检测 `Env` 值是否非数值约束/未绑定（导致返回 `NaN`）。
- 一旦发生 NaN / JIT 编译不支持的形式 / 内部错误，JIT Wrapper （`VDLISP__call_from_jit`）能够侦测到，进而立刻回退（Fallback）至普通解释器求值 `VDLISP__call_interpreted_from_jit`。并为该闭包永久置起 `jit_failed = true` ，防止退化导致的后续无尽重新尝试开销。

*(注：系统执行期间通过 `jit_active_state` 去记录跨运行时的 Global State 上下文，供异常态的 JIT 去查找全局 Fallback 函数栈)*

## 6. 文件与模块系统 (`require`)

解释器支持构建并服用模块的能力。

### 6.1 `require` 语法
通过内置的 `require` （如 `(require "path/to/mod.lisp")`）执行其他文件代码并带回结果。
执行具有环境隔离感知并采用 **路径规范化 (Canonical Path)** 为键（Key），存入 `loaded_modules`。

### 6.2 模块缓存机制
1. 如果已加载过某绝对路径的对应模块，直接由缓存返回其终态 Value ，避免重复求值以及死循环包含。
2. 对于相对路径加载，其会优先根据“执行发起点”（即上一个调用方所在目录）拼凑相对查找；若该文件不存在时，再退回到基于当前系统启动目录的工作路径。