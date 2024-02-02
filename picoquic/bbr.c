/*
* Author: Christian Huitema
* Copyright (c) 2019, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "picoquic_internal.h"
#include <stdlib.h>
#include <string.h>
#include "cc_common.h"
#include "picoquic_utils.h"

/*
Implementation of the BBR algorithm, tuned for Picoquic.

The main idea of BBR is to track the "bottleneck bandwidth", and to tune the
transport stack to send exactly at that speed. This ensures good network
utilisation while avoiding the building of queues. To do that the stack
needs to constantly estimate the available data rate. It does that by
measuring the rate at which acknowledgements come back, providing what it
calls the delivery rate.

That approach includes an implicit feedback loop. The delivery rate can never
exceed the sending rate. That will effectively detects a transmission slow
down due to partial congestion, but if the algorithm just did that the
sending rate will remain constant when the network is lightly loaded
and ratchet down during time of congestion, leading to very low efficiency.
The available bandwidth can only be tested by occasionally sending faster
than the measured delivery rate.

BBR does that by following a cycle of "send, test and drain". During the
sending period, the stack sends at the measured rate. During the testing
period, it sends faster, 25% faster with recommended parameters. This
risk creating a queue if the bandwidth had not increased, so the test
period is followed by a drain period during which the stack sends 25%
slower than the measured rate. If the test is successful, the new bandwidth
measurement will be available at the end of the draining period, and
the increased bandwidth will be used in the next cycle.

Tuning the sending rate does not guarantee a short queue, it only
guarantees a stable queue. BBR controls the queue by limiting the
amount of data "in flight" (congestion window, CWIN) to the product
of the bandwidth estimate by the RTT estimate, plus a safety marging to ensure
continuous transmission. Using the average RTT there would lead to a runaway
loop in which oversized windows lead to increased queues and then increased
average RTT. Instead of average RTT, BBR uses a minimum RTT. Since the
mimimum RTT might vary with routing changes, the minimum RTT is measured
on a sliding window of 10 seconds.

The bandwidth estimation needs to be robust against short term variations
common in wireless networks. BBR retains the maximum
delivery rate observed over a series of probing intervals. Each interval
starts with a specific packet transmission and ends when that packet
or a later transmission is acknowledged. BBR does that by tracking
the delivered counter associated with packets and comparing it to
the delivered counter at start of period.

During start-up, BBR performs its own equivalent of Reno's slow-start.
It does that by using a pacing gain of 2.89, i.e. sending 2.89 times
faster than the measured maximum. It exits slow start when it found
a bandwidth sufficient to fill the pipe.

The bandwidth measurements can be wrong if the application is not sending
enough data to fill the pipe. BBR tracks that, and does not reduce bandwidth
or exit slow start if the application is limiting transmission.

This implementation follows draft-cardwell-iccrg-bbr-congestion-control,
with a couple of changes for handling the multipath nature of quic.
There is a BBR control state per path.
Most of BBR the variables defined in the draft are implemented
in the "BBR state" structure, with a few exceptions:

* BBR.delivered is represented by path_x.delivered, and is maintained
  as part of ACK processing

* We use "bytes_in_transit", which is already maintained by the stack.

* Compute bytes_delivered by summing all calls to ACK(bytes) before
  the call to RTT update.

* In the Probe BW mode, the draft suggests cwnd_gain = 2. We observed
  that this results in queue sizes of 2, which is too high, so we
  reset that to 1.125.

The "packet" variables are defined in the picoquic_packet_t.

Early testing showed that BBR startup phase requires several more RTT
than the Hystart process used in modern versions of Reno or Cubic. BBR
only ramps up the data rate after the first bandwidth measurement is
available, 2*RTT after start, while Reno or Cubic start ramping up
after just 1 RTT. BBR only exits startup if three consecutive RTT
pass without significant BW measurement increase, which not only
adds delay but also creates big queues as data is sent at 2.89 times
the bottleneck rate. This is a tradeoff: longer search for bandwidth in
slow start is less likely to stop too early because of transient
issues, but one high bandwidth and long delay links this translates
to long delays and a big batch of packet losses.

This BBR implementation addresses these issues by switching to
Hystart instead of startup if the RTT is above the Reno target of
100 ms. 

*/

/* Detection of leaky-bucket pacers.
 * This is based on code added to BBR after the IETF draft was published.
 * The code detect whether the connection is being "policed" by a leaky-bucket based
 * parser, and introduces state variables:
 * - lt_use_bw, boolean: check whether the connection is currently constrained
 *   to use a limited bandwidth.
 * - lt_rtt_cnt: number of RTT during which the bandwidth has been limited. Exit the
 *   limited bandwidth state when this reaches the maximum value.
 * - lt_is_sampling: whether the connection is sampling the number of loss
 *   intervals. This starts when the first losses are noticed. It is reset if
 *   the bandwidth is app limited.
 * Sampling lasts for a number of rounds, as counted by the round_start
 * variable. No action taken except updating counters in that state.
 * Sampling can only ends on an interval when losses are detected, to avoid
 * undercounting. If no loss, sampling continues after the min required
 * interval. If no losses for too long, sampling state is reset.
 * At the end of the sampling interval compute the ratio of packets lost
 * to packet delivered. If it is below threshold, continue sampling, i.e.,
 * extend the interval.
 * If loss rate exceed the threshold, compute the duration of the interval.
 * If it is too short, extend. If is is too long, reset the sampling.
 * Else, compute the delivery rate for the interval. Check whether this
 * is the first detection, in which case do nothing but remember the estimated
 * rate in "lt_bw". Else, check whether the new rate is "close enough" to
 * the previous rate, which indicates active policing. If that is true,
 * activate policing to the average rate between current and previous sample.
 */

/* Reaction to losses and ECN
 * This code is an implementation of BBRv1, which pretty much ignores packet
 * losses or ECN marks. Google has still developed BBRv2, which is generally
 * considered much more robust. Once the BBRv2 specification is available,
 * we should develop it. However, before BBRv2 is there, we need to fix the
 * most egregious issues in BBR v1. For example, in a test, we show that if
 * a receiver starts a high speed download and then disappears, the sender
 * will only close the connection after repeating over 1000 packets,
 * compared to only 32 with New Reno or Cubic. This is because BBR does
 * not slow down or reduce the CWIN on loss indication, even when there
 * are many loss indications. 
 *
 * We implement the following fixes:
 *
 * - On basic loss indication, run a filter to determine whether the loss
 *   rate is getting too high. This will allow the code to continue
 *   ignoring low loss rates, but somehow react to high loss rates.
 *
 * - If high loss rate is detected, halve the congestion window. Do
 *   the same if an EC mark is received.
 *
 * - If a timeout loss is detected, reduce the window to the minimum
 *   value.
 *
 * This needs to be coordinated with the BBR state machine. We implement
 * it as such:
 *
 * - if the state is start-up or start-up-long-rtt, exit startup
 *   and move to a drain state.
 * - if the state is probe-bw, start the new period with a conservative
 *   packet window (trigger by cycle_on_loss state variable)
 * - if the state is probe-RTT, do nothing special...
 *
 * The packet losses and congestion signals should be used only once per
 * RTT. We filter with a "loss period start time" value, and only
 * take signals into account if they happen 1-RTT after the current
 * loss start time. However, if the previous loss was not due to
 * timeout, the timeout will still be handled.
 */

/*
* Handling of suspension
* 
* After a timeout, the path is suspended, and the congestion window is
* immediately reduced. If do not do anything in particular, the
* suspended state will be cleared on the first next acknowledgement,
* and the congestion window will be restored gradually.
* 
* This is correct in general, when the timeout is due to some series
* of packet loss events. It is not so good in the particular case of
* Wi-Fi suspension, when the timeout is caused by the Wi-Fi link
* being "suspended" for the time needed to scan other channels. In that
* case, the code will receive a "spurious time out" notification,
* typically triggered when an ACK queued "in the network" is delivered
* when transmission resume. Waiting for the next ACK has two
* downsides:
* 
* - it comes some times later, something like 1/2 RTT to 1 full RTT.
* - the CWIND is lower than if the suspension had not happened.
*
* The reasonable solution is to exit the suspended state upon
* notification of spurious reset, and restore the prior cwin.
*/

typedef enum {
    picoquic_bbr_alg_startup = 0,
    picoquic_bbr_alg_drain,
    /* picoquic_bbr_alg_probe_bw, */
    picoquic_bbr_alg_probe_bw_down,
    picoquic_bbr_alg_probe_bw_cruise,
    picoquic_bbr_alg_probe_bw_refill,
    picoquic_bbr_alg_probe_bw_up,
    picoquic_bbr_alg_probe_rtt,
    picoquic_bbr_alg_startup_long_rtt
} picoquic_bbr_alg_state_t;

typedef enum {
    picoquic_bbr_acks_probe_starting = 0,
    picoquic_bbr_acks_probe_stopping,
    picoquic_bbr_acks_refilling,
    picoquic_bbr_acks_probe_feedback,
} picoquic_bbr_ack_phase_t;

#if 0
/* Constants in pre-v3 BBR*/
#define BBR_BTL_BW_FILTER_LENGTH 10
#define BBR_min_rtt_FILTER_LENGTH 10
#define BBR_HIGH_GAIN 2.8853900817779 /* 2/ln(2) */
#define BBR_MIN_PIPE_CWND(mss) ((mss)*4)
#define BBR_GAIN_CYCLE_LEN 8
#define BBR_PROBE_RTT_INTERVAL 10000000 /* 10 sec, 10000000 microsecs */
#define BBR_PROBE_RTT_DURATION 200000 /* 200msec, 200000 microsecs */
#define BBR_PACING_RATE_LOW 150000.0 /* 150000 B/s = 1.2 Mbps */
#define BBR_PACING_RATE_MEDIUM 3000000.0 /* 3000000 B/s = 24 Mbps */
#define BBR_GAIN_CYCLE_LEN 8
#define BBR_GAIN_CYCLE_MAX_START 5
#define BBR_LT_BW_INTERVAL_MIN_RTT 4
#define BBR_LT_BW_RATIO_SCALE 1024
#define BBR_LT_BW_RATIO_SCALED_TARGET 205 /* 205/1024 is very close 20% */

