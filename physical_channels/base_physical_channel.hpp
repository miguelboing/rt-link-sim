#pragma once

#include <vector>
#include <memory>
#include <random>

#include "system_model/system_model.hpp"

class BasePhysicalChannel
{
public:
    explicit BasePhysicalChannel(unsigned int frequency):
        frequency(frequency) {};

    virtual ~BasePhysicalChannel() = default;

    virtual received_frame_t gen_frame_with_probability(transmitted_frame_t transmitted_frame) = 0;
    virtual double gen_probability(unsigned int transmission_power) = 0;

    /* Long-run mean decode probability at a transmission power, averaged over
       the channel's own state distribution rather than read off the current
       state:

           rho_bar_j = sum_s pi_s rho_s(SNR(P_j))

       This is a *static* property of the channel, not a prediction -- it does
       not depend on the tick or the current state, so it is safe to evaluate
       once at construction. It exists for the schedulers that size their
       redundancy from average channel quality instead of refreshed channel
       information (SRM, SEDF), which never enter RX_MODE and so are never
       handed a prediction. Pure virtual on purpose: a new channel has to say
       what its own long-run average means. */
    virtual double mean_probability(unsigned int transmission_power) = 0;

    /* Per-tick FSMC step. Default no-op for channels without internal state. */
    virtual void advance_fsmc_state(void) {}

    unsigned int frequency;
};

