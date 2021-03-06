#include "load.h"
#include <stdio.h>
#include "vi_assert.h"
#include "asset/lookup.h"
#if !SERVER
#include <AK/SoundEngine/Common/AkSoundEngine.h>
#include "lodepng/lodepng.h"
#endif
#include "cjson/cJSON.h"
#include "data/json.h"
#include "data/unicode.h"
#include "ai.h"
#include "settings.h"
#include "game/master.h"
#include "game/overworld.h"

namespace VI
{

const char* Loader::data_directory;
LoopSwapper* Loader::swapper;

namespace Settings
{
	Gamepad gamepads[MAX_GAMEPADS];
	s32 display_mode_index;
	s32 framerate_limit;
#if SERVER
	u64 secret;
	u16 port;
#endif
	Region region;
	ShadowQuality shadow_quality;
	char master_server[MAX_PATH_LENGTH + 1];
	char username[MAX_USERNAME + 1];
	char gamejolt_username[MAX_PATH_LENGTH + 1];
	char gamejolt_token[MAX_AUTH_KEY + 1];
	char itch_api_key[MAX_AUTH_KEY + 1];
#if SERVER
	char public_ipv4[NET_MAX_ADDRESS];
	char public_ipv6[NET_MAX_ADDRESS];
#endif
	u8 sfx;
	u8 music;
	u8 fov;
	WindowMode window_mode;
	b8 vsync;
	b8 volumetric_lighting;
	b8 antialiasing;
	b8 waypoints;
	b8 scan_lines;
	b8 subtitles;
	b8 ssao;
	b8 record;
	b8 expo;
	b8 shell_casings;
	b8 god_mode;
	b8 parkour_reticle;
	NetClientInterpolationMode net_client_interpolation_mode;
	PvpColorScheme pvp_color_scheme;

