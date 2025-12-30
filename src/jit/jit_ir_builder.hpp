#ifndef JIT_JIT_IR_BUILDER_HPP
#define JIT_JIT_IR_BUILDER_HPP

#include <llvm/IR/Function.h>
#include <string>

namespace llvm { class Module; class LLVMContext; }
namespace vdlisp { class FuncData; }

auto build_func_ir(vdlisp::FuncData* func, llvm::Module &M, llvm::LLVMContext &context, const std::string &name) -> llvm::Function*;

#endif // JIT_JIT_IR_BUILDER_HPP
