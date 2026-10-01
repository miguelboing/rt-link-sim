#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "physical_channels/base_physical_channel.hpp"

/* Deterministic channel that replays a recorded per-tick success table.

   The predictor (CATS) sees a fixed view of the channel: 0.07 / 0.58 / 0.80
   for 1 / 10 / 25 W. The actual per-frame outcome at tick t is taken from
   the CSV column matching the chosen transmission power (success_prob set
   to 1.0 or 0.0 so the receiver's Bernoulli draw is deterministic). */
class ReplayChannel: public BasePhysicalChannel
{
public:
    ReplayChannel(unsigned int frequency,
                  const std::string& csv_path,
                  std::shared_ptr<unsigned int> sys_tick);

    double gen_probability(unsigned int transmission_power) override;
    double mean_probability(unsigned int transmission_power) override;
    received_frame_t gen_frame_with_probability(transmitted_frame_t transmitted_frame) override;

private:
    /* Column of `rows` carrying the outcomes for a transmission power, or -1
       if the power is not one of the three recorded ones. */
    static int power_column(unsigned int transmission_power);

    /* CSV row: [success_1W, success_10W, success_25W] as 0/1 ints. */
    std::vector<std::array<int, 3>> rows;
    std::shared_ptr<unsigned int> system_tick;
};

