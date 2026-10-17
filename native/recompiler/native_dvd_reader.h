#ifndef HMAWL_NATIVE_DVD_READER_H
#define HMAWL_NATIVE_DVD_READER_H

#include "disc_import.h"
#include "ppc_state.h"

class NativeDvdReader {
public:
    explicit NativeDvdReader(RvzImage& disc);

    // Reads an original DVDFileInfo's range; does not submit SDK commands or callbacks.
    uint32_t ReadOpenedFile(PpcState& state, uint32_t fileInfo, uint32_t destination,
                            uint32_t length, uint32_t offset);
    uint64_t ValidateOpenedFileRead(const PpcState& state, uint32_t fileInfo,
                                   uint32_t destination, uint32_t length,
                                   uint32_t offset) const;

private:
    RvzImage& disc_;
    std::vector<DiscFileEntry> files_;
};

#endif
