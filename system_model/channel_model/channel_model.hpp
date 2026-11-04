#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "system_model/system_model.hpp"

/* The *estimated* channel model a scheduler reasons with:

       M_hat_c = ( S_c, A_hat_c, { rho_hat_{c,s,j} } )

   an FSMC over S_c model states, a row-stochastic transition kernel, and one
   sigmoid decode response per state. It is the scheduler's abstraction of the
   channel, not the channel itself -- the physical process is free to differ.

   The two are built from the same pair of files here, so the shipped
   configuration is the zero-mismatch reference case of the model: the
   scheduler holds a *copy* of the fitted model, which is where a perturbation
   (Delta_A, Delta_rho) would be injected to study mismatch without touching
   the physical side.

   Both sides share this class on purpose. SigmoidChannel owns one and steps a
   chain through it; each belief-driven scheduler owns its own copy and reads
   decode estimates off it. A second implementation of the sigmoid or of the
   SNR calibration would let the two drift apart silently, and every gap
   measured against the mean predictor would then be confounded by it. */
class ChannelModel
{
public:
    /* Fixed link budget: 1 / 10 / 25 W land at 10 / 20 / 24 dB. The operating
       points every state curve is calibrated against, so it belongs to the
       model rather than to one side of it. */
    static constexpr double DEFAULT_PATHLOSS_DB = 20.0;

    /* Reads physical_channels/<channel_name>/{fsmc_states.json,
       transition_matrix.kov} and cross-checks them: the state table defines
       S_c and the matrix must be square of that size. Throws std::runtime_error
       on anything a mis-built channel directory can produce. */
    static ChannelModel from_directory(const std::string& channel_name);

    std::size_t n_states(void) const { return this->curves.size(); }

    /* rho_hat_{c,s,j}: decode probability of model state s at power P_j. */
    double rho(std::size_t state, unsigned int transmission_power) const;

    /* rho_bar_{c,j} = sum_s pi_s rho_hat_{c,s,j}. Also the estimate to use
       before a channel's first observation, where s_hat = bottom. */
    double rho_bar(unsigned int transmission_power) const;

    /* A_hat_ss: the probability of *staying* in state s over one slot, so
       1 - p_self(s) is the probability of leaving it. A held estimate of state
       s is still valid after d slots with probability p_self(s)^d, which is
       what a scheduler's confidence in it decays by. */
    double p_self(std::size_t state) const;

    const std::vector<markov_state_t>& state_curves(void) const { return this->curves; }
    const std::vector<double>& transition_matrix(void) const { return this->A; }
    const std::vector<double>& stationary(void) const { return this->pi; }

    /* Calibrated effective SNR of a power level, SNR_c(P_j). */
    double snr_db(unsigned int transmission_power) const;

private:
    ChannelModel(std::vector<markov_state_t> state_curves,
                 std::vector<double> kernel,
                 double pathloss_db);

    static std::vector<markov_state_t> load_curves(const std::string& path);
    static std::vector<double> load_kernel(const std::string& path);

    /* Left eigenvector of A (pi A = pi), by power iteration. */
    static std::vector<double> solve_stationary(const std::vector<double>& A, std::size_t n);

    std::vector<markov_state_t> curves;  /* one sigmoid per model state */
    std::vector<double> A;               /* row-major S*S, rows normalised */
    std::vector<double> pi;              /* stationary distribution */
    double pathloss_db;
};
