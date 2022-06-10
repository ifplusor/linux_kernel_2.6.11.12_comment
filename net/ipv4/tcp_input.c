/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Implementation of the Transmission Control Protocol(TCP).
 *
 * Version:	$Id: tcp_input.c,v 1.243 2002/02/01 22:01:04 davem Exp $
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Mark Evans, <evansmp@uhura.aston.ac.uk>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Florian La Roche, <flla@stud.uni-sb.de>
 *		Charles Hedrick, <hedrick@klinzhai.rutgers.edu>
 *		Linus Torvalds, <torvalds@cs.helsinki.fi>
 *		Alan Cox, <gw4pts@gw4pts.ampr.org>
 *		Matthew Dillon, <dillon@apollo.west.oic.com>
 *		Arnt Gulbrandsen, <agulbra@nvg.unit.no>
 *		Jorge Cwik, <jorge@laser.satlink.net>
 */

/*
 * Changes:
 *		Pedro Roque	:	Fast Retransmit/Recovery.
 *					Two receive queues.
 *					Retransmit queue handled by TCP.
 *					Better retransmit timer handling.
 *					New congestion avoidance.
 *					Header prediction.
 *					Variable renaming.
 *
 *		Eric		:	Fast Retransmit.
 *		Randy Scott	:	MSS option defines.
 *		Eric Schenk	:	Fixes to slow start algorithm.
 *		Eric Schenk	:	Yet another double ACK bug.
 *		Eric Schenk	:	Delayed ACK bug fixes.
 *		Eric Schenk	:	Floyd style fast retrans war avoidance.
 *		David S. Miller	:	Don't allow zero congestion window.
 *		Eric Schenk	:	Fix retransmitter so that it sends
 *					next packet on ack of previous packet.
 *		Andi Kleen	:	Moved open_request checking here
 *					and process RSTs for open_requests.
 *		Andi Kleen	:	Better prune_queue, and other fixes.
 *		Andrey Savochkin:	Fix RTT measurements in the presnce of
 *					timestamps.
 *		Andrey Savochkin:	Check sequence numbers correctly when
 *					removing SACKs due to in sequence incoming
 *					data segments.
 *		Andi Kleen:		Make sure we never ack data there is not
 *					enough room for. Also make this condition
 *					a fatal error if it might still happen.
 *		Andi Kleen:		Add tcp_measure_rcv_mss to make 
 *					connections with MSS<min(MTU,ann. MSS)
 *					work without delayed acks. 
 *		Andi Kleen:		Process packets with PSH set in the
 *					fast path.
 *		J Hadi Salim:		ECN support
 *	 	Andrei Gurtov,
 *		Pasi Sarolahti,
 *		Panu Kuhlberg:		Experimental audit of TCP (re)transmission
 *					engine. Lots of bugs are found.
 *		Pasi Sarolahti:		F-RTO for dealing with spurious RTOs
 *		Angelo Dell'Aera:	TCP Westwood+ support
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sysctl.h>
#include <net/tcp.h>
#include <net/inet_common.h>
#include <linux/ipsec.h>

/* 是否启用TCP时间戳选项 */
int sysctl_tcp_timestamps = 1;
/* 标识是否启用TCP窗口扩大因子选项 */
int sysctl_tcp_window_scaling = 1;
/* 标识是否启用选择性确认SACKS选项。对广域网来说可以打开此选项。 */
int sysctl_tcp_sack = 1;
/* 是否启用FACK拥塞避免与快速重传功能。 */
int sysctl_tcp_fack = 1;
/**
 * 在不支持SACK时，为由于连接接收到重复确认而进入快速恢复段的重复确认数阀值 
 * 在支持SACK时，在没有确定丢失包的情况下，是TCP流中可以重排序的数据段数。
 */
int sysctl_tcp_reordering = TCP_FASTRETRANS_THRESH;
/* 是否启用TCP的显式拥塞通知功能。 */
int sysctl_tcp_ecn;
/* 是否支持D-SACK */
int sysctl_tcp_dsack = 1;
/* 为应用程序缓存使用，保留max(window/2^sysctl_tcp_app_win, mss)大小的窗口。为0表示不保留。 */
int sysctl_tcp_app_win = 31;
/* 当通过调节接收窗口来进行流量控制的情况下，计算调整接收缓存和接收窗口时，用来对计算的参数进行微调 */
int sysctl_tcp_adv_win_scale = 2;

/* 标识是否使用TCP紧急指针字段标准解释。为了与已有系统兼容，关闭此功能。这将与rfc标准不兼容。 */
int sysctl_tcp_stdurg;
/* 预防在rfc1337中描述的TIME-WAIT问题，默认为0，将丢弃那些发往TIME-WAIT状态的RST段。 */
int sysctl_tcp_rfc1337;
/**
 * 系统最多能处理的孤儿套接口(不属于任何进程的套接口)数量，默认为16384个 
 * 如果超过这个数量，则不属于任何进程的连接会被立即复位。
 * 每个孤儿接口消耗64K的内存。
 */
int sysctl_tcp_max_orphans = NR_FILE;
/* 是否启用frto,经常用于无线环境，使用优化的TCP重传算法 */
int sysctl_tcp_frto;
int sysctl_tcp_nometrics_save;
int sysctl_tcp_westwood;
int sysctl_tcp_vegas_cong_avoid;

/**
 * 标识是否启动自动调节接收缓冲区大小。
 * 如果启动，则自动地调整接收缓冲区的大小，以此来进行流量控制。
 */
int sysctl_tcp_moderate_rcvbuf = 1;

/* Default values of the Vegas variables, in fixed-point representation
 * with V_PARAM_SHIFT bits to the right of the binary point.
 */
#define V_PARAM_SHIFT 1
int sysctl_tcp_vegas_alpha = 1<<V_PARAM_SHIFT;
int sysctl_tcp_vegas_beta  = 3<<V_PARAM_SHIFT;
int sysctl_tcp_vegas_gamma = 1<<V_PARAM_SHIFT;
int sysctl_tcp_bic = 1;
int sysctl_tcp_bic_fast_convergence = 1;
int sysctl_tcp_bic_low_window = 14;
int sysctl_tcp_bic_beta = 819;		/* = 819/1024 (BICTCP_BETA_SCALE) */

/* 接收到的ACK段的标志 */

/* 接收的ACK段是负荷数据的 */
#define FLAG_DATA		0x01 /* Incoming frame contained data.		*/
/* 接收的ACk段更新了发送窗口 */
#define FLAG_WIN_UPDATE		0x02 /* Incoming ACK was a window update.	*/
/* 接收的ACk段确认了新的数据 */
#define FLAG_DATA_ACKED		0x04 /* This ACK acknowledged new data.		*/
/* 此段已经重传过 */
#define FLAG_RETRANS_DATA_ACKED	0x08 /* "" "" some of which was retransmitted.	*/
/* 接收的ACk段确认了SYN段 */
#define FLAG_SYN_ACKED		0x10 /* This ACK acknowledged SYN.		*/
/* 新的SACK */
#define FLAG_DATA_SACKED	0x20 /* New SACK.				*/
/* 接收到显式拥塞通知 */
#define FLAG_ECE		0x40 /* ECE in this ACK				*/
/* 由SACK标识的数据已丢失 */
#define FLAG_DATA_LOST		0x80 /* SACK detected data lossage.		*/
/* 在慢速路径中处理的 */
#define FLAG_SLOWPATH		0x100 /* Do not skip RFC checks for window update.*/

#define FLAG_ACKED		(FLAG_DATA_ACKED|FLAG_SYN_ACKED)
#define FLAG_NOT_DUP		(FLAG_DATA|FLAG_WIN_UPDATE|FLAG_ACKED)
#define FLAG_CA_ALERT		(FLAG_DATA_SACKED|FLAG_ECE)
#define FLAG_FORWARD_PROGRESS	(FLAG_ACKED|FLAG_DATA_SACKED)

#define IsReno(tp) ((tp)->rx_opt.sack_ok == 0)
#define IsFack(tp) ((tp)->rx_opt.sack_ok & 2)
#define IsDSack(tp) ((tp)->rx_opt.sack_ok & 4)

#define TCP_REMNANT (TCP_FLAG_FIN|TCP_FLAG_URG|TCP_FLAG_SYN|TCP_FLAG_PSH)

/* Adapt the MSS value used to make delayed ack decision to the 
 * real world.
 */ 
/* 接收到报文后，估算发送方的MSS */
static inline void tcp_measure_rcv_mss(struct tcp_sock *tp,
				       struct sk_buff *skb)
{
	unsigned int len, lss;

	lss = tp->ack.last_seg_size; 
	tp->ack.last_seg_size = 0; 

	/* skb->len may jitter because of SACKs, even if peer
	 * sends good full-sized frames.
	 */
	len = skb->len;
	if (len >= tp->ack.rcv_mss) {/* 接收到的段报文大于发送方MSS，则更新MSS */
		tp->ack.rcv_mss = len;
	} else {
		/* Otherwise, we make more careful check taking into account,
		 * that SACKs block is variable.
		 *
		 * "len" is invariant segment length, including TCP header.
		 */
		len += skb->data - skb->h.raw;/* TCP报文长度 */
		if (len >= TCP_MIN_RCVMSS + sizeof(struct tcphdr) ||/* 大于536 */
		    /* If PSH is not set, packet should be
		     * full sized, provided peer TCP is not badly broken.
		     * This observation (if it is correct 8)) allows
		     * to handle super-low mtu links fairly.
		     */
		    (len >= TCP_MIN_MSS + sizeof(struct tcphdr) &&/* 大于最小TCP段长度88 */
		     !(tcp_flag_word(skb->h.th)&TCP_REMNANT))) {/* 没有PSH标志，说明是全尺寸段 */
			/* Subtract also invariant (if peer is RFC compliant),
			 * tcp header plus fixed timestamp option length.
			 * Resulting "len" is MSS free of SACK jitter.
			 */
			len -= tp->tcp_header_len;
			tp->ack.last_seg_size = len;
			if (len == lss) {/* 与上次接收到的段相同，说明此MSS值是可信的，更新MSS */
				tp->ack.rcv_mss = len;
				return;
			}
		}
		/* 其他情况下则认为接收到小包，设置TCP_ACK_PUSHED标志。新版本增加了TCP_ACK_PUSHED2标志 */
		tp->ack.pending |= TCP_ACK_PUSHED;
	}
}

static void tcp_incr_quickack(struct tcp_sock *tp)
{
	unsigned quickacks = tp->rcv_wnd/(2*tp->ack.rcv_mss);

	if (quickacks==0)
		quickacks=2;
	if (quickacks > tp->ack.quick)
		tp->ack.quick = min(quickacks, TCP_MAX_QUICKACKS);
}

/* 进入快速确认模式 */
void tcp_enter_quickack_mode(struct tcp_sock *tp)
{
	/* 根据接收窗口和MSS计算快速确认段数，超过后进入慢速确认模式 */
	tcp_incr_quickack(tp);
	tp->ack.pingpong = 0;/* 快速确认模式 */
	/* 延迟40毫秒就必须发送确认 */
	tp->ack.ato = TCP_ATO_MIN;
}

/* Send ACKs quickly, if "quick" count is not exhausted
 * and the session is not interactive.
 */
/* 当前是否是快速确认模式 */
static __inline__ int tcp_in_quickack_mode(struct tcp_sock *tp)
{
	return (tp->ack.quick && !tp->ack.pingpong);
}

/* Buffer size and advertised window tuning.
 *
 * 1. Tuning sk->sk_sndbuf, when connection enters established state.
 */

static void tcp_fixup_sndbuf(struct sock *sk)
{
	int sndmem = tcp_sk(sk)->rx_opt.mss_clamp + MAX_TCP_HEADER + 16 +
		     sizeof(struct sk_buff);

	if (sk->sk_sndbuf < 3 * sndmem)
		sk->sk_sndbuf = min(3 * sndmem, sysctl_tcp_wmem[2]);
}

/* 2. Tuning advertised window (window_clamp, rcv_ssthresh)
 *
 * All tcp_full_space() is split to two parts: "network" buffer, allocated
 * forward and advertised in receiver window (tp->rcv_wnd) and
 * "application buffer", required to isolate scheduling/application
 * latencies from network.
 * window_clamp is maximal advertised window. It can be less than
 * tcp_full_space(), in this case tcp_full_space() - window_clamp
 * is reserved for "application" buffer. The less window_clamp is
 * the smoother our behaviour from viewpoint of network, but the lower
 * throughput and the higher sensitivity of the connection to losses. 8)
 *
 * rcv_ssthresh is more strict window_clamp used at "slow start"
 * phase to predict further behaviour of this connection.
 * It is used for two goals:
 * - to enforce header prediction at sender, even when application
 *   requires some significant "application buffer". It is check #1.
 * - to prevent pruning of receive queue because of misprediction
 *   of receiver window. Check #2.
 *
 * The scheme does not work when sender sends good segments opening
 * window and then starts to feed us spagetti. But it should work
 * in common situations. Otherwise, we have to rely on queue collapsing.
 */

/* Slow part of check#2. */
static int __tcp_grow_window(struct sock *sk, struct tcp_sock *tp,
			     struct sk_buff *skb)
{
	/* Optimize this! */
	int truesize = tcp_win_from_space(skb->truesize)/2;
	int window = tcp_full_space(sk)/2;

	while (tp->rcv_ssthresh <= window) {
		if (truesize <= skb->len)
			return 2*tp->ack.rcv_mss;

		truesize >>= 1;
		window >>= 1;
	}
	return 0;
}

/* 当接收到报文后，增加接收窗口大小阀值 */
static inline void tcp_grow_window(struct sock *sk, struct tcp_sock *tp,
				   struct sk_buff *skb)
{
	/* Check #1 */
	if (tp->rcv_ssthresh < tp->window_clamp &&/* 当前接收窗口大小阀值小于滑动窗口最大值 */
	    (int)tp->rcv_ssthresh < tcp_space(sk) &&/* 也小于TCP可用接收空间 */
	    !tcp_memory_pressure) {/* TCP缓存未告警 */
		int incr;

		/* Check #2. Increase window, if skb with such overhead
		 * will fit to rcvbuf in future.
		 */
		/* 递增当前接收窗口大小的阀值 */
		if (tcp_win_from_space(skb->truesize) <= skb->len)
			incr = 2*tp->advmss;
		else
			incr = __tcp_grow_window(sk, tp, skb);

		if (incr) {
			tp->rcv_ssthresh = min(tp->rcv_ssthresh + incr, tp->window_clamp);
			tp->ack.quick |= 1;
		}
	}
}

/* 3. Tuning rcvbuf, when connection enters established state. */

static void tcp_fixup_rcvbuf(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int rcvmem = tp->advmss + MAX_TCP_HEADER + 16 + sizeof(struct sk_buff);

	/* Try to select rcvbuf so that 4 mss-sized segments
	 * will fit to window and correspoding skbs will fit to our rcvbuf.
	 * (was 3; 4 is minimum to allow fast retransmit to work.)
	 */
	while (tcp_win_from_space(rcvmem) < tp->advmss)
		rcvmem += 128;
	if (sk->sk_rcvbuf < 4 * rcvmem)
		sk->sk_rcvbuf = min(4 * rcvmem, sysctl_tcp_rmem[2]);
}

/* 4. Try to fixup all. It is made iimediately after connection enters
 *    established state.
 */
static void tcp_init_buffer_space(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int maxwin;

	if (!(sk->sk_userlocks & SOCK_RCVBUF_LOCK))
		tcp_fixup_rcvbuf(sk);
	if (!(sk->sk_userlocks & SOCK_SNDBUF_LOCK))
		tcp_fixup_sndbuf(sk);

	tp->rcvq_space.space = tp->rcv_wnd;

	maxwin = tcp_full_space(sk);

	if (tp->window_clamp >= maxwin) {
		tp->window_clamp = maxwin;

		if (sysctl_tcp_app_win && maxwin > 4 * tp->advmss)
			tp->window_clamp = max(maxwin -
					       (maxwin >> sysctl_tcp_app_win),
					       4 * tp->advmss);
	}

	/* Force reservation of one segment. */
	if (sysctl_tcp_app_win &&
	    tp->window_clamp > 2 * tp->advmss &&
	    tp->window_clamp + tp->advmss > maxwin)
		tp->window_clamp = max(2 * tp->advmss, maxwin - tp->advmss);

	tp->rcv_ssthresh = min(tp->rcv_ssthresh, tp->window_clamp);
	tp->snd_cwnd_stamp = tcp_time_stamp;
}

static void init_bictcp(struct tcp_sock *tp)
{
	tp->bictcp.cnt = 0;

	tp->bictcp.last_max_cwnd = 0;
	tp->bictcp.last_cwnd = 0;
	tp->bictcp.last_stamp = 0;
}

/* 5. Recalculate window clamp after socket hit its memory bounds. */
static void tcp_clamp_window(struct sock *sk, struct tcp_sock *tp)
{
	struct sk_buff *skb;
	unsigned int app_win = tp->rcv_nxt - tp->copied_seq;
	int ofo_win = 0;

	tp->ack.quick = 0;

	skb_queue_walk(&tp->out_of_order_queue, skb) {
		ofo_win += skb->len;
	}

	/* If overcommit is due to out of order segments,
	 * do not clamp window. Try to expand rcvbuf instead.
	 */
	if (ofo_win) {
		if (sk->sk_rcvbuf < sysctl_tcp_rmem[2] &&
		    !(sk->sk_userlocks & SOCK_RCVBUF_LOCK) &&
		    !tcp_memory_pressure &&
		    atomic_read(&tcp_memory_allocated) < sysctl_tcp_mem[0])
			sk->sk_rcvbuf = min(atomic_read(&sk->sk_rmem_alloc),
					    sysctl_tcp_rmem[2]);
	}
	if (atomic_read(&sk->sk_rmem_alloc) > sk->sk_rcvbuf) {
		app_win += ofo_win;
		if (atomic_read(&sk->sk_rmem_alloc) >= 2 * sk->sk_rcvbuf)
			app_win >>= 1;
		if (app_win > tp->ack.rcv_mss)
			app_win -= tp->ack.rcv_mss;
		app_win = max(app_win, 2U*tp->advmss);

		if (!ofo_win)
			tp->window_clamp = min(tp->window_clamp, app_win);
		tp->rcv_ssthresh = min(tp->window_clamp, 2U*tp->advmss);
	}
}

/* Receiver "autotuning" code.
 *
 * The algorithm for RTT estimation w/o timestamps is based on
 * Dynamic Right-Sizing (DRS) by Wu Feng and Mike Fisk of LANL.
 * <http://www.lanl.gov/radiant/website/pubs/drs/lacsi2001.ps>
 *
 * More detail on this code can be found at
 * <http://www.psc.edu/~jheffner/senior_thesis.ps>,
 * though this reference is out of date.  A new paper
 * is pending.
 */
/**
 * 更新RTT
 */
static void tcp_rcv_rtt_update(struct tcp_sock *tp, u32 sample, int win_dep)
{
	u32 new_sample = tp->rcv_rtt_est.rtt;/* new_sample为已经得到RTT */
	long m = sample;/* sample是本次采样得到的RTT */

	if (m == 0)/* 采样值不能为0，至少为1个tick */
		m = 1;

	if (new_sample != 0) {
		/* If we sample in larger samples in the non-timestamp
		 * case, we could grossly overestimate the RTT especially
		 * with chatty applications or bulk transfer apps which
		 * are stalled on filesystem I/O.
		 *
		 * Also, since we are only going for a minimum in the
		 * non-timestamp case, we do not smoothe things out
		 * else with timestamps disabled convergance takes too
		 * long.
		 */
		if (!win_dep) {/* 进行RTT微调，公式为rtt=rtt+(sample-rtt)/8 */
			m -= (new_sample >> 3);
			new_sample += m;
		} else if (m < new_sample)/* 不进行微调，如果RTT小于原来的值，则采用新值 */
			new_sample = m << 3;
	} else {/* 如果是第一次采样，则直接保存本次采样结果 */
		/* No previous mesaure. */
		new_sample = m << 3;
	}

	if (tp->rcv_rtt_est.rtt != new_sample)/* 更新RTT */
		tp->rcv_rtt_est.rtt = new_sample;
}

/* 在没有时间戳选项的情况下，或者当数据流量非常小的情况下，使用此函数采样RTT */
static inline void tcp_rcv_rtt_measure(struct tcp_sock *tp)
{
	if (tp->rcv_rtt_est.time == 0)/* 第一次接收到数据或第一次采用本方法，不能采样RTT */
		goto new_measure;
	if (before(tp->rcv_nxt, tp->rcv_rtt_est.seq))/* 检查是否已经到达进行RTT采样的时间点，即从上次采样后一个接收窗口数量的数据 */
		return;
	/* 采样RTT数据，第二个参数为RTT的采样，参数1表示不对RTT采样进行微调 */
	tcp_rcv_rtt_update(tp,
			   jiffies - tp->rcv_rtt_est.time,
			   1);

new_measure:
	/* 记录本次采样时间和下次采样的时间点 */
	tp->rcv_rtt_est.seq = tp->rcv_nxt + tp->rcv_wnd;
	tp->rcv_rtt_est.time = tcp_time_stamp;
}

/* 在有时间戳的情况下采样RTT */
static inline void tcp_rcv_rtt_measure_ts(struct tcp_sock *tp, struct sk_buff *skb)
{
	if (tp->rx_opt.rcv_tsecr &&/* TCP段中有时间戳回显 */
	    (TCP_SKB_CB(skb)->end_seq -
	     TCP_SKB_CB(skb)->seq >= tp->ack.rcv_mss))/* 报文大小大于等于接收方的MSS，表示是一个全数据段 */
		/* 更新采样数据，第二个参数是当前时间减去报文回显时间，第三个参数0表示对RTT进行微调 */
		tcp_rcv_rtt_update(tp, tcp_time_stamp - tp->rx_opt.rcv_tsecr, 0);
}

/*
 * This function should be called every time data is copied to user space.
 * It calculates the appropriate TCP receive buffer space.
 */
