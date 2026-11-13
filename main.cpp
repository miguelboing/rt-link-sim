#include <iostream>
#include <fstream>
#include <optional>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <algorithm>
#include <streambuf>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

/* System Model */
#include "system_model/system_model.hpp"
#include "system_model/buffer_packet/buffer_packet.hpp"
#include "system_model/channel_model/channel_model.hpp"
#include "system_model/radio_interface/radio_interface.hpp"
#include "system_model/target_receiver/target_receiver.hpp"
#include "system_model/ml_predictor/ml_predictor.hpp"

#include "packet_generators/fixed_rate/fixed_rate.hpp"
#include "physical_channels/base_physical_channel.hpp"
#include "physical_channels/sigmoid_channel/sigmoid_channel.hpp"
#include "physical_channels/replay_channel/replay_channel.hpp"

#include "schedulers.hpp"

// Null streambuf used to silence std::cout cheaply when running in summary
// mode — keeps the hot loop free of pipe writes and flushes.
struct NullBuf : std::streambuf {
    int overflow(int c) override { return c; }
};

int main(int argc, char* argv[])
{
    const std::string config_file = (argc > 1) ? argv[1] : "simulation_config.json";
    const std::string log_file    = (argc > 2) ? argv[2] : "simulation_log.json";
    // argv[3] == "summary" activates summary-only mode: stdout silenced,
    // per-frame JSON log skipped, and a tiny aggregate written to log_file.
    const bool summary_only       = (argc > 3) && std::string(argv[3]) == "summary";

    NullBuf null_buf;
    std::streambuf* orig_cout = nullptr;
    if (summary_only) orig_cout = std::cout.rdbuf(&null_buf);

    std::ifstream f(config_file);
    if (!f.is_open())
    {
        std::cerr << "ERROR: Could not open config file: " << config_file << std::endl;
        return -1;
    }
    json config = json::parse(f);

    std::shared_ptr<unsigned int> system_tick = std::make_shared<unsigned int>(0U);

    /* Initialize packet buffer */
    BufferPacket buffer(system_tick);

    /* Shared spawn log for all generators */
    std::shared_ptr<json> spawn_log = std::make_shared<json>(json::array());

    /* Initialize packet generators from config */
    std::vector<FixedRate_PacketGen> packet_gens;
    for (const auto& gen : config["packet_generators"])
    {
        if (gen["type"] == "fixed_rate")
        {
            std::vector<fixed_rate_packet_t> packets;
            for (const auto& p : gen["packets"])
            {
                packets.push_back(fixed_rate_packet_t(
                    p["relative_deadline"],
                    p["frames"],
                    p["reliability"],
                    p["id"],
                    p["period"],
                    p["phase"]
                ));
            }
            packet_gens.emplace_back(system_tick, packets, buffer.buffer_packet, spawn_log);
            if (summary_only) packet_gens.back().set_log_enabled(false);
        }
    }

    // Optional master seed for reproducibility. When present, every RNG is
    // reseeded with a distinct offset so streams stay independent.
    const bool has_seed   = config["simulation"].contains("seed");
    const uint64_t seed   = has_seed ? config["simulation"]["seed"].get<uint64_t>() : 0;

    /* Initialize channels from config. A channel is two data files, and the
       loaders throw when they disagree with each other or cannot be read --
       a mis-built channel directory, not a bug -- so report it as a config
       error rather than letting it abort the process. */
    auto channels = std::make_shared<std::vector<std::unique_ptr<BasePhysicalChannel>>>();
    try
    {
        /* Declaration order is the channel index c: the channel at position
           c in this vector is channel c everywhere else -- in a scheduled
           frame, in a predictor query, and in a scheduler's channel set. A
           channel carries its name for logs and errors, nothing addresses it
           by that. */
        size_t ch_idx = 0;
        for (const auto& ch : config["channels"])
        {
            if (ch["type"] == "sigmoid")
            {
                auto sc = std::make_unique<SigmoidChannel>(ch["name"]);
                if (has_seed) sc->seed_rng(seed + 0x100 + ch_idx);
                channels->emplace_back(std::move(sc));
            }
            else if (ch["type"] == "replay")
            {
                auto rc = std::make_unique<ReplayChannel>(
                    ch.value("name", std::string("replay")),
                    ch["csv_path"].get<std::string>(), system_tick);
                channels->emplace_back(std::move(rc));
            }
            else
            {
                std::cerr << "ERROR: Unknown channel type: "
                          << ch["type"].get<std::string>() << std::endl;
                return -1;
            }
            ++ch_idx;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return -1;
    }

    if (channels->empty())
    {
        std::cerr << "ERROR: config declares no channels" << std::endl;
        return -1;
    }

    /* Initialize scheduler from config */
    const auto& sched_cfg = config["scheduler"];
    const std::string sched_type = sched_cfg["type"];

    /* c: the channel this scheduler operates on, as an index into the
       `channels` block. Defaults to 0, the single channel most configs
       declare. Single-channel for now -- the index becomes a per-slot
       decision once the scheduler reasons over the whole channel set. */
    const std::size_t sched_channel = sched_cfg.value("channel", std::size_t{0});

    if (sched_channel >= channels->size())
    {
        std::cerr << "ERROR: " << sched_type << " asks for channel " << sched_channel
                  << " but the config declares " << channels->size()
                  << " channel(s) (valid indices 0.." << channels->size() - 1 << ")"
                  << std::endl;
        return -1;
    }

    /* The scheduler's channel set: its own *copy* of the fitted model of
       every channel in C, in the simulator's channel order, so the index c
       addresses the same channel on both sides. A scheduler that reasons about
       the channel reads its decode estimates and its confidence out of this
       and holds no per-channel state of its own.

       Copies, not pointers, because the estimated model is meant to be
       perturbable (Delta_A, Delta_rho) independently of the physical channel;
       as shipped the two are identical, which is the zero-mismatch reference
       case.

       Every channel must have one. A channel with no fitted model has no
       state space to label and no A_ss to age by, so it cannot be an entry --
       and leaving it out would renumber every channel after it. There is no
       longer a fallback for that case: the scheduler reasons about an FSMC or
       there is nothing for it to reason about, so say so here rather than let
       a run proceed on an estimate nobody can age. */
    std::vector<channel_estimate_t> sched_channels;
    sched_channels.reserve(channels->size());
    for (std::size_t c = 0; c < channels->size(); ++c)
    {
        const ChannelModel* m = (*channels)[c]->estimated_model();
        if (m == nullptr)
        {
            std::cerr << "ERROR: channel " << c << " (" << (*channels)[c]->name
                      << ") has no fitted FSMC, which a scheduler has no way to"
                         " reason about" << std::endl;
            return -1;
        }
        sched_channels.push_back({(*channels)[c]->name, *m});
    }

    std::unique_ptr<BaseScheduler> scheduler;
    if (sched_type == "CHEDF")
    {
        /* belief_threshold, not rx_period: listening is belief-driven, the
           same rule CATS uses. */
        scheduler = std::make_unique<CHEDF_scheduler>(
            sched_cfg["tx_power"],
            sched_channel,
            sched_cfg["belief_threshold"],
            &buffer,
            system_tick,
            std::move(sched_channels)
        );
    }
    else if (sched_type == "SCS")
    {
        /* Picks its own channel from the long-run means in the channel set, so
           it takes no `channel` field. Say so rather than let a config set one
           and silently have no effect. */
        if (sched_cfg.contains("channel"))
            std::cerr << "WARNING: " << sched_type << " chooses its own channel;"
                         " the `channel` field in the config is ignored." << std::endl;

        scheduler = std::make_unique<SCS_scheduler>(
            sched_cfg["tx_power"],
            &buffer,
            system_tick,
            std::move(sched_channels)
        );
    }
    else if (sched_type == "EDF")
    {
        scheduler = std::make_unique<EDF_scheduler>(
            sched_cfg["tx_power"],
            sched_channel,
            &buffer,
            system_tick,
            std::move(sched_channels)
        );
    }
    else if (sched_type == "CATS")
    {
        /* CATS is aware of the periodic task set so it can size demand/U
           against the horizon. Aggregate across all generators. */
        std::vector<periodic_task_t> periodic_tasks;
        for (const auto& gen : packet_gens)
        {
            auto t = gen.get_periodic_tasks();
            periodic_tasks.insert(periodic_tasks.end(), t.begin(), t.end());
        }

        /* The three ablation switches default to enabled, so a config that
           omits them is the published CATS. `tx_power` is only read when
           adaptive_power is false. */
        /* The constructor rejects a fixed tx_power that is not a predictor
           tier. Report it like any other bad config rather than letting the
           exception abort the process -- a sweep worker would otherwise see
           SIGABRT and no explanation. */
        try
        {
            scheduler = std::make_unique<CATS_scheduler>(
                sched_channel,
                sched_cfg["belief_threshold"],
                sched_cfg["utilization_threshold"],
                periodic_tasks,
                sched_cfg.value("adaptive_power", true),
                sched_cfg.value("tx_power", 25U),
                sched_cfg.value("early_drop", true),
                sched_cfg.value("urgency_check", true),
                &buffer,
                system_tick,
                std::move(sched_channels)
            );
        }
        catch (const std::exception& e)
        {
            std::cerr << "ERROR: " << e.what() << std::endl;
            return -1;
        }
    }
    else
    {
        std::cerr << "ERROR: Unknown scheduler type: " << sched_type << std::endl;
        return -1;
    }

    if (summary_only) scheduler->set_log_enabled(false);

    scheduled_frame_t scheduled_frame;

    /* Initialize the radio_interface */
    RadioInterface radio_interface(buffer.buffer_packet);
    transmitted_frame_t transmitted_frame;

    /* Initialize the ML Predictor */
    const double predict_error = config["simulation"].value("predict_error", 0.0);

    /* predict_error perturbs a predicted decode *probability*, and the
       predictor returns a state label -- there is nothing in that path to
       perturb, and now that every channel is an FSMC there is no other path.
       State-prediction error is the confusion matrix E_c and is not
       implemented yet, so say so rather than let a sweep draw identical curves
       at every error level and read them as insensitivity to prediction
       error. */
    if (predict_error > 0.0)
        std::cerr << "WARNING: predict_error=" << predict_error
                  << " has no effect: the predictor returns a channel state,"
                     " and state-prediction error (the confusion matrix) is"
                     " not implemented." << std::endl;

    MLPredictor ml_predictor(system_tick, channels, predict_error);
    if (has_seed) ml_predictor.seed_rng(seed + 0x300);

    /* Initialize the target receiver */
    TargetReceiver target_receiver(system_tick);
    if (has_seed) target_receiver.seed_rng(seed + 0x200);
    if (summary_only) target_receiver.set_log_enabled(false);
    received_frame_t recv_frame;

    const unsigned int duration = config["simulation"]["duration"];
    json simulation_log = json::array();

    // Summary-only aggregates — only touched when summary_only is true.
    std::map<std::pair<int,int>, int> instance_frames_needed;
    std::set<std::tuple<int,int,int>> received_slots;
    double total_tx_power = 0.0;

    /* Radio occupancy per channel, counted from what the scheduler decided
       rather than from what the transmitter managed: a slot committed to a
       channel is spent on it whether or not a frame went out. IDLE slots are
       not counted against any channel -- the radio is off, not somewhere. */
    std::vector<unsigned long> channel_tx_slots(channels->size(), 0UL);
    std::vector<unsigned long> channel_rx_slots(channels->size(), 0UL);

    for (unsigned int i = 0U; i < duration; i++)
    {
        for (auto& gen : packet_gens)
            gen.generate_packets();

        json frame_entry;
        if (!summary_only)
        {
            frame_entry["tick"] = *system_tick;

            /* Log buffer state */
            frame_entry["buffer"] = json::array();
            for (const auto& packet : *buffer.buffer_packet)
            {
                frame_entry["buffer"].push_back({
                    {"id",               packet.id},
                    {"id_count",         packet.id_count},
                    {"deadline",         packet.deadline},
                    {"frames",           packet.frames},
                    {"frame_count",      packet.frame_count},
                    {"reliability_req", packet.reliability_req}
                });
            }

            std::cout << "--------------------------------------------------------------------------------" << std::endl;
            std::cout << "Frame " << *system_tick << " - Buffer contents: " << std::endl;
            for (const auto& packet : *buffer.buffer_packet) {
                std::cout << "(id: "            << packet.id
                          << ", id_count: "     << packet.id_count
                          << ", deadline: "     << packet.deadline
                          << ", frames: "       << packet.frames
                          << ", reliability: " << packet.reliability_req << ") ";
                std::cout << std::endl;
            }

            std::cout << std::endl;
        }

        scheduled_frame = scheduler->schedule_frame();

        if (scheduled_frame.channel < channels->size())
        {
            if (scheduled_frame.radio_mode == TX_MODE)
                channel_tx_slots[scheduled_frame.channel]++;
            else if (scheduled_frame.radio_mode == RX_MODE)
                channel_rx_slots[scheduled_frame.channel]++;
        }

        if (!summary_only) std::cout << "Radio is in ";

        switch (scheduled_frame.radio_mode)
        {
            case TX_MODE:
            {
                if (!summary_only)
                {
                    std::cout << "TX MODE" << std::endl;
                    frame_entry["radio_mode"] = "TX_MODE";
                }

                auto result = radio_interface.transmit_frame(scheduled_frame);

                if (result.has_value())
                {
                    transmitted_frame = result.value();
                }
                else
                {
                    std::cerr << "Failed to transmit a frame" << std::endl;
                    return -1;
                }

                if (!summary_only)
                {
                    std::cout    << "Transmitted frame: "
                                 << "(id: "                << transmitted_frame.packet.id
                                 << ", id_count: "         << transmitted_frame.packet.id_count
                                 << ", deadline: "         << transmitted_frame.packet.deadline
                                 << ", frames: "           << transmitted_frame.packet.frames
                                 << ", frame_count: "      << transmitted_frame.packet.frame_count
                                 << ", reliability: "     << transmitted_frame.packet.reliability_req << ") ";
                    std::cout    << std::endl;
                }

                if (transmitted_frame.channel < channels->size())
                {
                    recv_frame = (*channels)[transmitted_frame.channel]
                                     ->gen_frame_with_probability(transmitted_frame);
                    bool received = target_receiver.recv_frame(recv_frame);

                    if (summary_only)
                    {
                        const int pid       = transmitted_frame.packet.id;
                        const int pid_count = transmitted_frame.packet.id_count;
                        instance_frames_needed[{pid, pid_count}] = transmitted_frame.packet.frames;
                        total_tx_power += recv_frame.transmission_power;
                        if (received)
                        {
                            received_slots.insert(std::make_tuple(
                                pid, pid_count, transmitted_frame.packet.frame_count));
                        }
                    }
                    else
                    {
                        std::cout   << "TX Power : "                   << recv_frame.transmission_power
                                    << "\nProbability for the frame: " << recv_frame.success_prob
                                    << " and required probability: "   << recv_frame.packet.reliability_req;
                        std::cout << std::endl;
                        std::cout << "Frame successfully received by the target? " << received << std::endl;

                        frame_entry["transmission"] = {
                            {"packet", {
                                {"id",               transmitted_frame.packet.id},
                                {"id_count",         transmitted_frame.packet.id_count},
                                {"deadline",         transmitted_frame.packet.deadline},
                                {"frames",           transmitted_frame.packet.frames},
                                {"frame_count",      transmitted_frame.packet.frame_count},
                                {"reliability_req", transmitted_frame.packet.reliability_req}
                            }},
                            {"tx_power",         recv_frame.transmission_power},
                            {"channel",          recv_frame.channel},
                            {"probability",      recv_frame.success_prob},
                            {"reliability_req", recv_frame.packet.reliability_req},
                            {"received",         received}
                        };
                    }
                }
                else
                {
                    std::cerr << "ERROR: frame scheduled on channel "
                              << transmitted_frame.channel << ", which does not exist"
                              << std::endl;
                }

                break;
            }
            case RX_MODE:
                if (!summary_only)
                {
                    std::cout << "RX MODE" << std::endl;
                    frame_entry["radio_mode"] = "RX_MODE";
                }

                /* An RX slot buys one thing: the model-state label observed
                   on the channel it was spent on. The scheduler derives its own
                   decode probabilities from it, off its copy of that channel's
                   response curves -- so a single observation refreshes every
                   power tier together, from the same state and with the same
                   age. */
                {
                    const int pred_state =
                        ml_predictor.predict_channel_state(scheduled_frame.channel);

                    if (!summary_only)
                    {
                        std::cout << "Predicted state on channel "
                                  << scheduled_frame.channel << ": " << pred_state
                                  << std::endl;
                    }

                    scheduler->receive_state(scheduled_frame.channel, pred_state);

                    if (!summary_only)
                    {
                        frame_entry["prediction"] = {
                            {"state",       pred_state},
                            {"channel",     scheduled_frame.channel}
                        };
                    }
                }

                break;

            case IDLE:
                if (!summary_only)
                {
                    std::cout << "IDLE MODE" << std::endl;
                    frame_entry["radio_mode"] = "IDLE";
                }
                break;

            default:
                break;
        }

        /* Log missed packets */
        auto missed_packets = buffer.check_deadlines();
        if (!summary_only) frame_entry["missed_packets"] = json::array();
        for (const auto& missed : missed_packets)
        {
            if (summary_only)
            {
                instance_frames_needed.try_emplace(
                    std::make_pair(missed.id, missed.id_count), missed.frames);
            }
            else
            {
                std::cout << "Missed deadline for packet:"
                          << " ID: "       << missed.id
                          << " ID Count: " << missed.id_count
                          << " Deadline: " << missed.deadline
                          << " Frames: "   << missed.frames
                          << " RR Req: "   << missed.reliability_req
                          << std::endl;

                frame_entry["missed_packets"].push_back({
                    {"id",               missed.id},
                    {"id_count",         missed.id_count},
                    {"deadline",         missed.deadline},
                    {"frames",           missed.frames},
                    {"frame_count",      missed.frame_count},
                    {"reliability_req", missed.reliability_req}
                });
            }
        }

        /* Log dropped packets */
        if (!summary_only) frame_entry["dropped_packets"] = json::array();
        for (const auto& dropped : buffer.dropped_packets)
        {
            if (summary_only)
            {
                instance_frames_needed.try_emplace(
                    std::make_pair(dropped.id, dropped.id_count), dropped.frames);
            }
            else
            {
                std::cout << "Dropped packet:"
                          << " ID: "       << dropped.id
                          << " ID Count: " << dropped.id_count
                          << " Deadline: " << dropped.deadline
                          << " Frames: "   << dropped.frames
                          << " RR Req: "   << dropped.reliability_req
                          << std::endl;

                frame_entry["dropped_packets"].push_back({
                    {"id",               dropped.id},
                    {"id_count",         dropped.id_count},
                    {"deadline",         dropped.deadline},
                    {"frames",           dropped.frames},
                    {"frame_count",      dropped.frame_count},
                    {"reliability_req", dropped.reliability_req}
                });
            }
        }
        buffer.dropped_packets.clear();

        /* Log FSMC status */
        if (!summary_only)
        {
            std::cout << "\nFSMC STATUS" << std::endl;
            frame_entry["fsmc"] = json::array();
        }
        for (std::size_t c = 0; c < channels->size(); ++c)
        {
            BasePhysicalChannel* ch = (*channels)[c].get();

            /* FSMC details come from the channel's own fitted model; a channel
               without one (e.g. ReplayChannel) carries no Markov state. */
            const ChannelModel* ch_model = ch->estimated_model();
            const int fsmc_state = ch->get_fsmc_state();
            if (ch_model != nullptr && fsmc_state >= 0 && !summary_only)
            {
                const markov_state_t& curve =
                    ch_model->state_curves()[static_cast<std::size_t>(fsmc_state)];
                std::cout << "Channel: " << c << " (" << ch->name << ")" << std::endl;
                std::cout << "State: " << fsmc_state << std::endl;
                std::cout << "Parameters"
                          << "\nSlope: " << curve.slope
                          << "\nSNR@50% [dB]: " << curve.snr_50_db
                          << "\nMax Saturation: " << curve.max_saturation;
                std::cout << std::endl << std::endl;

                frame_entry["fsmc"].push_back({
                    {"channel",         c},
                    {"name",            ch->name},
                    {"state",           fsmc_state},
                    {"slope",           curve.slope},
                    {"snr_50_db",       curve.snr_50_db},
                    {"max_saturation",  curve.max_saturation}
                });
            }

            ch->advance_fsmc_state();
        }

        if (!summary_only) simulation_log.push_back(frame_entry);
        (*system_tick)++;
    }

    if (orig_cout) std::cout.rdbuf(orig_cout);

    if (summary_only)
    {
        // Aggregate per-id instance counts. Mirrors the Python extract_metrics
        // logic so sched_ratio matches the full-log path exactly.
        std::map<int, int> per_id_generated;
        std::map<int, int> per_id_undelivered;
        std::map<int, double> per_id_req;
        /* Per-instance delivered/not, in release order. instance_frames_needed
           is keyed (id, id_count) in a std::map, so iterating it yields each
           id's instances already ordered by release. Used for the windowed
           (m,k) check and the burst-length metric below. */
        std::map<int, std::vector<char>> per_id_delivered;
        for (const auto& gen : config["packet_generators"])
        {
            for (const auto& p : gen["packets"])
            {
                int pid = p["id"];
                per_id_req[pid]         = p["reliability"];
                per_id_generated[pid]   = 0;
                per_id_undelivered[pid] = 0;
                per_id_delivered[pid]   = std::vector<char>();
            }
        }
        for (const auto& kv : instance_frames_needed)
        {
            const int pid       = kv.first.first;
            const int pid_count = kv.first.second;
            const int needed    = kv.second;
            per_id_generated[pid]++;
            int delivered = 0;
            for (int slot = 0; slot < needed; slot++)
            {
                if (received_slots.count(std::make_tuple(pid, pid_count, slot)))
                    delivered++;
            }
            const bool instance_ok = (delivered >= needed);
            if (!instance_ok) per_id_undelivered[pid]++;
            per_id_delivered[pid].push_back(instance_ok ? 1 : 0);
        }

        /* Weakly-hard / (m,k)-firm window. An id satisfies its requirement if
           every window of `window_k` consecutive instances delivers at least
           m = ceil(reliability_req * window_k) of them. Sliding (not
           tumbling) so a burst straddling a boundary can't be masked. Note
           window_k must be >= 1/(1 - reliability_req) or m == window_k and
           the constraint degenerates to zero-miss; window_m is emitted so the
           harness can flag that. The default k = 100 pairs with the harness's
           2-decimal reliability draws so that m/k reproduces the requirement
           exactly. */
        const unsigned int window_k = config["simulation"].value("window_k", 100U);

        json summary;
        summary["total_tx_power"] = total_tx_power;

        /* Radio occupancy per channel, in slots, in channel order. Each entry
           carries the channel's name so a reader does not have to re-derive it
           from the config, and TX and RX separately so "used for data" and
           "used for listening" can be told apart. */
        json channel_slots = json::array();
        for (std::size_t c = 0; c < channels->size(); c++)
            channel_slots.push_back({
                {"name", (*channels)[c]->name},
                {"tx",   channel_tx_slots[c]},
                {"rx",   channel_rx_slots[c]}
            });
        summary["channel_slots"] = channel_slots;
        summary["window_k"]       = window_k;
        json per_id = json::array();
        for (const auto& kv : per_id_req)
        {
            const int pid                   = kv.first;
            const std::vector<char>& seq    = per_id_delivered[pid];
            /* Smallest m with m/k >= req. The 1e-9 nudge is load-bearing:
               reliability_req arrives as a 2-decimal double, and a bare
               ceil() overshoots by one wherever that double rounds up --
               0.55*100 and 0.56*100 both do, silently making the constraint
               stricter than the task asked for. The epsilon is far below the
               smallest legitimate gap (1/100 at k=1), so it only ever cancels
               representation noise. Keep in sync with warn_window_k(). */
            const unsigned int window_m     = (window_k == 0U) ? 0U :
                static_cast<unsigned int>(
                    std::ceil(kv.second * static_cast<double>(window_k) - 1e-9));

            /* Longest run of consecutive undelivered instances. Parameter-free
               companion to the (m,k) check. */
            int max_burst = 0;
            int run       = 0;
            for (const char ok : seq)
            {
                run = ok ? 0 : run + 1;
                if (run > max_burst) max_burst = run;
            }

            /* Sliding window over a running count of delivered instances. */
            int windows_total      = 0;
            int window_violations  = 0;
            if (window_k > 0U && seq.size() >= window_k)
            {
                int in_window = 0;
                for (unsigned int i = 0U; i < window_k; i++)
                    in_window += seq[i];
                windows_total = 1;
                if (in_window < static_cast<int>(window_m)) window_violations++;
                for (unsigned int i = window_k; i < seq.size(); i++)
                {
                    in_window += seq[i] - seq[i - window_k];
                    windows_total++;
                    if (in_window < static_cast<int>(window_m)) window_violations++;
                }
            }

            per_id.push_back({
                {"id",                     pid},
                {"reliability_req",       kv.second},
                {"generated",              per_id_generated[pid]},
                {"undelivered",            per_id_undelivered[pid]},
                {"max_consecutive_misses", max_burst},
                {"window_m",               window_m},
                {"windows_total",          windows_total},
                {"window_violations",      window_violations}
            });
        }
        summary["per_id"] = per_id;

        std::ofstream sim_log_file(log_file);
        sim_log_file << summary.dump();
    }
    else
    {
        /* Save simulation log */
        std::ofstream sim_log_file(log_file);
        sim_log_file << simulation_log.dump(4);

        BasePacketGenerator::save_to_file(spawn_log, "generated_packets.json");
        target_receiver.save_to_file("received_packets.json");
        scheduler->save_to_file();
    }
}
