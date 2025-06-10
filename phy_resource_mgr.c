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
#include <linux/semaphore.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#define LOG_TAG           "phy_resource_mgr"
#define INTERVAL_SECONDS  1
#define USB_VENDOR_ID     0x303A
#define USB_PRODUCT_ID    0x4001
#define PROCFS_NAME       "mydriver"
#define BULK_IN_SIZE      512

static struct proc_dir_entry *proc_dir;
static struct proc_dir_entry *proc_entry_unit;
static struct proc_dir_entry *proc_entry_stats;
static bool use_gib = false;
static char last_stats[256];

static struct timer_list phy_resource_mgr_timer;
static struct workqueue_struct *phy_resource_mgr_wq;
static struct work_struct phy_resource_mgr_work;
static u64 last_user, last_idle, last_system;

struct phy_resource_mgr {
    struct usb_device    *udev;
    struct usb_interface *interface;
    __u8                  bulk_out_ep;
    __u8                  bulk_in_ep;
    struct semaphore      limit_sem;
};
static struct phy_resource_mgr *g_phy_resource_mgr_dev;

/* procfs: unit file */
static int proc_show(struct seq_file *m, void *v)
{
    seq_printf(m, "%s\n", use_gib ? "GiB" : "MiB");
    return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, proc_show, NULL);
}

