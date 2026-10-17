#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "replay_channel.hpp"

namespace
{
    /* Fixed predictor view of the channel — what CATS sees in RX mode.
       These are the empirical decode rates of the replayed CSV, so they must
       be regenerated together with it: the CSV's success flags come from a
       success-radius threshold X (ft8-dc's extra/success_probability.py), and
       changing X changes both. Currently X = 2250 km:
           1 W  60/160 = 0.37500
           10 W 131/160 = 0.81875
           25 W 153/160 = 0.95625
       (The previous X = 2500 km gave 0.07 / 0.58 / 0.80.) Nothing in the build
       checks the two against each other. */
    constexpr double PROB_1W  = 0.38;
    constexpr double PROB_10W = 0.82;
    constexpr double PROB_25W = 0.96;
}

ReplayChannel::ReplayChannel(unsigned int frequency,
                             const std::string& csv_path,
                             std::shared_ptr<unsigned int> sys_tick):
    BasePhysicalChannel(frequency), system_tick(sys_tick)
{
    std::ifstream f(csv_path);
    if (!f.is_open())
        throw std::runtime_error("ReplayChannel: could not open CSV: " + csv_path);

    std::string line;
    /* Skip header */
    if (!std::getline(f, line))
        throw std::runtime_error("ReplayChannel: empty CSV: " + csv_path);

    while (std::getline(f, line))
    {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string cell;

        /* Column 0: timeframe (ignored — implicit by row order) */
        if (!std::getline(ss, cell, ',')) continue;

        std::array<int, 3> succ{};
        for (int i = 0; i < 3; ++i)
        {
            if (!std::getline(ss, cell, ','))
                throw std::runtime_error("ReplayChannel: malformed row in " + csv_path);
            succ[i] = std::stoi(cell);
        }
        /* Remaining columns (maxdist_*) ignored. */
        rows.push_back(succ);
    }

    if (rows.empty())
        throw std::runtime_error("ReplayChannel: no data rows in " + csv_path);
}

double ReplayChannel::gen_probability(unsigned int transmission_power)
{
    switch (transmission_power)
    {
        case 1U:  return PROB_1W;
        case 10U: return PROB_10W;
        case 25U: return PROB_25W;
        default:
            std::cerr << "ReplayChannel: unsupported power " << transmission_power
                      << "W — returning 0.0\n";
            return 0.0;
    }
}

int ReplayChannel::power_column(unsigned int transmission_power)
{
    switch (transmission_power)
    {
        case 1U:  return 0;
        case 10U: return 1;
        case 25U: return 2;
        default:  return -1;
    }
}

double ReplayChannel::mean_probability(unsigned int transmission_power)
{
    /* The replay channel's analogue of sum_s pi_s rho_s: the empirical decode
       rate over the whole recorded window. There is no FSMC to average over,
       so the recording itself supplies the long-run distribution.

       Note this is the *measured* rate, whereas gen_probability() returns the
       rounded PROB_* view the predictor is shown. The two should agree to
       within that rounding; a wide gap means the constants are stale with
       respect to the CSV. */
    const int col = power_column(transmission_power);
    if (col < 0)
    {
        std::cerr << "ReplayChannel: unsupported power " << transmission_power
                  << "W — returning 0.0\n";
        return 0.0;
    }

    if (rows.empty()) return 0.0;  /* the constructor rejects this; belt and braces */

    unsigned long long successes = 0;
    for (const auto& row : rows)
        if (row[col] != 0) ++successes;

    return static_cast<double>(successes) / static_cast<double>(rows.size());
}

received_frame_t ReplayChannel::gen_frame_with_probability(transmitted_frame_t transmitted_frame)
{
    received_frame_t recv;
    recv.packet             = transmitted_frame.packet;
    recv.transmission_power = transmitted_frame.transmission_power;
    recv.frequency          = transmitted_frame.frequency;

    const int col = power_column(transmitted_frame.transmission_power);
    if (col < 0)
    {
        std::cerr << "ReplayChannel: unsupported power "
                  << transmitted_frame.transmission_power
                  << "W — treating frame as failed\n";
        recv.success_prob = 0.0;
        return recv;
    }

    const unsigned int t = *system_tick;
    if (t >= rows.size())
    {
        /* Past the recorded window — treat as failed rather than wrapping
           around, which would silently corrupt long-duration runs. */
        recv.success_prob = 0.0;
        return recv;
    }

    recv.success_prob = (rows[t][col] != 0) ? 1.0 : 0.0;
    return recv;
}

