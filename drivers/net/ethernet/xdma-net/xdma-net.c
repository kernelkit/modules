// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rtnetlink.h>

#include <linux/circ_buf.h>
#define CIRC_NEXT(_ptr, _size) (((_ptr) + 1) & ((_size) - 1))

#include <../drivers/dma/xilinx/xdma-regs.h>
#define XDMA_DESC_EOP               BIT(4)
#define XDMA_DESC_RELOAD            (XDMA_DESC_EOP | XDMA_DESC_COMPLETED)

#define XDMA_SGDMA_CONTROL          0x6010
#define XDMA_SGDMA_CONTROL_W1S      0x6014
#define XDMA_SGDMA_CONTROL_W1C      0x6018
#define XDMA_SGDMA_CREDIT_MODE      0x6020
#define XDMA_SGDMA_CREDIT_MODE_W1S  0x6024
#define XDMA_SGDMA_CREDIT_MODE_W1C  0x6028

struct xdma_c2h_wb {
	__le32 status;
	__le32 bytes;
} __packed;
#define XDMA_C2H_WB_MAGIC           0x52b4UL
#define XDMA_C2H_WB_MAGIC_BITS      GENMASK(31, 16)
#define XDMA_C2H_WB_FLAGS_BITS      GENMASK(1, 0)
#define XDMA_C2H_WB_EOP             BIT(0)
#define XDMA_C2H_WB_VALID           (					\
		FIELD_PREP(XDMA_C2H_WB_MAGIC_BITS, XDMA_C2H_WB_MAGIC) |	\
		FIELD_PREP(XDMA_C2H_WB_FLAGS_BITS, XDMA_C2H_WB_EOP)	\
		)


#define XDMA_NET_RING_LEN           256
#define XDMA_NET_RX_RING_FRAGSZ     2048
#define XDMA_NET_TX_WAKE_THRESHOLD  (XDMA_NET_RING_LEN >> 3)


struct xdma_net_desc {
	union {
		struct {
			void *frag;
		} rx;

		struct {
			struct sk_buff *skb;
		} tx;
	};

	struct {
		DEFINE_DMA_UNMAP_ADDR(addr);
		DEFINE_DMA_UNMAP_LEN(len);
	} dma;
};

struct xdma_net_ring {
	struct xdma_net *xn;
	u32 regbase;
	u8 id, chanbit;

	struct xdma_hw_desc *hwdesc;
	dma_addr_t hwdesc_phys;
	u16 len, head, tail, fragsz;
	u32 completed, unprocessed;

	struct napi_struct napi;

	struct {
		u64 packets;
		u64 bytes;
		u64 dropped;
	} stats;

	struct xdma_net_desc swdesc[0];
};

struct xdma_net {
	struct platform_device *pdev;
	struct net_device *ndev;
	struct regmap *regs;
	int irq;

	u8 nrx, ntx;
	struct xdma_net_ring *rx[XDMA_MAX_CHANNELS];
	struct xdma_net_ring *tx[XDMA_MAX_CHANNELS];

	u32 msg_enable;
};

