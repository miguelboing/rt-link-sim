#pragma once

#include <memory>

#include "schedulers/base_scheduler.hpp"

class RM_scheduler : public BaseScheduler
{
public:
    RM_scheduler(unsigned int tx_power, std::size_t channel, BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick);
    unsigned int tx_power;

    /* c: the one channel this scheduler transmits on. Single-channel for now;
       the choice becomes per-slot once the scheduler reasons over the whole
       channel set. */
    std::size_t channel;
    scheduled_frame_t do_schedule_frame(void) override;

    void receive_prediction(const std::vector<double>& pred_probs) override {}
    /* No get_prediction_powers() override: this scheduler transmits at a fixed
       power and needs no predictions, which is the base class's empty default. */

    std::string get_name() const override;
};
