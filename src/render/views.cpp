#include "views.h"
#include "load.h"
#include "asset/shader.h"
#include "asset/mesh.h"
#include "asset/texture.h"
#include "console.h"
#include "data/components.h"
#include "game/team.h"
#include "game/game.h"
#include "asset/Wwise_IDs.h"
#include "game/audio.h"
#include "settings.h"
#include "render/particles.h"

namespace VI
{

Bitmask<MAX_ENTITIES> View::list_alpha;
Bitmask<MAX_ENTITIES> View::list_additive;
#if DEBUG_VIEW
Array<View::DebugEntry> View::debug_entries;
#endif

View::View(AssetID m)
	: mesh(m),
	shader(AssetNull),
	texture(AssetNull),
	offset(Mat4::identity),
	color(-1, -1, -1, -1),
	mask(RENDER_MASK_DEFAULT),
	team(s8(AI::TeamNone)),
	radius()
{
}

View::View()
	: mesh(AssetNull),
	shader(AssetNull),
	texture(AssetNull),
	offset(Mat4::identity),
	color(-1, -1, -1, -1),
	mask(RENDER_MASK_DEFAULT),
	team(s8(AI::TeamNone)),
	radius()
{
}

View::~View()
{
	alpha_disable();
}

void View::draw_opaque(const RenderParams& params)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (!list_alpha.get(i.index) && !list_additive.get(i.index) && (i.item()->mask & params.camera->mask))
			i.item()->draw(params);
	}
}

void View::draw_additive(const RenderParams& params)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (list_additive.get(i.index) && (i.item()->mask & params.camera->mask))
			i.item()->draw(params);
	}
}

void View::draw_alpha(const RenderParams& params)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (list_alpha.get(i.index) && (i.item()->mask & params.camera->mask))
			i.item()->draw(params);
	}

#if DEBUG_VIEW
	Mat4 m;
	for (s32 i = 0; i < debug_entries.length; i++)
	{
		const DebugEntry& entry = debug_entries[i];
		m.make_transform(entry.pos, entry.scale, entry.rot);
		draw_mesh(params, entry.mesh, Asset::Shader::flat, AssetNull, m, entry.color);
	}
#endif
}

void View::draw_filtered(const RenderParams& params, Filter* filter)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (filter(params, i.item()))
			i.item()->draw(params);
	}
}

void View::alpha()
{
	list_alpha.set(id(), true);
	list_additive.set(id(), false);
}

void View::additive()
{
	list_alpha.set(id(), false);
	list_additive.set(id(), true);
}

void View::alpha_disable()
{
	list_alpha.set(id(), false);
	list_additive.set(id(), false);
}

AlphaMode View::alpha_mode() const
{
	if (list_alpha.get(id()))
		return AlphaMode::Alpha;
	else if (list_additive.get(id()))
		return AlphaMode::Additive;
	else
		return AlphaMode::Opaque;
}

void View::alpha_mode(AlphaMode m)
{
	switch (m)
	{
		case AlphaMode::Opaque:
		{
			alpha_disable();
			break;
		}
		case AlphaMode::Alpha:
		{
			alpha();
			break;
		}
		case AlphaMode::Additive:
		{
			additive();
			break;
		}
		default:
		{
			vi_assert(false);
			break;
		}
	}
}

void View::draw_mesh(const RenderParams& params, AssetID mesh, AssetID shader, AssetID texture, const Mat4& m, const Vec4& color, r32 radius)
{
	if (mesh == AssetNull || shader == AssetNull)
		return;

	const Mesh* mesh_data = Loader::mesh(mesh);

	{
		Mat3 scale;
		m.extract_mat3(scale);
		r32 r = radius == 0.0f ? mesh_data->bounds_radius : radius;
		Vec3 r3d = scale * Vec3(r);
		if (!params.camera->visible_sphere(m.translation(), vi_max(r3d.x, vi_max(r3d.y, r3d.z))))
			return;
	}

	Loader::shader(shader);
	Loader::texture(texture);

	RenderSync* sync = params.sync;
	sync->write(RenderOp::Shader);
	sync->write(shader);
	sync->write(params.technique);

	Mat4 mvp = m * params.view_projection;

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::mvp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(mvp);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::mv);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(m * params.view);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec4);
	sync->write<s32>(1);
	sync->write<Vec4>(color);

	if (texture != AssetNull)
	{
		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::diffuse_map);
		sync->write(RenderDataType::Texture);
		sync->write<s32>(1);
		sync->write<RenderTextureType>(RenderTextureType::Texture2D);
		sync->write<AssetID>(texture);
	}

	if (params.flags & RenderFlagEdges)
	{
		sync->write(RenderOp::MeshEdges);
		sync->write(mesh);
	}
	else
	{
		sync->write(RenderOp::Mesh);
		sync->write(RenderPrimitiveMode::Triangles);
		sync->write(mesh);
	}
}

