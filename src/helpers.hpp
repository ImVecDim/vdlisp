#ifndef VDLISP__HELPERS_HPP
#define VDLISP__HELPERS_HPP

#include "vdlisp.hpp"

#include <stdexcept>
#include <utility>

namespace vdlisp {

struct ParseError : public std::runtime_error {
    State::SourceLoc loc;
    std::vector<State::SourceLoc> call_chain;
    ParseError(State::SourceLoc loc, const std::string &msg)
        : std::runtime_error(msg), loc(std::move(loc)) {}
    ParseError(State::SourceLoc loc, const std::string &msg, std::vector<State::SourceLoc> chain)
        : std::runtime_error(msg), loc(std::move(loc)), call_chain(std::move(chain)) {}
};

// helpers from the interpreter moved out into a separate translation unit
void print_error_with_loc(const State &S, const State::SourceLoc &loc, const std::string &msg);

[[nodiscard]] auto value_equal(const Value &a, const Value &b) -> bool;

// Small helpers (inlined for performance)
[[nodiscard]] inline __attribute__((always_inline)) auto type_name(const Value &v) -> std::string {
    if (!v)
        return std::string("nil");
    return v.type_name();
}

[[nodiscard]] inline __attribute__((always_inline)) auto require_number(const Value &v, const char *who) -> double {
    if (!v || v.get_type() != TNUMBER) [[unlikely]]
        throw std::runtime_error(std::string(who) + std::string(": expected number, got ") + std::string(type_name(v)));
    return v.get_number();
}

// Small helpers to reduce repetitive get_type() checks
[[nodiscard]] inline __attribute__((always_inline)) auto pair_car(const Value &p) noexcept -> Value {
    if (!p)
        return {};
    if (p.get_type() != TPAIR)
        return {};
    return p.get_pair()->car;
}
[[nodiscard]] inline __attribute__((always_inline)) auto pair_cdr(const Value &p) noexcept -> Value {
    if (!p)
        return {};
    if (p.get_type() != TPAIR)
        return {};
    return p.get_pair()->cdr;
}
[[nodiscard]] inline __attribute__((always_inline)) auto is_pair(const Value &p) noexcept -> bool {
    return p && p.get_type() == TPAIR;
}
[[nodiscard]] inline __attribute__((always_inline)) auto is_symbol(const Value &p, const std::string &name) -> bool {
    return p && p.get_type() == TSYMBOL && *p.get_symbol() == name;
}
inline __attribute__((always_inline)) void pair_set_car(const Value &p, const Value &v) noexcept {
    if (!p)
        return;
    if (p.get_type() != TPAIR)
        return;
    p.get_pair()->car = v;
}
inline __attribute__((always_inline)) void pair_set_cdr(const Value &p, const Value &v) noexcept {
    if (!p)
        return;
    if (p.get_type() != TPAIR)
        return;
    p.get_pair()->cdr = v;
}

// Clear closure_env held by TFUNC/TMACRO Values: release the Env and null the pointer.
void clear_closure_env(Value &v) noexcept;

} // namespace vdlisp

#endif