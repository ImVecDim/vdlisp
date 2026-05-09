// jit.cpp (moved into src/jit)
#include "jit/jit.hpp"
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include "helpers.hpp"
#include "jit/jit_ir_builder.hpp"
#include "nanbox.hpp"
#include <unordered_map>

JITCompiler::JITCompiler() {
  // 初始化 LLVM 的本机目标，让 ExecutionEngine 能直接产出当前平台机器码。
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  auto m = std::make_unique<llvm::Module>("jit_module", context);

  std::string error;
  executionEngine = std::unique_ptr<llvm::ExecutionEngine>(
      llvm::EngineBuilder(std::move(m))
          .setErrorStr(&error)
          .setEngineKind(llvm::EngineKind::JIT)
          .create());

  if (!executionEngine) {
    throw vdlisp::LispError("ExecutionEngine creation failed: " + error);
  }
}

JITCompiler::~JITCompiler() noexcept = default;

// Concrete global JIT instance used by the runtime
JITCompiler global_jit;

auto JITCompiler::compileFunctionFromBuilder(
    const std::function<llvm::Function *(llvm::Module &)> &builder) -> void * {
  std::string mname = "jit_module";
  auto m = std::make_unique<llvm::Module>(mname, context);

  llvm::Function *f = builder(*m);
  if (f == nullptr) {
    return nullptr;
  }

  llvm::Module *mptr = m.get();

  // 如果 IR 用到了运行时桥接函数，需要先把符号映射给 ExecutionEngine。
  if (llvm::Function *bridge = mptr->getFunction("VDLISP__call_from_jit")) {
    executionEngine->addGlobalMapping(
        bridge, reinterpret_cast<void *>(VDLISP__call_from_jit));
  }
  if (llvm::Function *interp_bridge =
          mptr->getFunction("VDLISP__call_interpreted_from_jit")) {
    executionEngine->addGlobalMapping(
        interp_bridge,
        reinterpret_cast<void *>(VDLISP__call_interpreted_from_jit));
  }

  // 自由变量读取辅助函数同样需要显式映射。
  if (llvm::Function *lookup = mptr->getFunction("VDLISP__jit_lookup_number")) {
    executionEngine->addGlobalMapping(
        lookup, reinterpret_cast<void *>(VDLISP__jit_lookup_number));
  }

  executionEngine->addModule(std::move(m));
  executionEngine->finalizeObject();
  void *ptr = executionEngine->getPointerToFunction(f);
  if (ptr != nullptr) {
    module_for_fn[ptr] = mptr;
  }
  return ptr;
}

auto JITCompiler::releaseFunctionCode(void *fnPtr) noexcept -> void {
  if (!fnPtr) return;
  auto it = module_for_fn.find(fnPtr);
  if (it == module_for_fn.end()) return;
  llvm::Module *mptr = it->second;
  try { (void)executionEngine->removeModule(mptr); } catch (...) {}
  std::erase_if(module_for_fn, [mptr](const auto &p) { return p.second == mptr; });
}

auto JITCompiler::getContext() noexcept -> llvm::LLVMContext & {
  return context;
}

static auto collect_called_funcs(const vdlisp::Value &expr,
                                 std::vector<vdlisp::FuncData *> &out,
                                 vdlisp::Env *closure) -> void {
  if (!expr || expr.get_type() != vdlisp::TPAIR) return;
  vdlisp::PairData *pd = expr.get_pair();
  const auto &car = pd->car;
  if (car && car.get_type() == vdlisp::TSYMBOL) {
    std::string name = *car.get_symbol();
    for (auto *e = closure; e; e = e->parent) {
      auto it = e->map.find(name);
      if (it != e->map.end()) {
        if (it->second && it->second.get_type() == vdlisp::TFUNC)
          out.push_back(it->second.get_func());
        break;
      }
    }
  }
  for (auto walk = &expr; *walk; walk = &walk->get_pair()->cdr)
    collect_called_funcs(walk->get_pair()->car, out, closure);
}

auto JITCompiler::compileFuncData(vdlisp::FuncData *func) -> void * {
  if (!func) return nullptr;

  std::vector<vdlisp::FuncData *> to_compile;
  collect_called_funcs(func->body, to_compile, func->closure_env);
  for (auto *fd : to_compile) {
    if (fd && fd != func && !fd->compiled_code && !fd->jit_failed) {
      try { (void)compileFuncData(fd); } catch (...) {}
    }
  }

  std::string fname = "jit_fn_" + std::to_string(reinterpret_cast<uintptr_t>(func));
  auto builder = [func, this, &fname](llvm::Module &M) -> llvm::Function * {
    return build_func_ir(func, M, this->getContext(), fname);
  };

  void *ptr = nullptr;
  try { ptr = compileFunctionFromBuilder(builder); }
  catch (...) { func->jit_failed = true; return nullptr; }
  if (!ptr) { func->jit_failed = true; return nullptr; }
  func->compiled_code = ptr;
  return ptr;
}
