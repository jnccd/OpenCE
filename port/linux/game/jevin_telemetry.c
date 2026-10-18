/*
jevin_TELEMETRY.C

Give an external process the game's state: where every object near a unit is,
which way it faces, how fast it moves, whose side it is on, and how hurt it is.

This is the port side of an AI teammate. The game's own script language can
ask for a unit's vitality and whether one object can see another, but it has
no way to say where anything is, so a bot driven through scripts alone is
blind in the one sense that matters for positioning. These commands fill that
gap: a plain dump of what the engine already knows, with no policy in it.

They are parsed before the script compiler, as profile_console's commands are
(hs.c): a map's compiled scripts index the script function table, so a port
command must not add an entry to it.

The console gives one line per object, space separated, so that a small
parser reads it without JSON:

	JEVIN b <count> <reference> <rx> <ry> <rz>
	JEVIN o <index> <type> <team> <x> <y> <z> <radius> <vx> <vy> <vz> \
	        <forward x> <forward y> <forward z> <body> <shield> <definition>
	JEVIN e

<type> is the engine's object type (0 biped, 1 vehicle, 2 weapon, 3
equipment, 4 scenery, 5 machine, 6 control, 7 light fixture, 8 placeholder,
9 sound scenery, 10 projectile). <definition> is the tag path and comes last
because it is the only field that can hold a space.

Distances are not computed here: the reader has the reference position and
does its own geometry, which keeps this a dump rather than a query.
*/

#include "cseries.h"
#include "ai/ai_scenario_definitions.h"
#include "game/players.h"
#include "game/game.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "main/console.h"
#include "ai/actors.h"
#include "collisions.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "unit_control_data.h"
#include "units/units.h"
#include "interface/terminal.h"
#include "networking/telnet_console.h"
#include "jevin_telemetry.h"

#include <stdarg.h>
#include <stdio.h>

/* interface/terminal.c: when the console's text is drawn over the game, 0/1/2 */
extern long terminal_output_mode;

/* port_config.c: a setting's stored value */
int config_boolean(char const *name);

/* port: whether this run wants cutscenes skipped as they start.

A test harness cannot press a key, and injecting seven space presses was the
workaround. With this set, the accept input - which is what skips a cutscene - is
held down for as long as one is playing (input_abstraction.c). */
int jevin_skip_cinematics(
	void)
{
	return config_boolean("game.skip_cinematics") != 0;
}

#include <string.h>

/* the most objects one dump reports, so that a busy level cannot flood the
console (a30 keeps well under a hundred near a player) */
#define MAXIMUM_TELEMETRY_OBJECTS 256

/* a reply for the controlling process only.

The console's output buffer is what gets drawn over the game, and this module
talks ten times a second - the terrain grid alone is fifty lines. A harness does
not need its own telemetry painted on screen, so the frequent replies go straight
to the telnet client and never reach the buffer. What a person asks for (`pos`,
the order ack, the draw toggle) still uses console_printf and stays visible. */
static void jevin_printf_quiet(
	char const *format,
	...)
{
	char buffer[TERMINAL_PRINTF_MAXIMUM_LINE_LENGTH];
	va_list arglist;

	va_start(arglist, format);
	_vsnprintf(buffer, NUMBEROF(buffer) - 2, format, arglist);
	va_end(arglist);
	buffer[NUMBEROF(buffer) - 2] = '\0';
	telnet_console_print(buffer);
}

/* ---------- helpers */

/* what follows the command word an expression starts with (past spaces and
an opening parenthesis, as the console and init.txt give it), else NULL */
static char const *jevin_telemetry_word(
	char const *expression,
	char const *word)
{
	long length = (long)strlen(word);
	long index;

	while (*expression == ' ' || *expression == '\t' || *expression == '(')
		expression++;
	for (index = 0; index < length; index++)
	{
		char character = expression[index] >= 'A' && expression[index] <= 'Z' ? expression[index] - 'A' + 'a' : expression[index];

		if (character != word[index])
			return NULL;
	}
	expression += length;
	if (*expression && *expression != ' ' && *expression != '\t' && *expression != ')')
		return NULL;
	while (*expression == ' ' || *expression == '\t')
		expression++;
	return expression;
}

/* a real at the start of an expression, or `fallback` when there is none */
static boolean jevin_telemetry_real(
	char const **expression,
	real *value)
{
	char const *text = *expression;
	char *end;
	double parsed;

	while (*text == ' ' || *text == '\t')
		text++;
	if ((*text < '0' || *text > '9') && *text != '-' && *text != '.')
		return FALSE;
	parsed = strtod(text, &end);
	if (end == text)
		return FALSE;
	*value = (real)parsed;
	*expression = end;
	return TRUE;
}

/* a long at the start of an expression. Object indices are packed datum
handles in the hundreds of millions, which a real cannot hold, so they are
read as text and never as a float. */
static boolean jevin_telemetry_long(
	char const **expression,
	long *value)
{
	char const *text = *expression;
	char *end;
	long parsed;

	while (*text == ' ' || *text == '\t')
		text++;
	if (*text != '-' && *text != '+' && (*text < '0' || *text > '9'))
		return FALSE;
	parsed = strtol(text, &end, 10);
	if (end == text)
		return FALSE;
	*value = parsed;
	*expression = end;
	return TRUE;
}

/* where an object is, and its orientation */
static void jevin_telemetry_report(
	long index)
{
	struct object_datum *object = object_try_and_get(index);

	if (object == NULL)
	{
		jevin_printf_quiet("JEVIN tp missing %ld", index);
		return;
	}
	jevin_printf_quiet("JEVIN tp %ld %.3f %.3f %.3f", index,
		(double)object->object.position.n[0],
		(double)object->object.position.n[1],
		(double)object->object.position.n[2]);
}

/* put one object where another one is, or at a point. The AI teammate has no
other way to be brought to the player: a squad's actors only think while
their cluster is active, so a marine a hundred units away will not walk. */
static void jevin_telemetry_teleport(
	long index,
	long target_index,
	real x, real y, real z)
{
	struct object_datum *object = object_try_and_get(index);
	struct location location;
	real_point3d position;

	if (object == NULL)
	{
		jevin_printf_quiet("JEVIN tp missing %ld", index);
		return;
	}
	if (target_index != NONE)
	{
		struct object_datum *target = object_try_and_get(target_index);

		if (target == NULL)
		{
			jevin_printf_quiet("JEVIN tp missing_target %ld", target_index);
			return;
		}
		position = target->object.position;
	}
	else
	{
		position.n[0] = x;
		position.n[1] = y;
		position.n[2] = z;
	}
	/* where that point is in the level. A point that is in no cluster is
	outside the structure -- under the map, past the sky, or on a BSP that is
	not loaded -- and a biped moved there falls out of the world, which the
	game then erases. So find the location of the *destination* and refuse
	if it has none, rather than copying the location of the object's old
	place (which would also leave the actor pointing at the far cluster). */
	scenario_location_from_point(&location, &position);
	if (location.cluster_index == NONE)
	{
		jevin_printf_quiet("JEVIN tp outside %.3f %.3f %.3f",
			(double)position.n[0], (double)position.n[1], (double)position.n[2]);
		return;
	}
	object_translate(index, &position, &location);
	jevin_telemetry_report(index);
}

/* the local player's unit, or NONE */
static long jevin_telemetry_player_unit(
	void)
{
	struct player_datum *player;

	if (player_data == NULL)
		return NONE;
	player = player_try_and_get(0);
	if (player == NULL)
		return NONE;
	return player->unit_index;
}

/* ---------- commands */

/* where the player is, in one short line to read off the screen and paste
somewhere else. Typed at the game's console as `pos` during a test, so it is
deliberately *not* quiet: seeing it is the whole point, and it is the one reply a
person is expected to read rather than a process. */
static void jevin_telemetry_pos(
	void)
{
	long unit_index = jevin_telemetry_player_unit();
	struct object_datum *object = unit_index != NONE ? object_try_and_get(unit_index) : NULL;

	if (object == NULL)
	{
		console_printf(FALSE, "JEVIN pos none");
		return;
	}
	console_printf(FALSE, "JEVIN pos %.2f %.2f %.2f",
		(double)object->object.position.n[0],
		(double)object->object.position.n[1],
		(double)object->object.position.n[2]);
}

/* where a unit is, and what it is: the reader's origin and orientation */
static void jevin_telemetry_state(
	void)
{
	long unit_index = jevin_telemetry_player_unit();
	struct object_datum *object = unit_index != NONE ? object_try_and_get(unit_index) : NULL;

	if (object == NULL)
	{
		jevin_printf_quiet("JEVIN state none");
		return;
	}
	jevin_printf_quiet("JEVIN state %ld %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f",
		(long)unit_index,
		(double)object->object.position.n[0], (double)object->object.position.n[1], (double)object->object.position.n[2],
		(double)object->object.forward.n[0], (double)object->object.forward.n[1], (double)object->object.forward.n[2],
		(double)object->object.body_vitality, (double)object->object.shield_vitality);
}

