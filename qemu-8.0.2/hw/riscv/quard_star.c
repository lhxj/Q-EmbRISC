#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/char/serial.h"
#include "target/riscv/cpu.h"

#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/quard_star.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/numa.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/riscv_aplic.h"
#include "hw/intc/riscv_imsic.h"
#include "hw/platform-bus.h"
#include "hw/intc/sifive_plic.h"
#include "hw/misc/sifive_test.h"


#include "chardev/char.h"
#include "sysemu/device_tree.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "sysemu/tpm.h"

#include "hw/pci/pci.h"
#include "hw/pci-host/gpex.h"
#include "hw/display/ramfb.h"
#include "hw/acpi/aml-build.h"
#include "qapi/qapi-visit-common.h"

static const MemMapEntry quard_star_memmap[] = {
    [QUARD_STAR_MROM]  = {        0x0,        0x8000 },   
    [QUARD_STAR_SRAM]  = {     0x8000,        0x8000 },
    [QUARD_STAR_CLINT] = { 0x02000000,       0x10000 },
    [QUARD_STAR_PLIC]  = { 0x0c000000,     QUARD_STAR_PLIC_SIZE(QUARD_STAR_CPUS_MAX * 2) },
    [QUARD_STAR_UART0] = { 0x10000000,         0x100 },
    [QUARD_STAR_UART1] = { 0x10001000,         0x100 }, 
    [QUARD_STAR_UART2] = { 0x10002000,         0x100 }, 
    [QUARD_STAR_RTC] =   { 0x10003000,        0x1000 }, 
    [QUARD_STAR_FLASH] = { 0x20000000,     0x2000000 }, 
    [QUARD_STAR_DRAM]  = { 0x80000000,          0x80 },   
};
/*创建CPU */
static void quard_star_cpu_create(MachineState *machine)
{
    int i, base_hartid, hart_count;
    char *soc_name;
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);

    if (QUARD_STAR_SOCKETS_MAX < riscv_socket_count(machine)) {
        error_report("number of sockets/nodes should be less than %d",
            QUARD_STAR_SOCKETS_MAX);
        exit(1);
    }

    for (i = 0; i < riscv_socket_count(machine); i++) {
        if (!riscv_socket_check_hartids(machine, i)) {
            error_report("discontinuous hartids in socket%d", i);
            exit(1);
        }

        base_hartid = riscv_socket_first_hartid(machine, i);
        if (base_hartid < 0) {
            error_report("can't find hartid base for socket%d", i);
            exit(1);
        }

        hart_count = riscv_socket_hart_count(machine, i);
        if (hart_count < 0) {
            error_report("can't find hart count for socket%d", i);
            exit(1);
        }

        soc_name = g_strdup_printf("soc%d", i);
        object_initialize_child(OBJECT(machine), soc_name, &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);
        g_free(soc_name);
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_abort);
    }
}

/*  创建内存 */
static void quard_star_memory_create(MachineState *machine)
{
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    //分配三片存储空间 dram sram mrom
    MemoryRegion *dram_mem = g_new(MemoryRegion, 1);  //DRAM
    MemoryRegion *sram_mem = g_new(MemoryRegion, 1);  //SRAM
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);  //MROM  


    memory_region_init_ram(dram_mem, NULL, "riscv_quard_star_board.dram",
                           quard_star_memmap[QUARD_STAR_DRAM].size, &error_fatal);
    memory_region_add_subregion(system_memory, 
                                quard_star_memmap[QUARD_STAR_DRAM].base, dram_mem);

    memory_region_init_ram(sram_mem, NULL, "riscv_quard_star_board.sram",
                           quard_star_memmap[QUARD_STAR_SRAM].size, &error_fatal);
    memory_region_add_subregion(system_memory, 
                                quard_star_memmap[QUARD_STAR_SRAM].base, sram_mem);

    memory_region_init_rom(mask_rom, NULL, "riscv_quard_star_board.mrom",
                           quard_star_memmap[QUARD_STAR_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, 
                                quard_star_memmap[QUARD_STAR_MROM].base, mask_rom);
    /* 从0号cpu开始加载复位程序，随后跳转到 flash位置开始执行*/
    riscv_setup_rom_reset_vec(machine, &s->soc[0], 
                              quard_star_memmap[QUARD_STAR_FLASH].base,
                              quard_star_memmap[QUARD_STAR_MROM].base,
                              quard_star_memmap[QUARD_STAR_MROM].size,
                              0x0, 0x0);
}