static inline __printf(3, 4)
void __xdma_net_msg(struct xdma_net *xn, u32 cls, const char *fmt, ...)
{
	va_list ap;

	if (!(xn->msg_enable & cls))
		return;

	va_start(ap, fmt);
	dev_vprintk_emit(LOGLEVEL_INFO, &xn->pdev->dev, fmt, ap);
	va_end(ap);
}
#define xdma_net_msg(_xn, _cls, _fmt, ...) \
	__xdma_net_msg((_xn), NETIF_MSG_##_cls, #_cls " " _fmt, ##__VA_ARGS__)

static int xdma_net_read(struct xdma_net *xn, u32 reg, u32 *val)
{
	int err;

	err = regmap_read(xn->regs, reg, val);
	xdma_net_msg(xn, HW, "r 0x%04x: 0x%08x\n", reg, *val);
	return err;
}

static int xdma_net_write(struct xdma_net *xn, u32 reg, u32 val)
{
	xdma_net_msg(xn, HW, "w 0x%04x: 0x%08x\n", reg, val);
	return regmap_write(xn->regs, reg, val);
}

static bool xdma_net_ring_is_rx(const struct xdma_net_ring *xr)
{
	return xr->fragsz > 0;
}

static int xdma_net_ring_read(struct xdma_net_ring *xr, u32 chanreg, u32 *val)
{
	return xdma_net_read(xr->xn, xr->regbase + chanreg, val);
}

static int xdma_net_ring_write(struct xdma_net_ring *xr, u32 chanreg, u32 val)
{

	return xdma_net_write(xr->xn, xr->regbase + chanreg, val);
}

static int xdma_net_ring_set_irq(struct xdma_net_ring *xr, bool on)
{
	u32 chan_reg = on ? XDMA_CHAN_INTR_ENABLE_W1S : XDMA_CHAN_INTR_ENABLE_W1C;
	u32 glob_reg = on ? XDMA_IRQ_CHAN_INT_EN_W1S : XDMA_IRQ_CHAN_INT_EN_W1C;
	int err;

	err = xdma_net_ring_write(xr, chan_reg, CHAN_IM_ALL);
	err = err ? : xdma_net_write(xr->xn, glob_reg, xr->chanbit);
	return err;
}

static int xdma_net_ring_credit(struct xdma_net_ring *xr, u32 ndesc)
{
	return xdma_net_ring_write(xr, XDMA_SGDMA_DESC_CREDIT, ndesc);
}

static int xdma_net_ring_start(struct xdma_net_ring *xr)
{
	int err;

	err = xdma_net_ring_write(xr, XDMA_SGDMA_DESC_LO, xr->hwdesc_phys & 0xffffffff);
	err = err ? : xdma_net_ring_write(xr, XDMA_SGDMA_DESC_HI, xr->hwdesc_phys >> 32);
	err = err ? : xdma_net_ring_write(xr, XDMA_SGDMA_DESC_ADJ, XDMA_DESC_ADJACENT);
	err = err ? : xdma_net_write(xr->xn, XDMA_SGDMA_CREDIT_MODE_W1S, xr->chanbit);
	err = err ? : xdma_net_ring_write(xr, XDMA_CHAN_CONTROL, CHAN_CTRL_START);
	/* err = err ? : xdma_net_ring_read(xr, XDMA_CHAN_COMPLETED_DESC, &xr->completed); */
	err = err ? : xdma_net_ring_set_irq(xr, true);
	return err;
}

static int xdma_net_ring_stop(struct xdma_net_ring *xr)
{
	int err;

	err = xdma_net_ring_set_irq(xr, false);
	err = err ? : xdma_net_ring_write(xr, XDMA_CHAN_CONTROL_W1C,
					  CHAN_CTRL_RUN_STOP);
	return err;
}

static void xdma_net_ring_link(struct xdma_net_ring *xr)
{
	struct xdma_hw_desc *hwd;
	int i, blocki, adj;
	u64 next_desc;

	for (i = 0, hwd = xr->hwdesc; i < xr->len; i++, hwd++) {
		blocki = i % XDMA_DESC_ADJACENT;
		if (xr->len - i < XDMA_DESC_ADJACENT)
			adj = xr->len - i - 1;
		else if (blocki == XDMA_DESC_ADJACENT_MASK)
			adj = XDMA_DESC_ADJACENT;
		else
			adj = XDMA_DESC_ADJACENT - blocki - 1;

		next_desc = xr->hwdesc_phys;
		if (i < xr->len - 1)
			next_desc += (i + 1) * sizeof(*xr->hwdesc);

		hwd->control = cpu_to_le32(XDMA_DESC_CONTROL(adj, 0));
		hwd->next_desc = cpu_to_le64(next_desc);
	}
}

static struct xdma_net_ring *xdma_net_ring_alloc(struct xdma_net *xn, u8 id,
						 u16 len, u16 fragsz)
{
	struct xdma_net_ring *xr;

	xr = kzalloc(sizeof(*xr) + len * sizeof(xr->swdesc[0]), GFP_KERNEL);
	if (!xr)
		goto err;

	xr->xn = xn;
	xr->id = id;
	xr->len = len;
	xr->fragsz = fragsz;

	if (xdma_net_ring_is_rx(xr)) {
		xr->regbase = XDMA_CHAN_C2H_OFFSET;
		xr->chanbit = BIT(xn->ntx + id);
	} else {
		xr->regbase = XDMA_CHAN_H2C_OFFSET;
		xr->chanbit = BIT(id);
	}

	xr->regbase += id * XDMA_CHAN_STRIDE;

	xr->hwdesc = dma_alloc_coherent(&xn->pdev->dev, len * sizeof(*xr->hwdesc),
					&xr->hwdesc_phys, GFP_KERNEL | __GFP_ZERO);
	if (!xr->hwdesc) {
		goto err_free;
	}

	xdma_net_ring_link(xr);

	return xr;
err_free:
	kfree(xr);
err:
	return NULL;
}

static void xdma_net_ring_free(struct xdma_net_ring *xr)
{
	struct xdma_net *xn = xr->xn;

	dma_free_coherent(&xn->pdev->dev, xr->len * sizeof(*xr->hwdesc),
			  xr->hwdesc, xr->hwdesc_phys);
	kfree(xr);
}

static int xdma_net_rx_ring_refill(struct xdma_net_ring *xr)
{
	u16 space = CIRC_SPACE(xr->head, xr->tail, xr->len);
	struct device *dev = &xr->xn->pdev->dev;
	struct xdma_net_desc *swd;
	struct xdma_hw_desc *hwd;
	dma_addr_t fragphys;
	int i;

	for (i = 0; i < space; i++, xr->head = CIRC_NEXT(xr->head, xr->len)) {
		swd = &xr->swdesc[xr->head];
		hwd = &xr->hwdesc[xr->head];

		/* xdma_net_rx_ring_poll() might not have consumed the
		 * fragment due to inability to allocate skb memory, XDP
		 * drops, etc.
		 */
		if (likely(!swd->rx.frag)) {
			swd->rx.frag = netdev_alloc_frag(xr->fragsz);
			if (unlikely(!swd->rx.frag))
				break;
		}

		fragphys = dma_map_single(dev, swd->rx.frag,
					  xr->fragsz, DMA_FROM_DEVICE);
		if (unlikely(dma_mapping_error(dev, fragphys))) {
			skb_free_frag(swd->rx.frag);
			break;
		}

		dma_unmap_addr_set(&swd->dma, addr, fragphys);
                dma_unmap_len_set(&swd->dma, len, xr->fragsz);

		/* On C2H transfers, src_addr points to writeback
		 * info, which we choose to store at the start of the
		 * buffer.
		 */
		hwd->src_addr = cpu_to_le64(fragphys);
		hwd->dst_addr = cpu_to_le64(fragphys + sizeof(struct xdma_c2h_wb));
		hwd->bytes = cpu_to_le32(xr->fragsz);
		hwd->control = cpu_to_le32(le32_to_cpu(hwd->control) | XDMA_DESC_RELOAD);

		xdma_net_msg(xr->xn, RX_STATUS, "rx%u+%03u: refill %*ph\n",
			     xr->id, xr->head, (int)sizeof(*hwd), hwd);
	}

	dma_wmb();

	return i ? xdma_net_ring_credit(xr, i) : 0;
}

static int xdma_net_rx_ring_poll(struct napi_struct *napi, const int budget)
{
	struct xdma_net_ring *xr = container_of(napi, struct xdma_net_ring, napi);
	struct device *dev = &xr->xn->pdev->dev;
	struct net_device *ndev = xr->xn->ndev;
	struct xdma_net_desc *swd;
	struct xdma_hw_desc *hwd;
	struct xdma_c2h_wb *wb;
	struct list_head skbs;
	struct sk_buff *skb;
	u32 completed;
	int done;

	xdma_net_ring_read(xr, XDMA_CHAN_COMPLETED_DESC, &completed);
	xr->unprocessed += completed - xr->completed;
	xr->completed = completed;

	xdma_net_msg(xr->xn, RX_STATUS, "rx%u: poll, %d unprocessed\n",
		     xr->id, xr->unprocessed);

	INIT_LIST_HEAD(&skbs);

	for (done = 0; xr->unprocessed && (done < budget); done++) {
		swd = &xr->swdesc[xr->tail];
		hwd = &xr->hwdesc[xr->tail];

		xdma_net_msg(xr->xn, RX_STATUS, "rx%u+%03u: done %*ph\n",
			     xr->id, xr->tail, (int)sizeof(*hwd), hwd);

		xr->tail = CIRC_NEXT(xr->tail, xr->len);
		xr->unprocessed--;

		dma_unmap_single(dev,
				 dma_unmap_addr(&swd->dma, addr),
				 dma_unmap_len(&swd->dma, len), DMA_FROM_DEVICE);

		wb = swd->rx.frag;
		if (le32_to_cpu(wb->status) != XDMA_C2H_WB_VALID) {
			xr->stats.dropped++;
			break;
		}

		/* TODO: XDP */

		skb = build_skb(swd->rx.frag, xr->fragsz);
		if (unlikely(!skb)) {
			xr->stats.dropped++;
			break;
		}

		skb_reserve(skb, sizeof(struct xdma_c2h_wb));
		skb_put(skb, le32_to_cpu(wb->bytes));

		skb_record_rx_queue(skb, xr->id);
		skb->protocol = eth_type_trans(skb, ndev);
		list_add_tail(&skb->list, &skbs);

		xr->stats.packets++;
		xr->stats.bytes += le32_to_cpu(hwd->bytes);

		swd->rx.frag = NULL;
	}

	if (done)
		xdma_net_rx_ring_refill(xr);


	netif_receive_skb_list(&skbs);

	if (done < budget && napi_complete_done(napi, done))
		xdma_net_ring_set_irq(xr, true);

	return done;
}

static void xdma_net_rx_ring_close(struct xdma_net *xn, u8 id)
{
	struct xdma_net_ring *xr = xn->rx[id];
	struct xdma_net_desc *swd;
	int i;

	napi_disable_locked(&xr->napi);
	__netif_napi_del_locked(&xr->napi);
	xdma_net_ring_stop(xr);

	for (i = 0, swd = xr->swdesc; i < xr->len; i++, swd++) {
		if (!swd->rx.frag)
			continue;

		dma_unmap_single(&xn->pdev->dev,
				 dma_unmap_addr(&swd->dma, addr),
				 dma_unmap_len(&swd->dma, len), DMA_FROM_DEVICE);
		skb_free_frag(swd->rx.frag);
	}

	xdma_net_ring_free(xn->rx[id]);
	xn->rx[id] = NULL;
}

static int xdma_net_rx_ring_open(struct xdma_net *xn, u8 id)
{
	struct xdma_net_ring *xr;
	int err;

	xr = xdma_net_ring_alloc(xn, id, XDMA_NET_RING_LEN, XDMA_NET_RX_RING_FRAGSZ);
	if (!xr)
		return -ENOMEM;

	err = xdma_net_rx_ring_refill(xr);
	if (err)
		goto err_close;

	netif_napi_add(xn->ndev, &xr->napi, xdma_net_rx_ring_poll);
	xn->rx[id] = xr;

	napi_enable_locked(&xr->napi);
	err = xdma_net_ring_start(xr);
	if (err)
		goto err_close;

	return 0;

err_close:
	xdma_net_rx_ring_close(xn, id);
	return err;
}

static int xdma_net_tx_ring_poll(struct napi_struct *napi, const int budget)
{
	struct xdma_net_ring *xr = container_of(napi, struct xdma_net_ring, napi);
	struct device *dev = &xr->xn->pdev->dev;
	u16 head = smp_load_acquire(&xr->head);
	struct xdma_net_desc *swd;
	struct xdma_hw_desc *hwd;
	struct netdev_queue *txq;
	u32 completed;
	int done;

	xdma_net_ring_read(xr, XDMA_CHAN_COMPLETED_DESC, &completed);
	xr->unprocessed += completed - xr->completed;
	xr->completed = completed;

	xdma_net_msg(xr->xn, TX_DONE, "tx%u: poll, %u unprocessed\n",
		     xr->id, xr->unprocessed);

	for (done = 0; (xr->unprocessed != 0) && (done < budget); done++) {
		if (!CIRC_CNT(head, xr->tail, xr->len))
			break;

		swd = &xr->swdesc[xr->tail];
		hwd = &xr->hwdesc[xr->tail];

		xdma_net_msg(xr->xn, TX_DONE, "tx%u+%03u: done %*ph\n",
			     xr->id, xr->tail, (int)sizeof(*hwd), hwd);

		dma_unmap_single(dev,
				 dma_unmap_addr(&swd->dma, addr),
				 dma_unmap_len(&swd->dma, len), DMA_TO_DEVICE);

		dev_consume_skb_any(swd->tx.skb);
		swd->tx.skb = NULL;

		xr->tail = CIRC_NEXT(xr->tail, xr->len);
		xr->unprocessed--;
		if (CIRC_CNT(head, xr->tail, xr->len) > XDMA_NET_TX_WAKE_THRESHOLD) {
			txq = netdev_get_tx_queue(xr->xn->ndev, xr->id);
			if (netif_tx_queue_stopped(txq))
				netif_tx_wake_queue(txq);
		}
	}

	if (done < budget && napi_complete_done(napi, done))
		xdma_net_ring_set_irq(xr, true);

	return done;
}

static void xdma_net_tx_ring_close(struct xdma_net *xn, u8 id)
{
	struct xdma_net_ring *xr = xn->tx[id];
	struct xdma_net_desc *swd;
	int i;

	napi_disable_locked(&xr->napi);
	__netif_napi_del_locked(&xr->napi);
	xdma_net_ring_stop(xr);

	for (i = 0, swd = xr->swdesc; i < xr->len; i++, swd++) {
		if (!swd->tx.skb)
			continue;

		dma_unmap_single(&xn->pdev->dev,
				 dma_unmap_addr(&swd->dma, addr),
				 dma_unmap_len(&swd->dma, len), DMA_TO_DEVICE);
		kfree_skb(swd->tx.skb);
	}

	xdma_net_ring_free(xn->tx[id]);
	xn->tx[id] = NULL;
}

static int xdma_net_tx_ring_open(struct xdma_net *xn, u8 id)
{
	struct xdma_net_ring *xr;
	int err;

	xr = xdma_net_ring_alloc(xn, id, XDMA_NET_RING_LEN, 0);
	if (!xr)
		return -ENOMEM;

	netif_napi_add_tx(xn->ndev, &xr->napi, xdma_net_tx_ring_poll);
	xn->tx[id] = xr;

	napi_enable_locked(&xr->napi);
	err = xdma_net_ring_start(xr);
	if (err)
		goto err_close;

	return 0;

err_close:
	xdma_net_rx_ring_close(xn, id);
	return err;
}

static netdev_tx_t xdma_net_ndo_start_xmit(struct sk_buff *skb,
					   struct net_device *ndev)
{
	struct xdma_net *xn = netdev_priv(ndev);
	struct xdma_net_ring *xr = xn->tx[skb->queue_mapping];
	struct xdma_net_desc *swd = &xr->swdesc[xr->head];
	struct xdma_hw_desc *hwd = &xr->hwdesc[xr->head];
	dma_addr_t fragphys;

	if (!CIRC_SPACE(xr->head, xr->tail, xr->len)) {
		netif_stop_subqueue(ndev, xr->id);
		xr->stats.dropped++;
		return NETDEV_TX_BUSY;
	}

	if (skb_put_padto(skb, ETH_ZLEN))
		goto drop;

	fragphys = dma_map_single(&xn->pdev->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(&xn->pdev->dev, fragphys))
		goto drop;

	dma_unmap_addr_set(&swd->dma, addr, fragphys);
	dma_unmap_len_set(&swd->dma, len, skb->len);
	swd->tx.skb = skb;

	hwd->src_addr = cpu_to_le64(fragphys);
	hwd->bytes = cpu_to_le32(skb->len);
	hwd->control = cpu_to_le32(le32_to_cpu(hwd->control) | XDMA_DESC_RELOAD);

	xdma_net_msg(xr->xn, TX_QUEUED, "tx%u+%03u: queued %*ph\n",
		     xr->id, xr->head, (int)sizeof(*hwd), hwd);

	dma_wmb();
	xdma_net_ring_credit(xr, 1);

	xr->stats.packets++;
	xr->stats.bytes += skb->len;

	smp_store_release(&xr->head, CIRC_NEXT(xr->head, xr->len));

	if (!CIRC_SPACE(xr->head, xr->tail, xr->len) && netdev_xmit_more())
		netif_stop_subqueue(ndev, xr->id);

	return NETDEV_TX_OK;

drop:
	dev_kfree_skb_any(skb);
	xr->stats.dropped++;
	return NETDEV_TX_OK;
}

static irqreturn_t xdma_net_isr(int irq, void *_xn)
{
        struct xdma_net *xn = _xn;
        struct xdma_net_ring *xr;
	u32 chan, req, stat;
	int err;

	err = xdma_net_read(xn, XDMA_IRQ_CHAN_INT_REQ, &req);
	if (err || !req)
		return IRQ_NONE;

	xdma_net_msg(xn, INTR, "req:0x%08x\n", req);

	for (chan = 0; chan < (xn->ntx + xn->nrx); chan++) {
		if (!(req & BIT(chan)))
			continue;

		xr = (chan < xn->ntx) ? xn->tx[chan] : xn->rx[chan - xn->ntx];
		xdma_net_ring_set_irq(xr, false);
		xdma_net_ring_read(xr, XDMA_CHAN_STATUS_RC, &stat);
		napi_schedule(&xr->napi);
	}

        return IRQ_HANDLED;
}

static int xdma_net_ndo_open(struct net_device *ndev)
{
	struct xdma_net *xn = netdev_priv(ndev);
	int txdone = 0, rxdone = 0, err, i;

	for (i = 0; i < xn->ntx; i++) {
		err = xdma_net_tx_ring_open(xn, i);
		if (err)
			goto err;

		txdone++;
	}

	for (i = 0; i < xn->nrx; i++) {
		err = xdma_net_rx_ring_open(xn, i);
		if (err)
			goto err;

		rxdone++;
	}

	err = request_irq(xn->irq, xdma_net_isr, 0, netdev_name(ndev), xn);
	if (err)
		goto err;

	return 0;

err:
	for (i = rxdone - 1; i >= 0; i--)
		xdma_net_rx_ring_close(xn, i);

	for (i = txdone - 1; i >= 0; i--)
		xdma_net_tx_ring_close(xn, i);

	return err;
}

static int xdma_net_ndo_stop(struct net_device *ndev)
{
	struct xdma_net *xn = netdev_priv(ndev);
	int i;

	free_irq(xn->irq, xn);

	for (i = xn->nrx - 1; i >= 0; i--)
		xdma_net_rx_ring_close(xn, i);

	for (i = xn->ntx - 1; i >= 0; i--)
		xdma_net_tx_ring_close(xn, i);

	return 0;
}

static const struct net_device_ops xdma_net_ndos = {
        .ndo_open               = xdma_net_ndo_open,
        .ndo_stop               = xdma_net_ndo_stop,
        .ndo_start_xmit         = xdma_net_ndo_start_xmit,
        .ndo_validate_addr      = eth_validate_addr,
        .ndo_set_mac_address    = eth_mac_addr,
};

static struct regmap *xdma_net_probe_regmap(struct platform_device *pdev)
{
	static const struct regmap_config regs_config = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.max_register = XDMA_MAX_REG_OFFSET,
	};

	struct resource *res;
	struct regmap *regs;
	void __iomem *iomem;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "No I/O memory available");
		return NULL;
	}

	iomem = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(iomem)) {
		dev_err(&pdev->dev, "Unable to remap I/O memory: %pe", iomem);
		return NULL;
	}

	regs = devm_regmap_init_mmio(&pdev->dev, iomem, &regs_config);
	if (IS_ERR(regs)) {
		dev_err(&pdev->dev, "Unable to create regmap: %pe", regs);
		return NULL;
	}

	return regs;
}

