#include "localization.h"
#include "player.h"
#include "drone.h"
#include "data/components.h"
#include "entities.h"
#include "render/render.h"
#include "input.h"
#include "common.h"
#include "console.h"
#include "bullet/src/BulletCollision/NarrowPhaseCollision/btRaycastCallback.h"
#include "physics.h"
#include "asset/lookup.h"
#include "asset/mesh.h"
#include "asset/shader.h"
#include "asset/texture.h"
#include "asset/animation.h"
#include "asset/armature.h"
#include "asset/font.h"
#include "ease.h"
#include "render/skinned_model.h"
#include "render/views.h"
#include "audio.h"
#include "asset/Wwise_IDs.h"
#include "game.h"
#include "minion.h"
#include "walker.h"
#include "data/animator.h"
#include "mersenne/mersenne-twister.h"
#include "parkour.h"
#include "noise.h"
#include "settings.h"
#if DEBUG_AI_CONTROL
#include "ai_player.h"
#endif
#include "scripts.h"
#include "net.h"
#include "team.h"
#include "overworld.h"
#include "load.h"
#include "asset/level.h"
#include "data/unicode.h"

namespace VI
{


#define DEBUG_NET_SYNC 0

#define fov_zoom (35.0f * PI * 0.5f / 180.0f)
#define fov_sniper (17.5f * PI * 0.5f / 180.0f)
#define zoom_speed_multiplier 0.25f
#define zoom_speed_multiplier_sniper 0.15f
#define zoom_speed (1.0f / 0.15f)
#define speed_mouse (0.05f / 60.0f)
#define speed_joystick 5.0f
#define gamepad_rotation_acceleration (1.0f / 0.4f)
#define msg_time 0.75f
#define camera_shake_time 0.6f
#define arm_angle_offset -0.2f

#define NOTIFICATION_TIME_HIDDEN 4.0f
#define NOTIFICATION_TIME (6.0f + NOTIFICATION_TIME_HIDDEN)
#define LOG_TIME 4.0f
#define CHAT_TIME 10.0f
#define INTERACT_TIME 2.5f
#define INTERACT_LERP_ROTATION_SPEED 5.0f
#define INTERACT_LERP_TRANSLATION_SPEED 10.0f
#define EMOTE_TIMEOUT 3.0f
#define KILL_POPUP_TIME 4.0f

#define HP_BOX_SIZE (Vec2(UI_TEXT_SIZE_DEFAULT) * UI::scale)
#define HP_BOX_SPACING (8.0f * UI::scale)

#define MAP_VIEW_ROT Quat::look(Vec3(0, -1, 0))
#define MAP_VIEW_POS Vec3(0, 90, 0)
#define MAP_VIEW_NEAR 30.0f
#define MAP_VIEW_FAR 200.0f

r32 hp_width(u8 hp, s8 shield, r32 scale = 1.0f)
{
	const Vec2 box_size = HP_BOX_SIZE;
	return scale * ((shield + (hp - 1)) * (box_size.x + HP_BOX_SPACING) - HP_BOX_SPACING);
}

inline b8 pvp_colors()
{
	return Settings::pvp_color_scheme == Settings::PvpColorScheme::Normal;
}

void PlayerHuman::camera_setup_drone(Drone* drone, Camera* camera, Vec3* camera_center, r32 offset)
{
	Quat abs_rot;
	Vec3 abs_pos;
	drone->get<Transform>()->absolute(&abs_pos, &abs_rot);

	Vec3 final_camera_center;
	{
		Vec3 lerped_pos = drone->camera_center();
		if (camera_center)
		{
			r32 smoothness;
			if (drone->state() == Drone::State::Crawl)
				smoothness = vi_max(0.0f, DRONE_CAMERA_SMOOTH_TIME - (Game::time.total - drone->attach_time));
			else
				smoothness = 1.0f;

			if (smoothness == 0.0f)
				*camera_center = lerped_pos;
			else
				*camera_center += (lerped_pos - *camera_center) * vi_min(1.0f, LMath::lerpf(Ease::cubic_in_out<r32>(smoothness), 250.0f, 3.0f) * Game::time.delta);

			final_camera_center = *camera_center;
		}
		else
			final_camera_center = lerped_pos;
	}

	Vec3 abs_offset = camera->rot * Vec3(0, 0, -offset);
	camera->pos = final_camera_center + abs_offset;
	Vec3 camera_pos_final = abs_pos + abs_offset;
	Vec3 abs_wall_normal;

	b8 attached = drone->get<Transform>()->parent.ref();
	if (attached)
	{
		abs_wall_normal = abs_rot * Vec3(0, 0, 1);
		camera_pos_final += abs_wall_normal * 0.5f;
	}
	else
		abs_wall_normal = camera->rot * Vec3(0, 0, 1);

	Quat rot_inverse = camera->rot.inverse();

	camera->range_center = rot_inverse * (abs_pos - camera->pos);
	camera->range = drone->range();
	camera->flag(CameraFlagColors | CameraFlagFog, pvp_colors());

	Vec3 wall_normal_viewspace = rot_inverse * abs_wall_normal;
	camera->clip_planes[0].redefine(wall_normal_viewspace, camera->range_center + wall_normal_viewspace * -DRONE_RADIUS);
	camera->flag(CameraFlagCullBehindWall, abs_wall_normal.dot(camera_pos_final - abs_pos) < -DRONE_RADIUS + 0.02f); // camera is behind wall; set clip plane to wall
	camera->cull_range = camera->range_center.length();

	if (attached)
		camera->cull_center = Vec3(0, 0, offset);
	else
	{
		// blend cull radius down to zero as we fly away from the wall
		r32 t = Game::time.total - drone->attach_time;
		const r32 blend_time = 0.2f;
		if (t < blend_time)
		{
			r32 blend = 1.0f - (t / blend_time);
			camera->cull_range *= blend;
			camera->cull_center = Vec3(0, 0, offset);
		}
		else
		{
			camera->cull_range = 0.0f;
			camera->flag(CameraFlagCullBehindWall, false);
		}
	}
}

b8 PlayerHuman::players_on_same_client(const Entity* a, const Entity* b)
{
#if SERVER
	return a->has<PlayerControlHuman>()
		&& b->has<PlayerControlHuman>()
		&& Net::Server::client_id(a->get<PlayerControlHuman>()->player.ref()) == Net::Server::client_id(b->get<PlayerControlHuman>()->player.ref());
#else
	return true;
#endif
}

s32 PlayerHuman::count_local()
{
	s32 count = 0;
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
#if !SERVER
		if (i.item()->local() || Net::Client::replay_mode() == Net::Client::ReplayMode::Replaying)
#else
		if (i.item()->local())
#endif
			count++;
	}
	return count;
}

PlayerHuman* PlayerHuman::for_camera(const Camera* camera)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->camera.ref() == camera)
			return i.item();
	}
	return nullptr;
}

PlayerHuman* PlayerHuman::for_gamepad(s8 gamepad)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->local() && i.item()->gamepad == gamepad)
			return i.item();
	}
	return nullptr;
}

s32 PlayerHuman::count_local_before(PlayerHuman* h)
{
	s32 count = 0;
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->gamepad < h->gamepad)
		{
#if !SERVER
			if (i.item()->local() || Net::Client::replay_mode() == Net::Client::ReplayMode::Replaying)
#else
			if (i.item()->local())
#endif
				count++;
		}
	}
	return count;
}

Vec2 PlayerHuman::camera_topdown_movement(const Update& u, s8 gamepad, const Quat& rotation)
{
	Vec2 movement(0, 0);

	b8 keyboard = false;

	// buttons/keys
	{
		if ((u.input->get(Controls::Left, gamepad) && !u.last_input->get(Controls::Left, gamepad))
			|| (u.input->get(Controls::Right, gamepad) && !u.last_input->get(Controls::Right, gamepad))
			|| (u.input->get(Controls::Forward, gamepad) && !u.last_input->get(Controls::Forward, gamepad))
			|| (u.input->get(Controls::Backward, gamepad) && !u.last_input->get(Controls::Backward, gamepad)))
		{
			keyboard = true;
			if (u.input->get(Controls::Left, gamepad))
				movement.x -= 1.0f;
			if (u.input->get(Controls::Right, gamepad))
				movement.x += 1.0f;
			if (u.input->get(Controls::Forward, gamepad))
				movement.y -= 1.0f;
			if (u.input->get(Controls::Backward, gamepad))
				movement.y += 1.0f;
		}
	}

	// joysticks
	{
		Vec2 last_joystick(u.last_input->gamepads[gamepad].left_x, u.last_input->gamepads[gamepad].left_y);
		Input::dead_zone(&last_joystick.x, &last_joystick.y, UI_JOYSTICK_DEAD_ZONE);
		Vec2 current_joystick(u.input->gamepads[gamepad].left_x, u.input->gamepads[gamepad].left_y);
		Input::dead_zone(&current_joystick.x, &current_joystick.y, UI_JOYSTICK_DEAD_ZONE);

		if (last_joystick.length_squared() == 0.0f
			&& current_joystick.length_squared() > 0.0f)
			movement += current_joystick;
	}

	r32 movement_amount = movement.length();
	if (movement_amount > 0.0f)
	{
		// transitioning from one zone to another
		movement /= movement_amount; // normalize
		Vec3 movement3d = rotation * Vec3(-movement.x, -movement.y, 0);

		// raycast against the +y plane
		Vec3 ray = rotation * Vec3(0, 0, 1);
		r32 d = -movement3d.y / ray.y;
		movement3d += ray * d;

		movement = Vec2(movement3d.x, movement3d.z);
		movement.normalize();
		movement *= movement_amount;
	}

	return movement;
}

PlayerHuman::PlayerHuman(b8 local, s8 g)
	: gamepad(g),
	camera(),
	kill_popups(),
	msg_text(),
	msg_timer(0.0f),
	menu(),
	angle_horizontal(),
	angle_vertical(),
	menu_state(),
	kill_cam_rot(),
	camera_center(),
	rumble(),
	animation_time(),
	upgrade_last_visit_highest_available(Upgrade::None),
	score_summary_scroll(),
	spectate_index(),
	killed_by(),
	spawn_animation_timer(),
	last_supported(),
	audio_log_prompt_timer(),
	audio_log(AssetNull),
	energy_notification_accumulator(),
#if SERVER
	afk_timer(AFK_TIME),
#endif
	flags(local ? FlagLocal : 0),
	chat_field(),
	emote_category(EmoteCategory::None),
	chat_focus(),
	ability_upgrade_slot()
{
	menu.scroll.size = 10;
	if (local)
		uuid = Game::session.local_player_uuids[gamepad];
}

void PlayerHuman::awake()
{
	get<PlayerManager>()->spawn.link<PlayerHuman, const SpawnPosition&, &PlayerHuman::spawn>(this);
	get<PlayerManager>()->upgrade_completed.link<PlayerHuman, Upgrade, &PlayerHuman::upgrade_completed>(this);

	if (local()
#if !SERVER
		|| Net::Client::replay_mode() == Net::Client::ReplayMode::Replaying
#endif
		)
	{
		AI::Team team = get<PlayerManager>()->team.ref()->team();
		Audio::listener_enable(gamepad, team);

		camera = Camera::add(gamepad);
		camera.ref()->team = s8(team);
		camera.ref()->mask = 1 << camera.ref()->team;
		camera.ref()->flag(CameraFlagColors | CameraFlagFog, pvp_colors());

		camera.ref()->pos = MAP_VIEW_POS;
		camera.ref()->rot = kill_cam_rot = MAP_VIEW_ROT;
		camera.ref()->perspective(Settings::effective_fov(), MAP_VIEW_NEAR, MAP_VIEW_FAR);
	}

	if (!get<PlayerManager>()->flag(PlayerManager::FlagCanSpawn)
		&& Game::session.type == SessionType::Multiplayer
		&& (Team::match_state == Team::MatchState::Waiting || Team::match_state == Team::MatchState::TeamSelect)
		&& !Game::level.local
		&& local())
	{
		Menu::teams_select_match_start_init(this);
	}
}

PlayerHuman::~PlayerHuman()
{
	if (camera.ref())
	{
		camera.ref()->remove();
		camera = nullptr;
		Audio::listener_disable(gamepad);
	}
#if SERVER
	Net::Server::player_deleting(this);
#endif
}

void PlayerHuman::kill_popup(PlayerManager* victim)
{
	kill_popups.add({ KILL_POPUP_TIME, victim });
}

void PlayerHuman::team_set(AI::Team t)
{
	Camera* c = camera.ref();
	if (c)
	{
		c->team = t;
		c->mask = 1 << t;
	}
}

void PlayerHuman::rumble_add(r32 r)
{
	rumble = vi_max(rumble, r);
}

PlayerHuman::UIMode PlayerHuman::ui_mode() const
{
	if (Game::level.noclip)
		return UIMode::Noclip;
	else if (menu_state != Menu::State::Hidden)
		return UIMode::Pause;
	else if (Team::match_state == Team::MatchState::Done)
		return UIMode::PvpGameOver;
	else if (Team::match_state == Team::MatchState::Waiting || Team::match_state == Team::MatchState::TeamSelect)
		return UIMode::PvpSelectTeam;
	else
	{
		Entity* entity = get<PlayerManager>()->instance.ref();
		if (entity)
		{
			if (entity->has<Drone>())
			{
				UpgradeStation* station = UpgradeStation::drone_inside(entity->get<Drone>());
				if (station && station->mode != UpgradeStation::Mode::Deactivating)
					return UIMode::PvpUpgrade;
				else
					return UIMode::PvpDefault;
			}
			else
				return UIMode::ParkourDefault;
		}
		else
		{
			// dead
			if (Game::level.mode == Game::Mode::Pvp)
				return UIMode::PvpKillCam;
			else
				return UIMode::ParkourDead;
		}
	}
}

Vec2 PlayerHuman::ui_anchor(const RenderParams& params) const
{
	return params.camera->viewport.size * Vec2(0.5f, 0.1f) + Vec2(UI_TEXT_SIZE_DEFAULT * UI::scale * 6.0f, UI_TEXT_SIZE_DEFAULT * UI::scale * 0.5f);
}

// return true if we actually display the notification
b8 player_human_notification(Entity* entity, const Vec3& pos, AI::Team team, PlayerHuman::Notification::Type type)
{
	vi_assert(team != AI::TeamNone);
	Target* t = nullptr;
	if (entity)
	{
		t = entity->get<Target>();
		for (s32 i = 0; i < PlayerHuman::notifications.length; i++)
		{
			PlayerHuman::Notification* n = &PlayerHuman::notifications[i];
			if (n->team == team && n->target.ref() == t)
			{
				if (n->type == type)
				{
					n->timer = NOTIFICATION_TIME;
					return false; // notification already displayed
				}
				else
				{
					// replace existing notification
					PlayerHuman::notifications.remove(i);
					i--;
				}
			}
		}
	}

	for (auto i = PlayerHuman::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->local() && i.item()->get<PlayerManager>()->team.ref()->team() == team)
		{
			// a local player will receive this notification; play a sound
			if (type == PlayerHuman::Notification::Type::ForceFieldUnderAttack
				|| type == PlayerHuman::Notification::Type::BatteryUnderAttack
				|| type == PlayerHuman::Notification::Type::TurretUnderAttack
				|| type == PlayerHuman::Notification::Type::MinionSpawnerUnderAttack)
				Audio::post_global(AK::EVENTS::PLAY_NOTIFICATION_UNDER_ATTACK, i.item()->gamepad);
			else if (type == PlayerHuman::Notification::Type::ForceFieldDestroyed
				|| type == PlayerHuman::Notification::Type::TurretDestroyed
				|| type == PlayerHuman::Notification::Type::MinionSpawnerDestroyed)
				Audio::post_global(AK::EVENTS::PLAY_NOTIFICATION_LOST, i.item()->gamepad);

			break;
		}
	}

	PlayerHuman::Notification* n = PlayerHuman::notifications.add();
	n->target = t;
	n->attached = t;
	n->pos = t ? t->absolute_pos() : pos;
	n->timer = NOTIFICATION_TIME;
	n->team = team;
	n->type = type;
	return true;
}

// return true if we actually display the notification
void PlayerHuman::notification(const Vec3& pos, AI::Team team, Notification::Type type)
{
	player_human_notification(nullptr, pos, team, type);
}

// return true if we actually display the notification
b8 PlayerHuman::notification(Entity* e, AI::Team team, Notification::Type type)
{
	return player_human_notification(e, Vec3::zero, team, type);
}

void PlayerHuman::msg(const char* msg, Flags f)
{
	strncpy(msg_text, msg, UI_TEXT_MAX);
	msg_timer = msg_time;
	flag(FlagMessageGood, f & FlagMessageGood);
}

void PlayerHuman::energy_notify(s32 change)
{
	energy_notification_accumulator += s16(change);
	if (Game::session.config.ruleset.upgrades_allow)
	{
		char buffer[UI_TEXT_MAX + 1];
		sprintf(buffer, _(strings::energy_added), s32(energy_notification_accumulator));
		msg(buffer, FlagMessageGood);
	}
}

Array<PlayerHuman::LogEntry> PlayerHuman::logs;
Array<PlayerHuman::ChatEntry> PlayerHuman::chats;
Array<PlayerHuman::Notification> PlayerHuman::notifications;
void PlayerHuman::update_all(const Update& u)
{
	for (auto i = PlayerHuman::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);

	for (s32 i = logs.length - 1; i >= 0; i--)
	{
		if (logs[i].timestamp < Game::real_time.total - LOG_TIME)
			logs.remove_ordered(i);
	}

	for (s32 i = chats.length - 1; i >= 0; i--)
	{
		if (chats[i].timestamp < Game::real_time.total - CHAT_TIME)
			chats.remove_ordered(i);
	}

	for (s32 i = 0; i < notifications.length; i++)
	{
		Notification* n = &notifications[i];

		Target* target = n->target.ref();
		n->timer -= u.time.delta;
		if (n->timer < 0.0f || (n->attached && !n->target.ref()))
		{
			notifications.remove(i);
			i--;
		}
	}
}

void PlayerHuman::chat_add(const char* msg, PlayerManager* player, AI::TeamMask mask)
{
	PlayerHuman::ChatEntry* entry = chats.add();
	entry->timestamp = Game::real_time.total;
	entry->mask = mask;
	entry->team = player->team.ref()->team();
	strncpy(entry->username, player->username, MAX_USERNAME);
	strncpy(entry->msg, msg, MAX_CHAT);
	entry->vip = player->flag(PlayerManager::FlagIsVip);
}

void PlayerHuman::log_add(const char* a, AI::Team a_team, AI::TeamMask mask, b8 a_vip, const char* b, AI::Team b_team, b8 b_vip)
{
	PlayerHuman::LogEntry* entry = logs.add();
	entry->timestamp = Game::real_time.total;
	entry->mask = mask;
	entry->a_team = a_team;
	entry->b_team = b_team;
	entry->a_vip = a_vip;
	entry->b_vip = b_vip;
	strncpy(entry->a, a, UI_TEXT_MAX);
	if (b)
		strncpy(entry->b, b, UI_TEXT_MAX);
	else
		entry->b[0] = '\0';
}

void PlayerHuman::clear()
{
	logs.length = 0;
	chats.length = 0;
	notifications.length = 0;
}

void PlayerHuman::update_camera_rotation(const Update& u, r32 time_scale)
{
	{
		r32 s = speed_mouse * Settings::gamepads[gamepad].effective_sensitivity_mouse() * time_scale;
		angle_horizontal -= u.input->mouse_relative.x * s;
		angle_vertical += u.input->mouse_relative.y * s * (Settings::gamepads[gamepad].invert_y ? -1.0f : 1.0f);
	}

	if (u.input->gamepads[gamepad].type != Gamepad::Type::None)
	{
		r32 s = speed_joystick * Settings::gamepads[gamepad].effective_sensitivity_gamepad() * Game::real_time.delta * time_scale;
		Vec2 rotation(u.input->gamepads[gamepad].right_x, u.input->gamepads[gamepad].right_y);
		Input::dead_zone(&rotation.x, &rotation.y);
		angle_horizontal -= rotation.x * s;
		angle_vertical += rotation.y * s * (Settings::gamepads[gamepad].invert_y ? -1.0f : 1.0f);
	}

	if (angle_vertical < PI * -0.495f)
		angle_vertical = PI * -0.495f;
	if (angle_vertical > PI * 0.495f)
		angle_vertical = PI * 0.495f;

	camera.ref()->rot = Quat::euler(0, angle_horizontal, angle_vertical);
}

Entity* live_player_get(s32 index)
{
	s32 count = 0;
	for (auto i = PlayerCommon::list.iterator(); !i.is_last(); i.next())
	{
		if (count == index)
			return i.item()->entity();
		count++;
	}
	return nullptr;
}

namespace PlayerControlHumanNet
{

struct Message
{
	enum class Type : s8
	{
		Dash,
		DashCombo,
		Go,
		Reflect,
		UpgradeStart,
		AbilitySelect,
		Spot,
		count,
	};

	Vec3 pos;
	Quat rot;
	Vec3 dir;
	Vec3 target; // target position for dashes, or camera position for spotting
	Type type;
	Ref<Entity> entity;
	Ability ability = Ability::None;
	Upgrade upgrade = Upgrade::None;
	s8 ability_slot; // for upgrades
};

template<typename Stream> b8 serialize_msg(Stream* p, Message* msg)
{
	serialize_enum(p, Message::Type, msg->type);

	// position/dir
	if (msg->type == Message::Type::Dash
		|| msg->type == Message::Type::DashCombo
		|| msg->type == Message::Type::Go
		|| msg->type == Message::Type::Reflect
		|| msg->type == Message::Type::Spot)
	{
		if (!serialize_position(p, &msg->pos, Net::Resolution::High))
			net_error();
		if (!serialize_quat(p, &msg->rot, Net::Resolution::High))
			net_error();
		serialize_r32_range(p, msg->dir.x, -1.0f, 1.0f, 16);
		serialize_r32_range(p, msg->dir.y, -1.0f, 1.0f, 16);
		serialize_r32_range(p, msg->dir.z, -1.0f, 1.0f, 16);
	}

	if (msg->type == Message::Type::DashCombo || msg->type == Message::Type::Spot)
	{
		if (!serialize_position(p, &msg->target, Net::Resolution::High))
			net_error();
	}
	else if (Stream::IsReading)
		msg->target = Vec3::zero;

	// ability
	if (msg->type == Message::Type::Go
		|| msg->type == Message::Type::AbilitySelect)
	{
		b8 has_ability;
		if (Stream::IsWriting)
			has_ability = msg->ability != Ability::None;
		serialize_bool(p, has_ability);
		if (has_ability)
			serialize_enum(p, Ability, msg->ability);
		else if (Stream::IsReading)
			msg->ability = Ability::None;
	}
	else if (Stream::IsReading)
		msg->ability = Ability::None;

	// upgrade
	if (msg->type == Message::Type::UpgradeStart)
	{
		serialize_enum(p, Upgrade, msg->upgrade);
		serialize_int(p, s8, msg->ability_slot, 0, MAX_ABILITIES - 1);
	}
	else if (Stream::IsReading)
	{
		msg->upgrade = Upgrade::None;
		msg->ability_slot = 0;
	}

	// what did we reflect off of
	if (msg->type == Message::Type::Reflect)
		serialize_ref(p, msg->entity);
	else if (Stream::IsReading)
		msg->entity = nullptr;

	return true;
}

b8 send(PlayerControlHuman* c, Message* msg)
{
	using Stream = Net::StreamWrite;
	Net::StreamWrite* p = Net::msg_new(Net::MessageType::PlayerControlHuman);
	Ref<PlayerControlHuman> ref = c;
	serialize_ref(p, ref);
	if (!serialize_msg(p, msg))
		net_error();
	Net::msg_finalize(p);
	return true;
}

}

void PlayerHuman::upgrade_menu_show()
{
	Entity* instance = get<PlayerManager>()->instance.ref();
	if (instance && !UpgradeStation::drone_inside(instance->get<Drone>()))
	{
		UpgradeStation* station = UpgradeStation::drone_at(instance->get<Drone>());
		if (station)
			station->drone_enter(instance->get<Drone>());
	}

	if (UpgradeStation::drone_inside(instance->get<Drone>()))
	{
		animation_time = Game::real_time.total;
		menu.animate();
		menu.selected = 0;
		flag(FlagUpgradeMenuOpen, true);
		Audio::post_global(AK::EVENTS::PLAY_DIALOG_SHOW, gamepad);
		upgrade_last_visit_highest_available = get<PlayerManager>()->upgrade_highest_owned_or_available();
	}
}

void PlayerHuman::upgrade_menu_hide()
{
	flag(FlagUpgradeMenuOpen, false);
	Audio::post_global(AK::EVENTS::PLAY_DIALOG_CANCEL, gamepad);
	upgrade_station_try_exit();
}

void PlayerHuman::upgrade_station_try_exit()
{
	Entity* instance = get<PlayerManager>()->instance.ref();
	if (instance && get<PlayerManager>()->state() != PlayerManager::State::Upgrading)
	{
		UpgradeStation* station = UpgradeStation::drone_inside(instance->get<Drone>());
		if (station && station->mode != UpgradeStation::Mode::Deactivating)
		{
			station->drone_exit();
			upgrade_last_visit_highest_available = get<PlayerManager>()->upgrade_highest_owned_or_available();
		}
	}
}

void PlayerHuman::upgrade_completed(Upgrade u)
{
	ability_upgrade_slot = (ability_upgrade_slot + 1) % MAX_ABILITIES;
}

AssetID emote_strings[s32(PlayerHuman::EmoteCategory::count)][s32(PlayerHuman::EmoteCategory::count)] =
{
	{
		strings::emote_teama1,
		strings::emote_teama2,
		strings::emote_teama3,
		strings::emote_teama4,
	},
	{
		strings::emote_teamb1,
		strings::emote_teamb2,
		strings::emote_teamb3,
		strings::emote_teamb4,
	},
	{
		strings::emote_everyone1,
		strings::emote_everyone2,
		strings::emote_everyone3,
		strings::emote_everyone4,
	},
	{
		strings::emote_misc1,
		strings::emote_misc2,
		strings::emote_misc3,
		strings::emote_misc4,
	},
};

b8 PlayerHuman::chat_enabled() const
{
	return gamepad == 0 && emotes_enabled();
}