/*创建flash*/
static void quard_star_flash_create(MachineState *machine)
{
    #define QUARD_STAR_FLASH_SECTOR_SIZE (256 * KiB) //定义了Flash存储的扇区大小
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory(); //获取虚拟机的系统内存区域，用于将Flash设备映射到虚拟机的地址空间中
    DeviceState *dev = qdev_new(TYPE_PFLASH_CFI01); //创建一个新的Flash设备实例，设备类型为 PFLASH_CFI01

    qdev_prop_set_uint64(dev, "sector-length", QUARD_STAR_FLASH_SECTOR_SIZE); //设置扇区的大小为 256 KiB
    qdev_prop_set_uint8(dev, "width", 4); //设置设备宽度为 4 字节
    qdev_prop_set_uint8(dev, "device-width", 2); //设置每个Flash存储单元的宽度为 2 字节
    qdev_prop_set_bit(dev, "big-endian", false); //设置设备为小端序模式
    qdev_prop_set_uint16(dev, "id0", 0X89); //设置设备的制造商ID（id0）为 0x89，即Intel的ID
    qdev_prop_set_uint16(dev, "id1", 0X18); //设置设备的型号ID（id1）为 0x18
    qdev_prop_set_uint16(dev, "id2", 0X00); //置设备的 id2 和 id3 值为 0x00，这些值用于标识设备的特性
    qdev_prop_set_uint16(dev, "id3", 0X00);
    qdev_prop_set_string(dev, "name", "quard-star.flash0"); //设置设备的名字为 quard-star.flash0
    //将创建的Flash设备 dev 添加为 QuardStarState 对象的子设备，并给它起名为 quard-star.flash0
    object_property_add_child(OBJECT(s), "quard-star.flash0", OBJECT(dev)); 
    //创建设备的别名，将 pflash0 指向Flash设备
    object_property_add_alias(OBJECT(s), "pflash0", OBJECT(dev), "drive");

    s->flash = PFLASH_CFI01(dev);//将设备 dev 转换为 PFLASH_CFI01 类型，并存储到 s->flash 中。
    pflash_cfi01_legacy_drive(s->flash, drive_get(IF_PFLASH, 0, 0));//将Flash设备与虚拟机的一个闪存驱动器关联

    hwaddr flashsize = quard_star_memmap[QUARD_STAR_FLASH].size;//获取Flash存储设备的大小，flashsize 从 quard_star_memmap（内存映射数组）中取出
    hwaddr flashbase = quard_star_memmap[QUARD_STAR_FLASH].base;//获取Flash存储设备的基地址，flashbase 同样从 quard_star_memmap 中取得

    assert(QEMU_IS_ALIGNED(flashsize, QUARD_STAR_FLASH_SECTOR_SIZE));//检查Flash大小是否与扇区大小对齐。如果不对齐，程序会中断
    assert(flashsize / QUARD_STAR_FLASH_SECTOR_SIZE <= UINT32_MAX);//确认Flash存储的扇区数量不超过 UINT32_MAX（避免溢出）
    qdev_prop_set_uint32(dev, "num-blocks", flashsize / QUARD_STAR_FLASH_SECTOR_SIZE);//设置Flash设备的块数量，即将Flash的总大小除以扇区大小
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);//将设备注册到系统总线，并释放不再需要的引用
    //将Flash设备的内存区域添加到虚拟机的系统内存区域，并将其映射到虚拟机的地址空间（通过获取设备的MMIO区域并指定其基地址 flashbase）
    memory_region_add_subregion(system_memory, flashbase, 
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
}

//创建plic
static void quard_star_plic_create(MachineState *machine)
{
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);
    int socket_count = riscv_socket_count(machine);
    int base_hartid, hart_count;

    for(int i = 0; i < socket_count; i++)
    {
        hart_count = riscv_socket_hart_count(machine, i);//返回第 i 个处理器插槽中的硬件线程（HART）数量
        base_hartid = riscv_socket_first_hartid(machine, i);//用来获取第 i 个插槽中第一个硬件线程的 HART ID（硬件线程的唯一标识符）
        char *plic_hart_config;
        plic_hart_config = riscv_plic_hart_config_string(hart_count);

        s->plic[i] = sifive_plic_create(
            quard_star_memmap[QUARD_STAR_PLIC].base + i * quard_star_memmap[QUARD_STAR_PLIC].size,
            plic_hart_config, hart_count, base_hartid,
            QUARD_STAR_PLIC_NUM_SOURCES,
            QUARD_STAR_PLIC_NUM_PRIORITIES,
            QUARD_STAR_PLIC_PRIORITY_BASE,
            QUARD_STAR_PLIC_PENDING_BASE,
            QUARD_STAR_PLIC_ENABLE_BASE,
            QUARD_STAR_PLIC_ENABLE_STRIDE,
            QUARD_STAR_PLIC_CONTEX_BASE,
            QUARD_STAR_PLIC_CONTEX_STRIDE,
            quard_star_memmap[QUARD_STAR_PLIC].size
        );
        g_free(plic_hart_config);

    }
}

