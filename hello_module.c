#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/smp.h>

#define LOG_TAG "zasoby_module"
#define INTERVAL_SECONDS 1

static struct timer_list zasoby_timer;

// Przechowywane wartości z poprzedniego pomiaru
static u64 last_user = 0;
static u64 last_idle = 0;
static u64 last_system = 0;

static void zasoby_callback(struct timer_list *timer)
{
    struct sysinfo info;
    unsigned long total_mib, free_mib;
    u64 total_user = 0, total_idle = 0, total_system = 0;
    int cpu;
    int online_cpus = num_online_cpus();
    int total_cpus = num_possible_cpus();

    // Sumowanie czasu CPU ze wszystkich rdzeni
    for_each_online_cpu(cpu) {
        struct kernel_cpustat kstat = kcpustat_cpu(cpu);
        total_user   += kstat.cpustat[CPUTIME_USER];
        total_idle   += kstat.cpustat[CPUTIME_IDLE];
        total_system += kstat.cpustat[CPUTIME_SYSTEM];
    }

    // Wyliczanie różnic
    u64 delta_user   = total_user   - last_user;
    u64 delta_idle   = total_idle   - last_idle;
    u64 delta_system = total_system - last_system;
    u64 delta_total  = delta_user + delta_idle + delta_system;

    last_user   = total_user;
    last_idle   = total_idle;
    last_system = total_system;

    // Procentowe użycie CPU
    unsigned int user_percent = 0, system_percent = 0, idle_percent = 0;
    if (delta_total > 0) {
        user_percent   = (100 * delta_user) / delta_total;
        system_percent = (100 * delta_system) / delta_total;
        idle_percent   = 100 - user_percent - system_percent;
    }

    // RAM w MiB (zamiast KB)
    si_meminfo(&info);
    total_mib = (info.totalram << (PAGE_SHIFT - 10)) / 1024;
    free_mib  = (info.freeram  << (PAGE_SHIFT - 10)) / 1024;

    // Wypisanie do logu
    printk(KERN_INFO LOG_TAG
        ": CPU: user=%u%% system=%u%% idle=%u%% [%d/%d] | RAM: total=%lu free=%lu [MiB]\n",
        user_percent, system_percent, idle_percent,
        online_cpus, total_cpus, total_mib, free_mib);

    // Restart timera
    mod_timer(&zasoby_timer, jiffies + HZ * INTERVAL_SECONDS);
}

static int __init zasoby_init(void)
{
    printk(KERN_INFO LOG_TAG ": moduł załadowany\n");

    timer_setup(&zasoby_timer, zasoby_callback, 0);
    mod_timer(&zasoby_timer, jiffies + HZ * INTERVAL_SECONDS);

    return 0;
}

static void __exit zasoby_exit(void)
{
    del_timer(&zasoby_timer);
    printk(KERN_INFO LOG_TAG ": moduł usunięty\n");
}

module_init(zasoby_init);
module_exit(zasoby_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ChatGPT");
MODULE_DESCRIPTION("Moduł wypisujący procentowe użycie CPU i RAM (w MiB) z agregacją ze wszystkich CPU");
