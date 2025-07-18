/******************************************************************************
 *
 * Project:  GDAL
 * Purpose:  MapML Translator
 * Author:   Even Rouault, Even Rouault <even dot rouault at spatialys dot com>
 *
 ******************************************************************************
 * Copyright (c) 2020, Even Rouault <even dot rouault at spatialys dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_minixml.h"
#include "gdal_pam.h"
#include "ogrsf_frmts.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

constexpr int EPSG_CODE_WGS84 = 4326;
constexpr int EPSG_CODE_CBMTILE = 3978;
constexpr int EPSG_CODE_APSTILE = 5936;
constexpr int EPSG_CODE_OSMTILE = 3857;

static const struct
{
    int nEPSGCode;
    const char *pszName;
} asKnownCRS[] = {
    {EPSG_CODE_WGS84, "WGS84"},
    {EPSG_CODE_CBMTILE, "CBMTILE"},
    {EPSG_CODE_APSTILE, "APSTILE"},
    {EPSG_CODE_OSMTILE, "OSMTILE"},
};

/************************************************************************/
/*                     OGRMapMLReaderDataset                            */
/************************************************************************/

class OGRMapMLReaderLayer;

class OGRMapMLReaderDataset final : public GDALPamDataset
{
    friend class OGRMapMLReaderLayer;

    std::vector<std::unique_ptr<OGRMapMLReaderLayer>> m_apoLayers{};
    CPLXMLTreeCloser m_oRootCloser{nullptr};
    CPLString m_osDefaultLayerName{};

  public:
    int GetLayerCount() override
    {
        return static_cast<int>(m_apoLayers.size());
    }

    OGRLayer *GetLayer(int idx) override;

    static int Identify(GDALOpenInfo *poOpenInfo);
    static GDALDataset *Open(GDALOpenInfo *);
};

/************************************************************************/
/*                         OGRMapMLReaderLayer                          */
/************************************************************************/

class OGRMapMLReaderLayer final
    : public OGRLayer,
      public OGRGetNextFeatureThroughRaw<OGRMapMLReaderLayer>
{
    OGRMapMLReaderDataset *m_poDS = nullptr;
    OGRFeatureDefn *m_poFeatureDefn = nullptr;
    OGRSpatialReference *m_poSRS = nullptr;

    // not to be destroyed
    CPLXMLNode *m_psBody = nullptr;
    CPLXMLNode *m_psCurNode = nullptr;
    GIntBig m_nFID = 1;

    OGRFeature *GetNextRawFeature();

  public:
    OGRMapMLReaderLayer(OGRMapMLReaderDataset *poDS, const char *pszLayerName);
    ~OGRMapMLReaderLayer();

    OGRFeatureDefn *GetLayerDefn() override
    {
        return m_poFeatureDefn;
    }

    void ResetReading() override;
    DEFINE_GET_NEXT_FEATURE_THROUGH_RAW(OGRMapMLReaderLayer)
    int TestCapability(const char *pszCap) override;

    GDALDataset *GetDataset() override
    {
        return m_poDS;
    }
};

/************************************************************************/
/*                        OGRMapMLWriterDataset                         */
/************************************************************************/

class OGRMapMLWriterLayer;

class OGRMapMLWriterDataset final : public GDALPamDataset
{
    friend class OGRMapMLWriterLayer;

    VSILFILE *m_fpOut = nullptr;
    std::vector<std::unique_ptr<OGRMapMLWriterLayer>> m_apoLayers{};
    CPLXMLNode *m_psRoot = nullptr;
    CPLString m_osExtentUnits{};
    OGRSpatialReference m_oSRS{};
    OGREnvelope m_sExtent{};
    CPLStringList m_aosOptions{};
    const char *m_pszFormatCoordTuple = nullptr;

    // not to be destroyed
    CPLXMLNode *m_psLastChild = nullptr;

  public:
    explicit OGRMapMLWriterDataset(VSILFILE *fpOut);
    ~OGRMapMLWriterDataset() override;

    int GetLayerCount() override
    {
        return static_cast<int>(m_apoLayers.size());
    }

    OGRLayer *GetLayer(int idx) override;

    OGRLayer *ICreateLayer(const char *pszName,
                           const OGRGeomFieldDefn *poGeomFieldDefn,
                           CSLConstList papszOptions) override;

    int TestCapability(const char *) override;

    void AddFeature(CPLXMLNode *psNode);

    static GDALDataset *Create(const char *pszFilename, int nXSize, int nYSize,
                               int nBandsIn, GDALDataType eDT,
                               char **papszOptions);
};

/************************************************************************/
/*                         OGRMapMLWriterLayer                          */
/************************************************************************/

class OGRMapMLWriterLayer final : public OGRLayer
{
    OGRMapMLWriterDataset *m_poDS = nullptr;
    OGRFeatureDefn *m_poFeatureDefn = nullptr;
    GIntBig m_nFID = 1;
    std::unique_ptr<OGRCoordinateTransformation> m_poCT{};

    void writeLineStringCoordinates(CPLXMLNode *psContainer,
                                    const OGRLineString *poLS);
    void writePolygon(CPLXMLNode *psContainer, const OGRPolygon *poPoly);
    void writeGeometry(CPLXMLNode *psContainer, const OGRGeometry *poGeom,
                       bool bInGeometryCollection);

  public:
    OGRMapMLWriterLayer(OGRMapMLWriterDataset *poDS, const char *pszLayerName,
                        std::unique_ptr<OGRCoordinateTransformation> &&poCT);
    ~OGRMapMLWriterLayer();

    OGRFeatureDefn *GetLayerDefn() override
    {
        return m_poFeatureDefn;
    }

    void ResetReading() override
    {
    }

    OGRFeature *GetNextFeature() override
    {
        return nullptr;
    }

    OGRErr CreateField(const OGRFieldDefn *poFieldDefn, int) override;
    OGRErr ICreateFeature(OGRFeature *poFeature) override;
    int TestCapability(const char *) override;

    GDALDataset *GetDataset() override
    {
        return m_poDS;
    }
};

/************************************************************************/
/*                             Identify()                               */
/************************************************************************/

int OGRMapMLReaderDataset::Identify(GDALOpenInfo *poOpenInfo)
{
    return poOpenInfo->pabyHeader != nullptr &&
           strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                  "<mapml-") != nullptr;
}

/************************************************************************/
/*                               Open()                                 */
/************************************************************************/

