/*
 * host bridge related code
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/module.h>

#include "pci.h"

static LIST_HEAD(pci_host_bridge_list);
static DEFINE_MUTEX(phb_mutex);

static void pci_release_host_bridge_dev(struct device *dev)
{
	struct pci_host_bridge *bridge = to_pci_host_bridge(dev);

	if (bridge->release_fn)
		bridge->release_fn(bridge);

	pci_free_resource_list(&bridge->windows);
	kfree(bridge);
}

struct pci_host_bridge *pci_create_host_bridge(
		struct device *parent, u16 domain, u8 busnum, 
		struct list_head *resources, void *sysdata,
		struct pci_host_bridge_ops *ops)
{
	int error;
	struct pci_host_bridge *host;
	struct pci_host_bridge_window *window, *n;

	mutex_lock(&phb_mutex);
	list_for_each_entry(host, &pci_host_bridge_list, list) 
		if (host->domain == domain 
				&& host->busnum == busnum) {
			dev_dbg(&host->dev, "pci host bridge pci%04x:%02x exist\n",
					domain, busnum);
			mutex_unlock(&phb_mutex);
			return NULL;
		}
	mutex_unlock(&phb_mutex);

	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host)
		return NULL;

	host->ops = ops;
	host->busnum = busnum;
	host->domain = domain;
	host->dev.parent = parent;
	INIT_LIST_HEAD(&host->windows);
	host->dev.release = pci_release_host_bridge_dev;
	dev_set_drvdata(&host->dev, sysdata);

	if (host->ops && host->ops->phb_assign_domain_nr)
		host->ops->phb_assign_domain_nr(host);

	dev_set_name(&host->dev, "pci%04x:%02x", host->domain, 
			host->busnum);
	if (host->ops && host->ops->phb_prepare) {
		error = host->ops->phb_prepare(host);
		if(error) {
			kfree(host);
			return NULL;
		}
	}

	error = device_register(&host->dev);
	if (error) {
		put_device(&host->dev);
		return NULL;
	}

	list_for_each_entry_safe(window, n, resources, list)
		list_move_tail(&window->list, &host->windows);

	mutex_lock(&phb_mutex);
	list_add_tail(&host->list, &pci_host_bridge_list);
	mutex_unlock(&phb_mutex);
	return host;
}
EXPORT_SYMBOL(pci_create_host_bridge);

void pci_free_host_bridge(struct pci_host_bridge *host)
{
	mutex_lock(&phb_mutex);
	list_del(&host->list);
	mutex_unlock(&phb_mutex);

	device_unregister(&host->dev);
}

static struct pci_bus *find_pci_root_bus(struct pci_bus *bus)
{
	while (bus->parent)
		bus = bus->parent;

	return bus;
}

static struct pci_host_bridge *find_pci_host_bridge(struct pci_bus *bus)
{
	struct pci_bus *root_bus = find_pci_root_bus(bus);

	return to_pci_host_bridge(root_bus->bridge);
}

void pci_set_host_bridge_release(struct pci_host_bridge *bridge,
				 void (*release_fn)(struct pci_host_bridge *),
				 void *release_data)
{
	bridge->release_fn = release_fn;
	bridge->release_data = release_data;
}

void pcibios_resource_to_bus(struct pci_bus *bus, struct pci_bus_region *region,
			     struct resource *res)
{
	struct pci_host_bridge *bridge = find_pci_host_bridge(bus);
	struct pci_host_bridge_window *window;
	resource_size_t offset = 0;

	list_for_each_entry(window, &bridge->windows, list) {
		if (resource_contains(window->res, res)) {
			offset = window->offset;
			break;
		}
	}

	region->start = res->start - offset;
	region->end = res->end - offset;
}
EXPORT_SYMBOL(pcibios_resource_to_bus);

static bool region_contains(struct pci_bus_region *region1,
			    struct pci_bus_region *region2)
{
	return region1->start <= region2->start && region1->end >= region2->end;
}

void pcibios_bus_to_resource(struct pci_bus *bus, struct resource *res,
			     struct pci_bus_region *region)
{
	struct pci_host_bridge *bridge = find_pci_host_bridge(bus);
	struct pci_host_bridge_window *window;
	resource_size_t offset = 0;

	list_for_each_entry(window, &bridge->windows, list) {
		struct pci_bus_region bus_region;

		if (resource_type(res) != resource_type(window->res))
			continue;

		bus_region.start = window->res->start - window->offset;
		bus_region.end = window->res->end - window->offset;

		if (region_contains(&bus_region, region)) {
			offset = window->offset;
			break;
		}
	}

	res->start = region->start + offset;
	res->end = region->end + offset;
}
EXPORT_SYMBOL(pcibios_bus_to_resource);
