include "dafx_shaders.sh"
include "shader_global.sh"
include "dafx_helpers.sh"
include "fom_shadows.sh"
include "clustered/lights_cb.sh"
include "dynamic_lights_count.sh"
include "flexible_scale_rasterization.sh"

int modfx_debug_render = 0;
interval modfx_debug_render : off < 1, on;

int modfx_wboit_enabled = 0;
interval modfx_wboit_enabled : off < 1, on;

int modfx_wboit_pass = 2;
interval modfx_wboit_pass : color < 1, alpha < 2, combined;

int fx_apply_volfog_per_pixel = 0;
interval fx_apply_volfog_per_pixel: no < 1, yes; // must be disabled explicitly for WT compat for lack of UAV

int fx_has_volfog_injection = 0;
interval fx_has_volfog_injection: no < 1, yes;

int rendering_distortion_color = 0;
interval rendering_distortion_color: no < 1, yes;

int fom_double_intensity = 0;
interval fom_double_intensity: no < 1, yes;

int modfx_bboard_lighting_from_clustered_lights = 0;
interval modfx_bboard_lighting_from_clustered_lights: no < 1, yes;

texture wboit_color;
texture wboit_alpha;

texture haze_scene_depth_tex;
float haze_scene_depth_tex_lod;

shader dafx_modfx_bboard_render, dafx_modfx_bboard_render_atest, dafx_modfx_bboard_distortion, dafx_modfx_bboard_thermals, dafx_modfx_ribbon_render, dafx_modfx_ribbon_render_side_only, dafx_modfx_bboard_render_fom, dafx_modfx_bboard_rain, dafx_modfx_bboard_rain_distortion, dafx_modfx_volshape_render, dafx_modfx_volshape_thermal, dafx_modfx_volshape_depth, dafx_modfx_volshape_wboit_render, dafx_modfx_bboard_above_depth_placement, dafx_modfx_bboard_volfog_injection
{
  ENABLE_ASSERT(ps)
  if (fx_apply_volfog_per_pixel == yes && !(shader == dafx_modfx_bboard_distortion || shader == dafx_modfx_bboard_rain_distortion || shader == dafx_modfx_bboard_render_fom))
  {
    hlsl
    {
      #define MODFX_USE_FOG_PS_APPLY 1
    }
  }

  z_test = true;
  cull_mode = none;

  DAFXEX_USE_SHADOW()
  DAFXEX_USE_GI()

  hlsl
  {
    ##if hardware.metal
      #if !SHADER_COMPILER_DXC
        #define MODFX_USE_INVERTED_POS_W 1
      #endif
    ##endif
    #if !MOBILE_DEVICE
      #define MODFX_USE_LIGHTING 1
    #endif
    #define MODFX_USE_FRAMEBLEND 1
    #define MODFX_USE_DEPTH_MASK 1
    #define MODFX_USE_COLOR_MATRIX 1
    #define MODFX_USE_COLOR_REMAP 1
    #define MODFX_USE_PACK_HDR 1
    #define MODFX_USE_FOG 1
  }

  if ( modfx_debug_render == on )
  {
    z_write = false;
    z_test = false;

    blend_src=one;
    blend_dst=one;

    blend_asrc=one;
    blend_adst=one;

    hlsl { #define MODFX_DEBUG_RENDER_ENABLED 1 }
  }
  else if ( shader == dafx_modfx_bboard_render || shader == dafx_modfx_bboard_rain || shader == dafx_modfx_bboard_above_depth_placement)
  {
    z_write = false;
    blend_src=one;
    blend_dst=isa;

    blend_asrc=zero;
    blend_adst=isa;
  }
  else if ( shader == dafx_modfx_volshape_render || shader == dafx_modfx_volshape_thermal )
  {
    z_write = false;
    blend_src=one;
    blend_dst=isa;

    blend_asrc=zero;
    blend_adst=isa;

    if (modfx_wboit_enabled == on)
    {
      (ps)
      {
        wboit_color@smp2d = wboit_color;
        wboit_alpha@smp2d = wboit_alpha;
      }
      if ( modfx_wboit_pass == combined )
      {
        hlsl  {#define MODFX_WBOIT_PASS_COMBINED 1 }
      }
    }
    else
    {
      if ( shader == dafx_modfx_volshape_thermal )
      {
        hlsl
        {
          #define MODFX_SHADER_THERMALS 1
          #undef MODFX_USE_LIGHTING
          #undef MODFX_USE_SHADOW
          #undef MODFX_USE_GI
          #include "fx_thermals.hlsl"
        }
      }
    }

    hlsl
    {
      #define MODFX_SHADER_VOLSHAPE 1
      #undef MODFX_USE_DEPTH_MASK

      ##if modfx_wboit_enabled == on
        #define MODFX_SHADER_VOLSHAPE_WBOIT_APPLY 1
      ##endif
    }
  }
  else if ( shader == dafx_modfx_volshape_depth )
  {
    z_write = true;
    no_ablend;

    hlsl
    {
      #define MODFX_SHADER_VOLSHAPE_DEPTH 1
      #undef MODFX_USE_DEPTH_MASK
    }
  }
  else if ( shader == dafx_modfx_volshape_wboit_render )
  {
    z_write = false;
    if ( modfx_wboit_pass == combined )
    {
      blend_src=one;
      blend_dst=one;
      blend_asrc=zero;
      blend_adst=isa;
      hlsl  {#define MODFX_WBOIT_PASS_COMBINED 1 }
    }
    else if ( modfx_wboit_pass == color )
    {
      blend_src=one;
      blend_dst=one;
      blend_asrc=one;
      blend_adst=one;
      hlsl  {#define MODFX_WBOIT_PASS_COLOR 1 }
    }
    else if ( modfx_wboit_pass == alpha )
    {
      blend_src=zero;
      blend_dst=isc;
      hlsl  {#define MODFX_WBOIT_PASS_ALPHA 1 }
    }

    hlsl
    {
      #define MODFX_SHADER_VOLSHAPE_WBOIT 1
      #define MODFX_WBOIT_ENABLED 1
      #undef MODFX_USE_DEPTH_MASK
    }
  }
  else if ( shader == dafx_modfx_ribbon_render || shader == dafx_modfx_ribbon_render_side_only )
  {
    z_write = false;
    blend_src=one;
    blend_dst=isa;

    blend_asrc=zero;
    blend_adst=isa;

    hlsl
    {
      #define MODFX_SHADER_RIBBON 1
    }

    if (shader == dafx_modfx_ribbon_render_side_only)
    {
      hlsl
      {
        #define MODFX_SHADER_RIBBON_IS_SIDE_ONLY 1
      }
    }
    else
    {
      hlsl
      {
        #define MODFX_SHADER_RIBBON_IS_SIDE_ONLY 0
      }
    }
  }
  else if ( shader == dafx_modfx_bboard_render_atest )
  {
    z_write = false;
    blend_src=one;
    blend_dst=zero;
    blend_asrc=zero;
    blend_adst=isa;

    USE_ATEST_HALF()
    hlsl { #define MODFX_SHADER_ATEST 1 }
  }
  else if ( shader == dafx_modfx_bboard_distortion  || shader == dafx_modfx_bboard_rain_distortion)
  {
    z_write  =true;
    blend_src=one;
    blend_dst=one;

    blend_asrc=one;
    blend_adst=one;

    (ps)
    {
      haze_scene_depth_tex@smp2d = haze_scene_depth_tex;
      haze_scene_depth_tex_lod@f1 = (haze_scene_depth_tex_lod);
    }

    if (rendering_distortion_color == yes)
    {
      hlsl
      {
        #define MODFX_SHADER_DISTORTION_IS_COLORED 1
      }
    }

    hlsl
    {
      #define MODFX_SHADER_DISTORTION 1

      #undef HAS_STATIC_SHADOW
      #undef MODFX_USE_SHADOW
      #undef MODFX_USE_LIGHTING
      #undef MODFX_USE_GI
      #undef MODFX_USE_FRAMEBLEND
      #undef MODFX_USE_DEPTH_MASK
      #undef MODFX_USE_COLOR_MATRIX
      #undef MODFX_USE_COLOR_REMAP
      #undef MODFX_USE_PACK_HDR
      #undef MODFX_USE_FOG
    }

    USE_ATEST_1()
  }
  else if ( shader == dafx_modfx_bboard_thermals )
  {
    z_write = false;
    blend_src=one;
    blend_dst=isa;

    blend_asrc=zero;
    blend_adst=isa;

    hlsl
    {
      #define MODFX_SHADER_THERMALS 1
      #undef MODFX_USE_LIGHTING
      #undef MODFX_USE_SHADOW
      #undef MODFX_USE_GI
      #include "fx_thermals.hlsl"
    }
  }

  else if ( shader == dafx_modfx_bboard_volfog_injection )
  {
    if (fx_has_volfog_injection == no)
    {
      dont_render;
    }

    z_test = false;
    z_write = false;

    (ps)
    {
      view_inscatter_volume_resolution@f3 = (volfog_froxel_volume_res.x, volfog_froxel_volume_res.y, volfog_froxel_volume_res.z, 0);
      view_inscatter_inv_range@f1 = volfog_froxel_range_params.y;
      initial_media@uav : register(initial_media_no) hlsl {
        RWTexture3D<float4> initial_media@uav;
      }
    }

    hlsl
    {
      #define MODFX_SHADER_VOLFOG_INJECTION 1

      #undef HAS_STATIC_SHADOW
      #undef MODFX_USE_SHADOW
      #undef MODFX_USE_LIGHTING
      #undef MODFX_USE_GI
      #undef MODFX_USE_PACK_HDR
      #undef MODFX_USE_FOG
      #undef MODFX_USE_DEPTH_MASK
    }
  }


  else if ( shader == dafx_modfx_bboard_render_fom )
  {
    z_write = false;
    blend_src=one;
    blend_dst=one;

    hlsl
    {
      #define MODFX_SHADER_FOM 1
      #undef HAS_STATIC_SHADOW
      #undef MODFX_USE_SHADOW
      #undef MODFX_USE_LIGHTING
      #undef MODFX_USE_GI
      #undef MODFX_USE_PACK_HDR
      #undef MODFX_USE_FOG
      #undef MODFX_USE_DEPTH_MASK
      ##if fom_double_intensity == yes
        #define FX_FOM_DOUBLE_INTESITY 1
      ##endif
      #include "fom_shadows_inc.hlsl"
    }
  }

  if (shader == dafx_modfx_bboard_rain || shader == dafx_modfx_bboard_rain_distortion)
  {
    DAFXEX_USE_RAIN_CULLING()

    hlsl
    {
      #define MODFX_RAIN 1
    }
  }

  if (shader == dafx_modfx_bboard_above_depth_placement)
  {
    DAFXEX_USE_ABOVE_DEPTH_PLACEMENT()

    hlsl
    {
      #define MODFX_ABOVE_DEPTH_PLACEMENT 1
    }
  }

  DAFX_RENDER_INIT()
  DAFX_RENDER_USE()
  DAFX_SCREEN_POS_TO_TC()
  DECL_POSTFX_TC_VS_SCR()
  DAFXEX_USE_SCENE_BLOCK()
  DAFXEX_USE_DEPTH_MASK(ps)
  DAFXEX_USE_HDR()
  DAFXEX_USE_FOG()
  USE_FSR(ps)

  hlsl(vs)
  {
    // There is some bug in old adreno drivers (up to 512.420 version): vkCreateGraphicsPipelines can return null-handle with VK_OK result code.
    // Disabling that optimization helps to work around the problem. So, do it for now.
    ##if hardware.vulkan
      #pragma spir-v optimizer disable convert-local-access-chains
    ##endif
  }

  hlsl
  {
    #include "dafx_hlsl_funcs.hlsli"
    #include "dafx_globals.hlsli"
    #include "dafx_loaders.hlsli"
    #include "dafx_packers.hlsli"
    #include "dafx_random.hlsli"

    #include "modfx/modfx_decl.hlsl"
  }

  if (modfx_bboard_lighting_from_clustered_lights == yes)
  {
    INIT_OMNI_LIGHTS_CB(vs)
    INIT_AND_USE_LIGHTS_CLUSTERED_CB(vs)
    INIT_AND_USE_CLUSTERED_LIGHTS(vs)
    hlsl(vs)
    {
      #include "renderLights.hlsli"
      #define MODFX_BBOARD_LIGHTING_FROM_CLUSTERED_LIGHTS 1
    }
  }

  hlsl(vs)
  {
    #define FX_VS 1
    #include "modfx/modfx_bboard_render.hlsl"
    #undef FX_VS
  }

  hlsl(ps)
  {
    Texture2D g_tex_0 : register(t10);
    SamplerState g_tex_0_samplerstate:register(s10);

    Texture2D g_tex_1 : register(t11);
    SamplerState g_tex_1_samplerstate:register(s11);

    #define FX_PS 1
    #include "modfx/modfx_bboard_render.hlsl"
    #undef FX_PS
  }

  if (hardware.fsh_5_0)
  {
    compile( "ps_5_0", "dafx_bboard_ps" );
  }
  else
  {
    compile( "target_ps", "dafx_bboard_ps" );
  }
  compile( "target_vs", "dafx_bboard_vs" );
}
