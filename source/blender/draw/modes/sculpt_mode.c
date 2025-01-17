/*
 * Copyright 2016, Blender Foundation.
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
 * Contributor(s): Blender Institute
 *
 */

/** \file blender/draw/modes/sculpt_mode.c
 *  \ingroup draw
 */

#include "DRW_engine.h"
#include "DRW_render.h"

#include "DNA_object_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_pbvh.h"
#include "BKE_paint.h"

#include "DEG_depsgraph.h"

/* If builtin shaders are needed */
#include "GPU_shader.h"
#include "GPU_matrix.h"

#include "draw_common.h"

#include "draw_mode_engines.h"

extern char datatoc_sculpt_mask_vert_glsl[];
extern char datatoc_gpu_shader_flat_color_frag_glsl[];
extern char datatoc_gpu_shader_3D_smooth_color_frag_glsl[];

/* *********** LISTS *********** */
/* All lists are per viewport specific datas.
 * They are all free when viewport changes engines
 * or is free itself. Use SCULPT_engine_init() to
 * initialize most of them and SCULPT_cache_init()
 * for SCULPT_PassList */

typedef struct SCULPT_PassList {
	/* Declare all passes here and init them in
	 * SCULPT_cache_init().
	 * Only contains (DRWPass *) */
	struct DRWPass *pass;
} SCULPT_PassList;

typedef struct SCULPT_FramebufferList {
	/* Contains all framebuffer objects needed by this engine.
	 * Only contains (GPUFrameBuffer *) */
	struct GPUFrameBuffer *fb;
} SCULPT_FramebufferList;

typedef struct SCULPT_TextureList {
	/* Contains all framebuffer textures / utility textures
	 * needed by this engine. Only viewport specific textures
	 * (not per object). Only contains (GPUTexture *) */
	struct GPUTexture *texture;
} SCULPT_TextureList;

typedef struct SCULPT_StorageList {
	/* Contains any other memory block that the engine needs.
	 * Only directly MEM_(m/c)allocN'ed blocks because they are
	 * free with MEM_freeN() when viewport is freed.
	 * (not per object) */
	struct CustomStruct *block;
	struct SCULPT_PrivateData *g_data;
} SCULPT_StorageList;

typedef struct SCULPT_Data {
	/* Struct returned by DRW_viewport_engine_data_ensure.
	 * If you don't use one of these, just make it a (void *) */
	// void *fbl;
	void *engine_type; /* Required */
	SCULPT_FramebufferList *fbl;
	SCULPT_TextureList *txl;
	SCULPT_PassList *psl;
	SCULPT_StorageList *stl;
} SCULPT_Data;

/* *********** STATIC *********** */

static struct {
	/* Custom shaders :
	 * Add sources to source/blender/draw/modes/shaders
	 * init in SCULPT_engine_init();
	 * free in SCULPT_engine_free(); */
	struct GPUShader *shader_flat;
	struct GPUShader *shader_smooth;
} e_data = {NULL}; /* Engine data */

typedef struct SCULPT_PrivateData {
	/* This keeps the references of the shading groups for
	 * easy access in SCULPT_cache_populate() */
	DRWShadingGroup *group_flat;
	DRWShadingGroup *group_smooth;
} SCULPT_PrivateData; /* Transient data */

/* *********** FUNCTIONS *********** */

/* Init Textures, Framebuffers, Storage and Shaders.
 * It is called for every frames.
 * (Optional) */
static void SCULPT_engine_init(void *vedata)
{
	SCULPT_TextureList *txl = ((SCULPT_Data *)vedata)->txl;
	SCULPT_FramebufferList *fbl = ((SCULPT_Data *)vedata)->fbl;
	SCULPT_StorageList *stl = ((SCULPT_Data *)vedata)->stl;

	UNUSED_VARS(txl, fbl, stl);

	if (!e_data.shader_flat) {
		e_data.shader_flat = DRW_shader_create(datatoc_sculpt_mask_vert_glsl, NULL,
		                                       datatoc_gpu_shader_flat_color_frag_glsl,
		                                       "#define SHADE_FLAT");
	}
	if (!e_data.shader_smooth) {
		e_data.shader_smooth = DRW_shader_create(datatoc_sculpt_mask_vert_glsl, NULL,
		                                         datatoc_gpu_shader_3D_smooth_color_frag_glsl, NULL);
	}
}

/* Here init all passes and shading groups
 * Assume that all Passes are NULL */
static void SCULPT_cache_init(void *vedata)
{
	SCULPT_PassList *psl = ((SCULPT_Data *)vedata)->psl;
	SCULPT_StorageList *stl = ((SCULPT_Data *)vedata)->stl;

	if (!stl->g_data) {
		/* Alloc transient pointers */
		stl->g_data = MEM_mallocN(sizeof(*stl->g_data), __func__);
	}

	{
		/* Create a pass */
		DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_EQUAL | DRW_STATE_MULTIPLY;
		psl->pass = DRW_pass_create("Sculpt Pass", state);

		/* Create a shadingGroup using a function in draw_common.c or custom one */
		/*
		 * stl->g_data->group = shgroup_dynlines_uniform_color(psl->pass, ts.colorWire);
		 * -- or --
		 * stl->g_data->group = DRW_shgroup_create(e_data.custom_shader, psl->pass);
		 */
		stl->g_data->group_flat = DRW_shgroup_create(e_data.shader_flat, psl->pass);
		stl->g_data->group_smooth = DRW_shgroup_create(e_data.shader_smooth, psl->pass);
	}
}

