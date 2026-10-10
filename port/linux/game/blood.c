/*
BLOOD.C

Dynamic blood, with the game's own blood (each character's: its collision
model's area damage effect, "effects\blood aoe elite", and the splats of its
kind, "effects\decals\blood splats\blood elite splat"):

- A body a vehicle hits or drives over bleeds as hard as it is hit: from a
  trickle at walking pace to the full spray, splats under it smeared along
  the vehicle's way and blood on the vehicle at speed (blood_full_speed).
- Blood sticks to vehicles, painted on them as on walls and floors: on the
  triangles of the vehicle's drawn model (object_mesh.c), clipped to them as
  the game clips its decals to the level, each corner riding the node that
  moves that part of the model (its body, a wheel, a turret), moved with it
  every frame, as it is drawn (render_interpolation). Blood the game throws
  at a vehicle (a body shot beside it, a spray) paints it too.
- Blood on a wall (any blood decal landing on a steep surface) or on a
  vehicle's side runs: drips, each a trail of thin streaks laid down the
  surface as it runs, slowing, thinning and stopping at the surface's edge.
  No animated pictures: the trails are the game's own blood splats, drawn
  as they run.

Everything here is this machine's own (local random numbers, nothing over
the network). Units: world units (1 = 3.048 m) and ticks (1/30 s).
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "objects/damage.h"
#include "effects/effects.h"
#include "effects/decals.h"
#include "effects/decal_definitions.h"
#include "effects/effect_definitions.h"
#include "physics/collision_model_definitions.h"
#include "physics/collisions.h"
#include "physics/collision_usage.h"
#include "tag_files/tag_files.h"
#include "game/game.h"
#include "main/console.h"
#include "units/bipeds.h"

#include "scenario/scenario.h"
#include "physics/collision_bsp_definitions.h"
#include "blood.h"
#include "object_mesh.h"
/* (the ragdolls, port/linux/game/ragdoll.c, when the build has them: only
their biped flag, BIPED_PORT_RAGDOLL_BIT) */
#if __has_include("ragdoll.h")
#include "ragdoll.h"
#endif

#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if BLOOD_EFFECTS

/* render_interpolation.c: the node matrices as this frame draws them (NULL:
the object's own) */
struct real_matrix4x3 *render_interpolation_object_node_matrices(long object_index);
/* cache_files.c */
long tag_loaded(long group_tag, char const *name);

/* ---------- constants */

/* what blood stains besides the level: every solid object but characters
(rocks, trees, crates, machines, vehicles, dropped weapons) */
/* render_interpolation.c: how far between the last tick and the next this frame is */
real render_interpolation_fraction(void);

static real blood_on_vehicles = 1.6f;
/* (the middle share of a splat's picture painted: 1 all of it; a drip's run
less, its dense middle) */
static real blood_paint_core_along = 1.f;
static real blood_paint_core_across = 1.f;
/* (a run: the splat's picture nearly whole, stretched along it, so its
ragged round outline is the run's edge and ends, short stretches overlapping
in a chain; a bead: its picture whole, round. The picture's edge is see-
through, so what shows is narrower than the quad: BLOOD_RUN_WIDEN) */
#define BLOOD_RUN_CORE_ALONG 0.85f
#define BLOOD_RUN_CORE_ACROSS 1.f
#define BLOOD_BEAD_CORE 0.9f
#define BLOOD_RUN_WIDEN 1.6f
static real blood_debug = 0.f;
static real blood_drip_layers = 2.f;
/* (projectiles.c: while set, marks go on characters too: a Spartan's bare
armor) */
static boolean blood_marks_on_characters = FALSE;
/* (how square to the surface hit a triangle must face to take a mark's part:
blood wraps round edges, a hole or burn doesn't stretch over them) */
static real blood_paint_min_facing = 0.35f;
/* (a mark on a character is smaller than on a wall: its decal is drawn for
the level's scale) */
#define BLOOD_CHARACTER_MARK_SCALE 0.35f

/* (blood debug: a line on the console and in debug.txt) */
static void blood_say(char const *format, ...)
{
	char line[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	console_printf(FALSE, "%s", line);
	error(_error_silent, "blood: %s", line);
}

/* how far either side of a decal's point its object surface is looked for */
#define BLOOD_DECAL_SLACK 0.3f
#define BLOOD_PAINTED_OBJECTS (_object_mask_vehicle | _object_mask_scenery | _object_mask_device | _object_mask_item)

enum
{
	BLOOD_MAXIMUM_KINDS = 32,
	BLOOD_DEFAULT_VEHICLE_DECALS = 768,
	BLOOD_MAXIMUM_VEHICLE_DECALS = BLOOD_DEFAULT_VEHICLE_DECALS,
	/* (the quads of every vehicle's blood at most: the game's decals share
	one cache of 2560) */
	BLOOD_DEFAULT_VEHICLE_QUADS = 1200,
	BLOOD_MAXIMUM_PAINT_QUADS = 16,
	BLOOD_MAXIMUM_PAINT_CANDIDATES = 256,
	BLOOD_MAXIMUM_DRIPS = 160,
	BLOOD_MAXIMUM_RECENT_HITS = 32,
};

/* (the effect's markers, as damage.c gives them) */
enum
{
	_blood_marker_normal = 0,
	_blood_marker_incident,
	_blood_marker_negative_incident,
	_blood_marker_reflected,
	_blood_marker_gravity,
	NUMBER_OF_BLOOD_MARKERS,
};

#define BLOOD_METRES 3.048f
/* a surface this steep (its normal's height under this) runs */
#define BLOOD_STEEP 0.75f
/* (blood on a vehicle sits this far off its surface) */
#define BLOOD_LIFT 0.004f

/* ---------- structures */

/* a biped's blood: its spray and its splats */
struct blood_kind
{
	long biped_definition_index; /* NONE: free */
	long spray; /* effect */
	long splat; /* decal */
	long splat_large;
	long splat_small;
};

/* blood painted on an object: quads over its drawn triangles, each corner
in the frame of the node that moves it */
struct blood_vehicle_decal
{
	long decal_index; /* NONE: free */
	long decal_definition_index;
	long object_index;
	long creation_time;
	short quad_count;
	short pad;
	real_point3d corners[BLOOD_MAXIMUM_PAINT_QUADS * 4]; /* in their nodes' frames */
	byte nodes[BLOOD_MAXIMUM_PAINT_QUADS * 4];
};

/* blood running down a surface */
struct blood_drip
{
	boolean active;
	byte pad[3];
	long decal_definition_index;
	long object_index; /* NONE: the level's */
	short node_index;
	short cluster_index;
	long triangle; /* on an object: the one it is on */
	real_point3d position; /* (on a vehicle, in its node's frame) */
	real_vector3d normal;
	real_point3d laid; /* where the last streak ended */
	real speed; /* world units a tick */
	real slowing; /* its speed kept each tick */
	real remaining; /* world units */
	short age; /* ticks */
	short pause; /* ticks it holds still, gathering (a bead) */
	real length;
	real width;
	real wander; /* radians off straight down it runs, wandering */
	real start_width;
	/* (on the level: its live streak and bead, moved as it runs) */
	long live_decal;
	long live_bead;
	real_point3d head_before; /* its head a tick ago and now (world) */
	real_point3d head;
	real_vector3d live_normal;
	real live_half_width;
};

struct blood_recent_hit
{
	long biped_index;
	long vehicle_index;
	long time;
	real share; /* how hard it bled then */
};

/* ---------- globals */

struct blood_parameters blood_parameters = { 1.f, 1.f, 15.f, 1.f, 1.f, 300.f };

static struct blood_kind blood_kinds[BLOOD_MAXIMUM_KINDS];
static struct blood_vehicle_decal blood_vehicle_decals[BLOOD_MAXIMUM_VEHICLE_DECALS];
static struct blood_drip blood_drips[BLOOD_MAXIMUM_DRIPS];
static struct blood_recent_hit blood_recent_hits[BLOOD_MAXIMUM_RECENT_HITS];
static long blood_last_time = NONE;
/* (blood's own decals landing: no drips of drips) */
static boolean blood_laying = FALSE;
/* a decal definition known to be blood, or not: the last asked */
static long blood_checked_definition = NONE;
static boolean blood_checked_is_blood = FALSE;

/* ---------- vectors */

static real blood_dot(real_vector3d const *a, real_vector3d const *b)
{
	return a->i * b->i + a->j * b->j + a->k * b->k;
}

static real blood_normalize(real_vector3d *v)
{
	real length = (real)sqrt(blood_dot(v, v));

	if (length > 1e-6f && valid_real(length))
	{
		v->i /= length;
		v->j /= length;
		v->k /= length;
		return length;
	}
	return 0.f;
}

static void blood_add(real_point3d *p, real_vector3d const *v, real scale)
{
	p->x += v->i * scale;
	p->y += v->j * scale;
	p->z += v->k * scale;
}

/* down a surface: gravity along it (none on a floor) */
static boolean blood_downhill(real_vector3d const *normal, real_vector3d *down)
{
	down->i = normal->i * normal->k;
	down->j = normal->j * normal->k;
	down->k = -1.f + normal->k * normal->k;
	return blood_normalize(down) > 0.f;
}

/* a point and a normal from a node's frame to the world, and back */
static void blood_to_world(real_matrix4x3 const *m, real_point3d const *local, real_point3d *world)
{
	real_point3d p;

	p.x = m->position.x + (m->forward.i * local->x + m->left.i * local->y + m->up.i * local->z) * m->scale;
	p.y = m->position.y + (m->forward.j * local->x + m->left.j * local->y + m->up.j * local->z) * m->scale;
	p.z = m->position.z + (m->forward.k * local->x + m->left.k * local->y + m->up.k * local->z) * m->scale;
	*world = p;
}

static void blood_to_local(real_matrix4x3 const *m, real_point3d const *world, real_point3d *local)
{
	real_vector3d d;
	real inverse = m->scale != 0.f ? 1.f / m->scale : 1.f;

	d.i = world->x - m->position.x;
	d.j = world->y - m->position.y;
	d.k = world->z - m->position.z;
	local->x = blood_dot(&d, &m->forward) * inverse;
	local->y = blood_dot(&d, &m->left) * inverse;
	local->z = blood_dot(&d, &m->up) * inverse;
}

static void blood_normal_to_world(real_matrix4x3 const *m, real_vector3d const *local, real_vector3d *world)
{
	real_vector3d v;

	v.i = m->forward.i * local->i + m->left.i * local->j + m->up.i * local->k;
	v.j = m->forward.j * local->i + m->left.j * local->j + m->up.j * local->k;
	v.k = m->forward.k * local->i + m->left.k * local->j + m->up.k * local->k;
	*world = v;
}

static void blood_normal_to_local(real_matrix4x3 const *m, real_vector3d const *world, real_vector3d *local)
{
	real_vector3d v;

	v.i = blood_dot(world, &m->forward);
	v.j = blood_dot(world, &m->left);
	v.k = blood_dot(world, &m->up);
	*local = v;
}

/* an object's node matrix: as drawn this frame (rendering), else as it is */
static real_matrix4x3 const *blood_node_matrix(long object_index, short node_index, boolean drawn)
{
	struct object_datum *object = object_try_and_get(object_index);
	real_matrix4x3 const *matrices = NULL;
	long count;

	if (!object)
		return NULL;
	count = object->object.node_matrices.size / (long)sizeof(real_matrix4x3);
	if (node_index < 0 || node_index >= count)
		node_index = 0;
	if (count <= 0)
		return NULL;
	if (drawn)
		matrices = (real_matrix4x3 const *)render_interpolation_object_node_matrices(object_index);
	if (!matrices)
		matrices = (real_matrix4x3 const *)object_header_block_get(object_index, &object->object.node_matrices);
	return matrices ? &matrices[node_index] : NULL;
}

/* ---------- what a body bleeds */

/* a tag's name has a word */
static boolean blood_name_has(long tag_index, char const *word)
{
	char const *name = tag_index != NONE && tag_index != 0 ? tag_get_name(tag_index) : NULL;

	return name && strstr(name, word) != NULL;
}

/* the decal "effects\decals\blood splats\<name>", or NONE */
static long blood_splat_named(char const *name)
{
	char path[256];

	snprintf(path, sizeof(path), "effects\\decals\\blood splats\\%s", name);
	return tag_loaded(DECAL_GROUP_TAG, path);
}

static struct blood_kind const *blood_kind_get(long biped_index)
{
	struct object_datum *object = object_try_and_get(biped_index);
	struct object_definition *definition;
	struct blood_kind *kind = NULL;
	short index;

	if (!object)
		return NULL;
	for (index = 0; index < BLOOD_MAXIMUM_KINDS; index++)
	{
		if (blood_kinds[index].biped_definition_index == object->definition_index)
			return &blood_kinds[index];
		if (!kind && blood_kinds[index].biped_definition_index == NONE)
			kind = &blood_kinds[index];
	}
	if (!kind)
		kind = &blood_kinds[object->definition_index % BLOOD_MAXIMUM_KINDS];

	kind->biped_definition_index = object->definition_index;
	kind->spray = NONE;
	kind->splat = kind->splat_large = kind->splat_small = NONE;
	definition = object_definition_get(object->definition_index);

	/* its spray: the area damage effect of its collision model ("effects\blood
	aoe elite"), and its splats by the word after "aoe" */
	if (definition && definition->object.collision_model.index != NONE)
	{
		struct collision_model *model = collision_model_definition_get(definition->object.collision_model.index);
		char const *name;

		if (model)
			kind->spray = model->resistance.area_damage_effect.index;
		name = kind->spray != NONE ? tag_get_name(kind->spray) : NULL;
		if (name && strstr(name, "blood"))
		{
			char const *word = strstr(name, "aoe ");
			char species[64] = "";

			if (word)
				snprintf(species, sizeof(species), "%s", word + 4);
			if (!species[0] || !strcmp(species, "human"))
			{
				kind->splat = blood_splat_named("blood splat");
				kind->splat_large = blood_splat_named("blood splat large");
				kind->splat_small = blood_splat_named("blood splat small");
			}
			else
			{
				char splat[96];

				snprintf(splat, sizeof(splat), "blood %s splat", species);
				kind->splat = blood_splat_named(splat);
				snprintf(splat, sizeof(splat), "blood splat %s small", species);
				kind->splat_small = blood_splat_named(splat);
			}
		}
		else
		{
			/* (no blood: a sentinel, a robot) */
			kind->spray = NONE;
		}
	}
	if (kind->splat_large == NONE)
		kind->splat_large = kind->splat;
	if (kind->splat_small == NONE)
		kind->splat_small = kind->splat;
	return kind;
}

/* (not effects_corpse_nonviolent: that is the game's rule that a corpse,
once settled, shows no gore when shot, and it is on by default; this is
blood_parameters.enabled's to decide) */
/* port_config.c */
char const *config_string(char const *name);
int config_write(char const *name, char const *value);
unsigned long config_changes(void);

static unsigned long blood_config_changes = (unsigned long)-1;
static boolean blood_config_enabled = TRUE;

/* game.blood_effects (Video Settings' Blood Effects): "dynamic" (or unset)
or "classic", the game's own blood only; read again whenever a setting
changes */
static boolean blood_effects_enabled(void)
{
	unsigned long changes = config_changes();

	if (changes != blood_config_changes)
	{
		char const *value = config_string("game.blood_effects");

		blood_config_changes = changes;
		blood_config_enabled = !value || strcmp(value, "classic") != 0;
	}
	return blood_config_enabled;
}

/* Video Setup's Death and Blood: Blood Drips (config.toml's game.blood_drips):
"animated" draws the drop at a run's head and lets drops fall from edges and
undersides; "classic" the runs alone */
static unsigned long blood_drips_config_changes = (unsigned long)-1;
static boolean blood_drips_config_animated = TRUE;

boolean blood_drops_animated(
	void)
{
	unsigned long changes = config_changes();

	if (changes != blood_drips_config_changes)
	{
		char const *value = config_string("game.blood_drips");

		blood_drips_config_changes = changes;
		blood_drips_config_animated = !value || strcmp(value, "classic") != 0;
	}
	return blood_drips_config_animated && blood_parameters.drips >= 0.5f;
}

static void blood_drop_fall(long decal_definition_index, real_point3d const *point, real radius);

static boolean blood_violence_allowed(void)
{
	return blood_parameters.enabled >= 0.5f && decals_enabled && blood_effects_enabled();
}

/* the body's spray (its blood effect) at a point, outward along a normal */
static void blood_spray(long effect_index, real_point3d const *point, real_vector3d const *normal, real_vector3d const *incident)
{
	char const *marker_names[NUMBER_OF_BLOOD_MARKERS];
	real_point3d marker_points[NUMBER_OF_BLOOD_MARKERS];
	real_vector3d marker_forwards[NUMBER_OF_BLOOD_MARKERS];
	real_vector3d direction = *incident;
	real along;
	short index;

	if (effect_index == NONE)
		return;
	if (blood_normalize(&direction) == 0.f)
		direction = *global_down3d;
	marker_names[_blood_marker_normal] = "normal";
	marker_names[_blood_marker_incident] = "incident";
	marker_names[_blood_marker_negative_incident] = "negative incident";
	marker_names[_blood_marker_reflected] = "reflection";
	marker_names[_blood_marker_gravity] = "gravity";
	marker_forwards[_blood_marker_normal] = *normal;
	scale_vector3d(&direction, -1.f, &marker_forwards[_blood_marker_incident]);
	marker_forwards[_blood_marker_negative_incident] = direction;
	along = blood_dot(&direction, normal);
	marker_forwards[_blood_marker_reflected].i = direction.i - 2.f * along * normal->i;
	marker_forwards[_blood_marker_reflected].j = direction.j - 2.f * along * normal->j;
	marker_forwards[_blood_marker_reflected].k = direction.k - 2.f * along * normal->k;
	marker_forwards[_blood_marker_gravity] = *global_down3d;
	for (index = 0; index < NUMBER_OF_BLOOD_MARKERS; index++)
		marker_points[index] = *point;
	effect_new_unattached_from_markers(effect_index, NONE, global_zero_vector3d, NUMBER_OF_BLOOD_MARKERS,
		marker_names, marker_points, marker_forwards, 1.f, 0.f, NULL, NULL, FALSE);
}

/* ---------- painting objects: their drawn models */

/* the object's mesh posed as it is this tick: each vertex where the game
draws it (its nodes' matrices times their default inverses, weighted) */
enum
{
	BLOOD_SKIN_SLOTS = 8,
};

/* (the posed meshes of the last few objects this tick) */
struct blood_skin_slot
{
	long object_index; /* NONE: free */
	long time;
	long key;
	struct object_mesh const *mesh;
	unsigned long last_used;
	real_point3d minimum; /* its bounds, posed */
	real_point3d maximum;
	real_matrix4x3 world[OBJECT_MESH_MAXIMUM_NODES];
	real_point3d skinned[OBJECT_MESH_MAXIMUM_VERTICES];
};

static struct blood_skin_slot blood_skin_slots[BLOOD_SKIN_SLOTS];
static unsigned long blood_skin_clock = 0;
static boolean blood_skin_initialized = FALSE;
/* the object last posed: its nodes' matrices, its vertices, its bounds */
static real_matrix4x3 const *blood_world_nodes = NULL;
static real_point3d const *blood_skinned = NULL;
static real_point3d const *blood_skinned_minimum = NULL;
static real_point3d const *blood_skinned_maximum = NULL;

static void blood_skin_reset(void)
{
	short index;

	for (index = 0; index < BLOOD_SKIN_SLOTS; index++)
		blood_skin_slots[index].object_index = NONE;
	blood_skin_initialized = TRUE;
}

static struct object_mesh const *blood_object_skin(long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);
	struct object_mesh const *mesh = object ? object_mesh_get(object_index) : NULL;
	real_matrix4x3 const *matrices;
	real_matrix4x3 skin_nodes[OBJECT_MESH_MAXIMUM_NODES];
	struct blood_skin_slot *slot = NULL;
	long now = game_time_get();
	long count;
	long index;

	if (!blood_skin_initialized)
		blood_skin_reset();
	if (!mesh)
		return NULL;
	blood_skin_clock++;
	for (index = 0; index < BLOOD_SKIN_SLOTS; index++)
	{
		struct blood_skin_slot *candidate = &blood_skin_slots[index];

		if (candidate->object_index == object_index && candidate->time == now && candidate->mesh == mesh &&
			candidate->key == mesh->key)
		{
			slot = candidate;
			break;
		}
	}
	if (!slot)
	{
		for (index = 0; index < BLOOD_SKIN_SLOTS; index++)
		{
			struct blood_skin_slot *candidate = &blood_skin_slots[index];

			if (!slot || candidate->object_index == NONE || candidate->last_used < slot->last_used)
				slot = candidate;
			if (candidate->object_index == NONE)
				break;
		}
		matrices = (real_matrix4x3 const *)object_header_block_get(object_index, &object->object.node_matrices);
		count = object->object.node_matrices.size / (long)sizeof(real_matrix4x3);
		if (!matrices || count <= 0)
			return NULL;
		for (index = 0; index < mesh->node_count; index++)
		{
			slot->world[index] = matrices[index < count ? index : 0];
			matrix4x3_multiply(&slot->world[index], &mesh->inverse[index], &skin_nodes[index]);
		}
		for (index = 0; index < mesh->vertex_count; index++)
		{
			struct object_mesh_vertex const *vertex = &mesh->vertices[index];
			real_point3d p0;

			matrix4x3_transform_point(&skin_nodes[vertex->nodes[0]], &vertex->position, &p0);
			if (vertex->weight < 32767 && vertex->nodes[1] != vertex->nodes[0])
			{
				real_point3d p1;
				real w = (real)vertex->weight * (1.f / 32767.f);

				matrix4x3_transform_point(&skin_nodes[vertex->nodes[1]], &vertex->position, &p1);
				p0.x = p0.x * w + p1.x * (1.f - w);
				p0.y = p0.y * w + p1.y * (1.f - w);
				p0.z = p0.z * w + p1.z * (1.f - w);
			}
			slot->skinned[index] = p0;
			if (!index)
			{
				slot->minimum = slot->maximum = p0;
			}
			else
			{
				slot->minimum.x = MIN(slot->minimum.x, p0.x);
				slot->minimum.y = MIN(slot->minimum.y, p0.y);
				slot->minimum.z = MIN(slot->minimum.z, p0.z);
				slot->maximum.x = MAX(slot->maximum.x, p0.x);
				slot->maximum.y = MAX(slot->maximum.y, p0.y);
				slot->maximum.z = MAX(slot->maximum.z, p0.z);
			}
		}
		slot->object_index = object_index;
		slot->time = now;
		slot->mesh = mesh;
		slot->key = mesh->key;
	}
	slot->last_used = blood_skin_clock;
	blood_world_nodes = slot->world;
	blood_skinned = slot->skinned;
	blood_skinned_minimum = &slot->minimum;
	blood_skinned_maximum = &slot->maximum;
	return mesh;
}

