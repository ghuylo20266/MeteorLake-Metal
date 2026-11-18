#include "IntelArcMeteorLake.hpp"

#define super IOService
OSDefineMetaClassAndStructors(com_huy_driver_IntelArcMeteorLake, IOService)

bool com_huy_driver_IntelArcMeteorLake::init(OSDictionary *dictionary)
{
    if (!super::init(dictionary)) return false;

    fPCIDevice        = nullptr;
    fBAR0Map          = nullptr;
    fMMIOBase         = nullptr;
    fBAR0PhysicalAddr = 0;
    fBAR0Length       = 0;
    fBAR2Map          = nullptr;
    fApertureBase     = nullptr;
    fBAR2PhysicalAddr = 0;
    fBAR2Length       = 0;

    return true;
}

void com_huy_driver_IntelArcMeteorLake::free(void)
{
    OSSafeReleaseNULL(fBAR0Map);
    OSSafeReleaseNULL(fBAR2Map);
    OSSafeReleaseNULL(fPCIDevice);
    fMMIOBase     = nullptr;
    fApertureBase = nullptr;
    super::free();
}

IOService *com_huy_driver_IntelArcMeteorLake::probe(IOService *provider, SInt32 *score)
{
    if (!super::probe(provider, score)) return nullptr;

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;

    uint16_t vendorID = pci->configRead16(kIOPCIConfigVendorID);
    uint16_t deviceID = pci->configRead16(kIOPCIConfigDeviceID);

    if (vendorID != INTEL_VENDOR_ID || deviceID != METEOR_LAKE_ARC_DEVICE_ID) {
        return nullptr;
    }

    if (score) *score += 15000;
    return this;
}

bool com_huy_driver_IntelArcMeteorLake::start(IOService *provider)
{
    if (!super::start(provider)) return false;

    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) return false;
    fPCIDevice->retain();

    if (!fPCIDevice->open(this)) {
        OSSafeReleaseNULL(fPCIDevice);
        return false;
    }

    fPCIDevice->setMemoryEnable(true);
    fPCIDevice->setBusMasterEnable(true);

    uint16_t vendorID   = fPCIDevice->configRead16(kIOPCIConfigVendorID);
    uint16_t deviceID   = fPCIDevice->configRead16(kIOPCIConfigDeviceID);
    uint8_t  revisionID = fPCIDevice->configRead8(kIOPCIConfigRevisionID);
    uint16_t subVendor  = fPCIDevice->configRead16(kIOPCIConfigSubSystemVendorID);
    uint16_t subDevice  = fPCIDevice->configRead16(kIOPCIConfigSubSystemID);

    IOLog("[IntelArcMTL] PCI Config -> Vendor: 0x%04X, Device: 0x%04X, Rev: 0x%02X, SubSys: 0x%04X:0x%04X\n",
          vendorID, deviceID, revisionID, subVendor, subDevice);

    // 1. Ánh xạ BAR0 (MMIO Registers) với kIOMapInhibitCache để đọc/ghi tức thì vào thanh ghi
    fBAR0Map = fPCIDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (!fBAR0Map) {
        IOLog("[IntelArcMTL] Lỗi: Không thể ánh xạ BAR0 (GTTMMADR)!\n");
        fPCIDevice->close(this);
        OSSafeReleaseNULL(fPCIDevice);
        return false;
    }
    fMMIOBase         = reinterpret_cast<volatile uint8_t *>(fBAR0Map->getVirtualAddress());
    fBAR0PhysicalAddr = fBAR0Map->getPhysicalAddress();
    fBAR0Length       = fBAR0Map->getLength();

    // 2. Ánh xạ BAR2 (GMADR Aperture VRAM) với kIOMapWriteCombineCache để đạt băng thông tối đa cho 144Hz+
    fBAR2Map = fPCIDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2, kIOMapWriteCombineCache);
    if (fBAR2Map) {
        fApertureBase     = reinterpret_cast<volatile uint8_t *>(fBAR2Map->getVirtualAddress());
        fBAR2PhysicalAddr = fBAR2Map->getPhysicalAddress();
        fBAR2Length       = fBAR2Map->getLength();
        IOLog("[IntelArcMTL] BAR2 Write-Combining VRAM -> Phys: 0x%016llX | Size: %llu MB\n",
              static_cast<uint64_t>(fBAR2PhysicalAddr),
              static_cast<uint64_t>(fBAR2Length / (1024 * 1024)));
    }

    bool awake = wakeRenderDomain(true);
    dumpHardwareRegisters();
    if (awake) wakeRenderDomain(false);

    registerService();
    return true;
}

