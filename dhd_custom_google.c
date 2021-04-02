/*
 * Platform Dependent file for Hikey
 *
 * Copyright (C) 2021, Broadcom.
 *
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 *
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 *
 *
 * <<Broadcom-WL-IPTag/Open:>>
 *
 * $Id$
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/skbuff.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/of_gpio.h>
#ifdef CONFIG_WIFI_CONTROL_FUNC
#include <linux/wlan_plat.h>
#else
#include <dhd_plat.h>
#endif /* CONFIG_WIFI_CONTROL_FUNC */
#include <dhd_dbg.h>
#include <dhd.h>

#ifdef DHD_COREDUMP
#include <linux/platform_data/sscoredump.h>
#endif /* DHD_COREDUMP */

#ifdef CONFIG_BROADCOM_WIFI_RESERVED_MEM
extern int dhd_init_wlan_mem(void);
extern void *dhd_wlan_mem_prealloc(int section, unsigned long size);
#endif /* CONFIG_BROADCOM_WIFI_RESERVED_MEM */

#define WLAN_REG_ON_GPIO		491
#define WLAN_HOST_WAKE_GPIO		493

static int wlan_reg_on = -1;
#define DHD_DT_COMPAT_ENTRY		"android,bcmdhd_wlan"
#define WIFI_WL_REG_ON_PROPNAME		"wl_reg_on"

static int wlan_host_wake_up = -1;
#ifdef CONFIG_BCMDHD_OOB_HOST_WAKE
static int wlan_host_wake_irq = 0;
#endif /* CONFIG_BCMDHD_OOB_HOST_WAKE */
#define WIFI_WLAN_HOST_WAKE_PROPNAME    "wl_host_wake"

static int resched_streak = 0;
static int resched_streak_max = 0;
static uint64 last_resched_cnt_check_time_ns = 0;
static bool is_irq_on_big_core = FALSE;

#if defined(CONFIG_SOC_EXYNOS9810) || defined(CONFIG_SOC_EXYNOS9820) || \
	defined(CONFIG_SOC_GS101)
#define EXYNOS_PCIE_RC_ONOFF
extern int pcie_ch_num;
extern int exynos_pcie_pm_resume(int);
extern void exynos_pcie_pm_suspend(int);
#endif /* CONFIG_SOC_EXYNOS9810 || CONFIG_SOC_EXYNOS9820 || CONFIG_SOC_GS101 */

#ifdef DHD_COREDUMP
#define DEVICE_NAME "wlan"

static void sscd_release(struct device *dev);
static struct sscd_platform_data sscd_pdata;
static struct platform_device sscd_dev = {
	.name            = DEVICE_NAME,
	.driver_override = SSCD_NAME,
	.id              = -1,
	.dev             = {
		.platform_data = &sscd_pdata,
		.release       = sscd_release,
		},
};

static void sscd_release(struct device *dev)
{
	DHD_INFO(("%s: enter\n", __FUNCTION__));
}

/* trigger coredump */
static int
dhd_set_coredump(const char *buf, int buf_len, const char *info)
{
	struct sscd_platform_data *pdata = dev_get_platdata(&sscd_dev.dev);
	struct sscd_segment seg;

	if (pdata->sscd_report) {
		memset(&seg, 0, sizeof(seg));
		seg.addr = (void *) buf;
		seg.size = buf_len;
		pdata->sscd_report(&sscd_dev, &seg, 1, 0, info);
	}
	return 0;
}
#endif /* DHD_COREDUMP */

#ifdef GET_CUSTOM_MAC_ENABLE

#define CDB_PATH "/chosen/config"
#define WIFI_MAC "wlan_mac1"
static u8 wlan_mac[6] = {0};

static int
dhd_wlan_get_mac_addr(unsigned char *buf)
{
	if (memcmp(wlan_mac, "\0\0\0\0\0\0", 6)) {
		memcpy(buf, wlan_mac, sizeof(wlan_mac));
		return 0;
	}
	return -EIO;
}

