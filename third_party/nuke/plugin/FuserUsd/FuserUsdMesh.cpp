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

/// @file FuserUsdMesh.cpp
///
/// @author Jonathan Egstad


#include "FuserUsdMesh.h"
#include "FuserUsdShader.h"

#include <Fuser/ArgConstants.h> // for attrib names constants
#include <Fuser/ExecuteTargetContexts.h>
#include <Fuser/MeshPrimitive.h>
#include <Fuser/MeshUtils.h>
#include <Fuser/NodePrimitive.h>
#include <Fuser/GeoReader.h> // for getAttributeMapping()

#include <DDImage/GeoOp.h>
#include <DDImage/gl.h>
#include <DDImage/noise.h> // for prandom

#ifdef DWA_INTERNAL_BUILD
#  include <zprender/RenderContext.h> // for GenerateRenderPrimsContext
#endif


#if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 8)
#else
// Turn off -Wconversion warnings when including USD headers:
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wconversion"

#  include <pxr/base/tf/token.h>
#  include <pxr/base/gf/math.h>
#  include <pxr/base/gf/matrix3d.h>
#  include <pxr/base/gf/matrix4d.h>
#  include <pxr/base/gf/vec3d.h>

#  include <pxr/usd/usdShade/materialBindingAPI.h>

#  pragma GCC diagnostic pop
#endif

// Poly-reduction values for OpenGL display
#define STEP_THRESHOLD 1000
#define STEP_DIVISOR   1000

//#include <sys/time.h>


// TODO: Should we bake the mesh matrix into the point values?
//
// If we don't bake the points the DD::Image::RayCast Viewer
// object selection crashes, and I'm not sure why... There may
// be an assert in DDImage that's testing for whether a point
// location is inside the faces bboxes, but that would seem
// pointless as there's an explicit face intersection test
// in DD::Image::Primitive...   :(
//#define BAKE_XFORM_INTO_POINTS 1


namespace Fsr {


//--------------------------------------------------------------------------


/*!
*/
FuserUsdMesh::FuserUsdMesh(const Pxr::UsdStageRefPtr& stage,
                           const Pxr::UsdPrim&        mesh_prim,
                           const Fsr::ArgSet&         args,
                           Fsr::Node*                 parent) :
    FuserUsdXform(stage, mesh_prim, args, parent),
    m_topology_variance(ConstantTopology),
    m_subdivider(NULL)
{
    //std::cout << "  FuserUsdMesh::ctor(" << this << ") '" << mesh_prim.GetPath() << "'" << std::endl;

    // Make sure it's a UsdGeomPointBased:
    if (mesh_prim.IsValid() && mesh_prim.IsA<Pxr::UsdGeomPointBased>())
    {
        m_ptbased_schema = Pxr::UsdGeomPointBased(mesh_prim);

        // Bind the USD mesh object:
        const Pxr::UsdGeomMesh usd_mesh(m_ptbased_schema.GetPrim());

        // Get animating xform/point/topology states.
#if 0
        // TODO: fill the xform flag in correctly!
        if (getPrimAttribTimeSamples())
            m_topology_variance |= XformVaryingTopology;
#endif

        if (usd_mesh.GetPointsAttr().ValueMightBeTimeVarying())
            m_topology_variance |= PointVaryingTopology;

#if 1
        // Warning, this is not checking the actual data so if the attribs have keys
        // but the data is not actually varying then read performance will dramatically
        // suffer because the prims will rebuild on each frame change!
        //if (usd_mesh.GetFaceVertexCountsAttr().ValueMightBeTimeVarying() ||
        //    usd_mesh.GetFaceVertexIndicesAttr().ValueMightBeTimeVarying())
        //    m_topology_variance |= PrimitiveVaryingTopology;
#else
        // Unfortunately poorly-authored prims originating from Alembic files may have per-frame
        // vertex count/index data even the the meshes are topogically constant. So the simple
        // test of ValueMightBeTimeVarying() does not correctly catch this case.

        // Replicate the logic in the Alembic lib that correctly determines the
        // animation state of the 'faceVertexCount' and 'faceVertexIndices' attribs
        // so that the topology state is correct. We need to do this early enough so that
        // the geometry reader can determine how it updates frame to frame.

        // TODO: implement this routine!
#endif


        // Find material binding and create a child Fuser Node for it:
        //   ex.  'rel material:binding = </Root/Looks/dart_board_mat_inst>'
        //
        // Note that the bound prim may be outside the object's hierarchy which
        // means it may be outside the initial stage mask!
        // To check this we get the binding relationship and see if it's
        // outside the 


        const Pxr::UsdShadeMaterialBindingAPI bindingAPI(usd_mesh);
        m_material_binding = bindingAPI.ComputeBoundMaterial();
        if (!m_material_binding)
        {
            const Pxr::UsdRelationship relation = mesh_prim.GetRelationship(Pxr::TfToken("material:binding"));
            Pxr::SdfPathVector targets;
            relation.GetTargets(&targets);
            if (targets.size() > 0)
            {
                //stage->ExpandPopulationMask(std::function<bool (relation)>);
                //{
                //    static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock);
                //    stage->ExpandPopulationMask();
                //}

                //m_material_binding = bindingAPI.ComputeBoundMaterial();

                //if (!m_material_binding)
                {
                    std::cerr << "    FuserUsdMesh::ctor('" << mesh_prim.GetPath() << "'): ";
                    std::cerr << "warning, material binding prim '" << targets[0].GetString() << "'";
                    std::cerr << " cannot be resolved" << std::endl;
                }
            }
        }

        if (debug())
        {
            static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock); // lock to make the output print cleanly

            std::cout << "  FuserUsdMesh::ctor('" << mesh_prim.GetPath() << "')";
            std::cout << " topo_variance=" << m_topology_variance;

            std::cout << ", material=";
            if (m_material_binding)
                std::cout << "'" << m_material_binding.GetPath() << "'";
            else
                std::cout << "<none>";

            const Pxr::UsdAttribute purpose_attrib = usd_mesh.GetPurposeAttr();
            if (purpose_attrib)
            {
                Pxr::TfToken purpose;
                purpose_attrib.Get(&purpose);
                std::cout << " (" << purpose << " Purpose)";
            }

            printPrimAttributes("", mesh_prim, false/*verbose*/, std::cout);
            std::cout << std::endl;
        }
    }
    else
    {
        if (debug())
        {
            static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock); // lock to make the output print cleanly

            std::cerr << "    FuserUsdMesh::ctor(" << this << "): ";
            std::cerr << "warning, node '" << mesh_prim.GetPath() << "'(" << mesh_prim.GetTypeName() << ") ";
            std::cerr << "is invalid or wrong type";
            std::cerr << std::endl;
        }
    }
}


//!
/*virtual*/
FuserUsdMesh::~FuserUsdMesh()
{
    if (debug())
    {
        static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock); // lock to make the output print cleanly

        std::cout << "  FuserUsdMesh::dtor(" << this << ") '" << m_ptbased_schema.GetPath() << "'" << std::endl;
    }
}


/*! Translate a subd level string to a level.

    TODO: deprecate support for the 'subd:lo', 'subd:hi', subd:display' legacy values!
    TODO: move to Fuser::MeshPrimitive class.
*/
int getSubdLevel(const std::string&      level)
{
    if (level.empty() || level == "none" || level == "off")
        return 0;
    else if (level == "subd:lo" || level == "1")
        return 1;
    else if (level == "subd:hi" || level == "2")
        return 2;
    else if (level == "subd:display" || level == "3")
        return 3;
    else
        return 0;
}


//-------------------------------------------------------------------------------


/*! Search for the first attrib mappings match to the nuke attrib name, or the
    default name if not found.
*/
Pxr::TfToken
FuserUsdMesh::getPrimvarForNukeAttrib(const char* nuke_attrib_name,
                                      const char* default_primvar_name)
{
    if (!nuke_attrib_name || !nuke_attrib_name[0])
        return Pxr::TfToken();

    const Pxr::UsdGeomMesh usd_mesh(m_ptbased_schema.GetPrim());
    std::vector<std::string> mappings;
    if (FuserGeoReader::getNukeToFileAttribMappings(nuke_attrib_name, m_nuke_to_primvar, mappings))
    {
        // Search for the first match, likely in alphabetical order:
        for (size_t i=0; i < mappings.size(); ++i)
        {
            const Pxr::TfToken primvar_name(mappings[i]);
            if (usd_mesh.HasPrimvar(primvar_name))
                return Pxr::TfToken(primvar_name);
        }
    }

    // No mapping, does the default primvar name exist?
    if (!default_primvar_name || !default_primvar_name[0])
        return Pxr::TfToken();

    const Pxr::TfToken primvar_name(default_primvar_name);
    if (usd_mesh.HasPrimvar(primvar_name))
        return primvar_name;

    return Pxr::TfToken(); // no primvar found
}


//-------------------------------------------------------------------------------


