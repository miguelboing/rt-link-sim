#pragma once

#include <algorithm>
#include <vector>

#include "system_model/system_model.hpp"

/* ── Dequeue order: shortest period first ────────────────────────────────────

   Rate-monotonic order: of the queued packets, send the one whose task repeats
   most often. A fixed-priority rule, since a task's period does not change --
   two releases of the same task always rank the same way against each other,
   whatever their deadlines happen to be.

   A free function, because the rule carries no state: it reads the queue and
   answers.

   Unlike ordering by deadline this needs a periodicity guard, and that guard
   has a consequence worth knowing. An aperiodic packet has no period to rank
   by, so it sorts behind every periodic one and is only ever returned when
   nothing else is queued -- and the caller is told, because the returned
   packet is then an aperiodic one it can test. A scheduler that declines to
   send in that case will sit idle while aperiodic work waits, which is a real
   behaviour of fixed-priority order rather than a bug in it.

   Ties go to the earliest position in the buffer, since std::min_element
   returns the first of several equal minima. Two releases of the same task, or
   of two tasks sharing a period, are therefore served in release order. */

/* The reference cannot be const: the returned pointer is a mutable handle on a
   packet in this buffer, which is what a caller needs in order to schedule it.
   Taking a const reference and const_casting the result back would be the same
   access with the intent hidden. */
// cppcheck-suppress constParameterReference
inline packet_t* select_shortest_period(std::vector<packet_t>& buffer)
{
    const auto it = std::min_element(buffer.begin(), buffer.end(),
        [](const packet_t& a, const packet_t& b) {
            /* An aperiodic packet never outranks a periodic one, and two
               aperiodic ones do not outrank each other, which leaves them in
               buffer order at the back. */
            if (!a.is_periodic) return false;
            if (!b.is_periodic) return true;
            return a.period < b.period;
        });

    /* nullptr means an empty queue: there is nothing to send, and what to do
       with the slot instead is the caller's decision. A non-null packet may
       still be aperiodic, which the caller can test if its policy says not to
       send one. */
    return it == buffer.end() ? nullptr : &(*it);
}