/* every object within `radius` of the reference (0: all of them), with the
reference's own position in the header so the reader needs no second call */
static void jevin_telemetry_objects(
	real radius,
	long reference)
{
	struct object_datum *reference_object;
	long indices[MAXIMUM_TELEMETRY_OBJECTS];
	long count = 0;
	struct object_iterator iterator;
	real radius_squared = radius * radius;
	boolean filter;

	reference_object = reference != NONE ? object_try_and_get(reference) : NULL;
	if (reference_object == NULL)
		reference = jevin_telemetry_player_unit();
	if (reference != NONE)
		reference_object = object_try_and_get(reference);
	filter = radius > 0.0f && reference_object != NULL;

	object_iterator_new(&iterator, _object_mask_all, 0);
	while (count < MAXIMUM_TELEMETRY_OBJECTS)
	{
		struct object_datum *object = object_iterator_next(&iterator);
		long index;

		if (object == NULL)
			break;
		if (filter)
		{
			real dx = object->object.position.x - reference_object->object.position.x;
			real dy = object->object.position.y - reference_object->object.position.y;
			real dz = object->object.position.z - reference_object->object.position.z;

			if (dx * dx + dy * dy + dz * dz > radius_squared)
				continue;
		}
		index = iterator.index;
		indices[count++] = index;
	}

	if (reference_object != NULL)
		jevin_printf_quiet("JEVIN b %ld %ld %.3f %.3f %.3f", count, reference,
			(double)reference_object->object.position.x,
			(double)reference_object->object.position.y,
			(double)reference_object->object.position.z);
	else
		jevin_printf_quiet("JEVIN b %ld %ld 0 0 0", count, (long)NONE);

	{
		long position_index;

		for (position_index = 0; position_index < count; position_index++)
		{
			long index = indices[position_index];
			struct object_datum *object = object_try_and_get(index);
			char const *definition;

			if (object == NULL)
				continue;
			definition = tag_get_name(object->definition_index);
			console_printf(FALSE,
				"JEVIN o %ld %d %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %s",
				index, (int)object->object.type, (int)object->object.owner_team_index,
				(double)object->object.position.n[0], (double)object->object.position.n[1], (double)object->object.position.n[2],
				(double)object->object.bounding_sphere_radius,
				(double)object->object.translational_velocity.n[0],
				(double)object->object.translational_velocity.n[1],
				(double)object->object.translational_velocity.n[2],
				(double)object->object.forward.n[0], (double)object->object.forward.n[1], (double)object->object.forward.n[2],
				(double)object->object.body_vitality, (double)object->object.shield_vitality,
				definition ? definition : "");
		}
	}
	jevin_printf_quiet("JEVIN e");
}

/* ---------- public code */

/* the scenario's encounters and their squads: the <ai> names that ai_place,
ai_follow_target_players and friends take. They live in the map's tags, which
are compressed on disk, so this is the only way to read them out. */
static void jevin_telemetry_encounters(
	void)
{
	struct scenario *scenario = global_scenario_try_and_get();
	long count;
	long index;

	if (scenario == NULL)
	{
		jevin_printf_quiet("JEVIN enc none");
		return;
	}
	count = scenario->ai_encounters.count;
	jevin_printf_quiet("JEVIN encb %ld", count);
	for (index = 0; index < count; index++)
	{
		struct encounter_definition *definition = TAG_BLOCK_GET_ELEMENT(&scenario->ai_encounters, index, struct encounter_definition);
		long squad_count = definition->squads.count;
		long squad_index;

		jevin_printf_quiet("JEVIN enc %ld %d %s", index, (int)definition->team_index, definition->name);
		for (squad_index = 0; squad_index < squad_count; squad_index++)
		{
			struct squad_definition *squad = TAG_BLOCK_GET_ELEMENT(&definition->squads, squad_index, struct squad_definition);

			console_printf(FALSE, "JEVIN sqd %ld %ld %d %d %s",
				index, squad_index, (int)squad->min_count, (int)squad->max_count, squad->name);
		}
	}
	jevin_printf_quiet("JEVIN ence");
}

/* create an object of the same definition as another one - a second Spartan
from the player's own tag, say. With no point given it appears a little in
front of the player, which is a spot the engine has already accepted as
walkable because the player is standing there.

A biped is finished with unit_new: object_new alone leaves it as geometry
without a unit body. */
static void jevin_telemetry_spawn(
	long template_index,
	boolean at_point,
	real x, real y, real z)
{
	struct object_datum *template = object_try_and_get(template_index);
	struct object_datum *player;
	struct object_placement_data data;
	struct location location;
	real_point3d position;
	long player_index = jevin_telemetry_player_unit();
	long new_index;

	if (template == NULL)
	{
		jevin_printf_quiet("JEVIN spawn missing_template %ld", template_index);
		return;
	}
	player = player_index != NONE ? object_try_and_get(player_index) : NULL;
	if (at_point)
	{
		position.n[0] = x;
		position.n[1] = y;
		position.n[2] = z;
	}
	else
	{
		if (player == NULL)
		{
			jevin_printf_quiet("JEVIN spawn no_player");
			return;
		}
		position.n[0] = player->object.position.n[0] + player->object.forward.n[0] * 2.0f;
		position.n[1] = player->object.position.n[1] + player->object.forward.n[1] * 2.0f;
		position.n[2] = player->object.position.n[2] + player->object.forward.n[2] * 2.0f;
	}
	scenario_location_from_point(&location, &position);
	if (location.cluster_index == NONE)
	{
		jevin_printf_quiet("JEVIN spawn outside %.3f %.3f %.3f",
			(double)position.n[0], (double)position.n[1], (double)position.n[2]);
		return;
	}

	object_placement_data_new(&data, template->definition_index, NONE);
	data.position = position;
	if (player != NULL)
	{
		data.forward = player->object.forward;
		data.up = player->object.up;
	}
	else
	{
		data.forward = template->object.forward;
		data.up = template->object.up;
	}
	data.owner_team_index = template->object.owner_team_index;

	new_index = object_new(&data);
	if (new_index == NONE)
	{
		jevin_printf_quiet("JEVIN spawn failed");
		return;
	}
	if (object_try_and_get(new_index) != NULL && object_get_type(new_index) == _object_type_biped)
		unit_new(new_index);
	jevin_printf_quiet("JEVIN spawn %ld %d %.3f %.3f %.3f", new_index,
		(int)object_get_type(new_index),
		(double)position.n[0], (double)position.n[1], (double)position.n[2]);
}

/* how far below the ground is, at a point `ahead` of an object along its
forward axis. This is the ledge test: a probe that finds nothing, or finds
the ground far below, is a step off an edge.

collision_test_vector is the world test - the physics one only tests an
object against its own collision spheres. The result's `t` is the fraction of
the vector at which it hits, so the drop is that fraction times the probe
length. */
static void jevin_telemetry_ground(
	long index,
	real ahead,
	real drop)
{
	struct object_datum *object = object_try_and_get(index);
	struct collision_result collision;
	real_vector3d down;
	real_point3d point;
	/* structure and objects both: a biped can be standing on a crashed pod or a
	crate, which is an object collision model rather than the structure */
	unsigned long flags = (1UL << _collision_test_structure_bit) | (1UL << _collision_test_objects_bit);

	if (object == NULL)
	{
		jevin_printf_quiet("JEVIN ground missing %ld", index);
		return;
	}
	if (drop <= 0.0f)
		drop = 4.0f;
	point.n[0] = object->object.position.n[0] + object->object.forward.n[0] * ahead;
	point.n[1] = object->object.position.n[1] + object->object.forward.n[1] * ahead;
	point.n[2] = object->object.position.n[2] + object->object.forward.n[2] * ahead + 0.1f;
	down.n[0] = 0.0f;
	down.n[1] = 0.0f;
	down.n[2] = -drop;
	if (collision_test_vector(flags, &point, &down, index, &collision))
		jevin_printf_quiet("JEVIN ground yes %.3f", (double)(collision.t * drop));
	else
		jevin_printf_quiet("JEVIN ground none %.3f", (double)drop);
	jevin_printf_quiet("JEVIN gend");
}

/* the ground around a unit as a grid, in one console call.

A single probe ahead answers "is there a step here", and that is not enough: a
bridge is narrow, a ledge can run diagonally across the line of travel, and two
separate probes can both read safe across a gap between them. All of that is
what a body walks off. Sampling a square of points in one call gives the
controller the local *shape* of the ground for the price of one round trip.

	jevin_grid <unit> <spacing> <count> [drop] [layers]

`count` is the radius in cells, so a count of 3 is a 7 by 7 grid, and offsets are
in cells from the unit. `layers` is how many surfaces to look for in each column:
the ray starts above the unit, and after it lands the next layer starts just
below that surface, so a bridge deck and the ground under it come back as two
floors of the same column rather than one. Heights are relative to the unit's own
position, which is what the controller compares against:

	JEVIN g <i> <j> <layer> hit|none <height> <inside> <water> <same cluster>
	JEVIN gend
*/
static void jevin_telemetry_grid(
	long index,
	real spacing,
	long count,
	real drop,
	long layers)
{
	struct object_datum *object = object_try_and_get(index);
	struct collision_result collision;
	struct location probe_location;
	real_vector3d down;
	real_point3d point;
	/* structure and objects both: a biped can stand on a crate or a crashed
	pod, which are object collision models rather than the structure */
	unsigned long flags = (1UL << _collision_test_structure_bit) |
		(1UL << _collision_test_objects_bit);
	long i, j, layer;
	short own_cluster;

	if (object == NULL)
	{
		jevin_printf_quiet("JEVIN grid missing %ld", index);
		jevin_printf_quiet("JEVIN gend");
		return;
	}
	if (spacing <= 0.0f)
		spacing = 1.0f;
	if (count < 1)
		count = 1;
	if (count > 6)
		count = 6;                          /* the dump has to stay cheap */
	if (drop <= 0.0f)
		drop = 8.0f;
	if (layers < 1)
		layers = 1;
	if (layers > 4)
		layers = 4;
	own_cluster = object->object.location.cluster_index;
	down.n[0] = 0.0f;
	down.n[1] = 0.0f;
	down.n[2] = -drop;

	for (i = -count; i <= count; i++)
	{
		for (j = -count; j <= count; j++)
		{
			/* start just above his own feet, not high above his head: the roof
			of whatever he is standing under is the first thing a ray from up
			there finds, and then the floor he is actually on never appears in
			the column at all. Nine tenths of a unit above the origin still
			catches a step up, which is as much as he can climb. */
			real start_z = object->object.position.n[2] + 0.9f;

			for (layer = 0; layer < layers; layer++)
			{
				boolean found;
				real surface_z;

				point.n[0] = object->object.position.n[0] + (real)i * spacing;
				point.n[1] = object->object.position.n[1] + (real)j * spacing;
				point.n[2] = start_z;
				scenario_location_from_point(&probe_location, &point);
				found = collision_test_vector(flags, &point, &down, index, &collision) != FALSE;
				if (!found)
				{
					/* nothing below this point: report the bottom of the
					column that was searched, so the reader knows how far the
					floor is from here */
					jevin_printf_quiet("JEVIN g %ld %ld %ld none %.3f %d %d %d",
						i, j, layer,
						(double)(start_z - drop - object->object.position.n[2]),
						(int)(probe_location.cluster_index != NONE),
						(int)scenario_location_underwater(&probe_location, &point, NULL),
						(int)(probe_location.cluster_index == own_cluster));
					break;
				}

				surface_z = start_z - collision.t * drop;
				/* the surface, not the start of the ray, is the place whose
				cluster and water state matter */
				point.n[2] = surface_z + 0.05f;
				scenario_location_from_point(&probe_location, &point);
				jevin_printf_quiet("JEVIN g %ld %ld %ld hit %.3f %d %d %d",
					i, j, layer,
					(double)(surface_z - object->object.position.n[2]),
					(int)(probe_location.cluster_index != NONE),
					(int)scenario_location_underwater(&probe_location, &point, NULL),
					(int)(probe_location.cluster_index == own_cluster));

				start_z = surface_z - 0.05f;
				if (start_z <= object->object.position.n[2] - drop)
					break;              /* nothing left to search below */
			}
		}
	}
	jevin_printf_quiet("JEVIN gend");
}

