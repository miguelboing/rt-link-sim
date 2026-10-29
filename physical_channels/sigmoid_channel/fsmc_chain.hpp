#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

/* A finite-state Markov chain whose state count is known only at run time.

   This is what kovian::MarkovChain does, minus the compile-time N. The
   shipped channels have 6, 8, 2 and 3 states and one build has to load all of
   them, so the dimension cannot be a template parameter: it is read from the
   channel's own files instead. The file format is unchanged -- the same .kov
   rows, comma or whitespace separated -- so nothing that writes a channel has
   to know about this.

   The row-count and row-width checks kovian performed are kept here, and one
   is added: the matrix must be *square*, since with no N to check against, the
   row count is the only thing that can define it. */
class FsmcChain
{
public:
    /* Row-major N*N transition kernel. Rows need not be normalised -- the
       per-row distributions normalise them, as kovian's did. */
    using Matrix = std::vector<double>;

    static Matrix matrix_from_file(const std::string& path)
    {
        std::ifstream mfile(path);
        if (!mfile)
            throw std::runtime_error("FsmcChain: cannot open " + path);

        Matrix flat;
        std::size_t rows = 0;
        std::size_t width = 0;
        std::string line;
        while (std::getline(mfile, line))
        {
            /* Commas are separators, nothing else: swapping them for spaces
               lets the stream tokenise the row and keeps scientific notation
               working, which a hand-rolled split would have to handle itself.
               A far-rank transition can legitimately be written 2.5e-04. */
            for (char& c : line)
                if (c == ',') c = ' ';

            std::istringstream row_stream(line);
            std::size_t in_row = 0;
            double value = 0.0;
            while (row_stream >> value)
            {
                if (value < 0.0)
                    throw std::runtime_error("FsmcChain: negative probability in " + path +
                                             " row " + std::to_string(rows));
                flat.push_back(value);
                ++in_row;
            }

            if (in_row == 0) continue;   /* blank line */

            if (width == 0)
                width = in_row;
            else if (in_row != width)
                throw std::runtime_error("FsmcChain: " + path + " row " + std::to_string(rows) +
                                         " has " + std::to_string(in_row) + " entries, expected " +
                                         std::to_string(width));
            ++rows;
        }

        if (rows == 0)
            throw std::runtime_error("FsmcChain: " + path + " is empty");
        if (rows != width)
            throw std::runtime_error("FsmcChain: " + path + " is " + std::to_string(rows) +
                                     "x" + std::to_string(width) + ", expected a square matrix");
        return flat;
    }

    /* `n` is supplied by the caller rather than recovered from P.size(),
       because the caller is the one that knows how many states the channel
       claims to have -- SigmoidChannel passes the length of its state table.
       The check below is therefore the cross-check between the chain file and
       the state file, which no longer have a shared compile-time constant to
       be measured against. */
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