/* 当数据从接收缓存复制到用户空间后，调用此函数调整接收缓存的大小 */
void tcp_rcv_space_adjust(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int time;
	int space;
	
	if (tp->rcvq_space.time == 0)/* 第一次调整，即接收到第一个用户数据段，不必调整接收缓存，仅仅记录复制到用户空间的TCP序号和最后一次调整的时间 */
		goto new_measure;
	
	time = tcp_time_stamp - tp->rcvq_space.time;
	if (time < (tp->rcv_rtt_est.rtt >> 3) ||/* 距离上次调整时间小于RTT/8，或者还没有计算出RTT，则返回 */
	    tp->rcv_rtt_est.rtt == 0)
		return;

	/* 2倍往返时间内接收方应用程序接收的数据量 */
	space = 2 * (tp->copied_seq - tp->rcvq_space.seq);

	/* 发送方一个往返时间内发送的数据量 */
	space = max(tp->rcvq_space.space, space);

	if (tp->rcvq_space.space != space) {/* 需要调整接收窗口 */
		int rcvmem;

		tp->rcvq_space.space = space;

		if (sysctl_tcp_moderate_rcvbuf) {/* 允许自动调节接收窗口大小 */
			int new_clamp = space;

			/* Receive space grows, normalize in order to
			 * take into account packet headers and sk_buff
			 * structure overhead.
			 */
			space /= tp->advmss;
			if (!space)
				space = 1;
			rcvmem = (tp->advmss + MAX_TCP_HEADER +
				  16 + sizeof(struct sk_buff));
			while (tcp_win_from_space(rcvmem) < tp->advmss)
				rcvmem += 128;
			space *= rcvmem;
			space = min(space, sysctl_tcp_rmem[2]);
			if (space > sk->sk_rcvbuf) {
				sk->sk_rcvbuf = space;

				/* Make the window clamp follow along.  */
				tp->window_clamp = new_clamp;
			}
		}
	}
	
new_measure:
	tp->rcvq_space.seq = tp->copied_seq;
	tp->rcvq_space.time = tcp_time_stamp;
}

/* There is something which you must keep in mind when you analyze the
 * behavior of the tp->ato delayed ack timeout interval.  When a
 * connection starts up, we want to ack as quickly as possible.  The
 * problem is that "good" TCP's do slow start at the beginning of data
 * transmission.  The means that until we send the first few ACK's the
 * sender will sit on his end and only queue most of his data, because
 * he can only send snd_cwnd unacked packets at any given time.  For
 * each ACK we send, he increments snd_cwnd and transmits more of his
 * queue.  -DaveM
 */
/**
 * 在接收到数据后调用，用于处理接收到数据之后应该触发的一些事件
 * 如设置发送确认状态，估算对端MSS，计算接收方RTT，确定是进入快速确认还是慢速确认状态，以及更新最近一次接收到的数据包时间
 */
static void tcp_event_data_recv(struct sock *sk, struct tcp_sock *tp, struct sk_buff *skb)
{
	u32 now;

	/* 接收到新的段，那么就应当发送ACK，设置ACk发送标志 */
	tcp_schedule_ack(tp);

	/* 估算更新对方MSS */
	tcp_measure_rcv_mss(tp, skb);

	tcp_rcv_rtt_measure(tp);
	
	now = tcp_time_stamp;

	if (!tp->ack.ato) {/* 没有设置延时确认的时间，则进入快速确认模式 */
		/* The _first_ data packet received, initialize
		 * delayed ACK engine.
		 */
		tcp_incr_quickack(tp);
		tp->ack.ato = TCP_ATO_MIN;
	} else {/* 否则根据本次与上次接收到的时间间隔，重新设置延迟ACK的超时时间或者进入快速确认模式 */
		int m = now - tp->ack.lrcvtime;

		if (m <= TCP_ATO_MIN/2) {
			/* The fastest case is the first. */
			tp->ack.ato = (tp->ack.ato>>1) + TCP_ATO_MIN/2;
		} else if (m < tp->ack.ato) {
			tp->ack.ato = (tp->ack.ato>>1) + m;
			if (tp->ack.ato > tp->rto)
				tp->ack.ato = tp->rto;
		} else if (m > tp->rto) {
			/* Too long gap. Apparently sender falled to
			 * restart window, so that we send ACKs quickly.
			 */
			tcp_incr_quickack(tp);
			sk_stream_mem_reclaim(sk);
		}
	}
	/* 更新最近接收数据包的时间 */
	tp->ack.lrcvtime = now;

	/* 在支持显式拥塞通知的情况下，确定接收到的段是否经历了拥塞 */
	TCP_ECN_check_ce(tp, skb);

	if (skb->len >= 128)/* 增加当前接收窗口大小的阀值 */
		tcp_grow_window(sk, tp, skb);
}

/* When starting a new connection, pin down the current choice of 
 * congestion algorithm.
 */
void tcp_ca_init(struct tcp_sock *tp)
{
	if (sysctl_tcp_westwood) 
		tp->adv_cong = TCP_WESTWOOD;
	else if (sysctl_tcp_bic)
		tp->adv_cong = TCP_BIC;
	else if (sysctl_tcp_vegas_cong_avoid) {
		tp->adv_cong = TCP_VEGAS;
		tp->vegas.baseRTT = 0x7fffffff;
		tcp_vegas_enable(tp);
	} 
}

/* Do RTT sampling needed for Vegas.
 * Basically we:
 *   o min-filter RTT samples from within an RTT to get the current
 *     propagation delay + queuing delay (we are min-filtering to try to
 *     avoid the effects of delayed ACKs)
 *   o min-filter RTT samples from a much longer window (forever for now)
 *     to find the propagation delay (baseRTT)
 */
static inline void vegas_rtt_calc(struct tcp_sock *tp, __u32 rtt)
{
	__u32 vrtt = rtt + 1; /* Never allow zero rtt or baseRTT */

	/* Filter to find propagation delay: */
	if (vrtt < tp->vegas.baseRTT) 
		tp->vegas.baseRTT = vrtt;

	/* Find the min RTT during the last RTT to find
	 * the current prop. delay + queuing delay:
	 */
	tp->vegas.minRTT = min(tp->vegas.minRTT, vrtt);
	tp->vegas.cntRTT++;
}

/* Called to compute a smoothed rtt estimate. The data fed to this
 * routine either comes from timestamps, or from segments that were
 * known _not_ to have been retransmitted [see Karn/Partridge
 * Proceedings SIGCOMM 87]. The algorithm is from the SIGCOMM 88
 * piece by Van Jacobson.
 * NOTE: the next three routines used to be one big routine.
 * To save cycles in the RFC 1323 implementation it was better to break
 * it up into three procedures. -- erics
 */
/* 估算RTT，然后再设置重传超时时间 */
static void tcp_rtt_estimator(struct tcp_sock *tp, __u32 mrtt)
{
	long m = mrtt; /* RTT */

	if (tcp_vegas_enabled(tp))
		vegas_rtt_calc(tp, mrtt);

	/*	The following amusing code comes from Jacobson's
	 *	article in SIGCOMM '88.  Note that rtt and mdev
	 *	are scaled versions of rtt and mean deviation.
	 *	This is designed to be as fast as possible 
	 *	m stands for "measurement".
	 *
	 *	On a 1990 paper the rto value is changed to:
	 *	RTO = rtt + 4 * mdev
	 *
	 * Funny. This algorithm seems to be very broken.
	 * These formulae increase RTO, when it should be decreased, increase
	 * too slowly, when it should be incresed fastly, decrease too fastly
	 * etc. I guess in BSD RTO takes ONE value, so that it is absolutely
	 * does not matter how to _calculate_ it. Seems, it was trap
	 * that VJ failed to avoid. 8)
	 */
	if(m == 0)/* 估算RTT的采样不能为0 */
		m = 1;
	if (tp->srtt != 0) {/* 通过采样值与现有RTT估算新的RTT */
		/* 根据RFC2988的算法，SRTT=(1-1/8)*SRTT+1/8*RTT来获得RTT的平滑值 */
		m -= (tp->srtt >> 3);	/* m is now error in rtt est */
		tp->srtt += m;		/* rtt = 7/8 rtt + 1/8 new */
		/* 按mdev=3/4+1/4(|SRTT-RTT采样|)来获取mdev */
		if (m < 0) {
			m = -m;		/* m is now abs(error) */
			m -= (tp->mdev >> 2);   /* similar update on mdev */
			/* This is similar to one of Eifel findings.
			 * Eifel blocks mdev updates when rtt decreases.
			 * This solution is a bit different: we use finer gain
			 * for mdev in this case (alpha*beta).
			 * Like Eifel it also prevents growth of rto,
			 * but also it limits too fast rto decreases,
			 * happening in pure Eifel.
			 */
			if (m > 0)
				m >>= 3;
		} else {
			m -= (tp->mdev >> 2);   /* similar update on mdev */
		}
		tp->mdev += m;	    	/* mdev = 3/4 mdev + 1/4 new */
		/* 更新RTT抖动的最大范围和平滑的RTT平均偏差 */
		if (tp->mdev > tp->mdev_max) {
			tp->mdev_max = tp->mdev;
			if (tp->mdev_max > tp->rttvar)
				tp->rttvar = tp->mdev_max;
		}
		/* 检测是否应该复位mdev_max，即上次复位后接收方是否已经接收完一个接收窗口的值 */
		if (after(tp->snd_una, tp->rtt_seq)) {
			if (tp->mdev_max < tp->rttvar)
				tp->rttvar -= (tp->rttvar-tp->mdev_max)>>2;
			tp->rtt_seq = tp->snd_nxt;
			tp->mdev_max = TCP_RTO_MIN;
		}
	} else {/* 第一个RTT的测量 */
		/* no previous measure. */
		tp->srtt = m<<3;	/* take the measured time to be rtt */
		tp->mdev = m<<1;	/* make sure rto = 3*rtt */
		tp->mdev_max = tp->rttvar = max(tp->mdev, TCP_RTO_MIN);
		tp->rtt_seq = tp->snd_nxt;
	}

	tcp_westwood_update_rtt(tp, tp->srtt >> 3);
}

/* Calculate rto without backoff.  This is the second half of Van Jacobson's
 * routine referred to above.
 */
/* 根据最近一次得到的RTT来计算重传超时时间 */
static inline void tcp_set_rto(struct tcp_sock *tp)
{
	/* Old crap is replaced with new one. 8)
	 *
	 * More seriously:
	 * 1. If rtt variance happened to be less 50msec, it is hallucination.
	 *    It cannot be less due to utterly erratic ACK generation made
	 *    at least by solaris and freebsd. "Erratic ACKs" has _nothing_
	 *    to do with delayed acks, because at cwnd>2 true delack timeout
	 *    is invisible. Actually, Linux-2.4 also generates erratic
	 *    ACKs in some curcumstances.
	 */
	tp->rto = (tp->srtt >> 3) + tp->rttvar;

	/* 2. Fixups made earlier cannot be right.
	 *    If we do not estimate RTO correctly without them,
	 *    all the algo is pure shit and should be replaced
	 *    with correct one. It is exaclty, which we pretend to do.
	 */
}

/* NOTE: clamping at TCP_RTO_MIN is not required, current algo
 * guarantees that rto is higher.
 */
static inline void tcp_bound_rto(struct tcp_sock *tp)
{
	if (tp->rto > TCP_RTO_MAX)
		tp->rto = TCP_RTO_MAX;
}

/* Save metrics learned by this TCP session.
   This function is called only, when TCP finishes successfully
   i.e. when it enters TIME-WAIT or goes from LAST-ACK to CLOSE.
 */
void tcp_update_metrics(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct dst_entry *dst = __sk_dst_get(sk);

	if (sysctl_tcp_nometrics_save)
		return;

	dst_confirm(dst);

	if (dst && (dst->flags&DST_HOST)) {
		int m;

		if (tp->backoff || !tp->srtt) {
			/* This session failed to estimate rtt. Why?
			 * Probably, no packets returned in time.
			 * Reset our results.
			 */
			if (!(dst_metric_locked(dst, RTAX_RTT)))
				dst->metrics[RTAX_RTT-1] = 0;
			return;
		}

		m = dst_metric(dst, RTAX_RTT) - tp->srtt;

		/* If newly calculated rtt larger than stored one,
		 * store new one. Otherwise, use EWMA. Remember,
		 * rtt overestimation is always better than underestimation.
		 */
		if (!(dst_metric_locked(dst, RTAX_RTT))) {
			if (m <= 0)
				dst->metrics[RTAX_RTT-1] = tp->srtt;
			else
				dst->metrics[RTAX_RTT-1] -= (m>>3);
		}

		if (!(dst_metric_locked(dst, RTAX_RTTVAR))) {
			if (m < 0)
				m = -m;

			/* Scale deviation to rttvar fixed point */
			m >>= 1;
			if (m < tp->mdev)
				m = tp->mdev;

			if (m >= dst_metric(dst, RTAX_RTTVAR))
				dst->metrics[RTAX_RTTVAR-1] = m;
			else
				dst->metrics[RTAX_RTTVAR-1] -=
					(dst->metrics[RTAX_RTTVAR-1] - m)>>2;
		}

		if (tp->snd_ssthresh >= 0xFFFF) {
			/* Slow start still did not finish. */
			if (dst_metric(dst, RTAX_SSTHRESH) &&
			    !dst_metric_locked(dst, RTAX_SSTHRESH) &&
			    (tp->snd_cwnd >> 1) > dst_metric(dst, RTAX_SSTHRESH))
				dst->metrics[RTAX_SSTHRESH-1] = tp->snd_cwnd >> 1;
			if (!dst_metric_locked(dst, RTAX_CWND) &&
			    tp->snd_cwnd > dst_metric(dst, RTAX_CWND))
				dst->metrics[RTAX_CWND-1] = tp->snd_cwnd;
		} else if (tp->snd_cwnd > tp->snd_ssthresh &&
			   tp->ca_state == TCP_CA_Open) {
			/* Cong. avoidance phase, cwnd is reliable. */
			if (!dst_metric_locked(dst, RTAX_SSTHRESH))
				dst->metrics[RTAX_SSTHRESH-1] =
					max(tp->snd_cwnd >> 1, tp->snd_ssthresh);
			if (!dst_metric_locked(dst, RTAX_CWND))
				dst->metrics[RTAX_CWND-1] = (dst->metrics[RTAX_CWND-1] + tp->snd_cwnd) >> 1;
		} else {
			/* Else slow start did not finish, cwnd is non-sense,
			   ssthresh may be also invalid.
			 */
			if (!dst_metric_locked(dst, RTAX_CWND))
				dst->metrics[RTAX_CWND-1] = (dst->metrics[RTAX_CWND-1] + tp->snd_ssthresh) >> 1;
			if (dst->metrics[RTAX_SSTHRESH-1] &&
			    !dst_metric_locked(dst, RTAX_SSTHRESH) &&
			    tp->snd_ssthresh > dst->metrics[RTAX_SSTHRESH-1])
				dst->metrics[RTAX_SSTHRESH-1] = tp->snd_ssthresh;
		}

		if (!dst_metric_locked(dst, RTAX_REORDERING)) {
			if (dst->metrics[RTAX_REORDERING-1] < tp->reordering &&
			    tp->reordering != sysctl_tcp_reordering)
				dst->metrics[RTAX_REORDERING-1] = tp->reordering;
		}
	}
}

/* Numbers are taken from RFC2414.  */
__u32 tcp_init_cwnd(struct tcp_sock *tp, struct dst_entry *dst)
{
	__u32 cwnd = (dst ? dst_metric(dst, RTAX_INITCWND) : 0);

	if (!cwnd) {
		if (tp->mss_cache_std > 1460)
			cwnd = 2;
		else
			cwnd = (tp->mss_cache_std > 1095) ? 3 : 4;
	}
	return min_t(__u32, cwnd, tp->snd_cwnd_clamp);
}

/* Initialize metrics on socket. */

static void tcp_init_metrics(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct dst_entry *dst = __sk_dst_get(sk);

	if (dst == NULL)
		goto reset;

	dst_confirm(dst);

	if (dst_metric_locked(dst, RTAX_CWND))
		tp->snd_cwnd_clamp = dst_metric(dst, RTAX_CWND);
	if (dst_metric(dst, RTAX_SSTHRESH)) {
		tp->snd_ssthresh = dst_metric(dst, RTAX_SSTHRESH);
		if (tp->snd_ssthresh > tp->snd_cwnd_clamp)
			tp->snd_ssthresh = tp->snd_cwnd_clamp;
	}
	if (dst_metric(dst, RTAX_REORDERING) &&
	    tp->reordering != dst_metric(dst, RTAX_REORDERING)) {
		tp->rx_opt.sack_ok &= ~2;
		tp->reordering = dst_metric(dst, RTAX_REORDERING);
	}

	if (dst_metric(dst, RTAX_RTT) == 0)
		goto reset;

	if (!tp->srtt && dst_metric(dst, RTAX_RTT) < (TCP_TIMEOUT_INIT << 3))
		goto reset;

	/* Initial rtt is determined from SYN,SYN-ACK.
	 * The segment is small and rtt may appear much
	 * less than real one. Use per-dst memory
	 * to make it more realistic.
	 *
	 * A bit of theory. RTT is time passed after "normal" sized packet
	 * is sent until it is ACKed. In normal curcumstances sending small
	 * packets force peer to delay ACKs and calculation is correct too.
	 * The algorithm is adaptive and, provided we follow specs, it
	 * NEVER underestimate RTT. BUT! If peer tries to make some clever
	 * tricks sort of "quick acks" for time long enough to decrease RTT
	 * to low value, and then abruptly stops to do it and starts to delay
	 * ACKs, wait for troubles.
	 */
	if (dst_metric(dst, RTAX_RTT) > tp->srtt) {
		tp->srtt = dst_metric(dst, RTAX_RTT);
		tp->rtt_seq = tp->snd_nxt;
	}
	if (dst_metric(dst, RTAX_RTTVAR) > tp->mdev) {
		tp->mdev = dst_metric(dst, RTAX_RTTVAR);
		tp->mdev_max = tp->rttvar = max(tp->mdev, TCP_RTO_MIN);
	}
	tcp_set_rto(tp);
	tcp_bound_rto(tp);
	if (tp->rto < TCP_TIMEOUT_INIT && !tp->rx_opt.saw_tstamp)
		goto reset;
	tp->snd_cwnd = tcp_init_cwnd(tp, dst);
	tp->snd_cwnd_stamp = tcp_time_stamp;
	return;

reset:
	/* Play conservative. If timestamps are not
	 * supported, TCP will fail to recalculate correct
	 * rtt, if initial rto is too small. FORGET ALL AND RESET!
	 */
	if (!tp->rx_opt.saw_tstamp && tp->srtt) {
		tp->srtt = 0;
		tp->mdev = tp->mdev_max = tp->rttvar = TCP_TIMEOUT_INIT;
		tp->rto = TCP_TIMEOUT_INIT;
	}
}

static void tcp_update_reordering(struct tcp_sock *tp, int metric, int ts)
{
	if (metric > tp->reordering) {
		tp->reordering = min(TCP_MAX_REORDERING, metric);

		/* This exciting event is worth to be remembered. 8) */
		if (ts)
			NET_INC_STATS_BH(LINUX_MIB_TCPTSREORDER);
		else if (IsReno(tp))
			NET_INC_STATS_BH(LINUX_MIB_TCPRENOREORDER);
		else if (IsFack(tp))
			NET_INC_STATS_BH(LINUX_MIB_TCPFACKREORDER);
		else
			NET_INC_STATS_BH(LINUX_MIB_TCPSACKREORDER);
#if FASTRETRANS_DEBUG > 1
		printk(KERN_DEBUG "Disorder%d %d %u f%u s%u rr%d\n",
		       tp->rx_opt.sack_ok, tp->ca_state,
		       tp->reordering,
		       tp->fackets_out,
		       tp->sacked_out,
		       tp->undo_marker ? tp->undo_retrans : 0);
#endif
		/* Disable FACK yet. */
		tp->rx_opt.sack_ok &= ~2;
	}
}

/* This procedure tags the retransmission queue when SACKs arrive.
 *
 * We have three tag bits: SACKED(S), RETRANS(R) and LOST(L).
 * Packets in queue with these bits set are counted in variables
 * sacked_out, retrans_out and lost_out, correspondingly.
 *
 * Valid combinations are:
 * Tag  InFlight	Description
 * 0	1		- orig segment is in flight.
 * S	0		- nothing flies, orig reached receiver.
 * L	0		- nothing flies, orig lost by net.
 * R	2		- both orig and retransmit are in flight.
 * L|R	1		- orig is lost, retransmit is in flight.
 * S|R  1		- orig reached receiver, retrans is still in flight.
 * (L|S|R is logically valid, it could occur when L|R is sacked,
 *  but it is equivalent to plain S and code short-curcuits it to S.
 *  L|S is logically invalid, it would mean -1 packet in flight 8))
 *
 * These 6 states form finite state machine, controlled by the following events:
 * 1. New ACK (+SACK) arrives. (tcp_sacktag_write_queue())
 * 2. Retransmission. (tcp_retransmit_skb(), tcp_xmit_retransmit_queue())
 * 3. Loss detection event of one of three flavors:
 *	A. Scoreboard estimator decided the packet is lost.
 *	   A'. Reno "three dupacks" marks head of queue lost.
 *	   A''. Its FACK modfication, head until snd.fack is lost.
 *	B. SACK arrives sacking data transmitted after never retransmitted
 *	   hole was sent out.
 *	C. SACK arrives sacking SND.NXT at the moment, when the
 *	   segment was retransmitted.
 * 4. D-SACK added new rule: D-SACK changes any tag to S.
 *
 * It is pleasant to note, that state diagram turns out to be commutative,
 * so that we are allowed not to be bothered by order of our actions,
 * when multiple events arrive simultaneously. (see the function below).
 *
 * Reordering detection.
 * --------------------
 * Reordering metric is maximal distance, which a packet can be displaced
 * in packet stream. With SACKs we can estimate it:
 *
 * 1. SACK fills old hole and the corresponding segment was not
 *    ever retransmitted -> reordering. Alas, we cannot use it
 *    when segment was retransmitted.
 * 2. The last flaw is solved with D-SACK. D-SACK arrives
 *    for retransmitted and already SACKed segment -> reordering..
 * Both of these heuristics are not used in Loss state, when we cannot
 * account for retransmits accurately.
 */