b8 PlayerHuman::emotes_enabled() const
{
	UIMode mode = ui_mode();
	return (mode == UIMode::PvpDefault
		|| mode == UIMode::PvpUpgrade
		|| mode == UIMode::PvpKillCam
		|| mode == UIMode::PvpSelectTeam
		|| mode == UIMode::PvpSpectate
		|| mode == UIMode::PvpGameOver
		|| ((mode == UIMode::ParkourDefault || mode == UIMode::ParkourDead) && Game::session.type == SessionType::Multiplayer));
}

Upgrade player_confirm_upgrade[MAX_GAMEPADS];
void player_upgrade_start(s8 gamepad)
{
	PlayerHuman* player = PlayerHuman::for_gamepad(gamepad);
	Entity* entity = player->get<PlayerManager>()->instance.ref();
	if (entity)
	{
		PlayerControlHumanNet::Message msg;
		msg.type = PlayerControlHumanNet::Message::Type::UpgradeStart;
		msg.upgrade = player_confirm_upgrade[gamepad];
		msg.ability_slot = player->ability_upgrade_slot;
		PlayerControlHumanNet::send(entity->get<PlayerControlHuman>(), &msg);
	}
}

Rect2 player_button(const Rect2& viewport, s8 gamepad, AssetID string, UIMenu::EnableInput enable_input = UIMenu::EnableInput::Yes, const RenderParams* params = nullptr)
{
	// deploy prompt
	UIText text;
	text.anchor_x = UIText::Anchor::Center;
	text.anchor_y = UIText::Anchor::Min;
	text.text(gamepad, _(string));
	Vec2 pos = viewport.size * Vec2(0.5f, 0.1f);
	Rect2 box = text.rect(pos).outset(8 * UI::scale);
	if (params)
	{
		const Vec4* bg;
		if (enable_input == UIMenu::EnableInput::Yes
			&& (params->sync->input.get(Controls::Interact, gamepad)
			|| (gamepad == 0 && Game::ui_gamepad_types[0] == Gamepad::Type::None && box.contains(params->sync->input.cursor))))
		{
			text.color = UI::color_background;
			if (params->sync->input.keys.get(s32(KeyCode::MouseLeft)) && PlayerHuman::for_gamepad(0)->chat_focus == PlayerHuman::ChatFocus::None)
				bg = &UI::color_alert();
			else
				bg = &UI::color_accent();
		}
		else
		{
			text.color = UI::color_accent();
			bg = &UI::color_background;
		}
		UI::box(*params, box, *bg);
		text.draw(*params, pos);
	}
	return box;
}

void PlayerHuman::update(const Update& u)
{
#if SERVER
	if (Game::session.type == SessionType::Multiplayer
		&& Game::level.mode == Game::Mode::Pvp
		&& Team::match_state == Team::MatchState::Active)
	{
		afk_timer -= Game::real_time.delta;
		if (afk_timer < 0.0f)
		{
			get<PlayerManager>()->leave();
			return;
		}
	}
#endif

	for (s32 i = 0; i < kill_popups.length; i++)
	{
		KillPopup* k = &kill_popups[i];
		k->timer -= u.real_time.delta;
		if (k->timer < 0.0f || !k->victim.ref())
		{
			kill_popups.remove_ordered(i);
			i--;
		}
	}

	Entity* entity = get<PlayerManager>()->instance.ref();

	// record parkour support
	if (Game::level.local && Game::level.mode == Game::Mode::Parkour && entity)
	{
		btCollisionWorld::ClosestRayResultCallback ray_callback = entity->get<Walker>()->check_support();
		if (ray_callback.hasHit()
			&& ray_callback.m_hitNormalWorld.getY() > WALKER_TRACTION_DOT) // must have traction
		{
			const btRigidBody* bt_support = (const btRigidBody*)(ray_callback.m_collisionObject);
			RigidBody* support = Entity::list.data[bt_support->getUserIndex()].get<RigidBody>();

			Vec3 relative_position = support->get<Transform>()->to_local(entity->get<Transform>()->absolute_pos());
			b8 record_support = false;
			if (last_supported.length == 0)
				record_support = true;
			else
			{
				const SupportEntry& last_entry = last_supported[last_supported.length - 1];
				if (last_entry.support.ref() != support || (last_entry.relative_position - relative_position).length_squared() > 2.0f * 2.0f)
					record_support = true;
			}

			if (record_support)
			{
				if (last_supported.length >= 24)
					last_supported.remove_ordered(0);
				SupportEntry* entry = last_supported.add();
				entry->support = support;
				entry->relative_position = relative_position;
				entry->rotation = entity->get<Walker>()->target_rotation;
			}
		}
	}

	if (!local()
#if !SERVER
		&& Net::Client::replay_mode() != Net::Client::ReplayMode::Replaying
#endif
		)
		return;

#if !SERVER
	if (Net::Client::replay_mode() == Net::Client::ReplayMode::Replaying)
	{
		// if anyone hits a button, go back to the main menu
		if (Settings::expo
			&& Game::scheduled_load_level == AssetNull
			&& ((gamepad == 0 && u.input->keys.any()) || u.input->gamepads[gamepad].btns))
		{
			if (Game::session.type == SessionType::Story)
				Menu::title();
			else
				Menu::title_multiplayer();
		}
	}
	else // no rumble when replaying
#endif
	if (rumble > 0.0f)
	{
		u.input->gamepads[gamepad].rumble = Settings::gamepads[gamepad].rumble ? vi_min(1.0f, rumble) : 0.0f;
		rumble = vi_max(0.0f, rumble - u.time.delta);
	}

	// camera stuff
	if (!Overworld::modal())
	{
		s32 player_count;
#if DEBUG_AI_CONTROL
		player_count = count_local() + PlayerAI::list.count();
#else
		player_count = count_local();
#endif
		Camera::ViewportBlueprint* viewports = Camera::viewport_blueprints[player_count - 1];
		Camera::ViewportBlueprint* blueprint = &viewports[count_local_before(this)];

		const DisplayMode& display = Settings::display();
		camera.ref()->viewport =
		{
			Vec2(r32(s32(blueprint->x * r32(display.width))), r32(s32(blueprint->y * r32(display.height)))),
			Vec2(r32(s32(blueprint->w * r32(display.width))), r32(s32(blueprint->h * r32(display.height)))),
		};
		camera.ref()->flag(CameraFlagColors | CameraFlagFog, pvp_colors());

		if (entity || Game::level.noclip)
			camera.ref()->flag(CameraFlagActive, true);
		else
		{
			if (Game::level.mode == Game::Mode::Pvp)
			{
				camera.ref()->perspective(Settings::effective_fov(), camera.ref()->near_plane, camera.ref()->far_plane);
				camera.ref()->range = 0;
				if (get<PlayerManager>()->spawn_timer == 0.0f)
				{
					camera.ref()->cull_range = 0;
					camera.ref()->flag(CameraFlagCullBehindWall, false);
				}
				if (flag(FlagUpgradeMenuOpen))
					upgrade_menu_hide();
			}
			else if (Game::level.mode == Game::Mode::Parkour)
				camera.ref()->flag(CameraFlagActive, false);
		}
	}

	if (msg_timer > 0.0f)
	{
		msg_timer = vi_max(0.0f, msg_timer - Game::real_time.delta);
		if (msg_timer == 0.0f)
			energy_notification_accumulator = 0;
	}

	// after this point, it's all input-related stuff
	if (Console::visible
		|| (gamepad == 0 && Overworld::active())
		|| Game::level.mode == Game::Mode::Special
#if !SERVER
		|| Net::Client::replay_mode() == Net::Client::ReplayMode::Replaying
#endif
		)
		return;

	if (entity)
		spawn_animation_timer = vi_max(0.0f, spawn_animation_timer - u.time.delta); // for letterbox animation

	UIMode mode = ui_mode();

	// emotes
	if (emotes_enabled())
	{
		static Controls emote_bindings[s32(EmoteCategory::count)] =
		{
			Controls::Emote1,
			Controls::Emote2,
			Controls::Emote3,
			Controls::Emote4,
		};
		for (s32 i = 0; i < s32(EmoteCategory::count); i++)
		{
			if (u.input->get(emote_bindings[i], gamepad) && !u.last_input->get(emote_bindings[i], gamepad))
			{
				if (emote_category == EmoteCategory::None)
				{
					emote_category = EmoteCategory(i);
					emote_timer = EMOTE_TIMEOUT;
				}
				else
				{
					// category already chosen, send emote
					AI::TeamMask mask = (emote_category == EmoteCategory::TeamA || emote_category == EmoteCategory::TeamB) ? AI::TeamMask(1 << get<PlayerManager>()->team.ref()->team()) : AI::TeamAll;
					get<PlayerManager>()->chat(_(emote_strings[s32(emote_category)][i]), mask);
					emote_category = EmoteCategory::None;
					emote_timer = 0.0f;
				}
			}
		}

		// check if emote menu timed out
		if (emote_timer > 0.0f)
		{
			emote_timer = vi_max(0.0f, emote_timer - u.time.delta);
			if (emote_timer == 0.0f)
				emote_category = EmoteCategory::None;
		}
	}

	if (chat_enabled())
	{
		if (chat_focus == ChatFocus::None)
		{
			if (u.last_input->get(Controls::ChatAll, 0)
				&& !u.input->get(Controls::ChatAll, 0))
			{
				chat_focus = ChatFocus::All;
				chat_field.set(_(strings::chat_all_prompt));
			}
			else if (u.last_input->get(Controls::ChatTeam, 0)
				&& !u.input->get(Controls::ChatTeam, 0))
			{
				chat_focus = ChatFocus::Team;
				chat_field.set(_(strings::chat_team_prompt));
			}
		}
		else if (u.last_input->get(Controls::Cancel, 0)
			&& !u.input->get(Controls::Cancel, 0)
			&& !Game::cancel_event_eaten[0])
		{
			chat_field.set("");
			chat_focus = ChatFocus::None;
			Game::cancel_event_eaten[0] = true;
		}
	}

	switch (mode)
	{
		case UIMode::Noclip:
		case UIMode::ParkourDead:
			break;
		case UIMode::ParkourDefault:
		{
			PlayerControlHuman* control = get<PlayerManager>()->instance.ref()->get<PlayerControlHuman>();

			if (audio_log != AssetNull)
			{
				audio_log_prompt_timer = vi_max(0.0f, audio_log_prompt_timer - u.real_time.delta);
				if (control->input_enabled() && u.input->get(Controls::Scoreboard, gamepad) && !u.last_input->get(Controls::Scoreboard, gamepad))
				{
					if (flag(FlagAudioLogPlaying))
						audio_log_stop();
					else
					{
						audio_log_prompt_timer = 0.0f;
						Scripts::AudioLogs::play(audio_log);
						flag(FlagAudioLogPlaying, true);
					}
				}
			}

			if (Game::session.type == SessionType::Multiplayer && control->input_enabled())
			{
				if (u.input->get(Controls::InteractSecondary, gamepad) && !u.last_input->get(Controls::InteractSecondary, gamepad))
					get<PlayerManager>()->parkour_ready(!get<PlayerManager>()->flag(PlayerManager::FlagParkourReady));
			}

			break;
		}
		case UIMode::PvpDefault:
		{
			kill_cam_rot = camera.ref()->rot;
			if (UpgradeStation::drone_at(entity->get<Drone>())
				&& get<PlayerManager>()->can_transition_state()
				&& (Game::session.config.ruleset.upgrades_default || get<PlayerManager>()->energy > 0))
			{
				if (chat_focus == ChatFocus::None && !u.input->get(Controls::Interact, gamepad) && u.last_input->get(Controls::Interact, gamepad))
					upgrade_menu_show();
			}
			break;
		}
		case UIMode::PvpUpgrade:
		{
			if (flag(FlagUpgradeMenuOpen))
			{
				// upgrade menu
				if (!UpgradeStation::drone_inside(entity->get<Drone>())) // we got kicked out of the upgrade station; probably by the server
					upgrade_menu_hide();
				else if (chat_focus == ChatFocus::None
					&& !Menu::dialog_active(gamepad)
					&& !Game::cancel_event_eaten[gamepad]
					&& u.last_input->get(Controls::Cancel, gamepad) && !u.input->get(Controls::Cancel, gamepad))
				{
					Game::cancel_event_eaten[gamepad] = true;
					upgrade_menu_hide();
				}
				else
				{
					b8 upgrade_in_progress = !get<PlayerManager>()->can_transition_state();

					s8 last_selected = menu.selected;

					{
						UIMenu::Origin origin =
						{
							camera.ref()->viewport.size * Vec2(0.5f, 0.6f),
							UIText::Anchor::Center,
							UIText::Anchor::Center,
						};
						menu.start(u, origin, gamepad, chat_focus == ChatFocus::None ? UIMenu::EnableInput::Yes : UIMenu::EnableInput::No);
					}

					if (menu.item(u, _(strings::close), nullptr, false, Asset::Mesh::icon_close))
						upgrade_menu_hide();
					else
					{
						if (!upgrade_in_progress)
						{
							if (Game::ui_gamepad_types[gamepad] == Gamepad::Type::None)
							{
								// keyboard
								if (u.input->get(Controls::Ability2, gamepad) && !u.last_input->get(Controls::Ability2, gamepad))
									ability_upgrade_slot = 0;
								else if (u.input->get(Controls::Ability3, gamepad) && !u.last_input->get(Controls::Ability3, gamepad))
									ability_upgrade_slot = 1;
							}
							else
							{
								// gamepad
								if (u.input->get(Controls::Ability2, gamepad) && !u.last_input->get(Controls::Ability2, gamepad))
									ability_upgrade_slot = (ability_upgrade_slot + 1) % MAX_ABILITIES;
							}

							if (get<PlayerManager>()->ability_count() < MAX_ABILITIES)
							{
								// we have an empty ability slot; don't let the player replace an existing ability by accident
								while (get<PlayerManager>()->abilities[ability_upgrade_slot] != Ability::None)
									ability_upgrade_slot = (ability_upgrade_slot + 1) % MAX_ABILITIES;
							}
						}

						for (s32 i = 0; i < s32(Upgrade::count); i++)
						{
							Upgrade upgrade = Upgrade(i);
							if ((Game::session.config.ruleset.upgrades_allow | Game::session.config.ruleset.upgrades_default) & (1 << s32(upgrade)))
							{
								const UpgradeInfo& info = UpgradeInfo::list[s32(upgrade)];
								b8 can_upgrade = !upgrade_in_progress
									&& chat_focus == ChatFocus::None
									&& get<PlayerManager>()->upgrade_available(upgrade)
									&& (Game::level.has_feature(Game::FeatureLevel::All) || UpgradeInfo::list[i].type == UpgradeInfo::Type::Ability) // only allow ability upgrades in tutorial
									&& (Game::level.has_feature(Game::FeatureLevel::All) || AbilityInfo::list[i].type == AbilityInfo::Type::Shoot); // only allow shooting abilities in tutorial
								if (menu.item(u, _(info.name), nullptr, !can_upgrade, info.icon))
								{
									player_confirm_upgrade[gamepad] = upgrade;
									if (info.type == UpgradeInfo::Type::Consumable)
										player_upgrade_start(gamepad);
									else
									{
										Ability existing_ability = get<PlayerManager>()->abilities[ability_upgrade_slot];
										if (existing_ability == Ability::None)
											player_upgrade_start(gamepad);
										else
										{
											const UpgradeInfo& info = UpgradeInfo::list[s32(existing_ability)];
											Menu::dialog(gamepad, &player_upgrade_start, _(strings::confirm_upgrade_replace), _(info.name));
										}
									}
								}
							}
						}
					}

					menu.end(u);

					if (menu.selected != last_selected
						|| upgrade_in_progress) // once the upgrade is done, animate the new ability description
						animation_time = Game::real_time.total;
				}
			}
			else
			{
				// upgrade menu closed, but we're still in the upgrade station
				if (chat_focus == ChatFocus::None && !u.input->get(Controls::Interact, gamepad) && u.last_input->get(Controls::Interact, gamepad))
					upgrade_menu_show();
				else
					upgrade_station_try_exit();
			}
			break;
		}
		case UIMode::Pause:
		{
			UIMenu::Origin origin =
			{
				camera.ref()->viewport.size * Vec2(0, 0.5f),
				UIText::Anchor::Min,
				UIText::Anchor::Center,
			};
			Menu::pause_menu(u, origin, gamepad, &menu, &menu_state);
			if (menu_state == Menu::State::Hidden && Game::should_pause())
				Audio::post_global(AK::EVENTS::RESUME_ALL);
			break;
		}
		case UIMode::PvpSelectTeam:
		{
			// show team switcher
			UIMenu::Origin origin =
			{
				camera.ref()->viewport.size * Vec2(0.5f, 0.65f),
				UIText::Anchor::Center,
				UIText::Anchor::Max,
			};
			if (Menu::teams(u, origin, gamepad, &menu, Menu::TeamSelectMode::MatchStart, chat_focus == ChatFocus::None ? UIMenu::EnableInput::Yes : UIMenu::EnableInput::No) != Menu::State::Teams)
			{
				// user hit escape
				// make sure the cancel event is not eaten, so that our pause/unpause code below works
				Game::cancel_event_eaten[gamepad] = false;
			}
			break;
		}
		case UIMode::PvpKillCam:
		{
			// if something killed us, show the kill cam
			Entity* k = killed_by.ref();
			if (k)
				kill_cam_rot = Quat::look(Vec3::normalize(k->get<Transform>()->absolute_pos() - camera.ref()->pos));
			if (get<PlayerManager>()->spawn_timer < Game::session.config.ruleset.spawn_delay - 1.0f)
				camera.ref()->rot = Quat::slerp(vi_min(1.0f, 5.0f * Game::real_time.delta), camera.ref()->rot, kill_cam_rot);
			break;
		}
		case UIMode::PvpSpectate:
		{
			// we're dead but others still playing; spectate
			update_camera_rotation(u, Game::session.effective_time_scale());

			camera.ref()->perspective(Settings::effective_fov(), 0.02f, Game::level.far_plane_get());

			if (PlayerCommon::list.count() > 0)
			{
				spectate_index += chat_focus == ChatFocus::None ? UI::input_delta_horizontal(u, gamepad) : 0;
				if (spectate_index < 0)
					spectate_index = PlayerCommon::list.count() - 1;
				else if (spectate_index >= PlayerCommon::list.count())
					spectate_index = 0;

				Entity* spectating = live_player_get(spectate_index);

				if (spectating)
					camera_setup_drone(spectating->get<Drone>(), camera.ref(), &camera_center, 6.0f);
			}
			break;
		}
		case UIMode::PvpGameOver:
		{
			camera.ref()->range = 0;
			if (Game::real_time.total - Team::game_over_real_time > SCORE_SUMMARY_DELAY && chat_focus == ChatFocus::None)
			{
				// update score summary scroll
				if (gamepad == 0 && !Menu::dialog_active(0) && !UIMenu::active[0])
				{
					if (u.input->keys.get(s32(KeyCode::MouseWheelUp)))
						score_summary_scroll.pos = vi_max(0, score_summary_scroll.pos - 1);
					else if (u.input->keys.get(s32(KeyCode::MouseWheelDown)))
						score_summary_scroll.pos++;
				}
				score_summary_scroll.update(u, Team::score_summary.length, gamepad);

				if (!get<PlayerManager>()->flag(PlayerManager::FlagScoreAccepted) && Game::real_time.total - Team::game_over_real_time > SCORE_SUMMARY_DELAY + SCORE_SUMMARY_ACCEPT_DELAY)
				{
					// accept score summary
					if ((!u.input->get(Controls::Interact, gamepad) && u.last_input->get(Controls::Interact, gamepad))
						|| (!u.input->keys.get(s32(KeyCode::MouseLeft)) && u.last_input->keys.get(s32(KeyCode::MouseLeft)) && player_button(camera.ref()->viewport, gamepad, strings::prompt_accept).contains(u.input->cursor)))
					{
						get<PlayerManager>()->score_accept();
					}
				}
			}
			break;
		}
		default:
			vi_assert(false);
			break;
	}

	// close/open pause menu if needed
	{
#if RELEASE_BUILD
		if (Game::level.local && menu_state == Menu::State::Hidden && !u.input->focus && u.last_input->focus)
		{
			// pause when window loses focus
			menu_state = Menu::State::Visible;
			menu.animate();
			if (Game::should_pause())
				Audio::post_global(AK::EVENTS::PAUSE_ALL);
		}
		else
#endif
		if (!Game::cancel_event_eaten[gamepad]
			&& !flag(FlagUpgradeMenuOpen)
			&& ((u.last_input->get(Controls::Pause, gamepad) && !u.input->get(Controls::Pause, gamepad) && (menu_state == Menu::State::Hidden || menu_state == Menu::State::Visible))
				|| (menu_state == Menu::State::Visible && u.last_input->get(Controls::Cancel, gamepad) && !u.input->get(Controls::Cancel, gamepad))))
		{
			Game::cancel_event_eaten[gamepad] = true;
			menu_state = (menu_state == Menu::State::Hidden) ? Menu::State::Visible : Menu::State::Hidden;
			Audio::post_global(menu_state == Menu::State::Visible ? AK::EVENTS::PLAY_DIALOG_SHOW : AK::EVENTS::PLAY_DIALOG_CANCEL, gamepad);
			menu.animate();
			if (Game::should_pause())
				Audio::post_global(menu_state == Menu::State::Visible ? AK::EVENTS::PAUSE_ALL : AK::EVENTS::RESUME_ALL);
		}
	}
}

void PlayerHuman::update_late(const Update& u)
{
#if !SERVER
	if (Game::level.noclip)
	{
		// noclip
		update_camera_rotation(u, 1.0f);

		b8 noclip_controls = !Console::visible && chat_focus == ChatFocus::None;

		camera.ref()->perspective((noclip_controls && u.input->keys.get(s32(KeyCode::E))) ? fov_zoom : Settings::effective_fov(), 0.02f, Game::level.far_plane_get());
		camera.ref()->range = 0;
		camera.ref()->cull_range = 0;

		if (noclip_controls)
		{
			if (u.input->keys.get(s32(KeyCode::Space)) && !u.last_input->keys.get(s32(KeyCode::Space)))
			{
				if (Net::Client::replay_speed > 0.0f)
					Net::Client::replay_speed = 0.0f;
				else
					Net::Client::replay_speed = 1.0f;
			}
			if (u.input->keys.get(s32(KeyCode::MouseWheelDown)))
				Net::Client::replay_speed = vi_max(0.0f, Net::Client::replay_speed - 0.1f);
			else if (u.input->keys.get(s32(KeyCode::MouseWheelUp)))
				Net::Client::replay_speed = vi_min(4.0f, Net::Client::replay_speed + 0.1f);
			r32 speed = u.input->get(Controls::Parkour, gamepad) ? 24.0f : 4.0f;
			if (u.input->keys.get(s32(KeyCode::LAlt)))
				speed *= 0.2f;
			camera.ref()->pos += (u.real_time.delta * speed) * PlayerControlHuman::get_movement(u, camera.ref()->rot, gamepad);
		}
	}
	else if (Net::Client::replay_mode() == Net::Client::ReplayMode::Replaying)
	{
		camera.ref()->perspective(Settings::effective_fov(), 1.0f, Game::level.far_plane_get());

		Entity* e = get<PlayerManager>()->instance.ref();
		if (e)
		{
			camera.ref()->rot = Quat::euler(0.0f, PI * 0.25f, PI * 0.25f);
			camera_setup_drone(e->get<Drone>(), camera.ref(), &camera_center, 6.0f);
		}
	}
	
	if (camera.ref())
	{
		if (Game::level.noclip
			|| !get<PlayerManager>()->instance.ref()) // we're respawning
			Audio::listener_update(gamepad, camera.ref()->pos, camera.ref()->rot);
		else
		{
			if (Game::level.mode == Game::Mode::Parkour)
				Audio::listener_update(gamepad, camera.ref()->pos, camera.ref()->rot);
			else
			{
				Entity* instance = get<PlayerManager>()->instance.ref();
				if (!instance)
					instance = live_player_get(spectate_index);

				if (instance)
				{
					// either we're alive, or we're spectating someone
					// make sure the listener is in a valid place

					btCollisionWorld::ClosestRayResultCallback ray_callback(instance->get<Transform>()->absolute_pos(), camera.ref()->pos);
					Physics::raycast(&ray_callback, CollisionAudio);
					if (ray_callback.hasHit())
						Audio::listener_update(gamepad, ray_callback.m_hitPointWorld + ray_callback.m_hitNormalWorld * DRONE_RADIUS, camera.ref()->rot);
					else
						Audio::listener_update(gamepad, camera.ref()->pos, camera.ref()->rot);
				}
				else
					Audio::listener_update(gamepad, camera.ref()->pos, camera.ref()->rot);
			}
		}
	}

	if (chat_focus != ChatFocus::None)
	{
		s32 prompt_length = strlen(_(chat_focus == ChatFocus::Team ? strings::chat_team_prompt : strings::chat_all_prompt));
		chat_field.update(u, prompt_length, MAX_CHAT);
		if (!u.input->get(Controls::UIAcceptText, 0) && u.last_input->get(Controls::UIAcceptText, 0))
		{
			get<PlayerManager>()->chat(&chat_field.value.data[prompt_length], chat_focus == ChatFocus::All ? AI::TeamAll : (1 << get<PlayerManager>()->team.ref()->team()));
			chat_field.set("");
			chat_focus = ChatFocus::None;
		}
	}
#endif
}

void get_interactable_standing_position(Transform* i, Vec3* pos, r32* angle)
{
	Vec3 i_pos;
	Quat i_rot;
	i->absolute(&i_pos, &i_rot);
	Vec3 dir = i_rot * Vec3(-1, 0, 0);
	dir.y = 0.0f;
	dir.normalize();
	if (angle)
		*angle = atan2f(dir.x, dir.z);
	*pos = i_pos + dir * -1.0f;
	const r32 default_capsule_height = (WALKER_HEIGHT + WALKER_PARKOUR_RADIUS * 2.0f);
	pos->y += (default_capsule_height * 0.5f) + WALKER_SUPPORT_HEIGHT;
}

void get_standing_position(Transform* i, Vec3* pos, r32* angle)
{
	Vec3 i_pos;
	Quat i_rot;
	i->absolute(&i_pos, &i_rot);
	Vec3 dir = i_rot * Vec3(1, 0, 0);
	dir.y = 0.0f;
	dir.normalize();
	if (angle)
		*angle = atan2f(dir.x, dir.z);
	*pos = i_pos;
	const r32 default_capsule_height = (WALKER_HEIGHT + WALKER_PARKOUR_RADIUS * 2.0f);
	pos->y += (default_capsule_height * 0.5f) + WALKER_SUPPORT_HEIGHT;
}

