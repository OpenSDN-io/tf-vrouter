/*
 * vr_default_wc.c - functions to manage hash table workqueue
 * Copyright (c) 2025 Matvey Kraposhin
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/version.h>

#include "vrouter.h"

int vr_allocate_default_wq(void);
void vr_destroy_default_wq(void);
int
lh_schedule_work(unsigned int cpu, void (*fn)(void *), void *arg);
void
lh_soft_reset(struct vrouter *router);

struct work_arg {
    struct work_struct wa_work;
    void (*fn)(void *);
    void *wa_arg;
};

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,6,0)) || (defined(RHEL_MAJOR) && (RHEL_MAJOR >= 9) && (RHEL_MINOR >= 5)) //commit 20bdeda
struct workqueue_struct *vr_default_wq;
#endif

int vr_allocate_default_wq(void) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,6,0)) || (defined(RHEL_MAJOR) && (RHEL_MAJOR >= 9) && (RHEL_MINOR >= 5)) //commit 20bdeda
    vr_default_wq = create_workqueue("vr_default_wq");
    if (!vr_default_wq) {
        printk("%s:%d Failed to create the default work queue\n",
                __FUNCTION__, __LINE__);
        return -ENOMEM;
    }
#endif
return 0;
}

void vr_destroy_default_wq(void) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,6,0)) || (defined(RHEL_MAJOR) && (RHEL_MAJOR >= 9) && (RHEL_MINOR >= 5)) //commit 20bdeda
    if (vr_default_wq != NULL) {
        flush_workqueue(vr_default_wq);
        destroy_workqueue(vr_default_wq);
        vr_default_wq = NULL;
    }
#endif
}

void
lh_soft_reset(struct vrouter *router)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,6,0)) || (defined(RHEL_MAJOR) && (RHEL_MAJOR >= 9) && (RHEL_MINOR >= 5)) //commit 20bdeda
    flush_workqueue(vr_default_wq);
    //__flush_workqueue(system_wq);
#else
    flush_scheduled_work();
#endif
    rcu_barrier();

    return;
}

static void
lh_work(struct work_struct *work)
{
    struct work_arg *wa = container_of(work, struct work_arg, wa_work);

    rcu_read_lock();
    wa->fn(wa->wa_arg);
    rcu_read_unlock();
    kfree(wa);

    return;
}

int
lh_schedule_work(unsigned int cpu, void (*fn)(void *), void *arg)
{
    unsigned int alloc_flag;
    struct work_arg *wa;

    if (in_softirq()) {
        alloc_flag = GFP_ATOMIC;
    } else {
        alloc_flag = GFP_KERNEL;
    }

    wa = kzalloc(sizeof(*wa), alloc_flag);
    if (!wa)
        return -ENOMEM;

    wa->fn = fn;
    wa->wa_arg = arg;
    INIT_WORK(&wa->wa_work, lh_work);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,6,0)) || (defined(RHEL_MAJOR) && (RHEL_MAJOR >= 9) && (RHEL_MINOR >= 5)) //commit 20bdeda
    queue_work_on(cpu, vr_default_wq, &wa->wa_work);
#else
    schedule_work_on(cpu, &wa->wa_work);
#endif

    return 0;
}

//
//END-OF-FILE
//
