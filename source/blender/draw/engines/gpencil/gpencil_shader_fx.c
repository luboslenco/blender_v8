/*
 * Copyright 2017, Blender Foundation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Antonio Vazquez
 *
 */

/** \file blender/draw/engines/gpencil/gpencil_shader_fx.c
 *  \ingroup draw
 */
#include "DNA_gpencil_types.h"
#include "DNA_shader_fx_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"
#include "DNA_camera_types.h"

#include "BKE_gpencil.h"
#include "BKE_shader_fx.h"

#include "DRW_engine.h"
#include "DRW_render.h"

#include "BKE_camera.h"

#include "ED_view3d.h"
#include "ED_gpencil.h"

#include "gpencil_engine.h"

extern char datatoc_gpencil_fx_blur_frag_glsl[];
extern char datatoc_gpencil_fx_colorize_frag_glsl[];
extern char datatoc_gpencil_fx_flip_frag_glsl[];
extern char datatoc_gpencil_fx_light_frag_glsl[];
extern char datatoc_gpencil_fx_pixel_frag_glsl[];
extern char datatoc_gpencil_fx_rim_prepare_frag_glsl[];
extern char datatoc_gpencil_fx_rim_resolve_frag_glsl[];
extern char datatoc_gpencil_fx_swirl_frag_glsl[];
extern char datatoc_gpencil_fx_wave_frag_glsl[];

/* verify if this fx is active */
static bool effect_is_active(bGPdata *gpd, ShaderFxData *fx, bool is_render)
{
	if (fx == NULL) {
		return false;
	}

	if (gpd == NULL) {
		return false;
	}

	bool is_edit = GPENCIL_ANY_EDIT_MODE(gpd);
	if (((fx->mode & eShaderFxMode_Editmode) == 0) && (is_edit)) {
		return false;
	}

	if (((fx->mode & eShaderFxMode_Realtime) && (is_render == false)) ||
	    ((fx->mode & eShaderFxMode_Render) && (is_render == true)))
	{
		return true;
	}

	return false;
}

/**
 * Get normal of draw using one stroke of visible layer
 * \param gpd        GP datablock
 * \param r_point    Point on plane
 * \param r_normal   Normal vector
 */
static bool get_normal_vector(bGPdata *gpd, float r_point[3], float r_normal[3])
{
	for (bGPDlayer *gpl = gpd->layers.first; gpl; gpl = gpl->next) {
		if (gpl->flag & GP_LAYER_HIDE)
			continue;

		/* get frame  */
		bGPDframe *gpf = gpl->actframe;
		if (gpf == NULL)
			continue;
		for (bGPDstroke *gps = gpf->strokes.first; gps; gps = gps->next) {
			if (gps->totpoints >= 3) {
				bGPDspoint *pt = &gps->points[0];
				BKE_gpencil_stroke_normal(gps, r_normal);
				/* in some weird situations, the normal cannot be calculated, so try next stroke */
				if ((r_normal[0] != 0.0f) || (r_normal[1] != 0.0f) || (r_normal[2] != 0.0f)) {
					copy_v3_v3(r_point, &pt->x);
					return true;
				}
			}
		}
	}

	return false;
}

/* helper to get near and far depth of field values */
static void GPENCIL_dof_nearfar(Object *camera, float coc, float nearfar[2])
{
	if (camera == NULL) {
		return;
	}

	const DRWContextState *draw_ctx = DRW_context_state_get();
	Scene *scene = draw_ctx->scene;
	Camera *cam = (Camera *)camera->data;

	float fstop = cam->gpu_dof.fstop;
	float focus_dist = BKE_camera_object_dof_distance(camera);
	float focal_len = cam->lens;

	/* this is factor that converts to the scene scale. focal length and sensor are expressed in mm
	 * unit.scale_length is how many meters per blender unit we have. We want to convert to blender units though
	 * because the shader reads coordinates in world space, which is in blender units.
	 * Note however that focus_distance is already in blender units and shall not be scaled here (see T48157). */
	float scale = (scene->unit.system) ? scene->unit.scale_length : 1.0f;
	float scale_camera = 0.001f / scale;
	/* we want radius here for the aperture number  */
	float aperture_scaled = 0.5f * scale_camera * focal_len / fstop;
	float focal_len_scaled = scale_camera * focal_len;

	float hyperfocal = (focal_len_scaled * focal_len_scaled) / (aperture_scaled * coc);
	nearfar[0] = (hyperfocal * focus_dist) / (hyperfocal + focal_len);
	nearfar[1] = (hyperfocal * focus_dist) / (hyperfocal - focal_len);
}