/* the same ledge test, but from a point offset in world space rather than
along the object's facing. A body does not only move where it looks: it can
strafe, and it has width, so the probe has to be able to test sideways and to
test both sides of where it is going. */
static void jevin_telemetry_ground_at(
	long index,
	real dx, real dy, real drop)
{
	struct object_datum *object = object_try_and_get(index);
	struct collision_result collision;
	struct location probe_location;
	real_vector3d down;
	real_point3d point;
	/* structure and objects both: a biped can be standing on a crashed pod or a
	crate, which is an object collision model rather than the structure */
	unsigned long flags = (1UL << _collision_test_structure_bit) | (1UL << _collision_test_objects_bit);
	short own_cluster;

	if (object == NULL)
	{
		jevin_printf_quiet("JEVIN ground missing %ld", index);
		return;
	}
	if (drop <= 0.0f)
		drop = 4.0f;
	point.n[0] = object->object.position.n[0] + dx;
	point.n[1] = object->object.position.n[1] + dy;
	point.n[2] = object->object.position.n[2] + 0.1f;
	/* the collision world holds only the clusters that are loaded, so a probe
	that finds nothing may be over a hole or over ground that does not exist
	yet. Naming the cluster at the probe point tells the two apart. */
	scenario_location_from_point(&probe_location, &point);
	own_cluster = object->object.location.cluster_index;
	down.n[0] = 0.0f;
	down.n[1] = 0.0f;
	down.n[2] = -drop;
	if (collision_test_vector(flags, &point, &down, index, &collision))
		jevin_printf_quiet("JEVIN groundat %.3f %.3f yes %.3f %d %d %d",
			(double)dx, (double)dy, (double)(collision.t * drop),
			(int)probe_location.cluster_index, (int)own_cluster,
			(int)scenario_location_underwater(&probe_location, &point, NULL));
	else
		jevin_printf_quiet("JEVIN groundat %.3f %.3f none %.3f %d %d %d",
			(double)dx, (double)dy, (double)drop,
			(int)probe_location.cluster_index, (int)own_cluster,
			(int)scenario_location_underwater(&probe_location, &point, NULL));
	jevin_printf_quiet("JEVIN gend");
}

/* a compact per-tick readout for a control loop: the same objects as
jevin_objects, but only the fields a rule needs, positioned relative to the
reference, and with no tag lookup at all.

The tag lookup is the expensive part of jevin_objects and the definition name
is the largest field, so leaving both out is what makes this fast enough to
call every tick. Named positions are recovered from the header.

	JEVIN s <count> <reference> <rx> <ry> <rz>
	JEVIN n <index> <type> <team> <dx> <dy> <dz> <radius> <body> <shield> \
	        <shield damage> <body damage> <fx> <fy> <fz>
	JEVIN z
*/
static void jevin_telemetry_snapshot(
	real radius,
	long reference)
{
	struct object_datum *reference_object;
	long indices[MAXIMUM_TELEMETRY_OBJECTS];
	long count = 0;
	struct object_iterator iterator;
	real radius_squared = radius * radius;
	boolean filter;

	if (reference == NONE)
		reference = jevin_telemetry_player_unit();
	reference_object = reference != NONE ? object_try_and_get(reference) : NULL;
	filter = radius > 0.0f && reference_object != NULL;

	object_iterator_new(&iterator, _object_mask_all, 0);
	while (count < MAXIMUM_TELEMETRY_OBJECTS)
	{
		struct object_datum *object = object_iterator_next(&iterator);

		if (object == NULL)
			break;
		if (filter)
		{
			real dx = object->object.position.n[0] - reference_object->object.position.n[0];
			real dy = object->object.position.n[1] - reference_object->object.position.n[1];
			real dz = object->object.position.n[2] - reference_object->object.position.n[2];

			if (dx * dx + dy * dy + dz * dz > radius_squared)
				continue;
		}
		indices[count++] = iterator.index;
	}

	if (reference_object != NULL)
		jevin_printf_quiet("JEVIN s %ld %ld %.3f %.3f %.3f", count, reference,
			(double)reference_object->object.position.n[0],
			(double)reference_object->object.position.n[1],
			(double)reference_object->object.position.n[2]);
	else
		jevin_printf_quiet("JEVIN s %ld %ld 0 0 0", count, (long)NONE);

	{
		long position_index;

		for (position_index = 0; position_index < count; position_index++)
		{
			long index = indices[position_index];
			struct object_datum *object = object_try_and_get(index);

			if (object == NULL)
				continue;
			jevin_printf_quiet("JEVIN n %ld %d %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f",
				index, (int)object->object.type, (int)object->object.owner_team_index,
				reference_object != NULL ? (double)(object->object.position.n[0] - reference_object->object.position.n[0]) : 0.0,
				reference_object != NULL ? (double)(object->object.position.n[1] - reference_object->object.position.n[1]) : 0.0,
				reference_object != NULL ? (double)(object->object.position.n[2] - reference_object->object.position.n[2]) : 0.0,
				(double)object->object.bounding_sphere_radius,
				(double)object->object.body_vitality,
				(double)object->object.shield_vitality,
				(double)object->object.recent_shield_damage,
				(double)object->object.recent_body_damage,
				/* the facing, last so that a reader can ignore it: whether he
				is actually pointing at what he is shooting is not visible any
				other way, and it was silently zero here before */
				(double)object->object.forward.n[0],
				(double)object->object.forward.n[1],
				(double)object->object.forward.n[2]);
		}
	}
	jevin_printf_quiet("JEVIN z");
}

/* drive a body: write its velocity and turn it the way it is going.

A unit with no player at the controls and no AI never moves on its own, so
this is the seam where the harness stops being an inspector and becomes a
controller. The velocity is what the physics integrates; the forward vector is
kept upright, so the body turns about the vertical towards its motion rather
than tipping. */
static void jevin_telemetry_push(
	long index,
	real vx, real vy, real vz)
{
	struct object_datum *object = object_try_and_get(index);
	real_vector3d direction;

	if (object == NULL)
	{
		jevin_printf_quiet("JEVIN push missing %ld", index);
		return;
	}
	object->object.translational_velocity.n[0] = vx;
	object->object.translational_velocity.n[1] = vy;
	object->object.translational_velocity.n[2] = vz;
	direction.n[0] = vx;
	direction.n[1] = vy;
	direction.n[2] = 0.0f;
	if (normalize3d(&direction) != 0.0f)
	{
		/* Halo's world is Z-up: keep the body upright and turn it about the
		vertical, so the forward and up vectors stay orthogonal */
		object->object.forward = direction;
		object->object.up.n[0] = 0.0f;
		object->object.up.n[1] = 0.0f;
		object->object.up.n[2] = 1.0f;
	}
	jevin_printf_quiet("JEVIN push %ld %.3f %.3f %.3f %.3f %.3f %.3f",
		index,
		(double)object->object.position.n[0],
		(double)object->object.position.n[1],
		(double)object->object.position.n[2],
		(double)vx, (double)vy, (double)vz);
}

/* the colours a unit is drawn with. A unit's armour is coloured by its
"change colours": three real RGB values that the armour shader mixes in. */
static void jevin_telemetry_colors(
	long index)
{
	struct object_datum *object = object_try_and_get(index);
	long slot;

	if (object == NULL)
	{
		console_printf(FALSE, "JEVIN colors missing %ld", index);
		return;
	}
	console_printf(FALSE, "JEVIN colors %ld %d", index, (int)NUMBER_OF_OBJECT_CHANGE_COLORS);
	for (slot = 0; slot < NUMBER_OF_OBJECT_CHANGE_COLORS; slot++)
		console_printf(FALSE, "JEVIN color %ld %.3f %.3f %.3f", slot,
			(double)object->object.base_change_colors[slot].n[0],
			(double)object->object.base_change_colors[slot].n[1],
			(double)object->object.base_change_colors[slot].n[2]);
}