#define BBR_LT_BW_INTERVAL_MAX_RTT (4*BBR_LT_BW_INTERVAL_MIN_RTT)
#define BBR_LT_BW_RATIO_INVERSE 8
#define BBR_LT_BW_BYTES_PER_SEC_DIFF 4000
#define BBR_LT_BW_MAX_RTTS 48
#if 0
/* Use this setting when debugging BBR slow start */
#define BBR_HYSTART_THRESHOLD_RTT 1000000
#else
#define BBR_HYSTART_THRESHOLD_RTT 50000
#endif
#endif
/* Constants in BBRv3 */
#define BBRPacingMarginPercent 1 /* discount factor of 1% used to scale BBR.bw to produce BBR.pacing_rate */
#define BBRStartupPacingGain 2.77 /* constant, 4*ln(2), approx 2.77 */
#define BBRStartupCwndGain 2.0 /* constant */
#define BBRLossThresh 0.02 /* maximum tolerated packet loss (default: 2%) */
#define BBRLossAlpha 0.125
#define BBRBeta 0.7 /* Multiplicative decrease on packet loss (default: 0.7) */
#define BBRHeadroom 0.15 /* Realive amount of headroom left for other flows. (default: 0.15). (Erroneously set to 0.85 in draft-bbr-02) */
#define BBRMinPipeCwnd 4 /* Default to 4*SMSS, i.e, 4*PMTU */

#define BBRMaxBwFilterLen 2 /* record bw_max for previous cycle and for this one */
#define BBRExtraAckedFilterLen 10 /* to compute the extra acked parameter */

#define BBRMinRTTFilterLen 10000000 /* Length of min rtt filter -- 10 seconds. */
#define BBRProbeRTTCwndGain 0.5
#define BBRProbeRTTDuration 200000 /* 200msec, 200000 microsecs */
#define BBRProbeRTTInterval 5000000 /* 5 seconds */

#define BBRProbeBwDownPacingGain 0.9
#define BBRProbeBwDownCwndGain 2.0
#define BBRProbeBwCruisePacingGain 1.0
#define BBRProbeBwCruiseCwndGain 2.0
#define BBRProbeBwRefillPacingGain 1.25
#define BBRProbeBwRefillCwndGain 2.0
#define BBRProbeBwUpPacingGain 1.25
#define BBRProbeBwUpCwndGain 2.0
#if 1
#define BBRMinRttMarginPercent 2 /* Margin factor of 2% for avoiding firing RTT Probe too often */
#endif


#if 0
static const double bbr_pacing_gain_cycle[BBR_GAIN_CYCLE_LEN] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.25, 0.75};
#endif
typedef struct st_picoquic_bbr_state_t {
    /* Algorithm state: */
    picoquic_bbr_alg_state_t state;
    int round_count;
    int rounds_since_probe;
    unsigned int round_start : 1;
    uint64_t next_round_delivered; /* packet delivered value at end of round trip */
    /* Output */
    //uint64_t cwnd; /* new in BBRv3 */
    double pacing_rate;
    uint64_t send_quantum;
    uint64_t prior_cwnd;
    /* Pacing state */
    double pacing_gain;
    uint64_t next_departure_time; /* earliest departure time of next packet, per pacing conditions -- new in BBRv3 */
    /* CWND state */
    double cwnd_gain;
    unsigned int packet_conservation : 1; /* whether BBR is using conservation dynamics */
    /*  Data Rate parameters: */
    uint64_t max_bw; /* windowed maximum recent bandwidth sample -- new in BBRv3 */
    uint64_t bw_hi; /* long term maximum -- new in BBRv3 */
    uint64_t bw_lo; /* short term maximum -- new in BBRv3 */
    uint64_t bw; /* max bw for current cycle, min(max_bw, bw_hi, bw_lo) -- new in BBRv3 */
    uint64_t btl_bw; /* Not used in BBRv3, superced by "bw"? */

    /* Data volume parameters:*/
    uint64_t min_rtt; /* minimum RTT measured over last 10sec */
    uint64_t bdp; /* estimate of path BDP, bw* min_rtt  -- new part of state in BBRv3 */
    uint64_t extra_acked; /* estimate of ack aggregation on path -- new in BBRv3 */
    uint64_t offload_budget; /* data necessary for using TSO / GSO(or LRO, GRO) -- new in BBRv3 */
    uint64_t max_inflight; /* data necessary to fully use link, f(bdp, extra_acked, offload_budget, BBRMinPipeCwnd)  -- new in BBRv3 */
    uint64_t inflight_hi; /* long term maximum inflight -- when packet losses are observed -- new in BBRv3 */
    uint64_t inflight_lo; /* short term maximum, generally lower than inflight_hi -- new in BBRv3 */

    /* State for responding to congestion: */
    uint64_t bw_latest; /* 1 roundtrip max of delivered bw  -- new in BBRv3 */
    uint64_t inflight_latest; /* 1 roundtrip max of delivered volume -- new in BBRv3 */

    /* Estimate max_bw */
    uint64_t MaxBwFilter[BBRMaxBwFilterLen]; /* filter tracking maximum of ack.delivery_rate, for estimate max_bw -- new in BBRv3 */
    unsigned int cycle_count; /* for estimating max_bw filter, rotating it. */

    /* estimate extra acked */
    uint64_t extra_acked_interval_start; /* start of interval for which extra acked is tracked,  -- new in BBRv3 */
    uint64_t extra_acked_delivered; /* data delivered since BBR.extra_acked_interval_start,  -- new in BBRv3 */
    uint64_t ExtraACKedFilter[BBRExtraAckedFilterLen]; /* max filter tracking aggregation, -- new in BBRv3 */

    /* startup parameters (only used in startup state) */
    unsigned int filled_pipe : 1;
    uint64_t full_bw; /* baseline max_bw if filled_pipe is true */
    int full_bw_count; /* nb non-app-limited round trips without large increase of full_bw */

    /* probertt parameters */
    uint64_t min_rtt_stamp; /* when last min_rtt was obtained. -- new name in BBRv3 */
    uint64_t probe_rtt_min_delay; /* rtt sample in last interval */
    uint64_t probe_rtt_min_stamp; /* time when probe_rtt_min_delay was obtained */
    uint64_t probe_rtt_done_stamp;
#if 1
    uint64_t min_rtt_margin; /* Margin of error for min RTT, to avoid spurious expiry of probe RTT timer. */
#endif
    unsigned int probe_rtt_expired; /* indicates whether min rtt is due for a refresh */
    unsigned int probe_rtt_round_done;
    unsigned int idle_restart : 1;
    unsigned int path_is_app_limited : 1;

    /* probe BW parameters */
    uint32_t rounds_since_bw_probe;
    uint64_t bw_probe_wait;
    uint64_t cycle_stamp;
    uint32_t bw_probe_up_cnt;
    uint32_t bw_probe_up_rounds;
    uint32_t bw_probe_samples;
    uint64_t bw_probe_up_acks;
    picoquic_bbr_ack_phase_t ack_phase;

    /* Management of packet losses */
    unsigned int loss_in_round : 1;
    unsigned int loss_round_start : 1;
    uint64_t loss_round_delivered;

    double loss_rate_smoothed;
    double delivered_smoothed;
    double lost_smoothed;

    /* Per connection random state.*/
    uint64_t random_context;

#if 1
    /* manage startup long_rtt */
    picoquic_min_max_rtt_t rtt_filter;
    uint64_t bdp_seed;
#endif

} picoquic_bbr_state_t;

/* BBR v3 assumes that there is state associated with the acknowledgements.
 * BBR assumes that upon reception of an ACK the code immediately
 * schedule transmission of packets that are deemed lost (code was
 * modifed to do that).
 * 
 * From draft-cheng-iccrg-delivery-rate-estimation:
 * data_acked = C.delivered - P.delivered
 *            = path->delivered - packet->delivered_prior;
 * ack_elapsed = C.delivered_time - P.delivered_time
 *            = current_time - packet->delivered_time_prior
 * ack_rate = data_acked / ack_elapsed
 * 
 * ack_elapsed is NOT equal to rtt_sample, because packet->delivered_time_prior
 * may be lower than packet->send_time.
 * 
 * The ack rate is imprecise, because of ACK compression, etc. The Cheng draft
 * suggests:
 * - Define "P.first_sent_time" as the time of the first send in a flight of data,
 * - and "P.sent_time" as the time the final send in that flight of data
 *   (the send that transmits packet "P").
 * The elapsed time for sending the flight is:
 * send_elapsed = (P.sent_time - P.first_sent_time)
 *               = packet->send_time - packet->delivered_sent_prior
 * The delay to receive packets should never be larger than the send rate,
 * so we can use a filter:
 *   delivery_elapsed = max(ack_elapsed, send_elapsed)
 *   delivery_rate = data_acked / delivery_elapsed
 *
 */
typedef struct st_bbr_per_ack_state_t {
    uint64_t delivered; /* volume delivered between acked packet and current time */
    uint64_t delivery_rate;  /* delivery rate sample when packet was just acked. */
    uint64_t rtt_sample;
    uint64_t newly_acked; /* volume of data acked by current ack */
    uint64_t newly_lost; /* volume of data marked lost on ack received */
    uint64_t tx_in_flight; /* estimate of in flight data at the time the packet was sent. */
    uint64_t lost; /* volume lost between transmission of packet and arrival of ACK */
    /* Part of "RS" struct */
    unsigned int is_app_limited : 1; /* App marked limited at time of ACK? */
    unsigned int is_cwnd_limited : 1;
} bbr_per_ack_state_t;