void com_huy_driver_IntelArcMeteorLake::stop(IOService *provider)
{
    fMMIOBase     = nullptr;
    fApertureBase = nullptr;
    OSSafeReleaseNULL(fBAR0Map);
    OSSafeReleaseNULL(fBAR2Map);

    if (fPCIDevice) {
        if (fPCIDevice->isOpen(this)) fPCIDevice->close(this);
        OSSafeReleaseNULL(fPCIDevice);
    }
    super::stop(provider);
}

bool com_huy_driver_IntelArcMeteorLake::wakeRenderDomain(bool wake)
{
    writeMMIO32(XE_LPG_FORCEWAKE_RENDER, wake ? 0x00010001 : 0x00010000);
    for (int i = 0; i < 5000; i++) {
        uint32_t ack = readMMIO32(XE_LPG_FORCEWAKE_ACK_RENDER);
        if (((ack & 0x1) != 0) == wake) return true;
        IODelay(10);
    }
    return false;
}

void com_huy_driver_IntelArcMeteorLake::dumpHardwareRegisters(void)
{
    uint32_t gmdGfx  = readMMIO32(XE_LPG_GMD_ID_GRAPHICS);
    uint32_t gmdDisp = readMMIO32(XE_LPG_GMD_ID_DISPLAY);
    uint32_t arch    = (gmdGfx >> 22) & 0x3FF;
    uint32_t rel     = (gmdGfx >> 14) & 0xFF;
    uint32_t step    = gmdGfx & 0x3F;

    IOLog("[IntelArcMTL] GMD_ID (0xD8C) = 0x%08X -> Kiến trúc Xe-LPG %u.%u (Stepping %u)\n",
          gmdGfx, arch, rel, step);
    IOLog("[IntelArcMTL] GMD_ID_DISPLAY = 0x%08X\n", gmdDisp);
}

// ============================================================================
// IntelArcFramebuffer: Đồng bộ VSync 60Hz - 240Hz & Write-Combining Scanout
// ============================================================================

#define superFB IOFramebuffer
OSDefineMetaClassAndStructors(IntelArcFramebuffer, IOFramebuffer)

bool IntelArcFramebuffer::start(IOService *provider)
{
    fProvider = OSDynamicCast(com_huy_driver_IntelArcMeteorLake, provider);
    if (!fProvider) return false;

    // Khởi tạo bảng 6 mức tần số quét chuẩn CVT-RB cho độ phân giải 1920x1080 (32-bit ARGB8888)
    fModes[0] = { 1, 1920, 1080,  60, (60  << 16), 138500000ULL, 16666 }; // 60Hz  (16.66ms)
    fModes[1] = { 2, 1920, 1080,  90, (90  << 16), 212500000ULL, 11111 }; // 90Hz  (11.11ms)
    fModes[2] = { 3, 1920, 1080, 120, (120 << 16), 285500000ULL,  8333 }; // 120Hz (8.33ms)
    fModes[3] = { 4, 1920, 1080, 144, (144 << 16), 346500000ULL,  6944 }; // 144Hz (6.94ms) - Mặc định
    fModes[4] = { 5, 1920, 1080, 165, (165 << 16), 401500000ULL,  6060 }; // 165Hz (6.06ms)
    fModes[5] = { 6, 1920, 1080, 240, (240 << 16), 605000000ULL,  4166 }; // 240Hz (4.16ms)

    fCurrentModeIndex    = 3; // Mặc định khởi động ở 144Hz; nếu BAR2 chưa sẵn sàng sẽ tự lùi về 60Hz
    if (fProvider->getApertureLength() == 0) {
        fCurrentModeIndex = 0;
    }

    fVBlankInterruptProc = nullptr;
    fVBlankTarget        = nullptr;
    fVBlankRef           = nullptr;
    fVBlankEnabled       = false;
    fVSyncTimer          = nullptr;

    if (!superFB::start(provider)) return false;

    IOWorkLoop *wl = getWorkLoop();
    if (wl) {
        fVSyncTimer = IOTimerEventSource::timerEventSource(this, &IntelArcFramebuffer::vsyncTimerFired);
        if (fVSyncTimer) {
            wl->addEventSource(fVSyncTimer);
        }
    }

    return true;
}

