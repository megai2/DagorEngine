include "shader_global.sh"
include "monteCarlo.sh"
include "roughToMip.sh"

define_macro_if_not_defined SUPPORT_GLOBAL_FRAME()
endmacro

int relight_mip = 0;
interval relight_mip: glossy_mip < 1, rough_mip;
hlsl {
  #include <get_cubemap_vector.hlsl>
}

int integrate_face = 6;
interval integrate_face: integrate_face_one<6, integrate_face_all;

shader specular_cube
{
  SUPPORT_GLOBAL_FRAME()
  no_ablend;
  USE_ROUGH_TO_MIP()
  USE_POSTFX_VERTEX_POSITIONS()

  cull_mode = none;
  z_write = false;
  z_test = false;

  hlsl {
    struct VsOutput
    {
      VS_OUT_POSITION(pos)
      float2 tc : TEXCOORD0;
    };
  }

  hlsl(vs) {
    VsOutput light_probe_vs(uint vertex_id : SV_VertexID)
    {
      VsOutput output;
      float2 pos = getPostfxVertexPositionById(vertex_id);
      output.pos = float4(pos, 0, 1);
      output.tc = pos;

      return output;
    }
  }

  (ps) { relight_mip@f2 = (relight_mip,integrate_face,0,0); }
  hlsl(ps) {
    TextureCube dynamic_cube_tex:register(t7);
    SamplerState dynamic_cube_tex_samplerstate:register(s7);

    struct MRT_OUTPUT
    {
      half4 color0:SV_Target0;
      ##if integrate_face == integrate_face_all
      half4 color1:SV_Target1;
      half4 color2:SV_Target2;
      half4 color3:SV_Target3;
      half4 color4:SV_Target4;
      half4 color5:SV_Target5;
      ##endif
    };

    MRT_OUTPUT light_probe_ps(VsOutput input)
    {
      /*float3 outColor[6] = {
        float3(1,0,0),
        float3(0,1,0),
        float3(0,0,1),
        float3(1,1,0),
        float3(1,0,1),
        float3(0,0,0)
      };
      /*/
      float4 outColor[6];
      float linearRoughness = ComputeReflectionCaptureRoughnessFromMip(relight_mip.x);
      float ggxAlpha = linearRoughness*linearRoughness;
      half4 result;
      ##if integrate_face == integrate_face_all
      UNROLL
      for (int unroll_cubeFace = 0; unroll_cubeFace < 6; ++unroll_cubeFace)
      ##else
      int unroll_cubeFace = int(relight_mip.y);
      ##endif
      {
        float3 R = normalize(GetCubemapVector(input.tc, unroll_cubeFace));
        #define MIN_NUM_FILTER_SAMPLES 32
        #define MAX_NUM_FILTER_SAMPLES 128
        ##if relight_mip == glossy_mip
          result = texCUBElod(dynamic_cube_tex, float4(R.xyz,0) );
        ##else
        float weight = 0;
        float4 res = 0;
        uint numSamples = lerp(MIN_NUM_FILTER_SAMPLES, MAX_NUM_FILTER_SAMPLES, linearRoughness);
        LOOP
        for ( int i = 0; i < numSamples; ++i )
        {
          float2 E = hammersley( i, numSamples, 0 );

          float3 H = tangent_to_world( importance_sample_GGX_NDF( E, linearRoughness ).xyz, R );
          float3 L = 2 * dot( R, H ) * H - R;
          float NoL = saturate( dot( R, L ) );
          half4 cubeTex = texCUBElod( dynamic_cube_tex, float4(L.xyz,0) );
          res += half4(h3nanofilter(cubeTex.xyz) * NoL, cubeTex.w);
          weight += NoL;
        }
        result = half4(res.xyz*rcp(weight+0.0001f), res.w*rcp((float)numSamples));
        ##endif
        ##if integrate_face == integrate_face_all
        outColor[unroll_cubeFace] = result;
        ##endif
      }
      //*/
      MRT_OUTPUT res;
      ##if integrate_face == integrate_face_all
      res.color0 = outColor[0];
      res.color1 = outColor[1];
      res.color2 = outColor[2];
      res.color3 = outColor[3];
      res.color4 = outColor[4];
      res.color5 = outColor[5];
      ##else
      res.color0 = result;
      ##endif
      return res;
    }
  }
  compile("target_vs", "light_probe_vs");
  compile("target_ps", "light_probe_ps");
}