void PlayerHuman::spawn(const SpawnPosition& normal_spawn_pos)
{
	Entity* spawned;

	SpawnPosition spawn_pos;

	if (Game::level.mode == Game::Mode::Pvp)
	{
		// spawn drone
		spawn_pos = normal_spawn_pos;
		spawned = World::create<DroneEntity>(get<PlayerManager>()->team.ref()->team(), spawn_pos.pos);
	}
	else
	{
		// spawn traceur

		b8 spawned_at_last_supported = false;
		if (last_supported.length > 0)
		{
			// restore last supported position
			s32 backtrack = killed_by.ref() ? 10 : 1; // if we were killed by something specific, backtrack farther than if we just fell to our death
			for (s32 i = 0; i < backtrack; i++)
			{
				if (last_supported.length > 1)
					last_supported.remove_ordered(last_supported.length - 1);
				else
					break;
			}
			while (last_supported.length > 0) // try to spawn at last supported location
			{
				SupportEntry entry = last_supported[last_supported.length - 1];
				last_supported.remove_ordered(last_supported.length - 1);
				if (entry.support.ref())
				{
					spawn_pos.pos = entry.support.ref()->get<Transform>()->to_world(entry.relative_position);
					spawn_pos.angle = entry.rotation;
					spawned_at_last_supported = true;
					break;
				}
			}
		}

		if (!spawned_at_last_supported)
		{
			if (Game::save.inside_terminal) // spawn the player inside the terminal.
				get_interactable_standing_position(Game::level.terminal_interactable.ref()->get<Transform>(), &spawn_pos.pos, &spawn_pos.angle);
			else
			{
				// we are entering a level. if we're entering by tram, spawn in the tram. otherwise spawn at the SpawnPoint

				s8 track = -1;
				if (Game::save.zone_last != AssetNull)
				{
					for (s32 i = 0; i < Game::level.tram_tracks.length; i++)
					{
						const Game::TramTrack& t = Game::level.tram_tracks[i];
						if (t.level == Game::save.zone_last)
						{
							track = s8(i);
							break;
						}
					}
				}

				Tram* tram = Tram::by_track(track);

				if (tram)
				{
					// spawn in tram
					Quat rot;
					tram->get<Transform>()->absolute(&spawn_pos.pos, &rot);
					spawn_pos.pos.y -= 1.0f;
					Vec3 dir = rot * Vec3(0, 0, -1);
					dir.y = 0.0f;
					dir.normalize();
					spawn_pos.angle = atan2f(dir.x, dir.z);
				}
				else // spawn at normal position
					spawn_pos = normal_spawn_pos;
				spawn_pos.pos.y += 1.0f;
			}
		}

		spawned = World::create<Traceur>(spawn_pos.pos, spawn_pos.angle, get<PlayerManager>()->team.ref()->team());
	}

	spawned->get<Transform>()->absolute_pos(spawn_pos.pos);
	PlayerCommon* common = spawned->add<PlayerCommon>(get<PlayerManager>());
	common->angle_horizontal = spawn_pos.angle;

	spawned->add<PlayerControlHuman>(this);

	Net::finalize(spawned);

	if (Game::level.mode == Game::Mode::Pvp)
		ParticleEffect::spawn(ParticleEffect::Type::SpawnDrone, spawn_pos.pos + Vec3(0, DRONE_RADIUS, 0), Quat::look(Vec3(0, 1, 0)));
	else if (Game::save.inside_terminal)
	{
		Overworld::show(camera.ref(), Overworld::State::StoryMode);
		Overworld::skip_transition_half();
	}
}

r32 draw_icon_text(const RenderParams& params, s8 gamepad, const Vec2& pos, AssetID icon, char* string, r32 percentage, const Vec4& color, r32 total_width = 0.0f)
{
	r32 icon_size = UI_TEXT_SIZE_DEFAULT * UI::scale;
	r32 padding = 8 * UI::scale;

	UIText text;
	text.color = color;
	text.anchor_x = UIText::Anchor::Min;
	text.anchor_y = UIText::Anchor::Center;
	text.text(gamepad, string);

	if (total_width == 0.0f)
		total_width = icon_size + padding + text.bounds().x;
	else
		total_width -= padding * 2.0f;

	UI::box(params, Rect2(pos, Vec2(total_width, icon_size)).outset(padding), UI::color_background);
	if (icon != AssetNull)
		UI::mesh(params, icon, pos + Vec2(icon_size - padding, icon_size * 0.5f), Vec2(icon_size), text.color);

	if (percentage > 0.0f)
		UI::triangle_percentage(params, { pos + Vec2(icon_size + padding * 1.5f, padding * 1.25f), Vec2(icon_size + padding) }, percentage, text.color, PI);
	else
		text.draw(params, pos + Vec2(icon_size + padding, padding));

	return total_width + padding * 2.0f;
}

enum class AbilityDrawMode : s8
{
	InGameUI,
	UpgradeMenu,
	count,
};

r32 ability_draw(const RenderParams& params, const PlayerManager* manager, const Vec2& pos, s8 gamepad, s32 index, Controls binding, AbilityDrawMode mode)
{
	char string[255];

	Ability ability = index == 0 ? Ability::None : manager->abilities[index - 1];

	const AbilityInfo& info = AbilityInfo::list[s32(ability)];

	sprintf(string, "%s", Settings::gamepads[gamepad].bindings[s32(binding)].string(Game::ui_gamepad_types[gamepad]));
	const Vec4* color;
	if (mode == AbilityDrawMode::UpgradeMenu)
		color = manager->get<PlayerHuman>()->ability_upgrade_slot == index - 1 ? &UI::color_default : &UI::color_accent();
	else if (index > 0 && Game::real_time.total - manager->ability_flash_time[index - 1] < msg_time)
		color = UI::flash_function(Game::real_time.total) ? &UI::color_default : &UI::color_background;
	else if (info.type == AbilityInfo::Type::Passive)
		color = &UI::color_disabled();
	else if (!manager->ability_valid(ability) || !manager->instance.ref()->get<PlayerCommon>()->movement_enabled())
		color = params.sync->input.get(binding, gamepad) ? &UI::color_disabled() : &UI::color_alert();
	else if (manager->instance.ref()->get<Drone>()->current_ability == ability)
		color = &UI::color_default;
	else
		color = &UI::color_accent();
	
	r32 percentage;
	AssetID icon;
	if (mode == AbilityDrawMode::UpgradeMenu)
	{
		percentage = 0.0f;
		if (ability == Ability::None)
			icon = manager->get<PlayerHuman>()->ability_upgrade_slot == index - 1 ? Asset::Mesh::icon_ability_pip : AssetNull;
		else
			icon = info.icon;
	}
	else
	{
		icon = info.icon;
		if (info.cooldown_use == 0.0f)
			percentage = 0.0f;
		else
		{
			r32 cooldown = manager->ability_cooldown[s32(ability)];
			if (cooldown < info.cooldown_use_threshold)
				percentage = 0.0f;
			else
				percentage = 1.0f - ((cooldown - info.cooldown_use_threshold) / info.cooldown_use);
		}
	}

	return draw_icon_text(params, gamepad, pos, icon, string, percentage, *color);
}

r32 match_timer_width()
{
	return UI_TEXT_SIZE_DEFAULT * 2.5f * UI::scale;
}

void match_timer_draw(const RenderParams& params, const Vec2& pos, UIText::Anchor anchor_x)
{
	r32 time_limit;
	switch (Team::match_state)
	{
		case Team::MatchState::Waiting:
			vi_assert(false);
			break;
		case Team::MatchState::TeamSelect:
			time_limit = TEAM_SELECT_TIME;
			break;
		case Team::MatchState::Active:
		{
			if (Game::level.mode == Game::Mode::Parkour)
			{
				vi_assert(Game::session.type == SessionType::Multiplayer);
				time_limit = 60.0f * r32(Game::session.config.time_limit_parkour_ready);
			}
			else
				time_limit = Game::session.config.time_limit();
			break;
		}
		case Team::MatchState::Done:
			time_limit = SCORE_SUMMARY_ACCEPT_TIME;
			break;
		default:
			vi_assert(false);
			break;
	}
	r32 remaining = vi_max(0.0f, time_limit - Team::match_time);

	Vec2 box(match_timer_width(), UI_TEXT_SIZE_DEFAULT * UI::scale);
	r32 padding = 8.0f * UI::scale;

	Vec2 p = pos;
	switch (anchor_x)
	{
		case UIText::Anchor::Min:
			break;
		case UIText::Anchor::Center:
			p.x += box.x * -0.5f;
			break;
		case UIText::Anchor::Max:
			p.x -= box.x;
			break;
		default:
			vi_assert(false);
			break;
	}
		
	UI::box(params, Rect2(p, box).outset(padding), UI::color_background);

	{
		const Vec4* color;
		b8 draw;
		if (Game::level.mode == Game::Mode::Pvp && Team::match_state == Team::MatchState::Active)
		{
			if (remaining > Game::session.config.time_limit() * 0.5f)
				color = &UI::color_default;
			else if (remaining > Game::session.config.time_limit() * 0.25f)
				color = &UI::color_accent();
			else
				color = &UI::color_alert();

			if (remaining > Game::session.config.time_limit() * 0.2f)
				draw = true;
			else if (remaining > 30.0f)
				draw = UI::flash_function_slow(Game::real_time.total);
			else
				draw = UI::flash_function(Game::real_time.total);
		}
		else
		{
			color = &UI::color_default;
			draw = true;
		}

		if (draw)
		{
			s32 remaining_minutes = s32(remaining / 60.0f);
			s32 remaining_seconds = s32(remaining - (remaining_minutes * 60.0f));

			UIText text;
			text.anchor_x = UIText::Anchor::Min;
			text.anchor_y = UIText::Anchor::Min;
			text.color = *color;
			text.text(0, _(strings::timer), remaining_minutes, remaining_seconds);
			text.draw(params, p);
		}
	}
}

enum class ScoreboardPosition : s8
{
	Center,
	Bottom,
	count,
};

void scoreboard_draw(const RenderParams& params, const PlayerManager* manager, ScoreboardPosition position)
{
	const Rect2& vp = params.camera->viewport;
	Vec2 p;
	switch (position)
	{
		case ScoreboardPosition::Center:
			p = vp.size * Vec2(0.5f, 0.8f);
			break;
		case ScoreboardPosition::Bottom:
			p = vp.size * Vec2(0.5f, 0.3f);
			break;
		default:
		{
			vi_assert(false);
			p = Vec2::zero;
			break;
		}
	}

	if ((Game::level.mode == Game::Mode::Pvp && Team::match_state != Team::MatchState::Waiting)
		|| (Game::level.mode == Game::Mode::Parkour && PlayerManager::list.count() >= Game::session.config.min_players))
		match_timer_draw(params, p, UIText::Anchor::Center);

	UIText text;
	r32 width = MENU_ITEM_WIDTH * 1.25f;
	text.anchor_x = UIText::Anchor::Min;
	text.anchor_y = UIText::Anchor::Min;
	text.color = UI::color_default;
	p.y += text.bounds().y + MENU_ITEM_PADDING * -3.0f;
	p.x += width * -0.5f;

	{
		// game type
		Overworld::game_type_string(&text, Game::session.config.preset, Game::session.config.game_type, Team::list.count(), Game::session.config.max_players);
		UI::box(params, Rect2(p, Vec2(width, text.bounds().y)).outset(MENU_ITEM_PADDING), UI::color_background);
		text.draw(params, p);
		p.y -= text.bounds().y + MENU_ITEM_PADDING * 2.0f;
	}

	if (Game::level.mode == Game::Mode::Parkour)
	{
		// waiting
		vi_assert(Game::session.type == SessionType::Multiplayer);
		UI::box(params, Rect2(p, Vec2(width, text.bounds().y)).outset(MENU_ITEM_PADDING), UI::color_background);
		text.text(0, _(strings::waiting));
		text.draw(params, p);
		p.y -= text.bounds().y + MENU_ITEM_PADDING * 2.0f;
	}
	else if (!manager->instance.ref())
	{
		// deploying or waiting
		UI::box(params, Rect2(p, Vec2(width, text.bounds().y)).outset(MENU_ITEM_PADDING), UI::color_background);
		if (Team::match_state == Team::MatchState::Active)
		{
			if (Game::session.config.game_type == GameType::Assault)
				text.text(0, _(strings::deploy_timer_assault), _(Team::name_long(manager->team.ref()->team())), s32(manager->spawn_timer + 1));
			else
				text.text(0, _(strings::deploy_timer), s32(manager->spawn_timer + 1));
		}
		else
			text.text(0, _(strings::waiting));
		text.draw(params, p);
		p.y -= text.bounds().y + MENU_ITEM_PADDING * 2.0f;
	}

	// sort by team
	AI::Team team_mine = manager->team.ref()->team();
	AI::Team team = team_mine;
	while (true)
	{
		const Team& team_ref = Team::list[team];

		// team header
		s32 player_count = team_ref.player_count();

		s32 team_score;
		switch (Game::session.config.game_type)
		{
			case GameType::CaptureTheFlag:
				team_score = team_ref.flags_captured;
				break;
			default:
				team_score = team_ref.kills;
				break;
		}

		if (player_count > 1 || Game::session.config.game_type == GameType::Assault)
		{
			text.anchor_x = UIText::Anchor::Min;
			text.color = Team::color_ui(manager->team.ref()->team(), team);
			text.text_raw(0, _(Team::name_long(team)));
			UI::box(params, Rect2(p, Vec2(width, text.bounds().y)).outset(MENU_ITEM_PADDING), UI::color_background);
			text.draw(params, p);

			if (Game::level.mode == Game::Mode::Pvp)
			{
				// score
				text.anchor_x = UIText::Anchor::Max;
				text.text(0, "%d", team_score);
				text.draw(params, p + Vec2(width - MENU_ITEM_PADDING, 0));
			}

			p.y -= text.bounds().y + MENU_ITEM_PADDING * 2.0f;
		}

		// players
		for (auto i = PlayerManager::list.iterator(); !i.is_last(); i.next())
		{
			if (i.item()->team.ref()->team() == team)
			{
				UI::box(params, Rect2(p, Vec2(width, text.bounds().y)).outset(MENU_ITEM_PADDING), UI::color_background);

				text.anchor_x = UIText::Anchor::Min;

				if (!Game::level.local && i.item()->has<PlayerHuman>()) // todo: fake ping for ai players
				{
					// ping
					r32 rtt = Net::rtt(i.item()->get<PlayerHuman>());
					text.color = UI::color_ping(rtt);
					text.text(0, _(strings::ping), s32(rtt * 1000.0f));
					text.draw(params, p + Vec2(width * 0.75f, 0));
				}

				{
					// username
					if (Game::level.mode == Game::Mode::Pvp)
						text.color = Team::color_ui(manager->team.ref()->team(), i.item()->team.ref()->team());
					else
						text.color = UI::color_default;
					text.icon = i.item()->flag(PlayerManager::FlagIsVip) ? Asset::Mesh::icon_vip : AssetNull;
					text.text_raw(0, i.item()->username);
					text.draw(params, p);
					text.icon = AssetNull;
				}

				if (Game::level.mode == Game::Mode::Pvp)
				{
					// score
					text.anchor_x = UIText::Anchor::Max;
					text.wrap_width = 0;

					s32 score;
					if (player_count == 1)
						score = team_score;
					else
					{
						switch (Game::session.config.game_type)
						{
							case GameType::CaptureTheFlag:
								score = i.item()->flags_captured;
								break;
							default:
								score = i.item()->kills;
								break;
						}
					}

					text.text(0, "%d", score);
					text.draw(params, p + Vec2(width - MENU_ITEM_PADDING, 0));
				}
				else
				{
					// ready
					if (i.item()->flag(PlayerManager::FlagParkourReady))
					{
						const r32 icon_size = MENU_ITEM_FONT_SIZE * UI::scale;
						UI::mesh(params, Asset::Mesh::icon_checkmark, p + Vec2(width - MENU_ITEM_PADDING - icon_size, text.bounds().y * 0.5f), Vec2(icon_size), text.color);
					}
				}

				p.y -= text.bounds().y + MENU_ITEM_PADDING * 2.0f;
			}
		}

		team = AI::Team((s32(team) + 1) % Team::list.count());
		if (team == team_mine)
			break;
	}
}

Upgrade PlayerHuman::upgrade_selected() const
{
	Upgrade upgrade = Upgrade::None;
	{
		// purchased upgrades are removed from the menu
		// we have to figure out which one is selected
		s32 index = 0;
		for (s32 i = 0; i < s32(Upgrade::count); i++)
		{
			if ((Game::session.config.ruleset.upgrades_allow | Game::session.config.ruleset.upgrades_default) & (1 << i))
			{
				if (index == menu.selected - 1)
				{
					upgrade = Upgrade(i);
					break;
				}
				index++;
			}
		}
	}
	return upgrade;
}

b8 player_determine_visibility(PlayerCommon* me, PlayerCommon* other_player)
{
	// make sure we can see this guy
	const PlayerManager::Visibility& visibility = PlayerManager::visibility[PlayerManager::visibility_hash(me->manager.ref(), other_player->manager.ref())];
	return visibility.value;
}

void player_draw_flag(const RenderParams& params, const Flag* flag)
{
	Vec3 pos = flag->get<Transform>()->absolute_pos();
	Vec2 p;
	if (UI::project(params, pos, &p))
	{
		const Vec4& color = Team::color_ui(params.camera->team, flag->team);
		UI::centered_box(params, { p, Vec2(32.0f * UI::scale) }, UI::color_background);
		UI::mesh(params, Asset::Mesh::icon_flag, p, Vec2(24.0f * UI::scale), color);

		if (!flag->at_base && !flag->get<Transform>()->parent.ref())
		{
			// it's not at the base and not being carried, so it's sitting waiting to be restored
			Vec2 bar_size(40.0f * UI::scale, 8.0f * UI::scale);
			Rect2 bar = { p + Vec2(0, 32.0f * UI::scale) + (bar_size * -0.5f), bar_size };
			UI::box(params, bar, UI::color_background);
			UI::border(params, bar, 2, color);
			UI::box(params, { bar.pos, Vec2(bar.size.x * (1.0f - (flag->timer / FLAG_RESTORE_TIME)), bar.size.y) }, color);
		}
	}
}

void draw_bar(const RenderParams& params, r32 value, r32 max, const Vec2& p, const Vec4& color)
{
	Vec2 bar_size(40.0f * UI::scale, 8.0f * UI::scale);
	Rect2 bar = { p + (bar_size * -0.5f), bar_size };
	UI::box(params, bar, UI::color_background);
	UI::border(params, bar, 2, color);
	UI::box(params, { bar.pos, Vec2(bar.size.x * (value / max), bar.size.y) }, color);
}

void draw_health_bar(const RenderParams& params, const Health* health, const Vec2& p, const Vec4& color)
{
	draw_bar(params, r32(health->hp), r32(health->hp_max), p, color);
}

void PlayerHuman::draw_battery_flag_icons(const RenderParams& params) const
{
	UIMode mode = ui_mode();
	if (params.camera == camera.ref()
		&& (gamepad != 0 || !Overworld::active())
		&& local()
		&& (mode == UIMode::PvpSpectate || mode == UIMode::PvpDefault || mode == UIMode::PvpUpgrade))
	{
		Team* my_team = get<PlayerManager>()->team.ref();

		// battery icons
		{
			r32 range_sq;
			Vec3 my_pos;
			{
				Entity* e = get<PlayerManager>()->instance.ref();
				if (e)
				{
					my_pos = e->get<Transform>()->absolute_pos();
					range_sq = e->get<Drone>()->range();
					range_sq *= range_sq;
				}
				else
				{
					my_pos = Vec3::zero;
					range_sq = 0.0f;
				}
			}

			for (auto i = Battery::list.iterator(); !i.is_last(); i.next())
			{
				Vec2 p;
				if (UI::project(params, i.item()->get<Target>()->absolute_pos(), &p))
				{
					// energy bar
					draw_bar(params, i.item()->energy, BATTERY_ENERGY, p + Vec2(0, 32.0f * UI::scale), UI::color_accent());
				}
			}
		}

		// spot
		{
			Target* spot_target = get<PlayerManager>()->team.ref()->spot_target.ref();
			if (spot_target)
			{
				// if the target is offscreen, point toward it
				Vec2 p;
				Vec2 offset;
				if (UI::is_onscreen(params, spot_target->absolute_pos(), &p, &offset))
					UI::mesh(params, Asset::Mesh::icon_spot, p, Vec2(18.0f * UI::scale), UI::color_accent());
				else
					UI::triangle(params, { p, Vec2(18 * UI::scale) }, UI::color_accent(), atan2f(offset.y, offset.x) + PI * -0.5f);
			}
		}

		// flags
		if (Game::session.config.game_type == GameType::CaptureTheFlag)
		{
			AI::Team enemy_team = my_team->team() == 0 ? 1 : 0;

			Entity* instance = get<PlayerManager>()->instance.ref();

			{
				// enemy flag
				Flag* enemy_flag = Flag::for_team(enemy_team);
				if (!instance || enemy_flag != instance->get<Drone>()->flag.ref()) // don't show it if we're carrying it
					player_draw_flag(params, enemy_flag);
			}

			{
				// our flag
				Flag* our_flag = Flag::for_team(my_team->team());

				if (!our_flag->at_base)
				{
					// flag base icon
					Vec3 pos = get<PlayerManager>()->team.ref()->flag_base.ref()->absolute_pos();
					Vec2 p;
					if (UI::project(params, pos, &p))
					{
						UI::centered_box(params, { p, Vec2(32.0f * UI::scale) }, UI::color_background);
						UI::mesh(params, Asset::Mesh::icon_flag_base, p, Vec2(24.0f * UI::scale), Team::color_ui_friend());
					}
				}

				Transform* carrier = our_flag->get<Transform>()->parent.ref();
				if (carrier) // it's being carried; only show it if we can see the carrier
				{
					if (instance)
					{
						b8 carrier_visible = player_determine_visibility(instance->get<PlayerCommon>(), carrier->get<PlayerCommon>());
						if (carrier_visible)
							player_draw_flag(params, our_flag);
					}
				}
				else // flag is sitting somewhere
				{
					Vec3 pos = get<PlayerManager>()->team.ref()->flag_base.ref()->absolute_pos();
					if (instance && instance->get<Drone>()->flag.ref())
						UI::indicator(params, pos, Team::color_ui_friend(), true);

					player_draw_flag(params, our_flag);
				}
			}
		}

		// draw notifications
		for (s32 i = 0; i < notifications.length; i++)
		{
			const Notification& n = notifications[i];
			if (n.timer > NOTIFICATION_TIME_HIDDEN && n.team == my_team->team())
			{
				const Target* target = n.target.ref();
				Vec3 pos = target ? target->absolute_pos() : n.pos;
				Vec2 p;
				if (UI::project(params, pos, &p))
				{
					Vec2 size(18.0f * UI::scale);
					switch (n.type)
					{
						case Notification::Type::DroneDestroyed:
						case Notification::Type::TurretDestroyed:
						case Notification::Type::ForceFieldDestroyed:
						case Notification::Type::MinionSpawnerDestroyed:
							UI::mesh(params, Asset::Mesh::icon_close, p, size, UI::color_alert());
							break;
						case Notification::Type::TurretUnderAttack:
						case Notification::Type::MinionSpawnerUnderAttack:
						{
							if (UI::flash_function_slow(Game::real_time.total))
								UI::mesh(params, Asset::Mesh::icon_warning, p + Vec2(0, 56.0f * UI::scale), size, UI::color_accent());
							break;
						}
						case Notification::Type::ForceFieldUnderAttack:
						{
							if (UI::flash_function_slow(Game::real_time.total))
								UI::mesh(params, Asset::Mesh::icon_warning, p, size, UI::color_accent());
							break;
						}
						case Notification::Type::BatteryUnderAttack:
						{
							if (UI::flash_function_slow(Game::real_time.total))
								UI::mesh(params, Asset::Mesh::icon_warning, p + Vec2(0, 32.0f * UI::scale), size, UI::color_accent());
							break;
						}
						default:
							vi_assert(false);
							break;
					}
				}
			}
		}
	}
}

void PlayerHuman::draw_ui_early(const RenderParams& params) const
{
	UIMode mode = ui_mode();
	if (mode == UIMode::PvpDefault || mode == UIMode::PvpUpgrade)
		draw_battery_flag_icons(params);
}