/*! Called before execution to allow node to update local data from args.
*/
/*virtual*/ void
FuserUsdMesh::_validateState(const Fsr::NodeContext& args,
                             bool                    for_real)
{
    // Get the time value up to date:
    FuserUsdXform::_validateState(args, for_real);
    //std::cout << "FuserUsdMesh::_validateState(" << this << ") '" << m_ptbased_schema.GetPath() << "'" << std::endl;

    // Bind the USD mesh object:
    const Pxr::UsdGeomMesh usd_mesh(m_ptbased_schema.GetPrim());

    const double time = getDouble("frame");//(getDouble("frame") / getDouble("fps"));

    // These args are defined in the GeoReader plugin - support them:
    //m_translate_render_parts =   getBool("reader:translate_render_parts");
    //m_points_render_mode     = getString("reader:points_render_mode"    );


    //---------------------------------------------------------------------------
    // Get attibute name mappings from 'reader:attribute_mappings' arg.
    //      ex string. 'color=Cf Cd=Cf UV=uv pscale=size  subd::hi=subd_hi subd::display=subd_display'
    m_primvar_to_nuke.clear();
    m_nuke_to_primvar.clear();
    FuserGeoReader::buildAttributeMappings(getString("reader:attribute_mappings").c_str(),
                                           m_primvar_to_nuke,
                                           m_nuke_to_primvar);

    // Search for the attrib mappings for the known default Nuke attribs:
    m_uv_primvar_name         = getPrimvarForNukeAttrib(Fsr::NukeGeo::uvs_attrib_name,       "st"/*dflt*/);
    m_normals_primvar_name    = getPrimvarForNukeAttrib(Fsr::NukeGeo::normals_attrib_name,   "normals"/*dflt*/);
    m_colors_primvar_name     = getPrimvarForNukeAttrib(Fsr::NukeGeo::colors_attrib_name,    "displayColors"/*dflt*/);
    m_opacities_primvar_name  = getPrimvarForNukeAttrib(Fsr::NukeGeo::opacities_attrib_name, "displayOpacity"/*dflt*/);
    m_velocities_primvar_name = getPrimvarForNukeAttrib(Fsr::NukeGeo::velocity_attrib_name,  "velocities"/*dflt*/);
    //std::cout << "  m_uv_primvar_name='" << m_uv_primvar_name << "'" << std::endl;
    //std::cout << "  m_normals_primvar_name='" << m_normals_primvar_name << "'" << std::endl;
    //std::cout << "  m_colors_primvar_name='" << m_colors_primvar_name << "'" << std::endl;
    //std::cout << "  m_opacities_primvar_name='" << m_opacities_primvar_name << "'" << std::endl;
    //std::cout << "  m_velocities_primvar_name='" << m_velocities_primvar_name << "'" << std::endl;

    //---------------------------------------------------------------------------
    // Translate subd options usually set by the GeoReader on import.
    // These are mapped to the 'subd:*' attributes if those attributes don't
    // exist yet:
    const std::string& reader_subd_import_level  = getString("reader:subd_import_level");
    const std::string& reader_subd_render_level  = getString("reader:subd_render_level");
    const bool         reader_subd_force_enable  = getBool(  "reader:subd_force_enable",  false);
    const bool         reader_subd_snap_to_limit = getBool(  "reader:subd_snap_to_limit", false);
    const std::string& reader_subd_tessellator   = getString("reader:subd_tessellator" );
    if (!reader_subd_import_level.empty() && !hasArg("subd:current_level"))
    {
        // Mesh has not been subdivided yet, get reader import setting:
        const int import_level = getSubdLevel(reader_subd_import_level);
        if (import_level > 0)
            setInt("subd:import_level", import_level);
    }
    if (!reader_subd_render_level.empty() && !hasArg("subd:render_level"))
    {
        const int render_level = getSubdLevel(reader_subd_render_level);
        if (render_level > 0)
            setInt("subd:render_level", render_level);
    }
    //
    if (reader_subd_force_enable && !hasArg("subd:force_enable"))
        setBool("subd:force_enable", reader_subd_force_enable);
    //
    if (reader_subd_snap_to_limit && !hasArg("subd:snap_to_limit"))
        setBool("subd:snap_to_limit", reader_subd_snap_to_limit);
    //
    if (!reader_subd_tessellator.empty() && !hasArg("subd:tessellator"))
        setString("subd:tessellator", reader_subd_tessellator);

    // Get bbox (Extents). Caution - this attribute can sometimes be empty
    // if not explicitly authored!
    m_local_bbox.setToEmptyState();
    const Pxr::UsdAttribute extents_attrib = usd_mesh.GetExtentAttr();
    if (extents_attrib)
    {
        Pxr::VtArray<Pxr::GfVec3f> extent;
        extents_attrib.Get(&extent, time);
        // Handle empty extents:
        if (extent.size() == 2)
        {
            const Pxr::GfVec3f& min = extent[0];
            m_local_bbox.setMin(min[0], min[1], min[2]);
            const Pxr::GfVec3f& max = extent[1];
            m_local_bbox.setMax(max[0], max[1], max[2]);
        }
    }

    m_xform = getConcatenatedMatrixAtPrim(getPrim(), time);
    m_have_xform = !m_xform.isIdentity();

    if (debug())
    {
        static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock); // lock to make the output print cleanly

        std::cout << "--------------------------------------------------------------------------------------" << std::endl;
        std::cout << "FuserUsdMesh::_validateState(" << this << "): for_real=" << for_real << ", time=" << time;
        std::cout << ", m_local_bbox=" << m_local_bbox;
        std::cout << ", m_have_xform=" << m_have_xform;
        if (m_have_xform)
            std::cout << ", xform" << m_xform;
        std::cout << std::endl;
        std::cout << "  args[" << m_args << "]" << std::endl;
    }

}


/*! Return abort (-1) on user-interrupt so processing can be interrupted.
*/
/*virtual*/ int
FuserUsdMesh::_execute(const Fsr::NodeContext& target_context,
                       const char*             target_name,
                       void*                   target,
                       void*                   src0,
                       void*                   src1)
{
    // We need a context and a target name to figure out what to do:
    if (!target_name || !target_name[0])
        return -1; // no context target!

    if (debug())
    {
        static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock); // lock to make the output print cleanly

        std::cout << "  FuserUsdMesh::_execute(" << this << ") target='" << target_name << "'";
        std::cout << " Mesh";
        std::cout << " '" << getString(Arg::Scene::path) << "'";
        if (m_have_xform)
            std::cout << ", xform" << m_xform;
        else
            std::cout << ", xform disabled";
        std::cout << std::endl;
    }

    // Redirect execution depending on target type:
    if (strcmp(target_name, Arg::NukeGeo::node_topology_variance.c_str())==0)
    {
        uint32_t* topo_variance = reinterpret_cast<uint32_t*>(target);

        // Any null pointers throw a coding error:
        if (!topo_variance)
            return error("null objects in target '%s'. This is likely a coding error", target_name);

        *topo_variance = m_topology_variance;

        return 0; // success

    }
    else if (strncmp(target_name, "DRAW_GL"/*Fsr::PrimitiveViewerContext::name*/, 7)==0)
    {
        Fsr::PrimitiveViewerContext* pv_ctx =
            reinterpret_cast<Fsr::PrimitiveViewerContext*>(target);

        // Any null pointers throw a coding error:
        if (!pv_ctx || !pv_ctx->vtx || !pv_ctx->ptx)
            return error("null objects in target '%s'. This is likely a coding error", target_name);

        int draw_mode = -1;
        if      (strcmp(target_name, "DRAW_GL_BBOX"     )==0) draw_mode = Fsr::NodeContext::DRAW_GL_BBOX;
        else if (strcmp(target_name, "DRAW_GL_WIREFRAME")==0) draw_mode = Fsr::NodeContext::DRAW_GL_WIREFRAME;
        else if (strcmp(target_name, "DRAW_GL_SOLID"    )==0) draw_mode = Fsr::NodeContext::DRAW_GL_SOLID;
        else if (strcmp(target_name, "DRAW_GL_TEXTURED" )==0) draw_mode = Fsr::NodeContext::DRAW_GL_TEXTURED;
        drawMesh(pv_ctx->vtx, pv_ctx->ptx, draw_mode);

        return 0; // success

    }
    else if (strcmp(target_name, Fsr::GeoOpGeometryEngineContext::name)==0)
    {
        Fsr::GeoOpGeometryEngineContext* geo_ctx =
            reinterpret_cast<Fsr::GeoOpGeometryEngineContext*>(target);

        if (!geo_ctx || !geo_ctx->geo || !geo_ctx->geometry_list)
            return error("null objects in target '%s'. This is likely a coding error", target_name);

        geoOpGeometryEngine(*geo_ctx);

        return 0; // success

    }
    else if (strcmp(target_name, Fsr::FuserPrimitive::DDImageRenderSceneTessellateContext::name)==0)
    {
        Fsr::FuserPrimitive::DDImageRenderSceneTessellateContext* rtess_ctx =
            reinterpret_cast<Fsr::FuserPrimitive::DDImageRenderSceneTessellateContext*>(target);

        if (!rtess_ctx || !rtess_ctx->primitive || !rtess_ctx->render_scene || !rtess_ctx->ptx)
            return error("null objects in target '%s'. This is likely a coding error", target_name);

        tessellateToRenderScene(*rtess_ctx);

        return 0; // success
    }
#ifdef DWA_INTERNAL_BUILD
    else if (strcmp(target_name, zpr::GenerateRenderPrimsContext::name)==0)
    {
        zpr::GenerateRenderPrimsContext* rprim_ctx =
            reinterpret_cast<zpr::GenerateRenderPrimsContext*>(target);

        if (!rprim_ctx || !rprim_ctx->rtx || !rprim_ctx->stx)
            return error("null objects in target '%s'. This is likely a coding error", target_name);

        generateRenderPrims(*rprim_ctx->rtx, *rprim_ctx->stx);

        return 0; // success
    }
#endif

    // Let base class handle unrecognized targets:
    return FuserUsdXform::_execute(target_context, target_name, target, src0, src1);
}


//-------------------------------------------------------------------------------
//-------------------------------------------------------------------------------