/* a segment (from origin, along vector) through a box at all */
static boolean blood_segment_meets_box(real_point3d const *origin, real_vector3d const *vector,
	real_point3d const *minimum, real_point3d const *maximum, real margin)
{
	real enter = 0.f;
	real leave = 1.f;
	short axis;

	for (axis = 0; axis < 3; axis++)
	{
		real o = origin->n[axis];
		real v = vector->n[axis];
		real lo = minimum->n[axis] - margin;
		real hi = maximum->n[axis] + margin;

		if (fabs(v) < 1e-9f)
		{
			if (o < lo || o > hi)
				return FALSE;
		}
		else
		{
			real t0 = (lo - o) / v;
			real t1 = (hi - o) / v;

			if (t0 > t1)
			{
				real swap = t0;

				t0 = t1;
				t1 = swap;
			}
			enter = MAX(enter, t0);
			leave = MIN(leave, t1);
			if (enter > leave)
				return FALSE;
		}
	}
	return TRUE;
}

/* the node that moves a triangle (its first corner's, the one with the
most weight) */
static short blood_triangle_node(struct object_mesh const *mesh, long triangle)
{
	struct object_mesh_vertex const *vertex = &mesh->vertices[mesh->triangles[triangle][0]];

	return vertex->weight >= 16384 ? vertex->nodes[0] : vertex->nodes[1];
}

/* a triangle's normal as its corners turn (not facing anything) */
static real blood_triangle_normal(long triangle, struct object_mesh const *mesh, real_vector3d *normal)
{
	real_point3d const *a = &blood_skinned[mesh->triangles[triangle][0]];
	real_point3d const *b = &blood_skinned[mesh->triangles[triangle][1]];
	real_point3d const *c = &blood_skinned[mesh->triangles[triangle][2]];
	real_vector3d ab = { b->x - a->x, b->y - a->y, b->z - a->z };
	real_vector3d ac = { c->x - a->x, c->y - a->y, c->z - a->z };

	normal->i = ab.j * ac.k - ab.k * ac.j;
	normal->j = ab.k * ac.i - ab.i * ac.k;
	normal->k = ab.i * ac.j - ab.j * ac.i;
	return blood_normalize(normal);
}

/* a ray (from origin, along vector) to the object's drawn surface: the
nearest it meets, where, its normal facing back along the ray, which
triangle */
static boolean blood_object_raycast(long object_index, real_point3d const *origin, real_vector3d const *vector,
	real_point3d *point, real_vector3d *normal, long *triangle_out)
{
	struct object_datum *object = object_try_and_get(object_index);
	struct object_mesh const *mesh;
	real best = 2.f;
	long best_triangle = NONE;
	long triangle;
	boolean from_within;

	if (!object || !valid_real(vector->i) || !valid_real(origin->x))
		return FALSE;
	/* (the ray near the object at all) */
	{
		real_vector3d to_center = { object->object.bounding_sphere_center.x - origin->x,
			object->object.bounding_sphere_center.y - origin->y, object->object.bounding_sphere_center.z - origin->z };
		real length_squared = blood_dot(vector, vector);
		real along = length_squared > 0.f ? PIN(blood_dot(&to_center, vector) / length_squared, 0.f, 1.f) : 0.f;
		real_vector3d off = { to_center.i - vector->i * along, to_center.j - vector->j * along, to_center.k - vector->k * along };
		real reach = object->object.bounding_sphere_radius * 1.25f + 0.25f;

		if (blood_dot(&off, &off) > reach * reach)
			return FALSE;
	}
	mesh = blood_object_skin(object_index);
	if (!mesh || !blood_segment_meets_box(origin, vector, blood_skinned_minimum, blood_skinned_maximum, 0.01f))
		return FALSE;
	from_within = origin->x > blood_skinned_minimum->x && origin->x < blood_skinned_maximum->x &&
		origin->y > blood_skinned_minimum->y && origin->y < blood_skinned_maximum->y &&
		origin->z > blood_skinned_minimum->z && origin->z < blood_skinned_maximum->z;

	for (triangle = 0; triangle < mesh->triangle_count; triangle++)
	{
		real_point3d const *a = &blood_skinned[mesh->triangles[triangle][0]];
		real_point3d const *b = &blood_skinned[mesh->triangles[triangle][1]];
		real_point3d const *c = &blood_skinned[mesh->triangles[triangle][2]];
		real_vector3d e1 = { b->x - a->x, b->y - a->y, b->z - a->z };
		real_vector3d e2 = { c->x - a->x, c->y - a->y, c->z - a->z };
		real_vector3d p;
		real_vector3d s;
		real_vector3d q;
		real determinant;
		real inverse;
		real u;
		real v;
		real t;

		p.i = vector->j * e2.k - vector->k * e2.j;
		p.j = vector->k * e2.i - vector->i * e2.k;
		p.k = vector->i * e2.j - vector->j * e2.i;
		determinant = blood_dot(&e1, &p);
		/* (from outside the model's bounds, either face: the first surface
		the ray meets is its outside, whichever way the model's triangles
		happen to turn; from within them, its outer faces only, by the turn
		of the whole model's: a body's own blood leaving it meets nothing) */
		if (fabs(determinant) < 1e-12f || (from_within && mesh->outward * determinant <= 0.f))
			continue;
		inverse = 1.f / determinant;
		s.i = origin->x - a->x;
		s.j = origin->y - a->y;
		s.k = origin->z - a->z;
		u = blood_dot(&s, &p) * inverse;
		if (u < 0.f || u > 1.f)
			continue;
		q.i = s.j * e1.k - s.k * e1.j;
		q.j = s.k * e1.i - s.i * e1.k;
		q.k = s.i * e1.j - s.j * e1.i;
		v = blood_dot(vector, &q) * inverse;
		if (v < 0.f || u + v > 1.f)
			continue;
		t = blood_dot(&e2, &q) * inverse;
		if (t >= 0.f && t <= 1.f && t < best)
		{
			best = t;
			best_triangle = triangle;
		}
	}
	if (best_triangle == NONE || blood_triangle_normal(best_triangle, mesh, normal) == 0.f)
		return FALSE;
	/* (the face it met: toward where the ray came from) */
	if (from_within)
		scale_vector3d(normal, (real)mesh->outward, normal);
	else if (blood_dot(normal, vector) > 0.f)
		scale_vector3d(normal, -1.f, normal);
	point->x = origin->x + vector->i * best;
	point->y = origin->y + vector->j * best;
	point->z = origin->z + vector->k * best;
	if (triangle_out)
		*triangle_out = best_triangle;
	return TRUE;
}


static void blood_vehicle_decal_free(struct blood_vehicle_decal *entry, boolean delete_decal)
{
	if (entry->decal_index != NONE && delete_decal)
		decal_delete_polygons(entry->decal_index, entry->decal_definition_index);
	entry->decal_index = NONE;
}

/* a slot for blood on a vehicle, of so many quads: the oldest blood goes
for the new when the slots, or the quads blood may hold of the game's
decal cache, are all used */
static struct blood_vehicle_decal *blood_vehicle_decal_new(short quad_count)
{
	for (;;)
	{
		struct blood_vehicle_decal *oldest = NULL;
		struct blood_vehicle_decal *oldest_on_body = NULL;
		struct blood_vehicle_decal *free_entry = NULL;
		long quads = quad_count;
		long count = 1;
		long maximum_quads = BLOOD_DEFAULT_VEHICLE_QUADS;
		long maximum_count = BLOOD_DEFAULT_VEHICLE_DECALS;
		short index;

		for (index = 0; index < BLOOD_MAXIMUM_VEHICLE_DECALS; index++)
		{
			struct blood_vehicle_decal *entry = &blood_vehicle_decals[index];
			struct object_datum *object;

			if (entry->decal_index == NONE)
			{
				if (!free_entry)
					free_entry = entry;
				continue;
			}
			quads += entry->quad_count;
			count++;
			if (!oldest || entry->creation_time < oldest->creation_time)
				oldest = entry;
			/* (blood on characters, the dead above all, gives way before blood
			on vehicles: a vehicle's lasts, as the bodies come and go) */
			object = object_try_and_get(entry->object_index);
			if ((!object || object->object.type != _object_type_vehicle) &&
				(!oldest_on_body || entry->creation_time < oldest_on_body->creation_time))
			{
				oldest_on_body = entry;
			}
		}
		if (oldest_on_body)
			oldest = oldest_on_body;
		if (free_entry && ((quads <= maximum_quads && count <= maximum_count) || !oldest))
			return free_entry;
		if (!oldest)
			return &blood_vehicle_decals[0];
		blood_vehicle_decal_free(oldest, TRUE);
	}
}

/* a polygon clipped to one side of a line in the decal's plane (x along
axis 0, y along axis 1): keeps where value >= limit (sign 1) or <= (-1) */
struct blood_clip_vertex
{
	real_point3d world;
	real x;
	real y;
};

static short blood_clip(struct blood_clip_vertex const *in, short count, struct blood_clip_vertex *out,
	short axis, real sign, real limit)
{
	short made = 0;
	short index;

	for (index = 0; index < count && made < 12; index++)
	{
		struct blood_clip_vertex const *a = &in[index];
		struct blood_clip_vertex const *b = &in[(index + 1) % count];
		real da = sign * ((axis ? a->y : a->x) - limit);
		real db = sign * ((axis ? b->y : b->x) - limit);

		if (da >= 0.f)
			out[made++] = *a;
		if ((da >= 0.f) != (db >= 0.f) && made < 12)
		{
			real t = da / (da - db);
			struct blood_clip_vertex *v = &out[made++];

			v->world.x = a->world.x + (b->world.x - a->world.x) * t;
			v->world.y = a->world.y + (b->world.y - a->world.y) * t;
			v->world.z = a->world.z + (b->world.z - a->world.z) * t;
			v->x = a->x + (b->x - a->x) * t;
			v->y = a->y + (b->y - a->y) * t;
		}
	}
	return made;
}

