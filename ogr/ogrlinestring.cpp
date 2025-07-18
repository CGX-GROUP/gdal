/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  The OGRSimpleCurve and OGRLineString geometry classes.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999, Frank Warmerdam
 * Copyright (c) 2008-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_geometry.h"
#include "ogr_geos.h"
#include "ogr_p.h"

#include "geodesic.h"  // from PROJ

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <limits>
#include <new>

namespace
{

int DoubleToIntClamp(double dfValue)
{
    if (std::isnan(dfValue))
        return 0;
    if (dfValue >= std::numeric_limits<int>::max())
        return std::numeric_limits<int>::max();
    if (dfValue <= std::numeric_limits<int>::min())
        return std::numeric_limits<int>::min();
    return static_cast<int>(dfValue);
}

}  // namespace

/************************************************************************/
/*                OGRSimpleCurve( const OGRSimpleCurve& )               */
/************************************************************************/

/**
 * \brief Copy constructor.
 *
 * Note: before GDAL 2.1, only the default implementation of the constructor
 * existed, which could be unsafe to use.
 *
 * @since GDAL 2.1
 */

OGRSimpleCurve::OGRSimpleCurve(const OGRSimpleCurve &other)
    : OGRCurve(other), nPointCount(0), paoPoints(nullptr), padfZ(nullptr),
      padfM(nullptr)
{
    if (other.nPointCount > 0)
        setPoints(other.nPointCount, other.paoPoints, other.padfZ, other.padfM);
}

/************************************************************************/
/*                OGRSimpleCurve( OGRSimpleCurve&& )                    */
/************************************************************************/

/**
 * \brief Move constructor.
 *
 * @since GDAL 3.11
 */

// cppcheck-suppress-begin accessMoved
OGRSimpleCurve::OGRSimpleCurve(OGRSimpleCurve &&other)
    : OGRCurve(std::move(other)), nPointCount(other.nPointCount),
      m_nPointCapacity(other.m_nPointCapacity), paoPoints(other.paoPoints),
      padfZ(other.padfZ), padfM(other.padfM)
{
    other.nPointCount = 0;
    other.m_nPointCapacity = 0;
    other.paoPoints = nullptr;
    other.padfZ = nullptr;
    other.padfM = nullptr;
}

// cppcheck-suppress-end accessMoved

/************************************************************************/
/*                          ~OGRSimpleCurve()                           */
/************************************************************************/

OGRSimpleCurve::~OGRSimpleCurve()

{
    CPLFree(paoPoints);
    CPLFree(padfZ);
    CPLFree(padfM);
}

/************************************************************************/
/*                 operator=(const OGRSimpleCurve &other)               */
/************************************************************************/

/**
 * \brief Assignment operator.
 *
 * Note: before GDAL 2.1, only the default implementation of the operator
 * existed, which could be unsafe to use.
 *
 * @since GDAL 2.1
 */

OGRSimpleCurve &OGRSimpleCurve::operator=(const OGRSimpleCurve &other)
{
    if (this == &other)
        return *this;

    OGRCurve::operator=(other);

    setPoints(other.nPointCount, other.paoPoints, other.padfZ, other.padfM);
    flags = other.flags;

    return *this;
}

/************************************************************************/
/*                     operator=(OGRSimpleCurve &&other)                */
/************************************************************************/

/**
 * \brief Move assignment operator.
 *
 * @since GDAL 3.11
 */

OGRSimpleCurve &OGRSimpleCurve::operator=(OGRSimpleCurve &&other)
{
    if (this != &other)
    {
        // cppcheck-suppress-begin accessMoved
        OGRCurve::operator=(std::move(other));

        nPointCount = other.nPointCount;
        m_nPointCapacity = other.m_nPointCapacity;
        CPLFree(paoPoints);
        paoPoints = other.paoPoints;
        CPLFree(padfZ);
        padfZ = other.padfZ;
        CPLFree(padfM);
        padfM = other.padfM;
        flags = other.flags;
        other.nPointCount = 0;
        other.m_nPointCapacity = 0;
        other.paoPoints = nullptr;
        other.padfZ = nullptr;
        other.padfM = nullptr;
        // cppcheck-suppress-end accessMoved
    }

    return *this;
}

/************************************************************************/
/*                            flattenTo2D()                             */
/************************************************************************/

void OGRSimpleCurve::flattenTo2D()

{
    Make2D();
    setMeasured(FALSE);
}

/************************************************************************/
/*                               empty()                                */
/************************************************************************/

void OGRSimpleCurve::empty()

{
    setNumPoints(0);
}

/************************************************************************/
/*                       setCoordinateDimension()                       */
/************************************************************************/

bool OGRSimpleCurve::setCoordinateDimension(int nNewDimension)

{
    setMeasured(FALSE);
    if (nNewDimension == 2)
        Make2D();
    else if (nNewDimension == 3)
        return Make3D();
    return true;
}

bool OGRSimpleCurve::set3D(OGRBoolean bIs3D)

{
    if (bIs3D)
        return Make3D();
    else
        Make2D();
    return true;
}

bool OGRSimpleCurve::setMeasured(OGRBoolean bIsMeasured)

{
    if (bIsMeasured)
        return AddM();
    else
        RemoveM();
    return true;
}

/************************************************************************/
/*                              WkbSize()                               */
/*                                                                      */
/*      Return the size of this object in well known binary             */
/*      representation including the byte order, and type information.  */
/************************************************************************/

size_t OGRSimpleCurve::WkbSize() const

{
    return 5 + 4 + 8 * static_cast<size_t>(nPointCount) * CoordinateDimension();
}

//! @cond Doxygen_Suppress

/************************************************************************/
/*                               Make2D()                               */
/************************************************************************/

void OGRSimpleCurve::Make2D()

{
    if (padfZ != nullptr)
    {
        CPLFree(padfZ);
        padfZ = nullptr;
    }
    flags &= ~OGR_G_3D;
}

/************************************************************************/
/*                               Make3D()                               */
/************************************************************************/

bool OGRSimpleCurve::Make3D()

{
    if (padfZ == nullptr)
    {
        padfZ = static_cast<double *>(
            VSI_CALLOC_VERBOSE(sizeof(double), std::max(1, m_nPointCapacity)));
        if (padfZ == nullptr)
        {
            flags &= ~OGR_G_3D;
            CPLError(CE_Failure, CPLE_AppDefined,
                     "OGRSimpleCurve::Make3D() failed");
            return false;
        }
    }
    flags |= OGR_G_3D;
    return true;
}

/************************************************************************/
/*                               RemoveM()                              */
/************************************************************************/

void OGRSimpleCurve::RemoveM()

{
    if (padfM != nullptr)
    {
        CPLFree(padfM);
        padfM = nullptr;
    }
    flags &= ~OGR_G_MEASURED;
}

/************************************************************************/
/*                               AddM()                                 */
/************************************************************************/

bool OGRSimpleCurve::AddM()

{
    if (padfM == nullptr)
    {
        padfM = static_cast<double *>(
            VSI_CALLOC_VERBOSE(sizeof(double), std::max(1, m_nPointCapacity)));
        if (padfM == nullptr)
        {
            flags &= ~OGR_G_MEASURED;
            CPLError(CE_Failure, CPLE_AppDefined,
                     "OGRSimpleCurve::AddM() failed");
            return false;
        }
    }
    flags |= OGR_G_MEASURED;
    return true;
}

//! @endcond

/************************************************************************/
/*                              getPoint()                              */
/************************************************************************/

/**
 * \brief Fetch a point in line string.
 *
 * This method relates to the SFCOM ILineString::get_Point() method.
 *
 * @param i the vertex to fetch, from 0 to getNumPoints()-1.
 * @param poPoint a point to initialize with the fetched point.
 */

void OGRSimpleCurve::getPoint(int i, OGRPoint *poPoint) const

{
    CPLAssert(i >= 0);
    CPLAssert(i < nPointCount);
    CPLAssert(poPoint != nullptr);

    poPoint->setX(paoPoints[i].x);
    poPoint->setY(paoPoints[i].y);

    if ((flags & OGR_G_3D) && padfZ != nullptr)
        poPoint->setZ(padfZ[i]);
    if ((flags & OGR_G_MEASURED) && padfM != nullptr)
        poPoint->setM(padfM[i]);
}

/**
 * \fn int OGRSimpleCurve::getNumPoints() const;
 *
 * \brief Fetch vertex count.
 *
 * Returns the number of vertices in the line string.
 *
 * @return vertex count.
 */

/**
 * \fn double OGRSimpleCurve::getX( int iVertex ) const;
 *
 * \brief Get X at vertex.
 *
 * Returns the X value at the indicated vertex.   If iVertex is out of range a
 * crash may occur, no internal range checking is performed.
 *
 * @param iVertex the vertex to return, between 0 and getNumPoints()-1.
 *
 * @return X value.
 */

/**
 * \fn double OGRSimpleCurve::getY( int iVertex ) const;
 *
 * \brief Get Y at vertex.
 *
 * Returns the Y value at the indicated vertex.   If iVertex is out of range a
 * crash may occur, no internal range checking is performed.
 *
 * @param iVertex the vertex to return, between 0 and getNumPoints()-1.
 *
 * @return X value.
 */

/************************************************************************/
/*                                getZ()                                */
/************************************************************************/

/**
 * \brief Get Z at vertex.
 *
 * Returns the Z (elevation) value at the indicated vertex.  If no Z
 * value is available, 0.0 is returned.  If iVertex is out of range a
 * crash may occur, no internal range checking is performed.
 *
 * @param iVertex the vertex to return, between 0 and getNumPoints()-1.
 *
 * @return Z value.
 */

double OGRSimpleCurve::getZ(int iVertex) const

{
    if (padfZ != nullptr && iVertex >= 0 && iVertex < nPointCount &&
        (flags & OGR_G_3D))
        return (padfZ[iVertex]);
    else
        return 0.0;
}

/************************************************************************/
/*                                getM()                                */
/************************************************************************/

/**
 * \brief Get measure at vertex.
 *
 * Returns the M (measure) value at the indicated vertex.  If no M
 * value is available, 0.0 is returned.
 *
 * @param iVertex the vertex to return, between 0 and getNumPoints()-1.
 *
 * @return M value.
 */

double OGRSimpleCurve::getM(int iVertex) const

{
    if (padfM != nullptr && iVertex >= 0 && iVertex < nPointCount &&
        (flags & OGR_G_MEASURED))
        return (padfM[iVertex]);
    else
        return 0.0;
}

/************************************************************************/
/*                            setNumPoints()                            */
/************************************************************************/

