#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>

#include "schedulers/base_scheduler.hpp"

/* SCS -- "Static Channel Selection".

   Two decisions, both made from the channels' long-run statistics and neither
   ever revisited:

     1. *Which channel.* Before its first transmission it compares every
        channel's long-run mean decode probability at its transmit power and
        takes the best one, then transmits there for the rest of the run.
     2. *How much redundancy.* It sizes retransmissions from that channel's
        mean, with the accumulated-probability rule: a frame is retired once
        the probability that at least one of its transmissions was decoded
        clears the per-frame requirement.

   The estimate is

       rho_bar_{c,j} = sum_s pi_{c,s} rho_{c,s}(SNR(P_j)),

   with pi_c the stationary distribution of channel c's FSMC and rho_{c,s} its
   per-state decode curve, so the conditions the channel spends more time in
   weigh more. Because nothing moves, the redundancy is deterministic: every
   instance of a task is sent the same number of times, whatever state the
   channel is actually in, and the channel it is sent on is fixed from the
   first slot.

   It never enters RX_MODE. There is nothing to listen for: a static estimate
   learns nothing from an observation, so every slot is spent transmitting or
   idle and there is no listening knob. That also means it gets more transmit
   opportunities than a scheduler that gives slots up to listen, which is worth
   remembering when reading an energy row against one.

   This is the baseline for *what tracking the channel adds over knowing only
   its long-run quality* -- both in which channel to use and in how hard to
   push on it. A scheduler that picks per slot from a refreshed estimate should
   be read against this one. */
class SCS_scheduler : public BaseScheduler
{
public:
    SCS_scheduler(unsigned int tx_power, BufferPacket* buffer,
                  std::shared_ptr<unsigned int> sys_tick, std::vector<channel_estimate_t> channels);

    unsigned int tx_power;

    std::unordered_map<uint64_t, double> accumulated_prob;

    scheduled_frame_t do_schedule_frame(void) override;

    std::string get_name() const override;

    /* c_sel: the channel chosen at construction, and transmitted on for the
       whole run. Readable so a caller can see what was picked; there is no
       setter, because the choice is the scheduler's and not the config's. */
    std::size_t channel(void) const { return this->selected; }

private:
    /* Pick the channel to commit to, and complain if even the best of them
       cannot carry a packet. Called from the constructor: the channel set is a
       constructor argument, so there is nothing to wait for and the scheduler
       is never in a state where it has not chosen. */
    void select_channel(void);

    std::size_t selected = 0U;
};
