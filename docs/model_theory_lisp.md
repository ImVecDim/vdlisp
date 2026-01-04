# 用模型论语言描述 vd-mini-Lisp

本文以模型论（model theory）的视角，形式化描述仓库中实现的迷你 Lisp（下称 VDLisp）。内容覆盖签名（signature）、结构（structures）、解释（interpretation）、以及语义（evaluation relation）——同时指出与实现之间的映射。

**概览**

- 目标：把语言机制（语法、值域、环境与求值）用模型论术语公理化，使得语言规范可被形式化推理、验证或作为实现参考。
- 约定：术语 "表达式"（Expr）用于源代码 AST，"值"（Value）用于运行时值（number/string/symbol/pair/function/closure/macro 等）。

**1. 逻辑签名（Signature）**

使用一阶逻辑或多阶逻辑的符号来表达语言构件：

- 基本一元/多元符号（函数符与谓词）：
  - `isNumber(x)`, `isString(x)`, `isSymbol(x)`, `isPair(x)`, `isFunc(x)`, `isMacro(x)`：一元谓词区分值的类型。
  - `car(x)`, `cdr(x)`: 一元函数符，当 `isPair(x)` 成立时定义。
  - `cons(x,y)`: 二元函数符，构造 pair。
  - `apply(f, args)`: 函数应用的抽象符号（可在元语言中展开为具体语义规则）。
  - `quote(e)`: 抽象符号表示引用语义。

- 常量/符号：`nil`, `#t`（真值符号）、内置标识符如 `+` `-` `cons` 等（可视为语言签名中的命名符号）。

- 额外：解释环境相关符号 `Env(e,x,v)`（谓词，表示在环境 `e` 中符号 `x` 解释为值 `v`）。

形式上，签名 Σ 包含上述函数符、谓词符与个体常量。

**2. 结构（Structures）**

一个 Σ-结构 M = (D, I) 给出：

- 域 $D$：VDLisp 的运行时值集合（Value domain）。$D$ 包括：
  - 数值子域 $D_{num} \subset D$（以 IEEE double 表示）
  - 字符串子域 $D_{str}$
  - 符号子域 $D_{sym}$（符号表 interned strings）
  - 对 (pair) 子域 $D_{pair} = D \times D$（或者抽象为一类带 car/cdr 投影的元素）
  - 函数/宏子域 $D_{fn}$（包含闭包/原生/已编译 JIT 函数的表示）
  - 特殊值 `nil`（域中唯一的表示空链/假值）和 `#t` 表示真值符号

- 解释函数 $I$：为签名中的每个函数符与谓词符在域 $D$ 上给出具体解释，例如
  - $I(cons)$ 是将 $(v_1,v_2)\in D^2$ 映射到 $d\in D_{pair}$ 的二元函数。
  - $I(car)$ 在 $D_{pair}$ 上投影到第一个分量；未定义时可视作特定 error 值或部分函数。
  - $I(isNumber)$ 在 $D$ 上是一个指示函数（谓词），仅在 $D_{num}$ 上为真。

实现层面：在代码中，$D$ 通过 NaN-boxing 的 `Value` 表示（参见 [src/nanbox.hpp](src/nanbox.hpp) 与 [src/nanbox.cpp](src/nanbox.cpp)），`cons`/`car`/`cdr` 对应到 `pair` 结构与 `RcBase` 管理的堆对象。

**3. 语法（Syntax）——抽象语法树（AST）**

用模型论的语言，语法定义为一个代数结构：

- 词汇层（term constructors）：
  - `Sym(s)`, `Num(n)`, `Str(s)` 为原子构造子
  - `List(e1,...,en)` 或等价的 `Cons(e1, Cons(e2, ... Cons(en, Nil)...))` 表示列表（S-表达式）

在一阶结构中，表达式集合 Expr 被视作另一域（或定义为 $D_{AST}$），并由构造子 `Sym/Num/Str/Cons` 操作。

仓库中语法的参考实现在 [grammar.ebnf](grammar.ebnf)。解析器实现位于 [src/helpers.cpp](src/helpers.cpp) 与相关文件。

**4. 语义（Semantics）——评价关系（Evaluation Relation）**

我们使用大步语义（big-step semantics）或结构化自然语义来描述求值关系：

- 评价关系记作

  $$\Gamma \vdash_e e \Downarrow v$$

  解释为：在环境 $\Gamma$ 下，表达式 $e$ 求值得到运行时值 $v$。

- 环境 $\Gamma$：有限映射 $Sym \to D$，实现上对应 `Env` 链（每个 `Env` 有指向父 Env 的指针）。

主要语义规则（选取语言核心片段）：