/*! Fill in the mesh context.
*/
bool
FuserUsdMesh::initializeMeshSample(MeshSample&    mesh,
                                   Fsr::TimeValue time,
                                   uint32_t       id_index,
                                   int            target_subd_level,
                                   bool           get_uvs,
                                   bool           get_normals,
                                   bool           get_opacities,
                                   bool           get_colors,
                                   bool           get_velocities)
{
    mesh.time       = Pxr::UsdTimeCode(time);
    mesh.id_index   = id_index;
    mesh.nPoints    = 0;
    mesh.nVerts     = 0;
    mesh.nFaces     = 0;
    mesh.subd_level = 0;
    mesh.cw_winding = false;
    mesh.all_tris   = false;
    mesh.all_quads  = false;

    // This transform is only valid for the time the node was initialized:
    mesh.matrix   = m_xform;
    mesh.bbox     = m_local_bbox;

    if (debug())
    {
        static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock); // lock to make the output print cleanly

        std::cout << "    ----------------------------------------------------------------" << std::endl;
        std::cout << "    FuserUsdMesh::initializeMeshSample(" << this << "): time=" << time;
        std::cout << ", name='" << Fsr::Node::getName() << "'";
        std::cout << ", '" << getString(Arg::Scene::file) << "'";
        //std::cout << ", have_xform=" << m_have_xform;
        //std::cout << " args[" << m_args << "]";
        std::cout << std::endl;
    }

    // Bind the USD mesh object:
    const Pxr::UsdGeomMesh usd_mesh(m_ptbased_schema.GetPrim());


    // Reverse face winding order? Nuke is left-handed and USD can be either:
    {
        Pxr::TfToken orientation;
        usd_mesh.GetOrientationAttr().Get(&orientation, Pxr::UsdTimeCode::Default());
        // Reverse if orientation is not left-handed:
        mesh.cw_winding = (orientation == Pxr::UsdGeomTokens->leftHanded);
        //std::cout << "    cw_winding='" << mesh.cw_winding << "'" << std::endl;
    }

    // Get SubdivisionScheme ("Allowed Values": [catmullClark, loop, bilinear, none])
    {
        Pxr::TfToken usd_subd_scheme;
        usd_mesh.GetSubdivisionSchemeAttr().Get(&usd_subd_scheme, Pxr::UsdTimeCode::Default());
        //std::cout << "    subd-scheme='" << usd_subd_scheme << "'" << std::endl;

#if 0
        // TODO: map to the predefined types in Fsr::MeshPrimitive
        const std::string& scheme = usd_subd_scheme.GetString();
        if (scheme == "catmullClark") mesh.subd_scheme = Fsr::MeshPrimitive::subd_catmull_clark_type;
        if (scheme == "loop"        ) mesh.subd_scheme = Fsr::MeshPrimitive::subd_loop_type;
        if (scheme == "bilinear"    ) mesh.subd_scheme = Fsr::MeshPrimitive::subd_bilinear_type;
        else mesh.subd_scheme.clear();
#else
        // Make scheme lower case.
        mesh.subd_scheme = usd_subd_scheme.GetString();
        std::transform(mesh.subd_scheme.begin(), mesh.subd_scheme.end(), mesh.subd_scheme.begin(), ::tolower);
#endif
    }


#if 0
    // Get list of enabled facesets if it's changed:
    //DD::Image::Hash new_hash;
    //new_hash.append(getString("reader:enabled_facesets").c_str());
    if (1)//(new_hash != m_faceset_hash)
    {
        //m_faceset_hash = new_hash;
        std::vector<std::string> tokens; tokens.reserve(5);
        stringSplit(getString("reader:enabled_facesets"), " ", tokens);
        m_enabled_facesets.clear();
        for (size_t i=0; i < tokens.size(); ++i)
        {
            const std::string& token = tokens[i];
            if (token.empty())
                continue;
            // Is this string a valid child Faceset of this node?
            const size_t nChildNodes = object().getNumChildren();
            for (size_t j=0; j < nChildNodes; ++j)
                if (object().getChild(j).getName() == token)
                    m_enabled_facesets.insert((unsigned)j);
        }
    }
#endif


    // Get points. We copy them to a local array since it's very likely
    // the mesh will need to be subdivided before being used:
    {
        const Pxr::UsdAttribute points_attrib = usd_mesh.GetPointsAttr();
        if (!points_attrib)
        {
            //TF_WARN( "Invalid point attribute" );
            std::cout << "Invalid point attribute" << std::endl;
            return false; // need points!
        }

        Pxr::VtVec3fArray usd_points;
        points_attrib.Get(&usd_points, mesh.time);
        mesh.nPoints = usd_points.size();

        if (mesh.nPoints == 0)
        {
            //TF_WARN( "Invalid point count" );
            std::cout << "Invalid point count" << std::endl;
            return false; // need points!
        }

        mesh.points.resize(mesh.nPoints);
        memcpy(mesh.points.data(), usd_points[0].data(), mesh.nPoints*sizeof(Pxr::GfVec3f));
    }


    // Get verts-per-face counts:
    {
        const Pxr::UsdAttribute verts_per_face_attrib = usd_mesh.GetFaceVertexCountsAttr();
        if (!verts_per_face_attrib)
        {
            //TF_WARN( "Invalid vertex count attribute" );
            std::cout << "Invalid vertex count attribute" << std::endl;
            return false; // need vert counts!
        }

        Pxr::VtIntArray usd_verts_per_face;
        verts_per_face_attrib.Get(&usd_verts_per_face, mesh.time);
        mesh.nFaces = usd_verts_per_face.size();

        if (mesh.nFaces == 0)
        {
            //TF_WARN( "Invalid vertex count attribute" );
            std::cout << "Invalid vertex count attribute" << std::endl;
            return false; // need vert counts!
        }

        mesh.verts_per_face.resize(mesh.nFaces);
        memcpy(mesh.verts_per_face.data(), usd_verts_per_face.data(), mesh.nFaces*sizeof(int));
    }


    // Get vert indices:
    {
        const Pxr::UsdAttribute facevert_point_indices_attrib = usd_mesh.GetFaceVertexIndicesAttr();
        if (!facevert_point_indices_attrib)
        {
            //TF_WARN( "Invalid face vertex indicies attribute for %s.",
            //         usd_mesh.GetPrim().GetPath().GetText());
            std::cout << "Invalid face vertex indicies attribute" << std::endl;
            return false; // need faces!
        }

        Pxr::VtIntArray usd_facevert_point_indices;
        facevert_point_indices_attrib.Get(&usd_facevert_point_indices, mesh.time);
        mesh.nVerts = usd_facevert_point_indices.size();

        if (mesh.nVerts == 0)
        {
            //TF_WARN( "Invalid vertex indicies count of 0" );
            std::cout << "Invalid vertex indicies count of 0" << std::endl;
            return false; // need vert indices!
        }

        mesh.facevert_point_indices.resize(mesh.nVerts);
        if (mesh.cw_winding)
        {
            // Reverse CW to CCW winding:
            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const int nFaceVerts = mesh.verts_per_face[f];
                const int vstart = vindex;
                for (int v=nFaceVerts-1; v >= 0; --v)
                    mesh.facevert_point_indices[vindex++] = usd_facevert_point_indices[vstart + v];
            }
            assert(vindex == (int)mesh.nVerts);
        }
        else
        {
            // CCW winding matches Nuke's default:
            memcpy(mesh.facevert_point_indices.data(), usd_facevert_point_indices.data(), mesh.nVerts*sizeof(uint32_t));
        }


        // Verify the face vert counts matches the vert index array size:
        int face_verts = 0;
        for (size_t i=0; i < mesh.nFaces; ++i)
            face_verts += mesh.verts_per_face[i];
        if ((int)mesh.facevert_point_indices.size() != face_verts)
        {
            //TF_WARN( "Invalid topology found for %s. "
            //         "Expected at least %d verticies and only got %zd.",
            //         usd_mesh.GetPrim().GetPath().GetText(), numVerticiesExpected, usdFaceIndex.size() );
            if (1)//(debug())
            {
                static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock); // lock to make the output print cleanly

                std::cerr << "FuserUsdMesh::initializeMeshSample(" << this << "):";
                std::cerr << " error initializing mesh data in node '" << getString(Arg::Scene::file) << "'";
                std::cerr << ", expected " << mesh.facevert_point_indices.size() << " verticies";
                std::cerr << " but got " << face_verts << ", igoring!";
                std::cerr << std::endl;
            }
            return false; // bad topology!
        }

        // Verify none of the vert values exceed the point array size:
        for (size_t i=0; i < mesh.nVerts; ++i)
        {
            if (mesh.facevert_point_indices[i] >= mesh.nPoints)
            {
                //TF_WARN( "Invalid topology found for %s. "
                //         "Expected at least %d points and only got %zd.",
                //         usd_mesh.GetPrim().GetPath().GetText(), maxPointIndex, usdPoints.size() ); 
                if (1)//(debug())
                {
                    static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock); // lock to make the output print cleanly

                    std::cerr << "FuserUsdMesh::initializeMeshSample(" << this << "):";
                    std::cerr << " error initializing mesh data in node '" << getString(Arg::Scene::file) << "'";
                    std::cerr << ", vertex index " << i << " exceeds max point " << (mesh.nPoints-1) << ", igoring!";
                    std::cerr << std::endl;
                }
                return false; // bad topology!
            }
        }

    }

    if (get_uvs)
        getVertexUVs(mesh, m_uv_primvar_name, mesh.uvs);

    if (get_normals)
        getVertexNormals(mesh, m_normals_primvar_name, mesh.normals);

    if (get_colors || get_opacities)
        getVertexColors(mesh, m_colors_primvar_name, m_opacities_primvar_name, mesh.colors, get_opacities);

    if (get_velocities)
        getVertexVelocities(mesh, m_velocities_primvar_name, mesh.velocities);


    if (debug())
    {
        static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock); // lock to make the output print cleanly

        std::cout << "      nFaces=" << mesh.nFaces;
        std::cout << ", nVerts=" << mesh.nVerts;
        std::cout << ", nPoints=" << mesh.nPoints;
        std::cout << std::endl;
    }

    if (mesh.bbox.isEmpty() || mesh.nFaces == 0 || mesh.nVerts == 0 || mesh.nPoints == 0)
        return false;

    // Subdivide mesh if required on read:
    if (target_subd_level == 0)
        target_subd_level = getInt("subd:import_level", 0);
    const bool subd_force_meshes = getBool("subd:force_enable", false);
    if (target_subd_level > 0 &&
        (mesh.subd_scheme != "none" || subd_force_meshes))
    {
        // Make sure we have a subdivision provider node:
        if (!m_subdivider)
        {
            const std::string& tessellator_plugin = getString("subd:tessellator", "OpenSubdiv"/*default*/);

            m_subdivider = Fsr::Node::create(tessellator_plugin.c_str()/*node_class*/, Fsr::ArgSet());
            // Try to find the default subdivision tessellator plugin:
            // TODO: make this a built-in Fuser node:
            if (!m_subdivider)
                m_subdivider = Fsr::Node::create("SimpleSubdiv"/*node_class*/, Fsr::ArgSet());

            // TODO: throw a warning if no provider?
            //if (!m_subdivider)
        }

        // Apply subdivision if we now have a subdivider:
        if (m_subdivider)
        {
            // TODO: should we simply copy all args prefixed with 'subd:' to
            // subd_args? Or just pass a copy of this Node's args?
            Fsr::NodeContext subd_args;
            subd_args.setInt(   "subd:current_level", 0);
            subd_args.setInt(   "subd:target_level",  target_subd_level);
            subd_args.setString("subd:scheme",        mesh.subd_scheme);
            //subd_args.setBool("subd:snap_to_limit", getBool("subd:snap_to_limit", false));

            Fsr::MeshTessellateContext tessellate_ctx;
            tessellate_ctx.verts_per_face        = &mesh.verts_per_face;
            tessellate_ctx.vert_position_indices = &mesh.facevert_point_indices;

            // Point data:
            tessellate_ctx.position_lists.push_back(&mesh.points);

            // Vert attribs:
            if (mesh.normals.size() > 0)
                tessellate_ctx.vert_vec3_attribs.push_back(&mesh.normals);
            if (mesh.uvs.size() > 0)
                tessellate_ctx.vert_vec2_attribs.push_back(&mesh.uvs);
            if (mesh.colors.size() > 0)
                tessellate_ctx.vert_vec4_attribs.push_back(&mesh.colors);
            if (mesh.velocities.size() > 0)
                tessellate_ctx.vert_vec3_attribs.push_back(&mesh.velocities);

            int res = m_subdivider->execute(subd_args,           /*target_context*/
                                            tessellate_ctx.name, /*target_name*/
                                            &tessellate_ctx      /*target*/);
            if (res < 0)
            {
                //if (res == -2 && debug())
                //    std::cerr << "UsdMeshPrim::initializeMeshSample()" << " error '" << m_subdivider->errorMessage() << "'" << std::endl;
            }

            mesh.nFaces     = mesh.verts_per_face.size();
            mesh.nVerts     = mesh.facevert_point_indices.size();
            mesh.nPoints    = mesh.points.size();
            mesh.subd_level = target_subd_level;
            mesh.all_tris   = false;//(mesh.subd_scheme == Fsr::MeshPrimitive::subd_loop_type);
            mesh.all_quads  = true;//(mesh.subd_scheme == Fsr::MeshPrimitive::subd_catmull_clark_type);
            //std::cout << "UsdMesh(" << getName() << ")::subdivide " << mesh.subd_level << std::endl;

            buildVertexNormals(mesh, mesh.normals);
        }
    }

    if (mesh.normals.size() == 0)
        buildVertexNormals(mesh, mesh.normals);

    return true;

} // initializeMeshSample()


