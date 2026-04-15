#ifndef JIT_JIT_HPP
#define JIT_JIT_HPP

#include <functional>
#include <limits>
#include <llvm/IR/LLVMContext.h>
#include <memory>
#include <string>
#include <unordered_map>

#include "vdlisp.hpp"

namespace llvm {
class ExecutionEngine;
class Function;
class Module;
} // namespace llvm

namespace vdlisp {
class FuncData;
}

class JITCompiler {
  public:
        // 一个进程内共享一个 LLVM 执行引擎，按函数粒度动态生成机器码。
    JITCompiler();
    ~JITCompiler() noexcept;

    [[nodiscard]] auto compileFunctionFromBuilder(const std::function<llvm::Function *(llvm::Module &)> &builder) -> void *;
    [[nodiscard]] auto getContext() noexcept -> llvm::LLVMContext &;
    [[nodiscard]] auto compileFuncData(vdlisp::FuncData *func) -> void *;
    auto releaseFunctionCode(void *fnPtr) noexcept -> void;

  private:
    llvm::LLVMContext context;
    std::unique_ptr<llvm::ExecutionEngine> executionEngine;
    std::unordered_map<void *, llvm::Module *> module_for_fn;
};

// 下面几个 bridge 函数是 JIT 机器码和解释器运行时之间的窄接口。

extern "C" [[nodiscard]] inline auto VDLISP__call_from_jit(void *funcdata_ptr, double *args, int argc) noexcept -> double {
    try {
        vdlisp::State *S = vdlisp::jit_active_state;
        if (!S)
            return std::numeric_limits<double>::quiet_NaN();
        auto *fd = reinterpret_cast<vdlisp::FuncData *>(funcdata_ptr);
        if (!fd)
            return std::numeric_limits<double>::quiet_NaN();
        vdlisp::Value fptr = S->make_pooled_value(vdlisp::TFUNC);
        fptr.set_func(fd);
        vdlisp::Value head;
        vdlisp::Value *last = &head;
        // 机器码只处理 double 数组，这里再封装回 Lisp 层的参数链表。
        for (int i = 0; i < argc; ++i) {
            vdlisp::Value num = S->make_number(args[i]);
            *last = S->make_pair(std::move(num), vdlisp::Value());
            vdlisp::PairData *pd = (*last).get_pair();
            last = &pd->cdr;
        }
        vdlisp::Value res = S->call(fptr, head, nullptr);
        if (!res || res.get_type() != vdlisp::TNUMBER)
            return std::numeric_limits<double>::quiet_NaN();
        return res.get_number();
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

extern "C" [[nodiscard]] inline auto VDLISP__call_interpreted_from_jit(void *funcdata_ptr, double *args, int argc) noexcept -> double {
    try {
        vdlisp::State *S = vdlisp::jit_active_state;
        if (!S)
            return std::numeric_limits<double>::quiet_NaN();
        auto *fd = reinterpret_cast<vdlisp::FuncData *>(funcdata_ptr);
        if (!fd)
            return std::numeric_limits<double>::quiet_NaN();

        // 强制临时关掉这次调用的 compiled_code，确保回退路径不会再次跳回 JIT。
        void *saved_code = fd->compiled_code;
        bool saved_jit_failed = fd->jit_failed;
        fd->compiled_code = nullptr;
        fd->jit_failed = true;
        double result = VDLISP__call_from_jit(funcdata_ptr, args, argc);
        fd->compiled_code = saved_code;
        fd->jit_failed = saved_jit_failed;
        return result;
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

// 自由变量查询只支持 number；查不到或类型不符都返回 NaN，交给调用方决定是否回退。
extern "C" [[nodiscard]] inline auto VDLISP__jit_lookup_number(void *env_ptr, const char *name) noexcept -> double {
    try {
        if (!name)
            return std::numeric_limits<double>::quiet_NaN();
        vdlisp::Env *e = reinterpret_cast<vdlisp::Env *>(env_ptr);
        // If no closure env was captured, fall back to the currently-active state.
        if (!e) {
            vdlisp::State *S = vdlisp::jit_active_state;
            if (S)
                e = S->global;
        }
        if (!e)
            return std::numeric_limits<double>::quiet_NaN();

        const std::string key{name};
        for (vdlisp::Env *cur = e; cur; cur = cur->parent) {
            auto it = cur->map.find(key);
            if (it == cur->map.end())
                continue;
            const vdlisp::Value &v = it->second;
            if (!v || v.get_type() != vdlisp::TNUMBER)
                return std::numeric_limits<double>::quiet_NaN();
            return v.get_number();
        }
        return std::numeric_limits<double>::quiet_NaN();
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

extern JITCompiler global_jit;

#endif // JIT_JIT_HPP
