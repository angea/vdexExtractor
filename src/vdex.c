#include "common.h"
#include "log.h"
#include "utils.h"
#include "vdex.h"

/*
 * Verify if valid VDEX file
 */
bool vdex_isMagicValid(const uint8_t *cursor)
{
  const vdexHeader *pVdexHeader = (const vdexHeader*)cursor;
  return (memcmp(pVdexHeader->magic_, kVdexMagic, sizeof(kVdexMagic)) == 0);
}

bool vdex_isVersionValid(const uint8_t *cursor)
{
  const vdexHeader *pVdexHeader = (const vdexHeader*)cursor;
  return (memcmp(pVdexHeader->version_, kVdexVersion, sizeof(kVdexVersion)) == 0);
}

bool vdex_isValidVdex(const uint8_t *cursor)
{
  return vdex_isMagicValid(cursor) && vdex_isVersionValid(cursor);
}

bool vdex_hasDexSection(const uint8_t *cursor)
{
  const vdexHeader *pVdexHeader = (const vdexHeader*)cursor;
  return pVdexHeader->dex_size_ != 0;
}

uint32_t vdex_GetSizeOfChecksumsSection(const uint8_t *cursor)
{
  const vdexHeader *pVdexHeader = (const vdexHeader*)cursor;
  return sizeof(VdexChecksum) * pVdexHeader->number_of_dex_files_;
}

const uint8_t* vdex_DexBegin(const uint8_t *cursor)
{
  return cursor + sizeof(vdexHeader) + vdex_GetSizeOfChecksumsSection(cursor);
}

const uint8_t* vdex_DexEnd(const uint8_t *cursor)
{
  const vdexHeader *pVdexHeader = (const vdexHeader*)cursor;
  return vdex_DexBegin(cursor) + pVdexHeader->dex_size_;
}

const uint8_t* vdex_GetNextDexFileData(const uint8_t *cursor, uint32_t *offset)
{
  if (*offset == 0) {
    if (vdex_hasDexSection(cursor)) {
      const uint8_t *dexBuf = vdex_DexBegin(cursor);
      *offset = sizeof(vdexHeader) + vdex_GetSizeOfChecksumsSection(cursor);
      LOGMSG(l_DEBUG, "Processing first DEX file at offset:0x%x", *offset);

      // Adjust offset to point at the end of current DEX file
      dexHeader *pDexHeader = (dexHeader*)(dexBuf);
      *offset += pDexHeader->fileSize;
      return dexBuf;
    } else {
      return NULL;
    }
  } else {
    dexHeader *pDexHeader = (dexHeader*)(cursor + *offset);

    // Check boundaries
    const uint8_t *dexBuf = cursor + *offset;
    const uint8_t *dexBufMax = dexBuf + pDexHeader->fileSize;
    if (dexBufMax == vdex_DexEnd(cursor)) {
      LOGMSG(l_DEBUG, "Processing last DEX file at offset:0x%x", *offset);
    } else if (dexBufMax <= vdex_DexEnd(cursor)) {
      LOGMSG(l_DEBUG, "Processing DEX file at offset:0x%x", *offset);
    } else {
      LOGMSG(l_ERROR, "Invalid cursor offset '0x%x'", *offset);
      return NULL;
    }

    // Adjust offset to point at the end of current DEX file
    *offset += pDexHeader->fileSize;
    return dexBuf;
  }
}

uint32_t vdex_GetLocationChecksum(const uint8_t *cursor, uint32_t fileIdx)
{
  return (cursor + sizeof(vdexHeader))[fileIdx];
}

const uint8_t* vdex_GetVerifierDepsData(const uint8_t *cursor)
{
  const vdexHeader *pVdexHeader = (const vdexHeader*)cursor;
  return cursor + pVdexHeader->dex_size_;
}

uint32_t vdex_GetVerifierDepsDataSize(const uint8_t *cursor)
{
  const vdexHeader *pVdexHeader = (const vdexHeader*)cursor;
  return pVdexHeader->verifier_deps_size_;
}

const uint8_t* vdex_GetQuickeningInfo(const uint8_t *cursor)
{
  const vdexHeader *pVdexHeader = (const vdexHeader*)cursor;
  return cursor + pVdexHeader->dex_size_ + pVdexHeader->verifier_deps_size_;
}

uint32_t vdex_GetQuickeningInfoSize(const uint8_t *cursor)
{
  const vdexHeader *pVdexHeader = (const vdexHeader*)cursor;
  return pVdexHeader->quickening_info_size_;
}

