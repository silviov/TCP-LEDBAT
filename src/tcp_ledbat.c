/*
 * TCP-LEDBAT

 * Implement the congestion control algorithm described in
 * draft-shalunov-ledbat-congestion-00.txt available at 
 * http://tools.ietf.org/html/draft-shalunov-ledbat-congestion-00
 *
 * Our implementation is derived from the TCP-LP kernel implementation
 * (cfr. tcp_lp.c)
 *
 * Created by Silvio Valenti on tue 2nd June 2009
 */

#include <linux/module.h>
#include <net/tcp.h>
#include <linux/vmalloc.h>

/* resolution of owd */
#define LP_RESOL       1000

#define  DEBUG_SLOW_START    0
#define  DEBUG_DELAY         0
#define  DEBUG_OWD_HZ        0
#define  DEBUG_NOISE_FILTER  0
#define  DEBUG_BASE_HISTO    0

/* NOTE: len are the actual length - 1 */
static int base_histo_len = 10;
static int noise_filter_len = 4;
static int target = 100;
static int gain_num = 1;
static int gain_den = 1;
static int do_ss = 0;
static int ledbat_ssthresh = 0xffff;

module_param(base_histo_len, int, 0644);
MODULE_PARM_DESC(base_histo_len, "length of the base history vector");
module_param(noise_filter_len, int, 0644);
MODULE_PARM_DESC(noise_filter_len, "length of the noise_filter vector");
module_param(target, int, 0644);
MODULE_PARM_DESC(target, "target queuing delay");
module_param(gain_num, int, 0644);
MODULE_PARM_DESC(gain_num, "multiplicative factor of the gain");
module_param(gain_den, int, 0644);
MODULE_PARM_DESC(gain_den, "multiplicative factor of the gain");

/* Our extensions to play with slow start */
#define DO_NOT_SLOWSTART            0
#define DO_SLOWSTART                1
#define DO_SLOWSTART_WITH_THRESHOLD 2

module_param(do_ss, int, 0644);
MODULE_PARM_DESC(do_ss, "do slow start: 0 no, 1 yes, 2 with_ssthresh");
module_param(ledbat_ssthresh, int, 0644);
MODULE_PARM_DESC(ledbat_ssthresh, "slow start threshold");

struct owd_circ_buf {
	u32 *buffer;
	u8 first;
	u8 next;
	u8 len;
	u8 min;
};

/**
 * enum tcp_ledbat_state
 * @LP_VALID_RHZ: is remote HZ valid?
 * @LP_VALID_OWD: is OWD valid?
 * @LP_WITHIN_THR: are we within threshold?
 * @LP_WITHIN_INF: are we within inference?
 *
 * TCP-LEDBAT's state flags.
 * We create this set of state flags mainly for debugging.
 */
enum tcp_ledbat_state {
	LEDBAT_VALID_RHZ = (1 << 0),
	LEDBAT_VALID_OWD = (1 << 1),
	LEDBAT_INCREASING = (1 << 2),
	LEDBAT_CAN_SS = (1 << 3),
};

/**
 * struct ledbat
 */
struct ledbat {
	u32 last_rollover;
	u32 remote_hz;
	u32 remote_ref_time;
	u32 local_ref_time;
	u32 snd_cwnd_cnt;	/* already in struct tcp_sock but we need 32 bits. */
	u32 last_ack;
	struct owd_circ_buf base_history;
	struct owd_circ_buf noise_filter;
	u32 flag;
};

static int ledbat_init_circbuf(struct owd_circ_buf *buffer, u16 len)
{
	u32 *b = kmalloc(len * sizeof(u32), GFP_KERNEL);
	if (b == NULL)
		return 1;
	buffer->len = len;
	buffer->buffer = b;
	buffer->first = 0;
	buffer->next = 0;
	buffer->min = 0;
	return 0;
}

static void tcp_ledbat_release(struct sock *sk)
{
	struct ledbat *ledbat = inet_csk_ca(sk);
	kfree(ledbat->noise_filter.buffer);
	kfree(ledbat->base_history.buffer);
}

/**
 * tcp_ledbat_init
 */
