#include <algorithm>
#include <iostream>
#include <fstream>
#include <random>
#include <vector>
#include <memory>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "system_model/system_model.hpp"
#include "target_receiver.hpp"

TargetReceiver::TargetReceiver(std::shared_ptr<unsigned int> sys_tick):
    system_tick(sys_tick), frame_log(json::array()) {}

bool TargetReceiver::recv_frame(received_frame_t recv_frame)
{
    std::bernoulli_distribution distribution(0.0);

    distribution = std::bernoulli_distribution(recv_frame.success_prob);
    bool prob_result = distribution(this->generator);

    if (this->log_enabled)
    {
        json frame_entry = {
            {"system_tick", *(this->system_tick)},
            {"packet_id", recv_frame.packet.id},
            {"packet_id_count", recv_frame.packet.id_count},
            {"deadline", recv_frame.packet.deadline},
            {"frames", recv_frame.packet.frames},
            {"frame_count", recv_frame.packet.frame_count},
            {"reliability_req", recv_frame.packet.reliability_req},
            {"transmission_power", recv_frame.transmission_power},
            {"channel", recv_frame.channel},
            {"success_prob", recv_frame.success_prob},
            {"received", prob_result}
        };

        frame_log.push_back(frame_entry);
    }

    return prob_result;
}

void TargetReceiver::save_to_file(const std::string& filename)
{
    std::ofstream file(filename);
    file << frame_log.dump(4);
}

void TargetReceiver::seed_rng(uint64_t seed)
{
    generator.seed(seed);
}