	const DisplayMode& display()
	{
		return Loader::display_modes[display_mode_index];
	}
}

Array<Loader::Entry<Mesh> > Loader::meshes;
Array<Loader::Entry<Animation> > Loader::animations;
Array<Loader::Entry<Armature> > Loader::armatures;
Array<Loader::Entry<s8> > Loader::textures;
Array<Loader::Entry<s8> > Loader::shaders;
Array<Loader::Entry<Font> > Loader::fonts;
Array<Loader::Entry<s8> > Loader::dynamic_meshes;
Array<Loader::Entry<s8> > Loader::dynamic_textures;
Array<Loader::Entry<s8> > Loader::framebuffers;
#if !SERVER
Array<Loader::Entry<AkBankID> > Loader::soundbanks;
#endif
Array<DisplayMode> Loader::display_modes;

s32 Loader::compiled_level_count;
s32 Loader::compiled_static_mesh_count;
s32 Loader::static_mesh_count;
s32 Loader::static_texture_count;
s32 Loader::shader_count;
s32 Loader::armature_count;
s32 Loader::animation_count;

#define config_filename "config.txt"
#define offline_configs_filename "offline.txt"
#define config_version 2
#define mod_manifest_filename "mod.json"
#define default_master_server "master.lasercrabs.com"

Array<const char*> mod_level_names;
Array<const char*> mod_level_paths;
Array<const char*> mod_nav_paths;
Array<const char*> mod_level_mesh_names;
Array<const char*> mod_level_mesh_paths;

void Loader::init(LoopSwapper* s)
{
	swapper = s;

	// count levels, static meshes, and static textures at runtime to avoid recompiling all the time
	const char* p;
	while ((p = AssetLookup::Level::names[compiled_level_count]))
		compiled_level_count++;

	while ((p = AssetLookup::Texture::names[static_texture_count]))
		static_texture_count++;
	textures.resize(static_texture_count);

	while ((p = AssetLookup::Shader::names[shader_count]))
		shader_count++;
	shaders.resize(shader_count);

	while ((p = AssetLookup::Armature::names[armature_count]))
		armature_count++;
	armatures.resize(armature_count);

	while ((p = AssetLookup::Animation::names[animation_count]))
		animation_count++;
	animations.resize(animation_count);

	while ((p = AssetLookup::Mesh::names[compiled_static_mesh_count]))
		compiled_static_mesh_count++;
	static_mesh_count = compiled_static_mesh_count;

	{
		s32 i = 0;
		while ((p = AssetLookup::Font::names[i]))
			i++;
		fonts.resize(i);
	}

	// load mod levels and meshes
	{
		cJSON* mod_manifest = Json::load(mod_manifest_filename);
		if (mod_manifest)
		{
			{
				cJSON* mod_levels = cJSON_GetObjectItem(mod_manifest, "lvl");
				cJSON* mod_level = mod_levels->child;
				while (mod_level)
				{
					mod_level_names.add(mod_level->string);
					mod_level_paths.add(Json::get_string(mod_level, "lvl"));
					mod_nav_paths.add(Json::get_string(mod_level, "nav"));
					mod_level = mod_level->next;
				}
			}

			{
				cJSON* mod_level_meshes = cJSON_GetObjectItem(mod_manifest, "lvl_mesh");
				cJSON* mod_level_mesh = mod_level_meshes->child;
				while (mod_level_mesh)
				{
					mod_level_mesh_names.add(mod_level_mesh->string);
					mod_level_mesh_paths.add(mod_level_mesh->valuestring);
					mod_level_mesh = mod_level_mesh->next;
					static_mesh_count++;
				}
			}
		}
		// don't free the json object; we'll read strings directly from it
	}

	meshes.resize(static_mesh_count);

#if !SERVER
	RenderSync* sync = swapper->get();
	s32 i = 0;
	const char* uniform_name;
	while ((uniform_name = AssetLookup::Uniform::names[i]))
	{
		sync->write(RenderOp::AllocUniform);
		sync->write<AssetID>(i);
		s32 length = s32(strlen(uniform_name));
		sync->write(length);
		sync->write(uniform_name, length);
		i++;
	}
#endif
}

InputBinding input_binding(cJSON* parent, const char* key, const InputBinding& default_value)
{
	if (!parent)
		return default_value;

	cJSON* json = cJSON_GetObjectItem(parent, key);
	if (!json)
		return default_value;

	InputBinding binding;
	binding.key1 = KeyCode(Json::get_s32(json, "key", s32(default_value.key1)));
	if (s32(binding.key1) < 0 || s32(binding.key1) >= s32(KeyCode::count))
		binding.key1 = default_value.key1;
	binding.key2 = default_value.key2;
	binding.btn = Gamepad::Btn(Json::get_s32(json, "btn", s32(default_value.btn)));
	if (s32(binding.btn) < 0 || s32(binding.btn) >= s32(Gamepad::Btn::count))
		binding.btn = Gamepad::Btn(default_value.btn);
	return binding;
}

cJSON* input_binding_json(const InputBinding& binding, const InputBinding& default_value)
{
	b8 key_modified = binding.key1 != KeyCode::None && binding.key1 != default_value.key1;
	b8 btn_modified = binding.btn != Gamepad::Btn::None && binding.btn != default_value.btn;
	if (key_modified || btn_modified)
	{
		cJSON* json = cJSON_CreateObject();
		if (key_modified)
			cJSON_AddNumberToObject(json, "key", s32(binding.key1));
		if (btn_modified)
			cJSON_AddNumberToObject(json, "btn", s32(binding.btn));
		return json;
	}
	else
		return nullptr;
}

void Loader::offline_configs_load()
{
	Overworld::master_server_list_end(ServerListType::Mine, 0);

	char path[MAX_PATH_LENGTH + 1];
	user_data_path(path, offline_configs_filename);
	cJSON* json = Json::load(path);
	if (json)
	{
		if (Json::get_s32(json, "version") == config_version)
		{
			cJSON* entries = cJSON_GetObjectItem(json, "configs");
			if (entries)
			{
				u32 id = 1;
				cJSON* element = entries->child;
				while (element)
				{
					Net::Master::ServerListEntry entry;
					entry.max_players = MAX_PLAYERS;
					entry.server_state.id = id;
					entry.server_state.level = AssetNull;
					entry.server_state.max_players = MAX_PLAYERS;
					entry.server_state.player_slots = MAX_PLAYERS;
					entry.creator_username[0] = '\0';
					entry.creator_vip = false;
					strncpy(entry.name, Json::get_string(element, "name", ""), MAX_SERVER_CONFIG_NAME);
					entry.game_type = GameType(vi_max(0, vi_min(s32(GameType::count), Json::get_s32(element, "game_type"))));
					entry.team_count = vi_max(2, vi_min(MAX_TEAMS, Json::get_s32(element, "team_count")));
					entry.preset = Net::Master::Ruleset::Preset(vi_max(0, vi_min(s32(Net::Master::Ruleset::Preset::count), Json::get_s32(element, "preset"))));

					Overworld::master_server_list_entry(ServerListType::Mine, id - 1, entry);
					id++;

					element = element->next;
				}
			}
		}
		Json::json_free(json);
	}
}

void Loader::offline_config_get(s32 id, Net::Master::ServerConfig* config)
{
	char path[MAX_PATH_LENGTH + 1];
	user_data_path(path, offline_configs_filename);
	cJSON* json = Json::load(path);
	if (json)
	{
		if (Json::get_s32(json, "version") == config_version)
		{
			cJSON* entries = cJSON_GetObjectItem(json, "configs");
			if (entries)
			{
				cJSON* element = cJSON_GetArrayItem(entries, id - 1);
				vi_assert(element);
				Net::Master::server_config_parse(element, config);
				config->max_players = MAX_PLAYERS;
				config->id = id;
				strncpy(config->name, Json::get_string(element, "name", ""), MAX_SERVER_CONFIG_NAME);
				config->game_type = GameType(vi_max(0, vi_min(s32(GameType::count), Json::get_s32(element, "game_type"))));
				config->team_count = vi_max(2, vi_min(MAX_TEAMS, Json::get_s32(element, "team_count")));
				config->preset = Net::Master::Ruleset::Preset(vi_max(0, vi_min(s32(Net::Master::Ruleset::Preset::count), Json::get_s32(element, "preset"))));
			}
		}
		Json::json_free(json);
	}
}

void Loader::offline_config_save(Net::Master::ServerConfig* config)
{
	char path[MAX_PATH_LENGTH + 1];
	user_data_path(path, offline_configs_filename);
	cJSON* json = Json::load(path);
	if (!json
		|| Json::get_s32(json, "version") != config_version
		|| !cJSON_GetObjectItem(json, "configs"))
	{
		if (json)
			Json::json_free(json);

		json = cJSON_CreateObject();
		cJSON_AddNumberToObject(json, "version", config_version);
		cJSON_AddItemToObject(json, "configs", cJSON_CreateArray());
	}

	cJSON* configs = cJSON_GetObjectItem(json, "configs");

	cJSON* element = Net::Master::server_config_json(*config);

	{
		cJSON_AddStringToObject(element, "name", config->name);
		cJSON_AddNumberToObject(element, "game_type", s32(config->game_type));
		cJSON_AddNumberToObject(element, "team_count", s32(config->team_count));
		cJSON_AddNumberToObject(element, "preset", s32(config->preset));
	}

	{
		s32 existing_configs_length = cJSON_GetArraySize(configs);
		if (config->id == 0) // append to end
		{
			config->id = existing_configs_length + 1;
			cJSON_AddItemToArray(configs, element);
		}
		else
		{
			vi_assert(existing_configs_length >= config->id - 1);
			cJSON_ReplaceItemInArray(configs, config->id - 1, element);
		}
	}

	Json::save(json, path);
	Json::json_free(json);
}

void Loader::settings_load(const Array<DisplayMode>& modes, const DisplayMode& current_mode)
{
	char path[MAX_PATH_LENGTH + 1];
	user_data_path(path, config_filename);
	cJSON* json = Json::load(path);
	if (Json::get_s32(json, "version") != config_version)
	{
		Json::json_free(json);
		json = nullptr;
	}

	// resolution
	{
		for (s32 i = 0; i < modes.length; i++)
		{
			const DisplayMode& mode = modes[i];
			display_modes.add(mode);
			if (mode.width == current_mode.width && mode.height == current_mode.height)
				Settings::display_mode_index = i;
		}

		DisplayMode saved_display_mode =
		{
			Json::get_s32(json, "width"),
			Json::get_s32(json, "height"),
		};

		// check if saved resolution is actually valid
		for (s32 i = 0; i < modes.length; i++)
		{
			const DisplayMode& mode = modes[i];
			if (mode.width == saved_display_mode.width && mode.height == saved_display_mode.height)
			{
				Settings::display_mode_index = i;
				break;
			}
		}
	}

	{
		WindowMode default_window_mode;
#if defined(__APPLE__)
		default_window_mode = WindowMode::Windowed;
#else
		default_window_mode = WindowMode::Borderless;
#endif
		Settings::window_mode = WindowMode(vi_max(0, vi_min(s32(WindowMode::count) - 1, Json::get_s32(json, "fullscreen", s32(default_window_mode)))));
	}
	Settings::vsync = b8(Json::get_s32(json, "vsync", 0));
	Settings::sfx = u8(Json::get_s32(json, "sfx", 100));
	Settings::music = u8(Json::get_s32(json, "music", 100));
	Settings::framerate_limit = vi_max(30, vi_min(144, Json::get_s32(json, "framerate_limit", 144)));
	Settings::net_client_interpolation_mode = Settings::NetClientInterpolationMode(vi_max(0, vi_min(s32(Settings::NetClientInterpolationMode::count) - 1, Json::get_s32(json, "net_client_interpolation_mode"))));
	Settings::pvp_color_scheme = Settings::PvpColorScheme(vi_max(0, vi_min(s32(Settings::PvpColorScheme::count) - 1, Json::get_s32(json, "pvp_color_scheme"))));
	Settings::shadow_quality = Settings::ShadowQuality(vi_max(0, vi_min(Json::get_s32(json, "shadow_quality", s32(Settings::ShadowQuality::High)), s32(Settings::ShadowQuality::count) - 1)));
	Settings::region = Region(Json::get_s32(json, "region", s32(Region::Invalid)));
	if (s32(Region::count) <= 1)
		Settings::region = Region(0);
	else if (s32(Settings::region) < 0 || s32(Settings::region) >= s32(Region::count))
	{
		Settings::region = Region::Invalid;
#if SERVER
		fprintf(stderr, "%s", "Valid region must be specified in config file.");
		vi_assert(false);
#endif
	}
	Settings::volumetric_lighting = b8(Json::get_s32(json, "volumetric_lighting", 1));
	Settings::antialiasing = b8(Json::get_s32(json, "antialiasing", 1));
	Settings::ssao = b8(Json::get_s32(json, "ssao", 1));
	Settings::fov = u8(vi_max(40, vi_min(150, Json::get_s32(json, "fov", 80))));
	Settings::subtitles = b8(Json::get_s32(json, "subtitles", 1));
	Settings::waypoints = b8(Json::get_s32(json, "waypoints", 1));
	Settings::scan_lines = b8(Json::get_s32(json, "scan_lines", 1));
	Settings::record = b8(Json::get_s32(json, "record", 0));
	Settings::expo = b8(Json::get_s32(json, "expo", 0));
	Settings::god_mode = b8(Json::get_s32(json, "god_mode"));
	Settings::parkour_reticle = b8(Json::get_s32(json, "parkour_reticle"));
#if SERVER
	Settings::shell_casings = false;
#else
	Settings::shell_casings = b8(Json::get_s32(json, "shell_casings", 1));
#endif

	cJSON* gamepads = json ? cJSON_GetObjectItem(json, "gamepads") : nullptr;
	cJSON* gamepad = gamepads ? gamepads->child : nullptr;
	for (s32 i = 0; i < MAX_GAMEPADS; i++)
	{
		Settings::Gamepad* bindings = &Settings::gamepads[i];
		for (s32 j = 0; j < s32(Controls::count); j++)
		{
			const char* name = Input::control_setting_names[j];
			if (name)
				bindings->bindings[j] = input_binding(gamepad, name, Input::control_defaults[j]);
			else // no setting name; this binding cannot be changed
				bindings->bindings[j] = Input::control_defaults[j];
		}

		bindings->invert_y = Json::get_s32(gamepad, "invert_y", 0);
		bindings->zoom_toggle = Json::get_s32(gamepad, "zoom_toggle", 0);
		bindings->sensitivity_gamepad = u16(Json::get_s32(gamepad, "sensitivity_gamepad", 100));
		if (i == 0)
			bindings->sensitivity_mouse = u16(Json::get_s32(gamepad, "sensitivity_mouse", 100));
		bindings->rumble = b8(Json::get_s32(gamepad, "rumble", 1));
		gamepad = gamepad ? gamepad->next : nullptr;
	}

	strncpy(Settings::master_server, Json::get_string(json, "master_server", default_master_server), MAX_PATH_LENGTH);
	strncpy(Settings::username, Json::get_string(json, "username", "Anonymous"), MAX_USERNAME);
	strncpy(Settings::itch_api_key, Json::get_string(json, "itch_api_key", ""), MAX_AUTH_KEY);
	if (!Settings::gamejolt_username[0]) // if the username has already been acquired via other means, don't overwrite it
	{
		strncpy(Settings::gamejolt_username, Json::get_string(json, "gamejolt_username", ""), MAX_PATH_LENGTH);
		strncpy(Settings::gamejolt_token, Json::get_string(json, "gamejolt_token", ""), MAX_AUTH_KEY);
	}
#if SERVER
	{
		cJSON* s = cJSON_GetObjectItem(json, "secret");
		Settings::secret = s ? s->valueint : 0;
	}
	strncpy(Settings::public_ipv4, Json::get_string(json, "public_ipv4", ""), NET_MAX_ADDRESS);
	strncpy(Settings::public_ipv6, Json::get_string(json, "public_ipv6", ""), NET_MAX_ADDRESS);
#endif

	if (json)
		Json::json_free(json);
	else
		settings_save(); // failed to load the config file; save our own
}

void Loader::settings_save()
{
#if !SERVER
	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "version", config_version);
	if (Settings::record)
		cJSON_AddNumberToObject(json, "record", 1);
	if (Settings::expo)
		cJSON_AddNumberToObject(json, "expo", 1);