/* recolour a unit. Both the base and the outgoing colours are set, because the
renderer reads whichever the object is currently blending towards. */
static void jevin_telemetry_color(
	long index,
	real red, real green, real blue)
{
	struct object_datum *object = object_try_and_get(index);
	long slot;

	if (object == NULL)
	{
		console_printf(FALSE, "JEVIN color missing %ld", index);
		return;
	}
	for (slot = 0; slot < NUMBER_OF_OBJECT_CHANGE_COLORS; slot++)
	{
		object->object.base_change_colors[slot].n[0] = red;
		object->object.base_change_colors[slot].n[1] = green;
		object->object.base_change_colors[slot].n[2] = blue;
		object->object.outgoing_change_colors[slot].n[0] = red;
		object->object.outgoing_change_colors[slot].n[1] = green;
		object->object.outgoing_change_colors[slot].n[2] = blue;
	}
	console_printf(FALSE, "JEVIN color %ld %.3f %.3f %.3f", index,
		(double)red, (double)green, (double)blue);
}

/* every weapon object in the level, with its definition name. The compact
snapshot caps its object count, so a weapon far down the table - the player
own rifle, say - is not in it; this walks the whole table instead. */
static void jevin_telemetry_weapons(
	void)
{
	struct object_iterator iterator;
	long count = 0;

	object_iterator_new(&iterator, FLAG(_object_type_weapon), 0);
	jevin_printf_quiet("JEVIN wb");
	while (count < 128)
	{
		struct object_datum *object = object_iterator_next(&iterator);
		char const *definition;

		if (object == NULL)
			break;
		definition = tag_get_name(object->definition_index);
		jevin_printf_quiet("JEVIN w %ld %s", iterator.index, definition ? definition : "");
		count++;
	}
	jevin_printf_quiet("JEVIN we");
}
/* put a weapon in a unit inventory. The template is any object whose
definition is the weapon wanted, so a rifle already lying on the ground is
enough to arm a spawned body: the definition is copied, a weapon object is
made at the unit, and it goes into the inventory as a starting weapon so the
unit draws it.

unit_add_weapon_to_inventory takes a weapon *object*, not a definition, which
is why the object is made first. */
static void jevin_telemetry_give(
	long unit_index,
	long template_index)
{
	struct object_datum *unit = object_try_and_get(unit_index);
	struct object_datum *template = object_try_and_get(template_index);
	struct object_placement_data data;
	struct location location;
	real_point3d position;
	long weapon_index;

	if (unit == NULL || template == NULL)
	{
		jevin_printf_quiet("JEVIN give missing %ld %ld", unit_index, template_index);
		return;
	}
	position = unit->object.position;
	scenario_location_from_point(&location, &position);
	if (location.cluster_index == NONE)
	{
		jevin_printf_quiet("JEVIN give outside");
		return;
	}
	object_placement_data_new(&data, template->definition_index, NONE);
	data.position = position;
	data.forward = unit->object.forward;
	data.up = unit->object.up;
	weapon_index = object_new(&data);
	if (weapon_index == NONE)
	{
		jevin_printf_quiet("JEVIN give no_object");
		return;
	}
	if (!unit_add_weapon_to_inventory(unit_index, weapon_index, TRUE))
	{
		jevin_printf_quiet("JEVIN give refused %ld", weapon_index);
		object_delete(weapon_index);
		return;
	}
	jevin_printf_quiet("JEVIN give %ld %ld", unit_index, weapon_index);
}
/* state for the remote control section below; declared here because the
per-tick hook that the engine calls sits above it */
static struct unit_control_data jevin_remote_control;
static long jevin_remote_unit = NONE;
static boolean jevin_remote_ready;

/* Called from unit_update, which is where a unit's control flags are turned
into weapon, melee and reload actions. The moving-animation hook is too narrow
for buttons: it only runs when he walks, so a trigger pulled standing still was
overwritten by the AI before the weapon code ever saw it.

Only the bits this module manages are forced, so the AI keeps whatever it owns
(alertness, facing, and so on). */
void jevin_remote_before_weapon(
	long unit_index)
{
	struct unit_datum *unit;
	word managed = FLAG(_unit_control_weapon_primary_trigger_bit) |
		FLAG(_unit_control_weapon_secondary_trigger_bit) |
		FLAG(_unit_control_user_animation1_bit) |
		FLAG(_unit_control_user_animation2_bit) |
		FLAG(_unit_control_weapon_reload_bit) |
		FLAG(_unit_control_throw_grenade_bit) |
		FLAG(_unit_control_swap_weapons_bit) |
		FLAG(_unit_control_jump_bit) |
		FLAG(_unit_control_crouch_modifier_bit) |
		FLAG(_unit_control_action_bit) |
		FLAG(_unit_control_exact_facing_bit) |
		FLAG(_unit_control_integrated_light_bit);

	if (jevin_remote_unit == NONE || unit_index != jevin_remote_unit)
		return;
	unit = unit_try_and_get(jevin_remote_unit);
	if (unit == NULL)
	{
		jevin_remote_unit = NONE;
		return;
	}
	/* The control flags written below are discarded a few lines later by the
	unit update, which clears them outright for a unit it does not consider
	actively controlled. This hook runs before that, so re-asserting the bit
	here is what keeps them: it is the same statement a player's unit has, and
	setting it once when control begins is not enough, because an actor
	finishing an impulse, or the unit dying, can clear it again. */
	SET_FLAG(unit->unit.flags, _unit_actively_controlled_bit, TRUE);
	unit->unit.control_flags = (word)((unit->unit.control_flags & ~managed) |
		(jevin_remote_control.control_flags & managed));
	unit->unit.primary_trigger = jevin_remote_control.primary_trigger;
}

/* Called from biped_update_moving, right before the throttle is turned into a
walk cycle. The AI writes its own control every tick, so a throttle set by the
console survives only until the next actor update; re-applying it here - after
the AI has had its say, before the legs are chosen - is what makes a commanded
walk actually walk. Aiming and the upper body are left alone, because the AI's
own handling of them looked right. */
void jevin_remote_before_move(
	long unit_index)
{
	struct unit_datum *unit;

	if (jevin_remote_unit == NONE || unit_index != jevin_remote_unit)
		return;
	unit = unit_try_and_get(jevin_remote_unit);
	if (unit == NULL)
	{
		jevin_remote_unit = NONE;
		return;
	}
	unit->unit.throttle = jevin_remote_control.throttle;
	if (jevin_remote_control.primary_trigger > 0.0f)
		unit->unit.primary_trigger = jevin_remote_control.primary_trigger;
}

/* ---------- walls, and what the weapon is doing ---------- */

/* A horizontal ray from a point offset from a unit, to find walls. The ground
probe answers "is there floor"; this answers "is there something in the way",
which is what the steering needs to stop him walking into geometry. */
static void jevin_telemetry_probe(
	long index,
	real dx, real dy, real length, real height)
{
	struct object_datum *object = object_try_and_get(index);
	struct collision_result collision;
	real_point3d point;
	real_vector3d direction;
	/* the engine's own answer to "what stops a bullet here": structure, media
	and the object types a projectile collides with. Naming only the
	structure and "objects" bits was not enough - the object *types* have to be
	set too, which is why trees (scenery) were invisible to this and he fired
	straight through them. */
	unsigned long flags = _collision_test_for_projectiles_flags;

	if (object == NULL)
	{
		jevin_printf_quiet("JEVIN probe missing %ld", index);
		return;
	}
	if (length <= 0.0f)
		length = 2.0f;
	{
		real norm = (real)sqrt((double)(dx * dx + dy * dy));

		if (norm < 1e-6f)
		{
			jevin_printf_quiet("JEVIN probe direction");
			return;
		}
		dx /= norm;
		dy /= norm;
	}
	point.n[0] = object->object.position.n[0];
	point.n[1] = object->object.position.n[1];
	point.n[2] = object->object.position.n[2] + height;
	direction.n[0] = dx * length;
	direction.n[1] = dy * length;
	direction.n[2] = 0.0f;
	if (collision_test_vector(flags, &point, &direction, index, &collision))
		/* a hit: the distance to whatever is in the way */
		jevin_printf_quiet("JEVIN probe hit %.3f", (double)(collision.t * length));
	else
		jevin_printf_quiet("JEVIN probe clear %.3f", (double)length);
	jevin_printf_quiet("JEVIN pend");
}

/* a ray from an object to a point in the world, in three dimensions.

`jevin_probe` answers "is there a wall that way" with a horizontal ray, which is
what steering needs. This answers a different question - "can he see that" -
and it has to be three dimensional: a horizontal ray at chest height runs into
the ground on rising terrain well before it reaches anything standing on it,
which reads as "no line of sight" for every enemy on a slope. */
static void jevin_telemetry_ray(
	long index,
	real x, real y, real z)
{
	struct object_datum *object = object_try_and_get(index);
	struct collision_result collision;
	real_point3d point;
	real_vector3d direction;
	real length;
	/* what a *bullet* stops on, which is the question this has to answer. The
	engine's line-of-sight set is narrower - it does not include instanced
	geometry - so an enemy on the far side of the escape pod read as "clear",
	he fired into the wall, and the fight did no damage at all. */
	unsigned long flags = _collision_test_for_projectiles_flags;

	if (object == NULL)
	{
		jevin_printf_quiet("JEVIN ray missing %ld", index);
		jevin_printf_quiet("JEVIN rend");
		return;
	}
	point.n[0] = object->object.position.n[0];
	point.n[1] = object->object.position.n[1];
	point.n[2] = object->object.position.n[2] + 0.5f;
	direction.n[0] = x - point.n[0];
	direction.n[1] = y - point.n[1];
	direction.n[2] = z - point.n[2];
	length = (real)sqrt((double)(direction.n[0] * direction.n[0] +
		direction.n[1] * direction.n[1] +
		direction.n[2] * direction.n[2]));
	if (length < 1e-4f || normalize3d(&direction) == 0.0f)
	{
		jevin_printf_quiet("JEVIN ray gone %.3f", (double)length);
		jevin_printf_quiet("JEVIN rend");
		return;
	}
	/* the engine takes the ray as a displacement, not as a direction: without
	the scale it travels exactly one unit and reports everything clear, which
	is why every shot here was taken through trees and rocks */
	direction.n[0] *= length;
	direction.n[1] *= length;
	direction.n[2] *= length;
	if (collision_test_vector(flags, &point, &direction, index, &collision))
		jevin_printf_quiet("JEVIN ray hit %.3f %.3f",
			(double)(collision.t * length), (double)length);
	else
		jevin_printf_quiet("JEVIN ray clear %.3f", (double)length);
	jevin_printf_quiet("JEVIN rend");
}

