#include <cmath>
#include <cstddef>
#include <iostream>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "sigmoid_channel.hpp"

/* Both files live under physical_channels/<name>/, which is where the
   generators write them. */
static std::string channel_path(const std::string& channel_name, const std::string& file)
{
    return "physical_channels/" + channel_name + "/" + file;
}

std::vector<markov_state_t> SigmoidChannel::load_states(const std::string& channel_name)
{
    const std::string path = channel_path(channel_name, "fsmc_states.json");
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("SigmoidChannel: cannot open " + path);

    std::vector<markov_state_t> states;
    json data = json::parse(f);
    for (const auto& s : data)
    {
        states.push_back
        ({
            s["snr_50_db"],
            s["slope"],
            s["max_saturation"]
        });
    }

    /* The state table is what defines how many states the channel has, so an
       empty one leaves nothing to run. A table *shorter than the chain* is
       caught in the member initialiser instead, where FsmcChain compares the
       matrix against this length: that mismatch used to be a silent
       out-of-bounds read the first time the chain stepped into a missing
       state, and mean_probability() read past the end on every call. */
    if (states.empty())
        throw std::runtime_error("SigmoidChannel: " + path + " defines no states");

    return states;
}

FsmcChain::Matrix SigmoidChannel::load_matrix(const std::string& channel_name)
{
    return FsmcChain::matrix_from_file(channel_path(channel_name, "transition_matrix.kov"));
}

SigmoidChannel::SigmoidChannel(unsigned int frequency, const std::string& channel_name):
    BasePhysicalChannel(frequency),
    fsmc(load_states(channel_name)),
    transition_matrix(load_matrix(channel_name)),
    mc(transition_matrix, fsmc.size()),
    stationary(solve_stationary(transition_matrix, fsmc.size()))
{
    this->pathloss_db = 20;

    /* Defining initial state for fsmc. Drawn from the stationary distribution
       rather than uniformly: a run starts where the channel spends its time,
       so no warm-up is needed to shake off the initial condition. The two are
       the same thing only when pi is uniform, which held for the single
       shipped channel but does not for good_moderate_2 (60/40) or
       rare_excellent_3, where a uniform draw would open one run in three in a
       state the channel is in 5% of the time. */
    std::discrete_distribution<std::size_t> distribution(this->stationary.begin(),
                                                         this->stationary.end());
    this->mc.set_state(distribution(this->generator));
}

std::vector<double>
SigmoidChannel::solve_stationary(const FsmcChain::Matrix& P, std::size_t n)
{
    /* pi by power iteration from the uniform vector: pi <- pi P until it stops
       moving. Computed rather than hardcoded -- the 20m ring is a circulant
       and therefore doubly stochastic, so its pi is exactly uniform and the
       first iteration already converges, but a future matrix need not be.

       Mixing can be slow: the 20m chain's second eigenvalue has modulus
       ~0.990, so reaching 1e-15 takes a few thousand iterations from a
       non-uniform start. The cap is set well above that and the loop runs once
       per simulation, so the cost is irrelevant. */
    std::vector<double> pi(n, 1.0 / static_cast<double>(n));

    /* Row sums: the chain normalises the rows when it builds its per-row
       distributions, so the file is not required to carry normalised ones and
       neither is this. */
    std::vector<double> row_sum(n, 0.0);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j)
            row_sum[i] += P[i * n + j];

    constexpr unsigned int MAX_ITER = 100000;
    unsigned int iter = 0;
    for (; iter < MAX_ITER; ++iter)
    {
        std::vector<double> next(n, 0.0);
        for (std::size_t i = 0; i < n; ++i)
        {
            if (row_sum[i] <= 0.0) continue;  /* absorbing-into-nothing row */
            for (std::size_t j = 0; j < n; ++j)
                next[j] += pi[i] * P[i * n + j] / row_sum[i];
        }

        double delta = 0.0;
        for (std::size_t j = 0; j < n; ++j)
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

    /* Received power is used directly as the SNR. The link budget lives in
       pathloss_db above, which fixes the operating points at 10, 20 and 24 dB
       for 1, 10 and 25 W; each state's snr_50_db is calibrated against those.
       A separate noise floor would only shift every operating point by a
       constant, so it is folded into snr_50_db rather than carried per state. */
    const double snr_db = rx_power_dbm;

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
    for (std::size_t s = 0; s < this->n_states(); ++s)
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
    return static_cast<int>(this->mc.current());
}

void SigmoidChannel::seed_rng(uint64_t seed)
{
    // Distinct sub-seeds for the two streams (initial-state draw and the
    // FSMC transition RNG) to keep them statistically independent.
    this->generator.seed(seed);
    this->mc.seed(seed ^ 0x9E3779B97F4A7C15ULL);

    // Re-pick the initial state using the now-deterministic generator, from
    // the same stationary distribution the constructor drew from.
    std::discrete_distribution<std::size_t> distribution(this->stationary.begin(),
                                                         this->stationary.end());
    this->mc.set_state(distribution(this->generator));
}