/* blood painted on the object's drawn surface, as the game paints the
level's: a rectangle (half_length along axis, else any way; half_width
across) laid on the surface at point (on triangle, normal out of it),
clipped to every triangle of the surface it covers that faces the same
way, each piece riding the node that moves it */
static boolean blood_paint_object(long decal_definition_index, long object_index, real_point3d const *point,
	real_vector3d const *normal, long hit_triangle, real_vector3d const *axis, real half_length, real half_width)
{
	struct object_datum *object = object_try_and_get(object_index);
	struct object_mesh const *mesh = object ? blood_object_skin(object_index) : NULL;
	real_point3d positions[BLOOD_MAXIMUM_PAINT_QUADS * 4];
	real_point2d texcoords[BLOOD_MAXIMUM_PAINT_QUADS * 4];
	byte nodes[BLOOD_MAXIMUM_PAINT_QUADS * 4];
	short quad_count = 0;
	real_vector3d forward;
	real_vector3d left;
	real_vector3d raw;
	real sign;
	real depth;
	long triangle;

	if (!mesh || decal_definition_index == NONE || half_length <= 0.f || half_width <= 0.f ||
		!VALID_INDEX(hit_triangle, mesh->triangle_count) || object->object.location.cluster_index == NONE)
	{
		return FALSE;
	}
	/* (which way the mesh's triangles turn, from the one hit) */
	if (blood_triangle_normal(hit_triangle, mesh, &raw) == 0.f)
		return FALSE;
	/* (not on a visor, glass) */
	if (mesh->triangle_flags && (mesh->triangle_flags[hit_triangle] & OBJECT_MESH_TRIANGLE_NO_MARKS))
		return FALSE;
	sign = blood_dot(&raw, normal) >= 0.f ? 1.f : -1.f;

	/* its frame: across the surface, along axis or any way */
	if (axis)
	{
		real along = blood_dot(axis, normal);

		forward.i = axis->i - normal->i * along;
		forward.j = axis->j - normal->j * along;
		forward.k = axis->k - normal->k * along;
	}
	if (!axis || blood_normalize(&forward) == 0.f)
	{
		real angle = real_local_random_range(0.f, 2.f * _pi);
		real_vector3d tangent;
		real_vector3d other;

		perpendicular3d(normal, &tangent);
		blood_normalize(&tangent);
		other.i = normal->j * tangent.k - normal->k * tangent.j;
		other.j = normal->k * tangent.i - normal->i * tangent.k;
		other.k = normal->i * tangent.j - normal->j * tangent.i;
		forward.i = tangent.i * (real)cos(angle) + other.i * (real)sin(angle);
		forward.j = tangent.j * (real)cos(angle) + other.j * (real)sin(angle);
		forward.k = tangent.k * (real)cos(angle) + other.k * (real)sin(angle);
	}
	left.i = normal->j * forward.k - normal->k * forward.j;
	left.j = normal->k * forward.i - normal->i * forward.k;
	left.k = normal->i * forward.j - normal->j * forward.i;
	/* (the surface hit and what is flush with it: not a surface behind it,
	the body behind a wheel) */
	depth = MAX(half_length, half_width) * 0.25f + 0.01f;

	/* the triangles it may cover, nearest first (so a splat over more than it
	can hold keeps its middle) */
	{
		static long candidates[BLOOD_MAXIMUM_PAINT_CANDIDATES];
		static real candidate_distances[BLOOD_MAXIMUM_PAINT_CANDIDATES];
		short candidate_count = 0;
		short candidate;

		for (triangle = 0; triangle < mesh->triangle_count; triangle++)
		{
			real_point3d const *a = &blood_skinned[mesh->triangles[triangle][0]];
			real_point3d const *b = &blood_skinned[mesh->triangles[triangle][1]];
			real_point3d const *c = &blood_skinned[mesh->triangles[triangle][2]];
			boolean outside[6] = { TRUE, TRUE, TRUE, TRUE, TRUE, TRUE };
			real_point3d const *corners[3];
			real_vector3d centroid;
			real distance;
			short corner;
			short slot;

			corners[0] = a;
			corners[1] = b;
			corners[2] = c;
			for (corner = 0; corner < 3; corner++)
			{
				real_vector3d d = { corners[corner]->x - point->x, corners[corner]->y - point->y, corners[corner]->z - point->z };
				real x = blood_dot(&d, &forward);
				real y = blood_dot(&d, &left);
				real z = blood_dot(&d, normal);

				outside[0] = outside[0] && x < -half_length;
				outside[1] = outside[1] && x > half_length;
				outside[2] = outside[2] && y < -half_width;
				outside[3] = outside[3] && y > half_width;
				outside[4] = outside[4] && z > depth;
				outside[5] = outside[5] && z < -depth;
			}
			if (outside[0] || outside[1] || outside[2] || outside[3] || outside[4] || outside[5])
				continue;
			centroid.i = (a->x + b->x + c->x) / 3.f - point->x;
			centroid.j = (a->y + b->y + c->y) / 3.f - point->y;
			centroid.k = (a->z + b->z + c->z) / 3.f - point->z;
			distance = blood_dot(&centroid, &centroid);
			if (triangle == hit_triangle)
				distance = -1.f;
			/* (kept in order, the farthest dropped when full) */
			if (candidate_count == BLOOD_MAXIMUM_PAINT_CANDIDATES)
			{
				if (distance >= candidate_distances[candidate_count - 1])
					continue;
				candidate_count--;
			}
			for (slot = candidate_count; slot > 0 && candidate_distances[slot - 1] > distance; slot--)
			{
				candidates[slot] = candidates[slot - 1];
				candidate_distances[slot] = candidate_distances[slot - 1];
			}
			candidates[slot] = triangle;
			candidate_distances[slot] = distance;
			candidate_count++;
		}

	for (candidate = 0; candidate < candidate_count && quad_count < BLOOD_MAXIMUM_PAINT_QUADS; candidate++)
	{
		struct blood_clip_vertex polygon[12];
		struct blood_clip_vertex clipped[12];
		real_vector3d facing;
		short count = 3;
		short corner;
		short node;

		triangle = candidates[candidate];
		if (mesh->triangle_flags && (mesh->triangle_flags[triangle] & OBJECT_MESH_TRIANGLE_NO_MARKS))
			continue;
		for (corner = 0; corner < 3; corner++)
		{
			real_point3d const *v = &blood_skinned[mesh->triangles[triangle][corner]];
			real_vector3d d = { v->x - point->x, v->y - point->y, v->z - point->z };

			polygon[corner].world = *v;
			polygon[corner].x = blood_dot(&d, &forward);
			polygon[corner].y = blood_dot(&d, &left);
		}
		/* (only what faces as the surface hit does: not its far side, nor a
		face across an edge turned away) */
		if (blood_triangle_normal(triangle, mesh, &facing) == 0.f)
			continue;
		scale_vector3d(&facing, sign, &facing);
		if (blood_dot(&facing, normal) < blood_paint_min_facing)
			continue;

		count = blood_clip(polygon, count, clipped, 0, 1.f, -half_length);
		count = count >= 3 ? blood_clip(clipped, count, polygon, 0, -1.f, half_length) : 0;
		count = count >= 3 ? blood_clip(polygon, count, clipped, 1, 1.f, -half_width) : 0;
		count = count >= 3 ? blood_clip(clipped, count, polygon, 1, -1.f, half_width) : 0;
		if (count < 3)
			continue;

		/* (fans of quads: 0 i i+1 i+2) */
		node = blood_triangle_node(mesh, triangle);
		for (corner = 1; corner + 1 < count && quad_count < BLOOD_MAXIMUM_PAINT_QUADS; corner += 2)
		{
			short fan[4];
			short k;

			fan[0] = 0;
			fan[1] = corner;
			fan[2] = corner + 1;
			fan[3] = corner + 2 < count ? corner + 2 : corner + 1;
			for (k = 0; k < 4; k++)
			{
				struct blood_clip_vertex const *v = &polygon[fan[k]];
				short at = quad_count * 4 + k;

				positions[at] = v->world;
				blood_add(&positions[at], &facing, BLOOD_LIFT);
				texcoords[at].x = 0.5f + ((v->x + half_length) / (2.f * half_length) - 0.5f) * blood_paint_core_along;
				texcoords[at].y = 0.5f + ((v->y + half_width) / (2.f * half_width) - 0.5f) * blood_paint_core_across;
				nodes[at] = (byte)node;
			}
			quad_count++;
		}
	}
	}
	if (!quad_count)
		return FALSE;

	{
		struct blood_vehicle_decal *entry = blood_vehicle_decal_new(quad_count);
		long decal_index = decal_new_polygons(decal_definition_index, point, quad_count, positions, texcoords,
			object->object.location.cluster_index);
		short corner;

		if (decal_index == NONE)
			return FALSE;
		decal_keep_at_least(decal_index, blood_parameters.stay_seconds);
		entry->decal_index = decal_index;
		entry->decal_definition_index = decal_definition_index;
		entry->object_index = object_index;
		entry->creation_time = game_time_get();
		entry->quad_count = quad_count;
		for (corner = 0; corner < quad_count * 4; corner++)
		{
			entry->nodes[corner] = nodes[corner];
			blood_to_local(&blood_world_nodes[nodes[corner]], &positions[corner], &entry->corners[corner]);
		}
	}
	return TRUE;
}

static void blood_drip_start(long decal_definition_index, long object_index, short node_index, long triangle,
	real_point3d const *point, real_vector3d const *normal, real radius, short cluster_index);

/* a splat painted where a ray meets the object, running if the surface
there is steep */
static boolean blood_paint_along(long decal_definition_index, long object_index, real_point3d const *origin,
	real_vector3d const *ray, real radius, real lowest_normal_k)
{
	real_point3d point;
	real_vector3d normal;
	long triangle;
	struct object_mesh const *mesh;

	if (decal_definition_index == NONE || radius <= 0.f ||
		!blood_object_raycast(object_index, origin, ray, &point, &normal, &triangle) || normal.k < lowest_normal_k)
	{
		return FALSE;
	}
	if (!blood_paint_object(decal_definition_index, object_index, &point, &normal, triangle, NULL, radius, radius))
		return FALSE;
	mesh = blood_object_skin(object_index);
	if (mesh && blood_parameters.drips >= 0.5f && fabs(normal.k) < BLOOD_STEEP)
	{
		blood_drip_start(decal_definition_index, object_index, blood_triangle_node(mesh, triangle), triangle, &point,
			&normal, radius, NONE);
	}
	return TRUE;
}

/* a splat of blood on a vehicle, where a body met it: on the surface the
body met if it shows (not its underside), else on the faces the blood flies
at, the vehicle's front (its way) and its sides, low down */
static void blood_vehicle_splat(long decal_definition_index, long vehicle_index, real_point3d const *near,
	real_vector3d const *normal, real_vector3d const *heading, real radius)
{
	struct object_datum *vehicle = object_try_and_get(vehicle_index);
	real_vector3d side;
	boolean found = FALSE;
	short attempt;
	short first;
	real reach;

	if (decal_definition_index == NONE || radius <= 0.f || !vehicle)
		return;
	/* (rays from outside it all: as far as it is across) */
	reach = PIN(vehicle->object.bounding_sphere_radius, 0.3f, 3.f) * 2.f;
	side.i = -heading->j;
	side.j = heading->i;
	side.k = 0.f;
	if (blood_normalize(&side) == 0.f)
	{
		side.i = 1.f;
		side.j = 0.f;
	}
	if (blood_dot(&side, normal) < 0.f)
		scale_vector3d(&side, -1.f, &side);

	first = normal->k > -0.5f ? 0 : 1;
	for (attempt = first; attempt < 4 && !found; attempt++)
	{
		real_point3d origin = *near;
		real_vector3d ray;

		switch (attempt)
		{
		case 0: /* the surface it met */
			blood_add(&origin, normal, 0.3f);
			scale_vector3d(normal, -0.6f, &ray);
			break;
		case 1: /* the front, from ahead of it */
		case 2:
			if (blood_dot(heading, heading) < 0.5f)
				continue;
			origin.z += real_local_random_range(0.02f, 0.12f) * (attempt == 1 ? 1.f : 2.f);
			blood_add(&origin, heading, reach);
			scale_vector3d(heading, -1.5f * reach, &ray);
			break;
		default: /* the side by it */
			origin.z += real_local_random_range(0.03f, 0.15f);
			blood_add(&origin, &side, reach);
			scale_vector3d(&side, -1.5f * reach, &ray);
			break;
		}
		found = blood_paint_along(decal_definition_index, vehicle_index, &origin, &ray, radius, -0.5f);
	}
}

/* a drip's quad on the level: its run from one point to another (a thin
slice down the middle of the splat's picture stretched along it, soft
across), or a round bead at a point (the picture's round middle); its
corners as decal_new_quad winds them, clockwise seen from in front */
static void blood_drip_corners(struct blood_drip const *drip, real_point3d const *from, real_point3d const *to,
	boolean bead, real lift, real_point3d *middle, real_vector3d *forward, real *half_length, real *half_width,
	real_point3d corners[4])
{
	real_vector3d const *normal = &drip->live_normal;
	real_vector3d left;
	real length;
	real dot;
	short corner;

	forward->i = to->x - from->x;
	forward->j = to->y - from->y;
	forward->k = to->z - from->z;
	dot = blood_dot(forward, normal);
	forward->i -= normal->i * dot;
	forward->j -= normal->j * dot;
	forward->k -= normal->k * dot;
	length = blood_normalize(forward);
	if (length == 0.f && !blood_downhill(normal, forward))
		perpendicular3d(normal, forward);
	left.i = normal->j * forward->k - normal->k * forward->j;
	left.j = normal->k * forward->i - normal->i * forward->k;
	left.k = normal->i * forward->j - normal->j * forward->i;
	blood_normalize(&left);
	middle->x = (from->x + to->x) * 0.5f + normal->i * lift;
	middle->y = (from->y + to->y) * 0.5f + normal->j * lift;
	middle->z = (from->z + to->z) * 0.5f + normal->k * lift;
	if (bead)
	{
		*half_length = drip->live_half_width * BLOOD_RUN_WIDEN * 1.25f;
		*half_width = drip->live_half_width * BLOOD_RUN_WIDEN * 1.15f;
	}
	else
	{
		*half_length = length * 0.5f + drip->live_half_width * BLOOD_RUN_WIDEN * 0.7f;
		*half_width = drip->live_half_width * BLOOD_RUN_WIDEN;
	}
	for (corner = 0; corner < 4; corner++)
	{
		real x = (corner >= 2) ? *half_length : -*half_length;
		real y = (corner == 1 || corner == 2) ? *half_width : -*half_width;

		corners[corner].x = middle->x + forward->i * x + left.i * y;
		corners[corner].y = middle->y + forward->j * x + left.j * y;
		corners[corner].z = middle->z + forward->k * x + left.k * y;
	}
}

static long blood_drip_quad(struct blood_drip const *drip, real_point3d const *from, real_point3d const *to,
	boolean bead, real lift)
{
	real_point3d middle;
	real_vector3d forward;
	real half_length;
	real half_width;
	real_point3d corners[4];
	long decal_index;

	blood_drip_corners(drip, from, to, bead, lift, &middle, &forward, &half_length, &half_width, corners);
	if (half_length <= 0.f || half_width <= 0.f || drip->cluster_index == NONE)
		return NONE;
	decal_port_texture_core_along = bead ? BLOOD_BEAD_CORE : BLOOD_RUN_CORE_ALONG;
	decal_port_texture_core_across = bead ? BLOOD_BEAD_CORE : BLOOD_RUN_CORE_ACROSS;
	decal_port_full_intensity = TRUE;
	decal_index = decal_new_quad(drip->decal_definition_index, &middle, &drip->live_normal, &forward, half_length,
		half_width, drip->cluster_index, corners);
	decal_port_texture_core_along = decal_port_texture_core_across = 1.f;
	decal_port_full_intensity = FALSE;
	return decal_index;
}

