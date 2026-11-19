#pragma once

#include <memory>

#include "schedulers/base_scheduler.hpp"

/* RM -- rate-monotonic order at a fixed power, one send per frame.

   The fixed-priority baseline: no channel knowledge, no redundancy, no
   listening. It sends the queued packet whose task repeats most often and
   spends exactly one slot on it, so whatever reliability it reaches is
   whatever the channel gives for a single transmission.

   It exists as the control for queue discipline. Against the
   earliest-deadline baseline at the same power, everything else is held equal
   and the only difference is the order the queue is served in -- which under
   overload is a large effect, because fixed-priority order starves a
   long-period task indefinitely while a deadline order does not. */
class RM_scheduler : public BaseScheduler
{
public:
    RM_scheduler(unsigned int tx_power, std::size_t channel, BufferPacket* buffer,
                 std::shared_ptr<unsigned int> sys_tick, std::vector<channel_estimate_t> channels);

    unsigned int tx_power;

    /* c: the one channel this scheduler transmits on, from the config. With no
       channel knowledge there is nothing to choose on. */
    std::size_t channel;

    scheduled_frame_t do_schedule_frame(void) override;

    std::string get_name() const override;
};
