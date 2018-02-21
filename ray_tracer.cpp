#include "DXUT.h"
#include "ray_tracer.hpp"
#include "structs.hpp"
#include <algorithm>    // std::sort


RayTracer::RayTracer(): pd3dImmediateContext_(nullptr)
{
}


RayTracer::~RayTracer()
{
}


void RayTracer::loadScene()
{
	scene.loadObj("scene01.obj");


	renderer_primitives.resize(scene.triangle_count);
	ray_primitives.resize(scene.triangle_count);

	primitive_count = 0;
	for (auto&& geo : scene.geos)
	{
		for (int pg_index = 0; pg_index < geo.indicess.size(); ++pg_index)
		{
			const auto& indices = geo.indicess[pg_index];
			for (int triangle_index = 0; triangle_index < indices.size() / 3; ++triangle_index)
			{
				PrimitiveRender& prd = renderer_primitives[primitive_count + triangle_index];
				prd.matid = geo.material_ids[pg_index];
				prd.pad = -1;
				prd.vertices[0] = geo.vertices_xyzn[indices[triangle_index * 3 + 0]];
				prd.vertices[1] = geo.vertices_xyzn[indices[triangle_index * 3 + 1]];
				prd.vertices[2] = geo.vertices_xyzn[indices[triangle_index * 3 + 2]];

				PrimitiveRay& prr = ray_primitives[primitive_count + triangle_index];
				prr.index = primitive_count + triangle_index;
				prr.bound.min = bound_min(prd.vertices[0].pos, prd.vertices[1].pos, prd.vertices[2].pos);
				prr.bound.max = bound_max(prd.vertices[0].pos, prd.vertices[1].pos, prd.vertices[2].pos);
			}
			primitive_count += indices.size() / 3;
		}
	}

	bb_ = ray_primitives[0].bound;
	for (auto&& ray_primitive : ray_primitives)
	{
		mergeBound(bb_, ray_primitive.bound);
	}

	for (auto&& ray_primitive : ray_primitives)
	{
		ray_primitive.morton_code = genMortonCode(ray_primitive.bound, bb_);
	}

	std::sort(ray_primitives.begin(), ray_primitives.end(), [](auto x, auto y) { return x.morton_code < y.morton_code; });
	ray_cs_primitives.reserve(ray_primitives.size());
	for (auto&& ray_primitive : ray_primitives)
	{
		ray_cs_primitives.push_back({ray_primitive.bound,ray_primitive.index});
	}
}

void RayTracer::loadShaders()
{
	if (CreateCS(RADIX_SHADER_NAME, "CSMain"))
	{
		radix_build_shader_index = compute_shaders_.size() - 1;
	}
	if (CreateCS(BVH_SHADER_NAME, "CSMain"))
	{
		bvh_build_shader_index = compute_shaders_.size() - 1;
	}
	if (CreateCS(RAY_SHADER_NAME, "CSMain"))
	{
		ray_shader_index = compute_shaders_.size() - 1;
	}
}

void RayTracer::createBuffers()
{
	{
		CB_Radix rcb;
		rcb.node_count = primitive_count;
		addCB(sizeof(CB_Radix), 1, &rcb);
		radix_cb_index = constant_buffers_.size() - 1;
	}

	{
		auto tmp = malloc(sizeof(TreeNode) * primitive_count * 2);
		memset(tmp, 0, sizeof(TreeNode) * primitive_count * 2);
		addStructUAV(sizeof(TreeNode), primitive_count * 2, tmp);
		free(tmp);
		tree_uav_index = unordered_access_views.size() - 1;
	}

	{
		addSRV(sizeof(PrimitiveRayCS), primitive_count, ray_cs_primitives.data());
		primitive_ray_srv_index = shader_resource_views.size() - 1;
	}

	{
		auto tmp = malloc(sizeof(int32_t) * primitive_count);
		memset(tmp, 0, sizeof(int32_t) * (primitive_count));
		addByteUAV(sizeof(int32_t), primitive_count, tmp);
		free(tmp);
		bvh_build_lock_uav_index = unordered_access_views.size() - 1;
	}

	{
		CB_RT cbrt;
		D3DXMatrixIdentity(&cbrt.invModel);
		D3DXMatrixIdentity(&cbrt.invView);
		cbrt.tanHalfFovY = 0.3;
		cbrt.viewportDims.x = 640;
		cbrt.viewportDims.y = 480;
		addCB(sizeof(CB_RT), 1, &cbrt);
		rt_cb_index = constant_buffers_.size() - 1;
	}

	{
		addTextureUAV(sizeof(uint32_t), 640, 480);
		output_tex_uav_index = unordered_access_views.size() - 1;
	}

	{
		addSRV(sizeof(PrimitiveRender), renderer_primitives.size(), renderer_primitives.data());
		primitive_render_srv_index = shader_resource_views.size() - 1;
	}
}

