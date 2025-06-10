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

#define LOG_TAG           "zasoby_usb"
#define INTERVAL_SECONDS  1
#define USB_VENDOR_ID     0x303A    /* PODMIEŃ na VID */
#define USB_PRODUCT_ID    0x4001    /* PODMIEŃ na PID */
#define PROCFS_NAME       "mydriver"
#define BULK_IN_SIZE      512

static struct proc_dir_entry *proc_entry;
static bool use_gib = false;

static struct timer_list zasoby_timer;
static struct workqueue_struct *zasoby_wq;
static struct work_struct zasoby_work;
static u64 last_user, last_idle, last_system;

struct zasoby_usb {
    struct usb_device    *udev;
    struct usb_interface *interface;
    __u8                  bulk_out_ep;
    __u8                  bulk_in_ep;
    struct semaphore      limit_sem;
};
static struct zasoby_usb *g_zasoby_dev;

/* procfs handlers */
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

static const struct proc_ops proc_file_ops = {
    .proc_open    = proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
    .proc_write   = proc_write,
};

/* forward declarations */
static void zasoby_work_fn(struct work_struct *work);
static void zasoby_callback(struct timer_list *t);
static int zasoby_probe(struct usb_interface *intf,
                        const struct usb_device_id *id);
static void zasoby_disconnect(struct usb_interface *intf);
static void write_bulk_callback(struct urb *urb);

/* USB IDs */
static const struct usb_device_id zasoby_id_table[] = {
    { USB_DEVICE(USB_VENDOR_ID, USB_PRODUCT_ID) },
    { }
};
MODULE_DEVICE_TABLE(usb, zasoby_id_table);

static struct usb_driver zasoby_usb_driver = {
    .name       = "zasoby_usb",
    .probe      = zasoby_probe,
    .disconnect = zasoby_disconnect,
    .id_table   = zasoby_id_table,
};

/* write completion callback */
static void write_bulk_callback(struct urb *urb)
{
    struct zasoby_usb *dev = urb->context;
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
static void zasoby_work_fn(struct work_struct *work)
{
    struct sysinfo info;
    u64 tu = 0, ti = 0, ts = 0;
    u64 du, di, ds, dt;
    unsigned int pu = 0, ps = 0, pi = 0;
    int cpu, ret;
    char stats[64], cmd[80];
    struct zasoby_usb *dev = g_zasoby_dev;
    struct urb *urb;
    void *buf;
    dma_addr_t dma;

    /* CPU usage calc */
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

    /* Memory info */
    si_meminfo(&info);
    {
        unsigned long total_mib = (info.totalram << (PAGE_SHIFT - 10)) / 1024;
        unsigned long free_mib  = (info.freeram  << (PAGE_SHIFT - 10)) / 1024;
        unsigned long total = use_gib ? (total_mib >> 10) : total_mib;
        unsigned long free  = use_gib ? (free_mib  >> 10) : free_mib;
        const char *unit = use_gib ? "GiB" : "MiB";
        snprintf(stats, sizeof(stats), "User: %u;System: %u;Idle: %u;RAM USED:%lu%s;RAM FREEE:%lu%s",
                 pu, ps, pi, total, unit, free, unit);
    }

    /* build command with stats */
    snprintf(cmd, sizeof(cmd), "%s", stats);

    /* wait for buffer semaphore */
    if (down_interruptible(&dev->limit_sem))
        return;

    /* allocate URB and coherent buffer */
    urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!urb) { up(&dev->limit_sem); return; }
    buf = usb_alloc_coherent(dev->udev, strlen(cmd), GFP_KERNEL, &dma);
    if (!buf) { usb_free_urb(urb); up(&dev->limit_sem); return; }
    memcpy(buf, cmd, strlen(cmd));

    /* fill and submit URB */
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

    /* re-arm timer */
    mod_timer(&zasoby_timer, jiffies + HZ * INTERVAL_SECONDS);
}

/* timer callback: queue work */
static void zasoby_callback(struct timer_list *t)
{
    queue_work(zasoby_wq, &zasoby_work);
}

/* USB probe */
static int zasoby_probe(struct usb_interface *intf,
                        const struct usb_device_id *id)
{
    struct usb_host_interface *alt = intf->cur_altsetting;
    struct usb_endpoint_descriptor *ep;
    struct zasoby_usb *dev;
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
    g_zasoby_dev = dev;

    zasoby_wq = create_singlethread_workqueue("zasoby_wq");
    if (!zasoby_wq)
        goto fail;
    INIT_WORK(&zasoby_work, zasoby_work_fn);

    timer_setup(&zasoby_timer, zasoby_callback, 0);
    mod_timer(&zasoby_timer, jiffies + HZ * INTERVAL_SECONDS);

    pr_info(LOG_TAG ": connected\n");
    return 0;

fail:
    usb_put_dev(dev->udev);
    kfree(dev);
    return -ENODEV;
}

/* USB disconnect */
static void zasoby_disconnect(struct usb_interface *intf)
{
    struct zasoby_usb *dev = usb_get_intfdata(intf);

    del_timer_sync(&zasoby_timer);
    cancel_work_sync(&zasoby_work);
    destroy_workqueue(zasoby_wq);
    usb_put_dev(dev->udev);
    kfree(dev);
    pr_info(LOG_TAG ": disconnected\n");
}

/* module init/exit */
static int __init zasoby_init(void)
{
    int ret;
    proc_entry = proc_create(PROCFS_NAME, 0666, NULL, &proc_file_ops);
    if (!proc_entry)
        return -ENOMEM;

    ret = usb_register(&zasoby_usb_driver);
    if (ret) {
        proc_remove(proc_entry);
        return ret;
    }
    pr_info(LOG_TAG ": module loaded\n");
    return 0;
}

static void __exit zasoby_exit(void)
{
    usb_deregister(&zasoby_usb_driver);
    proc_remove(proc_entry);
    pr_info(LOG_TAG ": module unloaded\n");
}

module_init(zasoby_init);
module_exit(zasoby_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("student_debil");
MODULE_DESCRIPTION(
    "Moduł CPU/RAM co 1s + USB BULK OUT/IN cez URB + dynamic stats cmd"
);