void PlayerHuman::draw_ui(const RenderParams& params) const
{
	if (params.camera != camera.ref()
		|| (gamepad == 0 && Overworld::active())
		|| Game::level.noclip
		|| !local())
		return;

	const r32 line_thickness = 2.0f * UI::scale;

	const Rect2& vp = params.camera->viewport;

	UIMode mode = ui_mode();

	// emote menu
	if (emote_category != EmoteCategory::None && emotes_enabled())
	{
		UIText text;
		text.font = Asset::Font::pt_sans;
		text.anchor_x = UIText::Anchor::Min;
		text.anchor_y = UIText::Anchor::Max;
		switch (emote_category)
		{
			case EmoteCategory::TeamA:
			case EmoteCategory::TeamB:
				text.color = Team::color_ui_friend();
				break;
			case EmoteCategory::Everyone:
				text.color = UI::color_accent();
				break;
			case EmoteCategory::Misc:
				text.color = UI::color_default;
				break;
			default:
				vi_assert(false);
				break;
		}
		text.wrap_width = MENU_ITEM_WIDTH - MENU_ITEM_PADDING * 2.0f;
		Vec2 p = params.camera->viewport.size * Vec2(0, 0.5f) + Vec2(MENU_ITEM_PADDING * 5.0f, 0);
		const r32 height_one_row = UI_TEXT_SIZE_DEFAULT * UI::scale + MENU_ITEM_PADDING;
		const r32 height_total = MENU_ITEM_PADDING + s32(EmoteCategory::count) * height_one_row;
		Rect2 box =
		{
			p + Vec2(-MENU_ITEM_PADDING, -height_total + MENU_ITEM_PADDING),
			Vec2(MENU_ITEM_WIDTH, height_total),
		};
		UI::box(params, box, UI::color_background);
		for (s32 i = 0; i < s32(EmoteCategory::count); i++)
		{
			text.text(gamepad, "[{{Emote%d}}] %s", i + 1, _(emote_strings[s32(emote_category)][i]));
			text.draw(params, p);
			p.y -= UI_TEXT_SIZE_DEFAULT * UI::scale + MENU_ITEM_PADDING;
		}
	}

	// draw abilities
	if (Game::level.has_feature(Game::FeatureLevel::Abilities)
		&& (Game::session.config.ruleset.upgrades_allow | Game::session.config.ruleset.upgrades_default))
	{
		if (mode == UIMode::PvpDefault
			&& get<PlayerManager>()->can_transition_state()
			&& UpgradeStation::drone_at(get<PlayerManager>()->instance.ref()->get<Drone>())
			&& (get<PlayerManager>()->energy > 0 || Game::session.config.ruleset.upgrades_default))
		{
			// "upgrade!" prompt
			UIText text;
			text.text(gamepad, _(strings::prompt_upgrade));
			text.anchor_x = UIText::Anchor::Center;
			text.anchor_y = UIText::Anchor::Center;
			Vec2 pos = vp.size * Vec2(0.5f, 0.2f);
			const Vec4* bg;
			if (get<PlayerManager>()->upgrade_available() || Game::session.config.ruleset.upgrades_default)
			{
				if (chat_focus == ChatFocus::None && params.sync->input.get(Controls::Interact, gamepad))
				{
					text.color = UI::color_background;
					bg = &UI::color_accent();
				}
				else
				{
					text.color = UI::color_accent();
					bg = &UI::color_background;
				}
			}
			else
			{
				text.color = UI::color_disabled();
				bg = &UI::color_background;
			}
			UI::box(params, text.rect(pos).outset(8.0f * UI::scale), *bg);
			text.draw(params, pos);
		}

		if ((mode == UIMode::PvpDefault || mode == UIMode::PvpUpgrade))
		{
			// draw abilities

			Vec2 pos = ui_anchor(params);

			// ability 1 (jump)
			pos.x += ability_draw(params, get<PlayerManager>(), pos, gamepad, 0, Controls::Ability1, AbilityDrawMode::InGameUI);

			// ability 2
			if (get<PlayerManager>()->abilities[0] != Ability::None)
				pos.x += ability_draw(params, get<PlayerManager>(), pos, gamepad, 1, Controls::Ability2, AbilityDrawMode::InGameUI);

			// ability 3
			if (get<PlayerManager>()->abilities[1] != Ability::None)
				ability_draw(params, get<PlayerManager>(), pos, gamepad, 2, Controls::Ability3, AbilityDrawMode::InGameUI);
		}
	}

	if (Game::level.mode == Game::Mode::Pvp
		&& Game::level.has_feature(Game::FeatureLevel::Abilities)
		&& Game::session.config.ruleset.upgrades_allow
		&& (mode == UIMode::PvpDefault || mode == UIMode::PvpUpgrade))
	{
		// energy
		char buffer[16];
		snprintf(buffer, 16, "%hd", get<PlayerManager>()->energy);
		Vec2 p = ui_anchor(params) + Vec2(match_timer_width() + UI_TEXT_SIZE_DEFAULT * UI::scale, (UI_TEXT_SIZE_DEFAULT + 16.0f) * -UI::scale);
		draw_icon_text(params, gamepad, p, Asset::Mesh::icon_battery, buffer, 0.0f, UI::color_accent(), UI_TEXT_SIZE_DEFAULT * 5 * UI::scale);
	}

	if ((mode == UIMode::PvpDefault || mode == UIMode::PvpSpectate)
		|| (Game::session.type == SessionType::Multiplayer && (mode == UIMode::ParkourDefault || mode == UIMode::ParkourDead)))
	{
		if (chat_focus == ChatFocus::None && params.sync->input.get(Controls::Scoreboard, gamepad))
			scoreboard_draw(params, get<PlayerManager>(), ScoreboardPosition::Center);
	}

	if (mode == UIMode::PvpUpgrade)
	{
		if (flag(FlagUpgradeMenuOpen))
		{
			// draw ability slots
			{
				Rect2 menu_rect = menu.rect();

				Vec2 pos = menu_rect.pos + Vec2(MENU_ITEM_PADDING, menu_rect.size.y + MENU_ITEM_PADDING);
				pos.x += ability_draw(params, get<PlayerManager>(), pos, gamepad, 1, Controls::Ability2, AbilityDrawMode::UpgradeMenu);
				ability_draw(params, get<PlayerManager>(), pos, gamepad, 2, Controls::Ability3, AbilityDrawMode::UpgradeMenu);
			}

			menu.draw_ui(params);

			if (menu.selected > 0)
			{
				// show details of currently highlighted upgrade
				Upgrade upgrade = upgrade_selected();
				vi_assert(upgrade != Upgrade::None);

				if (get<PlayerManager>()->current_upgrade == Upgrade::None)
				{
					r32 padding = 8.0f * UI::scale;

					const UpgradeInfo& info = UpgradeInfo::list[s32(upgrade)];
					UIText text;
					text.color = menu.items[menu.selected].label.color;
					text.anchor_x = UIText::Anchor::Min;
					text.anchor_y = UIText::Anchor::Max;
					text.wrap_width = MENU_ITEM_WIDTH - padding * 2.0f;
					s16 cost = get<PlayerManager>()->upgrade_cost(upgrade);
					if (get<PlayerManager>()->has_upgrade(upgrade))
						text.text(gamepad, _(info.description), cost);
					else
					{
						char description[UI_TEXT_MAX];
						sprintf(description, "%s\n%s", _(strings::buy_cost), _(info.description));
						text.text(gamepad, description, cost);
					}
					UIMenu::text_clip(&text, gamepad, animation_time, 150.0f);

					Vec2 pos = menu.origin.pos + Vec2(MENU_ITEM_WIDTH * -0.5f + padding, menu.height() * -0.5f - padding * 7.0f);
					UI::box(params, text.rect(pos).outset(padding), UI::color_background);
					text.draw(params, pos);
				}
			}
		}

		// upgrade timer bar
		if (get<PlayerManager>()->state() == PlayerManager::State::Upgrading)
		{
			UIText text;
			text.size = 18.0f;
			text.color = UI::color_background;
			text.anchor_x = UIText::Anchor::Center;
			text.anchor_y = UIText::Anchor::Center;
			text.text(gamepad, _(strings::upgrading));
			Vec2 pos = params.camera->viewport.size * Vec2(0.5f, 0.2f);
			Rect2 bar = text.rect(pos).outset(MENU_ITEM_PADDING);
			UI::box(params, bar, UI::color_background);
			UI::border(params, bar, 2, UI::color_accent());
			UI::box(params, { bar.pos, Vec2(bar.size.x * (1.0f - (get<PlayerManager>()->state_timer / UPGRADE_TIME)), bar.size.y) }, UI::color_accent());
			text.draw(params, pos);
		}
	}
	else if (mode == UIMode::PvpSelectTeam)
	{
		// waiting for players or selecting teams
		Vec2 p(params.camera->viewport.size * Vec2(0.5f, 0.75f));
		{
			UIText text;
			text.anchor_x = UIText::Anchor::Min;
			text.anchor_y = UIText::Anchor::Min;
			text.color = UI::color_default;
			text.wrap_width = MENU_ITEM_WIDTH - MENU_ITEM_PADDING * 2.0f;
			p.y += text.bounds().y + MENU_ITEM_PADDING * -3.0f;
			p.x += MENU_ITEM_WIDTH * -0.5f;

			if (Team::match_state == Team::MatchState::TeamSelect)
				text.text(0, _(strings::team_select_timer), vi_max(0, s32(TEAM_SELECT_TIME - Team::match_time)));
			else // waiting for players to connect
				text.text(0, _(strings::waiting_players), vi_max(1, Game::session.config.min_players - PlayerHuman::list.count()));

			{
				Vec2 p2 = p + Vec2(MENU_ITEM_PADDING, 0);
				UI::box(params, text.rect(p2).outset(MENU_ITEM_PADDING), UI::color_background);
				text.draw(params, p2);
			}
		}
		menu.draw_ui(params);
	}
	else if (mode == UIMode::PvpKillCam)
		scoreboard_draw(params, get<PlayerManager>(), ScoreboardPosition::Bottom);
	else if (mode == UIMode::PvpSpectate)
	{
		// we're dead but others still playing; spectate

		Entity* spectating = live_player_get(spectate_index);
		if (spectating)
		{
			UIText text;
			text.anchor_x = UIText::Anchor::Center;
			text.anchor_y = UIText::Anchor::Max;

			// username
			text.color = Team::color_ui(get<PlayerManager>()->team.ref()->team(), spectating->get<AIAgent>()->team);
			{
				PlayerManager* spectating_manager = spectating->get<PlayerCommon>()->manager.ref();
				text.icon = spectating_manager->flag(PlayerManager::FlagIsVip) ? Asset::Mesh::icon_vip : AssetNull;
				text.text_raw(gamepad, spectating_manager->username);
			}
			Vec2 pos = vp.size * Vec2(0.5f, 0.2f);
			UI::box(params, text.rect(pos).outset(MENU_ITEM_PADDING), UI::color_background);
			text.draw(params, pos);

			// "spectating"
			text.color = UI::color_accent();
			text.text(gamepad, _(strings::spectating));
			pos = vp.size * Vec2(0.5f, 0.1f);
			UI::box(params, text.rect(pos).outset(MENU_ITEM_PADDING), UI::color_background);
			text.draw(params, pos);

			match_timer_draw(params, ui_anchor(params) + Vec2(0, (UI_TEXT_SIZE_DEFAULT + 8.0f) * -UI::scale), UIText::Anchor::Min);
		}
	}
	else if (mode == UIMode::PvpGameOver)
	{
		// show victory/defeat/draw message
		UIText text;
		text.anchor_x = UIText::Anchor::Center;
		text.anchor_y = UIText::Anchor::Center;
		text.size = 32.0f;

		Team* winner = Team::winner.ref();
		if (winner == get<PlayerManager>()->team.ref()) // we won
		{
			text.color = UI::color_accent();
			text.text(gamepad, _(strings::victory));
		}
		else if (!winner) // it's a draw
		{
			text.color = UI::color_alert();
			text.text(gamepad, _(strings::draw));
		}
		else // we lost
		{
			text.color = UI::color_alert();
			text.text(gamepad, _(strings::defeat));
		}
		UIMenu::text_clip(&text, gamepad, Team::game_over_real_time, 20.0f);

		b8 show_score_summary = Game::real_time.total - Team::game_over_real_time > SCORE_SUMMARY_DELAY;
		Vec2 title_pos = show_score_summary
			? vp.size * Vec2(0.5f, 1.0f) + Vec2(0, (text.size + 32) * -UI::scale)
			: vp.size * Vec2(0.5f, 0.5f);
		UI::box(params, text.rect(title_pos).outset(16 * UI::scale), UI::color_background);
		text.draw(params, title_pos);

		if (show_score_summary)
		{
			// score summary screen

			UIText text;
			text.wrap_width = MENU_ITEM_WIDTH - MENU_ITEM_PADDING * 2.0f;
			text.anchor_x = UIText::Anchor::Center;
			text.anchor_y = UIText::Anchor::Max;

			Vec2 p = title_pos + Vec2(0, -2.0f * (MENU_ITEM_HEIGHT + MENU_ITEM_PADDING));

			match_timer_draw(params, p + Vec2(0, MENU_ITEM_HEIGHT + MENU_ITEM_PADDING * 0.5f), UIText::Anchor::Center);

			p.y -= MENU_ITEM_PADDING * 2.0f;
			score_summary_scroll.start(params, p + Vec2(0, MENU_ITEM_PADDING));
			AI::Team team = get<PlayerManager>()->team.ref()->team();
			for (s32 i = score_summary_scroll.top(); i < score_summary_scroll.bottom(Team::score_summary.length); i++)
			{
				const Team::ScoreSummaryItem& item = Team::score_summary[i];
				text.color = item.player.ref() == get<PlayerManager>() ? UI::color_accent() : Team::color_ui(team, item.team);

				UIText amount = text;
				amount.anchor_x = UIText::Anchor::Max;
				amount.wrap_width = 0;

				text.icon = item.icon;
				text.text_raw(gamepad, item.label);
				UIMenu::text_clip(&text, gamepad, Team::game_over_real_time + SCORE_SUMMARY_DELAY, 50.0f + r32(vi_min(i, 6)) * -5.0f);
				UI::box(params, text.rect(p).outset(MENU_ITEM_PADDING), UI::color_background);
				text.draw(params, p);
				text.icon = AssetNull;
				if (item.amount != -1)
				{
					amount.text(gamepad, "%d", item.amount);
					amount.draw(params, p + Vec2(MENU_ITEM_WIDTH * 0.5f - MENU_ITEM_PADDING, 0));
				}
				p.y -= text.bounds().y + MENU_ITEM_PADDING * 2.0f;
			}
			score_summary_scroll.end(params, p + Vec2(0, MENU_ITEM_PADDING));

			// press A to continue
			if (Game::real_time.total - Team::game_over_real_time > SCORE_SUMMARY_DELAY + SCORE_SUMMARY_ACCEPT_DELAY)
				player_button(vp, gamepad, get<PlayerManager>()->flag(PlayerManager::FlagScoreAccepted) ? strings::waiting : strings::prompt_accept, chat_focus == ChatFocus::None ? UIMenu::EnableInput::Yes : UIMenu::EnableInput::No, &params);
		}
	}

	// game timer
	if (mode == UIMode::PvpDefault || mode == UIMode::PvpUpgrade)
		match_timer_draw(params, ui_anchor(params) + Vec2(0, (UI_TEXT_SIZE_DEFAULT + 16.0f) * -UI::scale), UIText::Anchor::Min);

	// network error icon
#if !SERVER
	if (!Game::level.local && Net::Client::lagging())
		UI::mesh(params, Asset::Mesh::icon_network_error, vp.size * Vec2(0.9f, 0.5f), Vec2(UI_TEXT_SIZE_DEFAULT * UI::scale), UI::color_alert());
#endif

	// message
	if (msg_timer > 0.0f)
	{
		b8 flash = UI::flash_function(Game::real_time.total);
		b8 last_flash = UI::flash_function(Game::real_time.total - Game::real_time.delta);
		if (flash)
		{
			UIText text;
			text.text(gamepad, msg_text);
			text.anchor_x = UIText::Anchor::Center;
			text.anchor_y = UIText::Anchor::Center;
			text.color = flag(FlagMessageGood) ? UI::color_accent() : UI::color_alert();

			Vec2 pos = params.camera->viewport.size * Vec2(0.5f, 0.6f);
			Rect2 box = text.rect(pos).outset(MENU_ITEM_PADDING);
			UI::box(params, box, UI::color_background);
			text.draw(params, pos);
			if (!last_flash)
				Audio::post_global(flag(FlagMessageGood) ? AK::EVENTS::PLAY_MESSAGE_BEEP_GOOD : AK::EVENTS::PLAY_MESSAGE_BEEP_BAD, gamepad);
		}
	}

	{
		AI::Team my_team = get<PlayerManager>()->team.ref()->team();

		// draw kill popups
		if ((mode == UIMode::PvpDefault
			|| mode == UIMode::PvpKillCam)
			&& kill_popups.length > 0)
		{
			UIText text;
			text.anchor_x = UIText::Anchor::Center;
			text.anchor_y = UIText::Anchor::Max;
			text.color = UI::color_accent();

			Vec2 pos = params.camera->viewport.size * Vec2(0.5f, 0.75f);
			for (s32 i = 0; i < kill_popups.length; i++)
			{
				const KillPopup& k = kill_popups[i];
				PlayerManager* victim = k.victim.ref();
				if (victim)
				{
					text.icon = victim->flag(PlayerManager::FlagIsVip) ? Asset::Mesh::icon_vip : AssetNull;
					text.text(gamepad, _(strings::killed_player), victim->username);
					UIMenu::text_clip_timer(&text, gamepad, KILL_POPUP_TIME - k.timer, 50.0f);
					Rect2 box = text.rect(pos).outset(MENU_ITEM_PADDING);
					UI::box(params, box, UI::color_background);
					text.draw(params, pos);
					pos.y -= box.size.y;
				}
			}
		}

		draw_chats(params);
		draw_logs(params, my_team, gamepad);
	}

	if (mode == UIMode::ParkourDefault)
	{
		if (Game::session.type == SessionType::Multiplayer)
		{
			// waiting to start game
			UIText text;
			text.anchor_x = UIText::Anchor::Max;
			text.anchor_y = UIText::Anchor::Min;
			text.wrap_width = MENU_ITEM_WIDTH * 0.5f;
			text.color = UI::color_accent();
			r32 timer = vi_max(0.0f, (60.0f * r32(Game::session.config.time_limit_parkour_ready)) - Team::match_time);
			s32 remaining_minutes = s32(timer / 60.0f);
			s32 remaining_seconds = s32(timer - (remaining_minutes * 60.0f));
			if (Team::parkour_game_start_impending())
				text.text(gamepad, _(strings::deploy_timer), remaining_seconds);
			else
			{
				if (PlayerManager::list.count() >= Game::session.config.min_players)
				{
					AssetID ready = get<PlayerManager>()->flag(PlayerManager::FlagParkourReady) ? strings::prompt_parkour_unready : strings::prompt_parkour_ready;
					text.text(gamepad, _(strings::parkour_ready_status_timer), remaining_minutes, remaining_seconds, PlayerManager::count_parkour_ready(), PlayerManager::list.count(), _(ready));
				}
				else
					text.text(gamepad, _(strings::parkour_ready_status), Game::session.config.min_players - PlayerManager::list.count());
			}

			{
				Vec2 p = Vec2(params.camera->viewport.size.x, 0) + Vec2(MENU_ITEM_PADDING * -5.0f, MENU_ITEM_PADDING * 24.0f);
				UI::box(params, text.rect(p).outset(MENU_ITEM_PADDING), UI::color_background);
				text.draw(params, p);
			}
		}

		// overworld notifications
		if (Overworld::zone_under_attack() != AssetNull && Game::session.zone_under_attack_timer > ZONE_UNDER_ATTACK_THRESHOLD)
		{
			UIText text;
			text.anchor_x = UIText::Anchor::Max;
			text.anchor_y = UIText::Anchor::Min;
			text.wrap_width = MENU_ITEM_WIDTH - MENU_ITEM_PADDING * 2.0f;
			text.color = UI::color_alert();
			r32 timer = Game::session.zone_under_attack_timer - ZONE_UNDER_ATTACK_THRESHOLD;
			s32 remaining_minutes = s32(timer / 60.0f);
			s32 remaining_seconds = s32(timer - (remaining_minutes * 60.0f));
			text.text(gamepad, _(strings::prompt_zone_defend), Loader::level_name(Overworld::zone_under_attack()), remaining_minutes, remaining_seconds);
			UIMenu::text_clip_timer(&text, gamepad, ZONE_UNDER_ATTACK_TIME - timer, 80.0f);

			{
				Vec2 p = Vec2(params.camera->viewport.size.x, 0) + Vec2(MENU_ITEM_PADDING * -5.0f, MENU_ITEM_PADDING * 5.0f);
				UI::box(params, text.rect(p).outset(MENU_ITEM_PADDING), UI::color_background);
				text.draw(params, p);
			}

			{
				text.wrap_width = 0.0f;
				text.text(gamepad, _(strings::timer), remaining_minutes, remaining_seconds);
				text.anchor_x = UIText::Anchor::Center;
				text.anchor_y = UIText::Anchor::Min;
				text.color = UI::color_alert();
				Vec2 p = UI::indicator(params, Game::level.terminal.ref()->get<Transform>()->absolute_pos(), text.color, true);
				p.y += UI_TEXT_SIZE_DEFAULT * 1.5f * UI::scale;
				UI::box(params, text.rect(p).outset(MENU_ITEM_PADDING * 0.5f), UI::color_background);
				text.draw(params, p);
			}
		}

		if (audio_log != AssetNull && (flag(FlagAudioLogPlaying) || audio_log_prompt_timer > 0.0f))
		{
			UIText text;
			text.anchor_x = UIText::Anchor::Max;
			text.anchor_y = UIText::Anchor::Min;
			text.color = UI::color_accent();
			text.text(gamepad, _(flag(FlagAudioLogPlaying) ? strings::prompt_stop : strings::prompt_listen));
			UIMenu::text_clip_timer(&text, gamepad, ZONE_UNDER_ATTACK_TIME - audio_log_prompt_timer, 80.0f);

			{
				Vec2 p = Vec2(params.camera->viewport.size.x, 0) + Vec2(MENU_ITEM_PADDING * -5.0f, MENU_ITEM_PADDING * 24.0f);
				UI::box(params, text.rect(p).outset(MENU_ITEM_PADDING), UI::color_background);
				text.draw(params, p);
			}
		}
	}

	if (get<PlayerManager>()->instance.ref() && spawn_animation_timer > 0.0f)
		Menu::draw_letterbox(params, spawn_animation_timer, TRANSITION_TIME);

	if (mode == UIMode::Pause) // pause menu always drawn on top
		menu.draw_ui(params);
}

void PlayerHuman::audio_log_pickup(AssetID id)
{
	audio_log = id;
	flag(FlagAudioLogPlaying, false);
	audio_log_prompt_timer = 8.0f;
}

void PlayerHuman::audio_log_stop()
{
	audio_log = AssetNull;
	flag(FlagAudioLogPlaying, false);
	audio_log_prompt_timer = 0.0f;
	Scripts::AudioLogs::stop();
}

void PlayerHuman::flag(Flags f, b8 value)
{
	if (value)
		flags |= f;
	else
		flags &= ~f;
}

void PlayerHuman::draw_chats(const RenderParams& params) const
{
	AI::Team my_team = Game::level.mode == Game::Mode::Parkour ? AI::TeamNone : get<PlayerManager>()->team.ref()->team();

	UIText text;
	text.font = Asset::Font::pt_sans;
	text.anchor_x = UIText::Anchor::Min;
	text.anchor_y = UIText::Anchor::Min;
	text.wrap_width = MENU_ITEM_WIDTH - MENU_ITEM_PADDING * 2.0f;

	// calculate height
	s32 count = 0;
	r32 height = 0;
	for (s32 i = chats.length - 1; i >= 0 && count < 4; i--)
	{
		const ChatEntry& entry = chats[i];
		if (my_team == AI::TeamNone || AI::match(my_team, entry.mask))
		{
			text.icon = entry.vip ? Asset::Mesh::icon_vip : AssetNull;
			if (entry.mask == 1 << my_team)
				text.text(gamepad, "%s %s: %s", entry.username, _(strings::chat_team_prefix), entry.msg);
			else
				text.text(gamepad, "%s: %s", entry.username, entry.msg);
			height += text.bounds().y + MENU_ITEM_PADDING;
			count++;
		}
	}

	Vec2 base_pos = params.camera->viewport.size * Vec2(0, 1) + Vec2(1, -1) * MENU_ITEM_PADDING * 5.0f;
	if (count > 0)
	{
		base_pos.y -= height;
		Vec2 p = base_pos;
		UI::box(params, { p + Vec2(-MENU_ITEM_PADDING), Vec2(MENU_ITEM_WIDTH, height + MENU_ITEM_PADDING * 0.5f) }, UI::color_background);
		for (s32 i = chats.length - 1; i >= 0 && count > 0; i--)
		{
			const ChatEntry& entry = chats[i];
			if (my_team == AI::TeamNone || AI::match(my_team, entry.mask))
			{
				if (my_team == AI::TeamNone)
					text.color = UI::color_accent();
				else
					text.color = Team::color_ui(my_team, entry.team);
				text.icon = entry.vip ? Asset::Mesh::icon_vip : AssetNull;
				if (entry.mask == 1 << my_team)
					text.text(gamepad, "%s %s: %s", entry.username, _(strings::chat_team_prefix), entry.msg);
				else
					text.text(gamepad, "%s: %s", entry.username, entry.msg);
				text.draw(params, p);

				p.y += text.bounds().y + MENU_ITEM_PADDING;
				count--;
			}
		}
	}

	if (chat_focus != ChatFocus::None)
	{
		base_pos.y -= text.size * UI::scale + MENU_ITEM_PADDING * 4.0f;
		text.icon = AssetNull;
		chat_field.get(&text, 32);
		UI::box(params, text.rect(base_pos).outset(MENU_ITEM_PADDING), UI::color_background);
		text.color = chat_focus == ChatFocus::Team ? Team::color_ui_friend() : UI::color_default;
		text.draw(params, base_pos);
	}
}

void PlayerHuman::draw_logs(const RenderParams& params, AI::Team my_team, s8 gamepad)
{
	UIText text;
	text.anchor_x = UIText::Anchor::Max;
	text.anchor_y = UIText::Anchor::Max;

	s32 count = 0;
	for (s32 i = 0; i < logs.length && count < 4; i++)
	{
		if (my_team == AI::TeamNone || AI::match(my_team, logs[i].mask))
			count++;
	}

	Vec2 p = params.camera->viewport.size + Vec2(MENU_ITEM_PADDING * -5.0f);
	r32 height = count * (text.size * UI::scale + MENU_ITEM_PADDING * 2.0f);
	p.y -= height;
	UI::box(params, { p + Vec2(-MENU_ITEM_WIDTH + MENU_ITEM_PADDING, MENU_ITEM_PADDING * -2.5f), Vec2(MENU_ITEM_WIDTH, height) }, UI::color_background);
	r32 wrap_width = MENU_ITEM_WIDTH - MENU_ITEM_PADDING * 2.0f;
	for (s32 i = logs.length - 1; i >= 0 && count > 0; i--)
	{
		const LogEntry& entry = logs[i];
		if (my_team == AI::TeamNone || AI::match(my_team, entry.mask))
		{
			text.wrap_width = wrap_width;
			if (Game::level.mode == Game::Mode::Parkour)
				text.color = UI::color_accent();
			else
				text.color = Team::color_ui(my_team, entry.a_team);

			text.icon = entry.a_vip ? Asset::Mesh::icon_vip : AssetNull;
			if (entry.b[0])
			{
				char buffer[MAX_USERNAME + 1] = {};
				strncpy(buffer, entry.a, MAX_USERNAME);
				Unicode::truncate(buffer, 17, "...");
				text.text_raw(0, buffer);
			}
			else
				text.text_raw(gamepad, entry.a);

			UIMenu::text_clip(&text, gamepad, entry.timestamp, 80.0f);
			text.draw(params, p);

			if (entry.b[0])
			{
				// "a killed b" format
				text.wrap_width = 0;
				text.anchor_x = UIText::Anchor::Center;
				text.color = UI::color_default;
				text.clip = 0;
				text.icon = AssetNull;
				text.text_raw(gamepad, "->");
				text.draw(params, p + Vec2(wrap_width * -0.5f, 0));

				text.anchor_x = UIText::Anchor::Max;
				if (Game::level.mode == Game::Mode::Parkour)
					text.color = UI::color_accent();
				else
					text.color = Team::color_ui(my_team, entry.b_team);
				text.icon = entry.b_vip ? Asset::Mesh::icon_vip : AssetNull;
				{
					char buffer[MAX_USERNAME + 1] = {};
					strncpy(buffer, entry.b, MAX_USERNAME);
					Unicode::truncate(buffer, 17, "...");
					text.text_raw(0, buffer);
				}
				UIMenu::text_clip(&text, gamepad, entry.timestamp, 80.0f);
				text.draw(params, p);
			}
			p.y += (text.size * UI::scale) + MENU_ITEM_PADDING * 2.0f;
			count--;
		}
	}
}