void RayTracer::run(ID3D11DeviceContext* pd3dImmediateContext)
{
	pd3dImmediateContext_ = pd3dImmediateContext;
	buildRadixTree();
	buildBVH();
	trace();
	finish();
}

void RayTracer::buildRadixTree()
{
	pd3dImmediateContext_->CSSetShader(compute_shaders_[radix_build_shader_index], nullptr, 0);
	pd3dImmediateContext_->CSSetUnorderedAccessViews(0, 1, &unordered_access_views[tree_uav_index], nullptr);
	pd3dImmediateContext_->CSSetConstantBuffers(0, 1, &constant_buffers_[radix_cb_index]);
	pd3dImmediateContext_->Dispatch(primitive_count / 64 + 1, 1, 1);
}

void RayTracer::buildBVH()
{
	pd3dImmediateContext_->CSSetShader(compute_shaders_[bvh_build_shader_index], nullptr, 0);
	pd3dImmediateContext_->CSSetShaderResources(0, 1, &shader_resource_views[primitive_ray_srv_index]);
	pd3dImmediateContext_->CSSetUnorderedAccessViews(0, 1, &unordered_access_views[tree_uav_index], nullptr);
	pd3dImmediateContext_->CSSetUnorderedAccessViews(1, 1, &unordered_access_views[bvh_build_lock_uav_index], nullptr);
	pd3dImmediateContext_->CSSetConstantBuffers(0, 1, &constant_buffers_[radix_cb_index]);
	pd3dImmediateContext_->Dispatch(primitive_count / 64 + 1, 1, 1);
}

void RayTracer::trace()
{
	pd3dImmediateContext_->CSSetShader(compute_shaders_[ray_shader_index], nullptr, 0);
	pd3dImmediateContext_->CSSetShaderResources(0, 1, &shader_resource_views[primitive_render_srv_index]);
	pd3dImmediateContext_->CSSetUnorderedAccessViews(0, 1, &unordered_access_views[tree_uav_index], nullptr);
	pd3dImmediateContext_->CSSetUnorderedAccessViews(1, 1, &unordered_access_views[output_tex_uav_index], nullptr);
	pd3dImmediateContext_->CSSetConstantBuffers(0, 1, &constant_buffers_[radix_cb_index]);
	pd3dImmediateContext_->CSSetConstantBuffers(1, 1, &constant_buffers_[rt_cb_index]);
	pd3dImmediateContext_->Dispatch(640 / 16, 480 / 16 , 1);
}

void RayTracer::finish()
{
	pd3dImmediateContext_->CSSetShader(nullptr, nullptr, 0);

	ID3D11UnorderedAccessView* ppUAViewNULL[] = {nullptr, nullptr};
	pd3dImmediateContext_->CSSetUnorderedAccessViews(0, 2, ppUAViewNULL, nullptr);

	ID3D11ShaderResourceView* ppSRVNULL[2] = {nullptr,nullptr};
	pd3dImmediateContext_->CSSetShaderResources(0, 2, ppSRVNULL);

	ID3D11Buffer* ppCBNULL[1] = {nullptr};
	pd3dImmediateContext_->CSSetConstantBuffers(0, 1, ppCBNULL);
}

void RayTracer::release()
{
	BufferManager::release();
	ShaderManager::release();
}