/* 当接收到ACK后，根据SACK选项标记重传队列中SKB的记分处于状态 */
static int
tcp_sacktag_write_queue(struct sock *sk, struct sk_buff *ack_skb, u32 prior_snd_una)
{
	struct tcp_sock *tp = tcp_sk(sk);
	/* 得到TCP选项中SACKED选项的偏移，以及SACK选项的个数 */
	unsigned char *ptr = ack_skb->h.raw + TCP_SKB_CB(ack_skb)->sacked;
	struct tcp_sack_block *sp = (struct tcp_sack_block *)(ptr+2);
	int num_sacks = (ptr[1] - TCPOLEN_SACK_BASE)>>3;
	int reord = tp->packets_out;
	int prior_fackets;
	/* 用于计算可能丢失的段的最大序号 */
	u32 lost_retrans = 0;
	int flag = 0;
	int i;

	/* So, SACKs for already sent large segments will be lost.
	 * Not good, but alternative is to resegment the queue. */
	if (sk->sk_route_caps & NETIF_F_TSO) {
		sk->sk_route_caps &= ~NETIF_F_TSO;
		sk->sk_no_largesend = 1;
		tp->mss_cache = tp->mss_cache_std;
	}

	if (!tp->sacked_out)
		tp->fackets_out = 0;
	prior_fackets = tp->fackets_out;

	for (i=0; i<num_sacks; i++, sp++) {/* 从选项中读取sack块 */
		struct sk_buff *skb;
		__u32 start_seq = ntohl(sp->start_seq);
		__u32 end_seq = ntohl(sp->end_seq);
		int fack_count = 0;
		int dup_sack = 0;

		/* Check for D-SACK. */
		if (i == 0) {/* 检测第一个块是不是DSACK */
			u32 ack = TCP_SKB_CB(ack_skb)->ack_seq;

			if (before(start_seq, ack)) {/* 第一个SACK块小于已确认块号，说明是DSACK */
				dup_sack = 1;
				tp->rx_opt.sack_ok |= 4;
				NET_INC_STATS_BH(LINUX_MIB_TCPDSACKRECV);
			} else if (num_sacks > 1 &&/* 如果第一个块大于已确认序号，则比较第一个SACK块和第二个SACK块，如果第一个SACK块包含在第二个SACK中，则也是DSACK块 */
				   !after(end_seq, ntohl(sp[1].end_seq)) &&
				   !before(start_seq, ntohl(sp[1].start_seq))) {
				dup_sack = 1;
				tp->rx_opt.sack_ok |= 4;
				NET_INC_STATS_BH(LINUX_MIB_TCPDSACKOFORECV);
			}

			/* D-SACK for already forgotten data...
			 * Do dumb counting. */
			/**
			 * undo_marker是超时重传或者FRTO时记录的UNA，prior_snd_una是本次ACK之前的UNA
			 * 如果DSACK在这之间，说明是超时重传或FRTO之后进行的重传
			 */
			if (dup_sack &&
			    !after(end_seq, prior_snd_una) &&
			    after(end_seq, tp->undo_marker))
				tp->undo_retrans--;/* 接收方重复接收了，减少undo_retrans，说明网络拥塞可能不是很严重，减到0时，应当恢复到正常状态 */

			/* Eliminate too old ACKs, but take into
			 * account more or less fresh ones, they can
			 * contain valid SACK info.
			 */
			/* ACK是一个窗口以前的，说明ACK太老了，不需要再处理 */
			if (before(ack, prior_snd_una - tp->max_window))
				return 0;
		}

		/* Event "B" in the comment above. */
		if (after(end_seq, tp->high_seq))/* SACK超过了重传队列的尾部，说明有段丢失，增加LOST标志 */
			flag |= FLAG_DATA_LOST;

		sk_stream_for_retrans_queue(skb, sk) {/* 遍历重传队列 */
			u8 sacked = TCP_SKB_CB(skb)->sacked;
			int in_sack;

			/* The retransmission queue is always in order, so
			 * we can short-circuit the walk early.
			 */
			/* 重传队列是排序的，因此如果当前SKB序号大于SACK右端序号时，则不必继续处理 */
			if(!before(TCP_SKB_CB(skb)->seq, end_seq))
				break;

			fack_count += tcp_skb_pcount(skb);

			/* 检测当前段是否完全在SACK中，如果是，则说明该段已经完全被接收到 */
			in_sack = !after(start_seq, TCP_SKB_CB(skb)->seq) &&
				!before(end_seq, TCP_SKB_CB(skb)->end_seq);

			/* Account D-SACK for retransmitted packet. */
			if ((dup_sack && in_sack) &&/* 重复接收，段完全在SACK之内 */
			    (sacked & TCPCB_RETRANS) &&/* 重传段 */
			    after(TCP_SKB_CB(skb)->end_seq, tp->undo_marker))/* 段位于上次拥塞序号之后 */
				tp->undo_retrans--;/* 说明接收方重复接收了该TCP段，因此减少undo_retrans */

			/* The frame is ACKed. */
			/* 该段已经确认过，则跳过 */
			if (!after(TCP_SKB_CB(skb)->end_seq, tp->snd_una)) {
				if (sacked&TCPCB_RETRANS) {/* 已经确认过的段重传过 */
					if ((dup_sack && in_sack) &&/* 当前SACK块确认了该段 */
					    (sacked&TCPCB_SACKED_ACKED))
						reord = min(fack_count, reord);
				} else {
					/* If it was in a hole, we detected reordering. */
					if (fack_count < prior_fackets &&
					    !(sacked&TCPCB_SACKED_ACKED))
						reord = min(fack_count, reord);
				}

				/* Nothing to do; acked frame is about to be dropped. */
				continue;
			}

			/* 可能丢失的段的范围为重传队列头和SACK块中最后一个重传段之间 */
			if ((sacked&TCPCB_SACKED_RETRANS) &&
			    after(end_seq, TCP_SKB_CB(skb)->ack_seq) &&
			    (!lost_retrans || after(end_seq, lost_retrans)))
				lost_retrans = end_seq;

			/* 不处理位于ACK块之间的段 */
			if (!in_sack)
				continue;

			if (!(sacked&TCPCB_SACKED_ACKED)) {
				if (sacked & TCPCB_SACKED_RETRANS) {/* SACK确认的是认为丢失并经过重传的段，说明并没有丢失 */
					/* If the segment is not tagged as lost,
					 * we do not clear RETRANS, believing
					 * that retransmission is still in flight.
					 */
					if (sacked & TCPCB_LOST) {
						/* 去除LOST标志 */
						TCP_SKB_CB(skb)->sacked &= ~(TCPCB_LOST|TCPCB_SACKED_RETRANS);
						tp->lost_out -= tcp_skb_pcount(skb);
						tp->retrans_out -= tcp_skb_pcount(skb);
					}
				} else {/* 确认的是未重传过的段 */
					/* New sack for not retransmitted frame,
					 * which was in hole. It is reordering.
					 */
					if (!(sacked & TCPCB_RETRANS) &&
					    fack_count < prior_fackets)
						reord = min(fack_count, reord);

					if (sacked & TCPCB_LOST) {/* 如果是LOST段，则清除LOST标志 */
						TCP_SKB_CB(skb)->sacked &= ~TCPCB_LOST;
						tp->lost_out -= tcp_skb_pcount(skb);
					}
				}

				/* 由于该处于SACK中，因此添加相关标记，累计sacked_out */
				TCP_SKB_CB(skb)->sacked |= TCPCB_SACKED_ACKED;
				flag |= FLAG_DATA_SACKED;
				tp->sacked_out += tcp_skb_pcount(skb);

				if (fack_count > tp->fackets_out)
					tp->fackets_out = fack_count;
			} else {
				if (dup_sack && (sacked&TCPCB_RETRANS))
					reord = min(fack_count, reord);
			}

			/* D-SACK. We can detect redundant retransmission
			 * in S|R and plain R frames and clear it.
			 * undo_retrans is decreased above, L|R frames
			 * are accounted above as well.
			 */
			if (dup_sack &&/* 对重传包来说，收到SACK说明重传是多余的 */
			    (TCP_SKB_CB(skb)->sacked&TCPCB_SACKED_RETRANS)) {
				TCP_SKB_CB(skb)->sacked &= ~TCPCB_SACKED_RETRANS;
				tp->retrans_out -= tcp_skb_pcount(skb);
			}
		}
	}

	/* Check for lost retransmit. This superb idea is
	 * borrowed from "ratehalving". Event "C".
	 * Later note: FACK people cheated me again 8),
	 * we have to account for reordering! Ugly,
	 * but should help.
	 */
	if (lost_retrans && tp->ca_state == TCP_CA_Recovery) {/* 拥塞机状态处于Recovery状态，并且存在可能丢失的段 */
		struct sk_buff *skb;

		sk_stream_for_retrans_queue(skb, sk) {/* 遍历所有重传队列中的段 */
			/* 只处理介于UNA和lost_retrans之间的段 */
			if (after(TCP_SKB_CB(skb)->seq, lost_retrans))
				break;
			if (!after(TCP_SKB_CB(skb)->end_seq, tp->snd_una))
				continue;
			if ((TCP_SKB_CB(skb)->sacked&TCPCB_SACKED_RETRANS) &&
			    after(lost_retrans, TCP_SKB_CB(skb)->ack_seq) &&
			    (IsFack(tp) ||
			     !before(lost_retrans,
				     TCP_SKB_CB(skb)->ack_seq + tp->reordering *
				     tp->mss_cache_std))) {
				TCP_SKB_CB(skb)->sacked &= ~TCPCB_SACKED_RETRANS;
				tp->retrans_out -= tcp_skb_pcount(skb);

				if (!(TCP_SKB_CB(skb)->sacked&(TCPCB_LOST|TCPCB_SACKED_ACKED))) {
					tp->lost_out += tcp_skb_pcount(skb);
					TCP_SKB_CB(skb)->sacked |= TCPCB_LOST;
					flag |= FLAG_DATA_SACKED;
					NET_INC_STATS_BH(LINUX_MIB_TCPLOSTRETRANSMIT);
				}
			}
		}
	}

	/* 计算已经离开主机但未被确认的段数，包括通过SACK确认的段和确认丢失的段 */
	tp->left_out = tp->sacked_out + tp->lost_out;

	/* 更新排序阀值 */
	if ((reord < tp->fackets_out) && tp->ca_state != TCP_CA_Loss)
		tcp_update_reordering(tp, ((tp->fackets_out + 1) - reord), 0);

#if FASTRETRANS_DEBUG > 0
	BUG_TRAP((int)tp->sacked_out >= 0);
	BUG_TRAP((int)tp->lost_out >= 0);
	BUG_TRAP((int)tp->retrans_out >= 0);
	BUG_TRAP((int)tcp_packets_in_flight(tp) >= 0);
#endif
	return flag;
}

/* RTO occurred, but do not yet enter loss state. Instead, transmit two new
 * segments to see from the next ACKs whether any data was really missing.
 * If the RTO was spurious, new ACKs should arrive.
 */
/**
 * 当发送段超时后，不直接进入LOSS状态，而是进入FRTO状态。
 * 而是传送两个段后根据接收到的ACK来确认数据是否丢失。避免虚假的超时。
 */
void tcp_enter_frto(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;

	tp->frto_counter = 1;/* 为1表示刚进入FRTO */

	if (tp->ca_state <= TCP_CA_Disorder ||
            tp->snd_una == tp->high_seq ||
            (tp->ca_state == TCP_CA_Loss && !tp->retransmits)) {/* 进入FRTO时，网络比较流畅 */
		tp->prior_ssthresh = tcp_current_ssthresh(tp);/* 保存慢启动阀值 */
		if (!tcp_westwood_ssthresh(tp))
			tp->snd_ssthresh = tcp_recalc_ssthresh(tp);
	}

	/* Have to clear retransmission markers here to keep the bookkeeping
	 * in shape, even though we are not yet in Loss state.
	 * If something was really lost, it is eventually caught up
	 * in tcp_enter_frto_loss.
	 */
	/* 清除与重传相关的标记，同时记录当前NUA，以便恢复 */
	tp->retrans_out = 0;
	tp->undo_marker = tp->snd_una;
	tp->undo_retrans = 0;

	sk_stream_for_retrans_queue(skb, sk) {
		TCP_SKB_CB(skb)->sacked &= ~TCPCB_RETRANS;
	}
	/* 刷新未确认的TCP段数量 */
	tcp_sync_left_out(tp);

	/* 记录下FRTO状态时的nxt */
	tcp_set_ca_state(tp, TCP_CA_Open);
	tp->frto_highmark = tp->snd_nxt;
}

/* Enter Loss state after F-RTO was applied. Dupack arrived after RTO,
 * which indicates that we should follow the traditional RTO recovery,
 * i.e. mark everything lost and do go-back-N retransmission.
 */
/**
 * 在FRTO阶段，如果接收到ACK发现确实是产生了传送超时，则调用此函数进入拥塞恢复阶段，开始慢启动过程。
 */
static void tcp_enter_frto_loss(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;
	int cnt = 0;

	/* 进入Loss状态后，重新统计相关的SACK、丢失等等数据 */
	tp->sacked_out = 0;
	tp->lost_out = 0;
	tp->fackets_out = 0;

	/* 遍历重传队列，重新标记LOST标志。 */
	sk_stream_for_retrans_queue(skb, sk) {
		cnt += tcp_skb_pcount(skb);
		TCP_SKB_CB(skb)->sacked &= ~TCPCB_LOST;
		if (!(TCP_SKB_CB(skb)->sacked&TCPCB_SACKED_ACKED)) {/* 已经确认过的不用修改 */

			/* Do not mark those segments lost that were
			 * forward transmitted after RTO
			 */
			if (!after(TCP_SKB_CB(skb)->end_seq,/* 设置标志 */
				   tp->frto_highmark)) {
				TCP_SKB_CB(skb)->sacked |= TCPCB_LOST;
				tp->lost_out += tcp_skb_pcount(skb);
			}
		} else {
			tp->sacked_out += tcp_skb_pcount(skb);
			tp->fackets_out = cnt;
		}
	}
	tcp_sync_left_out(tp);

	/* 进入Loss状态，重新设置拥塞窗口等等 */
	tp->snd_cwnd = tp->frto_counter + tcp_packets_in_flight(tp)+1;
	tp->snd_cwnd_cnt = 0;
	tp->snd_cwnd_stamp = tcp_time_stamp;
	tp->undo_marker = 0;
	tp->frto_counter = 0;

	tp->reordering = min_t(unsigned int, tp->reordering,
					     sysctl_tcp_reordering);
	tcp_set_ca_state(tp, TCP_CA_Loss);
	tp->high_seq = tp->frto_highmark;
	TCP_ECN_queue_cwr(tp);

	init_bictcp(tp);
}

void tcp_clear_retrans(struct tcp_sock *tp)
{
	tp->left_out = 0;
	tp->retrans_out = 0;

	tp->fackets_out = 0;
	tp->sacked_out = 0;
	tp->lost_out = 0;

	tp->undo_marker = 0;
	tp->undo_retrans = 0;
}

/* Enter Loss state. If "how" is not zero, forget all SACK information
 * and reset tags completely, otherwise preserve SACKs. If receiver
 * dropped its ofo queue, we will know this due to reneging detection.
 */
/* 进入LOSS状态 */
void tcp_enter_loss(struct sock *sk, int how)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;
	int cnt = 0;

	/* Reduce ssthresh if it has not yet been made inside this window. */
	if (tp->ca_state <= TCP_CA_Disorder || tp->snd_una == tp->high_seq ||
	    (tp->ca_state == TCP_CA_Loss && !tp->retransmits)) {/* 刚进入LOSS状态 */
	    /* 设置发送拥塞窗口的阀值 */
		tp->prior_ssthresh = tcp_current_ssthresh(tp);
		tp->snd_ssthresh = tcp_recalc_ssthresh(tp);
	}
	/* 将拥塞窗口设置为1个段 */
	tp->snd_cwnd	   = 1;
	/* 清除CAK段计数，和拥塞窗口时间 */
	tp->snd_cwnd_cnt   = 0;
	tp->snd_cwnd_stamp = tcp_time_stamp;

	tcp_clear_retrans(tp);

	/* Push undo marker, if it was plain RTO and nothing
	 * was retransmitted. */
	if (!how)/* 不清除SACK标记，则记录UNA以便在合适的时候能够进行拥塞窗口调整撤销操作 */
		tp->undo_marker = tp->snd_una;

	sk_stream_for_retrans_queue(skb, sk) {
		cnt += tcp_skb_pcount(skb);/* 段中GSO分段数量，用于累计fackets_out */
		/* 重传队列中段的记分牌已经有重传标志，则清除拥塞窗口调整撤销标记 */
		if (TCP_SKB_CB(skb)->sacked&TCPCB_RETRANS)
			tp->undo_marker = 0;
		/* 将重传队列中段记分牌去掉重传和丢失标记 */
		TCP_SKB_CB(skb)->sacked &= (~TCPCB_TAGBITS)|TCPCB_SACKED_ACKED;
		/* 段记分牌没有SACK标记或需要清除SACK标记 */
		if (!(TCP_SKB_CB(skb)->sacked&TCPCB_SACKED_ACKED) || how) {
			/* 清除SACK标记并加上LOST标记 */
			TCP_SKB_CB(skb)->sacked &= ~TCPCB_SACKED_ACKED;
			TCP_SKB_CB(skb)->sacked |= TCPCB_LOST;
			/* 统计丢失段的数量 */
			tp->lost_out += tcp_skb_pcount(skb);
		} else {
			/* 更新SACK确认的数量和fackets_out */
			tp->sacked_out += tcp_skb_pcount(skb);
			tp->fackets_out = cnt;
		}
	}
	tcp_sync_left_out(tp);

	/* 重排序的数量 */
	tp->reordering = min_t(unsigned int, tp->reordering,
					     sysctl_tcp_reordering);
	/* 设置拥塞状态为LOSS状态 */
	tcp_set_ca_state(tp, TCP_CA_Loss);
	/* 发生拥塞时的nxt */
	tp->high_seq = tp->snd_nxt;
	/* 清除重传的变量 */
	TCP_ECN_queue_cwr(tp);
}

/* 如果接收到的ACK确认的是已经通过SACK确认的段，则表示记录的SACK不能反映接收方实际的状态 */
static int tcp_check_sack_reneging(struct sock *sk, struct tcp_sock *tp)
{
	struct sk_buff *skb;

	/* If ACK arrived pointing to a remembered SACK,
	 * it means that our remembered SACKs do not reflect
	 * real state of receiver i.e.
	 * receiver _host_ is heavily congested (or buggy).
	 * Do processing similar to RTO timeout.
	 */
	if ((skb = skb_peek(&sk->sk_write_queue)) != NULL &&
	    (TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_ACKED)) {
		NET_INC_STATS_BH(LINUX_MIB_TCPSACKRENEGING);

		tcp_enter_loss(sk, 1);
		tp->retransmits++;
		tcp_retransmit_skb(sk, skb_peek(&sk->sk_write_queue));
		tcp_reset_xmit_timer(sk, TCP_TIME_RETRANS, tp->rto);
		return 1;
	}
	return 0;
}

static inline int tcp_fackets_out(struct tcp_sock *tp)
{
	return IsReno(tp) ? tp->sacked_out+1 : tp->fackets_out;
}

static inline int tcp_skb_timedout(struct tcp_sock *tp, struct sk_buff *skb)
{
	return (tcp_time_stamp - TCP_SKB_CB(skb)->when > tp->rto);
}

static inline int tcp_head_timedout(struct sock *sk, struct tcp_sock *tp)
{
	return tp->packets_out &&
	       tcp_skb_timedout(tp, skb_peek(&sk->sk_write_queue));
}

/* Linux NewReno/SACK/FACK/ECN state machine.
 * --------------------------------------
 *
 * "Open"	Normal state, no dubious events, fast path.
 * "Disorder"   In all the respects it is "Open",
 *		but requires a bit more attention. It is entered when
 *		we see some SACKs or dupacks. It is split of "Open"
 *		mainly to move some processing from fast path to slow one.
 * "CWR"	CWND was reduced due to some Congestion Notification event.
 *		It can be ECN, ICMP source quench, local device congestion.
 * "Recovery"	CWND was reduced, we are fast-retransmitting.
 * "Loss"	CWND was reduced due to RTO timeout or SACK reneging.
 *
 * tcp_fastretrans_alert() is entered:
 * - each incoming ACK, if state is not "Open"
 * - when arrived ACK is unusual, namely:
 *	* SACK
 *	* Duplicate ACK.
 *	* ECN ECE.
 *
 * Counting packets in flight is pretty simple.
 *
 *	in_flight = packets_out - left_out + retrans_out
 *
 *	packets_out is SND.NXT-SND.UNA counted in packets.
 *
 *	retrans_out is number of retransmitted segments.
 *
 *	left_out is number of segments left network, but not ACKed yet.
 *
 *		left_out = sacked_out + lost_out
 *
 *     sacked_out: Packets, which arrived to receiver out of order
 *		   and hence not ACKed. With SACKs this number is simply
 *		   amount of SACKed data. Even without SACKs
 *		   it is easy to give pretty reliable estimate of this number,
 *		   counting duplicate ACKs.
 *
 *       lost_out: Packets lost by network. TCP has no explicit
 *		   "loss notification" feedback from network (for now).
 *		   It means that this number can be only _guessed_.
 *		   Actually, it is the heuristics to predict lossage that
 *		   distinguishes different algorithms.
 *
 *	F.e. after RTO, when all the queue is considered as lost,
 *	lost_out = packets_out and in_flight = retrans_out.
 *
 *		Essentially, we have now two algorithms counting
 *		lost packets.
 *
 *		FACK: It is the simplest heuristics. As soon as we decided
 *		that something is lost, we decide that _all_ not SACKed
 *		packets until the most forward SACK are lost. I.e.
 *		lost_out = fackets_out - sacked_out and left_out = fackets_out.
 *		It is absolutely correct estimate, if network does not reorder
 *		packets. And it loses any connection to reality when reordering
 *		takes place. We use FACK by default until reordering
 *		is suspected on the path to this destination.
 *
 *		NewReno: when Recovery is entered, we assume that one segment
 *		is lost (classic Reno). While we are in Recovery and
 *		a partial ACK arrives, we assume that one more packet
 *		is lost (NewReno). This heuristics are the same in NewReno
 *		and SACK.
 *
 *  Imagine, that's all! Forget about all this shamanism about CWND inflation
 *  deflation etc. CWND is real congestion window, never inflated, changes
 *  only according to classic VJ rules.
 *
 * Really tricky (and requiring careful tuning) part of algorithm
 * is hidden in functions tcp_time_to_recover() and tcp_xmit_retransmit_queue().
 * The first determines the moment _when_ we should reduce CWND and,
 * hence, slow down forward transmission. In fact, it determines the moment
 * when we decide that hole is caused by loss, rather than by a reorder.
 *
 * tcp_xmit_retransmit_queue() decides, _what_ we should retransmit to fill
 * holes, caused by lost packets.
 *
 * And the most logically complicated part of algorithm is undo
 * heuristics. We detect false retransmits due to both too early
 * fast retransmit (reordering) and underestimated RTO, analyzing
 * timestamps and D-SACKs. When we detect that some segments were
 * retransmitted by mistake and CWND reduction was wrong, we undo
 * window reduction and abort recovery phase. This logic is hidden
 * inside several functions named tcp_try_undo_<something>.
 */

/* This function decides, when we should leave Disordered state
 * and enter Recovery phase, reducing congestion window.
 *
 * Main question: may we further continue forward transmission
 * with the same cwnd?
 */
