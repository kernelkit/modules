#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

static int xdma_mac_test_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct platform_device *xdma_net;
	struct resource res[2];
	int err, irqs;

	err = pcim_enable_device(pdev);
	if (err)
		return err;

	irqs = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (irqs < 0) {
		err = irqs;
		dev_err(&pdev->dev, "IRQ allocation failed: %pe\n", ERR_PTR(err));
		goto err_disable;
	}

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(&pdev->dev,
				"DMA configuration failed: %pe\n", ERR_PTR(err));
			goto err_free_irq;
		}
	}

	res[0] = (struct resource) {
		.parent = &pdev->resource[1],
		.flags = IORESOURCE_MEM,

		.start = pci_resource_start(pdev, 1),
		.end   = pci_resource_end(pdev, 1),
	};

	res[1] = (struct resource) {
		.flags = IORESOURCE_IRQ,

		.start = pci_irq_vector(pdev, 0),
		.end   = pci_irq_vector(pdev, 0),
	};

	xdma_net = platform_device_register_resndata(&pdev->dev, "xdma-net",
						     PLATFORM_DEVID_AUTO, res,
						     2, NULL, 0);
	if (IS_ERR(xdma_net)) {
		err = PTR_ERR(xdma_net);
		dev_err(&pdev->dev, "failed to register XDMA netdev: %pe\n", xdma_net);
		goto err_free_irq;
	}

	pci_set_drvdata(pdev, xdma_net);
	return 0;

err_free_irq:
	pci_disable_msix(pdev);
err_disable:
	pci_disable_device(pdev);
	return err;
}

static void xdma_mac_test_remove(struct pci_dev *pdev)
{
	struct platform_device *xdma_net = pci_get_drvdata(pdev);

	platform_device_unregister(xdma_net);
	pci_disable_msix(pdev);
	pci_disable_device(pdev);
}

static const struct pci_device_id xdma_mac_test_ids[] = {
    { PCI_DEVICE(0x10ee, 0x9034) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, xdma_mac_test_ids);

static struct pci_driver xdma_mac_test_driver = {
    .name = "xdma-mac-test",
    .id_table = xdma_mac_test_ids,
    .probe = xdma_mac_test_probe,
    .remove = xdma_mac_test_remove,
};

static int __init xdma_mac_test_init(void)
{
    return pci_register_driver(&xdma_mac_test_driver);
}

static void __exit xdma_mac_test_exit(void)
{
    pci_unregister_driver(&xdma_mac_test_driver);
}

module_init(xdma_mac_test_init);
module_exit(xdma_mac_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("XDMA Netdev over PCI");