/**
 * \brief Set number of points in geometry.
 *
 * This method primary exists to preset the number of points in a linestring
 * geometry before setPoint() is used to assign them to avoid reallocating
 * the array larger with each call to addPoint().
 *
 * This method has no SFCOM analog.
 *
 * @param nNewPointCount the new number of points for geometry.
 * @param bZeroizeNewContent whether to set to zero the new elements of arrays
 *                           that are extended.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setNumPoints(int nNewPointCount, int bZeroizeNewContent)

{
    CPLAssert(nNewPointCount >= 0);

    if (nNewPointCount > m_nPointCapacity)
    {
        // Overflow of sizeof(OGRRawPoint) * nNewPointCount can only occur on
        // 32 bit, but we don't really want to allocate 2 billion points even on
        // 64 bit...
        if (nNewPointCount > std::numeric_limits<int>::max() /
                                 static_cast<int>(sizeof(OGRRawPoint)))
        {
            CPLError(CE_Failure, CPLE_IllegalArg,
                     "Too many points on line/curve (%d points exceeds the "
                     "limit of %d points)",
                     nNewPointCount,
                     std::numeric_limits<int>::max() /
                         static_cast<int>(sizeof(OGRRawPoint)));
            return false;
        }

        // If first allocation, just aim for nNewPointCount
        // Otherwise aim for nNewPointCount + nNewPointCount / 3 to have
        // exponential growth.
        const int nNewCapacity =
            (nPointCount == 0 ||
             nNewPointCount > std::numeric_limits<int>::max() /
                                      static_cast<int>(sizeof(OGRRawPoint)) -
                                  nNewPointCount / 3)
                ? nNewPointCount
                : nNewPointCount + nNewPointCount / 3;

        if (nPointCount == 0 && paoPoints)
        {
            // If there was an allocated array, but the old number of points is
            // 0, then free the arrays before allocating them, to avoid
            // potential costly recopy of useless data.
            VSIFree(paoPoints);
            paoPoints = nullptr;
            VSIFree(padfZ);
            padfZ = nullptr;
            VSIFree(padfM);
            padfM = nullptr;
            m_nPointCapacity = 0;
        }

        OGRRawPoint *paoNewPoints = static_cast<OGRRawPoint *>(
            VSI_REALLOC_VERBOSE(paoPoints, sizeof(OGRRawPoint) * nNewCapacity));
        if (paoNewPoints == nullptr)
        {
            return false;
        }
        paoPoints = paoNewPoints;

        if (flags & OGR_G_3D)
        {
            double *padfNewZ = static_cast<double *>(
                VSI_REALLOC_VERBOSE(padfZ, sizeof(double) * nNewCapacity));
            if (padfNewZ == nullptr)
            {
                return false;
            }
            padfZ = padfNewZ;
        }

        if (flags & OGR_G_MEASURED)
        {
            double *padfNewM = static_cast<double *>(
                VSI_REALLOC_VERBOSE(padfM, sizeof(double) * nNewCapacity));
            if (padfNewM == nullptr)
            {
                return false;
            }
            padfM = padfNewM;
        }

        m_nPointCapacity = nNewCapacity;
    }

    if (nNewPointCount > nPointCount && bZeroizeNewContent)
    {
        // gcc 8.0 (dev) complains about -Wclass-memaccess since
        // OGRRawPoint() has a constructor. So use a void* pointer.  Doing
        // the memset() here is correct since the constructor sets to 0.  We
        // could instead use a std::fill(), but at every other place, we
        // treat this class as a regular POD (see above use of realloc())
        void *dest = static_cast<void *>(paoPoints + nPointCount);
        memset(dest, 0, sizeof(OGRRawPoint) * (nNewPointCount - nPointCount));

        if ((flags & OGR_G_3D) && padfZ)
            memset(padfZ + nPointCount, 0,
                   sizeof(double) * (nNewPointCount - nPointCount));

        if ((flags & OGR_G_MEASURED) && padfM)
            memset(padfM + nPointCount, 0,
                   sizeof(double) * (nNewPointCount - nPointCount));
    }

    nPointCount = nNewPointCount;
    return true;
}

/************************************************************************/
/*                              setPoint()                              */
/************************************************************************/

/**
 * \brief Set the location of a vertex in line string.
 *
 * If iPoint is larger than the number of necessary the number of existing
 * points in the line string, the point count will be increased to
 * accommodate the request.
 *
 * There is no SFCOM analog to this method.
 *
 * @param iPoint the index of the vertex to assign (zero based).
 * @param poPoint the value to assign to the vertex.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setPoint(int iPoint, OGRPoint *poPoint)

{
    if ((flags & OGR_G_3D) && (flags & OGR_G_MEASURED))
        return setPoint(iPoint, poPoint->getX(), poPoint->getY(),
                        poPoint->getZ(), poPoint->getM());
    else if (flags & OGR_G_3D)
        return setPoint(iPoint, poPoint->getX(), poPoint->getY(),
                        poPoint->getZ());
    else if (flags & OGR_G_MEASURED)
        return setPointM(iPoint, poPoint->getX(), poPoint->getY(),
                         poPoint->getM());
    else
        return setPoint(iPoint, poPoint->getX(), poPoint->getY());
}

/************************************************************************/
/*                           CheckPointCount()                          */
/************************************************************************/

static inline bool CheckPointCount(int iPoint)
{
    if (iPoint == std::numeric_limits<int>::max())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Too big point count.");
        return false;
    }
    return true;
}

/************************************************************************/
/*                              setPoint()                              */
/************************************************************************/

/**
 * \brief Set the location of a vertex in line string.
 *
 * If iPoint is larger than the number of necessary the number of existing
 * points in the line string, the point count will be increased to
 * accommodate the request.
 *
 * There is no SFCOM analog to this method.
 *
 * @param iPoint the index of the vertex to assign (zero based).
 * @param xIn input X coordinate to assign.
 * @param yIn input Y coordinate to assign.
 * @param zIn input Z coordinate to assign (defaults to zero).
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setPoint(int iPoint, double xIn, double yIn, double zIn)

{
    if (!(flags & OGR_G_3D))
    {
        if (!Make3D())
            return false;
    }

    if (iPoint >= nPointCount)
    {
        if (!CheckPointCount(iPoint) || !setNumPoints(iPoint + 1))
            return false;
    }
#ifdef DEBUG
    if (paoPoints == nullptr)
        return false;
#endif

    paoPoints[iPoint].x = xIn;
    paoPoints[iPoint].y = yIn;

    if (padfZ != nullptr)
    {
        padfZ[iPoint] = zIn;
    }
    return true;
}

/**
 * \brief Set the location of a vertex in line string.
 *
 * If iPoint is larger than the number of necessary the number of existing
 * points in the line string, the point count will be increased to
 * accommodate the request.
 *
 * There is no SFCOM analog to this method.
 *
 * @param iPoint the index of the vertex to assign (zero based).
 * @param xIn input X coordinate to assign.
 * @param yIn input Y coordinate to assign.
 * @param mIn input M coordinate to assign (defaults to zero).
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setPointM(int iPoint, double xIn, double yIn, double mIn)

{
    if (!(flags & OGR_G_MEASURED))
    {
        if (!AddM())
            return false;
    }

    if (iPoint >= nPointCount)
    {
        if (!CheckPointCount(iPoint) || !setNumPoints(iPoint + 1))
            return false;
    }
#ifdef DEBUG
    if (paoPoints == nullptr)
        return false;
#endif

    paoPoints[iPoint].x = xIn;
    paoPoints[iPoint].y = yIn;

    if (padfM != nullptr)
    {
        padfM[iPoint] = mIn;
    }
    return true;
}

/**
 * \brief Set the location of a vertex in line string.
 *
 * If iPoint is larger than the number of necessary the number of existing
 * points in the line string, the point count will be increased to
 * accommodate the request.
 *
 * There is no SFCOM analog to this method.
 *
 * @param iPoint the index of the vertex to assign (zero based).
 * @param xIn input X coordinate to assign.
 * @param yIn input Y coordinate to assign.
 * @param zIn input Z coordinate to assign (defaults to zero).
 * @param mIn input M coordinate to assign (defaults to zero).
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setPoint(int iPoint, double xIn, double yIn, double zIn,
                              double mIn)

{
    if (!(flags & OGR_G_3D))
    {
        if (!Make3D())
            return false;
    }
    if (!(flags & OGR_G_MEASURED))
    {
        if (!AddM())
            return false;
    }

    if (iPoint >= nPointCount)
    {
        if (!CheckPointCount(iPoint) || !setNumPoints(iPoint + 1))
            return false;
    }
#ifdef DEBUG
    if (paoPoints == nullptr)
        return false;
#endif

    paoPoints[iPoint].x = xIn;
    paoPoints[iPoint].y = yIn;

    if (padfZ != nullptr)
    {
        padfZ[iPoint] = zIn;
    }
    if (padfM != nullptr)
    {
        padfM[iPoint] = mIn;
    }
    return true;
}

/**
 * \brief Set the location of a vertex in line string.
 *
 * If iPoint is larger than the number of necessary the number of existing
 * points in the line string, the point count will be increased to
 * accommodate the request.
 *
 * There is no SFCOM analog to this method.
 *
 * @param iPoint the index of the vertex to assign (zero based).
 * @param xIn input X coordinate to assign.
 * @param yIn input Y coordinate to assign.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setPoint(int iPoint, double xIn, double yIn)

{
    if (iPoint >= nPointCount)
    {
        if (!CheckPointCount(iPoint) || !setNumPoints(iPoint + 1) || !paoPoints)
            return false;
    }

    paoPoints[iPoint].x = xIn;
    paoPoints[iPoint].y = yIn;
    return true;
}

/************************************************************************/
/*                                setZ()                                */
/************************************************************************/

/**
 * \brief Set the Z of a vertex in line string.
 *
 * If iPoint is larger than the number of necessary the number of existing
 * points in the line string, the point count will be increased to
 * accommodate the request.
 *
 * There is no SFCOM analog to this method.
 *
 * @param iPoint the index of the vertex to assign (zero based).
 * @param zIn input Z coordinate to assign.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setZ(int iPoint, double zIn)
{
    if (getCoordinateDimension() == 2)
    {
        if (!Make3D())
            return false;
    }

    if (iPoint >= nPointCount)
    {
        if (!CheckPointCount(iPoint) || !setNumPoints(iPoint + 1))
            return false;
    }

    if (padfZ != nullptr)
        padfZ[iPoint] = zIn;
    return true;
}

/************************************************************************/
/*                                setM()                                */
/************************************************************************/

/**
 * \brief Set the M of a vertex in line string.
 *
 * If iPoint is larger than the number of necessary the number of existing
 * points in the line string, the point count will be increased to
 * accommodate the request.
 *
 * There is no SFCOM analog to this method.
 *
 * @param iPoint the index of the vertex to assign (zero based).
 * @param mIn input M coordinate to assign.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setM(int iPoint, double mIn)
{
    if (!(flags & OGR_G_MEASURED))
    {
        if (!AddM())
            return false;
    }

    if (iPoint >= nPointCount)
    {
        if (!CheckPointCount(iPoint) || !setNumPoints(iPoint + 1))
            return false;
    }

    if (padfM != nullptr)
        padfM[iPoint] = mIn;
    return true;
}

/************************************************************************/
/*                              addPoint()                              */
/************************************************************************/

/**
 * \brief Add a point to a line string.
 *
 * The vertex count of the line string is increased by one, and assigned from
 * the passed location value.
 *
 * There is no SFCOM analog to this method.
 *
 * @param poPoint the point to assign to the new vertex.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::addPoint(const OGRPoint *poPoint)

{
    if (poPoint->Is3D() && poPoint->IsMeasured())
        return setPoint(nPointCount, poPoint->getX(), poPoint->getY(),
                        poPoint->getZ(), poPoint->getM());
    else if (poPoint->Is3D())
        return setPoint(nPointCount, poPoint->getX(), poPoint->getY(),
                        poPoint->getZ());
    else if (poPoint->IsMeasured())
        return setPointM(nPointCount, poPoint->getX(), poPoint->getY(),
                         poPoint->getM());
    else
        return setPoint(nPointCount, poPoint->getX(), poPoint->getY());
}

/************************************************************************/
/*                              addPoint()                              */
/************************************************************************/

/**
 * \brief Add a point to a line string.
 *
 * The vertex count of the line string is increased by one, and assigned from
 * the passed location value.
 *
 * There is no SFCOM analog to this method.
 *
 * @param x the X coordinate to assign to the new point.
 * @param y the Y coordinate to assign to the new point.
 * @param z the Z coordinate to assign to the new point (defaults to zero).
 * @param m the M coordinate to assign to the new point (defaults to zero).
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::addPoint(double x, double y, double z, double m)

{
    return setPoint(nPointCount, x, y, z, m);
}

/**
 * \brief Add a point to a line string.
 *
 * The vertex count of the line string is increased by one, and assigned from
 * the passed location value.
 *
 * There is no SFCOM analog to this method.
 *
 * @param x the X coordinate to assign to the new point.
 * @param y the Y coordinate to assign to the new point.
 * @param z the Z coordinate to assign to the new point (defaults to zero).
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::addPoint(double x, double y, double z)

{
    return setPoint(nPointCount, x, y, z);
}

/**
 * \brief Add a point to a line string.
 *
 * The vertex count of the line string is increased by one, and assigned from
 * the passed location value.
 *
 * There is no SFCOM analog to this method.
 *
 * @param x the X coordinate to assign to the new point.
 * @param y the Y coordinate to assign to the new point.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::addPoint(double x, double y)

{
    return setPoint(nPointCount, x, y);
}

/**
 * \brief Add a point to a line string.
 *
 * The vertex count of the line string is increased by one, and assigned from
 * the passed location value.
 *
 * There is no SFCOM analog to this method.
 *
 * @param x the X coordinate to assign to the new point.
 * @param y the Y coordinate to assign to the new point.
 * @param m the M coordinate to assign to the new point.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::addPointM(double x, double y, double m)

{
    return setPointM(nPointCount, x, y, m);
}

/************************************************************************/
/*                            removePoint()                             */
/************************************************************************/

