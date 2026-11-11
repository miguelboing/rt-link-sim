#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "system_model/system_model.hpp"
#include "physical_channels/base_physical_channel.hpp"

class MLPredictor
{
public:
    explicit MLPredictor(std::shared_ptr<unsigned int> sys_tick,
                         std::shared_ptr<std::vector<std::unique_ptr<BasePhysicalChannel>>> channels,
                         double predict_error = 0.0);

    /* g_c(y_c(t)): the model-state label inferred from what an RX slot
       observed on channel c, or -1 when c is out of range or the channel it
       names has no state space to label. This is the predictor's
       whole output -- one label, not a decode probability per power. The
       scheduler turns it into the row rho_hat_{c,s,.} off its own copy of the
       fitted response curves, so a single observation refreshes every power
       level together and a mislabelled state moves all of them at once.

       State-prediction error (the confusion matrix E_c) belongs here and is
       not implemented yet: the label returned is the channel's own, so this is
       the identity-confusion case. `predict_error` does not reach this path:
       it perturbs a probability, and there is no probability here to perturb.
       main.cpp warns when a run sets it and takes this path anyway. */
    int predict_channel_state(std::size_t c);

    void seed_rng(uint64_t seed);

    std::shared_ptr<unsigned int> system_tick;

    std::shared_ptr<std::vector<std::unique_ptr<BasePhysicalChannel>>> channels;

    /* Half-width of the additive uniform noise that *used* to be applied to
       each predicted decode probability. Dead while the predictor returns a
       state label: there is no probability here to perturb, and the confusion
       matrix E_c that would take its place is not implemented. main.cpp warns
       when a run sets it. Kept so the config field and the error sweep still
       parse. */
    double predict_error;

private:
    std::default_random_engine generator;
};

