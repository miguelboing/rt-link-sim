#include <cstddef>
#include <random>
#include <string>

#include "sigmoid_channel.hpp"

SigmoidChannel::SigmoidChannel(const std::string& channel_name):
    BasePhysicalChannel(channel_name),
    model(ChannelModel::from_directory(channel_name)),
    mc(model.transition_matrix(), model.n_states())
{
    /* Defining initial state for fsmc. Drawn from the stationary distribution
       rather than uniformly: a run starts where the channel spends its time,
       so no warm-up is needed to shake off the initial condition. The two are
       the same thing only when pi is uniform, which holds for both rings but
       not for good_moderate_2 (60/40) or rare_excellent_3, where a uniform
       draw would open one run in three in a state the channel is in 5% of the
       time. */
    std::discrete_distribution<std::size_t> distribution(this->model.stationary().begin(),
                                                         this->model.stationary().end());
    this->mc.set_state(distribution(this->generator));
}

double SigmoidChannel::gen_probability(unsigned int transmission_power)
{
    /* Getting the current state */
    return this->model.rho(this->mc.current(), transmission_power);
}

double SigmoidChannel::mean_probability(unsigned int transmission_power)
{
    return this->model.rho_bar(transmission_power);
}

received_frame_t SigmoidChannel::gen_frame_with_probability(transmitted_frame_t transmitted_frame)
{
    received_frame_t recv_frame;
    recv_frame.packet = transmitted_frame.packet;
    recv_frame.transmission_power = transmitted_frame.transmission_power;
    recv_frame.channel = transmitted_frame.channel;

    recv_frame.success_prob = this->gen_probability(recv_frame.transmission_power);

    return recv_frame;
}

void SigmoidChannel::advance_fsmc_state(void)
{
    (void)this->mc.advance();
}

int SigmoidChannel::get_fsmc_state(void) const
{
    return static_cast<int>(this->mc.current());
}

void SigmoidChannel::seed_rng(uint64_t seed)
{
    // Distinct sub-seeds for the two streams (initial-state draw and the
    // FSMC transition RNG) to keep them statistically independent.
    this->generator.seed(seed);
    this->mc.seed(seed ^ 0x9E3779B97F4A7C15ULL);

    // Re-pick the initial state using the now-deterministic generator, from
    // the same stationary distribution the constructor drew from.
    std::discrete_distribution<std::size_t> distribution(this->model.stationary().begin(),
                                                         this->model.stationary().end());
    this->mc.set_state(distribution(this->generator));
}
