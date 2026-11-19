#pragma once

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "system_model/system_model.hpp"

/* ── Feasibility: can this packet still make its deadline? ───────────────────

   Given a per-attempt decode probability, how many slots a queued packet still
   needs, and therefore whether it can still be delivered in the time it has
   left. Two schedulers can act on that answer very differently -- one drops
   the hopeless packet to free the slot, another lets it run to expiry, a third
   only uses it to decide whether it can afford to stop transmitting and listen
   -- so the classification lives here and what to *do* about it stays with the
   caller.

   Free functions, because the question is a pure function of the queue, the
   clock and one probability. Nothing is remembered between slots.

   Dropping a packet is deliberately not part of this. The caller removes what
   it wants removed, because removal usually has to be paired with forgetting
   whatever per-release state the scheduler was keeping for it, and this policy
   knows nothing about that. Keeping the classification separate from the
   removal is also what lets a scheduler switch dropping off while still
   *knowing* which packets are hopeless: without that split, disabling the drop
   would let hopeless packets fall through into the urgent set below and start
   suppressing listening, so one switch would be making two changes. */

/* Transmissions needed for one frame to clear `sr_req` at per-attempt decode
   probability `p`:

       P(success in k attempts) = 1 - (1-p)^k >= sr_req
       => k >= log(1 - sr_req) / log(1 - p).

   Infinite when the requirement cannot be met at any finite count. */
inline double retx_count_required(double p, double sr_req)
{
    if (sr_req <= 0.0) return 1.0;             /* still need to send the frame once */
    if (p >= 1.0)      return 1.0;
    if (p <= 0.0)      return std::numeric_limits<double>::infinity();
    if (sr_req >= 1.0) return std::numeric_limits<double>::infinity();

    const double k = std::log(1.0 - sr_req) / std::log(1.0 - p);
    return std::ceil(k);
}

/* What a pass over the queue found. `infeasible` holds (id, id_count) pairs --
   the globally unique key of a release rather than a buffer position, since a
   caller acting on this may well be removing packets and invalidating
   positions as it goes. */
struct feasibility_t
{
    std::vector<std::pair<unsigned int, unsigned int>> infeasible;

    /* True when some packet has *exactly* enough slots left: one more slot
       spent elsewhere and it becomes infeasible. */
    bool any_urgent = false;
};

/* Classify every queued packet at decode probability `decode_prob`.

   A packet needs `remaining_frames * retx_count_required(p, RR^(1/frames))`
   slots, against the `deadline - now` it has left. Short of that it is
   infeasible; exactly equal to it, it is urgent.

   Urgency is therefore an *equality*, which is not the rare coincidence it
   looks like. The urgent test is the else-branch of the infeasible one, so it
   is only reached when the packet still fits, and its own `<=` then leaves
   only the equal case. Both sides are counted in whole slots, and the time
   remaining falls by one every slot while the need moves slowly -- so every
   packet that tightens toward infeasibility passes through the equality on the
   way. Urgency fires routinely, at precisely the last slot where the packet
   can still be saved.

   A `decode_prob` of zero or less falls back to one slot per frame rather than
   declaring everything infeasible: before anything has been observed, a zero
   estimate says nothing has been measured, not that the channel is dead.

   Packets already past their deadline are skipped -- expiry is the buffer's
   job, and counting them here would classify them twice. */
inline feasibility_t classify_feasibility(const std::vector<packet_t>& buffer,
                                          unsigned int now, double decode_prob)
{
    feasibility_t out;

    for (const packet_t& pkt : buffer)
    {
        if (pkt.deadline <= now) continue;

        const double       ticks_available  = static_cast<double>(pkt.deadline - now);
        const unsigned int remaining_frames = pkt.frames - pkt.frame_count;
        const double       rr_per_frame     = std::pow(pkt.reliability_req,
                                                       1.0 / pkt.frames);
        const double       slots_per_frame  = (decode_prob <= 0.0)
                                              ? 1.0
                                              : retx_count_required(decode_prob, rr_per_frame);
        const double       needed_slots     = static_cast<double>(remaining_frames)
                                              * slots_per_frame;

        if (std::isinf(needed_slots) || ticks_available < needed_slots)
            out.infeasible.emplace_back(pkt.id, pkt.id_count);
        else if (ticks_available <= needed_slots)
            out.any_urgent = true;
    }

    return out;
}
