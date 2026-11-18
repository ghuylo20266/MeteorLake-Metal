#include "IntelArcMeteorLake.hpp"

#define super IOService

OSDefineMetaClassAndStructors(com_huy_driver_IntelArcMeteorLake, IOService)

bool com_huy_driver_IntelArcMeteorLake::init(OSDictionary *dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }

    fPCIDevice        = nullptr;
    fBAR0Map          = nullptr;
    fMMIOBase         = nullptr;
    fBAR0PhysicalAddr = 0;
    fBAR0Length       = 0;

    IOLog("[IntelArcMTL] init(): Khởi tạo cấu trúc đối tượng Kext thành công.\n");
    return true;
}

void com_huy_driver_IntelArcMeteorLake::free(void)
{
    IOLog("[IntelArcMTL] free(): Giải phóng vùng nhớ đối tượng Kext.\n");

    if (fBAR0Map) {
        fBAR0Map->release();
        fBAR0Map = nullptr;
    }

    if (fPCIDevice) {
        fPCIDevice->release();
        fPCIDevice = nullptr;
    }

    fMMIOBase = nullptr;
    super::free();
}

IOService *com_huy_driver_IntelArcMeteorLake::probe(IOService *provider, SInt32 *score)
{
    IOService *result = super::probe(provider, score);
    if (!result) {
        return nullptr;
    }

    IOPCIDevice *pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        IOLog("[IntelArcMTL] probe(): Provider không phải là một IOPCIDevice hợp lệ.\n");
        return nullptr;
    }

    uint16_t vendorID   = pciDevice->configRead16(kIOPCIConfigVendorID);
    uint16_t deviceID   = pciDevice->configRead16(kIOPCIConfigDeviceID);
    uint8_t  revisionID = pciDevice->configRead8(kIOPCIConfigRevisionID);
    uint16_t subVendor  = pciDevice->configRead16(kIOPCIConfigSubSystemVendorID);
    uint16_t subDevice  = pciDevice->configRead16(kIOPCIConfigSubSystemID);

    IOLog("[IntelArcMTL] probe(): Phát hiện thiết bị PCI [Vendor: 0x%04X, Device: 0x%04X, Rev: 0x%02X, SubSys: 0x%04X:0x%04X]\n",
          vendorID, deviceID, revisionID, subVendor, subDevice);

    if (vendorID != INTEL_VENDOR_ID || deviceID != METEOR_LAKE_ARC_DEVICE_ID) {
        IOLog("[IntelArcMTL] probe(): Thiết bị không khớp với Intel Arc Meteor Lake (0x8086:0x7D55).\n");
        return nullptr;
    }

    if (score) {
        *score += 10000;
    }

    IOLog("[IntelArcMTL] probe(): Xác thực thành công iGPU Intel Arc Meteor Lake!\n");
    return this;
}

