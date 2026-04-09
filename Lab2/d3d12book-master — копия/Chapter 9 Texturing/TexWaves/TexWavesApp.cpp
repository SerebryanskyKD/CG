//***************************************************************************************
// TexWavesApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/GeometryGenerator.h"
#include "FrameResource.h"
#include "Waves.h"
#include "RenderingSystem.h"
#include "ObjModelLoader.h"
#include "TgaTextureLoader.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <limits>
#include <unordered_set>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")

const int gNumFrameResources = 3;

struct RenderItem
{
	RenderItem() = default;

	XMFLOAT4X4 World = MathHelper::Identity4x4();

	XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

	int NumFramesDirty = gNumFrameResources;

	// Index into GPU constant buffer corresponding to the ObjectCB for this render item.
	UINT ObjCBIndex = -1;

	Material* Mat = nullptr;
	MeshGeometry* Geo = nullptr;

	// Primitive topology.
	D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	// DrawIndexedInstanced parameters.
	UINT IndexCount = 0;
	UINT StartIndexLocation = 0;
	int BaseVertexLocation = 0;
};

struct ScatterInstance
{
	XMFLOAT4X4 World = MathHelper::Identity4x4();
	BoundingBox Bounds;
};

struct OctreeNode
{
	BoundingBox Bounds;
	std::vector<UINT> InstanceIndices;
	std::array<std::unique_ptr<OctreeNode>, 8> Children;

	bool IsLeaf() const
	{
		for (const auto& child : Children)
		{
			if (child)
				return false;
		}
		return true;
	}
};

enum class ScatterCullingMode : int
{
	Off = 0,
	Frustum,
	Octree
};

namespace
{
	XMFLOAT3 Add(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return XMFLOAT3(a.x + b.x, a.y + b.y, a.z + b.z);
	}

	XMFLOAT3 Subtract(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return XMFLOAT3(a.x - b.x, a.y - b.y, a.z - b.z);
	}

	BoundingBox MergeBounds(const BoundingBox& a, const BoundingBox& b)
	{
		XMFLOAT3 minA = Subtract(a.Center, a.Extents);
		XMFLOAT3 maxA = Add(a.Center, a.Extents);
		XMFLOAT3 minB = Subtract(b.Center, b.Extents);
		XMFLOAT3 maxB = Add(b.Center, b.Extents);

		XMFLOAT3 minP(
			(std::min)(minA.x, minB.x),
			(std::min)(minA.y, minB.y),
			(std::min)(minA.z, minB.z));
		XMFLOAT3 maxP(
			(std::max)(maxA.x, maxB.x),
			(std::max)(maxA.y, maxB.y),
			(std::max)(maxA.z, maxB.z));

		BoundingBox merged;
		merged.Center = XMFLOAT3(
			0.5f * (minP.x + maxP.x),
			0.5f * (minP.y + maxP.y),
			0.5f * (minP.z + maxP.z));
		merged.Extents = XMFLOAT3(
			0.5f * (maxP.x - minP.x),
			0.5f * (maxP.y - minP.y),
			0.5f * (maxP.z - minP.z));
		return merged;
	}

	bool ContainsFully(const BoundingBox& outer, const BoundingBox& inner)
	{
		const XMFLOAT3 outerMin = Subtract(outer.Center, outer.Extents);
		const XMFLOAT3 outerMax = Add(outer.Center, outer.Extents);
		const XMFLOAT3 innerMin = Subtract(inner.Center, inner.Extents);
		const XMFLOAT3 innerMax = Add(inner.Center, inner.Extents);

		return innerMin.x >= outerMin.x && innerMax.x <= outerMax.x &&
			innerMin.y >= outerMin.y && innerMax.y <= outerMax.y &&
			innerMin.z >= outerMin.z && innerMax.z <= outerMax.z;
	}

	BoundingBox MakeChildBounds(const BoundingBox& parent, int childIndex)
	{
		const XMFLOAT3 childExtents(
			parent.Extents.x * 0.5f,
			parent.Extents.y * 0.5f,
			parent.Extents.z * 0.5f);

		const XMFLOAT3 offset(
			(childIndex & 1) ? childExtents.x : -childExtents.x,
			(childIndex & 2) ? childExtents.y : -childExtents.y,
			(childIndex & 4) ? childExtents.z : -childExtents.z);

		BoundingBox childBounds;
		childBounds.Center = Add(parent.Center, offset);
		childBounds.Extents = childExtents;
		return childBounds;
	}

	const wchar_t* ScatterModeName(ScatterCullingMode mode)
	{
		switch (mode)
		{
		case ScatterCullingMode::Off:
			return L"Off";
		case ScatterCullingMode::Frustum:
			return L"Frustum";
		case ScatterCullingMode::Octree:
			return L"Octree";
		default:
			return L"Unknown";
		}
	}
}

enum class RenderLayer : int
{
	Opaque = 0,
	Tessellated,
	Count
};

class TexWavesApp : public D3DApp
{
public:
	TexWavesApp(HINSTANCE hInstance);
	TexWavesApp(const TexWavesApp& rhs) = delete;
	TexWavesApp& operator=(const TexWavesApp& rhs) = delete;
	~TexWavesApp();

	virtual bool Initialize()override;

private:
	virtual void OnResize()override;
	virtual void Update(const GameTimer& gt)override;
	virtual void Draw(const GameTimer& gt)override;

	virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
	virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
	virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

	void OnKeyboardInput(const GameTimer& gt);
	void UpdateCamera(const GameTimer& gt);
	void AnimateMaterials(const GameTimer& gt);
	void UpdateObjectCBs(const GameTimer& gt);
	void UpdateMaterialCBs(const GameTimer& gt);
	void UpdateMainPassCB(const GameTimer& gt);
	void UpdateWaves(const GameTimer& gt);

	void LoadTextures();
	void BuildRootSignature();
	void BuildDescriptorHeaps();
	void BuildGBufferDescriptorHeaps();
	void BuildDebugRootSignature();
	void BuildShadersAndInputLayout();
	void BuildLandGeometry();
	void BuildWavesGeometry();
	void BuildBoxGeometry();
	void BuildSponzaGeometry();
	void BuildStonePathwayGeometry();
	void BuildScatterBoxGeometry();
	void BuildLightSphereGeometry();
	void BuildSponzaMaterials();
	void BuildStonePathwayMaterial();
	void BuildScatterMaterial();
	void BuildSponzaRenderItems();
	void BuildStonePathwayRenderItem();
	void BuildScatterInstances();
	void BuildPSOs();
	void BuildFrameResources();
	void BuildMaterials();
	void BuildRenderItems();
	void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
	void BuildInstancedRootSignature();
	void UpdateScatterInstanceData();
	void DrawScatterInstances(ID3D12GraphicsCommandList* cmdList);
	void BuildScatterOctree();
	std::unique_ptr<OctreeNode> BuildScatterOctreeNode(const BoundingBox& bounds, const std::vector<UINT>& instanceIndices, UINT depth);
	void CollectVisibleScatterInstances(const BoundingFrustum& frustum, std::vector<UINT>& visibleIndices) const;
	void CollectVisibleScatterInstancesFromNode(const OctreeNode* node, const BoundingFrustum& frustum, std::vector<UINT>& visibleIndices) const;
	void CollectAllScatterInstancesFromNode(const OctreeNode* node, std::vector<UINT>& visibleIndices) const;
	void UpdateScatterCaption() const;
	void BuildLightingRootSignature();
	void LoadSponzaTextures();
	void LoadStonePathwayTextures();
	bool ShouldTessellateMaterial(const std::string& materialName) const;
	std::filesystem::path BuildSiblingTexturePath(const std::filesystem::path& diffusePath, const std::string& suffix) const;

	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();

	float GetHillsHeight(float x, float z)const;
	XMFLOAT3 GetHillsNormal(float x, float z)const;

private:

	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource* mCurrFrameResource = nullptr;
	int mCurrFrameResourceIndex = 0;

	UINT mCbvSrvDescriptorSize = 0;

	ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
	ComPtr<ID3D12RootSignature> mInstancedRootSignature = nullptr;

	ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

	ComPtr<ID3D12DescriptorHeap> mGBufferRtvHeap = nullptr;
	ComPtr<ID3D12DescriptorHeap> mGBufferDsvHeap = nullptr;

	ComPtr<ID3D12RootSignature> mDebugRootSignature = nullptr;

	ComPtr<ID3D12RootSignature> mLightingRootSignature = nullptr;

	int mDebugGBufferIndex = 0;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
	std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
	std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;
	std::unique_ptr<RenderingSystem> mRenderingSystem;

	std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

	RenderItem* mWavesRitem = nullptr;

	// List of all the render items.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// Render items divided by PSO.
	std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

	std::unique_ptr<Waves> mWaves;

	PassConstants mMainPassCB;

	XMFLOAT3 mEyePos = { 0.0f, 8.0f, -25.0f };
	XMFLOAT4X4 mView = MathHelper::Identity4x4();
	XMFLOAT4X4 mProj = MathHelper::Identity4x4();

	float mYaw = 0.0f;
	float mPitch = 0.0f;

	float mMoveSpeed = 18.0f;
	float mMouseSensitivity = 0.0025f;

	bool mMouseLookActive = false;

	POINT mLastMousePos;

	ObjModelLoader::ModelData mSponzaModel;
	ObjModelLoader::ModelData mStonePathwayModel;

	std::filesystem::path mSponzaDirectory;
	std::filesystem::path mStonePathwayDirectory;
	std::unordered_map<std::string, std::string> mSponzaMaterialToTextureName;
	std::unordered_map<std::string, std::string> mSponzaMaterialToNormalTextureName;
	std::unordered_map<std::string, std::string> mSponzaMaterialToDisplacementTextureName;
	std::unordered_map<std::string, UINT> mTextureSrvHeapIndices;
	std::vector<std::string> mOrderedTextureNames;
	UINT mGBufferSrvHeapOffset = 0;

	std::vector<DirectX::XMFLOAT3> mGarlandColors;
	BoundingFrustum mCameraFrustum;
	BoundingBox mScatterLocalBounds;
	std::vector<ScatterInstance> mScatterInstances;
	std::unique_ptr<OctreeNode> mScatterOctree;
	UINT mScatterVisibleCount = 0;
	ScatterCullingMode mScatterCullingMode = ScatterCullingMode::Octree;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
	PSTR cmdLine, int showCmd)
{
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	try
	{
		TexWavesApp theApp(hInstance);
		if (!theApp.Initialize())
			return 0;

		return theApp.Run();
	}
	catch (DxException& e)
	{
		MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
		return 0;
	}
	catch (std::exception& e)
	{
		std::string msg = e.what();
		MessageBoxA(nullptr, msg.c_str(), "std::exception", MB_OK);
		return 0;
	}
}