bool vdex_Unquicken(const uint8_t *cursor)
{
  if (vdex_GetQuickeningInfoSize(cursor) == 0) {
    // If there is no quickening info, we bail early, as the code below expects at
    // least the size of quickening data for each method that has a code item.
    return true;
  }

  const vdexHeader *pVdexHeader = (const vdexHeader*)cursor;
  const uint8_t* quickening_info_ptr = vdex_GetQuickeningInfo(cursor);
  const uint8_t* const quickening_info_end = vdex_GetQuickeningInfo(cursor) +
    vdex_GetQuickeningInfoSize(cursor);

  const uint8_t *dexFileBuf = NULL;
  uint32_t offset = 0;

  // For each dex file
  for (size_t dex_file_idx = 0;
       dex_file_idx < pVdexHeader->number_of_dex_files_;
       ++dex_file_idx) {
    dexFileBuf = vdex_GetNextDexFileData(cursor, &offset);
    if (dexFileBuf == NULL) {
      LOGMSG(l_ERROR, "Failed to unquicken 'classes%zu.dex' - skipping",
             dex_file_idx);
      continue;
    }

    const dexHeader *pDexHeader = (const dexHeader*)dexFileBuf;

    // Check if valid dex file
    dex_dumpHeaderInfo(pDexHeader);
    if (!dex_isValidDexMagic(pDexHeader)) {
      LOGMSG(l_ERROR, "Failed to unquicken 'classes%zu.dex' - skipping",
             dex_file_idx);
      continue;
    }

    // For each class
    LOGMSG(l_DEBUG, "[%zu] number of classes: %"PRIu32, dex_file_idx,
           pDexHeader->classDefsSize);
    dexClassDef *dexClassDefs =
        (dexClassDef*)(dexFileBuf + pDexHeader->classDefsOff);

    for (uint32_t i = 0; i < pDexHeader->classDefsSize; ++i) {
      LOGMSG(l_DEBUG, "[%zu] class #%"PRIu32": class_data_off=%"PRIu32, dex_file_idx,
             i, dexClassDefs[i].classDataOff);

      // cursor for currently processed class data item
      const uint8_t *curClassDataCursor;
      if (dexClassDefs[i].classDataOff == 0) {
        continue;
      } else {
        curClassDataCursor = dexFileBuf + dexClassDefs[i].classDataOff;
      }

      dexClassDataHeader pDexClassDataHeader;
      memset(&pDexClassDataHeader, 0, sizeof(dexClassDataHeader));
      dex_readClassDataHeader(&curClassDataCursor, &pDexClassDataHeader);

      LOGMSG(l_DEBUG, "[%zu] class #%"PRIu32": static_fields=%"PRIu32", "
             "instance_fields=%"PRIu32", direct_methods=%"PRIu32", "
             "virtual_methods=%"PRIu32, dex_file_idx, i,
             pDexClassDataHeader.staticFieldsSize,
             pDexClassDataHeader.instanceFieldsSize,
             pDexClassDataHeader.directMethodsSize,
             pDexClassDataHeader.virtualMethodsSize);

      // Skip static fields
      for (uint32_t j = 0; j < pDexClassDataHeader.staticFieldsSize; ++j) {
        dexField pDexField;
        memset(&pDexField, 0, sizeof(dexField));
        dex_readClassDataField(&curClassDataCursor, &pDexField);
      }

      // Skip instance fields
      for (uint32_t j = 0; j < pDexClassDataHeader.instanceFieldsSize; ++j) {
        dexField pDexField;
        memset(&pDexField, 0, sizeof(dexField));
        dex_readClassDataField(&curClassDataCursor, &pDexField);
      }

      // For each direct method
      for (uint32_t j = 0; j < pDexClassDataHeader.directMethodsSize; ++j) {
        dexMethod pDexMethod;
        memset(&pDexMethod, 0, sizeof(dexMethod));
        dex_readClassDataMethod(&curClassDataCursor, &pDexMethod);

        // Skip empty methods
        if (pDexMethod.codeOff == 0) {
          continue;
        }

        // Get method code offset and revert quickened instructions
        dexCode *pDexCode = (dexCode*)(dexFileBuf + pDexMethod.codeOff);

        // For quickening info blob the first 4bytes are the inner blobs size
        uint32_t quickening_size = *quickening_info_ptr;
        quickening_info_ptr += sizeof(uint32_t);
        if (!dex_DexcompileDriver(pDexCode,
                                  quickening_info_ptr,
                                  quickening_size, true)) {
          LOGMSG(l_ERROR, "Failed to decompile DEX file");
          return false;
        }
        quickening_info_ptr += quickening_size;
      }

      // For each virtual method
      for (uint32_t j = 0; j < pDexClassDataHeader.virtualMethodsSize; ++j) {
        dexMethod pDexMethod;
        memset(&pDexMethod, 0, sizeof(dexMethod));
        dex_readClassDataMethod(&curClassDataCursor, &pDexMethod);

        // Skip native or abstract methods
        if (pDexMethod.codeOff == 0) {
          continue;
        }

        // Get method code offset and revert quickened instructions
        dexCode *pDexCode = (dexCode*)(dexFileBuf + pDexMethod.codeOff);

        // For quickening info blob the first 4bytes are the inner blobs size
        uint32_t quickening_size = *quickening_info_ptr;
        quickening_info_ptr += sizeof(uint32_t);
        if (!dex_DexcompileDriver(pDexCode,
                                  quickening_info_ptr,
                                  quickening_size, true)) {
          LOGMSG(l_ERROR, "Failed to decompile DEX file");
          return false;
        }
        quickening_info_ptr += quickening_size;
      }
    }
  }

  if (quickening_info_ptr != quickening_info_end) {
    LOGMSG(l_ERROR, "Failed to process all outer quickening info");
    return false;
  }

  return true;
}