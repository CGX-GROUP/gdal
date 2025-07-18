/******************************************************************************
 *
 * Project:  FlatGeobuf driver
 * Purpose:  Implements OGRFlatGeobufLayer class.
 * Author:   Björn Harrtell <bjorn at wololo dot org>
 *
 ******************************************************************************
 * Copyright (c) 2018-2020, Björn Harrtell <bjorn at wololo dot org>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogrsf_frmts.h"
#include "cpl_vsi_virtual.h"
#include "cpl_conv.h"
#include "cpl_json.h"
#include "cpl_http.h"
#include "cpl_time.h"
#include "ogr_p.h"
#include "ograrrowarrayhelper.h"
#include "ogrlayerarrow.h"
#include "ogr_recordbatch.h"

#include "ogr_flatgeobuf.h"
#include "cplerrors.h"
#include "geometryreader.h"
#include "geometrywriter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

using namespace flatbuffers;
using namespace FlatGeobuf;
using namespace ogr_flatgeobuf;

static OGRErr CPLErrorMemoryAllocation(const char *message)
{
    CPLError(CE_Failure, CPLE_AppDefined, "Could not allocate memory: %s",
             message);
    return OGRERR_NOT_ENOUGH_MEMORY;
}

static OGRErr CPLErrorIO(const char *message)
{
    CPLError(CE_Failure, CPLE_AppDefined, "Unexpected I/O failure: %s",
             message);
    return OGRERR_FAILURE;
}

OGRFlatGeobufLayer::OGRFlatGeobufLayer(const Header *poHeader, GByte *headerBuf,
                                       const char *pszFilename, VSILFILE *poFp,
                                       uint64_t offset)
{
    m_poHeader = poHeader;
    CPLAssert(poHeader);
    m_headerBuf = headerBuf;
    CPLAssert(pszFilename);
    if (pszFilename)
        m_osFilename = pszFilename;
    m_poFp = poFp;
    m_offsetFeatures = offset;
    m_offset = offset;
    m_create = false;

    m_featuresCount = m_poHeader->features_count();
    m_geometryType = m_poHeader->geometry_type();
    m_indexNodeSize = m_poHeader->index_node_size();
    m_hasZ = m_poHeader->has_z();
    m_hasM = m_poHeader->has_m();
    m_hasT = m_poHeader->has_t();
    const auto envelope = m_poHeader->envelope();
    if (envelope && envelope->size() == 4 && std::isfinite((*envelope)[0]) &&
        std::isfinite((*envelope)[1]) && std::isfinite((*envelope)[2]) &&
        std::isfinite((*envelope)[3]))
    {
        m_sExtent.MinX = (*envelope)[0];
        m_sExtent.MinY = (*envelope)[1];
        m_sExtent.MaxX = (*envelope)[2];
        m_sExtent.MaxY = (*envelope)[3];
    }

    CPLDebugOnly("FlatGeobuf", "geometryType: %d, hasZ: %d, hasM: %d, hasT: %d",
                 (int)m_geometryType, m_hasZ, m_hasM, m_hasT);

    const auto crs = m_poHeader->crs();
    if (crs != nullptr)
    {
        m_poSRS = new OGRSpatialReference();
        m_poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        const auto org = crs->org();
        const auto code = crs->code();
        const auto crs_wkt = crs->wkt();
        CPLString wkt = crs_wkt ? crs_wkt->c_str() : "";
        double dfCoordEpoch = 0;
        if (STARTS_WITH_CI(wkt.c_str(), "COORDINATEMETADATA["))
        {
            size_t nPos = std::string::npos;
            // We don't want to match FRAMEEPOCH[
            for (const char *pszEpoch :
                 {",EPOCH[", " EPOCH[", "\tEPOCH[", "\nEPOCH[", "\rEPOCH["})
            {
                nPos = wkt.ifind(pszEpoch);
                if (nPos != std::string::npos)
                    break;
            }
            if (nPos != std::string::npos)
            {
                dfCoordEpoch = CPLAtof(wkt.c_str() + nPos + strlen(",EPOCH["));
                wkt.resize(nPos);
                wkt = wkt.substr(strlen("COORDINATEMETADATA["));
            }
        }

        if ((org == nullptr || EQUAL(org->c_str(), "EPSG")) && code != 0)
        {
            m_poSRS->importFromEPSG(code);
        }
        else if (org && code != 0)
        {
            CPLString osCode;
            osCode.Printf("%s:%d", org->c_str(), code);
            if (m_poSRS->SetFromUserInput(
                    osCode.c_str(),
                    OGRSpatialReference::
                        SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE &&
                !wkt.empty())
            {
                m_poSRS->importFromWkt(wkt.c_str());
            }
        }
        else if (!wkt.empty())
        {
            m_poSRS->importFromWkt(wkt.c_str());
        }

        if (dfCoordEpoch > 0)
            m_poSRS->SetCoordinateEpoch(dfCoordEpoch);
    }

    m_eGType = getOGRwkbGeometryType();

    if (const auto title = poHeader->title())
        SetMetadataItem("TITLE", title->c_str());

    if (const auto description = poHeader->description())
        SetMetadataItem("DESCRIPTION", description->c_str());

    if (const auto metadata = poHeader->metadata())
    {
        CPLJSONDocument oDoc;
        CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
        if (oDoc.LoadMemory(metadata->c_str()) &&
            oDoc.GetRoot().GetType() == CPLJSONObject::Type::Object)
        {
            for (const auto &oItem : oDoc.GetRoot().GetChildren())
            {
                if (oItem.GetType() == CPLJSONObject::Type::String)
                {
                    SetMetadataItem(oItem.GetName().c_str(),
                                    oItem.ToString().c_str());
                }
            }
        }
    }

    const char *pszName =
        m_poHeader->name() ? m_poHeader->name()->c_str() : "unknown";
    m_poFeatureDefn = new OGRFeatureDefn(pszName);
    SetDescription(m_poFeatureDefn->GetName());
    m_poFeatureDefn->SetGeomType(wkbNone);
    auto poGeomFieldDefn =
        std::make_unique<OGRGeomFieldDefn>(nullptr, m_eGType);
    if (m_poSRS != nullptr)
        poGeomFieldDefn->SetSpatialRef(m_poSRS);
    m_poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
    readColumns();
    m_poFeatureDefn->Reference();
}

OGRFlatGeobufLayer::OGRFlatGeobufLayer(
    GDALDataset *poDS, const char *pszLayerName, const char *pszFilename,
    const OGRSpatialReference *poSpatialRef, OGRwkbGeometryType eGType,
    bool bCreateSpatialIndexAtClose, VSILFILE *poFpWrite,
    std::string &osTempFile, CSLConstList papszOptions)
    : m_eGType(eGType), m_poDS(poDS), m_create(true),
      m_bCreateSpatialIndexAtClose(bCreateSpatialIndexAtClose),
      m_poFpWrite(poFpWrite), m_aosCreationOption(papszOptions),
      m_osTempFile(osTempFile)
{
    if (pszLayerName)
        m_osLayerName = pszLayerName;
    if (pszFilename)
        m_osFilename = pszFilename;
    m_geometryType = GeometryWriter::translateOGRwkbGeometryType(eGType);
    if wkbHasZ (eGType)
        m_hasZ = true;
    if wkbHasM (eGType)
        m_hasM = true;
    if (poSpatialRef)
        m_poSRS = poSpatialRef->Clone();

    CPLDebugOnly("FlatGeobuf", "geometryType: %d, hasZ: %d, hasM: %d, hasT: %d",
                 (int)m_geometryType, m_hasZ, m_hasM, m_hasT);

    SetMetadataItem(OLMD_FID64, "YES");

    m_poFeatureDefn = new OGRFeatureDefn(pszLayerName);
    SetDescription(m_poFeatureDefn->GetName());
    m_poFeatureDefn->SetGeomType(eGType);
    m_poFeatureDefn->Reference();
}

OGRwkbGeometryType OGRFlatGeobufLayer::getOGRwkbGeometryType()
{
    OGRwkbGeometryType ogrType = OGRwkbGeometryType::wkbUnknown;
    if (static_cast<int>(m_geometryType) <= 17)
        ogrType = (OGRwkbGeometryType)m_geometryType;
    if (m_hasZ)
        ogrType = wkbSetZ(ogrType);
    if (m_hasM)
        ogrType = wkbSetM(ogrType);
    return ogrType;
}

static ColumnType toColumnType(const char *pszFieldName, OGRFieldType type,
                               OGRFieldSubType subType)
{
    switch (type)
    {
        case OGRFieldType::OFTInteger:
            return subType == OFSTBoolean ? ColumnType::Bool
                   : subType == OFSTInt16 ? ColumnType::Short
                                          : ColumnType::Int;
        case OGRFieldType::OFTInteger64:
            return ColumnType::Long;
        case OGRFieldType::OFTReal:
            return subType == OFSTFloat32 ? ColumnType::Float
                                          : ColumnType::Double;
        case OGRFieldType::OFTString:
            return ColumnType::String;
        case OGRFieldType::OFTDate:
            return ColumnType::DateTime;
        case OGRFieldType::OFTTime:
            return ColumnType::DateTime;
        case OGRFieldType::OFTDateTime:
            return ColumnType::DateTime;
        case OGRFieldType::OFTBinary:
            return ColumnType::Binary;
        default:
            CPLError(CE_Warning, CPLE_AppDefined,
                     "toColumnType: %s field is of type %s, which is not "
                     "handled natively. Falling back to String.",
                     pszFieldName, OGRFieldDefn::GetFieldTypeName(type));
    }
    return ColumnType::String;
}

static OGRFieldType toOGRFieldType(ColumnType type, OGRFieldSubType &eSubType)
{
    eSubType = OFSTNone;
    switch (type)
    {
        case ColumnType::Byte:
            return OGRFieldType::OFTInteger;
        case ColumnType::UByte:
            return OGRFieldType::OFTInteger;
        case ColumnType::Bool:
            eSubType = OFSTBoolean;
            return OGRFieldType::OFTInteger;
        case ColumnType::Short:
            eSubType = OFSTInt16;
            return OGRFieldType::OFTInteger;
        case ColumnType::UShort:
            return OGRFieldType::OFTInteger;
        case ColumnType::Int:
            return OGRFieldType::OFTInteger;
        case ColumnType::UInt:
            return OGRFieldType::OFTInteger64;
        case ColumnType::Long:
            return OGRFieldType::OFTInteger64;
        case ColumnType::ULong:
            return OGRFieldType::OFTReal;
        case ColumnType::Float:
            eSubType = OFSTFloat32;
            return OGRFieldType::OFTReal;
        case ColumnType::Double:
            return OGRFieldType::OFTReal;
        case ColumnType::String:
            return OGRFieldType::OFTString;
        case ColumnType::Json:
            return OGRFieldType::OFTString;
        case ColumnType::DateTime:
            return OGRFieldType::OFTDateTime;
        case ColumnType::Binary:
            return OGRFieldType::OFTBinary;
    }
    return OGRFieldType::OFTString;
}

const std::vector<Offset<Column>>
OGRFlatGeobufLayer::writeColumns(FlatBufferBuilder &fbb)
{
    std::vector<Offset<Column>> columns;
    for (int i = 0; i < m_poFeatureDefn->GetFieldCount(); i++)
    {
        const auto field = m_poFeatureDefn->GetFieldDefn(i);
        const auto name = field->GetNameRef();
        const auto columnType =
            toColumnType(name, field->GetType(), field->GetSubType());
        auto title = field->GetAlternativeNameRef();
        if (EQUAL(title, ""))
            title = nullptr;
        const std::string &osComment = field->GetComment();
        const char *description =
            !osComment.empty() ? osComment.c_str() : nullptr;
        auto width = -1;
        auto precision = -1;
        auto scale = field->GetPrecision();
        if (scale == 0)
            scale = -1;
        if (columnType == ColumnType::Float || columnType == ColumnType::Double)
            precision = field->GetWidth();
        else
            width = field->GetWidth();
        auto nullable = CPL_TO_BOOL(field->IsNullable());
        auto unique = CPL_TO_BOOL(field->IsUnique());
        auto primaryKey = false;
        // CPLDebugOnly("FlatGeobuf", "Create column %s (index %d)", name, i);
        const auto column =
            CreateColumnDirect(fbb, name, columnType, title, description, width,
                               precision, scale, nullable, unique, primaryKey);
        columns.push_back(column);
        // CPLDebugOnly("FlatGeobuf", "DEBUG writeColumns: Created column %s
        // added as index %d", name, i);
    }
    CPLDebugOnly("FlatGeobuf", "Created %lu columns for writing",
                 static_cast<long unsigned int>(columns.size()));
    return columns;
}

void OGRFlatGeobufLayer::readColumns()
{
    const auto columns = m_poHeader->columns();
    if (columns == nullptr)
        return;
    for (uint32_t i = 0; i < columns->size(); i++)
    {
        const auto column = columns->Get(i);
        const auto type = column->type();
        const auto name = column->name()->c_str();
        const auto title =
            column->title() != nullptr ? column->title()->c_str() : nullptr;
        const auto width = column->width();
        const auto precision = column->precision();
        const auto scale = column->scale();
        const auto nullable = column->nullable();
        const auto unique = column->unique();
        OGRFieldSubType eSubType = OFSTNone;
        const auto ogrType = toOGRFieldType(column->type(), eSubType);
        OGRFieldDefn field(name, ogrType);
        field.SetSubType(eSubType);
        field.SetAlternativeName(title);
        if (column->description())
            field.SetComment(column->description()->str());
        if (width != -1 && type != ColumnType::Float &&
            type != ColumnType::Double)
            field.SetWidth(width);
        if (precision != -1)
            field.SetWidth(precision);
        field.SetPrecision(scale != -1 ? scale : 0);
        field.SetNullable(nullable);
        field.SetUnique(unique);
        m_poFeatureDefn->AddFieldDefn(&field);
        // CPLDebugOnly("FlatGeobuf", "DEBUG readColumns: Read column %s added
        // as index %d", name, i);
    }
    CPLDebugOnly("FlatGeobuf",
                 "Read %lu columns and added to feature definition",
                 static_cast<long unsigned int>(columns->size()));
}

void OGRFlatGeobufLayer::writeHeader(VSILFILE *poFp, uint64_t featuresCount,
                                     std::vector<double> *extentVector)
{
    size_t c;
    c = VSIFWriteL(&magicbytes, sizeof(magicbytes), 1, poFp);
    CPLDebugOnly("FlatGeobuf", "Wrote magicbytes (%lu bytes)",
                 static_cast<long unsigned int>(c * sizeof(magicbytes)));
    m_writeOffset += sizeof(magicbytes);

    FlatBufferBuilder fbb;
    fbb.TrackMinAlign(8);
    auto columns = writeColumns(fbb);

    flatbuffers::Offset<Crs> crs = 0;
    if (m_poSRS)
    {
        int nAuthorityCode = 0;
        const char *pszAuthorityName = m_poSRS->GetAuthorityName(nullptr);
        if (pszAuthorityName == nullptr || strlen(pszAuthorityName) == 0)
        {
            // Try to force identify an EPSG code.
            m_poSRS->AutoIdentifyEPSG();

            pszAuthorityName = m_poSRS->GetAuthorityName(nullptr);
            if (pszAuthorityName != nullptr && EQUAL(pszAuthorityName, "EPSG"))
            {
                const char *pszAuthorityCode =
                    m_poSRS->GetAuthorityCode(nullptr);
                if (pszAuthorityCode != nullptr && strlen(pszAuthorityCode) > 0)
                {
                    /* Import 'clean' SRS */
                    m_poSRS->importFromEPSG(atoi(pszAuthorityCode));

                    pszAuthorityName = m_poSRS->GetAuthorityName(nullptr);
                }
            }
        }
        if (pszAuthorityName != nullptr && strlen(pszAuthorityName) > 0)
        {
            // For the root authority name 'EPSG', the authority code
            // should always be integral
            nAuthorityCode = atoi(m_poSRS->GetAuthorityCode(nullptr));
        }

        // Translate SRS to WKT.
        char *pszWKT = nullptr;
        const char *const apszOptionsWkt[] = {"FORMAT=WKT2_2019", nullptr};
        m_poSRS->exportToWkt(&pszWKT, apszOptionsWkt);
        if (pszWKT && pszWKT[0] == '\0')
        {
            CPLFree(pszWKT);
            pszWKT = nullptr;
        }

        if (pszWKT && m_poSRS->GetCoordinateEpoch() > 0)
        {
            std::string osCoordinateEpoch =
                CPLSPrintf("%f", m_poSRS->GetCoordinateEpoch());
            if (osCoordinateEpoch.find('.') != std::string::npos)
            {
                while (osCoordinateEpoch.back() == '0')
                    osCoordinateEpoch.pop_back();
            }

            std::string osWKT("COORDINATEMETADATA[");
            osWKT += pszWKT;
            osWKT += ",EPOCH[";
            osWKT += osCoordinateEpoch;
            osWKT += "]]";
            CPLFree(pszWKT);
            pszWKT = CPLStrdup(osWKT.c_str());
        }

        if (pszWKT && !CPLIsUTF8(pszWKT, -1))
        {
            char *pszWKTtmp = CPLForceToASCII(pszWKT, -1, '?');
            CPLFree(pszWKT);
            pszWKT = pszWKTtmp;
        }
        crs = CreateCrsDirect(fbb, pszAuthorityName, nAuthorityCode,
                              m_poSRS->GetName(), nullptr, pszWKT);
        CPLFree(pszWKT);
    }

    std::string osTitle(m_aosCreationOption.FetchNameValueDef("TITLE", ""));
    std::string osDescription(
        m_aosCreationOption.FetchNameValueDef("DESCRIPTION", ""));
    std::string osMetadata;
    CPLJSONObject oMetadataJSONObj;
    bool bEmptyMetadata = true;
    for (GDALMajorObject *poContainer :
         {static_cast<GDALMajorObject *>(this),
          static_cast<GDALMajorObject *>(
              m_poDS && m_poDS->GetLayerCount() == 1 ? m_poDS : nullptr)})
    {
        if (poContainer)
        {
            if (char **papszMD = poContainer->GetMetadata())
            {
                for (CSLConstList papszIter = papszMD; *papszIter; ++papszIter)
                {
                    char *pszKey = nullptr;
                    const char *pszValue =
                        CPLParseNameValue(*papszIter, &pszKey);
                    if (pszKey && pszValue && !EQUAL(pszKey, OLMD_FID64))
                    {
                        if (EQUAL(pszKey, "TITLE"))
                        {
                            if (osTitle.empty())
                                osTitle = pszValue;
                        }
                        else if (EQUAL(pszKey, "DESCRIPTION"))
                        {
                            if (osDescription.empty())
                                osDescription = pszValue;
                        }
                        else
                        {
                            bEmptyMetadata = false;
                            oMetadataJSONObj.Add(pszKey, pszValue);
                        }
                    }
                    CPLFree(pszKey);
                }
            }
        }
    }
    if (!bEmptyMetadata)
    {
        osMetadata =
            oMetadataJSONObj.Format(CPLJSONObject::PrettyFormat::Plain);
    }

    const auto header = CreateHeaderDirect(
        fbb, m_osLayerName.c_str(), extentVector, m_geometryType, m_hasZ,
        m_hasM, m_hasT, m_hasTM, &columns, featuresCount, m_indexNodeSize, crs,
        osTitle.empty() ? nullptr : osTitle.c_str(),
        osDescription.empty() ? nullptr : osDescription.c_str(),
        osMetadata.empty() ? nullptr : osMetadata.c_str());
    fbb.FinishSizePrefixed(header);
    c = VSIFWriteL(fbb.GetBufferPointer(), 1, fbb.GetSize(), poFp);
    CPLDebugOnly("FlatGeobuf", "Wrote header (%lu bytes)",
                 static_cast<long unsigned int>(c));
    m_writeOffset += c;
}