int
dhd_wlan_init_mac_addr(void)
{
	u8 mac[6] = {0};
	unsigned int size;
	unsigned char *mac_addr = NULL;
	struct device_node *node;
	unsigned int mac_found = 0;

	node = of_find_node_by_path(CDB_PATH);
	if (!node) {
		DHD_ERROR(("CDB Node not created under %s\n", CDB_PATH));
		return -ENODEV;
	} else {
		mac_addr = (unsigned char *)
				of_get_property(node, WIFI_MAC, &size);
	}

	/* In case Missing Provisioned MAC Address, exit with error */
	if (!mac_addr) {
		DHD_ERROR(("Missing Provisioned MAC address\n"));
		return -EINVAL;
	}

	/* Start decoding MAC Address
	 * Note that 2 formats are supported for now
	 * AA:BB:CC:DD:EE:FF (with separating colons) and
	 * AABBCCDDEEFF (without separating colons)
	 */
	if (sscanf(mac_addr,
			"%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
			&mac[0], &mac[1], &mac[2], &mac[3], &mac[4],
			&mac[5]) == 6) {
		mac_found = 1;
	} else if (sscanf(mac_addr,
			"%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
			&mac[0], &mac[1], &mac[2], &mac[3], &mac[4],
			&mac[5]) == 6) {
		mac_found = 1;
	}

	/* Make sure Address decoding succeeds */
	if (!mac_found) {
		DHD_ERROR(("Invalid format for Provisioned MAC Address\n"));
		return -EINVAL;
	}

	/* Make sure Provisioned MAC Address is globally Administered */
	if (mac[0] & 2) {
		DHD_ERROR(("Invalid Provisioned MAC Address\n"));
		return -EINVAL;
	}

	memcpy(wlan_mac, mac, sizeof(mac));
	return 0;
}
#endif /* GET_CUSTOM_MAC_ENABLE */

#ifdef SUPPORT_MULTIPLE_NVRAM

#define CMDLINE_REVISION_KEY "androidboot.revision="
#define CMDLINE_SKU_KEY "androidboot.hardware.sku="

char val_revision[MAX_HW_INFO_LEN] = {0};
char val_sku[MAX_HW_INFO_LEN] = {0};

int
dhd_wlan_init_hardware_info(void)
{

	struct device_node *node;
	char *cp;
	const char *command_line = NULL;
	char match_str[MAX_HW_INFO_LEN] = {0};
	size_t len;

	node = of_find_node_by_path("/chosen");
	if (!node) {
		DHD_ERROR(("Node not created under chosen\n"));
		return -ENODEV;
	} else {

		of_property_read_string(node, "bootargs", &command_line);
		len = strlen(command_line);

		cp = strnstr(command_line, CMDLINE_REVISION_KEY, len);
		if (cp) {
			sscanf(cp, CMDLINE_REVISION_KEY"%s", val_revision);
		}

		cp = strnstr(command_line, CMDLINE_SKU_KEY, len);
		if (cp) {
			sscanf(cp, CMDLINE_SKU_KEY"%s", match_str);
			if (strcmp(match_str, "G9S9B") == 0 ||
				strcmp(match_str, "G8V0U") == 0 ||
				strcmp(match_str, "GFQM1") == 0) {
				strcpy(val_sku, "MMW");
			} else if (strcmp(match_str, "GR1YH") == 0 ||
				strcmp(match_str, "GF5KQ") == 0 ||
				strcmp(match_str, "GPQ72") == 0) {
				strcpy(val_sku, "JPN");
			} else if (strcmp(match_str, "GB7N6") == 0 ||
				strcmp(match_str, "GLU0G") == 0 ||
				strcmp(match_str, "GNA8F") == 0) {
				strcpy(val_sku, "ROW");
			} else {
				strcpy(val_sku, "NA");
			}
		}
	}

	return 0;
}
#endif /* SUPPORT_MULTIPLE_NVRAM */

