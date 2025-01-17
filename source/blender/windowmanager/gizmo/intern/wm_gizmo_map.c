/*
 * ***** BEGIN GPL LICENSE BLOCK *****
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
 * The Original Code is Copyright (C) 2014 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/windowmanager/gizmo/intern/wm_gizmo_map.c
 *  \ingroup wm
 */

#include <string.h>

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_ghash.h"

#include "BKE_context.h"
#include "BKE_global.h"

#include "ED_screen.h"
#include "ED_select_utils.h"
#include "ED_view3d.h"

#include "GPU_glew.h"
#include "GPU_matrix.h"
#include "GPU_select.h"

#include "MEM_guardedalloc.h"

#include "WM_api.h"
#include "WM_types.h"
#include "wm_event_system.h"

/* for tool-tips */
#include "UI_interface.h"

#include "DEG_depsgraph.h"

/* own includes */
#include "wm_gizmo_wmapi.h"
#include "wm_gizmo_intern.h"

/**
 * Store all gizmo-maps here. Anyone who wants to register a gizmo for a certain
 * area type can query the gizmo-map to do so.
 */
static ListBase gizmomaptypes = {NULL, NULL};

/**
 * Update when gizmo-map types change.
 */
/* so operator removal can trigger update */
typedef enum eWM_GizmoFlagGroupTypeGlobalFlag {
	WM_GIZMOMAPTYPE_GLOBAL_UPDATE_INIT = (1 << 0),
	WM_GIZMOMAPTYPE_GLOBAL_UPDATE_REMOVE = (1 << 1),
} eWM_GizmoFlagGroupTypeGlobalFlag;

static eWM_GizmoFlagGroupTypeGlobalFlag wm_gzmap_type_update_flag = 0;

/**
 * Gizmo-map update tagging.
 */
enum {
	/** #gizmomap_prepare_drawing has run */
	GIZMOMAP_IS_PREPARE_DRAW = (1 << 0),
	GIZMOMAP_IS_REFRESH_CALLBACK = (1 << 1),
};


/* -------------------------------------------------------------------- */
/** \name wmGizmoMap Selection Array API
 *
 * Just handle ``wm_gizmomap_select_array_*``, not flags or callbacks.
 *
 * \{ */

static void wm_gizmomap_select_array_ensure_len_alloc(wmGizmoMap *gzmap, int len)
{
	wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
	if (len <= msel->len_alloc) {
		return;
	}
	msel->items = MEM_reallocN(msel->items, sizeof(*msel->items) * len);
	msel->len_alloc = len;
}

void wm_gizmomap_select_array_clear(wmGizmoMap *gzmap)
{
	wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
	MEM_SAFE_FREE(msel->items);
	msel->len = 0;
	msel->len_alloc = 0;
}

void wm_gizmomap_select_array_shrink(wmGizmoMap *gzmap, int len_subtract)
{
	wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
	msel->len -= len_subtract;
	if (msel->len <= 0) {
		wm_gizmomap_select_array_clear(gzmap);
	}
	else {
		if (msel->len < msel->len_alloc / 2) {
			msel->items = MEM_reallocN(msel->items, sizeof(*msel->items) * msel->len);
			msel->len_alloc = msel->len;
		}
	}
}

void wm_gizmomap_select_array_push_back(wmGizmoMap *gzmap, wmGizmo *gz)
{
	wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
	BLI_assert(msel->len <= msel->len_alloc);
	if (msel->len == msel->len_alloc) {
		msel->len_alloc = (msel->len + 1) * 2;
		msel->items = MEM_reallocN(msel->items, sizeof(*msel->items) * msel->len_alloc);
	}
	msel->items[msel->len++] = gz;
}

void wm_gizmomap_select_array_remove(wmGizmoMap *gzmap, wmGizmo *gz)
{
	wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
	/* remove gizmo from selected_gizmos array */
	for (int i = 0; i < msel->len; i++) {
		if (msel->items[i] == gz) {
			for (int j = i; j < (msel->len - 1); j++) {
				msel->items[j] = msel->items[j + 1];
			}
			wm_gizmomap_select_array_shrink(gzmap, 1);
			break;
		}
	}

}

/** \} */


/* -------------------------------------------------------------------- */
/** \name wmGizmoMap
 *
 * \{ */

/**
 * Creates a gizmo-map with all registered gizmos for that type
 */
wmGizmoMap *WM_gizmomap_new_from_type(
        const struct wmGizmoMapType_Params *gzmap_params)
{
	wmGizmoMapType *gzmap_type = WM_gizmomaptype_ensure(gzmap_params);
	wmGizmoMap *gzmap;

	gzmap = MEM_callocN(sizeof(wmGizmoMap), "GizmoMap");
	gzmap->type = gzmap_type;
	WM_gizmomap_tag_refresh(gzmap);

	/* create all gizmo-groups for this gizmo-map. We may create an empty one
	 * too in anticipation of gizmos from operators etc */
	for (wmGizmoGroupTypeRef *gzgt_ref = gzmap_type->grouptype_refs.first; gzgt_ref; gzgt_ref = gzgt_ref->next) {
		wm_gizmogroup_new_from_type(gzmap, gzgt_ref->type);
	}

	return gzmap;
}