/* ****************  Shader Effects ***************************** */

/* Gaussian Blur FX
 * The effect is done using two shading groups because is faster to apply horizontal
 * and vertical in different operations.
 */
static void DRW_gpencil_fx_blur(
        ShaderFxData *fx, int ob_idx, GPENCIL_e_data *e_data,
        GPENCIL_Data *vedata, tGPencilObjectCache *cache)
{
	if (fx == NULL) {
		return;
	}

	BlurShaderFxData *fxd = (BlurShaderFxData *)fx;

	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;
	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	const DRWContextState *draw_ctx = DRW_context_state_get();
	View3D *v3d = draw_ctx->v3d;
	RegionView3D *rv3d = draw_ctx->rv3d;
	DRWShadingGroup *fx_shgrp;

	fxd->blur[0] = fxd->radius[0];
	fxd->blur[1] = fxd->radius[1];

	/* init weight */
	if (fxd->flag & FX_BLUR_DOF_MODE) {
		/* viewport and opengl render */
		Object *camera = NULL;
		if (rv3d) {
			if (rv3d->persp == RV3D_CAMOB) {
				camera = v3d->camera;
			}
		}
		else {
			camera = stl->storage->camera;
		}

		if (camera) {
			float nearfar[2];
			GPENCIL_dof_nearfar(camera, fxd->coc, nearfar);
			float zdepth = stl->g_data->gp_object_cache[ob_idx].zdepth;
			/* the object is on focus area */
			if ((zdepth >= nearfar[0]) && (zdepth <= nearfar[1])) {
				fxd->blur[0] = 0;
				fxd->blur[1] = 0;
			}
			else {
				float f;
				if (zdepth < nearfar[0]) {
					f = nearfar[0] - zdepth;
				}
				else {
					f = zdepth - nearfar[1];
				}
				fxd->blur[0] = f;
				fxd->blur[1] = f;
				CLAMP2(&fxd->blur[0], 0, fxd->radius[0]);
			}
		}
		else {
			/* if not camera view, the blur is disabled */
			fxd->blur[0] = 0;
			fxd->blur[1] = 0;
		}
	}

	struct GPUBatch *fxquad = DRW_cache_fullscreen_quad_get();

	fx_shgrp = DRW_shgroup_create(
	        e_data->gpencil_fx_blur_sh,
	        psl->fx_shader_pass_blend);
	DRW_shgroup_call_add(fx_shgrp, fxquad, NULL);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeColor", &e_data->temp_color_tx_a);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeDepth", &e_data->temp_depth_tx_a);
	DRW_shgroup_uniform_int(fx_shgrp, "blur", &fxd->blur[0], 2);

	DRW_shgroup_uniform_vec3(fx_shgrp, "loc", &cache->loc[0], 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixsize", stl->storage->pixsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixelsize", &U.pixelsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixfactor", &cache->pixfactor, 1);

	fxd->runtime.fx_sh = fx_shgrp;
}

/* Colorize FX */
static void DRW_gpencil_fx_colorize(
        ShaderFxData *fx, GPENCIL_e_data *e_data, GPENCIL_Data *vedata)
{
	if (fx == NULL) {
		return;
	}
	ColorizeShaderFxData *fxd = (ColorizeShaderFxData *)fx;
	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	DRWShadingGroup *fx_shgrp;

	struct GPUBatch *fxquad = DRW_cache_fullscreen_quad_get();
	fx_shgrp = DRW_shgroup_create(e_data->gpencil_fx_colorize_sh, psl->fx_shader_pass);
	DRW_shgroup_call_add(fx_shgrp, fxquad, NULL);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeColor", &e_data->temp_color_tx_a);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeDepth", &e_data->temp_depth_tx_a);
	DRW_shgroup_uniform_vec4(fx_shgrp, "low_color", &fxd->low_color[0], 1);
	DRW_shgroup_uniform_vec4(fx_shgrp, "high_color", &fxd->high_color[0], 1);
	DRW_shgroup_uniform_int(fx_shgrp, "mode", &fxd->mode, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "factor", &fxd->factor, 1);

	fxd->runtime.fx_sh = fx_shgrp;
}

