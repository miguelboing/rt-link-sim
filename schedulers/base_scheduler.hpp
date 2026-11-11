#pragma once

#include <vector>
#include <memory>
#include <fstream>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <optional>

#include "system_model/system_model.hpp"
#include "system_model/buffer_packet/buffer_packet.hpp"
#include "system_model/channel_model/channel_model.hpp"

/* Fallback per-slot decay of a scheduler's confidence in the estimate it last
   heard, used only for a channel that has no estimated model to decay by --
   ReplayChannel, which has no state space and so no self-transition
   probability. It is the modulus of cyclic_fast_6's second-largest
   eigenvalue, |0.98 + 0.02 e^{i pi/3}| ~ 0.99015, which is that one chain's
   forgetting rate and nothing else's.

   A scheduler that *does* hold a model never reads this: it decays belief by
   the self-transition probability of the state it believes the channel is in
   (see belief_decay() below), which is the right rate per channel and per
   state rather than one constant standing in for all of them. This is why the
   four shipped channels no longer need a constant each. */
inline constexpr float BELIEF_DECAY = 0.99015f;

/* Per-slot persistence of a held state estimate: the model probability that
   the channel has not left the state it was last observed in, so confidence
   after d transmitting slots is p_self(s)^d and the probability of having
   left is 1 - p_self(s)^d. Falls back to BELIEF_DECAY when there is no model
   or no observation yet, which is the only case that constant still covers.

   Lives here because CATS, CHARM and CHEDF all decay belief this way and a
   copy in each of the three is the drift hazard the constant already was. */
inline float belief_decay(const std::optional<ChannelModel>& model, int believed_state)
{
    if (!model.has_value() || believed_state < 0)
        return BELIEF_DECAY;

    return static_cast<float>(model->p_self(static_cast<std::size_t>(believed_state)));
}

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

    /* The predictor's output when the scheduler holds an estimated model:
       s_hat_c(t), a model-state label, or -1 for no usable label. The
       scheduler derives its own decode estimates from it. */
    virtual void receive_state(int state);

    /* True when an RX slot should be answered with receive_state() rather than
       receive_prediction(). A scheduler holding a model says yes; one pointed
       at a channel with no model (ReplayChannel) has no state space to read a
       label in and keeps the probability path. */
    virtual bool wants_state_prediction() const { return false; }

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

inline void BaseScheduler::receive_state(int state)
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
