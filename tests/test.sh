#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# Build to ensure binary is present (use CMake)
if [ ! -d build ]; then
  cmake -S . -B build -D ENABLE_LTO=OFF
fi
cmake --build build -j$(nproc)


# Interpreter binary (allow override)
VDLISP__BIN=${VDLISP__BIN:-build/vdlisp}

# Pool lifecycle test: run the interpreter on a script that performs many allocations
{
  echo "Running pool lifecycle test (via interpreter)..."
  if "$VDLISP__BIN" tests/pool_test.lisp 2>&1 | grep -q "pool_test_ok"; then
    echo "ok: pool lifecycle test -> pool_test_ok"
  else
    echo "FAILED: pool lifecycle test failed (interpreter did not print pool_test_ok)"
    exit 1
  fi
}

# Test cases: each item is "<expr>" "<expected>".
# If expected starts with 'err:' we assert the interpreter prints an error
# containing the substring after 'err:'. For error cases we also assert the
# source filename and source line are shown along with a caret pointing to
# the column.
TESTS=(
  # Arithmetic
  '(+ 1 2)' '3'
  '(- 10 3)' '7'
  '(* 2 3)' '6'
  '(/ 12 3)' '4'

  # Lists and printing
  '(list 1 2 3)' '(1 2 3)'
  '(cons 1 (list 2 3))' '(1 2 3)'
  '(car (list 1 2))' '1'
  '(cdr (list 1 2))' '(2)'

  # Types and builtins
  '(type "hi")' 'string'
  '(type (list))' 'nil'

  # Equality / identity
  '(< 1 2)' '#t'
  '(> 3 2)' '#t'
  '(< 1 2 3)' 'err:< requires exactly two arguments'
  '(> 3 2 1)' 'err:> requires exactly two arguments'
  '(= 1 1 1)' 'err:= requires exactly two arguments'

  # apply / higher-order
  '(apply + (list 1 2))' '3'

  # Parsing and strings (including escapes)
  '(parse "(+ 1 2)")' '(+ 1 2)'
  '(parse "\"a\\\"b\"")' 'a"b'

  # Modules / require
  $'(require "tests/mod.lisp")\n(type __req_test)' 'number'
  '(require "no-such-file-123.lisp")' 'err:could not open file'

  # Quote, fn, macro
  "'(1 2 3)" '(1 2 3)'
  "'(x)" '(x)'
  "'x" 'x'
  "'(fn (x) (+ x 1))" '(fn (x) (+ x 1))'
  $'(set f (fn (x) (+ x 1)))\n(f 3)' '4'

  # Macro expansion behavior
  $'(set m (macro (x) (list + x x)))\n(m 3)' '6'
  # quoted call should remain unevaluated
  $'(set m (macro (x) (list + x x)))\n\'(m 3)' '(m 3)'
  # Macro expansion error should report the call site, not the macro def
  $'(set m (macro () (/ 1 0)))\n(m)' 'err:division by zero'
  # Error from evaluating expanded code should point to call site
  $'(set m (macro (x) (list '\''f x)))\n(m 1)' 'err:unbound symbol'
  # Call chain should include macro calls
  $'(set m (macro () (list f)))\n(m)' 'err:Call chain'
  $'(set f (fn (x) (cond ((> x 0) 1) (1 0))))\n(f 1)' '1'

  # Dotted-tail (variadic) tests: functions and macros
  $'(set f (fn (a b . rest) (list a b rest)))\n(f 1 2 3 4)' '(1 2 (3 4))'
    $'(set m (macro (a . rest) (list + a (car rest))))\n(m 1 2 3)' '3'

  # Conditionals, set, let, while
  $'(cond ((> 2 3) 1) ((< 2 3) 2))' '2'
  $'(cond ((> 1 2) 1) ((< 1 2) (+ 1 2)))' '3'
  $'(cond (#t 42))' '42'
  $'(set x 5)\nx' '5'
  $'(let (x 1 y 2) (+ x y))' '3'
  $'(let (i 0) (while (< i 3) (set i (+ i 1))) i)' '3'

  # Mutation helpers
  $'(set p (cons 1 (list 2 3))) (setcar p 4) (car p)' '4'

  # Quasiquote / unquote
  '`(a ,(+ 1 2))' '(a 3)'
  '(set y 10) `(foo ,y bar)' '(foo 10 bar)'
  '`(1 ,(+ 2 3) 4)' '(1 5 4)'

  # Nested JIT calls (numeric path)
  $'(set h (fn (x) (+ x 1)))\n(set g (fn (x) (h (+ x 2))))\n(set f (fn (x) (g (+ x 3))))\n(f 10)' '16'

  # Nested call where inner returns non-number -> triggers fallback
  $'(set h (fn (x) (list x)))\n(set g (fn (x) (+ (car (h x)) 1)))\n(set f (fn (x) (g (+ x 3))))\n(f 5)' '9'

  # JIT representation checks: function should be reported as a function initially,
  # and as a jit_func / <jit_func> after real JIT compilation is triggered. We trigger
  # compilation by calling the function multiple times so the runtime's heuristic
  # or JIT driver can compile it.
  $'(set f (fn (x) (+ x 1)))\n(type f)' 'function'
  $'(set f (fn (x) (+ x 1)))\n(f 1)\n(f 2)\n(f 3)\n(f 4)\n(f 5)\n(type f)' 'jit_func'
  $'(set f (fn (x) (+ x 1)))\n(f 1)\n(f 2)\n(f 3)\n(f 4)\n(f 5)\n(print f)' '<jit_func>'

  # JIT with external numeric variable (free var lookup)
  $'(set y 10)\n(set f (fn (x) (+ x y)))\n(f 1)\n(f 1)\n(f 1)\n(f 1)\n(f 1)\n(type f)' 'jit_func'

  # Error cases
  '(parse 1)' 'err:parse requires a string'
  '(apply)' 'err:apply requires a function'
  '(/ 1 0)' 'err:division by zero'

  # Parse errors should include file/line/col + caret
  '(' 'err:unexpected EOF while reading list'
  '"abc' 'err:unexpected EOF while reading string'
  ')' 'err:unexpected )'
)