/* Forward definition of key functions */
static int IsInAProbeBWState(picoquic_bbr_state_t* bbr_state);
static void BBREnterDrain(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time);
#if 0
static void BBRHandleRestartFromIdle(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time);
#endif
void BBRltbwSampling(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time);
static void BBRResetProbeBwMode(picoquic_bbr_state_t* bbr_state, uint64_t current_time);
static void BBRStartProbeBW_DOWN(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time);
static void BBRStartProbeBW_CRUISE(picoquic_bbr_state_t* bbr_state);
static void BBRStartProbeBW_REFILL(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x);
static void BBREnterStartup(picoquic_bbr_state_t* bbr_state);
static void BBRUpdateRound(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static void BBRStartRound(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static void BBRSetRsFromAckState(picoquic_path_t* path_x, picoquic_per_ack_state_t* ack_state, bbr_per_ack_state_t* rs);
static int IsInflightTooHigh(picoquic_path_t * path_x, bbr_per_ack_state_t* rs);
static void BBRHandleInflightTooHigh(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t* rs, uint64_t current_time);
static uint64_t BBRTargetInflight(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x);
static void BBRInitRoundCounting(picoquic_bbr_state_t* bbr_state);
static void BBRCheckProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs, uint64_t current_time);
static void BBRUpdateMaxBw(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs);
static void BBRInitPacingRate(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static void BBRResetCongestionSignals(picoquic_bbr_state_t* bbr_state);
static void BBRResetLowerBounds(picoquic_bbr_state_t* bbr_state);
static uint64_t BBRInflightWithBw(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain, uint64_t bw);
static void BBRUpdateMaxInflight(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static uint64_t BBRInflightWithHeadroom(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static uint64_t BBRBDPMultiple(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain);

/* Init processes for BBRv3 */

/* Windowed max filter.
* Several parts of the BBR algorithm use "filters":
* MaxBwFilter[BBRMaxBwFilterLen]: max delivery rate during the last two cycles.
* In the simple case, the value is updated at the end of the cycle (?)
 */

uint64_t update_windowed_max_filter(uint64_t* filter, uint64_t v, unsigned int cycle, unsigned int filterLen)
{
#if 1
    if (filter[cycle % filterLen] < v) {
        filter[cycle % filterLen] = v;
    }
#else
    filter[cycle % filterLen] = v;
#endif
    for (unsigned int i = 0; i < filterLen; i++) {
        if (filter[i] > v) {
            v = filter[i];
        }
    }
    return v;
}

void start_windowed_max_filter_period(uint64_t* filter, unsigned int cycle, unsigned int filterLen)
{
    filter[cycle % filterLen] = 0;
}

uint64_t update_windowed_min_filter(uint64_t* filter, uint64_t v, unsigned int cycle, unsigned int filterLen)
{
    filter[cycle % filterLen] = v;
    for (unsigned int i = 0; i < filterLen; i++) {
        if (filter[i] < v) {
            v = filter[i];
        }
    }
    return v;
}


/* Init per connection random state.
* Should be initialized to a constant when running in test, to
* something unique when running in production. We do that by
* mixing:
* - the "current time", which is constant in tests but varies in production,
* - the connection type, 1 for client, 0 for server, so that even in tests
*   server and clients use different seeds,
* - the path unique number, so that different paths will use different seeds,
*   even in tests.
*/
static void BBRInitRandom(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    uint64_t random_context = 0xfedcba9876543210ull;
    random_context ^= current_time;
    if (path_x->cnx->client_mode) {
        random_context += 0x0123456789abcdefull;
    }
    if (path_x->unique_path_id > 0 && path_x->unique_path_id != UINT64_MAX) {
        random_context *= (path_x->unique_path_id + 1);
    }
    bbr_state->random_context = random_context;
}

static void BBRInitFullPipe(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->filled_pipe = 0;
    bbr_state->full_bw = 0;
    bbr_state->full_bw_count = 0;
}

/* Initialization of the BBR state */
static void BBROnInit(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    /* TODO:
    init_windowed_max_filter(filter = BBR.MaxBwFilter, value = 0, time = 0)
    */
    memset(bbr_state, 0, sizeof(picoquic_bbr_state_t));
    BBRInitRandom(bbr_state, path_x, current_time);
    /* If RTT was already sampled, use it, other wise set min RTT to infinity */
    if (path_x->smoothed_rtt == PICOQUIC_INITIAL_RTT
        && path_x->rtt_variant == 0) {
        bbr_state->min_rtt = UINT64_MAX;
    }
    else {
        bbr_state->min_rtt = path_x->smoothed_rtt;
    }

    bbr_state->probe_rtt_min_stamp = current_time;
    bbr_state->probe_rtt_min_delay = bbr_state->min_rtt;
    bbr_state->min_rtt_stamp = current_time;
    bbr_state->extra_acked_interval_start = current_time;
    bbr_state->extra_acked_delivered = 0;

    /* TODO, maybe: plug in the "shadow RTT".
     */
    BBRResetCongestionSignals(bbr_state);
    BBRResetLowerBounds(bbr_state);
    BBRInitRoundCounting(bbr_state);
    BBRInitFullPipe(bbr_state);
    BBRInitPacingRate(bbr_state, path_x);
    BBREnterStartup(bbr_state);
}

static void picoquic_bbr_reset(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    BBROnInit(bbr_state, path_x, current_time);
}

static void picoquic_bbr_init(picoquic_cnx_t * cnx, picoquic_path_t* path_x, uint64_t current_time)
{
    /* Initialize the state of the congestion control algorithm */
    picoquic_bbr_state_t* bbr_state = (picoquic_bbr_state_t*)malloc(sizeof(picoquic_bbr_state_t));

    path_x->congestion_alg_state = (void*)bbr_state;
    if (bbr_state != NULL) {
        BBROnInit(bbr_state, path_x, current_time);
    }
}

/* End of init processes for BBr v3 */

/* Release the state of the congestion control algorithm */
static void picoquic_bbr_delete(picoquic_path_t* path_x)
{
    if (path_x->congestion_alg_state != NULL) {
        free(path_x->congestion_alg_state);
        path_x->congestion_alg_state = NULL;
    }
}

/* Path model functions */

/* Managing PTO and recovery */
/* Discuss. This is already largely handled by the transport code.
 * How much of this is needed?
 */
static void BBRModulateCwndForRecovery(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    if (rs->newly_lost > 0) {
        if (path_x->cwin > rs->newly_lost + path_x->send_mtu) {
            path_x->cwin = path_x->cwin - rs->newly_lost;
        }
        else {
            path_x->cwin = path_x->send_mtu;
        }
    }
    if (bbr_state->packet_conservation && path_x->cwin < (path_x->bytes_in_transit + rs->newly_acked)) {
        path_x->cwin = path_x->bytes_in_transit + rs->newly_acked;
    }
}
  

static void BBRBoundCwndForModel(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    uint64_t cap = UINT64_MAX;
    if (IsInAProbeBWState(bbr_state) &&
        bbr_state->state != picoquic_bbr_alg_probe_bw_cruise) {
        if (bbr_state->inflight_hi > 0) {
            cap = bbr_state->inflight_hi;
        }
    }
    else if (bbr_state->state == picoquic_bbr_alg_probe_rtt ||
        bbr_state->state == picoquic_bbr_alg_probe_bw_cruise) {
        cap = BBRInflightWithHeadroom(bbr_state, path_x);
    }

    /* apply inflight_lo (possibly infinite): */
    if (cap > bbr_state->inflight_lo) {
        cap = bbr_state->inflight_lo;
    }
    if (cap < BBRMinPipeCwnd * path_x->send_mtu) {
        cap = BBRMinPipeCwnd * path_x->send_mtu;
    }
    if (path_x->cwin > cap) {
        path_x->cwin = cap;
    }
}

static uint64_t BBRProbeRTTCwnd(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    uint64_t probe_rtt_cwnd = BBRBDPMultiple( bbr_state, path_x, BBRProbeRTTCwndGain);
    if (probe_rtt_cwnd < BBRMinPipeCwnd * path_x->send_mtu) {
        probe_rtt_cwnd = BBRMinPipeCwnd * path_x->send_mtu;
    }
    return probe_rtt_cwnd;
}

static void BBRBoundCwndForProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if (bbr_state->state == picoquic_bbr_alg_probe_rtt) {
        uint64_t cap = BBRProbeRTTCwnd(bbr_state, path_x);
        if (path_x->cwin > cap) {
            path_x->cwin = cap;
        }
    }
}

static void BBRSetCwnd(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    BBRUpdateMaxInflight(bbr_state, path_x);
    BBRModulateCwndForRecovery(bbr_state, path_x, rs);
    if (!bbr_state->packet_conservation) {
        if (bbr_state->filled_pipe) {
            path_x->cwin += rs->newly_acked;
            if (path_x->cwin > bbr_state->max_inflight) {
                path_x->cwin = bbr_state->max_inflight;
            }
        }
        else if (path_x->cwin < bbr_state->max_inflight || path_x->delivered < PICOQUIC_CWIN_INITIAL) {
            path_x->cwin = path_x->cwin+ rs->newly_acked;
        }
        if (path_x->cwin < BBRMinPipeCwnd * path_x->send_mtu) {
            path_x->cwin = BBRMinPipeCwnd * path_x->send_mtu;
        }
    }
    BBRBoundCwndForProbeRTT(bbr_state, path_x);
    BBRBoundCwndForModel(bbr_state, path_x);
}


static uint64_t BBRSaveCwnd(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    /* The original test was:
    *  if (!InLossRecovery(bbr_state) && bbr_state->state != picoquic_bbr_alg_probe_rtt)
    * We are not handling a loss recovery state, so we don't need to test for it.
    */
    if ( /*!InLossRecovery(bbr_state) && */ bbr_state->state != picoquic_bbr_alg_probe_rtt) {
        return path_x->cwin;
    }
    else {
        if (bbr_state->prior_cwnd > path_x->cwin) {
            return bbr_state->prior_cwnd;
        }
        else {
            return path_x->cwin;
        }
    }
}

static uint64_t BBRRestoreCwnd(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if (bbr_state->prior_cwnd > path_x->cwin) {
        return bbr_state->prior_cwnd;
    }
    else {
        return path_x->cwin;
    }
}

/* The draft includes this "enter fast recovery" notion, but does not actually
* define a "fast recovery" state. The QUIC implementation is doing RACK, and does not
* treat "recevery from packet losses" as a special state.
* The draft pseudo code has "packet_conservation" set here, but there is
* no example of setting it to zero. In traditional TCP, this is done when
* the packet sent after the enter recovery event is acknowledged.
 */
static void BBROnEnterFastRecovery(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    bbr_state->prior_cwnd = BBRSaveCwnd(bbr_state, path_x);
    uint64_t additional_cwnd = path_x->send_mtu;
    if (rs->newly_acked > additional_cwnd) {
        additional_cwnd = rs->newly_acked;
    }
    path_x->cwin = path_x->bytes_in_transit + additional_cwnd;
    bbr_state->packet_conservation = 1;
}

/* In picoquic, the arrival of an RTO maps to a "timer based" packet loss.
 */
static void BBROnEnterRTO(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    bbr_state->prior_cwnd = BBRSaveCwnd(bbr_state, path_x);
    path_x->cwin = path_x->bytes_in_transit + path_x->send_mtu;
}

/* Computing the congestion window */
static uint64_t BBRBDPMultipleWithBw(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain, uint64_t bw)
{
    if (bbr_state->min_rtt == UINT64_MAX) {
        return PICOQUIC_CWIN_INITIAL*path_x->send_mtu; /* no valid RTT samples yet */
    }
    bbr_state->bdp = (bw * bbr_state->min_rtt) / 1000000;
    return (uint64_t)(gain * (double)bbr_state->bdp);
}

static uint64_t BBRBDPMultiple(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain)
{
    return BBRBDPMultipleWithBw(bbr_state, path_x, gain, bbr_state->bw);
}

static void BBRUpdateOffloadBudget(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->offload_budget = 3 * bbr_state->send_quantum;
}

static uint64_t BBRQuantizationBudget(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t inflight)
{
    BBRUpdateOffloadBudget(bbr_state);
    if (inflight < bbr_state->offload_budget) {
        inflight = bbr_state->offload_budget;
    }
    if (inflight < BBRMinPipeCwnd * path_x->send_mtu) {
        inflight = BBRMinPipeCwnd * path_x->send_mtu;
    }
    if (bbr_state->state == picoquic_bbr_alg_probe_bw_up) {
        inflight += 2*path_x->send_mtu;
    }
    return inflight;
}

static uint64_t BBRInflightWithBw(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain, uint64_t bw)
{
    uint64_t inflight = BBRBDPMultipleWithBw(bbr_state, path_x, gain, bw);
    return BBRQuantizationBudget(bbr_state, path_x, inflight);
}

static uint64_t BBRInflight(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain)
{
    return BBRInflightWithBw(bbr_state, path_x, gain, bbr_state->bw);
}

static void BBRUpdateMaxInflight(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    /*  The draft mentions here a call to BBRUpdateAggregationBudget(),
    * but does not define that function. Its purpose is apparently to set
    * `extra_acked`, but that variable is computed in
    * BBRUpdateACKAggregation(), which is called as part of 
    * BBRUpdateModelAndState(). There is probably no need to do an extra
    * call here. */
    uint64_t inflight = BBRBDPMultiple(bbr_state, path_x, bbr_state->cwnd_gain);
    inflight += bbr_state->extra_acked;
    bbr_state->max_inflight = BBRQuantizationBudget(bbr_state, path_x, inflight);
}

/* Pacing rate functions */
static void BBRInitPacingRate(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    /* nominal_bandwidth = InitialCwnd / (SRTT ? SRTT : 1ms); */
    uint64_t initial_rtt = PICOQUIC_INITIAL_RTT; /* 1ms */
    if (path_x->smoothed_rtt != PICOQUIC_INITIAL_RTT || path_x->rtt_variant != 0) {
        initial_rtt = path_x->smoothed_rtt;
    }
    double nominal_bandwidth = ((double)(1000000ull * PICOQUIC_CWIN_INITIAL)) / (double)initial_rtt;
    bbr_state->pacing_rate = BBRStartupPacingGain * nominal_bandwidth;
}

static void BBRSetPacingRateWithGain(picoquic_bbr_state_t* bbr_state, double pacing_gain)
{
    double rate = pacing_gain * ((double)(bbr_state->bw * (100 - BBRPacingMarginPercent))) / (double)100;
    if (bbr_state->filled_pipe || rate > bbr_state->pacing_rate) {
        bbr_state->pacing_rate = rate;
    }
}

static void  BBRSetPacingRate(picoquic_bbr_state_t* bbr_state)
{
    BBRSetPacingRateWithGain(bbr_state, bbr_state->pacing_gain);
}

static void BBRSetSendQuantum(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    /* 1.2 Mbps = 150 kBps = 150000Bps  */
    uint64_t floor = 2 * path_x->send_mtu;
    if (bbr_state->pacing_rate < 150000) {
        floor = 1 * path_x->send_mtu;
    }
    /* 1 ms = 1000000us/1000 */
    bbr_state->send_quantum = (uint64_t)(bbr_state->pacing_rate / 1000.0); 
    if (bbr_state->send_quantum > 0x10000) {
        bbr_state->send_quantum = 0x10000;
    }
    if (bbr_state->send_quantum < floor) {
        bbr_state->send_quantum = floor;
    }
}


/* Path model functions when not probing for bandwith */
  /* Near start of ACK processing: */
static void BBRUpdateLatestDeliverySignals(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    /* BBR.bw_latest = max(BBR.bw_latest, rs.delivery_rate) */
    bbr_state->loss_round_start = 0;
    if (bbr_state->bw_latest < rs->delivery_rate) {
        bbr_state->bw_latest = rs->delivery_rate;
    }
    /* BBR.inflight_latest = max(BBR.inflight_latest, rs.delivered) */
    if (bbr_state->inflight_latest < rs->delivered) {
        bbr_state->inflight_latest = rs->delivered;
    }
    
    uint64_t prior_delivered = path_x->delivered - rs->delivered;
    if (prior_delivered >= bbr_state->loss_round_delivered) {
        bbr_state->loss_round_delivered = path_x->delivered;
        bbr_state->loss_round_start = 1;
    }
}

  /* Near end of ACK processing: */
static void BBRAdvanceLatestDeliverySignals(picoquic_bbr_state_t* bbr_state, bbr_per_ack_state_t * rs) {
    if (bbr_state->loss_round_start) {
        bbr_state->bw_latest = rs->delivery_rate;
        bbr_state->inflight_latest = rs->delivered;
    }
}

static void BBRResetCongestionSignals(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->loss_in_round = 0;
    bbr_state->bw_latest = 0;
    bbr_state->inflight_latest = 0;
}

/* Handle the first congestion episode in this cycle */
static void BBRInitLowerBounds(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if (bbr_state->bw_lo == UINT64_MAX) {
        bbr_state->bw_lo = bbr_state->max_bw;
    }
    if (bbr_state->inflight_lo == UINT64_MAX) {
        bbr_state->inflight_lo = path_x->cwin;
    }
}

/* Adjust model once per round based on loss */
static void BBRLossLowerBounds(picoquic_bbr_state_t* bbr_state)
{
    /* set: bw_lo = max(bw_latest, bw_lo*BBRBeta) */
    bbr_state->bw_lo = (uint64_t)(BBRBeta * (double)bbr_state->bw_lo);
    if (bbr_state->bw_lo < bbr_state->bw_latest) {
        bbr_state->bw_lo = bbr_state->bw_latest;
    }
    /* Set: inflight_lo = max(inflight_latest, BBRBeta * bbr_state->inflight_lo) */
    bbr_state->inflight_lo = (uint64_t)(BBRBeta * (double)bbr_state->inflight_lo);
    if (bbr_state->inflight_lo < bbr_state->inflight_latest) {
        bbr_state->inflight_lo = bbr_state->inflight_latest;
    }
}

/* Once per round-trip respond to congestion */
static void BBRAdaptLowerBoundsFromCongestion(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if (IsInAProbeBWState(bbr_state))
        return;
    if (bbr_state->loss_in_round) {
        BBRInitLowerBounds(bbr_state, path_x);
        BBRLossLowerBounds(bbr_state);
    }
}
#if 1
/* Update loss rate tracker on every ACK */
static void BBRTrackLossRate(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs)
{
    bbr_state->delivered_smoothed = (1.0 - BBRLossAlpha) * bbr_state->delivered_smoothed + BBRLossAlpha * (double)(rs->newly_acked + rs->newly_lost);
    bbr_state->lost_smoothed = (1.0 - BBRLossAlpha) * bbr_state->lost_smoothed + BBRLossAlpha * (double)(rs->newly_lost);
    bbr_state->loss_rate_smoothed = bbr_state->lost_smoothed / bbr_state->delivered_smoothed;
}
#endif

/* Update congestion state on every ACK */
static void  BBRUpdateCongestionSignals(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    BBRUpdateMaxBw(bbr_state, path_x, rs);
    if (rs->newly_lost > 0) {
        bbr_state->loss_in_round = 1;
    }
    if (!bbr_state->loss_round_start) {
        return;  /* wait until end of round trip */
    }
    BBRAdaptLowerBoundsFromCongestion(bbr_state, path_x);
    bbr_state->loss_in_round = 0;
}

static void BBRResetLowerBounds(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->bw_lo = UINT64_MAX;
    bbr_state->inflight_lo = UINT64_MAX;
}
        
static void  BBRBoundBWForModel(picoquic_bbr_state_t* bbr_state) {
    /* set bw = min(max_bw, bw_lo, bw_hi)  */
    bbr_state->bw = bbr_state->max_bw;
    if (bbr_state->bw > bbr_state->bw_lo) {
        bbr_state->bw = bbr_state->bw_lo;
    }
    /* TODO: remove the test bw_hi != 0 once variables properly initialized. */
    if (bbr_state->bw > bbr_state->bw_hi && bbr_state->bw_hi != 0) {
        bbr_state->bw = bbr_state->bw_hi;
    }
}


/* Path Model functions when probing for bandwidth */
static void BBRUpdateMaxBw(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    BBRUpdateRound(bbr_state, path_x);
    if (rs->delivery_rate >= bbr_state->max_bw || !rs->is_app_limited) {
#if 1
        if (rs->delivery_rate < bbr_state->max_bw && !path_x->cnx->client_mode) {
            DBG_PRINTF("%s", "Bug");
        }
#endif
        bbr_state->max_bw = update_windowed_max_filter(
            bbr_state->MaxBwFilter, rs->delivery_rate, bbr_state->cycle_count, BBRMaxBwFilterLen);
    }
}

static void BBRAdvanceMaxBwFilter(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->cycle_count++;
    /* Should the current cycle value be set to zero? Or do we simply rely on the
     * natural rythm of updates, keeping the old value if we only see app limited updates?
     */
    start_windowed_max_filter_period(bbr_state->MaxBwFilter, bbr_state->cycle_count, BBRMaxBwFilterLen);
}

static void BBRUpdateACKAggregation(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    /* Find excess ACKed beyond expected amount over this interval */
    uint64_t interval = (current_time - bbr_state->extra_acked_interval_start);
    uint64_t expected_delivered = bbr_state->bw * interval;
    /* Reset interval if ACK rate is below expected rate: */
    if (bbr_state->extra_acked_delivered <= expected_delivered) {
        bbr_state->extra_acked_delivered = 0;
        bbr_state->extra_acked_interval_start = current_time;
        expected_delivered = 0;
    }
    bbr_state->extra_acked_delivered += rs->newly_acked;
    uint64_t extra = bbr_state->extra_acked_delivered - expected_delivered;
    if (extra > path_x->cwin) {
        extra = path_x->cwin;
    }
    bbr_state->extra_acked =
        update_windowed_max_filter(bbr_state->ExtraACKedFilter, extra, bbr_state->round_count, BBRExtraAckedFilterLen);
}

/* Do loss signals suggest inflight is too high?
* If so, react.
* This test can trigger spuriously if there are too few packets in transit.
* For example, if there are two packets in transit and one is lost, the
* test assumes a loss rate of 50%, but this could be a random event that
* happens once every 50 RTT. Decisions made because of that would be wrong.
*/
static int IsInflightTooHigh(picoquic_path_t * path_x, bbr_per_ack_state_t* rs)
{
#if 0
    return (rs->lost > (uint64_t)(((double)rs->tx_in_flight) * BBRLossThresh) &&
        rs->tx_in_flight > path_x->send_mtu * 50);
#else
    return (rs->lost > (uint64_t)(((double)rs->tx_in_flight) * BBRLossThresh));
#endif
}

static void BBRHandleInflightTooHigh(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    /* The computation below compares the number of bytes in flight when the 
     * acked packet was sent to the current target */
    bbr_state->bw_probe_samples = 0;  /* only react once per bw probe */
    if (!rs->is_app_limited) {
        uint64_t beta_target = (uint64_t)(((double)BBRTargetInflight(bbr_state, path_x)) * BBRBeta);
        bbr_state->inflight_hi = (rs->tx_in_flight > beta_target) ? rs->tx_in_flight : beta_target;
    }
    if(bbr_state->state == picoquic_bbr_alg_probe_bw_up) {
        BBRStartProbeBW_DOWN(bbr_state, path_x, current_time);
    }
}

static int CheckInflightTooHigh(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    if (IsInflightTooHigh(path_x, rs))
    {
        if (bbr_state->bw_probe_samples)
        {
            BBRHandleInflightTooHigh(bbr_state, path_x, rs, current_time);
        }
        return 1;  /* inflight too high */
    }
    else {
        return 0;
    }
}

/* BBR Round counting functions */
static void BBRInitRoundCounting(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->next_round_delivered = 0;
    bbr_state->round_start = 0;
    bbr_state->round_count = 0;
}

static void BBRStartRound(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
#if 1
    bbr_state->next_round_delivered = path_x->delivered + path_x->bytes_in_transit;
#else
    bbr_state->next_round_delivered = path_x->delivered;
#endif
}

static void BBRUpdateRound(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    if (path_x->delivered >= bbr_state->next_round_delivered) {
        BBRStartRound(bbr_state, path_x);
        bbr_state->round_count++;
        bbr_state->rounds_since_probe++;
        bbr_state->round_start = 1;
        start_windowed_max_filter_period(bbr_state->ExtraACKedFilter, bbr_state->round_count, BBRExtraAckedFilterLen);
    }
    else {
        bbr_state->round_start = 0;
    }
}


/* End of BBR round counting functions */

/* Restart from idle process
* TODO: add a congestion callback "restart from idle" if sending a packet
* after a long silence. The tests should be done in the transport loop. This
* will need to be handled in their own way by all agorithms, and thus cannot
* be implemented in this PR.
* The required call back is mentioned in section 4.1. of RFC 5681,
* Restarting Idle Connections. This is defined for TCP, but should
* apply equally to a QUIC path. the text says "Therefore, a TCP SHOULD
* set cwnd to no more than RW before beginning transmission if the TCP
* has not sent data in an interval exceeding the retransmission timeout."
* For QUIC, this perhaps should be qualified as "as not sent ack-eliciting data".
* The idle test checks "no bytes in transit"; this imply the callback
* should happen before updating "bytes in transit" for the new packet.
 */
#if 0
static void BBRHandleRestartFromIdle(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time)
{
    if (path_x->bytes_in_transit == 0 && bbr_state->path_is_app_limited)
    {
        bbr_state->idle_restart = 1;
        bbr_state->extra_acked_interval_start = current_time;
        if (IsInAProbeBWState(bbr_state)) {
            BBRSetPacingRateWithGain(bbr_state, 1);
        }
        else if (bbr_state->state == picoquic_bbr_alg_probe_rtt) {
            BBRCheckProbeRTTDone(bbr_state, current_time);
        }
    }
}


/* TODO: check definition of app limited. Why is it expressed here? */
static void MarkConnectionAppLimited(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->path_is_app_limited =
        (C.delivered + packets_in_flight) ?  0 : 1;
}


/* Called when the app has sent something. */
void BBROnTransmit(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    BBRHandleRestartFromIdle(bbr_state, path_x, current_time);
}
#endif
/* End of idle functions */

/* ProbeRTT processes for BBv3 */

#if 1
/* Adapt RTT min margin based on packet transmission time */
static void BBRAdaptMinRttMargin(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    uint64_t margin = ((bbr_state->min_rtt * BBRMinRttMarginPercent) * 100 / 1000000);
    if (bbr_state->max_bw > 0) {
        margin += 2 * path_x->send_mtu * 1000000 / bbr_state->max_bw;
    }
    bbr_state->min_rtt_margin = margin;
}
#endif

static void BBRUpdateMinRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
#if 1
    BBRAdaptMinRttMargin(bbr_state, path_x);
#endif
    /* TODO: replace constants by state variables, computed as function of
     * min RTT */
    bbr_state->probe_rtt_expired =
        current_time > bbr_state->probe_rtt_min_stamp + BBRProbeRTTInterval;
    if (rs->rtt_sample >= 0 &&
        (rs->rtt_sample < bbr_state->probe_rtt_min_delay ||
            bbr_state->probe_rtt_expired)) {
        bbr_state->probe_rtt_min_delay = rs->rtt_sample;
        bbr_state->probe_rtt_min_stamp = current_time;
    }
#if 1
    else {
        /* Deviation from BBRv3: test whether the new measurment does not differ from min_rtt
         * by more than a "margin of error, and in that case delay the need to reevaluate min_rtt */
        if (rs->rtt_sample >= 0 && rs->rtt_sample < (bbr_state->min_rtt + bbr_state->min_rtt_margin)) {
            bbr_state->probe_rtt_min_stamp = current_time;
            bbr_state->min_rtt_stamp = current_time;
        }
    }
#endif
    int min_rtt_expired =
        current_time > bbr_state->min_rtt_stamp + BBRMinRTTFilterLen;
    if (bbr_state->probe_rtt_min_delay < bbr_state->min_rtt ||
        min_rtt_expired) {
        bbr_state->min_rtt = bbr_state->probe_rtt_min_delay;
        bbr_state->min_rtt_stamp = bbr_state->probe_rtt_min_stamp;
    }
}

static void BBRExitProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time)
{
    BBRResetLowerBounds(bbr_state);
    if (bbr_state->filled_pipe) {
        BBRStartProbeBW_DOWN(bbr_state, path_x, current_time);
        BBRStartProbeBW_CRUISE(bbr_state);
    }
    else {
        BBREnterStartup(bbr_state);
    }
}

static void BBRCheckProbeRTTDone(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time)
{
    if (bbr_state->probe_rtt_done_stamp != 0 &&
        current_time > bbr_state->probe_rtt_done_stamp)
    {
        /* schedule next ProbeRTT: */
        bbr_state->probe_rtt_min_stamp = current_time;
        path_x->cwin = BBRRestoreCwnd(bbr_state, path_x);
        BBRExitProbeRTT(bbr_state, path_x, current_time);
    }
}

static void BBRHandleProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    /* Ignore low rate samples during ProbeRTT.*/
    /* We do not implement:
    * MarkConnectionAppLimited();
    * because the app_limited status is maintained as part of app logic.
    */
    /* 
    * testing the bytes in flight when the last ACK was sent,
    * as they reflect the size of the queue encountered when
    * measuring the RTT.
    */
    if (bbr_state->probe_rtt_done_stamp == 0 &&
        rs->tx_in_flight <= BBRProbeRTTCwnd(bbr_state, path_x)) {
        /* Wait for at least ProbeRTTDuration to elapse: */
        bbr_state->probe_rtt_done_stamp =
            current_time + BBRProbeRTTDuration;
            /* Wait for at least one round to elapse: */
        bbr_state->probe_rtt_round_done = 0;
            BBRStartRound(bbr_state, path_x);
    }
    else if (bbr_state->probe_rtt_done_stamp != 0) {
        if (bbr_state->round_start)
            bbr_state->probe_rtt_round_done = 1;
        if (bbr_state->probe_rtt_round_done)
            BBRCheckProbeRTTDone(bbr_state, path_x, current_time);
    }
}