- 文字（literal）：

  $$\dfrac{}{\Gamma \vdash_e n \Downarrow n}\quad(\text{NUM})$$
  $$\dfrac{}{\Gamma \vdash_e "s" \Downarrow "s"}\quad(\text{STR})$$
  $$\dfrac{}{\Gamma \vdash_e 'x \Downarrow \text{quote}(x)}\quad(\text{QUOTE-LIT})$$

- 变量引用：

  $$\dfrac{\Gamma(x)=v}{\Gamma \vdash_e x \Downarrow v}\quad(\text{VAR})$$

- 引用（quote）与 quasiquote/unquote：按标准 Lisp 规则在语法/展开级处理；展开后产生 AST，再评估该 AST（或在宏展开期停止求值）。

- 函数（lambda / fn）：

  $$\dfrac{}{\Gamma \vdash_e (fn\,(p_1\,...,\,p_n)\;t) \Downarrow \langle\text{closure}\;p_1...p_n, t, \Gamma\rangle}\quad(\text{LAMBDA})$$

  这里闭包是三元组：参数列表、函数体 AST、定义时的环境 $\Gamma$。

- 函数调用：

  $$\dfrac{\Gamma \vdash_e e_f \Downarrow v_f\quad \forall i\;\Gamma \vdash_e e_i \Downarrow v_i \quad v_f=\langle\text{closure}\;p,t,\Gamma'\rangle\\ \Gamma'[p_i\mapsto v_i]_{i=1..n}\vdash_e t \Downarrow v}{\Gamma \vdash_e (e_f\;e_1\,...,\,e_n) \Downarrow v}\quad(\text{APP})$$

- 原语（primitive）与内建特殊形式（non-evaluating args）：例如 `quote`、`set`、`fn`、`macro`、`let`、`cond` 等，定义为语义规则的特殊分支；实现中以 C++ 函数（或解释器中的 switch-case）直接处理。

- 宏（macro）：宏在展开期被求值，得到一个新的 AST，然后再次对该 AST 进行求值（或继续展开）。宏的语义可用两层评价字面化：

  1. 宏展开：$\Gamma \vdash_m m(e_1,...,e_n) \Rightarrow e'$（在宏展开语义下求值产生 AST）
  2. 求值：$\Gamma \vdash_e e' \Downarrow v$

实现注意：仓库在 `macro` 的实现上保留调用链信息以在错误报告中定位宏调用点（见 README 的错误定位说明）。

**5. 语义性质与公理化断言（可在模型论中表达）**

下面的语句可以视为对结构 M 的公理/约束：

- 唯一性/互斥类型：对任意 $d\in D$，若 $isNumber(d)$ 则不能同时 $isString(d)$ 等（类型判定互斥）。
- `car(cons(x,y)) = x` 与 `cdr(cons(x,y)) = y`（构造投影律）。
- 环境一致性：若 $Env(e,x,v)$ 且 $Env(e,x,w)$ 则 $v=w$（一个环境对同一符号只有唯一绑定）。
- 闭包隔离：闭包的环境为其定义时捕获的 $\Gamma$，即闭包对外部环境的访问受定义环境控制（词法作用域）。

这些公理能帮助进行简单的形式化证明，例如证明词法作用域下变量捕获的不可变性、或宏展开前后产生 AST 的结构性质。

**6. 与实现的对应（Implementation Mapping）**

- 值域 $D$ ↔ `Value`（NaN-boxing），参见 [src/nanbox.hpp](src/nanbox.hpp)。
- 环境 $\Gamma$ ↔ `Env` 链（`Env` 对象与 `EnvGuard` 的生存期管理通过非原子引用计数 `RcBase` 实现），参见 [src/nanbox.cpp](src/nanbox.cpp) 与 `RcBase` 的实现文件。
- 解析器（AST 构造）↔ [src/helpers.cpp](src/helpers.cpp) 与 [grammar.ebnf](grammar.ebnf)。
- 求值规则 ↔ [src/vdlisp.cpp](src/vdlisp.cpp)、[src/core.cpp](src/core.cpp) 中的大量 `eval` / `apply` / special-form` 处理代码。
- JIT 行为 ↔ [src/jit/](src/jit/)：JIT 编译器基于运行时类型信息（“参数全为 number”）触发，产生可直接调用的本地代码；在出现非数值或异常路径时回退解释器。

**7. 示例归纳：小型证明 / 推理目标**

- 例 1（car-cons 逆律）：

  定理：对于所有 $a,b\in D$，在结构 M 中成立 $car(cons(a,b)) = a$。

  证明（语义直接由构造/投影公理）：由 `cons` 的定义及投影律立即成立。

- 例 2（词法闭包）：

  设 $f=(fn\,(x)\; (fn\,(y)\; (+ x y)))$，证明在任意环境 $\Gamma$ 下，$f$ 的内部函数引用的 `x` 来自外层定义环境而非调用环境（词法作用域）。

  证明思路：按 LAMBDA 和 APP 规则，内部闭包在定义时捕获当前 $\Gamma$ 并随闭包存储，后续调用时应用捕获环境扩展绑定 `y`；$x$ 的解释继续由捕获环境决定，从而与调用处的同名绑定无关。

**8. 建议的后续形式化工作**

- 用简单的归约语义/操作语义（small-step semantics）形式化宏展开与求值的相互作用（便于证明终止或观察中间状态）
- 将 JIT 触发条件形式化为某些谓词（如 $AllNum(e_i)$）并研究“JIT 与回退”在模型上的一致性条件
- 编写 Coq/Isabelle 风格的机械化规范：定义 AST、环境与大步语义、公理化 `cons/car`、证明若干性质（词法捕获、不变量保持等）

**9. 参考实现位置（仓库内）**

- 解释器与核心：[src/vdlisp.cpp](src/vdlisp.cpp), [src/core.cpp](src/core.cpp)
- 值表示（NaN-boxing）：[src/nanbox.hpp](src/nanbox.hpp), [src/nanbox.cpp](src/nanbox.cpp)
- JIT：[src/jit/](src/jit/)
- 词法/语法定义：[grammar.ebnf](grammar.ebnf)
- 启动/REPL：[src/main.cpp](src/main.cpp)

