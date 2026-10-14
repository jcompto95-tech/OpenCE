/*
PROJECTILE_LIGHTS.C

Projectiles that light what is around them (Projectile Effects,
game.projectile_effects): a plasma projectile's own light (a plasma rifle's
or pistol's bolt, a Ghost's, a Banshee's, a Covenant turret's, a Wraith's)
lights the level and the objects it passes, as an overcharged bolt's and a
plasma grenade's already do, in its own color and size; and a needle's pink
light too, dimmer, at less than half its color's strength (with its
definition's smaller size). The maps' definitions of those lights are not
dynamic: they are glows only, and one without a lens flare was not made at
all. light_new (object_lights.c) asks projectile_lights_dynamic as it makes
a light, and lights_preprocess_scene projectile_lights_color as it colors it.

Only the drawing changes: nothing the game simulates, nothing sent between
machines.
*/

#include "cseries.h"
#include "real_math.h"
#include "objects/objects.h"
#include "tag_files/tag_groups.h"

#include <string.h>

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
unsigned long config_changes(void);

/* how strong a needle's light is, of its color (a plasma bolt's is at full) */
#define PROJECTILE_LIGHTS_NEEDLE_STRENGTH 0.45f

/* the kind of projectile a light is on */
enum
{
	_projectile_light_none,
	_projectile_light_plasma,
	_projectile_light_needle,
};

/* the last projectile kind asked about, and its kind */
static long projectile_lights_definition = NONE;
static short projectile_lights_definition_kind = _projectile_light_none;

static boolean projectile_lights_on(
	void)
{
	static unsigned long changes = (unsigned long)-1;
	static boolean on = FALSE;

	if (changes != config_changes())
	{
		changes = config_changes();
		on = config_boolean("game.projectile_effects") != 0;
	}
	return on;
}

static short projectile_lights_kind(
	long object_index)
{
	struct object_datum *object = object_index != NONE ? object_try_and_get(object_index) : NULL;
	char const *name;

	if (!object || object->object.type != _object_type_projectile)
		return _projectile_light_none;
	if (object->definition_index == projectile_lights_definition)
		return projectile_lights_definition_kind;
	projectile_lights_definition = object->definition_index;
	name = tag_get_name(object->definition_index);
	projectile_lights_definition_kind = !name || strstr(name, "grenade") ? _projectile_light_none :
		strstr(name, "needle") ? _projectile_light_needle :
		strstr(name, "plasma") || strstr(name, "bolt") || strstr(name, "wraith") || strstr(name, "mortar") ?
			_projectile_light_plasma : _projectile_light_none;
	return projectile_lights_definition_kind;
}

/* (object_lights.c's light_new) whether a light an object carries, which
its definition does not make dynamic, lights the world: a plasma
projectile's, or a needle's */
boolean projectile_lights_dynamic(
	long definition_index,
	long object_index)
{
	(void)definition_index;
	return projectile_lights_on() && projectile_lights_kind(object_index) != _projectile_light_none;
}

/* (object_lights.c's lights_preprocess_scene) a light's color as it shines:
a needle's dimmer than a bolt's */
void projectile_lights_color(
	long object_index,
	real_rgb_color *color)
{
	if (!color || projectile_lights_kind(object_index) != _projectile_light_needle || !projectile_lights_on())
		return;
	color->red *= PROJECTILE_LIGHTS_NEEDLE_STRENGTH;
	color->green *= PROJECTILE_LIGHTS_NEEDLE_STRENGTH;
	color->blue *= PROJECTILE_LIGHTS_NEEDLE_STRENGTH;
}

/* a new map: its projectile kinds forgotten (objects.c) */
void projectile_lights_reset(
	void)
{
	projectile_lights_definition = NONE;
}