static bool SupportsSeekWhileWriting(const std::string &osFilename)
{
    return (!STARTS_WITH(osFilename.c_str(), "/vsi")) ||
           STARTS_WITH(osFilename.c_str(), "/vsimem/");
}

bool OGRFlatGeobufLayer::CreateFinalFile()
{
    // no spatial index requested, we are (almost) done
    if (!m_bCreateSpatialIndexAtClose)
    {
        if (m_poFpWrite == nullptr || !SupportsSeekWhileWriting(m_osFilename))
        {
            return true;
        }

        // Rewrite header
        VSIFSeekL(m_poFpWrite, 0, SEEK_SET);
        m_writeOffset = 0;
        std::vector<double> extentVector;
        if (!m_sExtent.IsInit())
        {
            extentVector.resize(4, std::numeric_limits<double>::quiet_NaN());
        }
        else
        {
            extentVector.push_back(m_sExtent.MinX);
            extentVector.push_back(m_sExtent.MinY);
            extentVector.push_back(m_sExtent.MaxX);
            extentVector.push_back(m_sExtent.MaxY);
        }
        writeHeader(m_poFpWrite, m_featuresCount, &extentVector);
        // Sanity check to verify that the dummy header and the real header
        // have the same size.
        if (m_featuresCount)
        {
            CPLAssert(m_writeOffset == m_offsetAfterHeader);
        }
        CPL_IGNORE_RET_VAL(m_writeOffset);  // otherwise checkers might tell the
                                            // member is not used
        return true;
    }

    m_poFp = VSIFOpenL(m_osFilename.c_str(), "wb");
    if (m_poFp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "Failed to create %s:\n%s",
                 m_osFilename.c_str(), VSIStrerror(errno));
        return false;
    }

    // check if something has been written, if not write empty layer and bail
    if (m_writeOffset == 0 || m_featuresCount == 0)
    {
        CPLDebugOnly("FlatGeobuf", "Writing empty layer");
        writeHeader(m_poFp, 0, nullptr);
        return true;
    }

    CPLDebugOnly("FlatGeobuf", "Writing second pass sorted by spatial index");

    const uint64_t nTempFileSize = m_writeOffset;
    m_writeOffset = 0;
    m_indexNodeSize = 16;

    size_t c;

    if (m_featuresCount >= std::numeric_limits<size_t>::max() / 8)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Too many features for this architecture");
        return false;
    }

    NodeItem extent = calcExtent(m_featureItems);
    auto extentVector = extent.toVector();

    writeHeader(m_poFp, m_featuresCount, &extentVector);

    CPLDebugOnly("FlatGeobuf", "Sorting items for Packed R-tree");
    hilbertSort(m_featureItems);
    CPLDebugOnly("FlatGeobuf", "Calc new feature offsets");
    uint64_t featureOffset = 0;
    for (auto &item : m_featureItems)
    {
        item.nodeItem.offset = featureOffset;
        featureOffset += item.size;
    }
    CPLDebugOnly("FlatGeobuf", "Creating Packed R-tree");
    c = 0;
    try
    {
        const auto fillNodeItems = [this](NodeItem *dest)
        {
            size_t i = 0;
            for (const auto &featureItem : m_featureItems)
            {
                dest[i] = featureItem.nodeItem;
                ++i;
            }
        };
        PackedRTree tree(fillNodeItems, m_featureItems.size(), extent);
        CPLDebugOnly("FlatGeobuf", "PackedRTree extent %f, %f, %f, %f",
                     extentVector[0], extentVector[1], extentVector[2],
                     extentVector[3]);
        tree.streamWrite([this, &c](uint8_t *data, size_t size)
                         { c += VSIFWriteL(data, 1, size, m_poFp); });
    }
    catch (const std::exception &e)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Create: %s", e.what());
        return false;
    }
    CPLDebugOnly("FlatGeobuf", "Wrote tree (%lu bytes)",
                 static_cast<long unsigned int>(c));
    m_writeOffset += c;

    CPLDebugOnly("FlatGeobuf", "Writing feature buffers at offset %lu",
                 static_cast<long unsigned int>(m_writeOffset));

    c = 0;

    // For temporary files not in memory, we use a batch strategy to write the
    // final file. That is to say we try to separate reads in the source
    // temporary file and writes in the target file as much as possible, and by
    // reading source features in increasing offset within a batch.
    const bool bUseBatchStrategy =
        !STARTS_WITH(m_osTempFile.c_str(), "/vsimem/");
    if (bUseBatchStrategy)
    {
        const uint32_t nMaxBufferSize = std::max(
            m_maxFeatureSize,
            static_cast<uint32_t>(std::min(
                static_cast<uint64_t>(100 * 1024 * 1024), nTempFileSize)));
        if (ensureFeatureBuf(nMaxBufferSize) != OGRERR_NONE)
            return false;
        uint32_t offsetInBuffer = 0;

        struct BatchItem
        {
            size_t featureIdx;  // index of m_featureItems[]
            uint32_t offsetInBuffer;
        };

        std::vector<BatchItem> batch;

        const auto flushBatch = [this, &batch, &offsetInBuffer]()
        {
            // Sort by increasing source offset
            std::sort(batch.begin(), batch.end(),
                      [this](const BatchItem &a, const BatchItem &b)
                      {
                          return m_featureItems[a.featureIdx].offset <
                                 m_featureItems[b.featureIdx].offset;
                      });

            // Read source features
            for (const auto &batchItem : batch)
            {
                const auto &item = m_featureItems[batchItem.featureIdx];
                if (VSIFSeekL(m_poFpWrite, item.offset, SEEK_SET) == -1)
                {
                    CPLErrorIO("seeking to temp feature location");
                    return false;
                }
                if (VSIFReadL(m_featureBuf + batchItem.offsetInBuffer, 1,
                              item.size, m_poFpWrite) != item.size)
                {
                    CPLErrorIO("reading temp feature");
                    return false;
                }
            }

            // Write target features
            if (offsetInBuffer > 0 &&
                VSIFWriteL(m_featureBuf, 1, offsetInBuffer, m_poFp) !=
                    offsetInBuffer)
            {
                CPLErrorIO("writing feature");
                return false;
            }

            batch.clear();
            offsetInBuffer = 0;
            return true;
        };

        for (size_t i = 0; i < m_featuresCount; i++)
        {
            const auto &featureItem = m_featureItems[i];
            const auto featureSize = featureItem.size;

            if (offsetInBuffer + featureSize > m_featureBufSize)
            {
                if (!flushBatch())
                {
                    return false;
                }
            }

            BatchItem bachItem;
            bachItem.offsetInBuffer = offsetInBuffer;
            bachItem.featureIdx = i;
            batch.emplace_back(bachItem);
            offsetInBuffer += featureSize;
            c += featureSize;
        }

        if (!flushBatch())
        {
            return false;
        }
    }
    else
    {
        const auto err = ensureFeatureBuf(m_maxFeatureSize);
        if (err != OGRERR_NONE)
            return false;

        for (const auto &featureItem : m_featureItems)
        {
            const auto featureSize = featureItem.size;

            // CPLDebugOnly("FlatGeobuf", "featureItem.offset: %lu",
            // static_cast<long unsigned int>(featureItem.offset));
            // CPLDebugOnly("FlatGeobuf", "featureSize: %d", featureSize);
            if (VSIFSeekL(m_poFpWrite, featureItem.offset, SEEK_SET) == -1)
            {
                CPLErrorIO("seeking to temp feature location");
                return false;
            }
            if (VSIFReadL(m_featureBuf, 1, featureSize, m_poFpWrite) !=
                featureSize)
            {
                CPLErrorIO("reading temp feature");
                return false;
            }
            if (VSIFWriteL(m_featureBuf, 1, featureSize, m_poFp) != featureSize)
            {
                CPLErrorIO("writing feature");
                return false;
            }
            c += featureSize;
        }
    }

    CPLDebugOnly("FlatGeobuf", "Wrote feature buffers (%lu bytes)",
                 static_cast<long unsigned int>(c));
    m_writeOffset += c;

    CPLDebugOnly("FlatGeobuf", "Now at offset %lu",
                 static_cast<long unsigned int>(m_writeOffset));

    return true;
}