/**
 * \brief Remove a point from a line string.
 *
 * There is no SFCOM analog to this method.
 *
 * @param nIndex Point index
 * @since GDAL 3.3
 */

bool OGRSimpleCurve::removePoint(int nIndex)
{
    if (nIndex < 0 || nIndex >= nPointCount)
        return false;
    if (nIndex < nPointCount - 1)
    {
        memmove(paoPoints + nIndex, paoPoints + nIndex + 1,
                sizeof(OGRRawPoint) * (nPointCount - 1 - nIndex));
        if (padfZ)
        {
            memmove(padfZ + nIndex, padfZ + nIndex + 1,
                    sizeof(double) * (nPointCount - 1 - nIndex));
        }
        if (padfM)
        {
            memmove(padfM + nIndex, padfM + nIndex + 1,
                    sizeof(double) * (nPointCount - 1 - nIndex));
        }
    }
    nPointCount--;
    return true;
}

/************************************************************************/
/*                             setPointsM()                             */
/************************************************************************/

/**
 * \brief Assign all points in a line string.
 *
 * This method clears any existing points assigned to this line string,
 * and assigns a whole new set.  It is the most efficient way of assigning
 * the value of a line string.
 *
 * There is no SFCOM analog to this method.
 *
 * @param nPointsIn number of points being passed in paoPointsIn
 * @param paoPointsIn list of points being assigned.
 * @param padfMIn the M values that go with the points.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setPointsM(int nPointsIn, const OGRRawPoint *paoPointsIn,
                                const double *padfMIn)

{
    if (!setNumPoints(nPointsIn, FALSE)
#ifdef DEBUG
        || paoPoints == nullptr
#endif
    )
        return false;

    if (nPointsIn)
        memcpy(paoPoints, paoPointsIn, sizeof(OGRRawPoint) * nPointsIn);

    /* -------------------------------------------------------------------- */
    /*      Check measures.                                                 */
    /* -------------------------------------------------------------------- */
    if (padfMIn == nullptr && (flags & OGR_G_MEASURED))
    {
        RemoveM();
    }
    else if (padfMIn)
    {
        if (!AddM())
            return false;
        if (padfM && nPointsIn)
            memcpy(padfM, padfMIn, sizeof(double) * nPointsIn);
    }
    return true;
}

/************************************************************************/
/*                             setPoints()                              */
/************************************************************************/

/**
 * \brief Assign all points in a line string.
 *
 * This method clears any existing points assigned to this line string,
 * and assigns a whole new set.  It is the most efficient way of assigning
 * the value of a line string.
 *
 * There is no SFCOM analog to this method.
 *
 * @param nPointsIn number of points being passed in paoPointsIn
 * @param paoPointsIn list of points being assigned.
 * @param padfZIn the Z values that go with the points.
 * @param padfMIn the M values that go with the points.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setPoints(int nPointsIn, const OGRRawPoint *paoPointsIn,
                               const double *padfZIn, const double *padfMIn)

{
    if (!setNumPoints(nPointsIn, FALSE)
#ifdef DEBUG
        || paoPoints == nullptr
#endif
    )
        return false;

    if (nPointsIn)
        memcpy(paoPoints, paoPointsIn, sizeof(OGRRawPoint) * nPointsIn);

    /* -------------------------------------------------------------------- */
    /*      Check 2D/3D.                                                    */
    /* -------------------------------------------------------------------- */
    if (padfZIn == nullptr && getCoordinateDimension() > 2)
    {
        Make2D();
    }
    else if (padfZIn)
    {
        if (!Make3D())
            return false;
        if (padfZ && nPointsIn)
            memcpy(padfZ, padfZIn, sizeof(double) * nPointsIn);
    }

    /* -------------------------------------------------------------------- */
    /*      Check measures.                                                 */
    /* -------------------------------------------------------------------- */
    if (padfMIn == nullptr && (flags & OGR_G_MEASURED))
    {
        RemoveM();
    }
    else if (padfMIn)
    {
        if (!AddM())
            return false;
        if (padfM && nPointsIn)
            memcpy(padfM, padfMIn, sizeof(double) * nPointsIn);
    }
    return true;
}

/************************************************************************/
/*                             setPoints()                              */
/************************************************************************/

/**
 * \brief Assign all points in a line string.
 *
 * This method clears any existing points assigned to this line string,
 * and assigns a whole new set.  It is the most efficient way of assigning
 * the value of a line string.
 *
 * There is no SFCOM analog to this method.
 *
 * @param nPointsIn number of points being passed in paoPointsIn
 * @param paoPointsIn list of points being assigned.
 * @param padfZIn the Z values that go with the points (optional, may be NULL).
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setPoints(int nPointsIn, const OGRRawPoint *paoPointsIn,
                               const double *padfZIn)

{
    if (!setNumPoints(nPointsIn, FALSE)
#ifdef DEBUG
        || paoPoints == nullptr
#endif
    )
        return false;

    if (nPointsIn)
    {
        const void *pUnaligned = paoPointsIn;
        memcpy(paoPoints, pUnaligned, sizeof(OGRRawPoint) * nPointsIn);
    }

    /* -------------------------------------------------------------------- */
    /*      Check 2D/3D.                                                    */
    /* -------------------------------------------------------------------- */
    if (padfZIn == nullptr && getCoordinateDimension() > 2)
    {
        Make2D();
    }
    else if (padfZIn)
    {
        if (!Make3D())
            return false;
        if (padfZ && nPointsIn)
            memcpy(padfZ, padfZIn, sizeof(double) * nPointsIn);
    }
    return true;
}

/************************************************************************/
/*                             setPoints()                              */
/************************************************************************/

/**
 * \brief Assign all points in a line string.
 *
 * This method clear any existing points assigned to this line string,
 * and assigns a whole new set.
 *
 * There is no SFCOM analog to this method.
 *
 * @param nPointsIn number of points being passed in padfX and padfY.
 * @param padfX list of X coordinates of points being assigned.
 * @param padfY list of Y coordinates of points being assigned.
 * @param padfZIn list of Z coordinates of points being assigned (defaults to
 * NULL for 2D objects).
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setPoints(int nPointsIn, const double *padfX,
                               const double *padfY, const double *padfZIn)

{
    /* -------------------------------------------------------------------- */
    /*      Check 2D/3D.                                                    */
    /* -------------------------------------------------------------------- */
    if (padfZIn == nullptr)
        Make2D();
    else
    {
        if (!Make3D())
            return false;
    }

    /* -------------------------------------------------------------------- */
    /*      Assign values.                                                  */
    /* -------------------------------------------------------------------- */
    if (!setNumPoints(nPointsIn, FALSE))
        return false;

    for (int i = 0; i < nPointsIn; i++)
    {
        paoPoints[i].x = padfX[i];
        paoPoints[i].y = padfY[i];
    }

    if (padfZ && padfZIn && nPointsIn)
    {
        memcpy(padfZ, padfZIn, sizeof(double) * nPointsIn);
    }
    return true;
}

/************************************************************************/
/*                             setPointsM()                             */
/************************************************************************/

/**
 * \brief Assign all points in a line string.
 *
 * This method clear any existing points assigned to this line string,
 * and assigns a whole new set.
 *
 * There is no SFCOM analog to this method.
 *
 * @param nPointsIn number of points being passed in padfX and padfY.
 * @param padfX list of X coordinates of points being assigned.
 * @param padfY list of Y coordinates of points being assigned.
 * @param padfMIn list of M coordinates of points being assigned.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setPointsM(int nPointsIn, const double *padfX,
                                const double *padfY, const double *padfMIn)

{
    /* -------------------------------------------------------------------- */
    /*      Check 2D/3D.                                                    */
    /* -------------------------------------------------------------------- */
    if (padfMIn == nullptr)
        RemoveM();
    else
    {
        if (!AddM())
            return false;
    }

    /* -------------------------------------------------------------------- */
    /*      Assign values.                                                  */
    /* -------------------------------------------------------------------- */
    if (!setNumPoints(nPointsIn, FALSE))
        return false;

    for (int i = 0; i < nPointsIn; i++)
    {
        paoPoints[i].x = padfX[i];
        paoPoints[i].y = padfY[i];
    }

    if (padfMIn && padfM && nPointsIn)
    {
        memcpy(padfM, padfMIn, sizeof(double) * nPointsIn);
    }
    return true;
}

/************************************************************************/
/*                             setPoints()                              */
/************************************************************************/

/**
 * \brief Assign all points in a line string.
 *
 * This method clear any existing points assigned to this line string,
 * and assigns a whole new set.
 *
 * There is no SFCOM analog to this method.
 *
 * @param nPointsIn number of points being passed in padfX and padfY.
 * @param padfX list of X coordinates of points being assigned.
 * @param padfY list of Y coordinates of points being assigned.
 * @param padfZIn list of Z coordinates of points being assigned.
 * @param padfMIn list of M coordinates of points being assigned.
 * @return (since 3.10) true in case of success, false in case of memory allocation error
 */

bool OGRSimpleCurve::setPoints(int nPointsIn, const double *padfX,
                               const double *padfY, const double *padfZIn,
                               const double *padfMIn)

{
    /* -------------------------------------------------------------------- */
    /*      Check 2D/3D.                                                    */
    /* -------------------------------------------------------------------- */
    if (padfZIn == nullptr)
        Make2D();
    else
    {
        if (!Make3D())
            return false;
    }

    /* -------------------------------------------------------------------- */
    /*      Check measures.                                                 */
    /* -------------------------------------------------------------------- */
    if (padfMIn == nullptr)
        RemoveM();
    else
    {
        if (!AddM())
            return false;
    }

    /* -------------------------------------------------------------------- */
    /*      Assign values.                                                  */
    /* -------------------------------------------------------------------- */
    if (!setNumPoints(nPointsIn, FALSE))
        return false;

    for (int i = 0; i < nPointsIn; i++)
    {
        paoPoints[i].x = padfX[i];
        paoPoints[i].y = padfY[i];
    }

    if (padfZ != nullptr && padfZIn && nPointsIn)
        memcpy(padfZ, padfZIn, sizeof(double) * nPointsIn);
    if (padfM != nullptr && padfMIn && nPointsIn)
        memcpy(padfM, padfMIn, sizeof(double) * nPointsIn);
    return true;
}

/************************************************************************/
/*                          getPoints()                                 */
/************************************************************************/

/**
 * \brief Returns all points of line string.
 *
 * This method copies all points into user list. This list must be at
 * least sizeof(OGRRawPoint) * OGRGeometry::getNumPoints() byte in size.
 * It also copies all Z coordinates.
 *
 * There is no SFCOM analog to this method.
 *
 * @param paoPointsOut a buffer into which the points is written.
 * @param padfZOut the Z values that go with the points (optional, may be NULL).
 */

void OGRSimpleCurve::getPoints(OGRRawPoint *paoPointsOut,
                               double *padfZOut) const
{
    if (!paoPointsOut || nPointCount == 0)
        return;

    void *pUnaligned = paoPointsOut;
    memcpy(pUnaligned, paoPoints, sizeof(OGRRawPoint) * nPointCount);

    /* -------------------------------------------------------------------- */
    /*      Check 2D/3D.                                                    */
    /* -------------------------------------------------------------------- */
    if (padfZOut)
    {
        if (padfZ)
            memcpy(padfZOut, padfZ, sizeof(double) * nPointCount);
        else
            memset(padfZOut, 0, sizeof(double) * nPointCount);
    }
}

/**
 * \brief Returns all points of line string.
 *
 * This method copies all points into user arrays. The user provides the
 * stride between 2 consecutive elements of the array.
 *
 * On some CPU architectures, care must be taken so that the arrays are properly
 * aligned.
 *
 * There is no SFCOM analog to this method.
 *
 * @param pabyX a buffer of at least (nXStride * nPointCount) bytes, may be
 * NULL.
 * @param nXStride the number of bytes between 2 elements of pabyX.
 * @param pabyY a buffer of at least (nYStride * nPointCount) bytes, may be
 * NULL.
 * @param nYStride the number of bytes between 2 elements of pabyY.
 * @param pabyZ a buffer of at last size (nZStride * nPointCount) bytes, may be
 * NULL.
 * @param nZStride the number of bytes between 2 elements of pabyZ.
 * @param pabyM a buffer of at last size (nMStride * nPointCount) bytes, may be
 * NULL.
 * @param nMStride the number of bytes between 2 elements of pabyM.
 *
 * @since OGR 2.1.0
 */

