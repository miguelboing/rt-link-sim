#pragma once

#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "system_model/system_model.hpp"
#include "system_model/channel_model/channel_model.hpp"

class BasePhysicalChannel
{
public:
    explicit BasePhysicalChannel(std::string name):
        name(std::move(name)) {};

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
       information (MPRM, MPEDF), which never enter RX_MODE and so are never
       handed a prediction. Pure virtual on purpose: a new channel has to say
       what its own long-run average means. */
    virtual double mean_probability(unsigned int transmission_power) = 0;

    /* Per-tick FSMC step. Default no-op for channels without internal state. */
    virtual void advance_fsmc_state(void) {}

    /* The channel's current model-state label, or -1 for a channel with no
       notion of state. This is what an RX slot observes: the predictor maps it
       to s_hat_c(t) and the scheduler turns that into decode estimates off its
       own fitted response curves. */
    virtual int get_fsmc_state(void) const { return -1; }

    /* The fitted FSMC a scheduler should reason with, or nullptr when the
       channel has none -- a recorded trace, for instance, which has no state
       space to label. A scheduler handed nullptr has no model to hold and
       falls back to being given decode probabilities directly.

       The pointer is the channel's own model and stays valid for its lifetime;
       a scheduler copies it at construction, since the estimated model is
       meant to be perturbable independently of the physical one. */
    virtual const ChannelModel* estimated_model(void) const { return nullptr; }

    /* The channel's own name, which for a sigmoid channel is exactly the
       directory under physical_channels/ it was built from -- "cyclic_fast_6".
       It is for logs, config and error messages; it is *not* how anything
       addresses the channel. That is the index c, which is the channel's
       position in the simulator's channel vector, so there is no second
       identifier to keep in step with it. */
    std::string name;
};

