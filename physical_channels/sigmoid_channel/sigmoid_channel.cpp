#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <kovian/kovian.hpp>
using namespace kovian::aliases;

#include "sigmoid_channel.hpp"

SigmoidChannel::SigmoidChannel(unsigned int frequency, const std::string& channel_name):
    BasePhysicalChannel(frequency),
    transition_matrix(kovian::transitionMatrixFromFile<N_STATES>(
        "physical_channels/" + channel_name + "/transition_matrix.kov")),
    mc(transition_matrix),
    stationary(solve_stationary(transition_matrix))
{
    this->pathloss_db = 20;

    /* Loading States */
    std::ifstream f("physical_channels/" + channel_name + "/fsmc_states.json");

    json data = json::parse(f);
    for (const auto& s : data)
    {
        fsmc.push_back
        ({
            s["snr_50_db"],
            s["slope"],
            s["max_saturation"],
            s["noise_floor_dbm"]
        });
    }

    /* The state table has to cover every state of the chain. A short table
       used to be a silent out-of-bounds read the first time the chain stepped
       into a missing state; mean_probability() would read past the end on
       every call. */
    if (this->fsmc.size() != N_STATES)
        throw std::runtime_error("SigmoidChannel: " + channel_name + "/fsmc_states.json has "
                                 + std::to_string(this->fsmc.size()) + " states, expected "
                                 + std::to_string(N_STATES));

    /* Defining initial state for fsmc */
    std::uniform_int_distribution<int> distribution(0, static_cast<int>(N_STATES) - 1);
    this->mc.setState(distribution(generator));
}

std::array<double, SigmoidChannel::N_STATES>
SigmoidChannel::solve_stationary(const kovian::TransitionMatrix<N_STATES>& P)
{
    /* pi by power iteration from the uniform vector: pi <- pi P until it stops
       moving. Computed rather than hardcoded -- the 20m ring is a circulant
       and therefore doubly stochastic, so its pi is exactly uniform and the
       first iteration already converges, but a future matrix need not be.

       Mixing can be slow: the 20m chain's second eigenvalue has modulus
       ~0.990, so reaching 1e-15 takes a few thousand iterations from a
       non-uniform start. The cap is set well above that and the loop runs once
       per simulation, so the cost is irrelevant. */
    std::array<double, N_STATES> pi;
    pi.fill(1.0 / static_cast<double>(N_STATES));

    /* Row sums: kovian normalises the rows when it builds the chain, so the
       file is not required to carry normalised ones and neither is this. */
    std::array<double, N_STATES> row_sum{};
    for (std::size_t i = 0; i < N_STATES; ++i)
        for (std::size_t j = 0; j < N_STATES; ++j)
            row_sum[i] += P[i * N_STATES + j];

    constexpr unsigned int MAX_ITER = 100000;
    unsigned int iter = 0;
    for (; iter < MAX_ITER; ++iter)
    {
        std::array<double, N_STATES> next{};
        for (std::size_t i = 0; i < N_STATES; ++i)
        {
            if (row_sum[i] <= 0.0) continue;  /* absorbing-into-nothing row */
            for (std::size_t j = 0; j < N_STATES; ++j)
                next[j] += pi[i] * P[i * N_STATES + j] / row_sum[i];
        }

        double delta = 0.0;
        for (std::size_t j = 0; j < N_STATES; ++j)
            delta += std::fabs(next[j] - pi[j]);

        pi = next;
        if (delta < 1e-15) break;
    }

    if (iter == MAX_ITER)
        std::cerr << "WARNING: SigmoidChannel stationary distribution did not converge in "
                  << MAX_ITER << " iterations; using the last iterate.\n";

    return pi;
}

double SigmoidChannel::state_probability(std::size_t state, unsigned int transmission_power) const
{
    /* Convert power W to dbmW */
    const double tx_power_dbm = 10 * log10(transmission_power * 1000);

    /* Power after pathloss */
    const double rx_power_dbm = tx_power_dbm - this->pathloss_db;

    /* SNR considering the noisefloor */
    const double snr_db = rx_power_dbm; //- this->fsmc[state].noise_floor_dbm;

    /* Calculating the probability on the sigmoid slope */
    return this->fsmc[state].max_saturation / (1.0 + exp(-(this->fsmc[state].slope) * (snr_db - (this->fsmc[state].snr_50_db))));
}

double SigmoidChannel::gen_probability(unsigned int transmission_power)
{
    /* Getting the current state */
    return this->state_probability(this->mc.current(), transmission_power);
}

double SigmoidChannel::mean_probability(unsigned int transmission_power)
{
    /* rho_bar_j = sum_s pi_s rho_s(SNR(P_j)): the decode probability this
       power would earn on average over the chain's long-run state
       distribution. Weights the frequent conditions more heavily, and is
       independent of where the chain happens to be now. */
    double mean = 0.0;
    for (std::size_t s = 0; s < N_STATES; ++s)
        mean += this->stationary[s] * this->state_probability(s, transmission_power);

    return mean;
}

received_frame_t SigmoidChannel::gen_frame_with_probability(transmitted_frame_t transmitted_frame)
{
    received_frame_t recv_frame;
    recv_frame.packet = transmitted_frame.packet;
    recv_frame.transmission_power = transmitted_frame.transmission_power;
    recv_frame.frequency = transmitted_frame.frequency;

    recv_frame.success_prob = this->gen_probability(recv_frame.transmission_power);

    return recv_frame;
}

void SigmoidChannel::advance_fsmc_state(void)
{
    (void)this->mc.advance();
}

int SigmoidChannel::get_fsmc_state(void)
{
    return this->mc.current();
}

void SigmoidChannel::seed_rng(uint64_t seed)
{
    // Distinct sub-seeds for the two streams (initial-state draw and the
    // FSMC transition RNG) to keep them statistically independent.
    this->generator.seed(seed);
    this->mc.seed(seed ^ 0x9E3779B97F4A7C15ULL);

    // Re-pick the initial state using the now-deterministic generator.
    std::uniform_int_distribution<int> distribution(0, static_cast<int>(N_STATES) - 1);
    this->mc.setState(distribution(this->generator));
}