//-------------------------------------------------------------------------------
//-------------------------------------------------------------------------------


/*! Get vertex uvs in Nuke-natural (CCW) order.
*/
void
FuserUsdMesh::getVertexUVs(const MeshSample&   mesh,
                           const Pxr::TfToken& primvar_name,
                           Fsr::Vec2fList&     uvs)
{
    uvs.clear();
    if (mesh.nVerts == 0 || primvar_name.IsEmpty())
        return; // don't crash...

    // Note the GetPrimvar() method automatically prefixes 'primvar:' to attribute name:
    const Pxr::UsdGeomPrimvar& uvs_primvar = m_ptbased_schema.GetPrimvar(primvar_name);

    Fsr::Vec2fList src_uvs;
    if (getArrayPrimvar<Pxr::GfVec2f>(uvs_primvar,
                                      mesh.time,
                                      src_uvs,
                                      Pxr::TfToken("")/*scope_mask*/,
                                      false/*debug*/))
    {
        uvs.resize(mesh.nVerts);

        // Got mesh uvs, copy to output verts possibly converting scope:
        const Pxr::TfToken& scope = uvs_primvar.GetInterpolation();
        if      (scope == Pxr::UsdGeomTokens->vertex && src_uvs.size() == mesh.nPoints)
        {
            // Promote point attrib to vertex level:
            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const int nFaceVerts = mesh.verts_per_face[f];
                for (int v=0; v < nFaceVerts; ++v, ++vindex)
                {
                    const int32_t pindex = mesh.facevert_point_indices[vindex];
                    uvs[vindex] = src_uvs[pindex];
                }
            }
        }
        else if (scope == Pxr::UsdGeomTokens->faceVarying && src_uvs.size() == mesh.nVerts)
        {
            if (mesh.cw_winding)
            {
                // Reverse CW to CCW winding:
                int vindex = 0;
                for (size_t f=0; f < mesh.nFaces; ++f)
                {
                    const int nFaceVerts = mesh.verts_per_face[f];
                    const int vstart = vindex;
                    for (int v=nFaceVerts-1; v >= 0; --v, ++vindex)
                        uvs[vindex] = src_uvs[vstart + v];
                }
            }
            else
                uvs = src_uvs; // CCW winding matches Nuke's default, copy the raw array
        }
        else if (scope == Pxr::UsdGeomTokens->uniform && src_uvs.size() == mesh.nFaces)
        {
            // Winding order doesn't matter when all the vert values are the same:
            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const Fsr::Vec2f& uv = src_uvs[f];
                const int nFaceVerts = mesh.verts_per_face[f];
                for (int v=0; v < nFaceVerts; ++v, ++vindex)
                    uvs[vindex] = uv;
            }
        }
        else if (scope == Pxr::UsdGeomTokens->constant && src_uvs.size() == 1)
        {
            const Fsr::Vec2f& uv = src_uvs[0];
            for (size_t v=0; v < mesh.nVerts; ++v)
                uvs[v] = uv;
        }

    }

} // getVertexUVs()


/*! Get vertex normals in Nuke-natural (CCW) order.
*/
void
FuserUsdMesh::getVertexNormals(const MeshSample&   mesh,
                               const Pxr::TfToken& primvar_name,
                               Fsr::Vec3fList&     normals)
{
    normals.clear();
    if (mesh.nVerts == 0 || primvar_name.IsEmpty())
        return; // don't crash...

    // Note the GetPrimvar() method automatically prefixes 'primvar:' to attribute name:
    const Pxr::UsdGeomPrimvar& normals_primvar = m_ptbased_schema.GetPrimvar(primvar_name);

    Fsr::Vec3fList src_normals;
    if (getArrayPrimvar<Pxr::GfVec3f>(normals_primvar,
                                      mesh.time,
                                      src_normals,
                                      Pxr::TfToken("")/*scope_mask*/,
                                      false/*debug*/))
    {
        normals.resize(mesh.nVerts);

        // Got mesh normals, copy to output verts possibly converting scope:
        const Pxr::TfToken& scope = normals_primvar.GetInterpolation();
        if      (scope == Pxr::UsdGeomTokens->vertex && src_normals.size() == mesh.nPoints)
        {
            // Promote point attrib to vertex level:
            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const int nFaceVerts = mesh.verts_per_face[f];
                for (int v=0; v < nFaceVerts; ++v, ++vindex)
                {
                    const int32_t pindex = mesh.facevert_point_indices[vindex];
                    normals[vindex] = src_normals[pindex];
                }
            }
        }
        else if (scope == Pxr::UsdGeomTokens->faceVarying && src_normals.size() == mesh.nVerts)
        {
            if (mesh.cw_winding)
            {
                // Reverse CW to CCW winding:
                int vindex = 0;
                for (size_t f=0; f < mesh.nFaces; ++f)
                {
                    const int nFaceVerts = mesh.verts_per_face[f];
                    const int vstart = vindex;
                    for (int v=nFaceVerts-1; v >= 0; --v, ++vindex)
                        normals[vindex] = src_normals[vstart + v];
                }
            }
            else
                normals = src_normals; // CCW winding matches Nuke's default, copy the raw array
        }
        else if (scope == Pxr::UsdGeomTokens->uniform && src_normals.size() == mesh.nFaces)
        {
            // Winding order doesn't matter when all the vert values are the same:
            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const Fsr::Vec3f& N = src_normals[f];
                const int nFaceVerts = mesh.verts_per_face[f];
                for (int v=0; v < nFaceVerts; ++v, ++vindex)
                    normals[vindex] = N;
            }
        }
        else if (scope == Pxr::UsdGeomTokens->constant && src_normals.size() == 1)
        {
            const Fsr::Vec3f& N = src_normals[0];
            for (size_t v=0; v < mesh.nVerts; ++v)
                normals[v] = N;
        }

    }

}


/*! Build vertex normals based on the mesh topology.
*/
void
FuserUsdMesh::buildVertexNormals(const MeshSample& mesh,
                                 Fsr::Vec3fList&   normals)
{
    normals.clear();
    if (mesh.nVerts == 0)
        return; // don't crash...

    Fsr::Vec3fList point_normals;
    Fsr::calcPointNormals(mesh.nPoints,
                          mesh.pointLocations(),
                          mesh.nVerts,
                          mesh.faceVertPointIndices(),
                          mesh.nFaces,
                          mesh.vertsPerFace(),
                          mesh.all_tris,
                          mesh.all_quads,
                          point_normals);
    assert(point_normals.size() == mesh.nPoints);

    // Copy point normals to verts, winding order is moot for this:
    normals.resize(mesh.nVerts);
    int vindex = 0;
    for (size_t f=0; f < mesh.nFaces; ++f)
    {
        const int nFaceVerts = mesh.verts_per_face[f];
        for (int v=0; v < nFaceVerts; ++v, ++vindex)
            normals[vindex] = point_normals[mesh.facevert_point_indices[vindex]];
    }
}


/*! Get vertex velocities in Nuke-natural (CCW) order.
*/
void
FuserUsdMesh::getVertexVelocities(const MeshSample&   mesh,
                                  const Pxr::TfToken& primvar_name,
                                  Fsr::Vec3fList&     velocities)
{
    velocities.clear();
    if (mesh.nVerts == 0 || primvar_name.IsEmpty())
        return; // don't crash...

    // Note the GetPrimvar() method automatically prefixes 'primvar:' to attribute name:
    const Pxr::UsdGeomPrimvar& velocities_primvar = m_ptbased_schema.GetPrimvar(primvar_name);

    Fsr::Vec3fList src_velocities;
    if (getArrayPrimvar<Pxr::GfVec3f>(velocities_primvar,
                                      mesh.time,
                                      src_velocities,
                                      Pxr::TfToken("")/*scope_mask*/,
                                      false/*debug*/))
    {
        velocities.resize(mesh.nVerts);

        // Got mesh velocities, copy to output verts possibly converting scope:
        const Pxr::TfToken& scope = velocities_primvar.GetInterpolation();
        if      (scope == Pxr::UsdGeomTokens->vertex && src_velocities.size() == mesh.nPoints)
        {
            // Promote point attrib to vertex level:
            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const int nFaceVerts = mesh.verts_per_face[f];
                for (int v=0; v < nFaceVerts; ++v, ++vindex)
                {
                    const int32_t pindex = mesh.facevert_point_indices[vindex];
                    velocities[vindex] = src_velocities[pindex];
                }
            }
        }
        else if (scope == Pxr::UsdGeomTokens->faceVarying && src_velocities.size() == mesh.nVerts)
        {
            if (mesh.cw_winding)
            {
                // Reverse CW to CCW winding:
                int vindex = 0;
                for (size_t f=0; f < mesh.nFaces; ++f)
                {
                    const int nFaceVerts = mesh.verts_per_face[f];
                    const int vstart = vindex;
                    for (int v=nFaceVerts-1; v >= 0; --v, ++vindex)
                        velocities[vindex] = src_velocities[vstart + v];
                }
            }
            else
                velocities = src_velocities; // CCW winding matches Nuke's default, copy the raw array
        }
        else if (scope == Pxr::UsdGeomTokens->uniform && src_velocities.size() == mesh.nFaces)
        {
            // Winding order doesn't matter when all the vert values are the same:
            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const Fsr::Vec3f& vel = src_velocities[f];
                const int nFaceVerts = mesh.verts_per_face[f];
                for (int v=0; v < nFaceVerts; ++v, ++vindex)
                    velocities[vindex] = vel;
            }
        }
        else if (scope == Pxr::UsdGeomTokens->constant && src_velocities.size() == 1)
        {
            const Fsr::Vec3f& vel = src_velocities[0];
            for (size_t v=0; v < mesh.nVerts; ++v)
                velocities[v] = vel;
        }

    }

}


