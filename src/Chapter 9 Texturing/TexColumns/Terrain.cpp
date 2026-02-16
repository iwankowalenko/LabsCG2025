#include "Terrain.h"
#include <DirectXCollision.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_set>

using namespace DirectX;

void Terrain::SetWorldSize(float sizeXZ)
{
	mWorldSizeXZ = sizeXZ;
}

void Terrain::SetOriginY(float y)
{
	mOriginY = y;
}

void Terrain::SetLODDistances(float maxDistLOD1, float maxDistLOD2)
{
	// Two-knob control:
	// - maxDistLOD1: split L0 -> L1
	// - maxDistLOD2: split L1 -> L2 (deeper levels halve this threshold each level)
	mSplitDistL0 = (std::max)(0.0f, maxDistLOD1);
	mSplitDistL1 = (std::max)(0.0f, maxDistLOD2);

	// Enforce monotonicity (deeper splits must be closer than root split).
	if (mSplitDistL1 > mSplitDistL0)
		mSplitDistL1 = mSplitDistL0;
}

void Terrain::BuildQuadtree()
{
	mRoot = std::make_unique<TerrainNode>();
	mRoot->LOD = 0;
	mRoot->TileX = 0;
	mRoot->TileZ = 0;
	// Full terrain in XZ: [-mWorldSizeXZ/2, mWorldSizeXZ/2], Y from mOriginY to mOriginY + mHeightScale
	XMFLOAT3 center(0.f, mOriginY + mHeightScale * 0.5f, 0.f);
	XMFLOAT3 extents(mWorldSizeXZ * 0.5f, mHeightScale * 0.5f, mWorldSizeXZ * 0.5f);
	mRoot->Bounds = BoundingBox(center, extents);
	BuildNode(*mRoot, 0, 0, 0);

	// Reasonable defaults if user didn't call SetLODDistances.
	if (mSplitDistL0 <= 0.0f || mSplitDistL1 <= 0.0f)
	{
		// Root split distance and L1 split distance (rest derived by halving).
		mSplitDistL0 = mWorldSizeXZ * 0.85f;
		mSplitDistL1 = mWorldSizeXZ * 0.55f;
	}
}

void Terrain::BuildNode(TerrainNode& node, int lod, int tileX, int tileZ)
{
	node.LOD = lod;
	node.TileX = tileX;
	node.TileZ = tileZ;
	if (lod >= kTerrainMaxLOD) return; // leaf at max LOD
	float half = node.Bounds.Extents.x;
	float quarter = half * 0.5f;
	XMVECTOR c = XMLoadFloat3(&node.Bounds.Center);
	for (int i = 0; i < 4; ++i)
	{
		node.Children[i] = std::make_unique<TerrainNode>();
		int cx = i % 2;
		int cz = i / 2;
		float ox = (cx == 0) ? -quarter : quarter;
		float oz = (cz == 0) ? -quarter : quarter;
		XMFLOAT3 childCenter;
		XMStoreFloat3(&childCenter, c + XMVectorSet(ox, 0.f, oz, 0.f));
		node.Children[i]->Bounds = BoundingBox(childCenter, XMFLOAT3(quarter, node.Bounds.Extents.y, quarter));
		int childTileX = tileX * 2 + cx;
		int childTileZ = tileZ * 2 + cz;
		BuildNode(*node.Children[i], lod + 1, childTileX, childTileZ);
	}
}

void Terrain::GatherLeaves(TerrainNode& node, const XMFLOAT4X4& viewProj, const XMFLOAT3& eyePos,
	std::vector<TerrainNode*>& outLeaves)
{
	if (!IntersectsFrustum(node.Bounds, viewProj))
		return;

	const float dist = DistanceToNode(node.Bounds, eyePos);

	float splitDist = 0.0f;
	if (node.LOD == 0)
		splitDist = mSplitDistL0;
	else
	{
		const int shift = node.LOD - 1;
		const float denom = (shift >= 0 && shift < 30) ? (float)(1u << shift) : 1.0f;
		splitDist = mSplitDistL1 / denom;
	}

	const bool canSplit = (node.LOD < kTerrainMaxLOD) && (node.Children[0] != nullptr);
	const bool wantSplit = canSplit && (dist < splitDist);

	if (wantSplit)
	{
		for (int i = 0; i < 4; ++i)
			GatherLeaves(*node.Children[i], viewProj, eyePos, outLeaves);
	}
	else
	{
		outLeaves.push_back(&node);
	}
}