#if DEBUG_VIEW
void View::debug(AssetID mesh, const Vec3& pos, const Quat& rot, const Vec3& scale, const Vec4& color)
{
	DebugEntry* entry = debug_entries.add();
	entry->mesh = mesh;
	entry->pos = pos;
	entry->rot = rot;
	entry->scale = scale;
	entry->color = color;
}
#endif

void View::draw(const RenderParams& params) const
{
	if (mesh == AssetNull || shader == AssetNull)
		return;

	const Mesh* mesh_data = Loader::mesh(mesh);

	Mat4 m;
	get<Transform>()->mat(&m);
	m = offset * m;

	{
		r32 r = radius == 0.0f ? mesh_data->bounds_radius : radius;
		Vec3 r3d = (offset * Vec4(r, r, r, 1)).xyz();
		if (!params.camera->visible_sphere(m.translation(), vi_max(r3d.x, vi_max(r3d.y, r3d.z))))
			return;
	}

	// if allow_culled_shader is false, replace the culled shader with the standard shader.
	b8 allow_culled_shader = params.camera->cull_range > 0.0f && !(params.flags & RenderFlagEdges);
	AssetID shader_actual = allow_culled_shader || shader != Asset::Shader::culled ? shader : Asset::Shader::standard;

	Loader::shader(shader_actual);
	Loader::texture(texture);

	RenderSync* sync = params.sync;
	sync->write(RenderOp::Shader);
	sync->write(shader_actual);
	sync->write(params.technique);

	Mat4 mvp = m * params.view_projection;

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::mvp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(mvp);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::mv);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(m * params.view);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec4);
	sync->write<s32>(1);

	{
		Vec4 color_final;
		if (team == s8(AI::TeamNone))
		{
			if (params.camera->flag(CameraFlagColors) || list_alpha.get(id()) || list_additive.get(id()))
				color_final = color;
			else if (color.w == MATERIAL_INACCESSIBLE || (params.flags & RenderFlagBackFace))
				color_final = PVP_INACCESSIBLE;
			else if (color.w == MATERIAL_NO_OVERRIDE)
				color_final = PVP_ACCESSIBLE_NO_OVERRIDE;
			else
				color_final = PVP_ACCESSIBLE;
		}
		else
		{
			if (params.flags & RenderFlagBackFace)
				color_final = PVP_INACCESSIBLE;
			else if (list_alpha.get(id()) || list_additive.get(id()))
				color_final = Vec4(Team::color_alpha(AI::Team(team), AI::Team(params.camera->team)), color.w);
			else
				color_final = Team::color(AI::Team(team), AI::Team(params.camera->team));
		}
		if (params.flags & RenderFlagAlphaOverride)
			color_final.w = 0.7f;
		sync->write(color_final);
	}

	if (shader_actual == Asset::Shader::culled)
	{
		// write culling info
		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::range_center);
		sync->write(RenderDataType::Vec3);
		sync->write<s32>(1);
		sync->write<Vec3>(params.camera->range_center);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::cull_center);
		sync->write(RenderDataType::Vec3);
		sync->write<s32>(1);
		sync->write<Vec3>(params.camera->cull_center);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::cull_radius);
		sync->write(RenderDataType::R32);
		sync->write<s32>(1);
		sync->write<r32>(params.camera->cull_range);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::wall_normal);
		sync->write(RenderDataType::Vec3);
		sync->write<s32>(1);
		sync->write<Vec3>(params.camera->clip_planes[0].normal);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::cull_behind_wall);
		sync->write(RenderDataType::S32);
		sync->write<s32>(1);
		sync->write<s32>(params.camera->flag(CameraFlagCullBehindWall));

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::frontface);
		sync->write(RenderDataType::S32);
		sync->write<s32>(1);
		sync->write<s32>(!(params.flags & RenderFlagBackFace));
	}

	if (texture != AssetNull)
	{
		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::diffuse_map);
		sync->write(RenderDataType::Texture);
		sync->write<s32>(1);
		sync->write(RenderTextureType::Texture2D);
		sync->write<AssetID>(texture);
	}

	if (params.flags & RenderFlagEdges)
	{
		sync->write(RenderOp::MeshEdges);
		sync->write(mesh);
	}
	else
	{
		sync->write(RenderOp::Mesh);
		sync->write(RenderPrimitiveMode::Triangles);
		sync->write(mesh);
	}
}