TexWavesApp::TexWavesApp(HINSTANCE hInstance)
	: D3DApp(hInstance)
{
}

TexWavesApp::~TexWavesApp()
{
	if (md3dDevice != nullptr)
		FlushCommandQueue();
}

bool TexWavesApp::Initialize()
{
	if (!D3DApp::Initialize())
		return false;

	// Reset the command list to prep for initialization commands.
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	// Get the increment size of a descriptor in this heap type.  This is hardware specific, 
	// so we have to query this information.
	mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	mRenderingSystem = std::make_unique<RenderingSystem>();
	mRenderingSystem->Initialize(
		md3dDevice.Get(),
		mClientWidth,
		mClientHeight,
		mBackBufferFormat,
		mDepthStencilFormat,
		mRtvDescriptorSize,
		mDsvDescriptorSize,
		mCbvSrvDescriptorSize);

	mWaves = std::make_unique<Waves>(128, 128, 1.0f, 0.03f, 4.0f, 0.2f);

	LoadTextures();
	BuildSponzaGeometry();
	LoadSponzaTextures();
	BuildStonePathwayGeometry();
	LoadStonePathwayTextures();

	BuildRootSignature();
	BuildInstancedRootSignature();
	BuildDebugRootSignature();
	BuildLightingRootSignature();

	BuildDescriptorHeaps();
	BuildGBufferDescriptorHeaps();

	BuildShadersAndInputLayout();
	BuildLandGeometry();
	BuildWavesGeometry();
	BuildBoxGeometry();
	BuildScatterBoxGeometry();
	BuildLightSphereGeometry();

	BuildMaterials();
	BuildSponzaMaterials();
	BuildStonePathwayMaterial();
	BuildScatterMaterial();

	BuildRenderItems();
	BuildSponzaRenderItems();
	BuildStonePathwayRenderItem();
	BuildScatterInstances();

	BuildFrameResources();
	BuildPSOs();

	// Execute the initialization commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until initialization is complete.
	FlushCommandQueue();

	return true;
}

void TexWavesApp::OnResize()
{
	D3DApp::OnResize();

	// The window resized, so update the aspect ratio and recompute the projection matrix.
	XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
	XMStoreFloat4x4(&mProj, P);
	BoundingFrustum::CreateFromMatrix(mCameraFrustum, P);
	if (mRenderingSystem)
	{
		mRenderingSystem->OnResize(mClientWidth, mClientHeight);
	}
}

void TexWavesApp::Update(const GameTimer& gt)
{
	OnKeyboardInput(gt);
	UpdateCamera(gt);

	// Cycle through the circular frame resource array.
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

	// Has the GPU finished processing the commands of the current frame resource?
	// If not, wait until the GPU has completed commands up to this fence point.
	if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

	AnimateMaterials(gt);
	UpdateObjectCBs(gt);
	UpdateMaterialCBs(gt);
	UpdateMainPassCB(gt);
	UpdateWaves(gt);
	UpdateScatterInstanceData();
	UpdateScatterCaption();
}

void TexWavesApp::Draw(const GameTimer& gt)
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

	ThrowIfFailed(cmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), nullptr));

	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	auto& gbuffer = mRenderingSystem->GetGBuffer();

	mCommandList->SetPipelineState(mPSOs["gbuffer"].Get());

	gbuffer.TransitionToWrite(mCommandList.Get());

	const float gbufferClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	gbuffer.Clear(mCommandList.Get(), gbufferClear);
	gbuffer.SetAsRenderTarget(mCommandList.Get());

	ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(4, passCB->GetGPUVirtualAddress());

	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

	mCommandList->SetPipelineState(mPSOs["gbufferTess"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Tessellated]);

	DrawScatterInstances(mCommandList.Get());

	gbuffer.TransitionToRead(mCommandList.Get());

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET));

	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::Black, 0, nullptr);
	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);

	mCommandList->SetPipelineState(mPSOs["deferredLighting"].Get());
	mCommandList->SetGraphicsRootSignature(mLightingRootSignature.Get());

	CD3DX12_GPU_DESCRIPTOR_HANDLE lightSrv(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	lightSrv.Offset(mGBufferSrvHeapOffset, mCbvSrvDescriptorSize);

	mCommandList->SetGraphicsRootDescriptorTable(0, lightSrv);
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

	mCommandList->IASetVertexBuffers(0, 0, nullptr);
	mCommandList->IASetIndexBuffer(nullptr);
	mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	mCommandList->DrawInstanced(3, 1, 0, 0);

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PRESENT));

	ThrowIfFailed(mCommandList->Close());

	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	mCurrFrameResource->Fence = ++mCurrentFence;
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void TexWavesApp::OnMouseDown(WPARAM btnState, int x, int y)
{
	mLastMousePos.x = x;
	mLastMousePos.y = y;

	if ((btnState & MK_RBUTTON) != 0)
	{
		mMouseLookActive = true;
		SetCapture(mhMainWnd);
	}
}

void TexWavesApp::OnMouseUp(WPARAM btnState, int x, int y)
{
	UNREFERENCED_PARAMETER(btnState);
	UNREFERENCED_PARAMETER(x);
	UNREFERENCED_PARAMETER(y);

	mMouseLookActive = false;
	ReleaseCapture();
}

void TexWavesApp::OnMouseMove(WPARAM btnState, int x, int y)
{
	if (mMouseLookActive && ((btnState & MK_RBUTTON) != 0))
	{
		float dx = static_cast<float>(x - mLastMousePos.x);
		float dy = static_cast<float>(y - mLastMousePos.y);

		mYaw += dx * mMouseSensitivity;
		mPitch += -dy * mMouseSensitivity;

		mPitch = MathHelper::Clamp(mPitch, -XM_PIDIV2 + 0.1f, XM_PIDIV2 - 0.1f);
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}

void TexWavesApp::OnKeyboardInput(const GameTimer& gt)
{
	if (!mMouseLookActive)
		return;

	float dt = gt.DeltaTime();
	float speed = mMoveSpeed;

	if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
		speed *= 2.5f;

	XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMVECTOR forward = XMVectorSet(
		cosf(mPitch) * sinf(mYaw),
		sinf(mPitch),
		cosf(mPitch) * cosf(mYaw),
		0.0f);

	forward = XMVector3Normalize(forward);

	XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));
	XMVECTOR flatForward = XMVector3Normalize(XMVectorSet(XMVectorGetX(forward), 0.0f, XMVectorGetZ(forward), 0.0f));

	XMVECTOR pos = XMLoadFloat3(&mEyePos);

	if (GetAsyncKeyState('W') & 0x8000)
		pos += flatForward * (speed * dt);

	if (GetAsyncKeyState('S') & 0x8000)
		pos -= flatForward * (speed * dt);

	if (GetAsyncKeyState('A') & 0x8000)
		pos -= right * (speed * dt);

	if (GetAsyncKeyState('D') & 0x8000)
		pos += right * (speed * dt);

	if (GetAsyncKeyState('E') & 0x8000)
		pos += worldUp * (speed * dt);

	if (GetAsyncKeyState('Q') & 0x8000)
		pos -= worldUp * (speed * dt);

	XMStoreFloat3(&mEyePos, pos);
}

void TexWavesApp::UpdateCamera(const GameTimer& gt)
{
	UNREFERENCED_PARAMETER(gt);

	XMVECTOR pos = XMLoadFloat3(&mEyePos);

	XMVECTOR forward = XMVectorSet(
		cosf(mPitch) * sinf(mYaw),
		sinf(mPitch),
		cosf(mPitch) * cosf(mYaw),
		0.0f);

	forward = XMVector3Normalize(forward);

	XMVECTOR target = pos + forward;
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&mView, view);
}

void TexWavesApp::AnimateMaterials(const GameTimer& gt)
{
	auto waterMat = mMaterials["water"].get();
	auto stonePathwayMat = mMaterials["stonePathway"].get();

	float& tu = waterMat->MatTransform(3, 0);
	float& tv = waterMat->MatTransform(3, 1);

	tu += 0.1f * gt.DeltaTime();
	tv += 0.02f * gt.DeltaTime();

	if (tu >= 1.0f)
		tu -= 1.0f;

	if (tv >= 1.0f)
		tv -= 1.0f;

	waterMat->MatTransform(3, 0) = tu;
	waterMat->MatTransform(3, 1) = tv;

	waterMat->NumFramesDirty = gNumFrameResources;

	const float stoneAngle = 0.35f * gt.TotalTime();
	XMStoreFloat4x4(
		&stonePathwayMat->MatTransform,
		XMMatrixTranslation(-0.5f, -0.5f, 0.0f) *
		XMMatrixRotationZ(stoneAngle) *
		XMMatrixTranslation(0.5f, 0.5f, 0.0f));
	stonePathwayMat->NumFramesDirty = gNumFrameResources;
}

void TexWavesApp::UpdateObjectCBs(const GameTimer& gt)
{
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
	for (auto& e : mAllRitems)
	{
		if (e->NumFramesDirty > 0)
		{
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));

			currObjectCB->CopyData(e->ObjCBIndex, objConstants);

			// Next FrameResource need to be updated too.
			e->NumFramesDirty--;
		}
	}
}

