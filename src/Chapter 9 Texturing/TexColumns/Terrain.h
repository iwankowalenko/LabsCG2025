#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/MathHelper.h"
#include <DirectXCollision.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Quadtree LOD levels: L0..L5 (6 levels total), where L5 is the most detailed.
constexpr int kTerrainMaxLOD = 5;
constexpr int kTerrainLODLevels = kTerrainMaxLOD + 1;

struct TerrainTile
{
	int LOD = 0;                 // 0..5
	int TileX = 0;               // tile index in X (0..(2^LOD)-1)
	int TileZ = 0;               // tile index in Z (0..(2^LOD)-1)
	int HeightmapSrvIndex = -1;  // SRV index for heightmap (t0)
	int DiffuseSrvIndex = -1;    // SRV index for diffuse (t1)
	int NormalSrvIndex = -1;     // SRV index for normal (t2)
	std::uint8_t NeighborCoarserMask = 0; // bits: 0=left,1=right,2=bottom(-Z),3=top(+Z)
	DirectX::BoundingBox AABB;   // world AABB for frustum culling
	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 PrevWorld = MathHelper::Identity4x4();
};

// Quadtree node for LOD selection
struct TerrainNode
{
	DirectX::BoundingBox Bounds;
	int LOD = 0;
	int TileX = 0;
	int TileZ = 0;
	int HeightmapSrvIndex = -1;
	int DiffuseSrvIndex = -1;
	int NormalSrvIndex = -1;
	std::unique_ptr<TerrainNode> Children[4];
	bool IsLeaf() const { return !Children[0]; }
};

class Terrain
{
public:
	Terrain() = default;

	// World size in XZ (e.g. 100 = terrain from -50..50)
	void SetWorldSize(float sizeXZ);
	// Origin Y: terrain AABB min Y = originY, center Y = originY + heightScale*0.5
	void SetOriginY(float y);
	float GetOriginY() const { return mOriginY; }
	// Height scale: heightmap value 0..1 multiplied by this
	void SetHeightScale(float scale) { mHeightScale = scale; }
	float GetHeightScale() const { return mHeightScale; }
	// LOD distance thresholds (legacy 2-knob API; expanded internally for L0..L5).
	void SetLODDistances(float maxDistLOD1, float maxDistLOD2);

	// Build quadtree: L0 = 1 tile ... L5 = 32x32 tiles
	void BuildQuadtree();

	// Update visible tiles: frustum culling + LOD by distance, fill mVisibleTiles
	void Update(const DirectX::XMFLOAT4X4& viewProj, const DirectX::XMFLOAT3& eyePos);

	const std::vector<TerrainTile>& GetVisibleTiles() const { return mVisibleTiles; }

	// Tile world transform and AABB for a node
	void FillTileFromNode(const TerrainNode& node, TerrainTile& outTile) const;
	// Assign SRV indices to each node (call after descriptors built).
	// Each array element is a flattened (tilesPerSide * tilesPerSide) list in row-major: idx = z*tilesPerSide + x.
	void AssignTileSrvIndices(
		const std::array<std::vector<int>, kTerrainLODLevels>& heightSrv,
		const std::array<std::vector<int>, kTerrainLODLevels>& diffuseSrv,
		const std::array<std::vector<int>, kTerrainLODLevels>& normalSrv);

	const TerrainNode* GetRoot() const { return mRoot.get(); }
	float GetWorldSizeXZ() const { return mWorldSizeXZ; }

private:
	float mWorldSizeXZ = 100.0f;
	float mHeightScale = 50.0f;
	float mOriginY = 0.0f;
	// Split distances:
	// - L0 splits into L1 when dist < mSplitDistL0
	// - L1 splits into L2 when dist < mSplitDistL1
	// - Deeper levels halve the threshold each level (L2 < mSplitDistL1/2, ...).
	float mSplitDistL0 = 0.0f;
	float mSplitDistL1 = 0.0f;
	std::unique_ptr<TerrainNode> mRoot;
	std::vector<TerrainTile> mVisibleTiles;

	void GatherLeaves(TerrainNode& node, const DirectX::XMFLOAT4X4& viewProj, const DirectX::XMFLOAT3& eyePos,
		std::vector<TerrainNode*>& outLeaves);
	void BalanceLeaves(std::vector<TerrainNode*>& leaves, const DirectX::XMFLOAT4X4& viewProj, const DirectX::XMFLOAT3& eyePos);

	void BuildNode(TerrainNode& node, int lod, int tileX, int tileZ);
	bool IntersectsFrustum(const DirectX::BoundingBox& box, const DirectX::XMFLOAT4X4& viewProj) const;
	float DistanceToNode(const DirectX::BoundingBox& box, const DirectX::XMFLOAT3& eyePos) const;
	void SelectLOD(const TerrainNode& node, const DirectX::XMFLOAT4X4& viewProj, const DirectX::XMFLOAT3& eyePos);
	void AssignTileSrvIndicesRecursive(
		TerrainNode& node,
		const std::array<std::vector<int>, kTerrainLODLevels>& heightSrv,
		const std::array<std::vector<int>, kTerrainLODLevels>& diffuseSrv,
		const std::array<std::vector<int>, kTerrainLODLevels>& normalSrv);
};
