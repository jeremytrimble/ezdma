/*
 * ezdma module -- Simple zero-copy DMA to/from userspace for
 * dmaengine-compatible hardware.
 * 
 * Copyright (C) 2015 Jeremy Trimble
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <asm/param.h>  /* HZ */
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/mm.h>

#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/wait.h>

#define EZDMA_DEV_NAME_MAX_CHARS (16)

#define SEM_TAKE_TIMEOUT (5)

enum ezdma_dir {
    EZDMA_DEV_TO_CPU = 1,   // RX
    EZDMA_CPU_TO_DEV = 2,   // TX
};

/* Right now the I/O concept is very simple -- all reads and writes
 * are blocking, and concurrent reads and writes are not allowed.
 * Concurrent open is also disallowed.
 */
enum dma_fsm_state {
    DMA_IDLE = 0,
    DMA_IN_FLIGHT = 1,
    DMA_COMPLETING = 3,
};

// These fields should only be valid during an ongoing read/write call.
struct ezdma_inflight_info {
    struct page **  pinned_pages;
    struct sg_table table;
    unsigned int    num_pages;
    bool            table_allocated;
    bool            pages_pinned;
    bool            dma_mapped;
    bool            dma_started;
};

struct ezdma_drvdata {
    struct platform_device *pdev;

    char name[EZDMA_DEV_NAME_MAX_CHARS];
    uint32_t dir;   // ezdma_dir

    struct semaphore sem;   /* protects mutable data below */

    bool        in_use;
    atomic_t    accepting;

    spinlock_t state_lock;  // protects state below, may be taken from interrupt (tasklet) context
    enum dma_fsm_state state;
    struct ezdma_inflight_info inflight;

    wait_queue_head_t    wq;

    /* dmaengine */
    struct dma_chan *chan;

    /* device accounting */
    dev_t           ezdma_devt;
    struct cdev     ezdma_cdev;
    struct device * ezdma_dev;

    /* Statistics */
    atomic_t    packets_sent;
    atomic_t    packets_rcvd;

    struct list_head node;
};

/* LOCK ORDERING:  if taking both sem and state_lock, must always take sem first */

struct ezdma_pdev_drvdata {
    struct list_head ezdma_list;    // list of ezdma_drvdata instances created in
                                    // relation to this platform device
};


#define NUM_DEVICE_NUMBERS_TO_ALLOCATE (8)
static dev_t base_devno;
static int devno_in_use[NUM_DEVICE_NUMBERS_TO_ALLOCATE];
static struct class *ezdma_class;
static DEFINE_SEMAPHORE(devno_lock);

static inline int get_free_devno(dev_t * p_dev)
{
    int i;
    int rv = -ENODEV;

    down( &devno_lock );

    for (i = 0; i < NUM_DEVICE_NUMBERS_TO_ALLOCATE; i++ )
    {
        if ( !devno_in_use[i] )
        {
            *p_dev = MKDEV( MAJOR(base_devno), i );
            devno_in_use[i] = 1;
            rv = 0;
            break;
        }
    }

    up( &devno_lock );

    return rv;
}

static inline int put_devno(dev_t dev)
{
    down( &devno_lock );

    BUG_ON( 0 == devno_in_use[ MINOR(dev) ] );
    devno_in_use[ MINOR(dev) ] = 0;

    up( &devno_lock );
    return 0;
}



static int ezdma_open(struct inode *inode, struct file *filp);
static ssize_t ezdma_read(struct file *filp, char __user *userbuf, size_t count, loff_t *f_pos);
static ssize_t ezdma_write(struct file *filp, const char __user *userbuf, size_t count, loff_t *f_pos);
static int ezdma_release(struct inode *inode, struct file *filp);

static const struct file_operations ezdma_fops = {
    .owner      = THIS_MODULE,
    .open       = ezdma_open,
    .read       = ezdma_read,
    .write      = ezdma_write,
    .release    = ezdma_release,
    //.poll       = ezdma_poll,
};