static bool object_is_flat(const Object *ob)
{
	Mesh *me = ob->data;
	if (me->mpoly && me->mpoly[0].flag & ME_SMOOTH) {
		return false;
	}
	else {
		return true;
	}
}

static void sculpt_draw_mask_cb(
        DRWShadingGroup *shgroup,
        void (*draw_fn)(DRWShadingGroup *shgroup, struct GPUBatch *geom),
        void *user_data)
{
	Object *ob = user_data;
	PBVH *pbvh = ob->sculpt->pbvh;

	if (pbvh) {
		BKE_pbvh_draw_cb(
		        pbvh, NULL, NULL, false, true,
		        (void (*)(void *, struct GPUBatch *))draw_fn, shgroup);
	}
}

/* Add geometry to shadingGroups. Execute for each objects */
static void SCULPT_cache_populate(void *vedata, Object *ob)
{
	SCULPT_PassList *psl = ((SCULPT_Data *)vedata)->psl;
	SCULPT_StorageList *stl = ((SCULPT_Data *)vedata)->stl;

	UNUSED_VARS(psl, stl);

	if (ob->type == OB_MESH) {
		const DRWContextState *draw_ctx = DRW_context_state_get();

		if (ob->sculpt && (ob == draw_ctx->obact)) {
			/* XXX, needed for dyntopo-undo (which clears).
			 * probably depsgraph should handlle? in 2.7x getting derived-mesh does this (mesh_build_data) */
			if (ob->sculpt->pbvh == NULL) {
				/* create PBVH immediately (would be created on the fly too,
				 * but this avoids waiting on first stroke) */
				Scene *scene = draw_ctx->scene;

				BKE_sculpt_update_mesh_elements(draw_ctx->depsgraph, scene, scene->toolsettings->sculpt, ob, false, false);
			}

			PBVH *pbvh = ob->sculpt->pbvh;
			if (pbvh && pbvh_has_mask(pbvh)) {
				/* Get geometry cache */
				DRWShadingGroup *shgroup = object_is_flat(ob) ? stl->g_data->group_flat : stl->g_data->group_smooth;

				DRW_shgroup_call_generate_add(shgroup, sculpt_draw_mask_cb, ob, ob->obmat);
			}
		}
	}
}

/* Optional: Post-cache_populate callback */
static void SCULPT_cache_finish(void *vedata)
{
	SCULPT_PassList *psl = ((SCULPT_Data *)vedata)->psl;
	SCULPT_StorageList *stl = ((SCULPT_Data *)vedata)->stl;

	/* Do something here! dependant on the objects gathered */
	UNUSED_VARS(psl, stl);
}

/* Draw time ! Control rendering pipeline from here */
static void SCULPT_draw_scene(void *vedata)
{
	SCULPT_PassList *psl = ((SCULPT_Data *)vedata)->psl;
	SCULPT_FramebufferList *fbl = ((SCULPT_Data *)vedata)->fbl;

	/* Default framebuffer and texture */
	DefaultFramebufferList *dfbl = DRW_viewport_framebuffer_list_get();
	DefaultTextureList *dtxl = DRW_viewport_texture_list_get();

	UNUSED_VARS(fbl, dfbl, dtxl);

	/* Show / hide entire passes, swap framebuffers ... whatever you fancy */
	/*
	 * DRW_framebuffer_texture_detach(dtxl->depth);
	 * DRW_framebuffer_bind(fbl->custom_fb);
	 * DRW_draw_pass(psl->pass);
	 * DRW_framebuffer_texture_attach(dfbl->default_fb, dtxl->depth, 0, 0);
	 * DRW_framebuffer_bind(dfbl->default_fb);
	 */

	/* ... or just render passes on default framebuffer. */
	DRW_draw_pass(psl->pass);

	/* If you changed framebuffer, double check you rebind
	 * the default one with its textures attached before finishing */
}

/* Cleanup when destroying the engine.
 * This is not per viewport ! only when quitting blender.
 * Mostly used for freeing shaders */
static void SCULPT_engine_free(void)
{
	DRW_SHADER_FREE_SAFE(e_data.shader_flat);
	DRW_SHADER_FREE_SAFE(e_data.shader_smooth);
}

static const DrawEngineDataSize SCULPT_data_size = DRW_VIEWPORT_DATA_SIZE(SCULPT_Data);

DrawEngineType draw_engine_sculpt_type = {
	NULL, NULL,
	N_("SculptMode"),
	&SCULPT_data_size,
	&SCULPT_engine_init,
	&SCULPT_engine_free,
	&SCULPT_cache_init,
	&SCULPT_cache_populate,
	&SCULPT_cache_finish,
	NULL, /* draw_background but not needed by mode engines */
	&SCULPT_draw_scene,
	NULL,
	NULL,
};
