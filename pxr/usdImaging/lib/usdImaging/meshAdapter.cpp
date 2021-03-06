//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/usdImaging/usdImaging/meshAdapter.h"

#include "pxr/usdImaging/usdImaging/debugCodes.h"
#include "pxr/usdImaging/usdImaging/delegate.h"
#include "pxr/usdImaging/usdImaging/tokens.h"

#include "pxr/imaging/hd/mesh.h"
#include "pxr/imaging/hd/perfLog.h"

#include "pxr/imaging/pxOsd/meshTopology.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/xformCache.h"

#include "pxr/base/tf/type.h"

PXR_NAMESPACE_OPEN_SCOPE


TF_REGISTRY_FUNCTION(TfType)
{
    typedef UsdImagingMeshAdapter Adapter;
    TfType t = TfType::Define<Adapter, TfType::Bases<Adapter::BaseAdapter> >();
    t.SetFactory< UsdImagingPrimAdapterFactory<Adapter> >();
}

UsdImagingMeshAdapter::~UsdImagingMeshAdapter()
{
}

SdfPath
UsdImagingMeshAdapter::Populate(UsdPrim const& prim,
                            UsdImagingIndexProxy* index,
                            UsdImagingInstancerContext const* instancerContext)
{
    index->InsertMesh(prim.GetPath(),
                      GetShaderBinding(prim),
                      instancerContext);
    HD_PERF_COUNTER_INCR(UsdImagingTokens->usdPopulatedPrimCount);

    return prim.GetPath();
}

void
UsdImagingMeshAdapter::TrackVariabilityPrep(UsdPrim const& prim,
                                            SdfPath const& cachePath,
                                            int requestedBits,
                                            UsdImagingInstancerContext const* 
                                                instancerContext)
{
    // Let the base class track what it needs.
    BaseAdapter::TrackVariabilityPrep(
        prim, cachePath, requestedBits, instancerContext);
}

void
UsdImagingMeshAdapter::TrackVariability(UsdPrim const& prim,
                                        SdfPath const& cachePath,
                                        int requestedBits,
                                        int* dirtyBits,
                                        UsdImagingInstancerContext const* 
                                            instancerContext)
{
    BaseAdapter::TrackVariability(
        prim, cachePath, requestedBits, dirtyBits, instancerContext);
    // WARNING: This method is executed from multiple threads, the value cache
    // has been carefully pre-populated to avoid mutating the underlying
    // container during update.

    if (requestedBits & HdChangeTracker::DirtyPoints) {
        // Discover time-varying points.
        _IsVarying(prim,
                   UsdGeomTokens->points,
                   HdChangeTracker::DirtyPoints,
                   UsdImagingTokens->usdVaryingPrimVar,
                   dirtyBits,
                   /*isInherited*/false);
    }

    if (requestedBits & HdChangeTracker::DirtyTopology) {
        // Discover time-varying topology.
        if (!_IsVarying(prim,
                           UsdGeomTokens->faceVertexCounts,
                           HdChangeTracker::DirtyTopology,
                           UsdImagingTokens->usdVaryingTopology,
                           dirtyBits,
                           /*isInherited*/false)) {
            // Only do this check if the faceVertexCounts is not already known
            // to be varying.
            if (!_IsVarying(prim,
                               UsdGeomTokens->faceVertexIndices,
                               HdChangeTracker::DirtyTopology,
                               UsdImagingTokens->usdVaryingTopology,
                               dirtyBits,
                               /*isInherited*/false)) {
                // Only do this check if both faceVertexCounts and
                // faceVertexIndices are not known to be varying.
                _IsVarying(prim,
                           UsdGeomTokens->holeIndices,
                           HdChangeTracker::DirtyTopology,
                           UsdImagingTokens->usdVaryingTopology,
                           dirtyBits,
                           /*isInherited*/false);
            }
        }
    }
}

void
UsdImagingMeshAdapter::UpdateForTimePrep(UsdPrim const& prim,
                                   SdfPath const& cachePath,
                                   UsdTimeCode time,
                                   int requestedBits,
                                   UsdImagingInstancerContext const* 
                                       instancerContext)
{
    BaseAdapter::UpdateForTimePrep(
        prim, cachePath, time, requestedBits, instancerContext);
    UsdImagingValueCache* valueCache = _GetValueCache();

    if (requestedBits & HdChangeTracker::DirtyPoints)
        valueCache->GetPoints(cachePath);

    if (requestedBits & HdChangeTracker::DirtySubdivTags)
        valueCache->GetSubdivTags(cachePath);

    if (requestedBits & HdChangeTracker::DirtyTopology)
        valueCache->GetTopology(cachePath);
}