/* 用于检测能否进入快速恢复状态。对于NewReno来说，连续接收到3个重复确认，便会进入recover状态 */
static int tcp_time_to_recover(struct sock *sk, struct tcp_sock *tp)
{
	__u32 packets_out;

	/* Trick#1: The loss is proven. */
	if (tp->lost_out)/* 有丢失的段，则可以进入recover状态 */
		return 1;

	/* Not-A-Trick#2 : Classic rule... */
	if (tcp_fackets_out(tp) > tp->reordering)/* 丢失的段超过乱序的段 */
		return 1;

	/* Trick#3 : when we use RFC2988 timer restart, fast
	 * retransmit can be triggered by timeout of queue head.
	 */
	if (tcp_head_timedout(sk, tp))/* 重传队列队首的段发送超时，可以进入recover状态 */
		return 1;

	/* Trick#4: It is still not OK... But will it be useful to delay
	 * recovery more?
	 */
	packets_out = tp->packets_out;
	/* 未确认的段较少，通过SACK确认的段超过未确认的一半，同时没有段需要及时输出 */
	if (packets_out <= tp->reordering &&
	    tp->sacked_out >= max_t(__u32, packets_out/2, sysctl_tcp_reordering) &&
	    !tcp_may_send_now(sk, tp)) {
		/* We have nothing to send. This connection is limited
		 * either by receiver window or by application.
		 */
		return 1;
	}

	return 0;
}

/* If we receive more dupacks than we expected counting segments
 * in assumption of absent reordering, interpret this as reordering.
 * The only another reason could be bug in receiver TCP.
 */
static void tcp_check_reno_reordering(struct tcp_sock *tp, int addend)
{
	u32 holes;

	holes = max(tp->lost_out, 1U);
	holes = min(holes, tp->packets_out);

	if ((tp->sacked_out + holes) > tp->packets_out) {
		tp->sacked_out = tp->packets_out - holes;
		tcp_update_reordering(tp, tp->packets_out+addend, 0);
	}
}

/* Emulate SACKs for SACKless connection: account for a new dupack. */

static void tcp_add_reno_sack(struct tcp_sock *tp)
{
	tp->sacked_out++;
	tcp_check_reno_reordering(tp, 0);
	tcp_sync_left_out(tp);
}

/* Account for ACK, ACKing some data in Reno Recovery phase. */

static void tcp_remove_reno_sacks(struct sock *sk, struct tcp_sock *tp, int acked)
{
	if (acked > 0) {
		/* One ACK acked hole. The rest eat duplicate ACKs. */
		if (acked-1 >= tp->sacked_out)
			tp->sacked_out = 0;
		else
			tp->sacked_out -= acked-1;
	}
	tcp_check_reno_reordering(tp, acked);
	tcp_sync_left_out(tp);
}

static inline void tcp_reset_reno_sack(struct tcp_sock *tp)
{
	tp->sacked_out = 0;
	tp->left_out = tp->lost_out;
}

/* Mark head of queue up as lost. */
/**
 * 从重传队列首部或上次标记丢失段的位置开始，为记分牌为0的段添加LOST标记。
 * 直到所有被标记为LOST的段达到packets或被标记序号超过high_seq为止。
 */
static void tcp_mark_head_lost(struct sock *sk, struct tcp_sock *tp,
			       int packets, u32 high_seq)
{
	struct sk_buff *skb;
	int cnt = packets;

	BUG_TRAP(cnt <= tp->packets_out);

	sk_stream_for_retrans_queue(skb, sk) {
		cnt -= tcp_skb_pcount(skb);
		if (cnt < 0 || after(TCP_SKB_CB(skb)->end_seq, high_seq))
			break;
		if (!(TCP_SKB_CB(skb)->sacked&TCPCB_TAGBITS)) {
			TCP_SKB_CB(skb)->sacked |= TCPCB_LOST;
			tp->lost_out += tcp_skb_pcount(skb);
		}
	}
	tcp_sync_left_out(tp);
}

/* Account newly detected lost packet(s) */

/* 为确定丢失的段更新记分牌。如果确认接收到重复ACK或者重传队首的段传送超时时被调用。 */
static void tcp_update_scoreboard(struct sock *sk, struct tcp_sock *tp)
{
	/* 为重传队列上的段添加LOST标记 */
	if (IsFack(tp)) {
		int lost = tp->fackets_out - tp->reordering;
		if (lost <= 0)
			lost = 1;
		tcp_mark_head_lost(sk, tp, lost, tp->high_seq);
	} else {
		tcp_mark_head_lost(sk, tp, 1, tp->high_seq);
	}

	/* New heuristics: it is possible only after we switched
	 * to restart timer each time when something is ACKed.
	 * Hence, we can detect timed out packets during fast
	 * retransmit without falling to slow start.
	 */
	if (tcp_head_timedout(sk, tp)) {/* 重传队列队首的段已经超时 */
		struct sk_buff *skb;

		sk_stream_for_retrans_queue(skb, sk) {/* 为已经超时且记分牌为空的段添加LOST标志 */
			if (tcp_skb_timedout(tp, skb) &&
			    !(TCP_SKB_CB(skb)->sacked&TCPCB_TAGBITS)) {
				TCP_SKB_CB(skb)->sacked |= TCPCB_LOST;
				tp->lost_out += tcp_skb_pcount(skb);
			}
		}
		/* 计算已经离开主机但是没有确认的段数 */
		tcp_sync_left_out(tp);
	}
}

/* CWND moderation, preventing bursts due to too big ACKs
 * in dubious situations.
 */
/**
 * 对拥塞窗口进行微调。
 * 再取拥塞窗口大小和已发送但未确认段数量加3之间的最小值作为当前拥塞窗口。
 * 并记录最近一次调整拥塞窗口的时间。
 */
static inline void tcp_moderate_cwnd(struct tcp_sock *tp)
{
	tp->snd_cwnd = min(tp->snd_cwnd,
			   tcp_packets_in_flight(tp)+tcp_max_burst(tp));
	tp->snd_cwnd_stamp = tcp_time_stamp;
}

/* Decrease cwnd each second ack. */

static void tcp_cwnd_down(struct tcp_sock *tp)
{
	int decr = tp->snd_cwnd_cnt + 1;
	__u32 limit;

	/*
	 * TCP Westwood
	 * Here limit is evaluated as BWestimation*RTTmin (for obtaining it
	 * in packets we use mss_cache). If sysctl_tcp_westwood is off
	 * tcp_westwood_bw_rttmin() returns 0. In such case snd_ssthresh is
	 * still used as usual. It prevents other strange cases in which
	 * BWE*RTTmin could assume value 0. It should not happen but...
	 */

	if (!(limit = tcp_westwood_bw_rttmin(tp)))
		limit = tp->snd_ssthresh/2;

	tp->snd_cwnd_cnt = decr&1;
	decr >>= 1;

	if (decr && tp->snd_cwnd > limit)
		tp->snd_cwnd -= decr;

	tp->snd_cwnd = min(tp->snd_cwnd, tcp_packets_in_flight(tp)+1);
	tp->snd_cwnd_stamp = tcp_time_stamp;
}

/* Nothing was retransmitted or returned timestamp is less
 * than timestamp of the first retransmission.
 */
static inline int tcp_packet_delayed(struct tcp_sock *tp)
{
	return !tp->retrans_stamp ||
		(tp->rx_opt.saw_tstamp && tp->rx_opt.rcv_tsecr &&
		 (__s32)(tp->rx_opt.rcv_tsecr - tp->retrans_stamp) < 0);
}

/* Undo procedures. */

#if FASTRETRANS_DEBUG > 1
static void DBGUNDO(struct sock *sk, struct tcp_sock *tp, const char *msg)
{
	struct inet_sock *inet = inet_sk(sk);
	printk(KERN_DEBUG "Undo %s %u.%u.%u.%u/%u c%u l%u ss%u/%u p%u\n",
	       msg,
	       NIPQUAD(inet->daddr), ntohs(inet->dport),
	       tp->snd_cwnd, tp->left_out,
	       tp->snd_ssthresh, tp->prior_ssthresh,
	       tp->packets_out);
}
#else
#define DBGUNDO(x...) do { } while (0)
#endif

/* 不再缩小拥塞窗口 */
static void tcp_undo_cwr(struct tcp_sock *tp, int undo)
{
	if (tp->prior_ssthresh) {/* 根据慢启动阀值的旧值存在与否，来确定撤销操作 */
		/* 在当前的拥塞窗口和2倍大的慢启动值之间选择较大值作为当前的拥塞窗口大小 */
		tp->snd_cwnd = max(tp->snd_cwnd, tp->snd_ssthresh<<1);

		/* 撤销慢启动阀值及TCP_ECN_DEMAND_CWR标志 */
		if (undo && tp->prior_ssthresh > tp->snd_ssthresh) {
			tp->snd_ssthresh = tp->prior_ssthresh;
			TCP_ECN_withdraw_cwr(tp);
		}
	} else {/* 不存在启动阀值的旧值 */
		/* 取拥塞窗口大小和启动阀值之间的较大为当前拥塞窗口 */
		tp->snd_cwnd = max(tp->snd_cwnd, tp->snd_ssthresh);
	}
	/* 对拥塞窗口进行微调，取拥塞窗口大小和已发送但未确认段数量加3之间的最小值作为当前拥塞窗口 */
	tcp_moderate_cwnd(tp);
	tp->snd_cwnd_stamp = tcp_time_stamp;
}

/* 在进行拥塞窗口调整撤销之前，调用此函数检测能否撤销 */
static inline int tcp_may_undo(struct tcp_sock *tp)
{
	return tp->undo_marker &&/* 重传起始点不为0 */
		(!tp->undo_retrans || tcp_packet_delayed(tp));/* 没有可撤销的重传段数，或者没有重传或重传了之后还没有接收到对方发送的确认 */
}

/* People celebrate: "We love our President!" */
/* 从recovery状态撤销 */
static int tcp_try_undo_recovery(struct sock *sk, struct tcp_sock *tp)
{
	if (tcp_may_undo(tp)) {/* 可以进行撤销 */
		/* Happy end! We did not retransmit anything
		 * or our original transmission succeeded.
		 */
		DBGUNDO(sk, tp, tp->ca_state == TCP_CA_Loss ? "loss" : "retrans");
		/* 恢复拥塞窗口和拥塞控制时慢启动的阀值 */
		tcp_undo_cwr(tp, 1);
		if (tp->ca_state == TCP_CA_Loss)
			NET_INC_STATS_BH(LINUX_MIB_TCPLOSSUNDO);
		else
			NET_INC_STATS_BH(LINUX_MIB_TCPFULLUNDO);
		tp->undo_marker = 0;
	}
	if (tp->snd_una == tp->high_seq && IsReno(tp)) {/* 不支持SACK */
		/* Hold old state until something *above* high_seq
		 * is ACKed. For Reno it is MUST to prevent false
		 * fast retransmits (RFC2582). SACK TCP is safe. */
		 /* 只对拥塞窗口进行微调 */
		tcp_moderate_cwnd(tp);
		return 1;
	}
	/* 支持SACK，撤销到OPEN状态 */
	tcp_set_ca_state(tp, TCP_CA_Open);
	return 0;
}

/* Try to undo cwnd reduction, because D-SACKs acked all retransmitted data */
static void tcp_try_undo_dsack(struct sock *sk, struct tcp_sock *tp)
{
	if (tp->undo_marker && !tp->undo_retrans) {
		DBGUNDO(sk, tp, "D-SACK");
		tcp_undo_cwr(tp, 1);
		tp->undo_marker = 0;
		NET_INC_STATS_BH(LINUX_MIB_TCPDSACKUNDO);
	}
}

/* Undo during fast recovery after partial ACK. */

/* 在Recovery拥塞状态，如果ACK确认了部分重传的段，调用此函数进行拥塞窗口的撤销 */
static int tcp_try_undo_partial(struct sock *sk, struct tcp_sock *tp,
				int acked)
{
	/* Partial ACK arrived. Force Hoe's retransmit. */
	int failed = IsReno(tp) || tp->fackets_out>tp->reordering;

	if (tcp_may_undo(tp)) {
		/* Plain luck! Hole if filled with delayed
		 * packet, rather than with a retransmit.
		 */
		if (tp->retrans_out == 0)
			tp->retrans_stamp = 0;

		tcp_update_reordering(tp, tcp_fackets_out(tp)+acked, 1);

		DBGUNDO(sk, tp, "Hoe");
		tcp_undo_cwr(tp, 0);
		NET_INC_STATS_BH(LINUX_MIB_TCPPARTIALUNDO);

		/* So... Do not make Hoe's retransmit yet.
		 * If the first packet was delayed, the rest
		 * ones are most probably delayed as well.
		 */
		failed = 0;
	}
	return failed;
}

/* Undo during loss recovery after partial ACK. */
/* 在接收到新的确认时，尝试从LOSS状态进入open状态，返回1表示撤销成功 */
static int tcp_try_undo_loss(struct sock *sk, struct tcp_sock *tp)
{
	if (tcp_may_undo(tp)) {/* 是否可以从Loss状态撤销 */
		struct sk_buff *skb;
		/* 清除重传队列上，所有段的记分牌上的LOST标志 */
		sk_stream_for_retrans_queue(skb, sk) {
			TCP_SKB_CB(skb)->sacked &= ~TCPCB_LOST;
		}
		DBGUNDO(sk, tp, "partial loss");
		/* 清除所有与拥塞控制相关的标志 */
		tp->lost_out = 0;
		tp->left_out = tp->sacked_out;
		tcp_undo_cwr(tp, 1);
		NET_INC_STATS_BH(LINUX_MIB_TCPLOSSUNDO);
		tp->retransmits = 0;
		tp->undo_marker = 0;
		if (!IsReno(tp))
			tcp_set_ca_state(tp, TCP_CA_Open);
		return 1;
	}
	return 0;
}

/* 结束拥塞窗口减小，将拥塞窗口更新为拥塞窗口与慢启动的阀值之间的较小值。记录最近一次调整拥塞窗口的时间 */
static inline void tcp_complete_cwr(struct tcp_sock *tp)
{
	if (tcp_westwood_cwnd(tp)) 
		tp->snd_ssthresh = tp->snd_cwnd;
	else
		tp->snd_cwnd = min(tp->snd_cwnd, tp->snd_ssthresh);
	tp->snd_cwnd_stamp = tcp_time_stamp;
}

static void tcp_try_to_open(struct sock *sk, struct tcp_sock *tp, int flag)
{
	tp->left_out = tp->sacked_out;

	if (tp->retrans_out == 0)
		tp->retrans_stamp = 0;

	if (flag&FLAG_ECE)
		tcp_enter_cwr(tp);

	if (tp->ca_state != TCP_CA_CWR) {
		int state = TCP_CA_Open;

		if (tp->left_out || tp->retrans_out || tp->undo_marker)
			state = TCP_CA_Disorder;

		if (tp->ca_state != state) {
			tcp_set_ca_state(tp, state);
			tp->high_seq = tp->snd_nxt;
		}
		tcp_moderate_cwnd(tp);
	} else {
		tcp_cwnd_down(tp);
	}
}

/* Process an event, which can update packets-in-flight not trivially.
 * Main goal of this function is to calculate new estimate for left_out,
 * taking into account both packets sitting in receiver's buffer and
 * packets lost by network.
 *
 * Besides that it does CWND reduction, when packet loss is detected
 * and changes state of machine.
 *
 * It does _not_ decide what to send, it is made in function
 * tcp_xmit_retransmit_queue().
 */
/**
 * 拥塞控制状态的处理 
 * 包括处理显式拥塞通知，判断SACK是否虚假等等
 */
static void
tcp_fastretrans_alert(struct sock *sk, u32 prior_snd_una,
		      int prior_packets, int flag)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int is_dupack = (tp->snd_una == prior_snd_una && !(flag&FLAG_NOT_DUP));

	/* Some technical things:
	 * 1. Reno does not count dupacks (sacked_out) automatically. */
	/* 对sacked_out和fackets_out进行微调 */
	if (!tp->packets_out)
		tp->sacked_out = 0;
        /* 2. SACK counts snd_fack in packets inaccurately. */
	if (tp->sacked_out == 0)
		tp->fackets_out = 0;

        /* Now state machine starts.
	 * A. ECE, hence prohibit cwnd undoing, the reduction is required. */
	if (flag&FLAG_ECE)/* 接收到显式拥塞通知，因此禁止拥塞窗口撤销，并开始减小拥塞窗口 */
		tp->prior_ssthresh = 0;

	/* B. In all the states check for reneging SACKs. */
	/**
	 * 如果接收的ACK指向已记录的SACK，说明记录的SACK不能反应接收方的真实状态，那么按照重传超时处理 
	 * 因为一般情况下，ACK应当指向SACK后面未接收的地方。
	 */
	if (tp->sacked_out && tcp_check_sack_reneging(sk, tp))
		return;

	/* C. Process data loss notification, provided it is valid. */
	if ((flag&FLAG_DATA_LOST) &&/* 通过SACK发现有段丢失 */
	    before(tp->snd_una, tp->high_seq) &&
	    tp->ca_state != TCP_CA_Open &&
	    tp->fackets_out > tp->reordering) {
	    /* 从重传队列首部或上次标识丢失段的位置开始，为记分牌为0的段添加LOST标记 */
		tcp_mark_head_lost(sk, tp, tp->fackets_out-tp->reordering, tp->high_seq);
		NET_INC_STATS_BH(LINUX_MIB_TCPLOSS);
	}

	/* D. Synchronize left_out to current state. */
	/* 更新已经离开主机，但是在网络中未确认的TCP段数 */
	tcp_sync_left_out(tp);

	/* E. Check state exit conditions. State can be terminated
	 *    when high_seq is ACKed. */
	/* 开始处理从拥塞状态撤销 */
	if (tp->ca_state == TCP_CA_Open) {/* 当前是open状态 */
		if (!sysctl_tcp_frto)
			BUG_TRAP(tp->retrans_out == 0);
		tp->retrans_stamp = 0;/* 清除上次重传阶段的第一个重传段的发送时间 */
	} else if (!before(tp->snd_una, tp->high_seq)) {/* 当拥塞时记录的nxt被确认时，拥塞状况已经好转，可以视情况回到open状态 */
		switch (tp->ca_state) {
		case TCP_CA_Loss:/* 从Loss状态撤销到open状态 */
			tp->retransmits = 0;
			if (tcp_try_undo_recovery(sk, tp))/* 撤销不成功则返回 */
				return;
			/* 撤销成功则继续处理open状态 */
			break;

		case TCP_CA_CWR:
			/* CWR is to be held something *above* high_seq
			 * is ACKed for CWR bit to reach receiver. */
			if (tp->snd_una != tp->high_seq) {/* 拥塞时记录的nxt段都被确认了 */
				/* 减小拥塞窗口，并撤销到open状态 */
				tcp_complete_cwr(tp);
				tcp_set_ca_state(tp, TCP_CA_Open);
			}
			break;

		case TCP_CA_Disorder:
			/* 如果DSACK确认了所有重传的段，则要撤销拥塞窗口 */
			tcp_try_undo_dsack(sk, tp);
			if (!tp->undo_marker ||/* 拥塞时记录的nxt之后的段都被确认了 */
			    /* For SACK case do not Open to allow to undo
			     * catching for all duplicate ACKs. */
			    IsReno(tp) || tp->snd_una != tp->high_seq) {/* 启用了NewReno */
			    /* 恢复到open状态 */
				tp->undo_marker = 0;
				tcp_set_ca_state(tp, TCP_CA_Open);
			}
			break;

		case TCP_CA_Recovery:
			if (IsReno(tp))/* 如果启用了NewReno */
				tcp_reset_reno_sack(tp);/* 复位SACK相关的数据 */
			if (tcp_try_undo_recovery(sk, tp))/* 尝试从Recovery状态撤销 */
				return;
			/* 如果撤销成功，则结束拥塞窗口减小 */
			tcp_complete_cwr(tp);
			break;
		}
	}

	/* F. Process state. */
	switch (tp->ca_state) {
	case TCP_CA_Recovery:
		if (prior_snd_una == tp->snd_una) {/* 没有段被确认 */
			if (IsReno(tp) && is_dupack)/* 是重复ACK，记录接收到的重复ACK数量 */
				tcp_add_reno_sack(tp);
		} else {/* 有新的段被确认 */
			/* 计算被确认的段数量 */
			int acked = prior_packets - tp->packets_out;
			if (IsReno(tp))
				tcp_remove_reno_sacks(sk, tp, acked);
			/* 处理拥塞窗口的撤销 */
			is_dupack = tcp_try_undo_partial(sk, tp, acked);
		}
		break;
	case TCP_CA_Loss:
		if (flag&FLAG_DATA_ACKED)/* 确认了新的段 */
			tp->retransmits = 0;
		if (!tcp_try_undo_loss(sk, tp)) {/* 尝试撤销到open状态 */
			/* 微调拥塞窗口 */
			tcp_moderate_cwnd(tp);
			/* 开始重传那些标记丢失的段 */
			tcp_xmit_retransmit_queue(sk);
			return;
		}
		if (tp->ca_state != TCP_CA_Open)
			return;
		/* Loss is undone; fall through to processing in Open state. */
	default:/* 从Disorder进入Recovery状态 */
		if (IsReno(tp)) {/* 不支持SACK */
			if (tp->snd_una != prior_snd_una)/* 有新的段被确认 */
				tcp_reset_reno_sack(tp);/* 复位重复确认计数 */
			if (is_dupack)
				tcp_add_reno_sack(tp);
		}

		if (tp->ca_state == TCP_CA_Disorder)/* 如果处于Disorder状态 */
			tcp_try_undo_dsack(sk, tp);/* 如果D-SACK确认了所有重传的段，则尝试撤销"缩小拥塞窗口" */

		if (!tcp_time_to_recover(sk, tp)) {/* 判断是否能够进入recover状态 */
			/* 如果不能进入recover状态，则尝试是否能够进入open状态 */
			tcp_try_to_open(sk, tp, flag);
			return;
		}

		/* Otherwise enter Recovery state */

		/* 统计mib计数 */
		if (IsReno(tp))
			NET_INC_STATS_BH(LINUX_MIB_TCPRENORECOVERY);
		else
			NET_INC_STATS_BH(LINUX_MIB_TCPSACKRECOVERY);

		tp->high_seq = tp->snd_nxt;
		tp->prior_ssthresh = 0;
		tp->undo_marker = tp->snd_una;
		tp->undo_retrans = tp->retrans_out;

		if (tp->ca_state < TCP_CA_CWR) {
			if (!(flag&FLAG_ECE))/* 保存当前的慢启动阀值 */
				tp->prior_ssthresh = tcp_current_ssthresh(tp);
			/* 根据不同的算法设置当前的慢启动阀值 */
			tp->snd_ssthresh = tcp_recalc_ssthresh(tp);
			TCP_ECN_queue_cwr(tp);
		}

		/* 清除计数后进入recover状态 */
		tp->snd_cwnd_cnt = 0;
		tcp_set_ca_state(tp, TCP_CA_Recovery);
	}

	/* 接收到重复ACK，或者重传队首的段传送超时 */
	if (is_dupack || tcp_head_timedout(sk, tp))
		tcp_update_scoreboard(sk, tp);/* 为丢失的段更新记分牌 */
	/* 在CWR和Recovery状态，拥塞窗口每隔一个新到的确认就减少一个段，直到拥塞窗口大小等于拥塞窗口阀值为止 */
	tcp_cwnd_down(tp);
	/* 重传重传队列中标记为LOST的段，同时重置RTO定时器 */
	tcp_xmit_retransmit_queue(sk);
}