/*! Get vertex colors/opacities in Nuke-natural (CCW) order.
    Translate to Vec4f's by combining displayColor and displayOpacity attributes.
*/
void
FuserUsdMesh::getVertexColors(const MeshSample&   mesh,
                              const Pxr::TfToken& colors_primvar_name,
                              const Pxr::TfToken& opacities_primvar_name,
                              Fsr::Vec4fList&     Cfs,
                              bool                get_opacities)
{
    Cfs.clear();
    if (mesh.nVerts == 0 || (colors_primvar_name.IsEmpty() && opacities_primvar_name.IsEmpty()))
        return; // don't crash...

    if (getBool("reader:use_geometry_colors"))
    {
        // Note the GetPrimvar() method automatically prefixes 'primvar:' to attribute name:
        const Pxr::UsdGeomPrimvar& color_primvar   = m_ptbased_schema.GetPrimvar(colors_primvar_name);
        const Pxr::UsdGeomPrimvar& opacity_primvar = m_ptbased_schema.GetPrimvar(opacities_primvar_name);

        Fsr::Vec3fList colors;
        getArrayPrimvar<Pxr::GfVec3f>(color_primvar,
                                      mesh.time,
                                      colors,
                                      Pxr::TfToken("")/*scope_mask*/,
                                      false/*debug*/);

        Fsr::FloatList opacities;
        getArrayPrimvar<float>(opacity_primvar,
                               mesh.time,
                               opacities,
                               Pxr::TfToken("")/*scope_mask*/,
                               false/*debug*/);
        //std::cout << "colors=" << colors.size() << ", opacities=" << opacities.size() << std::endl;

        if (colors.size() > 0)
        {
            Cfs.resize(mesh.nVerts);

            // Copy vec4's to output vertex attrib:
            const Pxr::TfToken& scope = color_primvar.GetInterpolation();
            if      (scope == Pxr::UsdGeomTokens->vertex && colors.size() == mesh.nPoints)
            {
                // Promote point attrib to vertex level:
                int vindex = 0;
                for (size_t f=0; f < mesh.nFaces; ++f)
                {
                    const int nFaceVerts = mesh.verts_per_face[f];
                    for (int v=0; v < nFaceVerts; ++v, ++vindex)
                    {
                        const int32_t pindex = mesh.facevert_point_indices[vindex];
                        Cfs[vindex] = Fsr::Vec4f(colors[pindex], 1.0f);
                    }
                }
            }
            else if (scope == Pxr::UsdGeomTokens->faceVarying && colors.size() == mesh.nVerts)
            {
                if (mesh.cw_winding)
                {
                    // Reverse CW to CCW winding:
                    int vindex = 0;
                    for (size_t f=0; f < mesh.nFaces; ++f)
                    {
                        const int nFaceVerts = mesh.verts_per_face[f];
                        const int vstart = vindex;
                        for (int v=nFaceVerts-1; v >= 0; --v, ++vindex)
                            Cfs[vindex] = Fsr::Vec4f(colors[vstart + v], 1.0f);
                    }
                }
                else
                {
                    // CCW winding matches Nuke's default:
                    for (size_t v=0; v < mesh.nVerts; ++v)
                        Cfs[v] = Fsr::Vec4f(colors[v], 1.0f);
                }
            }
            else if (scope == Pxr::UsdGeomTokens->uniform && colors.size() == mesh.nFaces)
            {
                // Winding order doesn't matter when all the vert values are the same:
                int vindex = 0;
                for (size_t f=0; f < mesh.nFaces; ++f)
                {
                    const Fsr::Vec4f Cf = Fsr::Vec4f(colors[f], 1.0f);
                    const int nFaceVerts = mesh.verts_per_face[f];
                    for (int v=0; v < nFaceVerts; ++v, ++vindex)
                        Cfs[vindex] = Cf;
                }
            }
            else if (scope == Pxr::UsdGeomTokens->constant && colors.size() == 1)
            {
                const Fsr::Vec4f Cf = Fsr::Vec4f(colors[0], 1.0f);
                for (size_t v=0; v < mesh.nVerts; ++v)
                    Cfs[v] = Cf;
            }
        }

        if (opacities.size() > 0)
        {
            if (colors.size() == 0)
                Cfs.resize(mesh.nVerts, Fsr::Vec4f(1.0f));

            // Copy opacity to 4th element:
            const Pxr::TfToken& scope = opacity_primvar.GetInterpolation();
            if      (scope == Pxr::UsdGeomTokens->vertex && opacities.size() == mesh.nPoints)
            {
                // Promote point attrib to vertex level:
                int vindex = 0;
                for (size_t f=0; f < mesh.nFaces; ++f)
                {
                    const int nFaceVerts = mesh.verts_per_face[f];
                    for (int v=0; v < nFaceVerts; ++v, ++vindex)
                    {
                        const int32_t pindex = mesh.facevert_point_indices[vindex];
                        Cfs[vindex].w = opacities[pindex];
                    }
                }
            }
            else if (scope == Pxr::UsdGeomTokens->faceVarying && opacities.size() == mesh.nVerts)
            {
                if (mesh.cw_winding)
                {
                    // Reverse CW to CCW winding:
                    int vindex = 0;
                    for (size_t f=0; f < mesh.nFaces; ++f)
                    {
                        const int nFaceVerts = mesh.verts_per_face[f];
                        const int vstart = vindex;
                        for (int v=nFaceVerts-1; v >= 0; --v, ++vindex)
                            Cfs[vindex].w = opacities[vstart + v];
                    }
                }
                else
                {
                    // CCW winding matches Nuke's default:
                    for (size_t v=0; v < mesh.nVerts; ++v)
                        Cfs[v].w = opacities[v];
                }
            }
            else if (scope == Pxr::UsdGeomTokens->uniform && opacities.size() == mesh.nFaces)
            {
                // Winding order doesn't matter when all the vert values are the same:
                int vindex = 0;
                for (size_t f=0; f < mesh.nFaces; ++f)
                {
                    const float opacity = opacities[f];
                    const int nFaceVerts = mesh.verts_per_face[f];
                    for (int v=0; v < nFaceVerts; ++v, ++vindex)
                        Cfs[vindex].w = opacity;
                }
            }
            else if (scope == Pxr::UsdGeomTokens->constant && opacities.size() == 1)
            {
                const float opacity = opacities[0];
                for (size_t v=0; v < mesh.nVerts; ++v)
                    Cfs[v].w = opacity;
            }

        }

        if (Cfs.size() > 0)
            return; // assigned colors or opacities
    }

    if (getBool("reader:color_objects"))
    {
        // Set all vertex colors the same:
        const int object_index = mesh.id_index;
        DD::Image::Vector4 Cf;
        if (object_index == 0)
            Cf.set(1.0f, 1.0f, 1.0f, 1.0f);
        else
            Cf.set(float(clamp(DD::Image::p_random(object_index*3+0))),
                   float(clamp(DD::Image::p_random(object_index*3+1))),
                   float(clamp(DD::Image::p_random(object_index*3+2))),
                   1.0f);
        Cfs.resize(mesh.nVerts);
        for (size_t i=0; i < mesh.nVerts; ++i)
            Cfs[i] = Cf;

        return;
    }

    if (getBool("reader:color_facesets"))
    {
        // Set the vertex color to a random value by faceset id, overwriting default:
        // TODO: implement, we need the GeomSubsets...!

        Cfs.resize(mesh.nVerts, Fsr::Vec4f(1.0f)); // default to white for now...

        return;
    }

} // getVertexColors()


//-------------------------------------------------------------------------------
//-------------------------------------------------------------------------------