/* what the weapon system thinks of him: which inventory slot is out, which
weapon that slot holds, whether one is wanted, and how much ammunition it has.
Firing produced no projectiles, and this is the query that says why - a unit
with no current weapon never reaches the code that pulls the trigger.

`current_weapon_index` is a slot in the inventory, not an object: the weapon
itself is what `unit_inventory_get_weapon` resolves it to, which is the index
the dumps elsewhere report. */
static void jevin_telemetry_weaponry(
	long unit_index)
{
	struct unit_datum *unit = unit_try_and_get(unit_index);
	long slot = NONE;
	long current = NONE;
	long desired = NONE;
	long count = NONE;

	if (unit == NULL)
	{
		jevin_printf_quiet("JEVIN weaponry not_a_unit %ld", unit_index);
		return;
	}
	slot = unit->unit.current_weapon_index;
	current = unit_inventory_get_weapon(unit_index, (short)slot);
	desired = unit_inventory_get_weapon(unit_index, unit->unit.desired_weapon_index);
	count = unit_get_weapon_count(unit_index);
	jevin_printf_quiet("JEVIN weaponry %ld slot %ld current %ld desired %ld count %ld trigger %.2f flags %04x",
		unit_index, slot, current, desired, count,
		(double)unit->unit.primary_trigger, (unsigned)unit->unit.control_flags);
	jevin_printf_quiet("JEVIN wend");
}

/* what the weapon itself has been told, as opposed to what the unit asked
for: the flags the unit handed it, whether it thinks its owner is busy, and
how many rounds are in the magazine.

Firing is decided on this side (weapons.c: "if (!TEST_FLAG(weapon->weapon.
control_flags, _weapon_control_user_busy_bit) && weapon->weapon.state_timer
<=0)"), so this is where a trigger that never becomes a shot shows itself. A
trigger bit that never arrives, a weapon left perpetually busy by a busy
animation, and an empty magazine all look different here.

`lastfired` and `now` are engine ticks, and `lastfired` is written inside the
firing code itself (weapons.c, "weapon->weapon.game_time_last_fired =
game_time_get()"). That makes it a better answer than counting projectiles: a
shot that fired but whose projectile was removed before anyone counted it
still moves `lastfired`. */
static void jevin_telemetry_weapon_state(
	long unit_index)
{
	struct unit_datum *unit = unit_try_and_get(unit_index);
	struct weapon_datum *weapon;
	struct weapon_definition const *weapon_definition;
	long weapon_index;
	short magazine_index;
	short count;

	if (unit == NULL)
	{
		jevin_printf_quiet("JEVIN wstate not_a_unit %ld", unit_index);
		jevin_printf_quiet("JEVIN wsend");
		return;
	}
	weapon_index = unit_inventory_get_weapon(unit_index, (short)unit->unit.current_weapon_index);
	jevin_printf_quiet("JEVIN wstate %ld flags %04x active %d controllable %d weapon %ld aim %.3f %.3f %.3f face %.3f %.3f %.3f",
		unit_index, (unsigned)unit->unit.flags,
		(int)(TEST_FLAG(unit->unit.flags, _unit_actively_controlled_bit) ? 1 : 0),
		(int)(TEST_FLAG(unit->unit.flags, _unit_controllable_bit) ? 1 : 0),
		weapon_index,
		(double)unit->unit.aiming_vector.n[0],
		(double)unit->unit.aiming_vector.n[1],
		(double)unit->unit.aiming_vector.n[2],
		(double)unit->object.forward.n[0],
		(double)unit->object.forward.n[1],
		(double)unit->object.forward.n[2]);
	if (weapon_index == NONE || object_get_type(weapon_index) != _object_type_weapon)
	{
		jevin_printf_quiet("JEVIN wdetail none");
		jevin_printf_quiet("JEVIN wsend");
		return;
	}
	weapon = (struct weapon_datum *)object_try_and_get(weapon_index);
	if (weapon == NULL)
	{
		jevin_printf_quiet("JEVIN wdetail missing %ld", weapon_index);
		jevin_printf_quiet("JEVIN wsend");
		return;
	}
	weapon_definition = weapon_definition_get(weapon->definition_index);
	jevin_printf_quiet("JEVIN wdetail %ld ctrl %04x state %d timer %d trig %.2f heat %.2f lastfired %ld now %ld",
		weapon_index, (unsigned)weapon->weapon.control_flags,
		(int)weapon->weapon.state, (int)weapon->weapon.state_timer,
		(double)weapon->weapon.primary_trigger, (double)weapon->weapon.heat,
		weapon->weapon.game_time_last_fired, game_time_get());
	count = (short)weapon_definition->weapon.magazines.count;
	if (count > (short)NUMBEROF(weapon->weapon.magazines))
		count = (short)NUMBEROF(weapon->weapon.magazines);
	for (magazine_index = 0; magazine_index < count; ++magazine_index)
	{
		struct weapon_magazine *magazine = &weapon->weapon.magazines[magazine_index];

		jevin_printf_quiet("JEVIN wmag %d loaded %d total %d state %d timer %d",
			(int)magazine_index, (int)magazine->rounds_loaded,
			(int)magazine->rounds_total, (int)magazine->state,
			(int)magazine->state_timer);
	}
	jevin_printf_quiet("JEVIN wsend");
}

/* ---------- give the body an actor ---------- */

/* A spawned unit has no actor, and the engine's locomotion, animation, firing
and melee all hang off actor control (actors.c calls unit_control every tick).
Giving the unit an actor is therefore the difference between a body that slides
and a body that walks, aims and shoots.

The actor variant - the AI behaviour tag - is taken from an actor that already
exists in the level, because the variant names live in compressed map tags and
a placed squad is the only easy way to name one. So place an encounter first
(ai_place first_marine) and then call this. */
static void jevin_actor_attach(
	long unit_index)
{
	struct object_datum *object = object_try_and_get(unit_index);
	long index;
	long donor = NONE;

	if (object == NULL || object_get_type(unit_index) != _object_type_biped)
	{
		jevin_printf_quiet("JEVIN actor not_a_biped %ld", unit_index);
		return;
	}
	if (actor_data == NULL)
	{
		jevin_printf_quiet("JEVIN actor no_actor_data");
		return;
	}
	for (index = 0; index < (long)actor_data->count; index++)
	{
		struct actor_datum *actor = actor_try_and_get(index);

		if (actor != NULL && actor->meta.unit_index != NONE &&
			actor->meta.variant_definition_index != NONE)
		{
			donor = index;
			break;
		}
	}
	if (donor == NONE)
	{
		jevin_printf_quiet("JEVIN actor no_donor (ai_place an encounter first)");
		return;
	}
	{
		struct actor_datum *source = actor_get(donor);
		long created = actor_create_for_unit(
			FALSE, unit_index, source->meta.variant_definition_index,
			NONE, NONE, FALSE, NONE, FALSE,
			source->state.initial_state, source->state.default_state, NONE, NONE);
		if (created == NONE)
		{
			jevin_printf_quiet("JEVIN actor failed");
			return;
		}
		{
			struct actor_datum *mine = actor_get(created);

			/* an encounterless actor has to be forced awake: nothing else will
			activate him, because he belongs to no encounter */
			mine->meta.encounterless = TRUE;
			mine->meta.force_active = TRUE;
			mine->meta.active = TRUE;
		}
		jevin_printf_quiet("JEVIN actor %ld %ld %ld", unit_index, created, donor);
	}
}

/* ---------- remote control: the player-like way to drive a unit ---------- */

/* Writing a unit's velocity moves it but skips its locomotion: the legs do not
walk, because nothing told the unit to walk. `unit_control` is the structure a
player's sticks and triggers fill in, and the unit update turns it into
locomotion, animation, firing, reloads and weapon switches. So this holds the
same structure, lets the console commands edit it, and hands it over.

The values are requests and persist until changed, which is why the console
does not have to be called every tick. The state itself is declared above, next
to the per-tick hook the engine calls. */

static void jevin_remote_reset_control(
	void)
{
	memset(&jevin_remote_control, 0, sizeof(jevin_remote_control));
	jevin_remote_control.animation_state = _unit_animation_state_in_combat;
	jevin_remote_control.aiming_speed = 0;
	jevin_remote_control.weapon_index = NONE;
	jevin_remote_control.grenade_index = NONE;
	jevin_remote_control.zoom_level = NONE;
	jevin_remote_control.control_flags = 0;
	/* `_unit_control_exact_facing_bit` is set by `jevin_aim` and cleared by
	`jevin_walk`, not here: it tightens the standing turn to about eight
	degrees, which is what makes him face what he is shooting, but it also
	makes the engine abort a turn animation the moment it passes its target,
	and with it set while walking he kept losing his stride. */
	jevin_remote_control.throttle.n[0] = 0.0f;
	jevin_remote_control.throttle.n[1] = 0.0f;
	jevin_remote_control.throttle.n[2] = 0.0f;
	jevin_remote_control.primary_trigger = 0.0f;
	/* Halo's world is Z-up, so a unit standing still faces +y */
	jevin_remote_control.facing_vector.n[0] = 0.0f;
	jevin_remote_control.facing_vector.n[1] = 1.0f;
	jevin_remote_control.facing_vector.n[2] = 0.0f;
	jevin_remote_control.aiming_vector = jevin_remote_control.facing_vector;
	jevin_remote_control.looking_vector = jevin_remote_control.facing_vector;
	jevin_remote_ready = TRUE;
}

