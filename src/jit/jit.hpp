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

  [[nodiscard]] auto compileFunctionFromBuilder(
      const std::function<llvm::Function *(llvm::Module &)> &builder) -> void *;
  [[nodiscard]] auto getContext() noexcept -> llvm::LLVMContext &;
  [[nodiscard]] auto compileFuncData(vdlisp::FuncData *func) -> void *;
  auto releaseFunctionCode(void *fnPtr) noexcept -> void;

private:
  llvm::LLVMContext context;
  std::unique_ptr<llvm::ExecutionEngine> executionEngine;
  std::unordered_map<void *, llvm::Module *> module_for_fn;
};

namespace {

// 共享实现：把 double 数组参数包装成 Lisp 链表再调用解释器
[[nodiscard]] inline auto call_via_jit_bridge(void *funcdata_ptr, double *args, int argc) noexcept -> double {
  vdlisp::State *S = vdlisp::jit_active_state;
  if (S == nullptr) return std::numeric_limits<double>::quiet_NaN();
  auto *fd = reinterpret_cast<vdlisp::FuncData *>(funcdata_ptr);
  if (fd == nullptr) return std::numeric_limits<double>::quiet_NaN();

  vdlisp::Value fptr{vdlisp::TFUNC};
  fd->inc_ref();
  fptr.set_func(fd);

  vdlisp::ListBuilder lb;
  for (int i = 0; i < argc; ++i)
    lb.add(*S, S->make_number(args[i]));

  vdlisp::Value res = S->call(fptr, std::move(lb).done(), nullptr);
  if (!res || res.get_type() != vdlisp::TNUMBER)
    return std::numeric_limits<double>::quiet_NaN();
  return res.get_number();
}

} // namespace

extern "C" [[nodiscard]] inline auto
VDLISP__call_from_jit(void *funcdata_ptr, double *args, int argc) noexcept -> double {
  try { return call_via_jit_bridge(funcdata_ptr, args, argc); }
  catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
}

extern "C" [[nodiscard]] inline auto
VDLISP__call_interpreted_from_jit(void *funcdata_ptr, double *args,
                                  int argc) noexcept -> double {
  try {
    auto *fd = reinterpret_cast<vdlisp::FuncData *>(funcdata_ptr);
    if (fd == nullptr) return std::numeric_limits<double>::quiet_NaN();
    fd->inc_ref();
    void *saved_code = fd->compiled_code;
    bool saved_failed = fd->jit_failed;
    fd->compiled_code = nullptr;
    fd->jit_failed = true;
    double result = call_via_jit_bridge(funcdata_ptr, args, argc);
    fd->compiled_code = saved_code;
    fd->jit_failed = saved_failed;
    fd->dec_ref();
    return result;
  } catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
}

// 自由变量查询只支持 number；查不到或类型不符都返回
// NaN，交给调用方决定是否回退。
extern "C" [[nodiscard]] inline auto
VDLISP__jit_lookup_number(void *env_ptr, const char *name) noexcept -> double {
  try {
    if (!name) return std::numeric_limits<double>::quiet_NaN();
    auto *e = reinterpret_cast<vdlisp::Env *>(env_ptr);
    if (!e) {
      vdlisp::State *S = vdlisp::jit_active_state;
      if (S) e = S->global;
    }
    if (!e) return std::numeric_limits<double>::quiet_NaN();

    std::string key{name};
    for (auto *cur = e; cur; cur = cur->parent) {
      auto it = cur->map.find(key);
      if (it != cur->map.end()) {
        const auto &v = it->second;
        if (v && v.get_type() == vdlisp::TNUMBER)
          return v.get_number();
      }
    }
    return std::numeric_limits<double>::quiet_NaN();
  } catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
}

extern JITCompiler global_jit;

#endif // JIT_JIT_HPP