void PlayerHuman::draw_alpha_late(const RenderParams& params) const
{
	if (ui_mode() == UIMode::PvpKillCam)
	{
		Entity* k = killed_by.ref();
		if (k)
		{
			RenderSync* sync = params.sync;
			sync->write(RenderOp::DepthTest);
			sync->write(false);

			{
				RenderParams p = params;
				p.flags |= RenderFlagAlphaOverride;
				if (k->has<View>())
					k->get<View>()->draw(p);
				else if (k->has<SkinnedModel>())
					k->get<SkinnedModel>()->draw(p);
			}

			sync->write(RenderOp::DepthTest);
			sync->write(true);
		}
	}
}

PlayerCommon::PlayerCommon(PlayerManager* m)
	: angle_horizontal(),
	angle_vertical(),
	recoil(),
	recoil_velocity(),
	manager(m)
{
}

void PlayerCommon::awake()
{
	link_arg<const HealthEvent&, &PlayerCommon::health_changed>(get<Health>()->changed);
	manager.ref()->instance = entity();
}

r32 PlayerCommon::angle_vertical_total() const
{
	return LMath::clampf(angle_vertical - recoil, -DRONE_VERTICAL_ANGLE_LIMIT, DRONE_VERTICAL_ANGLE_LIMIT);
}

void PlayerCommon::recoil_add(r32 velocity)
{
	recoil_velocity = vi_max(recoil_velocity, velocity);
}

void PlayerCommon::update(const Update& u)
{
	recoil_velocity = vi_max(vi_min(-0.1f, recoil * -9.0f), recoil_velocity - 8.0f * u.time.delta);
	recoil = vi_max(0.0f, recoil + recoil_velocity * u.time.delta);
}

void PlayerCommon::health_changed(const HealthEvent& e)
{
	if (e.hp + e.shield < 0 && e.source.ref())
	{
		PlayerManager* rewardee = PlayerManager::owner(e.source.ref());
		if (rewardee && rewardee->team.ref() != manager.ref()->team.ref())
			rewardee->add_energy_and_notify(s32(e.hp + e.shield) * -ENERGY_DRONE_DAMAGE);
	}
}

b8 PlayerCommon::movement_enabled() const
{
	if (has<Drone>())
	{
		return get<Drone>()->state() == Drone::State::Crawl // must be attached to wall
			&& manager.ref()->state() == PlayerManager::State::Default; // can't move while upgrading and stuff
	}
	else
		return true;
}

Entity* PlayerCommon::incoming_attacker() const
{
	Vec3 me = get<Transform>()->absolute_pos();

	// check incoming Drones
	PlayerManager* manager = get<PlayerCommon>()->manager.ref();
	for (auto i = PlayerCommon::list.iterator(); !i.is_last(); i.next())
	{
		const PlayerManager::Visibility& visibility = PlayerManager::visibility[PlayerManager::visibility_hash(manager, i.item()->manager.ref())];
		if (visibility.value)
		{
			// determine if they're attacking us
			if (i.item()->get<Drone>()->state() != Drone::State::Crawl
				&& Vec3::normalize(i.item()->get<Drone>()->velocity).dot(Vec3::normalize(me - i.item()->get<Transform>()->absolute_pos())) > 0.98f)
				return i.item()->entity();
		}
	}

	// check incoming bolts
	AI::Team my_team = get<AIAgent>()->team;
	for (auto i = Bolt::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->team != my_team)
		{
			Vec3 velocity = Vec3::normalize(i.item()->velocity);
			Vec3 bolt_pos = i.item()->get<Transform>()->absolute_pos();
			Vec3 to_me = me - bolt_pos;
			r32 dot = velocity.dot(to_me);
			if (dot > 0.0f && dot < DRONE_MAX_DISTANCE && velocity.dot(Vec3::normalize(to_me)) > 0.98f)
			{
				// only worry about it if it can actually see us
				btCollisionWorld::ClosestRayResultCallback ray_callback(me, bolt_pos);
				Physics::raycast(&ray_callback, ~CollisionDroneIgnore);
				if (!ray_callback.hasHit())
					return i.item()->entity();
			}
		}
	}

	// check grenades
	for (auto i = Grenade::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->team != my_team && i.item()->state != Grenade::State::Exploded)
		{
			Vec3 grenade_pos = i.item()->get<Transform>()->absolute_pos();
			if ((grenade_pos - me).length_squared() < GRENADE_RANGE * GRENADE_RANGE)
			{
				// only worry about it if it can actually see us
				btCollisionWorld::ClosestRayResultCallback ray_callback(me, grenade_pos);
				Physics::raycast(&ray_callback, CollisionStatic);
				if (!ray_callback.hasHit())
					return i.item()->entity();
			}
		}
	}

	return nullptr;
}

Vec3 PlayerCommon::look_dir() const
{
	return look() * Vec3(0, 0, 1);
}

Quat PlayerCommon::look() const
{
	return Quat::euler(0, angle_horizontal, angle_vertical_total());
}

void PlayerCommon::clamp_rotation(const Vec3& direction, r32 dot_limit)
{
	Vec3 forward = look_dir();

	r32 dot = forward.dot(direction);
	s32 iterations = 0;
	b8 apply = false;
	while (iterations < 10 && dot < -dot_limit - 0.002f)
	{
		apply = true;
		forward = Vec3::normalize(forward - (dot + dot_limit) * direction);
		dot = forward.dot(direction);
		iterations++;
	}
	if (apply)
	{
		angle_vertical = -asinf(forward.y);
		angle_horizontal = atan2f(forward.x, forward.z);
	}
}

void reticle_raycast(RaycastCallbackExcept* ray_callback)
{
	for (auto i = UpgradeStation::list.iterator(); !i.is_last(); i.next()) // ignore drones inside upgrade stations
	{
		if (i.item()->drone.ref())
			ray_callback->ignore(i.item()->drone.ref()->entity());
	}
	Physics::raycast(ray_callback, ~CollisionDroneIgnore & ~CollisionAllTeamsForceField);
}

b8 PlayerControlHuman::net_msg(Net::StreamRead* p, PlayerControlHuman* c, Net::MessageSource src, Net::SequenceID seq)
{
	using Stream = Net::StreamRead;

	PlayerControlHumanNet::Message msg;
	if (!serialize_msg(p, &msg))
		net_error();

	if (src != Net::MessageSource::Loopback // it's from remote
		&& !Game::level.local // we are a client
		&& msg.type != PlayerControlHumanNet::Message::Type::Reflect) // reflect messages can go both ways; all others go only from client to server
		net_error();

	if (!c)
	{
		// ignore, we're probably just receiving this message from the client after the player has already been destroyed
		return true;
	}

	if (src == Net::MessageSource::Invalid // message is from a client who doesn't actually own this player
		|| (msg.ability != Ability::None && !c->player.ref()->get<PlayerManager>()->has_ability(msg.ability))) // don't have that ability
	{
		// invalid message, ignore
		net_error();
	}

	if (src == Net::MessageSource::Remote)
	{
#if SERVER
		// update RTT based on the sequence number
		c->rtt = Net::Server::rtt(c->player.ref(), seq);

		c->player.ref()->afk_timer = AFK_TIME;
#endif

		if (msg.type == PlayerControlHumanNet::Message::Type::Dash
			|| msg.type == PlayerControlHumanNet::Message::Type::DashCombo
			|| msg.type == PlayerControlHumanNet::Message::Type::Go
			|| msg.type == PlayerControlHumanNet::Message::Type::Spot)
		{
			// make sure we are where the remote thinks we are when we start processing this message
			r32 dist_sq = (c->get<Transform>()->absolute_pos() - msg.pos).length_squared();
			r32 tolerance_pos;
			c->remote_position(&tolerance_pos);

			if (dist_sq < tolerance_pos * tolerance_pos)
				c->get<Transform>()->absolute(msg.pos, msg.rot);
			else
			{
#if DEBUG_NET_SYNC
				vi_debug_break();
#endif
				return true;
			}
		}
	}

	switch (msg.type)
	{
		case PlayerControlHumanNet::Message::Type::Dash:
		{
			if (c->get<Drone>()->dash_start(msg.dir, c->get<Transform>()->absolute_pos())) // HACK: set target to current position so that it is not used
				c->flag(PlayerControlHuman::FlagTryPrimary | PlayerControlHuman::FlagTrySecondary, false);
			break;
		}
		case PlayerControlHumanNet::Message::Type::DashCombo:
		{
			if (c->get<Drone>()->dash_start(msg.dir, msg.target))
				c->flag(PlayerControlHuman::FlagTryPrimary | PlayerControlHuman::FlagTrySecondary, false);
			break;
		}
		case PlayerControlHumanNet::Message::Type::Go:
		{
			Ability old_ability = c->get<Drone>()->current_ability;
			c->get<Drone>()->current_ability = msg.ability;

			if (c->get<Drone>()->go(msg.dir))
			{
				if (msg.ability == Ability::None)
					c->flag(PlayerControlHuman::FlagTryPrimary | PlayerControlHuman::FlagTrySecondary, false);
				else if (msg.ability == Ability::Bolter)
					c->player.ref()->rumble_add(0.2f);
				else
				{
					c->flag(PlayerControlHuman::FlagTryPrimary, false);
					c->player.ref()->rumble_add(0.5f);
				}
			}

			if (AbilityInfo::list[s32(msg.ability)].type == AbilityInfo::Type::Other)
				c->get<Drone>()->current_ability = old_ability;

			break;
		}
		case PlayerControlHumanNet::Message::Type::UpgradeStart:
			c->get<PlayerCommon>()->manager.ref()->upgrade_start(msg.upgrade, msg.ability_slot);
			break;
		case PlayerControlHumanNet::Message::Type::Reflect:
		{
			if (src == Net::MessageSource::Remote)
			{
				vi_assert(Game::level.local); // server should not send reflect messages to client
				c->get<Drone>()->handle_remote_reflection(msg.entity.ref(), msg.pos, msg.dir);
			}
			break;
		}
		case PlayerControlHumanNet::Message::Type::AbilitySelect:
		{
			if (msg.ability == Ability::None || c->get<PlayerCommon>()->manager.ref()->has_ability(msg.ability))
			{
				if (msg.ability != Ability::None)
				{
					// drop flag if we're holding one
					if (Game::level.local)
					{
						Flag* flag = c->get<Drone>()->flag.ref();
						if (flag)
							flag->drop();
					}
					c->get<Drone>()->flag = nullptr;
				}

				c->get<Drone>()->ability(msg.ability);
			}
			break;
		}
		case PlayerControlHumanNet::Message::Type::Spot:
		{
			if (Game::level.local) // spotting is all server-side
			{
#if SERVER
				vi_assert(src == Net::MessageSource::Remote);
#endif
				if (c->spot_timer == 0.0f)
				{
					r32 closest_dot = 0.95f;
					Target* target = nullptr;

					// turrets
					for (auto i = Turret::list.iterator(); !i.is_last(); i.next())
					{
						r32 dot = Vec3::normalize(i.item()->get<Transform>()->absolute_pos() - msg.target).dot(msg.dir);
						if (dot > closest_dot)
						{
							closest_dot = dot;
							target = i.item()->get<Target>();
						}
					}

					// minion spawners
					for (auto i = MinionSpawner::list.iterator(); !i.is_last(); i.next())
					{
						r32 dot = Vec3::normalize(i.item()->get<Transform>()->absolute_pos() - msg.target).dot(msg.dir);
						if (dot > closest_dot)
						{
							closest_dot = dot;
							target = i.item()->get<Target>();
						}
					}

					// force fields
					for (auto i = ForceField::list.iterator(); !i.is_last(); i.next())
					{
						if (!(i.item()->flags & ForceField::FlagInvincible))
						{
							Vec3 to_target = i.item()->get<Transform>()->absolute_pos() - msg.target;
							r32 distance = to_target.length();
							if (distance < DRONE_MAX_DISTANCE)
							{
								r32 dot = (to_target / distance).dot(msg.dir);
								if (dot > closest_dot)
								{
									closest_dot = dot;
									target = i.item()->get<Target>();
								}
							}
						}
					}

					// batteries
					for (auto i = Battery::list.iterator(); !i.is_last(); i.next())
					{
						r32 dot = Vec3::normalize(i.item()->get<Transform>()->absolute_pos() - msg.target).dot(msg.dir);
						if (dot > closest_dot)
						{
							closest_dot = dot;
							target = i.item()->get<Target>();
						}
					}

					AI::Team my_team = c->get<AIAgent>()->team;

					// flags
					for (auto i = Flag::list.iterator(); !i.is_last(); i.next())
					{
						if (i.item()->team != my_team || !i.item()->get<Transform>()->parent.ref()) // can't spot it if it's our own flag being carried by an enemy
						{
							r32 dot = Vec3::normalize(i.item()->get<Target>()->absolute_pos() - msg.target).dot(msg.dir);
							if (dot > closest_dot)
							{
								closest_dot = dot;
								target = i.item()->get<Target>();
							}
						}
					}

					// minions
					for (auto i = Minion::list.iterator(); !i.is_last(); i.next())
					{
						if (i.item()->get<AIAgent>()->team != my_team) // only spot enemies
						{
							r32 dot = Vec3::normalize(i.item()->get<Target>()->absolute_pos() - msg.target).dot(msg.dir);
							if (dot > closest_dot && i.item()->can_see(c->entity()))
							{
								closest_dot = dot;
								target = i.item()->get<Target>();
							}
						}
					}

					// drones
					for (auto i = Drone::list.iterator(); !i.is_last(); i.next())
					{
						if (i.item()->get<AIAgent>()->team != my_team) // only spot enemies
						{
							r32 dot = Vec3::normalize(i.item()->get<Transform>()->absolute_pos() - msg.target).dot(msg.dir);
							if (dot > closest_dot)
							{
								b8 visible = player_determine_visibility(c->get<PlayerCommon>(), i.item()->get<PlayerCommon>());
								if (visible)
								{
									closest_dot = dot;
									target = i.item()->get<Target>();
								}
							}
						}
					}

					if (target)
					{
						PlayerManager* manager = c->get<PlayerCommon>()->manager.ref();
						manager->spot(target);
						for (auto i = Minion::list.iterator(); !i.is_last(); i.next())
						{
							if (i.item()->get<AIAgent>()->team == my_team)
							{
								PlayerManager* owner = i.item()->owner.ref();
								if (!owner || owner == manager) // don't boss around minions created by other players
									i.item()->new_goal(); // minion will automatically go after spotted entities
							}
						}
						c->spot_timer = 2.0f;
					}
				}
				else
					c->spot_timer = vi_max(0.5f, vi_min(6.0f, c->spot_timer * 2.0f));
			}
			break;
		}
		default:
			vi_assert(false);
			break;
	}

	return true;
}

s32 PlayerControlHuman::count_local()
{
	s32 count = 0;
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->local())
			count++;
	}
	return count;
}

void PlayerControlHuman::drone_done_flying_or_dashing()
{
	camera_shake_timer = 0.0f; // stop screen shake
}

void player_add_target_indicator(PlayerControlHuman* p, Target* target, PlayerControlHuman::TargetIndicator::Type type)
{
	Vec3 me = p->get<Transform>()->absolute_pos();

	b8 show = true;
	r32 range = p->get<Drone>()->range();
	b8 in_range = (target->absolute_pos() - me).length_squared() < range * range;
	if (!in_range)
	{
		// out of range; some indicators just disappear; others change
		if (type == PlayerControlHuman::TargetIndicator::Type::Battery)
			type = PlayerControlHuman::TargetIndicator::Type::BatteryOutOfRange;
		else if (type == PlayerControlHuman::TargetIndicator::Type::BatteryEnemy)
			type = PlayerControlHuman::TargetIndicator::Type::BatteryEnemyOutOfRange;
		else if (type == PlayerControlHuman::TargetIndicator::Type::BatteryFriendly)
			type = PlayerControlHuman::TargetIndicator::Type::BatteryFriendlyOutOfRange;
		else
			show = false; // don't show this indicator because it's out of range
	}

	if (show)
	{
		if (in_range && type != PlayerControlHuman::TargetIndicator::Type::BatteryFriendly)
		{
			// calculate target intersection trajectory
			Vec3 intersection;
			if (p->get<Drone>()->predict_intersection(target, nullptr, &intersection, p->get<Drone>()->target_prediction_speed()))
				p->target_indicators.add({ intersection, target->velocity(), target, type });
		}
		else // just show the target's actual position
			p->target_indicators.add({ target->absolute_pos(), target->velocity(), target, type });
	}
}

void player_collect_target_indicators(PlayerControlHuman* p)
{
	p->target_indicators.length = 0;

	Vec3 me = p->get<Transform>()->absolute_pos();
	AI::Team team = p->get<AIAgent>()->team;

	// drone indicators
	for (auto other_player = PlayerCommon::list.iterator(); !other_player.is_last(); other_player.next())
	{
		if (other_player.item()->get<AIAgent>()->team != team)
		{
			b8 visible = player_determine_visibility(p->get<PlayerCommon>(), other_player.item());

			if (visible)
				player_add_target_indicator(p, other_player.item()->get<Target>(), PlayerControlHuman::TargetIndicator::Type::DroneVisible);
		}
	}

	// headshot indicators
	for (auto i = Minion::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->get<AIAgent>()->team != team)
			player_add_target_indicator(p, i.item()->get<Target>(), PlayerControlHuman::TargetIndicator::Type::Minion);
	}

	// batteries
	if (Game::level.has_feature(Game::FeatureLevel::Batteries))
	{
		for (auto i = Battery::list.iterator(); !i.is_last(); i.next())
		{
			PlayerControlHuman::TargetIndicator::Type type = i.item()->team == team
				? PlayerControlHuman::TargetIndicator::Type::BatteryFriendly
				: (i.item()->team == AI::TeamNone ? PlayerControlHuman::TargetIndicator::Type::Battery : PlayerControlHuman::TargetIndicator::Type::BatteryEnemy);
			player_add_target_indicator(p, i.item()->get<Target>(), type);
		}
	}

	// rectifiers
	for (auto i = Rectifier::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->team != team)
			player_add_target_indicator(p, i.item()->get<Target>(), PlayerControlHuman::TargetIndicator::Type::Rectifier);
	}

	// minion spawners
	for (auto i = MinionSpawner::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->team != team)
			player_add_target_indicator(p, i.item()->get<Target>(), PlayerControlHuman::TargetIndicator::Type::MinionSpawner);
	}

	// turrets
	for (auto i = Turret::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->team != team)
			player_add_target_indicator(p, i.item()->get<Target>(), PlayerControlHuman::TargetIndicator::Type::Turret);
	}

	// grenades
	for (auto i = Grenade::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->team != team)
			player_add_target_indicator(p, i.item()->get<Target>(), PlayerControlHuman::TargetIndicator::Type::Grenade);
	}

	// force fields
	for (auto i = ForceField::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->team != team && !(i.item()->flags & ForceField::FlagInvincible))
			player_add_target_indicator(p, i.item()->get<Target>(), PlayerControlHuman::TargetIndicator::Type::ForceField);
	}
}

void player_ability_select(const Update& u, PlayerControlHuman* control, s32 index)
{
	PlayerManager* manager = control->player.ref()->get<PlayerManager>();

	Ability ability;
	if (index == 0)
		ability = Ability::None;
	else
	{
		ability = manager->abilities[index - 1];
		if (ability == Ability::None)
			return;
	}

	const AbilityInfo& info = AbilityInfo::list[s32(ability)];
	if (info.type == AbilityInfo::Type::Passive)
	{
		// do nothing
	}
	else if (info.type == AbilityInfo::Type::Other)
	{
		if (manager->ability_valid(ability))
		{
			PlayerControlHumanNet::Message msg;
			control->get<Transform>()->absolute(&msg.pos, &msg.rot);
			msg.dir = Vec3::normalize(control->reticle.pos - msg.pos);
			msg.type = PlayerControlHumanNet::Message::Type::Go;
			msg.ability = ability;
			PlayerControlHumanNet::send(control, &msg);
		}
	}
	else if (control->get<Drone>()->current_ability != ability)
		control->ability_select(ability);
}

PlayerControlHuman::PlayerControlHuman(PlayerHuman* p)
	: fov(Settings::effective_fov()),
	flags(),
	camera_shake_timer(),
	target_indicators(),
	last_gamepad_input_time(),
	gamepad_rotation_speed(),
	remote_control(),
	spot_timer(),
	player(p),
	position_history(),
	cooldown_last(),
#if SERVER
	rtt(),
#endif
	anim_base()
{
}

void PlayerControlHuman::awake()
{
#if SERVER
	player.ref()->afk_timer = AFK_TIME;

	rtt = Net::rtt(player.ref());
#endif

	if (local())
	{
		get<Audio>()->entry()->flag(AudioEntry::FlagEnableObstructionOcclusion | AudioEntry::FlagEnableForceFieldObstruction, false);
		get<SkinnedModel>()->first_person_camera = player.ref()->camera.ref();

		if (!Game::level.local)
		{
			Transform* t = get<Transform>();
			remote_control.pos = t->pos;
			remote_control.rot = t->rot;
			remote_control.parent = t->parent;
		}
	}

	player.ref()->killed_by = nullptr;
	player.ref()->spawn_animation_timer = TRANSITION_TIME * 0.5f;

	link_arg<const HealthEvent&, &PlayerControlHuman::health_changed>(get<Health>()->changed);
	link_arg<Entity*, &PlayerControlHuman::killed>(get<Health>()->killed);

	if (has<Drone>())
	{
		last_pos = get<Drone>()->center_lerped();
		link<&PlayerControlHuman::drone_detaching>(get<Drone>()->detaching);
		link<&PlayerControlHuman::drone_done_flying_or_dashing>(get<Drone>()->done_flying);
		link<&PlayerControlHuman::drone_done_flying_or_dashing>(get<Drone>()->done_dashing);
		link_arg<const DroneReflectEvent&, &PlayerControlHuman::drone_reflecting>(get<Drone>()->reflecting);
		link_arg<Entity*, &PlayerControlHuman::hit_target>(get<Drone>()->hit);

		player.ref()->camera_center = get<Drone>()->center_lerped();
	}
	else
	{
		last_pos = get<Transform>()->absolute_pos();
		link_arg<r32, &PlayerControlHuman::parkour_landed>(get<Walker>()->land);
		link<&PlayerControlHuman::terminal_enter_animation_callback>(get<Animator>()->trigger(Asset::Animation::character_terminal_enter, 2.5f));
		link<&PlayerControlHuman::interact_animation_callback>(get<Animator>()->trigger(Asset::Animation::character_interact, 3.8f));
		link<&PlayerControlHuman::interact_animation_callback>(get<Animator>()->trigger(Asset::Animation::character_terminal_exit, 4.0f));
		Audio::post_global(AK::EVENTS::PLAY_PARKOUR_WIND, player.ref()->gamepad);
		Audio::param_global(AK::GAME_PARAMETERS::PARKOUR_WIND, 0.0f, player.ref()->gamepad);
	}
}

PlayerControlHuman::~PlayerControlHuman()
{
	if (has<Parkour>())
	{
		get<Audio>()->stop_all();
		if (player.ref()) // if the player has already been deleted, everything's getting deleted, STOP_ALL will stop it, don't worry about it
			Audio::post_global(AK::EVENTS::STOP_PARKOUR_WIND, player.ref()->gamepad);
	}
}

void PlayerControlHuman::health_changed(const HealthEvent& e)
{
	s8 total = e.hp + e.shield;
	if (total < 0)
	{
		if (has<Drone>()) // de-scope when damaged
			flag(PlayerControlHuman::FlagTrySecondary, false);
		if (has<Drone>() || e.source.ref()) // no rumble if you just fall in parkour mode
			camera_shake(total < -1 ? 1.0f : 0.7f);
	}
}

void PlayerControlHuman::killed(Entity* killed_by)
{
	if (killed_by)
	{
		if (killed_by->has<Bolt>())
			player.ref()->killed_by = killed_by->get<Bolt>()->owner.ref();
		else if (killed_by->has<Grenade>())
		{
			PlayerManager* owner = killed_by->get<Grenade>()->owner.ref();
			player.ref()->killed_by = owner ? owner->instance.ref() : nullptr;
		}
		else
			player.ref()->killed_by = killed_by;
	}
	else
		player.ref()->killed_by = nullptr;
}

void PlayerControlHuman::drone_reflecting(const DroneReflectEvent& e)
{
	// send message if we are a client in a network game.
	if (!Game::level.local)
	{
		PlayerControlHumanNet::Message msg;
		get<Transform>()->absolute(&msg.pos, &msg.rot);
		msg.dir = Vec3::normalize(e.new_velocity);
		msg.entity = e.entity;
		msg.type = PlayerControlHumanNet::Message::Type::Reflect;
		PlayerControlHumanNet::send(this, &msg);
	}
}

void PlayerControlHuman::parkour_landed(r32 velocity_diff)
{
	ParkourState parkour_state = get<Parkour>()->fsm.current;
	if (velocity_diff < LANDING_VELOCITY_LIGHT
		&& (parkour_state == ParkourState::Normal || parkour_state == ParkourState::HardLanding))
	{
		player.ref()->rumble_add(velocity_diff < LANDING_VELOCITY_HARD ? 0.5f : 0.2f);
	}
}

void PlayerControlHuman::terminal_exit()
{
	get<Animator>()->layers[3].set(Asset::Animation::character_terminal_exit, 0.0f); // bypass animation blending
	get<PlayerControlHuman>()->anim_base = Game::level.terminal_interactable.ref();
}