void View::awake()
{
	const Mesh* m = Loader::mesh(mesh);
	if (m)
	{
		if (color.x < 0.0f)
			color.x = m->color.x;
		if (color.y < 0.0f)
			color.y = m->color.y;
		if (color.z < 0.0f)
			color.z = m->color.z;
		if (color.w < 0.0f)
			color.w = m->color.w;
	}
}

void SkyDecals::draw_alpha(const RenderParams& p)
{
	RenderSync* sync = p.sync;

	Loader::shader_permanent(Asset::Shader::sky_decal);

	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::sky_decal);
	sync->write(p.technique);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::p);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(p.camera->projection);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::frustum);
	sync->write(RenderDataType::Vec3);
	sync->write<s32>(4);
	sync->write<Vec3>(p.camera->frustum_rays, 4);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::fog_start);
	sync->write(RenderDataType::R32);
	sync->write<s32>(1);
	sync->write<r32>(Game::level.fog_start_get());

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::fog_extent);
	sync->write(RenderDataType::R32);
	sync->write<s32>(1);
	sync->write<r32>(Game::level.fog_end_get() - Game::level.fog_start_get());

	Vec2 inv_buffer_size = Vec2(1.0f / r32(Settings::display().width), 1.0f / r32(Settings::display().height));

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::uv_offset);
	sync->write(RenderDataType::Vec2);
	sync->write<s32>(1);
	sync->write<Vec2>(p.camera->viewport.pos * inv_buffer_size);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::uv_scale);
	sync->write(RenderDataType::Vec2);
	sync->write<s32>(1);
	sync->write<Vec2>(p.camera->viewport.size * inv_buffer_size);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::depth_buffer);
	sync->write(RenderDataType::Texture);
	sync->write<s32>(1);
	sync->write<RenderTextureType>(RenderTextureType::Texture2D);
	sync->write<AssetID>(p.depth_buffer);

	sync->write(RenderOp::DepthTest);
	sync->write(false);

	Loader::mesh_permanent(Asset::Mesh::sky_decal);
	for (s32 i = 0; i < Game::level.sky_decals.length; i++)
	{
		const Config& config = Game::level.sky_decals[i];

		Loader::texture(config.texture);

		Mat4 m;
		m.make_transform(config.rot * Vec3(0, 0, 1), Vec3(config.scale), config.rot);
		Mat4 v = p.view;
		v.translation(Vec3::zero);

		Mat4 mvp = m * (v * p.camera->projection);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::mvp);
		sync->write(RenderDataType::Mat4);
		sync->write<s32>(1);
		sync->write<Mat4>(mvp);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::diffuse_color);
		sync->write(RenderDataType::Vec4);
		sync->write<s32>(1);
		if (p.camera->flag(CameraFlagColors))
			sync->write<Vec4>(config.color);
		else
			sync->write<Vec4>(LMath::desaturate(config.color));

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::diffuse_map);
		sync->write(RenderDataType::Texture);
		sync->write<s32>(1);
		sync->write<RenderTextureType>(RenderTextureType::Texture2D);
		sync->write<AssetID>(config.texture);

		sync->write(RenderOp::Mesh);
		sync->write(RenderPrimitiveMode::Triangles);
		sync->write(Asset::Mesh::sky_decal);
	}

	sync->write(RenderOp::DepthTest);
	sync->write(true);
}

b8 Skybox::Config::valid() const
{
	return shader != AssetNull && mesh != AssetNull;
}

