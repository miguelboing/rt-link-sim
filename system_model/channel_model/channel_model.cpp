#include <cmath>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "channel_model.hpp"

/* Both files live under physical_channels/<name>/, which is where the
   generators write them. */
static std::string channel_path(const std::string& channel_name, const std::string& file)
{
    return "physical_channels/" + channel_name + "/" + file;
}

ChannelModel::ChannelModel(std::vector<markov_state_t> state_curves,
                           std::vector<double> kernel,
                           double pathloss_db):
    curves(std::move(state_curves)),
    A(std::move(kernel)),
    pathloss_db(pathloss_db)
{
    const std::size_t n = this->curves.size();

    /* The cross-check the shared compile-time N_STATES used to provide: the
       state table says how many states the channel has, and the kernel has to
       agree with it. */
    if (this->A.size() != n * n)
        throw std::runtime_error("ChannelModel: transition matrix has " +
                                 std::to_string(this->A.size()) + " entries, expected " +
                                 std::to_string(n * n) + " for " + std::to_string(n) +
                                 " states");

    /* Normalise the rows once, here, so A is genuinely row-stochastic: p_self()
       is then a probability and powers of A propagate a distribution. The
       shipped files are already normalised to ~1e-10, which is well inside the
       tolerance a hand-written regime would need. */
    for (std::size_t i = 0; i < n; ++i)
    {
        double row_sum = 0.0;
        for (std::size_t j = 0; j < n; ++j)
            row_sum += this->A[i * n + j];

        /* An all-zero row leaves the chain nowhere to go, which no transition
           file ever means to say. */
        if (row_sum <= 0.0)
            throw std::runtime_error("ChannelModel: row " + std::to_string(i) +
                                     " of the transition matrix sums to zero");

        for (std::size_t j = 0; j < n; ++j)
            this->A[i * n + j] /= row_sum;
    }

    this->pi = solve_stationary(this->A, n);
}

ChannelModel ChannelModel::from_directory(const std::string& channel_name)
{
    return ChannelModel(load_curves(channel_path(channel_name, "fsmc_states.json")),
                        load_kernel(channel_path(channel_name, "transition_matrix.kov")),
                        DEFAULT_PATHLOSS_DB);
}

std::vector<markov_state_t> ChannelModel::load_curves(const std::string& path)
{
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("ChannelModel: cannot open " + path);

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
       empty one leaves nothing to run. A table *shorter than the kernel* is
       caught by the constructor, which compares the two: that mismatch used to
       be a silent out-of-bounds read the first time the chain stepped into a
       missing state. */
    if (states.empty())
        throw std::runtime_error("ChannelModel: " + path + " defines no states");

    return states;
}

std::vector<double> ChannelModel::load_kernel(const std::string& path)
{
    std::ifstream mfile(path);
    if (!mfile)
        throw std::runtime_error("ChannelModel: cannot open " + path);

    std::vector<double> flat;
    std::size_t rows = 0;
    std::size_t width = 0;
    std::string line;
    while (std::getline(mfile, line))
    {
        /* Commas are separators, nothing else: swapping them for spaces lets
           the stream tokenise the row and keeps scientific notation working,
           which a hand-rolled split would have to handle itself. A far-rank
           transition can legitimately be written 2.5e-04. */
        for (char& c : line)
            if (c == ',') c = ' ';

        std::istringstream row_stream(line);
        std::size_t in_row = 0;
        double value = 0.0;
        while (row_stream >> value)
        {
            if (value < 0.0)
                throw std::runtime_error("ChannelModel: negative probability in " + path +
                                         " row " + std::to_string(rows));
            flat.push_back(value);
            ++in_row;
        }

        if (in_row == 0) continue;   /* blank line */

        if (width == 0)
            width = in_row;
        else if (in_row != width)
            throw std::runtime_error("ChannelModel: " + path + " row " + std::to_string(rows) +
                                     " has " + std::to_string(in_row) + " entries, expected " +
                                     std::to_string(width));
        ++rows;
    }

    if (rows == 0)
        throw std::runtime_error("ChannelModel: " + path + " is empty");
    if (rows != width)
        throw std::runtime_error("ChannelModel: " + path + " is " + std::to_string(rows) +
                                 "x" + std::to_string(width) + ", expected a square matrix");
    return flat;
}

std::vector<double>
ChannelModel::solve_stationary(const std::vector<double>& A, std::size_t n)
{
    /* pi by power iteration from the uniform vector: pi <- pi A until it stops
       moving. Computed rather than hardcoded -- both rings are circulants and
       therefore doubly stochastic, so their pi is exactly uniform and the
       first iteration already converges, but the two reversible chains are not.

       Mixing can be slow: rare_excellent_3's second eigenvalue has modulus
       ~0.998, so reaching 1e-15 takes a few thousand iterations from a
       non-uniform start. The cap is set well above that and the loop runs once
       per model, so the cost is irrelevant. */
    std::vector<double> pi(n, 1.0 / static_cast<double>(n));

    constexpr unsigned int MAX_ITER = 100000;
    unsigned int iter = 0;
    for (; iter < MAX_ITER; ++iter)
    {
        std::vector<double> next(n, 0.0);
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < n; ++j)
                next[j] += pi[i] * A[i * n + j];

        double delta = 0.0;
        for (std::size_t j = 0; j < n; ++j)
            delta += std::fabs(next[j] - pi[j]);

        pi = next;
        if (delta < 1e-15) break;
    }

    if (iter == MAX_ITER)
        std::cerr << "WARNING: ChannelModel stationary distribution did not converge in "
                  << MAX_ITER << " iterations; using the last iterate.\n";

    return pi;
}

double ChannelModel::snr_db(unsigned int transmission_power) const
{
    /* Convert power W to dbmW */
    const double tx_power_dbm = 10 * log10(transmission_power * 1000);

    /* Received power is used directly as the SNR. The link budget lives in
       pathloss_db, which fixes the operating points at 10, 20 and 24 dB for
       1, 10 and 25 W; each state's snr_50_db is calibrated against those. A
       separate noise floor would only shift every operating point by a
       constant, so it is folded into snr_50_db rather than carried per
       state. */
    return tx_power_dbm - this->pathloss_db;
}

double ChannelModel::rho(std::size_t state, unsigned int transmission_power) const
{
    const double snr_db = this->snr_db(transmission_power);

    /* Calculating the probability on the sigmoid slope */
    return this->curves[state].max_saturation / (1.0 + exp(-(this->curves[state].slope) * (snr_db - (this->curves[state].snr_50_db))));
}

double ChannelModel::rho_bar(unsigned int transmission_power) const
{
    /* rho_bar_j = sum_s pi_s rho_s(SNR(P_j)): the decode probability this
       power would earn on average over the chain's long-run state
       distribution. Weights the frequent conditions more heavily, and is
       independent of where the chain happens to be now. */
    double mean = 0.0;
    for (std::size_t s = 0; s < this->n_states(); ++s)
        mean += this->pi[s] * this->rho(s, transmission_power);

    return mean;
}

double ChannelModel::p_self(std::size_t state) const
{
    return this->A[state * this->n_states() + state];
}
