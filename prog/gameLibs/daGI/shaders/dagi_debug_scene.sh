include "sky_shader_global.sh"
include "viewVecVS.sh"
include "frustum.sh"
include "gbuffer.sh"
include "dagi_scene_voxels_common.sh"
//include "sample_voxels.sh"

int ssgi_debug_rasterize_scene = 0;
interval ssgi_debug_rasterize_scene:raycast<1, exact_voxels<2, lit_voxels;

int ssgi_debug_rasterize_scene_cascade = 0;

define_macro_if_not_defined INIT_VOXELS_HEIGHTMAP_HELPERS(code)
  hlsl(code) {
    float ssgi_get_heightmap_2d_height(float3 worldPos) {return worldPos.y-100;}
  }
endmacro

shader ssgi_debug_rasterize_voxels
{
  z_test = false;
  cull_mode = none;
  blend_src = 1; blend_dst = isa;

  (ps) {
    envi_probe_specular@smpCube = envi_probe_specular;
    ssgi_debug_rasterize_scene_cascade@f1 = (ssgi_debug_rasterize_scene_cascade);
    world_view_pos@f3 = world_view_pos;
  }

  RAY_CAST_VOXELS(ps)
  USE_AND_INIT_VIEW_VEC_VS()
  INIT_ZNZFAR()

  hlsl {
    struct VsOutput
    {
      VS_OUT_POSITION(pos)
      float2 tc : TEXCOORD0;
      float3 viewVect     : TEXCOORD1;
    };
  }
  
  DECL_POSTFX_TC_VS_RT()
  
  hlsl(vs) {

    VsOutput debug_rasterize_vs(uint vertexId : SV_VertexID)
    {
      VsOutput output;
      float2 pos =  float2((vertexId == 2) ? +3.0 : -1.0, (vertexId == 1) ? -3.0 : 1.0);
      output.pos = float4(pos.xy,0,1);
      output.tc = pos * float2(0.5, -0.5) + float2(0.50001, 0.50001);
      output.tc.xy = output.tc;
      output.viewVect = view_vecLT + output.tc.x*(view_vecRT-view_vecLT) + output.tc.y*(view_vecLB-view_vecLT);
      //output.viewVect = pos.y > 0 ? (pos.x < 0 ? view_vecLT : view_vecRT) : (pos.x < 0 ? view_vecLB : view_vecRB);

      return output;
    }
  }

  hlsl(ps) {
    float get_box_offset(uint cascade, float3 wpos, float3 wdir)
    {
      float3 bmin, bmax;
      getSceneVoxelBox(cascade, bmin, bmax);
      float3 cb = (wdir >= 0.0f) ? bmin : bmax;

      float3 rzr = 1.0 / wdir;
      bool3 nonzero = (abs(wdir) > 1e-6);
      float3 startOfs = nonzero ? (cb - wpos) * rzr : 0;
      return max(0, max3(startOfs.x, startOfs.y, startOfs.z));
    }

    float get_box_offset2(uint cascade, float3 wpos, float3 wdir)
    {
      float3 bmin, bmax;
      getSceneVoxelBox(cascade, bmin, bmax);
      float3 cb = (wdir >= 0.0f) ? bmax : bmin;

      float3 rzr = 1.0 / wdir;
      bool3 nonzero = (abs(wdir) > 1e-6);
      float3 startOfs = nonzero ? (cb-wpos) * rzr : 0;
      return max(0, max3(startOfs.x, startOfs.y, startOfs.z));
    }

    float3 ray_box_intersect_normal(float3 wpos, float3 wdir, float3 bmin, float3 bmax)
    {
      float3 cb = (wdir >= 0.0f) ? bmin : bmax;

      float3 rzr = 1.0 / wdir;
      bool3 nonzero = (abs(wdir) > 1e-6);
      float3 startOfs = nonzero ? max(0, (cb - wpos) * rzr) : 0;
      float maxStart = max3(startOfs.x, startOfs.y, startOfs.z);
      return -(maxStart == startOfs.x ? float3(sign(wdir.x),0,0) : maxStart == startOfs.y ? float3(0, sign(wdir.y),0) : float3(0, 0, sign(wdir.z)));
    }

    struct WooRay3d{
      float3 p, wdir;
      int3 outCell, pt, endCell, stepCell;
      float3 startP, tMax, tDelta;
    };

    #define MAX_REAL 1e6

    WooRay3d initWoo(float3 p, float3 wdir, float3 leafSize, int3 dimensions, int3 end_cell)
    {
      WooRay3d ray;
      ray.p = p;
      ray.wdir = wdir;
      ray.startP = float3(ray.p)/leafSize;
      ray.pt = int3(floor(ray.startP));
      ray.endCell = end_cell;

      ray.stepCell = (wdir >= 0.0f) ? int3(1,1,1) : int3(-1,-1,-1);
      float3 csp = ceil(ray.startP);
      float3 cb = (wdir >= 0.0f) ? leafSize*(csp+(ray.startP>=csp)) : leafSize*floor(ray.startP);

      float3 rzr = 1.0 / wdir;
      bool3 nonzero = (abs(wdir) > 1e-6);
      ray.tMax = nonzero ? (cb - p) * rzr : MAX_REAL;

      ray.tDelta = nonzero ? leafSize * (ray.stepCell>0 ? rzr:-rzr) : 0;
      ray.endCell = nonzero ? ray.endCell : ray.pt;

      ray.outCell = ray.stepCell>0 ? min(dimensions, ray.endCell+1) : max(-1, ray.endCell-1);
      return ray;
    }

    void nextCell(inout WooRay3d ray, inout float t)
    {
      if (ray.tMax.x < ray.tMax.y) {
        if (ray.tMax.x < ray.tMax.z) {
          ray.pt.x += ray.stepCell.x;
          t = ray.tMax.x;
          ray.tMax.x += ray.tDelta.x;
        } else {
          ray.pt.z += ray.stepCell.z;
          t = ray.tMax.z;
          ray.tMax.z += ray.tDelta.z;
        }
      } else {
        if (ray.tMax.y < ray.tMax.z) {
          ray.pt.y += ray.stepCell.y;
          t = ray.tMax.y;
          ray.tMax.y += ray.tDelta.y;
        } else {
          ray.pt.z += ray.stepCell.z;
          t = ray.tMax.z;
          ray.tMax.z += ray.tDelta.z;
        }
      }
    }

    half3 lit_voxel(float3 normal, float t, float3 diffuse)
    {
      float3 light = normalize(float3(1,2,0.6));
      float3 backlight = -light.zyx;
      float sun=1;
      float backsun=sun*0.1;
      float amb=0.1;
      float fog = exp2(-t*0.025);
      return fog*(sun*saturate(dot(normal, light)) + backsun*saturate(dot(normal, backlight)) + amb*(normal.y*0.5+0.5))*diffuse;//saturate(dot(normal, light))//(-normal.xzy)
    }

    bool raycast_woo(float3 wpos, float3 wdir, float maxDist, out float dist, out float3 voxel_normal, out uint3 pt, out float4 result)
    {
      uint cascade = ssgi_debug_rasterize_scene_cascade;
      float startT = max(0, get_box_offset(cascade, wpos, wdir)-0.01);
      float3 bmin, bmax;
      getSceneVoxelBox(cascade, bmin, bmax);

      float3 worldPos = wpos + startT*wdir;
      float3 ofsPos = worldPos.xzy - bmin.xzy;
      float3 bwd = getSceneVoxelSize(cascade);
      WooRay3d ray = initWoo(ofsPos, wdir.xzy, bwd.xzy, VOXEL_RESOLUTION, sceneWorldPosToCoord(worldPos+wdir*maxDist, cascade));
      int3 diff = abs(ray.endCell - ray.pt);
      int n = 4*dot(diff, 1)+1;
      float t = 0;
      pt = 0;

      dist = MAX_REAL;
      voxel_normal = 0;
      result = half4(0,0,0,1);

      LOOP
      for (;n;n--) {
        if (any(ray.pt < 0 && ray.stepCell < 0) || any(ray.pt >= VOXEL_RESOLUTION && ray.stepCell > 0))
          return false;
        int3 wrapCoord = wrapSceneVoxelCoord(ray.pt, cascade);
        float alpha = getVoxelsAlpha(wrapCoord, cascade);
        //alpha *= alpha;//for visibility
        if (alpha > 0)
        {
          float3 bbmin = bmin + ray.pt.xzy*bwd;
          voxel_normal = ray_box_intersect_normal(wpos, wdir, bbmin, bbmin+bwd);
          half3 voxelColor = getVoxelsColor(wrapCoord, cascade);
          ##if ssgi_debug_rasterize_scene != exact_voxels
            voxelColor.rgb = lit_voxel(voxel_normal, startT+t, voxelColor);
          ##endif
          result.rgb += result.a*voxelColor;
          result.a *= 1-alpha;
          if (result.a<0.01)
          {
            dist = startT+t;
            pt = ray.pt;
            return true;
          }
        }
        if (all(ray.pt == ray.endCell))
          return false;

        float3 rayMin = min(min(ray.tMax.x, ray.tMax.y), ray.tMax.z);
        //voxel_normal = (rayMin == ray.tMax ? -ray.stepCell : 0).xzy;
        nextCell(ray, t);

        if (t>maxDist)
          return false;//half4(0,1,0,1);
      }
      return false;
    }

    #define MAX_DIST 100
    //#define FIXED_LOOP_COUNT 1

    half4 lit_raycast_woo(float3 wpos, float3 wdir)
    {
      float t;
      float3 voxel_normal;
      uint3 coord;
      half4 color;
      raycast_woo(wpos, wdir, MAX_DIST, t, voxel_normal, coord, color);
      return half4(color.rgb, 1-color.a);
    }

    half4 raycast_loop2(float3 wpos, float3 wdir)
    {
      uint cascade = 0;
      float startT = get_box_offset(getSceneVoxelNumCascades()-1, wpos, wdir)+0.01;
      float3 worldPos = wpos+wdir*startT;

      //half4 ret = raycast_voxel_loop_coord(cascade, wpos, wdir, MAX_DIST);
      #if FIXED_LOOP_COUNT
        #define VIS_MAX_DIST 8
      #else
        #define VIS_MAX_DIST 35.f
      #endif
      half4 ret = raycast_cone(0, wpos, wdir, VIS_MAX_DIST);
      return half4(ret.rgb, 1-ret.a);
    }

    half4 debug_rasterize_ps(VsOutput IN) : SV_Target
    {
      float3 view = normalize(IN.viewVect);
      ##if ssgi_debug_rasterize_scene == raycast
      half4 result = raycast_loop2(world_view_pos, normalize(IN.viewVect));
      //half4 result = lit_raycast_loop(world_view_pos, normalize(IN.viewVect));
      //half4 result = cone_trace_loop(world_view_pos, normalize(IN.viewVect));
      //half4 result = lit_raycast_loop_wdf(world_view_pos, normalize(IN.viewVect));
      //half4 result = lit_raycast_loop_df(world_view_pos, normalize(IN.viewVect));
      //half4 result = lit_raycast_loop_df_woo(world_view_pos, normalize(IN.viewVect));
      //half4 result = lit_raycast_loop_woo(world_view_pos, normalize(IN.viewVect));
      ##else
      half4 result = lit_raycast_woo(world_view_pos, normalize(IN.viewVect));
      ##endif
      clip(result.a-0.001);
      return result;
    }
  }
  compile("target_vs", "debug_rasterize_vs");
  compile("target_ps", "debug_rasterize_ps");
}
