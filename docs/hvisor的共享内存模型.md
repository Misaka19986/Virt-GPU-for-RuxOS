### hvisor与zone0
hvisor与zone0唯一共用的内存是virtio-bridge，由处于内核态的hvisor.ko内核模块来进行管理，通过hvc、mmap、ioctl来初始化；在hvisor.ko初始化时需要在内核申请一块空闲内存，之后通过hvc来让hvisor知道有这块内存，然后还需要在hvisor-tool(virtio守护进程)启动时通过配置文件让hvisor-tool知道有这块内存，从而可以获取到来自其他zone的request

具体是这样实现的:
hvisor.ko初始化时，在linux注册一个miscdevice，则hvisor会作为一个可访问设备出现为/dev/hvisor，同时定义了这个设备的ioctl、mmap处理函数——hvisor_ioctl和hvisor_map，对该设备的ioctl和mmap会调用这两个函数进行处理
之后，hvisor会尝试在zone0设备树上找到hvisor_virtio_device节点，该节点定义了virtio_device的中断号，hvisor.ko将注册这个中断的处理函数，从而当接收到zonex的virtio请求时，可以通过hvisor产生中断来唤醒virtio守护进程(当中断时，会调用virtio_irq_handler)

在hvisor.ko注册完毕后，将等待virtio守护进程通过ioctl来进行virtio设备的初始化(hvisor_init_virtio)，在该函数中会申请一页物理内存，并将其设为reserved，并通过hvc通知hvisor这块共享内存作为virtio_bridge

而virtio的启动配置virtio_cfg_\*.json规定了所有zone的内存区域，在memory_region这个字段所代表的数组中，其每一个元素存在zone0_ipa、zonex_ipa、size三个字段。其中zone0_ipa、size表明将zone0 linux的可用物理内存中从zone0_ipa开始，长size的内存段映射到virtio守护进程的某个虚拟地址上(一般而言，就是zone0的部分可用内存，这部分在zone0的设备树中的reserved-memory节点定义，而memory节点会定义zone0的全部可用内存；以目前的参考设备来说，reserved-memory为0x50000000\~0x80000000，zone0全部可用内存为0x50000000\~0xd0000000)。这段内存和zonex中从zonex_ipa开始，长size的物理内存区域存在映射关系

### zone0与zonex
由于所有zone都运行在hvisor之上，而hvisor会对内存做二次映射来保证隔离性和安全性，因此所有zone的pa实际上都是ipa(特别的，针对virtio设备的mmio区域不做二次映射，从而可以通过缺页错误来产生中断)

在启动zone时，zone的配置文件zone_\*.json会指明zone使用的内存区域，其被hvisor-tool读取后将会由hvisor.ko通过hvc传递到hvisor，通过hv_zone_start进行处理。这些内存区域在memory_regions字段，具有type、physical_start、virtual_start、size四个字段

其中type字段指明是ram/io/virtio，其中virtio对应的是mmio区域，该区域不会做二次映射；physical_start指明内存区域在host的映射起始地址，而virtual_start指明在guest对应的映射起始地址，size代表长度

对于virtio设备而言，已经在virtio_cfg_\*.json中设置了其mmio区域的起始地址和长度，而在处理zone_\*.json时，hvisor会将guest(zonex)的相应内存区域映射到host的相应区域(zone0)(注: 这里的映射是hvisor将zonex对内存的访问通过页表映射到zone0的内存区域，并不是在zone0或zonex级别的映射，即zone0或zonex都不知道他们的内存区域被映射了)。现有的zone_*.json会将zonex的全部内存、对io设备的访问、对virtio设备的mmio区域的访问直接映射到zone0的相同地址的区域(即基本上可以说zone0和zonex共用同一块内存，只不过zonex所使用的全部内存是zone0的一小部分，而二者共用约定好的mmio设备区域)

在zone0的设备树中，使用reserverd-memory来保证这部分内存不被zone0使用
