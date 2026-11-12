#pragma once

#include <algorithm>
#include <vector>

#include "system_model/system_model.hpp"

/* ── Dequeue order: earliest deadline first ──────────────────────────────────

   Which queued packet to send next, if one is sent at all. That is a decision,
   not a property of the queue: the buffer holds packets and enforces their
   deadlines without taking a view on service order, so ranking them is the
   scheduler's call -- by deadline, by period, by slack, by reliability debt.
   This is the deadline rule.

   A free function, because the rule carries no state: it reads the queue and
   answers. Nothing to construct, nothing to tick.

   Ties go to the earliest *position in the buffer*, since std::min_element
   returns the first of several equal minima. Two packets sharing an absolute
   deadline are therefore served in buffer order, which is release order.
   Arbitrary, but deterministic and identical for every caller -- so it can
   never be the reason two schedulers differ.

   No periodicity guard is needed: a deadline is well defined for an aperiodic
   packet too, so every buffered packet is rankable and none has to be skipped. */
/* The reference cannot be const: the returned pointer is a mutable handle on a
   packet in this buffer, which is what a caller needs in order to schedule it.
   Taking a const reference and const_casting the result back would be the same
   access with the intent hidden. */
// cppcheck-suppress constParameterReference
inline packet_t* select_earliest_deadline(std::vector<packet_t>& buffer)
{
    const auto it = std::min_element(buffer.begin(), buffer.end(),
        [](const packet_t& a, const packet_t& b) { return a.deadline < b.deadline; });

    /* nullptr means an empty queue: there is nothing to send, and what to do
       with the slot instead is the caller's decision. */
    return it == buffer.end() ? nullptr : &(*it);
}
