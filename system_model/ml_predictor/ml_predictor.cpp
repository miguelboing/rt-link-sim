#include <algorithm>
#include <random>
#include <vector>

#include "system_model/system_model.hpp"
#include "ml_predictor.hpp"

MLPredictor::MLPredictor(std::shared_ptr<unsigned int> sys_tick,
                         std::shared_ptr<std::vector<std::unique_ptr<BasePhysicalChannel>>> channels,
                         double predict_error):
    system_tick(sys_tick), channels(channels), predict_error(predict_error) {};

int MLPredictor::predict_channel_state(std::size_t c)
{
    if (c >= this->channels->size())
        return -1;  /* no such channel */

    /* No confusion model yet, so the inferred label is the observed one. When
       E_c arrives it applies here, between reading the state and returning
       it -- the one place that knows the model's state space and can draw a
       wrong label from the right row. */
    return (*this->channels)[c]->get_fsmc_state();
}

std::vector<double> MLPredictor::predict_channel_conditions(std::size_t c, const std::vector<unsigned int>& powers)
{
    if (c < this->channels->size())
    {
        BasePhysicalChannel* ch = (*this->channels)[c].get();
        std::vector<double> probs;
        probs.reserve(powers.size());
        const bool noisy = this->predict_error > 0.0;
        std::uniform_real_distribution<double> noise(-this->predict_error, this->predict_error);
        for (unsigned int power : powers)
        {
            double p = ch->gen_probability(power);
            if (noisy)
            {
                p += noise(this->generator);
                p = std::max(0.0, std::min(1.0, p));
            }
            probs.push_back(p);
        }
        return probs;
    }

    /* -1 represents that the MLPredictor has no such channel */
    return std::vector<double>(powers.size(), -1.0);
}

void MLPredictor::seed_rng(uint64_t seed)
{
    generator.seed(seed);
}

