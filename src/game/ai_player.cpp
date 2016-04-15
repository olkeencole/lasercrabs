#include "ai_player.h"
#include "mersenne/mersenne-twister.h"
#include "usernames.h"
#include "entities.h"
#include "console.h"
#include "awk.h"
#include "minion.h"
#include "bullet/src/BulletCollision/NarrowPhaseCollision/btRaycastCallback.h"
#include "minion.h"
#include "noise.h"
#if DEBUG_AI_CONTROL
#include "render/views.h"
#endif

namespace VI
{


#define MAX_AIM_TIME 2.5f

PinArray<AIPlayer, MAX_AI_PLAYERS> AIPlayer::list;

AIPlayer::AIPlayer(PlayerManager* m)
	: manager(m), revision()
{
	strcpy(manager.ref()->username, Usernames::all[mersenne::rand_u32() % Usernames::count]);
	m->spawn.link<AIPlayer, &AIPlayer::spawn>(this);
}

void AIPlayer::spawn()
{
	Entity* e = World::create<AwkEntity>(manager.ref()->team.ref()->team());

	e->add<PlayerCommon>(manager.ref());

	manager.ref()->entity = e;

	e->add<AIPlayerControl>(this);
	Vec3 pos;
	Quat rot;
	manager.ref()->team.ref()->player_spawn.ref()->absolute(&pos, &rot);
	pos += Vec3(0, 0, PLAYER_SPAWN_RADIUS * 0.5f); // spawn it around the edges
	e->get<Transform>()->absolute(pos, rot);
}

AIPlayerControl::AIPlayerControl(AIPlayer* p)
	: player(p),
	path_index(),
	memory(),
	behavior_callback(),
	path_request_active(),
	path_priority(),
	path(),
	loop_high_level(),
	loop_low_level(),
	target(),
	hit_target()
{
#if DEBUG_AI_CONTROL
	camera = Camera::add();
#endif
}

void AIPlayerControl::awake()
{
#if DEBUG_AI_CONTROL
	camera->fog = false;
	camera->team = (u8)get<AIAgent>()->team;
	camera->mask = 1 << camera->team;
	camera->range = AWK_MAX_DISTANCE;
#endif
	link<&AIPlayerControl::awk_attached>(get<Awk>()->attached);
	link_arg<Entity*, &AIPlayerControl::awk_hit>(get<Awk>()->hit);
	link<&AIPlayerControl::awk_detached>(get<Awk>()->detached);
}

b8 AIPlayerControl::in_range(const Vec3& p, r32 range) const
{
	Vec3 to_entity = p - get<Transform>()->absolute_pos();
	r32 distance_squared = to_entity.length_squared();
	return distance_squared < range * range;
}

AIPlayerControl::~AIPlayerControl()
{
#if DEBUG_AI_CONTROL
	camera->remove();
#endif
	loop_high_level->~Repeat();
	loop_low_level->~Parallel();
}

void AIPlayerControl::awk_attached()
{
	aim_timer = 0.0f;
	if (path_index < path.length)
	{
		if ((path[path_index] - get<Awk>()->center()).length_squared() < (AWK_RADIUS * 2.0f) * (AWK_RADIUS * 2.0f))
			path_index++;
	}
}

void AIPlayerControl::awk_detached()
{
	hit_target = false;
	aim_timer = 0.0f;
}

void AIPlayerControl::awk_hit(Entity* e)
{
	hit_target = true;
}

void AIPlayerControl::set_target(Target* t, Behavior* callback)
{
	aim_timer = 0.0f;
	target = t;
	hit_target = false;
	path.length = 0;
	behavior_callback = callback;
}

void AIPlayerControl::pathfind(const Vec3& p, Behavior* callback, s8 priority)
{
	aim_timer = 0.0f;
	path.length = 0;
	behavior_callback = callback;
	path_priority = priority;
	path_request_active = true;
	AI::awk_pathfind(get<Transform>()->absolute_pos(), p, ObjectLinkEntryArg<AIPlayerControl, const AI::Path&, &AIPlayerControl::set_path>(id()));
}

void AIPlayerControl::random_path(Behavior* callback)
{
	aim_timer = 0.0f;
	path.length = 0;
	behavior_callback = callback;
	path_priority = 0;
	path_request_active = true;
	AI::awk_random_path(get<Transform>()->absolute_pos(), ObjectLinkEntryArg<AIPlayerControl, const AI::Path&, &AIPlayerControl::set_path>(id()));
}

b8 AIPlayerControl::resume_loop_high_level()
{
	if (!loop_high_level->active())
		loop_high_level->run();
	return true;
}

void AIPlayerControl::set_path(const AI::Path& p)
{
	path_request_active = false;
	aim_timer = 0.0f;
	path = p;
	path_index = 0;
}

b8 AIPlayerControl::go(const Vec3& target)
{
	Vec3 pos;
	Quat quat;
	get<Transform>()->absolute(&pos, &quat);

	Vec3 wall_normal = quat * Vec3(0, 0, 1);
	Vec3 to_goal = Vec3::normalize(target - pos);
	{
		const r32 random_range = 0.01f;
		to_goal = Quat::euler(mersenne::randf_oo() * random_range, mersenne::randf_oo() * random_range, mersenne::randf_oo() * random_range) * to_goal;
		if (get<Awk>()->can_go(to_goal))
		{
			get<Awk>()->detach(to_goal);
			return true;
		}
	}

	return false;
}

#define LOOK_SPEED 2.0f

b8 AIPlayerControl::aim_and_shoot(const Update& u, const Vec3& target, b8 exact)
{
	PlayerCommon* common = get<PlayerCommon>();
	if (common->cooldown == 0.0f)
		aim_timer += u.time.delta;

	Vec3 pos = get<Awk>()->center();
	Vec3 to_target = Vec3::normalize(target - pos);
	Vec3 wall_normal = common->attach_quat * Vec3(0, 0, 1);

	r32 target_angle_horizontal;
	{
		target_angle_horizontal = LMath::closest_angle(atan2(to_target.x, to_target.z), common->angle_horizontal);
		r32 dir_horizontal = target_angle_horizontal > common->angle_horizontal ? 1.0f : -1.0f;

		{
			// make sure we don't try to turn through the wall
			r32 half_angle = (common->angle_horizontal + target_angle_horizontal) * 0.5f;
			if ((Quat::euler(0, half_angle, 0) * Vec3(0, 0, 1)).dot(wall_normal) < -0.5f)
				dir_horizontal *= -1.0f; // go the other way
		}

		common->angle_horizontal = dir_horizontal > 0.0f
			? vi_min(target_angle_horizontal, common->angle_horizontal + vi_max(0.2f, target_angle_horizontal - common->angle_horizontal) * LOOK_SPEED * u.time.delta)
			: vi_max(target_angle_horizontal, common->angle_horizontal + vi_min(-0.2f, target_angle_horizontal - common->angle_horizontal) * LOOK_SPEED * u.time.delta);
		common->angle_horizontal = LMath::angle_range(common->angle_horizontal);
	}

	r32 target_angle_vertical;
	{
		target_angle_vertical = LMath::closest_angle(atan2(-to_target.y, Vec2(to_target.x, to_target.z).length()), common->angle_vertical);
		r32 dir_vertical = target_angle_vertical > common->angle_vertical ? 1.0f : -1.0f;

		{
			// make sure we don't try to turn through the wall
			r32 half_angle = (common->angle_vertical + target_angle_vertical) * 0.5f;
			if (half_angle < -PI * 0.5f
				|| half_angle > PI * 0.5f
				|| (Quat::euler(half_angle, common->angle_horizontal, 0) * Vec3(0, 0, 1)).dot(wall_normal) < -0.5f)
				dir_vertical *= -1.0f; // go the other way
		}

		common->angle_vertical = dir_vertical > 0.0f
			? vi_min(target_angle_vertical, common->angle_vertical + vi_max(0.2f, target_angle_vertical - common->angle_vertical) * LOOK_SPEED * u.time.delta)
			: vi_max(target_angle_vertical, common->angle_vertical + vi_min(-0.2f, target_angle_vertical - common->angle_vertical) * LOOK_SPEED * u.time.delta);
		common->angle_vertical = LMath::angle_range(common->angle_vertical);
	}

	common->angle_vertical = LMath::clampf(common->angle_vertical, PI * -0.495f, PI * 0.495f);
	common->clamp_rotation(wall_normal, 0.5f);

	if (common->cooldown == 0.0f
		&& common->angle_horizontal == target_angle_horizontal
		&& common->angle_vertical == target_angle_vertical)
	{
		Vec3 look_dir = common->look_dir();
		Vec3 hit;
		if (get<Awk>()->can_go(look_dir, &hit))
		{
			if (!exact || (hit - target).length() < AWK_RADIUS * 2.0f) // make sure we're actually going to land at the right spot
			{
				if (get<Awk>()->detach(look_dir))
					return true;
			}
		}
	}
	
	return false;
}

b8 health_pickup_filter(const AIPlayerControl* control, const HealthPickup* h)
{
	return h->owner.ref() == nullptr;
}

b8 minion_filter(const AIPlayerControl* control, const MinionAI* m)
{
	return m->get<AIAgent>()->team != control->get<AIAgent>()->team;
}

b8 awk_filter(const AIPlayerControl* control, const Awk* a)
{
	return a->get<AIAgent>()->team != control->get<AIAgent>()->team;
}

b8 minion_spawn_filter(const AIPlayerControl* control, const MinionSpawn* m)
{
	return m->minion.ref() == nullptr;
}

template<typename T> b8 default_filter(const AIPlayerControl* control, const T* t)
{
	return true;
}

void AIPlayerControl::update(const Update& u)
{
	if (get<Transform>()->parent.ref())
	{
		if (!loop_high_level)
		{
			loop_high_level = Repeat::alloc
			(
				Succeed::alloc
				(
					Sequence::alloc
					(
						Select::alloc
						(
							Sequence::alloc
							(
								Invert::alloc(Execute::alloc()->method<Health, &Health::is_full>(get<Health>())), // make sure we need health
								AIBehaviors::Find<HealthPickup>::alloc(1, &health_pickup_filter)
							),
							AIBehaviors::Find<MinionAI>::alloc(1, &minion_filter),
							AIBehaviors::Find<MinionSpawn>::alloc(1, &minion_spawn_filter),
							AIBehaviors::Find<Awk>::alloc(1, &awk_filter),
							AIBehaviors::RandomPath::alloc()
						),
						Delay::alloc(1.0f)
					)
				)
			);
			loop_high_level->set_context(this);
			loop_high_level->run();


			loop_low_level = Parallel::alloc
			(
				Repeat::alloc // memory update loop
				(
					Sequence::alloc
					(
						Delay::alloc(0.1f),
						Execute::alloc()->method<AIPlayerControl, &AIPlayerControl::update_memory<HealthPickup, &health_pickup_filter> >(this),
						Execute::alloc()->method<AIPlayerControl, &AIPlayerControl::update_memory<MinionAI, &minion_filter> >(this),
						Execute::alloc()->method<AIPlayerControl, &AIPlayerControl::update_memory<MinionSpawn, &minion_spawn_filter> >(this),
						Execute::alloc()->method<AIPlayerControl, &AIPlayerControl::update_memory<Awk, &awk_filter> >(this)
					)
				),

				Repeat::alloc // reaction loop
				(
					Succeed::alloc
					(
						Sequence::alloc
						(
							Delay::alloc(0.3f),
							Select::alloc // if any of these succeed, they will abort the high level loop
							(
								Sequence::alloc
								(
									Invert::alloc(Execute::alloc()->method<Health, &Health::is_full>(get<Health>())), // make sure we need health
									AIBehaviors::React<HealthPickup>::alloc(0, 1, &health_pickup_filter)
								),
								AIBehaviors::React<MinionAI>::alloc(0, 1, &default_filter<MinionAI>),
								AIBehaviors::React<MinionSpawn>::alloc(0, 1, &minion_spawn_filter),
								AIBehaviors::React<Awk>::alloc(0, 1, &awk_filter)
							),
							Execute::alloc()->method<AIPlayerControl, &AIPlayerControl::resume_loop_high_level>(this) // restart the high level loop if necessary
						)
					)
				)
			);
			loop_low_level->set_context(this);
			loop_low_level->run();
		}

		if (target.ref())
		{
			Vec3 intersection;
			if (get<Awk>()->can_hit(target.ref(), &intersection))
				aim_and_shoot(u, intersection, false);
			else
				task_done(false); // we can't hit it
		}
		else if (path_index < path.length)
		{
			// look at next target
			if (aim_timer > MAX_AIM_TIME)
				task_done(false); // we can't hit it
			else
				aim_and_shoot(u, path[path_index], true);
		}
		else
		{
			// look randomly
			PlayerCommon* common = get<PlayerCommon>();
			r32 offset = Game::time.total * 0.2f;
			common->angle_horizontal += noise::sample3d(Vec3(offset)) * LOOK_SPEED * 2.0f * u.time.delta;
			common->angle_vertical += noise::sample3d(Vec3(offset + 64)) * LOOK_SPEED * u.time.delta;
			common->angle_vertical = LMath::clampf(common->angle_vertical, PI * -0.495f, PI * 0.495f);
			common->clamp_rotation(common->attach_quat * Vec3(0, 0, 1), 0.5f);
		}
	}

	if (behavior_callback && !path_request_active && (!target.ref() && path_index >= path.length) || (target.ref() && hit_target))
		task_done(hit_target || path.length > 0);

#if DEBUG_AI_CONTROL
	// update camera
	s32 player_count = LocalPlayer::list.count() + AIPlayer::list.count();
	Camera::ViewportBlueprint* viewports = Camera::viewport_blueprints[player_count - 1];
	Camera::ViewportBlueprint* blueprint = &viewports[LocalPlayer::list.count() + player.id];

	camera->viewport =
	{
		Vec2((s32)(blueprint->x * (r32)u.input->width), (s32)(blueprint->y * (r32)u.input->height)),
		Vec2((s32)(blueprint->w * (r32)u.input->width), (s32)(blueprint->h * (r32)u.input->height)),
	};
	r32 aspect = camera->viewport.size.y == 0 ? 1 : (r32)camera->viewport.size.x / (r32)camera->viewport.size.y;
	camera->perspective((80.0f * PI * 0.5f / 180.0f), aspect, 0.02f, Skybox::far_plane);
	camera->rot = Quat::euler(0.0f, get<PlayerCommon>()->angle_horizontal, get<PlayerCommon>()->angle_vertical);
	camera->range = AWK_MAX_DISTANCE;
	camera->wall_normal = camera->rot.inverse() * ((get<Transform>()->absolute_rot() * get<Awk>()->lerped_rotation) * Vec3(0, 0, 1));
	camera->pos = get<Awk>()->center();
#endif
}

void AIPlayerControl::task_done(b8 success)
{
	Behavior* cb = behavior_callback;
	behavior_callback = nullptr;
	path_priority = 0;
	path.length = 0;
	target = nullptr;
	if (cb)
		cb->done(success);
}

namespace AIBehaviors
{

void RandomPath::run()
{
	active(true);
	control->random_path(this);
}

void update_active(const Update& u)
{
}

}

}