/* Read draft-ietf-tcplw-high-performance before mucking
 * with this code. (Superceeds RFC1323)
 */
static void tcp_ack_saw_tstamp(struct tcp_sock *tp, int flag)
{
	__u32 seq_rtt;

	/* RTTM Rule: A TSecr value received in a segment is used to
	 * update the averaged RTT measurement only if the segment
	 * acknowledges some new data, i.e., only if it advances the
	 * left edge of the send window.
	 *
	 * See draft-ietf-tcplw-high-performance-00, section 3.3.
	 * 1998/04/10 Andrey V. Savochkin <saw@msu.ru>
	 *
	 * Changed: reset backoff as soon as we see the first valid sample.
	 * If we do not, we get strongly overstimated rto. With timestamps
	 * samples are accepted even from very old segments: f.e., when rtt=1
	 * increases to 8, we retransmit 5 times and after 8 seconds delayed
	 * answer arrives rto becomes 120 seconds! If at least one of segments
	 * in window is lost... Voila.	 			--ANK (010210)
	 */
	seq_rtt = tcp_time_stamp - tp->rx_opt.rcv_tsecr;
	tcp_rtt_estimator(tp, seq_rtt);
	tcp_set_rto(tp);
	tp->backoff = 0;
	tcp_bound_rto(tp);
}

static void tcp_ack_no_tstamp(struct tcp_sock *tp, u32 seq_rtt, int flag)
{
	/* We don't have a timestamp. Can only use
	 * packets that are not retransmitted to determine
	 * rtt estimates. Also, we must not reset the
	 * backoff for rto until we get a non-retransmitted
	 * packet. This allows us to deal with a situation
	 * where the network delay has increased suddenly.
	 * I.e. Karn's algorithm. (SIGCOMM '87, p5.)
	 */

	if (flag & FLAG_RETRANS_DATA_ACKED)
		return;

	tcp_rtt_estimator(tp, seq_rtt);
	tcp_set_rto(tp);
	tp->backoff = 0;
	tcp_bound_rto(tp);
}

/* 当确认了发送报文时，调用此函数更新往返时间 */
static inline void tcp_ack_update_rtt(struct tcp_sock *tp,
				      int flag, s32 seq_rtt)
{
	/* Note that peer MAY send zero echo. In this case it is ignored. (rfc1323) */
	if (tp->rx_opt.saw_tstamp && tp->rx_opt.rcv_tsecr)
		tcp_ack_saw_tstamp(tp, flag);
	else if (seq_rtt >= 0)
		tcp_ack_no_tstamp(tp, seq_rtt, flag);
}

/*
 * Compute congestion window to use.
 *
 * This is from the implementation of BICTCP in
 * Lison-Xu, Kahaled Harfoush, and Injog Rhee.
 *  "Binary Increase Congestion Control for Fast, Long Distance
 *  Networks" in InfoComm 2004
 * Available from:
 *  http://www.csc.ncsu.edu/faculty/rhee/export/bitcp.pdf
 *
 * Unless BIC is enabled and congestion window is large
 * this behaves the same as the original Reno.
 */
static inline __u32 bictcp_cwnd(struct tcp_sock *tp)
{
	/* orignal Reno behaviour */
	if (!tcp_is_bic(tp))
		return tp->snd_cwnd;

	if (tp->bictcp.last_cwnd == tp->snd_cwnd &&
	   (s32)(tcp_time_stamp - tp->bictcp.last_stamp) <= (HZ>>5))
		return tp->bictcp.cnt;

	tp->bictcp.last_cwnd = tp->snd_cwnd;
	tp->bictcp.last_stamp = tcp_time_stamp;
      
	/* start off normal */
	if (tp->snd_cwnd <= sysctl_tcp_bic_low_window)
		tp->bictcp.cnt = tp->snd_cwnd;

	/* binary increase */
	else if (tp->snd_cwnd < tp->bictcp.last_max_cwnd) {
		__u32 	dist = (tp->bictcp.last_max_cwnd - tp->snd_cwnd)
			/ BICTCP_B;

		if (dist > BICTCP_MAX_INCREMENT)
			/* linear increase */
			tp->bictcp.cnt = tp->snd_cwnd / BICTCP_MAX_INCREMENT;
		else if (dist <= 1U)
			/* binary search increase */
			tp->bictcp.cnt = tp->snd_cwnd * BICTCP_FUNC_OF_MIN_INCR
				/ BICTCP_B;
		else
			/* binary search increase */
			tp->bictcp.cnt = tp->snd_cwnd / dist;
	} else {
		/* slow start amd linear increase */
		if (tp->snd_cwnd < tp->bictcp.last_max_cwnd + BICTCP_B)
			/* slow start */
			tp->bictcp.cnt = tp->snd_cwnd * BICTCP_FUNC_OF_MIN_INCR
				/ BICTCP_B;
		else if (tp->snd_cwnd < tp->bictcp.last_max_cwnd
			 		+ BICTCP_MAX_INCREMENT*(BICTCP_B-1))
			/* slow start */
			tp->bictcp.cnt = tp->snd_cwnd * (BICTCP_B-1)
				/ (tp->snd_cwnd-tp->bictcp.last_max_cwnd);
		else
			/* linear increase */
			tp->bictcp.cnt = tp->snd_cwnd / BICTCP_MAX_INCREMENT;
	}
	return tp->bictcp.cnt;
}

/* This is Jacobson's slow start and congestion avoidance. 
 * SIGCOMM '88, p. 328.
 */
static inline void reno_cong_avoid(struct tcp_sock *tp)
{
        if (tp->snd_cwnd <= tp->snd_ssthresh) {
                /* In "safe" area, increase. */
		if (tp->snd_cwnd < tp->snd_cwnd_clamp)
			tp->snd_cwnd++;
	} else {
                /* In dangerous area, increase slowly.
		 * In theory this is tp->snd_cwnd += 1 / tp->snd_cwnd
		 */
		if (tp->snd_cwnd_cnt >= bictcp_cwnd(tp)) {
			if (tp->snd_cwnd < tp->snd_cwnd_clamp)
				tp->snd_cwnd++;
			tp->snd_cwnd_cnt=0;
		} else
			tp->snd_cwnd_cnt++;
        }
	tp->snd_cwnd_stamp = tcp_time_stamp;
}

/* This is based on the congestion detection/avoidance scheme described in
 *    Lawrence S. Brakmo and Larry L. Peterson.
 *    "TCP Vegas: End to end congestion avoidance on a global internet."
 *    IEEE Journal on Selected Areas in Communication, 13(8):1465--1480,
 *    October 1995. Available from:
 *	ftp://ftp.cs.arizona.edu/xkernel/Papers/jsac.ps
 *
 * See http://www.cs.arizona.edu/xkernel/ for their implementation.
 * The main aspects that distinguish this implementation from the
 * Arizona Vegas implementation are:
 *   o We do not change the loss detection or recovery mechanisms of
 *     Linux in any way. Linux already recovers from losses quite well,
 *     using fine-grained timers, NewReno, and FACK.
 *   o To avoid the performance penalty imposed by increasing cwnd
 *     only every-other RTT during slow start, we increase during
 *     every RTT during slow start, just like Reno.
 *   o Largely to allow continuous cwnd growth during slow start,
 *     we use the rate at which ACKs come back as the "actual"
 *     rate, rather than the rate at which data is sent.
 *   o To speed convergence to the right rate, we set the cwnd
 *     to achieve the right ("actual") rate when we exit slow start.
 *   o To filter out the noise caused by delayed ACKs, we use the
 *     minimum RTT sample observed during the last RTT to calculate
 *     the actual rate.
 *   o When the sender re-starts from idle, it waits until it has
 *     received ACKs for an entire flight of new data before making
 *     a cwnd adjustment decision. The original Vegas implementation
 *     assumed senders never went idle.
 */
static void vegas_cong_avoid(struct tcp_sock *tp, u32 ack, u32 seq_rtt)
{
	/* The key players are v_beg_snd_una and v_beg_snd_nxt.
	 *
	 * These are so named because they represent the approximate values
	 * of snd_una and snd_nxt at the beginning of the current RTT. More
	 * precisely, they represent the amount of data sent during the RTT.
	 * At the end of the RTT, when we receive an ACK for v_beg_snd_nxt,
	 * we will calculate that (v_beg_snd_nxt - v_beg_snd_una) outstanding
	 * bytes of data have been ACKed during the course of the RTT, giving
	 * an "actual" rate of:
	 *
	 *     (v_beg_snd_nxt - v_beg_snd_una) / (rtt duration)
	 *
	 * Unfortunately, v_beg_snd_una is not exactly equal to snd_una,
	 * because delayed ACKs can cover more than one segment, so they
	 * don't line up nicely with the boundaries of RTTs.
	 *
	 * Another unfortunate fact of life is that delayed ACKs delay the
	 * advance of the left edge of our send window, so that the number
	 * of bytes we send in an RTT is often less than our cwnd will allow.
	 * So we keep track of our cwnd separately, in v_beg_snd_cwnd.
	 */

	if (after(ack, tp->vegas.beg_snd_nxt)) {
		/* Do the Vegas once-per-RTT cwnd adjustment. */
		u32 old_wnd, old_snd_cwnd;

		
		/* Here old_wnd is essentially the window of data that was
		 * sent during the previous RTT, and has all
		 * been acknowledged in the course of the RTT that ended
		 * with the ACK we just received. Likewise, old_snd_cwnd
		 * is the cwnd during the previous RTT.
		 */
		old_wnd = (tp->vegas.beg_snd_nxt - tp->vegas.beg_snd_una) /
			tp->mss_cache_std;
		old_snd_cwnd = tp->vegas.beg_snd_cwnd;

		/* Save the extent of the current window so we can use this
		 * at the end of the next RTT.
		 */
		tp->vegas.beg_snd_una  = tp->vegas.beg_snd_nxt;
		tp->vegas.beg_snd_nxt  = tp->snd_nxt;
		tp->vegas.beg_snd_cwnd = tp->snd_cwnd;

		/* Take into account the current RTT sample too, to
		 * decrease the impact of delayed acks. This double counts
		 * this sample since we count it for the next window as well,
		 * but that's not too awful, since we're taking the min,
		 * rather than averaging.
		 */
		vegas_rtt_calc(tp, seq_rtt);

		/* We do the Vegas calculations only if we got enough RTT
		 * samples that we can be reasonably sure that we got
		 * at least one RTT sample that wasn't from a delayed ACK.
		 * If we only had 2 samples total,
		 * then that means we're getting only 1 ACK per RTT, which
		 * means they're almost certainly delayed ACKs.
		 * If  we have 3 samples, we should be OK.
		 */

		if (tp->vegas.cntRTT <= 2) {
			/* We don't have enough RTT samples to do the Vegas
			 * calculation, so we'll behave like Reno.
			 */
			if (tp->snd_cwnd > tp->snd_ssthresh)
				tp->snd_cwnd++;
		} else {
			u32 rtt, target_cwnd, diff;

			/* We have enough RTT samples, so, using the Vegas
			 * algorithm, we determine if we should increase or
			 * decrease cwnd, and by how much.
			 */

			/* Pluck out the RTT we are using for the Vegas
			 * calculations. This is the min RTT seen during the
			 * last RTT. Taking the min filters out the effects
			 * of delayed ACKs, at the cost of noticing congestion
			 * a bit later.
			 */
			rtt = tp->vegas.minRTT;

			/* Calculate the cwnd we should have, if we weren't
			 * going too fast.
			 *
			 * This is:
			 *     (actual rate in segments) * baseRTT
			 * We keep it as a fixed point number with
			 * V_PARAM_SHIFT bits to the right of the binary point.
			 */
			target_cwnd = ((old_wnd * tp->vegas.baseRTT)
				       << V_PARAM_SHIFT) / rtt;

			/* Calculate the difference between the window we had,
			 * and the window we would like to have. This quantity
			 * is the "Diff" from the Arizona Vegas papers.
			 *
			 * Again, this is a fixed point number with
			 * V_PARAM_SHIFT bits to the right of the binary
			 * point.
			 */
			diff = (old_wnd << V_PARAM_SHIFT) - target_cwnd;

			if (tp->snd_cwnd < tp->snd_ssthresh) {
				/* Slow start.  */
				if (diff > sysctl_tcp_vegas_gamma) {
					/* Going too fast. Time to slow down
					 * and switch to congestion avoidance.
					 */
					tp->snd_ssthresh = 2;

					/* Set cwnd to match the actual rate
					 * exactly:
					 *   cwnd = (actual rate) * baseRTT
					 * Then we add 1 because the integer
					 * truncation robs us of full link
					 * utilization.
					 */
					tp->snd_cwnd = min(tp->snd_cwnd,
							   (target_cwnd >>
							    V_PARAM_SHIFT)+1);

				}
			} else {
				/* Congestion avoidance. */
				u32 next_snd_cwnd;

				/* Figure out where we would like cwnd
				 * to be.
				 */
				if (diff > sysctl_tcp_vegas_beta) {
					/* The old window was too fast, so
					 * we slow down.
					 */
					next_snd_cwnd = old_snd_cwnd - 1;
				} else if (diff < sysctl_tcp_vegas_alpha) {
					/* We don't have enough extra packets
					 * in the network, so speed up.
					 */
					next_snd_cwnd = old_snd_cwnd + 1;
				} else {
					/* Sending just as fast as we
					 * should be.
					 */
					next_snd_cwnd = old_snd_cwnd;
				}

				/* Adjust cwnd upward or downward, toward the
				 * desired value.
				 */
				if (next_snd_cwnd > tp->snd_cwnd)
					tp->snd_cwnd++;
				else if (next_snd_cwnd < tp->snd_cwnd)
					tp->snd_cwnd--;
			}
		}

		/* Wipe the slate clean for the next RTT. */
		tp->vegas.cntRTT = 0;
		tp->vegas.minRTT = 0x7fffffff;
	}

	/* The following code is executed for every ack we receive,
	 * except for conditions checked in should_advance_cwnd()
	 * before the call to tcp_cong_avoid(). Mainly this means that
	 * we only execute this code if the ack actually acked some
	 * data.
	 */

	/* If we are in slow start, increase our cwnd in response to this ACK.
	 * (If we are not in slow start then we are in congestion avoidance,
	 * and adjust our congestion window only once per RTT. See the code
	 * above.)
	 */
	if (tp->snd_cwnd <= tp->snd_ssthresh) 
		tp->snd_cwnd++;

	/* to keep cwnd from growing without bound */
	tp->snd_cwnd = min_t(u32, tp->snd_cwnd, tp->snd_cwnd_clamp);

	/* Make sure that we are never so timid as to reduce our cwnd below
	 * 2 MSS.
	 *
	 * Going below 2 MSS would risk huge delayed ACKs from our receiver.
	 */
	tp->snd_cwnd = max(tp->snd_cwnd, 2U);

	tp->snd_cwnd_stamp = tcp_time_stamp;
}

/* 拥塞避免，通过调用当前的拥塞控制算法重新计算拥塞窗口 */
static inline void tcp_cong_avoid(struct tcp_sock *tp, u32 ack, u32 seq_rtt)
{
	if (tcp_vegas_enabled(tp))
		vegas_cong_avoid(tp, ack, seq_rtt);
	else
		reno_cong_avoid(tp);
}

/* Restart timer after forward progress on connection.
 * RFC2988 recommends to restart timer to now+rto.
 */

static inline void tcp_ack_packets_out(struct sock *sk, struct tcp_sock *tp)
{
	if (!tp->packets_out) {
		tcp_clear_xmit_timer(sk, TCP_TIME_RETRANS);
	} else {
		tcp_reset_xmit_timer(sk, TCP_TIME_RETRANS, tp->rto);
	}
}

/* There is one downside to this scheme.  Although we keep the
 * ACK clock ticking, adjusting packet counters and advancing
 * congestion window, we do not liberate socket send buffer
 * space.
 *
 * Mucking with skb->truesize and sk->sk_wmem_alloc et al.
 * then making a write space wakeup callback is a possible
 * future enhancement.  WARNING: it is not trivial to make.
 */
static int tcp_tso_acked(struct sock *sk, struct sk_buff *skb,
			 __u32 now, __s32 *seq_rtt)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_skb_cb *scb = TCP_SKB_CB(skb); 
	__u32 seq = tp->snd_una;
	__u32 packets_acked;
	int acked = 0;

	/* If we get here, the whole TSO packet has not been
	 * acked.
	 */
	BUG_ON(!after(scb->end_seq, seq));

	packets_acked = tcp_skb_pcount(skb);
	if (tcp_trim_head(sk, skb, seq - scb->seq))
		return 0;
	packets_acked -= tcp_skb_pcount(skb);

	if (packets_acked) {
		__u8 sacked = scb->sacked;

		acked |= FLAG_DATA_ACKED;
		if (sacked) {
			if (sacked & TCPCB_RETRANS) {
				if (sacked & TCPCB_SACKED_RETRANS)
					tp->retrans_out -= packets_acked;
				acked |= FLAG_RETRANS_DATA_ACKED;
				*seq_rtt = -1;
			} else if (*seq_rtt < 0)
				*seq_rtt = now - scb->when;
			if (sacked & TCPCB_SACKED_ACKED)
				tp->sacked_out -= packets_acked;
			if (sacked & TCPCB_LOST)
				tp->lost_out -= packets_acked;
			if (sacked & TCPCB_URG) {
				if (tp->urg_mode &&
				    !before(seq, tp->snd_up))
					tp->urg_mode = 0;
			}
		} else if (*seq_rtt < 0)
			*seq_rtt = now - scb->when;

		if (tp->fackets_out) {
			__u32 dval = min(tp->fackets_out, packets_acked);
			tp->fackets_out -= dval;
		}
		tp->packets_out -= packets_acked;

		BUG_ON(tcp_skb_pcount(skb) == 0);
		BUG_ON(!before(scb->seq, scb->end_seq));
	}

	return acked;
}


/* Remove acknowledged frames from the retransmission queue. */
/* 删除重传队列中已经确认的段 */
static int tcp_clean_rtx_queue(struct sock *sk, __s32 *seq_rtt_p)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;
	__u32 now = tcp_time_stamp;
	int acked = 0;
	__s32 seq_rtt = -1;

	while ((skb = skb_peek(&sk->sk_write_queue)) &&
	       skb != sk->sk_send_head) {/* 遍历重传队列 */
		struct tcp_skb_cb *scb = TCP_SKB_CB(skb); 
		__u8 sacked = scb->sacked;

		/* If our packet is before the ack sequence we can
		 * discard it as it's confirmed to have arrived at
		 * the other end.
		 */
		if (after(scb->end_seq, tp->snd_una)) {/* 当前段位于una之后，只确认了段的一部分 */
			if (tcp_skb_pcount(skb) > 1)/* 是TSO段 */
				acked |= tcp_tso_acked(sk, skb,
						       now, &seq_rtt);/* 将已经确认的部分从TCP段中删除，同时更新SBK中GSO破关信息 */
			break;
		}

		/* Initial outgoing SYN's get put onto the write_queue
		 * just like anything else we transmit.  It is not
		 * true data, and if we misinform our callers that
		 * this ACK acks real data, we will erroneously exit
		 * connection startup slow start one packet too
		 * quickly.  This is severely frowned upon behavior.
		 */
		/* 以下的段都是被完全确认的。设置acked标志 */
		if (!(scb->flags & TCPCB_FLAG_SYN)) {
			acked |= FLAG_DATA_ACKED;
		} else {
			acked |= FLAG_SYN_ACKED;
			tp->retrans_stamp = 0;
		}

		if (sacked) {
			if (sacked & TCPCB_RETRANS) {/* 段重传过 */
				if(sacked & TCPCB_SACKED_RETRANS)
					tp->retrans_out -= tcp_skb_pcount(skb);
				acked |= FLAG_RETRANS_DATA_ACKED;
				seq_rtt = -1;
			} else if (seq_rtt < 0)/* 段没有重传过，获取往返时间和时间戳 */
				seq_rtt = now - scb->when;
			if (sacked & TCPCB_SACKED_ACKED)
				tp->sacked_out -= tcp_skb_pcount(skb);
			if (sacked & TCPCB_LOST)
				tp->lost_out -= tcp_skb_pcount(skb);
			if (sacked & TCPCB_URG) {/* 段中存在带外数据 */
				if (tp->urg_mode &&/* tcp目前还处于紧急模式 */
				    !before(scb->end_seq, tp->snd_up))/* 带外数据位于上次指示的带外数据之后 */
					tp->urg_mode = 0;/* 用户已经应答了带外数据，取消紧急模式 */
			}
		} else if (seq_rtt < 0)/* 未获得段的往返时间 */
			seq_rtt = now - scb->when;/* 以发送该段与接收到该段的ACK之间的时间作为往返回时间 */
		/* 当前段已经确认过了，因此调用fackets_out，并将段从重传队列中删除并释放 */
		tcp_dec_pcount_approx(&tp->fackets_out, skb);
		tcp_packets_out_dec(tp, skb);
		__skb_unlink(skb, skb->list);
		sk_stream_free_skb(sk, skb);
	}

	if (acked&FLAG_ACKED) {/* 本次处理有对数据和SYN的确认 */
		/* 测量更新往返时间，并确认是否启动重传定时器 */
		tcp_ack_update_rtt(tp, acked, seq_rtt);
		tcp_ack_packets_out(sk, tp);
	}

#if FASTRETRANS_DEBUG > 0
	BUG_TRAP((int)tp->sacked_out >= 0);
	BUG_TRAP((int)tp->lost_out >= 0);
	BUG_TRAP((int)tp->retrans_out >= 0);
	if (!tp->packets_out && tp->rx_opt.sack_ok) {
		if (tp->lost_out) {
			printk(KERN_DEBUG "Leak l=%u %d\n",
			       tp->lost_out, tp->ca_state);
			tp->lost_out = 0;
		}
		if (tp->sacked_out) {
			printk(KERN_DEBUG "Leak s=%u %d\n",
			       tp->sacked_out, tp->ca_state);
			tp->sacked_out = 0;
		}
		if (tp->retrans_out) {
			printk(KERN_DEBUG "Leak r=%u %d\n",
			       tp->retrans_out, tp->ca_state);
			tp->retrans_out = 0;
		}
	}