void OGRSimpleCurve::getPoints(void *pabyX, int nXStride, void *pabyY,
                               int nYStride, void *pabyZ, int nZStride,
                               void *pabyM, int nMStride) const
{
    if (pabyX != nullptr && nXStride == 0)
        return;
    if (pabyY != nullptr && nYStride == 0)
        return;
    if (pabyZ != nullptr && nZStride == 0)
        return;
    if (pabyM != nullptr && nMStride == 0)
        return;
    if (nXStride == sizeof(OGRRawPoint) && nYStride == sizeof(OGRRawPoint) &&
        static_cast<char *>(pabyY) ==
            static_cast<char *>(pabyX) + sizeof(double) &&
        (pabyZ == nullptr || nZStride == sizeof(double)))
    {
        getPoints(static_cast<OGRRawPoint *>(pabyX),
                  static_cast<double *>(pabyZ));
    }
    else
    {
        for (int i = 0; i < nPointCount; i++)
        {
            if (pabyX)
                *reinterpret_cast<double *>(static_cast<char *>(pabyX) +
                                            i * nXStride) = paoPoints[i].x;
            if (pabyY)
                *reinterpret_cast<double *>(static_cast<char *>(pabyY) +
                                            i * nYStride) = paoPoints[i].y;
        }

        if (pabyZ)
        {
            if (nZStride == sizeof(double))
            {
                if (padfZ)
                    memcpy(pabyZ, padfZ, sizeof(double) * nPointCount);
                else
                    memset(pabyZ, 0, sizeof(double) * nPointCount);
            }
            else
            {
                for (int i = 0; i < nPointCount; i++)
                {
                    *reinterpret_cast<double *>(static_cast<char *>(pabyZ) +
                                                i * nZStride) =
                        (padfZ) ? padfZ[i] : 0.0;
                }
            }
        }
    }
    if (pabyM)
    {
        if (nMStride == sizeof(double))
        {
            if (padfM)
                memcpy(pabyM, padfM, sizeof(double) * nPointCount);
            else
                memset(pabyM, 0, sizeof(double) * nPointCount);
        }
        else
        {
            for (int i = 0; i < nPointCount; i++)
            {
                *reinterpret_cast<double *>(static_cast<char *>(pabyM) +
                                            i * nMStride) =
                    (padfM) ? padfM[i] : 0.0;
            }
        }
    }
}

/************************************************************************/
/*                           reversePoints()                            */
/************************************************************************/

/**
 * \brief Reverse point order.
 *
 * This method updates the points in this line string in place
 * reversing the point ordering (first for last, etc).
 */

void OGRSimpleCurve::reversePoints()

{
    for (int i = 0; i < nPointCount / 2; i++)
    {
        std::swap(paoPoints[i], paoPoints[nPointCount - i - 1]);
        if (padfZ)
        {
            std::swap(padfZ[i], padfZ[nPointCount - i - 1]);
        }

        if (padfM)
        {
            std::swap(padfM[i], padfM[nPointCount - i - 1]);
        }
    }
}

/************************************************************************/
/*                          addSubLineString()                          */
/************************************************************************/

/**
 * \brief Add a segment of another linestring to this one.
 *
 * Adds the request range of vertices to the end of this line string
 * in an efficient manner.  If the nStartVertex is larger than the
 * nEndVertex then the vertices will be reversed as they are copied.
 *
 * @param poOtherLine the other OGRLineString.
 * @param nStartVertex the first vertex to copy, defaults to 0 to start
 * with the first vertex in the other linestring.
 * @param nEndVertex the last vertex to copy, defaults to -1 indicating
 * the last vertex of the other line string.
 */

void OGRSimpleCurve::addSubLineString(const OGRLineString *poOtherLine,
                                      int nStartVertex, int nEndVertex)

