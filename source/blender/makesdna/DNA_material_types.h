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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file DNA_material_types.h
 *  \ingroup DNA
 */

#ifndef __DNA_MATERIAL_TYPES_H__
#define __DNA_MATERIAL_TYPES_H__

#include "DNA_defs.h"
#include "DNA_ID.h"
#include "DNA_listBase.h"

#ifndef MAX_MTEX
#define MAX_MTEX	18
#endif

struct Image;
struct bNodeTree;
struct AnimData;
struct Ipo;

/* WATCH IT: change type? also make changes in ipo.h  */

typedef struct TexPaintSlot {
	struct Image *ima; /* image to be painted on */
	char *uvname;      /* customdata index for uv layer, MAX_NAME*/
	int valid;         /* do we have a valid image and UV map */
	int pad;
} TexPaintSlot;

typedef struct MaterialGPencilStyle {
	struct Image *sima;      /* Texture image for strokes */
	struct Image *ima;       /* Texture image for filling */
	float stroke_rgba[4];    /* color for paint and strokes (alpha included) */
	float fill_rgba[4];      /* color that should be used for drawing "fills" for strokes (alpha included) */
	float mix_rgba[4];       /* secondary color used for gradients and other stuff */
	short flag;              /* settings */
	short index;             /* custom index for passes */
	short stroke_style;      /* style for drawing strokes (used to select shader type) */
	short fill_style;        /* style for filling areas (used to select shader type) */
	float mix_factor;        /* factor used to define shader behavior (several uses) */
	float gradient_angle;    /* angle used for gradients orientation */
	float gradient_radius;   /* radius for radial gradients */
	float pattern_gridsize;  /* cheesboard size */
	float gradient_scale[2]; /* uv coordinates scale */
	float gradient_shift[2]; /* factor to shift filling in 2d space */
	float texture_angle;     /* angle used for texture orientation */
	float texture_scale[2];  /* texture scale (separated of uv scale) */
	float texture_offset[2]; /* factor to shift texture in 2d space */
	float texture_opacity;   /* texture opacity */
	float texture_pixsize;   /* pixel size for uv along the stroke */
	int mode;                /* drawing mode (line or dots) */

	int gradient_type;       /* type of gradient */
	char pad[4];
} MaterialGPencilStyle;

/* MaterialGPencilStyle->flag */
typedef enum eMaterialGPencilStyle_Flag {
	/* Fill Texture is a pattern */
	GP_STYLE_FILL_PATTERN = (1 << 0),
	/* don't display color */
	GP_STYLE_COLOR_HIDE = (1 << 1),
	/* protected from further editing */
	GP_STYLE_COLOR_LOCKED = (1 << 2),
	/* do onion skinning */
	GP_STYLE_COLOR_ONIONSKIN = (1 << 3),
	/* clamp texture */
	GP_STYLE_COLOR_TEX_CLAMP = (1 << 4),
	/* mix texture */
	GP_STYLE_COLOR_TEX_MIX = (1 << 5),
	/* Flip fill colors */
	GP_STYLE_COLOR_FLIP_FILL = (1 << 6),
	/* Stroke Texture is a pattern */
	GP_STYLE_STROKE_PATTERN = (1 << 7)
} eMaterialGPencilStyle_Flag;

typedef enum eMaterialGPencilStyle_Mode {
	GP_STYLE_MODE_LINE = 0, /* line */
	GP_STYLE_MODE_DOTS = 1, /* dots */
	GP_STYLE_MODE_BOX = 2, /* rectangles */
} eMaterialGPencilStyle_Mode;

typedef struct Material {
	ID id;
	struct AnimData *adt;	/* animation data (must be immediately after id for utilities to use it) */

	short flag, pad1[7];

	/* Colors from Blender Internal that we are still using. */
	float r, g, b;
	float specr, specg, specb;
	float alpha DNA_DEPRECATED;
	float ray_mirror  DNA_DEPRECATED;
	float spec;
	float gloss_mir  DNA_DEPRECATED; /* renamed and inversed to roughness */
	float roughness;
	float metallic;
	float pad4[2];

	/* Ror buttons and render. */
	char pr_type, use_nodes;
	short pr_lamp, pr_texture;

	/* Index for render passes. */
	short index;

	struct bNodeTree *nodetree;
	struct Ipo *ipo  DNA_DEPRECATED;  /* old animation system, deprecated for 2.5 */
	struct PreviewImage *preview;

	/* Freestyle line settings. */
	float line_col[4];
	short line_priority;
	short vcol_alpha;

	/* Texture painting slots. */
	short paint_active_slot;
	short paint_clone_slot;
	short tot_slots;
	short pad2[3];

	/* Transparency. */
	float alpha_threshold;
	float refract_depth;
	char blend_method;
	char blend_shadow;
	char blend_flag;
	char pad3[5];

	/* Cached slots for texture painting, must be refreshed in
	 * refresh_texpaint_image_cache before using. */
	struct TexPaintSlot *texpaintslot;

	/* Runtime cache for GLSL materials. */
	ListBase gpumaterial;

	/* grease pencil color */
	struct MaterialGPencilStyle *gp_style;
} Material;