static ssize_t proc_write(struct file *file,
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

static const struct proc_ops proc_unit_ops = {
    .proc_open    = proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
    .proc_write   = proc_write,
};

/* procfs: stats file */
static int proc_stats_show(struct seq_file *m, void *v)
{
    seq_printf(m, "%s\n", last_stats);
    return 0;
}

static int proc_stats_open(struct inode *inode, struct file *file)
{
    return single_open(file, proc_stats_show, NULL);
}

static const struct proc_ops proc_stats_ops = {
    .proc_open    = proc_stats_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* forward declarations */
static void phy_resource_mgr_work_fn(struct work_struct *work);
static void phy_resource_mgr_callback(struct timer_list *t);
static int phy_resource_mgr_probe(struct usb_interface *intf,
                        const struct usb_device_id *id);
static void phy_resource_mgr_disconnect(struct usb_interface *intf);
static void write_bulk_callback(struct urb *urb);

/* USB IDs */
static const struct usb_device_id phy_resource_mgr_id_table[] = {
    { USB_DEVICE(USB_VENDOR_ID, USB_PRODUCT_ID) },
    { }
};
MODULE_DEVICE_TABLE(usb, phy_resource_mgr_id_table);

static struct usb_driver phy_resource_mgr_driver = {
    .name       = "phy_resource_mgr",
    .probe      = phy_resource_mgr_probe,
    .disconnect = phy_resource_mgr_disconnect,
    .id_table   = phy_resource_mgr_id_table,
};

/* write completion callback */
static void write_bulk_callback(struct urb *urb)
{
    struct phy_resource_mgr *dev = urb->context;
    if (urb->status) {
        dev_err(&dev->interface->dev,
                "%s: status %d\n", __func__, urb->status);
    }
    usb_free_coherent(urb->dev,
                      urb->transfer_buffer_length,
                      urb->transfer_buffer,
                      urb->transfer_dma);
    up(&dev->limit_sem);
    usb_free_urb(urb);
}

/* workqueue handler: gather stats and send via USB */
static void phy_resource_mgr_work_fn(struct work_struct *work)
{
    struct sysinfo info;
    u64 tu = 0, ti = 0, ts = 0;
    u64 du, di, ds, dt;
    unsigned int pu = 0, ps = 0, pi = 0;
    int cpu, ret;
    char stats[256], cmd[256];
    struct phy_resource_mgr *dev = g_phy_resource_mgr_dev;
    struct urb *urb;
    void *buf;
    dma_addr_t dma;

    for_each_online_cpu(cpu) {
        struct kernel_cpustat k = kcpustat_cpu(cpu);
        tu += k.cpustat[CPUTIME_USER];
        ti += k.cpustat[CPUTIME_IDLE];
        ts += k.cpustat[CPUTIME_SYSTEM];
    }
    du = tu - last_user; di = ti - last_idle; ds = ts - last_system;
    dt = du + di + ds;
    last_user = tu; last_idle = ti; last_system = ts;
    if (dt) {
        pu = (100ULL * du) / dt;
        ps = (100ULL * ds) / dt;
        pi = 100 - pu - ps;
    }

    si_meminfo(&info);
    {
        unsigned long total_mib = (info.totalram << (PAGE_SHIFT - 10)) / 1024;
        unsigned long free_mib  = (info.freeram  << (PAGE_SHIFT - 10)) / 1024;
        unsigned long total = use_gib ? (total_mib >> 10) : total_mib;
        unsigned long free  = use_gib ? (free_mib  >> 10) : free_mib;
        const char *unit = use_gib ? "GiB" : "MiB";
        snprintf(stats, sizeof(stats), "User: %u%%; System: %u%%; Idle: %u%%; RAM_USED: %lu%s; OUT_OF: %lu%s",
                 pu, ps, pi, total - free, unit, total, unit);
        strncpy(last_stats, stats, sizeof(last_stats) - 1);
        last_stats[sizeof(last_stats) - 1] = '\0';
    }

    snprintf(cmd, sizeof(cmd), "%s", stats);

    if (down_interruptible(&dev->limit_sem))
        return;

    urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!urb) { up(&dev->limit_sem); return; }
    buf = usb_alloc_coherent(dev->udev, strlen(cmd), GFP_KERNEL, &dma);
    if (!buf) { usb_free_urb(urb); up(&dev->limit_sem); return; }
    memcpy(buf, cmd, strlen(cmd));

    usb_fill_bulk_urb(urb,
                      dev->udev,
                      usb_sndbulkpipe(dev->udev, dev->bulk_out_ep),
                      buf,
                      strlen(cmd),
                      write_bulk_callback,
                      dev);
    urb->transfer_dma = dma;
    urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
    ret = usb_submit_urb(urb, GFP_ATOMIC);
    if (ret) {
        dev_err(&dev->interface->dev,
                "%s submit error %d\n", __func__, ret);
        usb_free_coherent(dev->udev, strlen(cmd), buf, dma);
        usb_free_urb(urb);
        up(&dev->limit_sem);
    }

    mod_timer(&phy_resource_mgr_timer, jiffies + HZ * INTERVAL_SECONDS);
}

static void phy_resource_mgr_callback(struct timer_list *t)
{
    queue_work(phy_resource_mgr_wq, &phy_resource_mgr_work);
}

static int phy_resource_mgr_probe(struct usb_interface *intf,
                        const struct usb_device_id *id)
{
    struct usb_host_interface *alt = intf->cur_altsetting;
    struct usb_endpoint_descriptor *ep;
    struct phy_resource_mgr *dev;
    int i;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;
    dev->interface = intf;
    dev->udev = usb_get_dev(interface_to_usbdev(intf));
    sema_init(&dev->limit_sem, 1);

    for (i = 0; i < alt->desc.bNumEndpoints; i++) {
        ep = &alt->endpoint[i].desc;
        if (usb_endpoint_is_bulk_out(ep))
            dev->bulk_out_ep = ep->bEndpointAddress;
    }
    if (!dev->bulk_out_ep)
        goto fail;

    usb_set_intfdata(intf, dev);
    g_phy_resource_mgr_dev = dev;

    phy_resource_mgr_wq = create_singlethread_workqueue("phy_resource_mgr_wq");
    if (!phy_resource_mgr_wq)
        goto fail;
    INIT_WORK(&phy_resource_mgr_work, phy_resource_mgr_work_fn);

    timer_setup(&phy_resource_mgr_timer, phy_resource_mgr_callback, 0);
    mod_timer(&phy_resource_mgr_timer, jiffies + HZ * INTERVAL_SECONDS);

    pr_info(LOG_TAG ": connected\n");
    return 0;

fail:
    usb_put_dev(dev->udev);
    kfree(dev);
    return -ENODEV;
}

static void phy_resource_mgr_disconnect(struct usb_interface *intf)
{
    struct phy_resource_mgr *dev = usb_get_intfdata(intf);

    del_timer_sync(&phy_resource_mgr_timer);
    cancel_work_sync(&phy_resource_mgr_work);
    destroy_workqueue(phy_resource_mgr_wq);
    usb_put_dev(dev->udev);
    kfree(dev);
    pr_info(LOG_TAG ": disconnected\n");
}

static int __init phy_resource_mgr_init(void)
{
    int ret;

    proc_dir = proc_mkdir(PROCFS_NAME, NULL);
    if (!proc_dir)
        return -ENOMEM;

    proc_entry_unit = proc_create("unit", 0666, proc_dir, &proc_unit_ops);
    proc_entry_stats = proc_create("stats", 0444, proc_dir, &proc_stats_ops);
    if (!proc_entry_unit || !proc_entry_stats) {
        proc_remove(proc_dir);
        return -ENOMEM;
    }

    ret = usb_register(&phy_resource_mgr_driver);
    if (ret) {
        proc_remove(proc_dir);
        return ret;
    }
    pr_info(LOG_TAG ": module loaded\n");
    return 0;
}

static void __exit phy_resource_mgr_exit(void)
{
    usb_deregister(&phy_resource_mgr_driver);
    proc_remove(proc_dir);
    pr_info(LOG_TAG ": module unloaded\n");
}

module_init(phy_resource_mgr_init);
module_exit(phy_resource_mgr_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Piotrki: Baprawski i Polnau");
MODULE_DESCRIPTION(
    "Module CPU/RAM every 1s + USB BULK OUT/IN URB dynamic stats"
);