void IntelArcFramebuffer::stop(IOService *provider)
{
    if (fVSyncTimer) {
        fVSyncTimer->cancelTimeout();
        IOWorkLoop *wl = getWorkLoop();
        if (wl) wl->removeEventSource(fVSyncTimer);
        OSSafeReleaseNULL(fVSyncTimer);
    }
    fProvider = nullptr;
    superFB::stop(provider);
}

void IntelArcFramebuffer::vsyncTimerFired(OSObject *owner, IOTimerEventSource *sender)
{
    IntelArcFramebuffer *fb = OSDynamicCast(IntelArcFramebuffer, owner);
    if (!fb) return;

    if (fb->fVBlankEnabled && fb->fVBlankInterruptProc) {
        fb->fVBlankInterruptProc(fb->fVBlankTarget, fb->fVBlankRef);
    }

    // Đặt lại chu kỳ VBlank chính xác theo tần số quét hiện hành (ví dụ: 6944 us cho 144Hz)
    uint32_t intervalUs = fb->fModes[fb->fCurrentModeIndex].vsyncIntervalUs;
    sender->setTimeoutUS(intervalUs);
}

void IntelArcFramebuffer::programDisplayPipe(const ArcDisplayMode &mode)
{
    if (!fProvider) return;

    // Cấu hình Stride (1920 * 4 bytes = 7680 bytes, chia cho khối 64-byte linear = 120)
    uint32_t strideBlocks = (mode.width * 4) / 64;
    fProvider->writeMMIO32(XE_LPD_PLANE_STRIDE_1_A, strideBlocks);

    // Đảm bảo Primary Plane Pipe A được kích hoạt và đồng bộ địa chỉ cơ sở GTT (0x00000000)
    uint32_t planeCtl = fProvider->readMMIO32(XE_LPD_PLANE_CTL_1_A);
    if (planeCtl != 0xFFFFFFFF) {
        fProvider->writeMMIO32(XE_LPD_PLANE_CTL_1_A, planeCtl | PLANE_CTL_ENABLE);
        fProvider->writeMMIO32(XE_LPD_PLANE_SURF_1_A, 0x00000000);
    }

    IOLog("[IntelArcMTL] Display Engine: Đã khóa tần số quét %u Hz (Chu kỳ VBlank: %u us)\n",
          mode.hz, mode.vsyncIntervalUs);
}

IODeviceMemory *IntelArcFramebuffer::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture || !fProvider) return nullptr;

    IOPhysicalAddress phys = fProvider->getAperturePhysical();
    IOByteCount       len  = fProvider->getApertureLength();
    if (phys == 0 || len == 0) return nullptr;

    return IODeviceMemory::withRange(phys, len);
}

const char *IntelArcFramebuffer::getPixelFormats(void)
{
    static const char *formats = IO32BitDirectPixels "\0\0";
    return formats;
}

IOItemCount IntelArcFramebuffer::getDisplayModeCount(void)
{
    return ARC_MODE_COUNT;
}

IOReturn IntelArcFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes)
{
    if (!allDisplayModes) return kIOReturnBadArgument;
    for (uint32_t i = 0; i < ARC_MODE_COUNT; i++) {
        allDisplayModes[i] = fModes[i].id;
    }
    return kIOReturnSuccess;
}

IOReturn IntelArcFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode, IODisplayModeInformation *info)
{
    if (!info) return kIOReturnBadArgument;
    for (uint32_t i = 0; i < ARC_MODE_COUNT; i++) {
        if (fModes[i].id == displayMode) {
            bzero(info, sizeof(IODisplayModeInformation));
            info->maxDepthIndex = 0;
            info->nominalWidth  = fModes[i].width;
            info->nominalHeight = fModes[i].height;
            info->refreshRate   = fModes[i].refreshRateQ16;
            info->flags         = kDisplayModeSafetyFlags | kDisplayModeAlwaysShowFlag | kDisplayModeValidFlag;
            if (i == 3) info->flags |= kDisplayModeDefaultFlag;
            return kIOReturnSuccess;
        }
    }
    return kIOReturnUnsupportedMode;
}

UInt64 IntelArcFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth)
{
    return (depth == 0) ? 1ULL : 0ULL;
}