bool com_huy_driver_IntelArcMeteorLake::start(IOService *provider)
{
    if (!super::start(provider)) {
        IOLog("[IntelArcMTL] start(): Lỗi khi gọi IOService::start().\n");
        return false;
    }

    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) {
        IOLog("[IntelArcMTL] start(): Không thể ép kiểu provider sang IOPCIDevice.\n");
        return false;
    }

    fPCIDevice->retain();

    if (!fPCIDevice->open(this)) {
        IOLog("[IntelArcMTL] start(): Không thể mở phiên làm việc độc quyền (open) trên IOPCIDevice.\n");
        OSSafeReleaseNULL(fPCIDevice);
        return false;
    }

    // Kích hoạt Memory Space và Bus Master trong thanh ghi PCI Command (0x04)
    fPCIDevice->setMemoryEnable(true);
    fPCIDevice->setBusMasterEnable(true);

    uint16_t pciCommand = fPCIDevice->configRead16(kIOPCIConfigCommand);
    IOLog("[IntelArcMTL] start(): Thanh ghi PCI Command sau khi kích hoạt = 0x%04X\n", pciCommand);

    // Ánh xạ thanh ghi BAR0 (kIOPCIConfigBaseAddress0 = 0x10) vào không gian địa chỉ ảo của Kernel
    fBAR0Map = fPCIDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (!fBAR0Map) {
        IOLog("[IntelArcMTL] start(): Lỗi nghiêm trọng - Không thể ánh xạ thanh ghi BAR0!\n");
        fPCIDevice->close(this);
        OSSafeReleaseNULL(fPCIDevice);
        return false;
    }

    fMMIOBase         = reinterpret_cast<volatile uint8_t *>(fBAR0Map->getVirtualAddress());
    fBAR0PhysicalAddr = fBAR0Map->getPhysicalAddress();
    fBAR0Length       = fBAR0Map->getLength();

    if (!fMMIOBase || fBAR0Length == 0) {
        IOLog("[IntelArcMTL] start(): Địa chỉ ảo BAR0 không hợp lệ.\n");
        OSSafeReleaseNULL(fBAR0Map);
        fPCIDevice->close(this);
        OSSafeReleaseNULL(fPCIDevice);
        return false;
    }

    IOLog("[IntelArcMTL] start(): Ánh xạ BAR0 thành công -> Phys: 0x%016llX | Virt: %p | Size: %llu MB (%llu bytes)\n",
          static_cast<uint64_t>(fBAR0PhysicalAddr),
          fMMIOBase,
          static_cast<uint64_t>(fBAR0Length / (1024 * 1024)),
          static_cast<uint64_t>(fBAR0Length));

    // Đánh thức miền Render (Forcewake), đọc thanh ghi MMIO và đưa trở lại trạng thái ngủ
    bool isAwake = wakeRenderDomain(true);
    if (!isAwake) {
        IOLog("[IntelArcMTL] start(): Cảnh báo - Forcewake Render timeout, một số thanh ghi GT có thể trả về 0.\n");
    }

    dumpHardwareRegisters();

    if (isAwake) {
        wakeRenderDomain(false);
    }

    // Đăng ký dịch vụ để hiển thị trong cây IORegistry (kiểm tra bằng lệnh ioreg)
    registerService();
    IOLog("[IntelArcMTL] start(): Driver đã khởi chạy hoàn tất và đăng ký vào IORegistry.\n");

    return true;
}

void com_huy_driver_IntelArcMeteorLake::stop(IOService *provider)
{
    IOLog("[IntelArcMTL] stop(): Đang dừng driver và thu hồi ánh xạ BAR0...\n");

    fMMIOBase         = nullptr;
    fBAR0PhysicalAddr = 0;
    fBAR0Length       = 0;

    if (fBAR0Map) {
        fBAR0Map->release();
        fBAR0Map = nullptr;
        IOLog("[IntelArcMTL] stop(): Đã giải phóng ánh xạ bộ nhớ BAR0.\n");
    }

    if (fPCIDevice) {
        if (fPCIDevice->isOpen(this)) {
            fPCIDevice->close(this);
        }
        fPCIDevice->release();
        fPCIDevice = nullptr;
    }

    super::stop(provider);
    IOLog("[IntelArcMTL] stop(): Đã gỡ driver hoàn tất.\n");
}

bool com_huy_driver_IntelArcMeteorLake::wakeRenderDomain(bool wake)
{
    // Thanh ghi Forcewake của Intel sử dụng 16 bit cao làm Write Mask.
    // Bit 0 là Kernel/Driver Forcewake domain:
    // - Để bật: ghi (1 << 16) | 1 = 0x00010001
    // - Để tắt: ghi (1 << 16) | 0 = 0x00010000
    uint32_t fwValue = wake ? 0x00010001 : 0x00010000;
    writeMMIO32(XE_LPG_FORCEWAKE_RENDER, fwValue);

    // Chờ tối đa 50ms (50000us) để phần cứng xác nhận bit 0 trong FORCEWAKE_ACK_RENDER
    for (int retry = 0; retry < 5000; retry++) {
        uint32_t ack = readMMIO32(XE_LPG_FORCEWAKE_ACK_RENDER);
        bool bitSet = (ack & 0x1) != 0;
        if (bitSet == wake) {
            return true;
        }
        IODelay(10);
    }
    return false;
}