#endif
	*seq_rtt_p = seq_rtt;
	return acked;
}

static void tcp_ack_probe(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	/* Was it a usable window open? */

	if (!after(TCP_SKB_CB(sk->sk_send_head)->end_seq,
		   tp->snd_una + tp->snd_wnd)) {
		tp->backoff = 0;
		tcp_clear_xmit_timer(sk, TCP_TIME_PROBE0);
		/* Socket must be waked up by subsequent tcp_data_snd_check().
		 * This function is not for random using!
		 */
	} else {
		tcp_reset_xmit_timer(sk, TCP_TIME_PROBE0,
				     min(tp->rto << tp->backoff, TCP_RTO_MAX));
	}
}

static inline int tcp_ack_is_dubious(struct tcp_sock *tp, int flag)
{
	return (!(flag & FLAG_NOT_DUP) || (flag & FLAG_CA_ALERT) ||
		tp->ca_state != TCP_CA_Open);
}

static inline int tcp_may_raise_cwnd(struct tcp_sock *tp, int flag)
{
	return (!(flag & FLAG_ECE) || tp->snd_cwnd < tp->snd_ssthresh) &&
		!((1<<tp->ca_state)&(TCPF_CA_Recovery|TCPF_CA_CWR));
}

/* Check that window update is acceptable.
 * The function assumes that snd_una<=ack<=snd_next.
 */
static inline int tcp_may_update_window(struct tcp_sock *tp, u32 ack,
					u32 ack_seq, u32 nwin)
{
	return (after(ack, tp->snd_una) ||
		after(ack_seq, tp->snd_wl1) ||
		(ack_seq == tp->snd_wl1 && nwin > tp->snd_wnd));
}

/* Update our send window.
 *
 * Window update algorithm, described in RFC793/RFC1122 (used in linux-2.2
 * and in FreeBSD. NetBSD's one is even worse.) is wrong.
 */
/* 更新发送窗口 */
static int tcp_ack_update_window(struct sock *sk, struct tcp_sock *tp,
				 struct sk_buff *skb, u32 ack, u32 ack_seq)
{
	int flag = 0;
	/* 从TCP首部中获取接收方接收窗口大小 */
	u32 nwin = ntohs(skb->h.th->window);

	if (likely(!skb->h.th->syn))/* 并由窗口扩大因子计算出接收窗口的字节数 */
		nwin <<= tp->rx_opt.snd_wscale;

	if (tcp_may_update_window(tp, ack, ack_seq, nwin)) {/* 判断是否需要更新发送方的发送窗口 */
		flag |= FLAG_WIN_UPDATE;/* 更新标记 */
		tcp_update_wl(tp, ack, ack_seq);/* 记录最新ACK序号 */

		if (tp->snd_wnd != nwin) {/* 接收方的接收窗口与发送方的发送窗口不相等 */
			/* 以发送方的发送窗口为准 */
			tp->snd_wnd = nwin;

			/* Note, it is the only place, where
			 * fast path is recovered for sending TCP.
			 */
			tcp_fast_path_check(sk, tp);

			if (nwin > tp->max_window) {/* 接收方的接收窗口超过最大接收窗口 */
				/* 更新最大接收窗口，并重新计算MSS */
				tp->max_window = nwin;
				tcp_sync_mss(sk, tp->pmtu_cookie);
			}
		}
	}

	/* 更新发送窗口左端 */
	tp->snd_una = ack;

	return flag;
}

/* 当处于FRTO阶段时，确认段是否真的丢失，以及传送超时是不是虚假的 */
static void tcp_process_frto(struct sock *sk, u32 prior_snd_una)
{
	struct tcp_sock *tp = tcp_sk(sk);

	/* 接收到ACK后，刷新没有确认的TCP段数量 */
	tcp_sync_left_out(tp);
	
	if (tp->snd_una == prior_snd_una ||
	    !before(tp->snd_una, tp->frto_highmark)) {/* 接收到的ACK是重复的，说明传送超时是真的 */
		/* RTO was caused by loss, start retransmitting in
		 * go-back-N slow start
		 */
		tcp_enter_frto_loss(sk);/* 进入拥塞恢复阶段 */
		return;
	}

	if (tp->frto_counter == 1) {/* 进入FRTO后接收的第一个ACK */
		/* First ACK after RTO advances the window: allow two new
		 * segments out.
		 */
		/* 重新设置拥塞窗口，允许再发送两个报文 */
		tp->snd_cwnd = tcp_packets_in_flight(tp) + 2;
	} else {/* 进入FRTO后，接收到的第二个ACK */
		/* Also the second ACK after RTO advances the window.
		 * The RTO was likely spurious. Reduce cwnd and continue
		 * in congestion avoidance
		 */
		/* 进一步调整拥塞窗口 */
		tp->snd_cwnd = min(tp->snd_cwnd, tp->snd_ssthresh);
		tcp_moderate_cwnd(tp);
	}

	/* F-RTO affects on two new ACKs following RTO.
	 * At latest on third ACK the TCP behavor is back to normal.
	 */
	/* 增加计数，如果连续接收到两个对新数据的确认，则说明传送超时是虚假的，退出FRTO恢复 */
	tp->frto_counter = (tp->frto_counter + 1) % 3;
}

/*
 * TCP Westwood+
 */

/*
 * @init_westwood
 * This function initializes fields used in TCP Westwood+. We can't
 * get no information about RTTmin at this time so we simply set it to
 * TCP_WESTWOOD_INIT_RTT. This value was chosen to be too conservative
 * since in this way we're sure it will be updated in a consistent
 * way as soon as possible. It will reasonably happen within the first
 * RTT period of the connection lifetime.
 */

static void init_westwood(struct sock *sk)
{
        struct tcp_sock *tp = tcp_sk(sk);

        tp->westwood.bw_ns_est = 0;
        tp->westwood.bw_est = 0;
        tp->westwood.accounted = 0;
        tp->westwood.cumul_ack = 0;
        tp->westwood.rtt_win_sx = tcp_time_stamp;
        tp->westwood.rtt = TCP_WESTWOOD_INIT_RTT;
        tp->westwood.rtt_min = TCP_WESTWOOD_INIT_RTT;
        tp->westwood.snd_una = tp->snd_una;
}

/*
 * @westwood_do_filter
 * Low-pass filter. Implemented using constant coeffients.
 */

static inline __u32 westwood_do_filter(__u32 a, __u32 b)
{
	return (((7 * a) + b) >> 3);
}

static void westwood_filter(struct sock *sk, __u32 delta)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tp->westwood.bw_ns_est =
		westwood_do_filter(tp->westwood.bw_ns_est, 
				   tp->westwood.bk / delta);
	tp->westwood.bw_est =
		westwood_do_filter(tp->westwood.bw_est,
				   tp->westwood.bw_ns_est);
}

/* 
 * @westwood_update_rttmin
 * It is used to update RTTmin. In this case we MUST NOT use
 * WESTWOOD_RTT_MIN minimum bound since we could be on a LAN!
 */

static inline __u32 westwood_update_rttmin(const struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	__u32 rttmin = tp->westwood.rtt_min;

	if (tp->westwood.rtt != 0 &&
	    (tp->westwood.rtt < tp->westwood.rtt_min || !rttmin))
		rttmin = tp->westwood.rtt;

	return rttmin;
}

/*
 * @westwood_acked
 * Evaluate increases for dk. 
 */

static inline __u32 westwood_acked(const struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return tp->snd_una - tp->westwood.snd_una;
}

/*
 * @westwood_new_window
 * It evaluates if we are receiving data inside the same RTT window as
 * when we started.
 * Return value:
 * It returns 0 if we are still evaluating samples in the same RTT
 * window, 1 if the sample has to be considered in the next window.
 */

static int westwood_new_window(const struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	__u32 left_bound;
	__u32 rtt;
	int ret = 0;

	left_bound = tp->westwood.rtt_win_sx;
	rtt = max(tp->westwood.rtt, (u32) TCP_WESTWOOD_RTT_MIN);

	/*
	 * A RTT-window has passed. Be careful since if RTT is less than
	 * 50ms we don't filter but we continue 'building the sample'.
	 * This minimum limit was choosen since an estimation on small
	 * time intervals is better to avoid...
	 * Obvioulsy on a LAN we reasonably will always have
	 * right_bound = left_bound + WESTWOOD_RTT_MIN
         */

	if ((left_bound + rtt) < tcp_time_stamp)
		ret = 1;

	return ret;
}

/*
 * @westwood_update_window
 * It updates RTT evaluation window if it is the right moment to do
 * it. If so it calls filter for evaluating bandwidth. 
 */

static void __westwood_update_window(struct sock *sk, __u32 now)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 delta = now - tp->westwood.rtt_win_sx;

        if (delta) {
		if (tp->westwood.rtt)
			westwood_filter(sk, delta);

		tp->westwood.bk = 0;
		tp->westwood.rtt_win_sx = tcp_time_stamp;
	}
}


static void westwood_update_window(struct sock *sk, __u32 now)
{
	if (westwood_new_window(sk)) 
		__westwood_update_window(sk, now);
}

/*
 * @__tcp_westwood_fast_bw
 * It is called when we are in fast path. In particular it is called when
 * header prediction is successfull. In such case infact update is
 * straight forward and doesn't need any particular care.
 */

static void __tcp_westwood_fast_bw(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);

	westwood_update_window(sk, tcp_time_stamp);

	tp->westwood.bk += westwood_acked(sk);
	tp->westwood.snd_una = tp->snd_una;
	tp->westwood.rtt_min = westwood_update_rttmin(sk);
}

static inline void tcp_westwood_fast_bw(struct sock *sk, struct sk_buff *skb)
{
        if (tcp_is_westwood(tcp_sk(sk)))
                __tcp_westwood_fast_bw(sk, skb);
}


/*
 * @westwood_dupack_update
 * It updates accounted and cumul_ack when receiving a dupack.
 */

static void westwood_dupack_update(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tp->westwood.accounted += tp->mss_cache_std;
	tp->westwood.cumul_ack = tp->mss_cache_std;
}

static inline int westwood_may_change_cumul(struct tcp_sock *tp)
{
	return (tp->westwood.cumul_ack > tp->mss_cache_std);
}

static inline void westwood_partial_update(struct tcp_sock *tp)
{
	tp->westwood.accounted -= tp->westwood.cumul_ack;
	tp->westwood.cumul_ack = tp->mss_cache_std;
}

static inline void westwood_complete_update(struct tcp_sock *tp)
{
	tp->westwood.cumul_ack -= tp->westwood.accounted;
	tp->westwood.accounted = 0;
}

/*
 * @westwood_acked_count
 * This function evaluates cumul_ack for evaluating dk in case of
 * delayed or partial acks.
 */

static inline __u32 westwood_acked_count(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tp->westwood.cumul_ack = westwood_acked(sk);

        /* If cumul_ack is 0 this is a dupack since it's not moving
         * tp->snd_una.
         */
        if (!(tp->westwood.cumul_ack))
                westwood_dupack_update(sk);

        if (westwood_may_change_cumul(tp)) {
		/* Partial or delayed ack */
		if (tp->westwood.accounted >= tp->westwood.cumul_ack)
			westwood_partial_update(tp);
		else
			westwood_complete_update(tp);
	}

	tp->westwood.snd_una = tp->snd_una;

	return tp->westwood.cumul_ack;
}


/*
 * @__tcp_westwood_slow_bw
 * It is called when something is going wrong..even if there could
 * be no problems! Infact a simple delayed packet may trigger a
 * dupack. But we need to be careful in such case.
 */

static void __tcp_westwood_slow_bw(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);

	westwood_update_window(sk, tcp_time_stamp);

	tp->westwood.bk += westwood_acked_count(sk);
	tp->westwood.rtt_min = westwood_update_rttmin(sk);
}

static inline void tcp_westwood_slow_bw(struct sock *sk, struct sk_buff *skb)
{
        if (tcp_is_westwood(tcp_sk(sk)))
                __tcp_westwood_slow_bw(sk, skb);
}

/* This routine deals with incoming acks, but not outgoing ones. */
/* 处理接收到的ack报文 */
static int tcp_ack(struct sock *sk, struct sk_buff *skb, int flag)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 prior_snd_una = tp->snd_una;
	u32 ack_seq = TCP_SKB_CB(skb)->seq;
	u32 ack = TCP_SKB_CB(skb)->ack_seq;
	u32 prior_in_flight;
	s32 seq_rtt;
	int prior_packets;

	/* If the ack is newer than sent or older than previous acks
	 * then we can probably ignore it.
	 */
	if (after(ack, tp->snd_nxt))/* 确认的序号大于nxt，不合法 */
		goto uninteresting_ack;

	if (before(ack, prior_snd_una))/* 确认的序号小于una，不合法 */
		goto old_ack;

	if (!(flag&FLAG_SLOWPATH) && after(ack, prior_snd_una)) {/* 快速路径，并且更新了una */
		/* Window is constant, pure forward advance.
		 * No more checks are required.
		 * Note, we use the fact that SND.UNA>=SND.WL2.
		 */
		/* 更新发送窗口左边界 */
		tcp_update_wl(tp, ack, ack_seq);
		tp->snd_una = ack;
		/* 通知拥塞控制算法本次ACK是快速路径 */
		tcp_westwood_fast_bw(sk, skb);
		flag |= FLAG_WIN_UPDATE;

		NET_INC_STATS_BH(LINUX_MIB_TCPHPACKS);
	} else {/* 慢速路径 */
		if (ack_seq != TCP_SKB_CB(skb)->end_seq)/* 报文中包含有数据 */
			flag |= FLAG_DATA;
		else
			NET_INC_STATS_BH(LINUX_MIB_TCPPUREACKS);

		/* 更新发送窗口 */
		flag |= tcp_ack_update_window(sk, tp, skb, ack, ack_seq);

		if (TCP_SKB_CB(skb)->sacked)/* 如果有sack标志，则标记重传队列 */
			flag |= tcp_sacktag_write_queue(sk, skb, prior_snd_una);

		if (TCP_ECN_rcv_ecn_echo(tp, skb->h.th))/* 如果ACK段中存在ECE标志 */
			flag |= FLAG_ECE;

		/* 通知拥塞控制算法本次ACK是慢速路径 */
		tcp_westwood_slow_bw(sk,skb);
	}

	/* We passed data and got it acked, remove any soft error
	 * log. Something worked...
	 */
	sk->sk_err_soft = 0;
	/* 记录最近一次收到ACK的时间 */
	tp->rcv_tstamp = tcp_time_stamp;
	prior_packets = tp->packets_out;
	if (!prior_packets)/* 如果没有已经发出但还没有确认的段 */
		goto no_queue;

	/* 正在传输的段数 */
	prior_in_flight = tcp_packets_in_flight(tp);

	/* See if we can take anything off of the retransmit queue. */
	/* 在重传队列中删除已确认的段 */
	flag |= tcp_clean_rtx_queue(sk, &seq_rtt);

	if (tp->frto_counter)/* 当前处于FRTO阶段，进行处理以判断是否真的超时 */
		tcp_process_frto(sk, prior_snd_una);

	/* 不明确????或者拥塞状态不是open，则进行拥塞状态机状态的迁移。 */
	if (tcp_ack_is_dubious(tp, flag)) {/* 接收到的ACK是重复的，或者接收到SACK块或显式拥塞通知，或者当前状态不是open */
		/* Advanve CWND, if state allows this. */
		if ((flag & FLAG_DATA_ACKED) &&/* 确认了新的段 */
		    (tcp_vegas_enabled(tp) || prior_in_flight >= tp->snd_cwnd) &&
		    tcp_may_raise_cwnd(tp, flag))/* 拥塞窗口可以更新 */
			tcp_cong_avoid(tp, ack, seq_rtt);/* 进行拥塞避免 */
		/* 拥塞控制状态的处理 */
		tcp_fastretrans_alert(sk, prior_snd_una, prior_packets, flag);
	} else {
		if ((flag & FLAG_DATA_ACKED) && /* 确认了新的段 */
		    (tcp_vegas_enabled(tp) || prior_in_flight >= tp->snd_cwnd))
			tcp_cong_avoid(tp, ack, seq_rtt);/* 进行拥塞避免 */
	}

	/* 如果确认了新的段，或者接收到的ACK是重复的，则确认输出路由是有效的 */
	if ((flag & FLAG_FORWARD_PROGRESS) || !(flag&FLAG_NOT_DUP))
		dst_confirm(sk->sk_dst_cache);

	return 1;

no_queue:
	/* 由于接收到对端的ack，因此将TCP保活探测段未确认数清0，说明此时TCP连接是正常的 */
	tp->probes_out = 0;

	/* If this ack opens up a zero window, clear backoff.  It was
	 * being used to time the probes, and is probably far higher than
	 * it needs to be for normal retransmission.
	 */
	if (sk->sk_send_head)/* 还有待发送的数据 */
		/* 接收到ack后，如果对方接收窗口没有关闭，则清除持续定时器中指数退避算法指数，停止持续定时器，否则开启持续定时器 */
		tcp_ack_probe(sk);/* tcp_ack_probe用来确定是否需要进行0窗口探测 */
	return 1;

old_ack:
	if (TCP_SKB_CB(skb)->sacked)/* 如果是已经确认过的ACK，并且其中带有SACK选项信息，则标记重传队列中各个段的记分牌 */
		tcp_sacktag_write_queue(sk, skb, prior_snd_una);

uninteresting_ack:
	SOCK_DEBUG(sk, "Ack %u out of %u:%u\n", ack, tp->snd_una, tp->snd_nxt);
	return 0;
}


/* Look for tcp options. Normally only called on SYN and SYNACK packets.
 * But, this can also be called on packets in the established flow when
 * the fast version below fails.
 */
/* 完整的解析时间戳选项，除了在分析SYN和SYN+ACK段时调用外，在慢速流程中也调用，以解析除时间戳外的其他选项 */
void tcp_parse_options(struct sk_buff *skb, struct tcp_options_received *opt_rx, int estab)
{
	unsigned char *ptr;
	struct tcphdr *th = skb->h.th;
	int length=(th->doff*4)-sizeof(struct tcphdr);

	ptr = (unsigned char *)(th + 1);
	opt_rx->saw_tstamp = 0;

	while(length>0) {/* 遍历选项，直到所有选项分析完毕 */
	  	int opcode=*ptr++;/* 选项类型 */
		int opsize;

		switch (opcode) {
			case TCPOPT_EOL:
				return;/* 选项结束，返回 */
			case TCPOPT_NOP:	/* Ref: RFC 793 section 3.1 */
				length--;
				continue;/* NOP选项，直接跳到下一个选项 */
			default:
				opsize=*ptr++;/* 获得选项长度，并判断其合法性  */
				if (opsize < 2) /* "silly options" */
					return;
				if (opsize > length)/* 超过选项总长度，非法选项 */
					return;	/* don't parse partial options */
	  			switch(opcode) {
				case TCPOPT_MSS:/* MSS通告选项 */
					if(opsize==TCPOLEN_MSS && th->syn && !estab) {/* 此选项仅能出现在SYN段中,并且不能在接收报文阶段调用 */
						u16 in_mss = ntohs(*(__u16 *)ptr);
						if (in_mss) {/* 获取通告窗口大小 */
							/* 如果通告窗口大于用户设置的窗口，则以设置的窗口为准 */
							if (opt_rx->user_mss && opt_rx->user_mss < in_mss)
								in_mss = opt_rx->user_mss;
							opt_rx->mss_clamp = in_mss;/* 设置MSS最大值 */
						}
					}
					break;
				case TCPOPT_WINDOW:
					/* 扩大因子也只能出现在SYN段中 */
					if(opsize==TCPOLEN_WINDOW && th->syn && !estab)
						if (sysctl_tcp_window_scaling) {
							opt_rx->wscale_ok = 1;
							opt_rx->snd_wscale = *(__u8 *)ptr;
							if(opt_rx->snd_wscale > 14) {/* 窗口扩大因子不能大于14 */
								if(net_ratelimit())
									printk(KERN_INFO "tcp_parse_options: Illegal window "
									       "scaling value %d >14 received.\n",
									       opt_rx->snd_wscale);
								opt_rx->snd_wscale = 14;
							}
						}
					break;
				case TCPOPT_TIMESTAMP:
					if(opsize==TCPOLEN_TIMESTAMP) {/* 时间戳选项，判断其长度是否合法 */
						if ((estab && opt_rx->tstamp_ok) ||
						    (!estab && sysctl_tcp_timestamps)) {
							opt_rx->saw_tstamp = 1;
							opt_rx->rcv_tsval = ntohl(*(__u32 *)ptr);
							opt_rx->rcv_tsecr = ntohl(*(__u32 *)(ptr+4));
						}
					}
					break;
				case TCPOPT_SACK_PERM:
					/* SACK允许选项只能出现在SYN段中 */
					if(opsize==TCPOLEN_SACK_PERM && th->syn && !estab) {
						if (sysctl_tcp_sack) {
							opt_rx->sack_ok = 1;/* 允许SACK */
							tcp_sack_reset(opt_rx);
						}
					}
					break;

				case TCPOPT_SACK:
					if((opsize >= (TCPOLEN_SACK_BASE + TCPOLEN_SACK_PERBLOCK)) &&/* 至少包含一个SACK */
					   !((opsize - TCPOLEN_SACK_BASE) % TCPOLEN_SACK_PERBLOCK) &&/* SACK边界判断 */
					   opt_rx->sack_ok) {/* 允许SACK */
					   /* 将sacked指向SACK数据开始处 */
						TCP_SKB_CB(skb)->sacked = (ptr - 2) - (unsigned char *)th;
					}
	  			};
	  			ptr+=opsize-2;
	  			length-=opsize;
	  	};
	}
}

/* Fast parse options. This hopes to only see timestamps.
 * If it is wrong it falls back on tcp_parse_options().
 */
/* 在TCP慢速接收报文的阶段，调用此函数解析TCP选项 */
static inline int tcp_fast_parse_options(struct sk_buff *skb, struct tcphdr *th,
					 struct tcp_sock *tp)
{
	if (th->doff == sizeof(struct tcphdr)>>2) {/* 没有选项 */
		tp->rx_opt.saw_tstamp = 0;/* 将时间戳标志设置0后退出 */
		return 0;
	} else if (tp->rx_opt.tstamp_ok &&/* 启用时间戳选项 */
		   th->doff == (sizeof(struct tcphdr)>>2)+(TCPOLEN_TSTAMP_ALIGNED>>2)) {/* 并且仅仅只可能有时间戳选项 */
		__u32 *ptr = (__u32 *)(th + 1);
		if (*ptr == ntohl((TCPOPT_NOP << 24) | (TCPOPT_NOP << 16)
				  | (TCPOPT_TIMESTAMP << 8) | TCPOLEN_TIMESTAMP)) {/* 判断该选项是否是时间戳 */
			/* 获取时间戳的值 */
			tp->rx_opt.saw_tstamp = 1;
			++ptr;
			tp->rx_opt.rcv_tsval = ntohl(*ptr);
			++ptr;
			tp->rx_opt.rcv_tsecr = ntohl(*ptr);
			return 1;
		}
	}
	/* 除了时间戳外，还有其他选项。 */
	tcp_parse_options(skb, &tp->rx_opt, 1);
	return 1;
}

