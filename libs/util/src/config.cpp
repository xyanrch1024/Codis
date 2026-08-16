#include "config.h"

#include <toml++/toml.h>
#include <fstream>
#include <iostream>

namespace codis {

AppConfig AppConfig::load(const std::filesystem::path& path) {
    AppConfig cfg;
    try {
        auto tbl = toml::parse_file(path.string());

        if (auto providers = tbl["providers"].as_array()) {
            for (auto& node : *providers) {
                auto& p = *node.as_table();
                ProviderConfig pc;
                pc.name        = p["name"].value<std::string>().value_or("");
                pc.api_key     = p["api_key"].value<std::string>().value_or("");
                pc.api_key_env = p["api_key_env"].value<std::string>().value_or("");
                pc.model       = p["model"].value<std::string>().value_or("");
                pc.base_url    = p["base_url"].value<std::string>().value_or("");
                pc.resolve_api_key();
                if (!pc.name.empty() && !pc.api_key.empty()) {
                    cfg.providers.push_back(std::move(pc));
                }
            }
        }

        if (auto llm = tbl["llm"].as_table()) {
            cfg.llm.max_tokens   = (*llm)["max_tokens"].value<int>();
        }

        cfg.default_provider = tbl["default_provider"].value<std::string>().value_or("");
        cfg.timeout_seconds  = tbl["timeout_seconds"].value<int>().value_or(60);

        if (auto perm = tbl["permissions"].as_table()) {
            if (auto a = (*perm)["allow"].as_array())
                for (auto& v : *a)
                    if (auto s = v.value<std::string>()) cfg.permissions.allow.push_back(*s);
            if (auto a = (*perm)["ask"].as_array())
                for (auto& v : *a)
                    if (auto s = v.value<std::string>()) cfg.permissions.ask.push_back(*s);
            if (auto a = (*perm)["deny"].as_array())
                for (auto& v : *a)
                    if (auto s = v.value<std::string>()) cfg.permissions.deny.push_back(*s);
            cfg.permissions.confirm_timeout_seconds =
                (*perm)["confirm_timeout"].value<int>().value_or(120);
        }

        if (auto ws = tbl["websearch"].as_table()) {
            cfg.websearch.backend = (*ws)["backend"].value<std::string>().value_or("bing");
            cfg.websearch.api_key = (*ws)["api_key"].value<std::string>().value_or("");
            cfg.websearch.api_key_env = (*ws)["api_key_env"].value<std::string>().value_or("");
            cfg.websearch.max_results = (*ws)["max_results"].value<int>().value_or(5);
            cfg.websearch.timeout_seconds = (*ws)["timeout_seconds"].value<int>().value_or(15);
            cfg.websearch.resolve_api_key();
        }

    } catch (const toml::parse_error& e) {
        std::cerr << "Config parse error: " << e.what() << "\n";
    }
    return cfg;
}

AppConfig AppConfig::default_config() {
    AppConfig cfg;
    return cfg;
}

} // namespace codis
