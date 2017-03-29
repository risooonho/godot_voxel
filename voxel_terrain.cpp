#include "voxel_terrain.h"
#include <scene/3d/mesh_instance.h>
#include <os/os.h>
#include "voxel_raycast.h"

VoxelTerrain::VoxelTerrain(): Node(), _min_y(-4), _max_y(4), _generate_collisions(true) {

	_map = Ref<VoxelMap>(memnew(VoxelMap));
	_mesher = Ref<VoxelMesher>(memnew(VoxelMesher));
}

Vector3i g_viewer_block_pos; // TODO UGLY! Lambdas or pointers needed...

// Sorts distance to viewer
struct BlockUpdateComparator {
	inline bool operator()(const Vector3i & a, const Vector3i & b) const {
		return a.distance_sq(g_viewer_block_pos) > b.distance_sq(g_viewer_block_pos);
	}
};

void VoxelTerrain::set_provider(Ref<VoxelProvider> provider) {
	_provider = provider;
}

Ref<VoxelProvider> VoxelTerrain::get_provider() {
	return _provider;
}

Ref<VoxelLibrary> VoxelTerrain::get_voxel_library() {
	return _mesher->get_library();
}

void VoxelTerrain::set_generate_collisions(bool enabled) {
	_generate_collisions = enabled;
}

void VoxelTerrain::set_viewer_path(NodePath path) {
	if(!path.is_empty())
		ERR_FAIL_COND(get_viewer(path) == NULL);
	_viewer_path = path;
}

NodePath VoxelTerrain::get_viewer_path() {
	return _viewer_path;
}

Spatial * VoxelTerrain::get_viewer(NodePath path) {
	if(path.is_empty())
		return NULL;
	Node * node = get_node(path);
	if(node == NULL)
		return NULL;
	return node->cast_to<Spatial>();
}

//void VoxelTerrain::clear_update_queue() {
//	_block_update_queue.clear();
//	_dirty_blocks.clear();
//}

void VoxelTerrain::make_block_dirty(Vector3i bpos) {
	// TODO Immediate update viewer distance
	if(is_block_dirty(bpos) == false) {
		//OS::get_singleton()->print("Dirty (%i, %i, %i)", bpos.x, bpos.y, bpos.z);
		_block_update_queue.push_back(bpos);
		_dirty_blocks[bpos] = true;
	}
}

bool VoxelTerrain::is_block_dirty(Vector3i bpos) {
	return _dirty_blocks.has(bpos);
}

void VoxelTerrain::make_blocks_dirty(Vector3i min, Vector3i size) {
	Vector3i max = min + size;
	Vector3i pos;
	for(pos.z = min.z; pos.z < max.z; ++pos.z) {
		for(pos.y = min.y; pos.y < max.y; ++pos.y) {
			for(pos.x = min.x; pos.x < max.x; ++pos.x) {
				make_block_dirty(pos);
			}
		}
	}
}

void VoxelTerrain::make_voxel_dirty(Vector3i pos) {

	// Update the block in which the voxel is
	Vector3i bpos = VoxelMap::voxel_to_block(pos);
	make_block_dirty(bpos);

	// Update neighbor blocks if the voxel is touching a boundary

	Vector3i rpos = VoxelMap::to_local(pos);

	if(rpos.x == 0)
		make_block_dirty(bpos - Vector3i(1,0,0));
	if(rpos.y == 0)
		make_block_dirty(bpos - Vector3i(0,1,0));
	if(rpos.z == 0)
		make_block_dirty(bpos - Vector3i(0,0,1));

	if(rpos.x == VoxelBlock::SIZE-1)
		make_block_dirty(bpos + Vector3i(1,0,0));
	if(rpos.y == VoxelBlock::SIZE-1)
		make_block_dirty(bpos + Vector3i(0,1,0));
	if(rpos.z == VoxelBlock::SIZE-1)
		make_block_dirty(bpos + Vector3i(0,0,1));
}

int VoxelTerrain::get_block_update_count() {
	return _block_update_queue.size();
}

void VoxelTerrain::_notification(int p_what) {

	switch (p_what) {

	case NOTIFICATION_ENTER_TREE:
		set_process(true);
		break;

	case NOTIFICATION_PROCESS:
		_process();
		break;

	case NOTIFICATION_EXIT_TREE:
		break;

	default:
		break;
	}
}

void VoxelTerrain::_process() {
	update_blocks();
}