void
UsdImagingMeshAdapter::UpdateForTime(UsdPrim const& prim,
                               SdfPath const& cachePath,
                               UsdTimeCode time,
                               int requestedBits,
                               int* resultBits,
                               UsdImagingInstancerContext const* 
                                   instancerContext)
{
    BaseAdapter::UpdateForTime(
        prim, cachePath, time, requestedBits, resultBits, instancerContext);

    UsdImagingValueCache* valueCache = _GetValueCache();
    PrimvarInfoVector& primvars = valueCache->GetPrimvars(cachePath);

    if (requestedBits & HdChangeTracker::DirtyTopology) {
        VtValue& topology = valueCache->GetTopology(cachePath);
        _GetMeshTopology(prim, &topology, time);
    }

    if (requestedBits & HdChangeTracker::DirtyPoints) {
        VtValue& points = valueCache->GetPoints(cachePath);
        _GetPoints(prim, &points, time);
        UsdImagingValueCache::PrimvarInfo primvar;
        primvar.name = HdTokens->points;
        primvar.interpolation = UsdGeomTokens->vertex;
        _MergePrimvar(primvar, &primvars);
    }

    if (requestedBits & HdChangeTracker::DirtySubdivTags) {
        SubdivTags& tags = valueCache->GetSubdivTags(cachePath);
        _GetSubdivTags(prim, &tags, time);
    }
}

int
UsdImagingMeshAdapter::ProcessPropertyChange(UsdPrim const& prim,
                                      SdfPath const& cachePath,
                                      TfToken const& propertyName)
{
    if(propertyName == UsdGeomTokens->points)
        return HdChangeTracker::DirtyPoints;

    // TODO: support sparse topology and subdiv tag changes

    // Allow base class to handle change processing.
    return BaseAdapter::ProcessPropertyChange(prim, cachePath, propertyName);
}

// -------------------------------------------------------------------------- //
// Private IO Helpers
// -------------------------------------------------------------------------- //

void
UsdImagingMeshAdapter::_GetMeshTopology(UsdPrim const& prim,
                                         VtValue* topo,
                                         UsdTimeCode time)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();
    TfToken schemeToken;
    _GetPtr(prim, UsdGeomTokens->subdivisionScheme, time, &schemeToken);
    if (schemeToken == UsdGeomTokens->none)
        schemeToken = PxOsdOpenSubdivTokens->bilinear;

    *topo = HdMeshTopology(
        schemeToken,
        _Get<TfToken>(prim, UsdGeomTokens->orientation, time),
        _Get<VtIntArray>(prim, UsdGeomTokens->faceVertexCounts, time),
        _Get<VtIntArray>(prim, UsdGeomTokens->faceVertexIndices, time),
        _Get<VtIntArray>(prim, UsdGeomTokens->holeIndices, time));
}

void
UsdImagingMeshAdapter::_GetPoints(UsdPrim const& prim,
                                   VtValue* value,
                                   UsdTimeCode time)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();
    if (!prim.GetAttribute(UsdGeomTokens->points).Get(value, time)) {
        *value = VtVec3fArray();
    }
}


void
UsdImagingMeshAdapter::_GetSubdivTags(UsdPrim const& prim,
                                       SubdivTags* tags,
                                       UsdTimeCode time)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    if(!prim.IsA<UsdGeomMesh>())
        return;

    TfToken token; VtIntArray iarray; VtFloatArray farray;

    _GetPtr(prim, UsdGeomTokens->interpolateBoundary, time, &token);
    tags->SetVertexInterpolationRule(token);

    auto meshPrim = UsdGeomMesh(prim);
    token = meshPrim.GetFaceVaryingLinearInterpolation(time);
    tags->SetFaceVaryingInterpolationRule(token);

    // XXX uncomment after fixing USD schema

    //_GetPtr(prim, UsdGeomTokens->creaseMethod, time, &token);
    //tags->SetCreaseMethod(token);

    //_GetPtr(prim, UsdGeomTokens->trianglesSubdivision, time, &token);
    //tags->SetTriangleSubdivision(token);


    _GetPtr(prim, UsdGeomTokens->creaseIndices, time, &iarray);
    tags->SetCreaseIndices(iarray);

    _GetPtr(prim, UsdGeomTokens->creaseLengths, time, &iarray);
    tags->SetCreaseLengths(iarray);

    _GetPtr(prim, UsdGeomTokens->creaseSharpnesses, time, &farray);
    tags->SetCreaseWeights(farray);

    _GetPtr(prim, UsdGeomTokens->cornerIndices, time, &iarray);
    tags->SetCornerIndices(iarray);

    _GetPtr(prim, UsdGeomTokens->cornerSharpnesses, time, &farray);
    tags->SetCornerWeights(farray);

    _GetPtr(prim, UsdGeomTokens->holeIndices, time, &iarray);
    tags->SetHoleIndices(iarray);
}


PXR_NAMESPACE_CLOSE_SCOPE

