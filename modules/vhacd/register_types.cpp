/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "register_types.h"

#include "scene/resources/mesh.h"

#include <thirdparty/vhacd/public/VHACD.h>

static Vector<Vector<Vector3>> convex_decompose(const real_t *p_vertices, int p_vertex_count, const uint32_t *p_triangles, int p_triangle_count, const Ref<MeshConvexDecompositionSettings> &p_settings, Vector<Vector<uint32_t>> *r_convex_indices) {
	Vector<Vector<Vector3>> ret;
	if (r_convex_indices) {
		r_convex_indices->clear();
	}
	ERR_FAIL_COND_V(p_settings.is_null(), ret);
	ERR_FAIL_COND_V(!p_vertices || p_vertex_count < 4 || !p_triangles || p_triangle_count < 4, ret);
	for (int i = 0; i < p_vertex_count; i++) {
		ERR_FAIL_COND_V(!Vector3(p_vertices[i * 3], p_vertices[i * 3 + 1], p_vertices[i * 3 + 2]).is_finite(), ret);
	}
	for (int i = 0; i < p_triangle_count; i++) {
		for (int j = 0; j < 3; j++) {
			ERR_FAIL_COND_V(p_triangles[i * 3 + j] >= uint32_t(p_vertex_count), ret);
		}
	}

	VHACD::IVHACD::Parameters params;
	// V-HACD 4 measures percentage volume error, rather than V-HACD 2's
	// concavity. Preserve the property's range and direction, using percent.
	params.m_minimumVolumePercentErrorAllowed = p_settings->get_max_concavity() * 100.0;
	params.m_resolution = p_settings->get_resolution();
	params.m_maxNumVerticesPerCH = p_settings->get_max_num_vertices_per_convex_hull();
	params.m_maxConvexHulls = p_settings->get_max_convex_hulls();
	params.m_shrinkWrap = p_settings->get_project_hull_vertices();
	// Keep decomposition synchronous without an additional per-mesh worker pool.
	params.m_asyncACD = false;
	// Removed legacy tuning parameters remain serializable, but have no
	// equivalent in V-HACD 4. See MeshConvexDecompositionSettings documentation.

	VHACD::IVHACD *decomposer = VHACD::CreateVHACD();
	if (!decomposer->Compute(p_vertices, p_vertex_count, p_triangles, p_triangle_count, params)) {
		decomposer->Release();
		ERR_FAIL_V_MSG(ret, "V-HACD convex decomposition failed.");
	}

	int hull_count = decomposer->GetNConvexHulls();

	ret.resize(hull_count);

	if (r_convex_indices) {
		r_convex_indices->resize(hull_count);
	}

	for (int i = 0; i < hull_count; i++) {
		VHACD::IVHACD::ConvexHull hull;
		decomposer->GetConvexHull(i, hull);

		Vector<Vector3> &points = ret.write[i];
		points.resize(hull.m_points.size());

		Vector3 *w = points.ptrw();
		for (uint32_t j = 0; j < hull.m_points.size(); ++j) {
			for (int k = 0; k < 3; k++) {
				w[j][k] = hull.m_points[j][k];
			}
		}

		if (r_convex_indices) {
			Vector<uint32_t> &indices = r_convex_indices->write[i];
			indices.resize(hull.m_triangles.size() * 3);

			uint32_t *w_indices = indices.ptrw();
			for (uint32_t j = 0; j < hull.m_triangles.size(); j++) {
				w_indices[j * 3] = hull.m_triangles[j].mI0;
				w_indices[j * 3 + 1] = hull.m_triangles[j].mI1;
				w_indices[j * 3 + 2] = hull.m_triangles[j].mI2;
			}
		}
	}

	decomposer->Clean();
	decomposer->Release();

	return ret;
}

void initialize_vhacd_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	Mesh::convex_decomposition_function = convex_decompose;
}

void uninitialize_vhacd_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	Mesh::convex_decomposition_function = nullptr;
}