	// only save master server setting if it is not the default
	if (strncmp(Settings::master_server, default_master_server, MAX_PATH_LENGTH) != 0)
		cJSON_AddStringToObject(json, "master_server", Settings::master_server);

	cJSON_AddStringToObject(json, "username", Settings::username);
	if (Settings::gamejolt_username[0])
	{
		cJSON_AddStringToObject(json, "gamejolt_username", Settings::gamejolt_username);
		cJSON_AddStringToObject(json, "gamejolt_token", Settings::gamejolt_token);
	}
	if (Settings::itch_api_key[0])
		cJSON_AddStringToObject(json, "itch_api_key", Settings::itch_api_key);
	cJSON_AddNumberToObject(json, "framerate_limit", Settings::framerate_limit);
	cJSON_AddNumberToObject(json, "net_client_interpolation_mode", s32(Settings::net_client_interpolation_mode));
	cJSON_AddNumberToObject(json, "pvp_color_scheme", s32(Settings::pvp_color_scheme));
	cJSON_AddNumberToObject(json, "width", Settings::display().width);
	cJSON_AddNumberToObject(json, "height", Settings::display().height);
	cJSON_AddNumberToObject(json, "fullscreen", s32(Settings::window_mode));
	cJSON_AddNumberToObject(json, "vsync", Settings::vsync);
	cJSON_AddNumberToObject(json, "sfx", Settings::sfx);
	cJSON_AddNumberToObject(json, "music", Settings::music);
	cJSON_AddNumberToObject(json, "shadow_quality", s32(Settings::shadow_quality));
	cJSON_AddNumberToObject(json, "region", s32(Settings::region));
	cJSON_AddNumberToObject(json, "volumetric_lighting", s32(Settings::volumetric_lighting));
	cJSON_AddNumberToObject(json, "antialiasing", s32(Settings::antialiasing));
	cJSON_AddNumberToObject(json, "ssao", s32(Settings::ssao));
	cJSON_AddNumberToObject(json, "fov", s32(Settings::fov));
	cJSON_AddNumberToObject(json, "subtitles", s32(Settings::subtitles));
	cJSON_AddNumberToObject(json, "waypoints", s32(Settings::waypoints));
	cJSON_AddNumberToObject(json, "scan_lines", s32(Settings::scan_lines));
	cJSON_AddNumberToObject(json, "shell_casings", s32(Settings::shell_casings));
	if (Settings::god_mode)
		cJSON_AddNumberToObject(json, "god_mode", 1);
	cJSON_AddNumberToObject(json, "parkour_reticle", s32(Settings::parkour_reticle));