/* Flip FX */
static void DRW_gpencil_fx_flip(
        ShaderFxData *fx, GPENCIL_e_data *e_data, GPENCIL_Data *vedata)
{
	if (fx == NULL) {
		return;
	}
	FlipShaderFxData *fxd = (FlipShaderFxData *)fx;
	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	DRWShadingGroup *fx_shgrp;

	fxd->flipmode = 100;
	/* the number works as bit flag */
	if (fxd->flag & FX_FLIP_HORIZONTAL) {
		fxd->flipmode += 10;
	}
	if (fxd->flag & FX_FLIP_VERTICAL) {
		fxd->flipmode += 1;
	}

	struct GPUBatch *fxquad = DRW_cache_fullscreen_quad_get();
	fx_shgrp = DRW_shgroup_create(e_data->gpencil_fx_flip_sh, psl->fx_shader_pass);
	DRW_shgroup_call_add(fx_shgrp, fxquad, NULL);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeColor", &e_data->temp_color_tx_a);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeDepth", &e_data->temp_depth_tx_a);
	DRW_shgroup_uniform_int(fx_shgrp, "flipmode", &fxd->flipmode, 1);

	DRW_shgroup_uniform_vec2(fx_shgrp, "wsize", DRW_viewport_size_get(), 1);

	fxd->runtime.fx_sh = fx_shgrp;
}

/* Light FX */
static void DRW_gpencil_fx_light(
        ShaderFxData *fx, GPENCIL_e_data *e_data, GPENCIL_Data *vedata,
        tGPencilObjectCache *cache)
{
	if (fx == NULL) {
		return;
	}
	LightShaderFxData *fxd = (LightShaderFxData *)fx;

	if (fxd->object == NULL) {
		return;
	}
	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;
	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	DRWShadingGroup *fx_shgrp;

	struct GPUBatch *fxquad = DRW_cache_fullscreen_quad_get();
	fx_shgrp = DRW_shgroup_create(e_data->gpencil_fx_light_sh, psl->fx_shader_pass);
	DRW_shgroup_call_add(fx_shgrp, fxquad, NULL);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeColor", &e_data->temp_color_tx_a);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeDepth", &e_data->temp_depth_tx_a);

	DRW_shgroup_uniform_vec2(fx_shgrp, "Viewport", DRW_viewport_size_get(), 1);

	/* location of the light using obj location as origin */
	copy_v3_v3(fxd->loc, &fxd->object->loc[0]);

	/* Calc distance to strokes plane
	 * The w component of location is used to transfer the distance to drawing plane
	 */
	float r_point[3], r_normal[3];
	float r_plane[4];
	bGPdata *gpd = cache->gpd;
	if (!get_normal_vector(gpd, r_point, r_normal)) {
		return;
	}
	mul_mat3_m4_v3(cache->obmat, r_normal); /* only rotation component */
	plane_from_point_normal_v3(r_plane, r_point, r_normal);
	float dt = dist_to_plane_v3(fxd->object->loc, r_plane);
	fxd->loc[3] = dt; /* use last element to save it */

	DRW_shgroup_uniform_vec4(fx_shgrp, "loc", &fxd->loc[0], 1);

	DRW_shgroup_uniform_float(fx_shgrp, "energy", &fxd->energy, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "ambient", &fxd->ambient, 1);

	DRW_shgroup_uniform_float(fx_shgrp, "pixsize", stl->storage->pixsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixelsize", &U.pixelsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixfactor", &cache->pixfactor, 1);

	fxd->runtime.fx_sh = fx_shgrp;
}

