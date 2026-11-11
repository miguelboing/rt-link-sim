#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "physical_channels/base_physical_channel.hpp"
#include "system_model/channel_model/channel_model.hpp"
#include "fsmc_chain.hpp"

class MLPredictor;

class SigmoidChannel: public BasePhysicalChannel
{
friend class MLPredictor;
public:
    /* The number of FSMC states is a property of the *channel*, resolved at
       construction from fsmc_states.json and cross-checked against the shape
       of transition_matrix.kov. It used to be the compile-time constant
       N_STATES, which a single build could only ever hold one value of; the
       shipped channels have 6, 8, 2 and 3 states and are loaded by the same
       binary, so the count had to become data like everything else about a
       channel. Everything below sizes itself from n_states(), which is what
       keeps the chain, the initial-state draw and the stationary vector from
       drifting apart. */
    std::size_t n_states(void) const { return this->model.n_states(); }

    explicit SigmoidChannel(const std::string& channel_name);

    double gen_probability(unsigned int transmission_power) override;
    double mean_probability(unsigned int transmission_power) override;
    received_frame_t gen_frame_with_probability(transmitted_frame_t transmitted_frame) override;
    void advance_fsmc_state(void) override;
    int get_fsmc_state(void) const override;

    /* This channel *is* an FSMC, so the model fitted to it is the model it
       runs on: the zero-mismatch reference case. A scheduler is handed a copy,
       which is where a perturbation would be introduced. */
    const ChannelModel* estimated_model(void) const override { return &this->model; }

    // Reseed the channel's RNGs (initial-state draw + chain transitions) and
    // re-pick the initial state deterministically. Call after construction.
    void seed_rng(uint64_t seed);

    /* Declared first: it fixes the state count and carries the kernel the
       chain below is built from, in the initialiser list, which runs in
       declaration order. */
    ChannelModel model;

    FsmcChain mc;

private:
    std::default_random_engine generator;
};
