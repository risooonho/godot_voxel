
def can_build(env, platform):
  return True


def configure(env):
	pass

def get_icons_path():
  return "editor/icons"


def get_doc_classes():
  return [
    "VoxelServer",

    "VoxelBuffer",
    "VoxelMap",

    "Voxel",
    "VoxelLibrary",

    "VoxelTerrain",
    "VoxelLodTerrain",
    "VoxelViewer",

    "VoxelStream",
    "VoxelStreamFile",
    "VoxelStreamBlockFiles",
    "VoxelStreamRegionFiles",

    "VoxelGenerator",
    "VoxelGeneratorHeightmap",
    "VoxelGeneratorImage",
    "VoxelGeneratorNoise",
    "VoxelGeneratorNoise2D",
    "VoxelGeneratorTest",
    "VoxelGeneratorGraph",

    "VoxelBoxMover",
    "VoxelTool",
    "VoxelToolTerrain",
    "VoxelRaycastResult",
    "VoxelBlockSerializer",

    "VoxelMesher",
    "VoxelMesherBlocky",
    "VoxelMesherTransvoxel",
    "VoxelMesherDMC"
]


def get_doc_path():
  return "doc/classes"
