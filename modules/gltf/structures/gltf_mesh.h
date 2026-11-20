/**************************************************************************/
/*  gltf_mesh.h                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "../gltf_defines.h"

#include "core/variant/typed_array.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/mesh.h"

class GLTFMesh : public Resource {
	GDCLASS(GLTFMesh, Resource);

private:
	String original_name;
	Ref<ImporterMesh> mesh;
	Ref<ArrayMesh> runtime_mesh;
	Vector<float> blend_weights;
	TypedArray<Material> instance_materials;
	Dictionary additional_data;

protected:
	static void _bind_methods();

public:
	String get_original_name();
	void set_original_name(const String &p_name);
	Ref<ImporterMesh> get_mesh();
	void set_mesh(const Ref<ImporterMesh> &p_mesh);
	Ref<ArrayMesh> get_runtime_mesh() const { return runtime_mesh; }
	void set_runtime_mesh(const Ref<ArrayMesh> &p_mesh) { runtime_mesh = p_mesh; }
	Vector<float> get_blend_weights();
	void set_blend_weights(const Vector<float> &p_blend_weights);
	TypedArray<Material> get_instance_materials();
	void set_instance_materials(const TypedArray<Material> &p_instance_materials);
	Variant get_additional_data(const StringName &p_extension_name);
	void set_additional_data(const StringName &p_extension_name, Variant p_additional_data);
};
