# Linux-driver-ttyNetwork
一个Linux网络设备驱动程序

知识：学习x86体系结构及系统初始化过程，将操作系统基本原理与Linux系统内核的进程管理、存储器管理、设备管理及文件系统的具体实现相结合；了解Linux高级程序设计方法、设计流程、开发环境等；了解Linux字符设备和块设备驱动程序设计方法；了解Linux高级程序设计尤其与系统安全相关API，学习Linux安全相关特性。

能力：通过对Linux内核的深入剖析，加深对操作系统原理的理解，并培养对Linux操作系统的系统级分析能力；初步掌握Linux内核模块编程、Linux简单字符设备和块设备编程能力；培养Linux系统安全相关系统设计及程序开发能力；培养能够进行Linux系统级大型应用设计开发能力。



### 一、体系结构

![png](http://cdn.peckerwood.top/image002.png)

1、网络协议接口层

向网络层协议提供提供统一的数据包收发接口，不论上层协议为ARP还是IP，都通过dev_queue_xmit()函数发送数据，并通过netif_rx()函数接受数据。这一层的存在使得上层协议独立于具体的设备。

2、网络设备接口层

向协议接口层提供统一的用于描述具体网络设备属性和操作的结构体net_device，该结构体是设备驱动功能层中各函数的容器。实际上，网络设备接口层从宏观上规划了具体操作硬件的设备驱动功能层的结构。

3、设备驱动功能层

各函数是网络设备接口层net_device数据结构的具体成员，是驱使网络设备硬件完成相应动作的程序，他通过hard_start_xmit()函数启动发送操作，并通过网络设备上的中断触发接受操作。

4、网络设备与媒介层

是完成数据包发送和接受的物理实体，包括网络适配器和具体的传输媒介，网络适配器被驱动功能层中的函数物理上驱动。对于Linux系统而言，网络设备和媒介都可以是虚拟的。

 ### 二、虚拟网络设备驱动程序实现

![png](http://cdn.peckerwood.top/image003.png)

#### 驱动程序初始化

1、使用alloc_netdev()来分配一个net_device结构体

2、使用register_netdev()来注册net_device结构体

![png](http://cdn.peckerwood.top/image004.jpg)

#### 驱动程序销毁

1. 注销网络设备

2. 释放设备

![png](http://cdn.peckerwood.top/image005.jpg)

#### 设备激活时调用函数

注册中断服务

![png](http://cdn.peckerwood.top/image006.jpg)

#### 发包函数

![png](http://cdn.peckerwood.top/image007.jpg)



#### 收包函数

![png](http://cdn.peckerwood.top/image008.jpg)



#### 运行结果

![png](http://cdn.peckerwood.top/image009.jpg)

![png](http://cdn.peckerwood.top/image010.jpg)

### 四、Linux串口网卡

1. 一部分是运行在内核态的一个虚拟网卡驱动（第一部分的基础上实现）

2. 另一部分是运行在用户态的转发进程

 就是把以太帧通过串口发送出去、接收回来，内核需要网卡发送某个帧时，内核态程序就把这个帧交给用户态程序，用户态程序调用串口发送出去。而当用户态程序通过串口收到一个帧时，则把这个帧交给内核态程序，由内核态程序交给内核。

![png](http://cdn.peckerwood.top/image011.jpg)

如何搭建用户态进程与内核的桥梁？方法很多，我们用的是/proc文件系统，大致流程是这样的：用户态进程读/proc/eth_uart/uio这个虚拟文件，得到一个需要发送出去的帧；用户态进程写/proc/eth_uart/uio，告知驱动程序收到一个帧。每次的交互单位就是一个帧。用户态可以使用阻塞式read()，直到有帧后返回，也可以使用非阻塞式IO轮询，也可以使用poll机制等待可读。

 

对于/proc/eth_uart/uio而言，read()和write()的交互单位都是一整个帧，但是对于串口，读写的单位都是字节。串口的收发速度是有限的，select/poll机制下，需同时监控可读和可写事件。当可读时，就从串口中读取尽量多的字节。当可写时，就向串口写出尽量多的字节。因此，需要监控最多三个事件：

1. /proc/eth_uart/uio可读事件

2. 串口可读事件

3. 串口可写事件

#### 帧的开始和结束标识与数据转义

一个问题是，当帧变成字节流传输在串口线上时，如何标记帧的开始与结束？

我们的结局办法是定一个开头和结尾标记，并且真正的数据需要使用转义。

![png](http://cdn.peckerwood.top/image012.jpg)

1、两个连续字节如果是255, 0，那么是START命令，即一个帧的开头

2、两个连续字节如果是255, 1，那么是END命令，即一个帧的结尾

3、两个连续字节如果是255, x(x > 1)，那么就是单字节x

在该方案中，把255当作一个转义符号，表示其后面的一个字节有特殊含义。既然255作为转义符号，那么如果数据中本身就有255，那么怎么办？所以就变成两个字节255, 255。

#### 信息量的语义

信号量sg_sem_has_frame的计数与帧的有无是严格对应的，其计数初始化为0,表示没有帧。当内核传入一个帧时，计数加一，即up()；当需要取走一帧时，计数减一，即down_*()。这样充分利用了信号量的语义，保证了并发情形下不会出错。

当内核把一个帧，即数据结构struct sk_buff传给驱动程序时，内核并不知道驱动程序会如何处理该帧，因此驱动程序有责任在不再需要该帧时释放之，即：

dev_kfree_skb(struct sk_buff*);

当用户态把一帧通过/proc/eth_uart/uio传给驱动程序时，驱动程序只需要直接把它交给内核处理即可，而所有与并发有关的处理也由内核管理了，因此write()没有使用任何全局变量，是可重入的，在逻辑上简单地多。驱动通过：

netif_rx(struct sk_buff*);

告知内核收到了一帧。那么如何组建一个struct sk_buff呢？首先是分配内存空间，要使用专用的函数：

struct sk_buff* dev_alloc_skb(size_t);

代码中很奇怪，传入的size是帧长度count+2。为什么要多两字节呢？《Linux内核源码剖析：TCP/IP实现》第3.4.4节“数据预留和对齐”给出了答案

![png](http://cdn.peckerwood.top/image013.jpg)

最重要的原因还是，让14字节的以太网头部变成16字节，使得后面的负载对齐到16字节。

void get_random_bytes(void *buf, int nbytes);

 

最后一个比较有趣的是，如何在内核中产生随机数。内核中可以使用函数：

void get_random_bytes(void *buf, int nbytes);

以产生指定长度的随机字节序列

后三字节则随机产生，这样基本避免了MAC地址冲突。

// MAC地址前3字节固定为EC-A8-6B

​    memcpy(sg_dev->dev_addr, "\xEC\xA8\x6B", 3);

​    // MAC地址后3字节随机产生

get_random_bytes((char*)sg_dev->dev_addr + 3, 3);

 #### 实验过程

实现环境

Ubuntu 64位和此虚拟机的克隆机

Configure Virtual Serial Port Driver 6.8 （虚拟串口软件）

Serial Port Utility（串口调试工具）

##### 1. 虚拟串口连通测试

使用虚拟串口软件增加一对串口，com1和com2，使用串口调试工具管理这两个串口，相互收发数据，说明可以连通，如图所示：

![png](http://cdn.peckerwood.top/image014.jpg)

##### 2. 虚拟机增加串行端口

![png](http://cdn.peckerwood.top/image015.jpg)

使用虚拟串口软件虚拟出的串口，在Vmware可识别为物理串口，将配对的两个串口分别与两台虚拟机连通。

##### 3. 装载虚拟网络设备驱动程序

详细细节见第一部分

![png](http://cdn.peckerwood.top/image016.jpg)

##### 4. 关闭网络接口管理工具

![png](http://cdn.peckerwood.top/image017.jpg)

![png](http://cdn.peckerwood.top/image018.jpg)

在很多Ubuntu机器上，可能存在一些自动管理网络接口的工具，记得关闭，或者让工具不要管理eth_uart这个网口。因为自动管理网络接口的工具会在自动配置网络，有时候将eth_uart的ip配置好后，又会被修改，所以这里之间将network-manager服务关掉。

##### 5. 编译运行用户态的转发进程

接着编译、运行上面的代码（两台机器上都需要执行）

![png](http://cdn.peckerwood.top/image019.jpg)

串口网卡最后的实验结果并没有达到预期，两个虚拟机并不能相互ping通，我们在网络上大量搜索，只找到一篇使用物理串口，即两个USB-TTL将两台Linux机器连起来的博客。然而，我们在进行到这一步的时候已经来不及购买USB-TTL串口转接器和连线了。

##### 6. 对问题的思考

我们在对实验出现的问题进行了大量的思考，下图是两个串口，一端接虚拟机，另一端接串口调试器时，串口调试器是能够收到发送的ping包的。

![png](http://cdn.peckerwood.top/image020.jpg)

并且如下图也可以看到，两端同时连虚拟机时，两端数据的收发是对等的。

![png](http://cdn.peckerwood.top/image021.jpg)



ps: 问题出在了虚拟机串口设置那里，由于后面想到这里已经过去（wo）太远（lan）了，后面有兴趣可以自己改一下设置。



------

#### 参考资料

https://www.cnblogs.com/lifexy/p/7763352.html?tdsourcetag=s_pcqq_aiomsg

[https://zhoujianshi.github.io/articles/2017/Linux%E4%B8%B2%E5%8F%A3%E7%BD%91%E5%8D%A1%EF%BC%88%E4%B8%80%EF%BC%89%E2%80%94%E2%80%94%E9%80%9A%E7%94%A8%E8%99%9A%E6%8B%9F%E7%BD%91%E5%8D%A1%E7%9A%84%E5%AE%9E%E7%8E%B0/index.html](https://zhoujianshi.github.io/articles/2017/Linux串口网卡（一）——通用虚拟网卡的实现/index.html)

 