static boolean jevin_remote_begin(
	long unit_index)
{
	struct object_datum *object = object_try_and_get(unit_index);

	if (object == NULL || object_get_type(unit_index) != _object_type_biped)
	{
		console_printf(FALSE, "JEVIN control not_a_biped %ld", unit_index);
		return FALSE;
	}
	jevin_remote_unit = unit_index;
	/* Mark him the way a player's unit is marked, not merely as controllable.
	The distinction is the whole reason the trigger did nothing: a unit the
	engine does not consider actively controlled has its control flags cleared
	at the top of every unit update (units.c, "else if (!TEST_FLAG(...,
	_unit_actively_controlled_bit))"), which is after the hook above has set
	them. Walking survived that because the movement hook runs later still and
	sets the throttle itself; the trigger bit was simply erased before the
	weapon ever read it. */
	unit_set_actively_controlled(unit_index, TRUE);
	if (!jevin_remote_ready)
		jevin_remote_reset_control();
	jevin_remote_control.weapon_index = NONE;
	return TRUE;
}

static void jevin_remote_apply(
	void)
{
	struct object_datum *object;
	real magnitude;

	if (jevin_remote_unit == NONE)
		return;
	object = object_try_and_get(jevin_remote_unit);
	if (object == NULL || object_get_type(jevin_remote_unit) != _object_type_biped)
	{
		jevin_remote_unit = NONE;
		return;
	}
	/* The engine asserts that a unit's control throttle is at most 3, and a
	normalised direction multiplied by a speed of 3 lands a hair over it in
	single precision: that assert (units.c, "magnitude3d(&control_data->
	throttle)<=3.0f") took the whole game down at the end of two sessions. Clamp
	below the limit here, at the one place where every command hands control
	over, rather than trusting each caller to leave a margin. */
	magnitude = (real)sqrt((double)(jevin_remote_control.throttle.n[0] * jevin_remote_control.throttle.n[0] +
		jevin_remote_control.throttle.n[1] * jevin_remote_control.throttle.n[1] +
		jevin_remote_control.throttle.n[2] * jevin_remote_control.throttle.n[2]));
	if (magnitude > 2.9f)
	{
		real scale = 2.9f / magnitude;

		jevin_remote_control.throttle.n[0] *= scale;
		jevin_remote_control.throttle.n[1] *= scale;
		jevin_remote_control.throttle.n[2] *= scale;
	}
	unit_control(jevin_remote_unit, &jevin_remote_control);
}

/* walk in a direction: throttle makes the legs move, the facing turns him */
static void jevin_remote_walk(
	long unit_index,
	real dx, real dy, real speed)
{
	real_vector3d direction;
	real length;

	if (jevin_remote_unit != unit_index && !jevin_remote_begin(unit_index))
		return;
	length = (real)sqrt((double)(dx * dx + dy * dy));
	if (length < 1e-6f)
	{
		jevin_remote_control.throttle.n[0] = 0.0f;
		jevin_remote_control.throttle.n[1] = 0.0f;
		jevin_remote_control.throttle.n[2] = 0.0f;
	}
	else
	{
		real magnitude = speed;

		if (magnitude < 0.0f)
			magnitude = 0.0f;
		if (magnitude > 3.0f)
			magnitude = 3.0f;                            /* the engine asserts <= 3 */
		direction.n[0] = dx / length;
		direction.n[1] = dy / length;
		direction.n[2] = 0.0f;
		jevin_remote_control.throttle.n[0] = direction.n[0] * magnitude;
		jevin_remote_control.throttle.n[1] = direction.n[1] * magnitude;
		jevin_remote_control.throttle.n[2] = 0.0f;
		jevin_remote_control.facing_vector = direction;
		jevin_remote_control.aiming_vector = direction;
		jevin_remote_control.looking_vector = direction;
		/* turning is left approximate while travelling: exact facing makes the
		engine abort a turn animation as soon as it passes its target, and in a
		walk cycle that abort lands in the middle of a step */
		jevin_remote_control.control_flags &= ~FLAG(_unit_control_exact_facing_bit);
	}
	jevin_remote_apply();
	jevin_printf_quiet("JEVIN walk %ld %.3f %.3f %.3f", unit_index,
		(double)jevin_remote_control.throttle.n[0],
		(double)jevin_remote_control.throttle.n[1],
		(double)length);
}

/* move in the unit's own frame: forward is the way he is facing, side is left
and right, and the facing is left alone.

`jevin_walk` takes a world direction and turns him to face it, which is what
travelling should look like. This is for the other case: keeping the aim and
the body on something while stepping away from it or past it, which a person
does by walking backwards or side-stepping rather than by turning his back.

The two numbers are the stick the engine already expects: bipeds.c reads
throttle.i as forward/back and throttle.j as sideways, picks the walk
animation from their signs, and scales each by the biped's own forward and
sideways speeds. */
static void jevin_remote_throttle(
	long unit_index,
	real forward,
	real side,
	real speed)
{
	real magnitude;

	if (jevin_remote_unit != unit_index && !jevin_remote_begin(unit_index))
		return;
	if (forward > 1.0f)
		forward = 1.0f;
	if (forward < -1.0f)
		forward = -1.0f;
	if (side > 1.0f)
		side = 1.0f;
	if (side < -1.0f)
		side = -1.0f;
	if (speed < 0.0f)
		speed = 0.0f;
	magnitude = (real)sqrt((double)(forward * forward + side * side)) * speed;
	if (magnitude > 3.0f)                        /* the engine asserts <= 3 */
	{
		speed *= 3.0f / magnitude;
	}
	jevin_remote_control.throttle.n[0] = forward * speed;
	jevin_remote_control.throttle.n[1] = side * speed;
	jevin_remote_control.throttle.n[2] = 0.0f;
	jevin_remote_apply();
	jevin_printf_quiet("JEVIN throttle %ld %.3f %.3f", unit_index,
		(double)jevin_remote_control.throttle.n[0],
		(double)jevin_remote_control.throttle.n[1]);
}

/* aim at a world point: the unit turns to look, and shoot, at it */
static void jevin_remote_aim(
	long unit_index,
	real x, real y, real z)
{
	struct object_datum *object;

	if (jevin_remote_unit != unit_index && !jevin_remote_begin(unit_index))
		return;
	object = object_try_and_get(unit_index);
	if (object == NULL)
		return;
	{
		real_vector3d direction;
		real_vector3d facing;

		direction.n[0] = x - object->object.position.n[0];
		direction.n[1] = y - object->object.position.n[1];
		direction.n[2] = z - object->object.position.n[2];
		if (normalize3d(&direction) != 0.0f)
		{
			jevin_remote_control.aiming_vector = direction;
			jevin_remote_control.looking_vector = direction;
			/* turn exactly, so the rifle points where the shot goes: a biped
			stops its turn within the map's own tolerance, which left him 26
			degrees off his target, and the standing threshold becomes about
			eight with this bit */
			jevin_remote_control.control_flags |= FLAG(_unit_control_exact_facing_bit);
			/* Turn the body too, not only the aim. A biped rotates by
			playing its turn animations, and those are chosen against the
			facing vector (bipeds.c, the standing-turn branch); aiming at
			something without facing it left him firing past it. The facing
			is flattened, because a biped stays upright whatever it is
			aiming at. */
			facing.n[0] = direction.n[0];
			facing.n[1] = direction.n[1];
			facing.n[2] = 0.0f;
			if (normalize3d(&facing) != 0.0f)
			{
				jevin_remote_control.facing_vector = facing;
			}
			jevin_remote_apply();
		}
	}
	jevin_printf_quiet("JEVIN aim %ld %.3f %.3f %.3f", unit_index,
		(double)x, (double)y, (double)z);
}

/* pull the trigger. The analogue value and the flag are both set, because the
engine reads them in different places. */
static void jevin_remote_trigger(
	long unit_index,
	long pressed)
{
	if (jevin_remote_unit != unit_index && !jevin_remote_begin(unit_index))
		return;
	if (pressed)
	{
		jevin_remote_control.primary_trigger = 1.0f;
		jevin_remote_control.control_flags |= FLAG(_unit_control_weapon_primary_trigger_bit);
	}
	else
	{
		jevin_remote_control.primary_trigger = 0.0f;
		jevin_remote_control.control_flags &= ~FLAG(_unit_control_weapon_primary_trigger_bit);
	}
	jevin_remote_apply();
	jevin_printf_quiet("JEVIN trigger %ld %d", unit_index, (int)pressed);
}

static boolean jevin_remote_button_bit(
	char const *name,
	word *bit)
{
	if (strcmp(name, "jump") == 0) { *bit = FLAG(_unit_control_jump_bit); return TRUE; }
	if (strcmp(name, "crouch") == 0) { *bit = FLAG(_unit_control_crouch_modifier_bit); return TRUE; }
	if (strcmp(name, "melee") == 0) { *bit = FLAG(_unit_control_user_animation1_bit); return TRUE; }
	if (strcmp(name, "action") == 0) { *bit = FLAG(_unit_control_action_bit); return TRUE; }
	if (strcmp(name, "reload") == 0) { *bit = FLAG(_unit_control_weapon_reload_bit); return TRUE; }
	if (strcmp(name, "grenade") == 0) { *bit = FLAG(_unit_control_throw_grenade_bit); return TRUE; }
	if (strcmp(name, "swap") == 0) { *bit = FLAG(_unit_control_swap_weapons_bit); return TRUE; }
	if (strcmp(name, "light") == 0) { *bit = FLAG(_unit_control_integrated_light_bit); return TRUE; }
	return FALSE;
}

