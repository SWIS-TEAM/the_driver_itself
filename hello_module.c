// SPDX-License-Identifier: GPL-2.0
/*
 * usb zasoby module with procfs unit selection + USB BULK OUT
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/sysinfo.h>
#include <linux/kernel_stat.h>
#include <linux/smp.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>

#define LOG_TAG           "zasoby_usb"
#define INTERVAL_SECONDS  1
#define USB_VENDOR_ID     0x1234    /* PODMIENIĆ na VID */
#define USB_PRODUCT_ID    0x5678    /* PODMIENIĆ na PID */
#define PROCFS_NAME       "mydriver"/* nazwa w /proc */

static struct timer_list zasoby_timer;
static struct proc_dir_entry *proc_entry;
static bool use_gib;               /* false=MiB, true=GiB */

/* --- USB skeleton definitions --- */
static const struct usb_device_id zasoby_id_table[] = {
    { USB_DEVICE(USB_VENDOR_ID, USB_PRODUCT_ID) },
    { }  /* terminator */
};
MODULE_DEVICE_TABLE(usb, zasoby_id_table);

struct zasoby_usb {
    struct usb_device *udev;
    __u8               bulk_out_ep;
    struct mutex       lock;
};
static struct zasoby_usb *g_zasoby_dev;

/* --- procfs write handler --- */
static ssize_t procfs_write(struct file *file,
                            const char __user *buf,
                            size_t count, loff_t *pos)
{
    char kbuf[8];

    if (count >= sizeof(kbuf))
        return -EINVAL;
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;
    kbuf[count] = '\0';

    if (!strncmp(kbuf, "MiB", 3))
        use_gib = false;
    else if (!strncmp(kbuf, "GiB", 3))
        use_gib = true;
    else
        return -EINVAL;

    return count;
}

static const struct proc_ops proc_file_ops = {
    .proc_write = procfs_write,
};

/* --- previous CPU stats --- */
static u64 last_user, last_idle, last_system;

/* --- timer callback: zbiera i wysyła dane --- */
static void zasoby_callback(struct timer_list *t)
{
    struct sysinfo info;
    unsigned long total_mib, free_mib, total_gib, free_gib;
    u64 total_user=0, total_idle=0, total_sys=0;
    u64 d_user, d_idle, d_sys, d_tot;
    unsigned int p_user=0, p_sys=0, p_idle=0;
    int cpu, online, possible;
    char msg[128];
    int len, actual, ret;

    /* 1. CPU agregacja */
    for_each_online_cpu(cpu) {
        struct kernel_cpustat k = kcpustat_cpu(cpu);
        total_user += k.cpustat[CPUTIME_USER];
        total_idle += k.cpustat[CPUTIME_IDLE];
        total_sys  += k.cpustat[CPUTIME_SYSTEM];
    }
    d_user = total_user - last_user;
    d_idle = total_idle - last_idle;
    d_sys  = total_sys  - last_system;
    d_tot  = d_user + d_idle + d_sys;
    last_user   = total_user;
    last_idle   = total_idle;
    last_system = total_sys;
    if (d_tot) {
        p_user = (u64)100 * d_user / d_tot;
        p_sys  = (u64)100 * d_sys  / d_tot;
        p_idle = 100 - p_user - p_sys;
    }

    /* 2. RAM */
    si_meminfo(&info);
    total_mib = (info.totalram << (PAGE_SHIFT - 10)) / 1024;
    free_mib  = (info.freeram  << (PAGE_SHIFT - 10)) / 1024;
    total_gib = total_mib / 1024;
    free_gib  = free_mib  / 1024;

    /* 3. liczba CPU */
    online   = num_online_cpus();
    possible = num_possible_cpus();

    /* 4. formatowanie */
    if (use_gib)
        len = scnprintf(msg, sizeof(msg),
            "CPU u:%u%% s:%u%% i:%u%% [%d/%d]; RAM %lu/%lu GiB\n",
            p_user, p_sys, p_idle, online, possible,
            total_gib, free_gib);
    else
        len = scnprintf(msg, sizeof(msg),
            "CPU u:%u%% s:%u%% i:%u%% [%d/%d]; RAM %lu/%lu MiB\n",
            p_user, p_sys, p_idle, online, possible,
            total_mib, free_mib);

    printk(KERN_INFO LOG_TAG ": %s", msg);

    /* 5. wysyłka przez USB BULK OUT, tylko gdy podłączone */
    if (g_zasoby_dev) {
        mutex_lock(&g_zasoby_dev->lock);
        ret = usb_bulk_msg(g_zasoby_dev->udev,
            usb_sndbulkpipe(g_zasoby_dev->udev,
                            g_zasoby_dev->bulk_out_ep),
            msg, len, &actual, 1000);
        if (ret)
            dev_err(&g_zasoby_dev->udev->dev,
                "bulk-out error %d\n", ret);
        mutex_unlock(&g_zasoby_dev->lock);
    }

    /* 6. restart timera */
    mod_timer(&zasoby_timer, jiffies + HZ * INTERVAL_SECONDS);
}

