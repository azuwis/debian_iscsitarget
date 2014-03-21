/*
 * Event notification code.
 * (C) 2005 FUJITA Tomonori <tomof@acm.org>
 * This code is licenced under the GPL.
 *
 * Some functions are based on audit code.
 */

#include <linux/module.h>
#include <net/tcp.h>
#include "iet_u.h"
#include "iscsi_dbg.h"

static struct sock *nl;
static u32 ietd_pid;

static void event_recv_skb(struct sk_buff *skb)
{
	struct nlmsghdr	*nlh;
	u32 rlen;

	while (skb->len >= NLMSG_SPACE(0)) {
		nlh = (struct nlmsghdr *)skb->data;
		if (nlh->nlmsg_len < sizeof(*nlh) || skb->len < nlh->nlmsg_len)
			break;
		rlen = NLMSG_ALIGN(nlh->nlmsg_len);
		if (rlen > skb->len)
			rlen = skb->len;
		ietd_pid = NETLINK_CB(skb).portid;
		WARN_ON(ietd_pid == 0);
		if (nlh->nlmsg_flags & NLM_F_ACK)
			netlink_ack(skb, nlh, 0);
		skb_pull(skb, rlen);
	}
}

static int notify(void *data, int len, int gfp_mask)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	static u32 seq = 0;
	int payload = NLMSG_SPACE(len);

	if (!(skb = alloc_skb(payload, gfp_mask)))
		return -ENOMEM;

	WARN_ON(ietd_pid == 0);
	nlh = __nlmsg_put(skb, ietd_pid, seq++, NLMSG_DONE, payload - sizeof(*nlh), 0);

	memcpy(NLMSG_DATA(nlh), data, len);

	return netlink_unicast(nl, skb, ietd_pid, 0);
}

int event_send(u32 tid, u64 sid, u32 cid, u32 state, int atomic)
{
	int err;
	struct iet_event event;

	event.tid = tid;
	event.sid = sid;
	event.cid = cid;
	event.state = state;

	err = notify(&event, sizeof(struct iet_event), 0);

	return err;
}

int event_init(void)
{
	struct netlink_kernel_cfg cfg = {
		.groups = 1,
		.input = event_recv_skb,
		.cb_mutex = NULL,
		.bind = NULL,
	};

	nl = netlink_kernel_create(&init_net,
				   NETLINK_IET,
				   &cfg);
	if (!nl)
		return -ENOMEM;
	else
		return 0;
}

void event_exit(void)
{
	netlink_kernel_release(nl);
}