void Skybox::draw_alpha(const RenderParams& p)
{
	if (Game::level.skybox.mesh == AssetNull || p.technique != RenderTechnique::Default)
		return;

	Loader::shader_permanent(Game::level.skybox.shader);
	Loader::mesh_permanent(Game::level.skybox.mesh);
	Loader::texture(Game::level.skybox.texture);

	RenderSync* sync = p.sync;

	sync->write<RenderOp>(RenderOp::DepthTest);
	sync->write(false);

	sync->write(RenderOp::Shader);
	sync->write(Game::level.skybox.shader);

	b8 volumetric_lighting = p.shadow_buffer != AssetNull && p.camera->flag(CameraFlagFog);

	if (volumetric_lighting)
		sync->write(RenderTechnique::Shadow);
	else
		sync->write(RenderTechnique::Default);

	Mat4 mvp = p.view * Mat4::make_scale(Vec3(p.camera->far_plane));
	mvp.translation(Vec3::zero);
	mvp = mvp * p.camera->projection;

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::mvp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(mvp);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec3);
	sync->write<s32>(1);
	if (p.camera->flag(CameraFlagColors))
		sync->write<Vec3>(Game::level.skybox.color);
	else
		sync->write<Vec3>(LMath::desaturate(Game::level.skybox.color));

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::p);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(p.camera->projection);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::frustum);
	sync->write(RenderDataType::Vec3);
	sync->write<s32>(4);
	sync->write<Vec3>(p.camera->frustum_rays, 4);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::fog_start);
	sync->write(RenderDataType::R32);
	sync->write<s32>(1);
	sync->write<r32>(Game::level.fog_start_get());

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::fog_extent);
	sync->write(RenderDataType::R32);
	sync->write<s32>(1);
	sync->write<r32>(Game::level.fog_end_get() - Game::level.fog_start_get());

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::far_plane);
	sync->write(RenderDataType::R32);
	sync->write<s32>(1);
	sync->write<r32>(p.camera->far_plane);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::fog);
	sync->write(RenderDataType::S32);
	sync->write<s32>(1);
	sync->write<s32>(p.camera->flag(CameraFlagFog));

	Vec2 inv_buffer_size = Vec2(1.0f / r32(Settings::display().width), 1.0f / r32(Settings::display().height));

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::uv_offset);
	sync->write(RenderDataType::Vec2);
	sync->write<s32>(1);
	sync->write<Vec2>(p.camera->viewport.pos * inv_buffer_size);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::uv_scale);
	sync->write(RenderDataType::Vec2);
	sync->write<s32>(1);
	sync->write<Vec2>(p.camera->viewport.size * inv_buffer_size);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::depth_buffer);
	sync->write(RenderDataType::Texture);
	sync->write<s32>(1);
	sync->write<RenderTextureType>(RenderTextureType::Texture2D);
	sync->write<AssetID>(p.depth_buffer);

	if (volumetric_lighting)
	{
		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::shadow_map);
		sync->write(RenderDataType::Texture);
		sync->write<s32>(1);
		sync->write<RenderTextureType>(RenderTextureType::Texture2D);
		sync->write<AssetID>(p.shadow_buffer);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::light_vp);
		sync->write(RenderDataType::Mat4);
		sync->write<s32>(1);
		Mat4 view_rotation = p.view;
		view_rotation.translation(Vec3::zero);
		sync->write<Mat4>(view_rotation.inverse() * p.shadow_vp);

		Loader::texture_permanent(Asset::Texture::noise, RenderTextureWrap::Repeat, RenderTextureFilter::Nearest);
		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::noise_sampler);
		sync->write(RenderDataType::Texture);
		sync->write<s32>(1);
		sync->write<RenderTextureType>(RenderTextureType::Texture2D);
		sync->write<AssetID>(Asset::Texture::noise);
	}

	if (Game::level.skybox.texture != AssetNull)
	{
		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::diffuse_map);
		sync->write(RenderDataType::Texture);
		sync->write<s32>(1);
		sync->write<RenderTextureType>(RenderTextureType::Texture2D);
		sync->write<AssetID>(Game::level.skybox.texture);
	}

	sync->write(RenderOp::Mesh);
	sync->write(RenderPrimitiveMode::Triangles);
	sync->write<AssetID>(Game::level.skybox.mesh);

	sync->write<RenderOp>(RenderOp::DepthTest);
	sync->write(true);
}