void PlayerControlHuman::terminal_enter_animation_callback()
{
	Game::level.terminal_interactable.ref()->get<Interactable>()->interact_no_animation();
}

void PlayerControlHuman::interact_animation_callback()
{
	anim_base = nullptr;
}

void PlayerControlHuman::hit_target(Entity* target)
{
	player.ref()->rumble_add(0.5f);
}

void PlayerControlHuman::drone_detaching()
{
	camera_shake_timer = 0.0f; // stop screen shake
}

void PlayerControlHuman::camera_shake(r32 amount) // amount ranges from 0 to 1
{
	if (!has<Drone>() || get<Drone>()->state() == Drone::State::Crawl) // don't shake the screen if we reflect off something in the air
		camera_shake_timer = vi_max(camera_shake_timer, camera_shake_time * amount);
	player.ref()->rumble_add(amount);
}

b8 PlayerControlHuman::input_enabled() const
{
	PlayerHuman::UIMode ui_mode = player.ref()->ui_mode();
	return !Console::visible
		&& player.ref()->chat_focus == PlayerHuman::ChatFocus::None
		&& !cinematic_active()
		&& (player.ref()->gamepad != 0 || !Overworld::active())
		&& (ui_mode == PlayerHuman::UIMode::PvpDefault || ui_mode == PlayerHuman::UIMode::ParkourDefault)
		&& Team::match_state == Team::MatchState::Active
		&& !Menu::dialog_active(player.ref()->gamepad)
		&& !anim_base.ref();
}

b8 PlayerControlHuman::movement_enabled() const
{
	return input_enabled() && get<PlayerCommon>()->movement_enabled();
}

void PlayerControlHuman::ability_select(Ability a)
{
	const AbilityInfo& info = AbilityInfo::list[s32(a)];
	vi_assert(info.type == AbilityInfo::Type::Shoot || info.type == AbilityInfo::Type::Build);
	PlayerControlHumanNet::Message msg;
	msg.type = PlayerControlHumanNet::Message::Type::AbilitySelect;
	msg.ability = a;
	PlayerControlHumanNet::send(this, &msg);
}

void PlayerControlHuman::update_camera_input(const Update& u, r32 overall_rotation_multiplier, r32 gamepad_rotation_multiplier)
{
	if (input_enabled())
	{
		s32 gamepad = player.ref()->gamepad;
		if (gamepad == 0)
		{
			r32 s = overall_rotation_multiplier * speed_mouse * Settings::gamepads[gamepad].effective_sensitivity_mouse();
			get<PlayerCommon>()->angle_horizontal -= u.input->mouse_relative.x * s;
			get<PlayerCommon>()->angle_vertical += u.input->mouse_relative.y * s * (Settings::gamepads[gamepad].invert_y ? -1.0f : 1.0f);
		}

		if (u.input->gamepads[gamepad].type != Gamepad::Type::None)
		{
			Vec2 adjustment = Vec2
			(
				-u.input->gamepads[gamepad].right_x,
				u.input->gamepads[gamepad].right_y * (Settings::gamepads[gamepad].invert_y ? -1.0f : 1.0f)
			);
			Input::dead_zone(&adjustment.x, &adjustment.y);
			adjustment *= overall_rotation_multiplier * speed_joystick * Settings::gamepads[gamepad].effective_sensitivity_gamepad() * u.real_time.delta * gamepad_rotation_multiplier;
			r32 adjustment_length = adjustment.length();
			if (adjustment_length > 0.0f)
			{
				last_gamepad_input_time = u.real_time.total;

				// ramp gamepad rotation speed up at a constant rate until we reach the desired speed
				adjustment /= adjustment_length;
				gamepad_rotation_speed = vi_min(adjustment_length, gamepad_rotation_speed + u.real_time.delta * gamepad_rotation_acceleration);
			}
			else
			{
				// ramp gamepad rotation speed back down
				gamepad_rotation_speed = vi_max(0.0f, gamepad_rotation_speed + u.real_time.delta * -gamepad_rotation_acceleration);
			}
			get<PlayerCommon>()->angle_horizontal += adjustment.x * gamepad_rotation_speed;
			get<PlayerCommon>()->angle_vertical += adjustment.y * gamepad_rotation_speed;
		}

		get<PlayerCommon>()->angle_vertical = LMath::clampf(get<PlayerCommon>()->angle_vertical, -DRONE_VERTICAL_ANGLE_LIMIT, DRONE_VERTICAL_ANGLE_LIMIT);
	}
}

Vec3 PlayerControlHuman::get_movement(const Update& u, const Quat& rot, s8 gamepad)
{
	Vec3 movement = Vec3::zero;
	if (Game::ui_gamepad_types[gamepad] == Gamepad::Type::None)
	{
		if (u.input->get(Controls::Forward, gamepad))
			movement += Vec3(0, 0, 1);
		if (u.input->get(Controls::Backward, gamepad))
			movement += Vec3(0, 0, -1);
		if (u.input->get(Controls::Right, gamepad))
			movement += Vec3(-1, 0, 0);
		if (u.input->get(Controls::Left, gamepad))
			movement += Vec3(1, 0, 0);
	}
	else
	{
		Vec2 gamepad_movement(-u.input->gamepads[gamepad].left_x, -u.input->gamepads[gamepad].left_y);
		Input::dead_zone(&gamepad_movement.x, &gamepad_movement.y);
		movement.x = gamepad_movement.x;
		movement.z = gamepad_movement.y;
	}

	movement = rot * movement;

	r32 length_sq = movement.length_squared();
	return length_sq < 1.0f ? movement : movement / sqrtf(length_sq);
}

b8 PlayerControlHuman::local() const
{
	return player.ref()->local();
}

void PlayerControlHuman::remote_control_handle(const PlayerControlHuman::RemoteControl& control)
{
#if SERVER
	remote_control = control;
	if (control.movement.length_squared() > 0.0f)
		player.ref()->afk_timer = AFK_TIME;

	if (input_enabled())
	{
		if (has<Parkour>())
		{
			// remote control by a client
			// just trust the client, it's k
			Vec3 abs_pos_last = last_pos;
			get<Transform>()->pos = remote_control.pos;
			get<Transform>()->rot = Quat::identity;
			last_pos = remote_control.pos;
			get<Walker>()->absolute_pos(last_pos); // force rigid body
			get<PlayerCommon>()->angle_horizontal = get<Walker>()->rotation = get<Walker>()->target_rotation = remote_control.angle_horizontal;
			get<PlayerCommon>()->angle_vertical = remote_control.angle_vertical;
			get<Parkour>()->lean = remote_control.lean;
			get<Parkour>()->relative_wall_run_normal = remote_control.wall_normal;
			get<SkinnedModel>()->offset.translation(remote_control.model_offset);
			get<Target>()->net_velocity = get<Target>()->net_velocity * 0.7f + ((last_pos - abs_pos_last) / Net::tick_rate()) * 0.3f;
			for (s32 i = 0; i < MAX_ANIMATIONS; i++)
			{
				const AnimationLayer& input = remote_control.animations[i];
				Animator::Layer* output = &get<Animator>()->layers[i];
				output->animation = input.asset;
				output->time = input.time;
			}
		}
		else if (get<Drone>()->state() == Drone::State::Crawl // only if we're crawling
			&& remote_control.parent.ref()) // and only if the remote thinks we're crawling
		{
			Transform* t = get<Transform>();
			Vec3 abs_pos;
			Quat abs_rot;
			t->absolute(&abs_pos, &abs_rot);

			get<PlayerCommon>()->angle_horizontal = remote_control.angle_horizontal;
			get<PlayerCommon>()->angle_vertical = remote_control.angle_vertical;

			// if the remote position is close to what we think it is, snap to it
			Vec3 remote_abs_pos = remote_control.pos;
			Quat remote_abs_rot = remote_control.rot;
			remote_control.parent.ref()->to_world(&remote_abs_pos, &remote_abs_rot);
			r32 tolerance_pos;
			r32 tolerance_rot;
			remote_position(&tolerance_pos, &tolerance_rot);
			if ((remote_abs_pos - abs_pos).length_squared() < tolerance_pos * tolerance_pos
				&& Quat::angle(remote_abs_rot, abs_rot) < tolerance_rot)
			{
				t->parent = remote_control.parent;
				t->absolute(remote_abs_pos, remote_abs_rot);
			}
#if DEBUG_NET_SYNC
			else
				vi_debug("%f rejected sync. distance: %f", Game::real_time.total, (remote_abs_pos - abs_pos).length());
#endif
		}
	}
#else
	vi_assert(false); // this should only get called on the server
#endif
}

PlayerControlHuman::RemoteControl PlayerControlHuman::remote_control_get(const Update& u) const
{
	RemoteControl control;
	control.movement = movement_enabled() ? get_movement(u, get<PlayerCommon>()->look(), player.ref()->gamepad) : Vec3::zero;
	Transform* t = get<Transform>();
	control.pos = t->pos;
	control.rot = t->rot;
	control.parent = t->parent;
	if (has<Parkour>())
	{
		control.angle_horizontal = get<Walker>()->rotation;
		control.angle_vertical = get<PlayerCommon>()->angle_vertical;
		control.lean = get<Parkour>()->lean;
		control.wall_normal = get<Parkour>()->fsm.current == ParkourState::WallRun ? get<Parkour>()->absolute_wall_normal() : Vec3::zero;
		control.model_offset = get<SkinnedModel>()->offset.translation();

		for (s32 i = 0; i < MAX_ANIMATIONS; i++)
		{
			const Animator::Layer& input = get<Animator>()->layers[i];
			AnimationLayer* output = &control.animations[i];
			output->asset = input.animation;
			output->time = input.time;
		}
	}
	else
	{
		control.angle_horizontal = LMath::angle_range(get<PlayerCommon>()->angle_horizontal);
		control.angle_vertical = get<PlayerCommon>()->angle_vertical;
	}
	return control;
}

void PlayerControlHuman::camera_shake_update(const Update& u, Camera* camera)
{
	if (camera_shake_timer > 0.0f)
	{
		camera_shake_timer -= u.time.delta;
		if (!has<Drone>() || get<Drone>()->state() == Drone::State::Crawl)
		{
			r32 shake = (camera_shake_timer / camera_shake_time) * 0.2f;
			r32 offset = Game::time.total * 10.0f;
			camera->rot = camera->rot * Quat::euler(noise::sample2d(Vec2(offset)) * shake, noise::sample2d(Vec2(offset + 67)) * shake, noise::sample2d(Vec2(offset + 137)) * shake);
		}
	}
}

void player_confirm_tram_interactable(s8 gamepad)
{
	for (auto i = PlayerControlHuman::list.iterator(); !i.is_last(); i.next())
	{
		PlayerHuman* player = i.item()->player.ref();
		if (player->gamepad == gamepad)
		{
			Interactable* interactable = Interactable::closest(i.item()->get<Transform>()->absolute_pos());
			if (interactable)
			{
				interactable->interact();
				i.item()->get<Animator>()->layers[3].play(Asset::Animation::character_interact);
				i.item()->get<Audio>()->post(AK::EVENTS::PLAY_PARKOUR_INTERACT);
				i.item()->anim_base = interactable->entity();
			}
			break;
		}
	}
}

const PlayerControlHuman::PositionEntry* PlayerControlHuman::remote_position(r32* tolerance_pos, r32* tolerance_rot) const
{
	r32 timestamp = Game::real_time.total - Net::rtt(player.ref());
	const PositionEntry* position = nullptr;
	r32 tmp_tolerance_pos = 0.0f;
	r32 tmp_tolerance_rot = 0.0f;
	for (s32 i = position_history.length - 1; i >= 0; i--)
	{
		const PositionEntry& entry = position_history[i];
		if (entry.timestamp < timestamp)
		{
			position = &entry;
			// calculate tolerance based on velocity
			const s32 radius = 6;
			for (s32 j = vi_max(0, i - radius); j < vi_min(s32(position_history.length), i + radius + 1); j++)
			{
				if (i != j)
				{
					tmp_tolerance_pos = vi_max(tmp_tolerance_pos, (position_history[i].pos - position_history[j].pos).length());
					tmp_tolerance_rot = vi_max(tmp_tolerance_rot, vi_max(tmp_tolerance_pos * 4.0f, Quat::angle(position_history[i].rot, position_history[j].rot)));
				}
			}
			tmp_tolerance_pos *= 8.0f;
			tmp_tolerance_rot *= 8.0f;
			break;
		}
	}
	tmp_tolerance_pos += NET_SYNC_TOLERANCE_POS;
	tmp_tolerance_rot += NET_SYNC_TOLERANCE_ROT;
	if (tolerance_pos)
		*tolerance_pos = tmp_tolerance_pos;
	if (tolerance_rot)
		*tolerance_rot = tmp_tolerance_rot;
	return position;
}

// 0 to 1
r32 zoom_amount_get(PlayerControlHuman* player, const Update& u)
{
	s8 gamepad = player->player.ref()->gamepad;
	if (Settings::gamepads[gamepad].zoom_toggle)
		return player->flag(PlayerControlHuman::FlagTrySecondary) ? 1.0f : 0.0f;
	else
	{
		// analog zoom
		if (player->flag(PlayerControlHuman::FlagTrySecondary))
		{
			const InputBinding& binding = Settings::gamepads[gamepad].bindings[s32(Controls::Zoom)];
			if (u.input->keys.get(s32(binding.key1)) || u.input->keys.get(s32(binding.key2)))
				return 1.0f;

			Gamepad::Btn zoom_btn = binding.btn;
			r32 t;
			if (zoom_btn == Gamepad::Btn::LeftTrigger)
				t = u.input->gamepads[gamepad].left_trigger;
			else if (zoom_btn == Gamepad::Btn::RightTrigger)
				t = u.input->gamepads[gamepad].right_trigger;
			else
				t = 1.0f;

			if (t > 0.95f)
				return 1.0f;
			else if (t > 0.0f)
				return 0.5f;
			else
				return 0.0f;
		}
		else
			return 0.0f;
	}
}

