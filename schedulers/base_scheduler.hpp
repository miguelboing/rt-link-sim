#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
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

/* ── Per-channel knowledge (paper Sec. 3.2, "Radio Scheduler") ───────────────

   The scheduler is given a separately fitted model for *every* channel in
   C = {0, ..., C-1}, not one for the channel it happens to sit on, because
   the transmit action is the triple (frame, power, channel) and picking a
   channel means comparing them. This struct is one entry of that set: the
   fitted model M_hat_c, the latest inferred label s_hat_c(t), and the slot
   tau_c(t) it was inferred in.

   `believed_state < 0` is the paper's bottom symbol: the channel has not been
   observed yet, and its decode estimates come from the fitted stationary
   statistics instead (eq. 27).

   The index into the scheduler's vector *is* the paper's channel index c --
   the set is built in the simulator's channel order, so one number addresses
   the channel everywhere: in a scheduled_frame, in a predictor query and
   here. `name` is the channel's directory under physical_channels/, carried
   for logs and errors only; nothing looks a channel up by it.

   The model is held by value and is *not* optional: a channel the scheduler
   must choose between is a channel it has a fitted model of, and there is no
   coherent action to take on one it cannot evaluate. Building an entry
   therefore requires the model, which is what keeps a half-initialised
   channel set from existing. */
struct channel_estimate_t
{
    /* The channel's name, exactly its directory under physical_channels/
       ("cyclic_fast_6"). Descriptive, not an identifier -- c is. */
    std::string name;

    /* M_hat_c: the scheduler's own copy of the fitted model. By value, so a
       mismatch (Delta_A, Delta_rho) can be injected per scheduler without
       touching the physical channel. */
    ChannelModel model;

    /* s_hat_c(t). -1 is the paper's bottom: no observation yet. */
    int believed_state = -1;

    /* tau_c(t): the slot the label above was inferred in. Meaningful only
       once believed_state >= 0. */
    unsigned int last_observed = 0U;
};

class BaseScheduler
{
public:
    BaseScheduler(BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick,
                  std::vector<channel_estimate_t> channels = {});

    virtual ~BaseScheduler() = default;

    scheduled_frame_t schedule_frame(void);

    void reset_scheduler();

    void change_buffer_packet(std::shared_ptr<std::vector<packet_t>> buffer);

    void save_to_file(void);

    /* Toggle per-frame logging. Disabling skips building and storing the
       frame_entry JSON, which dominates per-binary memory at high tick counts. */
    void set_log_enabled(bool enabled) { log_enabled = enabled; }

    /* ── The channel set ──────────────────────────────────────────────────

       Supplied once, after construction, so adding the set did not have to
       ripple through the constructor of every scheduler that ignores it.
       Order is the config's order, which is what fixes the channel index. */
    void set_channels(std::vector<channel_estimate_t> channels)
    {
        this->channels = std::move(channels);
    }

    const std::vector<channel_estimate_t>& get_channels() const { return this->channels; }

    std::size_t n_channels() const { return this->channels.size(); }

    /* Record what an RX slot on channel c inferred: s_hat_c <- state,
       tau_c <- now. A negative label (the predictor had none) leaves the held
       estimate alone rather than discarding it -- a missing observation is not
       evidence the channel changed, and the age keeps running, so the estimate
       decays on its own. */
    void observe_state(std::size_t c, int state)
    {
        if (c >= this->channels.size() || state < 0) return;

        channel_estimate_t& ch = this->channels[c];
        ch.believed_state = state;
        ch.last_observed  = *(this->system_tick);
    }

    /* d = t - tau_c(t), the age of channel c's estimate in slots. Every
       channel ages every slot: the physical channel evolves whether the radio
       transmits, idles, or listens somewhere else. This is the part a running
       "belief *= decay on TX" cannot express once there is more than one
       channel, which is why age is stored and belief derived from it. */
    unsigned int observation_age(std::size_t c) const
    {
        const channel_estimate_t& ch = this->channels[c];
        if (ch.believed_state < 0) return 0U;

        const unsigned int now = *(this->system_tick);
        return now > ch.last_observed ? now - ch.last_observed : 0U;
    }

    /* Confidence that channel c is still in the state it was last seen in:
       A_ss(s_hat_c)^d, the model probability it has not left. Zero before the
       first observation, so an unobserved channel always reads as worth
       listening to. */
    float belief(std::size_t c) const
    {
        const channel_estimate_t& ch = this->channels[c];
        if (ch.believed_state < 0) return 0.0f;

        const float per_slot = static_cast<float>(
            ch.model.p_self(static_cast<std::size_t>(ch.believed_state)));
        float conf = 1.0f;
        for (unsigned int d = this->observation_age(c); d > 0U; --d)
            conf *= per_slot;

        return conf;
    }

    /* rho_hat_{c,j}(t), eq. 27: the decode estimate for channel c at this
       power, read off the fitted response at the last inferred state, or off
       the fitted stationary statistics when the channel has never been
       observed. One call per power rebuilds the whole row from one label,
       which is why a mislabelled state moves every power level together. */
    double rho_hat(std::size_t c, unsigned int transmission_power) const
    {
        const channel_estimate_t& ch = this->channels[c];

        return ch.believed_state < 0
            ? ch.model.rho_bar(transmission_power)
            : ch.model.rho(static_cast<std::size_t>(ch.believed_state), transmission_power);
    }

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

protected:
    /* C = {0, ..., C-1} in config order. Empty for a scheduler that was never
       given a channel set, which is every scheduler that does not reason about
       the channel (RM, EDF) and, until each is migrated, the ones that still
       hold their own single-channel copy. */
    std::vector<channel_estimate_t> channels;

private:
    virtual scheduled_frame_t do_schedule_frame(void) = 0;

    json frame_log;  /* Stores all received packets */
    bool log_enabled = true;
};

inline BaseScheduler::BaseScheduler(BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick,
                                    std::vector<channel_estimate_t> channels):
    system_tick(sys_tick), buffer(buffer), channels(std::move(channels)), frame_log(json::array())
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
                {"channel",           scheduled_frame.channel},
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
                {"channel",           scheduled_frame.channel},
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