static void jevin_remote_button(
	long unit_index,
	char const *name,
	long pressed)
{
	word bit = 0;

	if (!jevin_remote_button_bit(name, &bit))
	{
		jevin_printf_quiet("JEVIN button unknown %s", name);
		return;
	}
	if (jevin_remote_unit != unit_index && !jevin_remote_begin(unit_index))
		return;
	if (pressed)
		jevin_remote_control.control_flags |= bit;
	else
		jevin_remote_control.control_flags &= ~bit;
	jevin_remote_apply();
	jevin_printf_quiet("JEVIN button %ld %s %d", unit_index, name, (int)pressed);
}

/* choose a weapon slot in the unit's inventory */
static void jevin_remote_weapon(
	long unit_index,
	long slot)
{
	if (jevin_remote_unit != unit_index && !jevin_remote_begin(unit_index))
		return;
	if (slot < 0 || slot >= MAXIMUM_WEAPONS_PER_UNIT)
	{
		jevin_printf_quiet("JEVIN weapon slot_out_of_range %ld", slot);
		return;
	}
	jevin_remote_control.weapon_index = (short)slot;
	jevin_remote_apply();
	jevin_remote_control.weapon_index = NONE;
	jevin_printf_quiet("JEVIN weapon %ld %ld", unit_index, slot);
}

/* let go of everything: no throttle, no trigger, no buttons */
static void jevin_remote_stop(
	long unit_index)
{
	(void)unit_index;
	jevin_remote_reset_control();
	jevin_remote_apply();
	jevin_printf_quiet("JEVIN stop");
}

/* ---------- orders typed into the game console ---------- */

/* A ring of lines typed at the game console, waiting for the controlling
process to pick them up. This is how a person talks to Jevin without leaving
the game: `jevin_order wait here`, then in game he answers and does it. */
#define MAXIMUM_JEVIN_ORDERS 8
#define MAXIMUM_JEVIN_ORDER_LENGTH 160

static char jevin_order_text[MAXIMUM_JEVIN_ORDERS][MAXIMUM_JEVIN_ORDER_LENGTH];
static long jevin_order_next_write;
static long jevin_order_next_read;

static void jevin_telemetry_order(
	char const *text)
{
	long length = 0;
	long index = jevin_order_next_write;

	while ((text[length] == ' ' || text[length] == '\t') && text[length])
		length++;
	{
		char const *start = text + length;
		long used = 0;

		while (*start && *start != ')' && used < MAXIMUM_JEVIN_ORDER_LENGTH - 1)
		{
			jevin_order_text[index][used++] = *start++;
		}
		while (used > 0 && (jevin_order_text[index][used - 1] == ' ' || jevin_order_text[index][used - 1] == '\t'))
			used--;
		jevin_order_text[index][used] = '\0';
		if (used == 0)
		{
			jevin_printf_quiet("JEVIN order empty");
			return;
		}
	}
	jevin_order_next_write = (index + 1) % MAXIMUM_JEVIN_ORDERS;
	if (jevin_order_next_write == jevin_order_next_read)
		jevin_order_next_read = (jevin_order_next_read + 1) % MAXIMUM_JEVIN_ORDERS; /* drop the oldest */
	jevin_printf_quiet("JEVIN order taken %s", jevin_order_text[index]);
}

static void jevin_telemetry_poll(
	void)
{
	while (jevin_order_next_read != jevin_order_next_write)
	{
		jevin_printf_quiet("JEVIN order %s", jevin_order_text[jevin_order_next_read]);
		jevin_order_next_read = (jevin_order_next_read + 1) % MAXIMUM_JEVIN_ORDERS;
	}
	jevin_printf_quiet("JEVIN oend");
}
/* set an object team, or its vitality. Both exist for rehearsal: a reaction
that needs an enemy in view, or a wounded squadmate, is hard to arrange
otherwise without waiting for the level's own Covenant to walk over. */
static void jevin_telemetry_team(
	long index,
	long team)
{
	struct object_datum *object = object_try_and_get(index);

	if (object == NULL)
	{
		jevin_printf_quiet("JEVIN team missing %ld", index);
		return;
	}
	object->object.owner_team_index = (short)team;
	jevin_printf_quiet("JEVIN team %ld %d", index, (int)team);
}

static void jevin_telemetry_health(
	long index,
	real body,
	real shield)
{
	struct object_datum *object = object_try_and_get(index);

	if (object == NULL)
	{
		jevin_printf_quiet("JEVIN health missing %ld", index);
		return;
	}
	object->object.body_vitality = body;
	object->object.shield_vitality = shield;
	object->object.force_shield_update = TRUE;
	jevin_printf_quiet("JEVIN health %ld %.3f %.3f", index, (double)body, (double)shield);
}
/* clear the console's text from the screen. Whatever a command prints is
drawn over the game and does not fade, so a picture taken after a telemetry
dump is a picture of numbers. The first argument of console_printf is the
"clear" flag. */
static void jevin_telemetry_clear(
	void)
{
	console_printf(TRUE, "");
}

/* is a point inside the level? The same test the teleport makes, without
moving anything: a point in no structure cluster is outside the structure. */
static void jevin_telemetry_inside(
	real x, real y, real z)
{
	struct location location;
	real_point3d position;

	position.n[0] = x;
	position.n[1] = y;
	position.n[2] = z;
	scenario_location_from_point(&location, &position);
	if (location.cluster_index == NONE)
		jevin_printf_quiet("JEVIN inside no %.3f %.3f %.3f",
			(double)x, (double)y, (double)z);
	else
		jevin_printf_quiet("JEVIN inside yes %.3f %.3f %.3f %d",
			(double)x, (double)y, (double)z, (int)location.cluster_index);
}

