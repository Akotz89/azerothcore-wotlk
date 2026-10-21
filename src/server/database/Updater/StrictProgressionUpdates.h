#ifndef STRICT_PROGRESSION_UPDATES_H
#define STRICT_PROGRESSION_UPDATES_H

#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Production state machine, shared with the injected-seam tests. The caller
// reads the complete ledger successfully and validates the compiled manifest
// against the actual files before invoking Run. No inverse SQL is inferred.
namespace StrictProgression
{
struct File { std::string path, hash; };
struct Entry { std::string hash, state; };
using Ledger = std::map<std::string, Entry>;
inline std::string const Marker = "__progression_incomplete__";
inline std::string const Native = "__progression_native__";
inline void Require(bool value, char const* reason)
{
    if (!value) throw std::runtime_error(reason);
}
inline std::string Name(File const& file)
{
    return file.path.substr(file.path.find_last_of('/') + 1);
}
inline bool Hex(std::string const& value, std::size_t size)
{
    return value.size() == size && value.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}
// This narrow read-only guard runs even when progression loading or every
// update flag is disabled. A missing ledger/query failure cannot prove absence.
// Query must return one COUNT row; nullopt means it could not do so.
template<class Query, class Enabled>
inline bool StartupEnabled(Query const& query, Enabled const& enabled)
{
    for (auto database : {"auth", "characters", "world"})
    {
        auto count = query(database, "SELECT COUNT(*) FROM `updates` WHERE `name`='__progression_incomplete__'");
        Require(count.has_value(), "Cannot prove progression incomplete marker absent: missing ledger or query failure");
        Require(*count == 0, "Incomplete progression import: prove exact database restore and explicitly clear marker before retry");
    }
    return enabled();
}
// write/erase MUST perform synchronous readback or throw. A failed apply or
// failed ledger confirmation leaves Marker intact, including after restart.
inline std::size_t Run(std::vector<File> const& files, std::string const& nativeIdentity,
    bool auditedAdoption, Ledger const& ledger,
    std::function<void(std::string const&, Entry const&)> const& write,
    std::function<void(std::string const&)> const& erase,
    std::function<void(File const&)> const& apply,
    std::vector<File> const& reviewedManifest,
    std::function<std::optional<Entry>(std::string const&)> const& fresh)
{
    Require(Hex(nativeIdentity, 64), "Missing compiled progression native identity");
    Require(!ledger.count(Marker), "Incomplete progression import: prove exact database restore and explicitly clear marker before retry");
    if (files.empty()) return 0;
    std::map<std::string, std::string> reviewed;
    for (auto const& file : reviewedManifest)
        Require(reviewed.emplace(Name(file), file.hash).second, "Duplicate reviewed progression basename");
    std::set<std::string> names;
    std::string previous;
    bool legacy = false;
    for (auto const& file : files)
    {
        auto name = Name(file);
        Require(!name.empty() && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == std::string::npos,
            "Unsafe progression filename");
        Require(names.insert(name).second && (previous.empty() || previous < name), "Duplicate or unordered progression manifest");
        Require(Hex(file.hash, 40), "Invalid progression SQL hash");
        previous = name;
        auto found = ledger.find(name);
        if (found != ledger.end())
        {
            Require(found->second.state == "MODULE" && found->second.hash == file.hash,
                "Changed/empty progression hash or unexpected ledger state: replay prohibited");
            legacy = true;
        }
        else
        {
            for (auto const& item : ledger)
                if (item.second.hash == file.hash)
                {
                    auto known = reviewed.find(item.first);
                    Require(known != reviewed.end() && known->second == item.second.hash && item.second.state == "MODULE",
                        "Unknown progression rename or duplicate hash in ledger: explicit reconciliation required");
                }
        }
    }
    std::string const nativeKey = Native + nativeIdentity;
    bool priorNative = false;
    for (auto const& item : ledger)
        if (item.first.compare(0, Native.size(), Native) == 0)
        {
            auto identity = item.first.substr(Native.size());
            Require(Hex(identity, 64) && item.second.state == "MODULE" && item.second.hash == identity.substr(0, 40),
                "Malformed progression native identity history");
            priorNative = true;
        }
    auto native = ledger.find(nativeKey);
    if (native != ledger.end())
        Require(native->second.state == "MODULE" && native->second.hash == nativeIdentity.substr(0, 40),
            "Loaded progression native identity differs from audited ledger identity");
    else
        Require((!legacy && !priorNative) || auditedAdoption, "Legacy ledger or native upgrade requires explicit audited adoption");
    // Native binding is separate from SQL identities. Recording it changes no
    // SQL hash, phase state, or update-enable configuration.
    if (native == ledger.end()) write(nativeKey, {nativeIdentity.substr(0, 40), "MODULE"});
    std::size_t imported = 0;
    for (auto const& file : files)
    {
        auto name = Name(file);
        if (ledger.count(name)) continue;
        // Marker uses existing valid MODULE enum and 40-character hash field.
        // A crash in any subsequent step requires operator recovery, even if
        // the external mysql process may have completed successfully.
        // Distinct ownership tokens prevent two startup processes whose
        // preflight reads overlapped from both claiming the same pending file.
        std::random_device random;
        std::string token;
        char const* hex = "0123456789ABCDEF";
        for (unsigned i = 0; i != 40; ++i) token += hex[random() & 15];
        write(Marker, {token, "MODULE"});
        // The preflight snapshot may precede another startup's completed import.
        // Under our newly owned marker, distinguish a missing row from a failed
        // query and preserve any exact completion instead of replaying/replacing.
        auto completed = fresh(name);
        if (completed)
        {
            Require(completed->hash == file.hash && completed->state == "MODULE",
                "Fresh progression completion hash/state mismatch: replay prohibited");
            erase(Marker);
            continue;
        }
        apply(file);
        write(name, {file.hash, "MODULE"});
        erase(Marker);
        ++imported;
    }
    return imported;
}
}
#endif