/*! Output a UsdGeomMesh mesh to a DD::Image::GeometryList GeoInfo.

    Allocate a Fsr::MeshPrimitive and fill the point, normal, uv, etc attributes.

    This uses the thread-safe Fsr::GeoInfoCacheRef interface so multiple nodes
    can be writing to the same DD::Image::GeometryList simultaneously.
*/
void
FuserUsdMesh::geoOpGeometryEngine(Fsr::GeoOpGeometryEngineContext& geo_ctx)
{
    assert(geo_ctx.geo && geo_ctx.geometry_list);

    const bool reload_attribs = geo_ctx.geo->rebuild(DD::Image::Mask_Attributes);
    const bool reload_prims   = (geo_ctx.geo->rebuild(DD::Image::Mask_Primitives) ||
                                 geo_ctx.geo->rebuild(DD::Image::Mask_Vertices  ) ||
                                 geo_ctx.geo->rebuild(DD::Image::Mask_Object    ) ||
                                 geo_ctx.geo->rebuild(DD::Image::Mask_Attributes));
    // If we're rebuilding prims then force points to reload as well:
    const bool reload_points  = (reload_prims                                     ||
                                 geo_ctx.geo->rebuild(DD::Image::Mask_Points    ) ||
                                 geo_ctx.geo->rebuild(DD::Image::Mask_Object    ));

    // Get the unique path identifier to extract the object index
    // from the GeoInfoCacheRef:
    const std::string& scene_path = getString(Arg::Scene::path);

    // geoinfo_cache object is updated with thread-safe pointers to the underlying
    // geometry data structures stored in the GeoOp. The GeoInfo caches move
    // around in memory as the GeometryList appends objects to it:
    Fsr::GeoInfoCacheRef geoinfo_cache;
    const int obj = geo_ctx.getObjectThreadSafe(scene_path, geoinfo_cache);

    if (obj < 0)
    {
        if (debug())
        {
            std::cerr << "    FuserUsdMesh::geoOpGeometryEngine(" << this << "):";
            std::cerr << " error, node '" << Fsr::Node::getPath() << "'";
            std::cerr << " with scene path '" << scene_path << "'";
            std::cerr << " does not resolve to a valid object index, ignoring!";
            std::cerr << std::endl;
        }
        return; // don't crash...
    }

    const Fsr::TimeValue time = getDouble("frame");//(getDouble("frame") / getDouble("fps"));
    const int subd_import_level = getInt("subd:import_level", 0);

    //-------------------------------------------------------
    // Fill in the MeshSample for the scene time:
    MeshSample mesh;
    if (!initializeMeshSample(mesh,
                              time,
                              0/*id_index*/,
                              subd_import_level/*target_subd_level*/,
                              true/*get_uvs*/,
                              true/*get_normals*/,
                              true/*get_opacities*/,
                              true/*get_colors*/,
                              true/*get_velocities*/))
    {
        if (debug())
        {
            std::cerr << "    FuserUsdMesh::geoOpGeometryEngine(" << this << "):";
            std::cerr << " error initializing mesh data from node '" << Fsr::Node::getPath() << "'";
            std::cerr << " with scene path '" << scene_path << "'";
            std::cerr << ", ignoring!";
            std::cerr << std::endl;
        }
        return; // don't crash...
    }


    if (debug())
    {
        static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock); // lock to make the output print cleanly

        std::cout << "  --------------------------------------------------------------------------------------" << std::endl;
        std::cout << "  FuserUsdMesh::geoOpGeometryEngine(" << this << "):";
        std::cout << " obj=" << obj << ", time=" << time;
        std::cout << ", name='" << Fsr::Node::getName() << "'";
        std::cout << ", path='" << Fsr::Node::getPath() << "'";
        std::cout << ", '" << getString(Arg::Scene::file) << "'";
        std::cout << std::endl;
        std::cout << "      rebuild_mask=0x" << std::hex << geo_ctx.geo->rebuild_mask() << std::dec;
        std::cout << ": reload_prims=" << reload_prims << ", reload_points=" << reload_points << ", reload_attribs=" << reload_attribs;
        std::cout << ", m_local_bbox=" << m_local_bbox;
        std::cout << ", m_have_xform=" << m_have_xform;
        std::cout << ", mesh.subd_level=" << mesh.subd_level;
        std::cout << ", subd_render_level=" << getInt("subd:render_level", 0);
        if (m_have_xform)
            std::cout << ", m_xform" << m_xform;
        //std::cout << ", cw_winding=" << mesh.cw_winding;
        std::cout << std::endl;
        std::cout << "      args[" << m_args << "]";
        std::cout << std::endl;
    }


    //-------------------------------------------------------
    // Get Subd params to use when outputing face, vertex & point data:
    if (!mesh.subd_scheme.empty() && mesh.subd_scheme != "none")
    {
        // TODO: define these subd string constants somewhere common
        if (mesh.subd_level > 0)
            geo_ctx.setObjectIntThreadSafe(geoinfo_cache, "subd:current_level", mesh.subd_level);

        const int subd_render_level = getInt("subd:render_level", 0);
        if (subd_render_level > 0)
            geo_ctx.setObjectIntThreadSafe(geoinfo_cache, "subd:render_level", subd_render_level);

        if (!mesh.subd_scheme.empty())
            geo_ctx.setObjectStringThreadSafe(geoinfo_cache, "subd:scheme", mesh.subd_scheme.c_str());
    }


    // Acquire a Fsr::MeshPrimitive primitive:
    Fsr::MeshPrimitive* pmesh = NULL;
    if (!reload_prims)
    {
        //=========================================================
        //
        // Retrieve the previously-created MeshPrimitive
        //
        //=========================================================
        if (obj < 0 || geoinfo_cache.primitives_list == NULL)
            return; // don't crash...

        pmesh = dynamic_cast<Fsr::MeshPrimitive*>((*geoinfo_cache.primitives_list)[0]);

    }
    else
    {
        //=========================================================
        //
        // Rebuilding primitives - create new MeshPrimitive and fill
        // object/prim level attributes:
        //
        //=========================================================

        // Add name and path attributes:
        geo_ctx.setObjectStringThreadSafe(geoinfo_cache, "name", Fsr::Node::getName());

        // Add parent-path attribute - this will allow the xform path to be somewhat
        // reconstructed on output:
        geo_ctx.setObjectStringThreadSafe(geoinfo_cache, Arg::Scene::path.c_str(), Fsr::Node::getPath());

#if 0
        // Read the mesh attributes into the output GeoInfo attrib arrays:
// TODO: finish this!!!
        readPrimvars(mesh,
                     0x0/*scope_mask*/,
                     geoinfo_cache,
                     debugAttribs());
#endif

        //----------------------------------------------------------------------------------------

        // Set motion-blur method. If the face/vertex connectivity is changing
        // we can't pair up MeshPrimitives from multiple time samples, so switch
        // to using velocity:
        // TODO: move this to Fuser MeshNode base class
        if (m_topology_variance == ConstantTopology)
        {
            geo_ctx.setObjectStringThreadSafe(geoinfo_cache, "mblur_method", "constant");
        }
        else if (m_topology_variance & PrimitiveVaryingTopology)
        {
            if (mesh.velocities.size() == 0)
            {
                // no velocities, no motionblur...
                geo_ctx.setObjectStringThreadSafe(geoinfo_cache, "mblur_method", "constant");
            }
            else
            {
                // TODO: determine whether to do forward or backwards
                geo_ctx.setObjectStringThreadSafe(geoinfo_cache, "mblur_method", "velocity-forward");
            }
        }
        else if (m_topology_variance != ConstantTopology)
        {
            geo_ctx.setObjectStringThreadSafe(geoinfo_cache, "mblur_method", "multisample");
        }

        //----------------------------------------------------------------------------------------


        // Instantiate a new MeshPrimitive Fuser node:
        Fsr::Node* mesh_node = Fsr::Node::create(Fsr::MeshPrimitive::description,
                                                 m_args,
                                                 NULL/*parent*/);
        pmesh = dynamic_cast<Fsr::MeshPrimitive*>(mesh_node);
#if DEBUG
        assert(pmesh); // shouldn't happen...
#endif
        pmesh->setFrame(mesh.time.GetValue());
        pmesh->addFaces(mesh.nVerts,
                        mesh.faceVertPointIndices(),
                        mesh.nFaces,
                        mesh.vertsPerFace());

        // Add it to the cache:
        geo_ctx.appendNewPrimitiveThreadSafe(geoinfo_cache, pmesh, mesh.nVerts);

        //----------------------------------------------------------------------------------------

        if (mesh.uvs.size() > 0)
        {
            DD::Image::Attribute* out_uvs =
                geo_ctx.createWritableAttributeThreadSafe(geoinfo_cache,
                                                          DD::Image::Group_Vertices,
                                                          "uv",
                                                          DD::Image::VECTOR4_ATTRIB);
            assert(out_uvs);
            out_uvs->resize(mesh.uvs.size());

            for (size_t i=0; i < mesh.uvs.size(); ++i)
            {
                const Fsr::Vec2f& uv = mesh.uvs[i];
                out_uvs->vector4(i).set(uv.x, uv.y, 0.0f, 1.0f);
            }
        }

        //----------------------------------------------------------------------------------------

        if (mesh.colors.size() > 0)
        {
            DD::Image::Attribute* out_colors =
                geo_ctx.createWritableAttributeThreadSafe(geoinfo_cache,
                                                          DD::Image::Group_Vertices,
                                                          "Cf",
                                                          DD::Image::VECTOR4_ATTRIB);
            assert(out_colors); // shouldn't happen...
            out_colors->resize(mesh.nVerts); // just in case...
            memcpy(out_colors->array(), mesh.colors.data(), mesh.nVerts*sizeof(DD::Image::Vector4));
        }

        //----------------------------------------------------------------------------------------

        // TODO: handle GeomSubsets! We do this similar to materials, by adding child nodes



        //----------------------------------------------------------------------------------------

        // If there's a material binding create a child Fuser Node for it:
        //   ex.  'rel material:binding = </Root/Looks/dart_board_mat_inst>'
        if (m_material_binding)
        {
            // This Fuser Node takes responsibility for deleting the child pointer:
            const Pxr::UsdPrim mat_prim = getStage()->GetPrimAtPath(m_material_binding.GetPath());
            if (mat_prim.IsValid())
            {
                // The material creation args are slimmed down:
                Fsr::ArgSet mat_args;
                mat_args.setString(Arg::node_name, mat_prim.GetName().GetString());
                //mat_args.setString(Arg::node_path,   );

                // Usd scene path:
                mat_args.setString(Arg::Scene::path, m_material_binding.GetPath().GetString());
                // Local Fsr node path:'fsr:node:path' is the mesh + child node path:
                const std::string fsr_node_path = Fsr::buildPath(Fsr::Node::getPath(), mat_prim.GetName().GetString());
                mat_args.setString(Arg::node_path, fsr_node_path);

                // Local material binding path is the Fsr node path:
                geo_ctx.setObjectStringThreadSafe(geoinfo_cache, "material:binding", fsr_node_path);

                if (getBool(Arg::NukeGeo::read_debug, false))
                    mat_args.setInt(Arg::node_debug, 1/*DEBUG_1*/);

                pmesh->addChild(new FuserUsdShadeMaterialNode(getStage(), mat_prim, mat_args, pmesh/*parent*/));
            }
        }

        //----------------------------------------------------------------------------------------


    } // reload_prims
#if DEBUG
    assert(pmesh); // shouldn't happen...
