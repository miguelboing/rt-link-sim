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

void MLPredictor::seed_rng(uint64_t seed)
{
    generator.seed(seed);
}

