#pragma once

#include "data/entity.h"
#include "data/components.h"
#include "render.h"
#include "data/mesh.h"

namespace VI
{

struct Armature : public ComponentType<Armature>
{
	AssetID mesh;
	AssetID shader;
	AssetID texture;
	Animation* animation;
	Array<Mat4> bones;
	Array<Mat4> skin_transforms;
	Mat4 offset;
	float time;

	void draw(const RenderParams&);
	void update(const Update&);
	void update_world_transforms();
	void awake();
	Armature();
};

}
