#pragma once

#include "data/entity.h"
#include "lmath.h"
#include "physics.h"
#include "render/ui.h"
#include "ai.h"
#include "menu.h"
#include "team.h"

namespace VI
{

struct RigidBody;
struct Transform;
struct LocalPlayerControl;
struct PlayerManager;
struct Health;
struct DamageEvent;

struct LocalPlayer
{
	enum class UIMode { Default, Pause, Spawning, Upgrading, GameOver };

	static PinArray<LocalPlayer, MAX_PLAYERS> list;

	u8 gamepad;
	UIMenu menu;
	Ref<Transform> map_view;
	Ref<PlayerManager> manager;
	Camera* camera;
	r32 msg_timer;
	UIText msg_text;
	b8 msg_good;
	Revision revision;
	Menu::State menu_state;
	b8 upgrading;
	r32 upgrade_animation_time;
	UIScroll score_summary_scroll;
	
	struct SupportEntry
	{
		Ref<RigidBody> support;
		Vec3 relative_position;
		r32 rotation;
	};
	StaticArray<SupportEntry, 4> last_supported;

	inline ID id() const
	{
		return this - &list[0];
	}

	LocalPlayer(PlayerManager*, u8);

	static r32 danger;
	static void update_all(const Update&);
	void msg(const char*, b8);
	UIMode ui_mode() const;
	void update(const Update&);
	void draw_alpha(const RenderParams&) const;
	void ensure_camera(const Update&, b8);
	void spawn();
};

struct PlayerCommon : public ComponentType<PlayerCommon>
{
	Ref<PlayerManager> manager;
	r32 angle_horizontal;
	r32 last_angle_horizontal;
	r32 angle_vertical;
	Quat attach_quat;

	static s32 visibility_hash(const PlayerCommon*, const PlayerCommon*);
	static Bitmask<MAX_PLAYERS * MAX_PLAYERS> visibility;

	PlayerCommon(PlayerManager*);
	void awake();

	Vec3 look_dir() const;
	r32 detect_danger() const;
	void update(const Update&);
	void awk_detached();
	void awk_done_flying();
	void awk_bounce(const Vec3&);
	void clamp_rotation(const Vec3&, r32 = 0.0f);
	b8 movement_enabled() const;
};

struct LocalPlayerControl : public ComponentType<LocalPlayerControl>
{
	enum class ReticleType
	{
		None,
		Error,
		Normal,
		Target,
		Dash,
		DashError,
	};

	struct Reticle
	{
		ReticleType type;
		Vec3 pos;
	};

	struct TargetIndicator
	{
		enum class Type
		{
			AwkVisible,
			AwkTracking,
			Minion,
			MinionAttacking,
			Health,
		};

		Vec3 pos;
		Vec3 velocity;
		Type type;
	};

	static LocalPlayerControl* player_for_camera(const Camera*);

	Ref<LocalPlayer> player;

	Reticle reticle;
	StaticArray<TargetIndicator, 32> target_indicators;
	Camera* camera;
	r32 fov_blend;
	r32 damage_timer;
	r32 health_flash_timer;
	b8 try_secondary;
	b8 try_primary;
	b8 try_slide;
	u8 gamepad;
	r32 rumble;
	Vec3 last_pos;
	r32 last_gamepad_input_time;
	r32 gamepad_rotation_speed;

	LocalPlayerControl(u8);
	~LocalPlayerControl();
	void awake();

	void awk_done_flying_or_dashing();
	void hit_target(Entity*);
	void damaged(const DamageEvent&);
	void hit_by(const TargetEvent&);
	void health_picked_up();
	b8 add_target_indicator(Target*, TargetIndicator::Type);
	void parkour_landed(r32);

	void update(const Update&);
	void draw_alpha(const RenderParams&) const;

	void update_camera_input(const Update&, r32 = 1.0f);
	Vec3 get_movement(const Update&, const Quat&);
	b8 input_enabled() const;
	b8 movement_enabled() const;
};

}
