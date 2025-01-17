include "shader_global.sh"
include "snoise.sh"
include "water_heightmap.sh"
include "depth_above.sh"

texture flowmap_temp_tex;
int flowmap_texture_size = 1024;
float flowmap_texture_size_meters = 200;
texture flowmap_heightmap_tex;
int height_texture_size = 1024;
float4 flowmap_heightmap_min_max = (0, 0, 0, 0);
texture flowmap_floodfill_tex;
float wind_dir_x = 0.6;
float wind_dir_y = 0.8;
float dir_scale = 0.01;
float4 world_to_flowmap_prev = (1,1,0,0);
float4 world_to_flowmap_add = (1,1,0,0);
float4 world_to_flowmap_heightmap = (1/32,1/32,0.5,0.5);
float4 water_flowmap_depth = float4(1, 0.1, 0.3, 1);
int water_flowmap_num_winds = 0;
buffer water_flowmap_winds;
float water_flowmap_slope = 1.0;
float flowmap_damping = 0.5;

int flowmap_height = 0;
interval flowmap_height : depth_above < 1, heightmap;

shader copy_flowmap_texture
{
  cull_mode=none;
  z_test=false;
  z_write=false;

  POSTFX_VS(1)

  (ps) { flowmap_temp_tex@tex = flowmap_temp_tex hlsl { Texture2D<float> flowmap_temp_tex@tex; } }

  hlsl(ps) {
    float copy_ps(VsOutput input) : SV_Target0
    {
      return flowmap_temp_tex[input.pos.xy].r;
    }
  }

  compile("target_ps", "copy_ps");
}

