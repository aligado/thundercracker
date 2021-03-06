/* -*- mode: C; c-basic-offset: 4; intent-tabs-mode: nil -*-
 *
 * Thundercracker firmware
 *
 * Copyright <c> 2012 Sifteo, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "assetutil.h"
#include "assetslot.h"
#include "cubeslots.h"
#include "cube.h"
#include "svmruntime.h"
#include "svmmemory.h"
#include "svmloader.h"


_SYSAssetGroupCube *AssetUtil::mapGroupCube(SvmMemory::VirtAddr group, _SYSCubeID cid)
{
    ASSERT(cid < _SYS_NUM_CUBE_SLOTS);

    SvmMemory::VirtAddr va = group + sizeof(_SYSAssetGroup) + cid * sizeof(_SYSAssetGroupCube);
    SvmMemory::PhysAddr pa;

    if (!SvmMemory::mapRAM(va, sizeof(_SYSAssetGroupCube), pa))
        return 0;

    return reinterpret_cast<_SYSAssetGroupCube*>(pa);
}

_SYSAssetLoaderCube *AssetUtil::mapLoaderCube(SvmMemory::VirtAddr loader, _SYSCubeID cid)
{
    ASSERT(cid < _SYS_NUM_CUBE_SLOTS);

    SvmMemory::VirtAddr va = loader + sizeof(_SYSAssetLoader) + cid * sizeof(_SYSAssetLoaderCube);
    SvmMemory::PhysAddr pa;

    if (!SvmMemory::mapRAM(va, sizeof(_SYSAssetLoaderCube), pa))
        return 0;

    return reinterpret_cast<_SYSAssetLoaderCube*>(pa);
}

unsigned AssetUtil::loadedBaseAddr(SvmMemory::VirtAddr group, _SYSCubeID cid)
{
    _SYSAssetGroupCube *gc = mapGroupCube(group, cid);
    return gc ? gc->baseAddr : 0;
}

bool AssetUtil::isValidConfig(const _SYSAssetConfiguration *cfg, unsigned cfgSize)
{
    /*
     * Initial validation for a _SYSAssetConfiguration. We can't rely exclusively
     * on this pre-validation, since we're dealing with data in user RAM that
     * may change at any time, but we can use this validation to catch unintentional
     * errors early and deliver an appropriate fault.
     */

    // Too large to possibly work?
    if (cfgSize > _SYS_ASSET_GROUPS_PER_SLOT * _SYS_ASSET_SLOTS_PER_BANK)
        return false;

    struct {
        uint16_t slotTiles[_SYS_ASSET_SLOTS_PER_BANK];
        uint8_t slotGroups[_SYS_ASSET_SLOTS_PER_BANK];
    } count;
    memset(&count, 0, sizeof count);

    while (cfgSize) {
        unsigned numTiles = roundup<_SYS_ASSET_GROUP_SIZE_UNIT>(cfg->numTiles);
        unsigned slot = cfg->slot;
        SvmMemory::VirtAddr groupVA = cfg->pGroup;
        SvmMemory::PhysAddr groupPA;
        _SYSVolumeHandle volHandle = cfg->volume;

        if (volHandle) {
            // Make sure the handle is signed and this is a real volume
            FlashVolume volume(volHandle);
            if (!volume.isValid()) {
                LOG(("ASSET: Bad volume handle 0x%08x in _SYSAssetConfiguration\n", volHandle));
                return false;
            }
        }

        if (!SvmMemory::mapRAM(groupVA, sizeof(_SYSAssetGroup), groupPA)) {
            LOG(("ASSET: Bad _SYSAssetGroup pointer 0x%08x in _SYSAssetConfiguration\n", unsigned(groupVA)));
            return false;
        }

        if (slot >= _SYS_ASSET_SLOTS_PER_BANK || !VirtAssetSlots::isSlotBound(slot)) {
            LOG(("ASSET: Bad slot number %d in _SYSAssetConfiguration\n", slot));
            return false;
        }

        if (count.slotGroups[slot] >= _SYS_ASSET_GROUPS_PER_SLOT) {
            LOG(("ASSET: Bad _SYSAssetConfiguration, too many groups in slot %d\n", slot));
            return false;
        }

        if (unsigned(count.slotTiles[slot]) + numTiles > _SYS_TILES_PER_ASSETSLOT) {
            LOG(("ASSET: Bad _SYSAssetConfiguration, too many tiles in slot %d\n", slot));
            return false;
        }

        count.slotGroups[slot]++;
        count.slotTiles[slot] += numTiles;

        cfg++;
        cfgSize--;
    }

    return true;
}

