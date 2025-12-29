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

auto value_equal(Value a, Value b) -> bool;

auto type_name(Value v) -> std::string;

auto require_number(Value v, const char *who) -> double;

// Small helpers to reduce repetitive get_type() checks
auto pair_car(Value p) -> Value;
auto pair_cdr(Value p) -> Value;
auto is_pair(Value p) -> bool;
auto is_symbol(Value p, const std::string &name) -> bool;
void pair_set_car(Value p, Value v);
void pair_set_cdr(Value p, Value v);

} // namespace vdlisp

#endif