void VoxelTerrain::update_blocks() {
	OS & os = *OS::get_singleton();

	uint32_t time_before = os.get_ticks_msec();
	uint32_t max_time = 1000 / 60;

	// Get viewer location
	Spatial * viewer = get_viewer(_viewer_path);
	if(viewer)
		g_viewer_block_pos = VoxelMap::voxel_to_block(viewer->get_translation());
	else
		g_viewer_block_pos = Vector3i();

	// Sort updates so nearest blocks are done first
	_block_update_queue.sort_custom<BlockUpdateComparator>();

	// Update a bunch of blocks until none are left or too much time elapsed
	while (!_block_update_queue.empty() && (os.get_ticks_msec() - time_before) < max_time) {

		//printf("Remaining: %i\n", _block_update_queue.size());
		//float time_before = os.get_ticks_usec();

		// TODO Move this to a thread
		// TODO Have VoxelTerrainGenerator in C++

		// Get request
		Vector3i block_pos = _block_update_queue[_block_update_queue.size() - 1];

		bool entire_block_changed = false;

		if (!_map->has_block(block_pos)) {
			// Create buffer
			if(!_provider.is_null()) {
				Ref<VoxelBuffer> buffer_ref = Ref<VoxelBuffer>(memnew(VoxelBuffer));
				const Vector3i block_size(VoxelBlock::SIZE, VoxelBlock::SIZE, VoxelBlock::SIZE);
				buffer_ref->create(block_size.x, block_size.y, block_size.z);

				// Query voxel provider
				_provider->emerge_block(buffer_ref, block_pos);

				// Check script return
				ERR_FAIL_COND(buffer_ref->get_size() != block_size);

				// Store buffer
				_map->set_block_buffer(block_pos, buffer_ref);

				entire_block_changed = true;
			}
		}

		// Update views (mesh/collisions)

		if(entire_block_changed) {
			// All neighbors have to be checked
			Vector3i ndir;
			for (ndir.z = -1; ndir.z < 2; ++ndir.z) {
				for (ndir.x = -1; ndir.x < 2; ++ndir.x) {
					for (ndir.y = -1; ndir.y < 2; ++ndir.y) {
						Vector3i npos = block_pos + ndir;
						// TODO What if the map is really composed of empty blocks?
						if (_map->is_block_surrounded(npos)) {
							update_block_mesh(npos);
						}
					}
				}
			}
		}
		else {
			// Only update the block, neighbors will probably follow if needed
			update_block_mesh(block_pos);
			//OS::get_singleton()->print("Update (%i, %i, %i)\n", block_pos.x, block_pos.y, block_pos.z);
		}

		// Pop request
		_block_update_queue.resize(_block_update_queue.size() - 1);
		_dirty_blocks.erase(block_pos);
	}
}

void VoxelTerrain::update_block_mesh(Vector3i block_pos) {
	VoxelBlock * block = _map->get_block(block_pos);
	if (block == NULL) {
		return;
	}

	if (block->voxels->is_uniform(0) && block->voxels->get_voxel(0, 0, 0, 0) == 0) {
		// Optimization: the block contains nothing

		MeshInstance * mesh_instance = block->get_mesh_instance(*this);
		if(mesh_instance) {
			mesh_instance->set_mesh(Ref<Mesh>());
		}
		StaticBody * body = block->get_physics_body(*this);
		if(body) {
			body->set_shape(0, Ref<Mesh>());
		}

		return;
	}

	// Create buffer padded with neighbor voxels
	VoxelBuffer nbuffer;
	nbuffer.create(VoxelBlock::SIZE + 2, VoxelBlock::SIZE + 2, VoxelBlock::SIZE + 2);
	_map->get_buffer_copy(VoxelMap::block_to_voxel(block_pos) - Vector3i(1, 1, 1), nbuffer);

	Vector3 block_node_pos = VoxelMap::block_to_voxel(block_pos).to_vec3();

	// Build mesh (that part is the most CPU-intensive)
	// TODO Re-use existing meshes to optimize memory cost
	Ref<Mesh> mesh = _mesher->build(nbuffer);

	// TODO Don't use nodes! Use servers directly, it's faster
	MeshInstance * mesh_instance = block->get_mesh_instance(*this);
	if (mesh_instance == NULL) {
		// Create and spawn mesh
		mesh_instance = memnew(MeshInstance);
		mesh_instance->set_mesh(mesh);
		mesh_instance->set_translation(block_node_pos);
		add_child(mesh_instance);
		block->mesh_instance_path = mesh_instance->get_path();
	}
	else {
		// Update mesh
		mesh_instance->set_mesh(mesh);
	}

	if(get_tree()->is_editor_hint() == false && _generate_collisions) {

		// Generate collisions
		// TODO Need to select only specific surfaces because some may not have collisions
		Ref<Shape> shape = mesh->create_trimesh_shape();

		StaticBody * body = block->get_physics_body(*this);
		if(body == NULL) {
			// Create body
			body = memnew(StaticBody);
			body->set_translation(block_node_pos);
			body->add_shape(shape);
			add_child(body);
			block->body_path = body->get_path();
		}
		else {
			// Update body
			body->set_shape(0, shape);
		}
	}
}

