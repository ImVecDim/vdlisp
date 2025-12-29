#include "nanbox.hpp"
#include "vdlisp.hpp"
#include <limits>
#include <iostream>

using namespace vdlisp;

extern "C" auto VDLISP__call_from_jit(void* funcdata_ptr, double* args, int argc) -> double {
    try {
        State* S = jit_active_state;
        if (!S) return std::numeric_limits<double>::quiet_NaN();
        auto* fd = reinterpret_cast<FuncData*>(funcdata_ptr);
        if (!fd) return std::numeric_limits<double>::quiet_NaN();
        Value fptr = S->make_pooled_value(TFUNC);
        fptr.set_func(fd);
        Value head;
        Value *last = &head;
        for (int i = 0; i < argc; ++i) {
            Value num = S->make_number(args[i]);
            *last = S->make_pair(num, Value());
            PairData *pd = (*last).get_pair();
            last = &pd->cdr;
        }
        Value res = S->call(fptr, head, nullptr);
        if (!res || res.get_type() != TNUMBER) return std::numeric_limits<double>::quiet_NaN();
        return res.get_number();
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}