void Terrain::BalanceLeaves(std::vector<TerrainNode*>& leaves, const XMFLOAT4X4& viewProj, const XMFLOAT3& eyePos)
{
	constexpr int kMaxGrid = 1 << kTerrainMaxLOD; // 32
	std::array<TerrainNode*, kMaxGrid* kMaxGrid> cover = {};

	auto leafScale = [](const TerrainNode* n) -> int
		{
			return 1 << (kTerrainMaxLOD - n->LOD);
		};

	auto rebuildCover = [&]()
		{
			cover.fill(nullptr);
			for (TerrainNode* n : leaves)
			{
				if (!n) continue;
				const int s = leafScale(n);
				const int minX = n->TileX * s;
				const int minZ = n->TileZ * s;
				const int maxX = (n->TileX + 1) * s - 1;
				const int maxZ = (n->TileZ + 1) * s - 1;
				for (int z = minZ; z <= maxZ; ++z)
					for (int x = minX; x <= maxX; ++x)
						cover[z * kMaxGrid + x] = n;
			}
		};

	// Iteratively refine coarse leaves until neighbor LOD difference <= 1.
	for (int iter = 0; iter < 16; ++iter)
	{
		rebuildCover();

		std::vector<TerrainNode*> toSplit;
		toSplit.reserve(64);

		for (int z = 0; z < kMaxGrid; ++z)
		{
			for (int x = 0; x < kMaxGrid; ++x)
			{
				TerrainNode* a = cover[z * kMaxGrid + x];
				if (!a) continue;

				if (x + 1 < kMaxGrid)
				{
					TerrainNode* b = cover[z * kMaxGrid + (x + 1)];
					if (b && b != a && std::abs(a->LOD - b->LOD) > 1)
						toSplit.push_back((a->LOD < b->LOD) ? a : b);
				}
				if (z + 1 < kMaxGrid)
				{
					TerrainNode* b = cover[(z + 1) * kMaxGrid + x];
					if (b && b != a && std::abs(a->LOD - b->LOD) > 1)
						toSplit.push_back((a->LOD < b->LOD) ? a : b);
				}
			}
		}

		if (toSplit.empty())
			break;

		std::sort(toSplit.begin(), toSplit.end());
		toSplit.erase(std::unique(toSplit.begin(), toSplit.end()), toSplit.end());

		// Remove these coarse leaves.
		leaves.erase(std::remove_if(leaves.begin(), leaves.end(),
			[&](TerrainNode* n)
			{
				return std::binary_search(toSplit.begin(), toSplit.end(), n);
			}),
			leaves.end());

		// Split them and add (visible) children back using the regular distance rule.
		for (TerrainNode* n : toSplit)
		{
			if (!n || n->LOD >= kTerrainMaxLOD || n->Children[0] == nullptr)
				continue;
			for (int i = 0; i < 4; ++i)
				GatherLeaves(*n->Children[i], viewProj, eyePos, leaves);
		}
	}
}

