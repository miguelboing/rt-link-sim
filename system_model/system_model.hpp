#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory>
#include <string>

typedef struct
{
    unsigned int id;                  /* ID: Tasks unique identifier */
    unsigned int id_count;            /* PC: Identifier between packets with the same ID */
    unsigned int deadline;            /* D : This is the relative deadline */
    unsigned int frames;              /* C : A unit consumes one frame */
    unsigned int frame_count;         /* FC: The amount of frames transmitted from this packet_count */
    double       reliability_req;    /* RR : Reliability requirement for the packet */
    bool         is_periodic = false; /* Flag to warn the scheduler if this packet is periodic or not */
    unsigned int period;              /* P: If is_periodic is true, this value represents the period of the packet */
}
packet_t;

/* Globally-unique key for a packet release. id_count is per-id, so schedulers
   that key per-release state (CATS, CHARM accumulated_prob) must combine both
   to avoid cross-task collisions when two tasks happen to share a release
   number. id occupies the high 32 bits, id_count the low 32. */
inline uint64_t packet_key(unsigned int id, unsigned int id_count)
{
    return (static_cast<uint64_t>(id) << 32) | static_cast<uint64_t>(id_count);
}

typedef enum
{
    IDLE=0,
    TX_MODE,
    RX_MODE
} radio_mode_e;

typedef struct
{
    packet_t* packet;
    unsigned int transmission_power;

    /* c_sel(t): which channel of C = {0, ..., C-1} this slot uses. The index
       *is* the channel's identity -- it is the position in the simulator's
       channel vector, and the scheduler's channel set is in the same order.
       A channel's name (its directory under physical_channels/) is carried by
       the channel itself, for logs and errors. */
    std::size_t channel = 0;

    radio_mode_e radio_mode;
    bool remove_from_buffer = true;
} scheduled_frame_t;

typedef struct
{
    packet_t packet;
    unsigned int transmission_power;
    std::size_t channel = 0;   /* c: the channel this frame went out on */
} transmitted_frame_t;

typedef struct
{
    packet_t packet;
    unsigned int transmission_power;
    std::size_t channel = 0;   /* c: the channel this frame arrived on */
    double success_prob;
} received_frame_t;

typedef struct
{
    double snr_50_db;        /* beta: SNR for 50% success rate */
    double slope;            /* alpha: Steepness of sigmoid curve */
    double max_saturation;   /* gamma: The maximum possible transmission power when tx_power -> inf */
} markov_state_t;