/* TODO: Dynamically resolve these from platform data. Must match
 * synthesis config.
 */
#define XDMA_NET_N_H2C 1
#define XDMA_NET_N_C2H 1

static int xdma_net_probe(struct platform_device *pdev)
{
	u8 nrx = XDMA_NET_N_C2H, ntx = XDMA_NET_N_H2C;
	struct net_device *ndev;
	struct xdma_net *xn;
	int err;

	ndev = alloc_etherdev_mqs(sizeof(*xn), ntx, nrx);
	if (!ndev) {
		return -ENOMEM;
	}

	xn = netdev_priv(ndev);
	SET_NETDEV_DEV(ndev, &pdev->dev);
	platform_set_drvdata(pdev, xn);
	ndev->max_mtu = XDMA_NET_RX_RING_FRAGSZ - sizeof(struct xdma_c2h_wb);
	ndev->netdev_ops = &xdma_net_ndos;

	if (device_get_ethdev_address(&pdev->dev, ndev))
		eth_hw_addr_random(ndev);

	/* xn->msg_enable = BIT(NETIF_MSG_CLASS_COUNT) - 1; */
	/* xn->msg_enable &= ~(NETIF_MSG_HW | NETIF_MSG_TX_QUEUED | NETIF_MSG_TX_DONE); */

	xn->pdev = pdev;
	xn->ndev = ndev;

	xn->nrx = nrx;
	xn->ntx = ntx;

	xn->irq = platform_get_irq(pdev, 0);
	if (xn->irq < 0 ) {
		err = xn->irq;
		goto err_free;
	}

	xn->regs = xdma_net_probe_regmap(pdev);
	if (!xn->regs) {
		err = -ENODEV;
		goto err_free;
	}

	err = register_netdev(ndev);
	if (err) {
		netdev_err(ndev, "Registration failed: %pe\n", ERR_PTR(err));
		goto err_free;
	}

	return 0;

err_free:
	free_netdev(ndev);
	return err;
}

static void xdma_net_remove(struct platform_device *pdev)
{
	struct xdma_net *xn = platform_get_drvdata(pdev);

	rtnl_lock();
	if (netif_running(xn->ndev))
		dev_close(xn->ndev);
	rtnl_unlock();

	unregister_netdev(xn->ndev);
	free_netdev(xn->ndev);
}

static const struct platform_device_id xdma_net_id_table[] = {
	{ "xdma-net", 0},
	{ },
};
MODULE_DEVICE_TABLE(platform, xdma_net_id_table);

static struct platform_driver xdma_net_driver = {
	.driver		= {
		.name = "xdma-net",
	},
	.id_table	= xdma_net_id_table,
	.probe		= xdma_net_probe,
	.remove		= xdma_net_remove,
};

module_platform_driver(xdma_net_driver);

MODULE_DESCRIPTION("AMD XDMA Network driver");
MODULE_AUTHOR("Tobias Waldekranz <tobias@waldekranz.com>");
MODULE_LICENSE("GPL");