void Clouds::draw_alpha(const RenderParams& p)
{
	if (Game::level.clouds.length == 0 || p.technique != RenderTechnique::Default)
		return;

	Loader::shader_permanent(Asset::Shader::clouds);
	Loader::mesh_permanent(Asset::Mesh::clouds);
	Loader::texture_permanent(Asset::Texture::clouds);

	RenderSync* sync = p.sync;

	sync->write<RenderOp>(RenderOp::DepthTest);
	sync->write<b8>(false);

	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::clouds);
	sync->write(p.technique);

	for (s32 i = 0; i < Game::level.clouds.length; i++)
	{
		const Config& config = Game::level.clouds[i];

		Mat4 mvp = p.view * Mat4::make_scale(Vec3(p.camera->far_plane));
		mvp.translation(p.camera->rot.inverse() * Vec3(0, config.height - p.camera->pos.y, 0));
		mvp = mvp * p.camera->projection;

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::mvp);
		sync->write(RenderDataType::Mat4);
		sync->write<s32>(1);
		sync->write<Mat4>(mvp);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::diffuse_color);
		sync->write(RenderDataType::Vec4);
		sync->write<s32>(1);
		if (p.camera->flag(CameraFlagColors))
			sync->write<Vec4>(config.color);
		else
			sync->write<Vec4>(LMath::desaturate(config.color));

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::p);
		sync->write(RenderDataType::Mat4);
		sync->write<s32>(1);
		sync->write<Mat4>(p.camera->projection);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::frustum);
		sync->write(RenderDataType::Vec3);
		sync->write<s32>(4);
		sync->write<Vec3>(p.camera->frustum_rays, 4);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::fog_start);
		sync->write(RenderDataType::R32);
		sync->write<s32>(1);
		sync->write<r32>(Game::level.fog_start_get());

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::fog_extent);
		sync->write(RenderDataType::R32);
		sync->write<s32>(1);
		sync->write<r32>(Game::level.fog_end_get() - Game::level.fog_start_get());

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::cloud_inv_uv_scale);
		sync->write(RenderDataType::R32);
		sync->write<s32>(1);
		sync->write<r32>(1.0f / config.scale);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::cloud_uv_offset);
		sync->write(RenderDataType::Vec2);
		sync->write<s32>(1);
		sync->write<Vec2>(Vec2(p.camera->pos.z * 0.5f, p.camera->pos.x * -0.5f) * (1.0f / p.camera->far_plane) + config.uv_offset(p));

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::cloud_height_diff_scaled);
		sync->write(RenderDataType::R32);
		sync->write<s32>(1);
		sync->write<r32>((config.height - p.camera->pos.y) / p.camera->far_plane);

		Vec2 inv_buffer_size = Vec2(1.0f / r32(Settings::display().width), 1.0f / r32(Settings::display().height));

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::uv_offset);
		sync->write(RenderDataType::Vec2);
		sync->write<s32>(1);
		sync->write<Vec2>(p.camera->viewport.pos * inv_buffer_size);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::uv_scale);
		sync->write(RenderDataType::Vec2);
		sync->write<s32>(1);
		sync->write<Vec2>(p.camera->viewport.size * inv_buffer_size);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::depth_buffer);
		sync->write(RenderDataType::Texture);
		sync->write<s32>(1);
		sync->write<RenderTextureType>(RenderTextureType::Texture2D);
		sync->write<AssetID>(p.depth_buffer);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::cloud_map);
		sync->write(RenderDataType::Texture);
		sync->write<s32>(1);
		sync->write<RenderTextureType>(RenderTextureType::Texture2D);
		sync->write<AssetID>(Asset::Texture::clouds);

		sync->write(RenderOp::Mesh);
		sync->write(RenderPrimitiveMode::Triangles);
		sync->write<AssetID>(Asset::Mesh::clouds);
	}

	sync->write<RenderOp>(RenderOp::DepthTest);
	sync->write<b8>(true);
}

Vec2 Clouds::Config::uv_offset(const RenderParams& p) const
{
	return Vec2(velocity * (Game::time.total * 0.05f));
}

void SkyPattern::draw_opaque(const RenderParams& p)
{
	if (p.technique != RenderTechnique::Default)
		return;

	Loader::shader_permanent(Asset::Shader::standard_flat);
	Loader::mesh_permanent(Asset::Mesh::sky_pattern);

	RenderSync* sync = p.sync;

	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::standard_flat);
	sync->write(p.technique);

	Mat4 mvp = p.view * Mat4::make_scale(Vec3(p.camera->far_plane * 0.99f));
	mvp.translation(Vec3::zero);
	mvp = mvp * p.camera->projection;

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec4);
	sync->write<s32>(1);
	sync->write<Vec4>(Vec4(0, 0, 0, 1));

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::mvp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(mvp);

	sync->write(RenderOp::Mesh);
	sync->write(RenderPrimitiveMode::Triangles);
	sync->write(Asset::Mesh::sky_pattern);
}

