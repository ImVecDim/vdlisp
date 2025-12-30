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

// Forward declare FuncData to avoid including nanbox.hpp here.
namespace vdlisp { class FuncData; }

class JITCompiler {
public:
    JITCompiler();
    ~JITCompiler();

    auto compileFunctionFromBuilder(const std::function<llvm::Function*(llvm::Module&)>& builder) -> void*;
    auto getContext() -> llvm::LLVMContext&;
    auto compileFuncData(vdlisp::FuncData* func) -> void*;
    void releaseFunctionCode(void* fnPtr);

private:
    llvm::LLVMContext context;
    std::unique_ptr<llvm::ExecutionEngine> executionEngine;
    std::unordered_map<void*, llvm::Module*> module_for_fn;
};

// Global shared JIT instance used by the runtime; tests may rely on this being
// available to trigger compilation consistently.
extern JITCompiler global_jit;

#endif // JIT_JIT_HPP