void wm_gizmomap_remove(wmGizmoMap *gzmap)
{
	/* Clear first so further calls don't waste time trying to maintain correct array state. */
	wm_gizmomap_select_array_clear(gzmap);

	for (wmGizmoGroup *gzgroup = gzmap->groups.first, *gzgroup_next; gzgroup; gzgroup = gzgroup_next) {
		gzgroup_next = gzgroup->next;
		BLI_assert(gzgroup->parent_gzmap == gzmap);
		wm_gizmogroup_free(NULL, gzgroup);
	}
	BLI_assert(BLI_listbase_is_empty(&gzmap->groups));

	MEM_freeN(gzmap);
}


wmGizmoGroup *WM_gizmomap_group_find(
        struct wmGizmoMap *gzmap,
        const char *idname)
{
	wmGizmoGroupType *gzgt = WM_gizmogrouptype_find(idname, false);
	if (gzgt) {
		return WM_gizmomap_group_find_ptr(gzmap, gzgt);
	}
	return NULL;
}

wmGizmoGroup *WM_gizmomap_group_find_ptr(
        struct wmGizmoMap *gzmap,
        const struct wmGizmoGroupType *gzgt)
{
	for (wmGizmoGroup *gzgroup = gzmap->groups.first; gzgroup; gzgroup = gzgroup->next) {
		if (gzgroup->type == gzgt) {
			return gzgroup;
		}
	}
	return NULL;
}

const ListBase *WM_gizmomap_group_list(wmGizmoMap *gzmap)
{
	return &gzmap->groups;
}

bool WM_gizmomap_is_any_selected(const wmGizmoMap *gzmap)
{
	return gzmap->gzmap_context.select.len != 0;
}

/**
 * \note We could use a callback to define bounds, for now just use matrix location.
 */
bool WM_gizmomap_minmax(
        const wmGizmoMap *gzmap, bool UNUSED(use_hidden), bool use_select,
        float r_min[3], float r_max[3])
{
	if (use_select) {
		int i;
		for (i = 0; i < gzmap->gzmap_context.select.len; i++) {
			minmax_v3v3_v3(r_min, r_max, gzmap->gzmap_context.select.items[i]->matrix_basis[3]);
		}
		return i != 0;
	}
	else {
		bool ok = false;
		BLI_assert(!"TODO");
		return ok;
	}
}

/**
 * Creates and returns idname hash table for (visible) gizmos in \a gzmap
 *
 * \param poll  Polling function for excluding gizmos.
 * \param data  Custom data passed to \a poll
 *
 * TODO(campbell): this uses unreliable order,
 * best we use an iterator function instead of a hash.
 */
static GHash *WM_gizmomap_gizmo_hash_new(
        const bContext *C, wmGizmoMap *gzmap,
        bool (*poll)(const wmGizmo *, void *),
        void *data, const bool include_hidden)
{
	GHash *hash = BLI_ghash_ptr_new(__func__);

	/* collect gizmos */
	for (wmGizmoGroup *gzgroup = gzmap->groups.first; gzgroup; gzgroup = gzgroup->next) {
		if (WM_gizmo_group_type_poll(C, gzgroup->type)) {
			for (wmGizmo *gz = gzgroup->gizmos.first; gz; gz = gz->next) {
				if ((include_hidden || (gz->flag & WM_GIZMO_HIDDEN) == 0) &&
				    (!poll || poll(gz, data)))
				{
					BLI_ghash_insert(hash, gz, gz);
				}
			}
		}
	}

	return hash;
}

void WM_gizmomap_tag_refresh(wmGizmoMap *gzmap)
{
	if (gzmap) {
		/* We might want only to refresh some, for tag all steps. */
		for (int i = 0; i < WM_GIZMOMAP_DRAWSTEP_MAX; i++) {
			gzmap->update_flag[i] |= (
			        GIZMOMAP_IS_PREPARE_DRAW |
			        GIZMOMAP_IS_REFRESH_CALLBACK);
		}
	}
}

static bool gizmo_prepare_drawing(
        wmGizmoMap *gzmap, wmGizmo *gz,
        const bContext *C, ListBase *draw_gizmos,
        const eWM_GizmoFlagMapDrawStep drawstep)
{
	int do_draw = wm_gizmo_is_visible(gz);
	if (do_draw == 0) {
		/* skip */
	}
	else {
		/* Ensure we get RNA updates */
		if (do_draw & WM_GIZMO_IS_VISIBLE_UPDATE) {
			/* hover gizmos need updating, even if we don't draw them */
			wm_gizmo_update(gz, C, (gzmap->update_flag[drawstep] & GIZMOMAP_IS_PREPARE_DRAW) != 0);
		}
		if (do_draw & WM_GIZMO_IS_VISIBLE_DRAW) {
			BLI_addhead(draw_gizmos, BLI_genericNodeN(gz));
		}
		return true;
	}

	return false;
}

/**
 * Update gizmos of \a gzmap to prepare for drawing. Adds all gizmos that
 * should be drawn to list \a draw_gizmos, note that added items need freeing.
 */
