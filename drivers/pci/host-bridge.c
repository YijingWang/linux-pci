/*
 * host bridge related code
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/module.h>

#include "pci.h"

static LIST_HEAD(pci_host_bridge_list);
static DEFINE_MUTEX(pci_host_mutex);

static void pci_release_host_bridge_dev(struct device *dev)
{
	struct resource_entry *entry;
	struct pci_host_bridge *bridge = to_pci_host_bridge(dev);

	if (bridge->release_fn)
		bridge->release_fn(bridge);

	if (bridge->dynamic_busn) {
		entry = pci_busn_resource(&bridge->windows);
		kfree(entry->res);
	}

	pci_free_resource_list(&bridge->windows);
	kfree(bridge);
}

static int pci_host_busn_res_check(
		struct pci_host_bridge *new, struct pci_host_bridge *old)
{
	int i;
	bool conflict;
	struct resource_entry *entry;
	struct resource *res_new = NULL, *res_old = NULL;

	entry = pci_busn_resource(&old->windows);
	res_old = entry->res;
	entry = pci_busn_resource(&new->windows);
	res_new = entry->res;

	conflict = resource_overlaps(res_new, res_old);
	if (conflict) {
		/*
		 * We hope every host bridge has its own exclusive
		 * busn resource, then if new host bridge's busn
		 * resource conflicts with existing host bridge,
		 * we could fail it. But in reality, firmware may
		 * doesn't supply busn resource to every host bridge.
		 * Or worse, supply the wrong busn resource to
		 * host bridge. In order to avoid the introduction of
		 * regression, we try to adjust busn resource for
		 * both new and existing host bridges. But if root
		 * bus number conflicts, must fail it.
		 */
		pr_warn("pci%04x:%02x %pR conflicts with pci%04x:%02x %pR\n",
				new->domain, (int)res_new->start, res_new,
				old->domain, (int)res_old->start, res_old);
		if (res_new->start == res_old->start)
			return -ENOSPC;

		if (res_new->start < res_old->start) {
			res_new->end = res_old->start - 1;
			pr_warn("pci%04x:%02x busn resource update to %pR\n",
					new->domain, (int)res_new->start, res_new);
		} else {
			for (i = res_new->start;
					i < (res_old->end - res_new->start + 1); i++) {
				if (pci_find_bus(new->domain, i))
					return -ENOSPC;
			}
			res_old->end = res_new->start - 1;
			pr_warn("pci%04x:%02x busn resource update to %pR\n",
					old->domain, (int)res_old->start, res_old);
		}
	}

	return 0;
}

struct pci_host_bridge *pci_create_host_bridge(
		struct device *parent, int domain, void *sysdata,
		struct list_head *resources,
		struct pci_host_bridge_ops *ops)
{
	int error, bus;
	struct pci_host_bridge *host, *tmp;
	struct resource_entry *window, *n, *busn_res;

	busn_res = pci_busn_resource(resources);
	if (!busn_res)
		return NULL;

	bus = busn_res->res->start;
	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host)
		return NULL;

	INIT_LIST_HEAD(&host->windows);
	resource_list_for_each_entry_safe(window, n, resources)
		list_move_tail(&window->node, &host->windows);

	host->dev.parent = parent;
	pci_host_assign_domain_nr(host, domain);

	mutex_lock(&pci_host_mutex);
	list_for_each_entry(tmp, &pci_host_bridge_list, list) {
		if (tmp->domain == host->domain
			  && pci_host_busn_res_check(host, tmp)) {
			mutex_unlock(&pci_host_mutex);
			goto free_res;
		}
	}
	list_add_tail(&host->list, &pci_host_bridge_list);
	mutex_unlock(&pci_host_mutex);

	host->ops = ops;
	host->dev.release = pci_release_host_bridge_dev;
	dev_set_drvdata(&host->dev, sysdata);
	dev_set_name(&host->dev, "pci%04x:%02x",
			host->domain, bus);
	if (host->ops && host->ops->prepare) {
		error = host->ops->prepare(host);
		if (error)
			goto list_del;
	}

	error = device_register(&host->dev);
	if (error) {
		put_device(&host->dev);
		return NULL;
	}

	return host;
list_del:
	mutex_lock(&pci_host_mutex);
	list_del(&host->list);
	mutex_unlock(&pci_host_mutex);
free_res:
	pci_free_resource_list(&host->windows);
	kfree(host);
	return NULL;
}

void pci_free_host_bridge(struct pci_host_bridge *host)
{
	mutex_lock(&pci_host_mutex);
	list_del(&host->list);
	mutex_unlock(&pci_host_mutex);

	device_unregister(&host->dev);
}

static struct pci_bus *find_pci_root_bus(struct pci_bus *bus)
{
	while (bus->parent)
		bus = bus->parent;

	return bus;
}

struct pci_host_bridge *pci_find_host_bridge(struct pci_bus *bus)
{
	struct pci_bus *root_bus = find_pci_root_bus(bus);

	return to_pci_host_bridge(root_bus->bridge);
}

struct device *pci_get_host_bridge_device(struct pci_dev *dev)
{
	struct pci_bus *root_bus = find_pci_root_bus(dev->bus);
	struct device *bridge = root_bus->bridge;

	kobject_get(&bridge->kobj);
	return bridge;
}

void  pci_put_host_bridge_device(struct device *dev)
{
	kobject_put(&dev->kobj);
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
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	struct resource_entry *window;
	resource_size_t offset = 0;

	resource_list_for_each_entry(window, &bridge->windows) {
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
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	struct resource_entry *window;
	resource_size_t offset = 0;

	resource_list_for_each_entry(window, &bridge->windows) {
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