{
    int nOtherLineNumPoints = poOtherLine->getNumPoints();
    if (nOtherLineNumPoints == 0)
        return;

    /* -------------------------------------------------------------------- */
    /*      Do a bit of argument defaulting and validation.                 */
    /* -------------------------------------------------------------------- */
    if (nEndVertex == -1)
        nEndVertex = nOtherLineNumPoints - 1;

    if (nStartVertex < 0 || nEndVertex < 0 ||
        nStartVertex >= nOtherLineNumPoints ||
        nEndVertex >= nOtherLineNumPoints)
    {
        CPLAssert(false);
        return;
    }

    /* -------------------------------------------------------------------- */
    /*      Grow this linestring to hold the additional points.             */
    /* -------------------------------------------------------------------- */
    int nOldPoints = nPointCount;
    int nPointsToAdd = std::abs(nEndVertex - nStartVertex) + 1;

    if (!setNumPoints(nPointsToAdd + nOldPoints, FALSE)
#ifdef DEBUG
        || paoPoints == nullptr
#endif
    )
        return;

    /* -------------------------------------------------------------------- */
    /*      Copy the x/y points - forward copies use memcpy.                */
    /* -------------------------------------------------------------------- */
    if (nEndVertex >= nStartVertex)
    {
        memcpy(paoPoints + nOldPoints, poOtherLine->paoPoints + nStartVertex,
               sizeof(OGRRawPoint) * nPointsToAdd);
        if (poOtherLine->padfZ != nullptr)
        {
            Make3D();
            if (padfZ != nullptr)
            {
                memcpy(padfZ + nOldPoints, poOtherLine->padfZ + nStartVertex,
                       sizeof(double) * nPointsToAdd);
            }
        }
        if (poOtherLine->padfM != nullptr)
        {
            AddM();
            if (padfM != nullptr)
            {
                memcpy(padfM + nOldPoints, poOtherLine->padfM + nStartVertex,
                       sizeof(double) * nPointsToAdd);
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Copy the x/y points - reverse copies done double by double.     */
    /* -------------------------------------------------------------------- */
    else
    {
        for (int i = 0; i < nPointsToAdd; i++)
        {
            paoPoints[i + nOldPoints].x =
                poOtherLine->paoPoints[nStartVertex - i].x;
            paoPoints[i + nOldPoints].y =
                poOtherLine->paoPoints[nStartVertex - i].y;
        }

        if (poOtherLine->padfZ != nullptr)
        {
            Make3D();
            if (padfZ != nullptr)
            {
                for (int i = 0; i < nPointsToAdd; i++)
                {
                    padfZ[i + nOldPoints] =
                        poOtherLine->padfZ[nStartVertex - i];
                }
            }
        }
        if (poOtherLine->padfM != nullptr)
        {
            AddM();
            if (padfM != nullptr)
            {
                for (int i = 0; i < nPointsToAdd; i++)
                {
                    padfM[i + nOldPoints] =
                        poOtherLine->padfM[nStartVertex - i];
                }
            }
        }
    }
}

/************************************************************************/
/*                           importFromWkb()                            */
/*                                                                      */
/*      Initialize from serialized stream in well known binary          */
/*      format.                                                         */
/************************************************************************/

OGRErr OGRSimpleCurve::importFromWkb(const unsigned char *pabyData,
                                     size_t nSize, OGRwkbVariant eWkbVariant,
                                     size_t &nBytesConsumedOut)

{
    OGRwkbByteOrder eByteOrder;
    size_t nDataOffset = 0;
    int nNewNumPoints = 0;

    nBytesConsumedOut = 0;
    OGRErr eErr = importPreambleOfCollectionFromWkb(pabyData, nSize,
                                                    nDataOffset, eByteOrder, 16,
                                                    nNewNumPoints, eWkbVariant);
    if (eErr != OGRERR_NONE)
        return eErr;

    // Check if the wkb stream buffer is big enough to store
    // fetched number of points.
    const int dim = CoordinateDimension();
    const size_t nPointSize = dim * sizeof(double);
    if (nNewNumPoints < 0 ||
        static_cast<size_t>(nNewNumPoints) >
            std::numeric_limits<size_t>::max() / nPointSize)
    {
        return OGRERR_CORRUPT_DATA;
    }
    const size_t nBufferMinSize = nPointSize * nNewNumPoints;

    if (nSize != static_cast<size_t>(-1) && nBufferMinSize > nSize)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Length of input WKB is too small");
        return OGRERR_NOT_ENOUGH_DATA;
    }

    if (!setNumPoints(nNewNumPoints, FALSE))
        return OGRERR_NOT_ENOUGH_MEMORY;

    nBytesConsumedOut = 9 + 8 * static_cast<size_t>(nPointCount) *
                                (2 + ((flags & OGR_G_3D) ? 1 : 0) +
                                 ((flags & OGR_G_MEASURED) ? 1 : 0));

    /* -------------------------------------------------------------------- */
    /*      Get the vertex.                                                 */
    /* -------------------------------------------------------------------- */
    if ((flags & OGR_G_3D) && (flags & OGR_G_MEASURED))
    {
        for (size_t i = 0; i < static_cast<size_t>(nPointCount); i++)
        {
            memcpy(paoPoints + i, pabyData + 9 + i * 32, 16);
            memcpy(padfZ + i, pabyData + 9 + 16 + i * 32, 8);
            memcpy(padfM + i, pabyData + 9 + 24 + i * 32, 8);
        }
    }
    else if (flags & OGR_G_MEASURED)
    {
        for (size_t i = 0; i < static_cast<size_t>(nPointCount); i++)
        {
            memcpy(paoPoints + i, pabyData + 9 + i * 24, 16);
            memcpy(padfM + i, pabyData + 9 + 16 + i * 24, 8);
        }
    }
    else if (flags & OGR_G_3D)
    {
        for (size_t i = 0; i < static_cast<size_t>(nPointCount); i++)
        {
            memcpy(paoPoints + i, pabyData + 9 + i * 24, 16);
            memcpy(padfZ + i, pabyData + 9 + 16 + i * 24, 8);
        }
    }
    else if (nPointCount)
    {
        memcpy(paoPoints, pabyData + 9, 16 * static_cast<size_t>(nPointCount));
    }

    /* -------------------------------------------------------------------- */
    /*      Byte swap if needed.                                            */
    /* -------------------------------------------------------------------- */
    if (OGR_SWAP(eByteOrder))
    {
        for (size_t i = 0; i < static_cast<size_t>(nPointCount); i++)
        {
            CPL_SWAPDOUBLE(&(paoPoints[i].x));
            CPL_SWAPDOUBLE(&(paoPoints[i].y));
        }

        if (flags & OGR_G_3D)
        {
            for (size_t i = 0; i < static_cast<size_t>(nPointCount); i++)
            {
                CPL_SWAPDOUBLE(padfZ + i);
            }
        }

        if (flags & OGR_G_MEASURED)
        {
            for (size_t i = 0; i < static_cast<size_t>(nPointCount); i++)
            {
                CPL_SWAPDOUBLE(padfM + i);
            }
        }
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                            exportToWkb()                             */
/*                                                                      */
/*      Build a well known binary representation of this object.        */
/************************************************************************/

OGRErr OGRSimpleCurve::exportToWkb(unsigned char *pabyData,
                                   const OGRwkbExportOptions *psOptions) const

{
    if (psOptions == nullptr)
    {
        static const OGRwkbExportOptions defaultOptions;
        psOptions = &defaultOptions;
    }

    /* -------------------------------------------------------------------- */
    /*      Set the byte order.                                             */
    /* -------------------------------------------------------------------- */
    pabyData[0] = DB2_V72_UNFIX_BYTE_ORDER(
        static_cast<unsigned char>(psOptions->eByteOrder));

    /* -------------------------------------------------------------------- */
    /*      Set the geometry feature type.                                  */
    /* -------------------------------------------------------------------- */
    GUInt32 nGType = getGeometryType();

    if (psOptions->eWkbVariant == wkbVariantPostGIS1)
    {
        nGType = wkbFlatten(nGType);
        if (Is3D())
            // Explicitly set wkb25DBit.
            nGType =
                static_cast<OGRwkbGeometryType>(nGType | wkb25DBitInternalUse);
        if (IsMeasured())
            nGType = static_cast<OGRwkbGeometryType>(nGType | 0x40000000);
    }
    else if (psOptions->eWkbVariant == wkbVariantIso)
        nGType = getIsoGeometryType();

    if (psOptions->eByteOrder == wkbNDR)
    {
        CPL_LSBPTR32(&nGType);
    }
    else
    {
        CPL_MSBPTR32(&nGType);
    }

    memcpy(pabyData + 1, &nGType, 4);

    /* -------------------------------------------------------------------- */
    /*      Copy in the data count.                                         */
    /* -------------------------------------------------------------------- */
    memcpy(pabyData + 5, &nPointCount, 4);

    /* -------------------------------------------------------------------- */
    /*      Copy in the raw data.                                           */
    /* -------------------------------------------------------------------- */
    if ((flags & OGR_G_3D) && (flags & OGR_G_MEASURED))
    {
        for (size_t i = 0; i < static_cast<size_t>(nPointCount); i++)
        {
            memcpy(pabyData + 9 + 32 * i, paoPoints + i, 16);
            memcpy(pabyData + 9 + 16 + 32 * i, padfZ + i, 8);
            memcpy(pabyData + 9 + 24 + 32 * i, padfM + i, 8);
        }
        OGRRoundCoordinatesIEEE754XYValues<32>(
            psOptions->sPrecision.nXYBitPrecision, pabyData + 9, nPointCount);
        OGRRoundCoordinatesIEEE754<32>(psOptions->sPrecision.nZBitPrecision,
                                       pabyData + 9 + 2 * sizeof(uint64_t),
                                       nPointCount);
        OGRRoundCoordinatesIEEE754<32>(psOptions->sPrecision.nMBitPrecision,
                                       pabyData + 9 + 3 * sizeof(uint64_t),
                                       nPointCount);
    }
    else if (flags & OGR_G_MEASURED)
    {
        for (size_t i = 0; i < static_cast<size_t>(nPointCount); i++)
        {
            memcpy(pabyData + 9 + 24 * i, paoPoints + i, 16);
            memcpy(pabyData + 9 + 16 + 24 * i, padfM + i, 8);
        }
        OGRRoundCoordinatesIEEE754XYValues<24>(
            psOptions->sPrecision.nXYBitPrecision, pabyData + 9, nPointCount);
        OGRRoundCoordinatesIEEE754<24>(psOptions->sPrecision.nMBitPrecision,
                                       pabyData + 9 + 2 * sizeof(uint64_t),
                                       nPointCount);
    }
    else if (flags & OGR_G_3D)
    {
        for (size_t i = 0; i < static_cast<size_t>(nPointCount); i++)
        {
            memcpy(pabyData + 9 + 24 * i, paoPoints + i, 16);
            memcpy(pabyData + 9 + 16 + 24 * i, padfZ + i, 8);
        }
        OGRRoundCoordinatesIEEE754XYValues<24>(
            psOptions->sPrecision.nXYBitPrecision, pabyData + 9, nPointCount);
        OGRRoundCoordinatesIEEE754<24>(psOptions->sPrecision.nZBitPrecision,
                                       pabyData + 9 + 2 * sizeof(uint64_t),
                                       nPointCount);
    }
    else if (nPointCount)
    {
        memcpy(pabyData + 9, paoPoints, 16 * static_cast<size_t>(nPointCount));
        OGRRoundCoordinatesIEEE754XYValues<16>(
            psOptions->sPrecision.nXYBitPrecision, pabyData + 9, nPointCount);
    }

    /* -------------------------------------------------------------------- */
    /*      Swap if needed.                                                 */
    /* -------------------------------------------------------------------- */
    if (OGR_SWAP(psOptions->eByteOrder))
    {
        const int nCount = CPL_SWAP32(nPointCount);
        memcpy(pabyData + 5, &nCount, 4);

        const size_t nCoords =
            CoordinateDimension() * static_cast<size_t>(nPointCount);
        for (size_t i = 0; i < nCoords; i++)
        {
            CPL_SWAP64PTR(pabyData + 9 + 8 * i);
        }
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                           importFromWkt()                            */
/*                                                                      */
/*      Instantiate from well known text format.  Currently this is     */
/*      `LINESTRING ( x y, x y, ...)',                                  */
/************************************************************************/

OGRErr OGRSimpleCurve::importFromWkt(const char **ppszInput)

{
    int bHasZ = FALSE;
    int bHasM = FALSE;
    bool bIsEmpty = false;
    const OGRErr eErr =
        importPreambleFromWkt(ppszInput, &bHasZ, &bHasM, &bIsEmpty);
    flags = 0;
    if (eErr != OGRERR_NONE)
        return eErr;
    if (bHasZ)
        flags |= OGR_G_3D;
    if (bHasM)
        flags |= OGR_G_MEASURED;
    if (bIsEmpty)
    {
        return OGRERR_NONE;
    }

    const char *pszInput = *ppszInput;

    /* -------------------------------------------------------------------- */
    /*      Read the point list.                                            */
    /* -------------------------------------------------------------------- */
    int flagsFromInput = flags;
    nPointCount = 0;

    pszInput =
        OGRWktReadPointsM(pszInput, &paoPoints, &padfZ, &padfM, &flagsFromInput,
                          &m_nPointCapacity, &nPointCount);
    if (pszInput == nullptr)
        return OGRERR_CORRUPT_DATA;

    if ((flagsFromInput & OGR_G_3D) && !(flags & OGR_G_3D))
    {
        if (!set3D(TRUE))
            return OGRERR_NOT_ENOUGH_MEMORY;
    }
    if ((flagsFromInput & OGR_G_MEASURED) && !(flags & OGR_G_MEASURED))
    {
        if (!setMeasured(TRUE))
            return OGRERR_NOT_ENOUGH_MEMORY;
    }

    *ppszInput = pszInput;

    return OGRERR_NONE;
}

//! @cond Doxygen_Suppress
/************************************************************************/
/*                        importFromWKTListOnly()                       */
/*                                                                      */
/*      Instantiate from "(x y, x y, ...)"                              */
/************************************************************************/

OGRErr OGRSimpleCurve::importFromWKTListOnly(const char **ppszInput, int bHasZ,
                                             int bHasM,
                                             OGRRawPoint *&paoPointsIn,
                                             int &nMaxPointsIn,
                                             double *&padfZIn)

{
    const char *pszInput = *ppszInput;

    /* -------------------------------------------------------------------- */
    /*      Read the point list.                                            */
    /* -------------------------------------------------------------------- */
    int flagsFromInput = flags;
    int nPointCountRead = 0;
    double *padfMIn = nullptr;
    if (flagsFromInput == 0)  // Flags was not set, this is not called by us.
    {
        if (bHasM)
            flagsFromInput |= OGR_G_MEASURED;
        if (bHasZ)
            flagsFromInput |= OGR_G_3D;
    }

    pszInput =
        OGRWktReadPointsM(pszInput, &paoPointsIn, &padfZIn, &padfMIn,
                          &flagsFromInput, &nMaxPointsIn, &nPointCountRead);

    if (pszInput == nullptr)
    {
        CPLFree(padfMIn);
        return OGRERR_CORRUPT_DATA;
    }
    if ((flagsFromInput & OGR_G_3D) && !(flags & OGR_G_3D))
    {
        flags |= OGR_G_3D;
        bHasZ = TRUE;
    }
    if ((flagsFromInput & OGR_G_MEASURED) && !(flags & OGR_G_MEASURED))
    {
        flags |= OGR_G_MEASURED;
        bHasM = TRUE;
    }

    *ppszInput = pszInput;

    if (bHasM && bHasZ)
        setPoints(nPointCountRead, paoPointsIn, padfZIn, padfMIn);
    else if (bHasM && !bHasZ)
        setPointsM(nPointCountRead, paoPointsIn, padfMIn);
    else
        setPoints(nPointCountRead, paoPointsIn, padfZIn);

    CPLFree(padfMIn);

    return OGRERR_NONE;
}

//! @endcond

/************************************************************************/
/*                            exportToWkt()                             */
/*                                                                      */
/*      Translate this structure into its well known text format       */
/*      equivalent.  This could be made a lot more CPU efficient.       */
/************************************************************************/

std::string OGRSimpleCurve::exportToWkt(const OGRWktOptions &opts,
                                        OGRErr *err) const
{
    // LINEARRING or LINESTRING or CIRCULARSTRING
    std::string wkt = getGeometryName();
    wkt += wktTypeString(opts.variant);
    if (IsEmpty())
    {
        wkt += "EMPTY";
    }
    else
    {
        wkt += '(';

        OGRBoolean hasZ = Is3D();
        OGRBoolean hasM =
            (opts.variant != wkbVariantIso ? FALSE : IsMeasured());

        try
        {
            const int nOrdinatesPerVertex =
                2 + ((hasZ) ? 1 : 0) + ((hasM) ? 1 : 0);
            // At least 2 bytes per ordinate: one for the value,
            // and one for the separator...
            wkt.reserve(wkt.size() + 2 * static_cast<size_t>(nPointCount) *
                                         nOrdinatesPerVertex);

            for (int i = 0; i < nPointCount; i++)
            {
                if (i > 0)
                    wkt += ',';

                wkt += OGRMakeWktCoordinateM(
                    paoPoints[i].x, paoPoints[i].y, padfZ ? padfZ[i] : 0.0,
                    padfM ? padfM[i] : 0.0, hasZ, hasM, opts);
            }
            wkt += ')';
        }
        catch (const std::bad_alloc &e)
        {
            CPLError(CE_Failure, CPLE_OutOfMemory, "%s", e.what());
            if (err)
                *err = OGRERR_FAILURE;
            return std::string();
        }
    }
    if (err)
        *err = OGRERR_NONE;
    return wkt;
}

/************************************************************************/
/*                             get_Length()                             */
/*                                                                      */
/*      For now we return a simple euclidean 2D distance.               */
/************************************************************************/

double OGRSimpleCurve::get_Length() const

{
    double dfLength = 0.0;

    for (int i = 0; i < nPointCount - 1; i++)
    {

        const double dfDeltaX = paoPoints[i + 1].x - paoPoints[i].x;
        const double dfDeltaY = paoPoints[i + 1].y - paoPoints[i].y;
        dfLength += sqrt(dfDeltaX * dfDeltaX + dfDeltaY * dfDeltaY);
    }

    return dfLength;
}

/************************************************************************/
/*                             StartPoint()                             */
/************************************************************************/

void OGRSimpleCurve::StartPoint(OGRPoint *poPoint) const

{
    getPoint(0, poPoint);
}

/************************************************************************/
/*                              EndPoint()                              */
/************************************************************************/

void OGRSimpleCurve::EndPoint(OGRPoint *poPoint) const

{
    getPoint(nPointCount - 1, poPoint);
}

/************************************************************************/
/*                               Value()                                */
/*                                                                      */
/*      Get an interpolated point at some distance along the curve.     */
/************************************************************************/

void OGRSimpleCurve::Value(double dfDistance, OGRPoint *poPoint) const

{
    if (dfDistance < 0)
    {
        StartPoint(poPoint);
        return;
    }

    double dfLength = 0.0;

    for (int i = 0; i < nPointCount - 1; i++)
    {
        const double dfDeltaX = paoPoints[i + 1].x - paoPoints[i].x;
        const double dfDeltaY = paoPoints[i + 1].y - paoPoints[i].y;
        const double dfSegLength =
            sqrt(dfDeltaX * dfDeltaX + dfDeltaY * dfDeltaY);

        if (dfSegLength > 0)
        {
            if ((dfLength <= dfDistance) &&
                ((dfLength + dfSegLength) >= dfDistance))
            {
                double dfRatio = (dfDistance - dfLength) / dfSegLength;

                poPoint->setX(paoPoints[i].x * (1 - dfRatio) +
                              paoPoints[i + 1].x * dfRatio);
                poPoint->setY(paoPoints[i].y * (1 - dfRatio) +
                              paoPoints[i + 1].y * dfRatio);

                if (getCoordinateDimension() == 3)
                    poPoint->setZ(padfZ[i] * (1 - dfRatio) +
                                  padfZ[i + 1] * dfRatio);

                return;
            }

            dfLength += dfSegLength;
        }
    }

    EndPoint(poPoint);
}

/************************************************************************/
/*                              Project()                               */
/*                                                                      */
/* Return distance of point projected on line from origin of this line. */
/************************************************************************/

/**
 * \brief Project point on linestring.
 *
 * The input point projected on linestring. This is the shortest distance
 * from point to the linestring. The distance from begin of linestring to
 * the point projection returned.
 *
 * This method is built on the GEOS library. Check it for the
 * definition of the geometry operation.
 * If OGR is built without the GEOS library, this method will always return -1,
 * issuing a CPLE_NotSupported error.
 *
 * @return a distance from the begin of the linestring to the projected point.
 */

double OGRSimpleCurve::Project(const OGRPoint *poPoint) const

{
    double dfResult = -1;
#ifndef HAVE_GEOS
    CPL_IGNORE_RET_VAL(poPoint);
    CPLError(CE_Failure, CPLE_NotSupported, "GEOS support not enabled.");
    return dfResult;
#else
    GEOSGeom hThisGeosGeom = nullptr;
    GEOSGeom hPointGeosGeom = nullptr;

    GEOSContextHandle_t hGEOSCtxt = createGEOSContext();
    hThisGeosGeom = exportToGEOS(hGEOSCtxt);
    hPointGeosGeom = poPoint->exportToGEOS(hGEOSCtxt);
    if (hThisGeosGeom != nullptr && hPointGeosGeom != nullptr)
    {
        dfResult = GEOSProject_r(hGEOSCtxt, hThisGeosGeom, hPointGeosGeom);
    }
    GEOSGeom_destroy_r(hGEOSCtxt, hThisGeosGeom);
    GEOSGeom_destroy_r(hGEOSCtxt, hPointGeosGeom);
    freeGEOSContext(hGEOSCtxt);

    return dfResult;

#endif  // HAVE_GEOS
}

/************************************************************************/
/*                            getSubLine()                              */
/*                                                                      */
/*  Extracts a portion of this OGRLineString into a new OGRLineString.  */
/************************************************************************/

/**
 * \brief Get the portion of linestring.
 *
 * The portion of the linestring extracted to new one. The input distances
 * (maybe present as ratio of length of linestring) set begin and end of
 * extracted portion.
 *
 * @param dfDistanceFrom The distance from the origin of linestring, where the
 * subline should begins
 * @param dfDistanceTo The distance from the origin of linestring, where the
 * subline should ends
 * @param bAsRatio The flag indicating that distances are the ratio of the
 * linestring length.
 *
 * @return a newly allocated linestring now owned by the caller, or NULL on
 * failure.
 *
 * @since OGR 1.11.0
 */

OGRLineString *OGRSimpleCurve::getSubLine(double dfDistanceFrom,
                                          double dfDistanceTo,
                                          int bAsRatio) const

{
    auto poNewLineString = std::make_unique<OGRLineString>();

    poNewLineString->assignSpatialReference(getSpatialReference());
    poNewLineString->setCoordinateDimension(getCoordinateDimension());

    const double dfLen = get_Length();
    if (bAsRatio == TRUE)
    {
        // Convert to real distance.
        dfDistanceFrom *= dfLen;
        dfDistanceTo *= dfLen;
    }

    if (dfDistanceFrom < 0)
        dfDistanceFrom = 0;
    if (dfDistanceTo > dfLen)
        dfDistanceTo = dfLen;

    if (dfDistanceFrom > dfDistanceTo || dfDistanceFrom >= dfLen)
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Input distances are invalid.");

        return nullptr;
    }

    double dfLength = 0.0;

    // Get first point.

    int i = 0;  // Used after if blocks.
    if (dfDistanceFrom == 0)
    {
        bool bRet;
        if (getCoordinateDimension() == 3)
            bRet = poNewLineString->addPoint(paoPoints[0].x, paoPoints[0].y,
                                             padfZ[0]);
        else
            bRet = poNewLineString->addPoint(paoPoints[0].x, paoPoints[0].y);
        if (!bRet)
            return nullptr;
    }
    else
    {
        for (i = 0; i < nPointCount - 1; i++)
        {
            const double dfDeltaX = paoPoints[i + 1].x - paoPoints[i].x;
            const double dfDeltaY = paoPoints[i + 1].y - paoPoints[i].y;
            const double dfSegLength =
                sqrt(dfDeltaX * dfDeltaX + dfDeltaY * dfDeltaY);

            if (dfSegLength > 0)
            {
                if ((dfLength <= dfDistanceFrom) &&
                    ((dfLength + dfSegLength) >= dfDistanceFrom))
                {
                    double dfRatio = (dfDistanceFrom - dfLength) / dfSegLength;

                    double dfX = paoPoints[i].x * (1 - dfRatio) +
                                 paoPoints[i + 1].x * dfRatio;
                    double dfY = paoPoints[i].y * (1 - dfRatio) +
                                 paoPoints[i + 1].y * dfRatio;

                    bool bRet;
                    if (getCoordinateDimension() == 3)
                    {
                        bRet = poNewLineString->addPoint(
                            dfX, dfY,
                            padfZ[i] * (1 - dfRatio) + padfZ[i + 1] * dfRatio);
                    }
                    else
                    {
                        bRet = poNewLineString->addPoint(dfX, dfY);
                    }
                    if (!bRet)
                        return nullptr;

                    // Check if dfDistanceTo is in same segment.
                    if (dfLength <= dfDistanceTo &&
                        (dfLength + dfSegLength) >= dfDistanceTo)
                    {
                        dfRatio = (dfDistanceTo - dfLength) / dfSegLength;

                        dfX = paoPoints[i].x * (1 - dfRatio) +
                              paoPoints[i + 1].x * dfRatio;
                        dfY = paoPoints[i].y * (1 - dfRatio) +
                              paoPoints[i + 1].y * dfRatio;

                        if (getCoordinateDimension() == 3)
                        {
                            bRet = poNewLineString->addPoint(
                                dfX, dfY,
                                padfZ[i] * (1 - dfRatio) +
                                    padfZ[i + 1] * dfRatio);
                        }
                        else
                        {
                            bRet = poNewLineString->addPoint(dfX, dfY);
                        }

                        if (!bRet || poNewLineString->getNumPoints() < 2)
                        {
                            return nullptr;
                        }

                        return poNewLineString.release();
                    }
                    i++;
                    dfLength += dfSegLength;
                    break;
                }

                dfLength += dfSegLength;
            }
        }
    }

    // Add points.
    for (; i < nPointCount - 1; i++)
    {
        bool bRet;
        if (getCoordinateDimension() == 3)
            bRet = poNewLineString->addPoint(paoPoints[i].x, paoPoints[i].y,
                                             padfZ[i]);
        else
            bRet = poNewLineString->addPoint(paoPoints[i].x, paoPoints[i].y);
        if (!bRet)
            return nullptr;

        const double dfDeltaX = paoPoints[i + 1].x - paoPoints[i].x;
        const double dfDeltaY = paoPoints[i + 1].y - paoPoints[i].y;
        const double dfSegLength =
            sqrt(dfDeltaX * dfDeltaX + dfDeltaY * dfDeltaY);

        if (dfSegLength > 0)
        {
            if ((dfLength <= dfDistanceTo) &&
                ((dfLength + dfSegLength) >= dfDistanceTo))
            {
                const double dfRatio = (dfDistanceTo - dfLength) / dfSegLength;

                const double dfX = paoPoints[i].x * (1 - dfRatio) +
                                   paoPoints[i + 1].x * dfRatio;
                const double dfY = paoPoints[i].y * (1 - dfRatio) +
                                   paoPoints[i + 1].y * dfRatio;

                if (getCoordinateDimension() == 3)
                    bRet = poNewLineString->addPoint(
                        dfX, dfY,
                        padfZ[i] * (1 - dfRatio) + padfZ[i + 1] * dfRatio);
                else
                    bRet = poNewLineString->addPoint(dfX, dfY);
                if (!bRet)
                    return nullptr;

                return poNewLineString.release();
            }

            dfLength += dfSegLength;
        }
    }

    bool bRet;
    if (getCoordinateDimension() == 3)
        bRet = poNewLineString->addPoint(paoPoints[nPointCount - 1].x,
                                         paoPoints[nPointCount - 1].y,
                                         padfZ[nPointCount - 1]);
    else
        bRet = poNewLineString->addPoint(paoPoints[nPointCount - 1].x,
                                         paoPoints[nPointCount - 1].y);

    if (!bRet || poNewLineString->getNumPoints() < 2)
    {
        return nullptr;
    }

    return poNewLineString.release();
}

/************************************************************************/
/*                            getEnvelope()                             */
/************************************************************************/

void OGRSimpleCurve::getEnvelope(OGREnvelope *psEnvelope) const

{
    if (IsEmpty())
    {
        psEnvelope->MinX = 0.0;
        psEnvelope->MaxX = 0.0;
        psEnvelope->MinY = 0.0;
        psEnvelope->MaxY = 0.0;
        return;
    }

    double dfMinX = paoPoints[0].x;
    double dfMaxX = paoPoints[0].x;
    double dfMinY = paoPoints[0].y;
    double dfMaxY = paoPoints[0].y;

    for (int iPoint = 1; iPoint < nPointCount; iPoint++)
    {
        if (dfMaxX < paoPoints[iPoint].x)
            dfMaxX = paoPoints[iPoint].x;
        if (dfMaxY < paoPoints[iPoint].y)
            dfMaxY = paoPoints[iPoint].y;
        if (dfMinX > paoPoints[iPoint].x)
            dfMinX = paoPoints[iPoint].x;
        if (dfMinY > paoPoints[iPoint].y)
            dfMinY = paoPoints[iPoint].y;
    }

    psEnvelope->MinX = dfMinX;
    psEnvelope->MaxX = dfMaxX;
    psEnvelope->MinY = dfMinY;
    psEnvelope->MaxY = dfMaxY;
}

/************************************************************************/
/*                            getEnvelope()                             */
/************************************************************************/

void OGRSimpleCurve::getEnvelope(OGREnvelope3D *psEnvelope) const

{
    getEnvelope(static_cast<OGREnvelope *>(psEnvelope));

    if (IsEmpty() || padfZ == nullptr)
    {
        psEnvelope->MinZ = 0.0;
        psEnvelope->MaxZ = 0.0;
        return;
    }

    double dfMinZ = padfZ[0];
    double dfMaxZ = padfZ[0];

    for (int iPoint = 1; iPoint < nPointCount; iPoint++)
    {
        if (dfMinZ > padfZ[iPoint])
            dfMinZ = padfZ[iPoint];
        if (dfMaxZ < padfZ[iPoint])
            dfMaxZ = padfZ[iPoint];
    }

    psEnvelope->MinZ = dfMinZ;
    psEnvelope->MaxZ = dfMaxZ;
}

/************************************************************************/
/*                               Equals()                               */
/************************************************************************/

OGRBoolean OGRSimpleCurve::Equals(const OGRGeometry *poOther) const

{
    if (poOther == this)
        return TRUE;

    if (poOther->getGeometryType() != getGeometryType())
        return FALSE;

    if (IsEmpty() && poOther->IsEmpty())
        return TRUE;

    // TODO(schwehr): Test the SRS.

    auto poOLine = poOther->toSimpleCurve();
    if (getNumPoints() != poOLine->getNumPoints())
        return FALSE;

    for (int iPoint = 0; iPoint < getNumPoints(); iPoint++)
    {
        if (getX(iPoint) != poOLine->getX(iPoint) ||
            getY(iPoint) != poOLine->getY(iPoint) ||
            getZ(iPoint) != poOLine->getZ(iPoint))
            return FALSE;
    }

    return TRUE;
}

/************************************************************************/
/*                             transform()                              */
/************************************************************************/

OGRErr OGRSimpleCurve::transform(OGRCoordinateTransformation *poCT)

{
    /* -------------------------------------------------------------------- */
    /*   Make a copy of the points to operate on, so as to be able to       */
    /*   keep only valid reprojected points if partial reprojection enabled */
    /*   or keeping intact the original geometry if only full reprojection  */
    /*   allowed.                                                           */
    /* -------------------------------------------------------------------- */
    double *xyz = static_cast<double *>(
        VSI_MALLOC_VERBOSE(sizeof(double) * nPointCount * 3));
    int *pabSuccess =
        static_cast<int *>(VSI_CALLOC_VERBOSE(sizeof(int), nPointCount));
    if (xyz == nullptr || pabSuccess == nullptr)
    {
        VSIFree(xyz);
        VSIFree(pabSuccess);
        return OGRERR_NOT_ENOUGH_MEMORY;
    }

    for (int i = 0; i < nPointCount; i++)
    {
        xyz[i] = paoPoints[i].x;
        xyz[i + nPointCount] = paoPoints[i].y;
        if (padfZ)
            xyz[i + nPointCount * 2] = padfZ[i];
        else
            xyz[i + nPointCount * 2] = 0.0;
    }

    /* -------------------------------------------------------------------- */
    /*      Transform and reapply.                                          */
    /* -------------------------------------------------------------------- */
    poCT->Transform(nPointCount, xyz, xyz + nPointCount, xyz + nPointCount * 2,
                    nullptr, pabSuccess);

    const char *pszEnablePartialReprojection = nullptr;

    int j = 0;  // Used after for.
    for (int i = 0; i < nPointCount; i++)
    {
        if (pabSuccess[i])
        {
            xyz[j] = xyz[i];
            xyz[j + nPointCount] = xyz[i + nPointCount];
            xyz[j + 2 * nPointCount] = xyz[i + 2 * nPointCount];
            j++;
        }
        else
        {
            if (pszEnablePartialReprojection == nullptr)
                pszEnablePartialReprojection = CPLGetConfigOption(
                    "OGR_ENABLE_PARTIAL_REPROJECTION", nullptr);
            if (pszEnablePartialReprojection == nullptr)
            {
                static bool bHasWarned = false;
                if (!bHasWarned)
                {
                    // Check that there is at least one valid reprojected point
                    // and issue an error giving an hint to use
                    // OGR_ENABLE_PARTIAL_REPROJECTION.
                    bool bHasOneValidPoint = j != 0;
                    for (; i < nPointCount && !bHasOneValidPoint; i++)
                    {
                        if (pabSuccess[i])
                            bHasOneValidPoint = true;
                    }
                    if (bHasOneValidPoint)
                    {
                        bHasWarned = true;
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Full reprojection failed, but partial is "
                                 "possible if you define "
                                 "OGR_ENABLE_PARTIAL_REPROJECTION "
                                 "configuration option to TRUE");
                    }
                }

                CPLFree(xyz);
                CPLFree(pabSuccess);
                return OGRERR_FAILURE;
            }
            else if (!CPLTestBool(pszEnablePartialReprojection))
            {
                CPLFree(xyz);
                CPLFree(pabSuccess);
                return OGRERR_FAILURE;
            }
        }
    }

    if (j == 0 && nPointCount != 0)
    {
        CPLFree(xyz);
        CPLFree(pabSuccess);
        return OGRERR_FAILURE;
    }

    setPoints(j, xyz, xyz + nPointCount,
              (padfZ) ? xyz + nPointCount * 2 : nullptr);
    CPLFree(xyz);
    CPLFree(pabSuccess);

    assignSpatialReference(poCT->GetTargetCS());

    return OGRERR_NONE;
}

/************************************************************************/
/*                               IsEmpty()                              */
/************************************************************************/

OGRBoolean OGRSimpleCurve::IsEmpty() const
{
    return (nPointCount == 0);
}

/************************************************************************/
/*                     OGRSimpleCurve::segmentize()                     */
/************************************************************************/

bool OGRSimpleCurve::segmentize(double dfMaxLength)
{
    if (dfMaxLength <= 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "dfMaxLength must be strictly positive");
        return false;
    }
    if (nPointCount < 2)
        return true;

    // So as to make sure that the same line followed in both directions
    // result in the same segmentized line.
    if (paoPoints[0].x < paoPoints[nPointCount - 1].x ||
        (paoPoints[0].x == paoPoints[nPointCount - 1].x &&
         paoPoints[0].y < paoPoints[nPointCount - 1].y))
    {
        reversePoints();
        bool bRet = segmentize(dfMaxLength);
        reversePoints();
        return bRet;
    }

    int nNewPointCount = 0;
    const double dfSquareMaxLength = dfMaxLength * dfMaxLength;

    // First pass to compute new number of points
    constexpr double REL_EPSILON_LENGTH_SQUARE = 1e-5;
    constexpr double REL_EPSILON_ROUND = 1e-2;
    for (int i = 0; i < nPointCount; i++)
    {
        nNewPointCount++;

        if (i == nPointCount - 1)
            break;

        // Must be kept in sync with the second pass loop
        const double dfX = paoPoints[i + 1].x - paoPoints[i].x;
        const double dfY = paoPoints[i + 1].y - paoPoints[i].y;
        const double dfSquareDist = dfX * dfX + dfY * dfY;
        if (dfSquareDist - dfSquareMaxLength >
            REL_EPSILON_LENGTH_SQUARE * dfSquareMaxLength)
        {
            const double dfIntermediatePoints = floor(
                sqrt(dfSquareDist / dfSquareMaxLength) - REL_EPSILON_ROUND);
            const int nIntermediatePoints =
                DoubleToIntClamp(dfIntermediatePoints);

            // TODO(schwehr): Can these be tighter?
            // Limit allocation of paoNewPoints to a few GB of memory.
            // An OGRRawPoint is 2 doubles.
            // kMax is a guess of what a reasonable max might be.
            constexpr int kMax = 2 << 26;
            if (nNewPointCount > kMax || nIntermediatePoints > kMax)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Too many points in a segment: %d or %d",
                         nNewPointCount, nIntermediatePoints);
                return false;
            }

            nNewPointCount += nIntermediatePoints;
        }
    }

    if (nPointCount == nNewPointCount)
        return true;

    // Allocate new arrays
    OGRRawPoint *paoNewPoints = static_cast<OGRRawPoint *>(
        VSI_MALLOC_VERBOSE(sizeof(OGRRawPoint) * nNewPointCount));
    if (paoNewPoints == nullptr)
        return false;
    double *padfNewZ = nullptr;
    double *padfNewM = nullptr;
    if (padfZ != nullptr)
    {
        padfNewZ = static_cast<double *>(
            VSI_MALLOC_VERBOSE(sizeof(double) * nNewPointCount));
        if (padfNewZ == nullptr)
        {
            VSIFree(paoNewPoints);
            return false;
        }
    }
    if (padfM != nullptr)
    {
        padfNewM = static_cast<double *>(
            VSI_MALLOC_VERBOSE(sizeof(double) * nNewPointCount));
        if (padfNewM == nullptr)
        {
            VSIFree(paoNewPoints);
            VSIFree(padfNewZ);
            return false;
        }
    }

    // Second pass to fill new arrays
    // Must be kept in sync with the first pass loop
    nNewPointCount = 0;
    for (int i = 0; i < nPointCount; i++)
    {
        paoNewPoints[nNewPointCount] = paoPoints[i];

        if (padfZ != nullptr)
        {
            padfNewZ[nNewPointCount] = padfZ[i];
        }

        if (padfM != nullptr)
        {
            padfNewM[nNewPointCount] = padfM[i];
        }

        nNewPointCount++;

        if (i == nPointCount - 1)
            break;

        const double dfX = paoPoints[i + 1].x - paoPoints[i].x;
        const double dfY = paoPoints[i + 1].y - paoPoints[i].y;
        const double dfSquareDist = dfX * dfX + dfY * dfY;

        // Must be kept in sync with the initial pass loop
        if (dfSquareDist - dfSquareMaxLength >
            REL_EPSILON_LENGTH_SQUARE * dfSquareMaxLength)
        {
            const double dfIntermediatePoints = floor(
                sqrt(dfSquareDist / dfSquareMaxLength) - REL_EPSILON_ROUND);
            const int nIntermediatePoints =
                DoubleToIntClamp(dfIntermediatePoints);
            const double dfRatioX =
                dfX / (static_cast<double>(nIntermediatePoints) + 1);
            const double dfRatioY =
                dfY / (static_cast<double>(nIntermediatePoints) + 1);

            for (int j = 1; j <= nIntermediatePoints; j++)
            {
                // coverity[overflow_const]
                const int newI = nNewPointCount + j - 1;
                paoNewPoints[newI].x = paoPoints[i].x + j * dfRatioX;
                paoNewPoints[newI].y = paoPoints[i].y + j * dfRatioY;
                if (padfZ != nullptr)
                {
                    // No interpolation.
                    padfNewZ[newI] = padfZ[i];
                }
                if (padfM != nullptr)
                {
                    // No interpolation.
                    padfNewM[newI] = padfM[i];
                }
            }

            nNewPointCount += nIntermediatePoints;
        }
    }

    CPLFree(paoPoints);
    paoPoints = paoNewPoints;
    nPointCount = nNewPointCount;
    m_nPointCapacity = nNewPointCount;

    if (padfZ != nullptr)
    {
        CPLFree(padfZ);
        padfZ = padfNewZ;
    }
    if (padfM != nullptr)
    {
        CPLFree(padfM);
        padfM = padfNewM;
    }
    return true;
}

/************************************************************************/
/*                               swapXY()                               */
/************************************************************************/

void OGRSimpleCurve::swapXY()
{
    for (int i = 0; i < nPointCount; i++)
    {
        std::swap(paoPoints[i].x, paoPoints[i].y);
    }
}

/************************************************************************/
/*                       OGRSimpleCurvePointIterator                    */
/************************************************************************/

class OGRSimpleCurvePointIterator final : public OGRPointIterator
{
    CPL_DISALLOW_COPY_ASSIGN(OGRSimpleCurvePointIterator)

    const OGRSimpleCurve *poSC = nullptr;
    int iCurPoint = 0;

  public:
    explicit OGRSimpleCurvePointIterator(const OGRSimpleCurve *poSCIn)
        : poSC(poSCIn)
    {
    }

    OGRBoolean getNextPoint(OGRPoint *p) override;
};

/************************************************************************/
/*                            getNextPoint()                            */
/************************************************************************/

OGRBoolean OGRSimpleCurvePointIterator::getNextPoint(OGRPoint *p)
{
    if (iCurPoint >= poSC->getNumPoints())
        return FALSE;
    poSC->getPoint(iCurPoint, p);
    iCurPoint++;
    return TRUE;
}

/************************************************************************/
/*                         getPointIterator()                           */
/************************************************************************/

OGRPointIterator *OGRSimpleCurve::getPointIterator() const
{
    return new OGRSimpleCurvePointIterator(this);
}

/************************************************************************/
/*                  OGRLineString( const OGRLineString& )               */
/************************************************************************/

/**
 * \brief Copy constructor.
 *
 * Note: before GDAL 2.1, only the default implementation of the constructor
 * existed, which could be unsafe to use.
 *
 * @since GDAL 2.1
 */

OGRLineString::OGRLineString(const OGRLineString &) = default;

/************************************************************************/
/*                  OGRLineString( OGRLineString&& )                    */
/************************************************************************/

/**
 * \brief Move constructor.
 *
 * @since GDAL 3.11
 */

OGRLineString::OGRLineString(OGRLineString &&) = default;

/************************************************************************/
/*                    operator=( const OGRLineString& )                 */
/************************************************************************/

/**
 * \brief Assignment operator.
 *
 * Note: before GDAL 2.1, only the default implementation of the operator
 * existed, which could be unsafe to use.
 *
 * @since GDAL 2.1
 */

OGRLineString &OGRLineString::operator=(const OGRLineString &other)
{
    if (this != &other)
    {
        OGRSimpleCurve::operator=(other);
    }
    return *this;
}

/************************************************************************/
/*                    operator=( OGRLineString&& )                      */
/************************************************************************/

/**
 * \brief Move assignment operator.
 *
 * @since GDAL 3.11
 */

OGRLineString &OGRLineString::operator=(OGRLineString &&other)
{
    if (this != &other)
    {
        OGRSimpleCurve::operator=(std::move(other));
    }
    return *this;
}

/************************************************************************/
/*                          getGeometryType()                           */
/************************************************************************/

OGRwkbGeometryType OGRLineString::getGeometryType() const

{
    if ((flags & OGR_G_3D) && (flags & OGR_G_MEASURED))
        return wkbLineStringZM;
    else if (flags & OGR_G_MEASURED)
        return wkbLineStringM;
    else if (flags & OGR_G_3D)
        return wkbLineString25D;
    else
        return wkbLineString;
}

/************************************************************************/
/*                          getGeometryName()                           */
/************************************************************************/

const char *OGRLineString::getGeometryName() const

{
    return "LINESTRING";
}

/************************************************************************/
/*                          curveToLine()                               */
/************************************************************************/

OGRLineString *OGRLineString::CurveToLine(
    CPL_UNUSED double /* dfMaxAngleStepSizeDegrees */,
    CPL_UNUSED const char *const * /* papszOptions */) const
{
    return clone();
}

/************************************************************************/
/*                          get_LinearArea()                            */
/************************************************************************/

/**
 * \brief Compute area of ring / closed linestring.
 *
 * The area is computed according to Green's Theorem:
 *
 * Area is "Sum(x(i)*(y(i+1) - y(i-1)))/2" for i = 0 to pointCount-1,
 * assuming the last point is a duplicate of the first.
 *
 * @return computed area.
 */

double OGRSimpleCurve::get_LinearArea() const

{
    if (nPointCount < 2 ||
        (WkbSize() != 0 && /* if not a linearring, check it is closed */
         (paoPoints[0].x != paoPoints[nPointCount - 1].x ||
          paoPoints[0].y != paoPoints[nPointCount - 1].y)))
    {
        return 0;
    }

    double dfAreaSum =
        paoPoints[0].x * (paoPoints[1].y - paoPoints[nPointCount - 1].y);

    for (int i = 1; i < nPointCount - 1; i++)
    {
        dfAreaSum += paoPoints[i].x * (paoPoints[i + 1].y - paoPoints[i - 1].y);
    }

    dfAreaSum += paoPoints[nPointCount - 1].x *
                 (paoPoints[0].y - paoPoints[nPointCount - 2].y);

    return 0.5 * fabs(dfAreaSum);
}

/************************************************************************/
/*                             getCurveGeometry()                       */
/************************************************************************/

OGRGeometry *
OGRLineString::getCurveGeometry(const char *const *papszOptions) const
{
    return OGRGeometryFactory::curveFromLineString(this, papszOptions);
}

/************************************************************************/
/*                      TransferMembersAndDestroy()                     */
/************************************************************************/
//! @cond Doxygen_Suppress
OGRLineString *OGRLineString::TransferMembersAndDestroy(OGRLineString *poSrc,
                                                        OGRLineString *poDst)
{
    if (poSrc->Is3D())
        poDst->flags |= OGR_G_3D;
    if (poSrc->IsMeasured())
        poDst->flags |= OGR_G_MEASURED;
    poDst->assignSpatialReference(poSrc->getSpatialReference());
    poDst->nPointCount = poSrc->nPointCount;
    poDst->m_nPointCapacity = poSrc->m_nPointCapacity;
    poDst->paoPoints = poSrc->paoPoints;
    poDst->padfZ = poSrc->padfZ;
    poDst->padfM = poSrc->padfM;
    poSrc->nPointCount = 0;
    poSrc->m_nPointCapacity = 0;
    poSrc->paoPoints = nullptr;
    poSrc->padfZ = nullptr;
    poSrc->padfM = nullptr;
    delete poSrc;
    return poDst;
}

//! @endcond
/************************************************************************/
/*                         CastToLinearRing()                           */
/************************************************************************/

/**
 * \brief Cast to linear ring.
 *
 * The passed in geometry is consumed and a new one returned (or NULL in case
 * of failure)
 *
 * @param poLS the input geometry - ownership is passed to the method.
 * @return new geometry.
 */

OGRLinearRing *OGRLineString::CastToLinearRing(OGRLineString *poLS)
{
    if (poLS->nPointCount < 2 || !poLS->get_IsClosed())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot convert non-closed linestring to linearring");
        delete poLS;
        return nullptr;
    }
    OGRLinearRing *poLR = new OGRLinearRing();
    TransferMembersAndDestroy(poLS, poLR);
    return poLR;
}