static void gizmomap_prepare_drawing(
        wmGizmoMap *gzmap, const bContext *C, ListBase *draw_gizmos,
        const eWM_GizmoFlagMapDrawStep drawstep)
{
	if (!gzmap || BLI_listbase_is_empty(&gzmap->groups))
		return;
	wmGizmo *gz_modal = gzmap->gzmap_context.modal;

	/* only active gizmo needs updating */
	if (gz_modal) {
		if ((gz_modal->parent_gzgroup->type->flag & WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL) == 0) {
			if (wm_gizmogroup_is_visible_in_drawstep(gz_modal->parent_gzgroup, drawstep)) {
				if (gizmo_prepare_drawing(gzmap, gz_modal, C, draw_gizmos, drawstep)) {
					gzmap->update_flag[drawstep] &= ~GIZMOMAP_IS_PREPARE_DRAW;
				}
			}
			/* don't draw any other gizmos */
			return;
		}
	}

	for (wmGizmoGroup *gzgroup = gzmap->groups.first; gzgroup; gzgroup = gzgroup->next) {
		/* check group visibility - drawstep first to avoid unnecessary call of group poll callback */
		if (!wm_gizmogroup_is_visible_in_drawstep(gzgroup, drawstep) ||
		    !WM_gizmo_group_type_poll(C, gzgroup->type))
		{
			continue;
		}

		/* needs to be initialized on first draw */
		/* XXX weak: Gizmo-group may skip refreshing if it's invisible (map gets untagged nevertheless) */
		if (gzmap->update_flag[drawstep] & GIZMOMAP_IS_REFRESH_CALLBACK) {
			/* force refresh again. */
			gzgroup->init_flag &= ~WM_GIZMOGROUP_INIT_REFRESH;
		}
		/* Calls `setup`, `setup_keymap` and `refresh` if they're defined. */
		wm_gizmogroup_ensure_initialized(gzgroup, C);

		/* prepare drawing */
		if (gzgroup->type->draw_prepare) {
			gzgroup->type->draw_prepare(C, gzgroup);
		}

		for (wmGizmo *gz = gzgroup->gizmos.first; gz; gz = gz->next) {
			gizmo_prepare_drawing(gzmap, gz, C, draw_gizmos, drawstep);
		}
	}

	gzmap->update_flag[drawstep] &=
	        ~(GIZMOMAP_IS_REFRESH_CALLBACK |
	          GIZMOMAP_IS_PREPARE_DRAW);
}

/**
 * Draw all visible gizmos in \a gzmap.
 * Uses global draw_gizmos listbase.
 */
static void gizmos_draw_list(const wmGizmoMap *gzmap, const bContext *C, ListBase *draw_gizmos)
{
	/* Can be empty if we're dynamically added and removed. */
	if ((gzmap == NULL) || BLI_listbase_is_empty(&gzmap->groups)) {
		return;
	}

	/* TODO this will need it own shader probably? don't think it can be handled from that point though. */
/*	const bool use_lighting = (U.gizmo_flag & V3D_GIZMO_SHADED) != 0; */

	bool is_depth_prev = false;

	/* draw_gizmos contains all visible gizmos - draw them */
	for (LinkData *link = draw_gizmos->first, *link_next; link; link = link_next) {
		wmGizmo *gz = link->data;
		link_next = link->next;

		bool is_depth = (gz->parent_gzgroup->type->flag & WM_GIZMOGROUPTYPE_DEPTH_3D) != 0;

		/* Weak! since we don't 100% support depth yet (select ignores depth) always show highlighted */
		if (is_depth && (gz->state & WM_GIZMO_STATE_HIGHLIGHT)) {
			is_depth = false;
		}

		if (is_depth == is_depth_prev) {
			/* pass */
		}
		else {
			if (is_depth) {
				glEnable(GL_DEPTH_TEST);
			}
			else {
				glDisable(GL_DEPTH_TEST);
			}
			is_depth_prev = is_depth;
		}

		/* XXX force AntiAlias Gizmos. */
		glEnable(GL_LINE_SMOOTH);
		glEnable(GL_POLYGON_SMOOTH);

		gz->type->draw(C, gz);

		glDisable(GL_LINE_SMOOTH);
		glDisable(GL_POLYGON_SMOOTH);

		/* free/remove gizmo link after drawing */
		BLI_freelinkN(draw_gizmos, link);
	}

	if (is_depth_prev) {
		glDisable(GL_DEPTH_TEST);
	}
}

void WM_gizmomap_draw(
        wmGizmoMap *gzmap, const bContext *C,
        const eWM_GizmoFlagMapDrawStep drawstep)
{
	if (!WM_gizmo_context_check_drawstep(C, drawstep)) {
		return;
	}

	ListBase draw_gizmos = {NULL};

	gizmomap_prepare_drawing(gzmap, C, &draw_gizmos, drawstep);
	gizmos_draw_list(gzmap, C, &draw_gizmos);
	BLI_assert(BLI_listbase_is_empty(&draw_gizmos));
}

static void gizmo_draw_select_3D_loop(
        const bContext *C, ListBase *visible_gizmos,
        const wmGizmo *gz_stop)
{
	int select_id = 0;
	wmGizmo *gz;

	/* TODO(campbell): this depends on depth buffer being written to, currently broken for the 3D view. */
	bool is_depth_prev = false;
	bool is_depth_skip_prev = false;

	for (LinkData *link = visible_gizmos->first; link; link = link->next, select_id++) {
		gz = link->data;
		if (gz == gz_stop) {
			break;
		}
		if (gz->type->draw_select == NULL) {
			continue;
		}

		bool is_depth = (gz->parent_gzgroup->type->flag & WM_GIZMOGROUPTYPE_DEPTH_3D) != 0;
		if (is_depth == is_depth_prev) {
			/* pass */
		}
		else {
			if (is_depth) {
				glEnable(GL_DEPTH_TEST);
			}
			else {
				glDisable(GL_DEPTH_TEST);
			}
			is_depth_prev = is_depth;
		}
		bool is_depth_skip = (gz->flag & WM_GIZMO_SELECT_BACKGROUND) != 0;
		if (is_depth_skip == is_depth_skip_prev) {
			/* pass */
		}
		else {
			glDepthMask(!is_depth_skip);
			is_depth_skip_prev = is_depth_skip;
		}

		/* pass the selection id shifted by 8 bits. Last 8 bits are used for selected gizmo part id */

		gz->type->draw_select(C, gz, select_id << 8);
	}

	if (is_depth_prev) {
		glDisable(GL_DEPTH_TEST);
	}
	if (is_depth_skip_prev) {
		glDepthMask(true);
	}
}

