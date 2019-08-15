#include <linux/poll.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/semaphore.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>

MODULE_LICENSE("GPL");

// 网络设备对象
static struct net_device* sg_dev = 0;
// 与用户态转发程序对接的/proc/eth_uart目录
static struct proc_dir_entry* sg_proc = 0;
// 待发送的帧
static struct sk_buff* sg_frame = 0;
// 信号量，指示有待发送的帧（初始化为0）
static struct semaphore sg_sem_has_frame;
// 用于通知poll机制的等待队列
static wait_queue_head_t sg_poll_queue;

// 有数据帧要发送时，kernel会调用该函数
static int eth_uart_send_packet(struct sk_buff* skb,struct net_device *dev)
{
    // 告诉kernel不要传入更多的帧
    netif_stop_queue(sg_dev);
    // 统计已发送的数据包
    sg_dev->stats.tx_packets++;
    // 统计已发送的字节
    sg_dev->stats.tx_bytes += skb->len;
    // 复制帧
    sg_frame = skb;
    // 通知有待发送的帧
    up(&sg_sem_has_frame);
    // 唤醒阻塞的poll调用
    wake_up(&sg_poll_queue);
    return 0;
}

// 用户态转发程序通过读/proc/eth_uart/uio取走待发送的帧
static ssize_t eth_uart_uio_read(struct file* file, char* buf, size_t count, loff_t* offset)
{
    // 如果要求非阻塞操作
    if(file->f_flags & O_NONBLOCK)
    {
        // 查看是否有待发送的帧，如果没有则立即返回，否则锁定
        if(down_trylock(&sg_sem_has_frame) != 0)
            return -EAGAIN;
    }
    // 如果要求阻塞操作
    else
    {
        // 等待，直到有待发送的帧，若被中断则立即返回，否则锁定
        if(down_interruptible(&sg_sem_has_frame) != 0)
        {
            printk("<eth_uart.ko> down() interrupted...\n");
            return -EINTR;
        }
    }
    // 之所以复制一份sg_len，是为了避免netif_wake_queue()之后sg_frame可能被修改
    int len = sg_frame->len;
    // 空间不够
    if(count < len)
    {
        up(&sg_sem_has_frame);
        printk("<eth_uart.ko> no enough buffer to read the frame...\n");
        return -EFBIG;
    }
    // 把帧复制到用户态缓冲区
    copy_to_user(buf, sg_frame->data, len);
    // 释放数据帧
    dev_kfree_skb(sg_frame);
    sg_frame = 0;
    // 告诉内核可以传入更多帧了
    netif_wake_queue(sg_dev);
    return len;
}

// 用户可以对/proc/eth_uart/uio执行poll操作
static uint eth_uart_uio_poll(struct file* file, poll_table* queue)
{
    // 添加等待队列
    poll_wait(file, &sg_poll_queue, queue);
    // 不管如何，都是可写的
    uint mask = POLLOUT | POLLWRNORM;
    // 如果有帧，则设置状态码为可读
    if(sg_frame != 0)
        mask |= POLLIN | POLLRDNORM;
    return mask;
}

// 用户态转发程序从物理上收到一个帧，通过写/proc/eth_uart/uio告知驱动程序
static ssize_t eth_uart_uio_write(struct file* file, const char* buf, size_t count, loff_t* offset)
{
    // 分配count + 2字节的空间
    struct sk_buff* skb = dev_alloc_skb(count + 2);
    if(skb == 0)
    {
        printk("<eth_uart.ko> dev_alloc_skb() failed!\n");
        return -ENOMEM;
    }
    // 开头的2字节预留，这样14字节的以太头就能对其到16字节
    skb_reserve(skb, 2);
    // 把接下来的count字节复制进来
    copy_from_user(skb_put(skb, count), buf, count);
    skb->dev = sg_dev;
    // 得到协议号
    skb->protocol = eth_type_trans(skb, sg_dev);
    // 底层没有校验，交给内核计算
    skb->ip_summed = CHECKSUM_NONE;
    // 统计已接收的数据包
    sg_dev->stats.rx_packets++;
    // 统计已发收的字节
    sg_dev->stats.rx_bytes += skb->len;
    // 通知kernel收到一个数据包
    netif_rx(skb);
    return count;
}

// 驱动程序支持的操作
static struct net_device_ops sg_ops =
{
    // 发送数据帧
    .ndo_start_xmit = eth_uart_send_packet,
};

// /proc/eth_uart/uio支持的操作
static struct file_operations sg_uio_ops =
{
    .owner = THIS_MODULE,
    // 读，即用户态转发程序取走待发送的帧
    .read = eth_uart_uio_read,
    // poll， 即用户态程序等待有帧可取（但不一定真的能取走）
    .poll = eth_uart_uio_poll,
    // 写，即用户态转发程序从物理线路上收到一个帧
    .write = eth_uart_uio_write,
};

// 驱动程序初始化
static int eth_uart_init(void)
{
    int ret = 0;
    // 创建一个网络设备，名为“eth_uart"
    //sg_dev = alloc_netdev(0, "eth_uart", ether_setup);
    // kernel 4+上需要四个参数如下
     sg_dev=alloc_netdev(0,"eth_uart", NET_NAME_UNKNOWN, ether_setup);
    // 该网络设备的操作集
    if(sg_dev == 0)
    {
        printk("<eth_uart.ko> alloc_netdev() failed!\n");
        ret = -EEXIST;
        goto err_1;
    }
    // 该网络设备的操作集
    sg_dev->netdev_ops = &sg_ops;
    // MAC地址前3字节固定为EC-A8-6B
    memcpy(sg_dev->dev_addr, "\xEC\xA8\x6B", 3);
    // MAC地址后3字节随机产生
    get_random_bytes((char*)sg_dev->dev_addr + 3, 3);
    // 注册网络设备
    ret = register_netdev(sg_dev);
    if(ret != 0)
    {
        printk("<eth_uart.ko> register_netdev() failed!\n");
        goto err_2;
    }
    // 创建/proc/eth_uart目录
    sg_proc = proc_mkdir("eth_uart", 0);
    if(sg_proc == 0)
    {
        printk("<eth_uart.ko> proc_mkdir() failed!\n");
        ret = -EEXIST;
        goto err_3;
    }
    // 创建/proc/eth_uart/uio文件
    struct proc_dir_entry* t_proc_uio = proc_create("uio", 0666, sg_proc, &sg_uio_ops);
    if(t_proc_uio == 0)
    {
        printk("<eth_uart.ko> proc_create() failed!\n");
        ret = -EEXIST;
        goto err_4;
    }
    // 初始化信号量
    sema_init(&sg_sem_has_frame, 0);
    // 初始化poll队列
    init_waitqueue_head(&sg_poll_queue);
    return 0;
    err_4:
        // 删除/proc/eth_uart目录
        remove_proc_entry("eth_uart", 0);
    err_3:
        // 注销网络设备
        unregister_netdev(sg_dev);
    err_2:
        // 释放网络设备对象
        free_netdev(sg_dev);
    err_1:
        ;
    return ret;
}

// 驱动程序销毁
static void eth_uart_exit(void)
{
    // 删除/proc/eth_uart/uio文件
    remove_proc_entry("uio", sg_proc);
    // 删除/proc/eth_uart目录
    remove_proc_entry("eth_uart", 0);
    // 注销网络设备
    unregister_netdev(sg_dev);
    // 释放对象
    free_netdev(sg_dev);
    // 释放数据帧
    if(sg_frame != 0)
        dev_kfree_skb(sg_frame);
}

module_init(eth_uart_init);
module_exit(eth_uart_exit);
