#ifndef LOOT_RECEIPT_H
#define LOOT_RECEIPT_H

#include "Log.h"
#include <atomic>
#include <cstdint>

// Process-local IDs require the independently verified world invocation and source binding.
namespace LootReceipt
{
inline std::uint64_t Next() noexcept
{
    static std::atomic<std::uint64_t> sequence{0};
    return ++sequence;
}

template<class... Args>
inline void Emit(char const* format, Args const&... args) noexcept
{
    try { LOG_INFO("loot.receipt", "loot_event={} {}", Next(), Acore::StringFormat(format, args...)); }
    catch (...) { } // Missing records always mean incomplete evidence, never a payout veto.
}

inline thread_local std::uint64_t itemOperation = 0, itemGeneration = 0, itemCorpse = 0;
class ItemContext
{
    std::uint64_t op = itemOperation, generation = itemGeneration, corpse = itemCorpse;
public:
    ItemContext(std::uint64_t id, std::uint64_t gen, std::uint64_t source) noexcept
    { itemOperation = id; itemGeneration = gen; itemCorpse = source; }
    ~ItemContext() noexcept
    { itemOperation = op; itemGeneration = generation; itemCorpse = corpse; }
};

template<class L, class I, class P>
inline void ItemState(std::uint64_t op, char const* phase, L const* loot, I const* item,
                      P const* player, std::uint32_t slot, char const* slotDomain = "combined") noexcept
{
    Emit("loot_v1 item_state op={} generation={} phase={} corpse={} map={} instance={} actor={} slot={} entry={} count={} index={} property={} consumed={} blocked={} ffa={} threshold={} winner={} rr={} unlooted={} allowed={} slot_domain={} generation_known={}",
        op, loot->receiptGeneration, phase, loot->sourceWorldObjectGUID.GetRawValue(),
        loot->receiptMapId, loot->receiptInstanceId, player->GetGUID().GetRawValue(), slot,
        item->itemid, item->count, item->itemIndex, item->randomPropertyId, item->is_looted,
        item->is_blocked, item->freeforall, item->is_underthreshold, item->rollWinnerGUID.GetRawValue(),
        loot->roundRobinPlayer.GetRawValue(), loot->unlootedCount, item->GetAllowedLooters().size(), slotDomain, loot->receiptGeneration != 0);
    std::uint32_t i = 0;
    for (auto const& guid : item->GetAllowedLooters())
        Emit("loot_v1 item_allowed op={} phase={} index={} guid={}", op, phase, i++, guid.GetRawValue());
}

template<class P, class D>
inline void Destinations(std::uint64_t op, char const* phase, P* player, D const& dest) noexcept
{
    Emit("loot_v1 destinations op={} phase={} count={}", op, phase, dest.size());
    std::uint32_t i = 0;
    for (auto const& d : dest)
    {
        auto* item = player->GetItemByPos(d.pos);
        Emit("loot_v1 destination op={} phase={} index={} pos={} planned={} guid={} entry={} count={} owner={} container={} property={} suffix={}",
            op, phase, i++, d.pos, d.count, item ? item->GetGUID().GetRawValue() : 0,
            item ? item->GetEntry() : 0, item ? item->GetCount() : 0,
            item ? item->GetOwnerGUID().GetRawValue() : 0,
            item && item->GetContainer() ? item->GetContainer()->GetGUID().GetRawValue() : (item ? item->GetOwnerGUID().GetRawValue() : 0),
            item ? item->GetItemRandomPropertyId() : 0, item ? item->GetItemSuffixFactor() : 0);
    }
}

template<class L, class I, class P>
class Award
{
    L* loot;
    I* item;
    P* player;
    std::uint32_t slot;
public:
    std::uint64_t const id = Next();
    Award(char const* route, L* l, I* i, P* p, std::uint32_t s) noexcept : loot(l), item(i), player(p), slot(s)
    {
        Emit("loot_v1 item_begin op={} generation={} route={} corpse={} map={} instance={} actor={} slot={}",
            id, l->receiptGeneration, route, l->sourceWorldObjectGUID.GetRawValue(),
            l->receiptMapId, l->receiptInstanceId, p->GetGUID().GetRawValue(), s);
        Emit("loot_v1 actor_location op={} map={} instance={}", id, p->GetMapId(), p->GetInstanceId());
        ItemState(id, "resolved", l, i, p, s);
    }
    void End(char const* reason, std::uint32_t error) noexcept
    {
        ItemState(id, "terminal", loot, item, player, slot);
        Emit("loot_v1 item_end op={} reason={} error={} unlooted={}", id, reason, error, loot->unlootedCount);
    }
};

namespace Disenchant
{
struct Link
{
    std::uint64_t parent = 0, generation = 0, corpse = 0, actor = 0;
};
enum class Call { AutoStore, Retrieval, Mail };
struct Request
{
    Call call;
    void const* callee;
    Link link;
    std::uint32_t discriminator;
};
inline thread_local Request const* pending = nullptr;
class Scope
{
    Request request;
    Request const* previous;
public:
    Scope(Call call, void const* callee, Link link, std::uint32_t discriminator = 0) noexcept
        : request{call, callee, link, discriminator}, previous(pending) { pending = &request; }
    ~Scope() noexcept { pending = previous; }
    Scope(Scope const&) = delete;
    Scope& operator=(Scope const&) = delete;
};
inline Link Claim(Call call, void const* callee, std::uint64_t actor, std::uint32_t discriminator = 0) noexcept
{
    Request const* request = pending;
    pending = nullptr; // Even a mismatch must not arm an unrelated nested operation.
    if (!request || request->call != call || request->callee != callee ||
        request->link.actor != actor || request->discriminator != discriminator)
        return {};
    return request->link;
}
struct Generated
{
    void const* loot;
    Link link;
};
inline thread_local Generated const* generated = nullptr;
class GeneratedScope
{
    Generated value;
    Generated const* previous;
public:
    GeneratedScope(void const* loot, Link link) noexcept : value{loot, link}, previous(generated) { generated = &value; }
    ~GeneratedScope() noexcept { generated = previous; }
    GeneratedScope(GeneratedScope const&) = delete;
    GeneratedScope& operator=(GeneratedScope const&) = delete;
};
inline Link FromLoot(void const* loot, std::uint64_t actor) noexcept
{
    return generated && generated->loot == loot && generated->link.actor == actor ? generated->link : Link{};
}
}

class Money
{
public:
    template<class L, class P>
    Money(char const* route, L const* loot, P const* player) noexcept
        : id(Next()), generation(loot->receiptGeneration), pre(loot->gold)
    {
        Emit("loot_v1 money_begin op={} generation={} route={} corpse={} map={} instance={} actor={} pre={} generation_known={}",
            id, generation, route, loot->sourceWorldObjectGUID.GetRawValue(), loot->receiptMapId,
            loot->receiptInstanceId, player->GetGUID().GetRawValue(), pre, generation != 0);
        Emit("loot_v1 actor_location op={} map={} instance={}", id, player->GetMapId(), player->GetInstanceId());
    }
    void Post(std::uint32_t gold) noexcept
    {
        post = gold;
        Emit("loot_v1 money_post op={} gold={}", id, gold);
    }
    template<class Players>
    void Recipients(Players const& players) noexcept
    {
        Emit("loot_v1 money_set op={} count={}", id, players.size());
        std::uint32_t i = 0;
        for (auto* p : players)
            Emit("loot_v1 money_member op={} index={} guid={} money={}", id, i++, p->GetGUID().GetRawValue(), p->GetMoney());
    }
    template<class Players>
    void Balances(Players const& players) noexcept
    {
        std::uint32_t i = 0;
        for (auto* p : players)
            Emit("loot_v1 money_balance op={} index={} guid={} money={}", id, i++, p->GetGUID().GetRawValue(), p->GetMoney());
    }
    void Credit(std::uint64_t guid, std::uint32_t request, std::uint32_t before,
                std::uint32_t after, bool accepted, char const* phase) noexcept
    {
        auto delta = std::int64_t(after) - before;
        net += delta;
        Emit("loot_v1 money_credit op={} guid={} request={} before={} after={} delta={} accepted={} phase={}",
            id, guid, request, before, after, delta, accepted, phase);
    }
    void End(char const* reason, std::uint32_t retained, std::uint32_t removed = 0) noexcept
    {
        Emit("loot_v1 money_end op={} generation={} reason={} pre={} post={} retained={} removed={} modify_net={}",
            id, generation, reason, pre, post, retained, removed, net);
    }
    std::uint64_t const id, generation;
private:
    std::uint32_t pre, post = 0;
    std::int64_t net = 0;
};
}
#endif