void TexWavesApp::UpdateMaterialCBs(const GameTimer& gt)
{
	auto currMaterialCB = mCurrFrameResource->MaterialCB.get();
	for (auto& e : mMaterials)
	{
		Material* mat = e.second.get();
		if (mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

			MaterialConstants matConstants;
			matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
			matConstants.FresnelR0 = mat->FresnelR0;
			matConstants.Roughness = mat->Roughness;
			XMStoreFloat4x4(&matConstants.MatTransform, XMMatrixTranspose(matTransform));
			matConstants.DisplacementScale = mat->DisplacementScale;

			currMaterialCB->CopyData(mat->MatCBIndex, matConstants);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void TexWavesApp::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = XMLoadFloat4x4(&mView);
	XMMATRIX proj = XMLoadFloat4x4(&mProj);

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));

	mMainPassCB.EyePosW = mEyePos;
	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt.TotalTime();
	mMainPassCB.DeltaTime = gt.DeltaTime();

	mMainPassCB.AmbientLight = { 0.32f, 0.32f, 0.34f, 1.0f };

	mMainPassCB.Lights[0].Direction = { 0.45f, -0.70f, 0.55f };
	mMainPassCB.Lights[0].Strength = { 0.85f, 0.85f, 0.82f };

	mMainPassCB.Lights[1].Direction = { -0.60f, -0.45f, 0.30f };
	mMainPassCB.Lights[1].Strength = { 0.35f, 0.35f, 0.38f };

	mMainPassCB.Lights[2].Direction = { 0.15f, -0.80f, -0.55f };
	mMainPassCB.Lights[2].Strength = { 0.20f, 0.20f, 0.22f };

	//const int kFirstPointLight = 3;
	//const int kPointLightCount = 32;
	//const int kFirstSpotLight = kFirstPointLight + kPointLightCount; // 35

	for (int i = 0; i < MaxLights; ++i)
	{
		mMainPassCB.Lights[i].Strength = { 0.0f, 0.0f, 0.0f };
		mMainPassCB.Lights[i].FalloffStart = 0.0f;
		mMainPassCB.Lights[i].Direction = { 0.0f, -1.0f, 0.0f };
		mMainPassCB.Lights[i].FalloffEnd = 0.0f;
		mMainPassCB.Lights[i].Position = { 0.0f, 0.0f, 0.0f };
		mMainPassCB.Lights[i].SpotPower = 1.0f;
	}

	// ------------------------------------------------------------
	// Ambient
	// ------------------------------------------------------------
	mMainPassCB.AmbientLight = { 0.22f, 0.22f, 0.25f, 1.0f };

	mMainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
	mMainPassCB.Lights[0].Strength = { 1.1f, 1.1f, 1.0f };

	mMainPassCB.Lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
	mMainPassCB.Lights[1].Strength = { 0.45f, 0.45f, 0.40f };

	mMainPassCB.Lights[2].Direction = { 0.0f, -0.707f, -0.707f };
	mMainPassCB.Lights[2].Strength = { 0.30f, 0.30f, 0.35f };

	// ------------------------------------------------------------
	// Garland point lights
	// ------------------------------------------------------------
	const int kFirstPointLight = 3;
	const int kPointLightCount = 32;
	const int kFirstSpotLight = 35;

	assert(kFirstSpotLight + 3 < MaxLights);

	// Эти координаты потом подгонишь под свои колонны
	const DirectX::XMFLOAT3 startP = { -6.5f, 8.8f, 1.0f };
	const DirectX::XMFLOAT3 endP = { 6.5f, 8.8f, 1.0f };

	for (int i = 0; i < kPointLightCount; ++i)
	{
		float u = (float)i / (float)(kPointLightCount - 1);

		float x = startP.x + (endP.x - startP.x) * u;
		float z = startP.z + (endP.z - startP.z) * u;
		float sag = 0.7f * sinf(u * XM_PI);
		float y = startP.y - sag;

		int idx = kFirstPointLight + i;

		mMainPassCB.Lights[idx].Position = { x, y, z };
		auto c = mGarlandColors[i];

		mMainPassCB.Lights[idx].Strength =
		{
			c.x * 10.5f,
			c.y * 10.5f,
			c.z * 10.5f
		};
		mMainPassCB.Lights[idx].FalloffStart = 0.0f;
		mMainPassCB.Lights[idx].FalloffEnd = 1.0f;
	}

	// ------------------------------------------------------------
	// Spot lights
	// ------------------------------------------------------------

	const float t = gt.TotalTime();

	mMainPassCB.Lights[kFirstSpotLight + 0].Position = { 0.0f, 18.0f, 48.0f };
	mMainPassCB.Lights[kFirstSpotLight + 0].Direction = { 0.0f, -0.35f, -1.0f };
	mMainPassCB.Lights[kFirstSpotLight + 0].Strength = { 22.0f, 22.0f, 22.0f };
	mMainPassCB.Lights[kFirstSpotLight + 0].FalloffStart = 2.0f;
	mMainPassCB.Lights[kFirstSpotLight + 0].FalloffEnd = 58.0f;
	mMainPassCB.Lights[kFirstSpotLight + 0].SpotPower = 56.0f;

	mMainPassCB.Lights[kFirstSpotLight + 1].Position = { -48.0f, 16.0f, 0.0f };
	mMainPassCB.Lights[kFirstSpotLight + 1].Direction = { 1.0f, -0.28f, 0.0f };
	mMainPassCB.Lights[kFirstSpotLight + 1].Strength = { 20.0f, 16.0f, 12.0f };
	mMainPassCB.Lights[kFirstSpotLight + 1].FalloffStart = 2.0f;
	mMainPassCB.Lights[kFirstSpotLight + 1].FalloffEnd = 54.0f;
	mMainPassCB.Lights[kFirstSpotLight + 1].SpotPower = 72.0f;

	mMainPassCB.Lights[kFirstSpotLight + 2].Position = { 0.0f, 24.0f, 0.0f };
	mMainPassCB.Lights[kFirstSpotLight + 2].Direction = { cosf(0.55f * t), -0.60f, sinf(0.55f * t) };
	mMainPassCB.Lights[kFirstSpotLight + 2].Strength = { 14.0f, 20.0f, 24.0f };
	mMainPassCB.Lights[kFirstSpotLight + 2].FalloffStart = 2.0f;
	mMainPassCB.Lights[kFirstSpotLight + 2].FalloffEnd = 56.0f;
	mMainPassCB.Lights[kFirstSpotLight + 2].SpotPower = 48.0f;

	mMainPassCB.Lights[kFirstSpotLight + 3].Position = { 48.0f, 20.0f, -40.0f };
	mMainPassCB.Lights[kFirstSpotLight + 3].Direction = { -0.85f, -0.32f, 0.65f };
	mMainPassCB.Lights[kFirstSpotLight + 3].Strength = { 24.0f, 10.0f, 10.0f };
	mMainPassCB.Lights[kFirstSpotLight + 3].FalloffStart = 2.0f;
	mMainPassCB.Lights[kFirstSpotLight + 3].FalloffEnd = 60.0f;
	mMainPassCB.Lights[kFirstSpotLight + 3].SpotPower = 80.0f;

	for (int i = 0; i < 4; ++i)
	{
		XMVECTOR dir = XMLoadFloat3(&mMainPassCB.Lights[kFirstSpotLight + i].Direction);
		dir = XMVector3Normalize(dir);
		XMStoreFloat3(&mMainPassCB.Lights[kFirstSpotLight + i].Direction, dir);
	}

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void TexWavesApp::UpdateWaves(const GameTimer& gt)
{
	static float t_base = 0.0f;
	if ((mTimer.TotalTime() - t_base) >= 0.25f)
	{
		t_base += 0.25f;

		int i = MathHelper::Rand(4, mWaves->RowCount() - 5);
		int j = MathHelper::Rand(4, mWaves->ColumnCount() - 5);

		float r = MathHelper::RandF(0.2f, 0.5f);

		mWaves->Disturb(i, j, r);
	}

	// Update the wave simulation.
	mWaves->Update(gt.DeltaTime());

	// Update the wave vertex buffer with the new solution.
	auto currWavesVB = mCurrFrameResource->WavesVB.get();
	for (int i = 0; i < mWaves->VertexCount(); ++i)
	{
		Vertex v;

		v.Pos = mWaves->Position(i);
		v.Normal = mWaves->Normal(i);

		v.TexC.x = 0.5f + v.Pos.x / mWaves->Width();
		v.TexC.y = 0.5f - v.Pos.z / mWaves->Depth();

		currWavesVB->CopyData(i, v);
	}

	mWavesRitem->Geo->VertexBufferGPU = currWavesVB->Resource();
}

void TexWavesApp::LoadTextures()
{
	mOrderedTextureNames.clear();

	auto grassTex = std::make_unique<Texture>();
	grassTex->Name = "grassTex";
	grassTex->Filename = L"../../Textures/grass.dds";
	ThrowIfFailed(CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(),
		grassTex->Filename.c_str(), grassTex->Resource, grassTex->UploadHeap));
	mOrderedTextureNames.push_back(grassTex->Name);
	mTextures[grassTex->Name] = std::move(grassTex);

	auto waterTex = std::make_unique<Texture>();
	waterTex->Name = "waterTex";
	waterTex->Filename = L"../../Textures/water1.dds";
	ThrowIfFailed(CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(),
		waterTex->Filename.c_str(), waterTex->Resource, waterTex->UploadHeap));
	mOrderedTextureNames.push_back(waterTex->Name);
	mTextures[waterTex->Name] = std::move(waterTex);

	auto fenceTex = std::make_unique<Texture>();
	fenceTex->Name = "fenceTex";
	fenceTex->Filename = L"../../Textures/WireFence.dds";
	ThrowIfFailed(CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(),
		fenceTex->Filename.c_str(), fenceTex->Resource, fenceTex->UploadHeap));
	mOrderedTextureNames.push_back(fenceTex->Name);
	mTextures[fenceTex->Name] = std::move(fenceTex);

	auto defaultNormalTex = std::make_unique<Texture>();
	defaultNormalTex->Name = "defaultNormalTex";
	defaultNormalTex->Filename = L"../../Textures/default_nmap.dds";
	ThrowIfFailed(CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(),
		defaultNormalTex->Filename.c_str(), defaultNormalTex->Resource, defaultNormalTex->UploadHeap));
	mOrderedTextureNames.push_back(defaultNormalTex->Name);
	mTextures[defaultNormalTex->Name] = std::move(defaultNormalTex);

	auto defaultDisplacementTex = std::make_unique<Texture>();
	defaultDisplacementTex->Name = "defaultDisplacementTex";
	defaultDisplacementTex->Filename = L"../../Textures/white1x1.dds";
	ThrowIfFailed(CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(),
		defaultDisplacementTex->Filename.c_str(), defaultDisplacementTex->Resource, defaultDisplacementTex->UploadHeap));
	mOrderedTextureNames.push_back(defaultDisplacementTex->Name);
	mTextures[defaultDisplacementTex->Name] = std::move(defaultDisplacementTex);
}