//创建ACLINT
static void quard_star_aclint_create(MachineState *machine)
{
    int base_hartid, hart_count;
    int socket_count = riscv_socket_count(machine);

    for(int i = 0; i < socket_count; i++)
    {
        hart_count = riscv_socket_hart_count(machine, i);
        base_hartid = riscv_socket_first_hartid(machine, i);
        
        riscv_aclint_swi_create(
            quard_star_memmap[QUARD_STAR_CLINT].base + i * quard_star_memmap[QUARD_STAR_CLINT].size,
            base_hartid, hart_count, false);
        riscv_aclint_mtimer_create(quard_star_memmap[QUARD_STAR_CLINT].base +
                i * quard_star_memmap[QUARD_STAR_CLINT].size + RISCV_ACLINT_SWI_SIZE,
            RISCV_ACLINT_DEFAULT_MTIMER_SIZE, base_hartid, hart_count,
            RISCV_ACLINT_DEFAULT_MTIMECMP, RISCV_ACLINT_DEFAULT_MTIME,
            RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ, true);
    }
}

//创建RTC
static void quard_star_rtc_create(MachineState *machine)
{
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);
    sysbus_create_simple("goldfish_rtc", quard_star_memmap[QUARD_STAR_RTC].base,
        qdev_get_gpio_in(s->plic[0], QUARD_STAR_RTC_IRQ));//使用 qdev_get_gpio_in 获取中断引脚，并将 RTC 设备的中断连接到 PLIC（平台级中断控制器），使得 RTC 能够触发中断

}

//创建3路UART
static void quard_star_uart_create(MachineState *machine)
{
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();

    serial_mm_init(system_memory, quard_star_memmap[QUARD_STAR_UART0].base,
        0, qdev_get_gpio_in(DEVICE(s->plic[0]), QUARD_STAR_UART0_IRQ), 399193,
        serial_hd(0), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(system_memory, quard_star_memmap[QUARD_STAR_UART1].base,
        0, qdev_get_gpio_in(DEVICE(s->plic[0]), QUARD_STAR_UART1_IRQ), 399193,
        serial_hd(1), DEVICE_LITTLE_ENDIAN);    
    serial_mm_init(system_memory, quard_star_memmap[QUARD_STAR_UART2].base,
        0, qdev_get_gpio_in(DEVICE(s->plic[0]), QUARD_STAR_UART2_IRQ), 399193,
        serial_hd(2), DEVICE_LITTLE_ENDIAN);
    
}

/* quard-star 初始化各种硬件 */

static void quard_star_machine_init(MachineState *machine)
{
    //创建CPU
    quard_star_cpu_create(machine);
   // 创建主存
    quard_star_memory_create(machine);
    //创建flash
    quard_star_flash_create(machine);
    //创建PLIC
    quard_star_plic_create(machine);
    //创建ACLINT
    quard_star_aclint_create(machine);
    //创建三路UART
    quard_star_uart_create(machine);
    //创建RTC
    quard_star_rtc_create(machine);
    
}

static void quard_star_machine_instance_init(Object *obj)
{

}

/* 创建machine */
static void quard_star_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V Quard Star board";
    mc->init = quard_star_machine_init;
    mc->max_cpus = QUARD_STAR_CPUS_MAX;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->pci_allow_0_address = true;
    mc->possible_cpu_arch_ids = riscv_numa_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = riscv_numa_cpu_index_to_props;
    mc->get_default_cpu_node_id = riscv_numa_get_default_cpu_node_id;
    mc->numa_mem_supported = true;
}
/* 注册 quard-star */
static const TypeInfo quard_star_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("quard-star"),
    .parent     = TYPE_MACHINE,
    .class_init = quard_star_machine_class_init,
    .instance_init = quard_star_machine_instance_init,
    .instance_size = sizeof(QuardStarState),
    .interfaces = (InterfaceInfo[]) {
         { TYPE_HOTPLUG_HANDLER },
         { }
    },
};

static void quard_star_machine_init_register_types(void)
{
    type_register_static(&quard_star_machine_typeinfo);
}
type_init(quard_star_machine_init_register_types)