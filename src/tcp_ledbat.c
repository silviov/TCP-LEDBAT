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
#define  DEBUG_DELAY         1
#define  DEBUG_OWD_HZ        1
#define  DEBUG_NOISE_FILTER  0
#define  DEBUG_BASE_HISTO    0

/*remember that the len are the actual length - 1*/
static int base_histo_len   = 6;
static int noise_filter_len = 3;
static int target           = 25;
static int alpha1            = 1;
static int alpha2            = 1;
static int do_ss             = 0;
static int ledbat_ssthresh   = 0xffff;

module_param(base_histo_len, int, 0644);
MODULE_PARM_DESC(base_histo_len, "length of the base history vector");
module_param(noise_filter_len, int, 0644);
MODULE_PARM_DESC(noise_filter_len, "length of the noise_filter vector");
module_param(target, int, 0644);
MODULE_PARM_DESC(target, "target queuing delay");
module_param(alpha1, int, 0644);
MODULE_PARM_DESC(alpha1, "multiplicative factor of the gain");
module_param(alpha2, int, 0644);
MODULE_PARM_DESC(alpha2, "multiplicative factor of the gain");
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
	LEDBAT_VALID_RHZ  = (1 << 0),
	LEDBAT_VALID_OWD  = (1 << 1),
	LEDBAT_INCREASING = (1 << 2),
	LEDBAT_CAN_SS     = (1 << 3),
};

enum ledbat_min_type {
	MIN_BASE_HISTO,
	MIN_GLOBAL,
	MIN_MAV,
};

enum ledbat_target_type {
	TARGET_FIX,
	TARGET_SQUARE,
	TARGET_TRIANGLE,
};

/**
 * struct ledbat
 */
struct ledbat {
	u32 last_rollover;
	u32 remote_hz;
	u32 remote_ref_time;
	u32 local_ref_time;
	u32 snd_cwnd_cnt;

	struct owd_circ_buf base_history;
	struct owd_circ_buf noise_filter;

	u32 flag;            
};

static
int ledbat_init_circbuf(
	struct owd_circ_buf *buffer, 
	u16 len)
{
	u32 *b = kmalloc( len * sizeof(u32), GFP_KERNEL);
	if (b == NULL)
		return 1;
	/* printk ( KERN_DEBUG "size of ledbat_struct %d\n", sizeof(struct ledbat)); */
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
	printk( KERN_DEBUG "structure released...\n");
}

/**
 * tcp_ledbat_init
 */
static void tcp_ledbat_init(struct sock *sk)
{
	struct ledbat *ledbat = inet_csk_ca(sk);

	ledbat_init_circbuf( &(ledbat->base_history), 
				  base_histo_len + 1);

	ledbat_init_circbuf( &(ledbat->noise_filter), 
				  noise_filter_len + 1);

	ledbat->last_rollover = 0;
	ledbat->flag = 0;
	ledbat->remote_hz = 0;
	ledbat->remote_ref_time = 0;
	ledbat->local_ref_time = 0;
	printk( KERN_DEBUG "structure initialized...\n");
	if (do_ss) {
		ledbat->flag |= LEDBAT_CAN_SS;
	}
}

static u32 ledbat_min_circ_buff(struct owd_circ_buf *b) {
	if (b->first == b->next)
		return 0xffffffff;
	return b->buffer[b->min];
}

static
u32 ledbat_current_delay(struct ledbat *ledbat) {
	return ledbat_min_circ_buff(&(ledbat->noise_filter));
}

static
u32 ledbat_base_delay(struct ledbat *ledbat) {
	return ledbat_min_circ_buff(&(ledbat->base_history));
}

static
void print_delay(struct owd_circ_buf *cb, char *name) {
	u16 curr;
	
	curr = cb->first;

	printk( KERN_DEBUG "%s: time %u ", name, tcp_time_stamp);

	while (curr != cb->next) {
		printk( KERN_DEBUG "%u ", cb->buffer[curr]);
		curr = (curr + 1) % cb->len;
	}
	
	printk ( KERN_DEBUG "min %u, len %u, first %u, next %u\n", 
		cb->buffer[cb->min],
		cb->len,
		cb->first,
		cb->next);
}