void TexWavesApp::BuildRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE diffuseTable;
	diffuseTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	CD3DX12_DESCRIPTOR_RANGE normalTable;
	normalTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

	CD3DX12_DESCRIPTOR_RANGE displacementTable;
	displacementTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[6];

	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsDescriptorTable(1, &diffuseTable);
	slotRootParameter[1].InitAsDescriptorTable(1, &normalTable);
	slotRootParameter[2].InitAsDescriptorTable(1, &displacementTable);
	slotRootParameter[3].InitAsConstantBufferView(0);
	slotRootParameter[4].InitAsConstantBufferView(1);
	slotRootParameter[5].InitAsConstantBufferView(2);

	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(6, slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void TexWavesApp::BuildDebugRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE texTable;
	texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	CD3DX12_ROOT_PARAMETER slotRootParameter[1];
	slotRootParameter[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);

	auto staticSamplers = GetStaticSamplers();

	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
		1,
		slotRootParameter,
		(UINT)staticSamplers.size(),
		staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(
		&rootSigDesc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(),
		errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());

	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mDebugRootSignature.GetAddressOf())));
}

void TexWavesApp::BuildInstancedRootSignature()
{
	CD3DX12_ROOT_PARAMETER slotRootParameter[3];
	slotRootParameter[0].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	slotRootParameter[1].InitAsConstantBufferView(0);
	slotRootParameter[2].InitAsConstantBufferView(1);

	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
		3,
		slotRootParameter,
		0,
		nullptr,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(
		&rootSigDesc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(),
		errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());

	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mInstancedRootSignature.GetAddressOf())));
}

void TexWavesApp::BuildDescriptorHeaps()
{
	mTextureSrvHeapIndices.clear();

	mGBufferSrvHeapOffset = (UINT)mOrderedTextureNames.size();

	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = mGBufferSrvHeapOffset + 4; // RT0, RT1, Depth, RT2
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	for (UINT i = 0; i < (UINT)mOrderedTextureNames.size(); ++i)
	{
		const std::string& texName = mOrderedTextureNames[i];
		auto tex = mTextures[texName].get();

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = tex->Resource->GetDesc().Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvDesc, hDescriptor);
		mTextureSrvHeapIndices[texName] = i;

		hDescriptor.Offset(1, mCbvSrvDescriptorSize);
	}
}

void TexWavesApp::BuildGBufferDescriptorHeaps()
{
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = GBuffer::kBufferCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mGBufferRtvHeap)));

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mGBufferDsvHeap)));

	mRenderingSystem->BuildOffscreenDescriptors(
		mGBufferRtvHeap.Get(),
		0,
		mSrvDescriptorHeap.Get(),
		mGBufferSrvHeapOffset,
		mGBufferDsvHeap.Get(),
		0);
}