int
dhd_wifi_init_gpio(void)
{
	int gpio_reg_on_val;
	/* ========== WLAN_PWR_EN ============ */
	char *wlan_node = DHD_DT_COMPAT_ENTRY;
	struct device_node *root_node = NULL;

	root_node = of_find_compatible_node(NULL, NULL, wlan_node);
	if (!root_node) {
		DHD_ERROR(("failed to get device node of BRCM WLAN\n"));
		return -ENODEV;
	}

	wlan_reg_on = of_get_named_gpio(root_node, WIFI_WL_REG_ON_PROPNAME, 0);
	if (!gpio_is_valid(wlan_reg_on)) {
		DHD_ERROR(("Invalid gpio pin : %d\n", wlan_reg_on));
		return -ENODEV;
	}

	/* ========== WLAN_PWR_EN ============ */
	DHD_INFO(("%s: gpio_wlan_power : %d\n", __FUNCTION__, wlan_reg_on));

#ifdef EXYNOS_PCIE_RC_ONOFF
	if (of_property_read_u32(root_node, "ch-num", &pcie_ch_num)) {
		DHD_INFO(("%s: Failed to parse the channel number\n", __FUNCTION__));
		return -EINVAL;
	}
	/* ========== WLAN_PCIE_NUM ============ */
	DHD_INFO(("%s: pcie_ch_num : %d\n", __FUNCTION__, pcie_ch_num));
#endif /* EXYNOS_PCIE_RC_ONOFF */

	/*
	 * For reg_on, gpio_request will fail if the gpio is configured to output-high
	 * in the dts using gpio-hog, so do not return error for failure.
	 */
	if (gpio_request_one(wlan_reg_on, GPIOF_OUT_INIT_HIGH, "WL_REG_ON")) {
		DHD_ERROR(("%s: Failed to request gpio %d for WL_REG_ON, "
			"might have configured in the dts\n",
			__FUNCTION__, wlan_reg_on));
	} else {
		DHD_ERROR(("%s: gpio_request WL_REG_ON done - WLAN_EN: GPIO %d\n",
			__FUNCTION__, wlan_reg_on));
	}

	gpio_reg_on_val = gpio_get_value(wlan_reg_on);
	DHD_INFO(("%s: Initial WL_REG_ON: [%d]\n",
		__FUNCTION__, gpio_get_value(wlan_reg_on)));

	if (gpio_reg_on_val == 0) {
		DHD_INFO(("%s: WL_REG_ON is LOW, drive it HIGH\n", __FUNCTION__));
		if (gpio_direction_output(wlan_reg_on, 1)) {
			DHD_ERROR(("%s: WL_REG_ON is failed to pull up\n", __FUNCTION__));
			return -EIO;
		}
	}

	DHD_ERROR(("%s: WL_REG_ON is pulled up\n", __FUNCTION__));

	/* Wait for WIFI_TURNON_DELAY due to power stability */
	msleep(WIFI_TURNON_DELAY);
#ifdef EXYNOS_PCIE_RC_ONOFF
	if (exynos_pcie_pm_resume(pcie_ch_num)) {
		WARN(1, "pcie link up failure\n");
		return -ENODEV;
	}
#endif /* EXYNOS_PCIE_RC_ONOFF */
#ifdef CONFIG_BCMDHD_OOB_HOST_WAKE
	/* ========== WLAN_HOST_WAKE ============ */
	wlan_host_wake_up = of_get_named_gpio(root_node,
		WIFI_WLAN_HOST_WAKE_PROPNAME, 0);
	DHD_INFO(("%s: gpio_wlan_host_wake : %d\n", __FUNCTION__, wlan_host_wake_up));

	if (gpio_request_one(wlan_host_wake_up, GPIOF_IN, "WLAN_HOST_WAKE")) {
		DHD_ERROR(("%s: Failed to request gpio %d for WLAN_HOST_WAKE\n",
			__FUNCTION__, wlan_host_wake_up));
			return -ENODEV;
	} else {
		DHD_ERROR(("%s: gpio_request WLAN_HOST_WAKE done"
			" - WLAN_HOST_WAKE: GPIO %d\n",
			__FUNCTION__, wlan_host_wake_up));
	}

	if (gpio_direction_input(wlan_host_wake_up)) {
		DHD_ERROR(("%s: Failed to set WL_HOST_WAKE gpio direction\n", __FUNCTION__));
	}

	wlan_host_wake_irq = gpio_to_irq(wlan_host_wake_up);
#endif /* CONFIG_BCMDHD_OOB_HOST_WAKE */
	return 0;
}