GDALDataset *OGRMapMLReaderDataset::Open(GDALOpenInfo *poOpenInfo)
{
    if (!Identify(poOpenInfo) || poOpenInfo->eAccess == GA_Update)
        return nullptr;
    CPLXMLNode *psRoot = CPLParseXMLFile(poOpenInfo->pszFilename);
    CPLXMLTreeCloser oRootCloser(psRoot);
    if (psRoot == nullptr)
        return nullptr;
    CPLXMLNode *psBody = CPLGetXMLNode(psRoot, "=mapml-.map-body");
    if (psBody == nullptr)
        return nullptr;
    CPLString osDefaultLayerName(CPLGetBasenameSafe(poOpenInfo->pszFilename));
    std::set<std::string> oSetLayerNames;
    for (auto psNode = psBody->psChild; psNode; psNode = psNode->psNext)
    {
        if (psNode->eType != CXT_Element ||
            strcmp(psNode->pszValue, "map-feature") != 0)
        {
            continue;
        }
        const char *pszClass =
            CPLGetXMLValue(psNode, "class", osDefaultLayerName.c_str());
        oSetLayerNames.insert(pszClass);
    }
    if (oSetLayerNames.empty())
        return nullptr;
    auto poDS = new OGRMapMLReaderDataset();
    poDS->m_osDefaultLayerName = std::move(osDefaultLayerName);
    poDS->m_oRootCloser = std::move(oRootCloser);
    for (const auto &layerName : oSetLayerNames)
    {
        poDS->m_apoLayers.emplace_back(std::unique_ptr<OGRMapMLReaderLayer>(
            new OGRMapMLReaderLayer(poDS, layerName.c_str())));
    }
    return poDS;
}

/************************************************************************/
/*                             GetLayer()                               */
/************************************************************************/

OGRLayer *OGRMapMLReaderDataset::GetLayer(int idx)
{
    return idx >= 0 && idx < GetLayerCount() ? m_apoLayers[idx].get() : nullptr;
}

/************************************************************************/
/*                         OGRMapMLReaderLayer()                        */
/************************************************************************/