static inline void tcp_store_ts_recent(struct tcp_sock *tp)
{
	tp->rx_opt.ts_recent = tp->rx_opt.rcv_tsval;
	tp->rx_opt.ts_recent_stamp = xtime.tv_sec;
}

static inline void tcp_replace_ts_recent(struct tcp_sock *tp, u32 seq)
{
	if (tp->rx_opt.saw_tstamp && !after(seq, tp->rcv_wup)) {
		/* PAWS bug workaround wrt. ACK frames, the PAWS discard
		 * extra check below makes sure this can only happen
		 * for pure ACK frames.  -DaveM
		 *
		 * Not only, also it occurs for expired timestamps.
		 */

		if((s32)(tp->rx_opt.rcv_tsval - tp->rx_opt.ts_recent) >= 0 ||
		   xtime.tv_sec >= tp->rx_opt.ts_recent_stamp + TCP_PAWS_24DAYS)
			tcp_store_ts_recent(tp);
	}
}

/* Sorry, PAWS as specified is broken wrt. pure-ACKs -DaveM
 *
 * It is not fatal. If this ACK does _not_ change critical state (seqs, window)
 * it can pass through stack. So, the following predicate verifies that
 * this segment is not used for anything but congestion avoidance or
 * fast retransmit. Moreover, we even are able to eliminate most of such
 * second order effects, if we apply some small "replay" window (~RTO)
 * to timestamp space.
 *
 * All these measures still do not guarantee that we reject wrapped ACKs
 * on networks with high bandwidth, when sequence space is recycled fastly,
 * but it guarantees that such events will be very rare and do not affect
 * connection seriously. This doesn't look nice, but alas, PAWS is really
 * buggy extension.
 *
 * [ Later note. Even worse! It is buggy for segments _with_ data. RFC
 * states that events when retransmit arrives after original data are rare.
 * It is a blatant lie. VJ forgot about fast retransmit! 8)8) It is
 * the biggest problem on large power networks even with minor reordering.
 * OK, let's give it small replay window. If peer clock is even 1hz, it is safe
 * up to bandwidth of 18Gigabit/sec. 8) ]
 */

static int tcp_disordered_ack(struct tcp_sock *tp, struct sk_buff *skb)
{
	struct tcphdr *th = skb->h.th;
	u32 seq = TCP_SKB_CB(skb)->seq;
	u32 ack = TCP_SKB_CB(skb)->ack_seq;

	return (/* 1. Pure ACK with correct sequence number. */
		(th->ack && seq == TCP_SKB_CB(skb)->end_seq && seq == tp->rcv_nxt) &&

		/* 2. ... and duplicate ACK. */
		ack == tp->snd_una &&

		/* 3. ... and does not update window. */
		!tcp_may_update_window(tp, ack, seq, ntohs(th->window) << tp->rx_opt.snd_wscale) &&

		/* 4. ... and sits in replay window. */
		(s32)(tp->rx_opt.ts_recent - tp->rx_opt.rcv_tsval) <= (tp->rto*1024)/HZ);
}

static inline int tcp_paws_discard(struct tcp_sock *tp, struct sk_buff *skb)
{
	return ((s32)(tp->rx_opt.ts_recent - tp->rx_opt.rcv_tsval) > TCP_PAWS_WINDOW &&
		xtime.tv_sec < tp->rx_opt.ts_recent_stamp + TCP_PAWS_24DAYS &&
		!tcp_disordered_ack(tp, skb));
}

/* Check segment sequence number for validity.
 *
 * Segment controls are considered valid, if the segment
 * fits to the window after truncation to the window. Acceptability
 * of data (and SYN, FIN, of course) is checked separately.
 * See tcp_data_queue(), for example.
 *
 * Also, controls (RST is main one) are accepted using RCV.WUP instead
 * of RCV.NXT. Peer still did not advance his SND.UNA when we
 * delayed ACK, so that hisSND.UNA<=ourRCV.WUP.
 * (borrowed from freebsd)
 */

static inline int tcp_sequence(struct tcp_sock *tp, u32 seq, u32 end_seq)
{
	return	!before(end_seq, tp->rcv_wup) &&
		!after(seq, tp->rcv_nxt + tcp_receive_window(tp));
}

/* When we get a reset we do this. */
static void tcp_reset(struct sock *sk)
{
	/* We want the right error as BSD sees it (and indeed as we do). */
	switch (sk->sk_state) {
		case TCP_SYN_SENT:
			sk->sk_err = ECONNREFUSED;
			break;
		case TCP_CLOSE_WAIT:
			sk->sk_err = EPIPE;
			break;
		case TCP_CLOSE:
			return;
		default:
			sk->sk_err = ECONNRESET;
	}

	if (!sock_flag(sk, SOCK_DEAD))
		sk->sk_error_report(sk);

	tcp_done(sk);
}

/*
 * 	Process the FIN bit. This now behaves as it is supposed to work
 *	and the FIN takes effect when it is validly part of sequence
 *	space. Not before when we get holes.
 *
 *	If we are ESTABLISHED, a received fin moves us to CLOSE-WAIT
 *	(and thence onto LAST-ACK and finally, CLOSE, we never enter
 *	TIME-WAIT)
 *
 *	If we are in FINWAIT-1, a received FIN indicates simultaneous
 *	close and we go into CLOSING (and later onto TIME-WAIT)
 *
 *	If we are in FINWAIT-2, a received FIN moves us to TIME-WAIT.
 */
/* 当套接口接收到FIN段后，通知等待的进程 */
static void tcp_fin(struct sk_buff *skb, struct sock *sk, struct tcphdr *th)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tcp_schedule_ack(tp);/* 接收到FIN后，需要发送ACK */

	sk->sk_shutdown |= RCV_SHUTDOWN;/* 接收关闭 */
	sock_set_flag(sk, SOCK_DONE);/* 套口即将结束，不再接收后面的数据 */

	switch (sk->sk_state) {
		case TCP_SYN_RECV:
		case TCP_ESTABLISHED:
			/* Move to CLOSE_WAIT */
			/* 这种情况下，设置状态为TCP_CLOSE_WAIT，并延时发送ACK */
			tcp_set_state(sk, TCP_CLOSE_WAIT);
			tp->ack.pingpong = 1;
			break;

		case TCP_CLOSE_WAIT:
		case TCP_CLOSING:/* 这两种情况下，收到的FIN应当是重复段，忽略 */
			/* Received a retransmission of the FIN, do
			 * nothing.
			 */
			break;
		case TCP_LAST_ACK:
			/* RFC793: Remain in the LAST-ACK state. */
			break;

		case TCP_FIN_WAIT1:/* 此状态表示两端同时关闭 */
			/* This case occurs when a simultaneous close
			 * happens, we must ack the received FIN and
			 * enter the CLOSING state.
			 */
			/* 根据协议，应当向对方发送ACK，并且转换到CLOSING状态 */
			tcp_send_ack(sk);
			tcp_set_state(sk, TCP_CLOSING);
			break;
		case TCP_FIN_WAIT2:
			/* Received a FIN -- send ACK and enter TIME_WAIT. */
			/* 根据状态图，这种情况下应当向对方发送ACK并进入到TIME_WAIT状态 */
			tcp_send_ack(sk);
			tcp_time_wait(sk, TCP_TIME_WAIT, 0);
			break;
		default:/* LISTEN和CLOSE状态忽略FIN段 */
			/* Only TCP_LISTEN and TCP_CLOSE are left, in these
			 * cases we should never reach this piece of code.
			 */
			printk(KERN_ERR "%s: Impossible, sk->sk_state=%d\n",
			       __FUNCTION__, sk->sk_state);
			break;
	};

	/* It _is_ possible, that we have something out-of-order _after_ FIN.
	 * Probably, we should reset in this case. For now drop them.
	 */
	/* 清空接收到乱序队列上的段 */
	__skb_queue_purge(&tp->out_of_order_queue);
	if (tp->rx_opt.sack_ok)/* 清除SACK标志 */
		tcp_sack_reset(&tp->rx_opt);
	/* 释放套接口上的缓存 */
	sk_stream_mem_reclaim(sk);

	if (!sock_flag(sk, SOCK_DEAD)) {/* 连接还没有终止 */
		sk->sk_state_change(sk);/* 唤醒等待套口的进程 */

		/* Do not send POLL_HUP for half duplex close. */
		if (sk->sk_shutdown == SHUTDOWN_MASK ||
		    sk->sk_state == TCP_CLOSE)/* 读写已经关闭了，通知异步等待的线程 */
			sk_wake_async(sk, 1, POLL_HUP);
		else/* 只关闭了读，则通知异步等待的线程，读通道被关闭 */
			sk_wake_async(sk, 1, POLL_IN);
	}
}

static __inline__ int
tcp_sack_extend(struct tcp_sack_block *sp, u32 seq, u32 end_seq)
{
	if (!after(seq, sp->end_seq) && !after(sp->start_seq, end_seq)) {
		if (before(seq, sp->start_seq))
			sp->start_seq = seq;
		if (after(end_seq, sp->end_seq))
			sp->end_seq = end_seq;
		return 1;
	}
	return 0;
}

/* 当接收到重复段时，如果启用了DSACK，则调用此函数设置用于构成SACK选项的duplicate_sack数组 */
static inline void tcp_dsack_set(struct tcp_sock *tp, u32 seq, u32 end_seq)
{
	if (tp->rx_opt.sack_ok && sysctl_tcp_dsack) {
		if (before(seq, tp->rcv_nxt))
			NET_INC_STATS_BH(LINUX_MIB_TCPDSACKOLDSENT);
		else
			NET_INC_STATS_BH(LINUX_MIB_TCPDSACKOFOSENT);

		tp->rx_opt.dsack = 1;
		tp->duplicate_sack[0].start_seq = seq;
		tp->duplicate_sack[0].end_seq = end_seq;
		tp->rx_opt.eff_sacks = min(tp->rx_opt.num_sacks + 1, 4 - tp->rx_opt.tstamp_ok);
	}
}

static inline void tcp_dsack_extend(struct tcp_sock *tp, u32 seq, u32 end_seq)
{
	if (!tp->rx_opt.dsack)
		tcp_dsack_set(tp, seq, end_seq);
	else
		tcp_sack_extend(tp->duplicate_sack, seq, end_seq);
}

static void tcp_send_dupack(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (TCP_SKB_CB(skb)->end_seq != TCP_SKB_CB(skb)->seq &&
	    before(TCP_SKB_CB(skb)->seq, tp->rcv_nxt)) {
		NET_INC_STATS_BH(LINUX_MIB_DELAYEDACKLOST);
		tcp_enter_quickack_mode(tp);

		if (tp->rx_opt.sack_ok && sysctl_tcp_dsack) {
			u32 end_seq = TCP_SKB_CB(skb)->end_seq;

			if (after(TCP_SKB_CB(skb)->end_seq, tp->rcv_nxt))
				end_seq = tp->rcv_nxt;
			tcp_dsack_set(tp, TCP_SKB_CB(skb)->seq, end_seq);
		}
	}

	tcp_send_ack(sk);
}

/* These routines update the SACK block as out-of-order packets arrive or
 * in-order packets close up the sequence space.
 */
static void tcp_sack_maybe_coalesce(struct tcp_sock *tp)
{
	int this_sack;
	struct tcp_sack_block *sp = &tp->selective_acks[0];
	struct tcp_sack_block *swalk = sp+1;

	/* See if the recent change to the first SACK eats into
	 * or hits the sequence space of other SACK blocks, if so coalesce.
	 */
	for (this_sack = 1; this_sack < tp->rx_opt.num_sacks; ) {
		if (tcp_sack_extend(sp, swalk->start_seq, swalk->end_seq)) {
			int i;

			/* Zap SWALK, by moving every further SACK up by one slot.
			 * Decrease num_sacks.
			 */
			tp->rx_opt.num_sacks--;
			tp->rx_opt.eff_sacks = min(tp->rx_opt.num_sacks + tp->rx_opt.dsack, 4 - tp->rx_opt.tstamp_ok);
			for(i=this_sack; i < tp->rx_opt.num_sacks; i++)
				sp[i] = sp[i+1];
			continue;
		}
		this_sack++, swalk++;
	}
}

static __inline__ void tcp_sack_swap(struct tcp_sack_block *sack1, struct tcp_sack_block *sack2)
{
	__u32 tmp;

	tmp = sack1->start_seq;
	sack1->start_seq = sack2->start_seq;
	sack2->start_seq = tmp;

	tmp = sack1->end_seq;
	sack1->end_seq = sack2->end_seq;
	sack2->end_seq = tmp;
}

/* 当接收到乱序段后，调用此函数设置selective_acks数组，计算下一个发送段中SACK选项中SACK块数 */
static void tcp_sack_new_ofo_skb(struct sock *sk, u32 seq, u32 end_seq)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_sack_block *sp = &tp->selective_acks[0];
	int cur_sacks = tp->rx_opt.num_sacks;
	int this_sack;

	if (!cur_sacks)
		goto new_sack;

	for (this_sack=0; this_sack<cur_sacks; this_sack++, sp++) {
		if (tcp_sack_extend(sp, seq, end_seq)) {
			/* Rotate this_sack to the first one. */
			for (; this_sack>0; this_sack--, sp--)
				tcp_sack_swap(sp, sp-1);
			if (cur_sacks > 1)
				tcp_sack_maybe_coalesce(tp);
			return;
		}
	}

	/* Could not find an adjacent existing SACK, build a new one,
	 * put it at the front, and shift everyone else down.  We
	 * always know there is at least one SACK present already here.
	 *
	 * If the sack array is full, forget about the last one.
	 */
	if (this_sack >= 4) {
		this_sack--;
		tp->rx_opt.num_sacks--;
		sp--;
	}
	for(; this_sack > 0; this_sack--, sp--)
		*sp = *(sp-1);

new_sack:
	/* Build the new head SACK, and we're done. */
	sp->start_seq = seq;
	sp->end_seq = end_seq;
	tp->rx_opt.num_sacks++;
	tp->rx_opt.eff_sacks = min(tp->rx_opt.num_sacks + tp->rx_opt.dsack, 4 - tp->rx_opt.tstamp_ok);
}

/* RCV.NXT advances, some SACKs should be eaten. */
/* 在接收段的慢速流程中，如果待回复的ACk段中存在SACK选项，则调用此函数，根据接收到的段调整selective_acks数组 */
static void tcp_sack_remove(struct tcp_sock *tp)
{
	struct tcp_sack_block *sp = &tp->selective_acks[0];
	int num_sacks = tp->rx_opt.num_sacks;
	int this_sack;

	/* Empty ofo queue, hence, all the SACKs are eaten. Clear. */
	if (skb_queue_len(&tp->out_of_order_queue) == 0) {
		tp->rx_opt.num_sacks = 0;
		tp->rx_opt.eff_sacks = tp->rx_opt.dsack;
		return;
	}

	for(this_sack = 0; this_sack < num_sacks; ) {
		/* Check if the start of the sack is covered by RCV.NXT. */
		if (!before(tp->rcv_nxt, sp->start_seq)) {
			int i;

			/* RCV.NXT must cover all the block! */
			BUG_TRAP(!before(tp->rcv_nxt, sp->end_seq));

			/* Zap this SACK, by moving forward any other SACKS. */
			for (i=this_sack+1; i < num_sacks; i++)
				tp->selective_acks[i-1] = tp->selective_acks[i];
			num_sacks--;
			continue;
		}
		this_sack++;
		sp++;
	}
	if (num_sacks != tp->rx_opt.num_sacks) {
		tp->rx_opt.num_sacks = num_sacks;
		tp->rx_opt.eff_sacks = min(tp->rx_opt.num_sacks + tp->rx_opt.dsack, 4 - tp->rx_opt.tstamp_ok);
	}
}

/* This one checks to see if we can put data from the
 * out_of_order queue into the receive_queue.
 */
/* 该函数检测将接收的段是否能与乱序队列中的段合并并放到接收队列中 */
static void tcp_ofo_queue(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 dsack_high = tp->rcv_nxt;/* dsack_high是已经通过dsack处理的序号 */
	struct sk_buff *skb;

	while ((skb = skb_peek(&tp->out_of_order_queue)) != NULL) {/* 遍历乱序队列中所有的段 */
		if (after(TCP_SKB_CB(skb)->seq, tp->rcv_nxt))/* 当前段的序号超过预期接收的序号，退出 */
			break;

		if (before(TCP_SKB_CB(skb)->seq, dsack_high)) {/* 段序号小于预期的序号，说明重复接收了报文 */
			__u32 dsack = dsack_high;
			if (before(TCP_SKB_CB(skb)->end_seq, dsack_high))/* 处理DSACK */
				dsack_high = TCP_SKB_CB(skb)->end_seq;
			tcp_dsack_extend(tp, TCP_SKB_CB(skb)->seq, dsack);
		}

		if (!after(TCP_SKB_CB(skb)->end_seq, tp->rcv_nxt)) {/* 该段已经被全部接收，不必再保存在乱序队列中了 */
			SOCK_DEBUG(sk, "ofo packet was already received \n");
			__skb_unlink(skb, skb->list);
			__kfree_skb(skb);
			continue;
		}
		SOCK_DEBUG(sk, "ofo requeuing : rcv_next %X seq %X - %X\n",
			   tp->rcv_nxt, TCP_SKB_CB(skb)->seq,
			   TCP_SKB_CB(skb)->end_seq);

		/* 预期的段，将其从乱序队列中移到接收队列中 */
		__skb_unlink(skb, skb->list);
		__skb_queue_tail(&sk->sk_receive_queue, skb);
		/* 更新预期段的序号并处理FIN */
		tp->rcv_nxt = TCP_SKB_CB(skb)->end_seq;
		if(skb->h.th->fin)
			tcp_fin(skb, sk, skb->h.th);
	}
}

static int tcp_prune_queue(struct sock *sk);