static void tcp_ledbat_init(struct sock *sk)
{
	struct ledbat *ledbat = inet_csk_ca(sk);

	ledbat_init_circbuf(&(ledbat->base_history), base_histo_len + 1);
	ledbat_init_circbuf(&(ledbat->noise_filter), noise_filter_len + 1);

	ledbat->last_rollover = 0;
	ledbat->flag = 0;
	ledbat->remote_hz = 0;
	ledbat->remote_ref_time = 0;
	ledbat->local_ref_time = 0;
	ledbat->snd_cwnd_cnt = 0;
	ledbat->last_ack = 0;

	if (do_ss) {
		ledbat->flag |= LEDBAT_CAN_SS;
	}

}

typedef u32(*ledbat_filter_function) (struct owd_circ_buf *);

static u32 ledbat_min_circ_buff(struct owd_circ_buf *b)
{
	/* 
	   The draft requires all history to be set to +infinity on initialization. 
	   We obtain the same behavior returning +infinity in case of empty history.
	 */
	if (b->first == b->next)
		return 0xffffffff;
	return b->buffer[b->min];
}

static
u32 ledbat_current_delay(struct ledbat *ledbat, ledbat_filter_function filter)
{
	return filter(&(ledbat->noise_filter));
}

static u32 ledbat_base_delay(struct ledbat *ledbat)
{
	return ledbat_min_circ_buff(&(ledbat->base_history));
}

#if DEBUG_NOISE_FILTER || DEBUG_BASE_HISTO
static void print_delay(struct owd_circ_buf *cb, char *name)
{
	u16 curr = cb->first;
	printk(KERN_DEBUG "%s: time %u ", name, tcp_time_stamp);

	while (curr != cb->next) {
		printk(KERN_DEBUG "%u ", cb->buffer[curr]);
		curr = (curr + 1) % cb->len;
	}

	printk(KERN_DEBUG "min %u, len %u, first %u, next %u\n",
	       cb->buffer[cb->min], cb->len, cb->first, cb->next);
}
#endif

static
u32 tcp_ledbat_ssthresh(struct sock *sk)
{
	u32 res;
	switch (do_ss) {
	case DO_NOT_SLOWSTART:
	case DO_SLOWSTART:
	default:
		res = tcp_reno_ssthresh(sk);
		break;
	case DO_SLOWSTART_WITH_THRESHOLD:
		res = ledbat_ssthresh;
		break;
	}

	return res;
}

/**
 * tcp_ledbat_cong_avoid
 */
static void tcp_ledbat_cong_avoid(struct sock *sk, u32 ack, u32 acked)
// ns2 version takes also this two parameters: u32 rtt, int flag
{

	struct ledbat *ledbat = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	s64 queue_delay;
	s64 offset;
	s64 cwnd;
	u32 max_cwnd;
	s64 current_delay;
	s64 base_delay;

	/*if no valid data return */
	if (!(ledbat->flag & LEDBAT_VALID_OWD))
		return;

	max_cwnd = ((u32) (tp->snd_cwnd)) * target;

	/* 
	   This checks that we are not limited by the congestion window nor by the
	   application, and is basically the same check that it is performed in the
	   draft.
	 */
	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tp->snd_cwnd <= 1)
		ledbat->flag |= LEDBAT_CAN_SS;

	if (do_ss >= DO_SLOWSTART && tp->snd_cwnd <= tcp_ledbat_ssthresh(sk) &&
	    (ledbat->flag & LEDBAT_CAN_SS)) {
#if DEBUG_SLOW_START
		printk(KERN_DEBUG
		       "slow_start!!! clamp %d cwnd %d sshthresh %d \n",
		       tp->snd_cwnd_clamp, tp->snd_cwnd, tp->snd_ssthresh);
#endif
		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;
	} else {
		ledbat->flag &= ~LEDBAT_CAN_SS;
	}

	/* This allows to eventually define new filters for the current delay. */
	current_delay =
	    ((s64) ledbat_current_delay(ledbat, &ledbat_min_circ_buff));
	base_delay = ((s64) ledbat_base_delay(ledbat));

	queue_delay = current_delay - base_delay;
	offset = ((s64) target) - (queue_delay);

	offset *= gain_num;
	offset = do_div(offset, gain_den);

	/* Do not ramp more than TCP. */
	if (offset > target)
		offset = target;

#if DEBUG_DELAY
	printk(KERN_DEBUG
	       "time %u, queue_delay %lld, offset %lld cwnd_cnt %u, "
	       "cwnd %u, delay %lld, min %lld\n",
	       tcp_time_stamp, queue_delay, offset, ledbat->snd_cwnd_cnt,
	       tp->snd_cwnd, current_delay, base_delay);