void SkyPattern::draw_hollow(const RenderParams& p)
{
	if (!(p.flags & RenderFlagEdges))
		return;

	Loader::shader_permanent(Asset::Shader::flat);
	Loader::mesh_permanent(Asset::Mesh::sky_pattern);

	RenderSync* sync = p.sync;

	sync->write<RenderOp>(RenderOp::FillMode);
	sync->write(RenderFillMode::Point);

	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::flat);
	sync->write(p.technique);

	Mat4 mvp = p.view * Mat4::make_scale(Vec3(p.camera->far_plane * 0.95f));
	mvp.translation(Vec3::zero);
	mvp = mvp * p.camera->projection;

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::mvp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(mvp);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec4);
	sync->write<s32>(1);
	sync->write<Vec4>(Vec4(1, 1, 1, 1));

	sync->write(RenderOp::Mesh);
	sync->write(RenderPrimitiveMode::Triangles);
	sync->write(Asset::Mesh::sky_pattern);

	sync->write<RenderOp>(RenderOp::FillMode);
	sync->write(RenderFillMode::Fill);
}

Water::Config::Config(AssetID mesh_id)
	: mesh(mesh_id),
	color(-1, -1, -1, -1),
	displacement_horizontal(2.0f),
	displacement_vertical(0.75f),
	texture(Asset::Texture::water_normal),
	ocean()
{
}

Water::Water(AssetID mesh_id)
	: config(mesh_id),
	mask(RENDER_MASK_DEFAULT)
{
}

Water* Water::underwater(const Vec3& pos)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->contains(pos))
			return i.item();
	}
	return nullptr;
}

b8 Water::contains(const Vec3& pos) const
{
	const Mesh* m = Loader::mesh(config.mesh);
	Vec3 water_pos = get<Transform>()->absolute_pos();
	Vec3 bmin = water_pos + m->bounds_min;
	Vec3 bmax = water_pos + m->bounds_max;
	return pos.x > bmin.x && pos.z > bmin.z
		&& pos.x < bmax.x && pos.y < bmax.y && pos.z < bmax.z;
}

void Water::awake()
{
	const Mesh* m = Loader::mesh(config.mesh);
	if (m)
	{
		if (config.color.x < 0.0f)
			config.color.x = m->color.x;
		if (config.color.y < 0.0f)
			config.color.y = m->color.y;
		if (config.color.z < 0.0f)
			config.color.z = m->color.z;
		if (config.color.w < 0.0f)
			config.color.w = m->color.w;
	}

	for (s32 i = 0; i < MAX_GAMEPADS; i++)
	{
		s32 flags = AudioEntry::FlagEnableReverb | AudioEntry::FlagKeepalive;
		if (!config.ocean)
			flags |= AudioEntry::FlagEnableForceFieldObstruction | AudioEntry::FlagEnableObstructionOcclusion;
		AudioEntry* entry = Audio::post_global(config.ocean ? AK::EVENTS::PLAY_OCEAN_LOOP : AK::EVENTS::PLAY_WATER_LOOP, Vec3::zero, nullptr, flags); // no obstruction/occlusion
		entry->set_listener_mask(1 << i);
		audio_entries[i] = entry;
	}
}