bool Terrain::IntersectsFrustum(const BoundingBox& box, const XMFLOAT4X4& viewProj) const
{
	// IMPORTANT:
	// - Shaders use mul(pos, M) (row-vector convention).
	// - PassConstants stores matrices transposed for HLSL column-major layout.
	// For row-vector convention, frustum planes are derived from COLUMNS of the (non-transposed) ViewProj matrix.
	// Since we receive a transposed matrix (M^T), its ROWS correspond to COLUMNS of M.
	const float* m = &viewProj.m[0][0];
	auto getCol = [m](int c) -> XMVECTOR {
		const int base = c * 4;
		return XMVectorSet(m[base + 0], m[base + 1], m[base + 2], m[base + 3]);
	};
	const XMVECTOR C0 = getCol(0);
	const XMVECTOR C1 = getCol(1);
	const XMVECTOR C2 = getCol(2);
	const XMVECTOR C3 = getCol(3);
	XMVECTOR planes[6] = {
		XMVectorAdd(C3, C0),        // Left   :  x + w >= 0
		XMVectorSubtract(C3, C0),   // Right  : -x + w >= 0
		XMVectorAdd(C3, C1),        // Bottom :  y + w >= 0
		XMVectorSubtract(C3, C1),   // Top    : -y + w >= 0
		C2,                         // Near   :  z >= 0   (D3D clip space)
		XMVectorSubtract(C3, C2)    // Far    : -z + w >= 0
	};
	for (int i = 0; i < 6; ++i)
	{
		XMVECTOR p = planes[i];
		float len = XMVectorGetX(XMVector3Length(p));
		if (len < 1e-6f) continue;
		p = XMVectorScale(p, 1.f / len);
		// Plane equation: dot(n, x) + d >= 0 is inside.
		const float d = XMVectorGetW(p);
		// AABB: positive vertex along plane normal
		float nx = XMVectorGetX(p), ny = XMVectorGetY(p), nz = XMVectorGetZ(p);
		XMFLOAT3 pVertex(
			box.Center.x + (nx >= 0.f ? box.Extents.x : -box.Extents.x),
			box.Center.y + (ny >= 0.f ? box.Extents.y : -box.Extents.y),
			box.Center.z + (nz >= 0.f ? box.Extents.z : -box.Extents.z)
		);
		if (nx * pVertex.x + ny * pVertex.y + nz * pVertex.z + d < 0.f)
			return false;
	}
	return true;
}

float Terrain::DistanceToNode(const BoundingBox& box, const XMFLOAT3& eyePos) const
{
	// Distance from point to AABB (in world space).
	// Stable near edges (XZ) and naturally accounts for being above/below the terrain range.
	const float ax = (std::max)(std::abs(eyePos.x - box.Center.x) - box.Extents.x, 0.0f);
	const float ay = (std::max)(std::abs(eyePos.y - box.Center.y) - box.Extents.y, 0.0f);
	const float az = (std::max)(std::abs(eyePos.z - box.Center.z) - box.Extents.z, 0.0f);
	return std::sqrt(ax * ax + ay * ay + az * az);
}

void Terrain::Update(const XMFLOAT4X4& viewProj, const XMFLOAT3& eyePos)
{
	mVisibleTiles.clear();
	if (!mRoot) return;

	std::vector<TerrainNode*> leaves;
	leaves.reserve(512);
	GatherLeaves(*mRoot, viewProj, eyePos, leaves);
	BalanceLeaves(leaves, viewProj, eyePos);

	// Build coverage map (max LOD grid) to detect edges where neighbor is coarser.
	constexpr int kMaxGrid = 1 << kTerrainMaxLOD; // 32
	std::array<TerrainNode*, kMaxGrid* kMaxGrid> cover = {};
	cover.fill(nullptr);
	for (TerrainNode* n : leaves)
	{
		if (!n) continue;
		const int s = 1 << (kTerrainMaxLOD - n->LOD);
		const int minX = n->TileX * s;
		const int minZ = n->TileZ * s;
		const int maxX = (n->TileX + 1) * s - 1;
		const int maxZ = (n->TileZ + 1) * s - 1;
		for (int z = minZ; z <= maxZ; ++z)
			for (int x = minX; x <= maxX; ++x)
				cover[z * kMaxGrid + x] = n;
	}

	auto isCoarser = [](const TerrainNode* a, const TerrainNode* b) -> bool
		{
			return b && a && (b->LOD < a->LOD);
		};

	// Emit visible tiles with neighbor masks.
	mVisibleTiles.reserve(leaves.size());
	for (TerrainNode* n : leaves)
	{
		if (!n) continue;
		const int s = 1 << (kTerrainMaxLOD - n->LOD);
		const int minX = n->TileX * s;
		const int minZ = n->TileZ * s;
		const int maxX = (n->TileX + 1) * s - 1;
		const int maxZ = (n->TileZ + 1) * s - 1;

		std::uint8_t mask = 0;
		// left
		if (minX > 0)
			for (int z = minZ; z <= maxZ; ++z)
				if (isCoarser(n, cover[z * kMaxGrid + (minX - 1)])) { mask |= 1; break; }
		// right
		if (maxX + 1 < kMaxGrid)
			for (int z = minZ; z <= maxZ; ++z)
				if (isCoarser(n, cover[z * kMaxGrid + (maxX + 1)])) { mask |= 2; break; }
		// bottom (-Z)
		if (minZ > 0)
			for (int x = minX; x <= maxX; ++x)
				if (isCoarser(n, cover[(minZ - 1) * kMaxGrid + x])) { mask |= 4; break; }
		// top (+Z)
		if (maxZ + 1 < kMaxGrid)
			for (int x = minX; x <= maxX; ++x)
				if (isCoarser(n, cover[(maxZ + 1) * kMaxGrid + x])) { mask |= 8; break; }

		TerrainTile tile;
		FillTileFromNode(*n, tile);
		tile.NeighborCoarserMask = mask;
		mVisibleTiles.push_back(tile);
	}
}