/* 慢速路径处理TCP数据报文 */
static void tcp_data_queue(struct sock *sk, struct sk_buff *skb)
{
	struct tcphdr *th = skb->h.th;
	struct tcp_sock *tp = tcp_sk(sk);
	int eaten = -1;

	if (TCP_SKB_CB(skb)->seq == TCP_SKB_CB(skb)->end_seq)/* 没有数据负荷，退出 */
		goto drop;

	th = skb->h.th;
	/* 移动指针，跳过TCP报头 */
	__skb_pull(skb, th->doff*4);

	/* 处理CWR标志，如果接收到的TCP首部中存在此标志，表示发送方作了拥塞处理，所以本端可以去掉TCP_ECN_DEMAND_CWR标志 */
	TCP_ECN_accept_cwr(tp, skb);

	if (tp->rx_opt.dsack) {/* 上次的段中存在DSACK */
		tp->rx_opt.dsack = 0;/* 因为目前还不清楚下次发送的段是否存在DSACK，因此清除此标志 */
		tp->rx_opt.eff_sacks = min_t(unsigned int, tp->rx_opt.num_sacks,
						    4 - tp->rx_opt.tstamp_ok);
	}

	/*  Queue data for delivery to the user.
	 *  Packets in sequence go to the receive queue.
	 *  Out of sequence packets to the out_of_order_queue.
	 */
	if (TCP_SKB_CB(skb)->seq == tp->rcv_nxt) {/* 是预期的段，只有过存在选项而已 */
		if (tcp_receive_window(tp) == 0)/* 接收窗口为0，不能接收数据，向对方发送ACK后丢弃段。对方进行拥塞处理。 */
			goto out_of_window;

		/* Ok. In sequence. In window. */
		if (tp->ucopy.task == current &&
		    tp->copied_seq == tp->rcv_nxt && tp->ucopy.len &&
		    sock_owned_by_user(sk) && !tp->urg_data) {/* 判断是否可以复制到用户空间 */
			int chunk = min_t(unsigned int, skb->len,
							tp->ucopy.len);/* 复制到用户空间的数据长度 */

			__set_current_state(TASK_RUNNING);

			local_bh_enable();
			if (!skb_copy_datagram_iovec(skb, 0, tp->ucopy.iov, chunk)) {/* 将数据复制到用户空间 */
				/* 复制成功的话，更新用户空间缓存长度，复制的序号 */
				tp->ucopy.len -= chunk;
				tp->copied_seq += chunk;
				eaten = (chunk == skb->len && !th->fin);
				/* 更新接收缓存和接收窗口 */
				tcp_rcv_space_adjust(sk);
			}
			local_bh_disable();
		}

		if (eaten <= 0) {/* 没有复制到用户空间，缓存到接收队列中 */
queue_and_out:
			if (eaten < 0 &&/* 如果接收缓存不足，则丢弃 */
			    (atomic_read(&sk->sk_rmem_alloc) > sk->sk_rcvbuf ||
			     !sk_stream_rmem_schedule(sk, skb))) {
				if (tcp_prune_queue(sk) < 0 ||
				    !sk_stream_rmem_schedule(sk, skb))
					goto drop;
			}
			/* 设置宿主，并添加到接收队列队尾 */
			sk_stream_set_owner_r(skb, sk);
			__skb_queue_tail(&sk->sk_receive_queue, skb);
		}
		/* 成功接收了报文，更新下一个预期接收的序号 */
		tp->rcv_nxt = TCP_SKB_CB(skb)->end_seq;
		if(skb->len)/* 接收到有效数据，则处理一些数据接收相关的操作，主要是有关延时ACK以及ECN标志等等 */
			tcp_event_data_recv(sk, tp, skb);
		if(th->fin)/* 处理FIN */
			tcp_fin(skb, sk, th);

		if (skb_queue_len(&tp->out_of_order_queue)) {/* 乱序队列中存在数据 */
			/* 处理乱序队列，将其移到接收队列中 */
			tcp_ofo_queue(sk);

			/* RFC2581. 4.2. SHOULD send immediate ACK, when
			 * gap in queue is filled.
			 */
			if (!skb_queue_len(&tp->out_of_order_queue))/* 乱序队列中已经没有数据，去除pingpong标志，启用快速确认 */
				tp->ack.pingpong = 0;
		}

		if (tp->rx_opt.num_sacks)/* 接收到新数据，rcv.nxt发生了变化，可能需要去除selective_acks中的某些项 */
			tcp_sack_remove(tp);

		/* 在满足条件的情况下，重新设置首部预测的标志 */
		tcp_fast_path_check(sk, tp);

		if (eaten > 0)/* 复制数据到用户空间了 */
			__kfree_skb(skb);/* 释放报文 */
		else if (!sock_flag(sk, SOCK_DEAD))/* 没有复制数据到用户空间，并且套接口连接未断开，则唤醒等待接收数据的进程 */
			sk->sk_data_ready(sk, 0);
		return;
	}

	/* 运行到这里，说明报文不是预期的段，乱序了 */
	if (!after(TCP_SKB_CB(skb)->end_seq, tp->rcv_nxt)) {/* 本次接收的段是较早的段，说明对方重发了 */
		/* A retransmit, 2nd most common case.  Force an immediate ack. */
		NET_INC_STATS_BH(LINUX_MIB_DELAYEDACKLOST);
		/* 处理DSACK，在下一个确认中发送DSACK消息 */
		tcp_dsack_set(tp, TCP_SKB_CB(skb)->seq, TCP_SKB_CB(skb)->end_seq);

out_of_window:/* 调度ACK，让确认段尽可能快的发送给发送方 */
		tcp_enter_quickack_mode(tp);
		tcp_schedule_ack(tp);
drop:
		__kfree_skb(skb);/* 释放SKB并退出 */
		return;
	}

	/* Out of window. F.e. zero window probe. */
	/* 接收到的段序号较大，超过了接收窗口，跳到out_of_window处释放它 */
	if (!before(TCP_SKB_CB(skb)->seq, tp->rcv_nxt + tcp_receive_window(tp)))
		goto out_of_window;

	/* 需要接收的乱序的段，先进行快速确认处理 */
	tcp_enter_quickack_mode(tp);

	if (before(TCP_SKB_CB(skb)->seq, tp->rcv_nxt)) {/* 部分段已经接收，则先处理SACK选项的D-SACK */
		/* Partial packet, seq < rcv_next < end_seq */
		SOCK_DEBUG(sk, "partial packet: rcv_next %X seq %X - %X\n",
			   tp->rcv_nxt, TCP_SKB_CB(skb)->seq,
			   TCP_SKB_CB(skb)->end_seq);

		tcp_dsack_set(tp, TCP_SKB_CB(skb)->seq, tp->rcv_nxt);
		
		/* If window is closed, drop tail of packet. But after
		 * remembering D-SACK for its head made in previous line.
		 */
		if (!tcp_receive_window(tp))/* 接收窗口为0，只能丢弃了 */
			goto out_of_window;
		goto queue_and_out;/* 还允许接收，则跳转到queue_and_out接收数据，这里没有判断报文大小? */
	}

	/**
	 * 接收到了乱序的段，可能是传输过程中发生了拥塞，因此检测ECN标志 
	 * 如果有拥塞，则需要双方进行拥塞控制处理，否则尽快通知对方，让发送方尽可能的重传丢失的段
	 */
	TCP_ECN_check_ce(tp, skb);

	if (atomic_read(&sk->sk_rmem_alloc) > sk->sk_rcvbuf ||
	    !sk_stream_rmem_schedule(sk, skb)) {
		if (tcp_prune_queue(sk) < 0 ||/* 接收缓存空间不够，只能丢弃了 */
		    !sk_stream_rmem_schedule(sk, skb))
			goto drop;
	}

	/* Disable header prediction. */
	tp->pred_flags = 0;/* 去掉预测标志，因为只要有乱序报文存在，就不可能走快速流程，不必预测了 */
	tcp_schedule_ack(tp);/* 标识有确认需要发送 */

	SOCK_DEBUG(sk, "out of order segment: rcv_next %X seq %X - %X\n",
		   tp->rcv_nxt, TCP_SKB_CB(skb)->seq, TCP_SKB_CB(skb)->end_seq);

	sk_stream_set_owner_r(skb, sk);/* 设置skb的owner */

	if (!skb_peek(&tp->out_of_order_queue)) {/* 乱序队列目前为空 */
		/* Initial out of order segment, build 1 SACK. */
		if (tp->rx_opt.sack_ok) {/* 允许发送SACK，设置SACK的属性 */
			tp->rx_opt.num_sacks = 1;
			tp->rx_opt.dsack     = 0;
			tp->rx_opt.eff_sacks = 1;
			tp->selective_acks[0].start_seq = TCP_SKB_CB(skb)->seq;
			tp->selective_acks[0].end_seq =
						TCP_SKB_CB(skb)->end_seq;
		}
		/* 将报文加到乱序队列 */
		__skb_queue_head(&tp->out_of_order_queue,skb);
	} else {/* 已经有乱序的报文了 */
		struct sk_buff *skb1 = tp->out_of_order_queue.prev;/* 乱序队列中最后一个报文 */
		u32 seq = TCP_SKB_CB(skb)->seq;
		u32 end_seq = TCP_SKB_CB(skb)->end_seq;

		if (seq == TCP_SKB_CB(skb1)->end_seq) {/* 当前报文的序号是乱序队列最后一个报文的结束序号，即二者是连续的 */
			__skb_append(skb1, skb);/* 将新报文添加到尾部即可 */

			if (!tp->rx_opt.num_sacks ||/* SACK个数为0 */
			    tp->selective_acks[0].end_seq != seq)/* 或者与第一个DSACK的序号不相连 */
				goto add_sack;/* 新增一个DSACK */

			/* Common case: data arrive in order after hole. */
			/* 将当前报文的sack添加到第一个DSACK中即可 */
			tp->selective_acks[0].end_seq = end_seq;
			return;
		}

		/* Find place to insert this segment. */
		/* 运行到这里，说明接收的段与最后一个乱序的段不连续，需要进行查找 */
		do {
			if (!after(TCP_SKB_CB(skb1)->seq, seq))
				break;
		} while ((skb1 = skb1->prev) !=/* 从队尾向上遍历 */
			 (struct sk_buff*)&tp->out_of_order_queue);

		/* Do skb overlap to previous one? */
		if (skb1 != (struct sk_buff*)&tp->out_of_order_queue &&/* 不是队头结点 */
		    before(seq, TCP_SKB_CB(skb1)->end_seq)) {/* 部分重叠 */
			if (!after(end_seq, TCP_SKB_CB(skb1)->end_seq)) {/* 完全包含在该段内 */
				/* All the bits are present. Drop. */
				__kfree_skb(skb);/* 释放它 */
				/* 设置DSACK，并跳转到add_sack添加DSACK */
				tcp_dsack_set(tp, seq, end_seq);
				goto add_sack;
			}
			if (after(seq, TCP_SKB_CB(skb1)->seq)) {/* 再次确认是部分重叠，应该总是为true */
				/* Partial overlap. */
				/* 设置DSACK属性 */
				tcp_dsack_set(tp, seq, TCP_SKB_CB(skb1)->end_seq);
			} else {
				skb1 = skb1->prev;
			}
		}
		/* 将报文插入到合适的位置 */
		__skb_insert(skb, skb1, skb1->next, &tp->out_of_order_queue);
		
		/* And clean segments covered by new one as whole. */
		/* 将当前段后面的所有段中，包含在当前段内的段删除 */
		while ((skb1 = skb->next) !=
		       (struct sk_buff*)&tp->out_of_order_queue &&
		       after(end_seq, TCP_SKB_CB(skb1)->seq)) {
		       if (before(end_seq, TCP_SKB_CB(skb1)->end_seq)) {/* 部分重叠 */
			   	   /* 设置DSACK后退出查找过程 */
			       tcp_dsack_extend(tp, TCP_SKB_CB(skb1)->seq, end_seq);
			       break;
		       }
			   /* 完全包含，则将其从乱序队列中删除并释放，同时设置相关DSACK属性 */
		       __skb_unlink(skb1, skb1->list);
		       tcp_dsack_extend(tp, TCP_SKB_CB(skb1)->seq, TCP_SKB_CB(skb1)->end_seq);
		       __kfree_skb(skb1);
		}

add_sack:
		if (tp->rx_opt.sack_ok)/* 如果双方都支持SACK，则设置SACK块 */
			tcp_sack_new_ofo_skb(sk, seq, end_seq);
	}
}

/* Collapse contiguous sequence of skbs head..tail with
 * sequence numbers start..end.
 * Segments with FIN/SYN are not collapsed (only because this
 * simplifies code)
 */
static void
tcp_collapse(struct sock *sk, struct sk_buff *head,
	     struct sk_buff *tail, u32 start, u32 end)
{
	struct sk_buff *skb;

	/* First, check that queue is collapsable and find
	 * the point where collapsing can be useful. */
	for (skb = head; skb != tail; ) {
		/* No new bits? It is possible on ofo queue. */
		if (!before(start, TCP_SKB_CB(skb)->end_seq)) {
			struct sk_buff *next = skb->next;
			__skb_unlink(skb, skb->list);
			__kfree_skb(skb);
			NET_INC_STATS_BH(LINUX_MIB_TCPRCVCOLLAPSED);
			skb = next;
			continue;
		}

		/* The first skb to collapse is:
		 * - not SYN/FIN and
		 * - bloated or contains data before "start" or
		 *   overlaps to the next one.
		 */
		if (!skb->h.th->syn && !skb->h.th->fin &&
		    (tcp_win_from_space(skb->truesize) > skb->len ||
		     before(TCP_SKB_CB(skb)->seq, start) ||
		     (skb->next != tail &&
		      TCP_SKB_CB(skb)->end_seq != TCP_SKB_CB(skb->next)->seq)))
			break;

		/* Decided to skip this, advance start seq. */
		start = TCP_SKB_CB(skb)->end_seq;
		skb = skb->next;
	}
	if (skb == tail || skb->h.th->syn || skb->h.th->fin)
		return;

	while (before(start, end)) {
		struct sk_buff *nskb;
		int header = skb_headroom(skb);
		int copy = SKB_MAX_ORDER(header, 0);

		/* Too big header? This can happen with IPv6. */
		if (copy < 0)
			return;
		if (end-start < copy)
			copy = end-start;
		nskb = alloc_skb(copy+header, GFP_ATOMIC);
		if (!nskb)
			return;
		skb_reserve(nskb, header);
		memcpy(nskb->head, skb->head, header);
		nskb->nh.raw = nskb->head + (skb->nh.raw-skb->head);
		nskb->h.raw = nskb->head + (skb->h.raw-skb->head);
		nskb->mac.raw = nskb->head + (skb->mac.raw-skb->head);
		memcpy(nskb->cb, skb->cb, sizeof(skb->cb));
		TCP_SKB_CB(nskb)->seq = TCP_SKB_CB(nskb)->end_seq = start;
		__skb_insert(nskb, skb->prev, skb, skb->list);
		sk_stream_set_owner_r(nskb, sk);

		/* Copy data, releasing collapsed skbs. */
		while (copy > 0) {
			int offset = start - TCP_SKB_CB(skb)->seq;
			int size = TCP_SKB_CB(skb)->end_seq - start;

			if (offset < 0) BUG();
			if (size > 0) {
				size = min(copy, size);
				if (skb_copy_bits(skb, offset, skb_put(nskb, size), size))
					BUG();
				TCP_SKB_CB(nskb)->end_seq += size;
				copy -= size;
				start += size;
			}
			if (!before(start, TCP_SKB_CB(skb)->end_seq)) {
				struct sk_buff *next = skb->next;
				__skb_unlink(skb, skb->list);
				__kfree_skb(skb);
				NET_INC_STATS_BH(LINUX_MIB_TCPRCVCOLLAPSED);
				skb = next;
				if (skb == tail || skb->h.th->syn || skb->h.th->fin)
					return;
			}
		}
	}
}

/* Collapse ofo queue. Algorithm: select contiguous sequence of skbs
 * and tcp_collapse() them until all the queue is collapsed.
 */
static void tcp_collapse_ofo_queue(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb = skb_peek(&tp->out_of_order_queue);
	struct sk_buff *head;
	u32 start, end;

	if (skb == NULL)
		return;

	start = TCP_SKB_CB(skb)->seq;
	end = TCP_SKB_CB(skb)->end_seq;
	head = skb;

	for (;;) {
		skb = skb->next;

		/* Segment is terminated when we see gap or when
		 * we are at the end of all the queue. */
		if (skb == (struct sk_buff *)&tp->out_of_order_queue ||
		    after(TCP_SKB_CB(skb)->seq, end) ||
		    before(TCP_SKB_CB(skb)->end_seq, start)) {
			tcp_collapse(sk, head, skb, start, end);
			head = skb;
			if (skb == (struct sk_buff *)&tp->out_of_order_queue)
				break;
			/* Start new segment */
			start = TCP_SKB_CB(skb)->seq;
			end = TCP_SKB_CB(skb)->end_seq;
		} else {
			if (before(TCP_SKB_CB(skb)->seq, start))
				start = TCP_SKB_CB(skb)->seq;
			if (after(TCP_SKB_CB(skb)->end_seq, end))
				end = TCP_SKB_CB(skb)->end_seq;
		}
	}
}

/* Reduce allocated memory if we can, trying to get
 * the socket within its memory limits again.
 *
 * Return less than zero if we should start dropping frames
 * until the socket owning process reads some of the data
 * to stabilize the situation.
 */
static int tcp_prune_queue(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk); 

	SOCK_DEBUG(sk, "prune_queue: c=%x\n", tp->copied_seq);

	NET_INC_STATS_BH(LINUX_MIB_PRUNECALLED);

	if (atomic_read(&sk->sk_rmem_alloc) >= sk->sk_rcvbuf)
		tcp_clamp_window(sk, tp);
	else if (tcp_memory_pressure)
		tp->rcv_ssthresh = min(tp->rcv_ssthresh, 4U * tp->advmss);

	tcp_collapse_ofo_queue(sk);
	tcp_collapse(sk, sk->sk_receive_queue.next,
		     (struct sk_buff*)&sk->sk_receive_queue,
		     tp->copied_seq, tp->rcv_nxt);
	sk_stream_mem_reclaim(sk);

	if (atomic_read(&sk->sk_rmem_alloc) <= sk->sk_rcvbuf)
		return 0;

	/* Collapsing did not help, destructive actions follow.
	 * This must not ever occur. */

	/* First, purge the out_of_order queue. */
	if (skb_queue_len(&tp->out_of_order_queue)) {
		NET_ADD_STATS_BH(LINUX_MIB_OFOPRUNED, 
				 skb_queue_len(&tp->out_of_order_queue));
		__skb_queue_purge(&tp->out_of_order_queue);

		/* Reset SACK state.  A conforming SACK implementation will
		 * do the same at a timeout based retransmit.  When a connection
		 * is in a sad state like this, we care only about integrity
		 * of the connection not performance.
		 */
		if (tp->rx_opt.sack_ok)
			tcp_sack_reset(&tp->rx_opt);
		sk_stream_mem_reclaim(sk);
	}

	if (atomic_read(&sk->sk_rmem_alloc) <= sk->sk_rcvbuf)
		return 0;

	/* If we are really being abused, tell the caller to silently
	 * drop receive data on the floor.  It will get retransmitted
	 * and hopefully then we'll have sufficient space.
	 */
	NET_INC_STATS_BH(LINUX_MIB_RCVPRUNED);

	/* Massive buffer overcommit. */
	tp->pred_flags = 0;
	return -1;
}


/* RFC2861, slow part. Adjust cwnd, after it was not full during one rto.
 * As additional protections, we do not touch cwnd in retransmission phases,
 * and if application hit its sndbuf limit recently.
 */
/* 检测拥塞窗口的时间超过了RTO，重新调整检测拥塞窗口 */
void tcp_cwnd_application_limited(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (tp->ca_state == TCP_CA_Open &&/* 当前状态为open */
	    sk->sk_socket && !test_bit(SOCK_NOSPACE, &sk->sk_socket->flags)) {/* 发送队列未满，可能是应用程序或者对方接收窗口进行了限制 */
		/* Limited by application or receiver window. */
		/* 2是拥塞窗口的初始值 */
		u32 win_used = max(tp->snd_cwnd_used, 2U);
		if (win_used < tp->snd_cwnd) {/* 调节拥塞窗口 */
			tp->snd_ssthresh = tcp_current_ssthresh(tp);
			tp->snd_cwnd = (tp->snd_cwnd + win_used) >> 1;
		}
		tp->snd_cwnd_used = 0;
	}
	tp->snd_cwnd_stamp = tcp_time_stamp;
}


/* When incoming ACK allowed to free some skb from write_queue,
 * we remember this event in flag sk->sk_queue_shrunk and wake up socket
 * on the exit from tcp input handler.
 *
 * PROBLEM: sndbuf expansion does not work well with largesend.
 */
static void tcp_new_space(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (tp->packets_out < tp->snd_cwnd &&
	    !(sk->sk_userlocks & SOCK_SNDBUF_LOCK) &&
	    !tcp_memory_pressure &&
	    atomic_read(&tcp_memory_allocated) < sysctl_tcp_mem[0]) {
 		int sndmem = max_t(u32, tp->rx_opt.mss_clamp, tp->mss_cache_std) +
			MAX_TCP_HEADER + 16 + sizeof(struct sk_buff),
		    demanded = max_t(unsigned int, tp->snd_cwnd,
						   tp->reordering + 1);
		sndmem *= 2*demanded;
		if (sndmem > sk->sk_sndbuf)
			sk->sk_sndbuf = min(sndmem, sysctl_tcp_wmem[2]);
		tp->snd_cwnd_stamp = tcp_time_stamp;
	}

	sk->sk_write_space(sk);
}

static inline void tcp_check_space(struct sock *sk)
{
	if (sk->sk_queue_shrunk) {
		sk->sk_queue_shrunk = 0;
		if (sk->sk_socket &&
		    test_bit(SOCK_NOSPACE, &sk->sk_socket->flags))
			tcp_new_space(sk);
	}
}

static void __tcp_data_snd_check(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (after(TCP_SKB_CB(skb)->end_seq, tp->snd_una + tp->snd_wnd) ||
	    tcp_packets_in_flight(tp) >= tp->snd_cwnd ||
	    tcp_write_xmit(sk, tp->nonagle))
		tcp_check_probe_timer(sk, tp);
}

static __inline__ void tcp_data_snd_check(struct sock *sk)
{
	struct sk_buff *skb = sk->sk_send_head;

	if (skb != NULL)
		__tcp_data_snd_check(sk, skb);
	tcp_check_space(sk);
}

/*
 * Check if sending an ack is needed.
 */
static void __tcp_ack_snd_check(struct sock *sk, int ofo_possible)
{
	struct tcp_sock *tp = tcp_sk(sk);

	    /* More than one full frame received... */
	if (((tp->rcv_nxt - tp->rcv_wup) > tp->ack.rcv_mss/* 接收窗口中有多个全尺寸段还没有确认 */
	     /* ... and right edge of window advances far enough.
	      * (tcp_recvmsg() will send ACK otherwise). Or...
	      */
	     && __tcp_select_window(sk) >= tp->rcv_wnd) ||
	    /* We ACK each frame or... */
	    tcp_in_quickack_mode(tp) ||/* 当前处于快速确认模式 */
	    /* We have out of order data. */
	    (ofo_possible &&
	     skb_peek(&tp->out_of_order_queue))) {/* 需要判断乱序队列，并且乱序队列中存在段 */
		/* Then ack it now */
		tcp_send_ack(sk);
	} else {
		/* Else, send delayed ack. */
		tcp_send_delayed_ack(sk);
	}
}

/* 检查是否有确认需要发送 */
static __inline__ void tcp_ack_snd_check(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	if (!tcp_ack_scheduled(tp)) {/* 没有ACK需要发送，退出 */
		/* We sent a data segment already. */
		return;
	}
	/* 有ACK需要发送，根据条件启动延时发送或者立即发送 */
	__tcp_ack_snd_check(sk, 1);
}

/*
 *	This routine is only called when we have urgent data
 *	signalled. Its the 'slow' part of tcp_urg. It could be
 *	moved inline now as tcp_urg is only called from one
 *	place. We handle URGent data wrong. We have to - as
 *	BSD still doesn't use the correction from RFC961.
 *	For 1003.1g we should support a new option TCP_STDURG to permit
 *	either form (or just set the sysctl tcp_stdurg).
 */
/* 检测紧急指针是否有效 */
static void tcp_check_urg(struct sock * sk, struct tcphdr * th)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 ptr = ntohs(th->urg_ptr);/* 计算带外数据的位置 */

	if (ptr && !sysctl_tcp_stdurg)/* BSD一开始就错误了，将ptr解释成URG后一个指针，因此这里需要前移一个指针指向URG */
		ptr--;
	ptr += ntohl(th->seq);

	/* Ignore urgent data that we've already seen and read. */
	/* 带外指针已经被接收了，退出 */
	if (after(tp->copied_seq, ptr))
		return;

	/* Do not replay urg ptr.
	 *
	 * NOTE: interesting situation not covered by specs.
	 * Misbehaving sender may send urg ptr, pointing to segment,
	 * which we already have in ofo queue. We are not able to fetch
	 * such data and will stay in TCP_URG_NOTYET until will be eaten
	 * by recvmsg(). Seems, we are not obliged to handle such wicked
	 * situations. But it is worth to think about possibility of some
	 * DoSes using some hypothetical application level deadlock.
	 */
	/* 带外数据指针在预期接收报文之前，说明已经接收过，退出 */
	if (before(ptr, tp->rcv_nxt))
		return;

	/* Do we already have a newer (or duplicate) urgent pointer? */
	/* 有另外的带外数据，并且位于当前带外数据之后，也退出 */
	if (tp->urg_data && !after(ptr, tp->urg_seq))
		return;

	/* Tell the world about our new urgent pointer. */
	/* 向等待的进程发送信号，告知它有带外数据到达 */
	sk_send_sigurg(sk);

	/* We may be adding urgent data when the last byte read was
	 * urgent. To do this requires some care. We cannot just ignore
	 * tp->copied_seq since we would read the last urgent byte again
	 * as data, nor can we alter copied_seq until this data arrives
	 * or we break the sematics of SIOCATMARK (and thus sockatmark())
	 *
	 * NOTE. Double Dutch. Rendering to plain English: author of comment
	 * above did something sort of 	send("A", MSG_OOB); send("B", MSG_OOB);
	 * and expect that both A and B disappear from stream. This is _wrong_.
	 * Though this happens in BSD with high probability, this is occasional.
	 * Any application relying on this is buggy. Note also, that fix "works"
	 * only in this artificial test. Insert some normal data between A and B and we will
	 * decline of BSD again. Verdict: it is better to remove to trap
	 * buggy users.
	 */
	if (tp->urg_seq == tp->copied_seq && tp->urg_data &&/* 带外数据的序号正是要复制到用户态的序号