#endif

	/* calculate the new cwnd_cnt */
	cwnd = ledbat->snd_cwnd_cnt + offset;
	if (cwnd >= 0) {
		/* if we have a positive number update the cwnd_count */
		ledbat->snd_cwnd_cnt = cwnd;
		if (ledbat->snd_cwnd_cnt >= max_cwnd) {
			/* increase the cwnd */
			if (tp->snd_cwnd < tp->snd_cwnd_clamp)
				tp->snd_cwnd++;
			ledbat->snd_cwnd_cnt = 0;
		}
	} else {
		/* we need to decrease the cwnd but we do not want to set it to 0! */
		if (tp->snd_cwnd > 1) {
			tp->snd_cwnd--;
			/* set the cwnd_cnt to the max value - target */
			ledbat->snd_cwnd_cnt = (tp->snd_cwnd - 1) * target;
		} else {
			tp->snd_cwnd_cnt = 0;
		}
	}

}

/**
 * tcp_ledbat_remote_hz_estimator
 *
 * Estimate remote HZ.
 * We keep on updating the estimated value, where original TCP-LP
 * implementation only guesses it once and uses it forever.
 */
static u32 tcp_ledbat_remote_hz_estimator(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct ledbat *ledbat = inet_csk_ca(sk);
	s64 rhz = ledbat->remote_hz << 6;	/* remote HZ << 6 */
	s64 m = 0;

	if (ledbat->last_rollover == 0)
		ledbat->last_rollover = tcp_time_stamp;

	/* not yet record reference time
	 * go away!! record it before come back!! */
	if (ledbat->remote_ref_time == 0 || ledbat->local_ref_time == 0)
		goto out;

	/* we can't calc remote HZ with no difference!! */
	if (tp->rx_opt.rcv_tsval == ledbat->remote_ref_time
	    || tp->rx_opt.rcv_tsecr == ledbat->local_ref_time)
		goto out;

	m = HZ * (tp->rx_opt.rcv_tsval -
		  ledbat->remote_ref_time) / (tp->rx_opt.rcv_tsecr -
					      ledbat->local_ref_time);
	if (m < 0)
		m = -m;

	if (rhz > 0) {
		m -= rhz >> 6;	/* m is now wrong in remote HZ est */
		rhz += m;	/* 63/64 old + 1/64 new */
	} else
		rhz = m << 6;

 out:
	/* record time for successful remote HZ calc */
	if ((rhz >> 6) > 0)
		ledbat->flag |= LEDBAT_VALID_RHZ;
	else
		ledbat->flag &= ~LEDBAT_VALID_RHZ;

	/* record reference time stamp */

	ledbat->remote_ref_time = tp->rx_opt.rcv_tsval;
	ledbat->local_ref_time = tp->rx_opt.rcv_tsecr;

	return rhz >> 6;
}

/**
 * tcp_ledbat_owd_calculator
 *
 * I stole this code from tcp_lp.c. Please also see comment for 
 * tcp_lp_owd_calculator.
 */
static u32 tcp_ledbat_owd_calculator(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct ledbat *ledbat = inet_csk_ca(sk);
	s64 owd = 0;

	ledbat->remote_hz = tcp_ledbat_remote_hz_estimator(sk);

	if (ledbat->flag & LEDBAT_VALID_RHZ) {
		owd = tp->rx_opt.rcv_tsval * (LP_RESOL / ledbat->remote_hz) -
		    tp->rx_opt.rcv_tsecr * (LP_RESOL / HZ);
		if (owd < 0)
			owd = -owd;
	}

	/* Safe net. */
	if (owd > 0)
		ledbat->flag |= LEDBAT_VALID_OWD;
	else
		ledbat->flag &= ~LEDBAT_VALID_OWD;

#if DEBUG_OWD_HZ
	printk(KERN_DEBUG "my_hz %u, hz %u owd %u\n", HZ,
	       (u32) ledbat->remote_hz, (u32) owd);
#endif
	return owd;
}