void TexWavesApp::BuildShadersAndInputLayout()
{
	mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_0");

	mShaders["gbufferVS"] = d3dUtil::CompileShader(L"Shaders\\DeferredGeometry.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["gbufferPS"] = d3dUtil::CompileShader(L"Shaders\\DeferredGeometry.hlsl", nullptr, "PS", "ps_5_0");
	mShaders["tessVS"] = d3dUtil::CompileShader(L"Shaders\\TessellationGBuffer.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["tessHS"] = d3dUtil::CompileShader(L"Shaders\\TessellationGBuffer.hlsl", nullptr, "HS", "hs_5_0");
	mShaders["tessDS"] = d3dUtil::CompileShader(L"Shaders\\TessellationGBuffer.hlsl", nullptr, "DS", "ds_5_0");
	mShaders["tessPS"] = d3dUtil::CompileShader(L"Shaders\\TessellationGBuffer.hlsl", nullptr, "PS", "ps_5_0");

	mShaders["debugVS"] = d3dUtil::CompileShader(L"Shaders\\DebugGBuffer.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["debugPS"] = d3dUtil::CompileShader(L"Shaders\\DebugGBuffer.hlsl", nullptr, "PS", "ps_5_0");
	mShaders["instancedGBufferVS"] = d3dUtil::CompileShader(L"Shaders\\InstancedDeferred.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["instancedGBufferPS"] = d3dUtil::CompileShader(L"Shaders\\InstancedDeferred.hlsl", nullptr, "PS", "ps_5_0");

	mShaders["deferredLightVS"] = d3dUtil::CompileShader(L"Shaders\\DeferredLighting.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["deferredLightPS"] = d3dUtil::CompileShader(L"Shaders\\DeferredLighting.hlsl", nullptr, "PS", "ps_5_0");

	mInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
}

void TexWavesApp::BuildLandGeometry()
{
	GeometryGenerator geoGen;
	GeometryGenerator::MeshData grid = geoGen.CreateGrid(160.0f, 160.0f, 50, 50);

	std::vector<Vertex> vertices(grid.Vertices.size());
	for (size_t i = 0; i < grid.Vertices.size(); ++i)
	{
		auto& p = grid.Vertices[i].Position;
		vertices[i].Pos = p;
		vertices[i].Pos.y = GetHillsHeight(p.x, p.z);
		vertices[i].Normal = GetHillsNormal(p.x, p.z);
		vertices[i].TexC = grid.Vertices[i].TexC;
	}

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

	std::vector<std::uint16_t> indices = grid.GetIndices16();
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "landGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	geo->DrawArgs["grid"] = submesh;

	mGeometries["landGeo"] = std::move(geo);
}

void TexWavesApp::BuildWavesGeometry()
{
	std::vector<std::uint16_t> indices(3 * mWaves->TriangleCount()); // 3 indices per face
	assert(mWaves->VertexCount() < 0x0000ffff);

	// Iterate over each quad.
	int m = mWaves->RowCount();
	int n = mWaves->ColumnCount();
	int k = 0;
	for (int i = 0; i < m - 1; ++i)
	{
		for (int j = 0; j < n - 1; ++j)
		{
			indices[k] = i * n + j;
			indices[k + 1] = i * n + j + 1;
			indices[k + 2] = (i + 1) * n + j;

			indices[k + 3] = (i + 1) * n + j;
			indices[k + 4] = i * n + j + 1;
			indices[k + 5] = (i + 1) * n + j + 1;

			k += 6; // next quad
		}
	}

	UINT vbByteSize = mWaves->VertexCount() * sizeof(Vertex);
	UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "waterGeo";

	// Set dynamically.
	geo->VertexBufferCPU = nullptr;
	geo->VertexBufferGPU = nullptr;

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	geo->DrawArgs["grid"] = submesh;

	mGeometries["waterGeo"] = std::move(geo);
}

void TexWavesApp::BuildBoxGeometry()
{
	GeometryGenerator geoGen;
	GeometryGenerator::MeshData box = geoGen.CreateBox(8.0f, 8.0f, 8.0f, 3);

	std::vector<Vertex> vertices(box.Vertices.size());
	for (size_t i = 0; i < box.Vertices.size(); ++i)
	{
		auto& p = box.Vertices[i].Position;
		vertices[i].Pos = p;
		vertices[i].Normal = box.Vertices[i].Normal;
		vertices[i].TexC = box.Vertices[i].TexC;
	}

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

	std::vector<std::uint16_t> indices = box.GetIndices16();
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "boxGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	geo->DrawArgs["box"] = submesh;

	mGeometries["boxGeo"] = std::move(geo);
}

void TexWavesApp::BuildLightSphereGeometry()
{
	GeometryGenerator geoGen;
	GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.18f, 12, 12);
	// 0.18f ~= размер "с кулак" после твоего масштаба сцены

	std::vector<Vertex> vertices(sphere.Vertices.size());
	for (size_t i = 0; i < sphere.Vertices.size(); ++i)
	{
		vertices[i].Pos = sphere.Vertices[i].Position;
		vertices[i].Normal = sphere.Vertices[i].Normal;
		vertices[i].TexC = sphere.Vertices[i].TexC;
	}

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

	std::vector<std::uint16_t> indices = sphere.GetIndices16();
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "lightSphereGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(
		md3dDevice.Get(),
		mCommandList.Get(),
		vertices.data(),
		vbByteSize,
		geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(
		md3dDevice.Get(),
		mCommandList.Get(),
		indices.data(),
		ibByteSize,
		geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	geo->DrawArgs["sphere"] = submesh;

	mGeometries["lightSphereGeo"] = std::move(geo);
}

void TexWavesApp::BuildSponzaGeometry()
{
	std::filesystem::path objPath = L"C:/Users/theof/source/repos/CG/Lab2/d3d12book-master — копия/Chapter 9 Texturing/TexWaves/x64/Debug/sponza.obj";

	if (!std::filesystem::exists(objPath))
	{
		throw std::runtime_error(
			"Sponza OBJ file not found. Check path: " + objPath.string());
	}

	mSponzaDirectory = objPath.parent_path();
	mSponzaModel = ObjModelLoader::LoadFromFile(objPath);

	if (mSponzaModel.Vertices.empty())
	{
		throw std::runtime_error("Sponza load failed: Vertices are empty.");
	}

	if (mSponzaModel.Indices32.empty())
	{
		throw std::runtime_error("Sponza load failed: Indices are empty.");
	}

	if (mSponzaModel.Submeshes.empty())
	{
		throw std::runtime_error("Sponza load failed: Submeshes are empty.");
	}

	const UINT vbByteSize = (UINT)mSponzaModel.Vertices.size() * sizeof(ObjModelLoader::Vertex);
	const UINT ibByteSize = (UINT)mSponzaModel.Indices32.size() * sizeof(uint32_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "sponzaGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(
		geo->VertexBufferCPU->GetBufferPointer(),
		mSponzaModel.Vertices.data(),
		vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(
		geo->IndexBufferCPU->GetBufferPointer(),
		mSponzaModel.Indices32.data(),
		ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(
		md3dDevice.Get(),
		mCommandList.Get(),
		mSponzaModel.Vertices.data(),
		vbByteSize,
		geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(
		md3dDevice.Get(),
		mCommandList.Get(),
		mSponzaModel.Indices32.data(),
		ibByteSize,
		geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(ObjModelLoader::Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R32_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	for (const auto& sm : mSponzaModel.Submeshes)
	{
		SubmeshGeometry submesh;
		submesh.IndexCount = sm.IndexCount;
		submesh.StartIndexLocation = sm.StartIndexLocation;
		submesh.BaseVertexLocation = sm.BaseVertexLocation;

		geo->DrawArgs[sm.Name] = submesh;
	}

	mGeometries[geo->Name] = std::move(geo);
}

void TexWavesApp::BuildScatterBoxGeometry()
{
	GeometryGenerator geoGen;
	GeometryGenerator::MeshData box = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 0);

	std::vector<Vertex> vertices(box.Vertices.size());
	for (size_t i = 0; i < box.Vertices.size(); ++i)
	{
		vertices[i].Pos = box.Vertices[i].Position;
		vertices[i].Normal = box.Vertices[i].Normal;
		vertices[i].TexC = box.Vertices[i].TexC;
	}

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	std::vector<std::uint16_t> indices = box.GetIndices16();
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "scatterBoxGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(
		md3dDevice.Get(),
		mCommandList.Get(),
		vertices.data(),
		vbByteSize,
		geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(
		md3dDevice.Get(),
		mCommandList.Get(),
		indices.data(),
		ibByteSize,
		geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	geo->DrawArgs["scatterBox"] = submesh;
	mGeometries[geo->Name] = std::move(geo);

	mScatterLocalBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
	mScatterLocalBounds.Extents = XMFLOAT3(0.5f, 0.5f, 0.5f);
}

void TexWavesApp::BuildStonePathwayGeometry()
{
	std::filesystem::path objPath = L"Assets/StonePathway/StonePathway.obj";
	objPath = std::filesystem::absolute(objPath);

	if (!std::filesystem::exists(objPath))
		throw std::runtime_error("Stone Pathway OBJ file not found: " + objPath.string());

	mStonePathwayDirectory = objPath.parent_path();
	mStonePathwayModel = ObjModelLoader::LoadFromFile(objPath);

	if (mStonePathwayModel.Vertices.empty() || mStonePathwayModel.Indices32.empty())
		throw std::runtime_error("Stone Pathway load failed: geometry is empty.");

	const bool hasTexcoords = std::any_of(
		mStonePathwayModel.Vertices.begin(),
		mStonePathwayModel.Vertices.end(),
		[](const ObjModelLoader::Vertex& v)
		{
			return std::fabs(v.TexC.x) > 1e-6f || std::fabs(v.TexC.y) > 1e-6f;
		});

	if (!hasTexcoords)
	{
		float minY = FLT_MAX;
		float maxY = -FLT_MAX;

		for (const auto& v : mStonePathwayModel.Vertices)
		{
			minY = (std::min)(minY, v.Pos.y);
			maxY = (std::max)(maxY, v.Pos.y);
		}

		const float sizeY = (std::max)(maxY - minY, 1e-3f);
		const float tileU = 4.0f;
		const float tileV = 2.0f;
		const float seamShift = 0.25f;

		for (auto& v : mStonePathwayModel.Vertices)
		{
			float u = (std::atan2(v.Pos.z, v.Pos.x) + XM_PI) / XM_2PI;
			u = std::fmod(u + seamShift, 1.0f);
			if (u < 0.0f)
				u += 1.0f;

			const float vCoord = (v.Pos.y - minY) / sizeY;

			v.TexC.x = u * tileU;
			v.TexC.y = 1.0f - vCoord * tileV;
		}
	}

	const UINT vbByteSize = (UINT)mStonePathwayModel.Vertices.size() * sizeof(ObjModelLoader::Vertex);
	const UINT ibByteSize = (UINT)mStonePathwayModel.Indices32.size() * sizeof(uint32_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "stonePathwayGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), mStonePathwayModel.Vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), mStonePathwayModel.Indices32.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(
		md3dDevice.Get(),
		mCommandList.Get(),
		mStonePathwayModel.Vertices.data(),
		vbByteSize,
		geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(
		md3dDevice.Get(),
		mCommandList.Get(),
		mStonePathwayModel.Indices32.data(),
		ibByteSize,
		geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(ObjModelLoader::Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R32_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount = (UINT)mStonePathwayModel.Indices32.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	geo->DrawArgs["stonePathway"] = submesh;
	mGeometries[geo->Name] = std::move(geo);
}

void TexWavesApp::BuildSponzaMaterials()
{
	int nextMatCBIndex = (int)mMaterials.size();
	const int defaultNormalSrv = (int)mTextureSrvHeapIndices["defaultNormalTex"];
	const int defaultDisplacementSrv = (int)mTextureSrvHeapIndices["defaultDisplacementTex"];

	for (const auto& kv : mSponzaModel.Materials)
	{
		const auto& srcMat = kv.second;

		if (mMaterials.find(srcMat.Name) != mMaterials.end())
			continue;

		auto mat = std::make_unique<Material>();
		mat->Name = srcMat.Name;
		mat->MatCBIndex = nextMatCBIndex++;

		auto texNameIt = mSponzaMaterialToTextureName.find(srcMat.Name);
		if (texNameIt != mSponzaMaterialToTextureName.end())
		{
			const std::string& texName = texNameIt->second;
			auto srvIt = mTextureSrvHeapIndices.find(texName);
			mat->DiffuseSrvHeapIndex = (srvIt != mTextureSrvHeapIndices.end()) ? srvIt->second : 0;
		}
		else
		{
			mat->DiffuseSrvHeapIndex = 0;
		}

		auto normalNameIt = mSponzaMaterialToNormalTextureName.find(srcMat.Name);
		if (normalNameIt != mSponzaMaterialToNormalTextureName.end())
		{
			auto srvIt = mTextureSrvHeapIndices.find(normalNameIt->second);
			mat->NormalSrvHeapIndex = (srvIt != mTextureSrvHeapIndices.end()) ? srvIt->second : defaultNormalSrv;
		}
		else
		{
			mat->NormalSrvHeapIndex = defaultNormalSrv;
		}

		auto displacementNameIt = mSponzaMaterialToDisplacementTextureName.find(srcMat.Name);
		if (displacementNameIt != mSponzaMaterialToDisplacementTextureName.end())
		{
			auto srvIt = mTextureSrvHeapIndices.find(displacementNameIt->second);
			mat->DisplacementSrvHeapIndex = (srvIt != mTextureSrvHeapIndices.end()) ? srvIt->second : defaultDisplacementSrv;
		}
		else
		{
			mat->DisplacementSrvHeapIndex = defaultDisplacementSrv;
		}

		mat->DiffuseAlbedo = XMFLOAT4(srcMat.Kd.x, srcMat.Kd.y, srcMat.Kd.z, 1.0f);
		mat->FresnelR0 = XMFLOAT3(0.04f, 0.04f, 0.04f);
		mat->Roughness = 0.35f;
		mat->DisplacementScale = ShouldTessellateMaterial(srcMat.Name) ? 0.45f : 0.0f;

		XMStoreFloat4x4(&mat->MatTransform, XMMatrixIdentity());

		mMaterials[mat->Name] = std::move(mat);
	}

	if (mMaterials.find("default") == mMaterials.end())
	{
		auto mat = std::make_unique<Material>();
		mat->Name = "default";
		mat->MatCBIndex = nextMatCBIndex++;
		mat->DiffuseSrvHeapIndex = 0;
		mat->NormalSrvHeapIndex = defaultNormalSrv;
		mat->DisplacementSrvHeapIndex = defaultDisplacementSrv;
		mat->DiffuseAlbedo = XMFLOAT4(0.85f, 0.85f, 0.85f, 1.0f);
		mat->FresnelR0 = XMFLOAT3(0.04f, 0.04f, 0.04f);
		mat->Roughness = 0.5f;
		XMStoreFloat4x4(&mat->MatTransform, XMMatrixIdentity());

		mMaterials[mat->Name] = std::move(mat);
	}
}

void TexWavesApp::BuildStonePathwayMaterial()
{
	if (mMaterials.find("stonePathway") != mMaterials.end())
		return;

	auto mat = std::make_unique<Material>();
	mat->Name = "stonePathway";
	mat->MatCBIndex = (int)mMaterials.size();
	mat->DiffuseSrvHeapIndex = (int)mTextureSrvHeapIndices["stonePathwayBaseTex"];
	mat->NormalSrvHeapIndex = (int)mTextureSrvHeapIndices["stonePathwayNormalTex"];
	mat->DisplacementSrvHeapIndex = (int)mTextureSrvHeapIndices["stonePathwayHeightTex"];
	mat->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	mat->FresnelR0 = XMFLOAT3(0.04f, 0.04f, 0.04f);
	mat->Roughness = 0.55f;
	mat->DisplacementScale = 0.045f;
	XMStoreFloat4x4(&mat->MatTransform, XMMatrixIdentity());

	mMaterials[mat->Name] = std::move(mat);
}

void TexWavesApp::BuildScatterMaterial()
{
	if (mMaterials.find("scatterBox") != mMaterials.end())
		return;

	auto mat = std::make_unique<Material>();
	mat->Name = "scatterBox";
	mat->MatCBIndex = (int)mMaterials.size();
	mat->DiffuseSrvHeapIndex = 0;
	mat->NormalSrvHeapIndex = (int)mTextureSrvHeapIndices["defaultNormalTex"];
	mat->DisplacementSrvHeapIndex = (int)mTextureSrvHeapIndices["defaultDisplacementTex"];
	mat->DiffuseAlbedo = XMFLOAT4(0.90f, 0.35f, 0.20f, 1.0f);
	mat->FresnelR0 = XMFLOAT3(0.04f, 0.04f, 0.04f);
	mat->Roughness = 0.55f;
	XMStoreFloat4x4(&mat->MatTransform, XMMatrixIdentity());

	mMaterials[mat->Name] = std::move(mat);
}

void TexWavesApp::BuildSponzaRenderItems()
{
	// ------------------------------------------------------------
	// 1) Sponza OBJ
	// ------------------------------------------------------------
	auto geo = mGeometries["sponzaGeo"].get();

	for (const auto& sm : mSponzaModel.Submeshes)
	{
		auto ri = std::make_unique<RenderItem>();

		XMMATRIX world =
			XMMatrixScaling(0.15f, 0.15f, 0.15f) *
			XMMatrixTranslation(0.0f, 0.0f, 0.0f);

		XMStoreFloat4x4(&ri->World, world);
		XMStoreFloat4x4(&ri->TexTransform, XMMatrixIdentity());

		ri->ObjCBIndex = (UINT)mAllRitems.size();

		auto matIt = mMaterials.find(sm.MaterialName);
		if (matIt != mMaterials.end())
			ri->Mat = matIt->second.get();
		else
			ri->Mat = mMaterials["default"].get();

		ri->Geo = geo;
		const bool tessellated = (ri->Mat->DisplacementScale > 0.0f);
		ri->PrimitiveType = tessellated
			? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
			: D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		ri->IndexCount = sm.IndexCount;
		ri->StartIndexLocation = sm.StartIndexLocation;
		ri->BaseVertexLocation = sm.BaseVertexLocation;

		mRitemLayer[(int)(tessellated ? RenderLayer::Tessellated : RenderLayer::Opaque)].push_back(ri.get());
		mAllRitems.push_back(std::move(ri));
	}

	// ------------------------------------------------------------
	// 2) Garland of 500 small glowing spheres
	// ------------------------------------------------------------
	auto bulbGeo = mGeometries["lightSphereGeo"].get();
	auto bulbMat = mMaterials["lightBulb"].get();
	const int defaultNormalSrv = (int)mTextureSrvHeapIndices["defaultNormalTex"];
	const int defaultDisplacementSrv = (int)mTextureSrvHeapIndices["defaultDisplacementTex"];

	const int bulbCount = 500;

	const XMFLOAT3 startP = XMFLOAT3(-30.5f, 40.8f, -40.0f);
	const XMFLOAT3 endP = XMFLOAT3(30.5f, 40.8f, 40.0f);

	mGarlandColors.clear();
	mGarlandColors.reserve(bulbCount);

	for (int i = 0; i < bulbCount; ++i)
	{
		float u = (bulbCount == 1) ? 0.0f : (float)i / (float)(bulbCount - 1);

		float x = startP.x + (endP.x - startP.x) * u;
		float z = startP.z + (endP.z - startP.z) * u;

		float sag = 0.9f * sinf(u * XM_PI);
		float y = startP.y - sag;

		float hue = (float)i / bulbCount;

		// простая HSV ? RGB (псевдо)
		float r = fabsf(sinf(hue * XM_2PI));
		float g = fabsf(sinf(hue * XM_2PI + 2.0f));
		float b = fabsf(sinf(hue * XM_2PI + 4.0f));

		// немного усилим насыщенность
		r = powf(r, 0.5f);
		g = powf(g, 0.5f);
		b = powf(b, 0.5f);

		mGarlandColors.push_back({ r, g, b });

		auto ri = std::make_unique<RenderItem>();

		XMMATRIX world = XMMatrixTranslation(x, y, z);
		XMStoreFloat4x4(&ri->World, world);
		XMStoreFloat4x4(&ri->TexTransform, XMMatrixIdentity());

		ri->ObjCBIndex = (UINT)mAllRitems.size();
		auto mat = std::make_unique<Material>();

		mat->Name = "lightBulb_" + std::to_string(i);
		mat->MatCBIndex = (UINT)mMaterials.size();
		mat->DiffuseSrvHeapIndex = 0;
		mat->NormalSrvHeapIndex = defaultNormalSrv;
		mat->DisplacementSrvHeapIndex = defaultDisplacementSrv;
		mat->DiffuseAlbedo = XMFLOAT4(r * 6.0f, g * 6.0f, b * 6.0f, 1.0f);
		mat->FresnelR0 = XMFLOAT3(0.02f, 0.02f, 0.02f);
		mat->Roughness = 0.01f;

		Material* matPtr = mat.get();
		mMaterials[mat->Name] = std::move(mat);

		ri->Mat = matPtr;
		ri->Geo = bulbGeo;
		ri->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		ri->IndexCount = bulbGeo->DrawArgs["sphere"].IndexCount;
		ri->StartIndexLocation = bulbGeo->DrawArgs["sphere"].StartIndexLocation;
		ri->BaseVertexLocation = bulbGeo->DrawArgs["sphere"].BaseVertexLocation;

		mRitemLayer[(int)RenderLayer::Opaque].push_back(ri.get());
		mAllRitems.push_back(std::move(ri));
	}
}

void TexWavesApp::BuildPSOs()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc;

	ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	opaquePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	opaquePsoDesc.pRootSignature = mRootSignature.Get();
	opaquePsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
		mShaders["standardVS"]->GetBufferSize()
	};
	opaquePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
		mShaders["opaquePS"]->GetBufferSize()
	};
	opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	opaquePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	opaquePsoDesc.SampleMask = UINT_MAX;
	opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	opaquePsoDesc.NumRenderTargets = 1;
	opaquePsoDesc.RTVFormats[0] = mBackBufferFormat;
	opaquePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	opaquePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	opaquePsoDesc.DSVFormat = mDepthStencilFormat;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufferPsoDesc = opaquePsoDesc;
	gbufferPsoDesc.pRootSignature = mRootSignature.Get();
	gbufferPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["gbufferVS"]->GetBufferPointer()),
		mShaders["gbufferVS"]->GetBufferSize()
	};
	gbufferPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["gbufferPS"]->GetBufferPointer()),
		mShaders["gbufferPS"]->GetBufferSize()
	};
	gbufferPsoDesc.NumRenderTargets = 3;
	gbufferPsoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	gbufferPsoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	gbufferPsoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16_FLOAT;
	gbufferPsoDesc.DSVFormat = mDepthStencilFormat;

	// ВАЖНО:
	gbufferPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
		&gbufferPsoDesc,
		IID_PPV_ARGS(&mPSOs["gbuffer"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC instancedGBufferPsoDesc = gbufferPsoDesc;
	instancedGBufferPsoDesc.pRootSignature = mInstancedRootSignature.Get();
	instancedGBufferPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["instancedGBufferVS"]->GetBufferPointer()),
		mShaders["instancedGBufferVS"]->GetBufferSize()
	};
	instancedGBufferPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["instancedGBufferPS"]->GetBufferPointer()),
		mShaders["instancedGBufferPS"]->GetBufferSize()
	};

	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
		&instancedGBufferPsoDesc,
		IID_PPV_ARGS(&mPSOs["instancedGBuffer"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC tessPsoDesc = gbufferPsoDesc;
	tessPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["tessVS"]->GetBufferPointer()),
		mShaders["tessVS"]->GetBufferSize()
	};
	tessPsoDesc.HS =
	{
		reinterpret_cast<BYTE*>(mShaders["tessHS"]->GetBufferPointer()),
		mShaders["tessHS"]->GetBufferSize()
	};
	tessPsoDesc.DS =
	{
		reinterpret_cast<BYTE*>(mShaders["tessDS"]->GetBufferPointer()),
		mShaders["tessDS"]->GetBufferSize()
	};
	tessPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["tessPS"]->GetBufferPointer()),
		mShaders["tessPS"]->GetBufferSize()
	};
	tessPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
	tessPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
		&tessPsoDesc,
		IID_PPV_ARGS(&mPSOs["gbufferTess"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC debugPsoDesc = {};
	ZeroMemory(&debugPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	debugPsoDesc.InputLayout = { nullptr, 0 };
	debugPsoDesc.pRootSignature = mDebugRootSignature.Get();
	debugPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["debugVS"]->GetBufferPointer()),
		mShaders["debugVS"]->GetBufferSize()
	};
	debugPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["debugPS"]->GetBufferPointer()),
		mShaders["debugPS"]->GetBufferSize()
	};
	debugPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	debugPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	debugPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	debugPsoDesc.DepthStencilState.DepthEnable = false;
	debugPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	debugPsoDesc.SampleMask = UINT_MAX;
	debugPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	debugPsoDesc.NumRenderTargets = 1;
	debugPsoDesc.RTVFormats[0] = mBackBufferFormat;
	debugPsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	debugPsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	debugPsoDesc.DSVFormat = mDepthStencilFormat;

	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
		&debugPsoDesc,
		IID_PPV_ARGS(&mPSOs["debugGBuffer"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC lightPsoDesc = {};
	ZeroMemory(&lightPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	lightPsoDesc.InputLayout = { nullptr, 0 };
	lightPsoDesc.pRootSignature = mLightingRootSignature.Get();
	lightPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["deferredLightVS"]->GetBufferPointer()),
		mShaders["deferredLightVS"]->GetBufferSize()
	};
	lightPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["deferredLightPS"]->GetBufferPointer()),
		mShaders["deferredLightPS"]->GetBufferSize()
	};
	lightPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	lightPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	lightPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	lightPsoDesc.DepthStencilState.DepthEnable = false;
	lightPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	lightPsoDesc.SampleMask = UINT_MAX;
	lightPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	lightPsoDesc.NumRenderTargets = 1;
	lightPsoDesc.RTVFormats[0] = mBackBufferFormat;
	lightPsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	lightPsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	lightPsoDesc.DSVFormat = mDepthStencilFormat;

	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
		&lightPsoDesc,
		IID_PPV_ARGS(&mPSOs["deferredLighting"])));
}