/* its live run (from where its last kept run ended to its head) and the
bead at its head, moved there, or made if gone */
static void blood_drip_place(struct blood_drip *drip, real_point3d const *from, real_point3d const *head, boolean run)
{
	real_point3d middle;
	real_vector3d forward;
	real half_length;
	real half_width;
	real_point3d corners[4];

	if (drip->cluster_index == NONE || drip->live_half_width <= 0.f)
		return;
	if (run && distance3d(from, head) > drip->live_half_width * 0.2f)
	{
		blood_drip_corners(drip, from, head, FALSE, BLOOD_LIFT * 0.5f, &middle, &forward, &half_length, &half_width, corners);
		if (drip->live_decal == NONE ||
			!decal_move_quad(drip->live_decal, drip->decal_definition_index, corners, drip->cluster_index))
		{
			drip->live_decal = blood_drip_quad(drip, from, head, FALSE, BLOOD_LIFT * 0.5f);
		}
	}
	blood_drip_corners(drip, head, head, TRUE, BLOOD_LIFT, &middle, &forward, &half_length, &half_width, corners);
	if (drip->live_bead == NONE ||
		!decal_move_quad(drip->live_bead, drip->decal_definition_index, corners, drip->cluster_index))
	{
		drip->live_bead = blood_drip_quad(drip, head, head, TRUE, BLOOD_LIFT);
	}
}

/* every frame: each running drip on the level drawn out to where its head
is between ticks (smooth, not a stretch a tick) */
static void blood_drips_render(void)
{
	real fraction = PIN(render_interpolation_fraction(), 0.f, 1.f);
	short index;

	for (index = 0; index < BLOOD_MAXIMUM_DRIPS; index++)
	{
		struct blood_drip *drip = &blood_drips[index];
		real_point3d head;

		if (!drip->active || drip->object_index != NONE || drip->cluster_index == NONE ||
			(drip->live_decal == NONE && drip->live_bead == NONE))
		{
			continue;
		}
		head.x = drip->head_before.x + (drip->head.x - drip->head_before.x) * fraction;
		head.y = drip->head_before.y + (drip->head.y - drip->head_before.y) * fraction;
		head.z = drip->head_before.z + (drip->head.z - drip->head_before.z) * fraction;
		blood_laying = TRUE;
		blood_drip_place(drip, &drip->laid, &head, TRUE);
		blood_laying = FALSE;
	}
}

void blood_render_update(
	void)
{
	short index;

	if (blood_parameters.drips >= 0.5f)
		blood_drips_render();

	for (index = 0; index < BLOOD_MAXIMUM_VEHICLE_DECALS; index++)
	{
		struct blood_vehicle_decal *entry = &blood_vehicle_decals[index];
		struct object_datum *object;
		real_matrix4x3 const *matrices = NULL;
		real_point3d corners[BLOOD_MAXIMUM_PAINT_QUADS * 4];
		long count = 0;
		short corner;

		if (entry->decal_index == NONE)
			continue;
		object = object_try_and_get(entry->object_index);
		if (object)
		{
			count = object->object.node_matrices.size / (long)sizeof(real_matrix4x3);
			matrices = (real_matrix4x3 const *)render_interpolation_object_node_matrices(entry->object_index);
			if (!matrices)
				matrices = (real_matrix4x3 const *)object_header_block_get(entry->object_index, &object->object.node_matrices);
		}
		if (!object || !matrices || count <= 0)
		{
			/* (the vehicle gone: its blood with it) */
			blood_vehicle_decal_free(entry, TRUE);
			continue;
		}
		for (corner = 0; corner < entry->quad_count * 4; corner++)
			blood_to_world(&matrices[entry->nodes[corner] < count ? entry->nodes[corner] : 0], &entry->corners[corner], &corners[corner]);
		/* (marks on a player's own unit, seen from inside it: drawn as
		nothing, a point, not over the screen) */
		if (object->object.type == _object_type_biped && blood_object_seen_first_person(entry->object_index))
		{
			for (corner = 1; corner < entry->quad_count * 4; corner++)
				corners[corner] = corners[0];
		}
		if (!decal_move_polygons(entry->decal_index, entry->decal_definition_index, entry->quad_count, corners,
			&corners[0], object->object.location.cluster_index))
		{
			/* (faded, or its vertices given to newer decals) */
			blood_vehicle_decal_free(entry, FALSE);
		}
	}
}

/* ---------- drips */

/* the patterns a splat runs in: each splat picks one, and each of its drips
its own length, speed, width, wander and moment to start */
enum
{
	_blood_drip_pattern_single, /* one run */
	_blood_drip_pattern_pair, /* two, side by side */
	_blood_drip_pattern_curtain, /* a row across the splat, of all lengths */
	_blood_drip_pattern_fan, /* from one spot, spreading as they run */
	_blood_drip_pattern_trickle, /* many short stubs */
	_blood_drip_pattern_runner, /* one long heavy run, short ones beside it */
	_blood_drip_pattern_late, /* one, then more after a while (still seeping) */
	NUMBER_OF_BLOOD_DRIP_PATTERNS
};

static void blood_drip_start(long decal_definition_index, long object_index, short node_index, long triangle,
	real_point3d const *point, real_vector3d const *normal, real radius, short cluster_index)
{
	short pattern = (short)local_random_range(0, NUMBER_OF_BLOOD_DRIP_PATTERNS);
	short count;
	short made;
	real fan_spot = real_local_random_range(-0.3f, 0.3f);

	switch (pattern)
	{
	case _blood_drip_pattern_single: count = 1; break;
	case _blood_drip_pattern_pair: count = 2; break;
	case _blood_drip_pattern_curtain: count = (short)local_random_range(3, 7); break;
	case _blood_drip_pattern_fan: count = (short)local_random_range(2, 5); break;
	case _blood_drip_pattern_trickle: count = (short)local_random_range(4, 8); break;
	case _blood_drip_pattern_runner: count = (short)local_random_range(2, 4); break;
	default: count = (short)local_random_range(2, 4); break;
	}
	/* (a small splat, fewer) */
	if (radius < 0.05f)
		count = (short)MAX(1, count / 2);

	for (made = 0; made < count; made++)
	{
		struct blood_drip *drip = NULL;
		real_vector3d down;
		real_vector3d across;
		real_point3d start = *point;
		real length_scale = real_local_random_range(1.f, 3.5f);
		real width_scale = real_local_random_range(0.08f, 0.16f);
		real speed = real_local_random_range(0.25f, 0.6f);
		real wander = real_local_random_range(-0.15f, 0.15f);
		real offset_across = real_local_random_range(-0.5f, 0.5f);
		real offset_down = real_local_random_range(0.f, 0.4f);
		short pause = (short)local_random_range(0, 8);
		short index;

		for (index = 0; index < BLOOD_MAXIMUM_DRIPS; index++)
		{
			if (!blood_drips[index].active)
			{
				drip = &blood_drips[index];
				break;
			}
		}
		if (!drip || !blood_downhill(normal, &down))
			return;

		switch (pattern)
		{
		case _blood_drip_pattern_single:
			length_scale = real_local_random_range(1.5f, 4.5f);
			break;
		case _blood_drip_pattern_pair:
			offset_across = (made ? 1.f : -1.f) * real_local_random_range(0.1f, 0.35f);
			pause = (short)(made * local_random_range(0, 20));
			break;
		case _blood_drip_pattern_curtain:
			/* (spread evenly across, some long, most short) */
			offset_across = -0.6f + 1.2f * (made + real_local_random_range(0.2f, 0.8f)) / count;
			length_scale = local_random_range(0, 3) ? real_local_random_range(0.4f, 1.6f) : real_local_random_range(2.f, 4.f);
			width_scale = real_local_random_range(0.05f, 0.11f);
			pause = (short)local_random_range(0, 25);
			break;
		case _blood_drip_pattern_fan:
			offset_across = fan_spot + real_local_random_range(-0.05f, 0.05f);
			wander = -0.35f + 0.7f * (made + 0.5f) / count + real_local_random_range(-0.05f, 0.05f);
			length_scale = real_local_random_range(1.f, 3.f);
			break;
		case _blood_drip_pattern_trickle:
			length_scale = real_local_random_range(0.3f, 1.f);
			width_scale = real_local_random_range(0.05f, 0.1f);
			speed = real_local_random_range(0.15f, 0.35f);
			pause = (short)local_random_range(0, 40);
			break;
		case _blood_drip_pattern_runner:
			if (made == 0)
			{
				length_scale = real_local_random_range(3.5f, 6.f);
				width_scale = real_local_random_range(0.15f, 0.22f);
				speed = real_local_random_range(0.4f, 0.8f);
				offset_across = real_local_random_range(-0.15f, 0.15f);
			}
			else
			{
				length_scale = real_local_random_range(0.3f, 1.2f);
				width_scale = real_local_random_range(0.05f, 0.1f);
				pause = (short)local_random_range(5, 30);
			}
			break;
		case _blood_drip_pattern_late:
			/* (the first at once, the rest seeping out seconds later, slowly) */
			if (made > 0)
			{
				pause = (short)local_random_range(30, 120);
				speed = real_local_random_range(0.12f, 0.3f);
				length_scale = real_local_random_range(0.6f, 2.5f);
			}
			break;
		}

		/* (from somewhere in the splat's lower half) */
		across.i = normal->j * down.k - normal->k * down.j;
		across.j = normal->k * down.i - normal->i * down.k;
		across.k = normal->i * down.j - normal->j * down.i;
		blood_add(&start, &across, offset_across * radius);
		blood_add(&start, &down, offset_down * radius);

		drip->active = TRUE;
		drip->decal_definition_index = decal_definition_index;
		drip->object_index = object_index;
		drip->node_index = node_index;
		drip->triangle = triangle;
		drip->cluster_index = cluster_index;
		drip->width = PIN(radius * width_scale, 0.003f, 0.035f);
		drip->start_width = drip->width;
		drip->wander = PIN(wander, -0.45f, 0.45f);
		drip->pause = pause; /* (a moment gathering before it runs) */
		drip->live_decal = NONE;
		drip->live_bead = NONE;
		drip->head_before = drip->head = start;
		drip->live_normal = *normal;
		drip->live_half_width = 0.f;
		drip->length = PIN(radius * length_scale, 0.02f, 0.6f) *
			PIN(blood_parameters.drip_length, 0.f, 5.f);
		drip->remaining = drip->length;
		drip->age = 0;
		/* (metres a second, slowing as it runs) */
		drip->speed = speed / (BLOOD_METRES * 30.f);
		/* (slowing so it comes to rest about its length down: a run of speed /
		(1 - slowing)) */
		drip->slowing = PIN(1.f - drip->speed / MAX(drip->length, 1e-4f), 0.85f, 0.99f);
		if (object_index != NONE)
		{
			real_matrix4x3 const *matrix = blood_node_matrix(object_index, node_index, FALSE);

			if (!matrix)
			{
				drip->active = FALSE;
				return;
			}
			blood_to_local(matrix, &start, &drip->position);
			blood_normal_to_local(matrix, normal, &drip->normal);
		}
		else
		{
			drip->position = start;
			drip->normal = *normal;
		}
		drip->laid = drip->position;
	}
}

void blood_decal_landed(
	long decal_definition_index,
	real_point3d const *point,
	real_vector3d const *normal,
	real radius,
	short cluster_index)
{
	if (blood_laying || blood_parameters.drips < 0.5f || !blood_violence_allowed() ||
		fabs(normal->k) >= BLOOD_STEEP || radius <= 0.f)
	{
		return;
	}
	if (decal_definition_index != blood_checked_definition)
	{
		blood_checked_definition = decal_definition_index;
		blood_checked_is_blood = blood_name_has(decal_definition_index, "blood");
	}
	if (!blood_checked_is_blood)
		return;
	blood_drip_start(decal_definition_index, NONE, NONE, NONE, point, normal, radius, cluster_index);
}

/* a drip's step: it runs on down its surface, laying a streak behind it,
until it has run its length, slowed to nothing or come to the surface's edge */
/* what is left of a run, pooled where its surface turned level */
static void blood_drip_pool(struct blood_drip *drip, real_point3d const *point, real_vector3d const *normal)
{
	real share = PIN(drip->remaining / MAX(drip->length, 1e-4f), 0.f, 1.f);
	real radius = drip->start_width * (1.2f + 2.5f * share);
	real definition_radius = decal_definition_radius(drip->decal_definition_index);

	if (share < 0.1f || radius <= 0.f || definition_radius <= 0.f)
		return;
	blood_laying = TRUE;
	decal_port_full_intensity = TRUE;
	if (drip->object_index != NONE)
	{
		blood_paint_object(drip->decal_definition_index, drip->object_index, point, normal, drip->triangle, NULL,
			radius, radius);
	}
	else
	{
		real_point3d above = *point;
		real_vector3d into;

		blood_add(&above, normal, 0.05f);
		scale_vector3d(normal, -0.15f, &into);
		decal_new(drip->decal_definition_index, &above, &into, radius / definition_radius, FALSE, NONE, NULL);
	}
	decal_port_full_intensity = FALSE;
	blood_laying = FALSE;
}

/* a branch off a run: from where it is, with some of what is left of it,
wandering off its own way */
static void blood_drip_branch(struct blood_drip *drip, real_point3d const *point, real_vector3d const *normal)
{
	struct blood_drip *branch = NULL;
	short index;

	for (index = 0; index < BLOOD_MAXIMUM_DRIPS; index++)
	{
		if (!blood_drips[index].active)
		{
			branch = &blood_drips[index];
			break;
		}
	}
	if (!branch)
		return;
	*branch = *drip;
	branch->remaining = drip->remaining * 0.5f;
	branch->length = drip->length * 0.6f;
	branch->width = drip->width * 0.7f;
	branch->start_width = branch->width;
	branch->wander = drip->wander + (local_random_range(0, 2) ? 0.35f : -0.35f);
	branch->pause = 0;
	branch->live_decal = NONE;
	branch->live_bead = NONE;
	drip->remaining *= 0.7f;
	/* (where it is now: the branch's run starts there) */
	if (drip->object_index != NONE)
	{
		real_matrix4x3 const *matrix = blood_node_matrix(drip->object_index, drip->node_index, FALSE);

		if (!matrix)
		{
			branch->active = FALSE;
			return;
		}
		blood_to_local(matrix, point, &branch->position);
		blood_normal_to_local(matrix, normal, &branch->normal);
	}
	else
	{
		branch->position = *point;
		branch->normal = *normal;
	}
	branch->laid = branch->position;
}