OGRMapMLReaderLayer::OGRMapMLReaderLayer(OGRMapMLReaderDataset *poDS,
                                         const char *pszLayerName)
    : m_poDS(poDS)
{
    m_poFeatureDefn = new OGRFeatureDefn(pszLayerName);
    m_poFeatureDefn->Reference();
    SetDescription(pszLayerName);

    m_psBody = CPLGetXMLNode(poDS->m_oRootCloser.get(), "=mapml-.map-body");
    m_psCurNode = m_psBody->psChild;

    // get projection info from map-head/map-meta element
    const char *pszUnits = nullptr;
    CPLXMLNode *psHead =
        CPLGetXMLNode(poDS->m_oRootCloser.get(), "=mapml-.map-head");
    if (psHead)
    {
        for (CPLXMLNode *psMeta = psHead->psChild; psMeta;
             psMeta = psMeta->psNext)
        {
            if (psMeta->eType == CXT_Element &&
                strcmp(psMeta->pszValue, "map-meta") == 0)
            {
                const char *pszName = CPLGetXMLValue(psMeta, "name", nullptr);
                if (pszName && strcmp(pszName, "projection") == 0)
                {
                    pszUnits = CPLGetXMLValue(psMeta, "content", nullptr);
                    break;
                }
            }
        }
    }

    if (pszUnits)
    {
        for (const auto &knownCRS : asKnownCRS)
        {
            if (strcmp(pszUnits, knownCRS.pszName) == 0)
            {
                m_poSRS = new OGRSpatialReference();
                m_poSRS->importFromEPSG(knownCRS.nEPSGCode);
                m_poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                break;
            }
        }
    }
    m_poFeatureDefn->GetGeomFieldDefn(0)->SetSpatialRef(m_poSRS);

    // Guess layer geometry type and establish fields
    bool bMixed = false;
    OGRwkbGeometryType eLayerGType = wkbUnknown;
    std::vector<std::string> aosFieldNames;
    std::map<std::string, OGRFieldType> oMapFieldTypes;
    while (m_psCurNode != nullptr)
    {
        if (m_psCurNode->eType == CXT_Element &&
            strcmp(m_psCurNode->pszValue, "map-feature") == 0 &&
            strcmp(CPLGetXMLValue(m_psCurNode, "class",
                                  m_poDS->m_osDefaultLayerName.c_str()),
                   m_poFeatureDefn->GetName()) == 0)
        {
            const CPLXMLNode *psGeometry =
                CPLGetXMLNode(m_psCurNode, "map-geometry");
            if (!bMixed && psGeometry && psGeometry->psChild &&
                psGeometry->psChild->eType == CXT_Element)
            {
                OGRwkbGeometryType eGType = wkbUnknown;
                const char *pszType = psGeometry->psChild->pszValue;
                if (EQUAL(pszType, "map-point"))
                    eGType = wkbPoint;
                else if (EQUAL(pszType, "map-linestring"))
                    eGType = wkbLineString;
                else if (EQUAL(pszType, "map-polygon"))
                    eGType = wkbPolygon;
                else if (EQUAL(pszType, "map-multipoint"))
                    eGType = wkbMultiPoint;
                else if (EQUAL(pszType, "map-multilinestring"))
                    eGType = wkbMultiLineString;
                else if (EQUAL(pszType, "map-multipolygon"))
                    eGType = wkbMultiPolygon;
                else if (EQUAL(pszType, "map-geometrycollection"))
                    eGType = wkbGeometryCollection;
                if (eLayerGType == wkbUnknown)
                    eLayerGType = eGType;
                else if (eLayerGType != eGType)
                {
                    eLayerGType = wkbUnknown;
                    bMixed = true;
                }
            }

            const CPLXMLNode *psTBody =
                CPLGetXMLNode(m_psCurNode, "map-properties.div.table.tbody");
            if (psTBody)
            {
                for (const CPLXMLNode *psCur = psTBody->psChild; psCur;
                     psCur = psCur->psNext)
                {
                    if (psCur->eType == CXT_Element &&
                        strcmp(psCur->pszValue, "tr") == 0)
                    {
                        const CPLXMLNode *psTd = CPLGetXMLNode(psCur, "td");
                        if (psTd)
                        {
                            const char *pszFieldName =
                                CPLGetXMLValue(psTd, "itemprop", nullptr);
                            const char *pszValue =
                                CPLGetXMLValue(psTd, nullptr, nullptr);
                            if (pszFieldName && pszValue)
                            {
                                const auto eValType = CPLGetValueType(pszValue);
                                OGRFieldType eType = OFTString;
                                if (eValType == CPL_VALUE_INTEGER)
                                {
                                    const GIntBig nVal =
                                        CPLAtoGIntBig(pszValue);
                                    if (nVal < INT_MIN || nVal > INT_MAX)
                                        eType = OFTInteger64;
                                    else
                                        eType = OFTInteger;
                                }
                                else if (eValType == CPL_VALUE_REAL)
                                    eType = OFTReal;
                                else
                                {
                                    int nYear, nMonth, nDay, nHour, nMin, nSec;
                                    if (sscanf(pszValue,
                                               "%04d/%02d/%02d %02d:%02d:%02d",
                                               &nYear, &nMonth, &nDay, &nHour,
                                               &nMin, &nSec) == 6)
                                    {
                                        eType = OFTDateTime;
                                    }
                                    else if (sscanf(pszValue, "%04d/%02d/%02d",
                                                    &nYear, &nMonth,
                                                    &nDay) == 3)
                                    {
                                        eType = OFTDate;
                                    }
                                    else if (sscanf(pszValue, "%02d:%02d:%02d",
                                                    &nHour, &nMin, &nSec) == 3)
                                    {
                                        eType = OFTTime;
                                    }
                                }
                                auto oIter = oMapFieldTypes.find(pszFieldName);
                                if (oIter == oMapFieldTypes.end())
                                {
                                    aosFieldNames.emplace_back(pszFieldName);
                                    oMapFieldTypes[pszFieldName] = eType;
                                }
                                else if (oIter->second != eType)
                                {
                                    const auto eOldType = oIter->second;
                                    if (eType == OFTInteger64 &&
                                        eOldType == OFTInteger)
                                    {
                                        oIter->second = OFTInteger64;
                                    }
                                    else if (eType == OFTReal &&
                                             (eOldType == OFTInteger ||
                                              eOldType == OFTInteger64))
                                    {
                                        oIter->second = OFTReal;
                                    }
                                    else if ((eType == OFTInteger ||
                                              eType == OFTInteger64) &&
                                             (eOldType == OFTInteger64 ||
                                              eOldType == OFTReal))
                                    {
                                        // do nothing
                                    }
                                    else
                                    {
                                        oIter->second = OFTString;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        m_psCurNode = m_psCurNode->psNext;
    }

    m_poFeatureDefn->SetGeomType(eLayerGType);
    for (const auto &osFieldName : aosFieldNames)
    {
        OGRFieldDefn oField(osFieldName.c_str(), oMapFieldTypes[osFieldName]);
        m_poFeatureDefn->AddFieldDefn(&oField);
    }

    OGRMapMLReaderLayer::ResetReading();
}

/************************************************************************/
/*                        ~OGRMapMLReaderLayer()                        */
/************************************************************************/

OGRMapMLReaderLayer::~OGRMapMLReaderLayer()
{
    if (m_poSRS)
        m_poSRS->Release();
    m_poFeatureDefn->Release();
}

/************************************************************************/
/*                            TestCapability()                          */
/************************************************************************/

int OGRMapMLReaderLayer::TestCapability(const char *pszCap)
{

    if (EQUAL(pszCap, OLCStringsAsUTF8))
        return true;
    return false;
}

/************************************************************************/
/*                              ResetReading()                          */
/************************************************************************/

void OGRMapMLReaderLayer::ResetReading()
{
    m_psCurNode = m_psBody->psChild;
    m_nFID++;
}

/************************************************************************/
/*                              ParseGeometry()                         */
/************************************************************************/

static OGRGeometry *ParseGeometry(const CPLXMLNode *psElement)
{
    if (EQUAL(psElement->pszValue, "map-point"))
    {
        const char *pszCoordinates =
            CPLGetXMLValue(psElement, "map-coordinates", nullptr);
        if (pszCoordinates)
        {
            const CPLStringList aosTokens(
                CSLTokenizeString2(pszCoordinates, " ", 0));
            if (aosTokens.size() == 2)
            {
                return new OGRPoint(CPLAtof(aosTokens[0]),
                                    CPLAtof(aosTokens[1]));
            }
        }
    }

    if (EQUAL(psElement->pszValue, "map-linestring"))
    {
        const char *pszCoordinates =
            CPLGetXMLValue(psElement, "map-coordinates", nullptr);
        if (pszCoordinates)
        {
            const CPLStringList aosTokens(
                CSLTokenizeString2(pszCoordinates, " ", 0));
            if ((aosTokens.size() % 2) == 0)
            {
                OGRLineString *poLS = new OGRLineString();
                const int nNumPoints = aosTokens.size() / 2;
                poLS->setNumPoints(nNumPoints);
                for (int i = 0; i < nNumPoints; i++)
                {
                    poLS->setPoint(i, CPLAtof(aosTokens[2 * i]),
                                   CPLAtof(aosTokens[2 * i + 1]));
                }
                return poLS;
            }
        }
    }

    if (EQUAL(psElement->pszValue, "map-polygon"))
    {
        OGRPolygon *poPolygon = new OGRPolygon();
        for (const CPLXMLNode *psCur = psElement->psChild; psCur;
             psCur = psCur->psNext)
        {
            if (psCur->eType == CXT_Element &&
                strcmp(psCur->pszValue, "map-coordinates") == 0 &&
                psCur->psChild && psCur->psChild->eType == CXT_Text)
            {
                const CPLStringList aosTokens(
                    CSLTokenizeString2(psCur->psChild->pszValue, " ", 0));
                if ((aosTokens.size() % 2) == 0)
                {
                    OGRLinearRing *poLS = new OGRLinearRing();
                    const int nNumPoints = aosTokens.size() / 2;
                    poLS->setNumPoints(nNumPoints);
                    for (int i = 0; i < nNumPoints; i++)
                    {
                        poLS->setPoint(i, CPLAtof(aosTokens[2 * i]),
                                       CPLAtof(aosTokens[2 * i + 1]));
                    }
                    poPolygon->addRingDirectly(poLS);
                }
            }
        }
        return poPolygon;
    }

    if (EQUAL(psElement->pszValue, "map-multipoint"))
    {
        const char *pszCoordinates =
            CPLGetXMLValue(psElement, "map-coordinates", nullptr);
        if (pszCoordinates)
        {
            const CPLStringList aosTokens(
                CSLTokenizeString2(pszCoordinates, " ", 0));
            if ((aosTokens.size() % 2) == 0)
            {
                OGRMultiPoint *poMLP = new OGRMultiPoint();
                const int nNumPoints = aosTokens.size() / 2;
                for (int i = 0; i < nNumPoints; i++)
                {
                    poMLP->addGeometryDirectly(
                        new OGRPoint(CPLAtof(aosTokens[2 * i]),
                                     CPLAtof(aosTokens[2 * i + 1])));
                }
                return poMLP;
            }
        }
    }

    if (EQUAL(psElement->pszValue, "map-multilinestring"))
    {
        OGRMultiLineString *poMLS = new OGRMultiLineString();
        for (const CPLXMLNode *psCur = psElement->psChild; psCur;
             psCur = psCur->psNext)
        {
            if (psCur->eType == CXT_Element &&
                strcmp(psCur->pszValue, "map-coordinates") == 0 &&
                psCur->psChild && psCur->psChild->eType == CXT_Text)
            {
                const CPLStringList aosTokens(
                    CSLTokenizeString2(psCur->psChild->pszValue, " ", 0));
                if ((aosTokens.size() % 2) == 0)
                {
                    OGRLineString *poLS = new OGRLineString();
                    const int nNumPoints = aosTokens.size() / 2;
                    poLS->setNumPoints(nNumPoints);
                    for (int i = 0; i < nNumPoints; i++)
                    {
                        poLS->setPoint(i, CPLAtof(aosTokens[2 * i]),
                                       CPLAtof(aosTokens[2 * i + 1]));
                    }
                    poMLS->addGeometryDirectly(poLS);
                }
            }
        }
        return poMLS;
    }

    if (EQUAL(psElement->pszValue, "map-multipolygon"))
    {
        OGRMultiPolygon *poMLP = new OGRMultiPolygon();
        for (const CPLXMLNode *psCur = psElement->psChild; psCur;
             psCur = psCur->psNext)
        {
            if (psCur->eType == CXT_Element &&
                EQUAL(psCur->pszValue, "map-polygon"))
            {
                OGRGeometry *poSubGeom = ParseGeometry(psCur);
                if (poSubGeom)
                    poMLP->addGeometryDirectly(poSubGeom);
            }
        }
        return poMLP;
    }

    if (EQUAL(psElement->pszValue, "map-geometrycollection"))
    {
        OGRGeometryCollection *poGC = new OGRGeometryCollection();
        for (const CPLXMLNode *psCur = psElement->psChild; psCur;
             psCur = psCur->psNext)
        {
            if (psCur->eType == CXT_Element &&
                !EQUAL(psCur->pszValue, "map-geometrycollection"))
            {
                OGRGeometry *poSubGeom = ParseGeometry(psCur);
                if (poSubGeom)
                    poGC->addGeometryDirectly(poSubGeom);
            }
        }
        return poGC;
    }

    return nullptr;
}

/************************************************************************/
/*                            GetNextRawFeature()                       */
/************************************************************************/

OGRFeature *OGRMapMLReaderLayer::GetNextRawFeature()
{
    while (m_psCurNode != nullptr)
    {
        if (m_psCurNode->eType == CXT_Element &&
            strcmp(m_psCurNode->pszValue, "map-feature") == 0 &&
            strcmp(CPLGetXMLValue(m_psCurNode, "class",
                                  m_poDS->m_osDefaultLayerName.c_str()),
                   m_poFeatureDefn->GetName()) == 0)
        {
            break;
        }
        m_psCurNode = m_psCurNode->psNext;
    }
    if (m_psCurNode == nullptr)
        return nullptr;

    OGRFeature *poFeature = new OGRFeature(m_poFeatureDefn);
    poFeature->SetFID(m_nFID);
    const char *pszId = CPLGetXMLValue(m_psCurNode, "id", nullptr);
    if (pszId &&
        STARTS_WITH_CI(pszId,
                       (CPLString(m_poFeatureDefn->GetName()) + '.').c_str()))
    {
        poFeature->SetFID(
            CPLAtoGIntBig(pszId + strlen(m_poFeatureDefn->GetName()) + 1));
    }
    m_nFID++;

    const CPLXMLNode *psGeometry = CPLGetXMLNode(m_psCurNode, "map-geometry");
    if (psGeometry && psGeometry->psChild &&
        psGeometry->psChild->eType == CXT_Element)
    {
        OGRGeometry *poGeom = ParseGeometry(psGeometry->psChild);
        if (poGeom)
        {
            poGeom->assignSpatialReference(GetSpatialRef());
            poFeature->SetGeometryDirectly(poGeom);
        }
    }

    const CPLXMLNode *psTBody =
        CPLGetXMLNode(m_psCurNode, "map-properties.div.table.tbody");
    if (psTBody)
    {
        for (const CPLXMLNode *psCur = psTBody->psChild; psCur;
             psCur = psCur->psNext)
        {
            if (psCur->eType == CXT_Element &&
                strcmp(psCur->pszValue, "tr") == 0)
            {
                const CPLXMLNode *psTd = CPLGetXMLNode(psCur, "td");
                if (psTd)
                {
                    const char *pszFieldName =
                        CPLGetXMLValue(psTd, "itemprop", nullptr);
                    const char *pszValue =
                        CPLGetXMLValue(psTd, nullptr, nullptr);
                    if (pszFieldName && pszValue)
                    {
                        poFeature->SetField(pszFieldName, pszValue);
                    }
                }
            }
        }
    }

    m_psCurNode = m_psCurNode->psNext;

    return poFeature;
}

/************************************************************************/
/*                         OGRMapMLWriterDataset()                      */
/************************************************************************/

OGRMapMLWriterDataset::OGRMapMLWriterDataset(VSILFILE *fpOut) : m_fpOut(fpOut)
{
}

/************************************************************************/
/*                        ~OGRMapMLWriterDataset()                      */
/************************************************************************/

OGRMapMLWriterDataset::~OGRMapMLWriterDataset()
{
    if (m_fpOut)
    {
        // Add map-meta elements to map-head
        CPLXMLNode *psHead = CPLGetXMLNode(m_psRoot, "map-head");
        if (psHead && !m_osExtentUnits.empty())
        {
            // Add projection meta element
            CPLXMLNode *psProjectionMeta =
                CPLCreateXMLNode(psHead, CXT_Element, "map-meta");
            CPLAddXMLAttributeAndValue(psProjectionMeta, "name", "projection");
            CPLAddXMLAttributeAndValue(psProjectionMeta, "content",
                                       m_osExtentUnits);
            // Force end tag by adding empty text content
            CPLCreateXMLNode(psProjectionMeta, CXT_Text, "");

            // Add coordinate system meta element
            const char *pszCS = m_oSRS.IsProjected() ? "pcrs" : "gcrs";
            CPLXMLNode *psCSMeta =
                CPLCreateXMLNode(psHead, CXT_Element, "map-meta");
            CPLAddXMLAttributeAndValue(psCSMeta, "name", "cs");
            CPLAddXMLAttributeAndValue(psCSMeta, "content", pszCS);
            // Force end tag by adding empty text content
            CPLCreateXMLNode(psCSMeta, CXT_Text, "");

            // Add extent meta element
            const char *pszXAxis =
                m_oSRS.IsProjected() ? "easting" : "longitude";
            const char *pszYAxis =
                m_oSRS.IsProjected() ? "northing" : "latitude";

            CPLString osExtentContent;
            osExtentContent.Printf(
                "top-left-%s=%s, top-left-%s=%s, bottom-right-%s=%s, "
                "bottom-right-%s=%s",
                pszXAxis,
                m_sExtent.IsInit()
                    ? CPLSPrintf("%.2f", m_sExtent.MinX)
                    : m_aosOptions.FetchNameValueDef("EXTENT_XMIN", "0"),
                pszYAxis,
                m_sExtent.IsInit()
                    ? CPLSPrintf("%.2f", m_sExtent.MaxY)
                    : m_aosOptions.FetchNameValueDef("EXTENT_YMAX", "0"),
                pszXAxis,
                m_sExtent.IsInit()
                    ? CPLSPrintf("%.2f", m_sExtent.MaxX)
                    : m_aosOptions.FetchNameValueDef("EXTENT_XMAX", "0"),
                pszYAxis,
                m_sExtent.IsInit()
                    ? CPLSPrintf("%.2f", m_sExtent.MinY)
                    : m_aosOptions.FetchNameValueDef("EXTENT_YMIN", "0"));
            CPLXMLNode *psExtentMeta =
                CPLCreateXMLNode(psHead, CXT_Element, "map-meta");
            CPLAddXMLAttributeAndValue(psExtentMeta, "name", "extent");
            CPLAddXMLAttributeAndValue(psExtentMeta, "content",
                                       osExtentContent);
            CPLCreateXMLNode(psExtentMeta, CXT_Text, "");  // Force end tag

            // Add zoom meta element if zoom options provided
            if (CSLFetchNameValue(m_aosOptions, "EXTENT_ZOOM") ||
                CSLFetchNameValue(m_aosOptions, "EXTENT_ZOOM_MIN") ||
                CSLFetchNameValue(m_aosOptions, "EXTENT_ZOOM_MAX"))
            {
                CPLString osZoomContent;
                osZoomContent.Printf(
                    "min=%s,max=%s,value=%s",
                    m_aosOptions.FetchNameValueDef("EXTENT_ZOOM_MIN", "0"),
                    m_aosOptions.FetchNameValueDef("EXTENT_ZOOM_MAX", "22"),
                    m_aosOptions.FetchNameValueDef("EXTENT_ZOOM", "3"));

                CPLXMLNode *psZoomMeta =
                    CPLCreateXMLNode(psHead, CXT_Element, "map-meta");
                CPLAddXMLAttributeAndValue(psZoomMeta, "name", "zoom");
                CPLAddXMLAttributeAndValue(psZoomMeta, "content",
                                           osZoomContent);
                CPLCreateXMLNode(psZoomMeta, CXT_Text, "");  // Force end tag
            }

            const char *pszHeadLinks =
                CSLFetchNameValue(m_aosOptions, "HEAD_LINKS");
            if (pszHeadLinks)
            {
                CPLXMLNode *psLinks = CPLParseXMLString(pszHeadLinks);
                if (psLinks)
                {
                    // Force closing tags by adding empty text content to map-link element
                    CPLXMLNode *psCurrent = psLinks;
                    while (psCurrent)
                    {
                        if (psCurrent->eType == CXT_Element &&
                            strcmp(psCurrent->pszValue, "map-link") == 0)
                        {
                            // Add empty text content to force end tag
                            CPLCreateXMLNode(psCurrent, CXT_Text, "");
                        }
                        psCurrent = psCurrent->psNext;
                    }

                    // Add links as children of map-head, after all content
                    if (psHead->psChild == nullptr)
                    {
                        psHead->psChild = psLinks;
                    }
                    else
                    {
                        CPLXMLNode *psLastChild = psHead->psChild;
                        while (psLastChild->psNext)
                            psLastChild = psLastChild->psNext;
                        psLastChild->psNext = psLinks;
                    }
                }
            }
        }
        char *pszDoc = CPLSerializeXMLTree(m_psRoot);
        const size_t nSize = strlen(pszDoc);
        if (VSIFWriteL(pszDoc, 1, nSize, m_fpOut) != nSize)
        {
            CPLError(CE_Failure, CPLE_FileIO,
                     "Failed to write whole XML document");
        }
        VSIFCloseL(m_fpOut);
        VSIFree(pszDoc);
    }
    CPLDestroyXMLNode(m_psRoot);
}

/************************************************************************/
/*                              Create()                                */
/************************************************************************/

GDALDataset *OGRMapMLWriterDataset::Create(const char *pszFilename, int nXSize,
                                           int nYSize, int nBandsIn,
                                           GDALDataType eDT,
                                           char **papszOptions)
{
    if (nXSize != 0 || nYSize != 0 || nBandsIn != 0 || eDT != GDT_Unknown)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Only vector creation supported");
        return nullptr;
    }
    VSILFILE *fpOut = VSIFOpenL(pszFilename, "wb");
    if (fpOut == nullptr)
    {
        CPLError(CE_Failure, CPLE_FileIO, "Cannot create %s", pszFilename);
        return nullptr;
    }
    auto poDS = new OGRMapMLWriterDataset(fpOut);

    poDS->m_psRoot = CPLCreateXMLNode(nullptr, CXT_Element, "mapml-");
    CPLAddXMLAttributeAndValue(poDS->m_psRoot, "xmlns",
                               "http://www.w3.org/1999/xhtml");
    CPLXMLNode *psHead =
        CPLCreateXMLNode(poDS->m_psRoot, CXT_Element, "map-head");

    const char *pszHead = CSLFetchNameValue(papszOptions, "HEAD");
    if (pszHead)
    {
        CPLXMLNode *psHeadUser = pszHead[0] == '<' ? CPLParseXMLString(pszHead)
                                                   : CPLParseXMLFile(pszHead);
        if (psHeadUser)
        {
            if (psHeadUser->eType == CXT_Element &&
                strcmp(psHeadUser->pszValue, "map-head") == 0)
            {
                psHead->psChild = psHeadUser->psChild;
                psHeadUser->psChild = nullptr;
            }
            else if (psHeadUser->eType == CXT_Element)
            {
                psHead->psChild = psHeadUser;
                psHeadUser = nullptr;
            }
            CPLDestroyXMLNode(psHeadUser);
        }
    }

    const CPLString osExtentUnits =
        CSLFetchNameValueDef(papszOptions, "EXTENT_UNITS", "");
    if (!osExtentUnits.empty() && osExtentUnits != "AUTO")
    {
        int nTargetEPSGCode = 0;
        for (const auto &knownCRS : asKnownCRS)
        {
            if (osExtentUnits == knownCRS.pszName)
            {
                poDS->m_osExtentUnits = knownCRS.pszName;
                nTargetEPSGCode = knownCRS.nEPSGCode;
                break;
            }
        }
        if (nTargetEPSGCode == 0)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Unsupported value for EXTENT_UNITS");
            delete poDS;
            return nullptr;
        }
        poDS->m_oSRS.importFromEPSG(nTargetEPSGCode);
        poDS->m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }

    CPLXMLNode *psBody =
        CPLCreateXMLNode(poDS->m_psRoot, CXT_Element, "map-body");
    poDS->m_psLastChild = psBody;

    poDS->m_aosOptions = CSLDuplicate(papszOptions);

    return poDS;
}

/************************************************************************/
/*                             GetLayer()                               */
/************************************************************************/

OGRLayer *OGRMapMLWriterDataset::GetLayer(int idx)
{
    return idx >= 0 && idx < GetLayerCount() ? m_apoLayers[idx].get() : nullptr;
}

/************************************************************************/
/*                            TestCapability()                          */
/************************************************************************/

int OGRMapMLWriterDataset::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, ODsCCreateLayer))
        return true;
    return false;
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

OGRLayer *
OGRMapMLWriterDataset::ICreateLayer(const char *pszLayerName,
                                    const OGRGeomFieldDefn *poGeomFieldDefn,
                                    CSLConstList /*papszOptions*/)
{
    OGRSpatialReference oSRS_WGS84;
    const auto poSRSIn =
        poGeomFieldDefn ? poGeomFieldDefn->GetSpatialRef() : nullptr;
    const OGRSpatialReference *poSRS = poSRSIn;
    if (poSRS == nullptr)
    {
        oSRS_WGS84.SetFromUserInput(SRS_WKT_WGS84_LAT_LONG);
        oSRS_WGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        poSRS = &oSRS_WGS84;
    }

    if (m_oSRS.IsEmpty())
    {
        const char *pszAuthName = poSRS->GetAuthorityName(nullptr);
        const char *pszAuthCode = poSRS->GetAuthorityCode(nullptr);
        if (pszAuthName && pszAuthCode && EQUAL(pszAuthName, "EPSG"))
        {
            const int nEPSGCode = atoi(pszAuthCode);
            for (const auto &knownCRS : asKnownCRS)
            {
                if (nEPSGCode == knownCRS.nEPSGCode)
                {
                    m_osExtentUnits = knownCRS.pszName;
                    m_oSRS.importFromEPSG(nEPSGCode);
                    break;
                }
            }
        }
        if (m_oSRS.IsEmpty())
        {
            m_osExtentUnits = "WGS84";
            m_oSRS.importFromEPSG(EPSG_CODE_WGS84);
        }
        m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }
    m_pszFormatCoordTuple = m_oSRS.IsGeographic() ? "%.8f %.8f" : "%.2f %.2f";

    auto poCT = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(poSRS, &m_oSRS));
    if (!poCT)
        return nullptr;

    OGRMapMLWriterLayer *poLayer =
        new OGRMapMLWriterLayer(this, pszLayerName, std::move(poCT));

    m_apoLayers.push_back(std::unique_ptr<OGRMapMLWriterLayer>(poLayer));
    return m_apoLayers.back().get();
}

/************************************************************************/
/*                            AddFeature()                              */
/************************************************************************/

void OGRMapMLWriterDataset::AddFeature(CPLXMLNode *psNode)
{
    // Add features as children of map-body (m_psLastChild points to map-body)
    if (m_psLastChild->psChild == nullptr)
    {
        // First child of map-body
        m_psLastChild->psChild = psNode;
    }
    else
    {
        // Find last child of map-body and add as sibling
        CPLXMLNode *psLastChild = m_psLastChild->psChild;
        while (psLastChild->psNext)
            psLastChild = psLastChild->psNext;
        psLastChild->psNext = psNode;
    }
}

/************************************************************************/
/*                         OGRMapMLWriterLayer()                        */
/************************************************************************/

OGRMapMLWriterLayer::OGRMapMLWriterLayer(
    OGRMapMLWriterDataset *poDS, const char *pszLayerName,
    std::unique_ptr<OGRCoordinateTransformation> &&poCT)
    : m_poDS(poDS), m_poCT(std::move(poCT))
{
    m_poFeatureDefn = new OGRFeatureDefn(pszLayerName);
    m_poFeatureDefn->Reference();
}

/************************************************************************/
/*                        ~OGRMapMLWriterLayer()                        */
/************************************************************************/

OGRMapMLWriterLayer::~OGRMapMLWriterLayer()
{
    m_poFeatureDefn->Release();
}

/************************************************************************/
/*                            TestCapability()                          */
/************************************************************************/

int OGRMapMLWriterLayer::TestCapability(const char *pszCap)
{

    if (EQUAL(pszCap, OLCSequentialWrite) || EQUAL(pszCap, OLCCreateField))
        return true;
    return false;
}

/************************************************************************/
/*                            CreateField()                             */
/************************************************************************/

OGRErr OGRMapMLWriterLayer::CreateField(const OGRFieldDefn *poFieldDefn, int)
{
    m_poFeatureDefn->AddFieldDefn(poFieldDefn);
    return OGRERR_NONE;
}

/************************************************************************/
/*                   writeLineStringCoordinates()                       */
/************************************************************************/

void OGRMapMLWriterLayer::writeLineStringCoordinates(CPLXMLNode *psContainer,
                                                     const OGRLineString *poLS)
{
    CPLXMLNode *psCoordinates =
        CPLCreateXMLNode(psContainer, CXT_Element, "map-coordinates");
    std::string osCoordinates;
    for (int i = 0; i < poLS->getNumPoints(); i++)
    {
        if (!osCoordinates.empty())
            osCoordinates += ' ';
        osCoordinates += CPLSPrintf(m_poDS->m_pszFormatCoordTuple,
                                    poLS->getX(i), poLS->getY(i));
    }
    CPLCreateXMLNode(psCoordinates, CXT_Text, osCoordinates.c_str());
}

/************************************************************************/
/*                           writePolygon()                             */
/************************************************************************/

void OGRMapMLWriterLayer::writePolygon(CPLXMLNode *psContainer,
                                       const OGRPolygon *poPoly)
{
    CPLXMLNode *psPolygon =
        CPLCreateXMLNode(psContainer, CXT_Element, "map-polygon");
    bool bFirstRing = true;
    for (const auto poRing : *poPoly)
    {
        const bool bReversePointOrder =
            (bFirstRing && CPL_TO_BOOL(poRing->isClockwise())) ||
            (!bFirstRing && !CPL_TO_BOOL(poRing->isClockwise()));
        bFirstRing = false;
        CPLXMLNode *psCoordinates =
            CPLCreateXMLNode(psPolygon, CXT_Element, "map-coordinates");
        std::string osCoordinates;
        const int nPointCount = poRing->getNumPoints();
        for (int i = 0; i < nPointCount; i++)
        {
            if (!osCoordinates.empty())
                osCoordinates += ' ';
            const int idx = bReversePointOrder ? nPointCount - 1 - i : i;
            osCoordinates += CPLSPrintf(m_poDS->m_pszFormatCoordTuple,
                                        poRing->getX(idx), poRing->getY(idx));
        }
        CPLCreateXMLNode(psCoordinates, CXT_Text, osCoordinates.c_str());
    }
}

/************************************************************************/
/*                          writeGeometry()                             */
/************************************************************************/

void OGRMapMLWriterLayer::writeGeometry(CPLXMLNode *psContainer,
                                        const OGRGeometry *poGeom,
                                        bool bInGeometryCollection)
{
    switch (wkbFlatten(poGeom->getGeometryType()))
    {
        case wkbPoint:
        {
            const OGRPoint *poPoint = poGeom->toPoint();
            CPLXMLNode *psPoint =
                CPLCreateXMLNode(psContainer, CXT_Element, "map-point");
            CPLXMLNode *psCoordinates =
                CPLCreateXMLNode(psPoint, CXT_Element, "map-coordinates");
            CPLCreateXMLNode(psCoordinates, CXT_Text,
                             CPLSPrintf(m_poDS->m_pszFormatCoordTuple,
                                        poPoint->getX(), poPoint->getY()));
            break;
        }

        case wkbLineString:
        {
            const OGRLineString *poLS = poGeom->toLineString();
            CPLXMLNode *psLS =
                CPLCreateXMLNode(psContainer, CXT_Element, "map-linestring");
            writeLineStringCoordinates(psLS, poLS);
            break;
        }

        case wkbPolygon:
        {
            const OGRPolygon *poPoly = poGeom->toPolygon();
            writePolygon(psContainer, poPoly);
            break;
        }

        case wkbMultiPoint:
        {
            const OGRMultiPoint *poMP = poGeom->toMultiPoint();
            CPLXMLNode *psMultiPoint =
                CPLCreateXMLNode(psContainer, CXT_Element, "map-multipoint");
            CPLXMLNode *psCoordinates =
                CPLCreateXMLNode(psMultiPoint, CXT_Element, "map-coordinates");
            std::string osCoordinates;
            for (const auto poPoint : *poMP)
            {
                if (!poPoint->IsEmpty())
                {
                    if (!osCoordinates.empty())
                        osCoordinates += ' ';
                    osCoordinates +=
                        CPLSPrintf(m_poDS->m_pszFormatCoordTuple,
                                   poPoint->getX(), poPoint->getY());
                }
            }
            CPLCreateXMLNode(psCoordinates, CXT_Text, osCoordinates.c_str());
            break;
        }

        case wkbMultiLineString:
        {
            const OGRMultiLineString *poMLS = poGeom->toMultiLineString();
            CPLXMLNode *psMultiLineString = CPLCreateXMLNode(
                psContainer, CXT_Element, "map-multilinestring");
            for (const auto poLS : *poMLS)
            {
                if (!poLS->IsEmpty())
                {
                    writeLineStringCoordinates(psMultiLineString, poLS);
                }
            }
            break;
        }

        case wkbMultiPolygon:
        {
            const OGRMultiPolygon *poMLP = poGeom->toMultiPolygon();
            CPLXMLNode *psMultiPolygon =
                CPLCreateXMLNode(psContainer, CXT_Element, "map-multipolygon");
            for (const auto poPoly : *poMLP)
            {
                if (!poPoly->IsEmpty())
                {
                    writePolygon(psMultiPolygon, poPoly);
                }
            }
            break;
        }

        case wkbGeometryCollection:
        {
            const OGRGeometryCollection *poGC = poGeom->toGeometryCollection();
            CPLXMLNode *psGeometryCollection =
                bInGeometryCollection
                    ? psContainer
                    : CPLCreateXMLNode(psContainer, CXT_Element,
                                       "map-geometrycollection");
            for (const auto poSubGeom : *poGC)
            {
                if (!poSubGeom->IsEmpty())
                {
                    writeGeometry(psGeometryCollection, poSubGeom, true);
                }
            }
            break;
        }

        default:
            break;
    }
}

/************************************************************************/
/*                            ICreateFeature()                          */
/************************************************************************/

OGRErr OGRMapMLWriterLayer::ICreateFeature(OGRFeature *poFeature)
{
    CPLXMLNode *psFeature =
        CPLCreateXMLNode(nullptr, CXT_Element, "map-feature");
    GIntBig nFID = poFeature->GetFID();
    if (nFID < 0)
    {
        nFID = m_nFID;
        m_nFID++;
    }
    const CPLString osFID(
        CPLSPrintf("%s." CPL_FRMT_GIB, m_poFeatureDefn->GetName(), nFID));
    CPLAddXMLAttributeAndValue(psFeature, "id", osFID.c_str());
    CPLAddXMLAttributeAndValue(psFeature, "class", m_poFeatureDefn->GetName());

    const int nFieldCount = poFeature->GetFieldCount();
    if (nFieldCount > 0)
    {
        CPLXMLNode *psProperties =
            CPLCreateXMLNode(psFeature, CXT_Element, "map-properties");
        CPLXMLNode *psDiv = CPLCreateXMLNode(psProperties, CXT_Element, "div");
        CPLAddXMLAttributeAndValue(psDiv, "class", "table-container");
        CPLAddXMLAttributeAndValue(psDiv, "aria-labelledby",
                                   ("caption-" + osFID).c_str());
        CPLXMLNode *psTable = CPLCreateXMLNode(psDiv, CXT_Element, "table");
        CPLXMLNode *psCaption =
            CPLCreateXMLNode(psTable, CXT_Element, "caption");
        CPLAddXMLAttributeAndValue(psCaption, "id",
                                   ("caption-" + osFID).c_str());
        CPLCreateXMLNode(psCaption, CXT_Text, "Feature properties");
        CPLXMLNode *psTBody = CPLCreateXMLNode(psTable, CXT_Element, "tbody");
        {
            CPLXMLNode *psTr = CPLCreateXMLNode(psTBody, CXT_Element, "tr");
            {
                CPLXMLNode *psTh = CPLCreateXMLNode(psTr, CXT_Element, "th");
                CPLAddXMLAttributeAndValue(psTh, "role", "columnheader");
                CPLAddXMLAttributeAndValue(psTh, "scope", "col");
                CPLCreateXMLNode(psTh, CXT_Text, "Property name");
            }
            {
                CPLXMLNode *psTh = CPLCreateXMLNode(psTr, CXT_Element, "th");
                CPLAddXMLAttributeAndValue(psTh, "role", "columnheader");
                CPLAddXMLAttributeAndValue(psTh, "scope", "col");
                CPLCreateXMLNode(psTh, CXT_Text, "Property value");
            }
        }
        for (int i = 0; i < nFieldCount; i++)
        {
            if (poFeature->IsFieldSetAndNotNull(i))
            {
                const auto poFieldDefn = poFeature->GetFieldDefnRef(i);
                CPLXMLNode *psTr = CPLCreateXMLNode(psTBody, CXT_Element, "tr");
                {
                    CPLXMLNode *psTh =
                        CPLCreateXMLNode(psTr, CXT_Element, "th");
                    CPLAddXMLAttributeAndValue(psTh, "scope", "row");
                    CPLCreateXMLNode(psTh, CXT_Text, poFieldDefn->GetNameRef());
                }
                {
                    CPLXMLNode *psTd =
                        CPLCreateXMLNode(psTr, CXT_Element, "td");
                    CPLAddXMLAttributeAndValue(psTd, "itemprop",
                                               poFieldDefn->GetNameRef());
                    CPLCreateXMLNode(psTd, CXT_Text,
                                     poFeature->GetFieldAsString(i));
                }
            }
        }
    }

    const OGRGeometry *poGeom = poFeature->GetGeometryRef();
    if (poGeom && !poGeom->IsEmpty())
    {
        OGRGeometry *poGeomClone = poGeom->clone();
        if (poGeomClone->transform(m_poCT.get()) == OGRERR_NONE)
        {
            CPLXMLNode *psGeometry =
                CPLCreateXMLNode(nullptr, CXT_Element, "map-geometry");
            writeGeometry(psGeometry, poGeomClone, false);
            if (psGeometry->psChild == nullptr)
            {
                CPLDestroyXMLNode(psGeometry);
            }
            else
            {
                OGREnvelope sExtent;
                poGeomClone->getEnvelope(&sExtent);
                m_poDS->m_sExtent.Merge(sExtent);

                CPLXMLNode *psLastChild = psFeature->psChild;
                while (psLastChild->psNext)
                    psLastChild = psLastChild->psNext;
                psLastChild->psNext = psGeometry;
            }
        }
        delete poGeomClone;
    }

    m_poDS->AddFeature(psFeature);
    return OGRERR_NONE;
}

/************************************************************************/
/*                         RegisterOGRMapML()                           */
/************************************************************************/

void RegisterOGRMapML()

{
    if (GDALGetDriverByName("MapML") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("MapML");
    poDriver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_LAYER, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_FIELD, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "MapML");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/vector/mapml.html");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS, "OGRSQL SQLITE");

    poDriver->pfnIdentify = OGRMapMLReaderDataset::Identify;
    poDriver->pfnOpen = OGRMapMLReaderDataset::Open;
    poDriver->pfnCreate = OGRMapMLWriterDataset::Create;

    poDriver->SetMetadataItem(GDAL_DMD_CREATIONFIELDDATATYPES,
                              "Integer Integer64 Real String "
                              "Date DateTime Time");

    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONOPTIONLIST,
        "<CreationOptionList>"
        "  <Option name='HEAD' type='string' "
        "description='Filename or inline XML content for head element'/>"
        "  <Option name='EXTENT_UNITS' type='string-select' description='Force "
        "CRS'>"
        "    <Value>AUTO</Value>"
        "    <Value>WGS84</Value>"
        "    <Value>OSMTILE</Value>"
        "    <Value>CBMTILE</Value>"
        "    <Value>APSTILE</Value>"
        "  </Option>"
        "  <Option name='EXTENT_XMIN' type='float' description='Override "
        "extent xmin value'/>"
        "  <Option name='EXTENT_YMIN' type='float' description='Override "
        "extent ymin value'/>"
        "  <Option name='EXTENT_XMAX' type='float' description='Override "
        "extent xmax value'/>"
        "  <Option name='EXTENT_YMAX' type='float' description='Override "
        "extent ymax value'/>"
        "  <Option name='HEAD_LINKS' type='string' "
        "description='Inline XML content for extra content to insert as link "
        "elements in the body'/>"
        "</CreationOptionList>");

    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
