#ifndef VDLIST_HELPERS_HPP
#define VDLIST_HELPERS_HPP

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

auto value_equal(Ptr a, Ptr b) -> bool;

auto type_name(Ptr v) -> std::string;

auto require_number(Ptr v, const char *who) -> double;

// Small helpers to reduce repetitive get_type() checks
auto pair_car(Ptr p) -> Ptr;
auto pair_cdr(Ptr p) -> Ptr;
auto is_pair(Ptr p) -> bool;
auto is_symbol(Ptr p, const std::string &name) -> bool;
void pair_set_car(Ptr p, Ptr v);
void pair_set_cdr(Ptr p, Ptr v);

} // namespace vdlisp

#endif