#endif


    // Always update the frame number for the DD::Image::Primitive.
    // TODO: we should only set the frame # if the mesh is animating,
    //       else we need a way of indicating static geometry that the
    //       motion-blur code can handle:
    pmesh->setFrame(mesh.time.GetValue());


    // Update point locations and point xform:
    if (reload_points)
    {
        DD::Image::PointList* out_points = geo_ctx.createWritablePointsThreadSafe(geoinfo_cache);
        assert(out_points); // shouldn't happen...
        out_points->resize(mesh.nPoints); // just in case...
        Fsr::Vec3f* Parray = reinterpret_cast<Fsr::Vec3f*>(out_points->data());

        // Always bake the xform into the GeoInfo points (see note in
        // Fsr::PointBasedPrimitive class about why...)
        if (m_have_xform)
            mesh.matrix.transform(Parray, mesh.pointLocations(), mesh.nPoints);
        else
            memcpy(Parray, mesh.pointLocations(), mesh.nPoints*sizeof(Fsr::Vec3f));

#ifdef BAKE_XFORM_INTO_POINTS
#else
        //pmesh->setTransformAndLocalPoints(mesh.matrix, mesh.nPoints, mesh.pointLocations());
#endif
        geo_ctx.updateBBoxThreadSafe(geoinfo_cache);

        //----------------------------------------------------------------------------------------

        if (mesh.normals.size() > 0)
        {
            DD::Image::Attribute* out_normals =
                geo_ctx.createWritableAttributeThreadSafe(geoinfo_cache,
                                                          DD::Image::Group_Vertices,
                                                          "N",
                                                          DD::Image::NORMAL_ATTRIB);
            assert(out_normals); // shouldn't happen...
            out_normals->resize(mesh.nVerts); // just in case...

            // Always bake the xform into the GeoInfo points (see note in
            // Fsr::PointBasedPrimitive class about why...)
            if (m_have_xform)
            {
                const Fsr::Mat4d im = mesh.matrix.inverse();
                im.normalTransform(reinterpret_cast<Fsr::Vec3f*>(out_normals->array()),
                                   mesh.normals.data(),
                                   mesh.nVerts);
#ifdef BAKE_XFORM_INTO_POINTS
#else
#endif
            }
            else
            {
                memcpy(out_normals->array(), mesh.normals.data(), mesh.nVerts*sizeof(DD::Image::Vector3));
            }
        }

    } // reload_points

} // geoOpGeometryEngine()


//-------------------------------------------------------------------------------
//-------------------------------------------------------------------------------


/*! Normally called from a deferred-load NodePrimtive.

    The attribute names can be different for deferred-load vs. immediate since they're
    only coming from the Fuser::Node vs. a GeoInfo Primitive.

*/
void
FuserUsdMesh::tessellateToRenderScene(Fsr::FuserPrimitive::DDImageRenderSceneTessellateContext& rtess_ctx)
{
    // TODO: figure out motionblur logic that works with ScanlineRender. I think we just need
    // to make a single sample at the Node's time.

    const Fsr::TimeValue time = getDouble("frame");//(getDouble("frame") / getDouble("fps"));

    // Subd options:
    const int  subd_current_level =  getInt("subd:current_level", 0);
    const int  subd_render_level  =  getInt("subd:render_level",  0);
    const bool subd_force_meshes  = getBool("subd:force_enable",  false);

    //-------------------------------------------------------
    // Fill in the MeshSample for the scene time:
    MeshSample mesh;
    if (!initializeMeshSample(mesh,
                              time,
                              0/*id_index*/,
                              subd_render_level/*target_subd_level*/,
                              true/*get_uvs*/,
                              true/*get_normals*/,
                              true/*get_opacities*/,
                              true/*get_colors*/,
                              true/*get_velocities*/))
    {
        if (debug())
        {
            std::cerr << "    FuserUsdMesh::tessellateToRenderScene(" << this << "):";
            std::cerr << " error initializing mesh data from node '" << Fsr::Node::getPath() << "'";
            std::cerr << ", ignoring!";
            std::cerr << std::endl;
        }
        return; // don't crash...
    }

    if (mesh.nPoints == 0 || mesh.nVerts == 0 || mesh.nFaces == 0)
        return; // don't crash...

    if (debug())
    {
        static std::mutex m_lock; std::lock_guard<std::mutex> guard(m_lock); // lock to make the output print cleanly

        std::cout << "  --------------------------------------------------------------------------------------" << std::endl;
        std::cout << "  FuserUsdMesh::tessellateToRenderScene(" << this << "):";
        std::cout << " time=" << time;
        std::cout << ", name='" << Fsr::Node::getName() << "'";
        std::cout << ", path='" << Fsr::Node::getPath() << "'";
        std::cout << ", '" << getString(Arg::Scene::file) << "'";
        std::cout << std::endl;
        std::cout << "    nFaces=" << mesh.nFaces;
        std::cout << ", nVerts=" << mesh.nVerts;
        std::cout << ", nPoints=" << mesh.nPoints;
        std::cout << ", m_local_bbox=" << m_local_bbox;
        std::cout << ", m_have_xform=" << m_have_xform;
        if (m_have_xform)
            std::cout << ", mesh.matrix" << mesh.matrix;
        std::cout << std::endl;
        std::cout << "    subd_current_level=" << subd_current_level;
        std::cout << ", subd_render_level=" << subd_render_level;
        std::cout << ", subd_force_meshes=" << subd_force_meshes;
        std::cout << ", mesh.subd_level=" << mesh.subd_level;
        //std::cout << ", cw_winding=" << mesh.cw_winding;
        std::cout << std::endl;
        std::cout << "      args: " << m_args;
        std::cout << std::endl;
    }

    // Copy the MeshSample into a VertexBuffers.
    // TODO: merge the classes so that a MeshSample *is* a VertexBuffers class, or
    //       at least a subclass of VertexBuffers.
    Fsr::PointBasedPrimitive::VertexBuffers vbuffers(mesh.nPoints, mesh.nVerts, mesh.nFaces);
    {
        // TODO: all this logic can be in the Fuser MeshNode base class.
        if (m_have_xform)
            mesh.matrix.transform(vbuffers.PL.data(), mesh.points.data(), mesh.nPoints);
        else
            vbuffers.PL = mesh.points;
        memcpy(vbuffers.PW.data(), vbuffers.PL.data(), sizeof(Fsr::Vec3f)*mesh.nPoints);
        //
        vbuffers.Pidx = mesh.facevert_point_indices;
        vbuffers.interpolateChannels = DD::Image::ChannelSetInit(DD::Image::Mask_PL_ |
                                                                 DD::Image::Mask_PW_ |
                                                                 DD::Image::Mask_P_);
        //
        if (mesh.normals.size() == mesh.nVerts)
        {
            if (m_have_xform)
            {
                const Fsr::Mat4d im = mesh.matrix.inverse();
                im.normalTransform(vbuffers.N.data(),
                                   mesh.normals.data(),
                                   mesh.nVerts);
            }
            else
                vbuffers.N = mesh.normals;
            vbuffers.interpolateChannels += DD::Image::ChannelSetInit(DD::Image::Mask_N_);
        }
        if (mesh.uvs.size() == mesh.nVerts)
        {
            vbuffers.UV.resize(mesh.uvs.size());
            for (size_t i=0; i < mesh.uvs.size(); ++i)
                vbuffers.UV[i] = mesh.uvs[i];
            vbuffers.interpolateChannels += DD::Image::ChannelSetInit(DD::Image::Mask_UV_);
        }
        else
        {
            const Fsr::Vec4f default_uv(0.5f, 0.5f, 0.0f, 1.0f);
            Fsr::Vec4f* UVp = vbuffers.UV.data();
            for (size_t v=0; v < mesh.nVerts; ++v)
                *UVp++ = default_uv;
        }

        if (mesh.colors.size() == mesh.nVerts)
        {
            vbuffers.Cf = mesh.colors;
            vbuffers.interpolateChannels += DD::Image::ChannelSetInit(DD::Image::Mask_CF_);
        }
        //
        if (mesh.velocities.size() == mesh.nVerts)
        {
            vbuffers.VEL = mesh.velocities;
            vbuffers.interpolateChannels += DD::Image::ChannelSetInit(DD::Image::Mask_VEL_);
        }
        vbuffers.vertsPerFace = mesh.verts_per_face;
        vbuffers.allTris      = mesh.all_tris;
        vbuffers.allQuads     = mesh.all_quads;

    }

    // Allow vertex shaders to change values, and produce final transformed PW and N:
    vbuffers.applyVertexShader(rtess_ctx);

    // Have vertex buffer output render prims to render scene, in mesh mode.
    vbuffers.addToRenderScene(rtess_ctx, 0/*mode*/);
}


//-------------------------------------------------------------------------------
//-------------------------------------------------------------------------------


// TODO: move these to the Fuser MeshNode base class:


/*virtual*/
void
FuserUsdMesh::drawIcons()
{
    //char buf[128];
    //sprintf(buf, " %d", (int)ptx->index(Group_Primitives));
    //const Vector3& v = ptx->geoinfo()->point_array()[vertex(0)];
    //gl_text(buf, v.x, v.y, v.z);
std::cout << "drawIcons(): " << m_local_bbox << std::endl;

    glPushMatrix();
    glMultMatrixd(m_xform.array());

    glRasterPos3dv(m_local_bbox.min.array());
    DD::Image::gl_text(Fsr::Node::getPath().c_str());

    glPopMatrix();
}