run_one() {
  local expr="$1" expected="$2"
  local tmpf
  tmpf=$(mktemp --suffix=.lisp)
  printf "%s" "$expr" > "$tmpf"
  local out
  out=$("$VDLISP__BIN" "$tmpf" 2>&1 || true)

  # capture last non-empty line for normal checks
  local last
  last=$(echo "$out" | sed '/^$/d' | tail -n 1 || true)
  last=$(echo "$last" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
  # gather first line of source for caret check
  local srcline
  srcline=$(head -n 1 "$tmpf" || true)
  local base
  base=$(basename "$tmpf")
  rm -f "$tmpf"

  if [[ "$expected" == err:* ]]; then
    local substr="${expected#err:}"
    if ! echo "$out" | grep -Fq "$substr"; then
      echo "FAILED (expected error): $expr"
      echo "  expected to contain: '$substr'"
      echo "  got               : '$out'"
      exit 1
    fi
    # expect the filename to appear in the error output
    if ! echo "$out" | grep -Fq "$base"; then
      echo "FAILED (expected filename in error): $expr"
      echo "  expected to contain filename: '$base'"
      echo "  got                       : '$out'"
      exit 1
    fi
    # expect the source line to be echoed
    if ! echo "$out" | grep -Fq "$srcline"; then
      echo "FAILED (expected source line in error): $expr"
      echo "  expected source line: '$srcline'"
      echo "  got                : '$out'"
      exit 1
    fi
    # caret presence (allow optional ANSI color sequences before '^')
    if ! echo "$out" | grep -Eq $'^[[:space:]]*(\033\[[0-9;]*m)?\\^'; then
      echo "FAILED (expected caret in error): $expr"
      echo "  expected a line containing '^' under the source line (optionally colored)"
      echo "  got: '$out'"
      exit 1
    fi
    echo "ok (error): $expr -> $last"
  else
    if [[ "$last" != "$expected" ]]; then
      echo "FAILED: $expr"
      echo "  expected: '$expected'"
      echo "  got     : '$last'"
      echo "full output:\n$out"
      exit 1
    else
      echo "ok: $expr -> $last"
    fi
  fi
}

# Run tests
for ((i=0;i<${#TESTS[@]};i+=2)); do
  run_one "${TESTS[i]}" "${TESTS[i+1]}"
done

# Run JIT control forms script to exercise cond/let/while compiled paths
{
  echo "Running JIT control forms script..."
  out=$("$VDLISP__BIN" tests/jit_control_forms.lisp 2>&1 || true)
  if ! echo "$out" | grep -Fq "COND_DONE"; then
    echo "FAILED: jit control forms (cond)"; echo "$out"; exit 1; fi
  if ! echo "$out" | grep -Fq "LET_DONE"; then
    echo "FAILED: jit control forms (let)"; echo "$out"; exit 1; fi
  if ! echo "$out" | grep -Fq "WHILE_DONE"; then
    echo "FAILED: jit control forms (while)"; echo "$out"; exit 1; fi
  if ! echo "$out" | grep -Fq "jit_func"; then
    echo "FAILED: JIT not triggered"; echo "$out"; exit 1; fi
  if ! echo "$out" | grep -Fq "<jit_func>"; then
    echo "FAILED: JIT print form not found"; echo "$out"; exit 1; fi
  echo "ok: jit control forms script"
}

echo "All tests passed."