static void BBREnterProbeRTT(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->state = picoquic_bbr_alg_probe_rtt;
    bbr_state->pacing_gain = 1.0;
    bbr_state->cwnd_gain = BBRProbeRTTCwndGain;  /* 0.5 */
}

static void BBRCheckProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    if (bbr_state->state != picoquic_bbr_alg_probe_rtt &&
        bbr_state->probe_rtt_expired &&
        !bbr_state->idle_restart) {
        BBREnterProbeRTT(bbr_state);
        bbr_state->prior_cwnd = BBRSaveCwnd(bbr_state, path_x);
        bbr_state->probe_rtt_done_stamp = 0;
        bbr_state->ack_phase = picoquic_bbr_acks_probe_stopping;
        BBRStartRound(bbr_state, path_x);
    }
    if (bbr_state->state == picoquic_bbr_alg_probe_rtt) {
        BBRHandleProbeRTT(bbr_state, path_x, rs, current_time);
    }
    if (rs->delivered > 0) {
        bbr_state->idle_restart = 0;
    }
}

/* ProbeBW specific processes for BBRv3
* There are actually for states, DOWN, CRUISE, REFILL, and UP.
* TODO: Transition strategy between states is highly dependent on hypotheses,
* such as a BDP of about 63 packets. Investigate what to do if the
* BDP is much higher.
*/

