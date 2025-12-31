#ifndef JIT_JIT_HPP
#define JIT_JIT_HPP

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>
#include <memory>
#include <string>
#include <functional>

#include "nanbox.hpp"
#include "vdlisp.hpp"
#include <unordered_map>
#include <limits>

class JITCompiler {
public:
    JITCompiler();
    ~JITCompiler() noexcept;

    auto compileFunctionFromBuilder(const std::function<llvm::Function*(llvm::Module&)>& builder) -> void*;
    auto getContext() noexcept -> llvm::LLVMContext&;
    auto compileFuncData(vdlisp::FuncData* func) -> void*;
    void releaseFunctionCode(void* fnPtr);

private:
    llvm::LLVMContext context;
    std::unique_ptr<llvm::ExecutionEngine> executionEngine;
    std::unordered_map<void*, llvm::Module*> module_for_fn;
};

// Global shared JIT instance used by the runtime; tests may rely on this being
// available to trigger compilation consistently.

extern "C" inline auto VDLISP__call_from_jit(void* funcdata_ptr, double* args, int argc) noexcept -> double {
    try {
        vdlisp::State* S = vdlisp::jit_active_state;
        if (!S) return std::numeric_limits<double>::quiet_NaN();
        auto* fd = reinterpret_cast<vdlisp::FuncData*>(funcdata_ptr);
        if (!fd) return std::numeric_limits<double>::quiet_NaN();
        vdlisp::Value fptr = S->make_pooled_value(vdlisp::TFUNC);
        fptr.set_func(fd);
        vdlisp::Value head;
        vdlisp::Value *last = &head;
        for (int i = 0; i < argc; ++i) {
            vdlisp::Value num = S->make_number(args[i]);
            *last = S->make_pair(num, vdlisp::Value());
            vdlisp::PairData *pd = (*last).get_pair();
            last = &pd->cdr;
        }
        vdlisp::Value res = S->call(fptr, head, nullptr);
        if (!res || res.get_type() != vdlisp::TNUMBER) return std::numeric_limits<double>::quiet_NaN();
        return res.get_number();
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

extern JITCompiler global_jit;

#endif // JIT_JIT_HPP