static int gizmo_find_intersected_3d_intern(
        ListBase *visible_gizmos, const bContext *C, const int co[2],
        const int hotspot, const wmGizmo *gz_stop)
{
	ScrArea *sa = CTX_wm_area(C);
	ARegion *ar = CTX_wm_region(C);
	View3D *v3d = sa->spacedata.first;
	rcti rect;
	/* Almost certainly overkill, but allow for many custom gizmos. */
	GLuint buffer[MAXPICKBUF];
	short hits;
	const bool do_passes = GPU_select_query_check_active();

	BLI_rcti_init_pt_radius(&rect, co, hotspot);

	ED_view3d_draw_setup_view(CTX_wm_window(C), CTX_data_depsgraph(C), CTX_data_scene(C), ar, v3d, NULL, NULL, &rect);

	if (do_passes)
		GPU_select_begin(buffer, ARRAY_SIZE(buffer), &rect, GPU_SELECT_NEAREST_FIRST_PASS, 0);
	else
		GPU_select_begin(buffer, ARRAY_SIZE(buffer), &rect, GPU_SELECT_ALL, 0);
	/* do the drawing */
	gizmo_draw_select_3D_loop(C, visible_gizmos, gz_stop);

	hits = GPU_select_end();

	if (do_passes && (hits > 0)) {
		GPU_select_begin(buffer, ARRAY_SIZE(buffer), &rect, GPU_SELECT_NEAREST_SECOND_PASS, hits);
		gizmo_draw_select_3D_loop(C, visible_gizmos, gz_stop);
		GPU_select_end();
	}

	ED_view3d_draw_setup_view(CTX_wm_window(C), CTX_data_depsgraph(C), CTX_data_scene(C), ar, v3d, NULL, NULL, NULL);

	const GLuint *hit_near = GPU_select_buffer_near(buffer, hits);

	return hit_near ? hit_near[3] : -1;
}

/**
 * Try to find a 3D gizmo at screen-space coordinate \a co. Uses OpenGL picking.
 */
static wmGizmo *gizmo_find_intersected_3d(
        bContext *C, const int co[2], ListBase *visible_gizmos,
        int *r_part)
{
	wmGizmo *result = NULL;
	int hit = -1;

	*r_part = 0;

	/* set up view matrices */
	view3d_operator_needs_opengl(C);

	/* Search for 3D gizmo's that use the 2D callback for checking intersections. */
	bool has_3d = false;
	{
		int select_id = 0;
		for (LinkData *link = visible_gizmos->first; link; link = link->next, select_id++) {
			wmGizmo *gz = link->data;
			if (gz->type->test_select) {
				if ((*r_part = gz->type->test_select(C, gz, co)) != -1) {
					hit = select_id;
					result = gz;
					break;
				}
			}
			else {
				has_3d = true;
			}
		}
	}

	/* Search for 3D intersections if they're before 2D that have been found (if any).
	 * This way we always use the first hit. */
	if (has_3d) {
		const int hotspot_radii[] = {
			3 * U.pixelsize,
			/* This runs on mouse move, careful doing too many tests! */
			10 * U.pixelsize,
		};
		for (int i = 0; i < ARRAY_SIZE(hotspot_radii); i++) {
			hit = gizmo_find_intersected_3d_intern(visible_gizmos, C, co, hotspot_radii[i], result);
			if (hit != -1) {
				break;
			}
		}

		if (hit != -1) {
			LinkData *link = BLI_findlink(visible_gizmos, hit >> 8);
			if (link != NULL) {
				*r_part = hit & 255;
				result = link->data;
			}
			else {
				/* All gizmos should use selection ID they're given as part of the callback,
				 * if they don't it will attempt tp lookup non-existing index. */
				BLI_assert(0);
			}
		}
	}

	return result;
}

/**
 * Try to find a gizmo under the mouse position. 2D intersections have priority over
 * 3D ones (could check for smallest screen-space distance but not needed right now).
 */