static void blood_drip_update(struct blood_drip *drip)
{
	real_point3d world;
	real_vector3d normal;
	real_vector3d down;
	real_point3d next;
	real_matrix4x3 const *matrix = NULL;
	real_point3d origin;
	real_vector3d ray;
	boolean on_surface;
	real laid_distance;
	real_point3d laid_world;
	real_point3d hit_point;
	real_vector3d hit_normal;
	long hit_triangle = NONE;

	if (drip->object_index != NONE)
	{
		matrix = blood_node_matrix(drip->object_index, drip->node_index, FALSE);
		if (!matrix)
		{
			drip->active = FALSE;
			return;
		}
		blood_to_world(matrix, &drip->position, &world);
		blood_normal_to_world(matrix, &drip->normal, &normal);
		blood_to_world(matrix, &drip->laid, &laid_world);
	}
	else
	{
		world = drip->position;
		normal = drip->normal;
		laid_world = drip->laid;
	}
	if (!blood_downhill(&normal, &down) || fabs(normal.k) >= BLOOD_STEEP + 0.15f)
	{
		/* (its surface turned level: what is left of it pools there) */
		if (normal.k < -0.3f)
			blood_drop_fall(drip->decal_definition_index, &world, drip->width * 0.7f); /* (an underside: it drops) */
		else
			blood_drip_pool(drip, &world, &normal);
		drip->active = FALSE;
		return;
	}
	/* (a gathering bead: it holds, then lets go faster) */
	if (drip->pause > 0)
	{
		drip->pause--;
		if (drip->pause == 0)
			drip->speed *= 1.6f;
		/* (its head held where it is, frame to frame) */
		drip->head_before = drip->head;
		return;
	}
	/* (not straight down: wandering a little either way, as a run finds its
	way over a surface) */
	{
		real_vector3d across;

		across.i = normal.j * down.k - normal.k * down.j;
		across.j = normal.k * down.i - normal.i * down.k;
		across.k = normal.i * down.j - normal.j * down.i;
		drip->wander = PIN(drip->wander + real_local_random_range(-0.09f, 0.09f), -0.4f, 0.4f);
		down.i = down.i * (real)cos(drip->wander) + across.i * (real)sin(drip->wander);
		down.j = down.j * (real)cos(drip->wander) + across.j * (real)sin(drip->wander);
		down.k = down.k * (real)cos(drip->wander) + across.k * (real)sin(drip->wander);
	}

	next = world;
	blood_add(&next, &down, drip->speed * real_local_random_range(0.75f, 1.25f));

	/* still on its surface? */
	origin = next;
	blood_add(&origin, &normal, 0.04f);
	scale_vector3d(&normal, -0.1f, &ray);
	if (drip->object_index != NONE)
	{
		on_surface = blood_object_raycast(drip->object_index, &origin, &ray, &hit_point, &hit_normal, &hit_triangle);
	}
	else
	{
		struct collision_result collision;

		match_assert("blood.c", __LINE__, global_current_collision_user_depth < MAXIMUM_COLLISION_USER_STACK_DEPTH);
		global_current_collision_users[global_current_collision_user_depth++] = _collision_user_decals;
		on_surface = collision_test_vector(
			FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit),
			&origin, &ray, NONE, &collision) &&
			collision.type == _collision_result_structure && valid_real_plane3d(&collision.plane);
		global_current_collision_user_depth--;
		if (on_surface)
		{
			hit_point = collision.point;
			hit_normal = collision.plane.n;
			drip->cluster_index = collision.location.cluster_index;
		}
	}
	on_surface = on_surface && blood_dot(&hit_normal, &normal) > 0.9f;
	if (on_surface)
	{
		next = hit_point;
		normal = hit_normal;
		if (hit_triangle != NONE)
			drip->triangle = hit_triangle;
	}
	else
	{
		/* (over the surface's edge: it stops where it was, at the edge; what
		is left of it gathers there and falls) */
		next = world;
		if (drip->remaining > drip->length * 0.15f)
			blood_drop_fall(drip->decal_definition_index, &world, drip->width * 0.6f);
	}

	/* a streak from where the last ended, once it has run a few times its
	width (few decals: a trail is a handful of long thin quads), or stops */
	laid_distance = distance3d(&laid_world, &next);
	drip->remaining -= drip->speed;
	drip->speed *= drip->slowing;
	drip->age++;
	/* (all but run out: it stops, in a few seconds at most) */
	if (drip->remaining < drip->length * 0.12f || drip->speed < 0.0004f || drip->age > 6 * 30)
		drip->remaining = 0.f;
	/* (now and then it stops to gather, a bead at its head; now and then a
	branch runs off it with some of what is left) */
	if (on_surface && drip->remaining > drip->length * 0.3f && local_random_range(0, 40) == 0)
		drip->pause = (short)local_random_range(4, 16);
	if (on_surface && drip->remaining > drip->length * 0.45f && local_random_range(0, 70) == 0)
		blood_drip_branch(drip, &next, &normal);
	if (drip->object_index == NONE && drip->cluster_index != NONE)
	{
		/* on the level: a live run, its streak from where the last ended to
		its head and a bead at its head, both moved as it runs (and, between
		ticks, frame by frame: blood_render_update); kept and a new one begun
		every so often, or where it stops or gathers */
		boolean stopping = !on_surface || drip->remaining <= 0.f;
		boolean keep = stopping || drip->pause > 0 || laid_distance >= MAX(drip->width * 3.5f, 0.03f);

		drip->head_before = drip->live_decal != NONE || drip->live_bead != NONE ? drip->head : next;
		drip->head = next;
		drip->live_normal = normal;
		drip->live_half_width = drip->width * (0.45f + 0.55f * PIN(drip->remaining / MAX(drip->length, 1e-4f), 0.f, 1.f)) * 0.5f;
		blood_laying = TRUE;
		blood_drip_place(drip, &laid_world, &next, TRUE);
		if (keep)
		{
			/* (its run kept, laid over again for depth) */
			short layers = (short)PIN(blood_drip_layers, 1.f, 3.f);
			short layer;

			for (layer = 1; layer < layers; layer++)
				blood_drip_quad(drip, &laid_world, &next, FALSE, BLOOD_LIFT * (0.5f + 0.25f * layer));
			if (stopping || drip->pause > 0)
				blood_drip_quad(drip, &next, &next, TRUE, BLOOD_LIFT);
			drip->live_decal = NONE;
			drip->head_before = next;
			if (stopping)
				drip->live_bead = NONE;
			laid_world = next;
		}
		blood_laying = FALSE;
		if (stopping)
		{
			drip->active = FALSE;
			return;
		}
	}
	else if (laid_distance >= MAX(drip->width * 3.5f, 0.03f) || !on_surface || drip->remaining <= 0.f || drip->pause > 0)
	{
		/* on an object: a streak at a time, moved with it (blood_render_update) */
		real_point3d middle;
		real_vector3d along;
		real thinning = 0.45f + 0.55f * PIN(drip->remaining / MAX(drip->length, 1e-4f), 0.f, 1.f);
		real half_width = drip->width * thinning * 0.5f * BLOOD_RUN_WIDEN;
		real half_length = MAX(laid_distance * 0.5f + half_width * 0.7f, half_width);
		boolean stopping = !on_surface || drip->remaining <= 0.f;

		middle.x = (laid_world.x + next.x) * 0.5f;
		middle.y = (laid_world.y + next.y) * 0.5f;
		middle.z = (laid_world.z + next.z) * 0.5f;
		along.i = next.x - laid_world.x;
		along.j = next.y - laid_world.y;
		along.k = next.z - laid_world.z;
		if (blood_normalize(&along) == 0.f)
			along = down;

		blood_laying = TRUE;
		decal_port_full_intensity = TRUE;
		{
			short layers = 1; /* (on an object, once: what an object holds is shared, the vehicles' blood first) */
			short layer;
			boolean bead = stopping || drip->pause > 0;

			for (layer = 0; layer < layers; layer++)
			{
				blood_paint_core_along = BLOOD_RUN_CORE_ALONG;
				blood_paint_core_across = BLOOD_RUN_CORE_ACROSS;
				blood_paint_object(drip->decal_definition_index, drip->object_index, &middle, &normal, drip->triangle,
					&along, half_length, half_width);
				/* (a round bead where it stops, or gathers) */
				if (bead)
				{
					blood_paint_core_along = blood_paint_core_across = BLOOD_BEAD_CORE;
					blood_paint_object(drip->decal_definition_index, drip->object_index, &next, &normal, drip->triangle,
						&along, half_width * 1.4f, half_width * 1.3f);
				}
			}
		}
		blood_paint_core_along = blood_paint_core_across = 1.f;
		decal_port_full_intensity = FALSE;
		blood_laying = FALSE;
		if (stopping)
		{
			drip->active = FALSE;
			return;
		}
		laid_world = next;
	}

	if (drip->object_index != NONE)
	{
		/* (in the frame of the node that moves the surface it is on now) */
		struct object_mesh const *mesh = blood_object_skin(drip->object_index);

		if (mesh && VALID_INDEX(drip->triangle, mesh->triangle_count))
		{
			drip->node_index = blood_triangle_node(mesh, drip->triangle);
			matrix = blood_node_matrix(drip->object_index, drip->node_index, FALSE);
		}
		if (!matrix)
		{
			drip->active = FALSE;
			return;
		}
		blood_to_local(matrix, &next, &drip->position);
		blood_normal_to_local(matrix, &normal, &drip->normal);
		blood_to_local(matrix, &laid_world, &drip->laid);
	}
	else
	{
		drip->position = next;
		drip->normal = normal;
		drip->laid = laid_world;
	}
}

/* ---------- public code */

static void blood_spots_reset(void);
static void blood_falling_drops_reset(void);


/* ---------- plasma embers on vehicles */

/* port: a plasma bolt that struck a vehicle leaves sparks and smoke where it
hit, riding with the vehicle, for 1 to 5 seconds: the plasma impact's own
rare lingering sparks and smoke (an event of its effect the game plays on a
few hits only), and on 1 shot in 10 the charged bolt's crackling sparks too.
Purely effects drawn: no damage, no physics. */

#define BLOOD_MAXIMUM_EMBERS 24
#define BLOOD_MAXIMUM_FORCED_EFFECTS 32

struct blood_ember
{
	boolean active;
	long object_index;
	short node_index;
	short event_index; /* the effect's event it plays alone; NONE: the whole effect */
	boolean blue; /* a plasma rifle's: its sparks blue */
	byte pad_blue[3];
	long definition_index;
	long end_time;
	long next_time;
	long effects[6]; /* (the ones it made, stopped as it ends) */
	short effect_next;
	short pad_effects;
	real_point3d points[8]; /* (node space) */
	real_vector3d forwards[8];
};

static struct blood_ember blood_embers[BLOOD_MAXIMUM_EMBERS];
static real blood_plasma_embers = 1.f;
static long blood_ember_effect = NONE;
static boolean blood_ember_looked = FALSE;
static short blood_ember_count = 5;
/* (the effects embers make: one event of its definition alone, or the
whole; smaller; blue) */
static short blood_forced_next_event = NONE;
static boolean blood_ember_spawning = FALSE;
static boolean blood_ember_spawning_blue = FALSE;
static struct
{
	long effect_index; /* NONE: free */
	void const *effect;
	short event_index; /* NONE: the whole effect */
	boolean blue;
	byte pad;
} blood_forced_effects[BLOOD_MAXIMUM_FORCED_EFFECTS];
static short blood_forced_next_slot = 0;
static real blood_plasma_ember_scale = 0.3125f;

static long blood_ember_definition(void)
{
	static char const *const names[] =
	{
		"weapons\\plasma rifle\\effects\\charged bolt sparks",
		"weapons\\plasma pistol\\effects\\overheated",
	};
	short index;

	if (!blood_ember_looked)
	{
		blood_ember_looked = TRUE;
		for (index = 0; blood_ember_effect == NONE && index < (short)NUMBEROF(names); index++)
			blood_ember_effect = tag_loaded(EFFECT_DEFINITION_TAG, names[index]);
	}
	return blood_ember_effect;
}

/* the charged bolt's impact on metal: its second event, two seconds of
sparks and smoke together, where an overcharged bolt struck */
static long blood_linger_effect = NONE;
static boolean blood_linger_looked = FALSE;

static long blood_linger_definition(void)
{
	if (!blood_linger_looked)
	{
		static char const *const names[] =
		{
			"weapons\\plasma rifle\\effects\\charged bolt impact metal",
			"weapons\\plasma rifle\\effects\\charged bolt impact stone",
			"weapons\\plasma rifle\\effects\\charged bolt impact dirt",
		};
		short index;

		blood_linger_looked = TRUE;
		for (index = 0; blood_linger_effect == NONE && index < (short)NUMBEROF(names); index++)
		{
			long definition_index = tag_loaded(EFFECT_DEFINITION_TAG, names[index]);
			struct effect_definition const *definition = definition_index != NONE ?
				(struct effect_definition const *)tag_get(EFFECT_DEFINITION_TAG, definition_index) : NULL;

			if (definition && definition->events.count >= 2)
				blood_linger_effect = definition_index;
		}
	}
	return blood_linger_effect;
}

short blood_effect_rare_event(
	long definition_index)
{
	struct effect_definition const *definition;
	short best = NONE;
	real best_skip = 0.f;
	short index;

	if (definition_index == NONE)
		return NONE;
	definition = (struct effect_definition const *)tag_get(EFFECT_DEFINITION_TAG, definition_index);
	if (!definition || !definition->events.address)
		return NONE;
	for (index = 1; index < (short)definition->events.count; index++)
	{
		struct effect_event_definition const *event =
			TAG_BLOCK_GET_ELEMENT(&definition->events, index, struct effect_event_definition);

		if (event->skip_fraction > best_skip && event->skip_fraction < 1.f &&
			(event->particles.count > 0 || event->parts.count > 0))
		{
			best = index;
			best_skip = event->skip_fraction;
		}
	}
	return best;
}

short blood_effect_first_event(
	long effect_index,
	void const *effect)
{
	short event_index = blood_forced_next_event;
	short slot;

	if (!blood_ember_spawning)
		return 0;
	slot = blood_forced_next_slot;
	blood_forced_next_slot = (short)((blood_forced_next_slot + 1) % BLOOD_MAXIMUM_FORCED_EFFECTS);
	blood_forced_effects[slot].effect_index = effect_index;
	blood_forced_effects[slot].effect = effect;
	blood_forced_effects[slot].event_index = event_index;
	blood_forced_effects[slot].blue = blood_ember_spawning_blue;
	return event_index != NONE ? event_index : 0;
}

void blood_effect_deleted(
	long effect_index)
{
	short index;

	for (index = 0; index < BLOOD_MAXIMUM_FORCED_EFFECTS; index++)
	{
		if (blood_forced_effects[index].effect_index == effect_index)
			blood_forced_effects[index].effect_index = NONE;
	}
}

real blood_effect_event_skip(
	long effect_index,
	short event_index,
	short event_count,
	real skip_fraction)
{
	short index;

	(void)event_index;
	(void)event_count;
	for (index = 0; index < BLOOD_MAXIMUM_FORCED_EFFECTS; index++)
	{
		/* (its one event played: every later one skipped, and it ends) */
		if (blood_forced_effects[index].effect_index == effect_index && blood_forced_effects[index].event_index != NONE)
			return 2.f;
	}
	return skip_fraction;
}

void blood_effect_particle(
	void const *effect,
	real_point3d const *origin,
	real_vector3d const *carried,
	real_point3d *position,
	real_vector3d *velocity,
	real *radius,
	real_rgb_color *color)
{
	short index;

	if (!effect)
		return;
	for (index = 0; index < BLOOD_MAXIMUM_FORCED_EFFECTS; index++)
	{
		if (blood_forced_effects[index].effect_index != NONE && blood_forced_effects[index].effect == effect)
		{
			real scale = PIN(blood_plasma_ember_scale, 0.05f, 2.f);

			/* (smaller: nearer its origin, slower, its sprites smaller) */
			position->x = origin->x + (position->x - origin->x) * scale;
			position->y = origin->y + (position->y - origin->y) * scale;
			position->z = origin->z + (position->z - origin->z) * scale;
			velocity->i = carried->i + (velocity->i - carried->i) * scale;
			velocity->j = carried->j + (velocity->j - carried->j) * scale;
			velocity->k = carried->k + (velocity->k - carried->k) * scale;
			*radius *= scale;
			/* (a plasma rifle's: its green made the light blue of the rifle's
			bolts, as bright, a little brighter) */
			if (blood_forced_effects[index].blue)
			{
				real bright = MAX(color->red, MAX(color->green, color->blue));

				bright = MIN(bright * 1.3f, 1.f);
				color->red = 0.55f * bright;
				color->green = 0.85f * bright;
				color->blue = bright;
			}
			return;
		}
	}
}

static void blood_ember_start(
	long object_index,
	short node_index,
	long definition_index,
	short event_index,
	boolean blue,
	short marker_count,
	real_point3d const *marker_points,
	real_vector3d const *marker_forwards)
{
	struct blood_ember *ember = NULL;
	real_matrix4x3 const *matrix = object_get_node_matrix(object_index, node_index);
	short index;

	if (!matrix)
		return;
	for (index = 0; index < BLOOD_MAXIMUM_EMBERS && !ember; index++)
		if (!blood_embers[index].active)
			ember = &blood_embers[index];
	if (!ember)
	{
		/* (all burning: the one nearest its end gives way) */
		ember = &blood_embers[0];
		for (index = 1; index < BLOOD_MAXIMUM_EMBERS; index++)
			if (blood_embers[index].end_time < ember->end_time)
				ember = &blood_embers[index];
	}
	ember->active = TRUE;
	ember->object_index = object_index;
	ember->node_index = node_index;
	ember->definition_index = definition_index;
	ember->event_index = event_index;
	ember->blue = blue;
	ember->next_time = game_time_get();
	for (index = 0; index < (short)NUMBEROF(ember->effects); index++)
		ember->effects[index] = NONE;
	ember->effect_next = 0;
	ember->end_time = ember->next_time + (long)(real_local_random_range(1.f, 5.f) * TICKS_PER_SECOND);
	for (index = 0; index < marker_count; index++)
	{
		matrix4x3_inverse_transform_point(matrix, &marker_points[index], &ember->points[index]);
		matrix4x3_inverse_transform_vector(matrix, &marker_forwards[index], &ember->forwards[index]);
	}
	blood_ember_count = marker_count;
}

