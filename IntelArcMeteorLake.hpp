#ifndef IntelArcMeteorLake_hpp
#define IntelArcMeteorLake_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>

#define INTEL_VENDOR_ID                 0x8086
#define METEOR_LAKE_ARC_DEVICE_ID       0x7D55

// Thanh ghi kiến trúc Intel Xe-LPG (BAR0 MMIO)
#define XE_LPG_GMD_ID_GRAPHICS          0x000D8C
#define XE_LPG_FORCEWAKE_ACK_RENDER     0x000D84
#define XE_LPG_FORCEWAKE_RENDER         0x00A278
#define XE_LPG_GEN8_MASTER_IRQ          0x044200
#define XE_LPG_GMD_ID_DISPLAY           0x0451A0

// Thanh ghi Display Engine Pipe A (Intel Xe-LPD+ trên Meteor Lake)
#define XE_LPD_PLANE_CTL_1_A            0x070180
#define XE_LPD_PLANE_STRIDE_1_A         0x070188
#define XE_LPD_PLANE_SURF_1_A           0x07019C
#define PLANE_CTL_ENABLE                (1U << 31)

#define ARC_MODE_COUNT                  6

struct ArcDisplayMode {
    IODisplayModeID id;
    uint32_t        width;
    uint32_t        height;
    uint32_t        hz;             // Tần số nguyên (60, 90, 120, 144, 165, 240)
    uint32_t        refreshRateQ16; // Định dạng 16.16 Fixed-Point (hz << 16)
    uint64_t        pixelClockHz;   // Xung nhịp điểm ảnh chuẩn CVT-RB
    uint32_t        vsyncIntervalUs;// Chu kỳ VBlank tính bằng micro-giây (1000000 / hz)
};

class com_huy_driver_IntelArcMeteorLake : public IOService
{
    OSDeclareDefaultStructors(com_huy_driver_IntelArcMeteorLake)

private:
    IOPCIDevice             *fPCIDevice;

    // BAR0: GTTMMADR (2MB MMIO Registers - Uncached)
    IOMemoryMap             *fBAR0Map;
    volatile uint8_t        *fMMIOBase;
    IOPhysicalAddress       fBAR0PhysicalAddr;
    IOByteCount             fBAR0Length;

    // BAR2: GMADR (Aperture VRAM - Write-Combining để tối ưu tốc độ vẽ khung hình)
    IOMemoryMap             *fBAR2Map;
    volatile uint8_t        *fApertureBase;
    IOPhysicalAddress       fBAR2PhysicalAddr;
    IOByteCount             fBAR2Length;

    bool wakeRenderDomain(bool wake);
    void dumpHardwareRegisters(void);

public:
    virtual bool init(OSDictionary *dictionary = nullptr) override;
    virtual void free(void) override;
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

    inline uint32_t readMMIO32(uint32_t offset) const {
        if (!fMMIOBase || (offset + sizeof(uint32_t) > fBAR0Length)) return 0xFFFFFFFF;
        return *reinterpret_cast<volatile const uint32_t *>(fMMIOBase + offset);
    }

    inline void writeMMIO32(uint32_t offset, uint32_t value) {
        if (!fMMIOBase || (offset + sizeof(uint32_t) > fBAR0Length)) return;
        *reinterpret_cast<volatile uint32_t *>(fMMIOBase + offset) = value;
        OSSynchronizeIO();
    }

    inline IOPhysicalAddress getAperturePhysical(void) const { return fBAR2PhysicalAddr; }
    inline IOByteCount getApertureLength(void) const { return fBAR2Length; }
};

class IntelArcFramebuffer : public IOFramebuffer
{
    OSDeclareDefaultStructors(IntelArcFramebuffer)

private:
    com_huy_driver_IntelArcMeteorLake *fProvider;
    ArcDisplayMode                    fModes[ARC_MODE_COUNT];
    uint32_t                          fCurrentModeIndex;
    IOTimerEventSource                *fVSyncTimer;
    IOFBInterruptProc                 fVBlankInterruptProc;
    OSObject                          *fVBlankTarget;
    void                              *fVBlankRef;
    bool                              fVBlankEnabled;

    static void vsyncTimerFired(OSObject *owner, IOTimerEventSource *sender);
    void programDisplayPipe(const ArcDisplayMode &mode);

public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

    virtual IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    virtual const char *getPixelFormats(void) override;
    virtual IOItemCount getDisplayModeCount(void) override;
    virtual IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) override;
    virtual IOReturn getInformationForDisplayMode(IODisplayModeID displayMode, IODisplayModeInformation *info) override;
    virtual UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
    virtual IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pixelInfo) override;
    virtual IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;
    virtual IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
    virtual IOReturn getTimingInfoForDisplayMode(IODisplayModeID displayMode, IOTimingInformation *info) override;
    virtual IOReturn registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc, OSObject *target, void *ref, void **interruptRef) override;
    virtual IOReturn setInterruptState(void *interruptRef, UInt32 state) override;
    virtual IOReturn enableController(void) override;
};

#endif /* IntelArcMeteorLake_hpp */
