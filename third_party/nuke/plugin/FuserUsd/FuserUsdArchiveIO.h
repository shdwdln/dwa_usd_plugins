//
// Copyright 2019 DreamWorks Animation
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

/// @file FuserUsdArchiveIO.h
///
/// @author Jonathan Egstad


#ifndef FuserUsdArchiveIO_h
#define FuserUsdArchiveIO_h

#include "FuserUsdNode.h"

#include <Fuser/ArgConstants.h> // for attrib names constants


#ifdef __GNUC__
// Turn off conversion warnings when including USD headers:
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wconversion"
#  pragma GCC diagnostic ignored "-Wfloat-conversion"
#endif

#include <pxr/usd/usd/stageCache.h>
#include <pxr/usd/usdUtils/stageCache.h>

#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif


namespace Fsr {


//-------------------------------------------------------------------------------


//!
Pxr::UsdPrim findMatchingPrimByType(const Pxr::UsdPrim& prim,
                                    const std::string&  prim_type,
                                    bool                allow_inactive_prims);

//!
Pxr::UsdPrim findFirstMatchingPrim(const Pxr::UsdStageRefPtr& stage,
                                   const std::string&         start_path,
                                   const std::string&         prim_type,
                                   bool                       allow_inactive_prims);


//-------------------------------------------------------------------------------


/*!
*/
class StageCacheReference
{
  protected:
    Pxr::SdfLayerRefPtr         m_root_layer;       //!< 
    Pxr::UsdStagePopulationMask m_populate_mask;    //!< Populate mask to use for stage open and retrieval
    std::string                 m_stage_id;         //!< Stage cache identifier string returned from Pxr::UsdStageCache
    Pxr::SdfLayerRefPtr         m_session_layer;    //!< This layer must be unique per stage hash so caches are also unique


  public:
    //!
    StageCacheReference() {}

    //! Copy ctor
    StageCacheReference(const StageCacheReference& b) { *this = b; }

    //! Copy operator
    const StageCacheReference& operator = (const StageCacheReference& b)
    {
        m_root_layer    = b.m_root_layer;
        m_populate_mask = b.m_populate_mask;
        m_stage_id      = b.m_stage_id;
        m_session_layer = b.m_session_layer;
        return *this;
    }

    //!
    const char* stageId() const { return m_stage_id.c_str(); }

    //!
    const Pxr::UsdStagePopulationMask& populateMask() const { return m_populate_mask; }


    //! Attempt to Load/Find the Stage to pass to the FuserUsdNodes.
    Pxr::UsdStageRefPtr getStage(const std::string& scene_file,
                                 const uint64_t     stage_hash,
                                 bool               debug_stage_loading=false);


  public:
    //! Create a shared StageCacheReference keyed by 'hash'. parent_path and stage_id are optional.
    static StageCacheReference* createStageReference(uint64_t                        hash,
                                                     const std::vector<std::string>& paths);

    //! Find a shared StageCacheReference keyed by 'hash'.
    static StageCacheReference* findStageReference(uint64_t hash);


};


//-------------------------------------------------------------------------------


/*! Manage the acquisition, querying and release of USD Stage caches.

*/
class FuserUsdArchiveIO : public Fsr::Node
{
  protected:
    Pxr::UsdStageRefPtr     m_stage;    // Assigned when there's an existing stage to operate on


  public:
    //! Returns the class name, must implement.
    /*virtual*/ const char* fuserNodeClass() const { return "FuserUsdArchiveIO"; }

    //! No stage exists yet.
    FuserUsdArchiveIO(const Fsr::ArgSet& args);

    //! Wrap a previously created stage.
    FuserUsdArchiveIO(const Pxr::UsdStageRefPtr& stage,
                      const Fsr::ArgSet&         args);


    //! Returns -1 on user-interrupt so processing can be interrupted.
    /*virtual*/ int _execute(const Fsr::NodeContext& target_context,
                             const char*             target_name,
                             void*                   target,
                             void*                   src0,
                             void*                   src1);

};


//-------------------------------------------------------------------------------


} // namespace Fsr

#endif

// end of FuserUsdArchiveIO.h

//
// Copyright 2019 DreamWorks Animation
//
