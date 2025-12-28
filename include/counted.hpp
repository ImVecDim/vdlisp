// Deprecated compatibility header.
// The `counted` abstraction has been replaced by `sptr<T>` and the primary
// header is now `sptr.hpp`. New code should include `sptr.hpp` and use
// `sptr<T>` (e.g. `sptr<Value>`, `sptr<Env>`). This file remains temporarily
// to avoid breaking existing include paths; it will be removed in a future
// commit. Do not add new references to `counted`.

#pragma once
#include "sptr.hpp"