void Water::update_all(const Update& u)
{
	if (Audio::listener_mask)
	{
		struct Listener
		{
			Vec3 pos;
			r32 outdoor;
			s32 index;
		};

		StaticArray<Listener, MAX_GAMEPADS> listeners;

		// collect listeners
		for (s32 i = 0; i < MAX_GAMEPADS; i++)
		{
			if (Audio::listener_mask & (1 << i))
			{
				const Audio::Listener& listener = Audio::listener[i];
				Vec3 p = listener.pos;

				// make sure listener is not in a negative space
				for (s32 j = 0; j < Game::level.water_sound_negative_spaces.length; j++)
				{
					const Game::WaterSoundNegativeSpace& space = Game::level.water_sound_negative_spaces[j];
					Vec3 diff = p - space.pos;
					diff.y = 0.0f;
					r32 distance = diff.length();
					if (distance < space.radius)
					{
						r32 original_y = p.y;
						p = space.pos + diff * (space.radius / distance);
						p.y = original_y;
					}
				}
				Listener* l = listeners.add();
				l->pos = p;
				l->outdoor = listener.outdoor;
				l->index = i;
			}
		}

		// find closest position for each water sound
		for (auto i = list.iterator(); !i.is_last(); i.next())
		{
			const Mesh* m = Loader::mesh(i.item()->config.mesh);
			Vec3 water_pos = i.item()->get<Transform>()->absolute_pos();
			Vec3 bmin = water_pos + m->bounds_min;
			Vec3 bmax = water_pos + m->bounds_max;

			for (s32 j = 0; j < listeners.length; j++)
			{
				const Listener& listener = listeners[j];
				Vec3 p = listener.pos;
				p.y = water_pos.y;
				p.x = vi_max(p.x, bmin.x);
				p.x = vi_min(p.x, bmax.x);
				p.z = vi_max(p.z, bmin.z);
				p.z = vi_min(p.z, bmax.z);
				AudioEntry* audio_entry = i.item()->audio_entries[listener.index].ref();
				audio_entry->abs_pos = p;
				audio_entry->param(AK::GAME_PARAMETERS::AMBIENCE_INDOOR_OUTDOOR, listener.outdoor);
			}
		}
	}
}

void Water::draw_opaque(const RenderParams& params, const Config& cfg, const Vec3& pos, const Quat& rot)
{
	if (params.technique != RenderTechnique::Default)
		return;

	const Mesh* mesh_data = Loader::mesh(cfg.mesh);

	Mat4 m;
	m.make_transform(pos, Vec3(1), rot);

	if (!params.camera->visible_sphere(m.translation(), mesh_data->bounds_radius))
		return;

	Loader::shader_permanent(Asset::Shader::water);
	Loader::texture(cfg.texture);

	RenderSync* sync = params.sync;
	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::water);
	sync->write(params.technique);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::mvp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(m * params.view_projection);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::mv);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(m * params.view);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::time);
	sync->write(RenderDataType::R32);
	sync->write<s32>(1);
	sync->write<r32>(ParticleSystem::time);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::displacement);
	sync->write(RenderDataType::Vec3);
	sync->write<s32>(1);
	sync->write<Vec3>(Vec3(cfg.displacement_horizontal, cfg.displacement_vertical, cfg.displacement_horizontal));

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::normal_map);
	sync->write(RenderDataType::Texture);
	sync->write<s32>(1);
	sync->write<RenderTextureType>(RenderTextureType::Texture2D);
	sync->write<AssetID>(cfg.texture);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec4);
	sync->write<s32>(1);
	if (params.camera->flag(CameraFlagColors))
		sync->write<Vec4>(cfg.color);
	else
		sync->write<Vec4>(PVP_INACCESSIBLE);

	sync->write(RenderOp::Mesh);
	sync->write(RenderPrimitiveMode::Triangles);
	sync->write(cfg.mesh);
}

void Water::draw_hollow(const RenderParams& params, const Config& cfg, const Vec3& pos, const Quat& rot)
{
	const Mesh* mesh_data = Loader::mesh(cfg.mesh);

	Mat4 m;
	m.make_transform(pos, Vec3(1), rot);


	if (!params.camera->visible_sphere(pos, mesh_data->bounds_radius))
		return;

	Loader::shader_permanent(Asset::Shader::water);
	Loader::texture(cfg.texture);

	RenderSync* sync = params.sync;
	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::water);
	sync->write(params.technique);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::mvp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(m * params.view_projection);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::mv);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(m * params.view);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::time);
	sync->write(RenderDataType::R32);
	sync->write<s32>(1);
	sync->write<r32>(ParticleSystem::time);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::displacement);
	sync->write(RenderDataType::Vec3);
	sync->write<s32>(1);
	sync->write<Vec3>(Vec3(cfg.displacement_horizontal, cfg.displacement_vertical, cfg.displacement_horizontal));

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::normal_map);
	sync->write(RenderDataType::Texture);
	sync->write<s32>(1);
	sync->write<RenderTextureType>(RenderTextureType::Texture2D);
	sync->write<AssetID>(cfg.texture);

	sync->write(RenderOp::FillMode);
	sync->write(RenderFillMode::Point);

	sync->write(RenderOp::Mesh);
	sync->write(RenderPrimitiveMode::Points);
	sync->write(cfg.mesh);

	sync->write(RenderOp::FillMode);
	sync->write(RenderFillMode::Fill);
}