static int IsInAProbeBWState(picoquic_bbr_state_t* bbr_state)
{
    picoquic_bbr_alg_state_t state = bbr_state->state;

    return (state == picoquic_bbr_alg_probe_bw_down ||
        state == picoquic_bbr_alg_probe_bw_cruise ||
        state == picoquic_bbr_alg_probe_bw_refill ||
        state == picoquic_bbr_alg_probe_bw_up);
}

/* 
* Return a volume of data that tries to leave free
* headroom in the bottleneck buffer or link for
* other flows, for fairness convergence and lower
* RTTs and loss */
static uint64_t BBRInflightWithHeadroom(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    if (bbr_state->inflight_hi == UINT64_MAX) {
        return UINT64_MAX;
    }

    /* This diverges from draft-bbr-02, but is correct per feedback from BBR authors. */
    uint64_t inflight_with_headroom = (uint64_t)((1.0-BBRHeadroom) * ((double)bbr_state->inflight_hi));
    if (inflight_with_headroom < (BBRMinPipeCwnd*path_x->send_mtu)) {
        inflight_with_headroom = BBRMinPipeCwnd*path_x->send_mtu;
    }
    return inflight_with_headroom;
}

/* Raise inflight_hi slope if appropriate. */
static void BBRRaiseInflightHiSlope(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    uint64_t growth_this_round = path_x->send_mtu << bbr_state->bw_probe_up_rounds;
    bbr_state->bw_probe_up_rounds = (bbr_state->bw_probe_up_rounds + 1 < 30) ? bbr_state->bw_probe_up_rounds + 1 : 30;
    uint32_t up_cnt = (uint32_t)(path_x->cwin / growth_this_round);
    bbr_state->bw_probe_up_cnt = (up_cnt > 1) ? up_cnt : 1;
}