static int ezdma_open(struct inode *inode, struct file *filp)
{
    struct ezdma_drvdata * p_info = container_of(inode->i_cdev, struct ezdma_drvdata, ezdma_cdev); 
    int rv = 0;

    if ( down_interruptible( &p_info->sem ) )
        return -ERESTARTSYS;

    if ( p_info->in_use )
    {
        rv = -EBUSY;
    }
    else
    {
        p_info->in_use = 1;
        filp->private_data = p_info;
        atomic_set( &p_info->accepting, 1 );
    }
    
    up( &p_info->sem );

    return rv;
}

// this runs in tasklet (interrupt) context -- no sleeping!
static void ezdma_dmaengine_callback_func(void *data)
{
    struct ezdma_drvdata * p_info = (struct ezdma_drvdata*)data;
    unsigned long iflags;

    //printk( KERN_ERR KBUILD_MODNAME ": %s: callback fired for %s\n",
    //        p_info->name, p_info->dir == EZDMA_DEV_TO_CPU ? "RX" : "TX" );

    spin_lock_irqsave(&p_info->state_lock, iflags);

    if ( DMA_IN_FLIGHT == p_info->state )
    {
        p_info->state = DMA_COMPLETING;
        wake_up_interruptible( &p_info->wq );
    }
    // else: well, nevermind then...
    
    spin_unlock_irqrestore(&p_info->state_lock, iflags);
}


/* 
 * DMA procedure:
 * 
 * Figure out how many pages.
 * Allocate page array and scatterlist.
 * Try to pin pages using get_user_pages_fast().
 * Map the pages for DMA.
 * Issue DMA request.
 * Wait on condition variable.
 * if interrupted, terminate_all DMAs
 * Unmap DMA
 * SetPageDirty()   // if RX, assume all pages were written
 * free page array and scatterlist
 */
static void ezdma_unprepare_after_dma( struct ezdma_drvdata * p_info );

