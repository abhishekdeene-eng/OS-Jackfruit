#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>

#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");

/* ================= DATA STRUCT ================= */
struct monitor_entry {
    pid_t pid;
    unsigned long soft_limit;
    unsigned long hard_limit;
    struct monitor_entry *next;
};

static struct monitor_entry *head = NULL;
static DEFINE_MUTEX(lock);
static struct timer_list monitor_timer;
static int major;

/* ================= TIMER FUNCTION ================= */
static void monitor_fn(struct timer_list *t)
{
    struct monitor_entry *curr;

    mutex_lock(&lock);
    curr = head;

    while (curr) {
        struct task_struct *task;

        rcu_read_lock();
        for_each_process(task) {
            if (task->pid == curr->pid) {

                if (task->mm) {
                    unsigned long rss = get_mm_rss(task->mm) << PAGE_SHIFT;

                    if (rss > curr->hard_limit) {
                        printk(KERN_ALERT "[monitor] HARD LIMIT exceeded pid=%d\n", curr->pid);
                        send_sig(SIGKILL, task, 0);
                    }
                    else if (rss > curr->soft_limit) {
                        printk(KERN_WARNING "[monitor] SOFT LIMIT exceeded pid=%d\n", curr->pid);
                    }
                }
                break;
            }
        }
        rcu_read_unlock();

        curr = curr->next;
    }

    mutex_unlock(&lock);

    mod_timer(&monitor_timer, jiffies + HZ);
}

/* ================= IOCTL ================= */
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        struct monitor_entry *entry;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        entry->soft_limit = req.soft_limit_bytes;
        entry->hard_limit = req.hard_limit_bytes;

        mutex_lock(&lock);
        entry->next = head;
        head = entry;
        mutex_unlock(&lock);

        printk(KERN_INFO "[monitor] Registered pid=%d\n", req.pid);
    }

    return 0;
}

/* ================= FILE OPS ================= */
static struct file_operations fops = {
    .unlocked_ioctl = device_ioctl,
};

/* ================= INIT ================= */
static int __init monitor_init(void)
{
    major = register_chrdev(0, "container_monitor", &fops);

    if (major < 0) {
        printk(KERN_ALERT "Failed to register device\n");
        return major;
    }

    timer_setup(&monitor_timer, monitor_fn, 0);
    mod_timer(&monitor_timer, jiffies + HZ);

    printk(KERN_INFO "monitor module loaded\n");
    return 0;
}

/* ================= EXIT ================= */
static void __exit monitor_exit(void)
{
    struct monitor_entry *curr, *tmp;

    /* 🔥 FIX FOR YOUR ERROR (works on your kernel) */
    timer_delete_sync(&monitor_timer);

    mutex_lock(&lock);

    curr = head;
    while (curr) {
        tmp = curr;
        curr = curr->next;
        kfree(tmp);
    }

    mutex_unlock(&lock);

    unregister_chrdev(major, "container_monitor");

    printk(KERN_INFO "monitor module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