void Terrain::FillTileFromNode(const TerrainNode& node, TerrainTile& outTile) const
{
	outTile.LOD = node.LOD;
	outTile.TileX = node.TileX;
	outTile.TileZ = node.TileZ;
	outTile.HeightmapSrvIndex = node.HeightmapSrvIndex;
	outTile.DiffuseSrvIndex = node.DiffuseSrvIndex;
	outTile.NormalSrvIndex = node.NormalSrvIndex;
	outTile.AABB = node.Bounds;
	float half = node.Bounds.Extents.x;
	XMMATRIX world = XMMatrixScaling(half * 2.f, mHeightScale, half * 2.f);
	world = XMMatrixMultiply(world, XMMatrixTranslationFromVector(XMLoadFloat3(&node.Bounds.Center)));
	XMStoreFloat4x4(&outTile.World, XMMatrixTranspose(world));
	outTile.PrevWorld = outTile.World;
}

void Terrain::AssignTileSrvIndicesRecursive(
	TerrainNode& node,
	const std::array<std::vector<int>, kTerrainLODLevels>& heightSrv,
	const std::array<std::vector<int>, kTerrainLODLevels>& diffuseSrv,
	const std::array<std::vector<int>, kTerrainLODLevels>& normalSrv)
{
	const int L = node.LOD;
	const int tilesPerSide = 1 << L;
	const int idx = node.TileZ * tilesPerSide + node.TileX;
	if (L >= 0 && L < kTerrainLODLevels)
	{
		if (idx >= 0 && idx < (int)heightSrv[L].size()) node.HeightmapSrvIndex = heightSrv[L][idx];
		if (idx >= 0 && idx < (int)diffuseSrv[L].size()) node.DiffuseSrvIndex = diffuseSrv[L][idx];
		if (idx >= 0 && idx < (int)normalSrv[L].size()) node.NormalSrvIndex = normalSrv[L][idx];
	}
	for (int i = 0; i < 4; ++i)
		if (node.Children[i])
			AssignTileSrvIndicesRecursive(*node.Children[i], heightSrv, diffuseSrv, normalSrv);
}

void Terrain::AssignTileSrvIndices(
	const std::array<std::vector<int>, kTerrainLODLevels>& heightSrv,
	const std::array<std::vector<int>, kTerrainLODLevels>& diffuseSrv,
	const std::array<std::vector<int>, kTerrainLODLevels>& normalSrv)
{
	if (mRoot)
		AssignTileSrvIndicesRecursive(*mRoot, heightSrv, diffuseSrv, normalSrv);
}