	cJSON* gamepads = cJSON_CreateArray();
	cJSON_AddItemToObject(json, "gamepads", gamepads);

	for (s32 i = 0; i < MAX_GAMEPADS; i++)
	{
		Settings::Gamepad* bindings = &Settings::gamepads[i];
		cJSON* gamepad = cJSON_CreateObject();
		for (s32 j = 0; j < s32(Controls::count); j++)
		{
			const char* name = Input::control_setting_names[j];
			if (name)
			{
				cJSON* json = input_binding_json(bindings->bindings[j], Input::control_defaults[j]);
				if (json)
					cJSON_AddItemToObject(gamepad, name, json);
			}
		}
		cJSON_AddItemToObject(gamepad, "invert_y", cJSON_CreateNumber(bindings->invert_y));
		cJSON_AddItemToObject(gamepad, "sensitivity_gamepad", cJSON_CreateNumber(bindings->sensitivity_gamepad));
		if (i == 0)
			cJSON_AddItemToObject(gamepad, "sensitivity_mouse", cJSON_CreateNumber(bindings->sensitivity_mouse));
		cJSON_AddItemToObject(gamepad, "rumble", cJSON_CreateNumber(bindings->rumble));
		cJSON_AddItemToArray(gamepads, gamepad);
	}

	char path[MAX_PATH_LENGTH + 1];
	user_data_path(path, config_filename);

	Json::save(json, path);
	Json::json_free(json);
#endif
}