int
dhd_wlan_power(int onoff)
{
	DHD_INFO(("------------------------------------------------\n"));
	DHD_INFO(("------------------------------------------------\n"));
	DHD_INFO(("%s Enter: power %s\n", __func__, onoff ? "on" : "off"));

	if (onoff) {
		if (gpio_direction_output(wlan_reg_on, 1)) {
			DHD_ERROR(("%s: WL_REG_ON is failed to pull up\n", __FUNCTION__));
			return -EIO;
		}
		if (gpio_get_value(wlan_reg_on)) {
			DHD_INFO(("WL_REG_ON on-step-2 : [%d]\n",
				gpio_get_value(wlan_reg_on)));
		} else {
			DHD_ERROR(("[%s] gpio value is 0. We need reinit.\n", __func__));
			if (gpio_direction_output(wlan_reg_on, 1)) {
				DHD_ERROR(("%s: WL_REG_ON is "
					"failed to pull up\n", __func__));
			}
		}
	} else {
		if (gpio_direction_output(wlan_reg_on, 0)) {
			DHD_ERROR(("%s: WL_REG_ON is failed to pull up\n", __FUNCTION__));
			return -EIO;
		}
		if (gpio_get_value(wlan_reg_on)) {
			DHD_INFO(("WL_REG_ON on-step-2 : [%d]\n",
				gpio_get_value(wlan_reg_on)));
		}
	}
	return 0;
}
EXPORT_SYMBOL(dhd_wlan_power);

static int
dhd_wlan_reset(int onoff)
{
	return 0;
}

static int
dhd_wlan_set_carddetect(int val)
{
	return 0;
}

#ifndef SUPPORT_EXYNOS7420
#include <linux/exynos-pci-noti.h>
extern int exynos_pcie_register_event(struct exynos_pcie_register_event *reg);
extern int exynos_pcie_deregister_event(struct exynos_pcie_register_event *reg);
#endif /* !SUPPORT_EXYNOS7420 */

#include <dhd_plat.h>

typedef struct dhd_plat_info {
	struct exynos_pcie_register_event pcie_event;
	struct exynos_pcie_notify pcie_notify;
	struct pci_dev *pdev;
} dhd_plat_info_t;

static dhd_pcie_event_cb_t g_pfn = NULL;

uint32 dhd_plat_get_info_size(void)
{
	return sizeof(dhd_plat_info_t);
}

void plat_pcie_notify_cb(struct exynos_pcie_notify *pcie_notify)
{
	struct pci_dev *pdev;

	if (pcie_notify == NULL) {
		pr_err("%s(): Invalid argument to Platform layer call back \r\n", __func__);
		return;
	}

	if (g_pfn) {
		pdev = (struct pci_dev *)pcie_notify->user;
		pr_err("%s(): Invoking DHD call back with pdev %p \r\n",
				__func__, pdev);
		(*(g_pfn))(pdev);
	} else {
		pr_err("%s(): Driver Call back pointer is NULL \r\n", __func__);
	}
	return;
}

