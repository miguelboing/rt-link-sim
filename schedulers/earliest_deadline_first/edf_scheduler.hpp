#pragma once

#include <memory>

#include "schedulers/base_scheduler.hpp"

class EDF_scheduler : public BaseScheduler
{
public:
    EDF_scheduler(unsigned int tx_power, std::size_t channel, BufferPacket* buffer,
                  std::shared_ptr<unsigned int> sys_tick, std::vector<channel_estimate_t> channels);
    unsigned int tx_power;

    /* c: the one channel this scheduler transmits on. Single-channel for now;
       the choice becomes per-slot once the scheduler reasons over the whole
       channel set. */
    std::size_t channel;
    scheduled_frame_t do_schedule_frame(void) override;

    std::string get_name() const override;
};