// should be called with p_info->sem held, but not p_info->state_lock
static int ezdma_prepare_for_dma(
        struct ezdma_drvdata * p_info, 
        char __user *userbuf,
        size_t count
)
{
    int rv;

    BUG_ON( p_info->inflight.pinned_pages ); // should be NULL
    memset( &p_info->inflight, 0, sizeof( struct ezdma_inflight_info ) );
    
    p_info->inflight.num_pages = (offset_in_page(userbuf) + count + PAGE_SIZE-1) / PAGE_SIZE;
    p_info->inflight.pinned_pages = kmalloc( 
        p_info->inflight.num_pages * sizeof(struct page*),
        GFP_KERNEL);

    if ( !p_info->inflight.pinned_pages )
    {
        rv = -ENOMEM;
        goto err_out;
    }

    if ( (rv = sg_alloc_table(
                    &p_info->inflight.table, 
                    p_info->inflight.num_pages,
                    GFP_KERNEL )) )
    {
        printk( KERN_ERR KBUILD_MODNAME ": %s: sg_alloc_table() returned %d\n", 
                p_info->name, rv);
        goto err_out;
    }
    else
    {
        p_info->inflight.table_allocated = 1;
    }

    rv = get_user_pages_fast(
            (unsigned long)userbuf,             // start
            p_info->inflight.num_pages,
            p_info->dir == EZDMA_DEV_TO_CPU,    // write
            p_info->inflight.pinned_pages);

    if ( rv != p_info->inflight.num_pages )
    {
        printk( KERN_ERR KBUILD_MODNAME ": %s: get_user_pages_fast() returned %d, expected %d\n",
                p_info->name, rv, p_info->inflight.num_pages);
        goto err_out;
    }
    else
    {
        p_info->inflight.pages_pinned = 1;
    }

    // Build scatterlist.
    {
        int i;
        struct scatterlist * sg;
        struct scatterlist * const sgl = p_info->inflight.table.sgl;
        const unsigned int num_pages = p_info->inflight.num_pages;

        size_t left_to_map = count;

        for_each_sg( sgl, sg, num_pages, i )
        {
            unsigned int len;
            unsigned int offset;

            len = left_to_map > PAGE_SIZE ? PAGE_SIZE : left_to_map;

            if ( 0 == i )
            {
                offset = offset_in_page(userbuf);
                if ( (offset + len) > PAGE_SIZE )
                    len = PAGE_SIZE - offset;
            }
            else
            {
                offset = 0;
            }

            //printk( KERN_DEBUG KBUILD_MODNAME ": %s: sgl[%d]: page: %p, len: %d, offset: %d\n",
            //        p_info->name, i, p_info->inflight.pinned_pages[i], len, offset );

            //sg_set_page( sgl, p_info->inflight.pinned_pages[i], len, offset );
            sg_set_page( sg, p_info->inflight.pinned_pages[i], len, offset );
            left_to_map -= len;
        }
    }

    // Map the scatterlist 

    rv = dma_map_sg(p_info->ezdma_dev,
                p_info->inflight.table.sgl,
                p_info->inflight.num_pages,
                p_info->dir == EZDMA_DEV_TO_CPU ? DMA_FROM_DEVICE : DMA_TO_DEVICE);

    if ( rv != p_info->inflight.num_pages )
    {
        printk( KERN_ERR KBUILD_MODNAME ": %s: dma_map_sg() returned %d, expected %d\n", 
                p_info->name, rv, p_info->inflight.num_pages);
        goto err_out;
    }
    else
    {
        p_info->inflight.dma_mapped = 1;
    }

    // Issue DMA request here
    {
        struct dma_async_tx_descriptor * txn_desc;
        struct scatterlist * const sgl = p_info->inflight.table.sgl;
        dma_cookie_t cookie;

        txn_desc = dmaengine_prep_slave_sg(
                p_info->chan,
                sgl,
                p_info->inflight.num_pages,
                p_info->dir == EZDMA_DEV_TO_CPU ? DMA_FROM_DEVICE : DMA_TO_DEVICE,
                DMA_PREP_INTERRUPT);    // run callback after this one

        if ( !txn_desc )
        {
            printk( KERN_ERR KBUILD_MODNAME ": %s: dmaengine_prep_slave_sg() failed\n", p_info->name);
            rv = -ENOMEM;
            goto err_out;
        }

        txn_desc->callback = ezdma_dmaengine_callback_func;
        txn_desc->callback_param = p_info;

        spin_lock_irq( &p_info->state_lock );

        p_info->state = DMA_IN_FLIGHT;

        cookie = dmaengine_submit(txn_desc);

        if ( cookie < DMA_MIN_COOKIE )
        {
            printk( KERN_ERR KBUILD_MODNAME ": %s: dmaengine_submit() returned %d\n", p_info->name, cookie);
            rv = cookie;
            p_info->state = DMA_IDLE;
        }
        else
        {
            p_info->inflight.dma_started = 1;
            dma_async_issue_pending( p_info->chan );    // Bam!
            //printk( KERN_ERR KBUILD_MODNAME ": %s: issued pending for %s\n",
            //        p_info->name,
            //        p_info->dir == EZDMA_DEV_TO_CPU ? "RX" : "TX" );
        }

        spin_unlock_irq( &p_info->state_lock );

        if ( cookie < DMA_MIN_COOKIE )
            goto err_out;
    }

    return 0;

    err_out:

    spin_lock_irq( &p_info->state_lock );
    ezdma_unprepare_after_dma( p_info );
    spin_unlock_irq( &p_info->state_lock );

    return rv;
}