const Mesh* Loader::mesh(AssetID id)
{
	if (id == AssetNull)
		return nullptr;

	vi_assert(id < static_mesh_count);

	if (id >= meshes.length)
		meshes.resize(id + 1);
	if (meshes[id].type == AssetNone)
	{
		Array<Mesh::Attrib> extra_attribs;
		Mesh* mesh = &meshes[id].data;
		Mesh::read(mesh, mesh_path(id), &extra_attribs);

#if SERVER
		for (s32 i = 0; i < extra_attribs.length; i++)
			extra_attribs[i].~Attrib();
#else
		// GL

		RenderSync* sync = Loader::swapper->get();
		sync->write(RenderOp::AllocMesh);
		sync->write<AssetID>(id);
		sync->write<b8>(false); // whether the buffers should be dynamic or not

		sync->write<s32>(2 + extra_attribs.length); // attribute count

		sync->write(RenderDataType::Vec3); // position
		sync->write<s32>(1); // number of data elements per vertex

		sync->write(RenderDataType::Vec3); // normal
		sync->write<s32>(1); // number of data elements per vertex

		for (s32 i = 0; i < extra_attribs.length; i++)
		{
			Mesh::Attrib* a = &extra_attribs[i];
			sync->write<RenderDataType>(a->type);
			sync->write<s32>(a->count);
		}

		sync->write(RenderOp::UpdateAttribBuffers);
		sync->write<AssetID>(id);

		sync->write<s32>(mesh->vertices.length);
		sync->write(mesh->vertices.data, mesh->vertices.length);
		sync->write(mesh->normals.data, mesh->vertices.length);

		for (s32 i = 0; i < extra_attribs.length; i++)
		{
			Mesh::Attrib* a = &extra_attribs[i];
			sync->write(a->data.data, a->data.length);
			a->~Attrib(); // release data
		}

		sync->write(RenderOp::UpdateIndexBuffer);
		sync->write<AssetID>(id);
		sync->write<s32>(mesh->indices.length);
		sync->write(mesh->indices.data, mesh->indices.length);

		sync->write(RenderOp::UpdateEdgesIndexBuffer);
		sync->write<AssetID>(id);
		sync->write<s32>(mesh->edge_indices.length);
		sync->write(mesh->edge_indices.data, mesh->edge_indices.length);
#endif

		meshes[id].type = AssetTransient;
	}
	return &meshes[id].data;
}

const Mesh* Loader::mesh_permanent(AssetID id)
{
	const Mesh* m = mesh(id);
	if (m)
		meshes[id].type = AssetPermanent;
	return m;
}

const Mesh* Loader::mesh_instanced(AssetID id)
{
	Mesh* m = (Mesh*)mesh(id);
	if (m && !m->instanced)
	{
#if !SERVER
		RenderSync* sync = swapper->get();
		sync->write(RenderOp::AllocInstances);
		sync->write<AssetID>(id);
#endif
		m->instanced = true;
	}
	return m;
}

void Loader::mesh_free(AssetID id)
{
	if (id != AssetNull && meshes[id].type != AssetNone)
	{
		meshes[id].data.~Mesh();
#if !SERVER
		RenderSync* sync = swapper->get();
		sync->write(RenderOp::FreeMesh);
		sync->write<AssetID>(id);
#endif
		meshes[id].type = AssetNone;
	}
}

const Armature* Loader::armature(AssetID id)
{
	if (id == AssetNull || id >= armature_count)
		return 0;

	if (id >= armatures.length)
		armatures.resize(id + 1);
	if (armatures[id].type == AssetNone)
	{
		const char* path = AssetLookup::Armature::values[id];
		FILE* f = fopen(path, "rb");
		if (!f)
		{
			fprintf(stderr, "Can't open arm file '%s'\n", path);
			return 0;
		}

		Armature* arm = &armatures[id].data;
		new (arm) Armature();

		s32 bones;
		fread(&bones, sizeof(s32), 1, f);
		arm->hierarchy.resize(bones);
		fread(arm->hierarchy.data, sizeof(s32), bones, f);
		arm->bind_pose.resize(bones);
		arm->inverse_bind_pose.resize(bones);
		arm->abs_bind_pose.resize(bones);
		fread(arm->bind_pose.data, sizeof(Bone), bones, f);
		fread(arm->inverse_bind_pose.data, sizeof(Mat4), bones, f);
		for (s32 i = 0; i < arm->inverse_bind_pose.length; i++)
			arm->abs_bind_pose[i] = arm->inverse_bind_pose[i].inverse();

		s32 bodies;
		fread(&bodies, sizeof(s32), 1, f);
		arm->bodies.resize(bodies);
		fread(arm->bodies.data, sizeof(BodyEntry), bodies, f);

		fclose(f);

		armatures[id].type = AssetTransient;
	}
	return &armatures[id].data;
}

const Armature* Loader::armature_permanent(AssetID id)
{
	const Armature* m = armature(id);
	if (m)
		armatures[id].type = AssetPermanent;
	return m;
}

void Loader::armature_free(AssetID id)
{
	if (id != AssetNull && armatures[id].type != AssetNone)
	{
		armatures[id].data.~Armature();
		armatures[id].type = AssetNone;
	}
}

s32 Loader::dynamic_mesh(s32 attribs, b8 dynamic)
{
	s32 index = AssetNull;
	for (s32 i = 0; i < dynamic_meshes.length; i++)
	{
		if (dynamic_meshes[i].type == AssetNone)
		{
			index = static_mesh_count + i;
			break;
		}
	}

	if (index == AssetNull)
	{
		index = static_mesh_count + dynamic_meshes.length;
		dynamic_meshes.add();
	}

	dynamic_meshes[index - static_mesh_count].type = AssetTransient;

#if !SERVER
	RenderSync* sync = swapper->get();
	sync->write(RenderOp::AllocMesh);
	sync->write<AssetID>(index);
	sync->write<b8>(dynamic);
	sync->write<s32>(attribs);
#endif

	return index;
}

// Must be called immediately after dynamic_mesh() or dynamic_mesh_permanent()
void Loader::dynamic_mesh_attrib(RenderDataType type, s32 count)
{
#if !SERVER
	RenderSync* sync = swapper->get();
	sync->write(type);
	sync->write(count);
#endif
}

s32 Loader::dynamic_mesh_permanent(s32 attribs, b8 dynamic)
{
	s32 result = dynamic_mesh(attribs, dynamic);
	dynamic_meshes[result - static_mesh_count].type = AssetPermanent;
	return result;
}

