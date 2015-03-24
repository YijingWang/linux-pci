/*
 * host bridge related code
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/module.h>

#include "pci.h"

static LIST_HEAD(pci_host_bridge_list);
static DEFINE_MUTEX(pci_host_mutex);

static void pci_host_assign_domain_nr(struct pci_host_bridge *host);

static void pci_release_host_bridge_dev(struct device *dev)
{
	struct pci_host_bridge *bridge = to_pci_host_bridge(dev);

	if (bridge->release_fn)
		bridge->release_fn(bridge);

	pci_free_resource_list(&bridge->windows);
	kfree(bridge);
}

static void pci_host_update_busn_res(
		struct pci_host_bridge *host, int bus,
		struct list_head *resources)
{
	struct resource_entry *window;

	resource_list_for_each_entry(window, resources)
		if (window->res->flags & IORESOURCE_BUS) {
			if (bus > window->res->start)
				window->res->start = bus;
			return;
		}

	pr_info(
	 "No busn resource found for pci%04x:%02x, will use [bus %02x-ff]\n",
		host->domain, bus, bus);
	host->busn_res.flags = IORESOURCE_BUS;
	host->busn_res.start = bus;
	host->busn_res.end = 255;
	pci_add_resource(resources, &host->busn_res);
}

static bool pci_host_busn_res_overlap(
		struct pci_host_bridge *new, struct pci_host_bridge *old)
{
	struct resource_entry *entry;
	struct resource *res1 = NULL, *res2 = NULL;

	resource_list_for_each_entry(entry, &old->windows)
		if (entry->res->flags & IORESOURCE_BUS)
			res1 = entry->res;

	resource_list_for_each_entry(entry, &new->windows)
		if (entry->res->flags & IORESOURCE_BUS)
			res2 = entry->res;

	return resource_overlaps(res1, res2);
}

struct pci_host_bridge *pci_create_host_bridge(
		struct device *parent, int domain, int bus,
		void *sysdata, struct list_head *resources,
		struct pci_host_bridge_ops *ops)
{
	int error;
	struct pci_host_bridge *host, *tmp;
	struct resource_entry *window, *n;

	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host)
		return NULL;

	host->dev.parent = parent;
	INIT_LIST_HEAD(&host->windows);
	pci_host_update_busn_res(host, bus, resources);
	resource_list_for_each_entry_safe(window, n, resources)
		list_move_tail(&window->node, &host->windows);
	/*
	 * If support CONFIG_PCI_DOMAINS_GENERIC, use
	 * pci_host_assign_domain_nr() to update domain
	 * number.
	 */
	host->domain = domain;
	pci_host_assign_domain_nr(host);
	mutex_lock(&pci_host_mutex);
	list_for_each_entry(tmp, &pci_host_bridge_list, list) {
		if (tmp->domain == host->domain
			  && pci_host_busn_res_overlap(host, tmp)) {
			pr_warn("pci host bridge pci%04x:%02x exist\n",
					host->domain, bus);
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

static struct pci_host_bridge *find_pci_host_bridge(struct pci_bus *bus)
{
	struct pci_bus *root_bus = find_pci_root_bus(bus);

	return to_pci_host_bridge(root_bus->bridge);
}

#ifdef CONFIG_PCI_DOMAINS
int pci_domain_nr(struct pci_bus *bus)
{
	struct pci_host_bridge *host = find_pci_host_bridge(bus);

	return host->domain;
}
EXPORT_SYMBOL(pci_domain_nr);

static atomic_t __domain_nr = ATOMIC_INIT(-1);

int pci_get_new_domain_nr(void)
{
	return atomic_inc_return(&__domain_nr);
}

#ifdef CONFIG_PCI_DOMAINS_GENERIC
static int pci_assign_domain_nr(struct device *dev)
{
	static int use_dt_domains = -1;
	int domain = of_get_pci_domain_nr(dev->of_node);

	/*
	 * Check DT domain and use_dt_domains values.
	 *
	 * If DT domain property is valid (domain >= 0) and
	 * use_dt_domains != 0, the DT assignment is valid since this means
	 * we have not previously allocated a domain number by using
	 * pci_get_new_domain_nr(); we should also update use_dt_domains to
	 * 1, to indicate that we have just assigned a domain number from
	 * DT.
	 *
	 * If DT domain property value is not valid (ie domain < 0), and we
	 * have not previously assigned a domain number from DT
	 * (use_dt_domains != 1) we should assign a domain number by
	 * using the:
	 *
	 * pci_get_new_domain_nr()
	 *
	 * API and update the use_dt_domains value to keep track of method we
	 * are using to assign domain numbers (use_dt_domains = 0).
	 *
	 * All other combinations imply we have a platform that is trying
	 * to mix domain numbers obtained from DT and pci_get_new_domain_nr(),
	 * which is a recipe for domain mishandling and it is prevented by
	 * invalidating the domain value (domain = -1) and printing a
	 * corresponding error.
	 */
	if (domain >= 0 && use_dt_domains) {
		use_dt_domains = 1;
	} else if (domain < 0 && use_dt_domains != 1) {
		use_dt_domains = 0;
		domain = pci_get_new_domain_nr();
	} else {
		dev_err(dev, "Node %s has inconsistent \"linux,pci-domain\" property in DT\n",
			dev->of_node->full_name);
		domain = -1;
	}

	return domain;
}
#endif
#endif

static void pci_host_assign_domain_nr(struct pci_host_bridge *host)
{
#ifdef CONFIG_PCI_DOMAINS_GENERIC
	host->domain = pci_assign_domain_nr(host->dev.parent);
#endif
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
	struct pci_host_bridge *bridge = find_pci_host_bridge(bus);
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