OGRFlatGeobufLayer::~OGRFlatGeobufLayer()
{
    OGRFlatGeobufLayer::Close();

    if (m_poFeatureDefn)
        m_poFeatureDefn->Release();

    if (m_poSRS)
        m_poSRS->Release();

    if (m_featureBuf)
        VSIFree(m_featureBuf);

    if (m_headerBuf)
        VSIFree(m_headerBuf);
}

CPLErr OGRFlatGeobufLayer::Close()
{
    CPLErr eErr = CE_None;

    if (m_create)
    {
        if (!CreateFinalFile())
            eErr = CE_Failure;
        m_create = false;
    }

    if (m_poFp)
    {
        if (VSIFCloseL(m_poFp) != 0)
            eErr = CE_Failure;
        m_poFp = nullptr;
    }

    if (m_poFpWrite)
    {
        if (VSIFCloseL(m_poFpWrite) != 0)
            eErr = CE_Failure;
        m_poFpWrite = nullptr;
    }

    if (!m_osTempFile.empty())
    {
        VSIUnlink(m_osTempFile.c_str());
        m_osTempFile.clear();
    }

    return eErr;
}

OGRErr OGRFlatGeobufLayer::readFeatureOffset(uint64_t index,
                                             uint64_t &featureOffset)
{
    try
    {
        const auto treeSize =
            PackedRTree::size(m_featuresCount, m_indexNodeSize);
        const auto levelBounds =
            PackedRTree::generateLevelBounds(m_featuresCount, m_indexNodeSize);
        const auto bottomLevelOffset =
            m_offset - treeSize +
            (levelBounds.front().first * sizeof(NodeItem));
        const auto nodeItemOffset =
            bottomLevelOffset + (index * sizeof(NodeItem));
        const auto featureOffsetOffset = nodeItemOffset + (sizeof(double) * 4);
        if (VSIFSeekL(m_poFp, featureOffsetOffset, SEEK_SET) == -1)
            return CPLErrorIO("seeking feature offset");
        if (VSIFReadL(&featureOffset, sizeof(uint64_t), 1, m_poFp) != 1)
            return CPLErrorIO("reading feature offset");
#if !CPL_IS_LSB
        CPL_LSBPTR64(&featureOffset);
#endif
        return OGRERR_NONE;
    }
    catch (const std::exception &e)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Failed to calculate tree size: %s", e.what());
        return OGRERR_FAILURE;
    }
}

OGRFeature *OGRFlatGeobufLayer::GetFeature(GIntBig nFeatureId)
{
    if (m_indexNodeSize == 0)
    {
        return OGRLayer::GetFeature(nFeatureId);
    }
    else
    {
        if (nFeatureId < 0 ||
            static_cast<uint64_t>(nFeatureId) >= m_featuresCount)
        {
            return nullptr;
        }
        ResetReading();
        m_ignoreSpatialFilter = true;
        m_ignoreAttributeFilter = true;
        uint64_t featureOffset;
        const auto err = readFeatureOffset(nFeatureId, featureOffset);
        if (err != OGRERR_NONE)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unexpected error reading feature offset from id");
            return nullptr;
        }
        m_offset = m_offsetFeatures + featureOffset;
        OGRFeature *poFeature = GetNextFeature();
        if (poFeature != nullptr)
            poFeature->SetFID(nFeatureId);
        ResetReading();
        return poFeature;
    }
}

OGRErr OGRFlatGeobufLayer::readIndex()
{
    if (m_queriedSpatialIndex || !m_poFilterGeom)
        return OGRERR_NONE;
    if (m_sFilterEnvelope.IsInit() && m_sExtent.IsInit() &&
        m_sFilterEnvelope.MinX <= m_sExtent.MinX &&
        m_sFilterEnvelope.MinY <= m_sExtent.MinY &&
        m_sFilterEnvelope.MaxX >= m_sExtent.MaxX &&
        m_sFilterEnvelope.MaxY >= m_sExtent.MaxY)
        return OGRERR_NONE;
    const auto indexNodeSize = m_poHeader->index_node_size();
    if (indexNodeSize == 0)
        return OGRERR_NONE;
    const auto featuresCount = m_poHeader->features_count();
    if (featuresCount == 0)
        return OGRERR_NONE;

    if (VSIFSeekL(m_poFp, sizeof(magicbytes), SEEK_SET) ==
        -1)  // skip magic bytes
        return CPLErrorIO("seeking past magic bytes");
    uoffset_t headerSize;
    if (VSIFReadL(&headerSize, sizeof(uoffset_t), 1, m_poFp) != 1)
        return CPLErrorIO("reading header size");
    CPL_LSBPTR32(&headerSize);

    try
    {
        const auto treeSize =
            indexNodeSize > 0 ? PackedRTree::size(featuresCount) : 0;
        if (treeSize > 0 && m_poFilterGeom && !m_ignoreSpatialFilter)
        {
            CPLDebugOnly("FlatGeobuf", "Attempting spatial index query");
            OGREnvelope env;
            m_poFilterGeom->getEnvelope(&env);
            NodeItem n{env.MinX, env.MinY, env.MaxX, env.MaxY, 0};
            CPLDebugOnly("FlatGeobuf", "Spatial index search on %f,%f,%f,%f",
                         env.MinX, env.MinY, env.MaxX, env.MaxY);
            const auto treeOffset =
                sizeof(magicbytes) + sizeof(uoffset_t) + headerSize;
            const auto readNode =
                [this, treeOffset](uint8_t *buf, size_t i, size_t s)
            {
                if (VSIFSeekL(m_poFp, treeOffset + i, SEEK_SET) == -1)
                    throw std::runtime_error("I/O seek failure");
                if (VSIFReadL(buf, 1, s, m_poFp) != s)
                    throw std::runtime_error("I/O read file");
            };
            m_foundItems = PackedRTree::streamSearch(
                featuresCount, indexNodeSize, n, readNode);
            m_featuresCount = m_foundItems.size();
            CPLDebugOnly("FlatGeobuf",
                         "%lu features found in spatial index search",
                         static_cast<long unsigned int>(m_featuresCount));

            m_queriedSpatialIndex = true;
        }
    }
    catch (const std::exception &e)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "readIndex: Unexpected failure: %s", e.what());
        return OGRERR_FAILURE;
    }

    return OGRERR_NONE;
}