//void VoxelTerrain::block_removed(VoxelBlock & block) {
//    MeshInstance * mesh_instance = block.get_mesh_instance(*this);
//    if (mesh_instance) {
//        mesh_instance->queue_delete();
//    }
//}

static bool _raycast_binding_predicate(Vector3i pos, void *context) {

	ERR_FAIL_COND_V(context == NULL, false);
	VoxelTerrain * terrain = (VoxelTerrain*)context;

	Ref<VoxelLibrary> lib_ref = terrain->get_voxel_library();
	if(lib_ref.is_null())
		return false;
	const VoxelLibrary & lib = **lib_ref;

	Ref<VoxelMap> map = terrain->get_map();
	// TODO In the future we may want to query more channels
	int v = map->get_voxel(pos, 0);
	if(lib.has_voxel(v) == false)
		return false;

	const Voxel & voxel = lib.get_voxel_const(v);
	return !voxel.is_transparent();
}

Variant VoxelTerrain::_raycast_binding(Vector3 origin, Vector3 direction, real_t max_distance) {

	// TODO Transform input if the terrain is rotated (in the future it can be made a Spatial node)

	Vector3i hit_pos;
	Vector3i prev_pos;

	if(voxel_raycast(origin, direction, _raycast_binding_predicate, this, max_distance, hit_pos, prev_pos)) {

		Dictionary hit = Dictionary();
		hit["position"] = hit_pos.to_vec3();
		hit["prev_position"] = prev_pos.to_vec3();
		return hit;
	}
	else {
		return Variant(); // Null dictionary, no alloc
	}
}

void VoxelTerrain::_bind_methods() {

	ClassDB::bind_method(D_METHOD("set_provider", "provider:VoxelProvider"), &VoxelTerrain::set_provider);
	ClassDB::bind_method(D_METHOD("get_provider:VoxelProvider"), &VoxelTerrain::get_provider);

	ClassDB::bind_method(D_METHOD("get_block_update_count"), &VoxelTerrain::get_block_update_count);
	ClassDB::bind_method(D_METHOD("get_mesher:VoxelMesher"), &VoxelTerrain::get_mesher);

	ClassDB::bind_method(D_METHOD("get_generate_collisions"), &VoxelTerrain::get_generate_collisions);
	ClassDB::bind_method(D_METHOD("set_generate_collisions", "enabled"), &VoxelTerrain::set_generate_collisions);

	ClassDB::bind_method(D_METHOD("get_viewer"), &VoxelTerrain::get_viewer_path);
	ClassDB::bind_method(D_METHOD("set_viewer", "path"), &VoxelTerrain::set_viewer_path);

	ClassDB::bind_method(D_METHOD("get_storage:VoxelMap"), &VoxelTerrain::get_map);

	// TODO Make those two static in VoxelMap?
	ClassDB::bind_method(D_METHOD("voxel_to_block", "voxel_pos"), &VoxelTerrain::_voxel_to_block_binding);
	ClassDB::bind_method(D_METHOD("block_to_voxel", "block_pos"), &VoxelTerrain::_block_to_voxel_binding);

	ClassDB::bind_method(D_METHOD("make_block_dirty", "pos"), &VoxelTerrain::_make_block_dirty_binding);
	ClassDB::bind_method(D_METHOD("make_blocks_dirty", "min", "size"), &VoxelTerrain::_make_blocks_dirty_binding);
	ClassDB::bind_method(D_METHOD("make_voxel_dirty", "pos"), &VoxelTerrain::_make_voxel_dirty_binding);

	ClassDB::bind_method(D_METHOD("raycast:Dictionary", "origin", "direction", "max_distance"), &VoxelTerrain::_raycast_binding, DEFVAL(100));

}