wmGizmo *wm_gizmomap_highlight_find(
        wmGizmoMap *gzmap, bContext *C, const wmEvent *event,
        int *r_part)
{
	wmGizmo *gz = NULL;
	ListBase visible_3d_gizmos = {NULL};
	bool do_step[WM_GIZMOMAP_DRAWSTEP_MAX];

	for (int i = 0; i < ARRAY_SIZE(do_step); i++) {
		do_step[i] = WM_gizmo_context_check_drawstep(C, i);
	}

	for (wmGizmoGroup *gzgroup = gzmap->groups.first; gzgroup; gzgroup = gzgroup->next) {

		/* If it were important we could initialize here,
		 * but this only happens when events are handled before drawing,
		 * just skip to keep code-path for initializing gizmos simple. */
		if ((gzgroup->init_flag & WM_GIZMOGROUP_INIT_SETUP) == 0) {
			continue;
		}

		if (WM_gizmo_group_type_poll(C, gzgroup->type)) {
			eWM_GizmoFlagMapDrawStep step;
			if (gzgroup->type->flag & WM_GIZMOGROUPTYPE_3D) {
				step = WM_GIZMOMAP_DRAWSTEP_3D;
			}
			else {
				step = WM_GIZMOMAP_DRAWSTEP_2D;
			}

			if (do_step[step]) {
				if ((gzmap->update_flag[step] & GIZMOMAP_IS_REFRESH_CALLBACK) &&
				    (gzgroup->type->refresh != NULL))
				{
					gzgroup->type->refresh(C, gzgroup);
					/* cleared below */
				}
				if (step == WM_GIZMOMAP_DRAWSTEP_3D) {
					wm_gizmogroup_intersectable_gizmos_to_list(gzgroup, &visible_3d_gizmos);
				}
				else if (step == WM_GIZMOMAP_DRAWSTEP_2D) {
					if ((gz = wm_gizmogroup_find_intersected_gizmo(gzgroup, C, event, r_part))) {
						break;
					}
				}
			}
		}
	}

	if (!BLI_listbase_is_empty(&visible_3d_gizmos)) {
		/* 2D gizmos get priority. */
		if (gz == NULL) {
			gz = gizmo_find_intersected_3d(C, event->mval, &visible_3d_gizmos, r_part);
		}
		BLI_freelistN(&visible_3d_gizmos);
	}

	gzmap->update_flag[WM_GIZMOMAP_DRAWSTEP_3D] &= ~GIZMOMAP_IS_REFRESH_CALLBACK;
	gzmap->update_flag[WM_GIZMOMAP_DRAWSTEP_2D] &= ~GIZMOMAP_IS_REFRESH_CALLBACK;

	return gz;
}

void WM_gizmomap_add_handlers(ARegion *ar, wmGizmoMap *gzmap)
{
	wmEventHandler *handler;

	for (handler = ar->handlers.first; handler; handler = handler->next) {
		if (handler->gizmo_map == gzmap) {
			return;
		}
	}

	handler = MEM_callocN(sizeof(wmEventHandler), "gizmo handler");

	BLI_assert(gzmap == ar->gizmo_map);
	handler->gizmo_map = gzmap;
	BLI_addtail(&ar->handlers, handler);
}

void wm_gizmomaps_handled_modal_update(
        bContext *C, wmEvent *event, wmEventHandler *handler)
{
	const bool modal_running = (handler->op != NULL);

	/* happens on render or when joining areas */
	if (!handler->op_region || !handler->op_region->gizmo_map) {
		return;
	}

	wmGizmoMap *gzmap = handler->op_region->gizmo_map;
	wmGizmo *gz = wm_gizmomap_modal_get(gzmap);
	ScrArea *area = CTX_wm_area(C);
	ARegion *region = CTX_wm_region(C);

	wm_gizmomap_handler_context(C, handler);

	/* regular update for running operator */
	if (modal_running) {
		wmGizmoOpElem *mpop = gz ? WM_gizmo_operator_get(gz, gz->highlight_part) : NULL;
		if (gz && mpop && (mpop->type != NULL) && (mpop->type == handler->op->type)) {
			wmGizmoFnModal modal_fn = gz->custom_modal ? gz->custom_modal : gz->type->modal;
			if (modal_fn != NULL) {
				int retval = modal_fn(C, gz, event, 0);
				/* The gizmo is tried to the operator, we can't choose when to exit. */
				BLI_assert(retval & OPERATOR_RUNNING_MODAL);
				UNUSED_VARS_NDEBUG(retval);
			}
		}
	}
	/* operator not running anymore */
	else {
		wm_gizmomap_highlight_set(gzmap, C, NULL, 0);
		if (gz) {
			/* This isn't defined if it ends because of success of cancel, we may want to change. */
			bool cancel = true;
			if (gz->type->exit) {
				gz->type->exit(C, gz, cancel);
			}
			wm_gizmomap_modal_set(gzmap, C, gz, NULL, false);
		}
	}

	/* restore the area */
	CTX_wm_area_set(C, area);
	CTX_wm_region_set(C, region);
}

/**
 * Deselect all selected gizmos in \a gzmap.
 * \return if selection has changed.
 */
bool wm_gizmomap_deselect_all(wmGizmoMap *gzmap)
{
	wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;

	if (msel->items == NULL || msel->len == 0) {
		return false;
	}

	for (int i = 0; i < msel->len; i++) {
		wm_gizmo_select_set_ex(gzmap, msel->items[i], false, false, true);
	}

	wm_gizmomap_select_array_clear(gzmap);

	/* always return true, we already checked
	 * if there's anything to deselect */
	return true;
}

BLI_INLINE bool gizmo_selectable_poll(const wmGizmo *gz, void *UNUSED(data))
{
	return (gz->parent_gzgroup->type->flag & WM_GIZMOGROUPTYPE_SELECT);
}

/**
 * Select all selectable gizmos in \a gzmap.
 * \return if selection has changed.
 */
static bool wm_gizmomap_select_all_intern(
        bContext *C, wmGizmoMap *gzmap)
{
	wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
	/* GHash is used here to avoid having to loop over all gizmos twice (once to
	 * get tot_sel for allocating, once for actually selecting). Instead we collect
	 * selectable gizmos in hash table and use this to get tot_sel and do selection */

	GHash *hash = WM_gizmomap_gizmo_hash_new(C, gzmap, gizmo_selectable_poll, NULL, true);
	GHashIterator gh_iter;
	int i;
	bool changed = false;

	wm_gizmomap_select_array_ensure_len_alloc(gzmap, BLI_ghash_len(hash));

	GHASH_ITER_INDEX (gh_iter, hash, i) {
		wmGizmo *gz_iter = BLI_ghashIterator_getValue(&gh_iter);
		WM_gizmo_select_set(gzmap, gz_iter, true);
	}
	/* highlight first gizmo */
	wm_gizmomap_highlight_set(gzmap, C, msel->items[0], msel->items[0]->highlight_part);

	BLI_assert(BLI_ghash_len(hash) == msel->len);

	BLI_ghash_free(hash, NULL, NULL);
	return changed;
}