/* Increase inflight_hi if appropriate. */
static void BBRProbeInflightHiUpward(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t * rs)
{
    if (!rs->is_cwnd_limited || path_x->cwin < bbr_state->inflight_hi)
    {
        return;  /* not fully using inflight_hi, so don't grow it */
    }
    bbr_state->bw_probe_up_acks += rs->newly_acked;
    if (bbr_state->bw_probe_up_acks >= bbr_state->bw_probe_up_cnt) {
        uint64_t delta = (bbr_state->bw_probe_up_acks / bbr_state->bw_probe_up_cnt);
        bbr_state->bw_probe_up_acks -= delta * bbr_state->bw_probe_up_cnt;
        bbr_state->inflight_hi += delta;
    }

    if (bbr_state->round_start){
        BBRRaiseInflightHiSlope(bbr_state, path_x);
    }
}

/* Track ACK state and update BBR.max_bw window and
* BBR.inflight_hi and BBR.bw_hi. */
static void BBRAdaptUpperBounds(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    /*
    picoquic_bbr_acks_probe_starting = 0,
    picoquic_bbr_acks_probe_stopping,
    picoquic_bbr_acks_refilling,
    */
    if (bbr_state->ack_phase == picoquic_bbr_acks_probe_starting && bbr_state->round_start) {
        /* starting to get bw probing samples */
        bbr_state->ack_phase = picoquic_bbr_acks_probe_feedback;
    }
    if (bbr_state->ack_phase == picoquic_bbr_acks_probe_stopping && bbr_state->round_start) {
        /* end of samples from bw probing phase */
        if (IsInAProbeBWState(bbr_state) && !rs->is_app_limited) {
            BBRAdvanceMaxBwFilter(bbr_state);
        }
    }
    if (!CheckInflightTooHigh(bbr_state, path_x, rs, current_time)) {
        /* Loss rate is safe. Adjust upper bounds upward. */
        if (bbr_state->inflight_hi == UINT64_MAX || bbr_state->bw_hi == UINT64_MAX) {
            return; /* no upper bounds to raise */
        }
        if (rs->tx_in_flight > bbr_state->inflight_hi) {
            /* the bytes in flight at the time the packet was sent did not create a queue. */
            bbr_state->inflight_hi = rs->tx_in_flight;
        }
        if (rs->delivery_rate > bbr_state->bw_hi) {
            bbr_state->bw_hi = rs->delivery_rate;
        }
        if (bbr_state->state == picoquic_bbr_alg_probe_bw_up) {
            BBRProbeInflightHiUpward(bbr_state, path_x, rs);
        }
    }
}

/* Time to transition from DOWN to CRUISE? */
static int BBRCheckTimeToCruise(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    if (path_x->bytes_in_transit > BBRInflightWithHeadroom(bbr_state, path_x)) {
        return 0; /* not enough headroom */
    }
    if (path_x->bytes_in_transit <= BBRInflightWithBw(bbr_state, path_x, 1.0, bbr_state->max_bw)) {
        return 1;  /* inflight <= estimated BDP */
    }
    return 0;
}


/* Randomized decision about how long to wait until
* probing for bandwidth, using round count and wall clock.
* TODO: find an init strategy for the random numbers, so we
*       can have repetitive tests.
*/
static uint64_t BBRRandomIntBetween(picoquic_bbr_state_t* bbr_state, uint64_t low, uint64_t high)
{
    return (low + picoquic_test_uniform_random(&bbr_state->random_context, (high - low) + 1));
}

static double BBRRandomFloatBetween(picoquic_bbr_state_t* bbr_state, double low, double high)
{
    uint32_t random_32_bits = (uint32_t)picoquic_test_random(&bbr_state->random_context);
    double random_float = ((double)random_32_bits) / ((double)UINT32_MAX);
    return (low + random_float * (high - low));
}

static void BBRPickProbeWait(picoquic_bbr_state_t* bbr_state) 
{
    /* Decide random round-trip bound for wait: */
    bbr_state->rounds_since_bw_probe =
        (uint32_t)BBRRandomIntBetween(bbr_state, 0, 1); /* 0 or 1 */
    
    /* Decide the random wall clock bound for wait: */
    bbr_state->bw_probe_wait =
        2000000 + BBRRandomIntBetween(bbr_state, 0, 1000000); /* 0..1 sec, in usec */
}

/* How much data do we want in flight?
* Our estimated BDP, unless congestion cut cwnd. */
static uint64_t BBRTargetInflight(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    return (bbr_state->bdp < path_x->cwin) ? bbr_state->bdp : path_x->cwin;
}

static int BBRIsRenoCoexistenceProbeTime(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    uint64_t reno_rounds = BBRTargetInflight(bbr_state, path_x);
    uint64_t rounds = (reno_rounds < 63) ? reno_rounds : 63;
    return (bbr_state->rounds_since_bw_probe >= rounds);
}

/* Is it time to transition from DOWN or CRUISE to REFILL? */
static int BBRHasElapsedInPhase(picoquic_bbr_state_t* bbr_state, uint64_t interval, uint64_t current_time) {
    return current_time > bbr_state->cycle_stamp + interval;
}

static int BBRCheckTimeToProbeBW(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time)
{
    if (BBRHasElapsedInPhase(bbr_state, bbr_state->bw_probe_wait, current_time) ||
        BBRIsRenoCoexistenceProbeTime(bbr_state, path_x)) {
        BBRStartProbeBW_REFILL(bbr_state, path_x);
        return 1;
    }
    else {
        return 0;
    }
}

static void BBRStartProbeBW_DOWN(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time)
{
    bbr_state->pacing_gain = BBRProbeBwDownPacingGain;  /* pace a bit slowly */
    bbr_state->cwnd_gain = BBRProbeBwDownCwndGain;   /* maintain cwnd */
    BBRResetCongestionSignals(bbr_state);
    bbr_state->bw_probe_up_cnt = UINT32_MAX; /* not growing inflight_hi */
    BBRPickProbeWait(bbr_state);
    bbr_state->cycle_stamp = current_time;  /* start wall clock */
    bbr_state->ack_phase = picoquic_bbr_acks_probe_stopping;
    BBRStartRound(bbr_state, path_x);
    bbr_state->state = picoquic_bbr_alg_probe_bw_down;
}

static void BBRStartProbeBW_CRUISE(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->pacing_gain = BBRProbeBwCruisePacingGain;  /* pace at rate */
    bbr_state->cwnd_gain = BBRProbeBwCruiseCwndGain;   /* maintain cwnd */
    bbr_state->state = picoquic_bbr_alg_probe_bw_cruise;
}