/************************************************************************/
/*                               clone()                                */
/************************************************************************/

OGRLineString *OGRLineString::clone() const
{
    auto ret = new (std::nothrow) OGRLineString(*this);
    if (ret)
    {
        if (ret->getNumPoints() != getNumPoints())
        {
            delete ret;
            ret = nullptr;
        }
    }
    return ret;
}

//! @cond Doxygen_Suppress

/************************************************************************/
/*                     GetCasterToLineString()                          */
/************************************************************************/

static OGRLineString *CasterToLineString(OGRCurve *poCurve)
{
    return poCurve->toLineString();
}

OGRCurveCasterToLineString OGRLineString::GetCasterToLineString() const
{
    return ::CasterToLineString;
}

/************************************************************************/
/*                        GetCasterToLinearRing()                       */
/************************************************************************/

OGRLinearRing *OGRLineString::CasterToLinearRing(OGRCurve *poCurve)
{
    return OGRLineString::CastToLinearRing(poCurve->toLineString());
}

OGRCurveCasterToLinearRing OGRLineString::GetCasterToLinearRing() const
{
    return OGRLineString::CasterToLinearRing;
}

/************************************************************************/
/*                            get_Area()                                */
/************************************************************************/

double OGRLineString::get_Area() const
{
    return get_LinearArea();
}