GIntBig OGRFlatGeobufLayer::GetFeatureCount(int bForce)
{
    if (m_poFilterGeom != nullptr || m_poAttrQuery != nullptr ||
        m_featuresCount == 0)
        return OGRLayer::GetFeatureCount(bForce);
    else
        return m_featuresCount;
}

/************************************************************************/
/*                     ParseDateTime()                                  */
/************************************************************************/

static inline bool ParseDateTime(std::string_view sInput, OGRField *psField)
{
    return OGRParseDateTimeYYYYMMDDTHHMMSSZ(sInput, psField) ||
           OGRParseDateTimeYYYYMMDDTHHMMSSsssZ(sInput, psField);
}

OGRFeature *OGRFlatGeobufLayer::GetNextFeature()
{
    if (m_create)
        return nullptr;

    while (true)
    {
        if (m_featuresCount > 0 && m_featuresPos >= m_featuresCount)
        {
            CPLDebugOnly("FlatGeobuf", "GetNextFeature: iteration end at %lu",
                         static_cast<long unsigned int>(m_featuresPos));
            return nullptr;
        }

        if (readIndex() != OGRERR_NONE)
        {
            return nullptr;
        }

        if (m_queriedSpatialIndex && m_featuresCount == 0)
        {
            CPLDebugOnly("FlatGeobuf", "GetNextFeature: no features found");
            return nullptr;
        }

        auto poFeature = std::make_unique<OGRFeature>(m_poFeatureDefn);
        if (parseFeature(poFeature.get()) != OGRERR_NONE)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Fatal error parsing feature");
            return nullptr;
        }

        if (VSIFEofL(m_poFp) || VSIFErrorL(m_poFp))
        {
            CPLDebug("FlatGeobuf", "GetNextFeature: iteration end due to EOF");
            return nullptr;
        }

        m_featuresPos++;

        if ((m_poFilterGeom == nullptr || m_ignoreSpatialFilter ||
             FilterGeometry(poFeature->GetGeometryRef())) &&
            (m_poAttrQuery == nullptr || m_ignoreAttributeFilter ||
             m_poAttrQuery->Evaluate(poFeature.get())))
            return poFeature.release();
    }
}

OGRErr OGRFlatGeobufLayer::ensureFeatureBuf(uint32_t featureSize)
{
    if (m_featureBufSize == 0)
    {
        const auto newBufSize = std::max(1024U * 32U, featureSize);
        CPLDebugOnly("FlatGeobuf", "ensureFeatureBuf: newBufSize: %d",
                     newBufSize);
        m_featureBuf = static_cast<GByte *>(VSIMalloc(newBufSize));
        if (m_featureBuf == nullptr)
            return CPLErrorMemoryAllocation("initial feature buffer");
        m_featureBufSize = newBufSize;
    }
    else if (m_featureBufSize < featureSize)
    {
        // Do not increase this x2 factor without modifying
        // feature_max_buffer_size
        const auto newBufSize = std::max(m_featureBufSize * 2, featureSize);
        CPLDebugOnly("FlatGeobuf", "ensureFeatureBuf: newBufSize: %d",
                     newBufSize);
        const auto featureBuf =
            static_cast<GByte *>(VSIRealloc(m_featureBuf, newBufSize));
        if (featureBuf == nullptr)
            return CPLErrorMemoryAllocation("feature buffer resize");
        m_featureBuf = featureBuf;
        m_featureBufSize = newBufSize;
    }
    return OGRERR_NONE;
}

OGRErr OGRFlatGeobufLayer::parseFeature(OGRFeature *poFeature)
{
    GIntBig fid;
    auto seek = false;
    if (m_queriedSpatialIndex && !m_ignoreSpatialFilter)
    {
        const auto item = m_foundItems[m_featuresPos];
        m_offset = m_offsetFeatures + item.offset;
        fid = item.index;
        seek = true;
    }
    else
    {
        fid = m_featuresPos;
    }
    poFeature->SetFID(fid);

    // CPLDebugOnly("FlatGeobuf", "m_featuresPos: %lu", static_cast<long
    // unsigned int>(m_featuresPos));

    if (m_featuresPos == 0)
        seek = true;

    if (seek && VSIFSeekL(m_poFp, m_offset, SEEK_SET) == -1)
    {
        if (VSIFEofL(m_poFp))
            return OGRERR_NONE;
        return CPLErrorIO("seeking to feature location");
    }
    uint32_t featureSize;
    if (VSIFReadL(&featureSize, sizeof(featureSize), 1, m_poFp) != 1)
    {
        if (VSIFEofL(m_poFp))
            return OGRERR_NONE;
        return CPLErrorIO("reading feature size");
    }
    CPL_LSBPTR32(&featureSize);

    // Sanity check to avoid allocated huge amount of memory on corrupted
    // feature
    if (featureSize > 100 * 1024 * 1024)
    {
        if (featureSize > feature_max_buffer_size)
            return CPLErrorInvalidSize("feature");

        if (m_nFileSize == 0)
        {
            VSIStatBufL sStatBuf;
            if (VSIStatL(m_osFilename.c_str(), &sStatBuf) == 0)
            {
                m_nFileSize = sStatBuf.st_size;
            }
        }
        if (m_offset + featureSize > m_nFileSize)
        {
            return CPLErrorIO("reading feature size");
        }
    }

    const auto err = ensureFeatureBuf(featureSize);
    if (err != OGRERR_NONE)
        return err;
    if (VSIFReadL(m_featureBuf, 1, featureSize, m_poFp) != featureSize)
        return CPLErrorIO("reading feature");
    m_offset += featureSize + sizeof(featureSize);

    if (m_bVerifyBuffers)
    {
        Verifier v(m_featureBuf, featureSize);
        const auto ok = VerifyFeatureBuffer(v);
        if (!ok)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Buffer verification failed");
            CPLDebugOnly("FlatGeobuf", "m_offset: %lu",
                         static_cast<long unsigned int>(m_offset));
            CPLDebugOnly("FlatGeobuf", "m_featuresPos: %lu",
                         static_cast<long unsigned int>(m_featuresPos));
            CPLDebugOnly("FlatGeobuf", "featureSize: %d", featureSize);
            return OGRERR_CORRUPT_DATA;
        }
    }

    const auto feature = GetRoot<Feature>(m_featureBuf);
    const auto geometry = feature->geometry();
    if (!m_poFeatureDefn->IsGeometryIgnored() && geometry != nullptr)
    {
        auto geometryType = m_geometryType;
        if (geometryType == GeometryType::Unknown)
            geometryType = geometry->type();
        OGRGeometry *poOGRGeometry =
            GeometryReader(geometry, geometryType, m_hasZ, m_hasM).read();
        if (poOGRGeometry == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Failed to read geometry");
            return OGRERR_CORRUPT_DATA;
        }
        // #ifdef DEBUG
        //             char *wkt;
        //             poOGRGeometry->exportToWkt(&wkt);
        //             CPLDebugOnly("FlatGeobuf", "readGeometry as wkt: %s",
        //             wkt);
        // #endif
        if (m_poSRS != nullptr)
            poOGRGeometry->assignSpatialReference(m_poSRS);
        poFeature->SetGeometryDirectly(poOGRGeometry);
    }

    const auto properties = feature->properties();
    if (properties != nullptr)
    {
        const auto data = properties->data();
        const auto size = properties->size();

        // CPLDebugOnly("FlatGeobuf", "DEBUG parseFeature: size: %lu",
        // static_cast<long unsigned int>(size));

        // CPLDebugOnly("FlatGeobuf", "properties->size: %d", size);
        uoffset_t offset = 0;
        // size must be at least large enough to contain
        // a single column index and smallest value type
        if (size > 0 && size < (sizeof(uint16_t) + sizeof(uint8_t)))
            return CPLErrorInvalidSize("property value");
        while (offset + 1 < size)
        {
            if (offset + sizeof(uint16_t) > size)
                return CPLErrorInvalidSize("property value");
            uint16_t i = *((uint16_t *)(data + offset));
            CPL_LSBPTR16(&i);
            // CPLDebugOnly("FlatGeobuf", "DEBUG parseFeature: i: %hu", i);
            offset += sizeof(uint16_t);
            // CPLDebugOnly("FlatGeobuf", "DEBUG parseFeature: offset: %du",
            // offset);
            //  TODO: use columns from feature if defined
            const auto columns = m_poHeader->columns();
            if (columns == nullptr)
            {
                CPLErrorInvalidPointer("columns");
                return OGRERR_CORRUPT_DATA;
            }
            if (i >= columns->size())
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Column index %hu out of range", i);
                return OGRERR_CORRUPT_DATA;
            }
            const auto column = columns->Get(i);
            const auto type = column->type();
            const auto isIgnored = poFeature->GetFieldDefnRef(i)->IsIgnored();
            const auto ogrField = poFeature->GetRawFieldRef(i);
            if (!OGR_RawField_IsUnset(ogrField))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Field %d set more than once", i);
                return OGRERR_CORRUPT_DATA;
            }

            switch (type)
            {
                case ColumnType::Bool:
                    if (offset + sizeof(unsigned char) > size)
                        return CPLErrorInvalidSize("bool value");
                    if (!isIgnored)
                    {
                        ogrField->Integer = *(data + offset);
                    }
                    offset += sizeof(unsigned char);
                    break;

                case ColumnType::Byte:
                    if (offset + sizeof(signed char) > size)
                        return CPLErrorInvalidSize("byte value");
                    if (!isIgnored)
                    {
                        ogrField->Integer =
                            *reinterpret_cast<const signed char *>(data +
                                                                   offset);
                    }
                    offset += sizeof(signed char);
                    break;

                case ColumnType::UByte:
                    if (offset + sizeof(unsigned char) > size)
                        return CPLErrorInvalidSize("ubyte value");
                    if (!isIgnored)
                    {
                        ogrField->Integer =
                            *reinterpret_cast<const unsigned char *>(data +
                                                                     offset);
                    }
                    offset += sizeof(unsigned char);
                    break;

                case ColumnType::Short:
                    if (offset + sizeof(int16_t) > size)
                        return CPLErrorInvalidSize("short value");
                    if (!isIgnored)
                    {
                        short s;
                        memcpy(&s, data + offset, sizeof(int16_t));
                        CPL_LSBPTR16(&s);
                        ogrField->Integer = s;
                    }
                    offset += sizeof(int16_t);
                    break;

                case ColumnType::UShort:
                    if (offset + sizeof(uint16_t) > size)
                        return CPLErrorInvalidSize("ushort value");
                    if (!isIgnored)
                    {
                        uint16_t s;
                        memcpy(&s, data + offset, sizeof(uint16_t));
                        CPL_LSBPTR16(&s);
                        ogrField->Integer = s;
                    }
                    offset += sizeof(uint16_t);
                    break;

                case ColumnType::Int:
                    if (offset + sizeof(int32_t) > size)
                        return CPLErrorInvalidSize("int32 value");
                    if (!isIgnored)
                    {
                        memcpy(&ogrField->Integer, data + offset,
                               sizeof(int32_t));
                        CPL_LSBPTR32(&ogrField->Integer);
                    }
                    offset += sizeof(int32_t);
                    break;

                case ColumnType::UInt:
                    if (offset + sizeof(uint32_t) > size)
                        return CPLErrorInvalidSize("uint value");
                    if (!isIgnored)
                    {
                        uint32_t v;
                        memcpy(&v, data + offset, sizeof(int32_t));
                        CPL_LSBPTR32(&v);
                        ogrField->Integer64 = v;
                    }
                    offset += sizeof(int32_t);
                    break;

                case ColumnType::Long:
                    if (offset + sizeof(int64_t) > size)
                        return CPLErrorInvalidSize("int64 value");
                    if (!isIgnored)
                    {
                        memcpy(&ogrField->Integer64, data + offset,
                               sizeof(int64_t));
                        CPL_LSBPTR64(&ogrField->Integer64);
                    }
                    offset += sizeof(int64_t);
                    break;

                case ColumnType::ULong:
                    if (offset + sizeof(uint64_t) > size)
                        return CPLErrorInvalidSize("uint64 value");
                    if (!isIgnored)
                    {
                        uint64_t v;
                        memcpy(&v, data + offset, sizeof(v));
                        CPL_LSBPTR64(&v);
                        ogrField->Real = static_cast<double>(v);
                    }
                    offset += sizeof(int64_t);
                    break;

                case ColumnType::Float:
                    if (offset + sizeof(float) > size)
                        return CPLErrorInvalidSize("float value");
                    if (!isIgnored)
                    {
                        float f;
                        memcpy(&f, data + offset, sizeof(float));
                        CPL_LSBPTR32(&f);
                        ogrField->Real = f;
                    }
                    offset += sizeof(float);
                    break;

                case ColumnType::Double:
                    if (offset + sizeof(double) > size)
                        return CPLErrorInvalidSize("double value");
                    if (!isIgnored)
                    {
                        memcpy(&ogrField->Real, data + offset, sizeof(double));
                        CPL_LSBPTR64(&ogrField->Real);
                    }
                    offset += sizeof(double);
                    break;

                case ColumnType::String:
                case ColumnType::Json:
                {
                    if (offset + sizeof(uint32_t) > size)
                        return CPLErrorInvalidSize("string length");
                    uint32_t len;
                    memcpy(&len, data + offset, sizeof(int32_t));
                    CPL_LSBPTR32(&len);
                    offset += sizeof(uint32_t);
                    if (len > size - offset)
                        return CPLErrorInvalidSize("string value");
                    if (!isIgnored)
                    {
                        char *str =
                            static_cast<char *>(VSI_MALLOC_VERBOSE(len + 1));
                        if (str == nullptr)
                            return CPLErrorMemoryAllocation("string value");
                        memcpy(str, data + offset, len);
                        str[len] = '\0';
                        ogrField->String = str;
                    }
                    offset += len;
                    break;
                }

                case ColumnType::DateTime:
                {
                    if (offset + sizeof(uint32_t) > size)
                        return CPLErrorInvalidSize("datetime length ");
                    uint32_t len;
                    memcpy(&len, data + offset, sizeof(int32_t));
                    CPL_LSBPTR32(&len);
                    offset += sizeof(uint32_t);
                    if (len > size - offset || len > 32)
                        return CPLErrorInvalidSize("datetime value");
                    if (!isIgnored)
                    {
                        if (!ParseDateTime(
                                std::string_view(reinterpret_cast<const char *>(
                                                     data + offset),
                                                 len),
                                ogrField))
                        {
                            char str[32 + 1];
                            memcpy(str, data + offset, len);
                            str[len] = '\0';
                            if (!OGRParseDate(str, ogrField, 0))
                            {
                                OGR_RawField_SetUnset(ogrField);
                            }
                        }
                    }
                    offset += len;
                    break;
                }

                case ColumnType::Binary:
                {
                    if (offset + sizeof(uint32_t) > size)
                        return CPLErrorInvalidSize("binary length");
                    uint32_t len;
                    memcpy(&len, data + offset, sizeof(int32_t));
                    CPL_LSBPTR32(&len);
                    offset += sizeof(uint32_t);
                    if (len > static_cast<uint32_t>(INT_MAX) ||
                        len > size - offset)
                        return CPLErrorInvalidSize("binary value");
                    if (!isIgnored)
                    {
                        GByte *binary = static_cast<GByte *>(
                            VSI_MALLOC_VERBOSE(len ? len : 1));
                        if (binary == nullptr)
                            return CPLErrorMemoryAllocation("string value");
                        memcpy(binary, data + offset, len);
                        ogrField->Binary.nCount = static_cast<int>(len);
                        ogrField->Binary.paData = binary;
                    }
                    offset += len;
                    break;
                }
            }
        }
    }
    return OGRERR_NONE;
}