void TexWavesApp::BuildFrameResources()
{
	for (int i = 0; i < gNumFrameResources; ++i)
	{
		mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
			1, (UINT)mAllRitems.size(), (UINT)mMaterials.size(), mWaves->VertexCount(), (UINT)(std::max<size_t>(1, mScatterInstances.size()))));
	}
}

void TexWavesApp::BuildMaterials()
{
	const int defaultNormalSrv = (int)mTextureSrvHeapIndices["defaultNormalTex"];
	const int defaultDisplacementSrv = (int)mTextureSrvHeapIndices["defaultDisplacementTex"];

	auto grass = std::make_unique<Material>();
	grass->Name = "grass";
	grass->MatCBIndex = 0;
	grass->DiffuseSrvHeapIndex = 0;
	grass->NormalSrvHeapIndex = defaultNormalSrv;
	grass->DisplacementSrvHeapIndex = defaultDisplacementSrv;
	grass->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	grass->FresnelR0 = XMFLOAT3(0.01f, 0.01f, 0.01f);
	grass->Roughness = 0.125f;

	auto water = std::make_unique<Material>();
	water->Name = "water";
	water->MatCBIndex = 1;
	water->DiffuseSrvHeapIndex = 1;
	water->NormalSrvHeapIndex = defaultNormalSrv;
	water->DisplacementSrvHeapIndex = defaultDisplacementSrv;
	water->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	water->FresnelR0 = XMFLOAT3(0.2f, 0.2f, 0.2f);
	water->Roughness = 0.0f;

	auto wirefence = std::make_unique<Material>();
	wirefence->Name = "wirefence";
	wirefence->MatCBIndex = 2;
	wirefence->DiffuseSrvHeapIndex = 2;
	wirefence->NormalSrvHeapIndex = defaultNormalSrv;
	wirefence->DisplacementSrvHeapIndex = defaultDisplacementSrv;
	wirefence->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	wirefence->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
	wirefence->Roughness = 0.25f;

	auto lightBulb = std::make_unique<Material>();
	lightBulb->Name = "lightBulb";
	lightBulb->MatCBIndex = 3;
	lightBulb->DiffuseSrvHeapIndex = 0; // можно использовать любую существующую текстуру, нам важен цвет
	lightBulb->NormalSrvHeapIndex = defaultNormalSrv;
	lightBulb->DisplacementSrvHeapIndex = defaultDisplacementSrv;
	lightBulb->DiffuseAlbedo = XMFLOAT4(8.0f, 7.2f, 5.5f, 1.0f); // яркий тёплый
	lightBulb->FresnelR0 = XMFLOAT3(0.02f, 0.02f, 0.02f);
	lightBulb->Roughness = 0.02f;

	mMaterials["grass"] = std::move(grass);
	mMaterials["water"] = std::move(water);
	mMaterials["wirefence"] = std::move(wirefence);
	mMaterials["lightBulb"] = std::move(lightBulb);
}