/************************************************************************/
/*                           GetGeodesicInputs()                        */
/************************************************************************/

static bool GetGeodesicInputs(const OGRLineString *poLS,
                              const OGRSpatialReference *poSRSOverride,
                              const char *pszComputationType, geod_geodesic &g,
                              std::vector<double> &adfLat,
                              std::vector<double> &adfLon)
{
    if (!poSRSOverride)
        poSRSOverride = poLS->getSpatialReference();

    if (!poSRSOverride)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot compute %s on ellipsoid due to missing SRS",
                 pszComputationType);
        return false;
    }

    OGRErr eErr = OGRERR_NONE;
    double dfSemiMajor = poSRSOverride->GetSemiMajor(&eErr);
    if (eErr != OGRERR_NONE)
        return false;
    const double dfInvFlattening = poSRSOverride->GetInvFlattening(&eErr);
    if (eErr != OGRERR_NONE)
        return false;

    geod_init(&g, dfSemiMajor,
              dfInvFlattening != 0 ? 1.0 / dfInvFlattening : 0.0);

    const int nPointCount = poLS->getNumPoints();
    adfLat.reserve(nPointCount);
    adfLon.reserve(nPointCount);

    OGRSpatialReference oGeogCRS;
    if (oGeogCRS.CopyGeogCSFrom(poSRSOverride) != OGRERR_NONE)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot reproject geometry to geographic CRS");
        return false;
    }
    oGeogCRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto poCT = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(poSRSOverride, &oGeogCRS));
    if (!poCT)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot reproject geometry to geographic CRS");
        return false;
    }
    for (int i = 0; i < nPointCount; ++i)
    {
        adfLon.push_back(poLS->getX(i));
        adfLat.push_back(poLS->getY(i));
    }
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif
    std::vector<int> anSuccess;
    anSuccess.resize(adfLon.size());
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
    poCT->Transform(adfLon.size(), adfLon.data(), adfLat.data(), nullptr,
                    anSuccess.data());
    double dfToDegrees =
        oGeogCRS.GetAngularUnits(nullptr) / CPLAtof(SRS_UA_DEGREE_CONV);
    if (std::fabs(dfToDegrees - 1) <= 1e-10)
        dfToDegrees = 1.0;
    for (int i = 0; i < nPointCount; ++i)
    {
        if (!anSuccess[i])
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot reproject geometry to geographic CRS");
            return false;
        }
        adfLon[i] *= dfToDegrees;
        adfLat[i] *= dfToDegrees;
    }

    return true;
}