void blood_plasma_struck_vehicle(
	long object_index,
	short node_index,
	boolean blue,
	short marker_count,
	real_point3d const *marker_points,
	real_vector3d const *marker_forwards)
{
	if (blood_plasma_embers < 0.5f || !blood_effects_enabled() || marker_count <= 0 || marker_count > 8)
		return;
	if (node_index == NONE)
		node_index = 0;
	/* (the charged bolt's lingering sparks and smoke: every hit) */
	if (blood_linger_definition() != NONE)
	{
		blood_ember_start(object_index, node_index, blood_linger_definition(), 1, blue,
			marker_count, marker_points, marker_forwards);
	}
	/* (the charged bolt's crackle: 1 shot in 10) */
	if (blood_ember_definition() != NONE && local_random_range(0, 10) == 0)
	{
		blood_ember_start(object_index, node_index, blood_ember_definition(), NONE, blue,
			marker_count, marker_points, marker_forwards);
	}
}

static void blood_ember_stop(
	struct blood_ember *ember)
{
	short index;

	/* (its sparks and smoke stop coming; those already out fade as ever) */
	if (ember->event_index != NONE)
	{
		for (index = 0; index < (short)NUMBEROF(ember->effects); index++)
		{
			if (ember->effects[index] != NONE)
				effect_delete(ember->effects[index]);
			ember->effects[index] = NONE;
		}
	}
	ember->active = FALSE;
}

static void blood_embers_update(long now)
{
	static char const *marker_names[5] = { "normal", "incident", "negative incident", "reflection", "gravity" };
	real_point3d points[8];
	real_vector3d forwards[8];
	short index, marker;

	for (index = 0; index < BLOOD_MAXIMUM_EMBERS; index++)
	{
		struct blood_ember *ember = &blood_embers[index];
		real_matrix4x3 const *matrix;

		if (!ember->active)
			continue;
		if (now >= ember->end_time || blood_plasma_embers < 0.5f || !blood_effects_enabled() ||
			!object_try_and_get(ember->object_index) || ember->definition_index == NONE)
		{
			blood_ember_stop(ember);
			continue;
		}
		if (now < ember->next_time)
			continue;
		matrix = object_get_node_matrix(ember->object_index, ember->node_index);
		if (!matrix)
		{
			ember->active = FALSE;
			continue;
		}
		for (marker = 0; marker < blood_ember_count; marker++)
		{
			matrix4x3_transform_point(matrix, &ember->points[marker], &points[marker]);
			matrix4x3_transform_vector(matrix, &ember->forwards[marker], &forwards[marker]);
		}
		{
			long effect_index;

			blood_forced_next_event = ember->event_index;
			blood_ember_spawning = TRUE;
			blood_ember_spawning_blue = ember->blue;
			effect_index = effect_new_attached_from_markers(ember->definition_index, NONE, ember->object_index,
				ember->node_index, blood_ember_count, marker_names, points, forwards, 1.f, 1.f, NULL, NULL);
			blood_forced_next_event = NONE;
			blood_ember_spawning = FALSE;
			ember->effects[ember->effect_next] = effect_index;
			ember->effect_next = (short)((ember->effect_next + 1) % (short)NUMBEROF(ember->effects));
		}
		/* (the sparks and smoke last two seconds a time: again before they
		end, until the ember does; the crackle every quarter to half second) */
		ember->next_time = now + (long)(real_local_random_range(ember->event_index != NONE ? 1.5f : 0.25f,
			ember->event_index != NONE ? 1.8f : 0.5f) * TICKS_PER_SECOND) + 1;
	}
}

void blood_initialize_for_new_map(
	void)
{
	short index;

	for (index = 0; index < BLOOD_MAXIMUM_KINDS; index++)
		blood_kinds[index].biped_definition_index = NONE;
	for (index = 0; index < BLOOD_MAXIMUM_VEHICLE_DECALS; index++)
		blood_vehicle_decals[index].decal_index = NONE;
	for (index = 0; index < BLOOD_MAXIMUM_DRIPS; index++)
		blood_drips[index].active = FALSE;
	for (index = 0; index < BLOOD_MAXIMUM_EMBERS; index++)
		blood_embers[index].active = FALSE;
	blood_ember_effect = NONE;
	blood_ember_looked = FALSE;
	blood_linger_effect = NONE;
	blood_linger_looked = FALSE;
	blood_forced_next_event = NONE;
	for (index = 0; index < BLOOD_MAXIMUM_FORCED_EFFECTS; index++)
		blood_forced_effects[index].effect_index = NONE;
	for (index = 0; index < BLOOD_MAXIMUM_RECENT_HITS; index++)
		blood_recent_hits[index].biped_index = NONE;
	blood_last_time = NONE;
	blood_checked_definition = NONE;
	blood_spots_reset();
	blood_falling_drops_reset();
	blood_skin_reset();
}

/* (0: run-over blood as the game makes it; 1: more by the vehicle's speed) */
static real blood_run_over_extra = 0.f;

/* bodies a vehicle drives through that no ragdoll has (Death Physics
Classic, or a body dying as the game has it): blood as the ragdolls' contacts
give it, from where the body lies inside the vehicle's bounds */
static void blood_run_over_bodies(void)
{
	struct object_iterator vehicles;

	object_iterator_new(&vehicles, _object_mask_vehicle, 0);
	while (object_iterator_next(&vehicles))
	{
		struct object_datum *vehicle = object_get(vehicles.index);
		real speed = (real)sqrt(blood_dot(&vehicle->object.translational_velocity, &vehicle->object.translational_velocity));
		struct object_iterator bipeds;

		if (speed * 30.f * BLOOD_METRES < 0.8f || !valid_real(speed))
			continue;
		object_iterator_new(&bipeds, _object_mask_biped, 0);
		while (object_iterator_next(&bipeds))
		{
			struct biped_datum *biped = biped_get(bipeds.index);
			real_vector3d off;
			real reach = vehicle->object.bounding_sphere_radius * 0.85f;

			if (!TEST_FLAG(biped->object.damage_flags, _object_dead_bit) ||
#ifdef BIPED_PORT_RAGDOLL_BIT
				TEST_FLAG(biped->biped.flags, BIPED_PORT_RAGDOLL_BIT) ||
#endif
				biped->object.parent_object_index != NONE)
			{
				continue;
			}
			off.i = biped->object.bounding_sphere_center.x - vehicle->object.bounding_sphere_center.x;
			off.j = biped->object.bounding_sphere_center.y - vehicle->object.bounding_sphere_center.y;
			off.k = biped->object.bounding_sphere_center.z - vehicle->object.bounding_sphere_center.z;
			if (blood_dot(&off, &off) < reach * reach)
			{
				real_point3d point = biped->object.bounding_sphere_center;
				real_vector3d normal = off;

				if (blood_normalize(&normal) == 0.f)
					normal = *global_down3d;
				blood_vehicle_hit_body(bipeds.index, vehicles.index, &point, &normal, speed);
			}
		}
	}
}

/* ---------- falling drops (purely drawn: blood_drops.c draws them) */

enum
{
	BLOOD_MAXIMUM_FALLING_DROPS = 48,
};

static struct
{
	boolean active;
	long decal_definition_index;
	real_point3d previous;
	real_point3d position;
	real_vector3d velocity; /* world units a tick */
	real radius;
	short age;
} blood_falling_drops[BLOOD_MAXIMUM_FALLING_DROPS];

static long blood_nearest_object_along(real_point3d const *from, real_vector3d const *vector, long ignore,
	real_point3d *point, real_vector3d *normal, long *triangle);

static void blood_falling_drops_reset(void)
{
	short index;

	for (index = 0; index < BLOOD_MAXIMUM_FALLING_DROPS; index++)
		blood_falling_drops[index].active = FALSE;
}

/* a drop let go from a point (a run over an edge, an underside) */
static void blood_drop_fall(long decal_definition_index, real_point3d const *point, real radius)
{
	short index;

	if (!blood_drops_animated() || decal_definition_index == NONE || radius <= 0.f)
		return;
	for (index = 0; index < BLOOD_MAXIMUM_FALLING_DROPS; index++)
	{
		if (!blood_falling_drops[index].active)
		{
			blood_falling_drops[index].active = TRUE;
			blood_falling_drops[index].decal_definition_index = decal_definition_index;
			blood_falling_drops[index].previous = *point;
			blood_falling_drops[index].position = *point;
			blood_falling_drops[index].velocity.i = 0.f;
			blood_falling_drops[index].velocity.j = 0.f;
			blood_falling_drops[index].velocity.k = 0.f;
			blood_falling_drops[index].radius = PIN(radius, 0.004f, 0.03f);
			/* (a moment hanging, swelling, before it lets go) */
			blood_falling_drops[index].age = (short)-local_random_range(4, 12);
			return;
		}
	}
}

/* (a tick: hanging, then falling; where it lands, a small splat) */
static void blood_drop_update(short index)
{
	real_point3d point;
	real_vector3d normal;
	real_vector3d step;
	long triangle;
	long object_index;
	boolean landed = FALSE;

	blood_falling_drops[index].previous = blood_falling_drops[index].position;
	if (blood_falling_drops[index].age++ < 0)
		return;
	/* (gravity: about 9.8 m/s/s, in world units a tick each tick) */
	blood_falling_drops[index].velocity.k -= 9.8f / (BLOOD_METRES * 30.f * 30.f);
	step = blood_falling_drops[index].velocity;
	if (blood_falling_drops[index].age > 90)
	{
		blood_falling_drops[index].active = FALSE;
		return;
	}
	/* (the level first, then anything nearer) */
	{
		struct collision_result collision;

		match_assert("blood.c", __LINE__, global_current_collision_user_depth < MAXIMUM_COLLISION_USER_STACK_DEPTH);
		global_current_collision_users[global_current_collision_user_depth++] = _collision_user_decals;
		if (collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit),
			&blood_falling_drops[index].position, &step, NONE, &collision) &&
			collision.type == _collision_result_structure)
		{
			real_point3d above = collision.point;
			real_vector3d into;
			real definition_radius = decal_definition_radius(blood_falling_drops[index].decal_definition_index);

			landed = TRUE;
			blood_add(&above, &collision.plane.n, 0.05f);
			scale_vector3d(&collision.plane.n, -0.15f, &into);
			global_current_collision_user_depth--;
			if (definition_radius > 0.f)
			{
				blood_laying = TRUE;
				decal_new(blood_falling_drops[index].decal_definition_index, &above, &into,
					blood_falling_drops[index].radius * 1.6f / definition_radius, FALSE, NONE, NULL);
				blood_laying = FALSE;
			}
		}
		else
		{
			global_current_collision_user_depth--;
		}
	}
	if (!landed)
	{
		object_index = blood_nearest_object_along(&blood_falling_drops[index].position, &step, NONE, &point, &normal, &triangle);
		if (object_index != NONE)
		{
			landed = TRUE;
			blood_laying = TRUE;
			blood_paint_object(blood_falling_drops[index].decal_definition_index, object_index, &point, &normal, triangle,
				NULL, blood_falling_drops[index].radius * 1.4f, blood_falling_drops[index].radius * 1.4f);
			blood_laying = FALSE;
		}
	}
	if (landed)
	{
		blood_falling_drops[index].active = FALSE;
		return;
	}
	blood_add(&blood_falling_drops[index].position, &step, 1.f);
}

short blood_drops_for_drawing(
	real_point3d *positions,
	real_vector3d *directions,
	real *radii,
	long *decals,
	short maximum)
{
	real fraction = PIN(render_interpolation_fraction(), 0.f, 1.f);
	short count = 0;
	short index;

	if (!blood_drops_animated() || !blood_violence_allowed())
		return 0;
	/* (a run's head is led by its own bead, a decal like the run's, lit and
	coloured as it is: blood_drip_place; only drops in the air are drawn
	here) */
	/* (the falling ones: hanging, they swell; falling, they stretch along
	their way) */
	for (index = 0; index < BLOOD_MAXIMUM_FALLING_DROPS && count < maximum; index++)
	{
		if (!blood_falling_drops[index].active)
			continue;
		positions[count].x = blood_falling_drops[index].previous.x +
			(blood_falling_drops[index].position.x - blood_falling_drops[index].previous.x) * fraction;
		positions[count].y = blood_falling_drops[index].previous.y +
			(blood_falling_drops[index].position.y - blood_falling_drops[index].previous.y) * fraction;
		positions[count].z = blood_falling_drops[index].previous.z +
			(blood_falling_drops[index].position.z - blood_falling_drops[index].previous.z) * fraction;
		directions[count] = blood_falling_drops[index].velocity;
		radii[count] = blood_falling_drops[index].radius *
			(blood_falling_drops[index].age < 0 ? 0.8f + 0.04f * (12 + blood_falling_drops[index].age) : 1.f);
		decals[count] = blood_falling_drops[index].decal_definition_index;
		count++;
	}
	return count;
}

void blood_update(
	void)
{
	long now = game_time_get();
	long steps;
	short index;

	/* (a revert, a saved game loaded: the drips stop; blood on vehicles the
	saved game has still rides with them, and what came after it is gone,
	its decals no longer there: decal_move_polygons says so) */
	if (blood_last_time != NONE && now < blood_last_time)
	{
		for (index = 0; index < BLOOD_MAXIMUM_DRIPS; index++)
			blood_drips[index].active = FALSE;
		for (index = 0; index < BLOOD_MAXIMUM_FALLING_DROPS; index++)
			blood_falling_drops[index].active = FALSE;
		for (index = 0; index < BLOOD_MAXIMUM_EMBERS; index++)
			blood_embers[index].active = FALSE;
		blood_skin_reset();
		blood_last_time = now - 1;
	}
	/* (each tick once: decals_update runs every frame drawn, several a tick) */
	steps = blood_last_time == NONE ? 1 : now - blood_last_time;
	if (steps <= 0)
		return;
	blood_last_time = now;
	blood_embers_update(now);
	if (!blood_violence_allowed())
		return;
	for (steps = MIN(steps, 4); steps > 0; steps--)
	{
		for (index = 0; index < BLOOD_MAXIMUM_DRIPS; index++)
		{
			if (blood_drips[index].active)
				blood_drip_update(&blood_drips[index]);
		}
	}
	for (index = 0; index < BLOOD_MAXIMUM_FALLING_DROPS; index++)
	{
		if (blood_falling_drops[index].active)
			blood_drop_update(index);
	}
	if (blood_run_over_extra >= 0.5f)
		blood_run_over_bodies();
}

/* ---------- console */

struct blood_parameter_definition
{
	char const *name;
	real *value;
	real default_value;
	real minimum;
	real maximum;
	char const *description;
};

static struct blood_parameter_definition const blood_parameter_definitions[] =
{
	{ "amount", &blood_parameters.amount, 1.f, 0.f, 4.f, "times as much blood (and as large)" },
	{ "full_speed", &blood_parameters.full_speed, 15.f, 1.f, 60.f, "metres a second a vehicle bleeds a body fully at (slower bleeds less)" },
	{ "drips", &blood_parameters.drips, 1.f, 0.f, 1.f, "1: blood on walls, vehicles and characters runs down them" },
	{ "run_over", &blood_run_over_extra, 0.f, 0.f, 1.f, "1: bodies run over bleed more, by the vehicle's speed; 0: as the game has them bleed" },
	{ "stay_seconds", &blood_parameters.stay_seconds, 300.f, 0.f, 3600.f, "seconds blood on vehicles and characters lasts at least" },
	{ "drip_length", &blood_parameters.drip_length, 1.f, 0.f, 4.f, "times as long as blood runs" },
	{ "on_vehicles", &blood_on_vehicles, 1.6f, 0.f, 4.f, "times as large blood is on vehicles as on walls" },
	{ "drip_layers", &blood_drip_layers, 2.f, 1.f, 3.f, "times over a drip's run is laid: 1 lighter, 3 darkest" },
	{ "plasma_ember_scale", &blood_plasma_ember_scale, 0.3125f, 0.05f, 2.f, "times the size of plasma's sparks and smoke on vehicles: how large, how far they fly" },
	{ "plasma_embers", &blood_plasma_embers, 1.f, 0.f, 1.f, "1: plasma that hits a vehicle leaves sparks and smoke where it hit for 1 to 5 seconds (and 1 shot in 10, the charged bolt's crackle)" },
	{ "debug", &blood_debug, 0.f, 0.f, 1.f, "1: the console says what each decal meant for an object did" },
};