/************************************************************************/
/*                      GetNextArrowArray()                             */
/************************************************************************/

int OGRFlatGeobufLayer::GetNextArrowArray(struct ArrowArrayStream *stream,
                                          struct ArrowArray *out_array)
{
    if (!m_poSharedArrowArrayStreamPrivateData->m_anQueriedFIDs.empty() ||
        CPLTestBool(
            CPLGetConfigOption("OGR_FLATGEOBUF_STREAM_BASE_IMPL", "NO")))
    {
        return OGRLayer::GetNextArrowArray(stream, out_array);
    }

begin:
    int errorErrno = EIO;
    memset(out_array, 0, sizeof(*out_array));

    if (m_create)
        return EINVAL;

    if (m_bEOF || (m_featuresCount > 0 && m_featuresPos >= m_featuresCount))
    {
        return 0;
    }

    if (readIndex() != OGRERR_NONE)
        return EIO;

    OGRArrowArrayHelper sHelper(
        nullptr,  // dataset pointer. only used for field domains (not used by
                  // FlatGeobuf)
        m_poFeatureDefn, m_aosArrowArrayStreamOptions, out_array);
    if (out_array->release == nullptr)
    {
        return ENOMEM;
    }

    std::vector<bool> abSetFields(sHelper.m_nFieldCount);

    struct tm brokenDown;
    memset(&brokenDown, 0, sizeof(brokenDown));

    int iFeat = 0;
    bool bEOFOrError = true;

    if (m_queriedSpatialIndex && m_featuresCount == 0)
    {
        CPLDebugOnly("FlatGeobuf", "GetNextFeature: no features found");
        sHelper.m_nMaxBatchSize = 0;
    }

    const GIntBig nFeatureIdxStart = m_featuresPos;
    const bool bDateTimeAsString = m_aosArrowArrayStreamOptions.FetchBool(
        GAS_OPT_DATETIME_AS_STRING, false);

    const uint32_t nMemLimit = OGRArrowArrayHelper::GetMemLimit();
    while (iFeat < sHelper.m_nMaxBatchSize)
    {
        bEOFOrError = true;
        if (m_featuresCount > 0 && m_featuresPos >= m_featuresCount)
        {
            CPLDebugOnly("FlatGeobuf", "GetNextFeature: iteration end at %lu",
                         static_cast<long unsigned int>(m_featuresPos));
            break;
        }

        GIntBig fid;
        auto seek = false;
        if (m_queriedSpatialIndex && !m_ignoreSpatialFilter)
        {
            const auto item = m_foundItems[m_featuresPos];
            m_offset = m_offsetFeatures + item.offset;
            fid = item.index;
            seek = true;
        }
        else
        {
            fid = m_featuresPos;
        }

        if (sHelper.m_panFIDValues)
            sHelper.m_panFIDValues[iFeat] = fid;

        if (m_featuresPos == 0)
            seek = true;

        if (seek && VSIFSeekL(m_poFp, m_offset, SEEK_SET) == -1)
        {
            break;
        }
        uint32_t featureSize;
        if (VSIFReadL(&featureSize, sizeof(featureSize), 1, m_poFp) != 1)
        {
            if (VSIFEofL(m_poFp))
                break;
            CPLErrorIO("reading feature size");
            goto error;
        }
        CPL_LSBPTR32(&featureSize);

        // Sanity check to avoid allocated huge amount of memory on corrupted
        // feature
        if (featureSize > 100 * 1024 * 1024)
        {
            if (featureSize > feature_max_buffer_size)
            {
                CPLErrorInvalidSize("feature");
                goto error;
            }

            if (m_nFileSize == 0)
            {
                VSIStatBufL sStatBuf;
                if (VSIStatL(m_osFilename.c_str(), &sStatBuf) == 0)
                {
                    m_nFileSize = sStatBuf.st_size;
                }
            }
            if (m_offset + featureSize > m_nFileSize)
            {
                CPLErrorIO("reading feature size");
                goto error;
            }
        }

        const auto err = ensureFeatureBuf(featureSize);
        if (err != OGRERR_NONE)
            goto error;
        if (VSIFReadL(m_featureBuf, 1, featureSize, m_poFp) != featureSize)
        {
            CPLErrorIO("reading feature");
            goto error;
        }
        m_offset += featureSize + sizeof(featureSize);

        if (m_bVerifyBuffers)
        {
            Verifier v(m_featureBuf, featureSize);
            const auto ok = VerifyFeatureBuffer(v);
            if (!ok)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Buffer verification failed");
                CPLDebugOnly("FlatGeobuf", "m_offset: %lu",
                             static_cast<long unsigned int>(m_offset));
                CPLDebugOnly("FlatGeobuf", "m_featuresPos: %lu",
                             static_cast<long unsigned int>(m_featuresPos));
                CPLDebugOnly("FlatGeobuf", "featureSize: %d", featureSize);
                goto error;
            }
        }