/*!
*/
void
FuserUsdMesh::drawMesh(DD::Image::ViewerContext*    vtx,
                       DD::Image::PrimitiveContext* ptx,
                       int                          draw_mode)
{
    //std::cout << "FuserUsdMesh::drawMesh(" << this << "): draw_mode=" << draw_mode;
    //std::cout << ", what_to_draw=" << std::hex << vtx->what_to_draw() << std::dec << std::endl;

    if (draw_mode < 0)
        return;
    assert(vtx);
    assert(ptx);

    const Fsr::TimeValue time = getDouble("frame");//(getDouble("frame") / getDouble("fps"));
    const int subd_import_level = getInt("subd:import_level", 0);

    const bool get_normals   = (draw_mode == Fsr::NodeContext::DRAW_GL_SOLID ||
                                draw_mode == Fsr::NodeContext::DRAW_GL_TEXTURED);
    const bool get_uvs       = (draw_mode == Fsr::NodeContext::DRAW_GL_TEXTURED);
    const bool get_opacities = (draw_mode == Fsr::NodeContext::DRAW_GL_SOLID ||
                                draw_mode == Fsr::NodeContext::DRAW_GL_TEXTURED);
    const bool get_colors    = (draw_mode == Fsr::NodeContext::DRAW_GL_SOLID);

    // Fill in a MeshSample for the gui/OpenGL time:
    MeshSample mesh;
    if (!initializeMeshSample(mesh,
                              time,
                              0/*id_index*/,
                              subd_import_level/*target_subd_level*/,
                              get_uvs,
                              get_normals,
                              get_opacities,
                              get_colors,
                              false/*get_velocities*/))
        return; // mesh failed to initialize

    if (mesh.nPoints == 0 || mesh.nVerts == 0 || mesh.nFaces == 0)
        return; // don't crash...


    // Don't bother doing any hard work if we're only displaying
    // a bbox:
    // TODO: this can call Fsr::Node base class instead I think.
    if (draw_mode == Fsr::NodeContext::DRAW_GL_BBOX)
    {
        const Fsr::Vec3d& a = mesh.bbox.min;
        const Fsr::Vec3d& b = mesh.bbox.max;

        glPushMatrix();
        glMultMatrixd(mesh.matrix.array());
        glPushAttrib(GL_LINE_BIT);
        glLineWidth(1);
        {
            glBegin(GL_LINE_STRIP);
            {
                glVertex3d(a.x, a.y, b.z);
                glVertex3d(a.x, b.y, b.z);
                glVertex3d(b.x, b.y, b.z);
                glVertex3d(b.x, a.y, b.z);
                glVertex3d(a.x, a.y, b.z);
                glVertex3d(a.x, a.y, a.z);
                glVertex3d(a.x, b.y, a.z);
                glVertex3d(b.x, b.y, a.z);
                glVertex3d(b.x, a.y, a.z);
                glVertex3d(a.x, a.y, a.z);
            }
            glEnd();
            glBegin(GL_LINES);
            {
                glVertex3d(a.x, b.y, a.z);
                glVertex3d(a.x, b.y, b.z);
                glVertex3d(b.x, b.y, a.z);
                glVertex3d(b.x, b.y, b.z);
                glVertex3d(b.x, a.y, a.z);
                glVertex3d(b.x, a.y, b.z);
            }
            glEnd();
        }
        glPopMatrix();
        glPopAttrib(); // GL_LINE_BIT

        return;
    }

    // Calc possible face-skipping step factor:
    size_t face_step = 1;
    if (getString("reader:proxy_lod") == Fsr::NodePrimitive::lod_modes[Fsr::NodePrimitive::LOD_PROXY])
        face_step = (mesh.nFaces > STEP_THRESHOLD)?std::max((size_t)1, (mesh.nFaces / STEP_DIVISOR)):1;
    else if (getString("reader:proxy_lod") == Fsr::NodePrimitive::lod_modes[Fsr::NodePrimitive::LOD_RENDER])
        face_step = 1;

    if (draw_mode == Fsr::NodeContext::DRAW_GL_WIREFRAME)
    {
        // OpenGL wireframe display:
#ifdef BAKE_XFORM_INTO_POINTS
        Fsr::Vec3d Pt;
#else
        glPushMatrix();
        glMultMatrixd(mesh.matrix.array());
#endif

        Fsr::Vec4f cur_blend_color;
        glGetFloatv(GL_BLEND_COLOR, cur_blend_color.array());

        glPushAttrib(GL_COLOR_BUFFER_BIT | GL_LINE_BIT);
        glLineWidth(1);
        {
            glBlendColor(1.0f, 1.0f, 1.0f, 0.25f);
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);

            glEnable(GL_LINE_STIPPLE);
            //glLineStipple(1, 0x1010); // very dotty
            glLineStipple(1, 0xeee0); // dashed
            //
            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const int nFaceVerts = mesh.verts_per_face[f];
                if (f%face_step)
                {
                    vindex += nFaceVerts;
                    continue; // skip face
                }
                glBegin(GL_LINE_LOOP);
                for (int v=0; v < nFaceVerts; ++v, ++vindex)
                {
                    const int pindex = mesh.facevert_point_indices[vindex];
#ifdef BAKE_XFORM_INTO_POINTS
                    const Pxr::GfVec3f& P = mesh.points[pindex];
                    mesh.matrix.transform(reinterpret_cast<const Fsr::Vec3f&>(P), Pt);
                    glVertex3dv(Pt.array());
#else
                    glVertex3fv(mesh.points[pindex].array());
#endif
                }
                glEnd();
            }
        }
        glPopAttrib(); // GL_COLOR_BUFFER_BIT | GL_LINE_BIT
#ifdef BAKE_XFORM_INTO_POINTS
#else
        glPopMatrix();
#endif

        return;
    }
 
    const bool have_normals = (get_normals && mesh.normals.size() == mesh.nVerts);

    // Only draw textured if we have UVs, if not switch to solid:
    if (draw_mode == Fsr::NodeContext::DRAW_GL_TEXTURED && mesh.uvs.size() == 0)
        draw_mode = Fsr::NodeContext::DRAW_GL_SOLID;

    if (draw_mode == Fsr::NodeContext::DRAW_GL_SOLID)
    {
        // OpenGL solid display:
#ifdef BAKE_XFORM_INTO_POINTS
        Fsr::Vec3d Pt;
#else
        glPushMatrix();
        glMultMatrixd(mesh.matrix.array());
#endif

        //glPushAttrib(GL_POLYGON_BIT);
        //glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

        if (mesh.colors.size() == mesh.nVerts)
        {
            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const int nFaceVerts = mesh.verts_per_face[f];
                if (f%face_step)
                {
                    vindex += nFaceVerts;
                    continue; // skip face
                }
                glBegin(GL_POLYGON);
                for (int v=0; v < nFaceVerts; ++v, ++vindex)
                {
                    const int pindex = mesh.facevert_point_indices[vindex];

                    if (have_normals)
                        glNormal3fv(mesh.normals[vindex].array());

                    // Apply gamma 2.2 to all colors:
                    const Fsr::Vec4f& Cf = mesh.colors[vindex];
                    glColor4f(powf(Cf.x, 0.45f),
                              powf(Cf.y, 0.45f),
                              powf(Cf.z, 0.45f),
                              Cf.w);
#ifdef BAKE_XFORM_INTO_POINTS
                    const Pxr::GfVec3f& P = mesh.points[pindex];
                    mesh.matrix.transform(reinterpret_cast<const Fsr::Vec3f&>(P), Pt);
                    glVertex3dv(Pt.array());
#else
                    glVertex3fv(mesh.points[pindex].array());
#endif
                }
                glEnd();
            }
        }
        else
        {
            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const int nFaceVerts = mesh.verts_per_face[f];
                if (f%face_step)
                {
                    vindex += nFaceVerts;
                    continue; // skip face
                }
                glBegin(GL_POLYGON);
                for (int v=0; v < nFaceVerts; ++v, ++vindex)
                {
                    const int pindex = mesh.facevert_point_indices[vindex];

                    if (have_normals)
                        glNormal3fv(mesh.normals[vindex].array());

#ifdef BAKE_XFORM_INTO_POINTS
                    const Pxr::GfVec3f& P = mesh.points[pindex];
                    mesh.matrix.transform(reinterpret_cast<const Fsr::Vec3f&>(P), Pt);
                    glVertex3dv(Pt.array());
#else
                    glVertex3fv(mesh.points[pindex].array());
#endif
                }
                glEnd();
            }
        }

        //glPopAttrib(); // GL_POLYGON_BIT
#ifdef BAKE_XFORM_INTO_POINTS
#else
        glPopMatrix();
#endif

    }
    else if (draw_mode == Fsr::NodeContext::DRAW_GL_TEXTURED)
    {
        // OpenGL textured display:
        assert(mesh.uvs.size() == mesh.nVerts);
#ifdef BAKE_XFORM_INTO_POINTS
        Fsr::Vec3d Pt;
#else
        glPushMatrix();
        glMultMatrixd(mesh.matrix.array());
#endif

        if (mesh.colors.size() == mesh.nVerts)
        {
            // Support per-vertex opacity:
            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const int nFaceVerts = mesh.verts_per_face[f];
                if (f%face_step)
                {
                    vindex += nFaceVerts;
                    continue; // skip face
                }
                glBegin(GL_POLYGON);
                for (int v=0; v < nFaceVerts; ++v, ++vindex)
                {
                    const int pindex = mesh.facevert_point_indices[vindex];
                    if (have_normals)
                        glNormal3fv(mesh.normals[vindex].array());
                    glColor4f(1.0f, 1.0f, 1.0f, mesh.colors[vindex].w);
                    glTexCoord2fv(mesh.uvs[vindex].array());
#ifdef BAKE_XFORM_INTO_POINTS
                    const Pxr::GfVec3f& P = mesh.points[pindex];
                    mesh.matrix.transform(reinterpret_cast<const Fsr::Vec3f&>(P), Pt);
                    glVertex3dv(Pt.array());
#else
                    glVertex3fv(mesh.points[pindex].array());
#endif
                }
                glEnd();
            }
        }
        else
        {
            glColor4f(1,1,1,1); // surface color should always be white when texturing

            int vindex = 0;
            for (size_t f=0; f < mesh.nFaces; ++f)
            {
                const int nFaceVerts = mesh.verts_per_face[f];
                if (f%face_step)
                {
                    vindex += nFaceVerts;
                    continue; // skip face
                }
                glBegin(GL_POLYGON);
                for (int v=0; v < nFaceVerts; ++v, ++vindex)
                {
                    const int pindex = mesh.facevert_point_indices[vindex];
                    if (have_normals)
                        glNormal3fv(mesh.normals[vindex].array());
                    glTexCoord2fv(mesh.uvs[vindex].array());
#ifdef BAKE_XFORM_INTO_POINTS
                    const Pxr::GfVec3f& P = mesh.points[pindex];
                    mesh.matrix.transform(reinterpret_cast<const Fsr::Vec3f&>(P), Pt);
                    glVertex3dv(Pt.array());
#else
                    glVertex3fv(mesh.points[pindex].array());
#endif
                }
                glEnd();
            }
        }

#ifdef BAKE_XFORM_INTO_POINTS
#else
        glPopMatrix();
#endif
    } // draw textured


} // drawMesh()


} // namespace Fsr


// end of FuserUsdMesh.cpp

//
// Copyright 2019 DreamWorks Animation
//
