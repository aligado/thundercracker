#include "installer.h"
#include "usbprotocol.h"
#include "elfdebuginfo.h"

#include <sifteo/abi/elf.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>

int Installer::run(int argc, char **argv, IODevice &_dev)
{
    bool launcher = false;
    const char *path = NULL;

    for (unsigned i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) {
            launcher = true;
        } else if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "incorrect args\n");
            return 1;
        }
    }

    Installer installer(_dev);

    bool success = installer.install(path,
        IODevice::SIFTEO_VID, IODevice::BASE_PID, launcher);

    _dev.close();
    _dev.processEvents();

    return success ? 0 : 1;
}

Installer::Installer(IODevice &_dev) :
    dev(_dev)
{}

/*
 * High level program flow for installation.
 *
 * - Ensure given package has the required metatdata.
 * - Send the header info - device may take a while to process this as it
 *      erases/allocates space for upcoming transfer
 * - Send the content of the application.
 * - Commit the transaction.
 */
bool Installer::install(const char *path, int vid, int pid, bool launcher)
{
    isLauncher = launcher;
    if (!launcher && !getPackageMetadata(path))
        return false;

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "could not open %s: %s\n", path, strerror(errno));
        return false;
    }

    if (!dev.open(vid, pid)) {
        fprintf(stderr, "device is not attached\n");
        return false;
    }

    if (launcher)
        fprintf(stderr, "updating launcher\n");
    else
        fprintf(stderr, "installing %s, version %s\n", package.c_str(), version.c_str());

    fseek(f, 0L, SEEK_END);
    uint32_t filesz = ftell(f);
    if (!sendHeader(filesz))
        return false;

    if (!sendFileContents(f, filesz)) {
        return false;
    }

    if (!commit())
        return false;

    fclose(f);

    while (dev.numPendingOUTPackets()) {
        dev.processEvents();
    }
    return true;
}

/*
 * Read package metadata from the elf for this application.
 * For now, must include package and version strings.
 */
bool Installer::getPackageMetadata(const char *path)
{
    ELFDebugInfo dbgInfo;
    if (!dbgInfo.init(path)) {
        fprintf(stderr, "couldn't open %s: %s\n", path, strerror(errno));
        return false;
    }

    if (!dbgInfo.metadataString(_SYS_METADATA_PACKAGE_STR, package)) {
        fprintf(stderr, "couldn't find package string - ensure you've included a call to Metadata::package() in your application\n");
        return false;
    }

    if (!dbgInfo.metadataString(_SYS_METADATA_VERSION_STR, version)) {
        fprintf(stderr, "couldn't find version - ensure you've included a call to Metadata::package() in your application\n");
        return false;
    }

    return true;
}

bool Installer::sendHeader(uint32_t filesz)
{
    USBProtocolMsg m(USBProtocol::Installer);

    if (isLauncher) {
        m.header |= UsbVolumeManager::WriteLauncherHeader;
        m.append((uint8_t*)&filesz, sizeof filesz);
    } else {
        m.header |= UsbVolumeManager::WriteGameHeader;
        m.append((uint8_t*)&filesz, sizeof filesz);
        if (package.size() + 1 > m.bytesFree()) {
            fprintf(stderr, "package name too long\n");
            return false;
        }
        m.append((uint8_t*)package.c_str(), package.size());
    }

    dev.writePacket(m.bytes, m.len);

    // wait for response that header was processed
    while (dev.numPendingINPackets() == 0) {
        dev.processEvents();
    }

    m.len = dev.readPacket(m.bytes, m.MAX_LEN);
    if ((m.header & 0xff) != UsbVolumeManager::WroteHeaderOK) {
        fprintf(stderr, "not enough room for this app\n");
        return false;
    }

    return true;
}

/*
 * Write the body of this file.
 *
 * There are no restrictions on the format of the payload - just fit as much
 * into each packet as we can.
 */
bool Installer::sendFileContents(FILE *f, uint32_t filesz)
{
    rewind(f);
    unsigned percent = 0;
    unsigned progress = 0;

    while (!feof(f)) {

        USBProtocolMsg m(USBProtocol::Installer);
        m.header |= UsbVolumeManager::WritePayload;
        m.len += fread(m.payload, 1,  m.bytesFree(), f);
        if (m.len <= 0)
            continue;

        dev.writePacket(m.bytes, m.len);
        while (dev.numPendingOUTPackets() > IODevice::MAX_OUTSTANDING_OUT_TRANSFERS)
            dev.processEvents();

        progress += m.payloadLen();
        const unsigned progressPercent = ((float)progress / (float)filesz) * 100;
        if (progressPercent != percent) {
            percent = progressPercent;
            fprintf(stderr, "progress: %d%%\n", percent);
        }
    }

    return true;
}

/*
 * We're done sending payload data - tell the master to commit this app.
 */
bool Installer::commit()
{
    USBProtocolMsg m(USBProtocol::Installer);
    m.header |= UsbVolumeManager::WriteCommit;

    dev.writePacket(m.bytes, m.len);
    dev.processEvents();

    return true;
}