static char const *blood_skip_space(char const *text)
{
	while (*text == ' ' || *text == '\t' || *text == '(' || *text == ')' || *text == '"')
		text++;
	return text;
}

static short blood_read_word(char const **text, char *word, short size)
{
	short length = 0;

	*text = blood_skip_space(*text);
	while (**text && **text != ' ' && **text != '\t' && **text != ')' && **text != '"' && length < size - 1)
		word[length++] = (char)tolower((unsigned char)*(*text)++);
	word[length] = 0;
	return length;
}

long blood_console_command(
	char const *expression)
{
	char const *text = blood_skip_space(expression);
	char word[64];
	char value[64];
	short index;

	/* ("blood debug 1", or "blood_debug 1" as the game's own commands are
	spelled) */
	if (strncmp(text, "blood", 5) != 0 ||
		(text[5] && text[5] != ' ' && text[5] != '\t' && text[5] != ')' && text[5] != '_'))
	{
		return NONE;
	}
	text += text[5] == '_' ? 6 : 5;

	if (!blood_read_word(&text, word, sizeof(word)))
	{
		console_printf(FALSE, "blood effects %s. blood on|off; blood <name> <value> sets one; blood reset puts them back:",
			blood_effects_enabled() ? "on (dynamic)" : "off (classic)");
		for (index = 0; index < (short)NUMBEROF(blood_parameter_definitions); index++)
		{
			struct blood_parameter_definition const *definition = &blood_parameter_definitions[index];

			console_printf(FALSE, "blood %s %g  (%s)", definition->name, (double)*definition->value, definition->description);
		}
		return TRUE;
	}
	/* on and off: Video Settings' Blood Effects (config.toml's
	game.blood_effects), kept */
	if (!strcmp(word, "on") || !strcmp(word, "off") || !strcmp(word, "1") || !strcmp(word, "0") ||
		!strcmp(word, "true") || !strcmp(word, "false"))
	{
		boolean on = !strcmp(word, "on") || !strcmp(word, "1") || !strcmp(word, "true");

		config_write("game.blood_effects", on ? "dynamic" : "classic");
		console_printf(FALSE, "blood effects %s", blood_effects_enabled() ? "on (dynamic)" : "off (classic)");
		return TRUE;
	}
	if (!strcmp(word, "reset"))
	{
		for (index = 0; index < (short)NUMBEROF(blood_parameter_definitions); index++)
			*blood_parameter_definitions[index].value = blood_parameter_definitions[index].default_value;
		console_printf(FALSE, "blood parameters reset");
		return TRUE;
	}
	for (index = 0; index < (short)NUMBEROF(blood_parameter_definitions); index++)
	{
		struct blood_parameter_definition const *definition = &blood_parameter_definitions[index];

		if (strcmp(word, definition->name))
			continue;
		if (blood_read_word(&text, value, sizeof(value)))
		{
			char *end;
			double number = strtod(value, &end);

			if (end == value || *end)
			{
				console_printf(FALSE, "blood %s: %s is no number", definition->name, value);
				return FALSE;
			}
			*definition->value = PIN((real)number, definition->minimum, definition->maximum);
		}
		console_printf(FALSE, "blood %s %g  (%s)", definition->name, (double)*definition->value, definition->description);
		return TRUE;
	}
	console_printf(FALSE, "blood: no parameter %s (blood alone lists them)", word);
	return FALSE;
}

/* blood landing on an object where blood landed a moment ago adds nothing
(an elite's burst lands dozens of drops on one spot): skipped, to keep the
decal cache for blood that shows */
enum { BLOOD_RECENT_SPOTS = 64 };
static struct { long decal; long object_index; long time; real_point3d point; } blood_recent_spots[BLOOD_RECENT_SPOTS];
static short blood_recent_spot_next = 0;

/* where projectiles struck objects lately: the game's own impact effect
there may lay a bloody bullet hole, which an object gets plain */
enum { BLOOD_RECENT_IMPACTS = 16 };
static struct { long time; real_point3d point; } blood_recent_impacts[BLOOD_RECENT_IMPACTS];
static short blood_recent_impact_next = 0;

void blood_projectile_struck_object(
	real_point3d const *point)
{
	blood_recent_impacts[blood_recent_impact_next].time = game_time_get();
	blood_recent_impacts[blood_recent_impact_next].point = *point;
	blood_recent_impact_next = (short)((blood_recent_impact_next + 1) % BLOOD_RECENT_IMPACTS);
}

static boolean blood_near_recent_impact(real_point3d const *point)
{
	long now = game_time_get();
	short index;

	for (index = 0; index < BLOOD_RECENT_IMPACTS; index++)
	{
		if (blood_recent_impacts[index].time != NONE && now >= blood_recent_impacts[index].time &&
			now - blood_recent_impacts[index].time <= 2 && distance3d(&blood_recent_impacts[index].point, point) < 0.3f)
		{
			return TRUE;
		}
	}
	return FALSE;
}

static void blood_spots_reset(void)
{
	short index;

	for (index = 0; index < BLOOD_RECENT_SPOTS; index++)
		blood_recent_spots[index].object_index = NONE;
	for (index = 0; index < BLOOD_RECENT_IMPACTS; index++)
		blood_recent_impacts[index].time = NONE;
}

static boolean blood_spot_recent(long decal, long object_index, real_point3d const *point, real radius)
{
	long now = game_time_get();
	short index;

	for (index = 0; index < BLOOD_RECENT_SPOTS; index++)
	{
		if (blood_recent_spots[index].object_index == object_index && blood_recent_spots[index].decal == decal &&
			now - blood_recent_spots[index].time < 3 &&
			now >= blood_recent_spots[index].time &&
			distance3d(&blood_recent_spots[index].point, point) < MAX(radius * 0.4f, 0.01f))
		{
			return TRUE;
		}
	}
	blood_recent_spots[blood_recent_spot_next].decal = decal;
	blood_recent_spots[blood_recent_spot_next].object_index = object_index;
	blood_recent_spots[blood_recent_spot_next].time = now;
	blood_recent_spots[blood_recent_spot_next].point = *point;
	blood_recent_spot_next = (short)((blood_recent_spot_next + 1) % BLOOD_RECENT_SPOTS);
	return FALSE;
}

/* the nearest painted object's drawn surface along a segment */
static long blood_nearest_object_along(real_point3d const *from, real_vector3d const *vector, long ignore,
	real_point3d *point, real_vector3d *normal, long *triangle)
{
	struct object_iterator iterator;
	long best_object = NONE;
	real best = REAL_MAX;

	object_iterator_new(&iterator, BLOOD_PAINTED_OBJECTS, 0);
	while (object_iterator_next(&iterator))
	{
		real_point3d hit;
		real_vector3d hit_normal;
		long hit_triangle;

		if (iterator.index != ignore &&
			blood_object_raycast(iterator.index, from, vector, &hit, &hit_normal, &hit_triangle) &&
			distance3d(from, &hit) < best)
		{
			best = distance3d(from, &hit);
			best_object = iterator.index;
			*point = hit;
			*normal = hit_normal;
			*triangle = hit_triangle;
		}
	}
	return best_object;
}

/* the nearest body (alive or dead) within reach of a point whose blood is
known, or NONE */
static long blood_nearest_bleeding_body(real_point3d const *point, real reach)
{
	struct object_iterator iterator;
	long best = NONE;
	real best_distance = reach;

	object_iterator_new(&iterator, _object_mask_biped, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *body = object_try_and_get(iterator.index);
		struct blood_kind const *kind;
		real distance;

		if (!body)
			continue;
		distance = distance3d(point, &body->object.bounding_sphere_center);
		if (distance < best_distance && (kind = blood_kind_get(iterator.index)) != NULL && kind->splat != NONE)
		{
			best_distance = distance;
			best = iterator.index;
		}
	}
	return best;
}

/* the species' splat a blood particle's name says (its "elite", "grunt"...) */
static long blood_splat_for_name(char const *name)
{
	long splat = NONE;

	if (strstr(name, "elite") || strstr(name, "jackal"))
		splat = blood_splat_named("blood elite splat");
	else if (strstr(name, "grunt"))
		splat = blood_splat_named("blood grunt splat");
	else if (strstr(name, "hunter"))
		splat = blood_splat_named("blood hunter splat");
	else if (strstr(name, "engineer"))
		splat = blood_splat_named("blood engineer splat");
	if (splat == NONE)
		splat = blood_splat_named("blood splat");
	return splat;
}

boolean blood_splash_meets_object(
	long tag_index,
	real_point3d const *from,
	real_point3d const *to,
	real particle_radius)
{
	static long checked[8] = { NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE };
	static long checked_splat[8];
	static short next_checked = 0;
	real_vector3d vector;
	real_point3d point;
	real_vector3d normal;
	long triangle;
	long object_index;
	long splat;
	real radius;
	short index;

	if (tag_index == NONE || !blood_violence_allowed() || !from || !to)
		return FALSE;
	/* (blood's: "effects\particles\solid\blood elite burst"; its splat) */
	for (index = 0; index < 8 && checked[index] != tag_index; index++)
		;
	if (index < 8)
	{
		splat = checked_splat[index];
	}
	else
	{
		char const *name = tag_get_name(tag_index);

		splat = name && strstr(name, "blood") ? blood_splat_for_name(name) : NONE;
		checked[next_checked] = tag_index;
		checked_splat[next_checked] = splat;
		next_checked = (short)((next_checked + 1) % 8);
	}
	if (splat == NONE)
		return FALSE;
	vector.i = to->x - from->x;
	vector.j = to->y - from->y;
	vector.k = to->z - from->z;
	if (blood_dot(&vector, &vector) < 1e-10f || !valid_real(vector.i))
		return FALSE;
	/* (the species of the body it came from: the nearest that bleeds) */
	{
		long body = blood_nearest_bleeding_body(from, 3.f);
		struct blood_kind const *kind = body != NONE ? blood_kind_get(body) : NULL;

		if (kind && kind->splat != NONE)
			splat = kind->splat;
	}
	object_index = blood_nearest_object_along(from, &vector, NONE, &point, &normal, &triangle);
	if (object_index == NONE)
		return FALSE;
	radius = PIN(particle_radius * 3.f, 0.03f, 0.12f) * PIN(blood_parameters.amount, 0.f, 5.f) * real_local_random_range(0.7f, 1.f);
	if (object_try_and_get(object_index) && object_try_and_get(object_index)->object.type == _object_type_vehicle)
		radius *= PIN(blood_on_vehicles, 0.f, 4.f);
	if (radius > 0.f && !blood_spot_recent(splat, object_index, &point, radius) &&
		blood_paint_object(splat, object_index, &point, &normal, triangle, NULL, radius, radius))
	{
		struct object_mesh const *mesh = blood_object_skin(object_index);

		if (mesh && blood_parameters.drips >= 0.5f && fabs(normal.k) < BLOOD_STEEP && local_random_range(0, 3) == 0)
			blood_drip_start(splat, object_index, blood_triangle_node(mesh, triangle), triangle, &point, &normal, radius, NONE);
	}
	/* (it met the object: it ends there, painted or not) */
	return TRUE;
}

/* (a tag's name says blood: "effects\\blood collision elite"; remembered) */
static boolean blood_tag_is_blood(long tag_index)
{
	static long checked[16] = { NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE };
	static boolean checked_is_blood[16];
	static short next_checked = 0;
	boolean is_blood;
	short index;

	if (tag_index == NONE)
		return FALSE;
	for (index = 0; index < 16; index++)
	{
		if (checked[index] == tag_index)
			return checked_is_blood[index];
	}
	is_blood = blood_name_has(tag_index, "blood");
	checked[next_checked] = tag_index;
	checked_is_blood[next_checked] = is_blood;
	next_checked = (short)((next_checked + 1) % 16);
	return is_blood;
}

boolean blood_particle_meets_object(
	long collision_effect_index,
	long particle_definition_index,
	real_point3d const *from,
	real_point3d const *to,
	real_point3d *point,
	real_vector3d *normal)
{
	struct object_iterator iterator;
	real_vector3d vector;
	real best = REAL_MAX;
	boolean found = FALSE;

	if (!blood_violence_allowed() || !from || !to ||
		!(blood_tag_is_blood(collision_effect_index) || blood_tag_is_blood(particle_definition_index)))
	{
		return FALSE;
	}
	vector.i = to->x - from->x;
	vector.j = to->y - from->y;
	vector.k = to->z - from->z;
	if (blood_dot(&vector, &vector) < 1e-10f || !valid_real(vector.i))
		return FALSE;

	/* (vehicles, scenery, devices and dropped items: never characters) */
	object_iterator_new(&iterator, BLOOD_PAINTED_OBJECTS, 0);
	while (object_iterator_next(&iterator))
	{
		real_point3d hit;
		real_vector3d hit_normal;

		if (blood_object_raycast(iterator.index, from, &vector, &hit, &hit_normal, NULL))
		{
			real distance = distance3d(from, &hit);

			if (distance < best)
			{
				best = distance;
				*point = hit;
				*normal = hit_normal;
				found = TRUE;
			}
		}
	}
	return found;
}

short blood_decals_onto_object(
	long object_index,
	real_point3d const *point,
	real_vector3d const *normal,
	long const *decals,
	real const *radius_modifiers,
	short count)
{
	struct object_iterator iterator;
	struct object_datum *object;
	long best_object = NONE;
	real best_distance = REAL_MAX;
	real_point3d best_point;
	real_vector3d best_normal;
	long best_triangle = NONE;
	real_vector3d outward;
	short index;
	short painted = 0;

	if (blood_laying || !blood_violence_allowed() || !point || !normal || !decals || count <= 0 ||
		!valid_real(point->x) || !valid_real(normal->i))
	{
		return 0;
	}
	if (object_index != NONE)
	{
		object = object_try_and_get(object_index);
		if (!object || !(TEST_FLAG(BLOOD_PAINTED_OBJECTS, object->object.type) ||
			(blood_marks_on_characters && object->object.type == _object_type_biped)))
		{
			return 0;
		}
	}
	outward = *normal;
	if (blood_normalize(&outward) == 0.f)
		return 0;

	/* the drawn surface at the point: in along the normal from well outside
	it to well inside (the point is on the collision model; the drawn one can
	sit either side of it), its outer face the one seen */
	{
		real_point3d from = *point;
		real_vector3d ray = outward;

		blood_add(&from, &outward, BLOOD_DECAL_SLACK);
		scale_vector3d(&ray, -2.f * BLOOD_DECAL_SLACK, &ray);
		object_iterator_new(&iterator, BLOOD_PAINTED_OBJECTS | (blood_marks_on_characters ? _object_mask_biped : 0), 0);
		while (object_iterator_next(&iterator))
		{
			real_point3d hit;
			real_vector3d hit_normal;
			long triangle;

			if ((object_index == NONE || iterator.index == object_index) &&
				blood_object_raycast(iterator.index, &from, &ray, &hit, &hit_normal, &triangle) &&
				distance3d(point, &hit) < best_distance)
			{
				best_distance = distance3d(point, &hit);
				best_object = iterator.index;
				best_point = hit;
				best_normal = hit_normal;
				best_triangle = triangle;
			}
		}
	}
	if (best_object == NONE)
	{
		if (blood_debug >= 0.5f)
			blood_say("impact on object %ld: its drawn surface not found near", object_index);
		return 0;
	}
	object = object_try_and_get(best_object);
	for (index = 0; index < count; index++)
	{
		long decal = decals[index];
		boolean is_blood = blood_name_has(decal, "blood");
		real radius = decal_definition_radius(decal) * PIN(radius_modifiers ? radius_modifiers[index] : 1.f, 0.f, 4.f) *
			real_local_random_range(0.85f, 1.f) * (is_blood ? PIN(blood_parameters.amount, 0.f, 5.f) : 1.f);

		if (is_blood && object && object->object.type == _object_type_vehicle)
			radius *= PIN(blood_on_vehicles, 0.f, 4.f);
		if (object && object->object.type == _object_type_biped)
			radius *= BLOOD_CHARACTER_MARK_SCALE;
		blood_paint_min_facing = object && object->object.type == _object_type_biped ? 0.8f : is_blood ? 0.35f : 0.65f;
		if (radius <= 0.f)
			continue;
		if (blood_spot_recent(decal, best_object, &best_point, radius))
		{
			painted++;
			continue;
		}
		if (blood_paint_object(decal, best_object, &best_point, &best_normal, best_triangle, NULL, radius, radius))
		{
			struct object_mesh const *mesh = blood_object_skin(best_object);

			painted++;
			if (is_blood && mesh && blood_parameters.drips >= 0.5f && fabs(best_normal.k) < BLOOD_STEEP)
			{
				blood_drip_start(decal, best_object, blood_triangle_node(mesh, best_triangle), best_triangle,
					&best_point, &best_normal, radius, NONE);
			}
		}
		if (blood_debug >= 0.5f)
			blood_say("impact %s on object %ld: %s", tag_get_name(decal), best_object, painted ? "painted" : "nothing painted");
	}
	blood_paint_min_facing = 0.35f;
	return painted;
}