static void BBRStartProbeBW_REFILL(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    bbr_state->pacing_gain = BBRProbeBwRefillPacingGain;  /* pace at rate */
    bbr_state->cwnd_gain = BBRProbeBwRefillCwndGain;   /* maintain cwnd */
    BBRResetLowerBounds(bbr_state);
    bbr_state->bw_probe_up_rounds = 0;
    bbr_state->bw_probe_up_acks = 0;
    bbr_state->ack_phase = picoquic_bbr_acks_refilling;
    BBRStartRound(bbr_state, path_x);
    bbr_state->state = picoquic_bbr_alg_probe_bw_refill;
}

static void BBRStartProbeBW_UP(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time)
{
    bbr_state->pacing_gain = BBRProbeBwUpPacingGain;  /* pace at rate */
    bbr_state->cwnd_gain = BBRProbeBwUpCwndGain;   /* maintain cwnd */
    bbr_state->ack_phase = picoquic_bbr_acks_probe_starting;
    BBRStartRound(bbr_state, path_x);
    bbr_state->cycle_stamp = current_time; /* start wall clock */
    bbr_state->state = picoquic_bbr_alg_probe_bw_up;
    BBRRaiseInflightHiSlope(bbr_state, path_x);
}

/* The core state machine logic for ProbeBW: */
static void BBRUpdateProbeBWCyclePhase(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    if (!bbr_state->filled_pipe) {
        return;  /* only handling steady-state behavior here */
    }
    BBRAdaptUpperBounds(bbr_state, path_x, rs, current_time);

    switch (bbr_state->state) {
    case picoquic_bbr_alg_probe_bw_down:
        if (BBRCheckTimeToProbeBW(bbr_state, path_x, current_time))
            return; /* already decided state transition */
        if (BBRCheckTimeToCruise(bbr_state, path_x))
            BBRStartProbeBW_CRUISE(bbr_state);
        break;

    case picoquic_bbr_alg_probe_bw_cruise:
        if (BBRCheckTimeToProbeBW(bbr_state, path_x, current_time))
            return; /* already decided state transition */
        break;

    case picoquic_bbr_alg_probe_bw_refill:
        /* After one round of REFILL, start UP */
        if (bbr_state->round_start) {
            bbr_state->bw_probe_samples = 1;
            BBRStartProbeBW_UP(bbr_state, path_x, current_time);
        }
        break;

    case picoquic_bbr_alg_probe_bw_up:
        if (BBRHasElapsedInPhase(bbr_state, bbr_state->min_rtt, current_time) &&
            path_x->bytes_in_transit > BBRInflightWithBw(bbr_state, path_x, 1.25, bbr_state->max_bw)) {
            BBRStartProbeBW_DOWN(bbr_state, path_x, current_time);
        }
        break;

    default:
        /* In non probe BW states, do nothing. */
        break;
    }
}

static void BBREnterProbeBW(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    BBRStartProbeBW_DOWN(bbr_state, path_x, current_time);
}
/* End of probe BW specific algorithms */

/* Drain specific processes for BBRv3 */
static void BBREnterDrain(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    path_x->is_ssthresh_initialized = 1; /* Picoquic specific: notify transport that the startup phase is complete */
    bbr_state->state = picoquic_bbr_alg_drain;
    bbr_state->pacing_gain = 1.0 / BBRStartupCwndGain;  /* pace slowly */
    bbr_state->cwnd_gain = BBRStartupCwndGain;   /* maintain cwnd */
    bbr_state->cycle_count++; /* Trying to fix an issue */
}

static void BBRCheckDrain(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    if (bbr_state->state == picoquic_bbr_alg_drain && path_x->bytes_in_transit <= BBRInflight(bbr_state, path_x, 1.0)) {
        BBREnterProbeBW(bbr_state, path_x, current_time);  /* we estimate that the queue is drained */
    }
}
/* End of drain specific algorithms */

/* Startup specific processes for BBRv3 */
static void BBRCheckStartupHighLoss(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t * rs)
{
    /* TODO: no sample code provided */
    /*
    * A second method BBR uses for estimating the bottleneck is full is by looking at sustained packet losses.
    Specifically for a case where the following criteria are all met:
    The connection has been in fast recovery for at least one full round trip.
    The loss rate over the time scale of a single full round trip exceeds BBRLossThresh (2%).
    There are at least BBRStartupFullLossCnt=3 discontiguous sequence ranges lost in that round trip.

    If these criteria are all met, then BBRCheckStartupHighLoss() sets BBR.filled_pipe = true, which will cause exit Startup and enters Drain
    */
    if (IsInflightTooHigh(path_x, rs)) {
        bbr_state->filled_pipe = 1;
    }
}

#if 1
static void BBRCheckStartupHighRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t* rs)
{
    /* We have to be careful, because the RTT may get to very high values if the
    * trnasport is only sending ACKs.
     */
    if (!bbr_state->filled_pipe && bbr_state->min_rtt > 0 && rs->is_cwnd_limited) {
        uint64_t delay_cap = (bbr_state->min_rtt / 4) + 2 * path_x->rtt_variant;
        if (rs->rtt_sample > bbr_state->min_rtt + delay_cap) {
            bbr_state->filled_pipe = 1;
        }
    }
}
#endif

static void BBRCheckStartupFullBandwidth(picoquic_bbr_state_t* bbr_state,
    bbr_per_ack_state_t * rs)
{
    if (bbr_state->filled_pipe ||
        !bbr_state->round_start || rs->is_app_limited) {
        return;  /* no need to check for a full pipe now */
    }
    /* Using here 5/4 test instead of double 1.25 */
    if (4*bbr_state->max_bw >= 5*bbr_state->full_bw) {
        /* still growing? */
        bbr_state->full_bw = bbr_state->max_bw;    /* record new baseline level */
        bbr_state->full_bw_count = 0;
        return;
    }
    bbr_state->full_bw_count++; /* another round w/o much growth */
    if (bbr_state->full_bw_count >= 3) {
        bbr_state->filled_pipe = 1;
    }
}

static void BBRCheckStartupDone(picoquic_bbr_state_t* bbr_state,
    picoquic_path_t * path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    BBRCheckStartupFullBandwidth(bbr_state, rs);
    BBRCheckStartupHighLoss(bbr_state, path_x, rs);
#if 1
    BBRCheckStartupHighRTT(bbr_state, path_x, rs);
#endif

    if (bbr_state->state == picoquic_bbr_alg_startup &&
        bbr_state->filled_pipe) {
#if 1
        /* Deviation from draft: the "inflight_hi" value is not set yet.
        * The next update will be on the cycle transition, and will set inflight hi
        * to the low "transit" value used in ProbeDown.
         */
        if (bbr_state->inflight_hi == 0) {
#if 1
            bbr_state->inflight_hi = bbr_state->bdp;
#else
            bbr_state->inflight_hi = bbr_state->inflight_latest;
#endif
        }
#endif
        BBREnterDrain(bbr_state, path_x, current_time);
    }
}

static void BBREnterStartup(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->state = picoquic_bbr_alg_startup;
    bbr_state->pacing_gain = BBRStartupPacingGain;
    bbr_state->cwnd_gain = BBRStartupCwndGain;
}

/* End of BBRv3 startup specific */
#if 1
/* Startup long RTT -- in that state, the code uses Hystart rather than BBR Startup */
void BBREnterStartupLongRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    uint64_t cwnd = PICOQUIC_CWIN_INITIAL;
    bbr_state->state = picoquic_bbr_alg_startup_long_rtt;

    if (path_x->rtt_min > PICOQUIC_TARGET_RENO_RTT) {
        if (path_x->rtt_min > PICOQUIC_TARGET_SATELLITE_RTT) {
            cwnd = (uint64_t)((double)cwnd * (double)PICOQUIC_TARGET_SATELLITE_RTT / (double)PICOQUIC_TARGET_RENO_RTT);
        }
        else {
            cwnd = (uint64_t)((double)cwnd * (double)path_x->rtt_min / (double)PICOQUIC_TARGET_RENO_RTT);
        }
    }
    if (cwnd < bbr_state->bdp_seed) {
        cwnd = bbr_state->bdp_seed;
    }
    if (cwnd > path_x->cwin) {
        path_x->cwin = cwnd;
    }
}

static void BBRExitStartupLongRtt(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    /* Reset the round filter so it will start at current time */
    BBRStartRound(bbr_state, path_x);
    bbr_state->round_count++;
    bbr_state->rounds_since_probe++;
    bbr_state->round_start = 1;
    /* Set the filled pipe indicator */
    bbr_state->filled_pipe = 1;
    /* Check the RTT measurement for pathological cases */
    if ((bbr_state->rtt_filter.is_init || bbr_state->rtt_filter.sample_current > 0) &&
        bbr_state->min_rtt > 30000000 &&
        bbr_state->rtt_filter.sample_max < bbr_state->min_rtt) {
        bbr_state->min_rtt = bbr_state->rtt_filter.sample_max;
        bbr_state->min_rtt_stamp = current_time;
    }
    /* Enter drain */
    BBREnterDrain(bbr_state, path_x, current_time);
    /* If there were just few bytes in transit, enter probe */
    BBRCheckDrain(bbr_state, path_x, current_time);
}

void BBRCheckStartupLongRtt(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    if (bbr_state->state == picoquic_bbr_alg_startup &&
        path_x->rtt_min > PICOQUIC_TARGET_RENO_RTT) {
        BBREnterStartupLongRTT(bbr_state, path_x);
    }
    else if (bbr_state->state != picoquic_bbr_alg_startup_long_rtt) {
        return;
    }

    if (picoquic_hystart_test(&bbr_state->rtt_filter, rs->rtt_sample,
        path_x->pacing_packet_time_microsec, current_time, 0)) {
        BBRExitStartupLongRtt(bbr_state, path_x, current_time);
    }
    else {
        int excessive_loss = picoquic_hystart_loss_volume_test(&bbr_state->rtt_filter, picoquic_congestion_notification_repeat,
            rs->newly_acked, rs->newly_lost);
        if (excessive_loss) {
            BBRExitStartupLongRtt(bbr_state, path_x, current_time);
        }
    }
}