int dhd_plat_pcie_register_event(void *plat_info, struct pci_dev *pdev, dhd_pcie_event_cb_t pfn)
{
		dhd_plat_info_t *p = plat_info;

#ifndef SUPPORT_EXYNOS7420
		if ((p == NULL) || (pdev == NULL) || (pfn == NULL)) {
			pr_err("%s(): Invalid argument p %p, pdev %p, pfn %p\r\n",
				__func__, p, pdev, pfn);
			return -1;
		}
		g_pfn = pfn;
		p->pdev = pdev;
		p->pcie_event.events = EXYNOS_PCIE_EVENT_LINKDOWN;
		p->pcie_event.user = pdev;
		p->pcie_event.mode = EXYNOS_PCIE_TRIGGER_CALLBACK;
		p->pcie_event.callback = plat_pcie_notify_cb;
		exynos_pcie_register_event(&p->pcie_event);
		pr_err("%s(): Registered Event PCIe event pdev %p \r\n", __func__, pdev);
		return 0;
#else
		return 0;
#endif /* SUPPORT_EXYNOS7420 */
}

void dhd_plat_pcie_deregister_event(void *plat_info)
{
	dhd_plat_info_t *p = plat_info;
#ifndef SUPPORT_EXYNOS7420
	if (p) {
		exynos_pcie_deregister_event(&p->pcie_event);
	}
#endif /* SUPPORT_EXYNOS7420 */
	return;
}

static int
set_affinity(unsigned int irq, const struct cpumask *cpumask)
{
#ifdef BCMDHD_MODULAR
	return irq_set_affinity_hint(irq, cpumask);
#else
	return irq_set_affinity(irq, cpumask);
#endif
}

static void
irq_affinity_hysteresis_control(struct pci_dev *pdev, int resched_streak_max)
{
	int err = 0;
	if (!pdev) {
		DHD_ERROR(("%s : pdev is NULL\n", __FUNCTION__));
		return;
	}
	if (!is_irq_on_big_core && (resched_streak_max >= RESCHED_STREAK_MAX_HIGH)) {
		err = set_affinity(pdev->irq, cpumask_of(IRQ_AFFINITY_BIG_CORE));
		if (!err) {
			is_irq_on_big_core = TRUE;
			DHD_INFO(("%s switches to big core \n", __FUNCTION__));
		}
	}
	if (is_irq_on_big_core && (resched_streak_max <= RESCHED_STREAK_MAX_LOW)) {
		err = set_affinity(pdev->irq, &(CPU_MASK_ALL));
		if (!err) {
			is_irq_on_big_core = FALSE;
			DHD_INFO(("%s switches to all cores\n", __FUNCTION__));
		}
	}
}

/*
 * DHD Core layer reports whether the bottom half is getting rescheduled or not
 * resched = 1, BH is getting rescheduled.
 * resched = 0, BH is NOT getting rescheduled.
 * resched is used to detect bottom half load and configure IRQ affinity dynamically
 */
void dhd_plat_report_bh_sched(void *plat_info, int resched)
{
	dhd_plat_info_t *p = plat_info;
	uint64 curr_time_ns;
	uint64 time_delta_ns;

	if (resched > 0) {
		resched_streak++;
		return;
	}

	if (resched_streak > resched_streak_max) {
		resched_streak_max = resched_streak;
	}
	resched_streak = 0;

	curr_time_ns = OSL_LOCALTIME_NS();
	time_delta_ns = curr_time_ns - last_resched_cnt_check_time_ns;
	if (time_delta_ns < (RESCHED_CNT_CHECK_PERIOD_SEC * NSEC_PER_SEC)) {
		return;
	}
	last_resched_cnt_check_time_ns = curr_time_ns;

	DHD_INFO(("%s resched_streak_max=%d\n",
		__FUNCTION__, resched_streak_max));

	irq_affinity_hysteresis_control(p->pdev, resched_streak_max);

	resched_streak_max = 0;
	return;
}

#ifdef BCMSDIO
static int dhd_wlan_get_wake_irq(void)
{
	return gpio_to_irq(wlan_host_wake_up);
}
#endif /* BCMSDIO */