/**
 * Select/Deselect all selectable gizmos in \a gzmap.
 * \return if selection has changed.
 *
 * TODO select all by type
 */
bool WM_gizmomap_select_all(bContext *C, wmGizmoMap *gzmap, const int action)
{
	bool changed = false;

	switch (action) {
		case SEL_SELECT:
			changed = wm_gizmomap_select_all_intern(C, gzmap);
			break;
		case SEL_DESELECT:
			changed = wm_gizmomap_deselect_all(gzmap);
			break;
		default:
			BLI_assert(0);
			break;
	}

	if (changed)
		WM_event_add_mousemove(C);

	return changed;
}

/**
 * Prepare context for gizmo handling (but only if area/region is
 * part of screen). Version of #wm_handler_op_context for gizmos.
 */
void wm_gizmomap_handler_context(bContext *C, wmEventHandler *handler)
{
	bScreen *screen = CTX_wm_screen(C);

	if (screen) {
		if (handler->op_area == NULL) {
			/* do nothing in this context */
		}
		else {
			ScrArea *sa;

			for (sa = screen->areabase.first; sa; sa = sa->next)
				if (sa == handler->op_area)
					break;
			if (sa == NULL) {
				/* when changing screen layouts with running modal handlers (like render display), this
				 * is not an error to print */
				if (handler->gizmo_map == NULL)
					printf("internal error: modal gizmo-map handler has invalid area\n");
			}
			else {
				ARegion *ar;
				CTX_wm_area_set(C, sa);
				for (ar = sa->regionbase.first; ar; ar = ar->next)
					if (ar == handler->op_region)
						break;
				/* XXX no warning print here, after full-area and back regions are remade */
				if (ar)
					CTX_wm_region_set(C, ar);
			}
		}
	}
}

bool WM_gizmomap_cursor_set(const wmGizmoMap *gzmap, wmWindow *win)
{
	wmGizmo *gz = gzmap->gzmap_context.highlight;
	if (gz && gz->type->cursor_get) {
		WM_cursor_set(win, gz->type->cursor_get(gz));
		return true;
	}

	return false;
}

bool wm_gizmomap_highlight_set(
        wmGizmoMap *gzmap, const bContext *C, wmGizmo *gz, int part)
{
	if ((gz != gzmap->gzmap_context.highlight) ||
	    (gz && part != gz->highlight_part))
	{
		if (gzmap->gzmap_context.highlight) {
			gzmap->gzmap_context.highlight->state &= ~WM_GIZMO_STATE_HIGHLIGHT;
			gzmap->gzmap_context.highlight->highlight_part = -1;
		}

		gzmap->gzmap_context.highlight = gz;

		if (gz) {
			gz->state |= WM_GIZMO_STATE_HIGHLIGHT;
			gz->highlight_part = part;
			gzmap->gzmap_context.last_cursor = -1;

			if (C && gz->type->cursor_get) {
				wmWindow *win = CTX_wm_window(C);
				gzmap->gzmap_context.last_cursor = win->cursor;
				WM_cursor_set(win, gz->type->cursor_get(gz));
			}
		}
		else {
			if (C && gzmap->gzmap_context.last_cursor != -1) {
				wmWindow *win = CTX_wm_window(C);
				WM_cursor_set(win, gzmap->gzmap_context.last_cursor);
			}
		}

		/* tag the region for redraw */
		if (C) {
			ARegion *ar = CTX_wm_region(C);
			ED_region_tag_redraw(ar);
		}

		return true;
	}

	return false;
}

wmGizmo *wm_gizmomap_highlight_get(wmGizmoMap *gzmap)
{
	return gzmap->gzmap_context.highlight;
}

/**
 * Caller should call exit when (enable == False).
 */
