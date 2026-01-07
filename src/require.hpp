#ifndef VDLISP__REQUIRE_HPP
#define VDLISP__REQUIRE_HPP

#include "helpers.hpp"
#include "vdlisp.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace vdlisp {

inline __attribute__((always_inline)) void register_require(State &S) {
    // require: load a file and return its value; cache results using canonical paths and provide
    // better diagnostic info on failure
    S.register_builtin("require", [](State &S, const Value &args) -> Value {
        if (!args || !pair_car(args) || pair_car(args).get_type() != TSTRING)
            throw std::runtime_error("require requires a string");
        std::string name = *pair_car(args).get_string();

        // Build candidate paths: prefer caller-relative then the raw name
        State::SourceLoc loc;
        std::vector<std::string> candidates;
        if (!name.empty() && name[0] != '/') {
            if (S.current_expr && S.get_source_loc(S.current_expr, loc) && !loc.file.empty()) {
                auto p = loc.file;
                auto pos = p.find_last_of('/');
                if (pos != std::string::npos)
                    candidates.push_back(p.substr(0, pos + 1) + name);
            }
            candidates.push_back(name);
        } else {
            candidates.push_back(name);
        }

        std::error_code ec;
        std::vector<std::string> tried;

        for (const auto &cand : candidates) {
            std::filesystem::path fp(cand);
            std::string key = cand;
            if (std::filesystem::exists(fp, ec)) {
                auto can = std::filesystem::canonical(fp, ec);
                if (!ec)
                    key = can.string();
                else
                    key = std::filesystem::absolute(fp, ec).string();
            }
            // if module already loaded under canonical key, return it
            auto it = S.loaded_modules.find(key);
            if (it != S.loaded_modules.end())
                return it->second;
            // try opening candidate (prefer canonical/absolute path when available)
            std::ifstream f;
            if (!key.empty() && std::filesystem::exists(std::filesystem::path(key), ec))
                f.open(key);
            else
                f.open(cand);
            if (!f) {
                tried.push_back(key);
                continue;
            }
            // mark as loading to guard against cycles
            S.loaded_modules[key] = Value();
            std::ostringstream ss;
            ss << f.rdbuf();
            Value e = S.parse_all(ss.str(), key);
            Value r;
            if (e)
                r = S.do_list(e, S.global);
            S.loaded_modules[key] = r;
            return r;
        }

        // If none succeeded, include all tried/attempted paths in the error message
        std::ostringstream msg;
        msg << "could not open file: " << name << " (tried: ";
        for (size_t i = 0; i < tried.size(); ++i) {
            if (i)
                msg << ", ";
            msg << tried[i];
        }
        msg << ")";
        throw std::runtime_error(msg.str());
    });
}

} // namespace vdlisp

#endif // VDLISP__REQUIRE_HPP