/* **************** MATERIAL ********************* */

/* maximum number of materials per material array.
 * (on object, mesh, lamp, etc.). limited by
 * short mat_nr in verts, faces.
 * -1 because for active material we store the index + 1 */
#define MAXMAT			(32767-1)

/* flag */
		/* for render */
#define MA_IS_USED		1
		/* for dopesheet */
#define MA_DS_EXPAND	2
		/* for dopesheet (texture stack expander)
		 * NOTE: this must have the same value as other texture stacks,
		 * otherwise anim-editors will not read correctly
		 */
#define MA_DS_SHOW_TEXS	4

/* ramps */
#define MA_RAMP_BLEND		0
#define MA_RAMP_ADD			1
#define MA_RAMP_MULT		2
#define MA_RAMP_SUB			3
#define MA_RAMP_SCREEN		4
#define MA_RAMP_DIV			5
#define MA_RAMP_DIFF		6
#define MA_RAMP_DARK		7
#define MA_RAMP_LIGHT		8
#define MA_RAMP_OVERLAY		9
#define MA_RAMP_DODGE		10
#define MA_RAMP_BURN		11
#define MA_RAMP_HUE			12
#define MA_RAMP_SAT			13
#define MA_RAMP_VAL			14
#define MA_RAMP_COLOR		15
#define MA_RAMP_SOFT        16
#define MA_RAMP_LINEAR      17

/* texco */
#define TEXCO_ORCO		1
#define TEXCO_REFL		2
#define TEXCO_NORM		4
#define TEXCO_GLOB		8
#define TEXCO_UV		16
#define TEXCO_OBJECT	32
#define TEXCO_LAVECTOR	64
#define TEXCO_VIEW		128
#define TEXCO_STICKY_	256  // DEPRECATED
#define TEXCO_OSA		512
#define TEXCO_WINDOW	1024
#define NEED_UV			2048
#define TEXCO_TANGENT	4096
	/* still stored in vertex->accum, 1 D */
#define TEXCO_STRAND	8192
#define TEXCO_PARTICLE	8192 /* strand is used for normal materials, particle for halo materials */
#define TEXCO_STRESS	16384
#define TEXCO_SPEED		32768

/* mapto */
#define MAP_COL			1
#define MAP_ALPHA		128

/* pmapto */
/* init */
#define MAP_PA_INIT		31
#define MAP_PA_TIME		1
#define MAP_PA_LIFE		2
#define MAP_PA_DENS		4
#define MAP_PA_SIZE		8
#define MAP_PA_LENGTH	16
/* reset */
#define MAP_PA_IVEL		32
/* physics */
#define MAP_PA_PVEL		64
/* path cache */
#define MAP_PA_CACHE	912
#define MAP_PA_CLUMP	128
#define MAP_PA_KINK		256
#define MAP_PA_ROUGH	512
#define MAP_PA_FREQ		1024

/* pr_type */
#define MA_FLAT			0
#define MA_SPHERE		1
#define MA_CUBE			2
#define MA_MONKEY		3
#define MA_SPHERE_A		4
#define MA_TEXTURE		5
#define MA_LAMP			6
#define MA_SKY			7
#define MA_HAIR			10
#define MA_ATMOS		11

/* blend_method */
enum {
	MA_BM_SOLID,
	MA_BM_ADD,
	MA_BM_MULTIPLY,
	MA_BM_CLIP,
	MA_BM_HASHED,
	MA_BM_BLEND,
};

/* blend_flag */
enum {
	MA_BL_HIDE_BACKSIDE =        (1 << 0),
	MA_BL_SS_REFRACTION =        (1 << 1),
	MA_BL_SS_SUBSURFACE =        (1 << 2), /* DEPRECATED */
	MA_BL_TRANSLUCENCY =         (1 << 3),
};

/* blend_shadow */
enum {
	MA_BS_NONE = 0,
	MA_BS_SOLID,
	MA_BS_CLIP,
	MA_BS_HASHED,
};

/* Grease Pencil Stroke styles */
enum {
	GP_STYLE_STROKE_STYLE_SOLID = 0,
	GP_STYLE_STROKE_STYLE_TEXTURE
};

/* Grease Pencil Fill styles */
enum {
	GP_STYLE_FILL_STYLE_SOLID = 0,
	GP_STYLE_FILL_STYLE_GRADIENT,
	GP_STYLE_FILL_STYLE_CHESSBOARD,
	GP_STYLE_FILL_STYLE_TEXTURE
};

/* Grease Pencil Gradient Types */
enum {
	GP_STYLE_GRADIENT_LINEAR = 0,
	GP_STYLE_GRADIENT_RADIAL
};

#endif
