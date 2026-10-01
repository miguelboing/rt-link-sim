#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <kovian/kovian.hpp>

#include "physical_channels/base_physical_channel.hpp"

class MLPredictor;

class SigmoidChannel: public BasePhysicalChannel
{
friend class MLPredictor;
public:
    /* Number of FSMC states. The transition matrix (row count of the .kov
       file) and the state table (length of fsmc_states.json) must both match
       it -- kovian throws on a bad matrix and the constructor checks the state
       table. Using the constant everywhere is what keeps the chain, the
       initial-state draws and the stationary vector from drifting apart; they
       used to carry the literal 6 independently. */
    static constexpr std::size_t N_STATES = 6;

    explicit SigmoidChannel(unsigned int frequency, const std::string& channel_name);

    double gen_probability(unsigned int transmission_power) override;
    double mean_probability(unsigned int transmission_power) override;
    received_frame_t gen_frame_with_probability(transmitted_frame_t transmitted_frame) override;
    void advance_fsmc_state(void) override;
    int get_fsmc_state(void);

    // Reseed the channel's RNGs (initial-state draw + kovian transitions) and
    // re-pick the initial state deterministically. Call after construction.
    void seed_rng(uint64_t seed);

    std::vector<markov_state_t> fsmc;

    /* Declared ahead of mc because mc is constructed from it in the
       initialiser list. Kept afterwards rather than discarded: the stationary
       distribution is derived from it, and kovian's MarkovChain does not hand
       its kernel back. */
    kovian::TransitionMatrix<N_STATES> transition_matrix;

    kovian::MarkovChain<N_STATES> mc;

    /* pi: long-run fraction of time the chain spends in each state. Fixed for
       the whole run, computed once at construction. */
    std::array<double, N_STATES> stationary;

private:
    /* Decode probability of one *named* state, independent of where the chain
       currently is. gen_probability() evaluates this at mc.current();
       mean_probability() averages it over `stationary`. */
    double state_probability(std::size_t state, unsigned int transmission_power) const;

    /* Left eigenvector of P (pi P = pi), by power iteration. */
    static std::array<double, N_STATES> solve_stationary(const kovian::TransitionMatrix<N_STATES>& P);

    std::default_random_engine generator;
    double pathloss_db;     /* Pathloss of the channel in dB*/
};