void BBRUpdateStartupLongRtt(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
#if 0
    BBRUpdateBtlBw(bbr_state, path_x, current_time);
#endif
    if (path_x->last_time_acked_data_frame_sent > path_x->last_sender_limited_time) {
        picoquic_hystart_increase(path_x, &bbr_state->rtt_filter, rs->newly_acked);
    }

    uint64_t max_win = path_x->peak_bandwidth_estimate * bbr_state->min_rtt / 1000000;

    if (max_win < bbr_state->bdp_seed) {
        max_win = bbr_state->bdp_seed;
    }

    uint64_t min_win = max_win /= 2;

    if (path_x->cwin < min_win) {
        path_x->cwin = min_win;
    }
}

void BBRSetBdpSeed(picoquic_bbr_state_t* bbr_state, uint64_t bdp_seed)
{
    bbr_state->bdp_seed = bdp_seed;
}
#endif



/* BBRv3 per loss steps.
* TODO: this is part of "path" model
 */


/* At what prefix of packet did losses exceed BBRLossThresh? */
static uint64_t BBRInflightHiFromLostPacket(bbr_per_ack_state_t * rs, picoquic_per_ack_state_t* packet_state)
{
    uint64_t packet_size = packet_state->nb_bytes_newly_lost;
    /* What was in flight before this packet? */
    uint64_t inflight_prev = rs->tx_in_flight - packet_size;
    /* What was lost before this packet? */
    uint64_t lost_prev = rs->lost - packet_size;
    double lost_prefix = (BBRLossThresh *((double)(inflight_prev - lost_prev))) /
        (1.0 - BBRLossThresh);
    /* At what inflight value did losses cross BBRLossThresh? */
    uint64_t inflight = inflight_prev + (uint64_t)lost_prefix;
    return inflight;
}

static void BBRHandleLostPacket(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, picoquic_per_ack_state_t* packet_state, uint64_t current_time)
{
    if (bbr_state->bw_probe_samples == 0) {
        return; /* not a packet sent while probing bandwidth */
    }
    bbr_per_ack_state_t rs = { 0 };
    BBRSetRsFromAckState(path_x, packet_state, &rs);

    if (IsInflightTooHigh(path_x, &rs)) {
        rs.tx_in_flight = BBRInflightHiFromLostPacket(&rs, packet_state);
        BBRHandleInflightTooHigh(bbr_state, path_x, &rs, current_time);
    }
}

static void BBRUpdateOnLoss(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, picoquic_per_ack_state_t* packet_state, uint64_t current_time)
{
    BBRHandleLostPacket(bbr_state, path_x, packet_state, current_time);
}

/* BBRv3 per ACK steps
* The function BBRUpdateOnACK is executed for each ACK notification on the API 
*/
static void BBRUpdateModelAndState(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    BBRUpdateLatestDeliverySignals(bbr_state, path_x, rs);
    BBRUpdateCongestionSignals(bbr_state, path_x, rs);
    BBRUpdateACKAggregation(bbr_state, path_x, rs, current_time);
#if 1
    BBRCheckStartupLongRtt(bbr_state, path_x, rs, current_time);
#endif
    BBRCheckStartupDone(bbr_state, path_x, rs, current_time);
    BBRCheckDrain(bbr_state, path_x, current_time);
    BBRUpdateProbeBWCyclePhase(bbr_state, path_x, rs, current_time);
    BBRUpdateMinRTT(bbr_state, path_x, rs, current_time);
    BBRCheckProbeRTT(bbr_state, path_x, rs, current_time);
    BBRAdvanceLatestDeliverySignals(bbr_state, rs);
    BBRBoundBWForModel(bbr_state);
}

static void BBRUpdateControlParameters(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    BBRSetPacingRate(bbr_state);
    BBRSetSendQuantum(bbr_state, path_x);
    BBRSetCwnd(bbr_state, path_x, rs);
}

void  BBRUpdateOnACK(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    BBRUpdateModelAndState(bbr_state, path_x, rs, current_time);
    if (bbr_state->state == picoquic_bbr_alg_startup_long_rtt) {
        BBRUpdateStartupLongRtt(bbr_state, path_x, rs, current_time);
    }
    else {
        BBRUpdateControlParameters(bbr_state, path_x, rs);
    }
}

/* First step of BBR ACK processing:
* convert the discrete arguments of "picoquic_bbr_notify"
* into the "rs" structure used in the BBRv3 draft. We do
* that so that it is easy to compare the code to the draft.
* 
* 
 * Code maintains the following counters per path
 * - total_bytes_lost -- number of bytes deemed lost from beginning of path
 * - delivered -- amount delivered so far
 * - rtt_sample -- last rtt sample
 * - bytes_in_transit -- bytes currently in flight
 * 
 * It does not contain `data_lost`, but that could be inferred if
 * we keep a variable `nb_bytes_lost_since_packet_sent`.
 * The packet data contains: delivered_prior, so that the BBR variable
 * "delivered" can be computed = path->delivered - packet->delivered_prior.
 */
static void BBRSetRsFromAckState(picoquic_path_t* path_x, picoquic_per_ack_state_t* ack_state, bbr_per_ack_state_t* rs)
{
    /* Need to compute the delivery rate */
    if (path_x->bandwidth_estimate > 0) {
        rs->delivery_rate = path_x->bandwidth_estimate;
    }
    else if (ack_state->rtt_measurement > 0) {
        rs->delivery_rate = 1000000 * ack_state->nb_bytes_delivered_since_packet_sent / ack_state->rtt_measurement;
    }
    else
    {
        rs->delivery_rate = 40000;
    }
    rs->delivered = ack_state->nb_bytes_delivered_since_packet_sent;
    /* variable in path */
    rs->rtt_sample = path_x->rtt_sample;
    /* variables from call */
    rs->newly_acked = ack_state->nb_bytes_acknowledged; /* volume of data acked by current ack */
    rs->newly_lost = ack_state->nb_bytes_newly_lost; /* volume of data marked lost on ack received */
    rs->lost = ack_state->nb_bytes_lost_since_packet_sent;
    rs->tx_in_flight = ack_state->inflight_prior;
    rs->is_app_limited = ack_state->is_app_limited; /*Checked that this is properly implemented */   
    rs->is_cwnd_limited = ack_state->is_cwnd_limited; 
}

static void picoquic_bbr_notify_ack(
    picoquic_bbr_state_t* bbr_state,
    picoquic_path_t* path_x,
    picoquic_per_ack_state_t* ack_state,
    uint64_t current_time)
{
    bbr_per_ack_state_t rs = { 0 };
    BBRSetRsFromAckState(path_x, ack_state, &rs);
    /* TODO: call handle ACK */
    BBRUpdateOnACK(bbr_state, path_x, &rs, current_time);
}

/*
 * In order to implement BBR, we map generic congestion notification
 * signals to the corresponding BBR actions.
 */
static void picoquic_bbr_notify(
    picoquic_cnx_t* cnx,
    picoquic_path_t* path_x,
    picoquic_congestion_notification_t notification,
    picoquic_per_ack_state_t * ack_state,
    uint64_t current_time)
{
    picoquic_bbr_state_t* bbr_state = (picoquic_bbr_state_t*)path_x->congestion_alg_state;
    path_x->is_cc_data_updated = 1;

    if (bbr_state != NULL) {
        switch (notification) {
        case picoquic_congestion_notification_ecn_ec:
            /* TODO */
            break;
        case picoquic_congestion_notification_repeat:
        case picoquic_congestion_notification_timeout:
            BBRUpdateOnLoss(bbr_state, path_x, ack_state, current_time);
            /* TODO: if loss is PTO, we should start the OnPto processing */
            break;
        case picoquic_congestion_notification_spurious_repeat:
            /* TODO: handling of suspension */
            break;
        case picoquic_congestion_notification_rtt_measurement:
            /* TODO: this call is subsumed by the acknowledgement notification.
             * Consider removing it from the API once other CC algorithms are updated.  */
            break;
        case picoquic_congestion_notification_acknowledgement: 
            picoquic_bbr_notify_ack(bbr_state, path_x, ack_state, current_time);
            if (bbr_state->state == picoquic_bbr_alg_startup_long_rtt) {
                picoquic_update_pacing_data(cnx, path_x, 1);
            }
            else if (bbr_state->pacing_rate > 0) {
                /* Set the pacing rate in picoquic sender */
                picoquic_update_pacing_rate(cnx, path_x, bbr_state->pacing_rate, bbr_state->send_quantum);
            }
            break;
        case picoquic_congestion_notification_cwin_blocked:
            break;
        case picoquic_congestion_notification_reset:
            picoquic_bbr_reset(bbr_state, path_x, current_time);
            break;
        case picoquic_congestion_notification_seed_cwin:
            BBRSetBdpSeed(bbr_state, ack_state->nb_bytes_acknowledged);
            break;
        default:
            /* ignore */
            break;
        }
    }
#if 0
    /* Debug code -- when it is useful to put a break after a particular 
     * pattern is found in the traces, to see what caused it. */
    if (!cnx->client_mode && current_time > 40000 && path_x->cwin == 29576) {
        DBG_PRINTF("%s", "Bug");
    }
#endif
}

/* Observe the state of congestion control */

void picoquic_bbr_observe(picoquic_path_t* path_x, uint64_t* cc_state, uint64_t* cc_param)
{
    picoquic_bbr_state_t* bbr_state = (picoquic_bbr_state_t*)path_x->congestion_alg_state;
    *cc_state = (uint64_t)bbr_state->state;
    *cc_param = bbr_state->btl_bw;
}

#define picoquic_bbr_ID "bbr" /* BBR */

picoquic_congestion_algorithm_t picoquic_bbr_algorithm_struct = {
    picoquic_bbr_ID, PICOQUIC_CC_ALGO_NUMBER_BBR,
    picoquic_bbr_init,
    picoquic_bbr_notify,
    picoquic_bbr_delete,
    picoquic_bbr_observe
};

picoquic_congestion_algorithm_t* picoquic_bbr_algorithm = &picoquic_bbr_algorithm_struct;