void TexWavesApp::BuildRenderItems()
{
	auto wavesRitem = std::make_unique<RenderItem>();
	wavesRitem->World = MathHelper::Identity4x4();
	XMStoreFloat4x4(&wavesRitem->TexTransform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
	wavesRitem->ObjCBIndex = 0;
	wavesRitem->Mat = mMaterials["water"].get();
	wavesRitem->Geo = mGeometries["waterGeo"].get();
	wavesRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wavesRitem->IndexCount = wavesRitem->Geo->DrawArgs["grid"].IndexCount;
	wavesRitem->StartIndexLocation = wavesRitem->Geo->DrawArgs["grid"].StartIndexLocation;
	wavesRitem->BaseVertexLocation = wavesRitem->Geo->DrawArgs["grid"].BaseVertexLocation;

	mWavesRitem = wavesRitem.get();

	auto gridRitem = std::make_unique<RenderItem>();
	gridRitem->World = MathHelper::Identity4x4();
	XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
	gridRitem->ObjCBIndex = 1;
	gridRitem->Mat = mMaterials["grass"].get();
	gridRitem->Geo = mGeometries["landGeo"].get();
	gridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
	gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
	gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;

	auto boxRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&boxRitem->World, XMMatrixTranslation(3.0f, 2.0f, -9.0f));
	boxRitem->ObjCBIndex = 2;
	boxRitem->Mat = mMaterials["wirefence"].get();
	boxRitem->Geo = mGeometries["boxGeo"].get();
	boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
	boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
	boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;

	mAllRitems.push_back(std::move(wavesRitem));
	mAllRitems.push_back(std::move(gridRitem));
	mAllRitems.push_back(std::move(boxRitem));
}

void TexWavesApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

	auto objectCB = mCurrFrameResource->ObjectCB->Resource();
	auto matCB = mCurrFrameResource->MaterialCB->Resource();

	for (size_t i = 0; i < ritems.size(); ++i)
	{
		auto ri = ritems[i];

		cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
		cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
		cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

		CD3DX12_GPU_DESCRIPTOR_HANDLE diffuseTex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		diffuseTex.Offset(ri->Mat->DiffuseSrvHeapIndex, mCbvSrvDescriptorSize);

		CD3DX12_GPU_DESCRIPTOR_HANDLE normalTex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		normalTex.Offset(ri->Mat->NormalSrvHeapIndex, mCbvSrvDescriptorSize);

		CD3DX12_GPU_DESCRIPTOR_HANDLE displacementTex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		displacementTex.Offset(ri->Mat->DisplacementSrvHeapIndex, mCbvSrvDescriptorSize);

		D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
		D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex * matCBByteSize;

		cmdList->SetGraphicsRootDescriptorTable(0, diffuseTex);
		cmdList->SetGraphicsRootDescriptorTable(1, normalTex);
		cmdList->SetGraphicsRootDescriptorTable(2, displacementTex);
		cmdList->SetGraphicsRootConstantBufferView(3, objCBAddress);
		cmdList->SetGraphicsRootConstantBufferView(5, matCBAddress);

		cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
	}
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> TexWavesApp::GetStaticSamplers()
{
	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
		0.0f,                             // mipLODBias
		8);                               // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
		0.0f,                              // mipLODBias
		8);                                // maxAnisotropy

	return {
		pointWrap, pointClamp,
		linearWrap, linearClamp,
		anisotropicWrap, anisotropicClamp };
}

float TexWavesApp::GetHillsHeight(float x, float z)const
{
	return 0.3f * (z * sinf(0.1f * x) + x * cosf(0.1f * z));
}

XMFLOAT3 TexWavesApp::GetHillsNormal(float x, float z)const
{
	// n = (-df/dx, 1, -df/dz)
	XMFLOAT3 n(
		-0.03f * z * cosf(0.1f * x) - 0.3f * cosf(0.1f * z),
		1.0f,
		-0.3f * sinf(0.1f * x) + 0.03f * x * sinf(0.1f * z));

	XMVECTOR unitNormal = XMVector3Normalize(XMLoadFloat3(&n));
	XMStoreFloat3(&n, unitNormal);

	return n;
}

void TexWavesApp::BuildLightingRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE texTable;
	texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);

	CD3DX12_ROOT_PARAMETER slotRootParameter[2];
	slotRootParameter[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);
	slotRootParameter[1].InitAsConstantBufferView(0);

	auto staticSamplers = GetStaticSamplers();

	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
		2,
		slotRootParameter,
		(UINT)staticSamplers.size(),
		staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(
		&rootSigDesc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(),
		errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());

	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mLightingRootSignature.GetAddressOf())));
}

void TexWavesApp::LoadSponzaTextures()
{
	mSponzaMaterialToTextureName.clear();
	mSponzaMaterialToNormalTextureName.clear();
	mSponzaMaterialToDisplacementTextureName.clear();

	for (const auto& kv : mSponzaModel.Materials)
	{
		const auto& matInfo = kv.second;

		if (matInfo.DiffuseMap.empty())
			continue;

		std::filesystem::path texPath = mSponzaDirectory / matInfo.DiffuseMap;
		if (!std::filesystem::exists(texPath))
			continue;

		std::string textureName = "sponzaTex_" + matInfo.Name;
		mSponzaMaterialToTextureName[matInfo.Name] = textureName;

		if (mTextures.find(textureName) != mTextures.end())
			continue;

		auto tex = std::make_unique<Texture>();
		tex->Name = textureName;
		tex->Filename = texPath.wstring();

		ThrowIfFailed(CreateTGATextureFromFile12(
			md3dDevice.Get(),
			mCommandList.Get(),
			tex->Filename,
			tex->Resource,
			tex->UploadHeap));

		mOrderedTextureNames.push_back(tex->Name);
		mTextures[tex->Name] = std::move(tex);

		std::filesystem::path normalPath = BuildSiblingTexturePath(texPath, "_ddn");
		if (std::filesystem::exists(normalPath))
		{
			std::string normalTextureName = "sponzaNormal_" + matInfo.Name;
			mSponzaMaterialToNormalTextureName[matInfo.Name] = normalTextureName;

			if (mTextures.find(normalTextureName) == mTextures.end())
			{
				auto normalTex = std::make_unique<Texture>();
				normalTex->Name = normalTextureName;
				normalTex->Filename = normalPath.wstring();

				ThrowIfFailed(CreateTGATextureFromFile12(
					md3dDevice.Get(),
					mCommandList.Get(),
					normalTex->Filename,
					normalTex->Resource,
					normalTex->UploadHeap));

				mOrderedTextureNames.push_back(normalTex->Name);
				mTextures[normalTex->Name] = std::move(normalTex);
			}
		}

		if (ShouldTessellateMaterial(matInfo.Name))
		{
			std::string displacementTextureName = "sponzaDisp_" + matInfo.Name;
			mSponzaMaterialToDisplacementTextureName[matInfo.Name] = displacementTextureName;

			if (mTextures.find(displacementTextureName) == mTextures.end())
			{
				auto displacementTex = std::make_unique<Texture>();
				displacementTex->Name = displacementTextureName;
				displacementTex->Filename = texPath.wstring();

				ThrowIfFailed(CreateTGAHeightTextureFromFile12(
					md3dDevice.Get(),
					mCommandList.Get(),
					displacementTex->Filename,
					displacementTex->Resource,
					displacementTex->UploadHeap));

				mOrderedTextureNames.push_back(displacementTex->Name);
				mTextures[displacementTex->Name] = std::move(displacementTex);
			}
		}
	}
}

void TexWavesApp::BuildStonePathwayRenderItem()
{
	auto geo = mGeometries["stonePathwayGeo"].get();
	auto mat = mMaterials["stonePathway"].get();

	auto ri = std::make_unique<RenderItem>();
	XMMATRIX world =
		XMMatrixScaling(2.2f, 2.2f, 2.2f) *
		XMMatrixRotationY(0.35f * XM_PI) *
		XMMatrixTranslation(0.0f, 3.3f, 8.0f);

	XMStoreFloat4x4(&ri->World, world);
	XMStoreFloat4x4(&ri->TexTransform, XMMatrixScaling(2.5f, 2.5f, 1.0f));

	ri->ObjCBIndex = (UINT)mAllRitems.size();
	ri->Mat = mat;
	ri->Geo = geo;
	ri->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
	ri->IndexCount = geo->DrawArgs["stonePathway"].IndexCount;
	ri->StartIndexLocation = geo->DrawArgs["stonePathway"].StartIndexLocation;
	ri->BaseVertexLocation = geo->DrawArgs["stonePathway"].BaseVertexLocation;

	mRitemLayer[(int)RenderLayer::Tessellated].push_back(ri.get());
	mAllRitems.push_back(std::move(ri));
}

void TexWavesApp::BuildScatterInstances()
{
	mScatterInstances.clear();
	mScatterInstances.reserve(4096);

	const int countX = 20;
	const int countY = 8;
	const int countZ = 20;
	const float spacing = 8.0f;
	const XMFLOAT3 origin = XMFLOAT3(-76.0f, 1.0f, -76.0f);

	for (int y = 0; y < countY; ++y)
	{
		for (int z = 0; z < countZ; ++z)
		{
			for (int x = 0; x < countX; ++x)
			{
				const float worldX = origin.x + x * spacing;
				const float worldY = origin.y + y * 4.5f;
				const float worldZ = origin.z + z * spacing;

				XMMATRIX world =
					XMMatrixScaling(0.8f, 0.8f, 0.8f) *
					XMMatrixTranslation(worldX, worldY, worldZ);

				ScatterInstance instance;
				XMStoreFloat4x4(&instance.World, world);
				mScatterLocalBounds.Transform(instance.Bounds, world);

				mScatterInstances.push_back(instance);
			}
		}
	}

	BuildScatterOctree();
}

