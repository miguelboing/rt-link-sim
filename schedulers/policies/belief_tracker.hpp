#pragma once

#include <cstddef>
#include <vector>

#include "schedulers/base_scheduler.hpp"

/* ── Belief in a held channel estimate: a listening *policy* ────────────────

   How much a scheduler still trusts the label it last heard on a channel, and
   therefore when it gives up a slot to listen again. This is a policy, not
   part of the channel estimate: the estimate is the fitted model and the
   latest label (BaseScheduler's channel set, the paper's Sec. 3.2.1), and a
   scheduler is free to decide when to refresh it on any rule it likes --
   a fixed interval, an urgency test, the entropy of the propagated
   distribution. Belief decaying at the channel's own mixing rate is one
   such rule, and it belongs to the schedulers that chose it.

   Held by *composition*, not inherited, which is the point: CHARM, CHEDF and
   CATS each own one because they share this rule, and a scheduler that
   refreshes on a different rule simply does not own one. Nothing about the
   channel set presumes this policy exists.

   It lives in its own header rather than being copied into the three because
   the rule is identical in all of them -- CHARM and CHEDF are already
   verbatim copies of each other by design, and a third copy of the decay is
   the drift hazard the old global constant was. One definition, three owners.

   The rule: belief in channel c is 1 the slot after c was observed and is
   multiplied by A_ss(s_hat_c) on every slot since -- the model probability the
   channel has not left the state it was seen in, with 1 - A_ss the per-slot
   probability of leaving. It is per channel *and* per state, so the refresh
   interval it implies, ceil(ln(threshold) / ln(A_ss)), differs between two
   states of the same chain. Belief starts at 0, so a channel never observed
   always reads as worth listening to and nothing is ever transmitted on an
   unset estimate. */
class BeliefTracker
{
public:
    /* One slot of decay, for *every* channel. Every channel ages whether the
       radio transmits on it, idles, or listens somewhere else: the physical
       channel evolves regardless, so a belief decayed only on the slots
       spent on one channel would be wrong for the other C-1.

       One multiplication per channel per slot. The alternative -- storing
       tau_c and raising A_ss to (t - tau_c) on each query -- gives bit-identical
       results but pays per question asked rather than per slot, and needs a
       second stored field to do it.

       Call this once per slot, at the top of do_schedule_frame(). A scheduler
       that forgets never decays and so never listens, which shows up at once
       as a run with no RX slots. */
    void age(const std::vector<channel_estimate_t>& channels)
    {
        /* Sized from the set rather than at construction, because the set is
           handed over after the scheduler is built. */
        if (this->belief.size() != channels.size())
            this->belief.assign(channels.size(), 0.0f);

        for (std::size_t c = 0; c < channels.size(); ++c)
        {
            const channel_estimate_t& ch = channels[c];

            /* No observation yet: no A_ss to decay by, and 0 is a fixed point
               of the decay anyway. */
            if (ch.believed_state < 0) continue;

            this->belief[c] *= static_cast<float>(
                ch.model.p_self(static_cast<std::size_t>(ch.believed_state)));
        }
    }

    /* An observation landed on channel c: full belief in the new label. Only
       called for a label the scheduler actually recorded -- a listen that came
       back with nothing buys no belief, so the scheduler listens again. */
    void observed(std::size_t c)
    {
        if (c < this->belief.size()) this->belief[c] = 1.0f;
    }

    float of(std::size_t c) const
    {
        return c < this->belief.size() ? this->belief[c] : 0.0f;
    }

    /* The listening test: is channel c's estimate no longer worth holding? */
    bool stale(std::size_t c, float threshold) const
    {
        return this->of(c) < threshold;
    }

private:
    std::vector<float> belief;   /* one per channel, in the set's order */
};