/* Pixelate FX */
static void DRW_gpencil_fx_pixel(
        ShaderFxData *fx, GPENCIL_e_data *e_data, GPENCIL_Data *vedata,
        tGPencilObjectCache *cache)
{
	if (fx == NULL) {
		return;
	}
	PixelShaderFxData *fxd = (PixelShaderFxData *)fx;

	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;
	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	DRWShadingGroup *fx_shgrp;
	bGPdata *gpd = cache->gpd;

	fxd->size[2] = (int)fxd->flag & FX_PIXEL_USE_LINES;

	struct GPUBatch *fxquad = DRW_cache_fullscreen_quad_get();
	fx_shgrp = DRW_shgroup_create(e_data->gpencil_fx_pixel_sh, psl->fx_shader_pass);
	DRW_shgroup_call_add(fx_shgrp, fxquad, NULL);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeColor", &e_data->temp_color_tx_a);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeDepth", &e_data->temp_depth_tx_a);
	DRW_shgroup_uniform_int(fx_shgrp, "size", &fxd->size[0], 3);
	DRW_shgroup_uniform_vec4(fx_shgrp, "color", &fxd->rgba[0], 1);

	DRW_shgroup_uniform_vec3(fx_shgrp, "loc", &cache->loc[0], 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixsize", stl->storage->pixsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixelsize", &U.pixelsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixfactor", &gpd->pixfactor, 1);

	fxd->runtime.fx_sh = fx_shgrp;
}

/* Rim FX */
static void DRW_gpencil_fx_rim(
        ShaderFxData *fx, GPENCIL_e_data *e_data, GPENCIL_Data *vedata,
        tGPencilObjectCache *cache)
{
	if (fx == NULL) {
		return;
	}
	RimShaderFxData *fxd = (RimShaderFxData *)fx;

	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;
	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	DRWShadingGroup *fx_shgrp;

	struct GPUBatch *fxquad = DRW_cache_fullscreen_quad_get();
	/* prepare pass */
	fx_shgrp = DRW_shgroup_create(
	        e_data->gpencil_fx_rim_prepare_sh,
	        psl->fx_shader_pass_blend);
	DRW_shgroup_call_add(fx_shgrp, fxquad, NULL);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeColor", &e_data->temp_color_tx_a);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeDepth", &e_data->temp_depth_tx_a);
	DRW_shgroup_uniform_vec2(fx_shgrp, "Viewport", DRW_viewport_size_get(), 1);

	DRW_shgroup_uniform_int(fx_shgrp, "offset", &fxd->offset[0], 2);
	DRW_shgroup_uniform_vec3(fx_shgrp, "rim_color", &fxd->rim_rgb[0], 1);
	DRW_shgroup_uniform_vec3(fx_shgrp, "mask_color", &fxd->mask_rgb[0], 1);

	DRW_shgroup_uniform_vec3(fx_shgrp, "loc", &cache->loc[0], 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixsize", stl->storage->pixsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixelsize", &U.pixelsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixfactor", &cache->pixfactor, 1);

	fxd->runtime.fx_sh = fx_shgrp;

	/* blur pass */
	fx_shgrp = DRW_shgroup_create(
	        e_data->gpencil_fx_blur_sh,
	        psl->fx_shader_pass_blend);
	DRW_shgroup_call_add(fx_shgrp, fxquad, NULL);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeColor", &e_data->temp_color_tx_rim);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeDepth", &e_data->temp_depth_tx_rim);
	DRW_shgroup_uniform_int(fx_shgrp, "blur", &fxd->blur[0], 2);

	DRW_shgroup_uniform_vec3(fx_shgrp, "loc", &cache->loc[0], 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixsize", stl->storage->pixsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixelsize", &U.pixelsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixfactor", &cache->pixfactor, 1);

	fxd->runtime.fx_sh_b = fx_shgrp;

	/* resolve pass */
	fx_shgrp = DRW_shgroup_create(
	        e_data->gpencil_fx_rim_resolve_sh,
	        psl->fx_shader_pass_blend);
	DRW_shgroup_call_add(fx_shgrp, fxquad, NULL);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeColor", &e_data->temp_color_tx_a);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeDepth", &e_data->temp_depth_tx_a);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeRim", &e_data->temp_color_tx_rim);
	DRW_shgroup_uniform_vec3(fx_shgrp, "mask_color", &fxd->mask_rgb[0], 1);
	DRW_shgroup_uniform_int(fx_shgrp, "mode", &fxd->mode, 1);

	fxd->runtime.fx_sh_c = fx_shgrp;
}