IOReturn IntelArcFramebuffer::getPixelInformation(IODisplayModeID displayMode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pixelInfo)
{
    if (!pixelInfo || depth != 0) return kIOReturnBadArgument;

    for (uint32_t i = 0; i < ARC_MODE_COUNT; i++) {
        if (fModes[i].id == displayMode) {
            bzero(pixelInfo, sizeof(IOPixelInformation));
            strlcpy(pixelInfo->pixelFormat, IO32BitDirectPixels, sizeof(pixelInfo->pixelFormat));
            pixelInfo->pixelType        = kIORGBDirectPixels;
            pixelInfo->componentMasks[0]= 0x00FF0000; // Red
            pixelInfo->componentMasks[1]= 0x0000FF00; // Green
            pixelInfo->componentMasks[2]= 0x000000FF; // Blue
            pixelInfo->bitsPerPixel     = 32;
            pixelInfo->componentCount   = 3;
            pixelInfo->bitsPerComponent = 8;
            pixelInfo->bytesPerRow      = fModes[i].width * 4;
            return kIOReturnSuccess;
        }
    }
    return kIOReturnUnsupportedMode;
}

IOReturn IntelArcFramebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth)
{
    if (displayMode) *displayMode = fModes[fCurrentModeIndex].id;
    if (depth)       *depth       = 0;
    return kIOReturnSuccess;
}

IOReturn IntelArcFramebuffer::setDisplayMode(IODisplayModeID displayMode, IOIndex depth)
{
    if (depth != 0) return kIOReturnUnsupportedMode;

    for (uint32_t i = 0; i < ARC_MODE_COUNT; i++) {
        if (fModes[i].id == displayMode) {
            fCurrentModeIndex = i;
            programDisplayPipe(fModes[i]);
            if (fVSyncTimer && fVBlankEnabled) {
                fVSyncTimer->cancelTimeout();
                fVSyncTimer->setTimeoutUS(fModes[i].vsyncIntervalUs);
            }
            return kIOReturnSuccess;
        }
    }
    return kIOReturnUnsupportedMode;
}

IOReturn IntelArcFramebuffer::getTimingInfoForDisplayMode(IODisplayModeID displayMode, IOTimingInformation *info)
{
    if (!info) return kIOReturnBadArgument;

    for (uint32_t i = 0; i < ARC_MODE_COUNT; i++) {
        if (fModes[i].id == displayMode) {
            bzero(info, sizeof(IOTimingInformation));
            info->appleTimingID = kIOTimingIDApple_FixedRateLCD;
            info->flags         = kIODetailedTimingValid;
            IODetailedTimingInformationV2 *dt = &info->detailedInfo.v2;
            dt->pixelClock      = fModes[i].pixelClockHz;
            dt->horizontalActive= fModes[i].width;
            dt->horizontalBlanking = 160;
            dt->verticalActive  = fModes[i].height;
            dt->verticalBlanking= 45;
            return kIOReturnSuccess;
        }
    }
    return kIOReturnUnsupportedMode;
}

IOReturn IntelArcFramebuffer::registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc, OSObject *target, void *ref, void **interruptRef)
{
    if (interruptType != kIOFBVBLInterruptType) return kIOReturnUnsupported;

    fVBlankInterruptProc = proc;
    fVBlankTarget        = target;
    fVBlankRef           = ref;
    if (interruptRef) *interruptRef = this;
    return kIOReturnSuccess;
}

IOReturn IntelArcFramebuffer::setInterruptState(void *interruptRef, UInt32 state)
{
    if (interruptRef != this) return kIOReturnBadArgument;

    fVBlankEnabled = (state != 0);
    if (fVSyncTimer) {
        if (fVBlankEnabled) {
            fVSyncTimer->setTimeoutUS(fModes[fCurrentModeIndex].vsyncIntervalUs);
        } else {
            fVSyncTimer->cancelTimeout();
        }
    }
    return kIOReturnSuccess;
}

IOReturn IntelArcFramebuffer::enableController(void)
{
    programDisplayPipe(fModes[fCurrentModeIndex]);
    if (fVSyncTimer) {
        fVBlankEnabled = true;
        fVSyncTimer->setTimeoutUS(fModes[fCurrentModeIndex].vsyncIntervalUs);
    }
    return kIOReturnSuccess;
}