void Loader::dynamic_mesh_free(s32 id)
{
	if (id != AssetNull && dynamic_meshes[id - static_mesh_count].type != AssetNone)
	{
#if !SERVER
		RenderSync* sync = swapper->get();
		sync->write(RenderOp::FreeMesh);
		sync->write<AssetID>(id);
#endif
		dynamic_meshes[id - static_mesh_count].type = AssetNone;
	}
}

const Animation* Loader::animation(AssetID id)
{
	if (id == AssetNull)
		return 0;

	if (id >= animations.length)
		animations.resize(id + 1);
	if (animations[id].type == AssetNone)
	{
		const char* path = AssetLookup::Animation::values[id];
		FILE* f = fopen(path, "rb");
		if (!f)
		{
			fprintf(stderr, "Can't open anm file '%s'\n", path);
			return 0;
		}

		Animation* anim = &animations[id].data;
		new (anim)Animation();

		fread(&anim->duration, sizeof(r32), 1, f);

		s32 channel_count;
		fread(&channel_count, sizeof(s32), 1, f);
		anim->channels.reserve(channel_count);
		anim->channels.length = channel_count;

		for (s32 i = 0; i < channel_count; i++)
		{
			Channel* channel = &anim->channels[i];
			fread(&channel->bone_index, sizeof(s32), 1, f);
			s32 position_count;
			fread(&position_count, sizeof(s32), 1, f);
			channel->positions.reserve(position_count);
			channel->positions.length = position_count;
			fread(channel->positions.data, sizeof(Keyframe<Vec3>), position_count, f);

			s32 rotation_count;
			fread(&rotation_count, sizeof(s32), 1, f);
			channel->rotations.reserve(rotation_count);
			channel->rotations.length = rotation_count;
			fread(channel->rotations.data, sizeof(Keyframe<Quat>), rotation_count, f);

			s32 scale_count;
			fread(&scale_count, sizeof(s32), 1, f);
			channel->scales.reserve(scale_count);
			channel->scales.length = scale_count;
			fread(channel->scales.data, sizeof(Keyframe<Vec3>), scale_count, f);
		}

		fclose(f);

		animations[id].type = AssetTransient;
	}
	return &animations[id].data;
}

const Animation* Loader::animation_permanent(AssetID id)
{
	const Animation* anim = animation(id);
	if (anim)
		animations[id].type = AssetPermanent;
	return anim;
}

void Loader::animation_free(AssetID id)
{
	if (id != AssetNull && animations[id].type != AssetNone)
	{
		animations[id].data.~Animation();
		animations[id].type = AssetNone;
	}
}

void Loader::texture(AssetID id, RenderTextureWrap wrap, RenderTextureFilter filter)
{
#if !SERVER
	if (id == AssetNull || id >= static_texture_count)
		return;

	if (id >= textures.length)
		textures.resize(id + 1);
	if (textures[id].type == AssetNone)
	{
		textures[id].type = AssetTransient;

		const char* path = AssetLookup::Texture::values[id];
		u8* buffer;
		u32 width, height;

		u32 error = lodepng_decode32_file(&buffer, &width, &height, path);

		if (error)
		{
			fprintf(stderr, "Error loading texture '%s': %s\n", path, lodepng_error_text(error));
			vi_assert(false);
			return;
		}

		RenderSync* sync = swapper->get();
		sync->write(RenderOp::AllocTexture);
		sync->write<AssetID>(id);
		sync->write(RenderOp::LoadTexture);
		sync->write<AssetID>(id);
		sync->write(wrap);
		sync->write(filter);
		sync->write<s32>(width);
		sync->write<s32>(height);
		sync->write<u32>((u32*)buffer, width * height);
		free(buffer);
	}
#endif
}

void Loader::texture_permanent(AssetID id, RenderTextureWrap wrap, RenderTextureFilter filter)
{
	texture(id);
	if (id != AssetNull)
		textures[id].type = AssetPermanent;
}

void Loader::texture_free(AssetID id)
{
	if (id != AssetNull && textures[id].type != AssetNone)
	{
#if !SERVER
		RenderSync* sync = swapper->get();
		sync->write(RenderOp::FreeTexture);
		sync->write<AssetID>(id);
#endif
		textures[id].type = AssetNone;
	}
}

AssetID Loader::dynamic_texture(s32 width, s32 height, RenderDynamicTextureType type, RenderTextureWrap wrap, RenderTextureFilter filter, RenderTextureCompare compare)
{
	s32 index = AssetNull;
	for (s32 i = 0; i < dynamic_textures.length; i++)
	{
		if (dynamic_textures[i].type == AssetNone)
		{
			index = static_texture_count + i;
			break;
		}
	}

	if (index == AssetNull)
	{
		index = static_texture_count + dynamic_textures.length;
		dynamic_textures.add();
	}

	dynamic_textures[index - static_texture_count].type = AssetTransient;

#if !SERVER
	RenderSync* sync = swapper->get();
	sync->write(RenderOp::AllocTexture);
	sync->write<AssetID>(index);
	if (width > 0 && height > 0)
		dynamic_texture_redefine(index, width, height, type, wrap, filter, compare);
#endif

	return index;
}

void Loader::dynamic_texture_redefine(AssetID id, s32 width, s32 height, RenderDynamicTextureType type, RenderTextureWrap wrap, RenderTextureFilter filter, RenderTextureCompare compare)
{
#if !SERVER
	RenderSync* sync = swapper->get();
	sync->write(RenderOp::DynamicTexture);
	sync->write<AssetID>(id);
	sync->write<s32>(width);
	sync->write<s32>(height);
	sync->write<RenderDynamicTextureType>(type);
	sync->write<RenderTextureWrap>(wrap);
	sync->write<RenderTextureFilter>(filter);
	sync->write<RenderTextureCompare>(compare);
#endif
}