/* Swirl FX */
static void DRW_gpencil_fx_swirl(
        ShaderFxData *fx, GPENCIL_e_data *e_data, GPENCIL_Data *vedata,
       tGPencilObjectCache *cache)
{
	if (fx == NULL) {
		return;
	}
	SwirlShaderFxData *fxd = (SwirlShaderFxData *)fx;
	if (fxd->object == NULL) {
		return;
	}

	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;
	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	DRWShadingGroup *fx_shgrp;

	fxd->transparent = (int)fxd->flag & FX_SWIRL_MAKE_TRANSPARENT;

	struct GPUBatch *fxquad = DRW_cache_fullscreen_quad_get();
	fx_shgrp = DRW_shgroup_create(e_data->gpencil_fx_swirl_sh, psl->fx_shader_pass);
	DRW_shgroup_call_add(fx_shgrp, fxquad, NULL);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeColor", &e_data->temp_color_tx_a);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeDepth", &e_data->temp_depth_tx_a);

	DRW_shgroup_uniform_vec2(fx_shgrp, "Viewport", DRW_viewport_size_get(), 1);

	DRW_shgroup_uniform_vec3(fx_shgrp, "loc", &fxd->object->loc[0], 1);

	DRW_shgroup_uniform_int(fx_shgrp, "radius", &fxd->radius, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "angle", &fxd->angle, 1);
	DRW_shgroup_uniform_int(fx_shgrp, "transparent", &fxd->transparent, 1);

	DRW_shgroup_uniform_float(fx_shgrp, "pixsize", stl->storage->pixsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixelsize", &U.pixelsize, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "pixfactor", &cache->pixfactor, 1);

	fxd->runtime.fx_sh = fx_shgrp;
}

/* Wave Distorsion FX */
static void DRW_gpencil_fx_wave(
        ShaderFxData *fx, GPENCIL_e_data *e_data, GPENCIL_Data *vedata)
{
	if (fx == NULL) {
		return;
	}

	WaveShaderFxData *fxd = (WaveShaderFxData *)fx;

	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	struct GPUBatch *fxquad = DRW_cache_fullscreen_quad_get();

	DRWShadingGroup *fx_shgrp = DRW_shgroup_create(e_data->gpencil_fx_wave_sh, psl->fx_shader_pass);
	DRW_shgroup_call_add(fx_shgrp, fxquad, NULL);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeColor", &e_data->temp_color_tx_a);
	DRW_shgroup_uniform_texture_ref(fx_shgrp, "strokeDepth", &e_data->temp_depth_tx_a);
	DRW_shgroup_uniform_float(fx_shgrp, "amplitude", &fxd->amplitude, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "period", &fxd->period, 1);
	DRW_shgroup_uniform_float(fx_shgrp, "phase", &fxd->phase, 1);
	DRW_shgroup_uniform_int(fx_shgrp, "orientation", &fxd->orientation, 1);
	DRW_shgroup_uniform_vec2(fx_shgrp, "wsize", DRW_viewport_size_get(), 1);

	fxd->runtime.fx_sh = fx_shgrp;
}

/* ************************************************************** */

/* create all FX shaders */
void GPENCIL_create_fx_shaders(GPENCIL_e_data *e_data)
{
	/* fx shaders (all in screen space) */
	if (!e_data->gpencil_fx_blur_sh) {
		e_data->gpencil_fx_blur_sh = DRW_shader_create_fullscreen(
			datatoc_gpencil_fx_blur_frag_glsl, NULL);
	}
	if (!e_data->gpencil_fx_colorize_sh) {
		e_data->gpencil_fx_colorize_sh = DRW_shader_create_fullscreen(
			datatoc_gpencil_fx_colorize_frag_glsl, NULL);
	}
	if (!e_data->gpencil_fx_flip_sh) {
		e_data->gpencil_fx_flip_sh = DRW_shader_create_fullscreen(
			datatoc_gpencil_fx_flip_frag_glsl, NULL);
	}
	if (!e_data->gpencil_fx_light_sh) {
		e_data->gpencil_fx_light_sh = DRW_shader_create_fullscreen(
			datatoc_gpencil_fx_light_frag_glsl, NULL);
	}
	if (!e_data->gpencil_fx_pixel_sh) {
		e_data->gpencil_fx_pixel_sh = DRW_shader_create_fullscreen(
			datatoc_gpencil_fx_pixel_frag_glsl, NULL);
	}
	if (!e_data->gpencil_fx_rim_prepare_sh) {
		e_data->gpencil_fx_rim_prepare_sh = DRW_shader_create_fullscreen(
			datatoc_gpencil_fx_rim_prepare_frag_glsl, NULL);

		e_data->gpencil_fx_rim_resolve_sh = DRW_shader_create_fullscreen(
			datatoc_gpencil_fx_rim_resolve_frag_glsl, NULL);
	}
	if (!e_data->gpencil_fx_swirl_sh) {
		e_data->gpencil_fx_swirl_sh = DRW_shader_create_fullscreen(
			datatoc_gpencil_fx_swirl_frag_glsl, NULL);
	}
	if (!e_data->gpencil_fx_wave_sh) {
		e_data->gpencil_fx_wave_sh = DRW_shader_create_fullscreen(
			datatoc_gpencil_fx_wave_frag_glsl, NULL);
	}
}

