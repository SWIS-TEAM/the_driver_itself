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
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#define LOG_TAG           "zasoby_usb"
#define INTERVAL_SECONDS  1
#define USB_VENDOR_ID     0x303A    /* PODMIEŃ na VID */
#define USB_PRODUCT_ID    0x4001    /* PODMIEŃ na PID */
#define PROCFS_NAME       "mydriver"

static struct proc_dir_entry *proc_entry;
static bool use_gib = false;  /* domyślnie MiB */

static struct timer_list zasoby_timer;
static u64 last_user, last_idle, last_system;

/* Workqueue definitions */
static struct workqueue_struct *zasoby_wq;
static DECLARE_WORK(zasoby_work, zasoby_work_fn);

/* Forward declarations */
static void zasoby_callback(struct timer_list *t);
static void zasoby_work_fn(struct work_struct *work);
static int  zasoby_probe(struct usb_interface *intf,
                        const struct usb_device_id *id);
static void zasoby_disconnect(struct usb_interface *intf);

/* --- procfs show / write --- */
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

/* --- USB skeleton definitions --- */
static const struct usb_device_id zasoby_id_table[] = {
    { USB_DEVICE(USB_VENDOR_ID, USB_PRODUCT_ID) },
    { }
};
MODULE_DEVICE_TABLE(usb, zasoby_id_table);

struct zasoby_usb {
    struct usb_device *udev;
    __u8               bulk_out_ep;
    __u8               bulk_in_ep;
    struct mutex       lock;
};
static struct zasoby_usb *g_zasoby_dev;

/* timer callback: just re-arm timer and queue work */
static void zasoby_callback(struct timer_list *t)
{
    mod_timer(&zasoby_timer, jiffies + HZ * INTERVAL_SECONDS);
    queue_work(zasoby_wq, &zasoby_work);
}

/* workqueue handler: CPU/RAM + USB BULK OUT/IN */
static void zasoby_work_fn(struct work_struct *work)
{
    struct sysinfo info;
    u64 tu = 0, ti = 0, ts = 0;
    u64 du, di, ds, dt;
    unsigned int pu = 0, ps = 0, pi = 0;
    int cpu;
    char msg[128];
    int len, actual, ret;

    /* CPU usage calc */
    for_each_online_cpu(cpu) {
        struct kernel_cpustat k = kcpustat_cpu(cpu);
        tu += k.cpustat[CPUTIME_USER];
        ti += k.cpustat[CPUTIME_IDLE];
        ts += k.cpustat[CPUTIME_SYSTEM];
    }
    du = tu - last_user;  di = ti - last_idle;  ds = ts - last_system;
    dt = du + di + ds;
    last_user   = tu;
    last_idle   = ti;
    last_system = ts;
    if (dt) {
        pu = (100ULL * du) / dt;
        ps = (100ULL * ds) / dt;
        pi = 100 - pu - ps;
    }

    /* Memory info */
    si_meminfo(&info);

    /* prepare message (toggle between two commands) */
    {
        static bool second = false;
        const char *cmd1 = "CPU:30;RAM:20;DISK:10";
        const char *cmd2 = "CPU:40;RAM:50;DISK:30";
        const char *to_send = second ? cmd2 : cmd1;
        second = !second;
        len = strlen(to_send);
        memcpy(msg, to_send, len);
    }

    /* send via bulk out/in */
    if (g_zasoby_dev) {
        mutex_lock(&g_zasoby_dev->lock);
        ret = usb_bulk_msg(g_zasoby_dev->udev,
            usb_sndbulkpipe(g_zasoby_dev->udev,
                g_zasoby_dev->bulk_out_ep),
            msg, len, &actual, 1000);
        if (ret)
            dev_err(&g_zasoby_dev->udev->dev,
                "bulk-out error %d\n", ret);

        {
            char resp[128];
            int rlen;
            ret = usb_bulk_msg(g_zasoby_dev->udev,
                usb_rcvbulkpipe(g_zasoby_dev->udev,
                    g_zasoby_dev->bulk_in_ep),
                resp, sizeof(resp)-1, &rlen, 1000);
            if (!ret) {
                resp[rlen] = '\0';
                pr_info(LOG_TAG ": reply: %s\n", resp);
            } else {
                dev_err(&g_zasoby_dev->udev->dev,
                    "bulk-in error %d\n", ret);
            }
        }
        mutex_unlock(&g_zasoby_dev->lock);
    }
}

/* USB probe/disconnect */
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
        if (usb_endpoint_is_bulk_out(ep))
            g_zasoby_dev->bulk_out_ep = ep->bEndpointAddress;
        else if (usb_endpoint_is_bulk_in(ep))
            g_zasoby_dev->bulk_in_ep = ep->bEndpointAddress;
    }
    if (!g_zasoby_dev->bulk_out_ep || !g_zasoby_dev->bulk_in_ep) {
        dev_err(&intf->dev, "brak BULK OUT/IN\n");
        goto error_probe;
    }

    /* create workqueue */
    zasoby_wq = create_singlethread_workqueue("zasoby_wq");
    if (!zasoby_wq) {
        dev_err(&intf->dev, "failed to create workqueue\n");
        goto error_probe;
    }
    INIT_WORK(&zasoby_work, zasoby_work_fn);

    /* setup timer */
    timer_setup(&zasoby_timer, zasoby_callback, 0);
    mod_timer(&zasoby_timer, jiffies + HZ * INTERVAL_SECONDS);

    dev_info(&intf->dev, "zasoby_usb: device connected\n");
    return 0;

error_probe:
    if (g_zasoby_dev->udev)
        usb_put_dev(g_zasoby_dev->udev);
    kfree(g_zasoby_dev);
    g_zasoby_dev = NULL;
    return -ENODEV;
}

static void zasoby_disconnect(struct usb_interface *intf)
{
    if (!g_zasoby_dev)
        return;

    del_timer_sync(&zasoby_timer);
    flush_workqueue(zasoby_wq);
    destroy_workqueue(zasoby_wq);
    usb_put_dev(g_zasoby_dev->udev);
    kfree(g_zasoby_dev);
    g_zasoby_dev = NULL;
    dev_info(&intf->dev, "zasoby_usb: device disconnected\n");
}

/* module init/exit */
static int __init zasoby_init(void)
{
    int ret;

    proc_entry = proc_create(PROCFS_NAME, 0666, NULL, &proc_file_ops);
    if (!proc_entry) {
        pr_err("failed to create /proc/%s\n", PROCFS_NAME);
        return -ENOMEM;
    }

    ret = usb_register(&zasoby_usb_driver);
    if (ret) {
        pr_err("usb_register failed: %d\n", ret);
        proc_remove(proc_entry);
        return ret;
    }

    pr_info(LOG_TAG ": module loaded (default MiB)\n");
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
    "Moduł CPU/RAM co 1s + USB BULK OUT/IN + procfs MiB/GiB sel; workqueue zamiast timer callback");