/**
 * tcp_ledbat_cong_avoid
 *
 */
static void tcp_ledbat_cong_avoid(struct sock *sk, 
				  u32 ack, u32 in_flight) //u32 rtt, , int flag
{

	struct ledbat *ledbat = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	s64 queue_delay;
	s64 offset;
	s64 cwnd;
	u32 max_cwnd; 
	s64 current_delay;
	s64 base_delay;
	u32	ssthresh;

	/*if no valid data return*/
	if (!(ledbat->flag & LEDBAT_VALID_OWD))
		return;

	max_cwnd = ((u32) (tp->snd_cwnd))*target;

	/* XXX ??? 
	   copied from tcp_reno_congestion_avoidance...
	*/
	if (!tcp_is_cwnd_limited(sk, in_flight))
		return;

	if (tp->snd_cwnd <= 1) {
		ledbat->flag |= LEDBAT_CAN_SS;
	}

    ssthresh = (do_ss == 2 ? ledbat_ssthresh : tp->snd_ssthresh);

	if (do_ss && tp->snd_cwnd <= ssthresh && (ledbat->flag & LEDBAT_CAN_SS)) {
#if DEBUG_SLOW_START
		printk(KERN_DEBUG "slow_start!!! clamp %d cwnd %d sshthresh %d \n", 
			tp->snd_cwnd_clamp, tp->snd_cwnd, tp->snd_ssthresh);
#endif
		tcp_slow_start(tp);
		return;
	} else {
		ledbat->flag &= ~LEDBAT_CAN_SS;
	}

	current_delay = ((s64)ledbat_current_delay(ledbat));
	base_delay    = ((s64)ledbat_base_delay(ledbat));

	queue_delay = current_delay - base_delay;
	offset = ((s64)target) - (queue_delay);

	/* offset *= alpha1; offset /= alpha2; */

	/* do not ramp more than TCP */
	if (offset > target)
		offset = target;
	
#if DEBUG_DELAY
	printk ( KERN_DEBUG
	 "time %u, queue_delay %lld, offset %lld cwnd_cnt %u, cwnd %u, delay %lld, min %lld\n", 
		tcp_time_stamp, queue_delay, offset, tp->snd_cwnd_cnt, tp->snd_cwnd, 
		current_delay, base_delay);
#endif

	/* calculate the new cwnd_cnt */
	cwnd = tp->snd_cwnd_cnt + offset;
	if (cwnd >= 0) 
	{
	  /* if we have a positive number update the cwnd_count */
		tp->snd_cwnd_cnt = cwnd;
		if (tp->snd_cwnd_cnt >= max_cwnd) 
		{
		  /* increase the cwnd */
			if (tp->snd_cwnd < tp->snd_cwnd_clamp)
				tp->snd_cwnd++;
			tp->snd_cwnd_cnt = 0;
		} 
	} 
	else 
	{
	  /* we need to decrease the cwnd but 
	     we do not want to set it to 0!!! */
		if (tp->snd_cwnd > 1) 
		{
			tp->snd_cwnd--;
			/* set the cwnd_cnt to the max value - target */
			tp->snd_cwnd_cnt = (tp->snd_cwnd - 1) * target;
		 } 
		 else 
		 {
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
 * XXX old comment from lp authors...
 *
 * Calculate one way delay (in relative format).
 * Original implements OWD as minus of remote time difference to local time
 * difference directly. As this time difference just simply equal to RTT, when
 * the network status is stable, remote RTT will equal to local RTT, and result
 * OWD into zero.
 * It seems to be a bug and so we fixed it.
 */
static u32 tcp_ledbat_owd_calculator(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct ledbat *ledbat = inet_csk_ca(sk);
	s64 owd = 0;

	ledbat->remote_hz = tcp_ledbat_remote_hz_estimator(sk);

	if (ledbat->flag & LEDBAT_VALID_RHZ) {
		owd =
		    tp->rx_opt.rcv_tsval * (LP_RESOL / ledbat->remote_hz) -
		    tp->rx_opt.rcv_tsecr * (LP_RESOL / HZ);
		if (owd < 0)
			owd = -owd;
	}
	
	/*	owd = tp->rx_opt.rcv_tsval - tp->rx_opt.rcv_tsecr;
	owd *= LP_RESOL / HZ;

	if (owd < 0)
	owd = -owd; */

	if (owd > 0)
		ledbat->flag |= LEDBAT_VALID_OWD;
	else
		ledbat->flag &= ~ LEDBAT_VALID_OWD;

#if DEBUG_OWD_HZ
	printk(KERN_DEBUG "my_hz %u, hz %u owd %u\n", HZ, (u32)ledbat->remote_hz, (u32)owd);
#endif
	return owd;
}

static void ledbat_add_delay(struct owd_circ_buf *cb, u32 owd) {

	u8 i;

	if (cb->next == cb->first) 
	{ 
		/*buffer is empty */
		cb->buffer[cb->next] = owd;
		cb->min = cb->next;
		cb->next++;
		return;
	}

	/*set the new delay*/
	cb->buffer[cb->next] = owd;
	/* update the min if it is the case*/
	if (owd < cb->buffer[cb->min]) 
	{
		cb->min = cb->next;
	}
	/* increment the next pointer*/
	cb->next = (cb->next + 1) % cb->len;

	if (cb->next == cb->first)
	{ /* Discard the first element */ 

		if ( cb->min == cb->first ) {
			/* Discard the min, search a new one */
			cb->min = i = (cb->first + 1) % cb->len;
			while ( i != cb->next ) {
				if (cb->buffer[i] < cb->buffer[cb->min])
					cb->min = i;
				i = (i+1) % cb->len;
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
	printk ( KERN_DEBUG " added delay to noisefilter %u\n", owd);
	print_delay(&(ledbat->noise_filter), "noise_filter");
#endif
}

static void ledbat_update_base_delay(struct ledbat *ledbat, u32 owd)
{
	u32 last;
	struct owd_circ_buf *cb = &(ledbat->base_history);

	if (ledbat->base_history.next == ledbat->base_history.first) 
	{
		/* empty circular buffer */
		ledbat_add_delay(cb, owd); 
		return;
	}

	if (tcp_time_stamp - ledbat->last_rollover > 60 * HZ) {
		/* we have finished a minute */
#if DEBUG_BASE_HISTO
		printk ( KERN_DEBUG " time %u, new rollover \n",
			tcp_time_stamp);
#endif
		ledbat->last_rollover = tcp_time_stamp;
		ledbat_add_delay(cb, owd); 
	}
	else
	{
		/* update the last value and the min if it is the case */
		last = (cb->next + cb->len - 1) % cb->len;

		if ( owd < cb->buffer[last]) {
			cb->buffer[last] = owd;
			if (owd < cb->buffer[cb->min])
				cb->min = last;
		}
		
	}
#if DEBUG_BASE_HISTO
	printk ( KERN_DEBUG " added delay to base_history %s", "\n");
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
	if (!(ledbat->flag & LEDBAT_VALID_RHZ) || !(ledbat->flag & LEDBAT_VALID_OWD))
	{
		return;
	}

	ledbat_update_current_delay(ledbat, (u32) mowd);
	ledbat_update_base_delay(ledbat, (u32) mowd);
	

#if 0
	/* record the next min owd */
	if (mowd < ledbat->owd_min)
		ledbat->owd_min = mowd;

	/* always forget the max of the max
	 * we just set owd_max as one below it */
	if (mowd > ledbat->owd_max) {
		if (mowd > ledbat->owd_max_rsv) {
			if (ledbat->owd_max_rsv == 0)
				ledbat->owd_max = mowd;
			else
				ledbat->owd_max = ledbat->owd_max_rsv;
			ledbat->owd_max_rsv = mowd;
		} else
			ledbat->owd_max = mowd;
	}

	/* calc for smoothed owd */
	if (ledbat->sowd != 0) {
		mowd -= ledbat->sowd >> 3;	/* m is now error in owd est */
		ledbat->sowd += mowd;	/* owd = 7/8 owd + 1/8 new */
	} else
		ledbat->sowd = mowd << 3;	/* take the measured time be owd */
#endif
}

/**
 * tcp_ledbat_pkts_acked
 *
 * Implementation of pkts_acked.
 * Deal with active drop under Early Congestion Indication.
 * Only drop to half and 1 will be handle, because we hope to use back
 * newReno in increase case.
 * We work it out by following the idea from TCP-LP's paper directly
 */
static void tcp_ledbat_pkts_acked(struct sock *sk, u32 num_acked, s32 rtt_us)
/* ??? //ktime_t last) */
{
  /* struct tcp_sock *tp = tcp_sk(sk);
     struct ledbat *ledbat = inet_csk_ca(sk); */

	if (rtt_us > 0)
			tcp_ledbat_rtt_sample(sk, rtt_us);

	/* if (!ktime_equal(last, net_invalid_timestamp()))
	   tcp_ledbat_rtt_sample(sk,  ktime_to_us(net_timedelta(last))); */

#if 0
	/* calc inference */
	if (tcp_time_stamp > tp->rx_opt.rcv_tsecr)
		ledbat->inference = 3 * (tcp_time_stamp - tp->rx_opt.rcv_tsecr);

	/* test if within inference */
	if (ledbat->last_drop && (tcp_time_stamp - ledbat->last_drop < ledbat->inference))
		ledbat->flag |= LP_WITHIN_INF;
	else
		ledbat->flag &= ~LP_WITHIN_INF;

	/* test if within threshold */
	if (ledbat->sowd >> 3 <
	    ledbat->owd_min + 15 * (ledbat->owd_max - ledbat->owd_min) / 100)
		ledbat->flag |= LP_WITHIN_THR;
	else
		ledbat->flag &= ~LP_WITHIN_THR;

	printk( "TCP-LP: %05lo|%5lu|%5lu|%15lu|%15lu|%15lu\n", ledbat->flag,
		 tp->snd_cwnd, ledbat->remote_hz, ledbat->owd_min, ledbat->owd_max,
		 ledbat->sowd >> 3);

	if (ledbat->flag & LP_WITHIN_THR)
		return;

	/* FIXME: try to reset owd_min and owd_max here
	 * so decrease the chance the min/max is no longer suitable
	 * and will usually within threshold when whithin inference */
	ledbat->owd_min = ledbat->sowd >> 3;
	ledbat->owd_max = ledbat->sowd >> 2;
	ledbat->owd_max_rsv = ledbat->sowd >> 2;

	/* happened within inference
	 * drop snd_cwnd into 1 */
	if (ledbat->flag & LP_WITHIN_INF)
		tp->snd_cwnd = 1U;

	/* happened after inference
	 * cut snd_cwnd into half */
	else
		tp->snd_cwnd = max(tp->snd_cwnd >> 1U, 1U);

	/* record this drop time */
	ledbat->last_drop = tcp_time_stamp;
#endif
}

static u32 tcp_ledbat_min_cwnd(const struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	unsigned int res = tcp_reno_min_cwnd(sk);
	unsigned int prev = tp->snd_cwnd;

	/*	printk( KERN_DEBUG " time %u, detected loss, cwnd set from %u to %u\n", tcp_time_stamp, prev, res); */
	return res;
}

static
u32 tcp_ledbat_ssthresh(struct sock *sk)
{
	u32 res;
	
	switch (do_ss) {
		case 0:
		case 1:
		default:
			res = tcp_reno_ssthresh(sk);
			break;
		case 2:
			res = ledbat_ssthresh;
			break;
	}

	return res;
}

static struct tcp_congestion_ops tcp_ledbat = {
	.flags = TCP_CONG_RTT_STAMP,
	.init = tcp_ledbat_init,
	.release = tcp_ledbat_release,
	.ssthresh = tcp_ledbat_ssthresh,
	.cong_avoid = tcp_ledbat_cong_avoid,
	.min_cwnd = tcp_ledbat_min_cwnd,
	.pkts_acked = tcp_ledbat_pkts_acked,

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
