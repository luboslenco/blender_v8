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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file MOD_gpencil_modifiertypes.h
 *  \ingroup modifiers
 */

#ifndef __MOD_GPENCIL_MODIFIERTYPES_H__
#define __MOD_GPENCIL_MODIFIERTYPES_H__

#include "BKE_gpencil_modifier.h"

/* ****************** Type structures for all modifiers ****************** */

extern GpencilModifierTypeInfo modifierType_Gpencil_None;
extern GpencilModifierTypeInfo modifierType_Gpencil_Noise;
extern GpencilModifierTypeInfo modifierType_Gpencil_Subdiv;
extern GpencilModifierTypeInfo modifierType_Gpencil_Simplify;
extern GpencilModifierTypeInfo modifierType_Gpencil_Thick;
extern GpencilModifierTypeInfo modifierType_Gpencil_Tint;
extern GpencilModifierTypeInfo modifierType_Gpencil_Color;
extern GpencilModifierTypeInfo modifierType_Gpencil_Instance;
extern GpencilModifierTypeInfo modifierType_Gpencil_Build;
extern GpencilModifierTypeInfo modifierType_Gpencil_Opacity;
extern GpencilModifierTypeInfo modifierType_Gpencil_Lattice;
extern GpencilModifierTypeInfo modifierType_Gpencil_Mirror;
extern GpencilModifierTypeInfo modifierType_Gpencil_Smooth;
extern GpencilModifierTypeInfo modifierType_Gpencil_Hook;
extern GpencilModifierTypeInfo modifierType_Gpencil_Offset;
extern GpencilModifierTypeInfo modifierType_Gpencil_Armature;

/* MOD_gpencil_util.c */
void gpencil_modifier_type_init(GpencilModifierTypeInfo *types[]);

#endif  /* __MOD_GPENCIL_MODIFIERTYPES_H__ */
