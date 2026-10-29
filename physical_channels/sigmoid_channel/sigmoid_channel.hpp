#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "physical_channels/base_physical_channel.hpp"
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
    std::size_t n_states(void) const { return this->fsmc.size(); }

    explicit SigmoidChannel(unsigned int frequency, const std::string& channel_name);

    double gen_probability(unsigned int transmission_power) override;
    double mean_probability(unsigned int transmission_power) override;
    received_frame_t gen_frame_with_probability(transmitted_frame_t transmitted_frame) override;
    void advance_fsmc_state(void) override;
    int get_fsmc_state(void);

    // Reseed the channel's RNGs (initial-state draw + chain transitions) and
    // re-pick the initial state deterministically. Call after construction.
    void seed_rng(uint64_t seed);

    /* Declared first: it fixes the state count, and both members below are
       built from it in the initialiser list, which runs in declaration
       order. */
    std::vector<markov_state_t> fsmc;

    /* Kept rather than discarded once the chain is built: the stationary
       distribution is derived from it, and FsmcChain does not hand its kernel
       back. */
    FsmcChain::Matrix transition_matrix;

    FsmcChain mc;

    /* pi: long-run fraction of time the chain spends in each state. Fixed for
       the whole run, computed once at construction. */
    std::vector<double> stationary;

private:
    /* Decode probability of one *named* state, independent of where the chain
       currently is. gen_probability() evaluates this at mc.current();
       mean_probability() averages it over `stationary`. */
    double state_probability(std::size_t state, unsigned int transmission_power) const;

    /* The two files a channel is made of, read before the members that are
       built from them. Static so they can run in the initialiser list. */
    static std::vector<markov_state_t> load_states(const std::string& channel_name);
    static FsmcChain::Matrix load_matrix(const std::string& channel_name);

    /* Left eigenvector of P (pi P = pi), by power iteration. */
    static std::vector<double> solve_stationary(const FsmcChain::Matrix& P, std::size_t n);

    std::default_random_engine generator;
    double pathloss_db;     /* Pathloss of the channel in dB*/
};