AssetID Loader::dynamic_texture_permanent(s32 width, s32 height, RenderDynamicTextureType type, RenderTextureWrap wrap, RenderTextureFilter filter, RenderTextureCompare compare)
{
	AssetID id = dynamic_texture(width, height, type, wrap, filter, compare);
	if (id != AssetNull)
		dynamic_textures[id - static_texture_count].type = AssetPermanent;
	return id;
}

void Loader::dynamic_texture_free(AssetID id)
{
	if (id != AssetNull && dynamic_textures[id - static_texture_count].type != AssetNone)
	{
#if !SERVER
		RenderSync* sync = swapper->get();
		sync->write(RenderOp::FreeTexture);
		sync->write<AssetID>(id);
#endif
		dynamic_textures[id - static_texture_count].type = AssetNone;
	}
}

AssetID Loader::framebuffer(s32 attachments)
{
	s32 index = AssetNull;
	for (s32 i = 0; i < framebuffers.length; i++)
	{
		if (framebuffers[i].type == AssetNone)
		{
			index = i;
			break;
		}
	}

	if (index == AssetNull)
	{
		index = framebuffers.length;
		framebuffers.add();
	}

	framebuffers[index].type = AssetTransient;

#if !SERVER
	RenderSync* sync = swapper->get();
	sync->write(RenderOp::AllocFramebuffer);
	sync->write<AssetID>(index);
	sync->write<s32>(attachments);
#endif

	return index;
}

// Must be called immediately after framebuffer() or framebuffer_permanent()
void Loader::framebuffer_attach(RenderFramebufferAttachment attachment_type, AssetID dynamic_texture)
{
#if !SERVER
	RenderSync* sync = swapper->get();
	sync->write<RenderFramebufferAttachment>(attachment_type);
	sync->write<AssetID>(dynamic_texture);
#endif
}

AssetID Loader::framebuffer_permanent(s32 attachments)
{
	s32 id = framebuffer(attachments);
	if (id != AssetNull)
		framebuffers[id].type = AssetPermanent;
	return id;
}

void Loader::framebuffer_free(AssetID id)
{
	if (id != AssetNull && framebuffers[id].type != AssetNone)
	{
#if !SERVER
		RenderSync* sync = swapper->get();
		sync->write(RenderOp::FreeFramebuffer);
		sync->write<AssetID>(id);
#endif
		framebuffers[id].type = AssetNone;
	}
}

void Loader::shader(AssetID id)
{
	if (id == AssetNull || id >= shader_count)
		return;

	if (id >= shaders.length)
		shaders.resize(id + 1);
	if (shaders[id].type == AssetNone)
	{
		shaders[id].type = AssetTransient;

		const char* path = AssetLookup::Shader::values[id];

		Array<char> code;
		FILE* f = fopen(path, "r");
		if (!f)
		{
			fprintf(stderr, "Can't open shader source file '%s'", path);
			return;
		}

		const s32 chunk_size = 4096;
		s32 i = 1;
		while (true)
		{
			code.reserve(i * chunk_size + 1); // extra char since this will be a null-terminated string
			s32 read = s32(fread(&code.data[(i - 1) * chunk_size], sizeof(char), chunk_size, f));
			if (read < chunk_size)
			{
				code.length = ((i - 1) * chunk_size) + read;
				break;
			}
			i++;
		}
		fclose(f);

#if !SERVER
		RenderSync* sync = swapper->get();
		sync->write(RenderOp::LoadShader);
		sync->write<AssetID>(id);
		sync->write<s32>(code.length);
		sync->write(code.data, code.length);
#endif
	}
}

void Loader::shader_permanent(AssetID id)
{
	shader(id);
	if (id != AssetNull)
		shaders[id].type = AssetPermanent;
}

void Loader::shader_free(AssetID id)
{
	if (id != AssetNull && shaders[id].type != AssetNone)
	{
#if !SERVER
		RenderSync* sync = swapper->get();
		sync->write(RenderOp::FreeShader);
		sync->write<AssetID>(id);
#endif
		shaders[id].type = AssetNone;
	}
}

const Font* Loader::font(AssetID id)
{
#if SERVER
	return 0;
#else
	if (id == AssetNull)
		return 0;

	if (id >= fonts.length)
		fonts.resize(id + 1);
	if (fonts[id].type == AssetNone)
	{
		const char* path = AssetLookup::Font::values[id];
		FILE* f = fopen(path, "rb");
		if (!f)
		{
			fprintf(stderr, "Can't open fnt file '%s'\n", path);
			return 0;
		}

		Font* font = &fonts[id].data;
		new (font) Font();

		s32 j;

		fread(&j, sizeof(s32), 1, f);
		font->vertices.resize(j);
		fread(font->vertices.data, sizeof(Vec3), font->vertices.length, f);

		fread(&j, sizeof(s32), 1, f);
		font->indices.resize(j);
		fread(font->indices.data, sizeof(s32), font->indices.length, f);

		fread(&j, sizeof(s32), 1, f);
		for (s32 i = 0; i < j; i++)
		{
			Font::Character c;
			fread(&c, sizeof(Font::Character), 1, f);
			font->characters[c.codepoint] = c;
		}
		{
			Font::Character space;
			space.codepoint = Unicode::codepoint(" ");
			space.max.x = 0.3f;
			font->characters[space.codepoint] = space;
		}
		{
			Font::Character tab;
			tab.codepoint = Unicode::codepoint("\t");
			tab.max.x = 1.5f;
			font->characters[tab.codepoint] = tab;
		}

		fclose(f);

		fonts[id].type = AssetTransient;
	}
	return &fonts[id].data;
#endif
}

const Font* Loader::font_permanent(AssetID id)
{
	const Font* f = font(id);
	if (f)
		fonts[id].type = AssetPermanent;
	return f;
}

void Loader::font_free(AssetID id)
{
#if !SERVER
	if (id != AssetNull && fonts[id].type != AssetNone)
	{
		fonts[id].data.~Font();
		fonts[id].type = AssetNone;
	}
#endif
}