/************************************************************************/
/*                        get_GeodesicArea()                            */
/************************************************************************/

double
OGRLineString::get_GeodesicArea(const OGRSpatialReference *poSRSOverride) const
{
    geod_geodesic g;
    std::vector<double> adfLat;
    std::vector<double> adfLon;
    if (!GetGeodesicInputs(this, poSRSOverride, "area", g, adfLat, adfLon))
        return -1.0;
    double dfArea = -1.0;
    geod_polygonarea(&g, adfLat.data(), adfLon.data(),
                     static_cast<int>(adfLat.size()), &dfArea, nullptr);
    return std::fabs(dfArea);
}

/************************************************************************/
/*                        get_GeodesicLength()                          */
/************************************************************************/

double OGRLineString::get_GeodesicLength(
    const OGRSpatialReference *poSRSOverride) const
{
    geod_geodesic g;
    std::vector<double> adfLat;
    std::vector<double> adfLon;
    if (!GetGeodesicInputs(this, poSRSOverride, "length", g, adfLat, adfLon))
        return -1.0;
    double dfLength = 0;
    for (size_t i = 0; i + 1 < adfLon.size(); ++i)
    {
        double dfSegmentLength = 0;
        geod_inverse(&g, adfLat[i], adfLon[i], adfLat[i + 1], adfLon[i + 1],
                     &dfSegmentLength, nullptr, nullptr);
        dfLength += dfSegmentLength;
    }
    return dfLength;
}

/************************************************************************/
/*                       get_AreaOfCurveSegments()                      */
/************************************************************************/

double OGRLineString::get_AreaOfCurveSegments() const
{
    return 0;
}

/************************************************************************/
/*                            isClockwise()                             */
/************************************************************************/

/**
 * \brief Returns TRUE if the ring has clockwise winding (or less than 2 points)
 *
 * Assumes that the line is closed.
 *
 * @return TRUE if clockwise otherwise FALSE.
 */

int OGRLineString::isClockwise() const

{
    // WARNING: keep in sync OGRLineString::isClockwise(),
    // OGRCurve::isClockwise() and OGRWKBIsClockwiseRing()

    if (nPointCount < 2)
        return TRUE;

    bool bUseFallback = false;

    // Find the lowest rightmost vertex.
    int v = 0;  // Used after for.
    for (int i = 1; i < nPointCount - 1; i++)
    {
        // => v < end.
        if (paoPoints[i].y < paoPoints[v].y ||
            (paoPoints[i].y == paoPoints[v].y &&
             paoPoints[i].x > paoPoints[v].x))
        {
            v = i;
            bUseFallback = false;
        }
        else if (paoPoints[i].y == paoPoints[v].y &&
                 paoPoints[i].x == paoPoints[v].x)
        {
            // Two vertex with same coordinates are the lowest rightmost
            // vertex.  Cannot use that point as the pivot (#5342).
            bUseFallback = true;
        }
    }

    // Previous.
    int next = v - 1;
    if (next < 0)
    {
        next = nPointCount - 1 - 1;
    }

    constexpr double EPSILON = 1.0E-5;
    const auto epsilonEqual = [](double a, double b, double eps)
    { return ::fabs(a - b) < eps; };

    if (epsilonEqual(paoPoints[next].x, paoPoints[v].x, EPSILON) &&
        epsilonEqual(paoPoints[next].y, paoPoints[v].y, EPSILON))
    {
        // Don't try to be too clever by retrying with a next point.
        // This can lead to false results as in the case of #3356.
        bUseFallback = true;
    }

    const double dx0 = paoPoints[next].x - paoPoints[v].x;
    const double dy0 = paoPoints[next].y - paoPoints[v].y;

    // Following.
    next = v + 1;
    if (next >= nPointCount - 1)
    {
        next = 0;
    }

    if (epsilonEqual(paoPoints[next].x, paoPoints[v].x, EPSILON) &&
        epsilonEqual(paoPoints[next].y, paoPoints[v].y, EPSILON))
    {
        // Don't try to be too clever by retrying with a next point.
        // This can lead to false results as in the case of #3356.
        bUseFallback = true;
    }

    const double dx1 = paoPoints[next].x - paoPoints[v].x;
    const double dy1 = paoPoints[next].y - paoPoints[v].y;

    const double crossproduct = dx1 * dy0 - dx0 * dy1;

    if (!bUseFallback)
    {
        if (crossproduct > 0)  // CCW
            return FALSE;
        else if (crossproduct < 0)  // CW
            return TRUE;
    }

    // This is a degenerate case: the extent of the polygon is less than EPSILON
    // or 2 nearly identical points were found.
    // Try with Green Formula as a fallback, but this is not a guarantee
    // as we'll probably be affected by numerical instabilities.

    double dfSum =
        paoPoints[0].x * (paoPoints[1].y - paoPoints[nPointCount - 1].y);

    for (int i = 1; i < nPointCount - 1; i++)
    {
        dfSum += paoPoints[i].x * (paoPoints[i + 1].y - paoPoints[i - 1].y);
    }

    dfSum += paoPoints[nPointCount - 1].x *
             (paoPoints[0].y - paoPoints[nPointCount - 2].y);

    return dfSum < 0;
}

//! @endcond