        const auto feature = GetRoot<Feature>(m_featureBuf);
        const auto geometry = feature->geometry();
        const auto properties = feature->properties();
        if (!m_poFeatureDefn->IsGeometryIgnored() && geometry != nullptr)
        {
            auto geometryType = m_geometryType;
            if (geometryType == GeometryType::Unknown)
                geometryType = geometry->type();
            auto poOGRGeometry = std::unique_ptr<OGRGeometry>(
                GeometryReader(geometry, geometryType, m_hasZ, m_hasM).read());
            if (poOGRGeometry == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Failed to read geometry");
                goto error;
            }

            if (!FilterGeometry(poOGRGeometry.get()))
                goto end_of_loop;

            const int iArrowField = sHelper.m_mapOGRGeomFieldToArrowField[0];
            const size_t nWKBSize = poOGRGeometry->WkbSize();

            if (iFeat > 0)
            {
                auto psArray = out_array->children[iArrowField];
                auto panOffsets = static_cast<int32_t *>(
                    const_cast<void *>(psArray->buffers[1]));
                const uint32_t nCurLength =
                    static_cast<uint32_t>(panOffsets[iFeat]);
                if (nWKBSize <= nMemLimit && nWKBSize > nMemLimit - nCurLength)
                {
                    goto after_loop;
                }
            }

            GByte *outPtr =
                sHelper.GetPtrForStringOrBinary(iArrowField, iFeat, nWKBSize);
            if (outPtr == nullptr)
            {
                errorErrno = ENOMEM;
                goto error;
            }
            poOGRGeometry->exportToWkb(wkbNDR, outPtr, wkbVariantIso);
        }

        abSetFields.clear();
        abSetFields.resize(sHelper.m_nFieldCount);

        if (properties != nullptr)
        {
            const auto data = properties->data();
            const auto size = properties->size();

            uoffset_t offset = 0;
            // size must be at least large enough to contain
            // a single column index and smallest value type
            if (size > 0 && size < (sizeof(uint16_t) + sizeof(uint8_t)))
            {
                CPLErrorInvalidSize("property value");
                goto error;
            }

            while (offset + 1 < size)
            {
                if (offset + sizeof(uint16_t) > size)
                {
                    CPLErrorInvalidSize("property value");
                    goto error;
                }
                uint16_t i = *((uint16_t *)(data + offset));
                CPL_LSBPTR16(&i);
                offset += sizeof(uint16_t);
                // TODO: use columns from feature if defined
                const auto columns = m_poHeader->columns();
                if (columns == nullptr)
                {
                    CPLErrorInvalidPointer("columns");
                    goto error;
                }
                if (i >= columns->size())
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Column index %hu out of range", i);
                    goto error;
                }

                abSetFields[i] = true;
                const auto column = columns->Get(i);
                const auto type = column->type();
                const int iArrowField = sHelper.m_mapOGRFieldToArrowField[i];
                const bool isIgnored = iArrowField < 0;
                auto psArray =
                    isIgnored ? nullptr : out_array->children[iArrowField];

                switch (type)
                {
                    case ColumnType::Bool:
                        if (offset + sizeof(unsigned char) > size)
                        {
                            CPLErrorInvalidSize("bool value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            if (*(data + offset))
                            {
                                sHelper.SetBoolOn(psArray, iFeat);
                            }
                        }
                        offset += sizeof(unsigned char);
                        break;

                    case ColumnType::Byte:
                        if (offset + sizeof(signed char) > size)
                        {
                            CPLErrorInvalidSize("byte value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            sHelper.SetInt8(psArray, iFeat,
                                            *reinterpret_cast<const int8_t *>(
                                                data + offset));
                        }
                        offset += sizeof(signed char);
                        break;

                    case ColumnType::UByte:
                        if (offset + sizeof(unsigned char) > size)
                        {
                            CPLErrorInvalidSize("ubyte value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            sHelper.SetUInt8(psArray, iFeat,
                                             *reinterpret_cast<const uint8_t *>(
                                                 data + offset));
                        }
                        offset += sizeof(unsigned char);
                        break;

                    case ColumnType::Short:
                        if (offset + sizeof(int16_t) > size)
                        {
                            CPLErrorInvalidSize("short value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            short s;
                            memcpy(&s, data + offset, sizeof(int16_t));
                            CPL_LSBPTR16(&s);
                            sHelper.SetInt16(psArray, iFeat, s);
                        }
                        offset += sizeof(int16_t);
                        break;

                    case ColumnType::UShort:
                        if (offset + sizeof(uint16_t) > size)
                        {
                            CPLErrorInvalidSize("ushort value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            uint16_t s;
                            memcpy(&s, data + offset, sizeof(uint16_t));
                            CPL_LSBPTR16(&s);
                            sHelper.SetInt32(psArray, iFeat, s);
                        }
                        offset += sizeof(uint16_t);
                        break;

                    case ColumnType::Int:
                        if (offset + sizeof(int32_t) > size)
                        {
                            CPLErrorInvalidSize("int32 value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            int32_t nVal;
                            memcpy(&nVal, data + offset, sizeof(int32_t));
                            CPL_LSBPTR32(&nVal);
                            sHelper.SetInt32(psArray, iFeat, nVal);
                        }
                        offset += sizeof(int32_t);
                        break;

                    case ColumnType::UInt:
                        if (offset + sizeof(uint32_t) > size)
                        {
                            CPLErrorInvalidSize("uint value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            uint32_t v;
                            memcpy(&v, data + offset, sizeof(int32_t));
                            CPL_LSBPTR32(&v);
                            sHelper.SetInt64(psArray, iFeat, v);
                        }
                        offset += sizeof(int32_t);
                        break;

                    case ColumnType::Long:
                        if (offset + sizeof(int64_t) > size)
                        {
                            CPLErrorInvalidSize("int64 value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            int64_t v;
                            memcpy(&v, data + offset, sizeof(int64_t));
                            CPL_LSBPTR64(&v);
                            sHelper.SetInt64(psArray, iFeat, v);
                        }
                        offset += sizeof(int64_t);
                        break;

                    case ColumnType::ULong:
                        if (offset + sizeof(uint64_t) > size)
                        {
                            CPLErrorInvalidSize("uint64 value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            uint64_t v;
                            memcpy(&v, data + offset, sizeof(v));
                            CPL_LSBPTR64(&v);
                            sHelper.SetDouble(psArray, iFeat,
                                              static_cast<double>(v));
                        }
                        offset += sizeof(int64_t);
                        break;

                    case ColumnType::Float:
                        if (offset + sizeof(float) > size)
                        {
                            CPLErrorInvalidSize("float value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            float f;
                            memcpy(&f, data + offset, sizeof(float));
                            CPL_LSBPTR32(&f);
                            sHelper.SetFloat(psArray, iFeat, f);
                        }
                        offset += sizeof(float);
                        break;

                    case ColumnType::Double:
                        if (offset + sizeof(double) > size)
                        {
                            CPLErrorInvalidSize("double value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            double v;
                            memcpy(&v, data + offset, sizeof(double));
                            CPL_LSBPTR64(&v);
                            sHelper.SetDouble(psArray, iFeat, v);
                        }
                        offset += sizeof(double);
                        break;

                    case ColumnType::DateTime:
                    {
                        if (!bDateTimeAsString)
                        {
                            if (offset + sizeof(uint32_t) > size)
                            {
                                CPLErrorInvalidSize("datetime length ");
                                goto error;
                            }
                            uint32_t len;
                            memcpy(&len, data + offset, sizeof(int32_t));
                            CPL_LSBPTR32(&len);
                            offset += sizeof(uint32_t);
                            if (len > size - offset || len > 32)
                            {
                                CPLErrorInvalidSize("datetime value");
                                goto error;
                            }
                            if (!isIgnored)
                            {
                                OGRField ogrField;
                                if (ParseDateTime(
                                        std::string_view(
                                            reinterpret_cast<const char *>(
                                                data + offset),
                                            len),
                                        &ogrField))
                                {
                                    sHelper.SetDateTime(
                                        psArray, iFeat, brokenDown,
                                        sHelper.m_anTZFlags[i], ogrField);
                                }
                                else
                                {
                                    char str[32 + 1];
                                    memcpy(str, data + offset, len);
                                    str[len] = '\0';
                                    if (OGRParseDate(str, &ogrField, 0))
                                    {
                                        sHelper.SetDateTime(
                                            psArray, iFeat, brokenDown,
                                            sHelper.m_anTZFlags[i], ogrField);
                                    }
                                }
                            }
                            offset += len;
                            break;
                        }
                        else
                        {
                            [[fallthrough]];
                        }
                    }

                    case ColumnType::String:
                    case ColumnType::Json:
                    case ColumnType::Binary:
                    {
                        if (offset + sizeof(uint32_t) > size)
                        {
                            CPLErrorInvalidSize("string length");
                            goto error;
                        }
                        uint32_t len;
                        memcpy(&len, data + offset, sizeof(int32_t));
                        CPL_LSBPTR32(&len);
                        offset += sizeof(uint32_t);
                        if (len > size - offset)
                        {
                            CPLErrorInvalidSize("string value");
                            goto error;
                        }
                        if (!isIgnored)
                        {
                            if (iFeat > 0)
                            {
                                auto panOffsets = static_cast<int32_t *>(
                                    const_cast<void *>(psArray->buffers[1]));
                                const uint32_t nCurLength =
                                    static_cast<uint32_t>(panOffsets[iFeat]);
                                if (len <= nMemLimit &&
                                    len > nMemLimit - nCurLength)
                                {
                                    goto after_loop;
                                }
                            }

                            GByte *outPtr = sHelper.GetPtrForStringOrBinary(
                                iArrowField, iFeat, len);
                            if (outPtr == nullptr)
                            {
                                errorErrno = ENOMEM;
                                goto error;
                            }
                            memcpy(outPtr, data + offset, len);
                        }
                        offset += len;
                        break;
                    }
                }
            }
        }

        // Mark null fields
        for (int i = 0; i < sHelper.m_nFieldCount; i++)
        {
            if (!abSetFields[i] && sHelper.m_abNullableFields[i])
            {
                const int iArrowField = sHelper.m_mapOGRFieldToArrowField[i];
                if (iArrowField >= 0)
                {
                    sHelper.SetNull(iArrowField, iFeat);
                }
            }
        }

        iFeat++;

    end_of_loop:

        if (VSIFEofL(m_poFp) || VSIFErrorL(m_poFp))
        {
            CPLDebug("FlatGeobuf", "GetNextFeature: iteration end due to EOF");
            break;
        }

        m_featuresPos++;
        bEOFOrError = false;
    }
after_loop:
    if (bEOFOrError)
        m_bEOF = true;

    sHelper.Shrink(iFeat);

    if (out_array->length != 0 && m_poAttrQuery)
    {
        struct ArrowSchema schema;
        stream->get_schema(stream, &schema);
        CPLAssert(schema.release != nullptr);
        CPLAssert(schema.n_children == out_array->n_children);
        // Spatial filter already evaluated
        auto poFilterGeomBackup = m_poFilterGeom;
        m_poFilterGeom = nullptr;
        CPLStringList aosOptions;
        if (!m_poFilterGeom)
        {
            aosOptions.SetNameValue("BASE_SEQUENTIAL_FID",
                                    CPLSPrintf(CPL_FRMT_GIB, nFeatureIdxStart));
        }
        PostFilterArrowArray(&schema, out_array, aosOptions.List());
        schema.release(&schema);
        m_poFilterGeom = poFilterGeomBackup;
    }

    if (out_array->length == 0)
    {
        if (out_array->release)
            out_array->release(out_array);
        memset(out_array, 0, sizeof(*out_array));

        if (m_poAttrQuery || m_poFilterGeom)
        {
            goto begin;
        }
    }

    return 0;

error:
    sHelper.ClearArray();
    return errorErrno;
}

OGRErr OGRFlatGeobufLayer::CreateField(const OGRFieldDefn *poField,
                                       int /* bApproxOK */)
{
    // CPLDebugOnly("FlatGeobuf", "CreateField %s %s", poField->GetNameRef(),
    // poField->GetFieldTypeName(poField->GetType()));
    if (!TestCapability(OLCCreateField))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to create new fields after first feature written.");
        return OGRERR_FAILURE;
    }

    if (m_poFeatureDefn->GetFieldCount() > std::numeric_limits<uint16_t>::max())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot create features with more than 65536 columns");
        return OGRERR_FAILURE;
    }

    m_poFeatureDefn->AddFieldDefn(poField);

    return OGRERR_NONE;
}

OGRErr OGRFlatGeobufLayer::ICreateFeature(OGRFeature *poNewFeature)
{
    if (!m_create)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "CreateFeature() not supported on read-only layer");
        return OGRERR_FAILURE;
    }

    const auto fieldCount = m_poFeatureDefn->GetFieldCount();

    std::vector<uint8_t> &properties = m_writeProperties;
    properties.clear();
    properties.reserve(1024 * 4);
    FlatBufferBuilder fbb;
    fbb.TrackMinAlign(8);

    for (int i = 0; i < fieldCount; i++)
    {
        const auto fieldDef = m_poFeatureDefn->GetFieldDefn(i);
        if (!poNewFeature->IsFieldSetAndNotNull(i))
            continue;

        uint16_t column_index_le = static_cast<uint16_t>(i);
        CPL_LSBPTR16(&column_index_le);

        // CPLDebugOnly("FlatGeobuf", "DEBUG ICreateFeature: column_index_le:
        // %hu", column_index_le);

        std::copy(reinterpret_cast<const uint8_t *>(&column_index_le),
                  reinterpret_cast<const uint8_t *>(&column_index_le + 1),
                  std::back_inserter(properties));

        const auto fieldType = fieldDef->GetType();
        const auto fieldSubType = fieldDef->GetSubType();
        const auto field = poNewFeature->GetRawFieldRef(i);
        switch (fieldType)
        {
            case OGRFieldType::OFTInteger:
            {
                int nVal = field->Integer;
                if (fieldSubType == OFSTBoolean)
                {
                    GByte byVal = static_cast<GByte>(nVal);
                    std::copy(reinterpret_cast<const uint8_t *>(&byVal),
                              reinterpret_cast<const uint8_t *>(&byVal + 1),
                              std::back_inserter(properties));
                }
                else if (fieldSubType == OFSTInt16)
                {
                    short sVal = static_cast<short>(nVal);
                    CPL_LSBPTR16(&sVal);
                    std::copy(reinterpret_cast<const uint8_t *>(&sVal),
                              reinterpret_cast<const uint8_t *>(&sVal + 1),
                              std::back_inserter(properties));
                }
                else
                {
                    CPL_LSBPTR32(&nVal);
                    std::copy(reinterpret_cast<const uint8_t *>(&nVal),
                              reinterpret_cast<const uint8_t *>(&nVal + 1),
                              std::back_inserter(properties));
                }
                break;
            }
            case OGRFieldType::OFTInteger64:
            {
                GIntBig nVal = field->Integer64;
                CPL_LSBPTR64(&nVal);
                std::copy(reinterpret_cast<const uint8_t *>(&nVal),
                          reinterpret_cast<const uint8_t *>(&nVal + 1),
                          std::back_inserter(properties));
                break;
            }
            case OGRFieldType::OFTReal:
            {
                double dfVal = field->Real;
                if (fieldSubType == OFSTFloat32)
                {
                    float fVal = static_cast<float>(dfVal);
                    CPL_LSBPTR32(&fVal);
                    std::copy(reinterpret_cast<const uint8_t *>(&fVal),
                              reinterpret_cast<const uint8_t *>(&fVal + 1),
                              std::back_inserter(properties));
                }
                else
                {
                    CPL_LSBPTR64(&dfVal);
                    std::copy(reinterpret_cast<const uint8_t *>(&dfVal),
                              reinterpret_cast<const uint8_t *>(&dfVal + 1),
                              std::back_inserter(properties));
                }
                break;
            }
            case OGRFieldType::OFTDate:
            case OGRFieldType::OFTTime:
            case OGRFieldType::OFTDateTime:
            {
                char szBuffer[OGR_SIZEOF_ISO8601_DATETIME_BUFFER];
                const size_t len =
                    OGRGetISO8601DateTime(field, false, szBuffer);
                uint32_t l_le = static_cast<uint32_t>(len);
                CPL_LSBPTR32(&l_le);
                std::copy(reinterpret_cast<const uint8_t *>(&l_le),
                          reinterpret_cast<const uint8_t *>(&l_le + 1),
                          std::back_inserter(properties));
                std::copy(szBuffer, szBuffer + len,
                          std::back_inserter(properties));
                break;
            }
            case OGRFieldType::OFTString:
            {
                const size_t len = strlen(field->String);
                if (len >= feature_max_buffer_size ||
                    properties.size() > feature_max_buffer_size - len)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "ICreateFeature: String too long");
                    return OGRERR_FAILURE;
                }
                if (!CPLIsUTF8(field->String, static_cast<int>(len)))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "ICreateFeature: String '%s' is not a valid UTF-8 "
                             "string",
                             field->String);
                    return OGRERR_FAILURE;
                }

                // Valid cast since feature_max_buffer_size is 2 GB
                uint32_t l_le = static_cast<uint32_t>(len);
                CPL_LSBPTR32(&l_le);
                std::copy(reinterpret_cast<const uint8_t *>(&l_le),
                          reinterpret_cast<const uint8_t *>(&l_le + 1),
                          std::back_inserter(properties));
                try
                {
                    // to avoid coverity scan warning: "To avoid a quadratic
                    // time penalty when using reserve(), always increase the
                    // capacity
                    /// by a multiple of its current value"
                    if (properties.size() + len > properties.capacity() &&
                        properties.size() <
                            std::numeric_limits<size_t>::max() / 2)
                    {
                        properties.reserve(std::max(2 * properties.size(),
                                                    properties.size() + len));
                    }
                }
                catch (const std::bad_alloc &)
                {
                    CPLError(CE_Failure, CPLE_OutOfMemory,
                             "ICreateFeature: String too long");
                    return OGRERR_FAILURE;
                }
                std::copy(field->String, field->String + len,
                          std::back_inserter(properties));
                break;
            }

            case OGRFieldType::OFTBinary:
            {
                const size_t len = field->Binary.nCount;
                if (len >= feature_max_buffer_size ||
                    properties.size() > feature_max_buffer_size - len)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "ICreateFeature: Binary too long");
                    return OGRERR_FAILURE;
                }
                uint32_t l_le = static_cast<uint32_t>(len);
                CPL_LSBPTR32(&l_le);
                std::copy(reinterpret_cast<const uint8_t *>(&l_le),
                          reinterpret_cast<const uint8_t *>(&l_le + 1),
                          std::back_inserter(properties));
                try
                {
                    // to avoid coverity scan warning: "To avoid a quadratic
                    // time penalty when using reserve(), always increase the
                    // capacity
                    /// by a multiple of its current value"
                    if (properties.size() + len > properties.capacity() &&
                        properties.size() <
                            std::numeric_limits<size_t>::max() / 2)
                    {
                        properties.reserve(std::max(2 * properties.size(),
                                                    properties.size() + len));
                    }
                }
                catch (const std::bad_alloc &)
                {
                    CPLError(CE_Failure, CPLE_OutOfMemory,
                             "ICreateFeature: Binary too long");
                    return OGRERR_FAILURE;
                }
                std::copy(field->Binary.paData, field->Binary.paData + len,
                          std::back_inserter(properties));
                break;
            }

            default:
                CPLError(CE_Failure, CPLE_AppDefined,
                         "ICreateFeature: Missing implementation for "
                         "OGRFieldType %d",
                         fieldType);
                return OGRERR_FAILURE;
        }
    }

    // CPLDebugOnly("FlatGeobuf", "DEBUG ICreateFeature: properties.size():
    // %lu", static_cast<long unsigned int>(properties.size()));

    const auto ogrGeometry = poNewFeature->GetGeometryRef();