/* free FX shaders */
void GPENCIL_delete_fx_shaders(GPENCIL_e_data *e_data)
{
	DRW_SHADER_FREE_SAFE(e_data->gpencil_fx_blur_sh);
	DRW_SHADER_FREE_SAFE(e_data->gpencil_fx_colorize_sh);
	DRW_SHADER_FREE_SAFE(e_data->gpencil_fx_flip_sh);
	DRW_SHADER_FREE_SAFE(e_data->gpencil_fx_light_sh);
	DRW_SHADER_FREE_SAFE(e_data->gpencil_fx_pixel_sh);
	DRW_SHADER_FREE_SAFE(e_data->gpencil_fx_rim_prepare_sh);
	DRW_SHADER_FREE_SAFE(e_data->gpencil_fx_rim_resolve_sh);
	DRW_SHADER_FREE_SAFE(e_data->gpencil_fx_swirl_sh);
	DRW_SHADER_FREE_SAFE(e_data->gpencil_fx_wave_sh);
}

/* create all passes used by FX */
void GPENCIL_create_fx_passes(GPENCIL_PassList *psl)
{
	psl->fx_shader_pass = DRW_pass_create(
	        "GPencil Shader FX Pass",
	        DRW_STATE_WRITE_COLOR |
	        DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS);
	psl->fx_shader_pass_blend = DRW_pass_create(
	        "GPencil Shader FX Pass",
	        DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND |
	        DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS);
}


/* prepare fx shading groups */
void DRW_gpencil_fx_prepare(
        struct GPENCIL_e_data *e_data, struct GPENCIL_Data *vedata,
        struct tGPencilObjectCache *cache)
{
	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;
	int ob_idx = cache->idx;

	if (cache->shader_fx.first == NULL) {
		return;
	}
	/* loop FX */
	for (ShaderFxData *fx = cache->shader_fx.first; fx; fx = fx->next) {
		if (effect_is_active(cache->gpd, fx, stl->storage->is_render)) {
			switch (fx->type) {
				case eShaderFxType_Blur:
					DRW_gpencil_fx_blur(fx, ob_idx, e_data, vedata, cache);
					break;
				case eShaderFxType_Colorize:
					DRW_gpencil_fx_colorize(fx, e_data, vedata);
					break;
				case eShaderFxType_Flip:
					DRW_gpencil_fx_flip(fx, e_data, vedata);
					break;
				case eShaderFxType_Light:
					DRW_gpencil_fx_light(fx, e_data, vedata, cache);
					break;
				case eShaderFxType_Pixel:
					DRW_gpencil_fx_pixel(fx, e_data, vedata, cache);
					break;
				case eShaderFxType_Rim:
					DRW_gpencil_fx_rim(fx, e_data, vedata, cache);
					break;
				case eShaderFxType_Swirl:
					DRW_gpencil_fx_swirl(fx, e_data, vedata, cache);
					break;
				case eShaderFxType_Wave:
					DRW_gpencil_fx_wave(fx, e_data, vedata);
					break;
				default:
					break;
			}
		}
	}

}

