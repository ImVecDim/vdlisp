#ifndef VDLISP__REQUIRE_HPP
#define VDLISP__REQUIRE_HPP

#include "../../include/vdlisp.hpp"

#include <boost/container/small_vector.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace vdlisp {

// 注册模块加载内建 `require`。
inline auto register_require(State &S) -> void {
    S.register_builtin("require", [](State &S, const Value &args) -> Value {
        if (!args || !pair_car(args) || pair_car(args).get_type() != TSTRING)
            throw LispError("require requires a string");
        std::string name = *pair_car(args).get_string();

        SourceLoc loc;
        boost::container::small_vector<std::string, 4> candidates;
        // 相对路径优先相对当前脚本，再退回进程工作目录。
        if (!name.empty() && name[0] != '/' && !(name.size() > 2 && name[1] == ':' && (name[2] == '/' || name[2] == '\\'))) {
            if (S.current_expression() && S.get_source_loc(S.current_expression(), loc) && !loc.file.empty()) {
                auto p = loc.file;
                auto pos = p.find_last_of("/\\");
                if (pos != std::string::npos)
                    candidates.push_back(std::string(p.substr(0, pos + 1)) + name);
            }
            candidates.push_back(name);
        } else {
            candidates.push_back(name);
        }

        std::error_code ec;
        boost::container::small_vector<std::string, 4> tried;

        for (const auto &cand : candidates) {
            // 规范化路径后再查缓存，避免同一个模块被不同相对路径重复加载。
            std::filesystem::path fp(cand);
            std::string key = cand;
            if (std::filesystem::exists(fp, ec)) {
                auto can = std::filesystem::canonical(fp, ec);
                if (!ec)
                    key = can.string();
                else
                    key = std::filesystem::absolute(fp, ec).string();
            }
            if (auto *v = S.module_loaded(key))
                return *v;

            // 直接打开，无需重复 exists 检查
            std::ifstream f(key);
            if (!f) {
                tried.push_back(key);
                continue;
            }

            // 先写入一个占位值，可打断简单的循环 require。
            S.set_module(key, Value());
            std::ostringstream ss;
            ss << f.rdbuf();
            Value e = S.parse_all(ss.str(), key);
            Value r;
            if (e)
                r = S.do_list(e, S.global_env());
            S.set_module(key, r);
            return r;
        }

        std::ostringstream msg;
        msg << "could not open file: " << name << " (tried: ";
        for (size_t i = 0; i < tried.size(); ++i) {
            if (i)
                msg << ", ";
            msg << tried[i];
        }
        msg << ")";
        throw LispError(msg.str());
    });
}

} // namespace vdlisp

#endif // VDLISP__REQUIRE_HPP