static void ledbat_add_delay(struct owd_circ_buf *cb, u32 owd)
{
	u8 i;

	if (cb->next == cb->first) {
		/*buffer is empty */
		cb->buffer[cb->next] = owd;
		cb->min = cb->next;
		cb->next++;
		return;
	}

	/*set the new delay */
	cb->buffer[cb->next] = owd;
	/* update the min if it is the case */
	if (owd < cb->buffer[cb->min])
		cb->min = cb->next;

	/* increment the next pointer */
	cb->next = (cb->next + 1) % cb->len;

	if (cb->next == cb->first) {
		/* Discard the first element */
		if (cb->min == cb->first) {
			/* Discard the min, search a new one */
			cb->min = i = (cb->first + 1) % cb->len;
			while (i != cb->next) {
				if (cb->buffer[i] < cb->buffer[cb->min])
					cb->min = i;
				i = (i + 1) % cb->len;
			}
		}
		/* move the first */
		cb->first = (cb->first + 1) % cb->len;
	}
}

static void ledbat_update_current_delay(struct ledbat *ledbat, u32 owd)
{
	ledbat_add_delay(&(ledbat->noise_filter), owd);
#if DEBUG_NOISE_FILTER
	printk(KERN_DEBUG " added delay to noisefilter %u\n", owd);
	print_delay(&(ledbat->noise_filter), "noise_filter");
#endif
}

static void ledbat_update_base_delay(struct ledbat *ledbat, u32 owd)
{
	u32 last;
	struct owd_circ_buf *cb = &(ledbat->base_history);

	if (ledbat->base_history.next == ledbat->base_history.first) {
		/* empty circular buffer */
		ledbat_add_delay(cb, owd);
		return;
	}

	if (tcp_time_stamp - ledbat->last_rollover > 60 * HZ) {
		/* we have finished a minute */
#if DEBUG_BASE_HISTO
		printk(KERN_DEBUG " time %u, new rollover \n", tcp_time_stamp);
#endif
		ledbat->last_rollover = tcp_time_stamp;
		ledbat_add_delay(cb, owd);
	} else {
		/* update the last value and the min if it is the case */
		last = (cb->next + cb->len - 1) % cb->len;

		if (owd < cb->buffer[last]) {
			cb->buffer[last] = owd;
			if (owd < cb->buffer[cb->min])
				cb->min = last;
		}

	}
#if DEBUG_BASE_HISTO
	printk(KERN_DEBUG " added delay to base_history %s", "\n");
	print_delay(&(ledbat->base_history), "base_history");
#endif
}

/**
 * tcp_ledbat_rtt_sample
 *
 * - calculate the owd
 * - add the delay to noise filter
 * - if new minute add the delay to base delay or update last delay 
 */
static void tcp_ledbat_rtt_sample(struct sock *sk, u32 rtt)
{
	struct ledbat *ledbat = inet_csk_ca(sk);
	s64 mowd = tcp_ledbat_owd_calculator(sk);

	/* sorry that we don't have valid data */
	if (!(ledbat->flag & LEDBAT_VALID_RHZ)
	    || !(ledbat->flag & LEDBAT_VALID_OWD)) {
		return;
	}

	ledbat_update_current_delay(ledbat, (u32) mowd);
	ledbat_update_base_delay(ledbat, (u32) mowd);
}

/**
 * tcp_ledbat_pkts_acked
 */
static void tcp_ledbat_pkts_acked(struct sock *sk,
				  const struct ack_sample *sample)
{
	struct ledbat *ledbat = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	if (sample->rtt_us > 0)
		tcp_ledbat_rtt_sample(sk, sample->rtt_us);

	if (ledbat->last_ack == 0)
		ledbat->last_ack = tcp_time_stamp;
	else if (before(tcp_time_stamp, ledbat->last_ack + (tp->srtt_us >> 3))) {
		/* we haven't receive an acknoledgement for more than an rtt.
		   Set the congestion window to 1. */
		ledbat->last_ack = tcp_time_stamp;
		tp->snd_cwnd = 1;
	}

}

static struct tcp_congestion_ops tcp_ledbat = {
	.init = tcp_ledbat_init,
	.ssthresh = tcp_ledbat_ssthresh,
	.cong_avoid = tcp_ledbat_cong_avoid,
	.pkts_acked = tcp_ledbat_pkts_acked,
	.release = tcp_ledbat_release,

	.owner = THIS_MODULE,
	.name = "ledbat"
};

static int __init tcp_ledbat_register(void)
{
	BUILD_BUG_ON(sizeof(struct ledbat) > ICSK_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&tcp_ledbat);
}

static void __exit tcp_ledbat_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_ledbat);
}

module_init(tcp_ledbat_register);
module_exit(tcp_ledbat_unregister);

MODULE_AUTHOR("Silvio Valenti");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP Ledbat (Low Extra Delay Background Transport)");