#ifdef DEBUG
    // char *wkt;
    // ogrGeometry->exportToWkt(&wkt);
    // CPLDebugOnly("FlatGeobuf", "poNewFeature as wkt: %s", wkt);
#endif
    if (m_bCreateSpatialIndexAtClose &&
        (ogrGeometry == nullptr || ogrGeometry->IsEmpty()))
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "ICreateFeature: NULL geometry not supported with spatial index");
        return OGRERR_FAILURE;
    }
    if (ogrGeometry != nullptr && m_geometryType != GeometryType::Unknown &&
        ogrGeometry->getGeometryType() != m_eGType)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "ICreateFeature: Mismatched geometry type. "
                 "Feature geometry type is %s, "
                 "expected layer geometry type is %s",
                 OGRGeometryTypeToName(ogrGeometry->getGeometryType()),
                 OGRGeometryTypeToName(m_eGType));
        return OGRERR_FAILURE;
    }

    try
    {
        // FlatBuffer serialization will crash/assert if the vectors go
        // beyond FLATBUFFERS_MAX_BUFFER_SIZE. We cannot easily anticipate
        // the size of the FlatBuffer, but WKB might be a good approximation.
        // Takes an extra security margin of 10%
        flatbuffers::Offset<FlatGeobuf::Geometry> geometryOffset = 0;
        if (ogrGeometry && !ogrGeometry->IsEmpty())
        {
            const auto nWKBSize = ogrGeometry->WkbSize();
            if (nWKBSize > feature_max_buffer_size - nWKBSize / 10)
            {
                CPLError(CE_Failure, CPLE_OutOfMemory,
                         "ICreateFeature: Too big geometry");
                return OGRERR_FAILURE;
            }
            GeometryWriter writer{fbb, ogrGeometry, m_geometryType, m_hasZ,
                                  m_hasM};
            geometryOffset = writer.write(0);
        }
        const auto pProperties = properties.empty() ? nullptr : &properties;
        if (properties.size() > feature_max_buffer_size - geometryOffset.o)
        {
            CPLError(CE_Failure, CPLE_OutOfMemory,
                     "ICreateFeature: Too big feature");
            return OGRERR_FAILURE;
        }
        // TODO: write columns if mixed schema in collection
        const auto feature =
            CreateFeatureDirect(fbb, geometryOffset, pProperties);
        fbb.FinishSizePrefixed(feature);

        OGREnvelope psEnvelope;
        if (ogrGeometry != nullptr)
        {
            ogrGeometry->getEnvelope(&psEnvelope);
            if (m_sExtent.IsInit())
                m_sExtent.Merge(psEnvelope);
            else
                m_sExtent = psEnvelope;
        }

        if (m_featuresCount == 0)
        {
            if (m_poFpWrite == nullptr)
            {
                CPLErrorInvalidPointer("output file handler");
                return OGRERR_FAILURE;
            }
            if (!SupportsSeekWhileWriting(m_osFilename))
            {
                writeHeader(m_poFpWrite, 0, nullptr);
            }
            else
            {
                std::vector<double> dummyExtent(
                    4, std::numeric_limits<double>::quiet_NaN());
                const uint64_t dummyFeatureCount =
                    0xDEADBEEF;  // write non-zero value, otherwise the reserved
                                 // size is not OK
                writeHeader(m_poFpWrite, dummyFeatureCount,
                            &dummyExtent);  // we will update it later
                m_offsetAfterHeader = m_writeOffset;
            }
            CPLDebugOnly("FlatGeobuf", "Writing first feature at offset: %lu",
                         static_cast<long unsigned int>(m_writeOffset));
        }

        m_maxFeatureSize =
            std::max(m_maxFeatureSize, static_cast<uint32_t>(fbb.GetSize()));
        size_t c =
            VSIFWriteL(fbb.GetBufferPointer(), 1, fbb.GetSize(), m_poFpWrite);
        if (c == 0)
            return CPLErrorIO("writing feature");
        if (m_bCreateSpatialIndexAtClose)
        {
            FeatureItem item;
            item.size = static_cast<uint32_t>(fbb.GetSize());
            item.offset = m_writeOffset;
            item.nodeItem = {psEnvelope.MinX, psEnvelope.MinY, psEnvelope.MaxX,
                             psEnvelope.MaxY, 0};
            m_featureItems.emplace_back(std::move(item));
        }
        m_writeOffset += c;

        m_featuresCount++;

        return OGRERR_NONE;
    }
    catch (const std::bad_alloc &)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory,
                 "ICreateFeature: Memory allocation failure");
        return OGRERR_FAILURE;
    }
}