bool AssetGroupInfo::fromUserPointer(const _SYSAssetGroup *group)
{
    /*
     * 'group' is a userspace pointer, in the current volume.
     * We raise a fault and return false on error.
     *
     * Here we map the group header from flash and read metadata out of it.
     */

    // These groups never use volume remapping
    remapToVolume = false;

    // Capture user pointer prior to mapRAM
    va = reinterpret_cast<SvmMemory::VirtAddr>(group);

    if (!isAligned(group)) {
        SvmRuntime::fault(F_SYSCALL_ADDR_ALIGN);
        return false;
    }
    if (!SvmMemory::mapRAM(group)) {
        SvmRuntime::fault(F_SYSCALL_ADDRESS);
        return false;
    }

    SvmMemory::VirtAddr localHeaderVA = group->pHdr;
    headerVA = localHeaderVA;

    _SYSAssetGroupHeader header;
    if (!SvmMemory::copyROData(header, localHeaderVA)) {
        SvmRuntime::fault(F_SYSCALL_ADDRESS);
        return false;
    }

    dataSize = header.dataSize;
    numTiles = header.numTiles;
    ordinal = header.ordinal;

    volume = SvmLoader::volumeForVA(localHeaderVA);
    if (!volume.block.isValid()) {
        SvmRuntime::fault(F_SYSCALL_PARAM);
        return false;
    }

    return true;
}

bool AssetGroupInfo::fromAssetConfiguration(const _SYSAssetConfiguration *config)
{
    /*
     * The 'config' here has already been validated and mapped, but none of
     * its contents can be trusted, as the AssetConfiguration is resident
     * in userspace RAM.
     *
     * To further complicate things, alternate Volumes can be selected
     * other than the one that's currently executing. (This is used for
     * launcher icons, for example.)
     *
     * The division of labour between this function and AssetUtil::isValidConfig()
     * is subtle. That function needs to report the easy errors in very obvious
     * ways, to help userspace programmers debug their code. As such, it should
     * check for things that are commonly mistaken. But it can't do anything to
     * protect the system, since userspace could simply modify the config after
     * loading has started. So, this function needs to protect the system. But
     * it doesn't need to be particularly friendly, since at this point we should
     * only be hitting very obscure bugs or intentionally malicious code.
     *
     * On error, we return 'false' but do NOT raise any faults. We just log them.
     */

    // Must read these userspace fields exactly once
    SvmMemory::VirtAddr groupVA = config->pGroup;
    _SYSVolumeHandle volHandle = config->volume;

    // Save original group VA
    va = groupVA;

    // Local copy of _SYSAssetGroup itself
    _SYSAssetGroup group;
    if (!SvmMemory::copyROData(group, groupVA)) {
        LOG(("ASSET: Bad group pointer 0x%08x in _SYSAssetConfiguration\n", unsigned(groupVA)));
        return false;
    }

    // Save information originally from the header, copied to 'config' by userspace
    dataSize = config->dataSize;
    numTiles = config->numTiles;
    ordinal = config->ordinal;

    // Save header VA, from the _SYSAssetGroup
    SvmMemory::VirtAddr localHeaderVA = group.pHdr;
    headerVA = localHeaderVA;

    if (volHandle) {
        /*
         * A volume was specified. It needs to be valid, and the header VA must
         * be inside SVM Segment 1. We'll treat such a pointer as an offset into
         * the proper volume as if that volume was mapped into Segment 1.
         */

        FlashVolume localVolume(volHandle);
        if (!localVolume.isValid()) {
            LOG(("ASSET: Bad volume handle 0x%08x in _SYSAssetConfiguration\n", volHandle));
            return false;
        }

        uint32_t offset = localHeaderVA - SvmMemory::SEGMENT_1_VA;
        if (offset >= FlashMap::NUM_BYTES - sizeof(_SYSAssetGroupHeader)) {
            LOG(("ASSET: Header VA 0x%08x in _SYSAssetConfiguration not in SEGMENT_1\n",
                 unsigned(localHeaderVA)));
            return false;
        }

        volume = localVolume;
        remapToVolume = true;

    } else {
        /*
         * No volume specified. The VA can be anything, we'll treat it like a
         * normal address. Note that we need to explicitly remember that no
         * remapping is occurring, so that future reads of asset data from
         * Segment 1 come from the actual mapped volume, and not our substitute
         * fake mapping :)
         */

        FlashVolume localVolume = SvmLoader::volumeForVA(localHeaderVA);
        if (!localVolume.block.isValid()) {
            LOG(("ASSET: Bad local header VA 0x%08x in _SYSAssetConfiguration\n",
                unsigned(localHeaderVA)));
            return false;
        }

        volume = localVolume;
        remapToVolume = false;
    }

    return true;
}