boolean jevin_console_command(
	char const *expression)
{
	char const *rest;

	if ((rest = jevin_telemetry_word(expression, "pos")) != NULL ||
		(rest = jevin_telemetry_word(expression, "jevin_pos")) != NULL)
	{
		jevin_telemetry_pos();
		jevin_printf_quiet("JEVIN ok");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_state")) != NULL)
	{
		jevin_telemetry_state();
		jevin_printf_quiet("JEVIN ok");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_ground_at")) != NULL)
	{
		long index = NONE;
		real dx = 0.0f, dy = 0.0f, drop = 0.0f;

		if (jevin_telemetry_long(&rest, &index) && jevin_telemetry_real(&rest, &dx) &&
			jevin_telemetry_real(&rest, &dy) && jevin_telemetry_real(&rest, &drop))
			jevin_telemetry_ground_at(index, dx, dy, drop);
		else
			console_printf(FALSE, "JEVIN: jevin_ground_at <object> <dx> <dy> <drop>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_ground")) != NULL)
	{
		long index = NONE;
		real ahead = 0.0f, drop = 0.0f;

		if (jevin_telemetry_long(&rest, &index))
		{
			jevin_telemetry_real(&rest, &ahead);
			jevin_telemetry_real(&rest, &drop);
			jevin_telemetry_ground(index, ahead, drop);
		}
		else
			console_printf(FALSE, "JEVIN: jevin_ground <object> [ahead] [drop]");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_push")) != NULL)
	{
		long index = NONE;
		real vx = 0.0f, vy = 0.0f, vz = 0.0f;

		if (jevin_telemetry_long(&rest, &index) && jevin_telemetry_real(&rest, &vx) &&
			jevin_telemetry_real(&rest, &vy) && jevin_telemetry_real(&rest, &vz))
			jevin_telemetry_push(index, vx, vy, vz);
		else
			console_printf(FALSE, "JEVIN: jevin_push <object> <vx> <vy> <vz>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_colors")) != NULL)
	{
		long index = NONE;

		if (jevin_telemetry_long(&rest, &index))
			jevin_telemetry_colors(index);
		else
			console_printf(FALSE, "JEVIN: jevin_colors <object>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_color")) != NULL)
	{
		long index = NONE;
		real red = 0.0f, green = 0.0f, blue = 0.0f;

		if (jevin_telemetry_long(&rest, &index) && jevin_telemetry_real(&rest, &red) &&
			jevin_telemetry_real(&rest, &green) && jevin_telemetry_real(&rest, &blue))
			jevin_telemetry_color(index, red, green, blue);
		else
			console_printf(FALSE, "JEVIN: jevin_color <object> <r> <g> <b>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_console_draw")) != NULL)
	{
		long mode = 1;

		jevin_telemetry_long(&rest, &mode);
		if (mode < 0)
			mode = 0;
		if (mode > 2)
			mode = 2;
		terminal_output_mode = mode;
		console_printf(FALSE, "JEVIN console_draw %ld", mode);
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_probe")) != NULL)
	{
		long index = NONE;
		real dx = 1.0f, dy = 0.0f, length = 2.0f, height = 0.5f;

		if (jevin_telemetry_long(&rest, &index) && jevin_telemetry_real(&rest, &dx) &&
			jevin_telemetry_real(&rest, &dy))
		{
			jevin_telemetry_real(&rest, &length);
			/* the height the ray is cast at, so that a feeler can look over a
			slope instead of calling the ground a wall */
			jevin_telemetry_real(&rest, &height);
			jevin_telemetry_probe(index, dx, dy, length, height);
		}
		else
			console_printf(FALSE, "JEVIN: jevin_probe <object> <dx> <dy> [length] [height]");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_grid")) != NULL)
	{
		long index = NONE;
		real spacing = 1.0f;
		long count = 3;
		real drop = 8.0f;
		long layers = 2;

		if (jevin_telemetry_long(&rest, &index) && jevin_telemetry_real(&rest, &spacing) &&
			jevin_telemetry_long(&rest, &count))
		{
			jevin_telemetry_real(&rest, &drop);
			jevin_telemetry_long(&rest, &layers);
			jevin_telemetry_grid(index, spacing, count, drop, layers);
		}
		else
			console_printf(FALSE, "JEVIN: jevin_grid <unit> <spacing> <count> [drop] [layers]");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_ray")) != NULL)
	{
		long index = NONE;
		real x = 0.0f, y = 0.0f, z = 0.0f;

		if (jevin_telemetry_long(&rest, &index) && jevin_telemetry_real(&rest, &x) &&
			jevin_telemetry_real(&rest, &y) && jevin_telemetry_real(&rest, &z))
			jevin_telemetry_ray(index, x, y, z);
		else
			console_printf(FALSE, "JEVIN: jevin_ray <object> <x> <y> <z>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_weaponry")) != NULL)
	{
		long unit_index = NONE;

		if (jevin_telemetry_long(&rest, &unit_index))
			jevin_telemetry_weaponry(unit_index);
		else
			console_printf(FALSE, "JEVIN: jevin_weaponry <unit>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_wstate")) != NULL)
	{
		long unit_index = NONE;

		if (jevin_telemetry_long(&rest, &unit_index))
			jevin_telemetry_weapon_state(unit_index);
		else
			console_printf(FALSE, "JEVIN: jevin_wstate <unit>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_actor")) != NULL)
	{
		long unit_index = NONE;

		if (jevin_telemetry_long(&rest, &unit_index))
			jevin_actor_attach(unit_index);
		else
			console_printf(FALSE, "JEVIN: jevin_actor <unit>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_walk")) != NULL)
	{
		long unit_index = NONE;
		real dx = 0.0f, dy = 0.0f, speed = 1.0f;

		if (jevin_telemetry_long(&rest, &unit_index) && jevin_telemetry_real(&rest, &dx) &&
			jevin_telemetry_real(&rest, &dy))
		{
			jevin_telemetry_real(&rest, &speed);
			jevin_remote_walk(unit_index, dx, dy, speed);
		}
		else
			console_printf(FALSE, "JEVIN: jevin_walk <unit> <dx> <dy> [speed]");
		jevin_printf_quiet("JEVIN ok");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_throttle")) != NULL)
	{
		long unit_index = NONE;
		real forward = 0.0f, side = 0.0f, speed = 1.0f;

		if (jevin_telemetry_long(&rest, &unit_index) &&
			jevin_telemetry_real(&rest, &forward) && jevin_telemetry_real(&rest, &side))
		{
			jevin_telemetry_real(&rest, &speed);
			jevin_remote_throttle(unit_index, forward, side, speed);
		}
		else
			console_printf(FALSE, "JEVIN: jevin_throttle <unit> <forward> <side> [speed]");
		jevin_printf_quiet("JEVIN ok");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_aim")) != NULL)
	{
		long unit_index = NONE;
		real x = 0.0f, y = 0.0f, z = 0.0f;

		if (jevin_telemetry_long(&rest, &unit_index) && jevin_telemetry_real(&rest, &x) &&
			jevin_telemetry_real(&rest, &y) && jevin_telemetry_real(&rest, &z))
			jevin_remote_aim(unit_index, x, y, z);
		else
			console_printf(FALSE, "JEVIN: jevin_aim <unit> <x> <y> <z>");
		jevin_printf_quiet("JEVIN ok");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_trigger")) != NULL)
	{
		long unit_index = NONE;
		long pressed = 0;

		if (jevin_telemetry_long(&rest, &unit_index) && jevin_telemetry_long(&rest, &pressed))
			jevin_remote_trigger(unit_index, pressed);
		else
			console_printf(FALSE, "JEVIN: jevin_trigger <unit> <0|1>");
		jevin_printf_quiet("JEVIN ok");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_button")) != NULL)
	{
		long unit_index = NONE;
		char name[24];
		long used = 0;
		long pressed = 0;

		while (*rest == ' ' || *rest == '\t')
			rest++;
		if (jevin_telemetry_long(&rest, &unit_index))
		{
			while (*rest == ' ' || *rest == '\t')
				rest++;
			while (*rest && *rest != ' ' && *rest != '\t' && *rest != ')' &&
				used < (long)sizeof(name) - 1)
				name[used++] = *(rest++);
			name[used] = '\0';
			while (*rest == ' ' || *rest == '\t')
				rest++;
			if (*rest)
				jevin_telemetry_long(&rest, &pressed);
			jevin_remote_button(unit_index, name, pressed);
		}
		else
			console_printf(FALSE, "JEVIN: jevin_button <unit> <name> <0|1>");
		jevin_printf_quiet("JEVIN ok");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_weapon")) != NULL)
	{
		long unit_index = NONE;
		long slot = 0;

		if (jevin_telemetry_long(&rest, &unit_index) && jevin_telemetry_long(&rest, &slot))
			jevin_remote_weapon(unit_index, slot);
		else
			console_printf(FALSE, "JEVIN: jevin_weapon <unit> <slot>");
		jevin_printf_quiet("JEVIN ok");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_release")) != NULL)
	{
		jevin_remote_stop(0);
		jevin_printf_quiet("JEVIN ok");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_poll")) != NULL)
	{
		jevin_telemetry_poll();
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_talk")) != NULL ||
		(rest = jevin_telemetry_word(expression, "jt")) != NULL)
	{
		/* conversation, not an order: queued in the same ring with a prefix so
		the controlling process can tell the two apart */
		char buffer[MAXIMUM_JEVIN_ORDER_LENGTH];
		long used = 0;

		while (*rest == ' ' || *rest == '\t')
			rest++;
		buffer[used++] = 't';
		buffer[used++] = 'a';
		buffer[used++] = 'l';
		buffer[used++] = 'k';
		buffer[used++] = ' ';
		while (*rest && *rest != ')' && used < MAXIMUM_JEVIN_ORDER_LENGTH - 1)
			buffer[used++] = *(rest++);
		while (used > 5 && (buffer[used - 1] == ' ' || buffer[used - 1] == '\t'))
			used--;
		buffer[used] = '\0';
		if (used <= 5)
			jevin_printf_quiet("JEVIN talk empty");
		else
			jevin_telemetry_order(buffer);
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_order")) != NULL ||
		(rest = jevin_telemetry_word(expression, "jo")) != NULL)
	{
		jevin_telemetry_order(rest);
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_weapons")) != NULL)
	{
		jevin_telemetry_weapons();
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_give")) != NULL)
	{
		long unit_index = NONE;
		long template_index = NONE;

		if (jevin_telemetry_long(&rest, &unit_index) && jevin_telemetry_long(&rest, &template_index))
			jevin_telemetry_give(unit_index, template_index);
		else
			console_printf(FALSE, "JEVIN: jevin_give <unit> <template_object>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_team")) != NULL)
	{
		long index = NONE;
		long team = 0;

		if (jevin_telemetry_long(&rest, &index) && jevin_telemetry_long(&rest, &team))
			jevin_telemetry_team(index, team);
		else
			console_printf(FALSE, "JEVIN: jevin_team <object> <team>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_health")) != NULL)
	{
		long index = NONE;
		real body = 1.0f, shield = 1.0f;

		if (jevin_telemetry_long(&rest, &index) && jevin_telemetry_real(&rest, &body) &&
			jevin_telemetry_real(&rest, &shield))
			jevin_telemetry_health(index, body, shield);
		else
			console_printf(FALSE, "JEVIN: jevin_health <object> <body> <shield>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_clear")) != NULL)
	{
		jevin_telemetry_clear();
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_inside")) != NULL)
	{
		real x = 0.0f, y = 0.0f, z = 0.0f;

		if (jevin_telemetry_real(&rest, &x) && jevin_telemetry_real(&rest, &y) &&
			jevin_telemetry_real(&rest, &z))
			jevin_telemetry_inside(x, y, z);
		else
			console_printf(FALSE, "JEVIN: jevin_inside <x> <y> <z>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_encounters")) != NULL)
	{
		jevin_telemetry_encounters();
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_snapshot")) != NULL)
	{
		real radius = 0.0f;
		long reference = NONE;

		jevin_telemetry_real(&rest, &radius);
		jevin_telemetry_long(&rest, &reference);
		jevin_telemetry_snapshot(radius, reference);
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_objects")) != NULL)
	{
		real radius = 0.0f;
		long reference = NONE;

		jevin_telemetry_real(&rest, &radius);
		jevin_telemetry_long(&rest, &reference);
		jevin_telemetry_objects(radius, reference);
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_spawn")) != NULL)
	{
		long template = NONE;
		real x = 0.0f, y = 0.0f, z = 0.0f;

		if (jevin_telemetry_long(&rest, &template))
		{
			boolean at_point = jevin_telemetry_real(&rest, &x) &&
				jevin_telemetry_real(&rest, &y) && jevin_telemetry_real(&rest, &z);

			jevin_telemetry_spawn(template, at_point, x, y, z);
		}
		else
			console_printf(FALSE, "JEVIN: jevin_spawn <template_object> [x y z]");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_teleport_to")) != NULL)
	{
		long index = NONE;
		long target = NONE;

		if (jevin_telemetry_long(&rest, &index) && jevin_telemetry_long(&rest, &target))
			jevin_telemetry_teleport(index, target, 0.0f, 0.0f, 0.0f);
		else
			console_printf(FALSE, "JEVIN: jevin_teleport_to <object> <object>");
		return TRUE;
	}
	if ((rest = jevin_telemetry_word(expression, "jevin_teleport")) != NULL)
	{
		long index = NONE;
		real x = 0.0f, y = 0.0f, z = 0.0f;

		if (jevin_telemetry_long(&rest, &index) && jevin_telemetry_real(&rest, &x) &&
			jevin_telemetry_real(&rest, &y) && jevin_telemetry_real(&rest, &z))
			jevin_telemetry_teleport(index, NONE, x, y, z);
		else
			console_printf(FALSE, "JEVIN: jevin_teleport <object> <x> <y> <z>");
		return TRUE;
	}
	return FALSE;
}
