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
#include <cstdint>
#include <cmath>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

/* System Model */
#include "system_model/system_model.hpp"
#include "system_model/buffer_packet/buffer_packet.hpp"
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

    /* Initialize channels from config */
    auto channels = std::make_shared<std::vector<std::unique_ptr<BasePhysicalChannel>>>();
    {
        size_t ch_idx = 0;
        for (const auto& ch : config["channels"])
        {
            if (ch["type"] == "sigmoid")
            {
                auto sc = std::make_unique<SigmoidChannel>(ch["frequency"], ch["name"]);
                if (has_seed) sc->seed_rng(seed + 0x100 + ch_idx);
                channels->emplace_back(std::move(sc));
            }
            else if (ch["type"] == "replay")
            {
                auto rc = std::make_unique<ReplayChannel>(
                    ch["frequency"], ch["csv_path"].get<std::string>(), system_tick);
                channels->emplace_back(std::move(rc));
            }
            ++ch_idx;
        }
    }

    /* Initialize scheduler from config */
    const auto& sched_cfg = config["scheduler"];
    const std::string sched_type = sched_cfg["type"];

    std::unique_ptr<BaseScheduler> scheduler;
    if (sched_type == "CHARM")
    {
        scheduler = std::make_unique<CHARM_scheduler>(
            sched_cfg["tx_power"],
            sched_cfg["frequency"],
            sched_cfg["rx_period"],
            &buffer,
            system_tick
        );
    }
    else if (sched_type == "CHEDF")
    {
        /* Same parameters as CHARM — it is CHARM's policy over an EDF queue. */
        scheduler = std::make_unique<CHEDF_scheduler>(
            sched_cfg["tx_power"],
            sched_cfg["frequency"],
            sched_cfg["rx_period"],
            &buffer,
            system_tick
        );
    }
    else if (sched_type == "SRM" || sched_type == "SEDF")
    {
        /* Static-channel schedulers. They size their redundancy from the
           channel's long-run mean decode probability at their transmit power,

               rho_bar_j = sum_s pi_s rho_s(SNR(P_j))

           which is computed once, here, rather than delivered as a
           prediction: SRM/SEDF never enter RX_MODE, so nothing would ever
           hand them the value at run time. See
           BasePhysicalChannel::mean_probability(). */
        const unsigned int sched_tx_power  = sched_cfg["tx_power"];
        const unsigned int sched_frequency = sched_cfg["frequency"];

        auto ch_it = std::find_if(channels->begin(), channels->end(),
            [sched_frequency](const std::unique_ptr<BasePhysicalChannel>& ch) {
                return ch->frequency == sched_frequency;
            });

        if (ch_it == channels->end())
        {
            std::cerr << "ERROR: " << sched_type << " found no channel at frequency "
                      << sched_frequency << std::endl;
            return -1;
        }

        const double mean_prob = (*ch_it)->mean_probability(sched_tx_power);

        /* A non-positive static estimate can never accumulate to a
           reliability target, so every packet would be retransmitted until
           its deadline. That is a mis-specified channel rather than a policy
           choice, so say so instead of silently saturating the link. */
        if (mean_prob <= 0.0)
            std::cerr << "WARNING: static mean decode probability at " << sched_tx_power
                      << "W is " << mean_prob
                      << " - no packet can reach its reliability target." << std::endl;

        if (!summary_only)
            std::cout << "Static mean decode probability at " << sched_tx_power << "W: "
                      << mean_prob << std::endl;

        if (sched_type == "SRM")
        {
            scheduler = std::make_unique<SRM_scheduler>(
                sched_tx_power,
                sched_frequency,
                mean_prob,
                &buffer,
                system_tick
            );
        }
        else
        {
            /* SRM's policy over an EDF queue -- same parameters. */
            scheduler = std::make_unique<SEDF_scheduler>(
                sched_tx_power,
                sched_frequency,
                mean_prob,
                &buffer,
                system_tick
            );
        }
    }
    else if (sched_type == "EDF")
    {
        scheduler = std::make_unique<EDF_scheduler>(
            sched_cfg["tx_power"],
            sched_cfg["frequency"],
            &buffer,
            system_tick
        );
    }
    else if (sched_type == "Rate_M")
    {
        scheduler = std::make_unique<RM_scheduler>(
            sched_cfg["tx_power"],
            sched_cfg["frequency"],
            &buffer,
            system_tick
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

        scheduler = std::make_unique<CATS_scheduler>(
            sched_cfg["frequency"],
            sched_cfg["belief_threshold"],
            sched_cfg["utilization_threshold"],
            periodic_tasks,
            &buffer,
            system_tick
        );
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
    MLPredictor ml_predictor(system_tick, channels, predict_error);
    if (has_seed) ml_predictor.seed_rng(seed + 0x300);
    std::vector<double> pred_probs;

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

                auto it = std::find_if(channels->begin(), channels->end(),
                                        [&transmitted_frame](const std::unique_ptr<BasePhysicalChannel>& ch) {
                                            return ch->frequency == transmitted_frame.frequency;
                                        });

                if (it != channels->end())
                {
                    recv_frame = (*it)->gen_frame_with_probability(transmitted_frame);
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
                            {"frequency",        recv_frame.frequency},
                            {"probability",      recv_frame.success_prob},
                            {"reliability_req", recv_frame.packet.reliability_req},
                            {"received",         received}
                        };
                    }
                }
                else
                {
                    std::cerr << "ERROR: No matching channel found" << std::endl;
                }

                break;
            }
            case RX_MODE:
                if (!summary_only)
                {
                    std::cout << "RX MODE" << std::endl;
                    frame_entry["radio_mode"] = "RX_MODE";
                }

                pred_probs = ml_predictor.predict_channel_conditions(scheduled_frame.frequency, scheduler->get_prediction_powers());

                if (!summary_only)
                {
                    std::cout << "Predicted probabilities at frequency " << scheduled_frame.frequency << "Hz:";
                    for (size_t k = 0; k < scheduler->get_prediction_powers().size(); k++)
                        std::cout << " " << scheduler->get_prediction_powers()[k] << "W=" << pred_probs[k];
                    std::cout << std::endl;
                }

                scheduler->receive_prediction(pred_probs);

                if (!summary_only)
                {
                    frame_entry["prediction"] = {
                        {"powers",      scheduler->get_prediction_powers()},
                        {"probs",       pred_probs},
                        {"frequency",   scheduled_frame.frequency}
                    };
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
        for (auto& ch : *channels)
        {
            /* FSMC details are sigmoid-channel specific; other channel types
               (e.g. ReplayChannel) carry no Markov state. */
            SigmoidChannel* sc = dynamic_cast<SigmoidChannel*>(ch.get());
            if (sc && !summary_only)
            {
                int fsmc_state = sc->get_fsmc_state();
                std::cout << "Frequency: " << sc->frequency << std::endl;
                std::cout << "State: " << fsmc_state << std::endl;
                std::cout << "Parameters"
                          << "\nSlope: " << sc->fsmc[fsmc_state].slope
                          << "\nSNR@50% [dB]: " << sc->fsmc[fsmc_state].snr_50_db
                          << "\nMax Saturation: " << sc->fsmc[fsmc_state].max_saturation
                          << "\nNoise Floor [dBm]: " << sc->fsmc[fsmc_state].noise_floor_dbm;
                std::cout << std::endl << std::endl;

                frame_entry["fsmc"].push_back({
                    {"frequency",       sc->frequency},
                    {"state",           fsmc_state},
                    {"slope",           sc->fsmc[fsmc_state].slope},
                    {"snr_50_db",       sc->fsmc[fsmc_state].snr_50_db},
                    {"max_saturation",  sc->fsmc[fsmc_state].max_saturation},
                    {"noise_floor_dbm", sc->fsmc[fsmc_state].noise_floor_dbm}
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