void com_huy_driver_IntelArcMeteorLake::dumpHardwareRegisters(void)
{
    // 1. Đọc các thanh ghi cấu hình PCI cơ bản
    uint32_t bar0Low  = fPCIDevice->configRead32(kIOPCIConfigBaseAddress0);
    uint32_t bar0High = fPCIDevice->configRead32(kIOPCIConfigBaseAddress1);
    uint16_t ggcReg   = fPCIDevice->configRead16(0x50); // Mirror of Graphics Control (GGC)

    // 2. Đọc các thanh ghi MMIO từ BAR0
    uint32_t gmdGraphics = readMMIO32(XE_LPG_GMD_ID_GRAPHICS);
    uint32_t gmdDisplay  = readMMIO32(XE_LPG_GMD_ID_DISPLAY);
    uint32_t gmdMedia    = readMMIO32(XE_LPG_GMD_ID_MEDIA);
    uint32_t masterIrq   = readMMIO32(XE_LPG_GEN8_MASTER_IRQ);
    uint32_t fuse3       = readMMIO32(XE_LPG_MIRROR_FUSE3);
    uint32_t fwAck       = readMMIO32(XE_LPG_FORCEWAKE_ACK_RENDER);

    // Giải mã cấu trúc thanh ghi GMD_ID của kiến trúc Meteor Lake:
    // Bits [31:22]: Architecture Version (Ví dụ: 12)
    // Bits [21:14]: Release Version (Ví dụ: 70 cho Xe-LPG 12.70)
    // Bits [5:0]  : Stepping
    uint32_t gfxArch = (gmdGraphics >> 22) & 0x3FF;
    uint32_t gfxRel  = (gmdGraphics >> 14) & 0xFF;
    uint32_t gfxStep = gmdGraphics & 0x3F;

    IOLog("[IntelArcMTL] ========= BẢNG THÔNG SỐ THANH GHI INTEL ARC (0x7D55) =========\n");
    IOLog("[IntelArcMTL] PCI BAR0 Raw (64-bit) : High=0x%08X | Low=0x%08X\n", bar0High, bar0Low);
    IOLog("[IntelArcMTL] PCI GGC (Offset 0x50) : 0x%04X\n", ggcReg);
    IOLog("[IntelArcMTL] MMIO FORCEWAKE_ACK    : 0x%08X (Offset 0x%06X)\n", fwAck, XE_LPG_FORCEWAKE_ACK_RENDER);
    IOLog("[IntelArcMTL] MMIO GMD_ID_GRAPHICS  : 0x%08X -> Xe Arch: %u.%u (Stepping: %u)\n",
          gmdGraphics, gfxArch, gfxRel, gfxStep);
    IOLog("[IntelArcMTL] MMIO GMD_ID_DISPLAY   : 0x%08X (Offset 0x%06X)\n", gmdDisplay, XE_LPG_GMD_ID_DISPLAY);
    IOLog("[IntelArcMTL] MMIO GMD_ID_MEDIA     : 0x%08X (Offset 0x%06X)\n", gmdMedia, XE_LPG_GMD_ID_MEDIA);
    IOLog("[IntelArcMTL] MMIO GEN8_MASTER_IRQ  : 0x%08X (Offset 0x%06X)\n", masterIrq, XE_LPG_GEN8_MASTER_IRQ);
    IOLog("[IntelArcMTL] MMIO MIRROR_FUSE3     : 0x%08X (Offset 0x%06X)\n", fuse3, XE_LPG_MIRROR_FUSE3);
    IOLog("[IntelArcMTL] ==============================================================\n");

    // 3. Lưu thông số trực tiếp vào IORegistry để dễ dàng kiểm tra từ User-space
    setProperty("BAR0PhysicalAddress", fBAR0PhysicalAddr, 64);
    setProperty("BAR0Length", fBAR0Length, 64);
    setProperty("GMD_ID_Graphics", gmdGraphics, 32);
    setProperty("GMD_ID_Display", gmdDisplay, 32);
    setProperty("GMD_ID_Media", gmdMedia, 32);
}