/* --- USB probe / disconnect --- */
static int zasoby_probe(struct usb_interface *intf,
                        const struct usb_device_id *id)
{
    struct usb_host_interface *alt = intf->cur_altsetting;
    struct usb_endpoint_descriptor *ep;
    int i;

    if (g_zasoby_dev)
        return -EBUSY;

    g_zasoby_dev = kzalloc(sizeof(*g_zasoby_dev), GFP_KERNEL);
    if (!g_zasoby_dev)
        return -ENOMEM;

    mutex_init(&g_zasoby_dev->lock);
    g_zasoby_dev->udev = usb_get_dev(interface_to_usbdev(intf));

    for (i = 0; i < alt->desc.bNumEndpoints; ++i) {
        ep = &alt->endpoint[i].desc;
        if (usb_endpoint_is_bulk_out(ep)) {
            g_zasoby_dev->bulk_out_ep = ep->bEndpointAddress;
            break;
        }
    }
    if (!g_zasoby_dev->bulk_out_ep) {
        dev_err(&intf->dev, "no bulk-out endpoint\n");
        usb_put_dev(g_zasoby_dev->udev);
        kfree(g_zasoby_dev);
        g_zasoby_dev = NULL;
        return -ENODEV;
    }

    /* start timer dopiero teraz */
    timer_setup(&zasoby_timer, zasoby_callback, 0);
    mod_timer(&zasoby_timer, jiffies + HZ * INTERVAL_SECONDS);

    dev_info(&intf->dev, "zasoby_usb: device connected\n");
    return 0;
}

static void zasoby_disconnect(struct usb_interface *intf)
{
    if (!g_zasoby_dev)
        return;

    /* zatrzymaj timer */
    del_timer_sync(&zasoby_timer);

    usb_put_dev(g_zasoby_dev->udev);
    kfree(g_zasoby_dev);
    g_zasoby_dev = NULL;
    dev_info(&intf->dev, "zasoby_usb: device disconnected\n");
}

static struct usb_driver zasoby_usb_driver = {
    .name       = "zasoby_usb",
    .probe      = zasoby_probe,
    .disconnect = zasoby_disconnect,
    .id_table   = zasoby_id_table,
};

/* --- module init/exit --- */
static int __init zasoby_init(void)
{
    int ret;

    pr_info(LOG_TAG ": module init\n");

    /* procfs */
    proc_entry = proc_create(PROCFS_NAME, 0666, NULL, &proc_file_ops);
    if (!proc_entry) {
        pr_err("failed to create /proc/%s\n", PROCFS_NAME);
        return -ENOMEM;
    }

    /* rejestracja USB drivera */
    ret = usb_register(&zasoby_usb_driver);
    if (ret) {
        pr_err("usb_register failed: %d\n", ret);
        proc_remove(proc_entry);
        return ret;
    }
    return 0;
}

static void __exit zasoby_exit(void)
{
    /* deregister driver i procfs */
    usb_deregister(&zasoby_usb_driver);
    proc_remove(proc_entry);
    pr_info(LOG_TAG ": module exit\n");
}

module_init(zasoby_init);
module_exit(zasoby_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ChatGPT");
MODULE_DESCRIPTION(
    "Moduł CPU/RAM co 1s + USB BULK OUT + procfs MiB/GiB sel, timer w probe");