void PlayerControlHuman::update(const Update& u)
{
	s32 gamepad = player.ref()->gamepad;

	if (has<Drone>())
	{
		spot_timer = vi_max(0.0f, spot_timer - u.real_time.delta);

		if (Game::level.local || local())
		{
			// save our position history
			r32 cutoff = Game::real_time.total - (Net::rtt(player.ref()) * 2.0f) - Net::interpolation_delay(player.ref());
			while (position_history.length > 16 && position_history[0].timestamp < cutoff)
				position_history.remove_ordered(0);
			Transform* t = get<Transform>();
			Vec3 abs_pos;
			Quat abs_rot;
			t->absolute(&abs_pos, &abs_rot);
			position_history.add(
			{
				abs_rot,
				abs_pos,
				Game::real_time.total,
			});
		}

		if (local())
		{
			if (!Game::level.local && has<Parkour>())
			{
				// we are a client and this is a local player
				// make sure we never get too far from where the server says we should be
				// get the position entry at this time in the history

				// make sure we're not too far from it
				r32 tolerance_pos;
				r32 tolerance_rot;
				const PositionEntry* position = remote_position(&tolerance_pos, &tolerance_rot);
				if (position)
				{
					Vec3 remote_abs_pos = remote_control.pos;
					Quat remote_abs_rot = remote_control.rot;
					if (remote_control.parent.ref())
						remote_control.parent.ref()->to_world(&remote_abs_pos, &remote_abs_rot);

					if ((position->pos - remote_abs_pos).length_squared() > tolerance_pos * tolerance_pos
						|| Quat::angle(position->rot, remote_abs_rot) > tolerance_rot)
					{
						// snap our position to the server's position
#if DEBUG_NET_SYNC
						vi_debug_break();
#endif
						position_history.length = 0;
						Transform* t = get<Transform>();
						t->pos = remote_control.pos;
						t->rot = remote_control.rot;
						t->parent = remote_control.parent;
						if (!t->parent.ref())
							get<Drone>()->velocity = t->rot * Vec3(0, 0, vi_max(DRONE_DASH_SPEED, get<Drone>()->velocity.length()));
					}
				}
			}

			{
				r32 cooldown = get<Drone>()->cooldown;
				if (cooldown < DRONE_COOLDOWN_THRESHOLD && cooldown_last >= DRONE_COOLDOWN_THRESHOLD)
					Audio::post_global(AK::EVENTS::PLAY_DRONE_CHARGE_RESTORE, gamepad);
				cooldown_last = cooldown;
			}

			Camera* camera = player.ref()->camera.ref();

			r32 zoom_amount = zoom_amount_get(this, u);

			{
				// zoom
				b8 zoom_pressed = u.input->get(Controls::Zoom, gamepad);
				b8 last_zoom_pressed = u.last_input->get(Controls::Zoom, gamepad);
				if (zoom_pressed && !last_zoom_pressed)
				{
					if (get<Transform>()->parent.ref() && input_enabled())
					{
						// we can actually zoom
						if (Settings::gamepads[gamepad].zoom_toggle)
						{
							flag(FlagTrySecondary, !flag(FlagTrySecondary));
							Audio::post_global(flag(FlagTrySecondary) ? AK::EVENTS::PLAY_ZOOM_IN : AK::EVENTS::PLAY_ZOOM_OUT, gamepad);
						}
						else
						{
							flag(FlagTrySecondary, true);
							Audio::post_global(AK::EVENTS::PLAY_ZOOM_IN, gamepad);
						}
					}
				}
				else if (!Settings::gamepads[gamepad].zoom_toggle && !zoom_pressed)
				{
					if (flag(FlagTrySecondary))
						Audio::post_global(AK::EVENTS::PLAY_ZOOM_OUT, gamepad);
					flag(FlagTrySecondary, false);
				}

				r32 fov_target = LMath::lerpf(zoom_amount, Settings::effective_fov(), (get<Drone>()->current_ability == Ability::Sniper ? fov_sniper : fov_zoom));

				if (fov < fov_target)
					fov = vi_min(fov + zoom_speed * sinf(fov) * u.time.delta, fov_target);
				else if (fov > fov_target)
					fov = vi_max(fov - zoom_speed * sinf(fov) * u.time.delta, fov_target);
			}

			// update camera projection
			camera->perspective(fov, 0.005f, Game::level.far_plane_get());

			// collect target indicators
			player_collect_target_indicators(this);

			if (get<Transform>()->parent.ref()) // crawling or dashing
			{
				r32 gamepad_rotation_multiplier = 1.0f;

				r32 look_speed = LMath::lerpf(zoom_amount, 1.0f, get<Drone>()->current_ability == Ability::Sniper ? zoom_speed_multiplier_sniper : zoom_speed_multiplier);

				if (input_enabled() && u.input->gamepads[gamepad].type != Gamepad::Type::None)
				{
					// gamepad aim assist
					Vec3 to_reticle = reticle.pos - camera->pos;
					r32 reticle_distance = to_reticle.length();
					to_reticle /= reticle_distance;
					for (s32 i = 0; i < target_indicators.length; i++)
					{
						const TargetIndicator indicator = target_indicators[i];

						if (indicator.type == TargetIndicator::Type::BatteryOutOfRange
							|| indicator.type == TargetIndicator::Type::BatteryFriendly
							|| indicator.type == TargetIndicator::Type::BatteryFriendlyOutOfRange
							|| indicator.type == TargetIndicator::Type::BatteryEnemyOutOfRange)
							continue;

						Vec3 to_indicator = indicator.pos - camera->pos;
						r32 indicator_distance = to_indicator.length();
						if (indicator_distance > DRONE_THIRD_PERSON_OFFSET + DRONE_SHIELD_RADIUS * 2.0f
							&& indicator_distance < reticle_distance + 2.5f)
						{
							to_indicator /= indicator_distance;
							if (to_indicator.dot(to_reticle) > 0.99f)
							{
								// slow down gamepad rotation if we're hovering over this target
								gamepad_rotation_multiplier = 0.6f;

								if (Game::real_time.total - last_gamepad_input_time < 0.25f)
								{
									// adjust for relative velocity
									Vec2 predicted_offset;
									{
										Vec3 me = get<Drone>()->center_lerped();
										Vec3 my_velocity = get<Drone>()->center_lerped() - last_pos;
										{
											r32 my_speed = my_velocity.length_squared();
											if (my_speed == 0.0f || my_speed > DRONE_CRAWL_SPEED * 1.5f * DRONE_CRAWL_SPEED * 1.5f) // don't adjust if we're going too fast or not moving
												break;
										}
										Vec3 me_predicted = me + my_velocity;

										if (indicator.velocity.length_squared() > DRONE_DASH_SPEED * 0.5f * DRONE_DASH_SPEED * 0.5f) // enemy moving too fast
											continue;

										Vec3 target_predicted = indicator.pos + indicator.velocity * u.time.delta;
										Vec3 predicted_ray = Vec3::normalize(target_predicted - me_predicted);
										Vec2 predicted_angles(atan2f(predicted_ray.x, predicted_ray.z), -asinf(predicted_ray.y));
										predicted_offset = Vec2(LMath::angle_to(get<PlayerCommon>()->angle_horizontal, predicted_angles.x), LMath::angle_to(get<PlayerCommon>()->angle_vertical_total(), predicted_angles.y));
									}

									Vec2 current_offset;
									{
										Vec3 current_ray = Vec3::normalize(indicator.pos - get<Transform>()->absolute_pos());
										Vec2 current_angles(atan2f(current_ray.x, current_ray.z), -asinf(current_ray.y));
										current_offset = Vec2(LMath::angle_to(get<PlayerCommon>()->angle_horizontal, current_angles.x), LMath::angle_to(get<PlayerCommon>()->angle_vertical_total(), current_angles.y));
									}

									Vec2 adjustment(LMath::angle_to(current_offset.x, predicted_offset.x), LMath::angle_to(current_offset.y, predicted_offset.y));

									r32 max_adjustment = look_speed * 0.5f * speed_joystick * u.time.delta;

									if (current_offset.x > 0 == adjustment.x > 0 // only adjust if it's an adjustment toward the target
										&& fabsf(get<PlayerCommon>()->angle_vertical_total()) < PI * 0.4f) // only adjust if we're not looking straight up or down
										get<PlayerCommon>()->angle_horizontal = LMath::angle_range(get<PlayerCommon>()->angle_horizontal + vi_max(-max_adjustment, vi_min(max_adjustment, adjustment.x)));

									if (current_offset.y > 0 == adjustment.y > 0) // only adjust if it's an adjustment toward the target
										get<PlayerCommon>()->angle_vertical = LMath::angle_range(get<PlayerCommon>()->angle_vertical + vi_max(-max_adjustment, vi_min(max_adjustment, adjustment.y)));
								}

								break;
							}
						}
					}
				}

				update_camera_input(u, look_speed, gamepad_rotation_multiplier);
				{
					r32 scale = vi_min(2.0f, (u.time.total - get<Drone>()->attach_time) / 0.4f);
					if (scale > 1.0f)
						scale = 1.0f - (scale - 1.0f);
					if (scale > 0.0f)
						get<PlayerCommon>()->clamp_rotation(get<Transform>()->absolute_rot() * Vec3(0, 0, 1), LMath::lerpf(Ease::cubic_in_out<r32>(scale), 1.0f, 0.707f));
				}
				camera->rot = Quat::euler(0.0f, get<PlayerCommon>()->angle_horizontal, get<PlayerCommon>()->angle_vertical_total());

				// crawling
				{
					Vec3 movement = movement_enabled() ? get_movement(u, get<PlayerCommon>()->look(), gamepad) : Vec3::zero;
					get<Drone>()->crawl(movement, u.time.delta);
				}

				last_pos = get<Drone>()->center_lerped();
			}
			else // flying
				camera->rot = Quat::euler(0, get<PlayerCommon>()->angle_horizontal, get<PlayerCommon>()->angle_vertical_total());

			if (movement_enabled())
			{
				// ability inputs

				// make sure player is only selecting one ability input
				s32 selected_abilities = 0;
				if (u.input->get(Controls::Ability1, gamepad))
					selected_abilities |= 1 << 0;
				if (u.input->get(Controls::Ability2, gamepad))
					selected_abilities |= 1 << 1;
				if (u.input->get(Controls::Ability3, gamepad))
					selected_abilities |= 1 << 2;

				if (BitUtility::popcount(selected_abilities) == 1)
				{
					for (s32 i = 0; i < MAX_ABILITIES + 1; i++)
					{
						if (selected_abilities & (1 << i))
							player_ability_select(u, this, i);
					}
				}
			}

			camera_shake_update(u, camera);

			PlayerHuman::camera_setup_drone(get<Drone>(), camera, &player.ref()->camera_center, DRONE_THIRD_PERSON_OFFSET);

			// reticle
			{
				Vec3 trace_dir = camera->rot * Vec3(0, 0, 1);
				Vec3 me = get<Transform>()->absolute_pos();
				Vec3 trace_start = camera->pos + trace_dir * trace_dir.dot(me - camera->pos);

				Ability ability = get<Drone>()->current_ability;
				
				r32 raycast_radius = ability == Ability::None
					? DRONE_SHIELD_RADIUS
					: 0.0f;

				reticle.type = ReticleType::None;

				if (movement_enabled() && trace_dir.dot(get<Transform>()->absolute_rot() * Vec3(0, 0, 1)) > -0.9f)
				{
					Vec3 trace_end = trace_start + trace_dir * DRONE_SNIPE_DISTANCE;

					struct RayHit
					{
						Entity* entity;
						Vec3 pos;
						Vec3 normal;
						b8 hit;
					};

					RayHit static_ray_callback; // fallback raycast result; only tests against level geometry
					RayHit ray_callback; // could be level geometry or a target

					{
						{
							RaycastCallbackExcept bullet_ray_callback(trace_start, trace_end, entity());
							reticle_raycast(&bullet_ray_callback);

							ray_callback.hit = bullet_ray_callback.hasHit();
							ray_callback.pos = bullet_ray_callback.m_hitPointWorld;
							ray_callback.normal = bullet_ray_callback.m_hitNormalWorld;
							if (ray_callback.hit)
								ray_callback.entity = &Entity::list[bullet_ray_callback.m_collisionObject->getUserIndex()];

							static_ray_callback = ray_callback;
						}

						// check shields
						for (auto i = Shield::list.iterator(); !i.is_last(); i.next())
						{
							if (get<Drone>()->should_collide(i.item()->get<Target>()))
							{
								Vec3 shield_pos = i.item()->get<Target>()->absolute_pos();
								Vec3 intersection;
								if (LMath::ray_sphere_intersect_flattened_plane(trace_start, trace_end, shield_pos, me, DRONE_SHIELD_RADIUS + raycast_radius, &intersection))
								{
									b8 hit = true;
									if (ray_callback.hit)
									{
										// check if an existing hit is in front of this shield
										Vec3 intersection_front;
										LMath::ray_sphere_intersect(trace_start, trace_end, shield_pos, DRONE_SHIELD_RADIUS + raycast_radius, &intersection_front);
										if ((ray_callback.pos - trace_start).length_squared() < (intersection_front - trace_start).length_squared())
											hit = false;
									}

									if (hit)
									{
										ray_callback.hit = true;
										ray_callback.normal = Vec3::normalize(shield_pos - intersection);
										ray_callback.pos = intersection;
										ray_callback.entity = i.item()->entity();
									}
								}
							}
						}
					}

					if (ability == Ability::None || AbilityInfo::list[s32(ability)].type == AbilityInfo::Type::Shoot)
					{
						// check drone target predictions
						for (s32 i = 0; i < target_indicators.length; i++)
						{
							const TargetIndicator& indicator = target_indicators[i];
							Vec3 intersection;
							if (indicator.type == TargetIndicator::Type::DroneVisible
								&& LMath::ray_sphere_intersect_flattened_plane(trace_start, trace_end, indicator.pos, me, DRONE_SHIELD_RADIUS + raycast_radius, &intersection))
							{
								b8 hit = true;
								if (ray_callback.hit)
								{
									// check if an existing hit is in front of this shield
									Vec3 intersection_front;
									LMath::ray_sphere_intersect(trace_start, trace_end, indicator.pos, DRONE_SHIELD_RADIUS + raycast_radius, &intersection_front);
									if ((ray_callback.pos - trace_start).length_squared() < (intersection_front - trace_start).length_squared())
										hit = false;
								}

								if (hit)
								{
									ray_callback.hit = true;
									ray_callback.normal = Vec3::normalize(indicator.pos - intersection);
									ray_callback.pos = intersection;
									ray_callback.entity = indicator.target.ref()->entity();
								}
							}
						}
					}

					if (ray_callback.hit)
					{
						reticle.pos = ray_callback.pos;
						reticle.normal = ray_callback.normal;
						Vec3 detach_dir = reticle.pos - me;
						r32 distance = detach_dir.length();
						detach_dir /= distance;
						r32 dot_tolerance = distance < DRONE_DASH_DISTANCE ? 0.3f : 0.1f;
						if (ability == Ability::None) // normal movement
						{
							Vec3 hit;
							b8 hit_target;
							if (get<Drone>()->can_shoot(detach_dir, &hit, &hit_target))
							{
								if (hit_target)
									reticle.type = ReticleType::Target;
								else if ((hit - me).length() > (static_ray_callback.pos - me).length() - DRONE_RADIUS)
									reticle.type = ReticleType::Normal;
							}
							else if (get<Drone>()->direction_is_toward_attached_wall(detach_dir))
							{
								r32 range = get<Drone>()->range();
								if ((ray_callback.pos - me).length_squared() < range * range)
								{
									if (ray_callback.entity->has<Target>())
										reticle.type = ReticleType::DashTarget;
									else if (!(ray_callback.entity->get<RigidBody>()->collision_group & DRONE_INACCESSIBLE_MASK))
										reticle.type = ReticleType::DashCombo;
								}
							}
							else if (ray_callback.entity->has<Target>())
							{
								// when you're aiming at a target that is attached to the same surface you are,
								// sometimes the point you're aiming at is actually away from the wall,
								// so it registers as a shot rather than a dash.
								// and sometimes that shot can't actually be taken.
								// so we need to check for this case and turn it into a dash if we can.

								// check if they're in range and close enough to our wall
								Vec3 to_target = ray_callback.entity->get<Target>()->absolute_pos() - me;
								if (to_target.length_squared() < DRONE_DASH_DISTANCE * DRONE_DASH_DISTANCE
									&& fabsf(to_target.dot(get<Transform>()->absolute_rot() * Vec3(0, 0, 1))) < DRONE_SHIELD_RADIUS)
									reticle.type = ReticleType::Dash;
							}
						}
						else // spawning an ability
						{
							Vec3 hit;
							b8 hit_target;
							if (get<Drone>()->can_spawn(ability, detach_dir, nullptr, &hit, nullptr, nullptr, &hit_target))
							{
								if (AbilityInfo::list[s32(ability)].type == AbilityInfo::Type::Shoot)
								{
									reticle.type = ReticleType::Normal;
									if (hit_target)
										reticle.type = ReticleType::Target;
								}
								else if ((hit - ray_callback.pos).length_squared() < DRONE_RADIUS * DRONE_RADIUS)
									reticle.type = ReticleType::Normal;
							}
						}
					}
					else
					{
						// aiming at nothing
						reticle.pos = trace_end;
						reticle.normal = -trace_dir;
						if (ability != Ability::None
							&& get<Drone>()->can_spawn(ability, trace_dir)) // spawning an ability
						{
							reticle.type = ReticleType::Normal;
						}
					}
				}
				else
				{
					reticle.pos = trace_start + trace_dir * DRONE_SNIPE_DISTANCE;
					reticle.normal = -trace_dir;
				}
			}

			{
				b8 primary_pressed = u.input->get(Controls::Primary, gamepad);
				if (primary_pressed && !u.last_input->get(Controls::Primary, gamepad))
					flag(FlagTryPrimary, true);
				else if (!primary_pressed)
					flag(FlagTryPrimary, false);
			}

			if (movement_enabled())
			{
				// spot
				if (Game::level.has_feature(Game::FeatureLevel::All) // disable spotting in tutorial
					&& u.input->get(Controls::Spot, gamepad)
					&& !u.last_input->get(Controls::Spot, gamepad))
				{
					PlayerControlHumanNet::Message msg;
					msg.type = PlayerControlHumanNet::Message::Type::Spot;
					msg.dir = camera->rot * Vec3(0, 0, 1);
					msg.target = camera->pos;
					get<Transform>()->absolute(&msg.pos, &msg.rot);
					PlayerControlHumanNet::send(this, &msg);
				}

				if (reticle.type == ReticleType::None || !get<Drone>()->cooldown_can_shoot())
				{
					// can't shoot
					if (u.input->get(Controls::Primary, gamepad)) // player is mashing the fire button; give them some feedback
					{
						if (reticle.type == ReticleType::Dash)
							reticle.type = ReticleType::DashError;
						else
							reticle.type = ReticleType::Error;
					}
				}
				else
				{
					// we're aiming at something
					if (flag(FlagTryPrimary) && camera_shake_timer < 0.1f) // don't go anywhere if the camera is shaking wildly
					{
						PlayerControlHumanNet::Message msg;
						msg.dir = Vec3::normalize(reticle.pos - get<Transform>()->absolute_pos());
						get<Transform>()->absolute(&msg.pos, &msg.rot);
						if (reticle.type == ReticleType::DashCombo || reticle.type == ReticleType::DashTarget)
						{
							msg.type = PlayerControlHumanNet::Message::Type::DashCombo;
							msg.target = reticle.pos;
							PlayerControlHumanNet::send(this, &msg);
						}
						else if (reticle.type == ReticleType::Dash)
						{
							msg.type = PlayerControlHumanNet::Message::Type::Dash;
							PlayerControlHumanNet::send(this, &msg);
						}
						else
						{
							msg.ability = get<Drone>()->current_ability;
							if (msg.ability == Ability::None
								|| (player.ref()->get<PlayerManager>()->ability_valid(msg.ability) && (msg.ability != Ability::Bolter || get<Drone>()->bolter_can_fire())))
							{
								msg.type = PlayerControlHumanNet::Message::Type::Go;
								PlayerControlHumanNet::send(this, &msg);
							}
						}
					}
				}
			}
		}
		else if (Game::level.local)
		{
			// we are a server, but this Drone is being controlled by a client
#if SERVER
			// if we are crawling, update the RTT every frame
			// if we're dashing or flying, the RTT is set based on the sequence number of the command we received
			if (get<Drone>()->state() == Drone::State::Crawl)
				rtt = Net::rtt(player.ref());

			get<Drone>()->crawl(remote_control.movement, u.time.delta);
			last_pos = get<Drone>()->center_lerped();
#else
			vi_assert(false);
#endif
		}
		else
		{
			// we are a client and this Drone is not local
			// do nothing
		}
	}
	else
	{
		// parkour mode
		if (local())
		{
			{
				r32 cooldown = get<Parkour>()->grapple_cooldown;
				if (cooldown < GRAPPLE_COOLDOWN_THRESHOLD && cooldown_last >= GRAPPLE_COOLDOWN_THRESHOLD)
					Audio::post_global(AK::EVENTS::PLAY_DRONE_CHARGE_RESTORE, gamepad);
				cooldown_last = cooldown;
			}

			// start interaction
			if (input_enabled()
				&& get<Animator>()->layers[3].animation == AssetNull
				&& !u.input->get(Controls::InteractSecondary, gamepad)
				&& u.last_input->get(Controls::InteractSecondary, gamepad))
			{
				Interactable* interactable = Interactable::closest(get<Transform>()->absolute_pos());
				if (interactable)
				{
					switch (interactable->type)
					{
						case Interactable::Type::Terminal:
						{
							switch (Game::save.zones[Game::level.id])
							{
								case ZoneState::Locked:
								case ZoneState::ParkourUnlocked: // open up
								{
									interactable->interact();
									get<Animator>()->layers[3].play(Asset::Animation::character_interact);
									get<Audio>()->post(AK::EVENTS::PLAY_PARKOUR_INTERACT);
									anim_base = interactable->entity();
									break;
								}
								case ZoneState::ParkourOwned: // already open; get in
								{
									anim_base = interactable->entity();
									get<Animator>()->layers[3].play(Asset::Animation::character_terminal_enter); // animation will eventually trigger the interactable
									break;
								}
								default:
									vi_assert(false);
									break;
							}
							break;
						}
						case Interactable::Type::Tram: // tram interactable
						{
							s8 track = s8(interactable->user_data);
							const Game::TramTrack& entry = Game::level.tram_tracks[track];
							Tram* tram = Tram::by_track(track);
							if (tram->doors_open() // if the tram doors are open, we can always close them
								|| (!tram->arrive_only && entry.level != AssetNull // if the target zone doesn't exist, or if the tram is for arrivals only, nothing else matters, we can't do anything
									&& Game::save.zones[entry.level] != ZoneState::Locked)) // if we've already unlocked it, go ahead
							{
								interactable->interact();
								get<Animator>()->layers[3].play(Asset::Animation::character_interact);
								get<Audio>()->post(AK::EVENTS::PLAY_PARKOUR_INTERACT);
								anim_base = interactable->entity();
							}
							else if (tram->arrive_only || entry.level == AssetNull) // can't leave
								player.ref()->msg(_(strings::zone_unavailable), PlayerHuman::FlagNone);
							else if (Game::save.resources[s32(Resource::Energy)] >= entry.energy_threshold) // unlock the zone
								Menu::dialog(gamepad, &player_confirm_tram_interactable, _(strings::tram_energy_threshold_met), entry.energy_threshold);
							else // not enough to unlock it
								Menu::dialog(gamepad, &Menu::dialog_no_action, _(strings::tram_energy_threshold_warning), entry.energy_threshold);
							break;
						}
						case Interactable::Type::Shop:
						{
							Overworld::show(player.ref()->camera.ref(), Overworld::State::StoryModeOverlay, Overworld::StoryTab::Inventory);
							Overworld::shop_flags(interactable->user_data);
							break;
						}
						default:
							vi_assert(false); // invalid interactable type
							break;
					}
				}
			}

			update_camera_input(u);

			if (get<Parkour>()->fsm.current == ParkourState::Climb
				&& input_enabled()
				&& u.input->get(Controls::Parkour, gamepad))
			{
				Vec3 movement = movement_enabled() ? get_movement(u, Quat::identity, gamepad) : Vec3::zero;
				get<Parkour>()->climb_velocity = movement.z;
			}
			else
				get<Parkour>()->climb_velocity = 0.0f;

			// set movement unless we're climbing up and down
			if (!(get<Parkour>()->fsm.current == ParkourState::Climb && u.input->get(Controls::Parkour, gamepad)))
			{
				Vec3 movement = movement_enabled() ? get_movement(u, Quat::euler(0, get<PlayerCommon>()->angle_horizontal, 0), gamepad) : Vec3::zero;
				Vec2 dir = Vec2(movement.x, movement.z);
				get<Walker>()->dir = dir;
			}

			// parkour button
			{
				b8 parkour_pressed = movement_enabled() && u.input->get(Controls::Parkour, gamepad);

				if (get<Parkour>()->fsm.current == ParkourState::WallRun && !parkour_pressed)
				{
					get<Parkour>()->fsm.transition(ParkourState::Normal);
					get<Parkour>()->wall_run_state = ParkourWallRunState::None;
				}

				if (parkour_pressed && !u.last_input->get(Controls::Parkour, gamepad))
					flag(FlagTrySecondary, true);
				else if (!parkour_pressed)
					flag(FlagTrySecondary, false);

				if (flag(FlagTrySecondary))
				{
					if (get<Parkour>()->try_parkour())
						flag(FlagTrySecondary | FlagTryPrimary, false);
				}
			}

			// jump button
			{
				b8 jump_pressed = movement_enabled() && u.input->get(Controls::Jump, gamepad);
				if (jump_pressed && !u.last_input->get(Controls::Jump, gamepad))
					flag(FlagTryPrimary, true);
				else if (!jump_pressed)
					flag(FlagTryPrimary, false);

				if (jump_pressed)
					get<Parkour>()->lessen_gravity(); // jump higher when the player holds the jump button

				if (flag(FlagTryPrimary))
				{
					if (get<Parkour>()->try_jump(get<PlayerCommon>()->angle_horizontal))
						flag(FlagTrySecondary | FlagTryPrimary, false);
				}
			}

			// grapple button
			if (Parkour::ability_enabled(Resource::Grapple))
			{
				if (movement_enabled())
				{
					if (get<Parkour>()->flag(Parkour::FlagTryGrapple) && u.input->get(Controls::GrappleCancel, gamepad))
					{
						get<Parkour>()->grapple_cancel();
						flag(FlagGrappleCanceled, true);
					}

					b8 grapple_pressed = u.input->get(Controls::Grapple, gamepad);

					if (flag(FlagGrappleCanceled))
					{
						if (!grapple_pressed)
							flag(FlagGrappleCanceled, false);
					}
					else
					{
						Camera* camera = player.ref()->camera.ref();
						if (grapple_pressed && !get<Parkour>()->flag(Parkour::FlagTryGrapple))
						{
							get<Parkour>()->grapple_start(camera->pos, camera->rot);
							flag(FlagGrappleValid, false);
						}
						else if (!grapple_pressed && get<Parkour>()->flag(Parkour::FlagTryGrapple))
							get<Parkour>()->grapple_try(camera->pos, get<Parkour>()->grapple_pos);
					}
				}
				else
				{
					if (get<Parkour>()->flag(Parkour::FlagTryGrapple))
						get<Parkour>()->grapple_cancel();
				}

				if (get<Parkour>()->fsm.current != ParkourState::Grapple)
				{
					if (get<Parkour>()->flag(Parkour::FlagTryGrapple))
					{
						Camera* camera = player.ref()->camera.ref();
						Vec3 candidate_pos;
						Vec3 candidate_normal;
						b8 prev_grapple_valid = flag(FlagGrappleValid);
						if (prev_grapple_valid)
						{
							// check if it's still good
							prev_grapple_valid = get<Parkour>()->grapple_valid(camera->pos, Quat::look(Vec3::normalize(get<Parkour>()->grapple_pos - camera->pos)));
							flag(FlagGrappleValid, prev_grapple_valid);
						}

						b8 candidate_grapple_valid = get<Parkour>()->grapple_valid(camera->pos, camera->rot, &candidate_pos, &candidate_normal);
						if (candidate_grapple_valid
							|| !prev_grapple_valid
							|| Vec3::normalize(get<Parkour>()->grapple_pos - camera->pos).dot(Vec3::normalize(candidate_pos - camera->pos)) < 0.9f)
						{
							// new grapple target
							flag(FlagGrappleValid, candidate_grapple_valid);
							get<Parkour>()->grapple_pos = candidate_pos;
							get<Parkour>()->grapple_normal = candidate_normal;
						}
					}
				}
			}

			ParkourState parkour_state = get<Parkour>()->fsm.current;

			{
				// if we're just running and not doing any parkour
				// rotate arms to match the camera view
				// blend smoothly between the two states (rotating and not rotating)

				r32 arm_angle = LMath::clampf(get<PlayerCommon>()->angle_vertical_total() * 0.75f + arm_angle_offset, -PI * 0.2f, PI * 0.25f);

				const r32 blend_time = 0.2f;
				r32 blend;
				if (parkour_state == ParkourState::Normal || parkour_state == ParkourState::Grapple)
					blend = vi_min(1.0f, get<Parkour>()->fsm.time / blend_time);
				else if (get<Parkour>()->fsm.last == ParkourState::Normal)
					blend = vi_max(0.0f, 1.0f - (get<Parkour>()->fsm.time / blend_time));
				else
					blend = 0.0f;
				Quat offset = Quat::euler(arm_angle * blend, 0, 0);
				get<Animator>()->override_bone(Asset::Bone::character_upper_arm_L, Vec3::zero, offset);
				get<Animator>()->override_bone(Asset::Bone::character_upper_arm_R, Vec3::zero, offset);
			}

			if (parkour_state == ParkourState::WallRun)
			{
				Vec3 wall_normal = get<Parkour>()->last_support.ref()->get<Transform>()->to_world_normal(get<Parkour>()->relative_wall_run_normal);

				Vec3 forward = Quat::euler(get<Parkour>()->lean, get<PlayerCommon>()->angle_horizontal, get<PlayerCommon>()->angle_vertical_total()) * Vec3(0, 0, 1);

				if (get<Parkour>()->wall_run_state == ParkourWallRunState::Forward)
					get<PlayerCommon>()->clamp_rotation(-wall_normal); // make sure we're always facing the wall
				else
				{
					// we're running along the wall
					// make sure we can't look backward
					get<PlayerCommon>()->clamp_rotation(Quat::euler(0, get<Walker>()->rotation, 0) * Vec3(0, 0, 1));
					if (get<Parkour>()->wall_run_state == ParkourWallRunState::Left)
						get<PlayerCommon>()->clamp_rotation(Quat::euler(0, get<Walker>()->rotation + PI * -0.5f, 0) * Vec3(0, 0, 1));
					else
						get<PlayerCommon>()->clamp_rotation(Quat::euler(0, get<Walker>()->rotation + PI * 0.5f, 0) * Vec3(0, 0, 1));
				}
			}
			else if (parkour_state == ParkourState::HardLanding
				|| parkour_state == ParkourState::Mantle
				|| parkour_state == ParkourState::Climb
				|| parkour_state == ParkourState::Grapple)
			{
				get<PlayerCommon>()->clamp_rotation(Quat::euler(0, get<Walker>()->rotation, 0) * Vec3(0, 0, 1));
			}
			else
			{
				get<Walker>()->target_rotation = get<PlayerCommon>()->angle_horizontal;

				// make sure our body is facing within 90 degrees of our target rotation
				r32 delta = LMath::angle_to(get<Walker>()->rotation, get<PlayerCommon>()->angle_horizontal);
				if (delta > PI * 0.5f)
					get<Walker>()->rotation = LMath::angle_range(get<Walker>()->rotation + delta - PI * 0.5f);
				else if (delta < PI * -0.5f)
					get<Walker>()->rotation = LMath::angle_range(get<Walker>()->rotation + delta + PI * 0.5f);
			}
		}
	}
}

void PlayerControlHuman::cinematic(Entity* basis, AssetID anim)
{
	vi_assert(has<Parkour>());

	get<Animator>()->layers[3].set(anim, 0.0f);

	Vec3 target_pos;
	r32 target_angle;
	if (basis->has<Interactable>())
		get_interactable_standing_position(basis->get<Transform>(), &target_pos, &target_angle);
	else
		get_standing_position(basis->get<Transform>(), &target_pos, &target_angle);

	get<PlayerCommon>()->angle_horizontal = get<Parkour>()->last_angle_horizontal = get<Walker>()->rotation = get<Walker>()->target_rotation = target_angle;
	get<PlayerCommon>()->angle_vertical = 0.0f;
	get<PlayerCommon>()->recoil = 0.0f;
	get<PlayerCommon>()->recoil_velocity = 0.0f;
	get<Parkour>()->lean = 0.0f;
	get<Walker>()->absolute_pos(target_pos);

	anim_base = basis;
}

b8 PlayerControlHuman::cinematic_active() const
{
	// cinematic is active if we're playing an animation on layer 3
	// however, the collectible pickup animation also runs on layer 3 and it's not a cinematic
	AssetID anim = get<Animator>()->layers[3].animation;
	return anim != AssetNull && anim != Asset::Animation::character_pickup;
}

void PlayerControlHuman::update_late(const Update& u)
{
	if (anim_base.ref())
	{
		// an animation is playing
		// position player where they need to be
		// if anim_base is an interactable, place the player directly in front of it

		if (get<Animator>()->layers[3].animation == AssetNull)
			anim_base = nullptr; // animation done
		else
		{
			// desired rotation / position
			Vec3 target_pos;
			r32 target_angle;
			if (anim_base.ref()->has<Interactable>())
			{
				get_interactable_standing_position(anim_base.ref()->get<Transform>(), &target_pos, &target_angle);

				// lerp to interactable
				target_angle = LMath::closest_angle(target_angle, get<PlayerCommon>()->angle_horizontal);

				if (get<PlayerCommon>()->angle_horizontal > target_angle)
					get<PlayerCommon>()->angle_horizontal = LMath::angle_range(vi_max(target_angle, get<PlayerCommon>()->angle_horizontal - INTERACT_LERP_ROTATION_SPEED * u.time.delta));
				else
					get<PlayerCommon>()->angle_horizontal = LMath::angle_range(vi_min(target_angle, get<PlayerCommon>()->angle_horizontal + INTERACT_LERP_ROTATION_SPEED * u.time.delta));

				{
					r32 target_angle = -arm_angle_offset;
					if (get<PlayerCommon>()->angle_vertical > target_angle)
						get<PlayerCommon>()->angle_vertical = LMath::angle_range(vi_max(target_angle, get<PlayerCommon>()->angle_vertical - INTERACT_LERP_ROTATION_SPEED * u.time.delta));
					else
						get<PlayerCommon>()->angle_vertical = LMath::angle_range(vi_min(target_angle, get<PlayerCommon>()->angle_vertical + INTERACT_LERP_ROTATION_SPEED * u.time.delta));
				}

				Vec3 abs_pos = get<Transform>()->absolute_pos();
				Vec3 diff = target_pos - abs_pos;
				r32 distance = diff.length();
				r32 max_correction_distance = INTERACT_LERP_TRANSLATION_SPEED * u.time.delta;
				if (distance <= max_correction_distance)
					get<Walker>()->absolute_pos(target_pos);
				else
					get<Walker>()->absolute_pos(abs_pos + diff * (max_correction_distance / distance));
			}
			else
			{
				get_standing_position(anim_base.ref()->get<Transform>(), &target_pos, &target_angle);
				// instantly teleport
				get<Walker>()->absolute_pos(target_pos);
				get<PlayerCommon>()->angle_horizontal = target_angle;
				get<PlayerCommon>()->angle_vertical = 0.0f;
				get<PlayerCommon>()->recoil = 0.0f;
				get<PlayerCommon>()->recoil_velocity = 0.0f;
			}
			get<RigidBody>()->btBody->setLinearVelocity(Vec3::zero);
		}
	}

	if (has<Parkour>() && local())
	{
		Camera* camera = player.ref()->camera.ref();

		{
			camera->perspective(Settings::effective_fov(), 0.02f, Game::level.far_plane_get());
			camera->clip_planes[0] = Plane();
			camera->cull_range = 0.0f;
			camera->flag(CameraFlagCullBehindWall, false);
			camera->flag(CameraFlagFog, true);
			if (get<Parkour>()->flag(Parkour::FlagTryGrapple))
			{
				camera->range_center = camera->rot.inverse() * (get<Parkour>()->hand_pos() - camera->pos);
				camera->range = GRAPPLE_RANGE;
			}
			else
				camera->range = 0.0f;
		}

		{
			// camera bone affects rotation only
			Quat camera_animation = Quat::euler(PI * -0.5f, 0, 0);
			get<Animator>()->bone_transform(Asset::Bone::character_camera, nullptr, &camera_animation);
			camera->rot = Quat::euler(get<Parkour>()->lean, get<PlayerCommon>()->angle_horizontal, get<PlayerCommon>()->angle_vertical_total()) * Quat::euler(0, PI * 0.5f, 0) * camera_animation * Quat::euler(0, PI * -0.5f, 0);

			camera->pos = Vec3(0, 0, 0.1f);
			Quat q = Quat::identity;
			get<Parkour>()->head_to_object_space(&camera->pos, &q);
			camera->pos = get<Transform>()->to_world(camera->pos);

			// third-person
			//camera->pos += camera->rot * Vec3(0, 0, -2);
		}

		// wind sound and camera shake at high speed
		{
			ParkourState state = get<Parkour>()->fsm.current;
			r32 speed = (state == ParkourState::Mantle || state == ParkourState::Grapple || get<Walker>()->support.ref())
				? 0.0f
				: get<RigidBody>()->btBody->getInterpolationLinearVelocity().length();
			Audio::param_global(AK::GAME_PARAMETERS::PARKOUR_WIND, LMath::clampf((speed - 8.0f) / 25.0f, 0, 1), player.ref()->gamepad);
			r32 shake = LMath::clampf((speed - 13.0f) / 30.0f, 0, 1);
			player.ref()->rumble_add(shake);
			shake *= 0.2f;
			r32 offset = Game::time.total * 10.0f;
			camera->rot = camera->rot * Quat::euler(noise::sample2d(Vec2(offset)) * shake, noise::sample2d(Vec2(offset + 67)) * shake, noise::sample2d(Vec2(offset + 137)) * shake);
		}

		camera_shake_update(u, camera);
	}
}

