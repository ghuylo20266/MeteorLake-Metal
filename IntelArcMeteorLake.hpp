#ifndef IntelArcMeteorLake_hpp
#define IntelArcMeteorLake_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

// Định danh phần cứng PCI
#define INTEL_VENDOR_ID                 0x8086
#define METEOR_LAKE_ARC_DEVICE_ID       0x7D55

// Các thanh ghi MMIO đặc trưng của kiến trúc Intel Xe-LPG (Meteor Lake) trong BAR0
#define XE_LPG_GMD_ID_GRAPHICS          0x000D8C  // Graphics Architecture ID (Ver, Rel, Step)
#define XE_LPG_FORCEWAKE_ACK_RENDER     0x000D84  // Thanh ghi xác nhận trạng thái Forcewake Render
#define XE_LPG_FORCEWAKE_RENDER         0x00A278  // Thanh ghi điều khiển Forcewake Render (Masked)
#define XE_LPG_GEN8_MASTER_IRQ          0x044200  // Master Interrupt Control
#define XE_LPG_GMD_ID_DISPLAY           0x0451A0  // Display Engine Architecture ID
#define XE_LPG_MIRROR_FUSE3             0x009118  // Cấu hình Slice / Subslice / EU Fuse
#define XE_LPG_GMD_ID_MEDIA_BASE        0x1C0000  // Offset cơ sở của Media Tile (SAMedia)
#define XE_LPG_GMD_ID_MEDIA             (XE_LPG_GMD_ID_MEDIA_BASE + 0x000D8C)

class com_huy_driver_IntelArcMeteorLake : public IOService
{
    OSDeclareDefaultStructors(com_huy_driver_IntelArcMeteorLake)

private:
    IOPCIDevice             *fPCIDevice;
    IOMemoryMap             *fBAR0Map;
    volatile uint8_t        *fMMIOBase;
    IOPhysicalAddress       fBAR0PhysicalAddr;
    IOByteCount             fBAR0Length;

    // Đọc/ghi 32-bit trực tiếp vào vùng nhớ MMIO của BAR0
    inline uint32_t readMMIO32(uint32_t offset) const {
        if (!fMMIOBase || (offset + sizeof(uint32_t) > fBAR0Length)) {
            return 0xFFFFFFFF;
        }
        return *reinterpret_cast<volatile const uint32_t *>(fMMIOBase + offset);
    }

    inline void writeMMIO32(uint32_t offset, uint32_t value) {
        if (!fMMIOBase || (offset + sizeof(uint32_t) > fBAR0Length)) {
            return;
        }
        *reinterpret_cast<volatile uint32_t *>(fMMIOBase + offset) = value;
        OSSynchronizeIO();
    }

    bool wakeRenderDomain(bool wake);
    void dumpHardwareRegisters(void);

public:
    virtual bool init(OSDictionary *dictionary = nullptr) override;
    virtual void free(void) override;
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
};

#endif /* IntelArcMeteorLake_hpp */