OGRErr OGRFlatGeobufLayer::IGetExtent(int iGeomField, OGREnvelope *psExtent,
                                      bool bForce)
{
    if (m_sExtent.IsInit())
    {
        *psExtent = m_sExtent;
        return OGRERR_NONE;
    }
    return OGRLayer::IGetExtent(iGeomField, psExtent, bForce);
}

int OGRFlatGeobufLayer::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, OLCCreateField))
        return m_create;
    else if (EQUAL(pszCap, OLCSequentialWrite))
        return m_create;
    else if (EQUAL(pszCap, OLCRandomRead))
        return m_poHeader != nullptr && m_poHeader->index_node_size() > 0;
    else if (EQUAL(pszCap, OLCIgnoreFields))
        return true;
    else if (EQUAL(pszCap, OLCMeasuredGeometries))
        return true;
    else if (EQUAL(pszCap, OLCCurveGeometries))
        return true;
    else if (EQUAL(pszCap, OLCZGeometries))
        return true;
    else if (EQUAL(pszCap, OLCFastFeatureCount))
        return m_poFilterGeom == nullptr && m_poAttrQuery == nullptr &&
               m_featuresCount > 0;
    else if (EQUAL(pszCap, OLCFastGetExtent))
        return m_sExtent.IsInit();
    else if (EQUAL(pszCap, OLCFastSpatialFilter))
        return m_poHeader != nullptr && m_poHeader->index_node_size() > 0;
    else if (EQUAL(pszCap, OLCStringsAsUTF8))
        return true;
    else if (EQUAL(pszCap, OLCFastGetArrowStream))
        return true;
    else
        return false;
}

void OGRFlatGeobufLayer::ResetReading()
{
    CPLDebugOnly("FlatGeobuf", "ResetReading");
    m_offset = m_offsetFeatures;
    m_bEOF = false;
    m_featuresPos = 0;
    m_foundItems.clear();
    m_featuresCount = m_poHeader ? m_poHeader->features_count() : 0;
    m_queriedSpatialIndex = false;
    m_ignoreSpatialFilter = false;
    m_ignoreAttributeFilter = false;
    return;
}

std::string OGRFlatGeobufLayer::GetTempFilePath(const CPLString &fileName,
                                                CSLConstList papszOptions)
{
    const CPLString osDirname(CPLGetPathSafe(fileName.c_str()));
    const CPLString osBasename(CPLGetBasenameSafe(fileName.c_str()));
    const char *pszTempDir = CSLFetchNameValue(papszOptions, "TEMPORARY_DIR");
    std::string osTempFile =
        pszTempDir ? CPLFormFilenameSafe(pszTempDir, osBasename, nullptr)
        : (STARTS_WITH(fileName, "/vsi") && !STARTS_WITH(fileName, "/vsimem/"))
            ? CPLGenerateTempFilenameSafe(osBasename)
            : CPLFormFilenameSafe(osDirname, osBasename, nullptr);
    osTempFile += "_temp.fgb";
    return osTempFile;
}

VSILFILE *OGRFlatGeobufLayer::CreateOutputFile(const CPLString &osFilename,
                                               CSLConstList papszOptions,
                                               bool isTemp)
{
    std::string osTempFile;
    VSILFILE *poFpWrite;
    int savedErrno;
    if (isTemp)
    {
        CPLDebug("FlatGeobuf", "Spatial index requested will write to temp "
                               "file and do second pass on close");
        osTempFile = GetTempFilePath(osFilename, papszOptions);
        poFpWrite = VSIFOpenL(osTempFile.c_str(), "w+b");
        savedErrno = errno;
        // Unlink it now to avoid stale temporary file if killing the process
        // (only works on Unix)
        VSIUnlink(osTempFile.c_str());
    }
    else
    {
        CPLDebug("FlatGeobuf",
                 "No spatial index will write directly to output");
        if (!SupportsSeekWhileWriting(osFilename))
            poFpWrite = VSIFOpenL(osFilename, "wb");
        else
            poFpWrite = VSIFOpenL(osFilename, "w+b");
        savedErrno = errno;
    }
    if (poFpWrite == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "Failed to create %s:\n%s",
                 osFilename.c_str(), VSIStrerror(savedErrno));
        return nullptr;
    }
    return poFpWrite;
}

OGRFlatGeobufLayer *OGRFlatGeobufLayer::Create(
    GDALDataset *poDS, const char *pszLayerName, const char *pszFilename,
    const OGRSpatialReference *poSpatialRef, OGRwkbGeometryType eGType,
    bool bCreateSpatialIndexAtClose, CSLConstList papszOptions)
{
    std::string osTempFile = GetTempFilePath(pszFilename, papszOptions);
    VSILFILE *poFpWrite =
        CreateOutputFile(pszFilename, papszOptions, bCreateSpatialIndexAtClose);
    if (poFpWrite == nullptr)
        return nullptr;
    OGRFlatGeobufLayer *layer = new OGRFlatGeobufLayer(
        poDS, pszLayerName, pszFilename, poSpatialRef, eGType,
        bCreateSpatialIndexAtClose, poFpWrite, osTempFile, papszOptions);
    return layer;
}

OGRFlatGeobufLayer *OGRFlatGeobufLayer::Open(const Header *poHeader,
                                             GByte *headerBuf,
                                             const char *pszFilename,
                                             VSILFILE *poFp, uint64_t offset)
{
    OGRFlatGeobufLayer *layer =
        new OGRFlatGeobufLayer(poHeader, headerBuf, pszFilename, poFp, offset);
    return layer;
}

OGRFlatGeobufLayer *OGRFlatGeobufLayer::Open(const char *pszFilename,
                                             VSILFILE *fp, bool bVerifyBuffers)
{
    uint64_t offset = sizeof(magicbytes);
    CPLDebugOnly("FlatGeobuf", "Start at offset: %lu",
                 static_cast<long unsigned int>(offset));
    if (VSIFSeekL(fp, offset, SEEK_SET) == -1)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Unable to get seek in file");
        return nullptr;
    }
    uint32_t headerSize;
    if (VSIFReadL(&headerSize, 4, 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Failed to read header size");
        return nullptr;
    }
    CPL_LSBPTR32(&headerSize);
    CPLDebugOnly("FlatGeobuf", "headerSize: %d", headerSize);
    if (headerSize > header_max_buffer_size)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Header size too large (> 10 MB)");
        return nullptr;
    }
    std::unique_ptr<GByte, VSIFreeReleaser> buf(
        static_cast<GByte *>(VSIMalloc(headerSize)));
    if (buf == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Failed to allocate memory for header");
        return nullptr;
    }
    if (VSIFReadL(buf.get(), 1, headerSize, fp) != headerSize)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Failed to read header");
        return nullptr;
    }
    if (bVerifyBuffers)
    {
        Verifier v(buf.get(), headerSize, 64U, 1000000U, false);
        const auto ok = VerifyHeaderBuffer(v);
        if (!ok)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Header failed consistency verification");
            return nullptr;
        }
    }
    const auto header = GetHeader(buf.get());
    offset += 4 + headerSize;
    CPLDebugOnly("FlatGeobuf", "Add header size + length prefix to offset (%d)",
                 4 + headerSize);

    const auto featuresCount = header->features_count();

    if (featuresCount >
        std::min(static_cast<uint64_t>(std::numeric_limits<size_t>::max() / 8),
                 static_cast<uint64_t>(100) * 1000 * 1000 * 1000))
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Too many features");
        return nullptr;
    }

    const auto index_node_size = header->index_node_size();
    if (index_node_size > 0)
    {
        try
        {
            const auto treeSize = PackedRTree::size(featuresCount);
            CPLDebugOnly("FlatGeobuf", "Tree start at offset (%lu)",
                         static_cast<long unsigned int>(offset));
            offset += treeSize;
            CPLDebugOnly("FlatGeobuf", "Add tree size to offset (%lu)",
                         static_cast<long unsigned int>(treeSize));
        }
        catch (const std::exception &e)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Failed to calculate tree size: %s", e.what());
            return nullptr;
        }
    }

    CPLDebugOnly("FlatGeobuf", "Features start at offset (%lu)",
                 static_cast<long unsigned int>(offset));

    CPLDebugOnly("FlatGeobuf", "Opening OGRFlatGeobufLayer");
    auto poLayer = OGRFlatGeobufLayer::Open(header, buf.release(), pszFilename,
                                            fp, offset);
    poLayer->VerifyBuffers(bVerifyBuffers);

    return poLayer;
}

OGRFlatGeobufBaseLayerInterface::~OGRFlatGeobufBaseLayerInterface() = default;