const char* nav_mesh_path(AssetID id)
{
	vi_assert(id != AssetNull);
	if (id < Loader::compiled_level_count)
		return AssetLookup::NavMesh::values[id];
	else
		return mod_nav_paths[id - Loader::compiled_level_count];
}

cJSON* Loader::level(AssetID id)
{
	return Json::load(level_path(id));
}

void Loader::level_free(cJSON* json)
{
	Json::json_free((cJSON*)json);
}

void Loader::nav_mesh(AssetID id, GameType game_type)
{
	if (id == AssetNull)
		AI::load(AssetNull, nullptr);
	else
		AI::load(id, nav_mesh_path(id));
}

void Loader::nav_mesh_free()
{
	AI::load(AssetNull, nullptr);
}

b8 Loader::soundbank(AssetID id)
{
#if SERVER
	return true;
#else
	if (id == AssetNull)
		return false;

	if (id >= soundbanks.length)
		soundbanks.resize(id + 1);
	if (soundbanks[id].type == AssetNone)
	{
		soundbanks[id].type = AssetTransient;

		const char* path = AssetLookup::Soundbank::values[id];

		if (AK::SoundEngine::LoadBank(AssetLookup::Soundbank::values[id], AK_DEFAULT_POOL_ID, soundbanks[id].data) != AK_Success)
		{
			fprintf(stderr, "Failed to load soundbank '%s'\n", path);
			return false;
		}
	}

	return true;
#endif
}

b8 Loader::soundbank_permanent(AssetID id)
{
#if SERVER
	return true;
#else
	b8 success = soundbank(id);
	if (success)
		soundbanks[id].type = AssetPermanent;
	return success;
#endif
}

void Loader::soundbank_free(AssetID id)
{
#if !SERVER
	if (id != AssetNull && soundbanks[id].type != AssetNone)
	{
		soundbanks[id].type = AssetNone;
		AK::SoundEngine::UnloadBank(soundbanks[id].data, nullptr);
	}
#endif
}

void Loader::transients_free()
{
	nav_mesh_free();

	for (AssetID i = 0; i < meshes.length; i++)
	{
		if (meshes[i].type == AssetTransient)
			mesh_free(i);
	}

	for (AssetID i = 0; i < textures.length; i++)
	{
		if (textures[i].type == AssetTransient)
			texture_free(i);
	}

	for (AssetID i = 0; i < shaders.length; i++)
	{
		if (shaders[i].type == AssetTransient)
			shader_free(i);
	}

	for (AssetID i = 0; i < fonts.length; i++)
	{
		if (fonts[i].type == AssetTransient)
			font_free(i);
	}

	for (AssetID i = 0; i < dynamic_meshes.length; i++)
	{
		if (dynamic_meshes[i].type == AssetTransient)
			dynamic_mesh_free(static_mesh_count + i);
	}

	for (s32 i = 0; i < dynamic_textures.length; i++)
	{
		if (dynamic_textures[i].type == AssetTransient)
			dynamic_texture_free(static_texture_count + i);
	}

	for (s32 i = 0; i < framebuffers.length; i++)
	{
		if (framebuffers[i].type == AssetTransient)
			framebuffer_free(i);
	}

#if !SERVER
	for (AssetID i = 0; i < soundbanks.length; i++)
	{
		if (soundbanks[i].type == AssetTransient)
			soundbank_free(i);
	}
#endif
}

AssetID Loader::find(const char* name, const char** list, s32 max_id)
{
	if (!name || !list)
		return AssetNull;
	const char* p;
	s32 i = 0;
	while ((p = list[i]))
	{
		if (max_id >= 0 && i >= max_id)
			break;
		if (strcmp(name, p) == 0)
			return i;
		i++;
	}
	return AssetNull;
}

AssetID Loader::find_level(const char* name)
{
	AssetID result = find(name, AssetLookup::Level::names);
	if (result == AssetNull)
	{
		result = find(name, mod_level_names.data, mod_level_names.length);
		if (result != AssetNull)
			result += compiled_level_count;
	}
	return result;
}

AssetID Loader::find_mesh(const char* name)
{
	AssetID result = find(name, AssetLookup::Mesh::names);
	if (result == AssetNull)
	{
		result = find(name, mod_level_mesh_names.data, mod_level_mesh_names.length);
		if (result != AssetNull)
			result += compiled_static_mesh_count;
	}
	return result;
}

const char* Loader::level_name(AssetID lvl)
{
	vi_assert(lvl != AssetNull);
	if (lvl < compiled_level_count)
		return AssetLookup::Level::names[lvl];
	else
		return mod_level_names[lvl - compiled_level_count];
}

const char* Loader::level_path(AssetID lvl)
{
	vi_assert(lvl != AssetNull);
	if (lvl < compiled_level_count)
		return AssetLookup::Level::values[lvl];
	else
		return mod_level_paths[lvl - compiled_level_count];
}

const char* Loader::mesh_name(AssetID mesh)
{
	vi_assert(mesh != AssetNull);
	if (mesh < compiled_static_mesh_count)
		return AssetLookup::Mesh::names[mesh];
	else
		return mod_level_mesh_names[mesh - compiled_static_mesh_count];
}

const char* Loader::mesh_path(AssetID mesh)
{
	vi_assert(mesh != AssetNull);
	if (mesh < compiled_static_mesh_count)
		return AssetLookup::Mesh::values[mesh];
	else
		return mod_level_mesh_paths[mesh - compiled_static_mesh_count];
}

void Loader::user_data_path(char* path, const char* filename)
{
	vi_assert(strlen(Loader::data_directory) + strlen(filename) <= MAX_PATH_LENGTH);
	sprintf(path, "%s%s", Loader::data_directory, filename);
}


}