// should be called with p_info->sem held, and with p_info_state_lock
static void ezdma_unprepare_after_dma( struct ezdma_drvdata * p_info )
{
    p_info->state = DMA_IDLE;

    if ( p_info->inflight.dma_mapped )
    {
        dma_unmap_sg(p_info->ezdma_dev,
                p_info->inflight.table.sgl,
                p_info->inflight.num_pages,
                p_info->dir == EZDMA_DEV_TO_CPU ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
    }
    p_info->inflight.dma_mapped = 0;

    if ( p_info->inflight.pages_pinned )
    {
        if ( p_info->inflight.dma_started && p_info->dir == EZDMA_DEV_TO_CPU )
        {
            /* Mark all pages dirty for now (not sure how to do this more
             * efficiently yet -- dmaengine API doesn't seem to return any
             * notion of how much data was actually transferred).
             */

            int i;

            for (i = 0; i < p_info->inflight.num_pages; ++i)
            {
                struct page * const page = p_info->inflight.pinned_pages[i];

                set_page_dirty( page );
                put_page( page );
            }
        }
    }
    p_info->inflight.pages_pinned = 0;

    if ( p_info->inflight.table_allocated )
        sg_free_table( &p_info->inflight.table );
    p_info->inflight.table_allocated = 0;

    if ( p_info->inflight.pinned_pages )
    {
        kfree(p_info->inflight.pinned_pages);
        p_info->inflight.pinned_pages = NULL;
    }

}

static int check_not_in_flight( struct ezdma_drvdata * p_info )
{
    int rv;
    spin_lock_irq(&p_info->state_lock);
    
    rv = (p_info->state != DMA_IN_FLIGHT);
    
    spin_unlock_irq(&p_info->state_lock);

    return rv;
}


// Assume that reads/writes have to be multiples of this.
#define EZDMA_ALIGN_BYTES (1)

static ssize_t ezdma_read(struct file *filp, char __user *userbuf, size_t count, loff_t *f_pos)
{
    struct ezdma_drvdata * p_info = (struct ezdma_drvdata*)filp->private_data;
    ssize_t rv = count;

    // Ensure this is a readable device.
    if ( EZDMA_DEV_TO_CPU != p_info->dir )
    {
        printk( KERN_WARNING KBUILD_MODNAME ": %s: can't read, is a TX device\n", p_info->name);
        return -EINVAL;
    }

    if ( 0 != (count % EZDMA_ALIGN_BYTES) )
    {
        printk( KERN_WARNING KBUILD_MODNAME ": %s: unaligned read of %u bytes requested\n", p_info->name, count);
        return -EINVAL;
    }

    //TODO: verify size of count?

    if ( down_interruptible( &p_info->sem ) )
        return -ERESTARTSYS;

    if ( !atomic_read(&p_info->accepting ) )
    {
        rv = -EBADF;
        goto out;
    }
    else
    {
        int prep_rv;
        int wait_rv;

        prep_rv = ezdma_prepare_for_dma( p_info, userbuf, count );

        if (prep_rv)
        {
            rv = prep_rv;
            goto out;
        }

        up( &p_info->sem );

        wait_rv = wait_event_interruptible( p_info->wq, check_not_in_flight(p_info) );

        if ( down_timeout( &p_info->sem, SEM_TAKE_TIMEOUT ) )
        {
            printk( KERN_ALERT KBUILD_MODNAME 
                    ": %s: read sem take stalled for %d seconds -- probably broken\n",
                    p_info->name, 
                    SEM_TAKE_TIMEOUT);
            goto noup_out;
        }

        spin_lock_irq(&p_info->state_lock);
        if ( p_info->state == DMA_IN_FLIGHT && -ERESTARTSYS == wait_rv )
        {
            dmaengine_terminate_all( p_info->chan );
            rv = wait_rv;
        }

        ezdma_unprepare_after_dma( p_info );    // sets us back to DMA_IDLE
        spin_unlock_irq(&p_info->state_lock);
    }

    out:
    up( &p_info->sem );

    noup_out:
    return rv;
}

static ssize_t ezdma_write(struct file *filp, const char __user *userbuf, size_t count, loff_t *f_pos)
{
    struct ezdma_drvdata * p_info = (struct ezdma_drvdata*)filp->private_data;
    ssize_t rv = count;

    // Ensure this is a writable device.
    if ( EZDMA_CPU_TO_DEV != p_info->dir )
    {
        printk( KERN_WARNING KBUILD_MODNAME ": %s: can't write, is an RX device\n", p_info->name);
        return -EINVAL;
    }
    if ( 0 != (count % EZDMA_ALIGN_BYTES) )
    {
        printk( KERN_WARNING KBUILD_MODNAME ": %s: unaligned write of %u bytes requested\n", p_info->name, count);
        return -EINVAL;
    }

    // Ensure this is a writable device.

    if ( down_interruptible( &p_info->sem ) )
        return -ERESTARTSYS;

    if ( !atomic_read(&p_info->accepting ) )
    {
        rv = -EBADF;
        goto out;
    }
    else
    {
        int prep_rv;
        int wait_rv;

        prep_rv = ezdma_prepare_for_dma( p_info, (char __user*)userbuf, count );

        if (prep_rv)
        {
            rv = prep_rv;
            goto out;
        }

        up( &p_info->sem );

        wait_rv = wait_event_interruptible( p_info->wq, check_not_in_flight(p_info) );

        if ( down_timeout( &p_info->sem, SEM_TAKE_TIMEOUT ) )
        {
            printk( KERN_ALERT KBUILD_MODNAME 
                    ": %s: write sem take stalled for %d seconds -- probably broken\n",
                    p_info->name,
                    SEM_TAKE_TIMEOUT);
            goto noup_out;
        }

        spin_lock_irq(&p_info->state_lock);
        if ( p_info->state == DMA_IN_FLIGHT && -ERESTARTSYS == wait_rv )
        {
            dmaengine_terminate_all( p_info->chan );
            rv = wait_rv;
        }

        ezdma_unprepare_after_dma( p_info );    // sets us back to DMA_IDLE
        spin_unlock_irq(&p_info->state_lock);
    }

    out:
    up( &p_info->sem );

    noup_out:
    return rv;
}

static int ezdma_release(struct inode *inode, struct file *filp)
{
    struct ezdma_drvdata * p_info = container_of(inode->i_cdev, struct ezdma_drvdata, ezdma_cdev); 

    atomic_set( &p_info->accepting, 0 );    // disallow new reads/writes

    if ( down_interruptible( &p_info->sem ) )
        return -ERESTARTSYS;

    dmaengine_terminate_all(p_info->chan);
    // TODO: wake up any sleeping threads?

    p_info->in_use = 0;

    up( &p_info->sem );

    return 0;
}





static int ezdma_create_device( struct ezdma_drvdata * p_info )
{
    int rv;

    if ( (rv = get_free_devno( &p_info->ezdma_devt )) )
    {
        printk( KERN_ERR KBUILD_MODNAME ": get_free_devno() returned %d\n", rv);
        return rv;
    }

    cdev_init( &p_info->ezdma_cdev, &ezdma_fops );
    p_info->ezdma_cdev.owner = THIS_MODULE;

    if ( (rv = cdev_add( &p_info->ezdma_cdev, p_info->ezdma_devt, 1 )) )
    {
        printk(KERN_ERR KBUILD_MODNAME ": cdev_add() returned %d\n", rv);
        put_devno(p_info->ezdma_devt);
        p_info->ezdma_devt = MKDEV(0,0);
        return rv;
    }

    if ( NULL == (p_info->ezdma_dev = device_create( ezdma_class,
                              &p_info->pdev->dev, 
                              p_info->ezdma_devt,
                              p_info,
                              p_info->name)))
    {
        printk(KERN_ERR KBUILD_MODNAME ": device_create() failed\n");
        cdev_del( &p_info->ezdma_cdev );
        put_devno( p_info->ezdma_devt );
        p_info->ezdma_devt = MKDEV(0,0);
        return -ENOMEM;
    }

    return 0;
}

static void ezdma_teardown_device( struct ezdma_drvdata * p_info )
{
    device_destroy( ezdma_class, p_info->ezdma_devt );
    cdev_del( &p_info->ezdma_cdev );
    put_devno( p_info->ezdma_devt );
    p_info->ezdma_devt = MKDEV(0,0);
}

static void teardown_devices( struct ezdma_pdev_drvdata * p_pdev_info, struct platform_device *pdev);

static int create_devices( struct ezdma_pdev_drvdata * p_pdev_info, struct platform_device *pdev)
{
    /*
     * read number of "dma-names" in my device tree entry
     * for each
     *   allocate ezdma_drvdata
     *   create devices
     *   acquire slave channel
     *   add to list
     */

    int num_dma_names = of_property_count_strings(pdev->dev.of_node, "dma-names");
    int dma_name_idx;

    int outer_rv = 0;

    if ( 0 == num_dma_names )
    {
        printk( KERN_ERR KBUILD_MODNAME ": no DMAs specified in ezdma \"dma-names\" property\n");
        return -ENODEV;
    }
    else if ( num_dma_names < 0 )
    {
        printk( KERN_ERR KBUILD_MODNAME ": got %d when trying to count the elements of \"dma-names\" property\n", num_dma_names);
        return num_dma_names;   // contains error code
    }

    for (dma_name_idx = 0; dma_name_idx < num_dma_names; dma_name_idx++)
    {
        struct ezdma_drvdata * p_info;
        const char * p_dma_name;
        int rv;

        p_info = devm_kzalloc( &pdev->dev, sizeof(*p_info), GFP_KERNEL );

        if ( !p_info )
        {
            printk( KERN_ERR KBUILD_MODNAME ": failed to allocate ezdma_drvdata\n");
            outer_rv = -ENOMEM;
            break;
        }

        /* Initialize fields */
        p_info->pdev = pdev;
        p_info->in_use = 0;
        p_info->state = DMA_IDLE;
        spin_lock_init( &p_info->state_lock );
        list_add_tail( &p_info->node, &p_pdev_info->ezdma_list );
        sema_init( &p_info->sem, 1 );
        init_waitqueue_head( &p_info->wq );
        atomic_set( &p_info->packets_sent, 0 );
        atomic_set( &p_info->packets_rcvd, 0 );

        /* Read the dma name for the current index */
        rv = of_property_read_string_index(
                pdev->dev.of_node, "dma-names",
                dma_name_idx, &p_dma_name);

        if ( rv )
        {
            printk( KERN_ERR KBUILD_MODNAME
                    ": of_property_read_string_index() returned %d\n", rv);

            outer_rv = rv;
            break;
        }
        else
        {
            strncpy( p_info->name, p_dma_name, EZDMA_DEV_NAME_MAX_CHARS-1 );
            p_info->name[EZDMA_DEV_NAME_MAX_CHARS-1] = '\0';

            //printk( KERN_DEBUG KBUILD_MODNAME ": setting up %s\n", p_info->name);
        }


        /* Read the direction for the current index */
        rv = of_property_read_u32_index(
                pdev->dev.of_node, "ezdma,dirs",
                dma_name_idx, &p_info->dir);

        if ( rv )
        {
            printk( KERN_ERR KBUILD_MODNAME
                    ": couldn't read \"ezdma,dirs\" property for %s\n",
                    p_info->name );

            outer_rv = rv;
            break;
        }
        else if ( p_info->dir != EZDMA_CPU_TO_DEV && 
                  p_info->dir != EZDMA_DEV_TO_CPU )
        {
            printk( KERN_ERR KBUILD_MODNAME
                    ": %s specifies unsupported value of \"ezdma,dirs\": %d\n", 
                    p_info->name,
                    p_info->dir);

            outer_rv = -EINVAL;
            break;
        }

        if ( (rv = ezdma_create_device( p_info )) )
        {
            outer_rv = rv;
            break;
        }

        /* Get the named DMA channel */
        p_info->chan = dma_request_slave_channel(
                    &pdev->dev, p_dma_name);

        if ( !p_info->chan )
        {
            printk( KERN_WARNING KBUILD_MODNAME 
                    ": couldn't find dma channel: %s, deferring...\n",
                    p_info->name);

            outer_rv = -EPROBE_DEFER;
        }

        printk( KERN_ALERT KBUILD_MODNAME ": %s (%s) available\n", 
                p_info->name,
                p_info->dir == EZDMA_DEV_TO_CPU ? "RX" : "TX"
                );
    }


    if ( outer_rv )
    {
        // Unroll what we've done here
        teardown_devices( p_pdev_info, pdev );
    }

    return outer_rv;
}



static void teardown_devices( struct ezdma_pdev_drvdata * p_pdev_info, struct platform_device *pdev)
{
    struct ezdma_drvdata * p_info;

    list_for_each_entry( p_info, &p_pdev_info->ezdma_list, node )
    {
        // p_info might be partially-initialized, so check pointers and be careful
        printk( KERN_DEBUG KBUILD_MODNAME ": tearing down %s\n",
                p_info->name );    // name can only be all null-bytes or a valid string

        if ( p_info->chan )
        {
            dmaengine_terminate_all(p_info->chan);
            dma_release_channel(p_info->chan);
        }

        if ( p_info->ezdma_dev )
            ezdma_teardown_device(p_info);
    }

    /* Note: we don't bother with the deallocations here, since they'll be
     * cleaned up by devm_* unrolling. */
}

static int ezdma_probe(struct platform_device *pdev)
{
    struct ezdma_pdev_drvdata * p_pdev_info;
    int rv;

    printk(KERN_INFO "probing ezdma\n");

    p_pdev_info = devm_kzalloc( &pdev->dev, sizeof(*p_pdev_info), GFP_KERNEL );

    if (!p_pdev_info)
        return -ENOMEM;

    INIT_LIST_HEAD( &p_pdev_info->ezdma_list );

    if ( (rv = create_devices( p_pdev_info, pdev )) )
        return rv;  // devm_* unrolls automatically

    platform_set_drvdata( pdev, p_pdev_info );

    return 0;
}

static int ezdma_remove(struct platform_device *pdev)
{
    struct ezdma_pdev_drvdata * p_pdev_info = 
        (struct ezdma_pdev_drvdata *)platform_get_drvdata(pdev);

    teardown_devices( p_pdev_info, pdev );

    return 0;
}







/* Match table for of_platform binding */
static const struct of_device_id ezdma_of_match[] = {
    { .compatible = "ezdma" /*, .data = &... */ },
    { /* end of list */ },
};
MODULE_DEVICE_TABLE(of, ezdma_of_match);

static struct platform_driver ezdma_driver = {
    .driver = {
        .name = KBUILD_MODNAME,
        .owner = THIS_MODULE,
        .of_match_table = ezdma_of_match,
    },
    .probe      = ezdma_probe,
    .remove     = ezdma_remove,
};






static int __init ezdma_driver_init(void)
{
    int rv;
    ezdma_class = class_create(THIS_MODULE, "ezdma");

    if ( (rv = alloc_chrdev_region( &base_devno, 0, NUM_DEVICE_NUMBERS_TO_ALLOCATE, "ezdma" )) )
    {
        printk(KERN_ERR KBUILD_MODNAME ": alloc_chrdev_region() returned %d!\n", rv);
        return rv;
    }
    else
    {
        printk(KERN_INFO KBUILD_MODNAME ": allocated chrdev region: Major: %d, Minor: %d-%d\n",
                   MAJOR(base_devno),
                   MINOR(base_devno),
                   MINOR(base_devno) + NUM_DEVICE_NUMBERS_TO_ALLOCATE);
    }


    if ( (rv = platform_driver_register(&ezdma_driver)) )
    {
        unregister_chrdev_region( base_devno, NUM_DEVICE_NUMBERS_TO_ALLOCATE );
        class_destroy(ezdma_class);
        return rv;
    }
    return 0;
}

static void __exit ezdma_driver_exit(void)
{
    platform_driver_unregister(&ezdma_driver);
    class_destroy(ezdma_class);
    unregister_chrdev_region( base_devno, NUM_DEVICE_NUMBERS_TO_ALLOCATE );
}

module_init(ezdma_driver_init);
module_exit(ezdma_driver_exit);

MODULE_AUTHOR("Jeremy Trimble <jeremy.trimble@gmail.com>");
MODULE_DESCRIPTION("EZ DMA Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