/* helper to draw one FX pass and do ping-pong copy */
static void gpencil_draw_fx_pass(
        GPENCIL_e_data *e_data,
        GPENCIL_PassList *psl,
        GPENCIL_FramebufferList *fbl,
        DRWShadingGroup *shgrp, bool blend)
{
	if (shgrp == NULL) {
		return;
	}

	static float clearcol[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	GPU_framebuffer_bind(fbl->temp_fb_b);
	GPU_framebuffer_clear_color_depth(fbl->temp_fb_b, clearcol, 1.0f);

	/* draw effect pass in temp texture (B) using as source the previous image
	 * existing in the other temp texture (A) */
	if (!blend) {
		DRW_draw_pass_subset(psl->fx_shader_pass, shgrp, shgrp);
	}
	else {
		DRW_draw_pass_subset(psl->fx_shader_pass_blend, shgrp, shgrp);
	}

	/* copy pass from b to a for ping-pong frame buffers */
	e_data->input_depth_tx = e_data->temp_depth_tx_b;
	e_data->input_color_tx = e_data->temp_color_tx_b;

	GPU_framebuffer_bind(fbl->temp_fb_a);
	GPU_framebuffer_clear_color_depth(fbl->temp_fb_a, clearcol, 1.0f);
	DRW_draw_pass(psl->mix_pass_noblend);
}

/* helper to manage gaussian blur passes */
static void draw_gpencil_blur_passes(
        struct GPENCIL_e_data *e_data,
        struct GPENCIL_Data *vedata,
        struct BlurShaderFxData *fxd)
{
	if (fxd->runtime.fx_sh == NULL) {
		return;
	}

	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	GPENCIL_FramebufferList *fbl = ((GPENCIL_Data *)vedata)->fbl;
	DRWShadingGroup *shgrp = fxd->runtime.fx_sh;
	int samples = fxd->samples;

	float bx = fxd->blur[0];
	float by = fxd->blur[1];

	/* the blur is done in two steps (Hor/Ver) because is faster and
	 * gets better result
	 *
	 * samples could be 0 and disable de blur effects because sometimes
	 * is easier animate the number of samples only, instead to animate the
	 * hide/unhide and the number of samples to make some effects.
	 */
	for (int b = 0; b < samples; b++) {
		/* horizontal */
		if (bx > 0) {
			fxd->blur[0] = bx;
			fxd->blur[1] = 0;
			gpencil_draw_fx_pass(e_data, psl, fbl, shgrp, true);
		}
		/* vertical */
		if (by > 0) {
			fxd->blur[0] = 0;
			fxd->blur[1] = by;
			gpencil_draw_fx_pass(e_data, psl, fbl, shgrp, true);
		}
	}
}

static void draw_gpencil_rim_blur(
        struct GPENCIL_e_data *UNUSED(e_data),
        struct GPENCIL_Data *vedata,
        struct RimShaderFxData *fxd)
{
	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	GPENCIL_FramebufferList *fbl = ((GPENCIL_Data *)vedata)->fbl;
	static float clearcol[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	GPU_framebuffer_bind(fbl->temp_fb_b);
	GPU_framebuffer_clear_color_depth(fbl->temp_fb_b, clearcol, 1.0f);
	DRW_draw_pass_subset(psl->fx_shader_pass_blend,
		fxd->runtime.fx_sh_b, fxd->runtime.fx_sh_b);

	/* copy pass from b for ping-pong frame buffers */
	GPU_framebuffer_bind(fbl->temp_fb_rim);
	GPU_framebuffer_clear_color_depth(fbl->temp_fb_rim, clearcol, 1.0f);
	DRW_draw_pass(psl->mix_pass_noblend);
}

/* helper to draw RIM passes */
static void draw_gpencil_rim_passes(
        struct GPENCIL_e_data *e_data,
        struct GPENCIL_Data *vedata,
        struct RimShaderFxData *fxd)
{
	if (fxd->runtime.fx_sh_b == NULL) {
		return;
	}

	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	GPENCIL_FramebufferList *fbl = ((GPENCIL_Data *)vedata)->fbl;

	static float clearcol[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	int bx = fxd->blur[0];
	int by = fxd->blur[1];

	/* prepare mask */
	GPU_framebuffer_bind(fbl->temp_fb_rim);
	GPU_framebuffer_clear_color_depth(fbl->temp_fb_rim, clearcol, 1.0f);
	DRW_draw_pass_subset(
	        psl->fx_shader_pass_blend,
	        fxd->runtime.fx_sh, fxd->runtime.fx_sh);

	/* blur rim */
	e_data->input_depth_tx = e_data->temp_depth_tx_b;
	e_data->input_color_tx = e_data->temp_color_tx_b;

	if ((fxd->samples > 0) && ((bx > 0) || (by > 0))) {
		for (int x = 0; x < fxd->samples; x++) {

			/* horizontal */
			fxd->blur[0] = bx;
			fxd->blur[1] = 0;
			draw_gpencil_rim_blur(e_data, vedata, fxd);

			/* Vertical */
			fxd->blur[0] = 0;
			fxd->blur[1] = by;
			draw_gpencil_rim_blur(e_data, vedata, fxd);

			fxd->blur[0] = bx;
			fxd->blur[1] = by;
		}
	}
	/* resolve */
	GPU_framebuffer_bind(fbl->temp_fb_b);
	GPU_framebuffer_clear_color_depth(fbl->temp_fb_b, clearcol, 1.0f);
	DRW_draw_pass_subset(
	        psl->fx_shader_pass_blend,
	        fxd->runtime.fx_sh_c, fxd->runtime.fx_sh_c);

	/* copy pass from b to a for ping-pong frame buffers */
	e_data->input_depth_tx = e_data->temp_depth_tx_b;
	e_data->input_color_tx = e_data->temp_color_tx_b;

	GPU_framebuffer_bind(fbl->temp_fb_a);
	GPU_framebuffer_clear_color_depth(fbl->temp_fb_a, clearcol, 1.0f);
	DRW_draw_pass(psl->mix_pass_noblend);
}

/* apply all object fx effects */
void DRW_gpencil_fx_draw(
        struct GPENCIL_e_data *e_data,
        struct GPENCIL_Data *vedata, struct tGPencilObjectCache *cache)
{
	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;
	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	GPENCIL_FramebufferList *fbl = ((GPENCIL_Data *)vedata)->fbl;

	/* loop FX modifiers */
	for (ShaderFxData *fx = cache->shader_fx.first; fx; fx = fx->next) {
		if (effect_is_active(cache->gpd, fx, stl->storage->is_render)) {
			switch (fx->type) {

				case eShaderFxType_Blur:
				{
					BlurShaderFxData *fxd = (BlurShaderFxData *)fx;
					draw_gpencil_blur_passes(e_data, vedata, fxd);
					break;
				}
				case eShaderFxType_Colorize:
				{
					ColorizeShaderFxData *fxd = (ColorizeShaderFxData *)fx;
					gpencil_draw_fx_pass(e_data, psl, fbl, fxd->runtime.fx_sh, false);
					break;
				}
				case eShaderFxType_Flip:
				{
					FlipShaderFxData *fxd = (FlipShaderFxData *)fx;
					gpencil_draw_fx_pass(e_data, psl, fbl, fxd->runtime.fx_sh, false);
					break;
				}
				case eShaderFxType_Light:
				{
					LightShaderFxData *fxd = (LightShaderFxData *)fx;
					gpencil_draw_fx_pass(e_data, psl, fbl, fxd->runtime.fx_sh, false);
					break;
				}
				case eShaderFxType_Pixel:
				{
					PixelShaderFxData *fxd = (PixelShaderFxData *)fx;
					gpencil_draw_fx_pass(e_data, psl, fbl, fxd->runtime.fx_sh, false);
					break;
				}
				case eShaderFxType_Rim:
				{
					RimShaderFxData *fxd = (RimShaderFxData *)fx;
					draw_gpencil_rim_passes(e_data, vedata, fxd);
					break;
				}
				case eShaderFxType_Swirl:
				{
					SwirlShaderFxData *fxd = (SwirlShaderFxData *)fx;
					gpencil_draw_fx_pass(e_data, psl, fbl, fxd->runtime.fx_sh, false);
					break;
				}
				case eShaderFxType_Wave:
				{
					WaveShaderFxData *fxd = (WaveShaderFxData *)fx;
					gpencil_draw_fx_pass(e_data, psl, fbl, fxd->runtime.fx_sh, false);
					break;
				}
				default:
					break;
			}
		}
	}
}