void wm_gizmomap_modal_set(
        wmGizmoMap *gzmap, bContext *C, wmGizmo *gz, const wmEvent *event, bool enable)
{
	if (enable) {
		BLI_assert(gzmap->gzmap_context.modal == NULL);
		wmWindow *win = CTX_wm_window(C);

		WM_tooltip_clear(C, win);

		if (gz->type->invoke &&
		    (gz->type->modal || gz->custom_modal))
		{
			const int retval = gz->type->invoke(C, gz, event);
			if ((retval & OPERATOR_RUNNING_MODAL) == 0) {
				return;
			}
		}

		gz->state |= WM_GIZMO_STATE_MODAL;
		gzmap->gzmap_context.modal = gz;

		if ((gz->flag & WM_GIZMO_GRAB_CURSOR) &&
		    (event->is_motion_absolute == false))
		{
			WM_cursor_grab_enable(win, true, true, NULL);
			copy_v2_v2_int(gzmap->gzmap_context.event_xy, &event->x);
			gzmap->gzmap_context.event_grabcursor = win->grabcursor;
		}
		else {
			gzmap->gzmap_context.event_xy[0] = INT_MAX;
		}

		struct wmGizmoOpElem *mpop = WM_gizmo_operator_get(gz, gz->highlight_part);
		if (mpop && mpop->type) {
			const int retval = WM_operator_name_call_ptr(C, mpop->type, WM_OP_INVOKE_DEFAULT, &mpop->ptr);
			if ((retval & OPERATOR_RUNNING_MODAL) == 0) {
				wm_gizmomap_modal_set(gzmap, C, gz, event, false);
			}

			/* we failed to hook the gizmo to the operator handler or operator was cancelled, return */
			if (!gzmap->gzmap_context.modal) {
				gz->state &= ~WM_GIZMO_STATE_MODAL;
				MEM_SAFE_FREE(gz->interaction_data);
			}
			return;
		}
	}
	else {
		BLI_assert(ELEM(gzmap->gzmap_context.modal, NULL, gz));

		/* deactivate, gizmo but first take care of some stuff */
		if (gz) {
			gz->state &= ~WM_GIZMO_STATE_MODAL;
			MEM_SAFE_FREE(gz->interaction_data);
		}
		gzmap->gzmap_context.modal = NULL;

		if (C) {
			wmWindow *win = CTX_wm_window(C);
			if (gzmap->gzmap_context.event_xy[0] != INT_MAX) {
				/* Check if some other part of Blender (typically operators)
				 * have adjusted the grab mode since it was set.
				 * If so: warp, so we have a predictable outcome. */
				if (gzmap->gzmap_context.event_grabcursor == win->grabcursor) {
					WM_cursor_grab_disable(win, gzmap->gzmap_context.event_xy);
				}
				else {
					WM_cursor_warp(win, UNPACK2(gzmap->gzmap_context.event_xy));
				}
			}
			ED_region_tag_redraw(CTX_wm_region(C));
			WM_event_add_mousemove(C);
		}

		gzmap->gzmap_context.event_xy[0] = INT_MAX;
	}
}

wmGizmo *wm_gizmomap_modal_get(wmGizmoMap *gzmap)
{
	return gzmap->gzmap_context.modal;
}

wmGizmo **wm_gizmomap_selected_get(wmGizmoMap *gzmap, int *r_selected_len)
{
	*r_selected_len = gzmap->gzmap_context.select.len;
	return gzmap->gzmap_context.select.items;
}

ListBase *wm_gizmomap_groups_get(wmGizmoMap *gzmap)
{
	return &gzmap->groups;
}

void WM_gizmomap_message_subscribe(
        bContext *C, wmGizmoMap *gzmap, ARegion *ar, struct wmMsgBus *mbus)
{
	for (wmGizmoGroup *gzgroup = gzmap->groups.first; gzgroup; gzgroup = gzgroup->next) {
		if (!WM_gizmo_group_type_poll(C, gzgroup->type)) {
			continue;
		}
		for (wmGizmo *gz = gzgroup->gizmos.first; gz; gz = gz->next) {
			if (gz->flag & WM_GIZMO_HIDDEN) {
				continue;
			}
			WM_gizmo_target_property_subscribe_all(gz, mbus, ar);
		}
		if (gzgroup->type->message_subscribe != NULL) {
			gzgroup->type->message_subscribe(C, gzgroup, mbus);
		}
	}
}

/** \} */ /* wmGizmoMap */


/* -------------------------------------------------------------------- */
/** \name Tooltip Handling
 *
 * \{ */

struct ARegion *WM_gizmomap_tooltip_init(
        struct bContext *C, struct ARegion *ar, bool *r_exit_on_event)
{
	wmGizmoMap *gzmap = ar->gizmo_map;
	*r_exit_on_event = true;
	if (gzmap) {
		wmGizmo *gz = gzmap->gzmap_context.highlight;
		if (gz) {
			return UI_tooltip_create_from_gizmo(C, gz);
		}
	}
	return NULL;
}

/** \} */ /* wmGizmoMapType */

/* -------------------------------------------------------------------- */
/** \name wmGizmoMapType
 *
 * \{ */

wmGizmoMapType *WM_gizmomaptype_find(
        const struct wmGizmoMapType_Params *gzmap_params)
{
	for (wmGizmoMapType *gzmap_type = gizmomaptypes.first; gzmap_type; gzmap_type = gzmap_type->next) {
		if (gzmap_type->spaceid == gzmap_params->spaceid &&
		    gzmap_type->regionid == gzmap_params->regionid)
		{
			return gzmap_type;
		}
	}

	return NULL;
}

wmGizmoMapType *WM_gizmomaptype_ensure(
        const struct wmGizmoMapType_Params *gzmap_params)
{
	wmGizmoMapType *gzmap_type = WM_gizmomaptype_find(gzmap_params);

	if (gzmap_type) {
		return gzmap_type;
	}

	gzmap_type = MEM_callocN(sizeof(wmGizmoMapType), "gizmotype list");
	gzmap_type->spaceid = gzmap_params->spaceid;
	gzmap_type->regionid = gzmap_params->regionid;
	BLI_addhead(&gizmomaptypes, gzmap_type);

	return gzmap_type;
}

void wm_gizmomaptypes_free(void)
{
	for (wmGizmoMapType *gzmap_type = gizmomaptypes.first, *gzmap_type_next;
	     gzmap_type;
	     gzmap_type = gzmap_type_next)
	{
		gzmap_type_next = gzmap_type->next;
		for (wmGizmoGroupTypeRef *gzgt_ref = gzmap_type->grouptype_refs.first, *gzgt_next;
		     gzgt_ref;
		     gzgt_ref = gzgt_next)
		{
			gzgt_next = gzgt_ref->next;
			WM_gizmomaptype_group_free(gzgt_ref);
		}
		MEM_freeN(gzmap_type);
	}
}

