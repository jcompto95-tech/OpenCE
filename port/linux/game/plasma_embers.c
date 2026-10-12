/*
PLASMA_EMBERS.C

Projectile Effects (Video Settings', config.toml's game.projectile_effects):
a plasma bolt that strikes a vehicle, or a Spartan's bare armor, leaves sparks
and smoke where it hit, riding with what it hit, for 1 to 5 seconds: the
plasma impact's own rare lingering sparks and smoke (an event of its effect
the game plays on a few hits only), and on 1 shot in 10 the charged bolt's
crackling sparks too. Blue for the plasma rifle's, a Ghost's, a Banshee's
and a Wraith's bolts; the effects' own green for the plasma pistol's.
Purely effects drawn, each machine its own: no damage, no physics, nothing
sent.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "effects/effects.h"
#include "effects/effect_definitions.h"
#include "game/game.h"
#include "tag_files/tag_files.h"
#include "blood.h"
#include "plasma_embers.h"

#if BLOOD_EFFECTS

/* cache_files.c */
long tag_loaded(long group_tag, char const *name);

#define PLASMA_MAXIMUM_EMBERS 24
#define PLASMA_MAXIMUM_FORCED_EFFECTS 32

struct plasma_ember
{
	boolean active;
	long object_index;
	short node_index;
	short event_index; /* the effect's event it plays alone; NONE: the whole effect */
	boolean blue; /* its sparks blue (not the plasma pistol's) */
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

static struct plasma_ember plasma_embers[PLASMA_MAXIMUM_EMBERS];
static long plasma_ember_effect = NONE;
static boolean plasma_ember_looked = FALSE;
static short plasma_ember_count = 5;
/* (the effects embers make: one event of its definition alone, or the
whole; smaller; blue) */
static short plasma_forced_next_event = NONE;
static boolean plasma_ember_spawning = FALSE;
static boolean plasma_ember_spawning_blue = FALSE;
static struct
{
	long effect_index; /* NONE: free */
	void const *effect;
	short event_index; /* NONE: the whole effect */
	boolean blue;
	byte pad;
} plasma_forced_effects[PLASMA_MAXIMUM_FORCED_EFFECTS];
static short plasma_forced_next_slot = 0;
/* (how many of them are in use: none, and every effect the game makes is
left alone at once) */
static short plasma_forced_used = 0;
/* (how large its sparks and smoke are, how far they fly) */
#define PLASMA_EMBERS_SCALE 0.3125f

static long plasma_ember_definition(void)
{
	static char const *const names[] =
	{
		"weapons\\plasma rifle\\effects\\charged bolt sparks",
		"weapons\\plasma pistol\\effects\\overheated",
	};
	short index;

	if (!plasma_ember_looked)
	{
		plasma_ember_looked = TRUE;
		for (index = 0; plasma_ember_effect == NONE && index < (short)NUMBEROF(names); index++)
			plasma_ember_effect = tag_loaded(EFFECT_DEFINITION_TAG, names[index]);
	}
	return plasma_ember_effect;
}

/* the charged bolt's impact on metal: its second event, two seconds of
sparks and smoke together, where an overcharged bolt struck */
static long plasma_linger_effect = NONE;
static boolean plasma_linger_looked = FALSE;

static long plasma_linger_definition(void)
{
	if (!plasma_linger_looked)
	{
		static char const *const names[] =
		{
			"weapons\\plasma rifle\\effects\\charged bolt impact metal",
			"weapons\\plasma rifle\\effects\\charged bolt impact stone",
			"weapons\\plasma rifle\\effects\\charged bolt impact dirt",
		};
		short index;

		plasma_linger_looked = TRUE;
		for (index = 0; plasma_linger_effect == NONE && index < (short)NUMBEROF(names); index++)
		{
			long definition_index = tag_loaded(EFFECT_DEFINITION_TAG, names[index]);
			struct effect_definition const *definition = definition_index != NONE ?
				(struct effect_definition const *)tag_get(EFFECT_DEFINITION_TAG, definition_index) : NULL;

			if (definition && definition->events.count >= 2)
				plasma_linger_effect = definition_index;
		}
	}
	return plasma_linger_effect;
}

short plasma_embers_effect_first_event(
	long effect_index,
	void const *effect)
{
	short event_index = plasma_forced_next_event;
	short slot;

	if (!plasma_ember_spawning)
		return 0;
	slot = plasma_forced_next_slot;
	plasma_forced_next_slot = (short)((plasma_forced_next_slot + 1) % PLASMA_MAXIMUM_FORCED_EFFECTS);
	if (plasma_forced_effects[slot].effect_index == NONE)
		plasma_forced_used++;
	plasma_forced_effects[slot].effect_index = effect_index;
	plasma_forced_effects[slot].effect = effect;
	plasma_forced_effects[slot].event_index = event_index;
	plasma_forced_effects[slot].blue = plasma_ember_spawning_blue;
	return event_index != NONE ? event_index : 0;
}

void plasma_embers_effect_deleted(
	long effect_index)
{
	short index;

	if (plasma_forced_used <= 0)
		return;
	for (index = 0; index < PLASMA_MAXIMUM_FORCED_EFFECTS; index++)
	{
		if (plasma_forced_effects[index].effect_index == effect_index)
		{
			plasma_forced_effects[index].effect_index = NONE;
			plasma_forced_used--;
		}
	}
}

real plasma_embers_effect_event_skip(
	long effect_index,
	short event_index,
	short event_count,
	real skip_fraction)
{
	short index;

	(void)event_index;
	(void)event_count;
	if (plasma_forced_used <= 0)
		return skip_fraction;	for (index = 0; index < PLASMA_MAXIMUM_FORCED_EFFECTS; index++)
	{
		/* (its one event played: every later one skipped, and it ends) */
		if (plasma_forced_effects[index].effect_index == effect_index && plasma_forced_effects[index].event_index != NONE)
			return 2.f;
	}
	return skip_fraction;
}

void plasma_embers_effect_particle(
	void const *effect,
	real_point3d const *origin,
	real_vector3d const *carried,
	real_point3d *position,
	real_vector3d *velocity,
	real *radius,
	real_rgb_color *color)
{
	short index;

	if (!effect || plasma_forced_used <= 0)
		return;
	for (index = 0; index < PLASMA_MAXIMUM_FORCED_EFFECTS; index++)
	{
		if (plasma_forced_effects[index].effect_index != NONE && plasma_forced_effects[index].effect == effect)
		{
			real scale = PLASMA_EMBERS_SCALE;

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
			if (plasma_forced_effects[index].blue)
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

static void plasma_ember_start(
	long object_index,
	short node_index,
	long definition_index,
	short event_index,
	boolean blue,
	short marker_count,
	real_point3d const *marker_points,
	real_vector3d const *marker_forwards)
{
	struct plasma_ember *ember = NULL;
	real_matrix4x3 const *matrix = object_get_node_matrix(object_index, node_index);
	short index;

	if (!matrix)
		return;
	for (index = 0; index < PLASMA_MAXIMUM_EMBERS && !ember; index++)
		if (!plasma_embers[index].active)
			ember = &plasma_embers[index];
	if (!ember)
	{
		/* (all burning: the one nearest its end gives way) */
		ember = &plasma_embers[0];
		for (index = 1; index < PLASMA_MAXIMUM_EMBERS; index++)
			if (plasma_embers[index].end_time < ember->end_time)
				ember = &plasma_embers[index];
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
	plasma_ember_count = marker_count;
}

void plasma_embers_struck(
	long object_index,
	short node_index,
	boolean blue,
	short marker_count,
	real_point3d const *marker_points,
	real_vector3d const *marker_forwards)
{
	if (!blood_marks_allowed() || marker_count <= 0 || marker_count > 8)
		return;
	if (node_index == NONE)
		node_index = 0;
	/* (the charged bolt's lingering sparks and smoke: every hit) */
	if (plasma_linger_definition() != NONE)
	{
		plasma_ember_start(object_index, node_index, plasma_linger_definition(), 1, blue,
			marker_count, marker_points, marker_forwards);
	}
	/* (the charged bolt's crackle: 1 shot in 10) */
	if (plasma_ember_definition() != NONE && local_random_range(0, 10) == 0)
	{
		plasma_ember_start(object_index, node_index, plasma_ember_definition(), NONE, blue,
			marker_count, marker_points, marker_forwards);
	}
}

static void plasma_ember_stop(
	struct plasma_ember *ember)
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

void plasma_embers_update(
	long now)
{
	static char const *marker_names[5] = { "normal", "incident", "negative incident", "reflection", "gravity" };
	real_point3d points[8];
	real_vector3d forwards[8];
	short index, marker;

	for (index = 0; index < PLASMA_MAXIMUM_EMBERS; index++)
	{
		struct plasma_ember *ember = &plasma_embers[index];
		real_matrix4x3 const *matrix;

		if (!ember->active)
			continue;
		if (now >= ember->end_time || !blood_marks_allowed() ||
			!object_try_and_get(ember->object_index) || ember->definition_index == NONE)
		{
			plasma_ember_stop(ember);
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
		for (marker = 0; marker < plasma_ember_count; marker++)
		{
			matrix4x3_transform_point(matrix, &ember->points[marker], &points[marker]);
			matrix4x3_transform_vector(matrix, &ember->forwards[marker], &forwards[marker]);
		}
		{
			long effect_index;

			plasma_forced_next_event = ember->event_index;
			plasma_ember_spawning = TRUE;
			plasma_ember_spawning_blue = ember->blue;
			effect_index = effect_new_attached_from_markers(ember->definition_index, NONE, ember->object_index,
				ember->node_index, plasma_ember_count, marker_names, points, forwards, 1.f, 1.f, NULL, NULL);
			plasma_forced_next_event = NONE;
			plasma_ember_spawning = FALSE;
			ember->effects[ember->effect_next] = effect_index;
			ember->effect_next = (short)((ember->effect_next + 1) % (short)NUMBEROF(ember->effects));
		}
		/* (the sparks and smoke last two seconds a time: again before they
		end, until the ember does; the crackle every quarter to half second) */
		ember->next_time = now + (long)(real_local_random_range(ember->event_index != NONE ? 1.5f : 0.25f,
			ember->event_index != NONE ? 1.8f : 0.5f) * TICKS_PER_SECOND) + 1;
	}
}

void plasma_embers_initialize_for_new_map(
	void)
{
	short index;

	for (index = 0; index < PLASMA_MAXIMUM_EMBERS; index++)
		plasma_embers[index].active = FALSE;
	plasma_ember_effect = NONE;
	plasma_ember_looked = FALSE;
	plasma_linger_effect = NONE;
	plasma_linger_looked = FALSE;
	plasma_forced_next_event = NONE;
	for (index = 0; index < PLASMA_MAXIMUM_FORCED_EFFECTS; index++)
		plasma_forced_effects[index].effect_index = NONE;
	plasma_forced_used = 0;
}

#endif // BLOOD_EFFECTS