shader water_flowmap
{
  cull_mode=none;
  z_test=false;
  z_write=false;

  POSTFX_VS_TEXCOORD(1, tc)

  INIT_DEPTH_ABOVE(ps, depth_around)
  USE_DEPTH_ABOVE(ps, depth_around)

  (ps) {
    flowmap_temp_tex@smp2d = flowmap_temp_tex;
    wind_dir_dir_scale_water_flowmap_slope@f4 = (wind_dir_x, wind_dir_y, dir_scale*flowmap_texture_size/flowmap_texture_size_meters, water_flowmap_slope);
    height_texture_size@f4 = (1.0/height_texture_size, height_texture_size, 1./flowmap_texture_size, flowmap_texture_size);
    flowmap_heightmap_tex@smp2d = flowmap_heightmap_tex;
    flowmap_heightmap_min_max@f4 = flowmap_heightmap_min_max;
    flowmap_floodfill_tex@smp2d = flowmap_floodfill_tex;
    waterLevel_radius_flowmap_damping@f4 = (water_level, height_texture_size/flowmap_texture_size+2, height_texture_size/flowmap_texture_size, flowmap_damping);
    world_to_flowmap_prev@f4 = world_to_flowmap_prev;
    world_to_flowmap_heightmap@f4 = world_to_flowmap_heightmap;
    flowmap_add_to_world@f4 = (1.0/world_to_flowmap_add.x, 1.0/world_to_flowmap_add.y, -world_to_flowmap_add.z/world_to_flowmap_add.x, -world_to_flowmap_add.w/world_to_flowmap_add.y);
    water_flowmap_depth@f4 = water_flowmap_depth;
    water_flowmap_num_winds@i1 = (water_flowmap_num_winds);
    water_flowmap_winds_buf@cbuf = water_flowmap_winds hlsl {
      #include <fftWater/flow_map_inc.hlsli>
      cbuffer water_flowmap_winds_buf@cbuf
      {
        FlowmapWind water_flowmap_winds[MAX_FLOWMAP_WINDS];
      };
    };
  }

  INIT_WATER_HEIGHTMAP(ps)
  hlsl(ps) {
    #define water_heightmap_pages_samplerstate flowmap_heightmap_tex_samplerstate
  }
  USE_WATER_HEIGHTMAP(ps)

  hlsl(ps) {
    #define wind_dir (wind_dir_dir_scale_water_flowmap_slope.xy)
    #define dir_scale (wind_dir_dir_scale_water_flowmap_slope.z)
    #define water_flowmap_slope (wind_dir_dir_scale_water_flowmap_slope.w)
    #define waterLevel (waterLevel_radius_flowmap_damping.x)
    #define flowmap_damping (waterLevel_radius_flowmap_damping.w)

    float4 flowmap_ps(VsOutput input) : SV_Target0
    {
      float2 tc = input.tc;

      float2 worldPos = tc*flowmap_add_to_world.xy+flowmap_add_to_world.zw;
      float2 ftc = worldPos*world_to_flowmap_prev.xy+world_to_flowmap_prev.zw;
      float2 htc = worldPos*world_to_flowmap_heightmap.xy+world_to_flowmap_heightmap.zw;
      htc = floor(htc*height_texture_size.y)*height_texture_size.x;

      float4 f = tex2Dlod(flowmap_temp_tex, float4(ftc,0,0));
      float4 l = tex2Dlod(flowmap_temp_tex, float4(ftc.x-height_texture_size.z,ftc.y,0,0));
      float4 r = tex2Dlod(flowmap_temp_tex, float4(ftc.x+height_texture_size.z,ftc.y,0,0));
      float4 u = tex2Dlod(flowmap_temp_tex, float4(ftc.x,ftc.y-height_texture_size.z,0,0));
      float4 d = tex2Dlod(flowmap_temp_tex, float4(ftc.x,ftc.y+height_texture_size.z,0,0));

      f = tex2Dlod(flowmap_temp_tex, float4(ftc-f.xy*height_texture_size.z,0,0));
      f *= flowmap_damping;

      f.x += (l.w-r.w)*0.5;
      f.y += (u.w-d.w)*0.5;

      float waterHeight = waterLevel;
      get_water_height(worldPos, waterHeight);
      float bridgeHeight = waterHeight + 1;

      ##if flowmap_height == depth_above
        float depthVignette = 0;
        float h = getWorldBlurredDepth(float3(worldPos.x, 0, worldPos.y), depthVignette);
        if (h > bridgeHeight)
          h = tex2Dlod(flowmap_heightmap_tex, float4(htc,0,0)).r * flowmap_heightmap_min_max.z + flowmap_heightmap_min_max.w;
      ##else
        float h = tex2Dlod(flowmap_heightmap_tex, float4(htc,0,0)).r * flowmap_heightmap_min_max.z + flowmap_heightmap_min_max.w;
      ##endif

      float waterDepth = abs(waterHeight - h);

      bool isUp = h > waterHeight ? 1 : 0;
      bool isBorder = any(abs(tc*2-1)>1-height_texture_size.z*2);

      if (isUp || isBorder)
        f = 0;
      else
      {
        f.xy += wind_dir*dir_scale;
        float4 posNeg = float4(worldPos, -worldPos);
        for (int i = 0; i < water_flowmap_num_winds; ++i)
        {
          if (all(posNeg >= water_flowmap_winds[i].area))
            f.xy += water_flowmap_winds[i].dir*dir_scale;
        }

        ##if flowmap_height == depth_above
          float4 depthNeighbours = h;
          depthNeighbours.x = getWorldBlurredDepth(float3(worldPos.x - 1, 0, worldPos.y), depthVignette);
          depthNeighbours.y = getWorldBlurredDepth(float3(worldPos.x + 1, 0, worldPos.y), depthVignette);
          depthNeighbours.z = getWorldBlurredDepth(float3(worldPos.x, 0, worldPos.y - 1), depthVignette);
          depthNeighbours.w = getWorldBlurredDepth(float3(worldPos.x, 0, worldPos.y + 1), depthVignette);
          if (depthNeighbours.x > bridgeHeight)
             depthNeighbours.x = h;
          if (depthNeighbours.y > bridgeHeight)
             depthNeighbours.y = h;
          if (depthNeighbours.z > bridgeHeight)
             depthNeighbours.z = h;
          if (depthNeighbours.w > bridgeHeight)
             depthNeighbours.w = h;
          float2 depthGradient = float2(depthNeighbours.y - depthNeighbours.x, depthNeighbours.w - depthNeighbours.z);
          f.xy += depthGradient * water_flowmap_depth.x * max(1 - waterDepth * water_flowmap_depth.z, 0);
        ##endif

        ##if flowmap_floodfill_tex != NULL
          f.xy = (l.xy + r.xy + u.xy + d.xy) * 0.25;
          float4 heightNeighbours = h;
          heightNeighbours.x = tex2Dlod(flowmap_heightmap_tex, float4(htc.x - height_texture_size.x, htc.y, 0, 0)).r * flowmap_heightmap_min_max.z + flowmap_heightmap_min_max.w;
          heightNeighbours.y = tex2Dlod(flowmap_heightmap_tex, float4(htc.x + height_texture_size.x, htc.y, 0, 0)).r * flowmap_heightmap_min_max.z + flowmap_heightmap_min_max.w;
          heightNeighbours.z = tex2Dlod(flowmap_heightmap_tex, float4(htc.x, htc.y - height_texture_size.x, 0, 0)).r * flowmap_heightmap_min_max.z + flowmap_heightmap_min_max.w;
          heightNeighbours.w = tex2Dlod(flowmap_heightmap_tex, float4(htc.x, htc.y + height_texture_size.x, 0, 0)).r * flowmap_heightmap_min_max.z + flowmap_heightmap_min_max.w;
          if (any(heightNeighbours > waterHeight))
          {
            float2 heightGradient = float2(heightNeighbours.w - heightNeighbours.z, heightNeighbours.x - heightNeighbours.y);
            if (length(heightGradient) > 0.001)
            {
              heightGradient = normalize(heightGradient);
              float2 floodfillVec = tex2Dlod(flowmap_floodfill_tex, float4(htc,0,0)).rg * 2 - 1;
              heightGradient *= heightGradient.x * floodfillVec.x + heightGradient.y * floodfillVec.y;
              f.xy += heightGradient;
            }
          }
        ##endif

        float4 waterNeighbours = waterLevel;
        get_water_height(worldPos - float2(1, 0), waterNeighbours.x);
        get_water_height(worldPos + float2(1, 0), waterNeighbours.y);
        get_water_height(worldPos - float2(0, 1), waterNeighbours.z);
        get_water_height(worldPos + float2(0, 1), waterNeighbours.w);
        float2 waterGradient = float2(waterNeighbours.y - waterNeighbours.x, waterNeighbours.w - waterNeighbours.z);
        waterGradient = clamp(waterGradient, -1, 1);
        f.xy += waterGradient * water_flowmap_slope;
      }

      float speedFoam = length(f.xy) * water_flowmap_depth.y;
      float depthFoam = max(1 - waterDepth * water_flowmap_depth.w, 0);
      f.z = speedFoam + depthFoam;

      f.w = ((l.x-r.x+u.y-d.y)*0.5+(l.w+r.w+u.w+d.w))*0.25;

      return f;
    }
  }

  compile("target_ps", "flowmap_ps");
}