void TexWavesApp::BuildScatterOctree()
{
	mScatterOctree.reset();

	if (mScatterInstances.empty())
		return;

	BoundingBox rootBounds = mScatterInstances.front().Bounds;
	std::vector<UINT> instanceIndices;
	instanceIndices.reserve(mScatterInstances.size());

	for (UINT i = 0; i < (UINT)mScatterInstances.size(); ++i)
	{
		rootBounds = MergeBounds(rootBounds, mScatterInstances[i].Bounds);
		instanceIndices.push_back(i);
	}

	rootBounds.Extents.x += 0.01f;
	rootBounds.Extents.y += 0.01f;
	rootBounds.Extents.z += 0.01f;

	mScatterOctree = BuildScatterOctreeNode(rootBounds, instanceIndices, 0);
}

std::unique_ptr<OctreeNode> TexWavesApp::BuildScatterOctreeNode(const BoundingBox& bounds, const std::vector<UINT>& instanceIndices, UINT depth)
{
	auto node = std::make_unique<OctreeNode>();
	node->Bounds = bounds;

	const UINT kMaxDepth = 6;
	const size_t kLeafCapacity = 32;

	if (depth >= kMaxDepth || instanceIndices.size() <= kLeafCapacity)
	{
		node->InstanceIndices = instanceIndices;
		return node;
	}

	std::array<std::vector<UINT>, 8> childAssignments;
	std::vector<UINT> retainedIndices;

	for (UINT instanceIndex : instanceIndices)
	{
		const BoundingBox& instanceBounds = mScatterInstances[instanceIndex].Bounds;
		int containingChild = -1;

		for (int childIndex = 0; childIndex < 8; ++childIndex)
		{
			const BoundingBox childBounds = MakeChildBounds(bounds, childIndex);
			if (ContainsFully(childBounds, instanceBounds))
			{
				containingChild = childIndex;
				break;
			}
		}

		if (containingChild >= 0)
			childAssignments[containingChild].push_back(instanceIndex);
		else
			retainedIndices.push_back(instanceIndex);
	}

	bool hasChildren = false;
	for (int childIndex = 0; childIndex < 8; ++childIndex)
	{
		if (!childAssignments[childIndex].empty())
		{
			node->Children[childIndex] = BuildScatterOctreeNode(
				MakeChildBounds(bounds, childIndex),
				childAssignments[childIndex],
				depth + 1);
			hasChildren = true;
		}
	}

	if (!hasChildren)
		node->InstanceIndices = instanceIndices;
	else
		node->InstanceIndices = std::move(retainedIndices);

	return node;
}

void TexWavesApp::CollectVisibleScatterInstances(const BoundingFrustum& frustum, std::vector<UINT>& visibleIndices) const
{
	visibleIndices.clear();
	visibleIndices.reserve(mScatterInstances.size());

	switch (mScatterCullingMode)
	{
	case ScatterCullingMode::Off:
		for (UINT i = 0; i < (UINT)mScatterInstances.size(); ++i)
			visibleIndices.push_back(i);
		break;

	case ScatterCullingMode::Frustum:
		for (UINT i = 0; i < (UINT)mScatterInstances.size(); ++i)
		{
			if (frustum.Contains(mScatterInstances[i].Bounds) != DirectX::DISJOINT)
				visibleIndices.push_back(i);
		}
		break;

	case ScatterCullingMode::Octree:
		if (mScatterOctree)
			CollectVisibleScatterInstancesFromNode(mScatterOctree.get(), frustum, visibleIndices);
		break;
	}
}

void TexWavesApp::CollectVisibleScatterInstancesFromNode(const OctreeNode* node, const BoundingFrustum& frustum, std::vector<UINT>& visibleIndices) const
{
	if (node == nullptr)
		return;

	const ContainmentType nodeContainment = frustum.Contains(node->Bounds);
	if (nodeContainment == DirectX::DISJOINT)
		return;

	if (nodeContainment == DirectX::CONTAINS)
	{
		CollectAllScatterInstancesFromNode(node, visibleIndices);
		return;
	}

	for (UINT instanceIndex : node->InstanceIndices)
	{
		if (frustum.Contains(mScatterInstances[instanceIndex].Bounds) != DirectX::DISJOINT)
			visibleIndices.push_back(instanceIndex);
	}

	for (const auto& child : node->Children)
	{
		if (child)
			CollectVisibleScatterInstancesFromNode(child.get(), frustum, visibleIndices);
	}
}

void TexWavesApp::CollectAllScatterInstancesFromNode(const OctreeNode* node, std::vector<UINT>& visibleIndices) const
{
	if (node == nullptr)
		return;

	visibleIndices.insert(visibleIndices.end(), node->InstanceIndices.begin(), node->InstanceIndices.end());

	for (const auto& child : node->Children)
	{
		if (child)
			CollectAllScatterInstancesFromNode(child.get(), visibleIndices);
	}
}

void TexWavesApp::UpdateScatterInstanceData()
{
	if (mScatterInstances.empty())
	{
		mScatterVisibleCount = 0;
		return;
	}

	XMMATRIX view = XMLoadFloat4x4(&mView);
	XMVECTOR detView = XMMatrixDeterminant(view);
	XMMATRIX invView = XMMatrixInverse(&detView, view);

	BoundingFrustum worldFrustum;
	mCameraFrustum.Transform(worldFrustum, invView);

	std::vector<UINT> visibleIndices;
	CollectVisibleScatterInstances(worldFrustum, visibleIndices);

	auto currInstanceBuffer = mCurrFrameResource->ScatterInstanceBuffer.get();
	for (UINT i = 0; i < (UINT)visibleIndices.size(); ++i)
	{
		ScatterInstanceData instanceData;
		XMMATRIX world = XMLoadFloat4x4(&mScatterInstances[visibleIndices[i]].World);
		XMStoreFloat4x4(&instanceData.World, XMMatrixTranspose(world));
		currInstanceBuffer->CopyData(i, instanceData);
	}

	mScatterVisibleCount = (UINT)visibleIndices.size();
}

void TexWavesApp::DrawScatterInstances(ID3D12GraphicsCommandList* cmdList)
{
	if (mScatterVisibleCount == 0)
		return;

	auto geo = mGeometries["scatterBoxGeo"].get();
	auto mat = mMaterials["scatterBox"].get();
	auto passCB = mCurrFrameResource->PassCB->Resource();
	auto materialCB = mCurrFrameResource->MaterialCB->Resource();
	D3D12_VERTEX_BUFFER_VIEW vbView = geo->VertexBufferView();
	D3D12_INDEX_BUFFER_VIEW ibView = geo->IndexBufferView();

	const UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));
	D3D12_GPU_VIRTUAL_ADDRESS matCBAddress =
		materialCB->GetGPUVirtualAddress() + (UINT64)mat->MatCBIndex * matCBByteSize;

	cmdList->SetPipelineState(mPSOs["instancedGBuffer"].Get());
	cmdList->SetGraphicsRootSignature(mInstancedRootSignature.Get());
	cmdList->SetGraphicsRootShaderResourceView(0, mCurrFrameResource->ScatterInstanceBuffer->Resource()->GetGPUVirtualAddress());
	cmdList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());
	cmdList->SetGraphicsRootConstantBufferView(2, matCBAddress);
	cmdList->IASetVertexBuffers(0, 1, &vbView);
	cmdList->IASetIndexBuffer(&ibView);
	cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmdList->DrawIndexedInstanced(
		geo->DrawArgs["scatterBox"].IndexCount,
		mScatterVisibleCount,
		geo->DrawArgs["scatterBox"].StartIndexLocation,
		geo->DrawArgs["scatterBox"].BaseVertexLocation,
		0);
}

void TexWavesApp::UpdateScatterCaption() const
{
	const std::wstring caption =
		L"Сколько кубов показывается: " +
		std::to_wstring(mScatterVisibleCount) +
		L"/" +
		std::to_wstring(mScatterInstances.size());

	const_cast<TexWavesApp*>(this)->mMainWndCaption = caption;
}

void TexWavesApp::LoadStonePathwayTextures()
{
	const std::array<std::pair<std::string, std::wstring>, 3> textures =
	{ {
		{ "stonePathwayBaseTex", L"Assets/StonePathway/Stone_Pathway_Base_Color.tga" },
		{ "stonePathwayNormalTex", L"Assets/StonePathway/Stone_Pathway_Normal.tga" },
		{ "stonePathwayHeightTex", L"Assets/StonePathway/Stone_Pathway_Height.tga" },
	} };

	for (const auto& [name, pathStr] : textures)
	{
		if (mTextures.find(name) != mTextures.end())
			continue;

		auto tex = std::make_unique<Texture>();
		tex->Name = name;
		tex->Filename = std::filesystem::absolute(pathStr).wstring();

		ThrowIfFailed(CreateTGATextureFromFile12(
			md3dDevice.Get(),
			mCommandList.Get(),
			tex->Filename,
			tex->Resource,
			tex->UploadHeap));

		mOrderedTextureNames.push_back(tex->Name);
		mTextures[tex->Name] = std::move(tex);
	}
}

bool TexWavesApp::ShouldTessellateMaterial(const std::string& materialName) const
{
	static const std::unordered_set<std::string> kTessellatedMaterials =
	{
		"floor_a",
		"bricks_a",
		"column_a",
		"column_b",
		"column_c"
	};

	for (const std::string& token : kTessellatedMaterials)
	{
		if (materialName.find(token) != std::string::npos)
			return true;
	}

	return false;
}

std::filesystem::path TexWavesApp::BuildSiblingTexturePath(const std::filesystem::path& diffusePath, const std::string& suffix) const
{
	const std::string stem = diffusePath.stem().string();
	const std::string extension = diffusePath.extension().string();

	if (stem.size() >= 5 && stem.substr(stem.size() - 5) == "_diff")
		return diffusePath.parent_path() / (stem.substr(0, stem.size() - 5) + suffix + extension);

	return diffusePath.parent_path() / (stem + suffix + extension);
}