void Water::draw_alpha_late(const RenderParams& p)
{
	if (p.technique != RenderTechnique::Default)
		return;

	Water* w = underwater(p.camera->pos);
	if (w)
	{
		Loader::shader_permanent(Asset::Shader::underwater);

		RenderSync* sync = p.sync;

		sync->write(RenderOp::Shader);
		sync->write(Asset::Shader::underwater);
		sync->write(p.technique);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::diffuse_color);
		sync->write(RenderDataType::Vec3);
		sync->write<s32>(1);
		sync->write<Vec3>(p.camera->flag(CameraFlagColors) ? w->config.color.xyz() : Vec3(0.0f));

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::p);
		sync->write(RenderDataType::Mat4);
		sync->write<s32>(1);
		sync->write<Mat4>(p.camera->projection);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::depth_buffer);
		sync->write(RenderDataType::Texture);
		sync->write<s32>(1);
		sync->write<RenderTextureType>(RenderTextureType::Texture2D);
		sync->write<AssetID>(p.depth_buffer);

		sync->write(RenderOp::Mesh);
		sync->write(RenderPrimitiveMode::Triangles);
		sync->write<AssetID>(Game::screen_quad.mesh);
	}
}

void Water::draw_opaque(const RenderParams& params)
{
	if (params.technique == RenderTechnique::Default && list.count() > 0)
	{
		params.sync->write(RenderOp::CullMode);
		params.sync->write(RenderCullMode::None);
		for (auto i = list.iterator(); !i.is_last(); i.next())
		{
			if (i.item()->mask & params.camera->mask)
			{
				Vec3 pos;
				Quat rot;
				i.item()->get<Transform>()->absolute(&pos, &rot);
				draw_opaque(params, i.item()->config, pos, rot);
			}
		}
		params.sync->write(RenderOp::CullMode);
		params.sync->write(RenderCullMode::Back);
	}
}

void Water::draw_hollow(const RenderParams& params)
{
	if (mask & params.camera->mask)
	{
		Vec3 pos;
		Quat rot;
		get<Transform>()->absolute(&pos, &rot);
		draw_hollow(params, config, pos, rot);
	}
}

ScreenQuad::ScreenQuad()
	: mesh(AssetNull)
{
}

void ScreenQuad::init(RenderSync* sync)
{
	mesh = Loader::dynamic_mesh_permanent(3);
	Loader::dynamic_mesh_attrib(RenderDataType::Vec3);
	Loader::dynamic_mesh_attrib(RenderDataType::Vec3);
	Loader::dynamic_mesh_attrib(RenderDataType::Vec2);

	s32 indices[] =
	{
		0,
		1,
		2,
		1,
		3,
		2
	};

	sync->write(RenderOp::UpdateIndexBuffer);
	sync->write(mesh);
	sync->write<s32>(6);
	sync->write(indices, 6);
}

void ScreenQuad::set(RenderSync* sync, const Rect2& r, const Camera* camera, const Rect2& uv)
{
	Vec3 vertices[] =
	{
		Vec3(r.pos.x, r.pos.y, 0),
		Vec3(r.pos.x + r.size.x, r.pos.y, 0),
		Vec3(r.pos.x, r.pos.y + r.size.y, 0),
		Vec3(r.pos.x + r.size.x, r.pos.y + r.size.y, 0),
	};

	Vec2 uvs[] =
	{
		Vec2(uv.pos.x, uv.pos.y),
		Vec2(uv.pos.x + uv.size.x, uv.pos.y),
		Vec2(uv.pos.x, uv.pos.y + uv.size.y),
		Vec2(uv.pos.x + uv.size.x, uv.pos.y + uv.size.y),
	};

	sync->write(RenderOp::UpdateAttribBuffers);
	sync->write(mesh);
	sync->write<s32>(4);
	sync->write(vertices, 4);
	sync->write(camera->frustum_rays, 4);
	sync->write(uvs, 4);
}

}