/**
 * Initialize keymaps for all existing gizmo-groups
 */
void wm_gizmos_keymap(wmKeyConfig *keyconf)
{
	/* we add this item-less keymap once and use it to group gizmo-group keymaps into it */
	WM_keymap_ensure(keyconf, "Gizmos", 0, 0);

	for (wmGizmoMapType *gzmap_type = gizmomaptypes.first; gzmap_type; gzmap_type = gzmap_type->next) {
		for (wmGizmoGroupTypeRef *gzgt_ref = gzmap_type->grouptype_refs.first; gzgt_ref; gzgt_ref = gzgt_ref->next) {
			wm_gizmogrouptype_setup_keymap(gzgt_ref->type, keyconf);
		}
	}
}

/** \} */ /* wmGizmoMapType */

/* -------------------------------------------------------------------- */
/** \name Updates for Dynamic Type Registraion
 *
 * \{ */


void WM_gizmoconfig_update_tag_init(
        wmGizmoMapType *gzmap_type, wmGizmoGroupType *gzgt)
{
	/* tag for update on next use */
	gzmap_type->type_update_flag |= (WM_GIZMOMAPTYPE_UPDATE_INIT | WM_GIZMOMAPTYPE_KEYMAP_INIT);
	gzgt->type_update_flag |= (WM_GIZMOMAPTYPE_UPDATE_INIT | WM_GIZMOMAPTYPE_KEYMAP_INIT);

	wm_gzmap_type_update_flag |= WM_GIZMOMAPTYPE_GLOBAL_UPDATE_INIT;
}

void WM_gizmoconfig_update_tag_remove(
        wmGizmoMapType *gzmap_type, wmGizmoGroupType *gzgt)
{
	/* tag for update on next use */
	gzmap_type->type_update_flag |= WM_GIZMOMAPTYPE_UPDATE_REMOVE;
	gzgt->type_update_flag |= WM_GIZMOMAPTYPE_UPDATE_REMOVE;

	wm_gzmap_type_update_flag |= WM_GIZMOMAPTYPE_GLOBAL_UPDATE_REMOVE;
}

/**
 * Run incase new types have been added (runs often, early exit where possible).
 * Follows #WM_keyconfig_update concentions.
 */
void WM_gizmoconfig_update(struct Main *bmain)
{
	if (G.background)
		return;

	if (wm_gzmap_type_update_flag == 0)
		return;

	if (wm_gzmap_type_update_flag & WM_GIZMOMAPTYPE_GLOBAL_UPDATE_REMOVE) {
		for (wmGizmoMapType *gzmap_type = gizmomaptypes.first;
		     gzmap_type;
		     gzmap_type = gzmap_type->next)
		{
			if (gzmap_type->type_update_flag & WM_GIZMOMAPTYPE_GLOBAL_UPDATE_REMOVE) {
				gzmap_type->type_update_flag &= ~WM_GIZMOMAPTYPE_UPDATE_REMOVE;
				for (wmGizmoGroupTypeRef *gzgt_ref = gzmap_type->grouptype_refs.first, *gzgt_ref_next;
				     gzgt_ref;
				     gzgt_ref = gzgt_ref_next)
				{
					gzgt_ref_next = gzgt_ref->next;
					if (gzgt_ref->type->type_update_flag & WM_GIZMOMAPTYPE_UPDATE_REMOVE) {
						gzgt_ref->type->type_update_flag &= ~WM_GIZMOMAPTYPE_UPDATE_REMOVE;
						WM_gizmomaptype_group_unlink(NULL, bmain, gzmap_type, gzgt_ref->type);
					}
				}
			}
		}

		wm_gzmap_type_update_flag &= ~WM_GIZMOMAPTYPE_GLOBAL_UPDATE_REMOVE;
	}

	if (wm_gzmap_type_update_flag & WM_GIZMOMAPTYPE_GLOBAL_UPDATE_INIT) {
		for (wmGizmoMapType *gzmap_type = gizmomaptypes.first;
		     gzmap_type;
		     gzmap_type = gzmap_type->next)
		{
			const uchar type_update_all = WM_GIZMOMAPTYPE_UPDATE_INIT | WM_GIZMOMAPTYPE_KEYMAP_INIT;
			if (gzmap_type->type_update_flag & type_update_all) {
				gzmap_type->type_update_flag &= ~type_update_all;
				for (wmGizmoGroupTypeRef *gzgt_ref = gzmap_type->grouptype_refs.first;
				     gzgt_ref;
				     gzgt_ref = gzgt_ref->next)
				{
					if (gzgt_ref->type->type_update_flag & WM_GIZMOMAPTYPE_KEYMAP_INIT) {
						WM_gizmomaptype_group_init_runtime_keymap(bmain, gzgt_ref->type);
						gzgt_ref->type->type_update_flag &= ~WM_GIZMOMAPTYPE_KEYMAP_INIT;
					}

					if (gzgt_ref->type->type_update_flag & WM_GIZMOMAPTYPE_UPDATE_INIT) {
						WM_gizmomaptype_group_init_runtime(bmain, gzmap_type, gzgt_ref->type);
						gzgt_ref->type->type_update_flag &= ~WM_GIZMOMAPTYPE_UPDATE_INIT;
					}
				}
			}
		}

		wm_gzmap_type_update_flag &= ~WM_GIZMOMAPTYPE_GLOBAL_UPDATE_INIT;
	}
}

/** \} */
