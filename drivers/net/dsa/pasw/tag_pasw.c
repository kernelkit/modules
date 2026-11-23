// SPDX-License-Identifier: GPL-2.0-only

#include <linux/dsa/tag_pasw.h>

/* The tagger API is declared in linux/net/dsa/tag.h, so we add that
 * as an extra include path in Makefile in order to have an
 * out-of-tree tag driver. */
#include <tag.h>

static u8 pasw_tag_pq_queue(struct sk_buff *skb, struct net_device *dev)
{
	int tc;

	/* Without a TC configuration, send everything via the highest
	 * priority queue.
	 */
	if (!netdev_get_num_tc(dev))
		return 0;

	tc = netdev_txq_to_tc(dev, skb_get_queue_mapping(skb));
	if (tc < 0)
		return 0;

	/* Mirrors pasw_queue_from_prio(), i.e. TC0 is mapped to the
	 * lowest priority queue. The switch driver guarantees that
	 * there are never more TCs than there are TXQs.
	 */
	return dev->num_tx_queues - 1 - tc;
}

static struct sk_buff *pasw_tag_pq_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);
	u8 *tag;

	skb_push(skb, PASW_TAG_PQ_FROM_CPU_LEN);
	dsa_alloc_etype_header(skb, PASW_TAG_PQ_FROM_CPU_LEN);

	tag = dsa_etype_header_pos_tx(skb);
	tag[0] = (ETH_P_PASW_PQ_FROM_CPU >> 8) & 0xff;
	tag[1] = ETH_P_PASW_PQ_FROM_CPU & 0xff;
	tag[2] = BIT(dp->index);
	tag[3] = pasw_tag_pq_queue(skb, dev);
	tag[4] = (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP_NOBPF) ? 1 : 0;

	if (unlikely(__skb_put_padto(skb, ETH_ZLEN + PASW_TAG_PQ_FROM_CPU_LEN, false)))
		return NULL;

	return skb;
}

static void pasw_tag_pq_rcv_tstamp(struct sk_buff *skb, u8 *tag)
{
	struct skb_shared_hwtstamps *shhwtstamps;
	u64 sec;
	u32 ns;

	/* [0]: PTP
	 * [1]: Valid timestamp
	 *
	 * Settle for only saving timestamps for PTP packets. Ideally
	 * this would depend on the currently requested rx_filter on
	 * the port. But at the moment (for saline) we only support L2
	 * PTP anyways.
	 */
	if ((tag[4] & 0x3) != 0x3)
		return;

	sec = (u64)get_unaligned_le32(tag + 9) |
		((u64)get_unaligned_le16(tag + 13) << 32);
	ns = get_unaligned_le32(tag + 5);

	shhwtstamps = skb_hwtstamps(skb);
	memset(shhwtstamps, 0, sizeof(struct skb_shared_hwtstamps));
	shhwtstamps->hwtstamp = ktime_set(sec, ns);
}

static struct sk_buff *pasw_tag_pq_rcv(struct sk_buff *skb, struct net_device *dev)
{
	u8 *tag = dsa_etype_header_pos_rx(skb);
	struct net_device *user;

	if (tag[0] != ((ETH_P_PASW_PQ_TO_CPU >> 8) & 0xff) ||
	    tag[1] != (ETH_P_PASW_PQ_TO_CPU & 0xff))
		return NULL;

	user = dsa_conduit_find_user(dev, 0, tag[2]);
	if (!user)
		return NULL;

	skb->dev = user;
	dsa_default_offload_fwd_mark(skb);

	pasw_tag_pq_rcv_tstamp(skb, tag);

	skb_pull_rcsum(skb, PASW_TAG_PQ_TO_CPU_LEN);
	dsa_strip_etype_header(skb, PASW_TAG_PQ_TO_CPU_LEN);
	return skb;
}

static const struct dsa_device_ops pasw_tag_pq_ops = {
	.name	= "pasw-pq",
	.proto	= DSA_TAG_PROTO_PASW_PQ,
	.xmit	= pasw_tag_pq_xmit,
	.rcv	= pasw_tag_pq_rcv,
	.needed_headroom = PASW_TAG_PQ_FROM_CPU_LEN,
};

module_dsa_tag_driver(pasw_tag_pq_ops);
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_PASW_PQ, "pasw-pq");
MODULE_AUTHOR("Tobias Waldekranz <tobias@waldekranz.com>");
MODULE_DESCRIPTION("Packet Architects FlexSwitch tag driver");
MODULE_LICENSE("GPL");