#if defined(CONFIG_BCMDHD_OOB_HOST_WAKE) && defined(CONFIG_BCMDHD_GET_OOB_STATE)
int
dhd_get_wlan_oob_gpio(void)
{
	return gpio_is_valid(wlan_host_wake_up) ?
		gpio_get_value(wlan_host_wake_up) : -1;
}
EXPORT_SYMBOL(dhd_get_wlan_oob_gpio);
#endif /* CONFIG_BCMDHD_OOB_HOST_WAKE && CONFIG_BCMDHD_GET_OOB_STATE */

struct resource dhd_wlan_resources = {
	.name	= "bcmdhd_wlan_irq",
	.start	= 0, /* Dummy */
	.end	= 0, /* Dummy */
	.flags	= IORESOURCE_IRQ | IORESOURCE_IRQ_SHAREABLE |
	IORESOURCE_IRQ_HIGHEDGE,
};
EXPORT_SYMBOL(dhd_wlan_resources);

struct wifi_platform_data dhd_wlan_control = {
	.set_power	= dhd_wlan_power,
	.set_reset	= dhd_wlan_reset,
	.set_carddetect	= dhd_wlan_set_carddetect,
#ifdef DHD_COREDUMP
	.set_coredump = dhd_set_coredump,
#endif /* DHD_COREDUMP */
#ifdef CONFIG_BROADCOM_WIFI_RESERVED_MEM
	.mem_prealloc	= dhd_wlan_mem_prealloc,
#endif /* CONFIG_BROADCOM_WIFI_RESERVED_MEM */
#ifdef GET_CUSTOM_MAC_ENABLE
	.get_mac_addr = dhd_wlan_get_mac_addr,
#endif /* GET_CUSTOM_MAC_ENABLE */
#ifdef BCMSDIO
	.get_wake_irq	= dhd_wlan_get_wake_irq,
#endif // endif
};
EXPORT_SYMBOL(dhd_wlan_control);

int
dhd_wlan_init(void)
{
	int ret;

	DHD_INFO(("%s: START.......\n", __FUNCTION__));

#ifdef DHD_COREDUMP
	platform_device_register(&sscd_dev);
#endif /* DHD_COREDUMP */

	ret = dhd_wifi_init_gpio();
	if (ret < 0) {
		DHD_ERROR(("%s: failed to initiate GPIO, ret=%d\n",
			__FUNCTION__, ret));
		goto fail;
	}
#ifdef CONFIG_BCMDHD_OOB_HOST_WAKE
	dhd_wlan_resources.start = wlan_host_wake_irq;
	dhd_wlan_resources.end = wlan_host_wake_irq;
#endif /* CONFIG_BCMDHD_OOB_HOST_WAKE */

#ifdef CONFIG_BROADCOM_WIFI_RESERVED_MEM
	ret = dhd_init_wlan_mem();
	if (ret < 0) {
		DHD_ERROR(("%s: failed to alloc reserved memory,"
					" ret=%d\n", __FUNCTION__, ret));
		goto fail;
	}
#endif /* CONFIG_BROADCOM_WIFI_RESERVED_MEM */

#ifdef GET_CUSTOM_MAC_ENABLE
	dhd_wlan_init_mac_addr();
#endif /* GET_CUSTOM_MAC_ENABLE */

#ifdef SUPPORT_MULTIPLE_NVRAM
	dhd_wlan_init_hardware_info();
#endif /* SUPPORT_MULTIPLE_NVRAM */

fail:
	DHD_ERROR(("%s: FINISH.......\n", __FUNCTION__));
	return ret;
}

int
dhd_wlan_deinit(void)
{
	if (gpio_is_valid(wlan_host_wake_up)) {
		gpio_free(wlan_host_wake_up);
	}
	if (gpio_is_valid(wlan_reg_on)) {
		gpio_free(wlan_reg_on);
	}

#ifdef DHD_COREDUMP
	platform_device_unregister(&sscd_dev);
#endif /* DHD_COREDUMP */

	return 0;
}
#ifndef BCMDHD_MODULAR
/* Required only for Built-in DHD */
device_initcall(dhd_wlan_init);
#endif /* BCMDHD_MODULAR */
