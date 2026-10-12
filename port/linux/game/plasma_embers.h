/*
PLASMA_EMBERS.H

Plasma's sparks and smoke lingering where a bolt struck a vehicle or a
Spartan's bare armor (port/linux/game/plasma_embers.c), with Projectile
Effects on.
*/

#ifndef __PLASMA_EMBERS_H
#define __PLASMA_EMBERS_H
#pragma once

#if BLOOD_EFFECTS

/* from projectile_effect_new: plasma struck an object (a vehicle, a
Spartan's bare armor) at the impact's markers; blue: not the plasma pistol's */
void plasma_embers_struck(
	long object_index,
	short node_index,
	boolean blue,
	short marker_count,
	real_point3d const *marker_points,
	real_vector3d const *marker_forwards);

/* from effect_allocate: the event a new effect starts at (0, or the one
event an ember plays alone) */
short plasma_embers_effect_first_event(
	long effect_index,
	void const *effect);

/* from effect_delete */
void plasma_embers_effect_deleted(
	long effect_index);

/* from effect_generate_particles, each particle an effect makes: an ember's
smaller (about its origin, slower: less what it is carried at) or blue */
void plasma_embers_effect_particle(
	void const *effect,
	real_point3d const *origin,
	real_vector3d const *carried,
	real_point3d *position,
	real_vector3d *velocity,
	real *radius,
	real_rgb_color *color);

/* from effect_update: the chance an effect's next event is skipped (an
ember's effect: every one after its own) */
real plasma_embers_effect_event_skip(
	long effect_index,
	short event_index,
	short event_count,
	real skip_fraction);

/* from blood_initialize_for_new_map and blood_update (each tick) */
void plasma_embers_initialize_for_new_map(
	void);
void plasma_embers_update(
	long now);

#endif // BLOOD_EFFECTS

#endif // __PLASMA_EMBERS_H