void blood_note_armor_hit(
	long object_index,
	short material,
	real shield,
	short painted)
{
	/* (the first few a map, to debug.txt, always: what the armor marks did) */
	static long noted = 0;
	static long map_time = NONE;
	long now = game_time_get();

	if (map_time == NONE || now < map_time)
		noted = 0;
	map_time = now;
	if ((painted < 0 || noted >= 24) && blood_debug < 0.5f)
		return;
	noted++;
	error(_error_silent, "blood: Spartan hit (object %ld, material %d, shield %.2f): %s; mesh: %s", object_index, material,
		(double)shield, painted < 0 ? "shielded, no mark" : painted > 0 ? "marked" : "no mark made", object_mesh_report(object_index));
}

void blood_allow_character_marks(
	boolean allow)
{
	blood_marks_on_characters = allow;
}

boolean blood_decal_on_two_sided(
	long decal_definition_index)
{
	/* (blood, bullet holes, burns: every mark the level's other surfaces get) */
	return blood_violence_allowed() && decal_definition_index != NONE;
}

void blood_debug_impact(
	struct collision_result const *collision)
{
	if (blood_debug < 0.5f || !collision)
		return;
	if (collision->type == _collision_result_object)
	{
		struct object_datum *object = object_try_and_get(collision->object_index);

		blood_say("hit object %ld type %d (%s); %s; mesh: %s", collision->object_index,
			object ? object->object.type : -1, object ? tag_get_name(object->definition_index) : "?",
			object && TEST_FLAG(BLOOD_PAINTED_OBJECTS, object->object.type) ? "paintable" : "not painted (a character)",
			object ? object_mesh_report(collision->object_index) : "-");
	}
	else if (collision->type == _collision_result_structure)
	{
		struct collision_bsp *bsp = global_collision_bsp_get();
		int flags = -1;

		if (bsp && VALID_INDEX(collision->surface_index, bsp->surfaces.count))
			flags = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, collision->surface_index, struct collision_surface)->flags;
		blood_say("hit the level: surface %ld, flags %d (1 two-sided, 2 invisible, 4 climbable, 8 breakable), material %d",
			collision->surface_index, flags, collision->material_type);
	}
}

boolean blood_decal_onto_object(
	long decal_definition_index,
	real_point3d const *origin,
	real_vector3d const *vector,
	real radius_modifier)
{
	struct object_iterator iterator;
	long best_object = NONE;
	real best_distance = REAL_MAX;
	real_point3d best_point;
	real_vector3d best_normal;
	long best_triangle = NONE;
	real radius;
	real_vector3d reach;

	if (blood_laying || !blood_violence_allowed() || decal_definition_index == NONE || !origin || !vector ||
		!valid_real(vector->i) || blood_dot(vector, vector) < 1e-8f)
	{
		return FALSE;
	}
	if (decal_definition_index != blood_checked_definition)
	{
		blood_checked_definition = decal_definition_index;
		blood_checked_is_blood = blood_name_has(decal_definition_index, "blood");
	}
	/* (a projectile's impact on an object: its hole plain, painted by
	projectiles.c; the bloody variant its effect lays, nowhere) */
	if (blood_checked_is_blood && blood_near_recent_impact(origin))
		return TRUE;
	/* (every decal the level would get: blood, and bullet holes and scorch
	marks too, whose look the impact's own effect chose: a human bullet's hole,
	a plasma bolt's burn; only blood drips) */

	/* the object surface nearest the decal's point along its line, either way:
	an impact's point is on the object's collision model, which its drawn
	model can sit a little outside or inside of, and an impact effect's decal
	may be thrown along the surface's normal (out of it) rather than into it */
	{
		real length = (real)sqrt(blood_dot(vector, vector));
		real_vector3d direction;
		short pass;

		direction = *vector;
		scale_vector3d(&direction, 1.f / length, &direction);
		reach = direction;
		scale_vector3d(&reach, MAX(length, 0.1f) + BLOOD_DECAL_SLACK, &reach);
		for (pass = 0; pass < 2; pass++)
		{
			real_point3d from = *origin;
			real_vector3d ray;

			if (pass == 0)
			{
				/* (from a little short of the point, into the surface) */
				blood_add(&from, &direction, -BLOOD_DECAL_SLACK);
				ray = reach;
			}
			else
			{
				/* (from a little past it, back: a surface the decal was thrown
				away from) */
				blood_add(&from, &direction, BLOOD_DECAL_SLACK);
				ray = direction;
				scale_vector3d(&ray, -2.f * BLOOD_DECAL_SLACK, &ray);
			}
			object_iterator_new(&iterator, BLOOD_PAINTED_OBJECTS, 0);
			while (object_iterator_next(&iterator))
			{
				real_point3d point;
				real_vector3d normal;
				long triangle;

				if (blood_object_raycast(iterator.index, &from, &ray, &point, &normal, &triangle))
				{
					real distance = distance3d(origin, &point);

					if (distance < best_distance)
					{
						best_distance = distance;
						best_object = iterator.index;
						best_point = point;
						best_normal = normal;
						best_triangle = triangle;
					}
				}
			}
		}
	}
	if (best_object == NONE)
	{
		if (blood_debug >= 0.5f)
		return FALSE;
	}

	/* (unless the level is nearer) */
	{
		struct collision_result collision;
		boolean level;

		match_assert("blood.c", __LINE__, global_current_collision_user_depth < MAXIMUM_COLLISION_USER_STACK_DEPTH);
		global_current_collision_users[global_current_collision_user_depth++] = _collision_user_decals;
		level = collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit),
			origin, vector, NONE, &collision) && collision.type == _collision_result_structure;
		global_current_collision_user_depth--;
		if (level && distance3d(origin, &collision.point) < best_distance)
		{
			if (blood_debug >= 0.5f)
				blood_say("decal %s: the level is nearer than object %ld", tag_get_name(decal_definition_index), best_object);
			return FALSE;
		}
	}

	radius = decal_definition_radius(decal_definition_index) * PIN(radius_modifier, 0.f, 4.f) *
		real_local_random_range(0.85f, 1.f) * PIN(blood_parameters.amount, 0.f, 5.f);
	/* (blood clings to vehicles: heavier there) */
	if (blood_checked_is_blood && object_try_and_get(best_object) &&
		object_try_and_get(best_object)->object.type == _object_type_vehicle)
	{
		radius *= PIN(blood_on_vehicles, 0.f, 4.f);
	}
	if (radius > 0.f && blood_spot_recent(decal_definition_index, best_object, &best_point, radius))
		return TRUE;
	if (radius <= 0.f ||
		!blood_paint_object(decal_definition_index, best_object, &best_point, &best_normal, best_triangle, NULL, radius, radius))
	{
		if (blood_debug >= 0.5f)
			blood_say("decal %s: object %ld met, %s", tag_get_name(decal_definition_index), best_object,
				blood_object_skin(best_object) ? "but nothing painted" : "but its model can't be read");
		return FALSE;
	}
	if (blood_debug >= 0.5f)
		blood_say("decal %s: painted object %ld (radius %.2f)", tag_get_name(decal_definition_index), best_object, (double)radius);
	{
		struct object_mesh const *mesh = blood_object_skin(best_object);

		if (mesh && blood_checked_is_blood && blood_parameters.drips >= 0.5f && fabs(best_normal.k) < BLOOD_STEEP)
		{
			blood_drip_start(decal_definition_index, best_object, blood_triangle_node(mesh, best_triangle), best_triangle,
				&best_point, &best_normal, radius, NONE);
		}
	}
	return TRUE;
}

void blood_vehicle_hit_body(
	long biped_index,
	long vehicle_index,
	real_point3d const *point,
	real_vector3d const *normal,
	real speed)
{
	struct blood_kind const *kind;
	struct object_datum *vehicle = object_try_and_get(vehicle_index);
	real metres_per_second = speed * 30.f * BLOOD_METRES;
	real full = MAX(blood_parameters.full_speed, 1.f);
	real share;
	real amount = PIN(blood_parameters.amount, 0.f, 5.f);
	long now = game_time_get();
	struct blood_recent_hit *recent = NULL;
	struct blood_recent_hit *oldest = &blood_recent_hits[0];
	real_vector3d heading;
	short index;
	short count;

	boolean extra = blood_run_over_extra >= 0.5f;

	/* (the game's own run-over blood is untouched; this paints the vehicle
	that did it: its front, its wheels, its underside; with run_over 1, more
	blood on the ground and sprays too) */
	if (!vehicle || !blood_violence_allowed() || amount <= 0.f || !valid_real(speed))
		return;
	/* (from a trickle at walking pace to all of it at full_speed) */
	share = PIN((metres_per_second - 0.8f) / (full - 0.8f), 0.f, 1.f);
	if (share <= 0.f)
		return;
	kind = blood_kind_get(biped_index);
	if (!kind || (kind->spray == NONE && kind->splat == NONE))
		return;

	/* (not every tick a vehicle touches a body: sooner the faster) */
	for (index = 0; index < BLOOD_MAXIMUM_RECENT_HITS; index++)
	{
		struct blood_recent_hit *hit = &blood_recent_hits[index];

		if (hit->biped_index == biped_index && hit->vehicle_index == vehicle_index)
			recent = hit;
		if (hit->biped_index == NONE || hit->time < oldest->time)
			oldest = hit;
	}
	/* (once a second at most: the blood a hit gives is all in its first
	burst, as hard as it was; a harder hit than the last bleeds again at once) */
	if (recent && now - recent->time < 30 && now >= recent->time &&
		share < recent->share + 0.2f)
	{
		return;
	}
	/* (a harder hit soon after: only what it adds) */
	if (recent && now - recent->time < 30 && now >= recent->time)
		amount *= PIN((share - recent->share) / share, 0.f, 1.f);
	if (!recent)
		recent = oldest;
	recent->biped_index = biped_index;
	recent->vehicle_index = vehicle_index;
	recent->time = now;
	recent->share = share;

	heading = vehicle->object.translational_velocity;
	if (blood_normalize(&heading) == 0.f)
		heading = *normal;

	/* on the ground under it, smeared along the vehicle's way */
	if (extra && kind->splat != NONE)
	{
		count = (short)(1.f + 5.f * share * amount);
		for (index = 0; index < count; index++)
		{
			real_point3d origin = *point;
			real_vector3d down = { 0.f, 0.f, -0.6f };
			long splat = share > 0.6f && local_random_range(0, 2) ? kind->splat_large : kind->splat;

			blood_add(&origin, &heading, real_local_random_range(-0.05f, 0.35f) * share);
			origin.x += real_local_random_range(-0.06f, 0.06f);
			origin.y += real_local_random_range(-0.06f, 0.06f);
			origin.z += 0.15f;
			/* (for the ground: from under the vehicle, not its inside) */
			blood_laying = TRUE;
			decal_new(splat, &origin, &down, (0.45f + 0.9f * share) * amount, FALSE, NONE, NULL);
			blood_laying = FALSE;
		}
	}

	/* its spray */
	if (extra && kind->spray != NONE && (share > 0.25f || real_local_random_range(0.f, 1.f) < share * 3.f))
	{
		real_point3d at = *point;
		real_vector3d incident;

		blood_add(&at, normal, 0.05f);
		scale_vector3d(&heading, -1.f, &incident);
		count = share > 0.6f ? (short)(1.f + amount) : 1;
		for (index = 0; index < count; index++)
			blood_spray(kind->spray, &at, normal, &incident);
	}

	/* on the vehicle, where it met the body */
	if (kind->splat != NONE)
	{
		real radius = decal_definition_radius(kind->splat) * (0.3f + 1.f * share) * amount * PIN(blood_on_vehicles, 0.f, 4.f);
		real_vector3d tangent;
		real_vector3d other;

		perpendicular3d(normal, &tangent);
		blood_normalize(&tangent);
		other.i = normal->j * tangent.k - normal->k * tangent.j;
		other.j = normal->k * tangent.i - normal->i * tangent.k;
		other.k = normal->i * tangent.j - normal->j * tangent.i;
		count = (short)(1.f + 6.f * share * amount);
		for (index = 0; index < count; index++)
		{
			real_point3d near = *point;

			blood_add(&near, &tangent, real_local_random_range(-0.08f, 0.08f));
			blood_add(&near, &other, real_local_random_range(-0.08f, 0.08f));
			blood_vehicle_splat(local_random_range(0, 3) ? kind->splat : kind->splat_small, vehicle_index, &near,
				normal, &heading, radius * real_local_random_range(0.6f, 1.f));
		}

		/* (all around the body: whatever of the vehicle is near it, its
		wheels and underside as it goes over, its bumper as it hits) */
		{
			struct object_datum *body = object_try_and_get(biped_index);
			real_point3d center = body ? body->object.bounding_sphere_center : *point;
			short rays = (short)(2.f + 14.f * share * amount);

			for (index = 0; index < rays; index++)
			{
				real angle = real_local_random_range(0.f, 2.f * _pi);
				real_vector3d ray = { (real)cos(angle), (real)sin(angle), real_local_random_range(-0.3f, 0.3f) };
				real_point3d from = center;

				from.z += real_local_random_range(-0.1f, 0.15f);
				blood_normalize(&ray);
				scale_vector3d(&ray, 0.9f, &ray);
				blood_paint_along(local_random_range(0, 2) ? kind->splat : kind->splat_small, vehicle_index, &from, &ray,
					radius * real_local_random_range(0.5f, 0.9f), -0.5f);
			}
		}
	}
}

void blood_biped_damaged(
	long biped_index,
	struct damage_data const *damage,
	real damage_amount)
{
	struct blood_kind const *kind;
	struct object_iterator iterator;
	real_vector3d ray;
	real_point3d origin;
	long nearest = NONE;
	real nearest_distance = REAL_MAX;

	if (!blood_violence_allowed() || !(damage_amount > 0.f) || blood_parameters.amount <= 0.f)
		return;
	ray = damage->direction;
	if (blood_normalize(&ray) == 0.f || !valid_real(damage->epicenter.x))
		return;
	/* (not every hit) */
	if (real_local_random_range(0.f, 1.f) > 0.6f)
		return;
	kind = blood_kind_get(biped_index);
	if (!kind || kind->splat == NONE)
		return;

	/* through the body, onto a vehicle behind it (its drawn surface) */
	origin = damage->epicenter;
	scale_vector3d(&ray, 1.2f, &ray);
	object_iterator_new(&iterator, BLOOD_PAINTED_OBJECTS, 0);
	while (object_iterator_next(&iterator))
	{
		real_point3d point;
		real_vector3d normal;

		if (iterator.index != biped_index &&
			blood_object_raycast(iterator.index, &origin, &ray, &point, &normal, NULL) &&
			distance3d(&origin, &point) < nearest_distance)
		{
			nearest_distance = distance3d(&origin, &point);
			nearest = iterator.index;
		}
	}
	if (nearest != NONE)
	{
		real share = PIN(damage_amount / 40.f, 0.25f, 1.f);
		real radius = decal_definition_radius(kind->splat_small) * (0.5f + 0.5f * share) * PIN(blood_parameters.amount, 0.f, 5.f);

		blood_paint_along(kind->splat_small, nearest, &origin, &ray, radius, -1.f);
	}
}

#endif // BLOOD_EFFECTS
