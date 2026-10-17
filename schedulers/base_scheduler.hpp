#pragma once

#include <vector>
#include <memory>
#include <fstream>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "system_model/system_model.hpp"
#include "system_model/buffer_packet/buffer_packet.hpp"

/* Per-tick decay of a scheduler's confidence in the channel estimate it last
   heard. This is the modulus of the FSMC's second-largest eigenvalue -- the
   rate at which the chain's state distribution forgets where it started -- so
   belief tracks how stale the estimate actually is rather than how long ago it
   arrived. For the 20m ring (0.98 self-loop on a 6-state cycle) that modulus
   is |0.98 + 0.02 e^{i pi/3}| ~ 0.99015.

   Shared by every belief-driven scheduler (CATS, CHARM, CHEDF) so the three
   cannot drift apart; it was a bare literal inside CATS before CHARM and CHEDF
   adopted the same policy. It is a property of the *channel*, not of any
   scheduler, so if the transition matrix ever changes this constant has to be
   recomputed with it -- nothing in the build checks the two against each
   other. */
inline constexpr float BELIEF_DECAY = 0.99015f;

class BaseScheduler
{
public:
    BaseScheduler(BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick);

    virtual ~BaseScheduler() = default;

    scheduled_frame_t schedule_frame(void);

    void reset_scheduler();

    void change_buffer_packet(std::shared_ptr<std::vector<packet_t>> buffer);

    void save_to_file(void);

    /* Toggle per-frame logging. Disabling skips building and storing the
       frame_entry JSON, which dominates per-binary memory at high tick counts. */
    void set_log_enabled(bool enabled) { log_enabled = enabled; }

    /* Pure virtual functions that all schedulers must implement */
virtual std::string get_name() const = 0;

    /* Non Pure virtual function */
    virtual void receive_prediction(const std::vector<double>& pred_probs); /* This function handles the channel predictions */

    virtual std::vector<unsigned int> get_prediction_powers() const { return {}; } /* Powers (W) the scheduler wants predicted */

    std::shared_ptr<std::vector<packet_t>> buffer_packet; /* Vector that points to the queue to be scheduled */

    std::shared_ptr<unsigned int> system_tick;

    BufferPacket* buffer;

private:
    virtual scheduled_frame_t do_schedule_frame(void) = 0;

    json frame_log;  /* Stores all received packets */
    bool log_enabled = true;
};

inline BaseScheduler::BaseScheduler(BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick):
    system_tick(sys_tick), buffer(buffer), frame_log(json::array())
{
    this->change_buffer_packet(buffer->buffer_packet);
}

inline void BaseScheduler::change_buffer_packet(std::shared_ptr<std::vector<packet_t>> buffer)
{
    this->buffer_packet = buffer; /* Keep an untouched reference to the original queue */
}

inline void BaseScheduler::receive_prediction(const std::vector<double>& pred_probs)
{
    return;
}

inline scheduled_frame_t BaseScheduler::schedule_frame(void)
{
    scheduled_frame_t scheduled_frame = this->do_schedule_frame();

    if (!this->log_enabled) return scheduled_frame;

    json frame_entry;
    switch (scheduled_frame.radio_mode)
    {
        case TX_MODE:
            frame_entry = {
                {"radio_mode",        "TX_MODE"},
                {"system_tick",       *(this->system_tick)},
                {"frequency",         scheduled_frame.frequency},
                {"transmission_power",scheduled_frame.transmission_power},
                {"packet_id",         scheduled_frame.packet->id},
                {"packet_id_count",   scheduled_frame.packet->id_count},
                {"deadline",          scheduled_frame.packet->deadline},
                {"frames",            scheduled_frame.packet->frames},
                {"frame_count",       scheduled_frame.packet->frame_count},
                {"reliability_req",  scheduled_frame.packet->reliability_req}
            };
            break;

        case RX_MODE:
            frame_entry = {
                {"system_tick",       *(this->system_tick)},
                {"radio_mode",        "RX_MODE"},
                {"frequency",         scheduled_frame.frequency},
                {"transmission_power",scheduled_frame.transmission_power}
            };
            break;

        case IDLE:
            frame_entry = {
                {"radio_mode",  "IDLE"},
                {"system_tick", *(this->system_tick)}
            };
            break;
    }

    frame_log.push_back(frame_entry);

    return scheduled_frame;
}

inline void BaseScheduler::save_to_file(void)
{
    std::ofstream file(this->get_name() + "_scheduled_packets.json");
    file << frame_log.dump(4);
}
