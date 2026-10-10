/*
BLOOD.H

Dynamic blood (port/linux/game/blood.c): bodies run over bleed as hard as
they are hit, blood paints vehicles' drawn models and rides with them, and
blood on a wall or a vehicle's side runs down it.
*/

#ifndef __BLOOD_H
#define __BLOOD_H
#pragma once

/* the blood effects: built into every port build, on or off by Video
Settings' Blood Effects (config.toml's game.blood_effects); apart from the
ragdolls (ragdoll.h), which need Jolt Physics */
#ifndef BLOOD_EFFECTS
#define BLOOD_EFFECTS 1
#endif

#if BLOOD_EFFECTS

struct damage_data;

struct blood_parameters
{
	real enabled; /* (Video Settings' Blood Effects: on or off, config.toml's game.blood_effects) */
	real amount; /* times as much (and as large) */
	real full_speed; /* metres a second a vehicle bleeds a body fully at */
	real drips; /* 1: blood on walls and vehicles runs */
	real drip_length; /* times as long */
	real stay_seconds; /* blood on vehicles and characters lasts at least */
};

extern struct blood_parameters blood_parameters;

/* from projectile_effect_new: plasma struck a vehicle; the charged bolt
impact's sparks and smoke linger at the hit, riding with it, 1 to 5
seconds, and 1 shot in 10 the charged bolt's crackle too (visual only;
half size; blue for a plasma rifle's bolt) */
void blood_plasma_struck_vehicle(
	long object_index,
	short node_index,
	boolean blue,
	short marker_count,
	real_point3d const *marker_points,
	real_vector3d const *marker_forwards);

/* an effect's rare event: the one the game skips most often (its lingering
sparks and smoke), NONE: none */
short blood_effect_rare_event(
	long definition_index);

/* from effect_new..., the event a new effect starts at (0, or the one event
an ember plays alone) */
short blood_effect_first_event(
	long effect_index,
	void const *effect);

/* from effect_delete */
void blood_effect_deleted(
	long effect_index);

/* from effect_generate_particles, each particle an effect makes: an ember's
smaller (about its origin, slower: less what it is carried at) or recoloured */
void blood_effect_particle(
	void const *effect,
	real_point3d const *origin,
	real_vector3d const *carried,
	real_point3d *position,
	real_vector3d *velocity,
	real *radius,
	real_rgb_color *color);

/* from effects_update: the chance an effect's next event is skipped (an
ember's effect: every one after its own) */
real blood_effect_event_skip(
	long effect_index,
	short event_index,
	short event_count,
	real skip_fraction);

/* from decals_initialize_for_new_map */
void blood_initialize_for_new_map(
	void);

/* from decals_update, each tick: drips running */
void blood_update(
	void);

/* from render_window, each frame before the decals are drawn: blood on
vehicles moved with them, as they are drawn */
void blood_render_update(
	void);

/* from decal_new_from_collision: a decal landed on the level (blood on a
wall starts running) */
void blood_decal_landed(
	long decal_definition_index,
	real_point3d const *point,
	real_vector3d const *normal,
	real radius,
	short cluster_index);

/* from ragdoll.c, after each physics step: a vehicle meeting a dead body, at
a point on the vehicle (its normal out of the vehicle), at a speed (world
units a tick) */
void blood_vehicle_hit_body(
	long biped_index,
	long vehicle_index,
	real_point3d const *point,
	real_vector3d const *normal,
	real speed);

/* from decal_new: blood the game throws (a spray's splats) meeting a
vehicle's drawn surface before the level's paints the vehicle; TRUE when it
did (decal_new then lays nothing more) */
boolean blood_decal_onto_object(
	long decal_definition_index,
	real_point3d const *origin,
	real_vector3d const *vector,
	real radius_modifier);

/* from particle_update_physics: a particle whose landing makes blood (its
collision effect), moving from one point to another this tick, meeting a
vehicle's drawn surface on the way: where, and its normal (TRUE); the
particle lands there instead (its blood paints the vehicle) */
/* from particles.c and particle_systems.c: a blood particle (its tag's name
says blood) meeting an object's drawn surface (never a character's) between
two points: it paints it there with the blood of the body it came from, and
ends (TRUE) */
boolean blood_splash_meets_object(
	long tag_index,
	real_point3d const *from,
	real_point3d const *to,
	real particle_radius);

/* an impact's decals (each with its radius modifier) onto an object's drawn
surface at a point, the surface facing along normal: object_index's, or
(NONE) the nearest painted object's; how many it painted */
short blood_decals_onto_object(
	long object_index,
	real_point3d const *point,
	real_vector3d const *normal,
	long const *decals,
	real const *radius_modifiers,
	short count);

/* from projectiles.c: a shot on a Spartan, logged (the first few a map) */
void blood_note_armor_hit(
	long object_index,
	short material,
	real shield,
	short painted);

/* from projectiles.c: while set, an impact's marks go on characters too (a
Spartan's bare armor) */
void blood_allow_character_marks(
	boolean allow);

/* from decals.c: blood lands on the level's two-sided surfaces too (a
tree's trunk and leaves, a fence), as on any other */
boolean blood_decal_on_two_sided(
	long decal_definition_index);

/* from projectiles.c: what a projectile struck, on the console (blood debug 1) */
struct collision_result;
void blood_debug_impact(
	struct collision_result const *collision);

/* from projectiles.c: a projectile struck an object (not a character) here */
void blood_projectile_struck_object(
	real_point3d const *point);

/* a local player's own unit, seen from inside it (first person): no marks
on it drawn, they would cover the screen (blood_objects.c) */
boolean blood_object_seen_first_person(
	long object_index);

/* (blood_objects.c) an effect's decals onto an object as above (without_blood:
none whose name says blood, a bullet hole's bloody variant); a material
effects' impact (a hard surface's) likewise */
short blood_effect_onto_object(
	long effect_index,
	long object_index,
	real_point3d const *point,
	real_vector3d const *normal,
	boolean without_blood);
short blood_material_effect_onto_object(
	long material_effects_index,
	long object_index,
	real_point3d const *point,
	real_vector3d const *normal);

/* (Death and Blood's Blood Drips: animated) */
boolean blood_drops_animated(
	void);

/* blood_drops.c: the drops to draw this frame (a run's head, a drop falling):
where, which way (a surface's normal, or the way it falls), how big, and the
blood's splat (its species, its colour) */
short blood_drops_for_drawing(
	real_point3d *positions,
	real_vector3d *directions,
	real *radii,
	long *decals,
	short maximum);

/* render_particles.c: the drops drawn, after the game's particles
(blood_drops.c) */
void blood_drops_render(
	void);

/* the console's "blood" (hs.c): NONE when the expression is not one */
long blood_console_command(
	char const *expression);

/* from particles.c: a blood particle (its collision effect's, material
effects' or own name says blood) meeting an object's drawn surface between two
points (never a character's): where, and which way it faces */
boolean blood_particle_meets_object(
	long collision_effect_index,
	long particle_definition_index,
	real_point3d const *from,
	real_point3d const *to,
	real_point3d *point,
	real_vector3d *normal);

/* from object_damage_aftermath: a biped hit (blood on a vehicle behind it) */
void blood_biped_damaged(
	long biped_index,
	struct damage_data const *damage,
	real damage_amount);

#endif // BLOOD_EFFECTS

#endif // __BLOOD_H
