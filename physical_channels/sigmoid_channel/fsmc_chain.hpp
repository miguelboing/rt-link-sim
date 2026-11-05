#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

/* A finite-state Markov chain whose state count is known only at run time.

   This is what kovian::MarkovChain does, minus the compile-time N. The
   shipped channels have 6, 8, 2 and 3 states and one build has to load all of
   them, so the dimension cannot be a template parameter: it is read from the
   model instead, so nothing that writes a channel has to know about this.

   It is handed a kernel rather than a path: the files are read once by
   ChannelModel, which both this chain and the schedulers' estimated models are
   built from, so the row-width, row-sum and square-matrix validation lives
   there and is not repeated per consumer. The size check below stays, since it
   is the last line of defence against stepping into a state the table does not
   describe. */
class FsmcChain
{
public:
    /* Row-major N*N transition kernel. Rows need not be normalised -- the
       per-row distributions normalise them, as kovian's did. */
    using Matrix = std::vector<double>;

    /* `n` is supplied by the caller rather than recovered from P.size(),
       because the caller is the one that knows how many states the channel
       claims to have -- SigmoidChannel passes its model's state count, which
       came from fsmc_states.json. */
    FsmcChain(const Matrix& P, std::size_t n_states):
        n(n_states)
    {
        if (n == 0)
            throw std::runtime_error("FsmcChain: a chain needs at least one state");
        if (P.size() != n * n)
            throw std::runtime_error("FsmcChain: transition matrix has " +
                                     std::to_string(P.size()) + " entries, expected " +
                                     std::to_string(n * n) + " for " +
                                     std::to_string(n) + " states");

        onestep.reserve(n);
        for (std::size_t k = 0; k < n; ++k)
        {
            double row_sum = 0.0;
            for (std::size_t j = 0; j < n; ++j)
                row_sum += P[k * n + j];
            /* An all-zero row leaves the chain with nowhere to go:
               std::discrete_distribution would silently fall back to picking
               index 0, which is a transition the file never asked for. */
            if (row_sum <= 0.0)
                throw std::runtime_error("FsmcChain: row " + std::to_string(k) +
                                         " of the transition matrix sums to zero");
            onestep.emplace_back(P.begin() + static_cast<std::ptrdiff_t>(k * n),
                                 P.begin() + static_cast<std::ptrdiff_t>((k + 1) * n));
        }

        rng.seed(static_cast<std::default_random_engine::result_type>(
            std::chrono::system_clock::now().time_since_epoch().count()));
    }

    std::size_t size(void) const { return n; }

    std::size_t advance(void) { return state = onestep[state](rng); }

    std::size_t current(void) const { return state; }

    void set_state(std::size_t x)
    {
        if (x >= n)
            throw std::runtime_error("FsmcChain: tried to assign state " + std::to_string(x) +
                                     " to a chain with only " + std::to_string(n) + " states");
        state = x;
    }

    /* Reseed the transition RNG for deterministic replication. */
    void seed(uint64_t s)
    {
        rng.seed(static_cast<std::default_random_engine::result_type>(s));
    }

private:
    std::size_t n;
    std::size_t state = 0;
    std::default_random_engine rng;
    std::vector<std::discrete_distribution<std::size_t>> onestep;
};
