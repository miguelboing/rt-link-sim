#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <fstream>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "system_model/system_model.hpp"
#include "system_model/buffer_packet/buffer_packet.hpp"
#include "system_model/channel_model/channel_model.hpp"

/* ── Per-channel knowledge (paper Sec. 3.2, "Radio Scheduler") ───────────────

   The scheduler is given a separately fitted model for *every* channel in
   C = {0, ..., C-1}, not one for the channel it happens to sit on, because
   the transmit action is the triple (frame, power, channel) and picking a
   channel means comparing them. This struct is one entry of that set: the
   fitted model M_hat_c and the latest inferred label s_hat_c(t). That is the
   estimate and nothing more -- how much a scheduler still *trusts* the label,
   and when it refreshes it, is a listening policy and lives with the
   schedulers that chose one (see schedulers/policies/belief_tracker.hpp).

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

    /* Record what an RX slot on channel c inferred: s_hat_c <- state.
       Returns whether anything was recorded, so a caller tracking belief
       in the label knows whether it has a new one: a negative label (the
       predictor had none) leaves the held estimate alone rather than
       discarding it -- a missing observation is not evidence the channel
       changed -- and must not be read as a refresh. */
    bool observe_state(std::size_t c, int state)
    {
        if (c >= this->channels.size() || state < 0) return false;

        this->channels[c].believed_state = state;
        return true;
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

    /* ── The RX hook ─────────────────────────────────────────────────────

       s_hat_c(t) for the channel the slot was spent on: a model-state label,
       or -1 for no usable label. Recording it is all the base does, and all
       most schedulers need -- the decode estimates are derived from it on
       demand by rho_hat(), not cached, which is what lets a scheduler ask
       about a channel it is not currently using. */
    virtual void receive_state(std::size_t c, int state) { this->observe_state(c, state); }

    std::shared_ptr<std::vector<packet_t>> buffer_packet; /* Vector that points to the queue to be scheduled */

    std::shared_ptr<unsigned int> system_tick;

    BufferPacket* buffer;

protected:
    /* C = {0, ..., C-1} in config order. main.cpp populates this for every
       scheduler and refuses to start if any channel has no fitted model, so a
       scheduler that reads it always finds every channel here. The ones that
       do not reason about the channel (RM, EDF, MPRM, MPEDF) simply never look. */
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