void draw_cooldown(const RenderParams& params, r32 cooldown, const Vec2& pos, r32 threshold)
{
	b8 cooldown_can_go = cooldown < threshold;
	Rect2 box = { pos, Vec2(64.0f, 16.0f) * UI::scale };
	if (!cooldown_can_go)
		UI::centered_box(params, { box.pos, box.size * Vec2(cooldown / threshold, 1.0f) }, UI::color_accent());
	UI::centered_box(params, { box.pos, box.size * Vec2(vi_min(1.0f, cooldown / threshold), 1.0f) }, cooldown_can_go ? UI::color_accent() : UI::color_alert());
}

void PlayerControlHuman::draw_alpha_late(const RenderParams& params) const
{
	if (has<Parkour>())
	{
		Parkour* parkour = get<Parkour>();
		if (parkour->flag(Parkour::FlagTryGrapple) && params.camera == player.ref()->camera.ref())
		{
			{
				Loader::shader(Asset::Shader::flat_texture_offset);
				RenderSync* sync = params.sync;
				sync->write(RenderOp::Shader);
				sync->write(Asset::Shader::flat_texture_offset);
				sync->write(params.technique);

				sync->write(RenderOp::Uniform);
				sync->write(Asset::Uniform::uv_offset);
				sync->write(RenderDataType::Vec2);
				sync->write<s32>(1);
				sync->write<Vec2>(Vec2(0, Game::real_time.total * 5.0f));
			}

			Quat basis = Quat::look(parkour->grapple_normal);
			{
				Vec3 relative_dir = basis.inverse() * (params.camera->rot * Vec3(0, 0, 1));
				relative_dir.z = 0.0f;
				if (relative_dir.length_squared() > 0.001f)
				{
					r32 angle = atan2f(relative_dir.x, relative_dir.y);
					if (fabsf(parkour->grapple_normal.y) < 0.707f)
						angle = s32(angle / (PI * 0.5f)) * PI * 0.5f;
					basis = basis * Quat::euler(-angle, 0, 0);
				}
			}

			Mat4 m;
			m.make_transform(parkour->grapple_pos, Vec3(1), basis);

			View::draw_mesh(params, Asset::Mesh::reticle_grapple, Asset::Shader::flat_texture_offset, Asset::Texture::bars, m, flag(FlagGrappleValid) ? UI::color_accent() : UI::color_alert());
		}
	}
}

void draw_triangular_reticle(const RenderParams& params, const Vec4& color, const Vec4& center_dot_color = Vec4::zero)
{
	const r32 ratio = 0.8660254037844386f;
	const r32 spoke_length = 12.0f;
	const r32 spoke_width = 3.0f;
	const r32 start_radius = 8.0f + spoke_length * 0.5f;
	Vec2 pos = params.camera->viewport.size * 0.5f;
	UI::centered_box(params, { pos + Vec2(ratio, 0.5f) * UI::scale * start_radius, Vec2(spoke_length, spoke_width) * UI::scale }, color, PI * 0.5f * 0.33f);
	UI::centered_box(params, { pos + Vec2(-ratio, 0.5f) * UI::scale * start_radius, Vec2(spoke_length, spoke_width) * UI::scale }, color, PI * 0.5f * -0.33f);
	UI::centered_box(params, { pos + Vec2(0, -1.0f) * UI::scale * start_radius, Vec2(spoke_width, spoke_length) * UI::scale }, color);

	if (center_dot_color.w > 0.0f)
		UI::triangle(params, { pos, Vec2(10.0f * UI::scale) }, center_dot_color, PI);
}

void PlayerControlHuman::draw_ui(const RenderParams& params) const
{
	if (params.technique != RenderTechnique::Default
		|| params.camera != player.ref()->camera.ref()
		|| (player.ref()->gamepad == 0 && Overworld::active())
		|| Game::level.noclip
		|| Team::match_state == Team::MatchState::Done)
		return;

	const Rect2& viewport = params.camera->viewport;

	r32 range = has<Drone>() ? get<Drone>()->range() : DRONE_MAX_DISTANCE;

	AI::Team team = get<AIAgent>()->team;

#if DEBUG_NET_SYNC
	Vec3 remote_abs_pos = remote_control.pos;
	if (remote_control.parent.ref())
		remote_abs_pos = remote_control.parent.ref()->to_world(remote_abs_pos);
	UI::indicator(params, remote_abs_pos, UI::color_default, false);
#endif

	// target indicators
	Vec2 size(24.0f * UI::scale);
	for (s32 i = 0; i < target_indicators.length; i++)
	{
		const TargetIndicator& indicator = target_indicators[i];
		switch (indicator.type)
		{
			case TargetIndicator::Type::DroneVisible:
				UI::indicator(params, indicator.pos, UI::color_alert(), false);
				break;
			case TargetIndicator::Type::Battery:
			case TargetIndicator::Type::BatteryOutOfRange:
				UI::indicator(params, indicator.pos, UI::color_accent(), true, 1.0f, PI);
				break;
			case TargetIndicator::Type::BatteryEnemy:
			case TargetIndicator::Type::BatteryEnemyOutOfRange:
				UI::indicator(params, indicator.pos, Team::color_ui_enemy(), true, 1.0f, PI);
				break;
			case TargetIndicator::Type::BatteryFriendly:
			case TargetIndicator::Type::BatteryFriendlyOutOfRange:
				UI::indicator(params, indicator.pos, Team::color_ui_friend(), true, 1.0f, PI);
				break;
			case TargetIndicator::Type::Minion:
				UI::indicator(params, indicator.pos, Team::color_ui_enemy(), false, 1.0f, PI);
				break;
			case TargetIndicator::Type::Turret:
			case TargetIndicator::Type::MinionSpawner:
				UI::indicator(params, indicator.pos, Team::color_ui_enemy(), false);
				break;
			case TargetIndicator::Type::TurretAttacking:
			{
				if (UI::flash_function(Game::real_time.total))
					UI::indicator(params, indicator.pos, UI::color_alert(), true);
				break;
			}
			case TargetIndicator::Type::Rectifier:
			case TargetIndicator::Type::ForceField:
			case TargetIndicator::Type::Grenade:
				break;
			default:
				vi_assert(false);
				break;
		}
	}

	b8 enemy_visible = false;
	b8 enemy_dangerous_visible = false; // an especially dangerous enemy is visible

	{
		Vec3 me = get<Transform>()->absolute_pos();

		AI::Team my_team = get<AIAgent>()->team;

		// turret health bars
		for (auto i = Turret::list.iterator(); !i.is_last(); i.next())
		{
			Vec3 turret_pos = i.item()->get<Transform>()->absolute_pos();

			if ((turret_pos - me).length_squared() < range * range)
			{
				Vec2 p;
				if (UI::project(params, turret_pos, &p))
					draw_health_bar(params, i.item()->get<Health>(), p + Vec2(0, 32.0f * UI::scale), Team::color_ui(team, i.item()->team));

				if (i.item()->target.ref() == entity())
					enemy_visible = true;
			}
		}

		// minion spawner health bars
		for (auto i = MinionSpawner::list.iterator(); !i.is_last(); i.next())
		{
			Vec3 pos = i.item()->get<Transform>()->absolute_pos();

			if ((pos - me).length_squared() < range * range)
			{
				Vec2 p;
				if (UI::project(params, pos, &p))
					draw_health_bar(params, i.item()->get<Health>(), p + Vec2(0, 32.0f * UI::scale), Team::color_ui(team, i.item()->team));
			}
		}

		// force field health bars
		for (auto i = ForceField::list.iterator(); !i.is_last(); i.next())
		{
			if (!(i.item()->flags & ForceField::FlagInvincible))
			{
				Vec3 pos = i.item()->get<Transform>()->absolute_pos();
				if ((pos - me).length_squared() < range * range)
				{
					Vec2 p;
					if (UI::project(params, pos, &p))
						draw_health_bar(params, i.item()->get<Health>(), p + Vec2(0, 40.0f * UI::scale), Team::color_ui(team, i.item()->team));
				}
			}
		}

		// highlight enemy grenades in-air
		for (auto i = Grenade::list.iterator(); !i.is_last(); i.next())
		{
			if (i.item()->team != team
				&& !i.item()->get<Transform>()->parent.ref())
			{
				Vec3 pos = i.item()->get<Transform>()->absolute_pos();
				if ((me - pos).length_squared() < DRONE_MAX_DISTANCE * DRONE_MAX_DISTANCE)
				{
					enemy_visible = true;
					enemy_dangerous_visible = true;

					UI::indicator(params, pos, Team::color_ui_enemy(), true);

					UIText text;
					text.color = Team::color_ui(team, i.item()->team);
					text.text(player.ref()->gamepad, _(strings::grenade_incoming));
					text.anchor_x = UIText::Anchor::Center;
					text.anchor_y = UIText::Anchor::Center;
					Vec2 p;
					UI::is_onscreen(params, pos, &p);
					p.y += UI_TEXT_SIZE_DEFAULT * 2.0f * UI::scale;
					UI::box(params, text.rect(p).outset(8.0f * UI::scale), UI::color_background);
					if (UI::flash_function(Game::real_time.total))
						text.draw(params, p);
				}
			}
		}

		for (auto i = Bolt::list.iterator(); !i.is_last(); i.next())
		{
			if (i.item()->team != my_team && i.item()->visible())
			{
				Vec3 pos = i.item()->get<Transform>()->absolute_pos();
				Vec3 diff = me - pos;
				r32 distance = diff.length();
				if (distance < DRONE_MAX_DISTANCE
					&& (diff / distance).dot(Vec3::normalize(i.item()->velocity)) > 0.7f)
					enemy_dangerous_visible = true;
			}
		}
	}

	if (has<Drone>())
	{
		PlayerManager* manager = player.ref()->get<PlayerManager>();

		// highlight upgrade point if there is an upgrade available
		if (Game::level.has_feature(Game::FeatureLevel::Abilities)
			&& !get<Drone>()->flag.ref()
			&& (Game::level.has_feature(Game::FeatureLevel::All) || Game::level.feature_level == Game::FeatureLevel::Abilities) // disable prompt in tutorial after ability has been purchased
			&& manager->upgrade_available() && manager->upgrade_highest_owned_or_available() != player.ref()->upgrade_last_visit_highest_available
			&& !UpgradeStation::drone_at(get<Drone>())
			&& !UpgradeStation::drone_inside(get<Drone>()))
		{
			UpgradeStation* station = UpgradeStation::closest_available(get<AIAgent>()->team, get<Transform>()->absolute_pos());
			if (station)
			{
				Vec3 pos = station->get<Transform>()->absolute_pos();
				Vec2 p = UI::indicator(params, pos, Team::color_ui_friend(), true);

				p.y += UI_TEXT_SIZE_DEFAULT * 2.0f * UI::scale;
				if (UI::flash_function_slow(Game::real_time.total))
				{
					UIText text;
					text.color = Team::color_ui_friend();
					text.text(player.ref()->gamepad, _(strings::upgrade_notification));
					text.anchor_x = UIText::Anchor::Center;
					text.anchor_y = UIText::Anchor::Center;
					UI::box(params, text.rect(p).outset(8.0f * UI::scale), UI::color_background);
					text.draw(params, p);
				}
			}
		}
	}
	else
	{
		// parkour mode

		Interactable* closest_interactable = Interactable::closest(get<Transform>()->absolute_pos());

		b8 resource_changed = false;
		for (s32 i = 0; i < s32(Resource::count); i++)
		{
			if (Game::real_time.total - Overworld::resource_change_time(Resource(i)) < 2.0f)
			{
				resource_changed = true;
				break;
			}
		}

		if (closest_interactable || resource_changed)
		{
			// draw resources
			const Vec2 panel_size(MENU_ITEM_WIDTH * 0.3f, MENU_ITEM_PADDING * 2.0f + UI_TEXT_SIZE_DEFAULT * UI::scale);
			Vec2 pos(viewport.size.x * 0.9f, viewport.size.y * 0.1f);
			UIText text;
			text.anchor_y = UIText::Anchor::Center;
			text.anchor_x = UIText::Anchor::Max;
			text.size = UI_TEXT_SIZE_DEFAULT;
			for (s32 i = s32(Resource::ConsumableCount) - 1; i >= 0; i--)
			{
				UI::box(params, { pos + Vec2(-panel_size.x, 0), panel_size }, UI::color_background);

				r32 icon_size = UI_TEXT_SIZE_DEFAULT * UI::scale;

				const Overworld::ResourceInfo& info = Overworld::resource_info[i];

				b8 blink = Game::real_time.total - Overworld::resource_change_time(Resource(i)) < 0.5f;
				b8 draw = !blink || UI::flash_function(Game::real_time.total);

				if (draw)
				{
					const Vec4& color = blink
						?  UI::color_default
						: (Game::save.resources[i] == 0 ? UI::color_alert() : UI::color_accent());
					UI::mesh(params, info.icon, pos + Vec2(-panel_size.x + MENU_ITEM_PADDING + icon_size * 0.5f, panel_size.y * 0.5f), Vec2(icon_size), color);
					text.color = color;
					text.text(player.ref()->gamepad, "%d", Game::save.resources[i]);
					text.draw(params, pos + Vec2(-MENU_ITEM_PADDING, panel_size.y * 0.5f));
				}

				pos.y += panel_size.y;
			}
		}

		if (input_enabled())
		{
			// interact prompt
			if (closest_interactable)
				UI::prompt_interact(params);

			if (Settings::waypoints)
			{
				// highlight trams
				Vec3 look_dir = params.camera->rot * Vec3(0, 0, 1);
				for (auto i = Tram::list.iterator(); !i.is_last(); i.next())
				{
					if (i.item()->arrive_only)
						continue;

					Vec3 pos = i.item()->get<Transform>()->absolute_pos();
					Vec3 to_tram = pos - params.camera->pos;
					r32 distance = to_tram.length();
					if (distance > 8.0f)
					{
						to_tram /= distance;
						if (to_tram.dot(look_dir) > 0.92f)
						{
							Vec2 p;
							if (UI::project(params, pos + Vec3(0, 3, 0), &p))
							{
								const Game::TramTrack& entry = Game::level.tram_tracks[i.item()->track()];

								if (entry.level == AssetNull)
									continue;

								UIText text;
								switch (Game::save.zones[entry.level])
								{
									case ZoneState::PvpFriendly:
										text.color = Team::color_ui_friend();
										break;
									case ZoneState::ParkourUnlocked:
										text.color = UI::color_default;
										break;
									case ZoneState::ParkourOwned:
										text.color = UI::color_accent();
										break;
									case ZoneState::Locked:
									{
										if (Overworld::zone_is_pvp(entry.level))
											text.color = UI::color_default;
										else
											text.color = Game::save.resources[s32(Resource::Energy)] >= entry.energy_threshold ? UI::color_default : UI::color_disabled();
										break;
									}
									case ZoneState::PvpHostile:
										text.color = Team::color_ui_enemy();
										break;
									default:
										vi_assert(false);
										break;
								}
								text.text(player.ref()->gamepad, Loader::level_name(entry.level));
								text.anchor_x = UIText::Anchor::Center;
								text.anchor_y = UIText::Anchor::Center;
								text.size = UI_TEXT_SIZE_DEFAULT * 0.75f;
								UI::box(params, text.rect(p).outset(4.0f * UI::scale), UI::color_background);
								text.draw(params, p);
							}
						}
					}
				}

				// highlight shop
				if (Game::level.shop.ref())
				{
					Vec3 pos = Game::level.shop.ref()->get<Transform>()->absolute_pos();
					Vec3 to_shop = pos - params.camera->pos;
					r32 distance = to_shop.length();
					if (distance > 8.0f)
					{
						to_shop /= distance;
						if (to_shop.dot(look_dir) > 0.92f)
						{
							Vec2 p;
							if (UI::project(params, pos + Vec3(0, 3, 0), &p))
							{
								UIText text;
								text.color = UI::color_default;
								text.text(player.ref()->gamepad, _(strings::shop));
								text.anchor_x = UIText::Anchor::Center;
								text.anchor_y = UIText::Anchor::Center;
								text.size = UI_TEXT_SIZE_DEFAULT * 0.75f;
								UI::box(params, text.rect(p).outset(4.0f * UI::scale), UI::color_background);
								text.draw(params, p);
							}
						}
					}
				}

				if (get<Parkour>()->fsm.current == ParkourState::Climb)
				{
					// show climb controls
					UIText text;
					text.color = UI::color_accent();
					text.text(player.ref()->gamepad, "{{ClimbingMovement}}");
					text.anchor_x = UIText::Anchor::Center;
					text.anchor_y = UIText::Anchor::Center;
					Vec2 pos = params.camera->viewport.size * Vec2(0.5f, 0.1f);
					UI::box(params, text.rect(pos).outset(8.0f * UI::scale), UI::color_background);
					text.draw(params, pos);
				}
			}
		}

		if (get<Parkour>()->flag(Parkour::FlagTryGrapple))
		{
			// cancel grapple
			UIText text;
			text.color = UI::color_accent();
			text.text(player.ref()->gamepad, _(strings::prompt_cancel_grapple));
			text.anchor_x = UIText::Anchor::Center;
			text.anchor_y = UIText::Anchor::Center;
			Vec2 pos = params.camera->viewport.size * Vec2(0.5f, 0.2f);
			UI::box(params, text.rect(pos).outset(8.0f * UI::scale), UI::color_background);
			text.draw(params, pos);
		}
		else if (Settings::parkour_reticle && movement_enabled())
		{
			if (Parkour::ability_enabled(Resource::Grapple))
			{
				if (flag(FlagGrappleValid) && get<Parkour>()->grapple_cooldown < GRAPPLE_COOLDOWN_THRESHOLD)
					draw_triangular_reticle(params, UI::color_accent(), UI::color_accent()); // draw reticle with center dot
				else
					draw_triangular_reticle(params, UI::color_alert()); // no center dot
			}
			else
				draw_triangular_reticle(params, UI::color_accent()); // no center dot
		}

		draw_cooldown(params, get<Parkour>()->grapple_cooldown, viewport.size * Vec2(0.5f, 0.15f), GRAPPLE_COOLDOWN_THRESHOLD);
	}

	// common UI for both parkour and PvP modes

	// usernames directly over players' 3D positions
	for (auto other_player = PlayerCommon::list.iterator(); !other_player.is_last(); other_player.next())
	{
		if (other_player.item() != get<PlayerCommon>())
		{
			b8 visible = player_determine_visibility(get<PlayerCommon>(), other_player.item());

			b8 friendly = Game::level.mode == Game::Mode::Parkour || other_player.item()->get<AIAgent>()->team == team;

			if (visible && !friendly)
			{
				enemy_visible = true;
				enemy_dangerous_visible = true;
			}

			if (visible)
			{
				const Vec4* color = Game::level.mode == Game::Mode::Parkour
					? &UI::color_accent()
					: (friendly ? &Team::color_ui_friend() : &Team::color_ui_enemy());

				// if we can see or track them, the indicator has already been added using add_target_indicator in the update function

				Vec3 pos3d = other_player.item()->get<Transform>()->absolute_pos() + Vec3(0, DRONE_RADIUS * 2.0f, 0);
				if (other_player.item()->has<Parkour>())
					pos3d.y += MINION_HEAD_RADIUS * 0.75f;
				Vec2 p;
				if (UI::project(params, pos3d, &p))
				{
					Vec2 username_pos = p;
					username_pos.y += UI_TEXT_SIZE_DEFAULT * UI::scale;

					{
						UIText username;
						username.anchor_x = UIText::Anchor::Center;
						username.anchor_y = UIText::Anchor::Min;
						username.color = *color;
						{
							PlayerManager* other_manager = other_player.item()->manager.ref();
							username.icon = other_manager->flag(PlayerManager::FlagIsVip) ? Asset::Mesh::icon_vip : AssetNull;
							username.text_raw(player.ref()->gamepad, other_manager->username);
						}

						UI::box(params, username.rect(username_pos).outset(HP_BOX_SPACING), UI::color_background);

						username.draw(params, username_pos);
					}

					{
						PlayerManager* other_manager = other_player.item()->manager.ref();
						s32 ability_count = other_manager->ability_count();
						if (ability_count > 0)
						{
							r32 item_size = UI_TEXT_SIZE_DEFAULT * UI::scale * 0.75f;
							Vec2 p2 = username_pos + Vec2((ability_count * -0.5f + 0.5f) * item_size + ((ability_count - 1) * HP_BOX_SPACING * -0.5f), (UI_TEXT_SIZE_DEFAULT * UI::scale) + item_size);
							UI::box(params, { Vec2(p2.x + item_size * -0.5f - HP_BOX_SPACING, p2.y + item_size * -0.5f - HP_BOX_SPACING), Vec2((ability_count * item_size) + ((ability_count + 1) * HP_BOX_SPACING), item_size + HP_BOX_SPACING * 2.0f) }, UI::color_background);
							for (s32 i = 0; i < MAX_ABILITIES; i++)
							{
								Ability ability = other_manager->abilities[i];
								if (ability != Ability::None)
								{
									const AbilityInfo& info = AbilityInfo::list[s32(ability)];
									UI::mesh(params, info.icon, p2, Vec2(item_size), ability == other_player.item()->get<Drone>()->current_ability ? UI::color_default : *color);
									p2.x += item_size + HP_BOX_SPACING;
								}
							}
						}
					}
				}
			}
		}
	}

	{
		const Health* health = get<Health>();

		b8 is_vulnerable = get<Health>()->can_take_damage(nullptr) && health->hp == 1 && health->shield == 0 && health->shield_max > 0;

		Vec2 ui_anchor = player.ref()->ui_anchor(params);
		ui_anchor.y = params.camera->viewport.size.y * 0.5f + UI_TEXT_SIZE_DEFAULT * -2.0f;

		UIText text;
		text.anchor_x = UIText::Anchor::Min;
		text.anchor_y = UIText::Anchor::Max;

		b8 danger = Game::level.mode == Game::Mode::Pvp && enemy_visible && (enemy_dangerous_visible || is_vulnerable);

		if (has<Drone>() && get<Drone>()->flag.ref())
		{
			// flag indicator
			text.color = UI::color_accent();
			text.icon = Asset::Mesh::icon_flag;
			text.text(player.ref()->gamepad, _(strings::carrying_flag));
			UI::box(params, text.rect(ui_anchor).outset(8 * UI::scale), UI::color_background);
			text.draw(params, ui_anchor);
			text.icon = AssetNull;
			ui_anchor.y -= (UI_TEXT_SIZE_DEFAULT + 24) * UI::scale;
		}

		if (danger && (is_vulnerable ? UI::flash_function(Game::time.total) : UI::flash_function_slow(Game::time.total)))
		{
			// danger indicator
			text.color = UI::color_alert();
			text.text(player.ref()->gamepad, _(strings::danger));
			UI::box(params, text.rect(ui_anchor).outset(8 * UI::scale), UI::color_background);
			text.draw(params, ui_anchor);
		}
		ui_anchor.y -= (UI_TEXT_SIZE_DEFAULT + 24) * UI::scale;

		// shield indicator
		if (is_vulnerable)
		{
			if (danger ? UI::flash_function(Game::time.total) : UI::flash_function_slow(Game::time.total))
			{
				text.color = UI::color_alert();
				text.text(player.ref()->gamepad, _(strings::shield_down));
				UI::box(params, text.rect(ui_anchor).outset(8 * UI::scale), UI::color_background);
				text.draw(params, ui_anchor);
			}

			if (danger)
			{
				if (UI::flash_function(Game::time.total) && !UI::flash_function(Game::time.total - Game::time.delta))
					Audio::post_global(AK::EVENTS::PLAY_UI_SHIELD_DOWN_BEEP, player.ref()->gamepad);
			}
			else
			{
				if (UI::flash_function_slow(Game::time.total) && !UI::flash_function_slow(Game::time.total - Game::time.delta))
					Audio::post_global(AK::EVENTS::PLAY_DANGER_BEEP, player.ref()->gamepad);
			}
		}

		ui_anchor.y -= (UI_TEXT_SIZE_DEFAULT + 24) * UI::scale;
	}

	// reticle
	if (has<Drone>()
		&& movement_enabled()
#if !SERVER
		&& Net::Client::replay_mode() != Net::Client::ReplayMode::Replaying
#endif
		)
	{
		Vec2 pos = viewport.size * Vec2(0.5f, 0.5f);

		b8 cooldown_can_go = get<Drone>()->cooldown_can_shoot();

		b8 reticle_valid;
		const Vec4* color;
		if (reticle.type == ReticleType::Error || reticle.type == ReticleType::DashError)
		{
			color = &UI::color_disabled();
			reticle_valid = false;
		}
		else if (reticle.type != ReticleType::None
			&& cooldown_can_go
			&& (get<Drone>()->current_ability == Ability::None || player.ref()->get<PlayerManager>()->ability_valid(get<Drone>()->current_ability)))
		{
			color = &UI::color_accent();
			reticle_valid = true;
		}
		else
		{
			color = &UI::color_alert();
			reticle_valid = false;
		}

		Ability a = get<Drone>()->current_ability;
		const AbilityInfo& info = AbilityInfo::list[s32(a)];

		// reticle
		if (get<Drone>()->cooldown_ability_switch == 0.0f)
		{
			r32 cooldown_use = player.ref()->get<PlayerManager>()->ability_cooldown[s32(a)];
			if (info.cooldown_use_threshold > 0.0f && cooldown_use >= info.cooldown_use_threshold) // ability cooldown reticle
			{
				UI::mesh(params, Asset::Mesh::icon_reticle_invalid, pos, Vec2(32.0f * UI::scale), *color);
				UI::triangle_percentage(params, { pos, Vec2(47.0f * UI::scale) }, 1.0f - ((cooldown_use - info.cooldown_use_threshold) / info.cooldown_use), *color, PI);
			}
			else if (reticle_valid) // normal reticle
			{
				if (reticle.type == ReticleType::Normal
					|| reticle.type == ReticleType::Target
					|| reticle.type == ReticleType::DashTarget) // draw reticle with center dot
					draw_triangular_reticle(params, *color, reticle.type == ReticleType::Target || reticle.type == ReticleType::DashTarget ? UI::color_alert() : *color);
				else // no center dot
					draw_triangular_reticle(params, *color);
			}
			else // can't do it
				UI::mesh(params, Asset::Mesh::icon_reticle_invalid, pos, Vec2(32.0f * UI::scale), *color);
		}

		// cooldown indicator
		draw_cooldown(params, get<Drone>()->cooldown, pos + Vec2(0, -37.0f * UI::scale), DRONE_COOLDOWN_THRESHOLD);

		// ability icon
		UI::mesh(params, info.icon, pos + Vec2(0, -64.0f * UI::scale), Vec2(18.0f * UI::scale), *color);
	}
}


}