unsigned AssetFIFO::fetchFromGroup(_SYSAssetLoaderCube &sys, AssetGroupInfo &group, unsigned offset)
{
    /*
     * Fetch asset data from an AssetGroupInfo. If 'remapToVolume' is set,
     * the supplied VA is relative to SEGMENT_1, and we interpret it as an
     * offset into a specified volume which may not be mapped into SVM's
     * virtual address space.
     *
     * If 'remapToVolume' is not set, we treat headerVA as a normal SVM
     * virtual address.
     *
     * Starts reading from the group at 'offset'. Returns the actual number
     * of bytes transferred into the FIFO, which may be limited either by
     * available FIFO space or by running out of asset data.
     */

    /*
     * For better optimization, we keep a local AssetFIFO instance on the stack.
     * Most uses of 'head' and 'count' should totally optimize out.
     */
    AssetFIFO fifo(sys);

    unsigned dataSize = group.dataSize;
    if (offset >= dataSize)
        return 0;

    unsigned actualSize = MIN(dataSize - offset, fifo.writeAvailable());
    if (!actualSize)
        return 0;

    SvmMemory::VirtAddr va = group.headerVA + sizeof(_SYSAssetGroupHeader) + offset;

    if (group.remapToVolume) {
        // Copy using low-level FlashMap primitives, without using the SVM address space

        FlashBlockRef mapRef, dataRef;
        FlashMapSpan span = group.volume.getPayload(mapRef);        
        va -= SvmMemory::SEGMENT_1_VA;
        unsigned remaining = actualSize;

        do {
            unsigned chunk = MIN(remaining, _SYS_ASSETLOAD_BUF_SIZE - fifo.tail);
            span.copyBytes(dataRef, va, &fifo.sys.buf[fifo.tail], chunk);
            remaining -= chunk;
            va += chunk;
            fifo.tail += chunk;
            ASSERT(fifo.tail <= _SYS_ASSETLOAD_BUF_SIZE);
            if (fifo.tail == _SYS_ASSETLOAD_BUF_SIZE)
                fifo.tail = 0;
        } while (remaining);

    } else {
        // Treat the source as a normal SVM virtual address

        FlashBlockRef dataRef;
        unsigned remaining = actualSize;

        do {
            unsigned chunk = MIN(remaining, _SYS_ASSETLOAD_BUF_SIZE - fifo.tail);
            SvmMemory::copyROData(dataRef, &fifo.sys.buf[fifo.tail], va, chunk);
            remaining -= chunk;
            va += chunk;
            fifo.tail += chunk;
            ASSERT(fifo.tail <= _SYS_ASSETLOAD_BUF_SIZE);
            if (fifo.tail == _SYS_ASSETLOAD_BUF_SIZE)
                fifo.tail = 0;
        } while (remaining);
    }

    fifo.commitWrites();
    return actualSize;
}

void AssetGroupInfo::copyCRC(uint8_t *buffer) const
{
    /*
     * Read the CRC from an AssetGroup, as computed by stir.
     */

    SvmMemory::VirtAddr va = headerVA + offsetof(_SYSAssetGroupHeader, crc);

    if (remapToVolume) {
        // Low-level volume mapping
        FlashBlockRef mapRef, dataRef;
        FlashMapSpan span = volume.getPayload(mapRef);        
        va -= SvmMemory::SEGMENT_1_VA;
        span.copyBytes(dataRef, va, buffer, _SYS_ASSET_GROUP_CRC_SIZE);
    } else {
        // Normal SVM virtual address
        FlashBlockRef dataRef;
        SvmMemory::copyROData(dataRef, buffer, va, _SYS_ASSET_GROUP_CRC_SIZE);
    }
}
